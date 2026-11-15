#ifndef MP_LOG_H
#define MP_LOG_H

typedef enum { LVL_DEBUG = 0, LVL_INFO, LVL_WARN, LVL_ERROR } log_level;

void log_set_level(log_level lvl);

void log_write(log_level lvl, const char *file, int line, const char *fmt, ...)
    __attribute__((format(printf, 4, 5)));

void log_sys(const char *file, int line, const char *what);

#define LOG_DEBUG(...) log_write(LVL_DEBUG, __FILE__, __LINE__, __VA_ARGS__)
#define LOG_INFO(...)  log_write(LVL_INFO, __FILE__, __LINE__, __VA_ARGS__)
#define LOG_WARN(...)  log_write(LVL_WARN, __FILE__, __LINE__, __VA_ARGS__)
#define LOG_ERROR(...) log_write(LVL_ERROR, __FILE__, __LINE__, __VA_ARGS__)
#define LOG_SYS(what)  log_sys(__FILE__, __LINE__, (what))

#endif
