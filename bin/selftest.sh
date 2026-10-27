#!/bin/sh
# selftest.sh — Photo Player 设备端自检脚本
# 目标 shell：BusyBox ash / POSIX sh（禁止 bashism；必须 LF 换行）
#
# 用法:
#   sh selftest.sh                  # 或经 photo-player.sh --selftest 调用
#
# 行为:
#   - 全量诊断报告写入 <扩展目录>/logs/selftest.txt（/mnt/us USB 可读）
#   - 摘要 echo 到 stdout（KUAL 会话可见，会落 KUAL.log）
#   - 任何单项失败都不中断（逐项标注 FAILED），结束打印 SELFTEST DONE
#
# 注意: 禁止 bash 数组/[[]]；不要 set -e；不要全局 trap。

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PLUGIN_DIR="$(dirname "$SCRIPT_DIR")"
LOG_DIR="$PLUGIN_DIR/logs"
REPORT="$LOG_DIR/selftest.txt"

if ! mkdir -p "$LOG_DIR"; then
    echo "Error: cannot create $LOG_DIR"
    exit 1
fi
if ! : > "$REPORT"; then
    echo "Error: cannot write $REPORT"
    exit 1
fi

# 报告 + stdout 同步输出一行（stdout 即摘要）
say() {
    echo "$*" >> "$REPORT"
    echo "$*"
}

# 小节标题
section() {
    echo "" >> "$REPORT"
    echo "===== $* =====" >> "$REPORT"
    echo "[*] $*"
}

# 执行命令，输出记入报告；失败标注 FAILED，不中断
run() {
    echo "\$ $*" >> "$REPORT"
    "$@" >> "$REPORT" 2>&1 || echo "FAILED (exit $?)" >> "$REPORT"
}

# 同 run，但允许 shell 管道/重定向
runsh() {
    echo "\$ $*" >> "$REPORT"
    sh -c "$*" >> "$REPORT" 2>&1 || echo "FAILED (exit $?)" >> "$REPORT"
}

# 带 3s 超时跑一次 bin/photo-player（防止意外进入播放把自检挂住）：
# 记录退出码 + stdout/stderr 前 5 行
run_player() {
    label="$1"
    shift
    echo "\$ ./bin/photo-player $*" >> "$REPORT"
    ./bin/photo-player "$@" > /tmp/photo-player-selftest.out 2> /tmp/photo-player-selftest.err &
    bpid=$!
    waited=0
    while [ "$waited" -lt 3 ]; do
        if ! kill -0 "$bpid" 2>/dev/null; then
            break
        fi
        sleep 1
        waited=$((waited + 1))
    done
    note=""
    if kill -0 "$bpid" 2>/dev/null; then
        note=" (still running after 3s -> SIGTERM; 可能已进入播放)"
        kill "$bpid" 2>/dev/null
        sleep 1
        kill -9 "$bpid" 2>/dev/null
    fi
    wait "$bpid" 2>/dev/null
    brc=$?
    say "$label: exit code $brc$note"
    echo "--- stdout (first 5 lines) ---" >> "$REPORT"
    head -5 /tmp/photo-player-selftest.out >> "$REPORT" 2>&1
    echo "--- stderr (first 5 lines) ---" >> "$REPORT"
    head -5 /tmp/photo-player-selftest.err >> "$REPORT" 2>&1
}

say "photo-player selftest report"
say "report file: $REPORT"

# ---- 1. 系统信息 ---------------------------------------------------------
section "1. date / uname -a / id"
run date
run uname -a
run id

# ---- 2. 扩展目录树 -------------------------------------------------------
section "2. extension dir tree"
say "PLUGIN_DIR=$PLUGIN_DIR"
run ls -la "$PLUGIN_DIR"
run ls -la "$PLUGIN_DIR/bin"
run ls -la "$PLUGIN_DIR/config"

# ---- 3. 二进制存在性 + ELF 魔数 ------------------------------------------
section "3. bin/photo-player: existence + ELF magic (expect: 7f 45 4c 46)"
if [ -f "$PLUGIN_DIR/bin/photo-player" ]; then
    run ls -la "$PLUGIN_DIR/bin/photo-player"
    run wc -c "$PLUGIN_DIR/bin/photo-player"
    magic="$(od -An -tx1 -N4 "$PLUGIN_DIR/bin/photo-player" 2>/dev/null)"
    if [ -z "$magic" ]; then
        say "ELF magic: FAILED (od 无输出)"
    else
        say "ELF magic:$magic"
        case "$magic" in
            *"7f 45 4c 46"*)
                say "ELF check: OK"
                ;;
            *)
                say "ELF check: BAD (不是 ELF，可能拷贝损坏/占位文件)"
                ;;
        esac
    fi
else
    say "MISSING: $PLUGIN_DIR/bin/photo-player (尚未 make / make package?)"
fi

# ---- 4. --help 退出码与输出 ---------------------------------------------
section "4. ./bin/photo-player --help (exit code + output)"
if [ -f "$PLUGIN_DIR/bin/photo-player" ]; then
    run_player "help run" --help
else
    say "SKIPPED (binary missing)"
fi

# ---- 5. 无参运行：预期 config 校验报错 -----------------------------------
section "5. ./bin/photo-player (no args) — 预期 config 校验报错"
if [ -f "$PLUGIN_DIR/bin/photo-player" ]; then
    run_player "no-arg run"
else
    say "SKIPPED (binary missing)"
fi
rm -f /tmp/photo-player-selftest.out /tmp/photo-player-selftest.err

# ---- 6. 照片目录 ---------------------------------------------------------
section "6. /mnt/us/photos"
if [ -d /mnt/us/photos ]; then
    run ls -la /mnt/us/photos
else
    say "PHOTOS_DIR_MISSING: /mnt/us/photos"
fi

# ---- 7. /mnt/us 前 20 项 + extensions/ 存在性 ----------------------------
section "7. /mnt/us listing (first 20)"
runsh "ls /mnt/us | head -20"
if [ -d /mnt/us/extensions ]; then
    say "extensions/: present"
else
    say "extensions/: MISSING"
fi

# ---- 7a. 内核输入设备清单（触摸节点识别的真相来源） ----------------------
# /proc/bus/input/devices 直接给出每个输入设备的 Name/Handlers/H: 与
# 能力位十六进制位图（ABS/BIT maps），无需工具即可判定触摸节点、
# 有无 BTN_TOUCH/TRACKING_ID。
section "7a. cat /proc/bus/input/devices (Name/Handlers/能力位图)"
if [ -f /proc/bus/input/devices ]; then
    run cat /proc/bus/input/devices
else
    say "PROC_INPUT_MISSING: /proc/bus/input/devices"
fi

# ---- 7b. 输入设备节点与权限 ----------------------------------------------
section "7b. ls -la /dev/input/ + ls -la /dev/input/event*"
if [ -d /dev/input ]; then
    run ls -la /dev/input
else
    say "INPUT_DIR_MISSING: /dev/input"
fi
runsh "ls -la /dev/input/event* 2>/dev/null"

# ---- 7c. 逐 event 节点十六进制头 64 字节 ---------------------------------
# 注意：od 直接读事件设备，可能吞掉少量输入事件（诊断用，可接受）；
# 且 evdev read() 无待处理事件时会阻塞，故后台跑 + 1s 超时强杀，防自检挂住。
section "7c. event node headers (od -An -tx1 -N 64)"
found_events=0
for f in /dev/input/event*; do
    if [ -e "$f" ]; then
        found_events=$((found_events + 1))
        echo "-- $f" >> "$REPORT"
        echo "\$ od -An -tx1 -N 64 $f" >> "$REPORT"
        od -An -tx1 -N 64 "$f" >> "$REPORT" 2>/dev/null &
        opid=$!
        sleep 1
        od_timeout=0
        if kill -0 "$opid" 2>/dev/null; then
            od_timeout=1
            kill "$opid" 2>/dev/null
            sleep 1
            kill -9 "$opid" 2>/dev/null
        fi
        wait "$opid" 2>/dev/null
        orc=$?
        if [ "$od_timeout" -eq 1 ]; then
            echo "(od blocked -> killed after 1s: 无待处理事件，未读出头部)" >> "$REPORT"
        elif [ "$orc" -ne 0 ]; then
            echo "FAILED (exit $orc)" >> "$REPORT"
        fi
    fi
done
if [ "$found_events" -eq 0 ]; then
    say "7c: NO_EVENT_NODES (/dev/input/event* 无匹配)"
else
    say "7c: $found_events 个 event 节点头部已记入报告"
fi
say "NOTE: 7c 用 od 直接读事件设备，可能吞掉少量输入事件（诊断用，可接受）"

# ---- 8. 玩家日志尾部 -----------------------------------------------------
section "8. tail -20 /tmp/photo-player.log"
if [ -f /tmp/photo-player.log ]; then
    run tail -20 /tmp/photo-player.log
else
    say "NOT FOUND: /tmp/photo-player.log"
fi

# ---- 9. KUAL 日志尾部 ----------------------------------------------------
section "9. tail -20 /mnt/us/extensions/KUAL.log"
if [ -f /mnt/us/extensions/KUAL.log ]; then
    run tail -20 /mnt/us/extensions/KUAL.log
else
    say "NOT FOUND: /mnt/us/extensions/KUAL.log"
fi

# ---- 10. 双写日志环境变量 -------------------------------------------------
section "10. PHOTO_PLAYER_LOG_EXTRA"
say "PHOTO_PLAYER_LOG_EXTRA=$PHOTO_PLAYER_LOG_EXTRA"

# ---- 11. 轻触退出配置对照 -------------------------------------------------
section "11. TOUCH_EXIT (轻触退出配置对照)"
if [ -n "$TOUCH_EXIT" ]; then
    say "TOUCH_EXIT=$TOUCH_EXIT"
else
    say "TOUCH_EXIT (conf): $(grep '^TOUCH_EXIT=' "$PLUGIN_DIR/config/photo-player.conf" 2>/dev/null || echo NOT_SET)"
fi

# ---- 12. 抢屏进程探测（状态栏归属/防干扰排障） ---------------------------
# 确认本机型（Kindle 7 / FW 5.12.2.2）上状态栏到底谁画的、
# pillow/mesquite 是否存在，为后续精确防干扰提供真机证据。
section "12. ps + 抢屏进程探测 (framework/cvm/mesquite/pillow)"
ps_out="$(ps 2>/dev/null)"
echo "\$ ps" >> "$REPORT"
echo "$ps_out" >> "$REPORT"
lipc_val="$(lipc-get-prop com.lab126.pillow disableEnablePillow 2>/dev/null || echo PILLOW_PROP_NA)"
say "pillow disableEnablePillow: $lipc_val"
for hog in framework cvm mesquite pillow; do
    if echo "$ps_out" | grep -q "$hog"; then
        say "screen-hog $hog: RUNNING"
    else
        say "screen-hog $hog: absent"
    fi
done

echo "" >> "$REPORT"
echo "SELFTEST DONE -> logs/selftest.txt" >> "$REPORT"
echo "SELFTEST DONE -> logs/selftest.txt"
exit 0
