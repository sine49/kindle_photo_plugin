/* touch.h — 轻触/电源键退出（v1.1+）：输入设备发现 + 非阻塞轻触/电源键检测接口 */
#ifndef PHOTO_PLAYER_TOUCH_H
#define PHOTO_PLAYER_TOUCH_H

typedef struct TouchWatch TouchWatch;

/* 找触摸设备与电源键设备并打开（两路独立开关，互不牵连）；
 * 全都不可用/都未请求返回 NULL（非致命，调用方记 warning 继续） */
TouchWatch* touch_open(int want_touch, int want_power);
int  touch_poll(TouchWatch*);  /* 非阻塞：1=检测到轻触或电源键按下应退出，0=无事件 */
void touch_close(TouchWatch*); /* 幂等，NULL 安全 */

#endif /* PHOTO_PLAYER_TOUCH_H */
