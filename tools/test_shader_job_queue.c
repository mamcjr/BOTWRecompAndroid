// Host/Bionic unit tests for priority shader/pipeline job queue, work-stealing, and active drain.
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

#include "nvn/vulkan/job_queue.h"

static void dummy_fn(void *arg) { (void)arg; }

typedef struct {
    _Atomic int count;
    int idx;
    bool forward_to_tjobs;
} RaceItem;

typedef struct {
    pthread_mutex_t *lock;
    pthread_cond_t *cond;
    MwJob **head;
    MwJob **tail;
    int *active;
    bool *stop;
    pthread_mutex_t *tlock;
    pthread_cond_t *tcond;
    MwJob **thead;
    MwJob **ttail;
    int *tactive;
    pthread_mutex_t *done_lock;
    pthread_cond_t *done_cond;
    int *completed;
    int *q_idle_events;
    int *tq_idle_events;
    int target;
} RaceCtx;

static void forward_fn(void *arg) {
    RaceItem *it = (RaceItem *)arg;
    int prev = atomic_fetch_add_explicit(&it->count, 1, memory_order_relaxed);
    assert(prev == 0);
}

static void race_job_body(RaceCtx *ctx, RaceItem *it) {
    if (it->forward_to_tjobs) {
        it->forward_to_tjobs = false;
        assert(mw_job_push(ctx->tlock, ctx->tcond, ctx->thead, ctx->ttail, forward_fn, it, (it->idx % 5) == 0));
    } else {
        int prev = atomic_fetch_add_explicit(&it->count, 1, memory_order_relaxed);
        assert(prev == 0);
        pthread_mutex_lock(ctx->done_lock);
        (*ctx->completed)++;
        if (*ctx->completed == ctx->target) pthread_cond_broadcast(ctx->done_cond);
        pthread_mutex_unlock(ctx->done_lock);
    }
}

typedef struct {
    RaceCtx *ctx;
    RaceItem *it;
} WrappedItem;

static void wrapped_fn(void *arg) {
    WrappedItem *w = (WrappedItem *)arg;
    race_job_body(w->ctx, w->it);
}

static void *tjob_worker(void *arg) {
    RaceCtx *ctx = (RaceCtx *)arg;
    for (;;) {
        pthread_mutex_lock(ctx->tlock);
        while (!*ctx->thead && !*ctx->stop) pthread_cond_wait(ctx->tcond, ctx->tlock);
        if (!*ctx->thead && *ctx->stop) {
            pthread_mutex_unlock(ctx->tlock);
            return NULL;
        }
        MwJob *j = *ctx->thead;
        *ctx->thead = j->next;
        if (!*ctx->thead) *ctx->ttail = NULL;
        (*ctx->tactive)++;
        pthread_mutex_unlock(ctx->tlock);
        j->fn(j->arg);
        free(j);
        pthread_mutex_lock(ctx->done_lock);
        (*ctx->completed)++;
        pthread_mutex_unlock(ctx->done_lock);
        bool idle = mw_job_finish(ctx->tlock, ctx->thead, ctx->tactive);
        if (idle) {
            pthread_mutex_lock(ctx->done_lock);
            (*ctx->tq_idle_events)++;
            pthread_cond_broadcast(ctx->done_cond);
            pthread_mutex_unlock(ctx->done_lock);
        }
    }
}

static void *race_worker(void *arg) {
    RaceCtx *ctx = (RaceCtx *)arg;
    for (;;) {
        pthread_mutex_lock(ctx->lock);
        while (!*ctx->head && !*ctx->stop) pthread_cond_wait(ctx->cond, ctx->lock);
        if (!*ctx->head && *ctx->stop) {
            pthread_mutex_unlock(ctx->lock);
            return NULL;
        }
        MwJob *j = *ctx->head;
        *ctx->head = j->next;
        if (!*ctx->head) *ctx->tail = NULL;
        (*ctx->active)++;
        pthread_mutex_unlock(ctx->lock);
        j->fn(j->arg);
        free(j);
        if (mw_job_finish(ctx->lock, ctx->head, ctx->active)) {
            pthread_mutex_lock(ctx->done_lock);
            (*ctx->q_idle_events)++;
            pthread_cond_broadcast(ctx->done_cond);
            pthread_mutex_unlock(ctx->done_lock);
        }
    }
}

int main(void) {
    pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
    pthread_cond_t cond = PTHREAD_COND_INITIALIZER;
    MwJob *head = NULL, *tail = NULL;
    int active = 0;

    int a = 1, b = 2, c = 3, d = 4, e1 = 5, e2 = 6, f = 7, missing = 99;

    assert(mw_job_steal(&lock, &head, &tail, &a) == NULL);
    assert(head == NULL && tail == NULL);

    // Urgent push on empty queue (tail must be set too), then non-urgent append & active > 1 finish behavior
    assert(mw_job_push(&lock, &cond, &head, &tail, dummy_fn, &a, true));
    assert(head != NULL && head == tail && head->arg == &a);
    assert(mw_job_push(&lock, &cond, &head, &tail, dummy_fn, &b, false));
    assert(head && head->arg == &a && tail && tail->arg == &b && head->next == tail);
    MwJob *p1 = mw_job_pop_active(&lock, &cond, &head, &tail, &active);
    MwJob *p2 = mw_job_pop_active(&lock, &cond, &head, &tail, &active);
    assert(p1 && p1->arg == &a && p2 && p2->arg == &b);
    assert(head == NULL && tail == NULL && active == 2);
    free(p1);
    free(p2);
    assert(mw_job_finish(&lock, &head, &active) == false && active == 1);
    assert(mw_job_finish(&lock, &head, &active) == true && active == 0);
    assert(head == NULL && tail == NULL);

    // 1-item push and steal
    assert(mw_job_push(&lock, &cond, &head, &tail, dummy_fn, &a, false));
    assert(mw_job_steal(&lock, &head, &tail, &missing) == NULL);
    MwJob *s = mw_job_steal(&lock, &head, &tail, &a);
    assert(s != NULL && s->arg == &a);
    free(s);
    assert(head == NULL && tail == NULL);

    // Steal head, middle, tail; then verify non-urgent append, multiple urgent prepends, and pop_active/finish
    assert(mw_job_push(&lock, &cond, &head, &tail, dummy_fn, &a, false));
    assert(mw_job_push(&lock, &cond, &head, &tail, dummy_fn, &b, false));
    assert(mw_job_push(&lock, &cond, &head, &tail, dummy_fn, &c, false));
    assert(mw_job_push(&lock, &cond, &head, &tail, dummy_fn, &d, false));
    // [a, b, c, d] -> steal a, c, d -> [b] with tail == b
    s = mw_job_steal(&lock, &head, &tail, &a);
    assert(s && s->arg == &a);
    free(s);
    s = mw_job_steal(&lock, &head, &tail, &c);
    assert(s && s->arg == &c);
    free(s);
    s = mw_job_steal(&lock, &head, &tail, &d);
    assert(s && s->arg == &d);
    free(s);
    assert(mw_job_push(&lock, &cond, &head, &tail, dummy_fn, &f, false));
    assert(mw_job_push(&lock, &cond, &head, &tail, dummy_fn, &e1, true));
    assert(mw_job_push(&lock, &cond, &head, &tail, dummy_fn, &e2, true));
    // Expected order: [e2, e1, b, f]
    int *expected[4] = {&e2, &e1, &b, &f};
    for (int k = 0; k < 4; k++) {
        s = mw_job_pop_active(&lock, &cond, &head, &tail, &active);
        assert(s && s->arg == expected[k]);
        assert(active == 1);
        free(s);
        bool idle = mw_job_finish(&lock, &head, &active);
        assert(idle == (k == 3));
    }
    assert(head == NULL && tail == NULL && active == 0);

    // Multi-threaded worker + stealer + two-queue forwarding + active-drain condvar race test
    enum { N_ITEMS = 600, N_WORKERS = 3 };
    RaceItem *items = (RaceItem *)calloc(N_ITEMS, sizeof *items);
    WrappedItem *wrapped = (WrappedItem *)calloc(N_ITEMS, sizeof *wrapped);
    assert(items && wrapped);

    pthread_mutex_t tlock = PTHREAD_MUTEX_INITIALIZER, dlock = PTHREAD_MUTEX_INITIALIZER;
    pthread_cond_t tcond = PTHREAD_COND_INITIALIZER, dcond = PTHREAD_COND_INITIALIZER;
    MwJob *thead = NULL, *ttail = NULL;
    int tactive = 0, completed = 0, q_idle_events = 0, tq_idle_events = 0;
    bool stop = false;
    RaceCtx ctx = {
        .lock = &lock, .cond = &cond, .head = &head, .tail = &tail, .active = &active, .stop = &stop,
        .tlock = &tlock, .tcond = &tcond, .thead = &thead, .ttail = &ttail, .tactive = &tactive,
        .done_lock = &dlock, .done_cond = &dcond, .completed = &completed,
        .q_idle_events = &q_idle_events, .tq_idle_events = &tq_idle_events, .target = N_ITEMS,
    };
    for (int i = 0; i < N_ITEMS; i++) {
        items[i].idx = i;
        items[i].forward_to_tjobs = (i % 4) == 0;
        wrapped[i] = (WrappedItem){.ctx = &ctx, .it = &items[i]};
    }

    pthread_t th[N_WORKERS], tth;
    for (int i = 0; i < N_WORKERS; i++) assert(pthread_create(&th[i], NULL, race_worker, &ctx) == 0);
    assert(pthread_create(&tth, NULL, tjob_worker, &ctx) == 0);

    for (int i = 0; i < N_ITEMS; i++) {
        assert(mw_job_push(&lock, &cond, &head, &tail, wrapped_fn, &wrapped[i], (i % 7) == 0));
        if ((i % 3) == 0) {
            int target = i > 4 ? i - 3 : i;
            MwJob *st = mw_job_steal(&lock, &head, &tail, &wrapped[target]);
            if (st) {
                st->fn(st->arg);
                free(st);
            }
        }
        if ((i % 8) == 0) {
            MwJob *tst = mw_job_steal(&tlock, &thead, &ttail, &items[i]);
            if (tst) {
                tst->fn(tst->arg);
                free(tst);
                pthread_mutex_lock(&dlock);
                completed++;
                pthread_cond_broadcast(&dcond);
                pthread_mutex_unlock(&dlock);
            }
        }
    }
    for (int i = 0; i < N_ITEMS; i++) {
        MwJob *st = mw_job_steal(&lock, &head, &tail, &wrapped[i]);
        if (st) {
            st->fn(st->arg);
            free(st);
        }
    }
    for (int i = 0; i < N_ITEMS; i += 4) {
        MwJob *tst = mw_job_steal(&tlock, &thead, &ttail, &items[i]);
        if (tst) {
            tst->fn(tst->arg);
            free(tst);
            pthread_mutex_lock(&dlock);
            completed++;
            pthread_cond_broadcast(&dcond);
            pthread_mutex_unlock(&dlock);
        }
    }

    // Drain-wait like jobs_drain(): wait until both queues report zero queued AND zero active workers.
    pthread_mutex_lock(&dlock);
    for (;;) {
        pthread_mutex_lock(&lock);
        pthread_mutex_lock(&tlock);
        bool both_idle = (completed == N_ITEMS && !head && !active && !thead && !tactive);
        pthread_mutex_unlock(&tlock);
        pthread_mutex_unlock(&lock);
        if (both_idle) break;
        pthread_cond_wait(&dcond, &dlock);
    }
    assert(q_idle_events > 0);
    pthread_mutex_unlock(&dlock);

    pthread_mutex_lock(&lock);
    pthread_mutex_lock(&tlock);
    stop = true;
    pthread_cond_broadcast(&cond);
    pthread_cond_broadcast(&tcond);
    pthread_mutex_unlock(&tlock);
    pthread_mutex_unlock(&lock);

    for (int i = 0; i < N_WORKERS; i++) pthread_join(th[i], NULL);
    pthread_join(tth, NULL);

    for (int i = 0; i < N_ITEMS; i++) assert(atomic_load_explicit(&items[i].count, memory_order_relaxed) == 1);
    free(wrapped);
    free(items);
    assert(head == NULL && tail == NULL && active == 0 && thead == NULL && ttail == NULL && tactive == 0);

    puts("PASS: test_shader_job_queue (urgent push/steal/active-drain)");
    return 0;
}
