// Small services: parental control, add-on content, and other mostly-stubbed interfaces.
#include <string.h>

#include "../hle.h"
#include "../ipc.h"

static void ok_logger(Service *s, IpcReq *req, IpcResp *resp) {
    LOG("[svc] %s cmd %u (ok)\n", s->name, req->cmd);
    resp_size(resp, 0x10);
}

static uint32_t new_event_handle(void) {
    uint32_t w, r;
    kernel_create_event_pair(&w, &r);
    return r;
}

// ------------------------------------------------------------------ pctl
static void pctl_service(Service *s, IpcReq *req, IpcResp *resp) {
    (void)s;
    switch (req->cmd) {
    case 1031: case 1035: case 1018: OUT(uint8_t, 0) = 0; resp_size(resp, 1); break;  // restrictions off
    case 1032: OUT(uint32_t, 0) = 0; resp_size(resp, 4); break;
    default: LOG("[pctl] IParentalControlService cmd %u (ok)\n", req->cmd); resp_size(resp, 0x10);
    }
}

static void pctl_root(Service *s, IpcReq *req, IpcResp *resp) {
    (void)s;
    if (req->cmd == 0 || req->cmd == 1)
        ipc_return_object(resp, service_new("IParentalControlService", pctl_service, NULL));
}
SERVICE("pctl", pctl) { pctl_root(s, req, resp); }
SERVICE("pctl:s", pctl_s) { pctl_root(s, req, resp); }
SERVICE("pctl:a", pctl_a) { pctl_root(s, req, resp); }
SERVICE("pctl:r", pctl_r) { pctl_root(s, req, resp); }

// ------------------------------------------------------------------ aoc:u (the DLC)
// BOTW 1.6.0 / 1.8.2 list their add-on contents and go on only with two there (main 0xfb8540 in 1.8.2): index 1, the
// Master Trials - the data of both packs, mounted as aoc: (services/fs.c) - and 2, the Champions' Ballad (its unlock,
// no data of its own). Both listed once the DLC's data image is set up (fs_aoc_image), else none.
const char *fs_aoc_image(void);
SERVICE("aoc:u", aoc_u) {
    (void)s;
    static const uint32_t packs[] = {1, 2};
    uint32_t have = fs_aoc_image() && g_game_version > 100 ? 2 : 0;  // (1.0.0 came before the DLC)
    switch (req->cmd) {
    case 2: OUT(uint32_t, 0) = have; resp_size(resp, 4); break;  // CountAddOnContent
    case 3: {  // ListAddOnContent(offset, count): the packs' indices into the B buffer, how many
        uint32_t off = IN(uint32_t, 0), count = IN(uint32_t, 4), n = 0;
        IpcBuf *b = out_buf(req, 0);
        for (uint32_t i = off; b && i < have && n < count && (n + 1) * 4 <= b->size; i++)
            ((uint32_t *)(uintptr_t)b->addr)[n++] = packs[i];
        OUT(uint32_t, 0) = n;
        resp_size(resp, 4);
        LOG("[aoc] ListAddOnContent: %u of the DLC's packs\n", n);
        break;
    }
    case 5: OUT(uint64_t, 0) = 0x01007EF00011F000ull; resp_size(resp, 8); break;  // GetAddOnContentBaseId (+ index)
    case 7: break;                                                                  // PrepareAddOnContent
    case 8: case 10: resp->copy_handles[resp->ncopy++] = new_event_handle(); break;  // ...ListChangedEvent(WithProcessId)
    case 11: case 12: case 50: break;  // Notify(Un)MountAddOnContent, CheckAddOnContentMountStatus
    default: ok_logger(s, req, resp);
    }
}

// (nfp:user, amiibo: services/nfp.c)
