#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
resize_photos.py — Kindle photo-player 照片入库批量预缩放工具（PC/Windows, Python 3.8+ / Pillow）

用途：照片入库前批量预缩放到 Kindle 屏幕尺寸（默认 600x800），供 photo-player 直接播放。
设备直接解码 20MP 原图太慢/易 OOM，预缩放是设计约定。

处理顺序：open -> exif_transpose(方向矫正) -> resize(LANCZOS, 只缩不放)
          -> [--gray convert('L')] -> [--levels quantize(Floyd-Steinberg 抖动)] -> save(EXIF 剥离)

CLI:
    python resize_photos.py INPUT_DIR [OUTPUT_DIR] [--box 600x800] [--gray] [--levels N]
                            [--format jpg|png] [--quality 90] [--recursive] [--dry-run]
"""

import argparse
import os
import sys
from pathlib import Path

from PIL import Image, ImageOps, UnidentifiedImageError

# Pillow 9/10/11 常量改枚举的兼容写法
RESAMPLE = getattr(Image, "Resampling", Image).LANCZOS  # type: ignore[attr-defined]
DITHER = getattr(Image, "Dither", Image).FLOYDSTEINBERG  # type: ignore[attr-defined]

DEFAULT_BOX = (600, 800)
MIN_QUALITY, MAX_QUALITY = 60, 95
MIN_LEVELS, MAX_LEVELS = 2, 256
EXT_BY_FORMAT = {"jpg": ".jpg", "png": ".png"}


# ---------------------------------------------------------------- helpers

def norm_path(p):
    """Windows 大小写不敏感的绝对路径比较键。"""
    return os.path.normcase(os.path.abspath(str(p)))


def is_under(child, parent):
    """child 是否位于 parent 之下（含相等）。"""
    c, p = norm_path(child), norm_path(parent)
    return c == p or c.startswith(p + os.sep)


def parse_box(text):
    try:
        w_str, h_str = text.lower().split("x", 1)
        w, h = int(w_str), int(h_str)
    except ValueError:
        raise argparse.ArgumentTypeError("box 格式应为 WxH，例如 600x800")
    if w <= 0 or h <= 0:
        raise argparse.ArgumentTypeError("box 宽高必须为正整数")
    return (w, h)


def parse_quality(text):
    try:
        q = int(text)
    except ValueError:
        raise argparse.ArgumentTypeError("quality 必须是整数")
    if not MIN_QUALITY <= q <= MAX_QUALITY:
        raise argparse.ArgumentTypeError(
            "quality 取值范围 %d-%d" % (MIN_QUALITY, MAX_QUALITY))
    return q


def parse_levels(text):
    try:
        n = int(text)
    except ValueError:
        raise argparse.ArgumentTypeError("levels 必须是整数")
    if not MIN_LEVELS <= n <= MAX_LEVELS:
        raise argparse.ArgumentTypeError(
            "levels 取值范围 %d-%d" % (MIN_LEVELS, MAX_LEVELS))
    return n


def contain_size(w, h, box_w, box_h):
    """等比 contain：缩到完全装入 box，保持宽高比。只缩不放。"""
    if w <= box_w and h <= box_h:
        return w, h, False  # 不放大
    scale = min(box_w / w, box_h / h)
    nw = max(1, int(round(w * scale)))
    nh = max(1, int(round(h * scale)))
    # 浮点舍入防御：确保绝不越界
    if nw > box_w:
        nw = box_w
    if nh > box_h:
        nh = box_h
    return nw, nh, True


def quantize_gray(im, levels):
    """量化到 N 级灰阶 + Floyd-Steinberg 误差扩散抖动（消除渐变色带）。"""
    try:
        return im.quantize(colors=levels, dither=DITHER)
    except Exception:
        # 个别 Pillow 版本/模式限制：转 RGB 再量化（同灰度通道，结果仍为灰阶调色板）
        return im.convert("RGB").quantize(colors=levels, dither=DITHER)


def save_image(im, out_path, fmt, quality):
    """保存输出。EXIF 不回写（strip）。jpg+levels 时 P->L（JPEG 不支持 P 模式）。"""
    if fmt == "jpg":
        if im.mode == "P":
            im = im.convert("L")
        elif im.mode not in ("L", "RGB"):
            im = im.convert("RGB")  # RGBA/LA 等：JPEG 无 alpha
        im.save(out_path, format="JPEG", quality=quality, optimize=True)
    else:
        im.save(out_path, format="PNG", optimize=True)


# ---------------------------------------------------------------- scanning

def collect_files(input_dir, output_dir, recursive):
    """收集待处理文件；排除输出目录子树（防自吞）。返回 [(abs_path, rel_path), ...]。"""
    items = []
    if recursive:
        for root, dirs, files in os.walk(input_dir):
            dirs[:] = sorted(
                d for d in dirs
                if not is_under(os.path.join(root, d), output_dir))
            for name in sorted(files):
                p = os.path.join(root, name)
                if is_under(p, output_dir):
                    continue
                items.append((p, os.path.relpath(p, input_dir)))
    else:
        for name in sorted(os.listdir(input_dir)):
            p = os.path.join(input_dir, name)
            if os.path.isfile(p) and not is_under(p, output_dir):
                items.append((p, name))
    return items


# ---------------------------------------------------------------- per-file

def process_one(src_path, rel_path, out_dir, box, fmt, quality,
                gray, levels, dry_run, seen_outputs):
    """
    处理单个文件。返回 'processed' | 'unchanged' | 'skipped' | 'failed'。
    打印一行报告：`原WxH → 新WxH  bytes前/后  文件名`
    """
    rel_out = str(Path(rel_path).with_suffix(EXT_BY_FORMAT[fmt]))
    out_path = os.path.join(out_dir, rel_out)

    # 输出名冲突安全处理（如 a.jpg 与 a.png 同名映射；或不同子目录同名）
    key = norm_path(out_path)
    if key in seen_outputs:
        print("skipped  name conflict: %s -> %s" % (rel_path, rel_out))
        return "skipped"

    # --- 读入 + 处理（非图片/损坏：skipped，不中断批处理）---
    try:
        bytes_in = os.path.getsize(src_path)
        with Image.open(src_path) as im:
            im.load()
            # 1) EXIF 方向必须先矫正（漏了纪念照会横躺）
            im = ImageOps.exif_transpose(im)
            ow, oh = im.size

            # 2) 等比 contain，只缩不放
            nw, nh, resized = contain_size(ow, oh, box[0], box[1])
            if resized:
                im = im.resize((nw, nh), RESAMPLE)
            tag = "" if resized else "  [unchanged/skipped-upscale]"

            # 3) 灰度（luma, ITU-R 601，与 PIL convert('L') 一致）
            if gray:
                im = im.convert("L")

            # 4) 灰阶量化 + Floyd-Steinberg 抖动（仅 --gray）
            if levels is not None:
                im = quantize_gray(im, levels)
    except (UnidentifiedImageError, OSError, ValueError) as e:
        print("skipped  %s (%s: %s)" % (rel_path, type(e).__name__, e))
        return "skipped"
    except Exception as e:
        print("failed   %s (%s: %s)" % (rel_path, type(e).__name__, e))
        return "failed"

    seen_outputs.add(key)
    kind = "processed" if resized else "unchanged"

    if dry_run:
        print("%dx%d -> %dx%d%s  %d->(dry-run)  %s"
              % (ow, oh, nw, nh, tag, bytes_in, rel_out))
        return kind

    # --- 保存（EXIF 剥离；jpg+levels 时 P->L）写盘失败计 failed ---
    try:
        os.makedirs(os.path.dirname(out_path) or ".", exist_ok=True)
        save_image(im, out_path, fmt, quality)
        bytes_out = os.path.getsize(out_path)
    except Exception as e:
        print("failed   %s (%s: %s)" % (rel_path, type(e).__name__, e))
        return "failed"

    print("%dx%d -> %dx%d%s  %d->%d  %s"
          % (ow, oh, nw, nh, tag, bytes_in, bytes_out, rel_out))
    return kind


# ---------------------------------------------------------------- main

def build_parser():
    ap = argparse.ArgumentParser(
        prog="resize_photos.py",
        description="批量预缩放照片到 Kindle 屏幕尺寸（photo-player 入库前处理）",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="示例:\n"
               "  python resize_photos.py D:\\photos\n"
               "  python resize_photos.py D:\\photos D:\\out --box 600x800 --gray --levels 16 --format png\n"
               "  python resize_photos.py D:\\photos --recursive --dry-run\n")
    ap.add_argument("input_dir", metavar="INPUT_DIR", help="原图目录")
    ap.add_argument("output_dir", metavar="OUTPUT_DIR", nargs="?", default=None,
                    help="输出目录（缺省 INPUT_DIR_resized，绝不覆盖原图）")
    ap.add_argument("--box", type=parse_box, default=DEFAULT_BOX, metavar="WxH",
                    help="目标框 WxH，等比 contain，只缩不放（默认 600x800）")
    ap.add_argument("--gray", action="store_true",
                    help="转灰度（luma, ITU-R 601）")
    ap.add_argument("--levels", type=parse_levels, default=None, metavar="N",
                    help="量化到 N 级灰阶 + Floyd-Steinberg 抖动（仅配合 --gray）")
    ap.add_argument("--format", choices=("jpg", "png"), default="jpg",
                    help="输出格式（默认 jpg；png 无损，gray+png 是 FBInk 最快路径）")
    ap.add_argument("--quality", type=parse_quality, default=90,
                    help="JPEG 质量 %d-%d（默认 90）" % (MIN_QUALITY, MAX_QUALITY))
    ap.add_argument("--recursive", action="store_true",
                    help="递归子目录并保持相对结构")
    ap.add_argument("--dry-run", action="store_true",
                    help="只列处理计划，不写文件")
    return ap


def main(argv=None):
    try:
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")  # type: ignore[attr-defined]
        sys.stderr.reconfigure(encoding="utf-8", errors="replace")  # type: ignore[attr-defined]
    except Exception:
        pass  # 非交互/旧环境：忽略

    args = build_parser().parse_args(argv)

    if args.levels is not None and not args.gray:
        print("error: --levels 仅可与 --gray 同用", file=sys.stderr)
        return 2

    input_dir = Path(args.input_dir)
    if not input_dir.is_dir():
        print("error: INPUT_DIR 不存在或不是目录: %s" % input_dir, file=sys.stderr)
        return 2

    output_dir = Path(args.output_dir) if args.output_dir else \
        input_dir.parent / (input_dir.name + "_resized")

    # 绝不覆盖原图：输出目录与输入目录相同则报错退出
    if norm_path(output_dir) == norm_path(input_dir):
        print("error: OUTPUT_DIR 与 INPUT_DIR 相同，拒绝覆盖原图", file=sys.stderr)
        return 2

    if not args.dry_run:
        output_dir.mkdir(parents=True, exist_ok=True)

    files = collect_files(str(input_dir), str(output_dir), args.recursive)

    mode = []
    if args.gray:
        mode.append("gray")
    if args.levels is not None:
        mode.append("levels=%d(Floyd-Steinberg)" % args.levels)
    print("resize_photos: %d file(s) | box %dx%d | %s | -> %s%s"
          % (len(files), args.box[0], args.box[1],
             ", ".join(mode) if mode else "rgb",
             output_dir, " | DRY-RUN" if args.dry_run else ""))
    if args.format == "jpg" and args.levels is not None:
        print("note: --format jpg + --levels -> 量化(P) 后转回 'L' 再存（JPEG 不支持 P 模式）")

    counts = {"processed": 0, "unchanged": 0, "skipped": 0, "failed": 0}
    bytes_in_total = 0
    bytes_out_total = 0
    seen_outputs = set()

    for src_path, rel_path in files:
        try:
            b0 = os.path.getsize(src_path)
        except OSError:
            b0 = 0
        result = process_one(src_path, rel_path, str(output_dir), args.box,
                             args.format, args.quality, args.gray, args.levels,
                             args.dry_run, seen_outputs)
        counts[result] += 1
        if result in ("processed", "unchanged"):
            bytes_in_total += b0
            if not args.dry_run:
                out_path = os.path.join(str(output_dir),
                                        str(Path(rel_path).with_suffix(EXT_BY_FORMAT[args.format])))
                try:
                    bytes_out_total += os.path.getsize(out_path)
                except OSError:
                    pass

    saved = bytes_in_total - bytes_out_total
    pct = (saved * 100.0 / bytes_in_total) if bytes_in_total else 0.0
    print("-" * 60)
    print("summary: processed=%d  unchanged=%d  skipped=%d  failed=%d"
          % (counts["processed"], counts["unchanged"],
             counts["skipped"], counts["failed"]))
    if args.dry_run:
        print("total bytes: in=%d  out=(dry-run)  (未写盘)" % bytes_in_total)
    else:
        print("total bytes: %d -> %d  (saved: %d bytes, %.1f%%)"
              % (bytes_in_total, bytes_out_total, saved, pct))
    return 1 if counts["failed"] else 0


if __name__ == "__main__":
    sys.exit(main())
