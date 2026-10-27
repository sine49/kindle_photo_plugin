/* log.h — 日志接口：写 stderr 并追加 /tmp/photo-player.log */
#ifndef PHOTO_PLAYER_LOG_H
#define PHOTO_PLAYER_LOG_H

void log_set_debug(int on);
void log_info(const char* fmt, ...);   /* stderr + 追加 /tmp/photo-player.log（+ extra） */
void log_error(const char* fmt, ...);  /* 同上，带 error 级别前缀 */
void log_debug(const char* fmt, ...);  /* 仅 debug 开时输出 */

#endif /* PHOTO_PLAYER_LOG_H */
