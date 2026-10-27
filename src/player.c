/* player.c — 播放引擎实现：绝对 deadline 定时节拍（不累积漂移），假时钟可注入用于测试 */
#define _POSIX_C_SOURCE 200809L
#include "player.h"

#include <stdio.h>
#include <time.h>

#include "log.h"

#define PLAYER_PATH_MAX 1024

/* 默认时钟：CLOCK_MONOTONIC + nanosleep（单次调用，EINTR 即返回，交由调用方检查 quit） */
static long default_now_ms(void)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0;
    }
    return (long) ts.tv_sec * 1000L + (long)(ts.tv_nsec / 1000000L);
}

static void default_sleep_ms(long ms)
{
    struct timespec req;

    if (ms <= 0) {
        return;
    }
    req.tv_sec = (time_t)(ms / 1000L);
    req.tv_nsec = (long)(ms % 1000L) * 1000000L;
    (void) nanosleep(&req, NULL);
}

static const PlayerClock g_default_clock = { default_now_ms, default_sleep_ms };

const PlayerClock* player_clock_default(void)
{
    return &g_default_clock;
}

void player_init(Player* p, PhotoSource* src, PlayerShowFn show, void* user, const PlayerClock* clock, int interval_sec)
{
    if (p == NULL) {
        return;
    }
    p->source = src;
    p->show = show;
    p->show_user = user;
    p->clock = (clock != NULL) ? clock : player_clock_default();
    p->interval_sec = interval_sec;
    p->ticks = 0;
    p->consec_show_fail = 0;
}

int player_tick(Player* p)
{
    char path[PLAYER_PATH_MAX];
    int rv;

    if (p == NULL || p->source == NULL || p->source->next == NULL) {
        return -1;
    }
    p->ticks++;
    rv = p->source->next(p->source, path, sizeof(path));
    if (rv != 0) {
        return rv; /* -1=fatal, 1=skip（source 侧 skip 不计入 show 失败） */
    }
    if (p->show == NULL) {
        log_error("player: no show callback configured");
        return -1;
    }
    if (p->show(path, p->show_user) != 0) {
        /* 补丁2：连续 show 失败计数（成功清零），防"全坏图目录"空转 livelock */
        p->consec_show_fail++;
        log_error("player: show failed for '%s' (%d consecutive)", path, p->consec_show_fail);
        if (p->consec_show_fail >= PLAYER_MAX_CONSEC_SHOW_FAIL) {
            log_error("player: %d consecutive show failures, giving up", p->consec_show_fail);
            return 2;
        }
        return 1;
    }
    p->consec_show_fail = 0;
    return 0;
}

int player_run(Player* p, volatile sig_atomic_t* quit)
{
    long interval_ms;
    long deadline;

    if (p == NULL || quit == NULL || p->clock == NULL || p->clock->now_ms == NULL || p->clock->sleep_ms == NULL) {
        return -1;
    }
    if (p->interval_sec < 1) {
        log_error("player: bad interval %d", p->interval_sec);
        return -1;
    }
    interval_ms = (long) p->interval_sec * 1000L;

    /* §7：先立即 tick 一张，再睡到绝对 deadline；deadline 均匀递增，无累积漂移 */
    deadline = p->clock->now_ms() + interval_ms;
    while (!*quit) {
        int rv = player_tick(p);
        if (rv < 0) {
            return -1;
        }
        if (rv == 2) {
            return 2; /* 显示链路持续失败（补丁2） */
        }
        while (!*quit) {
            long now = p->clock->now_ms();
            long remain = deadline - now;
            if (remain <= 0) {
                break;
            }
            if (remain > interval_ms * 2) {
                /* 时钟回拨/回绕（long 约 24.8 天溢出）等异常：重新对齐节拍 */
                deadline = now;
                break;
            }
            p->clock->sleep_ms(remain); /* EINTR/早醒：回到循环重新检查 *quit 与剩余时间 */
        }
        deadline += interval_ms;
        if (deadline < p->clock->now_ms()) {
            /* 单次显示耗时超过一个间隔：跳过追赶突发，保持驻留节奏 */
            deadline = p->clock->now_ms() + interval_ms;
        }
    }
    return 0;
}
