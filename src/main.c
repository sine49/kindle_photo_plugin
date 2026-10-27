/* main.c — photo-player 生命周期：配置装配、单实例、信号、播放主循环与 §9.1 幂等清理 */
#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "config.h"
#include "display.h"
#include "log.h"
#include "platform.h"
#include "player.h"
#include "source.h"
#include "touch.h"
#include "util.h"

#define PID_FILE   "/tmp/photo-player.pid"
#define STOP_FILE  "/tmp/photo-player.stop"
#define DEFAULT_CONF "config/photo-player.conf"

static volatile sig_atomic_t g_quit = 0;
static TouchWatch* g_touch = NULL; /* 轻触退出（v1.1）；touch_exit=0 或无设备时保持 NULL */

/* 信号处理器：只置 sig_atomic_t 标志（异步信号安全），cleanup 由主循环统一执行 */
static void on_signal(int sig)
{
    (void) sig;
    g_quit = 1;
}

static void print_usage(void)
{
    printf("photo-player - Kindle photo slideshow (M1)\n");
    printf("usage: photo-player --source <dir|url> [options]\n");
    printf("\n");
    printf("options:\n");
    printf("  --source <path|url>     local directory or http(s):// URL (required; http is M3)\n");
    printf("  --order seq|random      play order (default: seq)\n");
    printf("  --interval <sec>        switch interval, 2..86400 (default: 60)\n");
    printf("  --fit contain|cover|stretch   scaling (default: contain)\n");
    printf("  --statusbar keep|cover  keep status bar or cover it (default: cover)\n");
    printf("  --flash-every <n>       full flash every n images, 0=off (default: 8)\n");
    printf("  --max-file-mb <n>       per-image size limit in MB, 0=unlimited (default: 8)\n");
    printf("  --start-delay <ms>      delay before first image, 0..60000 (default: 2000)\n");
    printf("  --no-framework          stop framework while playing\n");
    printf("  --no-touch-exit         disable touch-to-exit (on by default)\n");
    printf("  --no-power-exit         disable power-key-to-exit (on by default)\n");
    printf("  --debug                 verbose logging\n");
    printf("  --config <file>         config file (KEY=VALUE); default: " DEFAULT_CONF " if present\n");
    printf("  --help                  show this help and exit\n");
}

/* 进程存活：仅 kill(pid,0)==0 参与拒绝判断（契约：pid>0 && pid!=getpid() && kill==0） */
static int pid_alive(long pid)
{
    if (pid <= 0) {
        return 0;
    }
    return (kill((pid_t) pid, 0) == 0);
}

/* 防 PID 回收误判：/proc/<pid>/cmdline 可读时要求内容含 "photo-player"。
 * 返回 1=确认本程序实例；0=可读但不是本程序（PID 被回收，可安全接管）；
 *      -1=/proc 不可读（退回仅凭 kill 判断） */
static int pid_is_photo_player(long pid)
{
    static const char tag[] = "photo-player"; /* 12 字节 */
    char path[64];
    char buf[512];
    FILE* f;
    size_t n;
    size_t i;
    int wr;

    wr = snprintf(path, sizeof(path), "/proc/%ld/cmdline", pid);
    if (wr < 0 || (size_t) wr >= sizeof(path)) {
        return -1;
    }
    f = fopen(path, "rb");
    if (f == NULL) {
        return -1; /* 读不到 /proc：退回仅凭 kill 判断 */
    }
    n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = '\0';
    /* cmdline 以 NUL 分隔参数，直接在原始字节里做子串匹配 */
    for (i = 0; i + sizeof(tag) - 1 <= n; i++) {
        if (memcmp(buf + i, tag, sizeof(tag) - 1) == 0) {
            return 1;
        }
    }
    return 0;
}

/* 单实例：仅当 PID 文件指向"别的、活着的 photo-player"才拒绝。
 * 通过检查后二进制自己覆盖写入 getpid()（PID 文件唯一所有者，启动脚本不再写）。
 * 拒绝路径：只写日志、不调 fatal_screen、绝不 unlink PID 文件（那是运行中实例的）。 */
static int claim_pid_file(void)
{
    FILE* f;
    long existing = 0;

    f = fopen(PID_FILE, "r");
    if (f != NULL) {
        if (fscanf(f, "%ld", &existing) != 1) {
            existing = 0;
        }
        fclose(f);
        if (existing > 0 && existing != (long) getpid()) {
            if (pid_alive(existing)) {
                int known = pid_is_photo_player(existing);
                if (known != 0) {
                    /* 1=确认实例；-1=/proc 不可读时退回 kill 判断 → 同样拒绝 */
                    log_error("photo-player: already running (pid %ld)", existing);
                    return -1;
                }
                log_info("photo-player: pid %ld alive but not photo-player (reused pid), taking over", existing);
            } else {
                log_info("photo-player: stale pid %ld in %s, taking over", existing, PID_FILE);
            }
        }
        /* existing == getpid()：脚本/旧流程写入了本进程 PID（bug 场景），直接接管覆盖 */
    }
    f = fopen(PID_FILE, "w");
    if (f == NULL) {
        log_error("photo-player: cannot write %s: %s", PID_FILE, strerror(errno));
        return -1;
    }
    fprintf(f, "%d\n", (int) getpid());
    fclose(f);
    return 0;
}

static int stop_file_present(void)
{
    struct stat st;

    return (stat(STOP_FILE, &st) == 0);
}

/* 主循环时钟：util 实现 + 每 500ms 分片睡眠，顺带轮询停止文件（KUAL --stop 通道） */
static long main_now_ms(void)
{
    return util_now_ms();
}

static void main_sleep_ms(long ms)
{
    long remaining = ms;

    while (remaining > 0) {
        long slice = (remaining > 500) ? 500 : remaining;
        if (g_quit) {
            return;
        }
        util_sleep_ms(slice);
        remaining -= slice;
        if (g_quit) {
            return;
        }
        if (stop_file_present()) {
            log_info("photo-player: stop file detected");
            g_quit = 1;
            return;
        }
        if (g_touch != NULL && touch_poll(g_touch) != 0) {
            log_info("photo-player: touch/power-key press detected, exiting");
            g_quit = 1;
            return;
        }
    }
}

static const PlayerClock g_main_clock = { main_now_ms, main_sleep_ms };

typedef struct {
    Display* display;
    const AppConfig* cfg;
} ShowCtx;

static int show_image(const char* path, void* user)
{
    ShowCtx* ctx = (ShowCtx*) user;

    if (ctx == NULL || ctx->display == NULL) {
        return -1;
    }
    log_info("photo-player: showing %s", path);
    return display_show(ctx->display, path, ctx->cfg);
}

/* §9.1 清理（新顺序）：display_close → platform_restore → source/touch cleanup → 删 PID/停止文件 → 日志；幂等。
 * 先清屏后恢复 framework：保证 framework 首绘永远落在我们最后的清屏之上，不会被我们白刷盖掉。 */
static void cleanup_all(PhotoSource** src, Display** disp, int show_hint)
{
    static int done = 0;

    if (done) {
        return;
    }
    done = 1;
    if (disp != NULL && *disp != NULL) {
        display_close(*disp, show_hint); /* no-framework 时附退出提示 */
        *disp = NULL;
    }
    platform_restore();
    if (src != NULL && *src != NULL) {
        (*src)->cleanup(*src);
        *src = NULL;
    }
    touch_close(g_touch); /* NULL 安全 */
    g_touch = NULL;
    (void) unlink(PID_FILE);
    (void) unlink(STOP_FILE); /* M3 起还会有临时图片，一并在此删除 */
    log_info("photo-player: cleanup done");
}

int main(int argc, char** argv)
{
    AppConfig cfg;
    PhotoSource* src = NULL;
    Display* disp = NULL;
    Player player;
    ShowCtx show_ctx;
    const char* conf_path = NULL;
    char err[256];
    int rv;
    int exit_code = 0;
    int i;

    config_defaults(&cfg);

    /* 预扫描 --config：加载顺序 defaults → conf 文件 → CLI 覆盖 */
    for (i = 1; i < argc; i++) {
        if (argv[i] != NULL && strcmp(argv[i], "--config") == 0) {
            if (i + 1 >= argc || argv[i + 1] == NULL) {
                log_error("photo-player: --config requires a value");
                fatal_screen("ERROR: bad config");
                return 1;
            }
            conf_path = argv[i + 1];
        }
    }
    if (conf_path != NULL) {
        if (config_load_file(&cfg, conf_path) != 0) {
            log_error("photo-player: cannot load config '%s'", conf_path);
            fatal_screen("ERROR: bad config");
            return 1;
        }
    } else {
        struct stat st;
        if (stat(DEFAULT_CONF, &st) == 0 && config_load_file(&cfg, DEFAULT_CONF) != 0) {
            log_error("photo-player: cannot load config '%s'", DEFAULT_CONF);
            fatal_screen("ERROR: bad config");
            return 1;
        }
    }

    rv = config_parse_args(&cfg, argc, argv);
    if (rv == 1) {
        print_usage();
        return 0;
    }
    if (rv != 0) {
        log_error("photo-player: bad arguments (try --help)");
        fatal_screen("ERROR: bad config");
        return 1;
    }
    if (config_validate(&cfg, err, sizeof(err)) != 0) {
        log_error("photo-player: %s", err);
        fatal_screen((cfg.source[0] == '\0') ? "ERROR: no --source" : "ERROR: bad config");
        return 1;
    }
    if (cfg.source_kind == SOURCE_HTTP) {
        /* M3 再实现；config 照常解析并识别 */
        log_error("photo-player: http source not implemented until M3");
        fatal_screen("ERROR: http source M3");
        return 3;
    }

    log_set_debug(cfg.debug);

    if (claim_pid_file() != 0) {
        return 2;
    }
    (void) unlink(STOP_FILE); /* 清掉遗留停止文件，避免启动即停 */

    /* SIGHUP 忽略（KUAL 后台化会发）；INT/TERM 置退出标志；不设 SA_RESTART 以便 nanosleep 被 EINTR 打断 */
    {
        struct sigaction sa;
        memset(&sa, 0, sizeof(sa));
        sa.sa_handler = on_signal;
        sigemptyset(&sa.sa_mask);
        sa.sa_flags = 0;
        sigaction(SIGINT, &sa, NULL);
        sigaction(SIGTERM, &sa, NULL);
        sa.sa_handler = SIG_IGN;
        sigaction(SIGHUP, &sa, NULL);
    }

    (void) platform_setup(&cfg); /* 失败仅记日志，不阻断播放 */

    disp = display_open(&cfg);
    if (disp == NULL) {
        log_error("photo-player: display init failed");
        /* 先 cleanup 再画错误屏：错误必须最后留在屏上（fatal_screen 的 cls 会洗掉退出提示） */
        cleanup_all(&src, &disp, cfg.no_framework);
        fatal_screen("ERROR: display init");
        return 1;
    }

    {
        /* 补丁1：单图上限（MB→字节）；32 位 long 防溢出钳到 LONG_MAX */
        long max_bytes = 0;
        if (cfg.max_file_mb > 0) {
            long long mb = (long long) cfg.max_file_mb * 1024LL * 1024LL;
            max_bytes = (mb > (long long) LONG_MAX) ? LONG_MAX : (long) mb;
        }
        src = source_local_open(cfg.source, max_bytes);
    }
    if (src == NULL) {
        log_error("photo-player: cannot open local source '%s'", cfg.source);
        cleanup_all(&src, &disp, cfg.no_framework);
        fatal_screen("ERROR: no images in dir");
        return 1;
    }
    source_local_set_order(src, cfg.order);

    /* 轻触/电源键退出（v1.1）：两路独立开关；设备缺失/打不开不致命，其它停止通道照常 */
    if (cfg.touch_exit || cfg.power_exit) {
        g_touch = touch_open(cfg.touch_exit, cfg.power_exit);
        if (g_touch == NULL) {
            log_info("photo-player: warning: touch/power exit unavailable, continuing without it");
        }
    } else {
        log_info("photo-player: touch/power exit disabled (TOUCH_EXIT=0 POWER_EXIT=0)");
    }

    show_ctx.display = disp;
    show_ctx.cfg = &cfg;
    player_init(&player, src, show_image, &show_ctx, &g_main_clock, cfg.interval_sec);

    log_info("photo-player: starting from '%s' (order=%s interval=%ds fit=%d flash_every=%d)",
             cfg.source,
             (cfg.order == ORDER_RANDOM) ? "random" : "seq",
             cfg.interval_sec,
             (int) cfg.fit,
             cfg.flash_every);

    /* 启动延迟：KUAL 执行 action 后 1~2s 还有一次自身重绘，早上屏会被按钮残影盖住（go-player 同款坑）。
     * 用 500ms 分片轮询睡（同 main_sleep_ms），期间照常响应 stop 文件与 SIGINT/SIGTERM。 */
    if (cfg.start_delay_ms > 0) {
        log_info("photo-player: start delay %dms (let launcher UI settle)", cfg.start_delay_ms);
        main_sleep_ms((long) cfg.start_delay_ms);
    }

    rv = player_run(&player, &g_quit);
    if (rv == 2) {
        /* 补丁2：显示链路持续失败（5s 间隔最坏 25s 到此），上屏报错并退出 */
        log_error("photo-player: display failing, exiting");
        cleanup_all(&src, &disp, cfg.no_framework);
        fatal_screen("ERROR: display failing");
        return 1;
    }
    if (rv != 0) {
        log_error("photo-player: player stopped on fatal error");
        exit_code = 1;
    }

    cleanup_all(&src, &disp, cfg.no_framework);
    log_info("photo-player: bye");
    return exit_code;
}
