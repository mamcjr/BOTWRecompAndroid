// nfp:user (amiibo): a virtual amiibo reader. The player picks a figure in the menu (Controls > amiibo, ui/menu.mm) and
// "touches" it while the game looks for one (the amiibo rune): the tag is made from the figure's 8-byte id alone - its
// model info (what BOTW keys its drops on), a stable 7-byte UID per figure, no keys and no dumps. An application area
// (the Wolf Link figure's, 0x1019C800) lives in one small file per figure under the save folder; BOTW only reads it, so
// a figure starts without one and Wolf Link comes with 3 hearts. Protocol: switchbrew's NFC services page and Ryujinx's
// VirtualAmiibo / INfp (MIT) for the behaviour games expect; ids from AmiiboAPI. #181 #312 #403 #486.
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "../hle.h"
#include "../ipc.h"
#include "../kernel.h"

extern const char *g_save_dir;  // services/fs.c

typedef struct { uint64_t id; const char *name; } AmiiboEntry;
// id = character id 16 | variant 8 | type 8 : model number 16 | series 8 | 0x02 (AmiiboAPI)
static const AmiiboEntry k_amiibo[] = {
    {0x01030000024F0902ull, "Midna & Wolf Link"},
    {0x01000000034D0902ull, "Link - Twilight Princess"},
    {0x0100000000040002ull, "Link (Super Smash Bros.)"},
    {0x01000000034B0902ull, "Link - Ocarina of Time"},
    {0x01000000034C0902ull, "Link - Majora's Mask"},
    {0x01000000034E0902ull, "Link - Skyward Sword"},
    {0x01000000034F0902ull, "8-Bit Link"},
    {0x0100010003500902ull, "Toon Link - The Wind Waker"},
    {0x0101000003520902ull, "Toon Zelda - The Wind Waker"},
    {0x0100000003530902ull, "Link - Archer"},
    {0x0100000003540902ull, "Link - Rider"},
    {0x0101000003560902ull, "Zelda"},
    {0x0105000003580902ull, "Daruk"},
    {0x0106000003590902ull, "Urbosa"},
    {0x01070000035A0902ull, "Mipha"},
    {0x01080000035B0902ull, "Revali"},
    {0x0140000003550902ull, "Guardian"},
    {0x01410000035C0902ull, "Bokoblin"},
    {0x0100000003990902ull, "Link - Link's Awakening"},
    {0x0101030004140902ull, "Zelda & Loftwing"},
    {0x0100000004180902ull, "Link - Tears of the Kingdom"},
    {0x0101000004190902ull, "Zelda - Tears of the Kingdom"},
    {0x01020100041A0902ull, "Ganondorf - Tears of the Kingdom"},
    {0x01000000037C0002ull, "Young Link (Super Smash Bros.)"},
    {0x0100010000160002ull, "Toon Link (Super Smash Bros.)"},
    {0x01010000000E0002ull, "Zelda (Super Smash Bros.)"},
    {0x0101010000170002ull, "Sheik (Super Smash Bros.)"},
    {0x01020100001B0002ull, "Ganondorf (Super Smash Bros.)"},
    {0x0000000000000002ull, "Mario (Super Smash Bros.)"},
    {0x0000000000340102ull, "Mario (Super Mario)"},
    {0x0005000000140002ull, "Bowser (Super Smash Bros.)"},
    {0x1F000000000A0002ull, "Kirby (Super Smash Bros.)"},
    {0x1919000000090002ull, "Pikachu (Super Smash Bros.)"},
    {0x05C0000000060002ull, "Samus (Super Smash Bros.)"},
    {0x08000100003E0402ull, "Inkling Girl (Splatoon)"},
    {0x01810000037D0002ull, "Isabelle (Super Smash Bros.)"},
};
#define N_AMIIBO ((int)(sizeof k_amiibo / sizeof k_amiibo[0]))

#define NFP_RESULT(d) MAKE_RESULT(115, d)
enum { R_DEVICE_NOT_FOUND = 64, R_WRONG_ARGUMENT = 65, R_WRONG_STATE = 73, R_TAG_NOT_FOUND = 97,
       R_AREA_NULL = 128, R_AREA_EXISTS = 168 };
enum { DS_INITIALIZED, DS_SEARCHING, DS_TAG_FOUND, DS_TAG_REMOVED, DS_MOUNTED, DS_UNAVAILABLE };
#define AREA_SIZE 0xD8

// One figure's file (save/amiibo/<id>.bin), 0x100 bytes, little-endian
typedef struct {
    char magic[4];  // "AMBO"
    uint32_t version;
    uint64_t id;
    uint8_t uuid_len, uuid[7];
    uint16_t first_y; uint8_t first_m, first_d;
    uint16_t last_y; uint8_t last_m, last_d;
    uint16_t write_counter;
    uint8_t has_area, pad;
    uint32_t area_id;
    uint8_t area[AREA_SIZE];
} AmiiboFile;
_Static_assert(sizeof(AmiiboFile) == 0x100, "AmiiboFile");

static struct {
    pthread_mutex_t mtx;
    int svc_state;           // 0 not initialized, 1 initialized
    int dev_state;           // DS_*
    KEvent *activate, *deactivate;
    int pending;             // the figure touched by the player (k_amiibo index), -1 none
    int current;             // the figure on the reader now, -1 none
    bool area_open;
    AmiiboFile file;
} nfp = {PTHREAD_MUTEX_INITIALIZER, 0, DS_INITIALIZED, NULL, NULL, -1, -1, false, {{0}}};

RT_ON_RESET(nfp_reset) {  // (reboot: the handle table is new; the reader starts over)
    pthread_mutex_lock(&nfp.mtx);
    nfp.svc_state = 0, nfp.dev_state = DS_INITIALIZED;
    nfp.activate = nfp.deactivate = NULL;
    nfp.pending = nfp.current = -1;
    nfp.area_open = false;
    pthread_mutex_unlock(&nfp.mtx);
}

static void signal_event(KEvent *e) {
    if (!e) return;
    pthread_mutex_lock(&g_klock);
    kernel_signal(e);
    pthread_mutex_unlock(&g_klock);
}

static void today(uint16_t *y, uint8_t *m, uint8_t *d) {
    time_t t = time(NULL);
    struct tm tm;
    localtime_r(&t, &tm);
    *y = (uint16_t)(tm.tm_year + 1900), *m = (uint8_t)(tm.tm_mon + 1), *d = (uint8_t)tm.tm_mday;
}

static void file_path(char *out, size_t n, uint64_t id) {
    snprintf(out, n, "%s/amiibo/%016llX.bin", g_save_dir, (unsigned long long)id);
}

// (nfp.mtx held) the figure's file, or a new record: today's dates, a random UID starting 0x04 (NXP), no area
static void file_load(uint64_t id) {
    char p[512];
    file_path(p, sizeof p, id);
    FILE *f = fopen(p, "rb");
    AmiiboFile a;
    bool ok = f && fread(&a, sizeof a, 1, f) == 1 && !memcmp(a.magic, "AMBO", 4) && a.version == 1 && a.id == id;
    if (f) fclose(f);
    if (ok) { nfp.file = a; return; }
    memset(&a, 0, sizeof a);
    memcpy(a.magic, "AMBO", 4);
    a.version = 1, a.id = id, a.uuid_len = 7, a.uuid[0] = 0x04;
    srand((unsigned)time(NULL) ^ (unsigned)id ^ (unsigned)(id >> 32));
    for (int i = 1; i < 7; i++) a.uuid[i] = (uint8_t)(rand() >> 7);
    today(&a.first_y, &a.first_m, &a.first_d);
    a.last_y = a.first_y, a.last_m = a.first_m, a.last_d = a.first_d;
    nfp.file = a;
}

// (nfp.mtx held) written whole, through a temporary file and a rename
static void file_save(void) {
    char dir[480], p[512], tmp[520];
    snprintf(dir, sizeof dir, "%s/amiibo", g_save_dir);
    mkdir(dir, 0777);
    file_path(p, sizeof p, nfp.file.id);
    snprintf(tmp, sizeof tmp, "%s.tmp", p);
    FILE *f = fopen(tmp, "wb");
    if (!f) { LOG("[nfp] can't write %s\n", tmp); return; }
    bool ok = fwrite(&nfp.file, sizeof nfp.file, 1, f) == 1;
    ok = fclose(f) == 0 && ok;
    if (ok) rename(tmp, p);
}

static void file_written(void) {  // (nfp.mtx held) a write to the tag
    nfp.file.write_counter++;
    today(&nfp.file.last_y, &nfp.file.last_m, &nfp.file.last_d);
    file_save();
}

// (nfp.mtx held) the figure goes on the reader: TagFound, and the activate event to signal (returned)
static KEvent *tag_arrive(int i) {
    nfp.current = i, nfp.pending = -1;
    nfp.dev_state = DS_TAG_FOUND;
    nfp.area_open = false;
    file_load(k_amiibo[i].id);
    LOG("[nfp] amiibo on the reader: %s (%016llX)\n", k_amiibo[i].name, (unsigned long long)k_amiibo[i].id);
    return nfp.activate;
}

static uint32_t new_event(KEvent **out) {
    uint32_t w, r;
    *out = kernel_create_event_pair(&w, &r);
    return r;
}

static void fill_model_info(uint8_t *o, uint64_t id) {
    o[0] = (uint8_t)(id >> 56), o[1] = (uint8_t)(id >> 48);  // character id, as in the id
    o[2] = (uint8_t)(id >> 40);                                // variant
    o[3] = (uint8_t)(id >> 8);                                 // series
    uint16_t model = (uint16_t)(id >> 16);
    o[4] = (uint8_t)model, o[5] = (uint8_t)(model >> 8);       // model number, little-endian
    o[6] = (uint8_t)(id >> 32);                                // type (figure, card...)
}

static void nickname(char *out, int n, const char *name) {  // (up to 10 characters, without "(Super Smash Bros.)")
    int k = 0;
    for (; name[k] && name[k] != '(' && k < n - 1 && k < 10; k++) out[k] = name[k];
    while (k > 0 && out[k - 1] == ' ') k--;
    out[k] = 0;
}

static void nfp_user(Service *s, IpcReq *req, IpcResp *resp) {
    (void)s;
    uint32_t cmd = req->cmd;
    static int nlog;
    if (cmd != 19 && cmd != 20 && nlog < 200) nlog++, LOG("[nfp] IUser cmd %u (device state %d)\n", cmd, nfp.dev_state);
    KEvent *to_signal = NULL;
    pthread_mutex_lock(&nfp.mtx);
    bool mounted = nfp.dev_state == DS_MOUNTED, removed = nfp.dev_state == DS_TAG_REMOVED;
#define FAIL(d) do { resp->result = NFP_RESULT(d); goto done; } while (0)
#define NEED_MOUNT() do { if (removed) FAIL(R_TAG_NOT_FOUND); if (!mounted) FAIL(R_WRONG_STATE); } while (0)
    switch (cmd) {
    case 0:  // Initialize
        nfp.svc_state = 1, nfp.dev_state = DS_INITIALIZED, nfp.current = -1, nfp.area_open = false;
        break;
    case 1:  // Finalize
        nfp.svc_state = 0, nfp.dev_state = DS_INITIALIZED, nfp.current = -1, nfp.area_open = false;
        break;
    case 2: {  // ListDevices: the one virtual reader, handle 0
        IpcBuf *b = out_buf(req, 0);
        if (!nfp.svc_state) FAIL(R_DEVICE_NOT_FOUND);
        if (b && b->size >= 8) { memset((void *)(uintptr_t)b->addr, 0, b->size); *(uint64_t *)(uintptr_t)b->addr = 0; }
        OUT(uint32_t, 0) = 1;
        resp_size(resp, 4);
        break;
    }
    case 3:  // StartDetection: a figure already touched goes on at once
        nfp.dev_state = DS_SEARCHING, nfp.current = -1;
        if (nfp.pending >= 0) to_signal = tag_arrive(nfp.pending);
        break;
    case 4:  // StopDetection: the figure is lifted
        if (nfp.dev_state == DS_TAG_FOUND || nfp.dev_state == DS_MOUNTED) to_signal = nfp.deactivate;
        nfp.dev_state = DS_INITIALIZED, nfp.current = -1, nfp.area_open = false;
        break;
    case 5: {  // Mount(handle, model type, target)
        uint32_t model = IN(uint32_t, 8), target = IN(uint32_t, 12);
        if (model != 0 || !(target & 3)) FAIL(R_WRONG_ARGUMENT);
        if (removed) FAIL(R_TAG_NOT_FOUND);
        if (nfp.dev_state != DS_TAG_FOUND) FAIL(R_WRONG_STATE);
        nfp.dev_state = DS_MOUNTED;
        if (!nfp.file.first_y) file_load(k_amiibo[nfp.current].id);
        file_save();  // (the UID sticks)
        break;
    }
    case 6:  // Unmount
        if (removed) FAIL(R_TAG_NOT_FOUND);
        if (mounted) nfp.dev_state = DS_TAG_FOUND;
        nfp.area_open = false;
        break;
    case 7: {  // OpenApplicationArea(handle, access id)
        NEED_MOUNT();
        uint32_t aid = IN(uint32_t, 8);
        LOG("[nfp] OpenApplicationArea %#x: %s\n", aid, nfp.file.has_area && nfp.file.area_id == aid ? "opened" : "none");
        if (!nfp.file.has_area || nfp.file.area_id != aid) FAIL(R_AREA_NULL);
        nfp.area_open = true;
        break;
    }
    case 8: {  // GetApplicationArea -> size
        NEED_MOUNT();
        if (!nfp.area_open) FAIL(R_AREA_NULL);
        IpcBuf *b = out_buf(req, 0);
        if (!b) FAIL(R_WRONG_ARGUMENT);
        uint64_t n = b->size < AREA_SIZE ? b->size : AREA_SIZE;
        memset((void *)(uintptr_t)b->addr, 0, b->size);
        memcpy((void *)(uintptr_t)b->addr, nfp.file.area, n);
        OUT(uint32_t, 0) = AREA_SIZE;
        resp_size(resp, 4);
        break;
    }
    case 9: {  // SetApplicationArea
        NEED_MOUNT();
        if (!nfp.area_open) FAIL(R_AREA_NULL);
        IpcBuf *b = in_buf(req, 0);
        if (!b) FAIL(R_WRONG_ARGUMENT);
        uint64_t n = b->size < AREA_SIZE ? b->size : AREA_SIZE;
        memset(nfp.file.area, 0, AREA_SIZE);
        memcpy(nfp.file.area, (const void *)(uintptr_t)b->addr, n);
        break;
    }
    case 10:  // Flush
        NEED_MOUNT();
        file_written();
        break;
    case 11:  // Restore: nothing to restore
        break;
    case 12: case 24: {  // CreateApplicationArea / RecreateApplicationArea(handle, access id, data)
        NEED_MOUNT();
        if (cmd == 12 && nfp.file.has_area) FAIL(R_AREA_EXISTS);
        IpcBuf *b = in_buf(req, 0);
        uint64_t n = b ? (b->size < AREA_SIZE ? b->size : AREA_SIZE) : 0;
        nfp.file.has_area = 1, nfp.file.area_id = IN(uint32_t, 8);
        memset(nfp.file.area, 0, AREA_SIZE);
        if (n) memcpy(nfp.file.area, (const void *)(uintptr_t)b->addr, n);
        LOG("[nfp] %s application area %#x\n", cmd == 12 ? "created" : "recreated", nfp.file.area_id);
        file_written();
        break;
    }
    case 13: {  // GetTagInfo (0x58)
        IpcBuf *b = out_buf(req, 0);
        if (!b || b->size < 0x58) FAIL(R_WRONG_ARGUMENT);
        if (removed) FAIL(R_TAG_NOT_FOUND);
        if (nfp.dev_state != DS_TAG_FOUND && !mounted) FAIL(R_WRONG_STATE);
        uint8_t *o = (uint8_t *)(uintptr_t)b->addr;
        memset(o, 0, 0x58);
        memcpy(o, nfp.file.uuid, 7);
        o[0x0A] = 7;
        *(uint32_t *)(o + 0x20) = 0xFFFFFFFFu;  // protocol, tag type: "any" (as Ryujinx)
        *(uint32_t *)(o + 0x24) = 0xFFFFFFFFu;
        break;
    }
    case 14: {  // GetRegisterInfo (0x100): no owner Mii, the figure's name
        NEED_MOUNT();
        IpcBuf *b = out_buf(req, 0);
        if (!b || b->size < 0x100) FAIL(R_WRONG_ARGUMENT);
        uint8_t *o = (uint8_t *)(uintptr_t)b->addr;
        memset(o, 0, 0x100);
        *(uint16_t *)(o + 0x58) = nfp.file.first_y, o[0x5A] = nfp.file.first_m, o[0x5B] = nfp.file.first_d;
        nickname((char *)o + 0x5C, 0x29, k_amiibo[nfp.current].name);
        break;
    }
    case 15: {  // GetCommonInfo (0x40)
        NEED_MOUNT();
        IpcBuf *b = out_buf(req, 0);
        if (!b || b->size < 0x40) FAIL(R_WRONG_ARGUMENT);
        uint8_t *o = (uint8_t *)(uintptr_t)b->addr;
        memset(o, 0, 0x40);
        *(uint16_t *)o = nfp.file.last_y, o[2] = nfp.file.last_m, o[3] = nfp.file.last_d;
        *(uint16_t *)(o + 4) = nfp.file.write_counter;
        o[6] = 1;
        *(uint32_t *)(o + 8) = AREA_SIZE;
        break;
    }
    case 16: {  // GetModelInfo (0x40): what BOTW keys the drop on
        NEED_MOUNT();
        IpcBuf *b = out_buf(req, 0);
        if (!b || b->size < 0x40) FAIL(R_WRONG_ARGUMENT);
        uint8_t *o = (uint8_t *)(uintptr_t)b->addr;
        memset(o, 0, 0x40);
        fill_model_info(o, nfp.file.id);
        break;
    }
    case 17: resp->copy_handles[resp->ncopy++] = new_event(&nfp.activate); break;    // AttachActivateEvent
    case 18: resp->copy_handles[resp->ncopy++] = new_event(&nfp.deactivate); break;  // AttachDeactivateEvent
    case 19: OUT(uint32_t, 0) = (uint32_t)nfp.svc_state; resp_size(resp, 4); break;  // GetState
    case 20: OUT(uint32_t, 0) = (uint32_t)nfp.dev_state; resp_size(resp, 4); break;  // GetDeviceState
    case 21: OUT(uint32_t, 0) = 0; resp_size(resp, 4); break;                         // GetNpadId: player 1
    case 22: OUT(uint32_t, 0) = AREA_SIZE; resp_size(resp, 4); break;                 // GetApplicationAreaSize
    case 23: {  // AttachAvailabilityChangeEvent
        KEvent *e;
        resp->copy_handles[resp->ncopy++] = new_event(&e);
        break;
    }
    default:
        LOG("[nfp] IUser cmd %u (not handled, ok)\n", cmd);
        resp_size(resp, 0x10);
    }
done:
#undef FAIL
#undef NEED_MOUNT
    pthread_mutex_unlock(&nfp.mtx);
    if (resp->result) LOG("[nfp] IUser cmd %u -> %#x\n", cmd, resp->result);
    signal_event(to_signal);
}

SERVICE("nfp:user", nfp_user_manager) {
    (void)s;
    if (req->cmd == 0) ipc_return_object(resp, service_new("nfp:IUser", nfp_user, NULL));  // CreateUserInterface
    else LOG("[nfp] nfp:user cmd %u (ok)\n", req->cmd);
}

// ---- the menu's side (ui/menu.mm)
int nfp_amiibo_count(void) { return N_AMIIBO; }
const char *nfp_amiibo_name(int i) { return i >= 0 && i < N_AMIIBO ? k_amiibo[i].name : ""; }

// 0 the game isn't using amiibo now, 1 it's looking for one, 2 a figure is on the reader (found or read)
int nfp_reader_state(void) {
    pthread_mutex_lock(&nfp.mtx);
    int s = !nfp.svc_state ? 0 : nfp.dev_state == DS_SEARCHING ? 1
          : nfp.dev_state == DS_TAG_FOUND || nfp.dev_state == DS_MOUNTED ? 2 : 0;
    pthread_mutex_unlock(&nfp.mtx);
    return s;
}

// The player touches figure i while the game looks for one (the menu shows the button only then)
void nfp_touch(int i) {
    if (i < 0 || i >= N_AMIIBO) return;
    KEvent *e = NULL;
    pthread_mutex_lock(&nfp.mtx);
    if (nfp.dev_state == DS_SEARCHING) e = tag_arrive(i);  // (not before: the game may look while the rune aims)
    pthread_mutex_unlock(&nfp.mtx);
    signal_event(e);
}

// The player lifts the figure
void nfp_lift(void) {
    KEvent *e = NULL;
    pthread_mutex_lock(&nfp.mtx);
    nfp.pending = -1;
    if (nfp.dev_state == DS_TAG_FOUND || nfp.dev_state == DS_MOUNTED) {
        nfp.dev_state = DS_TAG_REMOVED, nfp.area_open = false, e = nfp.deactivate;
    }
    pthread_mutex_unlock(&nfp.mtx);
    signal_event(e);
}
