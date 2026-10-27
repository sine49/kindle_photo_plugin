/* util.c — 时间与睡眠工具实现（clock_gettime/nanosleep，POSIX 老接口） */
#define _POSIX_C_SOURCE 200809L
#include "util.h"

#include <errno.h>
#include <time.h>

long util_now_ms(void)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0;
    }
    /* 单调时钟自启动计秒；long 在 32 位上约 24.8 天溢出，调用方需按差值使用 */
    return (long) ts.tv_sec * 1000L + (long)(ts.tv_nsec / 1000000L);
}

void util_sleep_ms(long ms)
{
    struct timespec req;
    struct timespec rem;

    if (ms <= 0) {
        return;
    }
    req.tv_sec = (time_t)(ms / 1000L);
    req.tv_nsec = (long)(ms % 1000L) * 1000000L;
    while (nanosleep(&req, &rem) != 0) {
        if (errno != EINTR) {
            return;
        }
        req = rem; /* 被信号打断：继续睡完剩余时间 */
    }
}
