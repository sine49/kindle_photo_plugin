/* platform.h — 平台防干扰接口：preventScreenSaver / 可选 framework 启停与幂等恢复 */
#ifndef PHOTO_PLAYER_PLATFORM_H
#define PHOTO_PLAYER_PLATFORM_H

#include "config.h"

int  platform_setup(const AppConfig* cfg);  /* preventScreenSaver 1；no_framework 时 stop framework + pillow disable，并落状态标记文件（/tmp/photo-player.{framework-stopped,pillow-disabled}） */
void platform_restore(void);                /* 按 §9.1 逆序恢复，幂等，可重复调用；恢复成功才清标记，失败留给 photo-player.sh 代为重试（SIGKILL 场景） */

#endif /* PHOTO_PLAYER_PLATFORM_H */
