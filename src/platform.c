/* platform.c — 平台防干扰实现：fork+execl /bin/sh -c 执行 lipc/stop 命令，逆序幂等恢复 */
#define _POSIX_C_SOURCE 200809L
#include "platform.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "log.h"

/* 状态标记文件：把"本进程改过系统什么"持久化，SIGKILL 强杀后由 photo-player.sh 代为恢复 */
#define MARKER_FRAMEWORK "/tmp/photo-player.framework-stopped"
#define MARKER_PILLOW    "/tmp/photo-player.pillow-disabled"

static int g_prevent_set = 0;       /* 本进程是否设置过 preventScreenSaver */
static int g_framework_stopped = 0; /* 本进程是否 stop 过 framework */
static int g_pillow_disabled = 0;   /* 本进程是否 disable 过 pillow 覆盖层 */

/* fork+execl("/bin/sh","-c",...)，返回 0=命令成功；quiet=1 时命令失败不打 error（调用方自行记 info） */
static int run_cmd(const char* cmd, int quiet)
{
    pid_t pid;
    int status;

    if (cmd == NULL) {
        return -1;
    }
    pid = fork();
    if (pid < 0) {
        log_error("platform: fork failed: %s", strerror(errno));
        return -1;
    }
    if (pid == 0) {
        execl("/bin/sh", "sh", "-c", cmd, (char*) NULL);
        _exit(127); /* execl 失败 */
    }
    for (;;) {
        pid_t r = waitpid(pid, &status, 0);
        if (r == pid) {
            break;
        }
        if (r < 0 && errno == EINTR) {
            continue;
        }
        log_error("platform: waitpid failed: %s", strerror(errno));
        return -1;
    }
    if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
        return 0;
    }
    if (!quiet) {
        log_error("platform: command failed (exit=%d): %s", WIFEXITED(status) ? WEXITSTATUS(status) : -1, cmd);
    }
    return -1;
}

/* 创建状态标记；失败仅记日志忽略（不致命） */
static void marker_create(const char* path)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);

    if (fd < 0) {
        log_info("platform: warning: cannot create marker %s (%s)", path, strerror(errno));
        return;
    }
    (void) close(fd);
}

/* 删除状态标记；ENOENT 视为已删（幂等），其余失败保留标记供外部重试 */
static void marker_remove(const char* path)
{
    if (unlink(path) != 0 && errno != ENOENT) {
        log_info("platform: warning: cannot remove marker %s (%s), kept for external retry", path, strerror(errno));
    }
}

int platform_setup(const AppConfig* cfg)
{
    int ok = 0;

    /* §9 禁屏保（KOReader keepalive 同款）；失败不致命，继续播放 */
    if (run_cmd("lipc-set-prop com.lab126.powerd preventScreenSaver 1", 0) == 0) {
        g_prevent_set = 1;
        log_info("platform: preventScreenSaver=1");
    } else {
        log_error("platform: failed to set preventScreenSaver (continuing)");
        ok = -1;
    }
    if (cfg != NULL && cfg->no_framework) {
        if (run_cmd("stop framework", 0) == 0) {
            g_framework_stopped = 1;
            log_info("platform: framework stopped");
            marker_create(MARKER_FRAMEWORK); /* SIGKILL 兜底恢复的依据 */
        } else {
            log_error("platform: failed to stop framework (continuing)");
            ok = -1;
        }
        /* 追加固化：部分固件的 UI 覆盖层服务（pillow）；属性不存在时命令失败仅记 info 忽略，绝不致命 */
        if (run_cmd("lipc-set-prop com.lab126.pillow disableEnablePillow disable", 1) == 0) {
            g_pillow_disabled = 1;
            log_info("platform: pillow overlay disabled");
            marker_create(MARKER_PILLOW);
        } else {
            log_info("platform: pillow disableEnablePillow unavailable, ignored");
        }
    }
    return ok;
}

void platform_restore(void)
{
    /* §9.1 逆序恢复：pillow → framework → preventScreenSaver；仅恢复本进程改过的项，幂等。
     * 恢复成功才清标志/删标记：失败则两者都保留，重复调用可重试、外部（photo-player.sh）也可重试。 */
    if (g_pillow_disabled) {
        if (run_cmd("lipc-set-prop com.lab126.pillow disableEnablePillow enable", 1) == 0) {
            g_pillow_disabled = 0;
            log_info("platform: pillow overlay enabled");
            marker_remove(MARKER_PILLOW);
        } else {
            log_info("platform: pillow re-enable failed, keeping marker for external retry");
        }
    }
    if (g_framework_stopped) {
        if (run_cmd("start framework", 0) == 0) {
            g_framework_stopped = 0;
            log_info("platform: framework started");
            marker_remove(MARKER_FRAMEWORK);
        } else {
            log_error("platform: failed to start framework, keeping marker for external retry");
        }
    }
    if (g_prevent_set) {
        g_prevent_set = 0;
        if (run_cmd("lipc-set-prop com.lab126.powerd preventScreenSaver 0", 0) == 0) {
            log_info("platform: preventScreenSaver=0");
        } else {
            log_error("platform: failed to reset preventScreenSaver");
        }
    }
}
