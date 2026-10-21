/* Test: a NAS job asked for while another runs stops it and takes over (desktop builds, with the slow test job).
 *     make net_job_test && ./net_job_test
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "../engine/cdi_engine.c"

static int fails;
#define CHECK(c, what) do { if (!(c)) { printf("FAIL: %s\n", what); fails++; } else printf("ok: %s\n", what); } while (0)

static int wait_end(char *msg, size_t n, int ms)
{
    int st = 0;
    for (int t = 0; t < ms / 10 && (st = cdie_net_poll(msg, n, NULL, NULL)) == 0; t++)
        pl_delay(10);
    return st;
}

int main(void)
{
    char msg[300];
    CHECK(cdie_net_start(99, "first", NULL, NULL) == 0, "the first job starts");
    pl_delay(100);
    cdie_net_poll(msg, sizeof msg, NULL, NULL);
    CHECK(strstr(msg, "first") != NULL, "the first job is running");
    CHECK(cdie_net_start(99, "second", NULL, NULL) == 0, "a second job is accepted while the first runs");
    int st = wait_end(msg, sizeof msg, 6000);
    CHECK(st == 1 && strstr(msg, "second: done"), "the second job runs to its end");
    printf("  last: %s\n", msg);

    /* three in a row, quickly: only the last one finishes */
    cdie_net_start(99, "a", NULL, NULL);
    cdie_net_start(99, "b", NULL, NULL);
    cdie_net_start(99, "c", NULL, NULL);
    st = wait_end(msg, sizeof msg, 9000);
    CHECK(st == 1 && strstr(msg, "c: done"), "the last of three quick ones is the one that finishes");

    /* started, then backed out of at once: it stops */
    cdie_net_start(99, "x", NULL, NULL);
    cdie_net_start(99, "y", NULL, NULL);
    cdie_net_cancel();
    st = wait_end(msg, sizeof msg, 6000);
    CHECK(st == 2 && strstr(msg, "y: stopped"), "a job cancelled while it waited stops");
    net_stop();
    printf(fails ? "%d FAILED\n" : "all passed\n", fails);
    return fails != 0;
}
