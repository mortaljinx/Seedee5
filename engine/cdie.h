/*
 * cdie - the CD-i engine: everything ps5-cdi does except drawing.
 *
 * The game list (local folder, USB drives, NAS), cover pictures (found on disk or downloaded), getting a
 * game ready (copying it from the NAS, unpacking a .7z), and running it: the SAME_CDI core on a thread of
 * its own, paced by the clock and the sound, with the pointer, buttons, save states, disc swapping and
 * the per-game settings of the SDL build (host/main.c).
 *
 * What it does not do is draw or open devices. The program around it (the PS5 title, or the desktop test
 * program tools/cdie_sdl.c) shows the list, draws the newest picture, plays the sound it hands out and
 * passes the pad in. It is C, and every function here may be called from the program's main (render)
 * thread unless it says otherwise.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef CDIE_H
#define CDIE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- start and stop ---- */

typedef void (*cdie_log_fn)(const char *line);

typedef struct {
    const char *data_dir; /* where everything lives: "/data/ps5-cdi" on the console (roms/, covers/, saves/, system/...) */
    cdie_log_fn log;      /* every line of the log (NULL: stderr) */
    cdie_log_fn notice;   /* short news worth showing on screen (NULL: only the log) */
    int no_download;      /* 1: do not fetch missing covers from the internet */
    const char *core_path; /* desktop builds: the core's .so (a PS5 build has it linked in) */
} cdie_config;

/* Reads the settings files, finds the games (the NAS is read in the background) and starts the emulator
 * thread. 0 = ready. */
int cdie_init(const cdie_config *cfg);
/* Stops a running game (its NVRAM is written) and every thread. */
void cdie_shutdown(void);

/* ---- the game list ---- */

enum { CDIE_SRC_LOCAL = 0, CDIE_SRC_DRIVE = 1, CDIE_SRC_NAS = 2 };

typedef struct {
    char name[200];  /* what to show: "Burn:Cycle (Europe)" */
    char path[1024]; /* the file (for a NAS game: where its copy goes) */
    char drive[16];  /* "usb0" for a game on a drive, else "" */
    char letter;     /* 'A'..'Z', or '#' for a digit or a sign */
    int source;      /* CDIE_SRC_* */
    int discs;       /* 1, or how many discs */
    int packed;      /* a .7z */
    int here;        /* ready to start at once (not on the NAS, or already copied / unpacked) */
    int has_cover;   /* a picture is known (on disk now) */
} cdie_game;

/* Call once per frame. Merges a finished NAS scan or downloaded covers into the list. Returns a number that
 * changes whenever the list itself changed (games added or removed, the order changed): the indexes of
 * every other call refer to the list of that number. */
unsigned cdie_lib_update(void);
int cdie_lib_count(void);
int cdie_lib_game(int index, cdie_game *out); /* 0 = ok */
int cdie_lib_find(const char *path);          /* index of a game by its path, or -1 */
int cdie_lib_last(void);                      /* the game played last, or 0 */
void cdie_lib_rescan(void);                   /* look again (a drive was plugged in), the NAS too */
/* What the NAS and the cover downloads are doing, one line each ("NAS: 2020 game file(s) found",
 * "Downloading covers  12 / 340"); empty when there is nothing to say. Returns the NAS state: 0 none set up,
 * 1 connecting, 2 connected, 3 failed. */
int cdie_lib_status(char *nas, size_t nas_n, char *covers, size_t covers_n);
void cdie_covers_retry(void); /* try the downloads again (after a DNS change) */

/* Cover pictures are read and scaled (to at most `max_side` pixels, 512 by default) on a thread of their
 * own, the ones nearest `centre` first. Tell it which games are in view (centre and how many on each side
 * are wanted), then take the finished pictures: RGBA, top row first, freed with cdie_free(). A game whose
 * picture cannot be read never comes back. */
void cdie_cover_window(int centre, int radius, int max_side);
/* The covers wanted, by game index, the most wanted first (a filtered wheel: its neighbours are not next to each
 * other in the list). Takes the place of cdie_cover_window until that is called again. */
void cdie_cover_wanted(const int *idx, int n, int max_side);
int cdie_cover_take(int *index, int *w, int *h, uint8_t **rgba); /* 1 = one taken */
void cdie_cover_forget(int index); /* the program let go of a picture it took: read it again when it is in view */
void cdie_free(void *p);
/* ---- a game card's extras: its screenshot (the backdrop), its logo, and what Wikipedia says about it ----
 * Asked for when a card opens; fetched on a thread of their own and kept in <covers>/extras/. */
typedef struct {
    int index;           /* the game (the engine's list) */
    int state;           /* 1 looked up (about may still be ""), 2 the network did not answer */
    char about[1200];    /* the article's lead section, plain text, or "" */
    char reviews[300];   /* "AllGame 3/5 · Edge 6/10", or "" */
    char source[300];    /* the credit to show with them */
} cdie_extras_info;
/* The card of game `index` opened (-1: closed). title: the game's name as the list of CD-i games spells it (the
 * article's name, mostly), or NULL. */
/* A note for the user when the app folder was found replaced (an update by a store) and the safe copy of the
 * settings and saved games was put back, else "" */
const char *cdie_keep_note(void);

void cdie_extras_want(int index, const char *title);
/* 1: out holds what is known for game `index` */
int cdie_extras_info_get(int index, cdie_extras_info *out);
/* A picture ready: kind 0 the screenshot (backdrop), 1 the logo; RGBA, top row first, freed with cdie_free() */
int cdie_extras_take(int *index, int *kind, int *w, int *h, uint8_t **rgba);

/* PNG or JPEG bytes the program carries (its logo) to RGBA, top row first; NULL if unreadable. Freed with cdie_free(). */
uint8_t *cdie_decode_image(const void *data, int len, int *w, int *h);

/* ---- getting a game ready and starting it ---- */

typedef struct {
    int state;           /* -1 nothing going on, 0 working, 1 ready, 2 failed, 3 cancelled */
    char title[80];      /* "Copying from the NAS", "Unpacking" */
    char phase[200];     /* the file being copied */
    char why[300];       /* what went wrong (state 2) */
    uint64_t done, total, resumed;
    double mbps, eta_s;  /* speed and time left, once known (0 before) */
} cdie_job;

/* Starts making game `index` playable (copy from the NAS, unpack). A game that is ready already becomes
 * ready at once. Poll with cdie_prepare_poll until state is not 0. */
int cdie_prepare(int index);
int cdie_prepare_poll(cdie_job *out);
void cdie_prepare_cancel(void);

/* Starts the prepared game on the emulator thread. Waits until the core has loaded it (a second or two).
 * 0 = running; -1 = failed, cdie_last_error() says why. */
int cdie_start(void);
/* Starts a file directly (tests, or a game that is not in the list). */
int cdie_start_file(const char *path);
const char *cdie_last_error(void);
/* Back to the list: unloads the game (its NVRAM is written). */
void cdie_stop(void);
int cdie_running(void);
/* The fast start: just after cdie_start, the CD-i player's own start (its blue screen, reading the disc) runs as
 * fast as it can, unseen and unheard, until the game has its own picture. 1 while it runs: show a "starting"
 * card instead of the picture. cdie_boot_skip shows the game at once. options.cfg: host_fast_start=0 (off),
 * host_fast_start_max=<seconds of CD-i time, 30>, host_fast_start_settle=<frames, 40>. */
/* Continue: the game's automatic save, made when it was last left after playing for 10 s or more.
 * cdie_continue_info: 1 if game idx has one (when: "11 Oct 22:40"). cdie_continue_next(1) before cdie_start loads it
 * instead of starting the game. The "Play CD-i" click is learned the first time it is made and made by itself
 * after that (options.cfg host_auto_play=0: off). */
/* The CD-i buttons on Cross, Circle, Square, Triangle for every game (options.cfg): 1 = button 1, 2 = button 2,
 * 3 = both, 0 = nothing. Used from the next game started. */
void cdie_buttons_get(int map[4]);
int cdie_buttons_set(const int map[4]);
/* Time played with game idx (seconds) and when it was last played (Unix time); 0 if never. */
int cdie_play_info(int idx, long *seconds, long *last);
int cdie_continue_info(int idx, char *when, size_t n);
void cdie_continue_next(int on);
int cdie_booting(void);
/* Rewind (the in-game menu's "Rewind" setting, saves/rewind.cfg): while held is 1, the game goes back a step at a
 * time (0.2 s, up to 30 s). cdie_rewinding: 1 while it does, for a mark on the screen. */
void cdie_rewind(int held);
int cdie_rewinding(void);
void cdie_boot_skip(void);
const char *cdie_game_name(void);

/* ---- while a game runs ---- */

/* The pad. Buttons are the PS5 ones by position; sticks -32768..32767 (down and right positive),
 * triggers 0..255. mouse_* are for a desktop mouse (counts since the last call). */
enum {
    CDIE_PAD_CROSS = 1u << 0, CDIE_PAD_CIRCLE = 1u << 1, CDIE_PAD_SQUARE = 1u << 2, CDIE_PAD_TRIANGLE = 1u << 3,
    CDIE_PAD_L1 = 1u << 4, CDIE_PAD_R1 = 1u << 5, CDIE_PAD_L2 = 1u << 6, CDIE_PAD_R2 = 1u << 7,
    CDIE_PAD_L3 = 1u << 8, CDIE_PAD_R3 = 1u << 9, CDIE_PAD_OPTIONS = 1u << 10, CDIE_PAD_TOUCH = 1u << 11,
    CDIE_PAD_UP = 1u << 12, CDIE_PAD_DOWN = 1u << 13, CDIE_PAD_LEFT = 1u << 14, CDIE_PAD_RIGHT = 1u << 15,
};
typedef struct {
    uint32_t buttons;
    int16_t lx, ly, rx, ry;
    uint8_t l2, r2;
    int mouse_dx, mouse_dy, mouse_buttons; /* bit 0 left = CD-i button 1, bit 1 right = button 2 */
} cdie_pad;
void cdie_input(const cdie_pad *pad);

/* Pauses the game (the program's menu is open): the emulator stops and the sound goes quiet. When it goes
 * on again, the pad is ignored until every button is let go (the press that closed the menu must not
 * reach the game). */
void cdie_pause(int on);
int cdie_paused(void);

/* Sound for the console's output: 48000 Hz, stereo, interleaved. Called from the audio thread; always fills
 * `frames` (silence when there is nothing). */
void cdie_audio(int16_t *out, int frames);

/* The pictures. The emulator keeps its two newest frames (RGBA, top row first). Between lock and unlock
 * they do not change; copy them out and unlock again quickly. `serial` goes up by one with every frame
 * made, so a frame already copied can be recognised. Returns how many frames there are (0 to 2). */
typedef struct {
    const uint8_t *rgba;
    int w, h, pitch; /* pitch in bytes */
    double t;        /* when it was made (cdie_now_ms) */
    unsigned serial;
} cdie_image;
int cdie_video_lock(cdie_image *newest, cdie_image *previous);
void cdie_video_unlock(void);

/* Smooth video (a PAL game at 50 frames a second on a 60 Hz screen): call once per screen refresh. Returns how
 * much of the newest frame to show over the previous one, 0..1 (draw the previous frame, then the newest with
 * this opacity). 1 when smoothing is off or the rates match. */
float cdie_video_blend(double now_ms);

/* Which part of a fw x fh frame to show (the crop), and whether to fill a 16:9 screen (wide) or show a 4:3
 * box in the middle. */
typedef struct {
    int x, y, w, h;
    int wide;
} cdie_view;
void cdie_video_view(int fw, int fh, cdie_view *v);

double cdie_now_ms(void); /* the clock the frame times are on */

/* ---- the in-game menu ---- */

int cdie_slot_info(int slot, char *out, size_t n);         /* 1 = used ("saved 12 Oct 14:20"), 0 = empty */
int cdie_slot_save(int slot, char *msg, size_t n);         /* slots 1..4; 0 = ok; msg says what happened */
int cdie_slot_load(int slot, char *msg, size_t n);
int cdie_disc_count(void);                                 /* 1 for most games */
int cdie_disc_current(void);                               /* 0-based */
int cdie_disc_insert(int disc, char *msg, size_t n);
void cdie_reset(void);
void cdie_fit_crop(void);                                  /* crop to the picture on screen now */

/* Settings shown in the menu, changed with left / right. Saved as they change (per game where it says so). */
typedef struct {
    const char *label;          /* "Pointer speed" */
    int value, min, max, step;
    const char *const *names;   /* value names for a choice (value 0..max), or NULL for a number */
    const char *unit;           /* "%" or " ms" for a number */
} cdie_setting;
int cdie_setting_count(void);
int cdie_setting_get(int i, cdie_setting *out);
void cdie_setting_set(int i, int value);

/* One line of numbers: frames per second, time per frame, sound. */
void cdie_stats(char *out, size_t n);

/* ---- the settings screen ---- */

typedef struct {
    int bios;          /* the CD-i BIOS (cdimono1.zip, or its ROMs in a cdimono1 folder) is there */
    int dvc;           /* ... with vmpega.rom: digital video (FMV) games work */
    int nfs;           /* this build can use a NAS */
    char folder[600];  /* where the BIOS goes */
    char roms[600];    /* where games on the console go */
    char data[600];    /* the data folder */
} cdie_bios;
void cdie_bios_status(cdie_bios *out);

typedef struct {
    int enabled;
    char server[128];  /* an IPv4 address */
    char share[256];   /* the NFS export, as the NAS lists it */
    char path[512];    /* the folder inside it with the games ("" = the whole share) */
    int version;       /* 0 = try NFS 3 then 4; 3; 4 */
    int reserved;      /* connect from a reserved source port (some NAS insist; then the share needs no "insecure") */
    int cache_gb;      /* room for games copied to the console */
    int backup;        /* after each game, copy the saves folder to <share>/cdi-wheel-saves */
} cdie_nas_cfg;
/* Saves on the NAS: 0 copies the saves folder there (files newer than the share's), 1 brings back the share's files
 * that are newer than the console's. -1 when one is already running or there is no NAS. */
int cdie_backup_start(int restore);
/* -1 idle, 0 working, 1 done, 2 failed; msg says what happened */
int cdie_backup_poll(char *msg, size_t n);
void cdie_nas_get(cdie_nas_cfg *out);
/* Saves nfs.cfg and reads the NAS again with the new settings (the list follows when the scan ends). */
int cdie_nas_set(const cdie_nas_cfg *cfg);

int cdie_covers_on(void);
void cdie_covers_set(int on);
int cdie_local_ip(char *out, size_t n); /* this console's address on the network ("10.0.3.21"), or -1 */
void cdie_dns_get(char *out, size_t n); /* the DNS server for cover downloads, or "" for automatic */
void cdie_dns_set(const char *ip);      /* "" = automatic */

/* Network jobs, one at a time, on a thread of their own: poll until the state is not 0, then read the items. */
enum { CDIE_NET_FIND, CDIE_NET_SHARES, CDIE_NET_FOLDER, CDIE_NET_BIOS_FIND, CDIE_NET_BIOS_COPY };
enum { CDIE_NET_SERVER, CDIE_NET_SHARE, CDIE_NET_FOLDER_ITEM, CDIE_NET_FILE };
typedef struct {
    char name[256];  /* what to show: an address, a share, a folder's or file's name */
    char path[700];  /* what to hand back: the address, the share, the folder's path inside the share */
    int kind;        /* CDIE_NET_SERVER, CDIE_NET_SHARE, CDIE_NET_FOLDER_ITEM, CDIE_NET_FILE */
    int count;       /* a server's shares (-1 if it would not list them); a file's size in KB */
} cdie_net_item;
/* FIND: NFS servers on this console's network. SHARES: ip's shares. FOLDER: the folders in share/path, and how many
 * games are in it (cdie_net_poll's done). BIOS_FIND: cdimono1.zip anywhere under share/path. BIOS_COPY: copy
 * share/path (a cdimono1.zip) into the BIOS folder. */
int cdie_net_start(int what, const char *ip, const char *share, const char *path);
int cdie_net_poll(char *msg, size_t n, uint64_t *done, uint64_t *total); /* -1 idle, 0 working, 1 done, 2 failed */
int cdie_net_items(cdie_net_item *out, int max);
void cdie_net_cancel(void);

#ifdef __cplusplus
}
#endif
#endif
