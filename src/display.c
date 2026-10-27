/* display.c — FBInk 封装实现：运行时几何自适应、fit/状态栏/波形选择、单次全区域刷新 */
#define _POSIX_C_SOURCE 200809L
#include "display.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <fbink.h>

#include "log.h"

struct Display {
    int fbfd;
    FBInkState state;   /* 上次已知的 fb 状态（尺寸/旋转） */
    unsigned int shown; /* 已显示张数，用于 flash_every 全刷节拍 */
};

Display* display_open(const AppConfig* cfg)
{
    Display* d;
    FBInkConfig fc;
    int fbfd;

    fbfd = fbink_open();
    if (fbfd < 0) {
        log_error("display: fbink_open failed (%d)", fbfd);
        return NULL;
    }
    memset(&fc, 0, sizeof(fc));
    fc.is_quiet = true; /* 硬件信息走我们自己的日志 */
    fc.is_verbose = (cfg != NULL && cfg->debug) ? true : false;
    if (fbink_init(fbfd, &fc) != 0) {
        log_error("display: fbink_init failed");
        fbink_close(fbfd);
        return NULL;
    }
    d = (Display*) calloc(1, sizeof(*d));
    if (d == NULL) {
        fbink_close(fbfd);
        return NULL;
    }
    d->fbfd = fbfd;
    fbink_get_state(&fc, &d->state);
    log_info("display: %ux%u (view %ux%u) bpp=%u rota=%u",
             d->state.screen_width,
             d->state.screen_height,
             d->state.view_width,
             d->state.view_height,
             d->state.bpp,
             (unsigned int) d->state.current_rota);
    return d;
}

int display_show(Display* d, const char* path, const AppConfig* cfg)
{
    FBInkConfig fc;
    FBInkConfig cc;
    FBInkConfig rc;
    FBInkState cur;
    FBInkRect rect;
    short int y_off = 0;
    int vw;
    int vh;
    int top = 0;
    int region_h;
    int rv;

    if (d == NULL || d->fbfd < 0 || path == NULL || cfg == NULL) {
        return -1;
    }

    /* §8 节流：下次刷新前等上一次 EPDC 更新完成（FBInk >= 1.20 惯例） */
    (void) fbink_wait_for_complete(d->fbfd, fbink_get_last_marker());

    /* §8 旋转/尺寸自适应：几何变了就 reinit 再显示 */
    memset(&fc, 0, sizeof(fc));
    fc.is_quiet = true;
    fbink_get_state(&fc, &cur);
    if (cur.screen_width != d->state.screen_width || cur.screen_height != d->state.screen_height ||
        cur.current_rota != d->state.current_rota) {
        int rr = fbink_reinit(d->fbfd, &fc); /* 返回 OK_*_CHANGE 位掩码；必须使用返回值 */
        log_debug("display: fb geometry changed (reinit rv=%d)", rr);
        fbink_get_state(&fc, &d->state);
    }

    /* 运行时几何，绝不硬编码分辨率 */
    vw = (int) d->state.screen_width;
    vh = (int) d->state.screen_height;
    if (vw <= 0 || vh <= 0) {
        log_error("display: bad geometry %dx%d", vw, vh);
        return -1;
    }
    if (cfg->statusbar == SB_KEEP) {
        /* 状态栏高度：FBInkState 无此字段，按 screen_height 的 5.5% 估（600x800 → 44px） */
        top = (vh * 55 + 500) / 1000;
        if (top < 1) {
            top = 1;
        }
    }
    region_h = vh - top;
    if (region_h < 1) {
        top = 0;
        region_h = vh;
    }

    /* --- 组装 FBInkConfig（缩放语义见 fbink.h scaled_width/height 注释）--- */
    memset(&fc, 0, sizeof(fc));
    fc.is_quiet = true;
    fc.ignore_alpha = true; /* 背景已由我们清白，图片 alpha 直接压平 */
    fc.halign = CENTER;
    fc.valign = CENTER;
    switch (cfg->fit) {
        case FIT_STRETCH:
            if (cfg->statusbar == SB_KEEP) {
                /* 正数 = 指定输出尺寸：精确铺满状态栏以下区域 */
                fc.scaled_width = (short int) vw;
                fc.scaled_height = (short int) region_h;
                fc.valign = NONE;
                y_off = (short int) top;
            } else {
                /* -1 = 视口尺寸：拉伸铺满全屏 */
                fc.scaled_width = -1;
                fc.scaled_height = -1;
            }
            break;
        case FIT_COVER:
            /* M1 近似：contain 等比适配 + cls 白底（真 cover 需按比例裁剪，M2 改进） */
        case FIT_CONTAIN:
        default:
            /* < -1 = 等比适配视口（AR 保持）+ 居中 */
            fc.scaled_width = -2;
            fc.scaled_height = -2;
            if (cfg->statusbar == SB_KEEP) {
                /* FBInk 只能对全视口做 AR fit；这里把结果垂直居中到状态栏下方区域
                 * （y_off=top/2 + valign CENTER = 在 [top, vh] 内居中）。
                 * M1 已知局限：恰好顶到视口高度的竖图会轻微压到状态栏，M2 用图像解码取尺寸后精确子视口 fit。 */
                y_off = (short int)(top / 2);
            }
            break;
    }

    /* 白底清屏（含 cover 近似与 contain 留白），no_refresh：只写 fb 内存 */
    memset(&cc, 0, sizeof(cc));
    cc.is_quiet = true;
    cc.bg_color = BG_WHITE;
    cc.no_refresh = true;
    rect.left = 0;
    rect.top = (unsigned short int) top;
    rect.width = (unsigned short int) vw;
    rect.height = (unsigned short int) region_h;
    if (fbink_cls(d->fbfd, &cc, &rect, false) != 0) {
        log_error("display: cls failed");
        return -1;
    }

    /* 画图（no_refresh：与清屏合并为一次刷新，失败不刷新、保留旧画面） */
    fc.no_refresh = true;
    rv = fbink_print_image(d->fbfd, path, 0, y_off, &fc);
    if (rv != 0) {
        log_error("display: cannot show '%s' (%d)", path, rv);
        return -1;
    }

    /* 单次全区域刷新：常规 GC16_FAST，每 flash_every 张 GC16+is_flashing 全刷清残影 */
    d->shown++;
    rc = fc;
    rc.no_refresh = false;
    if (cfg->flash_every > 0 && (d->shown % (unsigned int) cfg->flash_every) == 0u) {
        rc.wfm_mode = WFM_GC16;
        rc.is_flashing = true;
    } else {
        rc.wfm_mode = WFM_GC16_FAST;
        rc.is_flashing = false;
    }
    if (fbink_refresh(d->fbfd, (uint32_t) top, 0u, (uint32_t) vw, (uint32_t) region_h, &rc) != 0) {
        log_error("display: refresh failed");
        return -1;
    }
    log_debug("display: showed '%s' #%u (flash=%d)", path, d->shown, rc.is_flashing ? 1 : 0);
    return 0;
}

void display_close(Display* d, int show_hint)
{
    FBInkConfig fc;

    if (d == NULL) {
        return;
    }
    if (d->fbfd >= 0) {
        /* §9.1：清屏（白底 + 全刷）再关 fb */
        memset(&fc, 0, sizeof(fc));
        fc.is_quiet = true;
        fc.bg_color = BG_WHITE;
        fc.wfm_mode = WFM_GC16;
        fc.is_flashing = true;
        if (!show_hint) {
            /* 默认路径（普通模式）：纯白全刷，行为与从前完全一致 */
            (void) fbink_cls(d->fbfd, &fc, NULL, false);
        } else {
            /* 防干扰模式退出提示：framework 冷启动 30-60s 才首绘，纯白会被用户当死机。
             * 白底上居中一行 ASCII 提示（FBInk 内置字体不保 CJK）；cls+文本合并一次全刷（WFM_GC16）。
             * 全程 best-effort：任何一步失败仅忽略。fatal 点随后的 fatal_screen 会 cls 洗掉提示再画 ERROR。 */
            fc.no_refresh = true;
            fc.is_flashing = false;
            (void) fbink_cls(d->fbfd, &fc, NULL, false);
            fc.is_centered = true;
            fc.is_halfway = true;
            fc.fg_color = FG_BLACK;
            (void) fbink_print(d->fbfd, "Returning to Kindle UI, please wait...", &fc);
            fc.no_refresh = false;
            fc.is_flashing = true;
            (void) fbink_refresh(d->fbfd, 0u, 0u, 0u, 0u, &fc); /* 空区域 = 全屏一次全刷 */
            (void) fbink_wait_for_complete(d->fbfd, fbink_get_last_marker());
        }
        (void) fbink_close(d->fbfd);
        d->fbfd = -1;
    }
    free(d);
}

/* best-effort 屏幕报错：给无 SSH、只能 USB 拷文件的用户看的最后手段。
 * 调用点多在 display_open 之前/失败时，因此自带完整 fbink 流程。
 * 所有 FBInk 调用忽略返回值：任何一步失败都不崩溃、不阻塞主流程；详细原因以日志为准。 */
void fatal_screen(const char* short_msg)
{
    FBInkConfig fc;
    char line[128];
    const char* msg = short_msg;
    int fbfd;

    /* fbink_print 空串返回 -(EINVAL)，且只接受合法 UTF-8 → 兜底非空短 ASCII */
    if (msg == NULL || msg[0] == '\0') {
        msg = "ERROR";
    }
    if (snprintf(line, sizeof(line), "%s", msg) < 0) {
        return;
    }

    fbfd = fbink_open();

    memset(&fc, 0, sizeof(fc));
    fc.is_quiet = true;
    fc.is_centered = true;  /* fbink_print 文本水平居中（文本路径用 is_centered） */
    fc.is_halfway = true;   /* 文本垂直居中 */
    fc.halign = CENTER;     /* ALIGN CENTER（图像/截屏对齐字段，文本路径不读，按需求同时置位） */
    fc.valign = CENTER;
    fc.fg_color = FG_BLACK;
    fc.bg_color = BG_WHITE;
    fc.wfm_mode = WFM_GC16; /* 照片级高保真波形 */
    fc.no_refresh = true;   /* cls/print 只写 fb，统一由末尾一次全刷上屏 */

    (void) fbink_init(fbfd, &fc);
    (void) fbink_cls(fbfd, &fc, NULL, false); /* 白底清全屏 */
    (void) fbink_print(fbfd, line, &fc);

    fc.no_refresh = false;
    fc.is_flashing = true;                    /* GC16 全刷：错误必须显眼并清残影 */
    (void) fbink_refresh(fbfd, 0u, 0u, 0u, 0u, &fc); /* 空区域 (0x0@0,0) = 全屏刷新 */
    (void) fbink_wait_for_complete(fbfd, fbink_get_last_marker());
    (void) fbink_close(fbfd);
}
