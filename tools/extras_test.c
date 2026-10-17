/* End-to-end test of a game card's extras (engine/cdi_engine.c, "a game card's extras"): one game in the list, its
 * card opened, then what comes back: the about text and reviews, the screenshot and the logo, and the cache.
 *
 *     make extras_test && CDIHOST_TEST_URLMAP='https://en.wikipedia.org/|https://127.0.0.1:8443/' ./extras_test <dir>
 *
 * The pictures come from GitHub (libretro-thumbnails) and are checked against host/cdi_extras.h; Wikipedia can be
 * a local stand-in (tools/fake_wikipedia.py) through the URL map.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "../engine/cdi_engine.c"

static int wait_for(int idx, int want_pics, cdie_extras_info *info, int *pics, int secs)
{
    int got_info = 0;
    for (int t = 0; t < secs * 20; t++) {
        if (!got_info && cdie_extras_info_get(idx, info))
            got_info = 1;
        int i, k, w, h;
        uint8_t *rgba;
        while (cdie_extras_take(&i, &k, &w, &h, &rgba)) {
            printf("  picture: game %d, %s, %dx%d\n", i, k ? "logo" : "screenshot", w, h);
            pics[k]++;
            cdie_free(rgba);
        }
        if (got_info && pics[0] + pics[1] >= want_pics)
            return 1;
        usleep(50000);
    }
    return got_info;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: %s <empty dir for the cache>\n", argv[0]);
        return 2;
    }
    snprintf(g_covers_dir, sizeof g_covers_dir, "%s", argv[1]);
    g_roms = calloc(2, sizeof *g_roms);
    g_nroms = 2;
    snprintf(g_roms[0].base, sizeof g_roms[0].base, "Mad Dog McCree (Europe) (Alt)");
    snprintf(g_roms[0].name, sizeof g_roms[0].name, "Mad Dog McCree");
    snprintf(g_roms[1].base, sizeof g_roms[1].base, "Burn Cycle (Europe)");
    snprintf(g_roms[1].name, sizeof g_roms[1].name, "Burn Cycle");
    int fails = 0;

    for (int pass = 0; pass < 2; pass++) {
        printf("pass %d (%s)\n", pass + 1, pass ? "from the cache" : "from the network");
        const struct timeval t0 = {0};
        (void)t0;
        struct timeval a, b;
        gettimeofday(&a, NULL);
        cdie_extras_info info;
        int pics[2] = {0, 0};
        cdie_extras_want(0, "Mad Dog McCree");
        const int ok = wait_for(0, 2, &info, pics, 60);
        gettimeofday(&b, NULL);
        printf("  %.1f s; info %s, state %d\n  about: %.160s...\n  reviews: %s\n  source: %s\n",
               (double)(b.tv_sec - a.tv_sec) + (double)(b.tv_usec - a.tv_usec) / 1e6, ok ? "yes" : "NO", info.state,
               info.about, info.reviews, info.source);
        if (!ok || !info.about[0] || !pics[0] || !pics[1]) {
            printf("  FAIL: expected about text, a screenshot and a logo\n");
            fails++;
        }
        /* the second game: the list's title is not the article's; the search finds it */
        int pics2[2] = {0, 0};
        cdie_extras_want(1, "Burn");
        const int ok2 = wait_for(1, 1, &info, pics2, 60);
        printf("  game 2: info %s; about: %.100s...; reviews: %s\n", ok2 ? "yes" : "NO", info.about, info.reviews);
        if (!ok2 || !strstr(info.about, "Burn:Cycle")) {
            printf("  FAIL: expected Burn:Cycle's article by the search\n");
            fails++;
        }
        cdie_extras_want(-1, NULL);
    }
    /* switching quickly: only the last wish's pictures come back */
    {
        printf("pass 3 (switching between cards)\n");
        cdie_extras_want(0, "Mad Dog McCree");
        cdie_extras_want(1, "Burn");
        cdie_extras_want(0, "Mad Dog McCree");
        cdie_extras_want(1, "Burn");
        int pics[2] = {0, 0}, wrong = 0;
        for (int t = 0; t < 100; t++) {
            int i, k, w, h;
            uint8_t *rgba;
            while (cdie_extras_take(&i, &k, &w, &h, &rgba)) {
                if (i != 1)
                    wrong++;
                pics[k]++;
                cdie_free(rgba);
            }
            usleep(50000);
        }
        printf("  pictures for the last card: %d, for another: %d\n", pics[0] + pics[1], wrong);
        if (wrong) {
            printf("  FAIL: a closed card's picture came back\n");
            fails++;
        }
    }
    ex_stop();
    printf(fails ? "%d FAILED\n" : "all passed\n", fails);
    return fails != 0;
}
