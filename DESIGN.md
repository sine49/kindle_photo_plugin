# photo_plugins — Kindle 照片滚动播放工具设计方案

> 状态：**待评审** v0.2（已按评审输入锁定：设备基线、HTTP 协议、仅 HTTP）
> 范围：设计 / 开发 / 测试均置于 `extensions/photo_plugins/` 目录下
> 基线：FBInk v1.25.0（`.slim/clonedeps/repos/NiLuJe__FBInk/`，只读）、KUAL 扩展框架

---

## 0. 已锁定决策（评审输入）

| 决策 | 结论 | 设计影响 |
|------|------|----------|
| 目标设备 | **Kindle 7，固件 5.12.2.2** | i.MX6SL 世代：softfp、EPDC v1（无硬件抖动）、旧 glibc；工具链锁定 `arm-kindlepw2-linux-gnueabi`（§10.1）；分辨率不硬编码，运行时自适应（2014 Basic 为 600×800，若指 PW3 则 1072×1448，两者均覆盖） |
| HTTP 协议 | **客户端带参数选图** | `pp_index` / `pp_order` / `pp_count` 三参数协议（§6.3） |
| 传输 | **首版仅 HTTP** | 不引入静态 libcurl/CA；HTTPS 列为后续里程碑 |

---

## 1. 目标与需求映射

| # | 需求 | 设计落点 |
|---|------|----------|
| R1 | 可配置启动参数：网络/本地目录图片库 | CLI 参数 + 配置文件 + KUAL 菜单项（§5） |
| R2 | 顺序/随机播放模式 | `--order seq\|random`，播放引擎（§7） |
| R3 | 播放间隔可配置 | `--interval <sec>`，定时驱动切换（§7） |
| R4 | 网络取图：HTTP，客户端带参数选图 | `http` 图片源，`pp_index` 取图协议（§6.3） |
| R5 | 本地取图：每次开目录下一张 | `local` 图片源，扫描+游标（§6.2） |
| R6 | 定时器按间隔触发切图 | 主循环定时节拍（§7） |
| R7 | 正常启动/退出、尽量全屏、少被系统刷屏干扰 | 生命周期 + 防干扰（§8、§9） |

---

## 2. 调研结论摘要

### 2.1 仓库现有实验（盘点结论）

| 实验 | 形态 | 可复用 | 不复用/需修复 |
|------|------|--------|---------------|
| `extensions/fbink-plugin/` | C 单体 ELF + KUAL；`content_module.h/.c` 是**进程内静态模块注册表**（`__attribute__((constructor))` 注册，非 dlopen） | KUAL 打包结构、Makefile 交叉编译+libfbink.a 静态链、`bin/*.sh` 启停脚本模式、停止文件/PID 停止机制 | ①现有 `bin/fbink-plugin` 含 `GLIBC_2.34/2.38` 符号，真机大概率起不来（工具链用错，见 §11）；②分辨率硬编码 600×800；③触摸分发死链；④`content_module` 对单功能应用是过度设计 |
| `extensions/go-player/` | shell + KUAL | 生命周期模式（start/stop/debug、trap 恢复、PID+停止文件双通道）、旋转保存/恢复多级 fallback、KUAL 返回后再清屏的经验、BusyBox ash POSIX 兼容教训 | 渲染走 `fbink` CLI/`eips`，本项目改用 C API 静态链接 |

**结论**：`photo_plugins` 采用"**单体 C ELF + KUAL 扩展**"形态；**不复用** `content_module` 注册表（照片播放是单一功能应用，单体分层即可，避免实验框架的复杂度与缺陷）；打包与启停脚本沿用两个实验已验证的模式。

### 2.2 外部机制（调研结论，来源见文末）

- **KUAL 扩展规范**：`/mnt/us/extensions/<id>/` 目录 = `config.xml` + `menu.json` + `bin/`；`menu.json` 的 `action` 为 `/bin/ash` 命令（后台运行、工作目录为扩展目录），`params` 拼接到 action 后传递参数；stderr 落 `KUAL.log` 需自行重定向。
- **FBInk 波形**：照片内容官方推荐 `WFM_GC16`（~450ms，高保真、残影风险最低）；快速切换可用 `GC16_FAST`；周期性 `is_flashing` 全刷平衡残影。灰度 PNG（8bpp fb）直拷最快，建议预缩放到屏分辨率。
- **防干扰**：`lipc-set-prop com.lab126.powerd preventScreenSaver 1/0` 对称设置/恢复（KOReader keepalive 同款）；激进做法 `stop framework`（须退出时 `start framework`）；副作用：preventScreenSaver 可能影响电源键感知，需真机验证。
- **HTTP**：固件有 busybox `wget`；自带 curl/wget 的 HTTPS 受老 OpenSSL 限制 → **首版只承诺 HTTP**，HTTPS 列为后续（静态 libcurl）。
- **工具链（已按目标设备锁定）**：Kindle 7 / FW 5.12.2.2 属 i.MX6SL 世代、**softfp** → koxtoolchain `arm-kindlepw2-linux-gnueabi`；**不能用 kindlehf**（hard-float 需 FW ≥ 5.16.3），更不能用宿主发行版 gnueabihf。FBInk `make static` 产出 `libfbink.a`。
- **EPDC v1 限制（本设备）**：无硬件抖动（`dithering_mode` 需 EPDC v2 / KOA2·PW4·KT4 世代起）→ 照片走**预缩放 + 预抖动**（灰度 PNG 在 8bpp fb 上最快），设备端兜底 `sw_dithering`。
- **开源先例**：`aesuli/Kindle-Photo-Frame`、`brunomarinho/labs-kindle-artframe`、`pascalw/kindle-dash`、`jefftko/kindle-dashboard` 等——共识是 GC16 全刷换图 + 预缩放 + 启停防休眠。

---

## 3. 总体架构

单一进程 `photo-player`（ELF，ARM32），后台常驻，事件节拍驱动：

```
KUAL 菜单 (menu.json params)
        │
        ▼
bin/photo-player.sh ──后台化/PID/日志/停止文件──▶ photo-player (ELF)
                                                    │
                        ┌─────────────── 主循环（定时节拍）───────────────┐
                        │                                                  │
                  到间隔触发                                          SIGTERM/停止文件
                        │                                                  │
                        ▼                                                  ▼
              source->next()  ──图片路径──▶  display->show()          cleanup() 退出
              (local | http)                (FBInk 全屏渲染)          (恢复系统状态)
                        │                                                  ▲
                        └──────── platform (防休眠/框架管理) ──────────────┘
```

### 3.1 模块划分

| 模块 | 文件 | 职责 |
|------|------|------|
| 生命周期 | `src/main.c` | 参数装配、信号处理、主循环、退出清理 |
| 配置 | `src/config.c/.h` | CLI 解析、配置文件读取、参数校验 |
| 图片源抽象 | `src/source.h` | `PhotoSource` 接口（next/peek/count/cleanup） |
| 本地源 | `src/source_local.c` | 目录扫描、扩展名过滤、顺序游标/洗牌随机 |
| 网络源 | `src/source_http.c` | HTTP GET 取图（首版 fork/exec `wget`，接口预留 libcurl 实现位） |
| 播放引擎 | `src/player.c/.h` | 定时节拍、顺序/随机调度、切图请求 |
| 显示 | `src/display.c/.h` | FBInk 封装：init/全屏显示/清屏/波形选择/旋转自适应/等待完成 |
| 平台 | `src/platform.c/.h` | preventScreenSaver 设置/恢复、可选 framework 管理、状态栏处理 |
| 工具 | `src/log.c/.h`、`src/util.c/.h` | 日志、字符串/时间工具 |

依赖方向：`main → player → source/display/platform`；`source/display/platform` 互不依赖。

---

## 4. 进程与运行形态

- **一个 ELF**：`photo-player`，静态链接 `libfbink.a`（+ `libi2c`/`pthread`/`m`），动态仅依赖 libc/ld（与 FBInk 官方 Kindle 静态产物一致）。
- **后台运行**：由 `photo-player.sh` fork 后台（`--debug` 时前台）；stdout/stderr 重定向到 `/tmp/photo-player.log`。
- **单实例**：PID 文件 `/tmp/photo-player.pid`，启动前检测并拒绝重复启动。
- **事件模型**：单线程 + sleep 节拍（不用 timerfd/epoll——老固件 glibc 符号风险，且单功能应用无并发需求）。信号处理器只置 `volatile sig_atomic_t` 标志，主循环以 `nanosleep` 可中断方式醒来后走统一 cleanup。

---

## 5. 启动参数与配置（R1/R2/R3）

### 5.1 命令行（`photo-player [options]`）

| 参数 | 类型/默认 | 说明 |
|------|-----------|------|
| `--source <path\|url>` | 必填* | 本地目录路径，或 `http://host[:port]/path` URL。*有配置文件时可省 |
| `--order <seq\|random>` | `seq` | 播放顺序（R2） |
| `--interval <sec>` | `60` | 切换间隔秒数，合法域 `[2, 86400]`（R3；下限 2s 留足 GC16 刷新余量） |
| `--fit <contain\|cover\|stretch>` | `contain` | 缩放策略：等比适配 / 等比裁剪铺满 / 拉伸 |
| `--statusbar <keep\|cover>` | `cover` | 保留顶部状态栏 or 全屏含状态栏区域 |
| `--flash-every <n>` | `8` | 每 n 张用闪烁全刷防残影（0=关闭） |
| `--no-framework` | 关 | 激进防干扰：播放期间 `stop framework`，退出恢复（R7 可选项） |
| `--debug` | 关 | 前台运行 + 详细日志 |
| `--config <file>` | 扩展内默认路径 | 指定配置文件 |

配置文件：`config/photo-player.conf`（`KEY=VALUE` 行，注释 `#`），**CLI 覆盖配置文件，配置文件覆盖内置默认**。KUAL `params` 即 CLI 参数，因此同一二进制可由菜单出多种预设。

### 5.2 KUAL 菜单项（menu.json 草案）

| 菜单项 | action/params |
|--------|---------------|
| 启动相册-本地顺序 | `./bin/photo-player.sh --source /mnt/us/photos --order seq --interval 600 --no-framework` |
| 启动相册-本地随机 | `./bin/photo-player.sh --source /mnt/us/photos --order random --interval 600 --no-framework` |
| 启动相册-网络 | `./bin/photo-player.sh --source http://REPLACE_ME:8080/photo --interval 600 --no-framework` |

（菜单收敛为 3 项：`--debug`/`--stop`/`--selftest` 保留为 CLI/SSH 工具，不进菜单。
网络 URL 为占位符 REPLACE_ME，部署时在 menu.json 里替换为实际服务地址。）

---

## 6. 图片源设计（R1/R4/R5）

### 6.1 接口

```c
typedef struct PhotoSource PhotoSource;
struct PhotoSource {
    int  (*next)(PhotoSource*, char* out_path, size_t len); // 0=成功拿到下一张(输出文件路径)
    void (*reset)(PhotoSource*);
    void (*cleanup)(PhotoSource*);
    const char* name;
};
```

### 6.2 `local`：本地目录

- 扫描目录（不递归 v1），过滤扩展名：`jpg jpeg png bmp gif tga pnm`（FBInk `fbink_print_image` 支持集）。
- `seq`：数组顺序游标，到尾回绕；`random`：Fisher–Yates 洗牌一轮内不重复，播完重洗（比纯随机体验好，见 §13-Q2）。
- 目录内容变更：v1 不热加载，`reset()` 时重扫；后续可加 SIGHUP 触发重扫。
- 文件拿不到/解码失败：跳过并记日志，连续失败超过目录内图片数则报错退出。

### 6.3 `http`：网络取图（R4，已锁定：客户端带参数选图）

**协议约定 v1**（服务端无状态、无需会话）：

| 请求 | 响应 | 说明 |
|------|------|------|
| `GET <base>?count=1` | `text/plain`，整数 N | 图片库总数（启动时取一次，`reset()` 时刷新） |
| `GET <base>?index=N` | `Content-Type: image/*` 二进制 | 取第 N 张（0 基）；N ≥ N总 返回 404，客户端回绕 |

- **顺序模式**：客户端 index = 0,1,…,N-1,0…；
- **随机模式**：客户端洗牌 0..N-1 一轮不重复（与 local 源同一调度语义，§6.2）。
- `--order`/`--interval` 的调度完全在客户端；服务端只做"按 index 发图"。
- 服务端可带 `X-Photo-Count`/`X-Photo-Index` 响应头便于日志，客户端不依赖。

**实现**：`fork/exec /usr/bin/wget -q -T <timeout> -O /tmp/photo-player-XXXX.jpg '<url>?index=N'`（避开老 OpenSSL；接口层留 `USE_CURL` 编译开关供后续静态 libcurl/HTTPS）。下载到临时文件再交 `fbink_print_image`（文件路径便于重试与调试）。

**失败策略**：404（index 越界）→ 重新 `count` 后回绕重试一次；网络失败/非图片 → 指数退避重试（3 次，1s/2s/4s）→ 仍失败保持当前画面、跳过本轮，连续 5 轮失败进入"错误等待"（每 `--interval` 重试一次）并持续记日志；`count` 获取失败 → 降级为**顺序逐增 index 直到 404 回 0**、随机退化为顺序并记警告日志。不做错误画面（E-Ink 刷新成本高）。

---

## 7. 播放引擎与定时器（R2/R3/R6）

```
主循环:
  next_deadline = now() + interval
  while (!quit_flag):
      取下一张: source->next()          // 顺序/随机由 source 内部实现
      display->show(path)               // 全屏渲染 + 刷新 + wait_for_complete
      睡到 next_deadline（nanosleep，被信号 EINTR 唤醒则检查 quit_flag）
      next_deadline += interval         // 固定节拍，不累计漂移
```

- **切图请求**即 `source->next()` + `display->show()`，由定时器到时触发（R6 的"图片切换请求"）。
- 间隔 = 显示驻留时间；实际可见节奏 ≈ `interval`（刷新耗时并入显示步骤，不额外叠加）。
- 顺序/随机语义见 §6.2/§6.3；`http` 源的节奏与选图均由客户端控制（按 `?index=N` 取图），服务端无状态。

---

## 8. 显示层（FBInk，R7）

- **初始化**：`fbink_open()` → `fbink_init()` → `fbink_get_state()` 取真实 `screen_width/height/bpp/currentRota`（**不硬编码分辨率**，修复现有实验的坑）。
- **全屏显示**：`fbink_print_image(fbfd, path, 0, 0, cfg)`，`cfg.scaled_width/height` 按 `--fit`：
  - `contain`：`<-1` 等比适配视口 + `halign/valign=CENTER`；
  - `cover`：等比放大到铺满后居中裁剪（v1 用等比适配+白底，真机验证后决定是否上裁剪）；
  - `stretch`：`= -1`（视口尺寸）。
- **波形**：常规切换 `WFM_GC16_FAST`（Kindle 主力、延迟更好）；每 `--flash-every` 张走 `WFM_GC16 + is_flashing` 全刷清残影（先例项目共识）。间隔 < 10s 的场景建议默认 `WFM_GC16`。
- **状态栏**：`--statusbar cover` 时图片画满含状态栏区域（系统栏可能被刷掉直到系统下次重绘——可接受，R7 允许先放放）；`keep` 时图片限位于状态栏以下。
- **旋转**：每轮 `fbink_get_state()` 检测 `currentRota`/尺寸变化，变了就 `fbink_reinit()` 并按新视口重算缩放。
- **节流**：`fbink_wait_for_complete(fbfd, fbink_get_last_marker())` 在下一次刷新前等待（FBInk ≥1.20 惯例），避免更新堆积。
- **性能建议**（文档化到 README）：图片预缩放到屏幕分辨率、灰度 PNG 最快；大图直喂会耗内存/时间。

---

## 9. 防干扰与系统集成（R7）

| 措施 | 启动时 | 退出时 | 风险/备注 |
|------|--------|--------|-----------|
| 禁屏保 | `lipc-set-prop com.lab126.powerd preventScreenSaver 1` | 恢复 `0` | 电源键响应可能变化，真机验证；备选 `deferSuspend` |
| 清 KUAL 残留 | 启动后延迟 ~1s 再清屏/首绘（go-player 经验） | — | — |
| `--no-framework` | `stop framework` | `start framework` | 可选、默认关；能根治系统刷屏，但期间系统 UI 全不可用，必须保证异常退出也能恢复（§9.1） |
| 触摸退出（可选 v1.1） | 读 `/dev/input/event*`，轻触即退出 | 同正常退出 | 参考 fbink-plugin 触摸线程（需修复其分发死链问题） |

### 9.1 退出清理（必须保证幂等、可重复调用）

```
cleanup():  # 信号处理器只置标志，主循环统一执行
  1. fbink 清屏 + fbink_close（先清屏后恢复 framework，保证 framework 首绘永远落在最后的清屏之上；
     退出提示：仅 no_framework 模式在白底上居中一行 "Returning to Kindle UI, please wait..."（framework
     冷启动 30-60s 才首绘，避免白屏被当死机），普通模式纯白不提示）
  2. 若曾 stop framework → start framework（+ pillow enable）
  3. preventScreenSaver 0（仅当本进程设置过）
  4. 删除临时图片、PID 文件、停止文件
  5. 日志落盘、exit(0)

标记文件：`stop framework`/pillow disable 成功后落 `/tmp/photo-player.framework-stopped`、`/tmp/photo-player.pillow-disabled`，对应恢复成功即删（失败保留供重试）；SIGKILL 强杀无法自救，由 `photo-player.sh --stop` 读标记代为恢复（崩溃不处理）。
```

**停止通道**（三通道，沿用两个实验已验证模式）：

1. KUAL `Stop` 菜单 → `photo-player.sh --stop`：touch `/tmp/photo-player.stop` → 等待 PID 消失 → `pkill -f photo-player` 兜底；
2. 信号：`SIGTERM`/`SIGINT` → 主循环检查标志退出；
3. （可选 v1.1）触摸屏轻触退出。

---

## 10. 构建与部署

### 10.0 宿主环境（已确认）

- 宿主机为 **Windows + WSL**：交叉编译、`host-test`、`package` 一律在 **WSL 内**执行（仓库路径 `/mnt/e/trae_source/kindle`）；Makefile/脚本只用 POSIX sh，禁 Windows 路径分隔符。
- 所有 `*.sh` 必须 **LF 换行** + BusyBox ash 兼容（bash 数组等 bashism 会炸，go-player 已踩坑）。
- `make install` 由 WSL 发起 `scp`（`make install KINDLE_IP=...`）。

### 10.1 工具链（修复现有实验的 glibc 坑）

- **必须用 koxtoolchain**（FBInk 官方推荐），不使用宿主发行版的 `arm-linux-gnueabihf`（现有 `fbink-plugin` 二进制的 `GLIBC_2.34/2.38` 符号就是这么来的）：
  - **本项目锁定：`arm-kindlepw2-linux-gnueabi`（softfp）**——Kindle 7 / FW 5.12.2.2 低于 kindlehf 的 FW ≥ 5.16.3 门槛，属 i.MX6SL/softfp 世代；
  - `CROSS_TC` 变量保留覆盖能力（日后上新机型可切 `arm-kindlehf-linux-gnueabihf`）。
- `CROSS_TC` 变量可覆盖；CI/本地校验步骤：`readelf --version-info` 审计 glibc 版本符号 ≤ 设备侧版本。
- FBInk：`make -C .slim/clonedeps/repos/NiLuJe__FBInk static` 产出 `libfbink.a` 后静态链接（沿用 fbink-plugin/Makefile 的做法，修掉其 `-DFBINK_SHAREDLIB` 与静态链接并存的可疑设置）。

### 10.2 Makefile 目标

| 目标 | 作用 |
|------|------|
| `make` | 交叉编译产出 `bin/photo-player`（strip） |
| `make fbink` | 先构建 FBInk 静态库 |
| `make host-test` | 宿主机 gcc 编译并跑单元测试（§12） |
| `make install KINDLE_IP=…` | scp 到 `root@<ip>:/mnt/us/extensions/photo_plugins/` |
| `make package` | 产出 `photo_plugins-release/photo_plugins/`（KUAL 可直接拷贝包） |
| `make check-abi` | 产物 glibc 符号审计 |

### 10.3 KUAL 包结构（对齐现有实验的发布包布局）

```
photo_plugins/                  ← /mnt/us/extensions/photo_plugins/
├── config.xml
├── menu.json
├── bin/
│   ├── photo-player            # 交叉编译 ELF
│   └── photo-player.sh         # 启停脚本（start/stop/debug）
└── config/
    └── photo-player.conf       # 默认配置（可被用户改）
```

---

## 11. 源码目录结构（开发态）

```
extensions/photo_plugins/
├── DESIGN.md                  # 本文档
├── README.md                  # 使用/部署/排障（开发完成后补）
├── Makefile
├── src/                       # §3.1 模块
├── bin/                       # 构建产物 + 启停脚本
├── config/photo-player.conf
├── test/
│   ├── host/                  # 宿主机单元测试（config/调度/洗牌/目录扫描）
│   ├── server/                # 服务端协议测试（test_photo_server.py，纯 Python）
│   └── device/                # 真机手工测试清单与脚本
├── tools/
│   ├── photo_server.py        # §6.3 协议的服务端参考实现（纯 Python 标准库）
│   └── resize_photos.py       # PC 端批量预缩放/灰度/预抖动工具
└── photo_plugins-release/     # make package 产物
```

---

## 12. 测试计划

### 12.1 宿主机单元测试（`make host-test`，native gcc，不碰 FBInk/设备）

| 对象 | 用例要点 |
|------|----------|
| `config` | CLI 覆盖配置文件、非法值拒绝（interval 边界、order 枚举）、缺 source 报错 |
| `source_local` | 扫描过滤、顺序回绕、洗牌一轮不重复、空目录/全坏文件行为 |
| `player` | 节拍不漂移（mock clock）、quit 标志中断睡眠 |
| `source_http` | 重试/退避逻辑（mock wget 脚本）、失败计数复位 |
| `photo_server`（tools/photo_server.py，Python） | §6.3 服务端协议：count/index、越界 404、非法 index 400、扩展名→ctype 映射与过滤、中文文件名、目录增删计数（`python3 test/server/test_photo_server.py`，独立于 make host-test） |

### 12.2 交叉编译验证（无设备也可做）

- `make check-abi`：glibc 符号版本 ≤ 目标固件；
- `file`/`readelf` 核对 ELF 架构（ARM32、hf/softfp 与工具链选择一致）；
- 与 `tools/photo_server.py` 联跑（qemu-user 或真机均可）。

### 12.3 真机手工测试清单（`test/device/DEVICE_CHECKLIST.md`）

1. 启动/停止/Debug 三种入口均可用，重复启动被拒绝；
2. 本地顺序：切图间隔准确（±2s 内）、到尾回绕；随机：一轮内不重复；
3. HTTP 源：mock 服务器取图成功；断网 → 重试 → 恢复后继续；
4. 全屏效果：contain/cover/stretch 三种 fit、状态栏 keep/cover；
5. 防干扰：播放 30min 不息屏；`--no-framework` 下无系统刷屏，退出后系统 UI 恢复；
6. 退出清理：三种停止通道后 `preventScreenSaver=0`、framework 在跑、无残留进程/临时文件；
7. 旋转：播放中旋屏，下一张按新方向正确显示；
8. 稳定性：连续播放 ≥ 4h 无内存增长（`/proc/<pid>/status` VmRSS 采样）；
9. 波形观感：`flash-every` 残影控制效果、GC16_FAST 观感。

---

## 13. 风险与开放问题（评审重点）

| # | 问题 | 状态 | 备注 |
|---|------|------|------|
| Q1 | **HTTP 协议形状** | **已锁定**：客户端带参数选图（`?count=1` / `?index=N`） | §6.3 |
| Q2 | **随机语义**：一轮内不重复（洗牌）vs 纯随机（允许重复） | 默认洗牌不重复 | 协议已定客户端调度，实现成本低，可加 `--random-mode`；如无异议按默认 |
| Q3 | **HTTPS** 是否需要？ | **已锁定**：首版仅 HTTP | HTTPS 需静态 libcurl + CA，列后续里程碑 |
| Q4 | **目标机型/固件** | **已锁定**：Kindle 7 / FW 5.12.2.2 | → kindlepw2 softfp（§10.1）、EPDC v1 无硬件抖动（§2.2）；分辨率运行时自适应（600×800 或 1072×1448 均覆盖） |
| Q5 | preventScreenSaver 的电源键副作用 | 先用它，真机验证后决定是否换 deferSuspend | §9 |
| Q6 | 触摸退出要不要 v1 就上？ | 默认 v1.1 再做 | 需修好触摸分发（现有实验是死链） |
| Q7 | 图片缩放放设备端还是服务端预缩放？ | 设备端 fit + README 建议预缩放 | 大图解码内存风险（本机 RAM 有限） |

## 14. 里程碑

| 阶段 | 内容 | 验收 |
|------|------|------|
| M1 | 骨架 + 配置 + local 源 + 顺序播放 + 全屏显示 + 启停/清理 | 真机本地顺序轮播可用，退出干净 |
| M2 | 随机播放 + 间隔/fit/flash 配置 + 防休眠 + `--no-framework` | §12.3-1~6 通过 |
| M3 | http 源 + photo_server + 重试退避 | §12.3-3 通过 |
| M4 | 单测补齐 + check-abi + package + README | `make host-test` 全绿，KUAL 包可拷贝即用 |

---

## 参考来源

- KUAL 规范：https://wiki.mobileread.com/wiki/KUAL_What%27s_New
- FBInk：https://github.com/NiLuJe/FBInk （本地 `.slim/clonedeps/repos/NiLuJe__FBInk/`）
- koxtoolchain：https://github.com/koreader/koxtoolchain
- LIPC/电源：https://kindlemodding.org/kindle-apps-and-services/com.lab126.powerd.html ；KOReader keepalive 插件
- 先例：aesuli/Kindle-Photo-Frame、brunomarinho/labs-kindle-artframe、pascalw/kindle-dash、jefftko/kindle-dashboard、OmniViewApp/kindle-omni-view
- 仓库内：`extensions/fbink-plugin/`（C+KUAL 实验）、`extensions/go-player/`（生命周期/旋转恢复经验）、`FBInk_API_Reference.md`
