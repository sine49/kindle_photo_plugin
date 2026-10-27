#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
test_photo_server.py — tools/photo_server.py 协议单元测试（DESIGN.md §6.3）

unittest + 标准库 urllib，服务起在临时端口（port=0）+ 守护线程，
夹具为临时目录里的任意字节文件（服务端不校验图片内容，只看扩展名）。

运行：
    python3 test/server/test_photo_server.py
    （或 python3 -m unittest discover -s test/server ）
"""

import os
import shutil
import sys
import tempfile
import threading
import unittest
import urllib.error
import urllib.request

sys.path.insert(
    0, os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..", "tools"))
)
import photo_server  # noqa: E402  # pyright: ignore[reportMissingImports]


class PhotoServerTest(unittest.TestCase):
    def setUp(self):
        self.tmpdir = tempfile.mkdtemp(prefix="photo_server_test_")
        self.httpd = photo_server.build_server(self.tmpdir, "127.0.0.1", 0)
        self.port = self.httpd.server_address[1]
        self.thread = threading.Thread(target=self.httpd.serve_forever, daemon=True)
        self.thread.start()
        # 无代理 opener，避免环境变量 HTTP_PROXY 劫持 127.0.0.1
        self.opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))

    def tearDown(self):
        self.httpd.shutdown()
        self.httpd.server_close()
        self.thread.join(timeout=5)
        shutil.rmtree(self.tmpdir, ignore_errors=True)

    # ---- 工具 ----
    def add_photo(self, name, data=b"fake-image-bytes", subdir=None):
        base = self.tmpdir if subdir is None else os.path.join(self.tmpdir, subdir)
        os.makedirs(base, exist_ok=True)
        with open(os.path.join(base, name), "wb") as fh:
            fh.write(data)
        return data

    def get(self, query):
        url = "http://127.0.0.1:%d/any/path?%s" % (self.port, query)
        with self.opener.open(url, timeout=10) as resp:
            return resp.status, resp.headers, resp.read()

    def get_status(self, query):
        url = "http://127.0.0.1:%d/any/path?%s" % (self.port, query)
        try:
            with self.opener.open(url, timeout=10) as resp:
                resp.read()
                return resp.status
        except urllib.error.HTTPError as err:
            err.read()
            return err.code

    def count(self):
        return self.get("count=1")[2]

    # ---- 用例 ----
    def test_01_empty_dir_count_zero(self):
        status, headers, body = self.get("count=1")
        self.assertEqual(status, 200)
        self.assertTrue(headers["Content-Type"].startswith("text/plain"))
        self.assertEqual(body, b"0")

    def test_02_count_reflects_photo_number(self):
        for i in range(5):
            self.add_photo("p%d.jpg" % i)
        status, _, body = self.get("count=1")
        self.assertEqual(status, 200)
        self.assertEqual(body, b"5")

    def test_03_index_bytes_ctype_and_headers(self):
        blobs = [
            ("p0.jpg", b"\xff\xd8blob0", "image/jpeg"),
            ("p1.png", b"\x89PNGblob1", "image/png"),
            ("p2.jpg", b"\xff\xd8blob2", "image/jpeg"),
        ]
        for name, data, _ in blobs:
            self.add_photo(name, data)
        # 文件名字节序：p0.jpg < p1.png < p2.jpg
        for idx, (name, data, ctype) in enumerate(blobs):
            status, headers, body = self.get("index=%d" % idx)
            self.assertEqual(status, 200, name)
            self.assertEqual(body, data, name)  # 逐字节一致
            self.assertEqual(headers["Content-Type"], ctype, name)
            self.assertEqual(headers["X-Photo-Count"], "3", name)
            self.assertEqual(headers["X-Photo-Index"], str(idx), name)

    def test_04_index_out_of_range_404(self):
        for i in range(3):
            self.add_photo("p%d.jpg" % i)
        self.assertEqual(self.get_status("index=3"), 404)    # =N
        self.assertEqual(self.get_status("index=4"), 404)    # =N+1
        self.assertEqual(self.get_status("index=-1"), 404)   # <0

    def test_05_index_not_integer_400(self):
        self.add_photo("p0.jpg")
        self.assertEqual(self.get_status("index=abc"), 400)
        self.assertEqual(self.get_status("index=1.5"), 400)
        self.assertEqual(self.get_status("index="), 400)

    def test_06_order_param_tolerated(self):
        data0 = self.add_photo("p0.jpg", b"\xff\xd8first")
        self.add_photo("p1.jpg", b"\xff\xd8second")
        for order in ("seq", "random"):
            status, headers, body = self.get("index=0&order=%s" % order)
            self.assertEqual(status, 200, order)
            self.assertEqual(body, data0, order)  # 不影响取图
            self.assertEqual(headers["X-Photo-Index"], "0", order)
        status, _, body = self.get("count=1&order=random")
        self.assertEqual(status, 200)
        self.assertEqual(body, b"2")

    def test_07_ctype_mapping_and_extension_filter(self):
        cases = [
            ("a.jpg", "image/jpeg"),
            ("b.jpeg", "image/jpeg"),
            ("c.png", "image/png"),
            ("d.bmp", "image/bmp"),
            ("e.gif", "image/gif"),
            ("f.tga", "image/x-tga"),
            ("g.pnm", "image/x-portable-anymap"),
        ]
        for name, _ in cases:
            self.add_photo(name, ("bytes-of-" + name).encode("ascii"))
        self.add_photo("z.txt", b"not-an-image")              # 非图片扩展名 -> 过滤
        self.add_photo("no_ext", b"no-extension")             # 无扩展名 -> 过滤
        self.add_photo("inside.png", b"sub", subdir="subdir")  # 子目录不递归

        self.assertEqual(self.count(), b"7")  # txt / 无扩展名 / 子目录均不入库
        for idx, (name, ctype) in enumerate(cases):
            status, headers, body = self.get("index=%d" % idx)
            self.assertEqual(status, 200, name)
            self.assertEqual(headers["Content-Type"], ctype, name)
            self.assertEqual(body, ("bytes-of-" + name).encode("ascii"), name)

    def test_08_chinese_filename(self):
        data = self.add_photo("照片.jpg", b"\xff\xd8\xe4\xb8\xad\xe6\x96\x87")
        self.assertEqual(self.count(), b"1")
        status, headers, body = self.get("index=0")
        self.assertEqual(status, 200)
        self.assertEqual(body, data)
        self.assertEqual(headers["Content-Type"], "image/jpeg")

    def test_09_count_reflects_add_and_remove(self):
        self.add_photo("p0.jpg", b"one")
        self.add_photo("p1.jpg", b"two")
        self.assertEqual(self.count(), b"2")
        self.add_photo("p2.jpg", b"three")
        self.assertEqual(self.count(), b"3")   # 加一张 -> count+1
        os.remove(os.path.join(self.tmpdir, "p1.jpg"))
        self.assertEqual(self.count(), b"2")   # 删一张 -> count-1
        # 增删后索引实时对应新列表：p0.jpg, p2.jpg
        status, headers, body = self.get("index=1")
        self.assertEqual(status, 200)
        self.assertEqual(body, b"three")
        self.assertEqual(headers["X-Photo-Count"], "2")

    def test_10_root_returns_help(self):
        url = "http://127.0.0.1:%d/" % self.port
        with self.opener.open(url, timeout=10) as resp:
            status, headers, body = resp.status, resp.headers, resp.read()
        self.assertEqual(status, 200)
        self.assertTrue(headers["Content-Type"].startswith("text/plain"))
        text = body.decode("utf-8")
        self.assertIn("count=1", text)
        self.assertIn("index=N", text)


if __name__ == "__main__":
    suite = unittest.defaultTestLoader.loadTestsFromModule(sys.modules[__name__])
    result = unittest.TextTestRunner(verbosity=2).run(suite)
    print("SUMMARY: tests=%d failures=%d errors=%d"
          % (result.testsRun, len(result.failures), len(result.errors)))
    sys.exit(0 if result.wasSuccessful() else 1)
