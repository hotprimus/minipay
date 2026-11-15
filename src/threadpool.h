#ifndef MP_THREADPOOL_H
#define MP_THREADPOOL_H

#include <stddef.h>

#include "common.h"

typedef void (*tp_task_fn)(int item, void *ctx);

typedef struct threadpool threadpool;

threadpool *tp_create(size_t nthreads, size_t queue_cap, tp_task_fn fn, void *ctx);

mp_status tp_submit(threadpool *tp, int item);

void tp_shutdown(threadpool *tp);

#endif
