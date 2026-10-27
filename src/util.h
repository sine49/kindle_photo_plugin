/* util.h — 时间与睡眠工具（CLOCK_MONOTONIC / nanosleep，兼容旧 glibc） */
#ifndef PHOTO_PLAYER_UTIL_H
#define PHOTO_PLAYER_UTIL_H

long util_now_ms(void);   /* CLOCK_MONOTONIC 毫秒 */
void util_sleep_ms(long ms); /* nanosleep 循环补足剩余时间，处理 EINTR */

#endif /* PHOTO_PLAYER_UTIL_H */
