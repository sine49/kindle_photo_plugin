/* test_player.c — player 单元测试：假时钟验证固定节拍不漂移、skip/fatal、quit 中断睡眠 */
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "player.h"
#include "source.h"

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

/* ---------- 假图片源 ---------- */
typedef struct {
    PhotoSource pub;
    int ret;  /* next() 返回值：0=ok,-1=fatal,1=skip */
    int calls;
    int idx;
} FakeSource;

static int fake_next(PhotoSource* s, char* out, size_t len)
{
    FakeSource* fs = (FakeSource*) s;
    fs->calls++;
    if (fs->ret != 0) {
        return fs->ret;
    }
    snprintf(out, len, "/fake/img%d.jpg", fs->idx++);
    return 0;
}

static void fake_reset(PhotoSource* s)
{
    (void) s;
}

static void fake_cleanup(PhotoSource* s)
{
    (void) s;
}

static void fake_init(FakeSource* fs, int ret)
{
    memset(fs, 0, sizeof(*fs));
    fs->pub.next = fake_next;
    fs->pub.reset = fake_reset;
    fs->pub.cleanup = fake_cleanup;
    fs->pub.name = "fake";
    fs->ret = ret;
}

/* ---------- 假时钟：记录 sleep 调用与假时间 ---------- */
typedef struct {
    long now;
    long sleeps[32];
    int nsleeps;
    volatile sig_atomic_t* quit;
    int quit_on_sleep; /* 睡满 N 次后置 quit；-1=不置 */
    int no_advance;    /* 1=睡眠不推进时间（模拟 EINTR 早醒） */
} FakeClock;

static FakeClock* g_clk;

static long fc_now(void)
{
    return g_clk->now;
}

static void fc_sleep(long ms)
{
    if (g_clk->nsleeps < (int)(sizeof(g_clk->sleeps) / sizeof(g_clk->sleeps[0]))) {
        g_clk->sleeps[g_clk->nsleeps] = ms;
    }
    g_clk->nsleeps++;
    if (!g_clk->no_advance) {
        g_clk->now += ms;
    }
    if (g_clk->quit != NULL && g_clk->quit_on_sleep >= 0 && g_clk->nsleeps >= g_clk->quit_on_sleep) {
        *g_clk->quit = 1;
    }
}

static const PlayerClock g_fake_clock = { fc_now, fc_sleep };

/* ---------- show 回调记录 ---------- */
typedef struct {
    FakeClock* clk;
    long cost_ms; /* 每次 show 消耗的假时间（模拟刷新耗时） */
    int calls;
    long call_times[32];
    char last[64];
    int ret;       /* seq_len==0 时的固定返回值 */
    int seq[64];   /* 脚本化返回值（按次取；耗尽后重复末位） */
    int seq_len;
    int seq_pos;
} ShowRec;

static int rec_show(const char* path, void* user)
{
    ShowRec* r = (ShowRec*) user;
    if (r->calls < (int)(sizeof(r->call_times) / sizeof(r->call_times[0]))) {
        r->call_times[r->calls] = r->clk->now;
    }
    r->calls++;
    snprintf(r->last, sizeof(r->last), "%s", path);
    if (r->clk != NULL) {
        r->clk->now += r->cost_ms;
    }
    if (r->seq_len > 0) {
        int v = (r->seq_pos < r->seq_len) ? r->seq[r->seq_pos] : r->seq[r->seq_len - 1];
        r->seq_pos++;
        return v;
    }
    return r->ret;
}

int test_player(void)
{
    FakeSource fs;
    ShowRec sr;
    FakeClock clk;
    Player p;
    volatile sig_atomic_t quit = 0;

    memset(&sr, 0, sizeof(sr));
    memset(&clk, 0, sizeof(clk));
    clk.quit = &quit;
    clk.quit_on_sleep = -1;
    g_clk = &clk;
    sr.clk = &clk;

    /* --- player_tick：ok / skip / fatal / show 失败 → skip --- */
    fake_init(&fs, 0);
    player_init(&p, &fs.pub, rec_show, &sr, &g_fake_clock, 2);
    ASSERT(player_tick(&p) == 0);
    ASSERT(sr.calls == 1);
    ASSERT(p.ticks == 1);
    ASSERT(strcmp(sr.last, "/fake/img0.jpg") == 0);

    fake_init(&fs, 1);
    player_init(&p, &fs.pub, rec_show, &sr, &g_fake_clock, 2);
    sr.calls = 0;
    ASSERT(player_tick(&p) == 1);
    ASSERT(sr.calls == 0);

    fake_init(&fs, -1);
    player_init(&p, &fs.pub, rec_show, &sr, &g_fake_clock, 2);
    ASSERT(player_tick(&p) == -1);

    fake_init(&fs, 0);
    player_init(&p, &fs.pub, rec_show, &sr, &g_fake_clock, 2);
    sr.calls = 0;
    sr.ret = -1;
    ASSERT(player_tick(&p) == 1); /* show 失败按 skip 处理 */
    ASSERT(sr.calls == 1);
    sr.ret = 0;

    /* --- 固定节拍不漂移：show 耗 300ms，interval=2s，睡 3 次后 quit ---
     * 期望：tick 时刻 0 / 2000 / 4000（严格 interval 节拍），每次 sleep = 1700ms */
    fake_init(&fs, 0);
    quit = 0;
    clk.now = 0;
    clk.nsleeps = 0;
    clk.quit_on_sleep = 3;
    clk.no_advance = 0;
    sr.calls = 0;
    sr.cost_ms = 300;
    player_init(&p, &fs.pub, rec_show, &sr, &g_fake_clock, 2);
    ASSERT(player_run(&p, &quit) == 0);
    ASSERT(sr.calls == 3);
    ASSERT(p.ticks == 3);
    ASSERT(clk.nsleeps == 3);
    ASSERT(clk.sleeps[0] == 1700 && clk.sleeps[1] == 1700 && clk.sleeps[2] == 1700);
    ASSERT(sr.call_times[0] == 0 && sr.call_times[1] == 2000 && sr.call_times[2] == 4000);
    ASSERT(clk.now == 6000); /* 3 节拍后正好停在 3*2000 */

    /* --- 无 show 耗时：每次 sleep 正好一个 interval --- */
    fake_init(&fs, 0);
    quit = 0;
    clk.now = 0;
    clk.nsleeps = 0;
    clk.quit_on_sleep = 4;
    sr.calls = 0;
    sr.cost_ms = 0;
    player_init(&p, &fs.pub, rec_show, &sr, &g_fake_clock, 5);
    ASSERT(player_run(&p, &quit) == 0);
    ASSERT(sr.calls == 4);
    ASSERT(clk.nsleeps == 4);
    ASSERT(clk.sleeps[0] == 5000 && clk.sleeps[3] == 5000);
    ASSERT(sr.call_times[3] == 15000);

    /* --- quit 中断睡眠：sleep 早醒并置 quit → 立即退出，不再 tick --- */
    fake_init(&fs, 0);
    quit = 0;
    clk.now = 0;
    clk.nsleeps = 0;
    clk.quit_on_sleep = 1;
    clk.no_advance = 1;
    sr.calls = 0;
    sr.cost_ms = 0;
    player_init(&p, &fs.pub, rec_show, &sr, &g_fake_clock, 2);
    ASSERT(player_run(&p, &quit) == 0);
    ASSERT(sr.calls == 1);
    ASSERT(clk.nsleeps == 1);
    ASSERT(quit == 1);

    /* --- skip 保持节拍继续 --- */
    fake_init(&fs, 1);
    quit = 0;
    clk.now = 0;
    clk.nsleeps = 0;
    clk.quit_on_sleep = 2;
    clk.no_advance = 0;
    sr.calls = 0;
    player_init(&p, &fs.pub, rec_show, &sr, &g_fake_clock, 3);
    ASSERT(player_run(&p, &quit) == 0);
    ASSERT(sr.calls == 0);
    ASSERT(clk.nsleeps == 2);

    /* --- fatal：run 立即返回 -1 --- */
    fake_init(&fs, -1);
    quit = 0;
    clk.now = 0;
    clk.nsleeps = 0;
    clk.quit_on_sleep = -1;
    player_init(&p, &fs.pub, rec_show, &sr, &g_fake_clock, 3);
    ASSERT(player_run(&p, &quit) == -1);

    /* --- quit 已置位：一拍不打 --- */
    fake_init(&fs, 0);
    quit = 1;
    clk.now = 0;
    clk.nsleeps = 0;
    sr.calls = 0;
    player_init(&p, &fs.pub, rec_show, &sr, &g_fake_clock, 3);
    ASSERT(player_run(&p, &quit) == 0);
    ASSERT(sr.calls == 0);
    ASSERT(p.ticks == 0);

    /* --- 补丁2：连续 show 失败 ≥5 → 返回 2（防空转 livelock） --- */
    fake_init(&fs, 0);
    player_init(&p, &fs.pub, rec_show, &sr, &g_fake_clock, 2);
    sr.calls = 0;
    sr.ret = -1;
    sr.seq_len = 0;
    ASSERT(player_tick(&p) == 1);
    ASSERT(player_tick(&p) == 1);
    ASSERT(player_tick(&p) == 1);
    ASSERT(player_tick(&p) == 1);
    ASSERT(player_tick(&p) == 2); /* 第 5 次连续失败 */

    /* 中途成功一次 → 计数清零，不提前触发 */
    fake_init(&fs, 0);
    player_init(&p, &fs.pub, rec_show, &sr, &g_fake_clock, 2);
    sr.calls = 0;
    {
        static const int script[10] = { -1, -1, -1, -1, 0, -1, -1, -1, -1, -1 };
        memcpy(sr.seq, script, sizeof(script));
        sr.seq_len = 10;
        sr.seq_pos = 0;
    }
    ASSERT(player_tick(&p) == 1);
    ASSERT(player_tick(&p) == 1);
    ASSERT(player_tick(&p) == 1);
    ASSERT(player_tick(&p) == 1);
    ASSERT(player_tick(&p) == 0); /* 成功 → 清零 */
    ASSERT(player_tick(&p) == 1);
    ASSERT(player_tick(&p) == 1);
    ASSERT(player_tick(&p) == 1);
    ASSERT(player_tick(&p) == 1);
    ASSERT(player_tick(&p) == 2); /* 再连续 5 次才触发 */
    sr.seq_len = 0;
    sr.ret = 0;

    /* source next 返回的 skip 不计入 show 失败 */
    fake_init(&fs, 0);
    player_init(&p, &fs.pub, rec_show, &sr, &g_fake_clock, 2);
    sr.calls = 0;
    sr.ret = -1;
    ASSERT(player_tick(&p) == 1); /* show 失败 1 */
    ASSERT(player_tick(&p) == 1); /* show 失败 2 */
    fs.ret = 1;
    ASSERT(player_tick(&p) == 1); /* source skip */
    ASSERT(player_tick(&p) == 1); /* source skip */
    fs.ret = 0;
    ASSERT(player_tick(&p) == 1); /* show 失败 3（skip 未计数） */
    ASSERT(player_tick(&p) == 1); /* show 失败 4 */
    ASSERT(player_tick(&p) == 2); /* show 失败 5 → 2 */
    sr.ret = 0;

    /* player_run 传播 2 */
    fake_init(&fs, 0);
    quit = 0;
    clk.now = 0;
    clk.nsleeps = 0;
    clk.quit_on_sleep = -1;
    clk.no_advance = 0;
    sr.calls = 0;
    sr.ret = -1;
    player_init(&p, &fs.pub, rec_show, &sr, &g_fake_clock, 2);
    ASSERT(player_run(&p, &quit) == 2);
    ASSERT(sr.calls == 5);
    ASSERT(clk.nsleeps == 4); /* 前 4 轮 skip 后照常睡满节拍 */

    /* --- 默认时钟冒烟：单调、sleep 推进时间 --- */
    {
        const PlayerClock* c = player_clock_default();
        long t1;
        long t2;
        ASSERT(c != NULL && c->now_ms != NULL && c->sleep_ms != NULL);
        if (c != NULL) {
            t1 = c->now_ms();
            c->sleep_ms(20);
            t2 = c->now_ms();
            ASSERT(t2 >= t1);
            ASSERT(t2 - t1 >= 5);
        }
    }

    return g_failures;
}
