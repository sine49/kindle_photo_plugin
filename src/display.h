/* display.h — FBInk 显示封装：初始化/全屏显示一张图/清屏关闭（opaque Display） */
#ifndef PHOTO_PLAYER_DISPLAY_H
#define PHOTO_PLAYER_DISPLAY_H

#include "config.h"

typedef struct Display Display;

Display* display_open(const AppConfig* cfg);                     /* fbink_open+init+get_state；失败 NULL */
int      display_show(Display* d, const char* path, const AppConfig* cfg); /* 全屏显示一张图；0=ok,-1=fail */
void     display_close(Display* d, int show_hint);               /* 清屏+fbink_close；show_hint!=0 时白底上画退出提示（no-framework 用）；NULL 安全，幂等 */

void fatal_screen(const char* short_msg); /* best-effort 屏幕报错（短 ASCII）；自带 fbink 全流程，任何失败忽略，绝不崩溃/阻塞 */

#endif /* PHOTO_PLAYER_DISPLAY_H */
