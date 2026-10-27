/* config.c — 配置装配实现：defaults → conf 文件 → CLI 覆盖（顺序由 main 保证） */
#define _POSIX_C_SOURCE 200809L
#include "config.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "log.h"

#define CONF_LINE_MAX 512

/* 去掉首尾空白（原地），返回新起点 */
static char* trim(char* s)
{
    char* end;

    while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n') {
        s++;
    }
    if (*s == '\0') {
        return s;
    }
    end = s + strlen(s) - 1;
    while (end > s && (*end == ' ' || *end == '\t' || *end == '\r' || *end == '\n')) {
        *end = '\0';
        end--;
    }
    return s;
}

static void set_err(char* err, size_t errlen, const char* fmt, ...)
{
    va_list ap;

    if (err == NULL || errlen == 0) {
        return;
    }
    va_start(ap, fmt);
    if (vsnprintf(err, errlen, fmt, ap) < 0) {
        err[0] = '\0';
    }
    va_end(ap);
}

/* 严格整数解析：仅十进制、无尾部垃圾、范围 [lo, hi] */
static int parse_long_range(const char* s, long lo, long hi, int* out)
{
    char* end = NULL;
    long v;

    if (s == NULL || *s == '\0') {
        return -1;
    }
    errno = 0;
    v = strtol(s, &end, 10);
    if (errno != 0 || end == s || *end != '\0') {
        return -1;
    }
    if (v < lo || v > hi) {
        return -1;
    }
    *out = (int) v;
    return 0;
}

static int parse_order(const char* v, PlayOrder* out)
{
    if (v == NULL) {
        return -1;
    }
    if (strcasecmp(v, "seq") == 0) {
        *out = ORDER_SEQ;
        return 0;
    }
    if (strcasecmp(v, "random") == 0) {
        *out = ORDER_RANDOM;
        return 0;
    }
    return -1;
}

static int parse_fit(const char* v, FitMode* out)
{
    if (v == NULL) {
        return -1;
    }
    if (strcasecmp(v, "contain") == 0) {
        *out = FIT_CONTAIN;
        return 0;
    }
    if (strcasecmp(v, "cover") == 0) {
        *out = FIT_COVER;
        return 0;
    }
    if (strcasecmp(v, "stretch") == 0) {
        *out = FIT_STRETCH;
        return 0;
    }
    return -1;
}

static int parse_statusbar(const char* v, StatusBarMode* out)
{
    if (v == NULL) {
        return -1;
    }
    if (strcasecmp(v, "keep") == 0) {
        *out = SB_KEEP;
        return 0;
    }
    if (strcasecmp(v, "cover") == 0) {
        *out = SB_COVER;
        return 0;
    }
    return -1;
}

/* 配置文件里的 flag 键（CLI 中为无值开关） */
static int parse_flag(const char* v, int* out)
{
    if (v == NULL) {
        return -1;
    }
    if (strcasecmp(v, "1") == 0 || strcasecmp(v, "true") == 0 || strcasecmp(v, "yes") == 0 ||
        strcasecmp(v, "on") == 0) {
        *out = 1;
        return 0;
    }
    if (strcasecmp(v, "0") == 0 || strcasecmp(v, "false") == 0 || strcasecmp(v, "no") == 0 ||
        strcasecmp(v, "off") == 0) {
        *out = 0;
        return 0;
    }
    return -1;
}

static int set_source(AppConfig* cfg, const char* val)
{
    size_t n;

    if (cfg == NULL || val == NULL) {
        return -1;
    }
    if (val[0] == '\0') {
        log_error("config: source must not be empty");
        return -1;
    }
    n = strlen(val);
    if (n >= sizeof(cfg->source)) {
        log_error("config: source too long (%u bytes)", (unsigned int) n);
        return -1;
    }
    memcpy(cfg->source, val, n + 1);
    if (strncmp(val, "http://", 7) == 0 || strncmp(val, "https://", 8) == 0) {
        cfg->source_kind = SOURCE_HTTP; /* M1 不实现 http，但解析照常接受并识别 */
    } else {
        cfg->source_kind = SOURCE_LOCAL;
    }
    return 0;
}

void config_defaults(AppConfig* cfg)
{
    if (cfg == NULL) {
        return;
    }
    memset(cfg, 0, sizeof(*cfg));
    cfg->source[0] = '\0';
    cfg->source_kind = SOURCE_LOCAL;
    cfg->order = ORDER_SEQ;
    cfg->interval_sec = 60;
    cfg->fit = FIT_CONTAIN;
    cfg->statusbar = SB_COVER;
    cfg->flash_every = 8;
    cfg->max_file_mb = 8; /* 单图上限 8MB（0=不限制） */
    cfg->start_delay_ms = 2000; /* 等 KUAL/启动器重绘完再上屏（0=不延迟） */
    cfg->no_framework = 0;
    cfg->debug = 0;
    cfg->touch_exit = 1; /* 轻触退出默认开 */
    cfg->power_exit = 1; /* 电源键退出默认开 */
    cfg->http_url[0] = '\0'; /* M3 备用，默认空串 */
}

/* 应用一个键值对；0=ok，1=未知键（调用方记警告），-1=格式/取值错误 */
static int apply_kv(AppConfig* cfg, const char* key, const char* val)
{
    if (strcasecmp(key, "SOURCE") == 0) {
        return set_source(cfg, val) == 0 ? 0 : -1;
    }
    if (strcasecmp(key, "ORDER") == 0) {
        PlayOrder o;
        if (parse_order(val, &o) != 0) {
            return -1;
        }
        cfg->order = o;
        return 0;
    }
    if (strcasecmp(key, "INTERVAL") == 0) {
        int iv;
        if (parse_long_range(val, 2, 86400, &iv) != 0) {
            return -1;
        }
        cfg->interval_sec = iv;
        return 0;
    }
    if (strcasecmp(key, "FIT") == 0) {
        FitMode fm;
        if (parse_fit(val, &fm) != 0) {
            return -1;
        }
        cfg->fit = fm;
        return 0;
    }
    if (strcasecmp(key, "STATUSBAR") == 0) {
        StatusBarMode sb;
        if (parse_statusbar(val, &sb) != 0) {
            return -1;
        }
        cfg->statusbar = sb;
        return 0;
    }
    if (strcasecmp(key, "FLASH_EVERY") == 0) {
        int fv;
        if (parse_long_range(val, 0, 1000, &fv) != 0) {
            return -1;
        }
        cfg->flash_every = fv;
        return 0;
    }
    if (strcasecmp(key, "MAX_FILE_MB") == 0) {
        int mv;
        if (parse_long_range(val, 0, 4096, &mv) != 0) {
            return -1;
        }
        cfg->max_file_mb = mv;
        return 0;
    }
    if (strcasecmp(key, "START_DELAY") == 0) {
        int sv;
        if (parse_long_range(val, 0, 60000, &sv) != 0) {
            return -1;
        }
        cfg->start_delay_ms = sv;
        return 0;
    }
    if (strcasecmp(key, "NO_FRAMEWORK") == 0) {
        int f;
        if (parse_flag(val, &f) != 0) {
            return -1;
        }
        cfg->no_framework = f;
        return 0;
    }
    if (strcasecmp(key, "HTTP_URL") == 0) {
        size_t n;
        if (val == NULL) {
            return -1;
        }
        n = strlen(val);
        if (n >= sizeof(cfg->http_url)) {
            log_error("config: HTTP_URL too long (%u bytes)", (unsigned int) n);
            return -1;
        }
        memcpy(cfg->http_url, val, n + 1); /* 允许空串（等同默认）；M3 用 */
        return 0;
    }
    if (strcasecmp(key, "DEBUG") == 0) {
        int f;
        if (parse_flag(val, &f) != 0) {
            return -1;
        }
        cfg->debug = f;
        return 0;
    }
    if (strcasecmp(key, "TOUCH_EXIT") == 0) {
        int f;
        if (parse_flag(val, &f) != 0) {
            return -1;
        }
        cfg->touch_exit = f;
        return 0;
    }
    if (strcasecmp(key, "POWER_EXIT") == 0) {
        int f;
        if (parse_flag(val, &f) != 0) {
            return -1;
        }
        cfg->power_exit = f;
        return 0;
    }
    return 1;
}

int config_load_file(AppConfig* cfg, const char* path)
{
    FILE* f;
    char line[CONF_LINE_MAX];
    int lineno = 0;

    if (cfg == NULL || path == NULL) {
        return -1;
    }
    f = fopen(path, "r");
    if (f == NULL) {
        log_error("config: cannot open '%s': %s", path, strerror(errno));
        return -1;
    }
    while (fgets(line, (int) sizeof(line), f) != NULL) {
        size_t l = strlen(line);
        char* s;
        char* eq;
        char* key;
        char* val;
        int rv;

        lineno++;
        if (l == sizeof(line) - 1 && line[l - 1] != '\n') {
            log_error("config: %s:%d: line too long", path, lineno);
            fclose(f);
            return -1;
        }
        s = trim(line);
        if (*s == '\0' || *s == '#') {
            continue;
        }
        eq = strchr(s, '=');
        if (eq == NULL) {
            log_error("config: %s:%d: expected KEY=VALUE", path, lineno);
            fclose(f);
            return -1;
        }
        *eq = '\0';
        key = trim(s);
        val = trim(eq + 1);
        if (*key == '\0') {
            log_error("config: %s:%d: empty key", path, lineno);
            fclose(f);
            return -1;
        }
        rv = apply_kv(cfg, key, val);
        if (rv < 0) {
            log_error("config: %s:%d: bad value for '%s'", path, lineno, key);
            fclose(f);
            return -1;
        }
        if (rv > 0) {
            log_info("config: %s:%d: warning: unknown key '%s' ignored", path, lineno, key);
        }
    }
    if (ferror(f)) {
        log_error("config: read error on '%s'", path);
        fclose(f);
        return -1;
    }
    fclose(f);
    return 0;
}

int config_parse_args(AppConfig* cfg, int argc, char** argv)
{
    int i;

    if (cfg == NULL || argv == NULL || argc < 1) {
        return -1;
    }
    for (i = 1; i < argc; i++) {
        const char* a = argv[i];
        const char* v;

        if (a == NULL) {
            return -1;
        }
        if (strcmp(a, "--help") == 0 || strcmp(a, "-h") == 0) {
            return 1;
        }
        if (strcmp(a, "--no-framework") == 0) {
            cfg->no_framework = 1;
            continue;
        }
        if (strcmp(a, "--debug") == 0) {
            cfg->debug = 1;
            continue;
        }
        if (strcmp(a, "--no-touch-exit") == 0) {
            cfg->touch_exit = 0;
            continue;
        }
        if (strcmp(a, "--no-power-exit") == 0) {
            cfg->power_exit = 0;
            continue;
        }
        if (strcmp(a, "--source") != 0 && strcmp(a, "--order") != 0 && strcmp(a, "--interval") != 0 &&
            strcmp(a, "--fit") != 0 && strcmp(a, "--statusbar") != 0 && strcmp(a, "--flash-every") != 0 &&
            strcmp(a, "--max-file-mb") != 0 && strcmp(a, "--start-delay") != 0 && strcmp(a, "--config") != 0) {
            log_error("config: unknown option '%s'", a);
            return -1;
        }
        /* 带值选项 */
        if (i + 1 >= argc || argv[i + 1] == NULL || strncmp(argv[i + 1], "--", 2) == 0) {
            log_error("config: %s requires a value", a);
            return -1;
        }
        v = argv[++i];

        if (strcmp(a, "--source") == 0) {
            if (set_source(cfg, v) != 0) {
                return -1;
            }
        } else if (strcmp(a, "--order") == 0) {
            PlayOrder o;
            if (parse_order(v, &o) != 0) {
                log_error("config: bad --order '%s' (want seq|random)", v);
                return -1;
            }
            cfg->order = o;
        } else if (strcmp(a, "--interval") == 0) {
            int iv;
            if (parse_long_range(v, 2, 86400, &iv) != 0) {
                log_error("config: bad --interval '%s' (want 2..86400)", v);
                return -1;
            }
            cfg->interval_sec = iv;
        } else if (strcmp(a, "--fit") == 0) {
            FitMode fm;
            if (parse_fit(v, &fm) != 0) {
                log_error("config: bad --fit '%s' (want contain|cover|stretch)", v);
                return -1;
            }
            cfg->fit = fm;
        } else if (strcmp(a, "--statusbar") == 0) {
            StatusBarMode sb;
            if (parse_statusbar(v, &sb) != 0) {
                log_error("config: bad --statusbar '%s' (want keep|cover)", v);
                return -1;
            }
            cfg->statusbar = sb;
        } else if (strcmp(a, "--flash-every") == 0) {
            int fv;
            if (parse_long_range(v, 0, 1000, &fv) != 0) {
                log_error("config: bad --flash-every '%s' (want 0..1000)", v);
                return -1;
            }
            cfg->flash_every = fv;
        } else if (strcmp(a, "--max-file-mb") == 0) {
            int mv;
            if (parse_long_range(v, 0, 4096, &mv) != 0) {
                log_error("config: bad --max-file-mb '%s' (want 0..4096)", v);
                return -1;
            }
            cfg->max_file_mb = mv;
        } else if (strcmp(a, "--start-delay") == 0) {
            int sv;
            if (parse_long_range(v, 0, 60000, &sv) != 0) {
                log_error("config: bad --start-delay '%s' (want 0..60000)", v);
                return -1;
            }
            cfg->start_delay_ms = sv;
        } else {
            /* --config：配置文件路径由调用方（main）在 CLI 覆盖前加载，此处仅消费 */
        }
    }
    return 0;
}

int config_validate(const AppConfig* cfg, char* err, size_t errlen)
{
    if (err != NULL && errlen > 0) {
        err[0] = '\0';
    }
    if (cfg == NULL) {
        set_err(err, errlen, "null config");
        return -1;
    }
    if (cfg->source[0] == '\0') {
        set_err(err, errlen, "source is required (--source <dir|url>)");
        return -1;
    }
    if (cfg->source_kind != SOURCE_LOCAL && cfg->source_kind != SOURCE_HTTP) {
        set_err(err, errlen, "bad source kind");
        return -1;
    }
    if (cfg->order != ORDER_SEQ && cfg->order != ORDER_RANDOM) {
        set_err(err, errlen, "bad order (want seq|random)");
        return -1;
    }
    if (cfg->fit != FIT_CONTAIN && cfg->fit != FIT_COVER && cfg->fit != FIT_STRETCH) {
        set_err(err, errlen, "bad fit (want contain|cover|stretch)");
        return -1;
    }
    if (cfg->statusbar != SB_KEEP && cfg->statusbar != SB_COVER) {
        set_err(err, errlen, "bad statusbar (want keep|cover)");
        return -1;
    }
    if (cfg->interval_sec < 2 || cfg->interval_sec > 86400) {
        set_err(err, errlen, "interval out of range [2, 86400]: %d", cfg->interval_sec);
        return -1;
    }
    if (cfg->flash_every < 0 || cfg->flash_every > 1000) {
        set_err(err, errlen, "flash-every out of range [0, 1000]: %d", cfg->flash_every);
        return -1;
    }
    if (cfg->max_file_mb < 0 || cfg->max_file_mb > 4096) {
        set_err(err, errlen, "max-file-mb out of range [0, 4096]: %d", cfg->max_file_mb);
        return -1;
    }
    if (cfg->start_delay_ms < 0 || cfg->start_delay_ms > 60000) {
        set_err(err, errlen, "start-delay out of range [0, 60000]: %d", cfg->start_delay_ms);
        return -1;
    }
    return 0;
}
