/* player.h — 播放引擎：可注入时钟的固定节拍轮播（到时切图，间隔不漂移） */
#ifndef PHOTO_PLAYER_PLAYER_H
#define PHOTO_PLAYER_PLAYER_H

#include <signal.h>

#include "source.h"

/* 连续 show 失败上限：达到即返回 2（显示链路持续失败，防空转 livelock），可按需调整 */
#define PLAYER_MAX_CONSEC_SHOW_FAIL 5

typedef struct {
    long (*now_ms)(void);
    void (*sleep_ms)(long ms);   /* 可被信号中断，醒来即返回 */
} PlayerClock;

const PlayerClock* player_clock_default(void); /* clock_gettime(CLOCK_MONOTONIC)+nanosleep 实现，在 player.c 内 */

typedef int (*PlayerShowFn)(const char* path, void* user); /* 0=ok */

typedef struct {
    PhotoSource*   source;
    PlayerShowFn   show; void* show_user;
    const PlayerClock* clock;
    int interval_sec;
    int ticks;             /* player_tick() 调用计数（含 skip/fatal 的那次） */
    int consec_show_fail;  /* 连续 show 失败计数（成功清零；source skip 不计入） */
} Player;

void player_init(Player* p, PhotoSource* src, PlayerShowFn show, void* user, const PlayerClock* clock, int interval_sec);
int  player_tick(Player* p);  /* 取一张+show 一次；0=ok,-1=fatal,1=skip,2=连续 show 失败≥PLAYER_MAX_CONSEC_SHOW_FAIL */
int  player_run(Player* p, volatile sig_atomic_t* quit); /* 固定节拍循环到 *quit；0=正常退出,-1=fatal,2=显示链路持续失败 */

#endif /* PHOTO_PLAYER_PLAYER_H */
