/* source_local.c — 本地目录图片源：非递归扫描、扩展名过滤、顺序游标回绕/随机洗牌（一轮不重复） */
#define _POSIX_C_SOURCE 200809L
#include "source.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "log.h"

/* FBInk fbink_print_image 支持的格式（DESIGN.md §6.2） */
static const char* const k_image_exts[] = { "jpg", "jpeg", "png", "bmp", "gif", "tga", "pnm", NULL };

typedef struct {
    PhotoSource pub; /* 必须是首成员：(LocalSource*) <-> (PhotoSource*) 双向转换 */
    char* dir;
    char** files;  /* 排序后的完整路径数组 */
    int n;
    int cursor;    /* ORDER_SEQ 游标 */
    int* order;    /* ORDER_RANDOM 洗牌下标 */
    int order_pos;
    unsigned int seed; /* rand_r 种子：time ^ pid */
    PlayOrder order_mode;
    int consec_fail;   /* 连续取图失败计数 */
    long max_bytes;    /* 单文件上限（字节），0=不限 */
} LocalSource;

static int has_image_ext(const char* name)
{
    const char* dot;
    const char* ext;
    int i;

    if (name == NULL) {
        return 0;
    }
    dot = strrchr(name, '.');
    if (dot == NULL || dot[1] == '\0') {
        return 0;
    }
    ext = dot + 1;
    for (i = 0; k_image_exts[i] != NULL; i++) {
        if (strcasecmp(ext, k_image_exts[i]) == 0) {
            return 1;
        }
    }
    return 0;
}

static int path_join(char** out, const char* dir, const char* name)
{
    size_t dl = strlen(dir);
    size_t nl = strlen(name);
    int need_slash = (dl > 0 && dir[dl - 1] != '/');
    char* p = (char*) malloc(dl + (size_t)(need_slash ? 1 : 0) + nl + 1);

    if (p == NULL) {
        return -1;
    }
    memcpy(p, dir, dl);
    if (need_slash) {
        p[dl] = '/';
    }
    memcpy(p + dl + (size_t)(need_slash ? 1 : 0), name, nl + 1);
    *out = p;
    return 0;
}

static void free_file_list(char** files, int n)
{
    int i;

    if (files == NULL) {
        return;
    }
    for (i = 0; i < n; i++) {
        free(files[i]);
    }
    free(files);
}

static int cmp_str(const void* a, const void* b)
{
    return strcmp(*(const char* const*) a, *(const char* const*) b);
}

/* 扫描目录并原子替换列表；失败时保留旧列表。成功时复位游标/洗牌表 */
static int local_scan(LocalSource* ls, const char* dir)
{
    DIR* d;
    struct dirent* ent;
    char** found = NULL;
    int n = 0;
    int cap = 0;
    int* order = NULL;
    int skipped_oversize = 0;
    int i;

    d = opendir(dir);
    if (d == NULL) {
        log_error("source_local: cannot open dir '%s': %s", dir, strerror(errno));
        return -1;
    }
    while ((ent = readdir(d)) != NULL) {
        char* full = NULL;
        struct stat st;

        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) {
            continue;
        }
        if (!has_image_ext(ent->d_name)) {
            continue;
        }
        if (path_join(&full, dir, ent->d_name) != 0) {
            log_error("source_local: out of memory");
            goto fail;
        }
        if (stat(full, &st) != 0 || S_ISDIR(st.st_mode)) {
            free(full);
            continue;
        }
        /* 补丁1：超大图直接拒之门外（防渐进式 JPEG 解码 OOM）；max_bytes<=0 零开销跳过 */
        if (ls->max_bytes > 0 && (long long) st.st_size > (long long) ls->max_bytes) {
            skipped_oversize++;
            log_info("source_local: warning: oversized '%s' (%lld bytes > limit %ld), skipped",
                     full,
                     (long long) st.st_size,
                     ls->max_bytes);
            free(full);
            continue;
        }
        if (access(full, R_OK) != 0) {
            log_info("source_local: not readable, ignoring: %s", full);
            free(full);
            continue;
        }
        if (n == cap) {
            int ncap = (cap > 0) ? cap * 2 : 16;
            char** tmp = (char**) realloc(found, (size_t) ncap * sizeof(*tmp));
            if (tmp == NULL) {
                free(full);
                log_error("source_local: out of memory");
                goto fail;
            }
            found = tmp;
            cap = ncap;
        }
        found[n++] = full;
    }
    closedir(d);
    d = NULL;

    if (skipped_oversize > 0) {
        log_info("source_local: %d oversized file(s) skipped (limit %ldMB)",
                 skipped_oversize,
                 ls->max_bytes / (1024L * 1024L));
    }
    if (n == 0) {
        log_error("source_local: no readable image in '%s'", dir);
        free(found);
        return -1;
    }
    qsort(found, (size_t) n, sizeof(*found), cmp_str);

    order = (int*) malloc((size_t) n * sizeof(int));
    if (order == NULL) {
        log_error("source_local: out of memory");
        goto fail;
    }
    for (i = 0; i < n; i++) {
        order[i] = i;
    }

    /* 提交新列表 */
    free_file_list(ls->files, ls->n);
    free(ls->order);
    ls->files = found;
    ls->n = n;
    ls->order = order;
    ls->cursor = 0;
    ls->order_pos = 0;
    ls->consec_fail = 0;
    return 0;

fail:
    if (d != NULL) {
        closedir(d);
    }
    free_file_list(found, n);
    free(order);
    return -1;
}

/* Fisher-Yates 洗牌（rand_r），一轮内不重复，播完由 next 重洗 */
static void local_reshuffle(LocalSource* ls)
{
    int i;

    if (ls->order == NULL || ls->n <= 0) {
        return;
    }
    for (i = ls->n - 1; i > 0; i--) {
        int j = (int)(rand_r(&ls->seed) % (unsigned int)(i + 1));
        int tmp = ls->order[i];
        ls->order[i] = ls->order[j];
        ls->order[j] = tmp;
    }
    ls->order_pos = 0;
}

static int local_next(PhotoSource* s, char* out_path, size_t len)
{
    LocalSource* ls = (LocalSource*) s;
    int idx;
    const char* path;
    int wr;

    if (ls == NULL || out_path == NULL || len == 0) {
        return -1;
    }
    if (ls->n <= 0 || ls->files == NULL) {
        return -1;
    }
    if (ls->order_mode == ORDER_RANDOM) {
        if (ls->order == NULL) {
            return -1;
        }
        if (ls->order_pos >= ls->n) {
            local_reshuffle(ls); /* 播完一轮重洗 */
        }
        idx = ls->order[ls->order_pos++];
    } else {
        idx = ls->cursor++;
        if (ls->cursor >= ls->n) {
            ls->cursor = 0; /* 到尾回绕 */
        }
    }
    if (idx < 0 || idx >= ls->n) {
        return -1;
    }
    path = ls->files[idx];
    if (access(path, R_OK) != 0) {
        ls->consec_fail++;
        log_error("source_local: skipping unreadable '%s' (%s)", path, strerror(errno));
        if (ls->consec_fail > ls->n) {
            /* 连续失败超过目录内图片数：报错退出（DESIGN.md §6.2） */
            log_error("source_local: %d consecutive failures, giving up", ls->consec_fail);
            return -1;
        }
        return 1;
    }
    wr = snprintf(out_path, len, "%s", path);
    if (wr < 0 || (size_t) wr >= len) {
        log_error("source_local: output buffer too small for '%s'", path);
        return -1;
    }
    ls->consec_fail = 0;
    return 0;
}

static void local_reset(PhotoSource* s)
{
    LocalSource* ls = (LocalSource*) s;

    if (ls == NULL) {
        return;
    }
    if (ls->dir == NULL || local_scan(ls, ls->dir) != 0) {
        log_info("source_local: reset: rescan failed, keeping current list");
    }
    ls->cursor = 0;
    ls->order_pos = 0;
    ls->consec_fail = 0;
    if (ls->order_mode == ORDER_RANDOM) {
        local_reshuffle(ls);
    }
}

static void local_cleanup(PhotoSource* s)
{
    LocalSource* ls = (LocalSource*) s;

    if (ls == NULL) {
        return;
    }
    free_file_list(ls->files, ls->n);
    free(ls->order);
    free(ls->dir);
    free(ls);
}

PhotoSource* source_local_open(const char* dir, long max_bytes)
{
    LocalSource* ls;

    if (dir == NULL || dir[0] == '\0') {
        return NULL;
    }
    ls = (LocalSource*) calloc(1, sizeof(*ls));
    if (ls == NULL) {
        return NULL;
    }
    ls->pub.next = local_next;
    ls->pub.reset = local_reset;
    ls->pub.cleanup = local_cleanup;
    ls->pub.name = "local";
    ls->order_mode = ORDER_SEQ;
    ls->max_bytes = (max_bytes > 0) ? max_bytes : 0;
    ls->seed = (unsigned int) time(NULL) ^ (unsigned int) getpid();
    if (ls->seed == 0u) {
        ls->seed = 1u;
    }
    if (local_scan(ls, dir) != 0) {
        free(ls);
        return NULL;
    }
    ls->dir = strdup(dir);
    if (ls->dir == NULL) {
        free_file_list(ls->files, ls->n);
        free(ls->order);
        free(ls);
        return NULL;
    }
    log_info("source_local: %d image(s) in '%s'", ls->n, dir);
    return &ls->pub;
}

void source_local_set_order(PhotoSource* s, PlayOrder order)
{
    LocalSource* ls = (LocalSource*) s;

    if (ls == NULL) {
        return;
    }
    if (order != ORDER_SEQ && order != ORDER_RANDOM) {
        return;
    }
    ls->order_mode = order;
    ls->cursor = 0;
    ls->order_pos = 0;
    ls->consec_fail = 0;
    if (order == ORDER_RANDOM) {
        local_reshuffle(ls);
    }
}
