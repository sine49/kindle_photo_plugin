/* touch.c — 轻触/电源键退出实现：自扫 /dev/input/event*
 * 触摸（Kindle 7 = Neonode zForce 红外）：EVIOCGBIT 判 MT 坐标/MT 触点/BTN_TOUCH+坐标，EVIOCGPROP 排除加速度计；
 * 电源键（gpio-keys/pmic 类）：EV_KEY 含 KEY_POWER，名字含 power|pmic|gpio 优先。
 * 被动监听、不 EVIOGRAB、不干预系统行为；O_NONBLOCK 读 struct input_event。
 * 说明：FBInk 的 fbink_input_scan 在独立 libfbink_input.a（不在 libfbink.a），故自扫。 */
#define _POSIX_C_SOURCE 200809L
#include "touch.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#include "log.h"

/* 老内核头文件兜底（FBInk 同款做法） */
#ifndef ABS_MT_TRACKING_ID
#	define ABS_MT_TRACKING_ID 0x39
#endif
#ifndef BTN_TOOL_FINGER
#	define BTN_TOOL_FINGER 0x145
#endif
#ifndef EVIOCGPROP
#	define EVIOCGPROP(len) _IOC(_IOC_READ, 'E', 0x09, len)
#endif
#ifndef INPUT_PROP_DIRECT
#	define INPUT_PROP_DIRECT 0x01
#endif
#ifndef INPUT_PROP_ACCELEROMETER
#	define INPUT_PROP_ACCELEROMETER 0x06
#endif
#ifndef INPUT_PROP_MAX
#	define INPUT_PROP_MAX 0x1f
#endif

#define DEV_INPUT_DIR "/dev/input"
#define FALLBACK_DEV  "/dev/input/event1" /* 触摸兜底：Kindle 7 zForce 落在 event1（go-player 同款） */
#define MAX_CANDIDATES 16

struct TouchWatch {
    int touch_fd;     /* 触摸设备；-1=未开 */
    int touch_zforce; /* 触摸 fd 是否 zForce（close 竞态防护） */
    int power_fd;     /* 电源键设备；-1=未开；可能与 touch_fd 同一节点 */
};

/* 内核兼容位测试（与 FBInk input scan 同款位运算语义） */
#define BITS_PER_LONG        (sizeof(unsigned long) * 8)
#define NBITS(x)             ((((x) - 1) / BITS_PER_LONG) + 1)
#define TEST_BIT(bit, array) \
    (((array)[(unsigned int) (bit) / BITS_PER_LONG] >> ((unsigned int) (bit) % BITS_PER_LONG)) & 1UL)

typedef struct {
    int hit;         /* 1=触摸设备 */
    int score;       /* 触摸择优权重 */
    char reason[64]; /* 诊断：命中/跳过原因 */
} TouchClass;

typedef struct {
    int fd;
    int score;     /* 触摸 score / 电源键 prio 共用 */
    int is_zforce;
    char path[288];
    char name[256];
} Candidate;

/* 大小写不敏感子串匹配（不用 GNU 的 strcasestr） */
static int name_has(const char* name, const char* tag)
{
    size_t n = strlen(name);
    size_t t = strlen(tag);
    size_t i;

    if (t == 0 || t > n) {
        return 0;
    }
    for (i = 0; i + t <= n; i++) {
        size_t j;
        for (j = 0; j < t; j++) {
            if (tolower((unsigned char) name[i + j]) != tolower((unsigned char) tag[j])) {
                break;
            }
        }
        if (j == t) {
            return 1;
        }
    }
    return 0;
}

static int is_zforce_name(const char* name)
{
    return (name_has(name, "zforce") || name_has(name, "neonode")) ? 1 : 0;
}

/* FBInk 对 zForce-ir-touch 的 50ms 惯例：让面板 Active/Deactivate 稳定 */
static void zforce_quiet(void)
{
    struct timespec ts;

    ts.tv_sec = 0;
    ts.tv_nsec = 50000000L;
    (void) nanosleep(&ts, NULL);
}

/* zForce 面板 close 竞态防护：close 前后各歇 50ms（只在关闭路径，绝不进主循环） */
static void close_device(int fd, int is_zforce)
{
    if (fd < 0) {
        return;
    }
    if (is_zforce) {
        zforce_quiet();
        (void) close(fd);
        zforce_quiet();
    } else {
        (void) close(fd);
    }
}

static void get_device_name(int fd, char* name, size_t len)
{
    if (len == 0) {
        return;
    }
    memset(name, 0, len);
    if (ioctl(fd, EVIOCGNAME(len - 1), name) < 0) {
        snprintf(name, len, "<unknown>");
    }
    name[len - 1] = '\0'; /* ioctl 不保证 NUL 终止，强制封口 */
}

/* 触摸设备判定（FBInk test_pointers 的 udev 配方移植） */
static void classify_device(int fd, const char* name, TouchClass* out)
{
    unsigned long ev_bits[NBITS(EV_MAX)];
    unsigned long key_bits[NBITS(KEY_MAX)];
    unsigned long abs_bits[NBITS(ABS_MAX)];
    unsigned long prop_bits[NBITS(INPUT_PROP_MAX)];
    int has_ev_abs = 0;
    int has_ev_key = 0;
    int has_btn_touch = 0; /* 注意：BTN_TOUCH 是 EV_KEY 码，只测 EVIOCGBIT(EV_KEY) 位图 */
    int has_mt_track = 0;
    int has_mt_xy = 0;
    int has_xy = 0;
    int has_z = 0;
    int has_prop_direct = 0;
    int has_prop_accel = 0;
    int named;

    memset(out, 0, sizeof(*out));
    named = (is_zforce_name(name) || name_has(name, "cyttsp") || name_has(name, "parade") ||
             name_has(name, "qt602240") || name_has(name, "synaptics") || name_has(name, "touch"));

    memset(ev_bits, 0, sizeof(ev_bits));
    if (ioctl(fd, EVIOCGBIT(0, sizeof(ev_bits)), ev_bits) < 0) {
        snprintf(out->reason, sizeof(out->reason), "skip: ev bits unreadable");
        return;
    }
    has_ev_abs = TEST_BIT(EV_ABS, ev_bits) ? 1 : 0;
    has_ev_key = TEST_BIT(EV_KEY, ev_bits) ? 1 : 0;

    /* props：老内核 EVIOCGPROP 返回 EINVAL 时忽略 props 并放宽启发式，绝不因拿不到 props 拒绝 */
    memset(prop_bits, 0, sizeof(prop_bits));
    if (ioctl(fd, EVIOCGPROP(sizeof(prop_bits)), prop_bits) == 0) {
        has_prop_direct = TEST_BIT(INPUT_PROP_DIRECT, prop_bits) ? 1 : 0;
        has_prop_accel = TEST_BIT(INPUT_PROP_ACCELEROMETER, prop_bits) ? 1 : 0;
    }

    memset(abs_bits, 0, sizeof(abs_bits));
    if (has_ev_abs && ioctl(fd, EVIOCGBIT(EV_ABS, sizeof(abs_bits)), abs_bits) == 0) {
        has_mt_xy = (TEST_BIT(ABS_MT_POSITION_X, abs_bits) && TEST_BIT(ABS_MT_POSITION_Y, abs_bits)) ? 1 : 0;
        has_mt_track = TEST_BIT(ABS_MT_TRACKING_ID, abs_bits) ? 1 : 0;
        has_xy = (TEST_BIT(ABS_X, abs_bits) && TEST_BIT(ABS_Y, abs_bits)) ? 1 : 0;
        has_z = TEST_BIT(ABS_Z, abs_bits) ? 1 : 0;
    }

    memset(key_bits, 0, sizeof(key_bits));
    if (has_ev_key && ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(key_bits)), key_bits) == 0) {
        has_btn_touch = TEST_BIT(BTN_TOUCH, key_bits) ? 1 : 0;
    }

    /* 排除加速度计 */
    if (has_prop_accel) {
        snprintf(out->reason, sizeof(out->reason), "skip: accel (input prop)");
        return;
    }
    if (!has_ev_key && has_xy && has_z) {
        snprintf(out->reason, sizeof(out->reason), "skip: accel (abs xyz, no ev_key)");
        return;
    }

    if (!has_ev_abs) {
        snprintf(out->reason, sizeof(out->reason), "skip: no ev_abs");
        return;
    }
    /* 触摸配方：MT 坐标（zForce 关键谓词）/ MT 触点跟踪 / BTN_TOUCH+坐标 */
    if (!(has_mt_xy || has_mt_track || (has_btn_touch && (has_xy || has_mt_xy)))) {
        snprintf(out->reason, sizeof(out->reason), "skip: no touch caps");
        return;
    }

    out->hit = 1;
    /* 择优：INPUT_PROP_DIRECT > 名字含 zforce|cyttsp|parade|qt602240|synaptics|touch > BTN_TOUCH+TRACKING > 仅 MT 坐标 */
    if (has_prop_direct) {
        out->score += 1000;
    }
    if (named) {
        out->score += 500;
    }
    if (has_btn_touch && has_mt_track) {
        out->score += 200;
    }
    if (has_mt_xy) {
        out->score += 100;
    }
    if (has_btn_touch && (has_xy || has_mt_xy)) {
        out->score += 50;
    }
    snprintf(out->reason, sizeof(out->reason), "hit: %s%s%s%s",
             has_mt_xy ? "mt-xy " : "",
             has_mt_track ? "mt-track " : "",
             has_btn_touch ? "btn-touch " : "",
             has_prop_direct ? "direct " : "");
}

/* 电源键判定：EV_KEY 含 KEY_POWER；名字含 power|pmic|gpio 者优先（*prio_out=1） */
static int classify_power(int fd, const char* name, int* prio_out)
{
    unsigned long ev_bits[NBITS(EV_MAX)];
    unsigned long key_bits[NBITS(KEY_MAX)];

    *prio_out = 0;
    memset(ev_bits, 0, sizeof(ev_bits));
    if (ioctl(fd, EVIOCGBIT(0, sizeof(ev_bits)), ev_bits) < 0) {
        return 0;
    }
    if (!TEST_BIT(EV_KEY, ev_bits)) {
        return 0;
    }
    memset(key_bits, 0, sizeof(key_bits));
    if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(key_bits)), key_bits) < 0) {
        return 0;
    }
    if (!TEST_BIT(KEY_POWER, key_bits)) {
        return 0;
    }
    *prio_out = (name_has(name, "power") || name_has(name, "pmic") || name_has(name, "gpio")) ? 1 : 0;
    return 1;
}

/* 开 fd 后先非阻塞排空陈旧事件（go-player 教训：不 drain 会误触发/不触发） */
static int drain_stale(int fd)
{
    struct input_event ev;
    int n = 0;

    if (fd < 0) {
        return 0;
    }
    while (read(fd, &ev, sizeof(ev)) == (ssize_t) sizeof(ev)) {
        n++;
    }
    return n;
}

/* 排空一路 fd 的事件：命中任一触发条件立即返回 1（按下即退，不做防抖等待） */
static int fd_triggered(int fd)
{
    struct input_event ev;

    if (fd < 0) {
        return 0;
    }
    for (;;) {
        ssize_t n = read(fd, &ev, sizeof(ev));
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return 0; /* 无事件 */
            }
            log_error("touch: read failed: %s", strerror(errno));
            return 0; /* 设备异常不致命，退出交给其它停止通道 */
        }
        if (n != (ssize_t) sizeof(ev)) {
            return 0; /* 不完整事件：保守丢弃 */
        }
        /* 触摸：BTN_TOUCH / BTN_TOOL_FINGER 按下（lab126 zForce/Parade 会报后者）或新 MT 触点 */
        if (ev.type == EV_KEY && (ev.code == BTN_TOUCH || ev.code == BTN_TOOL_FINGER) && ev.value == 1) {
            return 1;
        }
        if (ev.type == EV_ABS && ev.code == ABS_MT_TRACKING_ID && ev.value >= 0) {
            return 1;
        }
        /* 电源键：KEY_POWER 按下（value==1；忽略 release 的 0），按下即触发 */
        if (ev.type == EV_KEY && ev.code == KEY_POWER && ev.value == 1) {
            return 1;
        }
    }
}

TouchWatch* touch_open(int want_touch, int want_power)
{
    DIR* d;
    struct dirent* ent;
    Candidate tc[MAX_CANDIDATES];
    Candidate pc[MAX_CANDIDATES];
    int ntc = 0;
    int npc = 0;
    TouchWatch* tw = NULL;
    int best_t = -1;
    int best_p = -1;
    int keep_t = -1;
    int keep_p = -1;
    int i;

    if (!want_touch && !want_power) {
        return NULL;
    }
    d = opendir(DEV_INPUT_DIR);
    if (d == NULL) {
        log_info("touch: warning: %s not readable, touch/power exit disabled (%s)", DEV_INPUT_DIR, strerror(errno));
        return NULL;
    }
    while ((ent = readdir(d)) != NULL) {
        char path[288];
        char name[256];
        TouchClass cls;
        int fd;
        int wr;
        int zf;
        int prio = 0;
        int is_power = 0;
        int stored = 0;

        if (strncmp(ent->d_name, "event", 5) != 0) {
            continue;
        }
        wr = snprintf(path, sizeof(path), "%s/%s", DEV_INPUT_DIR, ent->d_name);
        if (wr < 0 || (size_t) wr >= sizeof(path)) {
            continue;
        }
        fd = open(path, O_RDONLY | O_NONBLOCK);
        if (fd < 0) {
            log_info("touch: scan %s: open failed: %s", path, strerror(errno)); /* errno 必打 */
            continue;
        }
        get_device_name(fd, name, sizeof(name));
        zf = is_zforce_name(name);

        if (want_touch) {
            classify_device(fd, name, &cls);
        } else {
            memset(&cls, 0, sizeof(cls));
            snprintf(cls.reason, sizeof(cls.reason), "touch: not requested");
        }
        if (want_power) {
            is_power = classify_power(fd, name, &prio);
        }

        log_info("touch: scan %s '%s': %s%s", path, name, cls.reason, is_power ? " | power-key" : "");

        if (cls.hit && ntc < MAX_CANDIDATES) {
            tc[ntc].fd = fd;
            tc[ntc].score = cls.score;
            tc[ntc].is_zforce = zf;
            memcpy(tc[ntc].path, path, strlen(path) + 1);
            memcpy(tc[ntc].name, name, strlen(name) + 1);
            ntc++;
            stored = 1;
        }
        if (is_power && npc < MAX_CANDIDATES) {
            pc[npc].fd = fd;
            pc[npc].score = prio;
            pc[npc].is_zforce = zf;
            memcpy(pc[npc].path, path, strlen(path) + 1);
            memcpy(pc[npc].name, name, strlen(name) + 1);
            npc++;
            stored = 1;
        }
        if (!stored) {
            close_device(fd, zf); /* 非候选（或候选表满）：zforce 安全关闭 */
        }
    }
    closedir(d);

    /* 触摸择优：分高者胜，同分先到先得 */
    for (i = 0; i < ntc; i++) {
        if (best_t < 0 || tc[i].score > tc[best_t].score) {
            best_t = i;
        }
    }
    /* 电源键择优：名字含 power|pmic|gpio 优先，取第一个能开的（open 失败已在扫描期跳过） */
    for (i = 0; i < npc; i++) {
        if (best_p < 0 || pc[i].score > pc[best_p].score) {
            best_p = i;
        }
    }
    keep_t = (best_t >= 0) ? tc[best_t].fd : -1;
    keep_p = (best_p >= 0) ? pc[best_p].fd : -1;

    /* 落选 fd 关闭（去重：共享节点只关一次） */
    for (i = 0; i < ntc; i++) {
        if (tc[i].fd != keep_t && tc[i].fd != keep_p) {
            close_device(tc[i].fd, tc[i].is_zforce);
        }
    }
    for (i = 0; i < npc; i++) {
        if (pc[i].fd != keep_t && pc[i].fd != keep_p) {
            close_device(pc[i].fd, pc[i].is_zforce);
        }
    }

    tw = (TouchWatch*) calloc(1, sizeof(*tw));
    if (tw == NULL) {
        if (keep_t >= 0) {
            close_device(keep_t, (best_t >= 0) ? tc[best_t].is_zforce : 0);
        }
        if (keep_p >= 0 && keep_p != keep_t) {
            close_device(keep_p, (best_p >= 0) ? pc[best_p].is_zforce : 0);
        }
        log_info("touch: warning: out of memory, touch/power exit disabled");
        return NULL;
    }
    tw->touch_fd = -1;
    tw->power_fd = -1;

    if (best_t >= 0) {
        tw->touch_fd = keep_t;
        tw->touch_zforce = tc[best_t].is_zforce;
        log_info("touch: watching touch %s '%s' (score %d)", tc[best_t].path, tc[best_t].name, tc[best_t].score);
    } else if (want_touch) {
        /* 触摸兜底：无命中 → 硬编码 event1（此机型 zForce 即 event1）；仍打不开才算失败 */
        int fd;

        log_info("touch: no touch candidate matched, trying fallback %s", FALLBACK_DEV);
        fd = open(FALLBACK_DEV, O_RDONLY | O_NONBLOCK);
        if (fd < 0) {
            log_info("touch: fallback %s open failed: %s", FALLBACK_DEV, strerror(errno));
        } else {
            char nbuf[256];
            get_device_name(fd, nbuf, sizeof(nbuf));
            tw->touch_fd = fd;
            tw->touch_zforce = is_zforce_name(nbuf);
            log_info("touch: watching touch fallback %s '%s'", FALLBACK_DEV, nbuf);
        }
    }
    if (best_p >= 0) {
        tw->power_fd = keep_p;
        log_info("touch: watching power %s '%s'", pc[best_p].path, pc[best_p].name);
    } else if (want_power) {
        log_info("touch: warning: no power key device found, power exit disabled");
    }

    if (tw->touch_fd < 0 && tw->power_fd < 0) {
        free(tw);
        log_info("touch: warning: no usable input device found, touch/power exit disabled");
        return NULL;
    }

    /* 开 fd 后先非阻塞排空陈旧事件（两路各自 drain） */
    {
        int dt = drain_stale(tw->touch_fd);
        int dp = (tw->power_fd != tw->touch_fd) ? drain_stale(tw->power_fd) : 0;
        log_info("touch: drained %d stale event(s) (touch), %d (power)", dt, dp);
    }
    return tw;
}

int touch_poll(TouchWatch* tw)
{
    if (tw == NULL) {
        return 0;
    }
    if (fd_triggered(tw->touch_fd)) {
        return 1;
    }
    if (tw->power_fd >= 0 && tw->power_fd != tw->touch_fd && fd_triggered(tw->power_fd)) {
        return 1;
    }
    return 0;
}

void touch_close(TouchWatch* tw)
{
    int shared;

    if (tw == NULL) {
        return;
    }
    shared = (tw->power_fd >= 0 && tw->power_fd == tw->touch_fd) ? 1 : 0;
    if (tw->touch_fd >= 0) {
        close_device(tw->touch_fd, tw->touch_zforce);
        tw->touch_fd = -1;
    }
    if (!shared && tw->power_fd >= 0) {
        close_device(tw->power_fd, 0);
        tw->power_fd = -1;
    }
    tw->power_fd = -1;
    free(tw);
}
