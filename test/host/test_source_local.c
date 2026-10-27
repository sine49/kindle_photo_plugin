/* test_source_local.c — source_local 单元测试：扩展名过滤/顺序回绕/洗牌一轮不重复/空目录与坏文件/超大图拒绝 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "config.h"
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

static char g_dir[128];

static void touch_file(const char* name)
{
    char p[256];
    FILE* f;
    int wr = snprintf(p, sizeof(p), "%s/%s", g_dir, name);
    if (wr < 0 || (size_t) wr >= sizeof(p)) {
        printf("FATAL: path too long\n");
        exit(1);
    }
    f = fopen(p, "w");
    if (f == NULL) {
        printf("FATAL: cannot create %s\n", p);
        exit(1);
    }
    fputs("x", f);
    fclose(f);
}

/* 快速造大文件：定位到 size-1 再写一字节（稀疏文件，WSL/tmpfs 友好） */
static void make_big_file(const char* name, long size)
{
    char p[256];
    FILE* f;
    int wr = snprintf(p, sizeof(p), "%s/%s", g_dir, name);
    if (wr < 0 || (size_t) wr >= sizeof(p)) {
        printf("FATAL: path too long\n");
        exit(1);
    }
    f = fopen(p, "wb");
    if (f == NULL) {
        printf("FATAL: cannot create %s\n", p);
        exit(1);
    }
    if (size > 0 && fseek(f, size - 1, SEEK_SET) == 0) {
        fputc('x', f);
    }
    fclose(f);
}

static void rm_file(const char* name)
{
    char p[256];
    if (snprintf(p, sizeof(p), "%s/%s", g_dir, name) > 0) {
        (void) unlink(p);
    }
}

/* 比较 next() 输出路径的 basename */
static int got_name(const char* got, const char* name)
{
    const char* b = strrchr(got, '/');
    b = (b != NULL) ? b + 1 : got;
    return strcmp(b, name) == 0;
}

int test_source_local(void)
{
    PhotoSource* src;
    char out[512];
    int i;

    snprintf(g_dir, sizeof(g_dir), "/tmp/photo_plugins_test_%d", (int) getpid());
    (void) mkdir(g_dir, 0755);

    /* --- 空目录 / 坏参数 → open 失败 --- */
    ASSERT(source_local_open(g_dir, 0) == NULL);
    ASSERT(source_local_open(NULL, 0) == NULL);
    ASSERT(source_local_open("", 0) == NULL);
    ASSERT(source_local_open("/tmp/photo_plugins_no_such_dir_42", 0) == NULL);

    /* --- 扩展名过滤（大小写不敏感）+ 顺序回绕 --- */
    touch_file("a.jpg");
    touch_file("b.JPEG");
    touch_file("c.png");
    touch_file("d.bmp");
    touch_file("e.gif");
    touch_file("f.tga");
    touch_file("g.pnm");
    touch_file("h.JPG");
    touch_file("skip.txt");
    touch_file("noext");
    touch_file("weird.jpg.bak");
    {
        static const char* const expect[8] = { "a.jpg", "b.JPEG", "c.png", "d.bmp",
                                               "e.gif", "f.tga",  "g.pnm", "h.JPG" };
        src = source_local_open(g_dir, 0);
        ASSERT(src != NULL);
        if (src != NULL) {
            ASSERT(strcmp(src->name, "local") == 0);
            for (i = 0; i < 8; i++) {
                int rv = src->next(src, out, sizeof(out));
                ASSERT(rv == 0);
                if (rv == 0) {
                    ASSERT(got_name(out, expect[i]));
                }
            }
            ASSERT(src->next(src, out, sizeof(out)) == 0); /* 回绕到头 */
            ASSERT(got_name(out, "a.jpg"));
            ASSERT(src->next(src, out, sizeof(out)) == 0);
            ASSERT(got_name(out, "b.JPEG"));
            src->cleanup(src);
        }
    }
    rm_file("a.jpg");
    rm_file("b.JPEG");
    rm_file("c.png");
    rm_file("d.bmp");
    rm_file("e.gif");
    rm_file("f.tga");
    rm_file("g.pnm");
    rm_file("h.JPG");
    rm_file("skip.txt");
    rm_file("noext");
    rm_file("weird.jpg.bak");

    /* --- 随机：一轮内不重复，播完重洗再一轮仍不重复 --- */
    for (i = 0; i < 6; i++) {
        char name[32];
        snprintf(name, sizeof(name), "p%d.jpg", i);
        touch_file(name);
    }
    src = source_local_open(g_dir, 0);
    ASSERT(src != NULL);
    if (src != NULL) {
        char seen[6][512];
        int round;
        source_local_set_order(src, ORDER_RANDOM);
        for (round = 0; round < 2; round++) {
            int j;
            for (i = 0; i < 6; i++) {
                ASSERT(src->next(src, out, sizeof(out)) == 0);
                snprintf(seen[i], sizeof(seen[i]), "%s", out);
            }
            for (i = 0; i < 6; i++) {
                for (j = i + 1; j < 6; j++) {
                    ASSERT(strcmp(seen[i], seen[j]) != 0); /* 一轮内不重复 */
                }
            }
        }
        /* reset 后仍可用 */
        src->reset(src);
        ASSERT(src->next(src, out, sizeof(out)) == 0);
        src->cleanup(src);
    }
    for (i = 0; i < 6; i++) {
        char name[32];
        snprintf(name, sizeof(name), "p%d.jpg", i);
        rm_file(name);
    }

    /* --- 坏文件：open 后文件消失 → skip，连续失败超图片数 → fatal --- */
    touch_file("v0.jpg");
    touch_file("v1.jpg");
    touch_file("v2.jpg");
    src = source_local_open(g_dir, 0);
    ASSERT(src != NULL);
    if (src != NULL) {
        rm_file("v0.jpg");
        rm_file("v1.jpg");
        rm_file("v2.jpg");
        ASSERT(src->next(src, out, sizeof(out)) == 1); /* skip */
        ASSERT(src->next(src, out, sizeof(out)) == 1);
        ASSERT(src->next(src, out, sizeof(out)) == 1);
        ASSERT(src->next(src, out, sizeof(out)) == -1); /* 连续失败 > 3 → fatal */
        src->cleanup(src);
    }

    /* --- 输出缓冲区过小 → fatal（防溢出） --- */
    touch_file("w.jpg");
    src = source_local_open(g_dir, 0);
    ASSERT(src != NULL);
    if (src != NULL) {
        char tiny[4];
        ASSERT(src->next(src, tiny, sizeof(tiny)) == -1);
        src->cleanup(src);
    }
    rm_file("w.jpg");

    /* --- 补丁1：超大图不入列表（max_bytes=1MB 时 >1MB 的被拒） --- */
    touch_file("s0.jpg");
    touch_file("s1.jpg");
    make_big_file("big.jpg", 1024L * 1024L + 1L); /* 1MB+1 > 1MB 上限 */
    src = source_local_open(g_dir, 1024L * 1024L);
    ASSERT(src != NULL);
    if (src != NULL) {
        for (i = 0; i < 4; i++) {
            ASSERT(src->next(src, out, sizeof(out)) == 0);
            ASSERT(!got_name(out, "big.jpg")); /* 超限文件被拒，只出小文件（回绕） */
        }
        src->cleanup(src);
    }

    /* --- 全部超限 → open 失败（沿用 no readable image 启动失败路径） --- */
    rm_file("s0.jpg");
    rm_file("s1.jpg");
    ASSERT(source_local_open(g_dir, 1024L * 1024L) == NULL);

    /* --- max_bytes=0：不过滤，大文件照常入列表 --- */
    src = source_local_open(g_dir, 0);
    ASSERT(src != NULL);
    if (src != NULL) {
        int seen_big = 0;
        for (i = 0; i < 3; i++) {
            ASSERT(src->next(src, out, sizeof(out)) == 0);
            if (got_name(out, "big.jpg")) {
                seen_big = 1;
            }
        }
        ASSERT(seen_big); /* 不过滤时大文件可播 */
        src->cleanup(src);
    }
    rm_file("big.jpg");

    (void) rmdir(g_dir);
    return g_failures;
}
