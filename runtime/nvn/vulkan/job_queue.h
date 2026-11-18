// Priority shader/pipeline worker job queue with work-stealing and active drain accounting.
// Included from runtime/nvn/vulkan/exec.c and tools/test_shader_job_queue.c.
#pragma once

#include <pthread.h>
#include <stdbool.h>
#include <stdlib.h>

typedef struct MwJob {
    void (*fn)(void *);
    void *arg;
    struct MwJob *next;
} MwJob;

static inline bool mw_job_push(pthread_mutex_t *lock, pthread_cond_t *cond, MwJob **head, MwJob **tail,
                               void (*fn)(void *), void *arg, bool urgent) {
    MwJob *j = (MwJob *)malloc(sizeof *j);
    if (!j) return false;
    j->fn = fn;
    j->arg = arg;
    j->next = NULL;
    pthread_mutex_lock(lock);
    if (urgent) {
        j->next = *head;
        *head = j;
        if (!*tail) *tail = j;
    } else {
        if (*tail) (*tail)->next = j;
        else *head = j;
        *tail = j;
    }
    pthread_cond_signal(cond);
    pthread_mutex_unlock(lock);
    return true;
}

static inline MwJob *mw_job_pop_active(pthread_mutex_t *lock, pthread_cond_t *cond, MwJob **head, MwJob **tail,
                                       int *active) {
    pthread_mutex_lock(lock);
    while (!*head) pthread_cond_wait(cond, lock);
    MwJob *j = *head;
    *head = j->next;
    if (!*head) *tail = NULL;
    if (active) (*active)++;
    pthread_mutex_unlock(lock);
    j->next = NULL;
    return j;
}

static inline bool mw_job_finish(pthread_mutex_t *lock, MwJob **head, int *active) {
    pthread_mutex_lock(lock);
    if (active) (*active)--;
    bool idle = !*head && (!active || *active == 0);
    pthread_mutex_unlock(lock);
    return idle;
}

static inline MwJob *mw_job_steal(pthread_mutex_t *lock, MwJob **head, MwJob **tail, const void *arg) {
    pthread_mutex_lock(lock);
    MwJob *prev = NULL, *cur = *head;
    while (cur && cur->arg != arg) {
        prev = cur;
        cur = cur->next;
    }
    if (cur) {
        if (prev) prev->next = cur->next;
        else *head = cur->next;
        if (!cur->next) *tail = prev;
        cur->next = NULL;
    }
    pthread_mutex_unlock(lock);
    return cur;
}
