/* test_config.c — config 单元测试：默认值/文件解析/CLI 覆盖/非法值拒绝/边界 interval */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "config.h"

static int g_failures = 0;

#define ASSERT(cond)                                                            \
    do {                                                                        \
        if (cond) {                                                             \
            printf("[PASS] %s:%d %s\n", __FILE__, __LINE__, #cond);             \
        } else {                                                                \
            printf("[FAIL] %s:%d %s\n", __FILE__, __LINE__, #cond);             \
            g_failures++;                                                       \
        }                                                                       \
    } while (0)

static void write_file(const char* path, const char* content)
{
    FILE* f = fopen(path, "w");
    if (f == NULL) {
        printf("FATAL: cannot write %s\n", path);
        exit(1);
    }
    fputs(content, f);
    fclose(f);
}

int test_config(void)
{
    AppConfig cfg;
    char err[128];
    char conf[128];

    snprintf(conf, sizeof(conf), "/tmp/photo_plugins_test_%d.conf", (int) getpid());

    /* --- 内置默认值 --- */
    config_defaults(&cfg);
    ASSERT(cfg.source[0] == '\0');
    ASSERT(cfg.source_kind == SOURCE_LOCAL);
    ASSERT(cfg.order == ORDER_SEQ);
    ASSERT(cfg.interval_sec == 60);
    ASSERT(cfg.fit == FIT_CONTAIN);
    ASSERT(cfg.statusbar == SB_COVER);
    ASSERT(cfg.flash_every == 8);
    ASSERT(cfg.max_file_mb == 8);
    ASSERT(cfg.start_delay_ms == 2000);
    ASSERT(cfg.no_framework == 0);
    ASSERT(cfg.debug == 0);
    ASSERT(cfg.touch_exit == 1);
    ASSERT(cfg.power_exit == 1);
    ASSERT(cfg.http_url[0] == '\0');

    /* --- validate：缺 source 报错 --- */
    err[0] = 'x';
    ASSERT(config_validate(&cfg, err, sizeof(err)) == -1);
    ASSERT(err[0] != '\0');

    /* --- 文件解析：全键 + 注释 + 空行 --- */
    write_file(conf,
               "# photo-player conf\n"
               "\n"
               "  # indented comment\n"
               "SOURCE=/mnt/us/photos\n"
               "ORDER=random\n"
               "INTERVAL=30\n"
               "FIT=cover\n"
               "STATUSBAR=keep\n"
               "FLASH_EVERY=5\n"
               "MAX_FILE_MB=16\n"
               "START_DELAY=1500\n"
               "NO_FRAMEWORK=1\n"
               "DEBUG=0\n"
               "TOUCH_EXIT=0\n"
               "POWER_EXIT=0\n"
               "HTTP_URL=http://192.168.15.1:8080/photos\n");
    config_defaults(&cfg);
    ASSERT(config_load_file(&cfg, conf) == 0);
    ASSERT(strcmp(cfg.source, "/mnt/us/photos") == 0);
    ASSERT(cfg.source_kind == SOURCE_LOCAL);
    ASSERT(cfg.order == ORDER_RANDOM);
    ASSERT(cfg.interval_sec == 30);
    ASSERT(cfg.fit == FIT_COVER);
    ASSERT(cfg.statusbar == SB_KEEP);
    ASSERT(cfg.flash_every == 5);
    ASSERT(cfg.max_file_mb == 16);
    ASSERT(cfg.start_delay_ms == 1500);
    ASSERT(cfg.no_framework == 1);
    ASSERT(cfg.debug == 0);
    ASSERT(cfg.touch_exit == 0);
    ASSERT(cfg.power_exit == 0);
    ASSERT(strcmp(cfg.http_url, "http://192.168.15.1:8080/photos") == 0);
    ASSERT(config_validate(&cfg, err, sizeof(err)) == 0);

    /* --- 未知键：警告并忽略，不影响其它键 --- */
    write_file(conf, "SOURCE=/a\nBOGUS_KEY=whatever\nINTERVAL=15\n");
    config_defaults(&cfg);
    ASSERT(config_load_file(&cfg, conf) == 0);
    ASSERT(strcmp(cfg.source, "/a") == 0);
    ASSERT(cfg.interval_sec == 15);

    /* --- 打不开的文件 --- */
    ASSERT(config_load_file(&cfg, "/tmp/photo_plugins_missing_xyz.conf") == -1);

    /* --- 文件非法值/格式错拒绝 --- */
    write_file(conf, "ORDER=sideways\n");
    config_defaults(&cfg);
    ASSERT(config_load_file(&cfg, conf) == -1);
    write_file(conf, "INTERVAL=abc\n");
    ASSERT(config_load_file(&cfg, conf) == -1);
    write_file(conf, "INTERVAL=1\n");
    ASSERT(config_load_file(&cfg, conf) == -1);
    write_file(conf, "FIT=fill\n");
    ASSERT(config_load_file(&cfg, conf) == -1);
    write_file(conf, "garbage line without equals\n");
    ASSERT(config_load_file(&cfg, conf) == -1);
    write_file(conf, "NO_FRAMEWORK=maybe\n");
    ASSERT(config_load_file(&cfg, conf) == -1);
    { /* HTTP_URL 超长（>=256）拒绝，缓冲区安全 */
        char longline[320];
        memset(longline, 'u', sizeof(longline));
        memcpy(longline, "HTTP_URL=", 9);
        longline[9 + 300] = '\n';
        longline[9 + 300 + 1] = '\0';
        write_file(conf, longline);
        ASSERT(config_load_file(&cfg, conf) == -1);
    }
    write_file(conf, "HTTP_URL=\nSOURCE=/keep\n"); /* 空值合法（等同默认） */
    config_defaults(&cfg);
    ASSERT(config_load_file(&cfg, conf) == 0);
    ASSERT(cfg.http_url[0] == '\0');
    ASSERT(strcmp(cfg.source, "/keep") == 0);

    /* --- 加载顺序：defaults → conf → CLI 覆盖 --- */
    write_file(conf, "SOURCE=/from/conf\nORDER=seq\nINTERVAL=45\nFIT=contain\nSTATUSBAR=cover\nFLASH_EVERY=3\n");
    config_defaults(&cfg);
    ASSERT(config_load_file(&cfg, conf) == 0);
    {
        char* argv[] = { (char*) "photo-player",
                         (char*) "--source",
                         (char*) "/from/cli",
                         (char*) "--order",
                         (char*) "random",
                         (char*) "--interval",
                         (char*) "90",
                         (char*) "--fit",
                         (char*) "stretch",
                         (char*) "--statusbar",
                         (char*) "keep",
                         (char*) "--flash-every",
                         (char*) "0",
                         (char*) "--max-file-mb",
                         (char*) "0",
                         (char*) "--start-delay",
                         (char*) "0",
                         (char*) "--no-framework",
                         (char*) "--no-touch-exit",
                         (char*) "--no-power-exit",
                         (char*) "--debug" };
        ASSERT(config_parse_args(&cfg, 21, argv) == 0);
    }
    ASSERT(strcmp(cfg.source, "/from/cli") == 0);
    ASSERT(cfg.order == ORDER_RANDOM);
    ASSERT(cfg.interval_sec == 90);
    ASSERT(cfg.fit == FIT_STRETCH);
    ASSERT(cfg.statusbar == SB_KEEP);
    ASSERT(cfg.flash_every == 0);
    ASSERT(cfg.max_file_mb == 0); /* CLI --max-file-mb 0 = 不限制 */
    ASSERT(cfg.start_delay_ms == 0); /* CLI --start-delay 0 = 不延迟 */
    ASSERT(cfg.no_framework == 1);
    ASSERT(cfg.touch_exit == 0); /* CLI --no-touch-exit 覆盖文件里的 1/默认值 */
    ASSERT(cfg.power_exit == 0); /* CLI --no-power-exit 同理，与 touch_exit 独立 */
    ASSERT(cfg.debug == 1);

    /* --- CLI：边界 interval（2 / 86400 合法；1 / 86401 / 非数字拒绝） --- */
    {
        char* a2[] = { (char*) "photo-player", (char*) "--interval", (char*) "2" };
        char* amax[] = { (char*) "photo-player", (char*) "--interval", (char*) "86400" };
        char* a1[] = { (char*) "photo-player", (char*) "--interval", (char*) "1" };
        char* aover[] = { (char*) "photo-player", (char*) "--interval", (char*) "86401" };
        char* abad[] = { (char*) "photo-player", (char*) "--interval", (char*) "abc" };
        config_defaults(&cfg);
        ASSERT(config_parse_args(&cfg, 3, a2) == 0 && cfg.interval_sec == 2);
        ASSERT(config_parse_args(&cfg, 3, amax) == 0 && cfg.interval_sec == 86400);
        ASSERT(config_parse_args(&cfg, 3, a1) == -1);
        ASSERT(config_parse_args(&cfg, 3, aover) == -1);
        ASSERT(config_parse_args(&cfg, 3, abad) == -1);
    }

    /* --- CLI：--max-file-mb 边界与非法值 --- */
    {
        char* m0[] = { (char*) "photo-player", (char*) "--max-file-mb", (char*) "0" };
        char* mm[] = { (char*) "photo-player", (char*) "--max-file-mb", (char*) "4096" };
        char* mover[] = { (char*) "photo-player", (char*) "--max-file-mb", (char*) "4097" };
        char* mneg[] = { (char*) "photo-player", (char*) "--max-file-mb", (char*) "-1" };
        char* mbad[] = { (char*) "photo-player", (char*) "--max-file-mb", (char*) "abc" };
        config_defaults(&cfg);
        ASSERT(config_parse_args(&cfg, 3, m0) == 0 && cfg.max_file_mb == 0);
        ASSERT(config_parse_args(&cfg, 3, mm) == 0 && cfg.max_file_mb == 4096);
        ASSERT(config_parse_args(&cfg, 3, mover) == -1);
        ASSERT(config_parse_args(&cfg, 3, mneg) == -1);
        ASSERT(config_parse_args(&cfg, 3, mbad) == -1);
    }

    /* --- CLI：--start-delay 边界与非法值 --- */
    {
        char* s0[] = { (char*) "photo-player", (char*) "--start-delay", (char*) "0" };
        char* smax[] = { (char*) "photo-player", (char*) "--start-delay", (char*) "60000" };
        char* sover[] = { (char*) "photo-player", (char*) "--start-delay", (char*) "60001" };
        char* sneg[] = { (char*) "photo-player", (char*) "--start-delay", (char*) "-1" };
        char* sbad[] = { (char*) "photo-player", (char*) "--start-delay", (char*) "abc" };
        config_defaults(&cfg);
        ASSERT(config_parse_args(&cfg, 3, s0) == 0 && cfg.start_delay_ms == 0);
        ASSERT(config_parse_args(&cfg, 3, smax) == 0 && cfg.start_delay_ms == 60000);
        ASSERT(config_parse_args(&cfg, 3, sover) == -1);
        ASSERT(config_parse_args(&cfg, 3, sneg) == -1);
        ASSERT(config_parse_args(&cfg, 3, sbad) == -1);
    }

    /* --- CLI：--help=1、未知选项/缺值 = -1 --- */
    {
        char* h[] = { (char*) "photo-player", (char*) "--help" };
        char* u[] = { (char*) "photo-player", (char*) "--bogus" };
        char* m[] = { (char*) "photo-player", (char*) "--source" };
        char* d[] = { (char*) "photo-player", (char*) "--source", (char*) "--debug" };
        char* c[] = { (char*) "photo-player", (char*) "--config", (char*) "/x.conf", (char*) "--interval", (char*) "10" };
        config_defaults(&cfg);
        ASSERT(config_parse_args(&cfg, 2, h) == 1);
        ASSERT(config_parse_args(&cfg, 2, u) == -1);
        ASSERT(config_parse_args(&cfg, 2, m) == -1);
        ASSERT(config_parse_args(&cfg, 3, d) == -1); /* 值缺失（下一个还是选项） */
        ASSERT(config_parse_args(&cfg, 5, c) == 0);  /* --config 被消费不报错 */
        ASSERT(cfg.interval_sec == 10);
    }

    /* --- http(s) source：config 层照常接受并识别 --- */
    {
        char* hv[] = { (char*) "photo-player", (char*) "--source", (char*)"http://host/x.jpg" };
        char* hs[] = { (char*) "photo-player", (char*) "--source", (char*)"https://host/y.jpg" };
        config_defaults(&cfg);
        ASSERT(config_parse_args(&cfg, 3, hv) == 0);
        ASSERT(cfg.source_kind == SOURCE_HTTP);
        ASSERT(config_validate(&cfg, err, sizeof(err)) == 0);
        ASSERT(config_parse_args(&cfg, 3, hs) == 0);
        ASSERT(cfg.source_kind == SOURCE_HTTP);
    }

    /* --- validate 拒绝直接写入的非法字段（边界） --- */
    config_defaults(&cfg);
    strcpy(cfg.source, "/x");
    cfg.interval_sec = 1;
    ASSERT(config_validate(&cfg, err, sizeof(err)) == -1);
    cfg.interval_sec = 86401;
    ASSERT(config_validate(&cfg, err, sizeof(err)) == -1);
    cfg.interval_sec = 86400;
    cfg.flash_every = 1001;
    ASSERT(config_validate(&cfg, err, sizeof(err)) == -1);
    cfg.flash_every = 1000;
    ASSERT(config_validate(&cfg, err, sizeof(err)) == 0);
    cfg.order = (PlayOrder) 99;
    ASSERT(config_validate(&cfg, err, sizeof(err)) == -1);

    (void) unlink(conf);
    return g_failures;
}
