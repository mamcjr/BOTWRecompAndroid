// NSO loading, relocation, and symbol resolution.
#include <stdlib.h>
#include <string.h>

#include "codemods.h"
#include "rt.h"

Module g_modules[4];
int g_nmodules;

enum { DT_NULL = 0, DT_PLTRELSZ = 2, DT_HASH = 4, DT_STRTAB = 5, DT_SYMTAB = 6, DT_RELA = 7, DT_RELASZ = 8,
       DT_JMPREL = 23, DT_INIT_ARRAY = 25, DT_FINI_ARRAY = 26, DT_INIT_ARRAYSZ = 27, DT_FINI_ARRAYSZ = 28,
       DT_RELRSZ = 35, DT_RELR = 36, DT_GNU_HASH = 0x6FFFFEF5 };
enum { R_ABS64 = 257, R_GLOB_DAT = 1025, R_JUMP_SLOT = 1026, R_RELATIVE = 1027 };

typedef struct { uint32_t name; uint8_t info, other; uint16_t shndx; uint64_t value, size; } Elf64Sym;
typedef struct { uint64_t off, info; int64_t addend; } Elf64Rela;

int g_game_version;

static size_t lz4_decompress(const uint8_t *src, size_t srclen, uint8_t *dst, size_t dstlen) {
    const uint8_t *s = src, *se = src + srclen;
    uint8_t *d = dst, *de = dst + dstlen;
    while (s < se) {
        uint8_t tok = *s++;
        size_t lit = tok >> 4;
        if (lit == 15) { uint8_t b; do { b = *s++; lit += b; } while (b == 255); }
        if (d + lit > de || s + lit > se) break;
        memcpy(d, s, lit);
        d += lit; s += lit;
        if (s >= se) break;
        size_t off = s[0] | (s[1] << 8);
        s += 2;
        size_t ml = tok & 15;
        if (ml == 15) { uint8_t b; do { b = *s++; ml += b; } while (b == 255); }
        ml += 4;
        uint8_t *m = d - off;
        if (m < dst || d + ml > de) break;
        while (ml--) *d++ = *m++;
    }
    return d - dst;
}

static uint8_t *read_file(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    *len = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *p = malloc(*len);
    fread(p, 1, *len, f);
    fclose(f);
    return p;
}

// Number of dynsym entries from a DT_GNU_HASH table: the end of its last chain. Newer SDK modules (sdk and subsdk0
// of BOTW 1.8.2) have no DT_HASH, whose nchain says it directly. (From SavageWindRecomp, MIT.)
static uint64_t gnu_hash_nsyms(uint64_t h) {
    const uint32_t *w = (const uint32_t *)h;
    uint32_t nbuckets = w[0], symoffset = w[1], bloom_size = w[2];
    const uint32_t *buckets = (const uint32_t *)(h + 16 + 8ull * bloom_size), *chain = buckets + nbuckets;
    uint32_t last = 0;
    for (uint32_t i = 0; i < nbuckets; i++)
        if (buckets[i] > last) last = buckets[i];
    if (last < symoffset) return symoffset;  // (no hashed symbols)
    while (!(chain[last - symoffset] & 1)) last++;
    return (uint64_t)last + 1;
}

// The game's version for main's build id (the NSO header's bytes 0x40-0x47; 0: unknown) - also runtime/gamebuild.c
int game_version_of_build(const uint8_t id[8]) {
    static const struct { uint8_t id[8]; int version; } builds[] = {
        {{0x20, 0x94, 0xf1, 0x52, 0x83, 0x66, 0x83, 0xa3}, 100},
        {{0x8e, 0x99, 0x78, 0xd5, 0x0b, 0xdd, 0x20, 0xb4}, 160},
        {{0xde, 0x0b, 0x6a, 0xc4, 0xef, 0xc1, 0xdd, 0xa7}, 182},
        {{0xcd, 0x57, 0xb2, 0x3f, 0xa4, 0xbb, 0xad, 0x65}, 190},  // (the update v1114112: SDK 21.4.1, NVN 55.15)
    };
    for (unsigned i = 0; i < sizeof builds / sizeof *builds; i++)
        if (!memcmp(id, builds[i].id, 8)) return builds[i].version;
    return 0;
}

static void load_nso(Module *m, const char *path, uint64_t base) {
    size_t len;
    uint8_t *raw = read_file(path, &len);
    if (!raw || memcmp(raw, "NSO0", 4)) FATAL("cannot load %s\n", path);
    if (base == MAIN_BASE) {  // (the game's version: g_game_version)
        g_game_version = game_version_of_build(raw + 0x40);
        LOG("[loader] main build %02x%02x%02x%02x...: game version %d\n", raw[0x40], raw[0x41], raw[0x42], raw[0x43],
            g_game_version);
    }
    uint32_t flags = *(uint32_t *)(raw + 0xC);
    uint32_t seg[3][3];  // file_off, mem_off, size
    for (int i = 0; i < 3; i++) memcpy(seg[i], raw + 0x10 + i * 0x10, 12);
    uint32_t bss = *(uint32_t *)(raw + 0x3C);
    uint64_t size = (seg[2][1] + seg[2][2] + bss + 0x3FFF) & ~0x3FFFull;
    plat_commit(base, size);  // inside the guest window: zero-filled, read/write
    for (int i = 0; i < 3; i++) {
        uint32_t csize = *(uint32_t *)(raw + 0x60 + i * 4);
        uint8_t *dst = (uint8_t *)base + seg[i][1];
        if (flags & (1 << i)) {
            size_t got = lz4_decompress(raw + seg[i][0], csize, dst, seg[i][2]);
            if (got != seg[i][2]) FATAL("lz4 %s seg %d: %zu != %u\n", path, i, got, seg[i][2]);
        } else {
            memcpy(dst, raw + seg[i][0], seg[i][2]);
        }
    }
    free(raw);
    m->base = base;
    m->size = size;
    m->text_off = seg[0][1]; m->text_size = seg[0][2];
    m->ro_off = seg[1][1];   m->ro_size = seg[1][2];
    m->data_off = seg[2][1]; m->data_size = seg[2][2] + bss;

    uint32_t mod0 = *(uint32_t *)(base + 4);
    uint64_t mod = base + mod0;
    m->dynamic = mod + *(int32_t *)(mod + 4);
    uint64_t gnu_hash = 0;
    for (uint64_t *d = (uint64_t *)m->dynamic; d[0] != DT_NULL; d += 2) {
        switch (d[0]) {
        case DT_GNU_HASH: gnu_hash = base + d[1]; break;
        case DT_SYMTAB: m->symtab = base + d[1]; break;
        case DT_STRTAB: m->strtab = base + d[1]; break;
        case DT_HASH: m->nsyms = *(uint32_t *)(base + d[1] + 4); break;
        case 12: m->init = base + d[1]; break;  // DT_INIT
        case 13: m->fini = base + d[1]; break;  // DT_FINI
        case DT_INIT_ARRAY: m->init_array = base + d[1]; break;
        case DT_INIT_ARRAYSZ: m->init_array_sz = d[1]; break;
        case DT_FINI_ARRAY: m->fini_array = base + d[1]; break;
        case DT_FINI_ARRAYSZ: m->fini_array_sz = d[1]; break;
        }
    }
    if (!m->nsyms && gnu_hash) m->nsyms = gnu_hash_nsyms(gnu_hash);
    LOG("loaded %-5s at %llx size %llx syms %llu\n", m->name, base, size, m->nsyms);
}

static Elf64Sym *sym(Module *m, uint64_t i) { return (Elf64Sym *)m->symtab + i; }
static const char *symname(Module *m, Elf64Sym *s) { return (const char *)(m->strtab + s->name); }

uint64_t loader_find_symbol(const char *name) {
    for (int mi = 0; mi < g_nmodules; mi++) {
        Module *m = &g_modules[mi];
        for (uint64_t i = 1; i < m->nsyms; i++) {
            Elf64Sym *s = sym(m, i);
            if (s->shndx && !strcmp(symname(m, s), name)) return m->base + s->value;
        }
    }
    return 0;
}

const char *loader_symbol_at(uint64_t addr, uint64_t *offset) {
    const char *best = NULL;
    uint64_t best_addr = 0;
    for (int mi = 0; mi < g_nmodules; mi++) {
        Module *m = &g_modules[mi];
        if (addr < m->base || addr >= m->base + m->size) continue;
        for (uint64_t i = 1; i < m->nsyms; i++) {
            Elf64Sym *s = sym(m, i);
            uint64_t a = m->base + s->value;
            if (s->shndx && (s->info & 0xF) == 2 && a <= addr && a > best_addr) {
                best = symname(m, s);
                best_addr = a;
            }
        }
    }
    if (offset) *offset = addr - best_addr;
    return best;
}

// Functions a module defines whose calls (through the module's own PLT, every call to an exported symbol) go to a host
// implementation anyway; it calls the module's own one (loader_find_symbol) when it has adjusted the arguments.
static const char *const override_defined[] = {
    "_ZN2nn2os6detail21AddressSpaceAllocatorC1Emmm",  // runtime/hle/address_space.c
};

static uint64_t resolve(Module *m, uint32_t symidx) {
    Elf64Sym *s = sym(m, symidx);
    const char *name = symname(m, s);
    if (s->shndx) {
        for (size_t i = 0; i < sizeof override_defined / sizeof *override_defined; i++) {
            HostFn impl = strcmp(name, override_defined[i]) ? NULL : hle_lookup_impl(name);
            if (impl) return hle_register(name, impl);
        }
        return m->base + s->value;
    }
    HostFn impl = hle_lookup_impl(name);
    if (impl && native_on() && !strcmp(name, "longjmp")) impl = NULL;  // (the guest's own works on the CPU: hle/setjmp.c)
    if (impl) return hle_register(name, impl);  // host implementation wins over the guest SDK's
    uint64_t a = loader_find_symbol(name);
    if (a) return a;
    if ((s->info >> 4) == 2) return 0;  // weak undefined
    return hle_register(name, NULL);
}

// DT_RELR: packed relative relocations (newer SDKs: every module of BOTW 1.8.2 keeps its R_RELATIVE ones only here).
// An even entry is the offset of a word to relocate; an odd one is a bitmap of the 63 words after the last one
// relocated. The addend is the word itself. Returns how many words were relocated. (From SavageWindRecomp, MIT.)
static uint64_t relocate_relr(Module *m, uint64_t relr, uint64_t size) {
    uint64_t *where = NULL, n = 0;
    for (uint64_t i = 0; i < size / 8; i++) {
        uint64_t e = ((uint64_t *)relr)[i];
        if (!(e & 1)) {
            where = (uint64_t *)(m->base + e);
            *where++ += m->base;
            n++;
            continue;
        }
        if (!where) FATAL("%s: DT_RELR bitmap before any address\n", m->name);
        for (int b = 0; b < 63; b++)
            if ((e >> (b + 1)) & 1) where[b] += m->base, n++;
        where += 63;
    }
    return n;
}

static void relocate(Module *m) {
    uint64_t rela = 0, relasz = 0, jmprel = 0, pltsz = 0, relr = 0, relrsz = 0;
    for (uint64_t *d = (uint64_t *)m->dynamic; d[0] != DT_NULL; d += 2) {
        if (d[0] == DT_RELA) rela = m->base + d[1];
        if (d[0] == DT_RELASZ) relasz = d[1];
        if (d[0] == DT_JMPREL) jmprel = m->base + d[1];
        if (d[0] == DT_PLTRELSZ) pltsz = d[1];
        if (d[0] == DT_RELR) relr = m->base + d[1];
        if (d[0] == DT_RELRSZ) relrsz = d[1];
    }
    uint64_t packed = relr ? relocate_relr(m, relr, relrsz) : 0;
    int unresolved = 0;
    for (int pass = 0; pass < 2; pass++) {
        Elf64Rela *r = (Elf64Rela *)(pass ? jmprel : rela);
        uint64_t n = (pass ? pltsz : relasz) / sizeof(Elf64Rela);
        for (uint64_t i = 0; i < n; i++) {
            uint64_t *where = (uint64_t *)(m->base + r[i].off);
            uint32_t type = (uint32_t)r[i].info, si = r[i].info >> 32;
            switch (type) {
            case R_RELATIVE: *where = m->base + r[i].addend; break;
            case R_ABS64:
            case R_GLOB_DAT:
            case R_JUMP_SLOT: {
                uint64_t s = resolve(m, si);
                *where = s ? s + r[i].addend : 0;
                if (s >= HLE_BASE && s < HLE_BASE + HLE_SIZE) unresolved++;
                break;
            }
            default: FATAL("%s: reloc type %u\n", m->name, type);
            }
        }
    }
    LOG("relocated %-5s (%llu packed relative, %d imports bound to host stubs)\n", m->name, (unsigned long long)packed,
        unresolved);
}

void loader_load(const char *exefs) {
    char path[1024];
    const struct { const char *name; uint64_t base; } mods[] = {{"main", MAIN_BASE}, {"sdk", SDK_BASE}};  // (not static: Android's bases move)
    for (unsigned i = 0; i < 2; i++) {
        Module *m = &g_modules[g_nmodules++];
        m->name = mods[i].name;
        snprintf(path, sizeof path, "%s/%s", exefs, m->name);
        load_nso(m, path, mods[i].base);
    }
    // Code mods (runtime/codemods.h): their patches into main as the generator applied them (tools/nso.py), and their
    // modules right after main, where the Switch's loader puts the next module and an exlaunch patch looks for it
    static CodeMods cm;
    snprintf(path, sizeof path, "%s/main", exefs);
    codemods_find(path, &cm);
    g_codemods_active = cm.hash != 0;
    Module *mm = &g_modules[0];
    for (int i = 0; i < cm.nips; i++) {
        uint64_t far = codemods_apply_ips(cm.ips[i], (uint8_t *)(uintptr_t)mm->base, mm->size);
        uint64_t text_end = mm->text_off + mm->text_size;
        if (far > text_end && far <= mm->ro_off) mm->text_size = ((far + 3) & ~3ull) - mm->text_off;  // (code after .text)
    }
    uint64_t at = MAIN_BASE + codemods_main_end(path);
    for (int i = 0; i < cm.nmod && g_nmodules < (int)(sizeof g_modules / sizeof *g_modules); i++) {
        Module *m = &g_modules[g_nmodules++];
        m->name = cm.mod_name[i];
        load_nso(m, cm.mod_path[i], at);
        at += codemods_module_size(cm.mod_path[i]);
    }
    for (int i = 0; i < g_nmodules; i++) relocate(&g_modules[i]);
}
