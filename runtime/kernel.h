// Kernel object types shared by kernel.c and the IPC/service layer.
#pragma once
#include <setjmp.h>

#include "rt.h"

#ifdef __ANDROID__
#define STACK_BASE (GUEST_LO + 0x0200000000ull)  // (rt.h: the window moves on Android)
#else
#define STACK_BASE 0x7200000000ull
#endif
#define STACK_SIZE 0x0040000000ull

typedef enum { KOBJ_THREAD = 1, KOBJ_EVENT, KOBJ_SESSION, KOBJ_SHMEM, KOBJ_TMEM } KType;
typedef enum { WAIT_NONE = 0, WAIT_MUTEX, WAIT_CONDVAR, WAIT_ARBITER } WaitKind;

typedef struct KObject {
    KType type;
    int refs;
    bool signaled;
} KObject;

typedef KObject KEvent;

struct KThread {
    KObject obj;  // signaled when the thread exits
    Cpu cpu;
    pthread_t host;
    uint64_t entry, tlr, id;
    uint32_t handle;
    int prio, core;        // core: current virtual core, accessed atomically
    int ideal_core;        // may be -1 (no preference); g_klock
    uint64_t core_mask;    // allowed virtual cores; g_klock
    bool cancel, in_sync_wait;
    // wait state (protected by g_klock)
    WaitKind wait_kind;
    uint64_t wait_addr, wait_mutex;
    uint32_t wait_tag, wait_result;
    KThread *wnext;
    KThread *snext;        // in WaitSynchronization (g_syncwaiters)
    pthread_cond_t cond;   // each thread sleeps on its own condition; wakers signal it directly
    // in-place reboot (kernel_stop_threads): thread_main's setjmp point, where the thread unwinds to and ends
    jmp_buf unwind;
    bool started, finished;
    KThread *anext;        // every thread of this boot (g_klock)
    // the svc in progress (LARECOMP_WATCHDOG, kernel.c): number + 1 (0: none), since when, its arguments and caller
    volatile uint32_t svc_now;
    volatile uint64_t svc_since, svc_args[4], svc_lr;
#ifdef __ANDROID__
    void *dcache;          // dispatch.c: this thread's guest -> host function cache (TLS is slow in a dlopened library)
    bool core0_held;       // kernel.c: this thread has core 0 (it runs; the others of core 0 wait)
    bool core0_evicted;    // atomic request to reconcile core ownership at the next guest safe point
    KThread *core0_next;   // core-0 runnable queue (c0_m)
#endif
};

void kernel_preempt_point(void);

typedef struct KSharedMem {
    KObject obj;
    uint64_t size, guest_addr;  // guest_addr: where the guest mapped it (0 if not mapped)
    void (*on_map)(struct KSharedMem *s);  // HLE owner fills initial contents
    void *owner;
} KSharedMem;

KSharedMem *kernel_create_shared_memory(uint64_t size, void (*on_map)(KSharedMem *), uint32_t *handle);

typedef struct {
    KObject obj;
    uint64_t addr, size;
} KTransferMem;

extern pthread_mutex_t g_klock;
extern KThread *g_waitlist;

KObject *handle_get(uint32_t h);
uint32_t kernel_new_handle(KObject *o);
void kernel_signal(KObject *o);  // caller holds g_klock
KEvent *kernel_create_event_pair(uint32_t *writable, uint32_t *readable);
KThread *kernel_current_thread(void);
uint64_t kernel_heap_size(void);  // bytes from HEAP_BASE set by SetHeapSize
