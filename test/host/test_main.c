/* test_main.c — 宿主机单元测试极简 runner：ASSERT 宏逐条 [PASS]/[FAIL]，失败总数作退出码 */
#include <stdio.h>

#include "log.h"
#include "util.h"

static int g_failures = 0;

#define ASSERT(cond)                                                            \
    do {                                                                        \
        if (cond) {                                                             \
            printf("[PASS] %s:%d %s\n", __FILE__, __LINE__, #cond);             \
        } else {                                                                \
            printf("[FAIL] %s:%d %s\n", __FILE__, __LINE__, #cond);             \
            g_failures++;                                                       \
        }                                                                       \
    } while (0)

int test_config(void);
int test_source_local(void);
int test_player(void);

static void test_util_smoke(void)
{
    long t1 = util_now_ms();
    long t2;

    ASSERT(t1 >= 0);
    util_sleep_ms(30);
    t2 = util_now_ms();
    ASSERT(t2 >= t1);
    ASSERT(t2 - t1 >= 10); /* 睡了 30ms，保守断言 */

    log_set_debug(1);
    log_debug("host-test smoke: debug log ok");
    log_set_debug(0);
    log_info("host-test smoke: info log ok");
}

int main(void)
{
    int fails = 0;

    printf("== util/log smoke ==\n");
    test_util_smoke();
    fails += g_failures;

    printf("== test_config ==\n");
    fails += test_config();

    printf("== test_source_local ==\n");
    fails += test_source_local();

    printf("== test_player ==\n");
    fails += test_player();

    printf("TOTAL FAILURES: %d\n", fails);
    return fails;
}
