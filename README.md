# photo_plugins

Kindle 电子墨水照片轮播（电子相框）KUAL 扩展：C 编写，基于 FBInk，直接写 framebuffer。

> E-Ink photo frame / slideshow KUAL extension for Kindle — C + FBInk, direct framebuffer rendering.

## 功能特性

- KUAL 菜单一键启动，三种相册源：本地顺序、本地随机、网络（HTTP）
- 切换间隔 / 播放顺序 / 缩放策略均可配置（CLI 与配置文件双通道）
- 防干扰模式（`--no-framework`）：`stop framework` + 禁用 pillow 覆盖层 + `preventScreenSaver`，根治状态栏压图
- 轻触屏幕 / 电源键退出（默认开启，可分别关闭）
- KUAL 残影规避：启动延迟后再上屏（`--start-delay`），避免菜单按钮残影盖住照片
- 超大图防护：单张大小上限（`--max-file-mb`），拦截超大/渐进式 JPEG 防解码 OOM
- 连续解码/显示失败自动报错退出，不死循环空转
- SIGKILL 强杀无法自救时，由停止脚本读标记文件代偿清理（恢复 framework / pillow）
- PC 端批量预缩放工具 `tools/resize_photos.py`（等比缩小 / 灰度 / 预抖动）

## 已验证环境

| 项目 | 值 |
|------|-----|
| 机型 | Kindle 7（2014 Basic / WP63GW / i.MX6SL，600×800 8bpp） |
| 固件 | 5.12.2.2，已越狱 + KUAL |
| 交叉编译 | armv7 softfp，koxtoolchain `arm-kindlepw2-linux-gnueabi` |

其它机型未验证；理论上同为 i.MX6SL 世代的机型可尝试（分辨率运行时自适应，非硬编码）。注意不可用 kindlehf 工具链（hard-float 需固件 ≥ 5.16.3）。

## 目录结构

```
photo_plugins/
├── src/            # C 源码（main / config / player / source_local / display / platform / touch / log / util）
├── test/host/      # 宿主机单元测试（make host-test）
├── bin/            # 构建产物 photo-player + 启停脚本 photo-player.sh
├── config/         # photo-player.conf 默认配置
├── tools/          # resize_photos.py PC 端批量预缩放
├── Makefile        # 构建 / 部署 / 打包
└── DESIGN.md       # 设计全文（架构 / 参数 / 协议 / 已知限制）
```

## 构建

依赖：

- WSL 或 Linux（所有 make 目标在 Linux 环境内执行）
- [koxtoolchain](https://github.com/koreader/koxtoolchain)：默认 `arm-kindlepw2-linux-gnueabi`
- FBInk v1.25.0 源码，默认路径 `../../.slim/clonedeps/repos/NiLuJe__FBInk`（可用 `FBINK_DIR` 覆盖），`make fbink` 会产出 `libfbink.a` 静态库

make 目标：

| 目标 | 作用 |
|------|------|
| `make all` | 交叉编译产出 `bin/photo-player`（strip；缺 libfbink.a 时自动先建 FBInk） |
| `make fbink` | 构建 FBInk 静态库（`make -C $FBINK_DIR static CROSS_TC=... KINDLE=true`） |
| `make host-test` | 宿主机 gcc 编译并运行单元测试 |
| `make install KINDLE_IP=<ip>` | scp 安装到设备 `/mnt/us/extensions/photo_plugins/` |
| `make package` | 产出 KUAL 可拷贝包 `photo_plugins-release/photo_plugins/` |
| `make check-abi` | glibc 符号版本审计（默认上限 `MAX_GLIBC=2.17`），确保产物兼容老固件 |
| `make clean` | 删除 `build/`、`bin/photo-player`、`photo_plugins-release/` |
| `make help` | 显示目标与变量说明 |

常用变量（均可命令行覆盖）：`CROSS_TC`、`FBINK_DIR`、`KINDLE_IP`、`OUT_DIR`、`MAX_GLIBC`、`HOSTCC`。

```sh
make host-test
make all
make check-abi
make package
```

## 部署

**方式 A：USB 拷贝**

```sh
make all && make package
```

把 `photo_plugins-release/photo_plugins/` 整个目录拷到设备 `/mnt/us/extensions/` 下，保持目录名 `photo_plugins`。

**方式 B：网络安装（WSL 内执行）**

```sh
make install KINDLE_IP=192.168.x.x
```

安装后在 KUAL 菜单中出现扩展入口。

## 使用

### KUAL 菜单（menu.json 原文）

| 菜单项 | action |
|--------|--------|
| 启动相册-本地顺序 | `./bin/photo-player.sh --source /mnt/us/photos --order seq --interval 600 --no-framework` |
| 启动相册-本地随机 | `./bin/photo-player.sh --source /mnt/us/photos --order random --interval 600 --no-framework` |
| 启动相册-网络 | `./bin/photo-player.sh --source http://REPLACE_ME:8080/photo --interval 600 --no-framework` |

本地菜单读取 `/mnt/us/photos` 目录；网络菜单的 URL 是占位符，部署前须替换 `REPLACE_ME`。也可用 `bin/photo-player.sh --stop` 停止、`--debug` 前台调试、`--selftest` 自检。

### 网络相册（PC 端服务）

PC 端起图片服务（Python 3.8+ 标准库，无第三方依赖）：

```sh
python3 tools/photo_server.py --dir photos --port 8080
```

KUAL 菜单「启动相册-网络」把 URL 写成 `http://<PC-IP>:8080/photo` 即可；服务端忽略 URL 路径、只认 query 参数（`/photo` 换成任意路径都行）。协议为 `?count=1` 取总数、`?index=N` 取第 N 张（0 基），按文件名字节序排序、每次请求实时扫描目录，照片增删即时生效。

### 退出方式

- 轻触屏幕（默认开，`--no-touch-exit` 关闭）
- 按电源键（默认开，`--no-power-exit` 关闭）

### CLI 参数（photo-player）

| 参数 | 默认 | 说明 |
|------|------|------|
| `--source <path\|url>` | —（必填，conf 可供） | 本地目录路径，或 `http://host[:port]/path` URL |
| `--order seq\|random` | `seq` | 播放顺序（random 为洗牌一轮内不重复） |
| `--interval <sec>` | `60` | 切换间隔秒数，合法范围 2..86400 |
| `--fit contain\|cover\|stretch` | `contain` | 等比适配 / 等比裁剪铺满 / 拉伸 |
| `--statusbar keep\|cover` | `cover` | 保留状态栏 / 全屏含状态栏区域 |
| `--flash-every <n>` | `8` | 每 n 张闪烁全刷防残影，0=关闭 |
| `--max-file-mb <n>` | `8` | 单张图片大小上限 MB，0=不限制 |
| `--start-delay <ms>` | `2000` | 首张上屏前延迟毫秒，0..60000（0=不延迟） |
| `--no-framework` | 关 | 播放期间 `stop framework`，退出恢复 |
| `--no-touch-exit` | 关（即触摸退出默认开） | 禁用轻触退出 |
| `--no-power-exit` | 关（即电源键退出默认开） | 禁用电源键退出 |
| `--debug` | 关 | 详细日志 |
| `--config <file>` | `config/photo-player.conf`（存在时） | 指定配置文件 |
| `--help` | — | 显示帮助 |

### 配置文件键（config/photo-player.conf，KEY=VALUE，`#` 注释）

| 键 | 默认（conf） | 说明 |
|----|--------------|------|
| `SOURCE` | `/mnt/us/photos` | 图片源：本地目录或 `http://` URL |
| `ORDER` | `seq` | `seq` \| `random` |
| `INTERVAL` | `600` | 切图间隔秒数 [2, 86400] |
| `START_DELAY` | `2000` | 启动延迟毫秒 [0, 60000] |
| `FIT` | `contain` | `contain` \| `cover` \| `stretch` |
| `STATUSBAR` | `cover` | `cover` \| `keep` |
| `FLASH_EVERY` | `8` | 每 N 张全刷一次，0=关闭 |
| `MAX_FILE_MB` | `8` | 单张大小上限 MB，0=不限制 |
| `NO_FRAMEWORK` | `0` | 1=播放期间 stop framework |
| `TOUCH_EXIT` | `1` | 1=轻触退出 |
| `POWER_EXIT` | `1` | 1=电源键退出（与 `TOUCH_EXIT` 独立） |
| `HTTP_URL` | 空 | 网络图片源基础 URL（`?count=1` / `?index=N` 协议） |

**参数优先级：CLI / KUAL params > 配置文件 > 程序内置默认。**

## 图片准备

`tools/resize_photos.py`（PC 端，Python 3.8+ / Pillow）：

```sh
# 常用：预缩放 + 灰度 + 16 级预抖动（输出到 photos_resized/）
python tools/resize_photos.py photos --gray --levels 16

# 只看处理计划，不写文件
python tools/resize_photos.py photos --dry-run

# 递归处理子目录，指定输出目录与目标尺寸
python tools/resize_photos.py photos out --recursive --box 600x800
```

可选参数：`--box WxH`（默认 600x800）、`--format jpg|png`、`--quality N`（默认 90）。建议预缩放：设备直接解码大图很慢且易 OOM，实测 20MP 原图单张解码约 5-8s，预缩放到屏幕分辨率后秒开，灰度 PNG 在 8bpp framebuffer 上直拷最快。

## 工作原理简述

主循环以固定节拍（不累积漂移的绝对 deadline）到时触发：从图片源取下一张（本地目录扫描/洗牌，或 HTTP 按 index 取图）→ FBInk 全屏渲染并等待刷新完成 → 睡到下个节拍。常规切换走 `WFM_GC16_FAST`，每 `--flash-every` 张用 `WFM_GC16` 闪烁全刷清残影。架构、协议与模块细节见 [DESIGN.md](DESIGN.md)。

## 已知限制

- 网络相册：服务端已提供（`tools/photo_server.py`，实现 `?count=1` / `?index=N` 协议），设备端 `source_http.c`（HTTP 取图）待实现；`menu.json` 中 URL 仍需替换 `REPLACE_ME`
- 暂不支持 HTTPS（老固件 OpenSSL 限制；后续计划引入静态 libcurl）
- `--no-framework` 退出后 framework 冷启动约 30-60s 才恢复界面（固件行为，退出时会显示等待提示）
- `cover` 缩放模式为近似实现（等比适配 + 白底），真裁剪铺满待后续版本
- FBInk 上游 `qSmoothScale` 失败路径存在内存泄漏，触发面窄（大图缩放失败时）

## License

GPL-3.0（见 [LICENSE](LICENSE)）。

本项目静态链接 [FBInk](https://github.com/NiLuJe/FBInk)（GPL-3.0），依其条款，本项目整体以 GPL-3.0 分发。
