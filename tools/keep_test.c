/* Test of the safe copy (engine/cdi_engine.c, "a safe copy of what matters"): a data folder is made, copied, then
 * replaced by an empty one as a store's update does, and what comes back is checked.
 *     make keep_test && ./keep_test <empty dir>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <utime.h>
#include "../engine/cdi_engine.c"

static int fails;
static void put(const char *root, const char *rel, const char *text)
{
    char p[1200], d[1200];
    snprintf(p, sizeof p, "%s/%s", root, rel);
    snprintf(d, sizeof d, "%s", p);
    *strrchr(d, '/') = 0;
    mkdirs(d);
    FILE *f = fopen(p, "w");
    fputs(text, f);
    fclose(f);
}
static int has(const char *root, const char *rel, const char *text)
{
    char p[1200], b[256] = "";
    snprintf(p, sizeof p, "%s/%s", root, rel);
    FILE *f = fopen(p, "r");
    if (!f)
        return 0;
    size_t n = fread(b, 1, sizeof b - 1, f);
    b[n] = 0;
    fclose(f);
    return !text || !strcmp(b, text);
}
#define CHECK(c, what) do { if (!(c)) { printf("FAIL: %s\n", what); fails++; } else printf("ok: %s\n", what); } while (0)

int main(int argc, char **argv)
{
    if (argc < 2)
        return 2;
    char data[600], keep[600], cmd[1400];
    snprintf(data, sizeof data, "%s/data", argv[1]);
    snprintf(keep, sizeof keep, "%s/keep", argv[1]);
    snprintf(cmd, sizeof cmd, "rm -rf '%s' '%s'", data, keep);
    if (system(cmd) != 0) return 2;
    setenv("CDIE_KEEP_DIR", keep, 1);
    snprintf(g_data_dir, sizeof g_data_dir, "%s", data);
    snprintf(g_roms_dir, sizeof g_roms_dir, "%s/roms", data);

    /* a folder in use */
    put(data, "nfs.cfg", "server=nas\n");
    put(data, "options.cfg", "a=1\n");
    put(data, "READ ME.txt", "readme");
    put(data, "saves/favourites.txt", "Burn Cycle\n");
    put(data, "saves/same_cdi/nvram/Game/cdimono1/mk48t08", "nvram");
    put(data, "saves/states/Game.slot0.state", "big state");
    put(data, "saves/title_log.txt", "log");
    put(data, "system/same_cdi/bios/cdimono1.zip", "bios");
    put(data, "roms/Game (Europe).chd", "game");
    put(data, "covers/Game.png", "png");
    mkdirs(data);
    keep_restore(); /* first start: nothing to put back; the marker is made */
    keep_backup();
    CHECK(has(data, KEEP_MARKER, NULL), "the data folder gets its marker");
    CHECK(strstr(g_keep_note, "1.21 or older") != NULL, "a first start says where an older version's data went");
    g_keep_note[0] = 0;
    CHECK(has(keep, "nfs.cfg", "server=nas\n") && has(keep, "saves/favourites.txt", NULL), "settings are copied");
    CHECK(has(keep, "saves/same_cdi/nvram/Game/cdimono1/mk48t08", "nvram"), "saved games are copied");
    CHECK(has(keep, "system/same_cdi/bios/cdimono1.zip", "bios"), "the BIOS is copied");
    CHECK(!has(keep, "saves/states/Game.slot0.state", NULL) && !has(keep, "roms/Game (Europe).chd", NULL) &&
          !has(keep, "covers/Game.png", NULL) && !has(keep, "saves/title_log.txt", NULL) && !has(keep, "READ ME.txt", NULL),
          "states, games, covers, logs and the read-me are not");
    CHECK(g_keep_note[0] == 0, "no note on a normal start");

    /* a setting changes; the next copy has it */
    put(data, "options.cfg", "a=2\n");
    struct utimbuf later = {time(NULL) + 5, time(NULL) + 5};
    char p[1200];
    snprintf(p, sizeof p, "%s/options.cfg", data);
    utime(p, &later);
    keep_backup();
    CHECK(has(keep, "options.cfg", "a=2\n"), "a changed setting is copied again");

    /* a normal start (marker there): nothing is put back, even what the user deleted */
    snprintf(p, sizeof p, "%s/nfs.cfg", data);
    remove(p);
    keep_restore();
    CHECK(!has(data, "nfs.cfg", NULL), "a file deleted by the user stays deleted");
    put(data, "nfs.cfg", "server=nas\n");
    keep_backup();

    /* the store replaces the app folder: an empty data folder */
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", data);
    if (system(cmd) != 0) return 2;
    mkdirs(data);
    g_keep_restored = 0;
    keep_restore();
    CHECK(has(data, "nfs.cfg", "server=nas\n") && has(data, "options.cfg", "a=2\n"), "settings come back");
    CHECK(has(data, "saves/same_cdi/nvram/Game/cdimono1/mk48t08", "nvram"), "saved games come back");
    CHECK(has(data, "system/same_cdi/bios/cdimono1.zip", "bios"), "the BIOS comes back");
    CHECK(has(data, KEEP_MARKER, NULL), "the new folder gets its marker");
    CHECK(strstr(g_keep_note, "prosperostore/previous") != NULL, "the note says where the games went");
    printf("  note: %s\n  restored: %d\n", g_keep_note, g_keep_restored);

    /* the next start is a normal one */
    g_keep_note[0] = 0;
    keep_restore();
    CHECK(g_keep_note[0] == 0, "no note on the start after");
    printf(fails ? "%d FAILED\n" : "all passed\n", fails);
    return fails != 0;
}
