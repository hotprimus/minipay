#include "threadpool.h"

#include <pthread.h>
#include <stdbool.h>
#include <stdlib.h>

struct threadpool {
    pthread_mutex_t lock;
    pthread_cond_t not_empty;
    pthread_cond_t not_full;
    int *queue;
    size_t cap, head, count;
    bool stopping;
    pthread_t *threads;
    size_t nthreads;
    tp_task_fn fn;
    void *ctx;
};

static void *worker(void *arg)
{
    threadpool *tp = arg;
    for (;;) {
        pthread_mutex_lock(&tp->lock);
        while (tp->count == 0 && !tp->stopping)
            pthread_cond_wait(&tp->not_empty, &tp->lock);
        if (tp->count == 0) {
            pthread_mutex_unlock(&tp->lock);
            return NULL;
        }
        int item = tp->queue[tp->head];
        tp->head = (tp->head + 1) % tp->cap;
        tp->count--;
        pthread_cond_signal(&tp->not_full);
        pthread_mutex_unlock(&tp->lock);

        tp->fn(item, tp->ctx);
    }
}

threadpool *tp_create(size_t nthreads, size_t queue_cap, tp_task_fn fn, void *ctx)
{
    if (nthreads == 0 || queue_cap == 0 || fn == NULL)
        return NULL;
    threadpool *tp = calloc(1, sizeof *tp);
    if (tp == NULL)
        return NULL;
    tp->queue = calloc(queue_cap, sizeof *tp->queue);
    tp->threads = calloc(nthreads, sizeof *tp->threads);
    if (tp->queue == NULL || tp->threads == NULL)
        goto fail_mem;
    if (pthread_mutex_init(&tp->lock, NULL) != 0)
        goto fail_mem;
    if (pthread_cond_init(&tp->not_empty, NULL) != 0)
        goto fail_lock;
    if (pthread_cond_init(&tp->not_full, NULL) != 0)
        goto fail_cond;
    tp->cap = queue_cap;
    tp->fn = fn;
    tp->ctx = ctx;

    for (size_t i = 0; i < nthreads; i++) {
        if (pthread_create(&tp->threads[i], NULL, worker, tp) != 0) {
            tp_shutdown(tp);
            return NULL;
        }
        tp->nthreads = i + 1;
    }
    return tp;

fail_cond:
    pthread_cond_destroy(&tp->not_empty);
fail_lock:
    pthread_mutex_destroy(&tp->lock);
fail_mem:
    free(tp->threads);
    free(tp->queue);
    free(tp);
    return NULL;
}

mp_status tp_submit(threadpool *tp, int item)
{
    if (tp == NULL)
        return MP_ERR_ARG;
    pthread_mutex_lock(&tp->lock);
    while (tp->count == tp->cap && !tp->stopping)
        pthread_cond_wait(&tp->not_full, &tp->lock);
    if (tp->stopping) {
        pthread_mutex_unlock(&tp->lock);
        return MP_ERR_CLOSED;
    }
    tp->queue[(tp->head + tp->count) % tp->cap] = item;
    tp->count++;
    pthread_cond_signal(&tp->not_empty);
    pthread_mutex_unlock(&tp->lock);
    return MP_OK;
}

void tp_shutdown(threadpool *tp)
{
    if (tp == NULL)
        return;
    pthread_mutex_lock(&tp->lock);
    tp->stopping = true;
    pthread_cond_broadcast(&tp->not_empty);
    pthread_cond_broadcast(&tp->not_full);
    pthread_mutex_unlock(&tp->lock);

    for (size_t i = 0; i < tp->nthreads; i++)
        pthread_join(tp->threads[i], NULL);

    pthread_cond_destroy(&tp->not_full);
    pthread_cond_destroy(&tp->not_empty);
    pthread_mutex_destroy(&tp->lock);
    free(tp->threads);
    free(tp->queue);
    free(tp);
}
