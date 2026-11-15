#include "log.h"

#include <errno.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

static atomic_int g_level = LVL_INFO;

static atomic_uint g_next_thread_no = 1;
static _Thread_local unsigned t_thread_no;

static const char *const LEVEL_NAMES[] = {"DEBUG", "INFO", "WARN", "ERROR"};

void log_set_level(log_level lvl)
{
    atomic_store(&g_level, (int)lvl);
}

static const char *basename_of(const char *path)
{
    const char *slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

static void vlog(log_level lvl, const char *file, int line, const char *fmt, va_list ap)
{
    if ((int)lvl < atomic_load(&g_level))
        return;
    if (t_thread_no == 0)
        t_thread_no = atomic_fetch_add(&g_next_thread_no, 1u);

    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm tm;
    localtime_r(&ts.tv_sec, &tm);

    char when[32];
    strftime(when, sizeof when, "%Y-%m-%d %H:%M:%S", &tm);

    char msg[1024];
    vsnprintf(msg, sizeof msg, fmt, ap);

    fprintf(stderr, "%s.%03ld %-5s [t%u] %s:%d %s\n", when, ts.tv_nsec / 1000000L,
            LEVEL_NAMES[lvl], t_thread_no, basename_of(file), line, msg);
}

void log_write(log_level lvl, const char *file, int line, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vlog(lvl, file, line, fmt, ap);
    va_end(ap);
}

void log_sys(const char *file, int line, const char *what)
{
    int saved = errno;
    char buf[128];
    if (strerror_r(saved, buf, sizeof buf) != 0)
        snprintf(buf, sizeof buf, "errno %d", saved);
    log_write(LVL_ERROR, file, line, "%s: %s", what, buf);
    errno = saved;
}
