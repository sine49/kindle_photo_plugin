#!/bin/sh
# photo-player.sh — Photo Player KUAL 启停脚本
# 目标 shell：BusyBox ash / POSIX sh（禁止 bashism；必须 LF 换行）
#
# 用法:
#   photo-player.sh [--debug] [--stop] [--selftest] [--help] [photo-player 参数...]
#
#   (无控制参数)  后台启动 bin/photo-player（工作目录 = 扩展目录），
#                 stdout/stderr -> /tmp/photo-player.log，
#                 PID 文件由 bin/photo-player 自行维护（脚本只读）；
#                 启动前查 PID 文件防重复
#   --debug       前台启动（透传其余参数；给二进制附加 --debug 详细日志）
#   --stop        停止：touch /tmp/photo-player.stop -> 轮询等 PID 消失
#                 -> 未消失则强制结束 -> 清理 PID/stop 文件
#   --selftest    前台运行 bin/selftest.sh 设备自检（多余参数忽略），
#                 报告写 logs/selftest.txt（USB 可读），exit 自检返回码
#   --help, -h    显示帮助
#
#   除上述控制参数外的参数（--source/--order/--interval/--fit/
#   --statusbar/--flash-every/--no-framework/--config ...）原样透传给
#   bin/photo-player。
#
# 注意:
#   - 不要加 `set -e`：清理路径上普通命令失败会让脚本提前退出，留下脏状态。
#   - 不要设置全局 EXIT trap：KUAL 后台化后父进程退出会误触发
#     （go-player 踩过的坑）。

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PLUGIN_DIR="$(dirname "$SCRIPT_DIR")"
PLAYER_BIN="./bin/photo-player"
PID_FILE="/tmp/photo-player.pid"
STOP_FILE="/tmp/photo-player.stop"
LOG_FILE="/tmp/photo-player.log"
# 状态标记文件：二进制改过系统状态时落标记（platform.c），SIGKILL 强杀后由本脚本代为恢复
MARKER_FRAMEWORK="/tmp/photo-player.framework-stopped"
MARKER_PILLOW="/tmp/photo-player.pillow-disabled"

show_help() {
    echo "Photo Player — Kindle 照片轮播"
    echo ""
    echo "用法: $0 [--debug] [--stop] [--selftest] [--help] [photo-player 参数...]"
    echo "  (无控制参数)  后台启动（日志: $LOG_FILE，USB 日志: logs/photo-player.log）"
    echo "  --debug       前台启动（详细日志）"
    echo "  --stop        停止正在运行的播放器"
    echo "  --selftest    设备自检（报告: logs/selftest.txt）"
    echo "  --help        显示此帮助"
    echo ""
    echo "透传参数示例:"
    echo "  --source /mnt/us/photos --order seq --interval 60"
    echo "  --source <dir|http-url> --order random --fit contain"
    echo "  --statusbar cover --flash-every 8 --no-framework --config <file>"
}

# 是否已有播放器在跑：以 PID 文件为准（对应进程存活即视为运行中）。
# PID 文件由 bin/photo-player 自行维护，脚本只读，不写入。
is_running() {
    if [ -f "$PID_FILE" ]; then
        pid="$(cat "$PID_FILE" 2>/dev/null)"
        if [ -n "$pid" ] && kill -0 "$pid" 2>/dev/null; then
            return 0
        fi
    fi
    return 1
}

check_binary() {
    if [ ! -x "$PLUGIN_DIR/bin/photo-player" ]; then
        echo "Error: photo-player binary not found or not executable"
        echo "Expected: $PLUGIN_DIR/bin/photo-player"
        return 1
    fi
    return 0
}

start_background() {
    if is_running; then
        echo "Photo Player is already running (PID: $(cat "$PID_FILE" 2>/dev/null))"
        echo "Use --stop first."
        return 0
    fi
    if ! check_binary; then
        return 1
    fi
    if ! cd "$PLUGIN_DIR"; then
        echo "Error: cannot cd to $PLUGIN_DIR"
        return 1
    fi
    rm -f "$STOP_FILE"
    if ! mkdir -p "$PLUGIN_DIR/logs"; then
        echo "Warning: cannot create $PLUGIN_DIR/logs (USB log disabled)"
    fi
    # C 侧消费此环境变量把日志双写到 USB 可见位置（重定向语义保持不变）
    PHOTO_PLAYER_LOG_EXTRA="$PLUGIN_DIR/logs/photo-player.log"
    export PHOTO_PLAYER_LOG_EXTRA
    echo "Starting Photo Player..."
    # PID 文件由二进制自己写（脚本写会被它自己的单实例检查读到而误判）
    # setsid：让二进制脱离 KUAL 的会话/进程组，stop framework 等信号不会
    # 连坐杀掉它（t0saki 记录的 KUAL 子进程被信号连坐问题）。
    # PID 语义：非交互 shell 的后台子进程不是进程组组长，BusyBox setsid
    # 此时不 fork、直接 exec，$! 仍等于二进制 PID（已实测确认）；即使
    # setsid fork（异常情形），PID 文件由二进制自写自删，stop/防重复不受影响。
    setsid "$PLAYER_BIN" "$@" >> "$LOG_FILE" 2>&1 &
    player_pid=$!
    echo "Photo Player started (PID: $player_pid, log: $LOG_FILE, USB log: logs/photo-player.log)"
    return 0
}

start_debug() {
    if is_running; then
        echo "Photo Player is already running (PID: $(cat "$PID_FILE" 2>/dev/null))"
        echo "Use --stop first."
        return 0
    fi
    if ! check_binary; then
        return 1
    fi
    if ! cd "$PLUGIN_DIR"; then
        echo "Error: cannot cd to $PLUGIN_DIR"
        return 1
    fi
    rm -f "$STOP_FILE"
    if ! mkdir -p "$PLUGIN_DIR/logs"; then
        echo "Warning: cannot create $PLUGIN_DIR/logs (USB log disabled)"
    fi
    # C 侧消费此环境变量把日志双写到 USB 可见位置（重定向语义保持不变）
    PHOTO_PLAYER_LOG_EXTRA="$PLUGIN_DIR/logs/photo-player.log"
    export PHOTO_PLAYER_LOG_EXTRA
    echo "Starting Photo Player in debug mode (foreground)..."
    # 不加 setsid：前台调试保留终端语义（Ctrl-C/信号直达）。
    # 后台启动再 wait：输出仍走前台，且能拿到退出码；
    # PID 文件由二进制自己写（脚本写会被它自己的单实例检查读到而误判）
    "$PLAYER_BIN" --debug "$@" &
    player_pid=$!
    wait "$player_pid"
    rc=$?
    echo "Photo Player exited (code: $rc)"
    return $rc
}

# 标记驱动恢复：SIGKILL 强杀后二进制自救不可能，由本函数代做系统状态恢复。
# 正常退出时二进制已自删标记，这里是 no-op；也顺带兜住历史残留。
# 语义与 platform.c 对称：恢复成功才删标记，失败保留标记供重试。
restore_system_state() {
    if [ -f "$MARKER_PILLOW" ]; then
        if lipc-set-prop com.lab126.pillow disableEnablePillow enable; then
            rm -f "$MARKER_PILLOW"
            echo "restored pillow overlay after force-kill"
        else
            echo "Warning: cannot re-enable pillow, keeping $MARKER_PILLOW for retry"
        fi
    fi
    if [ -f "$MARKER_FRAMEWORK" ]; then
        if start framework; then
            rm -f "$MARKER_FRAMEWORK"
            echo "restored framework after force-kill"
        else
            echo "Warning: cannot start framework, keeping $MARKER_FRAMEWORK for retry"
        fi
    fi
    return 0
}

stop_player() {
    echo "Stopping Photo Player..."
    touch "$STOP_FILE"
    count=0
    while [ "$count" -lt 5 ]; do
        if ! is_running; then
            echo "Photo Player stopped"
            restore_system_state
            rm -f "$PID_FILE" "$STOP_FILE"
            return 0
        fi
        sleep 1
        count=$((count + 1))
    done
    # 兜底强制结束。不能直接 `pkill -f photo-player`：本脚本和 KUAL 父进程
    # 的命令行里同样含有 "photo-player"，会被连带杀掉，清理代码就跑不到了。
    # 这里用 [p]hoto-player 正则避开 pgrep 自身，并排除自身/父进程。
    for p in $(pgrep -f '[p]hoto-player' 2>/dev/null); do
        if [ "$p" != "$$" ] && [ "$p" != "$PPID" ]; then
            echo "Force stopping PID: $p"
            kill -TERM "$p" 2>/dev/null
        fi
    done
    sleep 1
    for p in $(pgrep -f '[p]hoto-player' 2>/dev/null); do
        if [ "$p" != "$$" ] && [ "$p" != "$PPID" ]; then
            kill -KILL "$p" 2>/dev/null
        fi
    done
    restore_system_state
    rm -f "$PID_FILE" "$STOP_FILE"
    echo "Photo Player force stopped"
    return 0
}

run_selftest() {
    if ! cd "$PLUGIN_DIR"; then
        echo "Error: cannot cd to $PLUGIN_DIR"
        return 1
    fi
    if [ ! -f "./bin/selftest.sh" ]; then
        echo "Error: selftest script not found: $PLUGIN_DIR/bin/selftest.sh"
        return 1
    fi
    # 让自检报告里的 PHOTO_PLAYER_LOG_EXTRA 项显示真实生效值
    PHOTO_PLAYER_LOG_EXTRA="$PLUGIN_DIR/logs/photo-player.log"
    export PHOTO_PLAYER_LOG_EXTRA
    echo "Running selftest..."
    if [ -x "./bin/selftest.sh" ]; then
        ./bin/selftest.sh
    else
        sh ./bin/selftest.sh
    fi
    return $?
}

# 分离控制参数与透传参数：
# 控制参数消费掉，其余按原顺序挪到 "$@" 尾部，最后 "$@" 只剩透传参数。
mode="start"
argc=$#
while [ "$argc" -gt 0 ]; do
    case "$1" in
        --debug)
            mode="debug"
            ;;
        --stop)
            mode="stop"
            ;;
        --selftest)
            mode="selftest"
            ;;
        --help|-h)
            show_help
            exit 0
            ;;
        *)
            set -- "$@" "$1"
            ;;
    esac
    shift
    argc=$((argc - 1))
done

case "$mode" in
    stop)
        stop_player
        exit $?
        ;;
    selftest)
        # 多余的透传参数忽略
        run_selftest
        exit $?
        ;;
    debug)
        start_debug "$@"
        exit $?
        ;;
    *)
        start_background "$@"
        exit $?
        ;;
esac
