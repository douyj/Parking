#ifndef LOG_H
#define LOG_H

typedef enum {
    LOG_LEVEL_DEBUG = 0,
    LOG_LEVEL_INFO,
    LOG_LEVEL_WARN,
    LOG_LEVEL_ERROR
} LogLevel;

void log_set_level(LogLevel level);
LogLevel log_get_level(void);

void log_set_show_detail(int enable);
int log_get_show_detail(void);

/* 设置是否启用终端彩色输出，默认启用 */
void log_set_color(int enable);
int log_get_color(void);

void log_print(
    LogLevel level,
    const char *file,
    int line,
    const char *func,
    const char *fmt,
    ...
);

LogLevel log_level_from_string(const char *str);

#define LOG_DEBUG(fmt, ...) \
    log_print(LOG_LEVEL_DEBUG, __FILE__, __LINE__, __func__, fmt, ##__VA_ARGS__)

#define LOG_INFO(fmt, ...) \
    log_print(LOG_LEVEL_INFO, __FILE__, __LINE__, __func__, fmt, ##__VA_ARGS__)

#define LOG_WARN(fmt, ...) \
    log_print(LOG_LEVEL_WARN, __FILE__, __LINE__, __func__, fmt, ##__VA_ARGS__)

#define LOG_ERROR(fmt, ...) \
    log_print(LOG_LEVEL_ERROR, __FILE__, __LINE__, __func__, fmt, ##__VA_ARGS__)

#endif
