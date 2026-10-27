/* source.h — 图片源抽象接口（DESIGN.md §6.1）+ 本地源工厂 */
#ifndef PHOTO_PLAYER_SOURCE_H
#define PHOTO_PLAYER_SOURCE_H

#include <stddef.h>

#include "config.h" /* PlayOrder */

typedef struct PhotoSource PhotoSource;
struct PhotoSource {
    int  (*next)(PhotoSource*, char* out_path, size_t len); /* 0=ok,-1=fatal,1=skip本轮 */
    void (*reset)(PhotoSource*);
    void (*cleanup)(PhotoSource*);
    const char* name;
};

/* 失败返回 NULL（目录不可读/无一张可读图；max_bytes>0 时超限文件不入列表，全被拒同样失败）；
 * max_bytes：单文件大小上限（字节），0=不限制。分配在堆，cleanup 里释放。默认 ORDER_SEQ */
PhotoSource* source_local_open(const char* dir, long max_bytes);

/* 契约扩展（main 需按 --order 配置调度、测试需验证洗牌语义）：切换顺序模式并复位游标 */
void source_local_set_order(PhotoSource* src, PlayOrder order);

#endif /* PHOTO_PLAYER_SOURCE_H */
