#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
photo_server.py — photo_plugins 网络相册服务端参考实现（DESIGN.md §6.3 协议）

纯 Python 标准库（Python 3.8+，无第三方依赖），在 PC 上运行，供设备端
source_http.c 按协议取图。服务端无状态，选图/调度完全在客户端。

协议：
  GET <任意路径>?count=1           -> 200 text/plain，正文=十进制整数 N（图片总数）
  GET <任意路径>?index=N           -> 200 image/*，正文=第 N 张（0 基）原始字节；
                                     N<0 或 N>=总数 -> 404；index 非整数 -> 400
  GET <任意路径>?order=seq|random  -> 允许携带，服务端仅记日志后忽略（调度在客户端）
  GET /（无参数）                  -> 200 纯文本简短帮助

图片响应头带 X-Photo-Count / X-Photo-Index（客户端只作日志用）。
URL 路径被忽略，只认 query 参数。图片来自 --dir 指定目录：
不递归子目录，按文件名字节序排序确定索引，每次请求实时扫描（照片增删即时生效）；
扩展名白名单：.jpg/.jpeg/.png/.bmp/.gif/.tga/.pnm，其余过滤不入库。

用法：
  python3 tools/photo_server.py [--dir DIR] [--port 8080] [--host 0.0.0.0]
"""

import argparse
import os
import sys
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlparse

__version__ = "1.0"

# 扩展名 -> Content-Type（与 FBInk 支持集对齐，其余扩展名不入库）
CTYPE_BY_EXT = {
    ".jpg": "image/jpeg",
    ".jpeg": "image/jpeg",
    ".png": "image/png",
    ".bmp": "image/bmp",
    ".gif": "image/gif",
    ".tga": "image/x-tga",
    ".pnm": "image/x-portable-anymap",
}

HELP_TEXT = (
    "photo_server %s — photo_plugins network photo source (DESIGN.md 6.3)\n"
    "\n"
    "usage:\n"
    "  GET /?count=1            -> text/plain, photo count N\n"
    "  GET /?index=N            -> image bytes of photo N (0-based); out of range -> 404\n"
    "  GET /?order=seq|random   -> accepted and ignored (scheduling is client-side)\n"
    "  GET /                    -> this help\n"
    "\n"
    "The URL path is ignored; only the query string matters.\n"
    "Photos come from the configured directory, sorted by file name bytes,\n"
    "rescanned on every request; only image extensions are served.\n"
) % __version__


def list_photos(photo_dir):
    """扫描目录（不递归子目录），返回按文件名字节序排序的图片文件名列表。"""
    try:
        names = os.listdir(photo_dir)
    except OSError:
        return []
    photos = []
    for name in names:
        ext = os.path.splitext(name)[1].lower()
        if ext in CTYPE_BY_EXT and os.path.isfile(os.path.join(photo_dir, name)):
            photos.append(name)
    photos.sort(key=os.fsencode)
    return photos


class PhotoRequestHandler(BaseHTTPRequestHandler):
    """只处理 GET；每请求输出一行日志到 stdout，静默基类 stderr 访问日志。"""

    server_version = "photo_server/" + __version__

    # ---- 响应工具 -------------------------------------------------
    def _send_bytes(self, status, ctype, body, extra_headers=None):
        self.send_response(status)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Connection", "close")
        if extra_headers:
            for key, value in extra_headers:
                self.send_header(key, value)
        self.end_headers()
        self.wfile.write(body)

    def _log(self, status, note):
        line = '%s "GET %s" %s %s\n' % (self.address_string(), self.path, status, note)
        sys.stdout.write(line)
        sys.stdout.flush()

    def log_message(self, format, *args):  # noqa: A002 - 基类接口签名
        """静默基类的 stderr 日志；统一由 _log 每请求输出一行到 stdout。"""
        pass

    def _photo_dir(self):
        server = self.server
        if isinstance(server, PhotoServer):
            return server.photo_dir
        return ""

    # ---- 协议处理 -------------------------------------------------
    def _handle_count(self):
        photos = list_photos(self._photo_dir())
        body = str(len(photos)).encode("ascii")
        self._send_bytes(200, "text/plain; charset=utf-8", body)
        return "200", "count=%d" % len(photos)

    def _handle_index(self, raw):
        photo_dir = self._photo_dir()
        photos = list_photos(photo_dir)
        try:
            idx = int(raw)
        except (TypeError, ValueError):
            self.send_error(400, "index must be an integer")
            return "400", "index=%r" % raw
        if idx < 0 or idx >= len(photos):
            self.send_error(404, "index out of range")
            return "404", "index=%d photos=%d" % (idx, len(photos))
        name = photos[idx]
        path = os.path.join(photo_dir, name)
        ctype = CTYPE_BY_EXT[os.path.splitext(name)[1].lower()]
        try:
            with open(path, "rb") as fh:
                body = fh.read()
        except OSError:
            self.send_error(404, "cannot read photo")
            return "404", "unreadable=%s" % name
        self._send_bytes(
            200, ctype, body,
            extra_headers=[("X-Photo-Count", str(len(photos))),
                           ("X-Photo-Index", str(idx))],
        )
        return "200", "index=%d photos=%d file=%s" % (idx, len(photos), name)

    def _handle_help(self):
        body = HELP_TEXT.encode("utf-8")
        self._send_bytes(200, "text/plain; charset=utf-8", body)
        return "200", "help"

    def do_GET(self):
        parsed = urlparse(self.path)
        try:
            query = parse_qs(parsed.query, keep_blank_values=True)
        except ValueError:
            self.send_error(400, "bad query string")
            self._log("400", "bad query")
            return

        note = ""
        if "order" in query:
            note = " order=%s(ignored)" % query["order"][0]

        if "count" in query:
            status, info = self._handle_count()
        elif "index" in query:
            status, info = self._handle_index(query["index"][0])
        else:
            status, info = self._handle_help()
        self._log(status, (info + note).strip())


class PhotoServer(ThreadingHTTPServer):
    """ThreadingHTTPServer + 图片目录绑定。"""

    photo_dir = ""

    def __init__(self, server_addr, handler_cls, photo_dir):
        super().__init__(server_addr, handler_cls)
        self.photo_dir = photo_dir


def build_server(photo_dir, host, port):
    """创建（未启动的）PhotoServer；port=0 时由系统分配临时端口。"""
    return PhotoServer((host, port), PhotoRequestHandler, photo_dir)


def main(argv=None):
    parser = argparse.ArgumentParser(
        prog="photo_server.py",
        description="photo_plugins 网络相册服务端（DESIGN.md §6.3，纯标准库）",
    )
    parser.add_argument("--dir", default="./photos", metavar="DIR",
                        help="图片目录（默认 ./photos）")
    parser.add_argument("--port", type=int, default=8080, metavar="PORT",
                        help="监听端口（默认 8080）")
    parser.add_argument("--host", default="0.0.0.0", metavar="HOST",
                        help="监听地址（默认 0.0.0.0）")
    args = parser.parse_args(argv)

    photo_dir = os.path.abspath(args.dir)
    if not os.path.isdir(photo_dir):
        sys.stderr.write("photo_server: error: image directory not found: %s\n" % photo_dir)
        return 1

    server = build_server(photo_dir, args.host, args.port)
    host, port = server.server_address[0], server.server_address[1]
    count = len(list_photos(photo_dir))
    print("photo_server: listening on http://%s:%d/ dir=%s photos=%d"
          % (host, port, photo_dir, count))
    sys.stdout.flush()

    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("photo_server: interrupted, shutting down")
    finally:
        server.server_close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
