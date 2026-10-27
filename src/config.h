/* config.h — 应用配置：枚举/结构体 + 默认值、配置文件、CLI 解析与校验 */
#ifndef PHOTO_PLAYER_CONFIG_H
#define PHOTO_PLAYER_CONFIG_H

#include <stddef.h>

typedef enum { ORDER_SEQ = 0, ORDER_RANDOM = 1 } PlayOrder;
typedef enum { FIT_CONTAIN = 0, FIT_COVER = 1, FIT_STRETCH = 2 } FitMode;
typedef enum { SB_KEEP = 0, SB_COVER = 1 } StatusBarMode;
typedef enum { SOURCE_LOCAL = 0, SOURCE_HTTP = 1 } SourceKind;

typedef struct {
    char     source[256];
    SourceKind source_kind;
    PlayOrder  order;
    int      interval_sec;
    FitMode  fit;
    StatusBarMode statusbar;
    int      flash_every;
    int      max_file_mb;  /* MAX_FILE_MB / --max-file-mb：单图大小上限 MB，0=不限制，默认 8 */
    int      start_delay_ms; /* START_DELAY / --start-delay：启动延迟毫秒 [0,60000]，默认 2000，0=不延迟 */
    int      no_framework;
    int      debug;
    int      touch_exit;   /* TOUCH_EXIT（默认 1）/ CLI --no-touch-exit 置 0：轻触屏幕退出 */
    int      power_exit;   /* POWER_EXIT（默认 1）/ CLI --no-power-exit 置 0：电源键退出，与 touch_exit 独立 */
    char     http_url[256];  /* HTTP_URL 键存入，默认空串；M3 http 源用，无 CLI 参数 */
} AppConfig;

void config_defaults(AppConfig* cfg);                        /* 内置默认：SOURCE_LOCAL 空, ORDER_SEQ, interval 60, FIT_CONTAIN, SB_COVER, flash_every 8 */
int  config_load_file(AppConfig* cfg, const char* path);     /* KEY=VALUE 行, # 注释, 空行忽略; 未知键记警告并忽略; 0=ok, -1=打不开或格式错 */
int  config_parse_args(AppConfig* cfg, int argc, char** argv); /* CLI 覆盖 conf; --help 返回 1; 错误返回 -1 */
int  config_validate(const AppConfig* cfg, char* err, size_t errlen); /* 0=ok, -1=err 填 err */

#endif /* PHOTO_PLAYER_CONFIG_H */
