// Android host-side NVN regression tests. Include the implementation, discard unrelated HLE sections at link.
#include <assert.h>
#include "../runtime/nvn/objects.c"

static uint64_t test_frame;
static int destroyed, drains;
uint64_t renderer_present_count(void) { return test_frame; }
void backend_texture_destroyed(NvnTexture *t) { assert(t->refs == 0); destroyed++; }
void vk_async_drain(void) { drains++; }
void rt_fatal_trace(void) { abort(); }
void rt_fatal_message(const char *format, ...) { (void)format; abort(); }
uint64_t plat_now_ns(void) { return 0; }
const FormatInfo *nvn_format_info(uint32_t f) { (void)f; static const FormatInfo fi = {0}; return &fi; }
void tegra_layout_init(TexLayout *t) { (void)t; }
uint64_t tegra_total_size(const TexLayout *t) { (void)t; return 0; }

// A guest texture object (192 opaque bytes, objects.c; its first word holds the host pointer) initialized from a
// builder over pool `ph`.
typedef struct { uint64_t host, rest[23]; } GuestTexture;
static uint8_t builder[256];
static NvnTexture *init_texture(GuestTexture *obj, uint64_t *ph, uint64_t offset, uint32_t width) {
    memset(builder, 0, sizeof builder);
    G(uint32_t, builder, 0xC) = 1;  // 2D
    G(uint32_t, builder, 0x10) = width;
    G(uint32_t, builder, 0x14) = 256;
    G(uint32_t, builder, 0x18) = 1;
    G(uint32_t, builder, 0x1C) = 1;
    G(uint32_t, builder, 0x20) = 0x25;
    G(uint64_t, builder, 0x38) = (uintptr_t)ph;
    G(uint64_t, builder, 0x40) = offset;
    Cpu c = {0};
    c.x[0] = (uintptr_t)obj; c.x[1] = (uintptr_t)builder;
    hle_nvnTextureInitialize(&c);
    assert(c.x[0] == 1);
    return HOST(NvnTexture, obj);
}
static void finalize(GuestTexture *obj) {
    Cpu c = {0};
    c.x[0] = (uintptr_t)obj;
    hle_nvnTextureFinalize(&c);
}
static void sweep(void) {  // retire a scratch texture far in the future: every earlier deferred release runs
    GuestTexture obj;
    test_frame += 100;
    init_texture(&obj, NULL, 0, 4);
    finalize(&obj);
    test_frame += 100;
    GuestTexture obj2;
    init_texture(&obj2, NULL, 0, 4);
    finalize(&obj2);
}

int main(void) {
    NvnPool physical = {.storage = 0x300000000ull, .size = 0x7800000, .flags = 0x189};
    NvnPool virtual = {.size = physical.size, .flags = 0x2a1};
    uint64_t ph = (uintptr_t)&physical, vh = (uintptr_t)&virtual;
    uint64_t map[5] = {(uintptr_t)&ph, 0, 0, physical.size, 0};
    Cpu c = {0};
    c.x[0] = (uintptr_t)&vh; c.x[1] = 1; c.x[2] = (uintptr_t)map;
    hle_nvnMemoryPoolMapVirtual(&c);
    assert(c.x[0] == 1 && virtual.storage == physical.storage);
    // Invalid requests must not partially change the old alias.
    map[1] = 1;
    c.x[0] = (uintptr_t)&vh;
    hle_nvnMemoryPoolMapVirtual(&c);
    assert(c.x[0] == 0 && virtual.storage == physical.storage);
    map[1] = 0; map[3] /= 2;
    c.x[0] = (uintptr_t)&vh;
    hle_nvnMemoryPoolMapVirtual(&c);
    assert(c.x[0] == 0 && virtual.storage == physical.storage);

    // Descriptor slots keep a texture alive after its object is finalized; a shallow copy's finalize does nothing.
    GuestTexture ah, bh;
    NvnTexture *a = init_texture(&ah, &ph, 0x1000, 256), *b = init_texture(&bh, &ph, 0x20000, 256);
    assert(a != b && a->refs == 1 && a->owners == 1);
    GuestTexture ah_copy = ah;
    NvnTexEntry entries[2] = {0};
    NvnDescPool pool = {.count = 2, .entries = entries};
    uint64_t dh = (uintptr_t)&pool;
    c = (Cpu){0}; c.x[0] = (uintptr_t)&dh; c.x[2] = (uintptr_t)&ah;
    register_texture(&c);
    c.x[1] = 1;
    register_texture(&c);
    register_texture(&c);  // replacing a slot with the same texture must not add a reference
    assert(a->refs == 3);
    finalize(&ah);
    finalize(&ah_copy);  // a shallow guest copy must not release ownership twice
    assert(!ah.host && !ah_copy.host && a->refs == 3 && a->owners == 0 && a->finalized);
    test_frame = 10;
    c.x[0] = (uintptr_t)&dh; c.x[1] = 0; c.x[2] = (uintptr_t)&bh;
    register_texture(&c);
    assert(a->refs == 2 && destroyed == 0 && entries[1].tex == a);
    test_frame = 20;
    hle_nvnTexturePoolFinalize(&c);
    assert(!dh && !entries[0].tex && !entries[1].tex && destroyed == 0);
    test_frame = 30;
    finalize(&bh);
    assert(destroyed == 1 && b->refs == 1);
    sweep();
    assert(destroyed == 3 && g_all_tex && !g_all_tex->next && g_nretired == 1);  // (the last scratch texture waits)
    textures_reset();
    assert(!g_all_tex && !g_retired && !g_nretired && !g_owner_used);

    // Objects with the same description share one host texture; each owner holds a reference of its own.
    destroyed = 0;
    GuestTexture o1, o2, o3;
    NvnTexture *t1 = init_texture(&o1, &ph, 0x4000, 128), *t2 = init_texture(&o2, &ph, 0x4000, 128);
    NvnTexture *t3 = init_texture(&o3, &ph, 0x4000, 64);  // (another size: its own texture)
    assert(t1 == t2 && t1 != t3 && t1->refs == 2 && t1->owners == 2);
    c = (Cpu){0}; c.x[0] = (uintptr_t)&o1; c.x[1] = (uintptr_t)&o3;
    hle_nvnTextureCompare(&c);
    assert(c.x[0] == 0);
    c.x[0] = (uintptr_t)&o1; c.x[1] = (uintptr_t)&o2;
    hle_nvnTextureCompare(&c);
    assert(c.x[0] == 1);
    finalize(&o1);  // the first owner goes: the second keeps the texture
    sweep();
    assert(destroyed == 1 && t1->refs == 1 && t1->owners == 1 && !t1->finalized);
    // An object initialized again with the same description keeps its texture; with another, the old one goes.
    init_texture(&o2, &ph, 0x4000, 128);
    assert(HOST(NvnTexture, &o2) == t1 && t1->owners == 1);
    sweep();
    assert(t1->refs == 1 && destroyed == 3);
    NvnTexture *t4 = init_texture(&o2, &ph, 0x8000, 128);
    assert(t4 != t1 && t1->owners == 0);
    sweep();
    assert(destroyed == 6);  // t1 and the scratch textures
    finalize(&o2);
    finalize(&o3);
    sweep();
    assert(destroyed == 10 && g_all_tex && !g_all_tex->next);  // (only the last scratch texture, still deferred)
    textures_reset();

    // nvnSamplerCompare must ignore backend pointer differences at the end of NvnSamplerDesc.
    NvnSamplerDesc sb = {0}, s1 = {0}, s2 = {0};
    c = (Cpu){0}; c.x[0] = (uintptr_t)&sb;
    hle_nvnSamplerBuilderSetDefaults(&c);
    c.x[0] = (uintptr_t)&s1; c.x[1] = (uintptr_t)&sb;
    hle_nvnSamplerInitialize(&c);
    c.x[0] = (uintptr_t)&s2; c.x[1] = (uintptr_t)&sb;
    hle_nvnSamplerInitialize(&c);
    s1.backend = (void *)0x1234;
    s2.backend = (void *)0x5678;
    c.x[0] = (uintptr_t)&s1; c.x[1] = (uintptr_t)&s2;
    hle_nvnSamplerCompare(&c);
    assert(c.x[0] == 1);

    puts("PASS: virtual pool alias/bounds, descriptor retention, copies and second finalizes, shared descriptions, "
         "initialize again, pool teardown, reset, compare");
}
