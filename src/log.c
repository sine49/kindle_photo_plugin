/* log.c — 日志实现：时间戳 + 级别前缀，写 stderr 并追加 /tmp/photo-player.log */
#define _POSIX_C_SOURCE 200809L
#include "log.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define LOG_FILE    "/tmp/photo-player.log"
#define LOG_LINE_MAX 1024
#define LOG_EXTRA_MAX 512

static int g_log_debug = 0;
static char g_extra_path[LOG_EXTRA_MAX];
static int g_extra_resolved = 0; /* 0=env 未解析（惰性） */

void log_set_debug(int on)
{
    g_log_debug = (on != 0);
}

/* 首次写日志前从环境变量解析第二份日志路径（启停脚本注入 PHOTO_PLAYER_LOG_EXTRA，失败静默） */
static void log_resolve_extra(void)
{
    const char* env;

    if (g_extra_resolved) {
        return;
    }
    g_extra_resolved = 1;
    env = getenv("PHOTO_PLAYER_LOG_EXTRA");
    if (env != NULL && env[0] != '\0') {
        size_t n = strlen(env);
        if (n < sizeof(g_extra_path)) {
            memcpy(g_extra_path, env, n + 1);
        }
    }
}

static void log_write(const char* level, const char* fmt, va_list ap)
{
    char msg[LOG_LINE_MAX];
    char line[LOG_LINE_MAX + 64];
    char stamp[32];
    time_t t;
    struct tm tm_buf;
    FILE* f;

    if (fmt == NULL) {
        return;
    }
    /* vsnprintf 保证 NUL 结尾；超长截断可接受 */
    if (vsnprintf(msg, sizeof(msg), fmt, ap) < 0) {
        return;
    }
    t = time(NULL);
    if (localtime_r(&t, &tm_buf) != NULL) {
        if (strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", &tm_buf) == 0) {
            stamp[0] = '\0';
        }
    } else {
        stamp[0] = '\0';
    }
    if (snprintf(line, sizeof(line), "[%s] %s: %s", stamp, level, msg) < 0) {
        return;
    }
    fprintf(stderr, "%s\n", line);
    f = fopen(LOG_FILE, "a");
    if (f != NULL) {
        fprintf(f, "%s\n", line);
        fclose(f);
    }
    /* 第二份日志（USB 可见路径）：fopen 失败静默忽略，绝不影响主流程 */
    log_resolve_extra();
    if (g_extra_path[0] != '\0') {
        f = fopen(g_extra_path, "a");
        if (f != NULL) {
            fprintf(f, "%s\n", line);
            fclose(f);
        }
    }
}

void log_info(const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    log_write("info", fmt, ap);
    va_end(ap);
}

void log_error(const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    log_write("error", fmt, ap);
    va_end(ap);
}

void log_debug(const char* fmt, ...)
{
    va_list ap;
    if (!g_log_debug) {
        return;
    }
    va_start(ap, fmt);
    log_write("debug", fmt, ap);
    va_end(ap);
}
