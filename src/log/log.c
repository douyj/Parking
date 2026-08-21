#include "log.h"

#include <stdio.h>
#include <stdarg.h>
#include <time.h>
#include <string.h>
#include <unistd.h>

static LogLevel g_log_level = LOG_LEVEL_INFO;

/*
 * 0: 简洁模式，只显示 [time] [level] message
 * 1: 详细模式，显示 file:line func()
 */
static int g_show_detail = 0;
static int g_color_enabled = 1;

#define COLOR_RESET  "\033[0m"
#define COLOR_CYAN   "\033[36m"
#define COLOR_GREEN  "\033[32m"
#define COLOR_YELLOW "\033[33m"
#define COLOR_RED    "\033[31m"

static const char *level_to_str(LogLevel level)
{
    switch (level)
    {
        case LOG_LEVEL_DEBUG:
            return "DEBUG";

        case LOG_LEVEL_INFO:
            return "INFO";

        case LOG_LEVEL_WARN:
            return "WARN";

        case LOG_LEVEL_ERROR:
            return "ERROR";

        default:
            return "UNKNOWN";
    }
}

static const char *level_to_color(LogLevel level)
{
    switch (level)
    {
        case LOG_LEVEL_DEBUG:
            return COLOR_CYAN;
        case LOG_LEVEL_INFO:
            return COLOR_GREEN;
        case LOG_LEVEL_WARN:
            return COLOR_YELLOW;
        case LOG_LEVEL_ERROR:
            return COLOR_RED;
        default:
            return COLOR_RESET;
    }
}

static const char *get_base_name(const char *path)
{
    const char *p1;
    const char *p2;
    const char *p;

    if (path == NULL) {
        return "unknown";
    }

    p1 = strrchr(path, '/');
    p2 = strrchr(path, '\\');

    if (p1 > p2) {
        p = p1;
    } else {
        p = p2;
    }

    if (p == NULL) {
        return path;
    }

    return p + 1;
}

/* 设置日志等级 */
void log_set_level(LogLevel level)
{
    g_log_level = level;
}

/* 获取日志等级 */
LogLevel log_get_level(void)
{
    return g_log_level;
}

/* 设置是否显示详细日志 */
void log_set_show_detail(int enable)
{
    g_show_detail = enable;
}

/* 获取是否显示详细日志 */
int log_get_show_detail(void)
{
    return g_show_detail;
}

void log_set_color(int enable)
{
    g_color_enabled = enable != 0;
}

int log_get_color(void)
{
    return g_color_enabled;
}

void log_print(LogLevel level, const char *file, int line, const char *func, const char *fmt, ...)
{
    int use_color;

    if (level < g_log_level)
    {
        return;
    }

    time_t now = time(NULL);
    struct tm *tm_info = localtime(&now);

    char time_buf[32] = {0};
    use_color = g_color_enabled && isatty(fileno(stderr));

    if (tm_info != NULL) {
        strftime(time_buf, sizeof(time_buf), "%H:%M:%S", tm_info);
    } else {
        snprintf(time_buf, sizeof(time_buf), "unknown");
    }

    if (g_show_detail) {
        fprintf(
            stderr,
            "[%s] %s[%s]%s %s:%d %s() | ",
            time_buf,
            use_color ? level_to_color(level) : "",
            level_to_str(level),
            use_color ? COLOR_RESET : "",
            get_base_name(file),
            line,
            func
        );
    } else {
        fprintf(
            stderr,
            "[%s] %s[%s]%s ",
            time_buf,
            use_color ? level_to_color(level) : "",
            level_to_str(level),
            use_color ? COLOR_RESET : ""
        );
    }

    va_list args;
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);

    fprintf(stderr, "\n");
}

LogLevel log_level_from_string(const char *str)
{
    if (str == NULL) {
        return LOG_LEVEL_DEBUG;
    }

    if (strcmp(str, "DEBUG") == 0) {
        return LOG_LEVEL_DEBUG;
    }

    if (strcmp(str, "INFO") == 0) {
        return LOG_LEVEL_INFO;
    }

    if (strcmp(str, "WARN") == 0) {
        return LOG_LEVEL_WARN;
    }

    if (strcmp(str, "ERROR") == 0) {
        return LOG_LEVEL_ERROR;
    }

    return LOG_LEVEL_DEBUG;
}
