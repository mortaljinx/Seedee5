/*
 * cdi_engine.c - the CD-i engine (see cdie.h): everything ps5-cdi does except drawing.
 *
 * Most of this is the SDL build's host/main.c, moved here without its window, sound device and pad code:
 * the game list, covers, NAS, 7z, the libretro glue, pointer and buttons, save states and discs. What
 * the SDL build did in its main loop (pacing the emulator by the clock and the sound) runs on a thread of
 * its own here; the program around it draws the frames this hands out and plays the sound it pulls.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#define _GNU_SOURCE
#ifndef STATIC_CORE
#include <dlfcn.h>
#endif
#include <dirent.h>
#ifndef _WIN32
#include <sys/statvfs.h>
#define HAVE_STATVFS 1
#endif
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>

#include "cdie.h"
#include "libretro.h"
#include "cdi_boxarts.h"
#include "tlsget.h"
#include "arc7z.h"

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#pragma GCC diagnostic ignored "-Wsign-compare"
#define STB_IMAGE_STATIC /* the title links other copies of stb_image: keep this one to ourselves */
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_ONLY_BMP
#define STBI_MAX_DIMENSIONS 8192
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#include "stb_image.h"
#pragma GCC diagnostic pop

/* ------------------------------------------------------------- threads and time (pthreads) */

typedef struct pl_mutex { pthread_mutex_t m; } pl_mutex;
typedef struct pl_cond { pthread_cond_t c; } pl_cond;
typedef struct pl_thread { pthread_t t; int (*fn)(void *); void *arg; } pl_thread;

static pl_mutex *pl_mutex_create(void)
{
    pl_mutex *m = calloc(1, sizeof *m);
    if (m)
        pthread_mutex_init(&m->m, NULL);
    return m;
}
static void pl_mutex_destroy(pl_mutex *m)
{
    if (m) {
        pthread_mutex_destroy(&m->m);
        free(m);
    }
}
static void pl_lock(pl_mutex *m) { if (m) pthread_mutex_lock(&m->m); }
static void pl_unlock(pl_mutex *m) { if (m) pthread_mutex_unlock(&m->m); }

static pl_cond *pl_cond_create(void)
{
    pl_cond *c = calloc(1, sizeof *c);
    if (c)
        pthread_cond_init(&c->c, NULL);
    return c;
}
static void pl_cond_destroy(pl_cond *c)
{
    if (c) {
        pthread_cond_destroy(&c->c);
        free(c);
    }
}
static void pl_cond_wait(pl_cond *c, pl_mutex *m) { pthread_cond_wait(&c->c, &m->m); }
static void pl_cond_wait_ms(pl_cond *c, pl_mutex *m, int ms)
{
    struct timeval now;
    struct timespec until;
    gettimeofday(&now, NULL);
    long ns = (long)now.tv_usec * 1000L + (long)(ms % 1000) * 1000000L;
    until.tv_sec = now.tv_sec + ms / 1000 + ns / 1000000000L;
    until.tv_nsec = ns % 1000000000L;
    pthread_cond_timedwait(&c->c, &m->m, &until);
}
static void pl_cond_signal(pl_cond *c) { if (c) pthread_cond_signal(&c->c); }
static void pl_cond_broadcast(pl_cond *c) { if (c) pthread_cond_broadcast(&c->c); }

static void *pl_thread_main(void *p)
{
    pl_thread *t = p;
    t->fn(t->arg);
    return NULL;
}
/* The core (MAME) wants a big stack, and so does the 7z decoder: give every thread 4 MB. */
static pl_thread *pl_thread_create(int (*fn)(void *), const char *name, void *arg)
{
    (void)name;
    pl_thread *t = calloc(1, sizeof *t);
    if (!t)
        return NULL;
    t->fn = fn;
    t->arg = arg;
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setstacksize(&at, 4u << 20);
    int r = pthread_create(&t->t, &at, pl_thread_main, t);
    pthread_attr_destroy(&at);
    if (r != 0) {
        free(t);
        return NULL;
    }
    return t;
}
static void pl_thread_wait(pl_thread *t)
{
    if (t) {
        pthread_join(t->t, NULL);
        free(t);
    }
}
static void pl_thread_detach(pl_thread *t)
{
    if (t) {
        pthread_detach(t->t);
        /* the small record is kept: the thread still reads fn and arg from it */
    }
}

static double pc_ms(void) /* a monotonic clock in milliseconds */
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}
static uint32_t pl_ticks(void) { return (uint32_t)(uint64_t)pc_ms(); }
static void pl_delay(uint32_t ms)
{
    struct timespec ts = {(time_t)(ms / 1000), (long)(ms % 1000) * 1000000L};
    nanosleep(&ts, NULL);
}

/* ------------------------------------------------------------- the log */

static cdie_log_fn g_log_fn, g_notice_fn;
static pl_mutex *g_log_mtx;

static FILE *g_log_file; /* <saves>/cdi_log.txt: the engine's own copy of its log, written as it goes */
static void elog_line(const char *line)
{
    pl_lock(g_log_mtx);
    if (g_log_file) {
        fprintf(g_log_file, "%s\n", line);
        fflush(g_log_file);
    }
    if (g_log_fn)
        g_log_fn(line);
    else
        fprintf(stderr, "%s\n", line);
    pl_unlock(g_log_mtx);
}

/* "[host] ..." lines of the SDL build went to stderr: here they go to the program's log */
static void elogf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void setting_path(char *out, size_t n, const char *dir, const char *name); /* (settings files: below) */
static void elogf(const char *fmt, ...)
{
    char line[1200];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    size_t n = strlen(line);
    while (n && line[n - 1] == '\n')
        line[--n] = 0;
    elog_line(line);
}

/* News for the screen (the SDL build's on-screen notifications): logged, and handed to the program. */
static void notify(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void notify(const char *fmt, ...)
{
    char line[1200];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    const char *shown = !strncmp(line, "cdihost: ", 9) ? line + 9 : line;
    elogf("[notify] %s", shown);
    if (g_notice_fn)
        g_notice_fn(shown);
}

/* fprintf(stderr, ...) in the code taken from main.c */
#define ERRF(...) elogf(__VA_ARGS__)

/* ---------------------------------------------------------------- core */
static struct {
    void *handle;
    void (*retro_init)(void);
    void (*retro_deinit)(void);
    unsigned (*retro_api_version)(void);
    void (*retro_get_system_info)(struct retro_system_info *);
    void (*retro_get_system_av_info)(struct retro_system_av_info *);
    void (*retro_set_environment)(retro_environment_t);
    void (*retro_set_video_refresh)(retro_video_refresh_t);
    void (*retro_set_audio_sample)(retro_audio_sample_t);
    void (*retro_set_audio_sample_batch)(retro_audio_sample_batch_t);
    void (*retro_set_input_poll)(retro_input_poll_t);
    void (*retro_set_input_state)(retro_input_state_t);
    void (*retro_set_controller_port_device)(unsigned, unsigned);
    void (*retro_reset)(void);
    void (*retro_run)(void);
    size_t (*retro_serialize_size)(void);
    bool (*retro_serialize)(void *, size_t);
    bool (*retro_unserialize)(const void *, size_t);
    bool (*retro_load_game)(const struct retro_game_info *);
    void (*retro_unload_game)(void);
    void (*retro_flush_nvram)(void); /* optional: added by patches/0001-flush-nvram.patch */
    unsigned long *(*retro_cdi_audio_stats)(void); /* optional: patches/0004-cdi-audio-stats.patch */
} core;

#ifdef STATIC_CORE
/* PS5 build: the core is linked into the ELF, no dlopen. */
extern void retro_flush_nvram(void) __attribute__((weak));
extern unsigned long *retro_cdi_audio_stats(void) __attribute__((weak));
static bool core_open(const char *path)
{
    (void)path;
#define BIND(sym) core.sym = sym
    BIND(retro_init);
    BIND(retro_deinit);
    BIND(retro_api_version);
    BIND(retro_get_system_info);
    BIND(retro_get_system_av_info);
    BIND(retro_set_environment);
    BIND(retro_set_video_refresh);
    BIND(retro_set_audio_sample);
    BIND(retro_set_audio_sample_batch);
    BIND(retro_set_input_poll);
    BIND(retro_set_input_state);
    BIND(retro_set_controller_port_device);
    BIND(retro_reset);
    BIND(retro_run);
    BIND(retro_serialize_size);
    BIND(retro_serialize);
    BIND(retro_unserialize);
    BIND(retro_load_game);
    BIND(retro_unload_game);
#undef BIND
    core.retro_flush_nvram = retro_flush_nvram; /* NULL if the core was built without the patch */
    core.retro_cdi_audio_stats = retro_cdi_audio_stats; /* likewise */
    return true;
}
#else
#define LOAD(sym)                                                          \
    do {                                                                   \
        *(void **)&core.sym = dlsym(core.handle, #sym);                    \
        if (!core.sym) {                                                   \
            elogf("core is missing symbol %s\n", #sym);          \
            return false;                                                  \
        }                                                                  \
    } while (0)

static bool core_open(const char *path)
{
    core.handle = dlopen(path, RTLD_LAZY | RTLD_LOCAL);
    if (!core.handle) {
        elogf("dlopen failed: %s\n", dlerror());
        return false;
    }
    LOAD(retro_init);
    LOAD(retro_deinit);
    LOAD(retro_api_version);
    LOAD(retro_get_system_info);
    LOAD(retro_get_system_av_info);
    LOAD(retro_set_environment);
    LOAD(retro_set_video_refresh);
    LOAD(retro_set_audio_sample);
    LOAD(retro_set_audio_sample_batch);
    LOAD(retro_set_input_poll);
    LOAD(retro_set_input_state);
    LOAD(retro_set_controller_port_device);
    LOAD(retro_reset);
    LOAD(retro_run);
    LOAD(retro_serialize_size);
    LOAD(retro_serialize);
    LOAD(retro_unserialize);
    LOAD(retro_load_game);
    LOAD(retro_unload_game);
    *(void **)&core.retro_flush_nvram = dlsym(core.handle, "retro_flush_nvram"); /* optional */
    *(void **)&core.retro_cdi_audio_stats = dlsym(core.handle, "retro_cdi_audio_stats"); /* optional */
    return true;
}
#endif /* STATIC_CORE */

/* -------------------------------------------------------------- globals */

static char g_core_path[1024] = "(linked in)";
static char g_data_dir[1024] = ".";
static char g_system_dir[1024] = "system";
static char g_save_dir[1024] = "saves";
static char g_roms_dir[1024] = "roms";
static char g_options_dir[1024] = ".";
#define DEFAULT_ROMS_DIR g_roms_dir
#define DEFAULT_OPTIONS_DIR g_options_dir
#define DEFAULT_COVERS_DIR "covers" /* until cdie_init sets them under the data folder */
#define DEFAULT_CACHE_DIR "cache"

static enum retro_pixel_format g_fmt = RETRO_PIXEL_FORMAT_0RGB1555;
static float g_aspect;
static struct retro_system_av_info g_av;
static bool g_debug;
static volatile sig_atomic_t g_signal; /* never set here: the program stops the engine (the copied code still reads it) */
static volatile int g_view_mode;      /* 0 = 4:3, 1 = 16:9 (fill the screen) */

/* The CD-i frame carries dark margins of its own, and they differ from screen
 * to screen, so there is no single crop that fits everything. Crops are in
 * tenths of a percent of the frame: left, right, top, bottom.
 *   crop mode 0 = off (whole frame)
 *   crop mode 1 = manual: a fixed crop for each aspect (4:3 and 16:9)
 *   crop mode 2 = auto: follows the picture on screen */
enum { CROP_OFF = 0, CROP_MANUAL = 1, CROP_AUTO = 2 };
static volatile int g_crop[2][4] = {
    {0, 0, 75, 185},
    {30, 30, 75, 135},
};
static volatile int g_auto[4];            /* the auto crop currently applied */
static volatile int g_crop_mode = CROP_MANUAL;
static volatile int g_autofit_req;

/* Core options we answer for GET_VARIABLE. Anything not listed -> core default. */
#define MAX_OPTS 16
static struct { char key[64]; char val[64]; } g_opts[MAX_OPTS] = {
    {"same_cdi_mouse_enable", "enabled"}, /* the CD-i pointer is a mouse */
};
static int g_nopts = 1;

static void opt_set(const char *kv)
{
    const char *eq = strchr(kv, '=');
    if (!eq || eq == kv || eq - kv >= 63 || strlen(eq + 1) >= 63) {
        elogf("bad option '%s' (want key=value)", kv);
        return;
    }
    char key[64];
    snprintf(key, sizeof key, "%.*s", (int)(eq - kv), kv);
    for (int i = 0; i < g_nopts; i++)
        if (!strcmp(g_opts[i].key, key)) {
            snprintf(g_opts[i].val, sizeof g_opts[i].val, "%s", eq + 1);
            return;
        }
    if (g_nopts < MAX_OPTS) {
        snprintf(g_opts[g_nopts].key, sizeof g_opts[g_nopts].key, "%s", key);
        snprintf(g_opts[g_nopts].val, sizeof g_opts[g_nopts].val, "%s", eq + 1);
        g_nopts++;
    }
}
static unsigned long g_dupes, g_frames_in;

/* Sound is played from a buffer that is topped up one video frame at a time. The
 * buffer's low-water mark is g_audio_ms (the sound is on average about half a frame
 * later than that, plus the output's own buffer): lower = sound earlier, but too low
 * and it runs dry (the "audio underruns" in the stats). */
static int g_audio_ms = 30;
static int g_audio_samples = 512; /* unused here (the console's output decides its chunks); read from options.cfg */
static void audio_cfg_save(void);

/* Pointer (the CD-i's mouse) speed from the pad. 100 % means the sticks move the pointer
 * at up to PTR_MAX_COUNTS per video frame at full tilt; small movements are slow and fine
 * (a curve), and the D-pad speeds up the longer it is held. */
#define PTR_MAX_COUNTS 14.0f
#define PTR_PRECISION 0.4f /* holding L1 slows the pointer to this fraction, for fine aiming */
static int g_ptr_speed = 100;
static void ptr_cfg_save(void);

/* Pointer modes. 0 = relative: the sticks push the pointer along.
 * 1 = absolute: the stick position IS the pointer position, like a light gun - tilt right
 * and the crosshair is at the right edge at once; release and it returns to the centre.
 * The CD-i keeps an absolute pointer position (768 x 560 units) inside the player, so the
 * host tracks where it thinks that is (est_x/est_y, updated from every count it hands
 * over) and sends the difference to the target. Pushing to an edge overshoots on purpose,
 * which re-synchronises the estimate. */
#define PTR_DEV_W 768
static int g_ptr_mode;
static char g_game_name[200]; /* the game as the list names it (a .7z's own name, not the file inside), for per-game settings */

/* The CD-i's buttons. The emulated player has two (button 1 and button 2; a third is ignored). In this
 * core they come from the RetroPad's Y (button 1) and B (button 2), not from mouse clicks, which are
 * dropped. Each pad button below is mapped to a mask: 1 = button 1, 2 = button 2, 3 = both together,
 * 0 = none. Order: Cross, Circle, Square, Triangle, L1, R1, L2, R2. Per game in options.cfg:
 * host_button_cross=1 (or 2, 12 for both, none), and circle, square, triangle, l1, r1, l2, r2. */
static int g_btn_map[8] = {1, 2, 1, 2, 0, 0, 0, 0}; /* the face buttons only (v53: L1, R1, L2, R2 do nothing) */
static int g_dpad_digital; /* options.cfg host_dpad_digital=1: also hand the D-pad to the core as buttons (a fixed 2-unit step per frame) */
static int g_ptr_yrange = 560; /* options.cfg host_pointer_y_range: try 280 if the pointer reaches the bottom too early */
static int est_x, est_y, est_valid;

static int g_pad_log_left; /* lines of pad changes still to log in this game */
static volatile int g_input_hold; /* after the menu closes: ignore the pad until its buttons are released */

/* The core's disc tray (libretro disc control), given to us when a game is loaded. */
static struct retro_disk_control_callback g_disk;
static int g_have_disk;

/* ----------------------------------------------------------------- misc */

/* Folders are made 0777 and files 0666: on the console, the FTP server is another process, and a save, a log or
 * a setting it cannot read or replace is no use to anyone (the template's files-and-io.md). */
static void mkdirs(const char *path)
{
    char tmp[1024];
    snprintf(tmp, sizeof tmp, "%s", path);
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = 0;
            if (mkdir(tmp, 0777) == 0)
                chmod(tmp, 0777);
            *p = '/';
        }
    }
    if (mkdir(tmp, 0777) == 0)
        chmod(tmp, 0777);
}

/* Gives everything under dir the modes above (files left by older builds, or made with a umask). */
static void open_modes(const char *dir, int depth)
{
    DIR *d = opendir(dir);
    struct dirent *e;
    if (!d)
        return;
    chmod(dir, 0777);
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
            continue;
        char p[1400];
        struct stat st;
        snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
        if (stat(p, &st) != 0)
            continue;
        if (S_ISDIR(st.st_mode)) {
            if (depth < 4)
                open_modes(p, depth + 1);
        } else if ((st.st_mode & 0666) != 0666) {
            chmod(p, (st.st_mode & 07777) | 0666);
        }
    }
    closedir(d);
}

static void log_cb(enum retro_log_level level, const char *fmt, ...)
{
    static const char *names[] = {"DEBUG", "INFO", "WARN", "ERROR"};
    if (level == RETRO_LOG_DEBUG)
        return;
    char line[1000];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    elogf("[core %s] %s", names[level <= 3 ? level : 3], line);
}

/* Video smoothing: the console shows 60 pictures a second, a PAL CD-i makes 50. Showing each frame until the
 * next one arrives repeats one picture in six, and scrolling stutters. With smoothing, the picture shown at
 * every refresh is a blend of the two newest frames, weighted by where that refresh falls between them. It is
 * switched off by itself when the display rate and the frame rate match (an NTSC game at 60). The cost is
 * about one frame (20 ms) of extra delay and a slight softness on fast motion.
 * options.cfg: host_smooth_video=0 or 1, host_smooth_strength=0..100 (how much of the blend is used). */
static volatile int g_smooth = 1;

/* Fast start (v44): the CD-i player's own start (its blue screen, reading the disc, the switch of screen mode
 * into the game) runs as fast as the console can emulate it, unseen and unheard, and the game is shown from
 * the moment it has its own picture. The end is found from the picture: the player's start screen is one
 * size (mode), the game switches to its own; once the size has stayed put for host_fast_start_settle frames
 * after a change, or after host_fast_start_max seconds of CD-i time in all, the game is shown and heard.
 * Pressing a button shows it at once. options.cfg: host_fast_start=0 turns it off (per game too). */
static int g_fast_start = 1, g_fast_start0;
static int g_fast_max_s = 30, g_fast_max_s0;
static int g_fast_settle = 40, g_fast_settle0;   /* frames (0.8 s at 50 Hz) */
static int g_fast_ignore = 100, g_fast_ignore0;  /* mode changes in the first frames (2 s) are the player's own */
static volatile int g_booting;                   /* the fast start is running */
static volatile int g_boot_skip;                 /* the player asked to see it now */
static long g_boot_frames, g_boot_last_change, g_boot_changes;
static unsigned g_boot_w, g_boot_h;
static double g_boot_t0;
static void boot_watch(unsigned w, unsigned h, const void *data, size_t pitch);
static void boot_end(const char *why);

/* The "Play CD-i" click (v56). Every game stops at the player's own start screen, where the player clicks Play CD-i.
 * The first time, the moves and the click the player makes there are recorded (from the moment that screen shows
 * until the click, the quiet stretches shortened), and kept once the game's own picture follows within 15 s:
 * saves/start_click.txt. From then on the fast start plays them back itself, unseen, and goes on until the game's
 * own picture: choosing a game leads straight to its title. The emulation is exact, so the same moves from the same
 * screen land in the same place. If a playback is not followed by a new picture within 25 s, the player's start
 * screen is shown and the next click made there is recorded instead. options.cfg: host_auto_play=0 turns it off. */
static long e_frames; /* frames the core has made since the game was loaded (defined with the clock, below) */
typedef struct { int off; int dx, dy; unsigned mask; } click_ev;
#define CLICK_MAX 600
static click_ev g_click[CLICK_MAX];      /* the recording in use */
static int g_click_n, g_click_loaded;
static int g_auto_play = 1, g_auto_play0;
static int g_boot_phase;                 /* 1: on the way to the player's start screen, 2: after the click, to the game */
static long g_replay_at, g_phase2_at;    /* boot frames: where the playback starts, where phase 2 began */
static int g_replay_i;
static click_ev g_rec[CLICK_MAX];        /* a recording being made */
static int g_rec_n, g_rec_on, g_rec_click = -1, g_rec_pending;
static long g_rec_start, g_rec_until;
static unsigned g_vmode_w, g_vmode_h;    /* the screen mode outside the fast start (a new one confirms a recording) */
static long g_learn_frames;              /* this game's learned start: the frame its own picture showed, 0 unknown */
static long g_play_from;                 /* e_frames when the game was first shown (for the auto save) */
static int g_resume_req;                 /* Continue: load the automatic state instead of starting the game */
static double g_pt_t0 = -1;              /* time played: pc_ms() when the game showed, or -1 */
static void pt_load(void);
static void pt_add(const char *base, long secs);
static void click_load(void);
static void rec_start(void);
static void rec_commit(void);
static void learn_save(long frames);

int cdie_booting(void)
{
    return g_booting; /* (cleared when the game is unloaded) */
}

void cdie_boot_skip(void)
{
    g_boot_skip = 1;
}
static int g_smooth_strength = 100;

/* ---------------------------------------------------------------- audio */

/* The core's sound goes into a ring; the program's audio thread takes it out at 48 kHz (cdie_audio). The
 * emulator thread is paced so that the ring holds about g_audio_ms of sound (see emu_loop). */
#define RB_FRAMES 16384 /* stereo frames, power of two */
#define OUT_RATE 48000
static int16_t rb[RB_FRAMES * 2];
static size_t rb_r, rb_w, rb_count;
static unsigned long underruns, overruns;
static pl_mutex *g_audio_mtx;
static volatile int g_audio_live;     /* the ring is primed: the output plays it */
static volatile int g_audio_quiet;    /* paused, or no game: the output gets silence and the ring is kept */
static unsigned g_core_rate = OUT_RATE;
static double g_rs_pos;               /* resampling: where between two core frames the output is */
static double g_chunk_ema = 512;      /* how many frames the output asks for at a time */

static unsigned long g_audio_in; /* frames the core has given, in all */

static volatile int g_rewinding; /* (rewind, below: going back is not heard) */
static int g_rw_enabled, g_rw_cfg_read;
static void rw_clear(void);
static void rw_free(void);
static void keep_backup(void);
static void rw_cfg_load(void);
static size_t audio_batch_cb(const int16_t *data, size_t frames)
{
    g_audio_in += frames;
    if (g_booting || g_rewinding)
        return frames; /* the fast start is not heard, nor rewinding */
    pl_lock(g_audio_mtx);
    for (size_t i = 0; i < frames; i++) {
        if (rb_count >= RB_FRAMES) {
            overruns++;
            break;
        }
        rb[rb_w * 2] = data[i * 2];
        rb[rb_w * 2 + 1] = data[i * 2 + 1];
        rb_w = (rb_w + 1) & (RB_FRAMES - 1);
        rb_count++;
    }
    pl_unlock(g_audio_mtx);
    return frames;
}

static void audio_sample_cb(int16_t l, int16_t r)
{
    int16_t s[2] = {l, r};
    audio_batch_cb(s, 1);
}

static size_t audio_buffered(void)
{
    pl_lock(g_audio_mtx);
    size_t n = rb_count;
    pl_unlock(g_audio_mtx);
    return n;
}

static void audio_clear(void)
{
    pl_lock(g_audio_mtx);
    rb_r = rb_w = rb_count = 0;
    g_rs_pos = 0;
    g_audio_live = 0;
    pl_unlock(g_audio_mtx);
}

void cdie_audio(int16_t *out, int frames)
{
    if (frames <= 0)
        return;
    g_chunk_ema += 0.05 * ((double)frames - g_chunk_ema);
    pl_lock(g_audio_mtx);
    if (!g_audio_live || g_audio_quiet) {
        pl_unlock(g_audio_mtx);
        memset(out, 0, (size_t)frames * 2 * sizeof *out);
        return;
    }
    int i = 0;
    if (g_core_rate == OUT_RATE) {
        size_t n = (size_t)frames < rb_count ? (size_t)frames : rb_count;
        for (; i < (int)n; i++) {
            out[i * 2] = rb[rb_r * 2];
            out[i * 2 + 1] = rb[rb_r * 2 + 1];
            rb_r = (rb_r + 1) & (RB_FRAMES - 1);
        }
        rb_count -= n;
    } else { /* linear interpolation between the core's frames */
        const double step = (double)g_core_rate / (double)OUT_RATE;
        for (; i < frames; i++) {
            if (rb_count < 2)
                break;
            const size_t a = rb_r, b = (rb_r + 1) & (RB_FRAMES - 1);
            const double f = g_rs_pos;
            out[i * 2] = (int16_t)((double)rb[a * 2] * (1.0 - f) + (double)rb[b * 2] * f);
            out[i * 2 + 1] = (int16_t)((double)rb[a * 2 + 1] * (1.0 - f) + (double)rb[b * 2 + 1] * f);
            g_rs_pos += step;
            while (g_rs_pos >= 1.0 && rb_count >= 2) {
                g_rs_pos -= 1.0;
                rb_r = (rb_r + 1) & (RB_FRAMES - 1);
                rb_count--;
            }
        }
    }
    if (i < frames) {
        memset(out + i * 2, 0, (size_t)(frames - i) * 2 * sizeof *out);
        underruns++;
    }
    pl_unlock(g_audio_mtx);
}
/* --debug: report whether the core is really sending a picture. */
static void debug_scan(const void *data, unsigned w, unsigned h, size_t pitch)
{
    static unsigned n;
    if (!g_debug || n++ % 100 != 0)
        return;
    size_t nonzero = 0;
    for (unsigned y = 0; y < h; y++) {
        const unsigned char *row = (const unsigned char *)data + y * pitch;
        if (g_fmt == RETRO_PIXEL_FORMAT_XRGB8888) {
            const uint32_t *px = (const uint32_t *)row;
            for (unsigned x = 0; x < w; x++)
                nonzero += (px[x] & 0xFFFFFF) != 0;
        } else {
            const uint16_t *px = (const uint16_t *)row;
            for (unsigned x = 0; x < w; x++)
                nonzero += px[x] != 0;
        }
    }
    const uint32_t *r0 = (const uint32_t *)data;
    const uint32_t *rc = (const uint32_t *)((const unsigned char *)data + (h / 2) * pitch);
    elogf("[host] video %ux%u fmt %d nonblack %zu | px(0,0)=%08x px(centre)=%08x | frames %lu dupes %lu\n",
            w, h, (int)g_fmt, nonzero, r0[0], rc[w / 2], g_frames_in, g_dupes);
}

/* View settings persist in <save dir>/view.cfg:
 *   version mode crop_mode  L R T B (4:3)  L R T B (16:9)
 * Files from older builds (other version) are ignored, so a change of the
 * built-in defaults takes effect without deleting anything. */
#define VIEW_CFG_VERSION 14

static void view_save(void)
{
    char path[1100];
    snprintf(path, sizeof path, "%s/view.cfg", g_save_dir);
    FILE *f = fopen(path, "w");
    if (f) {
        fprintf(f, "%d %d %d", VIEW_CFG_VERSION, g_view_mode, g_crop_mode);
        for (int m = 0; m < 2; m++)
            for (int i = 0; i < 4; i++)
                fprintf(f, " %d", g_crop[m][i]);
        fprintf(f, "\n");
        fclose(f);
    }
}

static void view_load(void)
{
    char path[1100];
    int v, m, cm, c[2][4];
    snprintf(path, sizeof path, "%s/view.cfg", g_save_dir);
    FILE *f = fopen(path, "r");
    if (!f)
        return;
    int n = fscanf(f, "%d %d %d", &v, &m, &cm);
    for (int k = 0; k < 8 && n == 3 + k; k++)
        n += fscanf(f, "%d", &c[k / 4][k % 4]);
    if (n == 11 && v == VIEW_CFG_VERSION && cm >= 0 && cm <= 2) {
        int ok = 1;
        for (int k = 0; k < 8; k++)
            ok &= c[k / 4][k % 4] >= 0 && c[k / 4][k % 4] <= 300;
        if (ok) {
            g_view_mode = m ? 1 : 0;
            g_crop_mode = cm;
            for (int k = 0; k < 8; k++)
                g_crop[k / 4][k % 4] = c[k / 4][k % 4];
        }
    }
    fclose(f);
}

static const char *view_name(void) { return g_view_mode ? "16:9" : "4:3"; }

/* Measures where the picture sits inside the frame: anything that differs from
 * the frame's border colour (the colour of at least two corners). Returns the
 * margins in tenths of a percent (L R T B), less a small safety allowance.
 * Returns 0 if there is no picture to measure. */
static int measure_bounds(const void *data, unsigned w, unsigned h, size_t pitch, int out[4])
{
    if (g_fmt != RETRO_PIXEL_FORMAT_XRGB8888 || w < 32 || h < 32)
        return 0;
#define PX(x, y) (((const uint32_t *)((const unsigned char *)data + (size_t)(y) * pitch))[x] & 0xFFFFFF)
    const uint32_t corner[4] = {PX(0, 0), PX(w - 1, 0), PX(0, h - 1), PX(w - 1, h - 1)};
#undef PX
    uint32_t bg = corner[0];
    int agree = 0;
    for (int i = 1; i < 4; i++) {
        int d = 0;
        for (int sh = 0; sh < 24; sh += 8) {
            int x = (int)((corner[i] >> sh) & 255) - (int)((bg >> sh) & 255);
            d |= (x < -12 || x > 12);
        }
        agree += !d;
    }
    int dark = ((bg >> 16) & 255) <= 48 && ((bg >> 8) & 255) <= 48 && (bg & 255) <= 48;
    if (!agree || !dark) { /* the picture reaches the corners, or the corner colour is
                              not a dark border: nothing to trim */
        out[0] = out[1] = out[2] = out[3] = 0;
        return 1;
    }
    const int br = (int)((bg >> 16) & 255), bgc = (int)((bg >> 8) & 255), bb = (int)(bg & 255);
    unsigned minx = w, maxx = 0, miny = h, maxy = 0;
    for (unsigned y = 0; y < h; y++) {
        const uint32_t *row = (const uint32_t *)((const unsigned char *)data + y * pitch);
        for (unsigned x = 0; x < w; x++) {
            uint32_t c = row[x] & 0xFFFFFF;
            int dr = (int)((c >> 16) & 255) - br, dg = (int)((c >> 8) & 255) - bgc,
                db = (int)(c & 255) - bb;
            if (dr < -12 || dr > 12 || dg < -12 || dg > 12 || db < -12 || db > 12) {
                if (x < minx) minx = x;
                if (x > maxx) maxx = x;
                if (y < miny) miny = y;
                if (y > maxy) maxy = y;
            }
        }
    }
    if (maxx <= minx || maxy <= miny)
        return 0;
    int v[4] = {(int)(minx * 1000 / w) - 3, (int)((w - 1 - maxx) * 1000 / w) - 3,
                (int)(miny * 1000 / h) - 3, (int)((h - 1 - maxy) * 1000 / h) - 3};
    for (int i = 0; i < 4; i++)
        out[i] = v[i] < 0 ? 0 : v[i] > 300 ? 300 : v[i];
    return (w * (1000 - out[0] - out[1]) / 1000 >= w / 3 &&
            h * (1000 - out[2] - out[3]) / 1000 >= h / 3);
}

/* L2 + L3: set the manual crop of the current aspect to the picture on screen. */
static void autofit(const void *data, unsigned w, unsigned h, size_t pitch)
{
    int m[4];
    g_autofit_req = 0;
    if (!measure_bounds(data, w, h, pitch, m)) {
        notify("cdihost: auto-fit found no picture (try again on a full scene)");
        return;
    }
    for (int i = 0; i < 4; i++)
        g_crop[g_view_mode ? 1 : 0][i] = m[i];
    g_crop_mode = CROP_MANUAL;
    notify("cdihost: %s crop fitted: left %.1f%% right %.1f%% top %.1f%% bottom %.1f%%",
           view_name(), m[0] / 10.0, m[1] / 10.0, m[2] / 10.0, m[3] / 10.0);
    view_save();
}

/* Auto crop mode: re-measure every few frames and adopt a new crop only after
 * it has stayed the same for about half a second, so fades and flicker do not
 * make the picture jump around. */
static void auto_track(const void *data, unsigned w, unsigned h, size_t pitch)
{
    static unsigned tick;
    static int cand[4], stable;
    int m[4];
    if (++tick % 6)
        return;
    if (!measure_bounds(data, w, h, pitch, m))
        return; /* blank frame: keep what we have */
    int same = 1;
    for (int i = 0; i < 4; i++)
        same &= (m[i] - cand[i] <= 4 && cand[i] - m[i] <= 4);
    if (!same) {
        for (int i = 0; i < 4; i++)
            cand[i] = m[i];
        stable = 0;
        return;
    }
    if (stable < 5)
        stable++;
    if (stable >= 5)
        for (int i = 0; i < 4; i++)
            if (g_auto[i] - cand[i] > 4 || cand[i] - g_auto[i] > 4) {
                for (int k = 0; k < 4; k++)
                    g_auto[k] = cand[k];
                break;
            }
}

/* ---------------------------------------------------------------- video */

/* The emulator keeps its two newest frames, converted to RGBA, in three buffers: the newest, the one before
 * it, and one it fills next. The program reads the first two under g_vid_mtx (cdie_video_lock); the third is
 * only ever touched by the emulator thread, so filling it never waits for the program. */
typedef struct {
    uint8_t *rgba;
    size_t cap;
    int w, h;
    double t;
    unsigned serial;
} vframe_t;

static vframe_t g_vf[3];
static int g_vnew = -1, g_vprev = -1;
static unsigned g_vserial;
static pl_mutex *g_vid_mtx;
static double g_period = 20.0;      /* time between the emulator's frames, measured */
static double g_ref_period = 16.7;  /* time between the screen's refreshes, measured */
static double g_last_arrival, g_last_blend_call;
static unsigned long vid_frames;

static void convert_rgba(uint8_t *dst, const void *src, int w, int h, size_t pitch)
{
    for (int y = 0; y < h; y++) {
        uint32_t *d = (uint32_t *)(dst + (size_t)y * (size_t)w * 4);
        const uint8_t *row = (const uint8_t *)src + (size_t)y * pitch;
        if (g_fmt == RETRO_PIXEL_FORMAT_XRGB8888) {
            const uint32_t *s = (const uint32_t *)row;
            for (int x = 0; x < w; x++) {
                const uint32_t p = s[x];
                d[x] = 0xFF000000u | ((p >> 16) & 0xFFu) | (p & 0xFF00u) | ((p & 0xFFu) << 16);
            }
        } else if (g_fmt == RETRO_PIXEL_FORMAT_RGB565) {
            const uint16_t *s = (const uint16_t *)row;
            for (int x = 0; x < w; x++) {
                const unsigned p = s[x], r = (p >> 11) & 31, g = (p >> 5) & 63, b = p & 31;
                d[x] = 0xFF000000u | ((r << 3) | (r >> 2)) | (((g << 2) | (g >> 4)) << 8) | (((b << 3) | (b >> 2)) << 16);
            }
        } else {
            const uint16_t *s = (const uint16_t *)row;
            for (int x = 0; x < w; x++) {
                const unsigned p = s[x], r = (p >> 10) & 31, g = (p >> 5) & 31, b = p & 31;
                d[x] = 0xFF000000u | ((r << 3) | (r >> 2)) | (((g << 3) | (g >> 2)) << 8) | (((b << 3) | (b >> 2)) << 16);
            }
        }
    }
}

static void video_cb(const void *data, unsigned w, unsigned h, size_t pitch)
{
    if (!data || !w || !h) { /* NULL = duplicate previous frame */
        g_dupes++;
        return;
    }
    g_frames_in++;
    if (!g_booting && (w != g_vmode_w || h != g_vmode_h)) {
        if (g_vmode_w && g_rec_on && g_rec_click >= 0) { /* a new picture already, the click still being recorded */
            g_rec_on = 0;
            g_rec_pending = 1;
        }
        if (g_vmode_w && g_rec_pending)
            rec_commit(); /* the click was Play CD-i: the game's picture followed */
        if (g_vmode_w && g_play_from < 0)
            g_play_from = e_frames; /* the game itself shows from here (for the automatic save) */
        g_vmode_w = w;
        g_vmode_h = h;
    }
    if (g_booting) {
        boot_watch(w, h, data, pitch);
        if (g_autofit_req)
            autofit(data, w, h, pitch);
        if (g_crop_mode == CROP_AUTO)
            auto_track(data, w, h, pitch);
        return; /* not shown: no need to convert it */
    }
    if (g_autofit_req)
        autofit(data, w, h, pitch);
    if (g_crop_mode == CROP_AUTO)
        auto_track(data, w, h, pitch);
    debug_scan(data, w, h, pitch);
    int ws = 0; /* the buffer that is neither of the two the program may be reading */
    while (ws == g_vnew || ws == g_vprev)
        ws++;
    vframe_t *f = &g_vf[ws];
    const size_t need = (size_t)w * h * 4;
    if (f->cap < need) {
        free(f->rgba);
        f->rgba = malloc(need);
        f->cap = f->rgba ? need : 0;
        if (!f->rgba)
            return;
    }
    convert_rgba(f->rgba, data, (int)w, (int)h, pitch);
    f->w = (int)w;
    f->h = (int)h;
    f->t = pc_ms();
    pl_lock(g_vid_mtx);
    if (g_vnew >= 0) { /* the time between frames, as the emulator makes them */
        const double gap = f->t - g_last_arrival;
        if (gap > 5.0 && gap < 80.0)
            g_period += 0.05 * (gap - g_period);
    }
    g_last_arrival = f->t;
    f->serial = ++g_vserial;
    g_vprev = g_vnew;
    g_vnew = ws;
    vid_frames++;
    pl_unlock(g_vid_mtx);
}

int cdie_video_lock(cdie_image *newest, cdie_image *previous)
{
    pl_lock(g_vid_mtx);
    int n = 0;
    memset(newest, 0, sizeof *newest);
    memset(previous, 0, sizeof *previous);
    if (g_vnew >= 0) {
        const vframe_t *f = &g_vf[g_vnew];
        newest->rgba = f->rgba; newest->w = f->w; newest->h = f->h; newest->pitch = f->w * 4;
        newest->t = f->t; newest->serial = f->serial;
        n = 1;
        if (g_vprev >= 0) {
            const vframe_t *p = &g_vf[g_vprev];
            previous->rgba = p->rgba; previous->w = p->w; previous->h = p->h; previous->pitch = p->w * 4;
            previous->t = p->t; previous->serial = p->serial;
            n = 2;
        }
    }
    return n;
}

void cdie_video_unlock(void)
{
    pl_unlock(g_vid_mtx);
}

static void video_forget(void)
{
    pl_lock(g_vid_mtx);
    g_vnew = g_vprev = -1;
    g_last_arrival = 0;
    pl_unlock(g_vid_mtx);
}

/* Show the scene as it was (display lag) = one refresh + one frame ago. The newest frame is `age` old now (0 to
 * one frame), and the scene at that moment lies between the previous frame and the newest exactly at
 * p = age / frame time, whatever the phase between the frames and the refreshes. Cost: about one frame of
 * delay. */
float cdie_video_blend(double now)
{
    const double since = now - g_last_blend_call;
    if (g_last_blend_call > 0 && since > 5.0 && since < 60.0)
        g_ref_period += 0.05 * (since - g_ref_period);
    g_last_blend_call = now;
    if (!g_smooth)
        return 1.0f;
    pl_lock(g_vid_mtx);
    const int have2 = g_vnew >= 0 && g_vprev >= 0 && g_vf[g_vnew].w == g_vf[g_vprev].w && g_vf[g_vnew].h == g_vf[g_vprev].h;
    const double t_new = g_vnew >= 0 ? g_vf[g_vnew].t : 0, period = g_period, last = g_last_arrival;
    pl_unlock(g_vid_mtx);
    const bool rates_match = (period - g_ref_period < 1.2 && g_ref_period - period < 1.2);
    const bool usable = have2 && now - last < 3.0 * period;
    if (!usable || rates_match)
        return 1.0f;
    double p = (now - t_new) / period;
    p = p < 0 ? 0 : p > 1 ? 1 : p;
    if (g_smooth_strength < 100) {
        const double nearest = p < 0.5 ? 0.0 : 1.0;
        p = nearest + (p - nearest) * (double)g_smooth_strength / 100.0;
    }
    return (float)p;
}

/* Which part of the frame is shown: the crop (if it is on), and whether it fills a 16:9 screen or a 4:3 box. */
void cdie_video_view(int fw, int fh, cdie_view *v)
{
    const volatile int *c = g_crop[g_view_mode ? 1 : 0];
    if (g_crop_mode == CROP_AUTO)
        c = g_auto;
    if (g_crop_mode != CROP_OFF) {
        v->x = fw * c[0] / 1000;
        v->y = fh * c[2] / 1000;
        v->w = fw - v->x - fw * c[1] / 1000;
        v->h = fh - v->y - fh * c[3] / 1000;
    } else {
        v->x = 0; v->y = 0; v->w = fw; v->h = fh;
    }
    if (v->w < 16 || v->h < 16) { /* never crop the picture away */
        v->x = 0; v->y = 0; v->w = fw; v->h = fh;
    }
    v->wide = g_view_mode == 1;
}

double cdie_now_ms(void)
{
    return pc_ms();
}

/* ---------------------------------------------------------------- input */

#define BIT(id) (1u << (id))

static pl_mutex *g_pad_mtx;
static cdie_pad g_pad_in;           /* the newest pad state from the program */
static int g_pad_mdx, g_pad_mdy;    /* mouse counts added up since the core last read them */
static cdie_pad g_pad;              /* the state this frame works with */
static unsigned pad_mask;
static int mouse_dx, mouse_dy, mouse_buttons;
static int16_t analog[2][2]; /* [left/right][x/y] */

void cdie_input(const cdie_pad *p)
{
    pl_lock(g_pad_mtx);
    g_pad_in = *p;
    g_pad_mdx += p->mouse_dx;
    g_pad_mdy += p->mouse_dy;
    pl_unlock(g_pad_mtx);
}

static int pad_btn(uint32_t b) { return (g_pad.buttons & b) != 0; }
static int trig_l2(void) { return g_pad.l2 > 31 || pad_btn(CDIE_PAD_L2); }
static int trig_r2(void) { return g_pad.r2 > 31 || pad_btn(CDIE_PAD_R2); }

static void input_poll_cb(void)
{
    pl_lock(g_pad_mtx);
    g_pad = g_pad_in;
    /* moves from outside the pad's sticks (a desktop mouse, in the desktop build): in the relative mode they move
     * the pointer as they come; in the absolute mode they move the point the sticks aim around (below) */
    const int ext_dx = g_pad_mdx, ext_dy = g_pad_mdy;
    const int abs_mode = g_ptr_mode;
    if (!abs_mode) {
        mouse_dx += ext_dx;
        mouse_dy += ext_dy;
    }
    g_pad_mdx = g_pad_mdy = 0;
    pl_unlock(g_pad_mtx);
    mouse_buttons = g_pad.mouse_buttons & 7;
    /* After a game starts or the menu closes, the button that did it is still down: each button that was down
     * then is ignored until it has been let go once. (Until v47 the whole pad waited until every button was up;
     * a button or trigger that never read as up kept every button from the game.) */
    {
        enum { HOLD_L2 = 1u << 30, HOLD_R2 = 1u << 31 };
        uint32_t now = g_pad.buttons | (g_pad.l2 > 31 ? HOLD_L2 : 0) | (g_pad.r2 > 31 ? HOLD_R2 : 0) |
                       ((uint32_t)mouse_buttons << 24);
        static uint32_t held_over;
        static uint32_t since;
        if (g_input_hold) {
            g_input_hold = 0;
            held_over = now & ~(uint32_t)(CDIE_PAD_UP | CDIE_PAD_DOWN | CDIE_PAD_LEFT | CDIE_PAD_RIGHT);
            since = pl_ticks();
            if (held_over)
                elogf("[host] pad: buttons %x were down as the game took the pad: ignored until let go", held_over);
        }
        if (held_over) {
            held_over &= now;
            if (held_over && pl_ticks() - since > 3000) {
                elogf("[host] pad: buttons %x still read as down after 3 s (l2 %d r2 %d): given to the game again",
                      held_over, g_pad.l2, g_pad.r2);
                held_over = 0;
            }
            g_pad.buttons &= ~held_over;
            if (held_over & HOLD_L2) g_pad.l2 = 0;
            if (held_over & HOLD_R2) g_pad.r2 = 0;
            mouse_buttons &= ~(int)((held_over >> 24) & 7);
        }
    }

    unsigned m = 0;
    static const struct { uint32_t pad; int id; } map[] = {
        /* the face buttons are handled below: they are the CD-i's buttons (g_btn_map) */
        /* (v53: OPTIONS, L1 and R1 are no longer handed to the core: the CD-i has no such buttons) */
        {CDIE_PAD_UP, RETRO_DEVICE_ID_JOYPAD_UP},
        {CDIE_PAD_DOWN, RETRO_DEVICE_ID_JOYPAD_DOWN},
        {CDIE_PAD_LEFT, RETRO_DEVICE_ID_JOYPAD_LEFT},
        {CDIE_PAD_RIGHT, RETRO_DEVICE_ID_JOYPAD_RIGHT},
    };
    for (size_t i = 0; i < sizeof map / sizeof map[0]; i++)
        if (pad_btn(map[i].pad))
            m |= BIT(map[i].id);

    /* the pad's buttons as CD-i buttons (see g_btn_map) */
    int cdi_buttons = 0; /* bit 0 = CD-i button 1, bit 1 = button 2, bit 2 = button 3 */
    {
        static const uint32_t face[6] = {CDIE_PAD_CROSS, CDIE_PAD_CIRCLE, CDIE_PAD_SQUARE, CDIE_PAD_TRIANGLE,
                                         CDIE_PAD_L1, CDIE_PAD_R1};
        for (int i = 0; i < 6; i++)
            if (pad_btn(face[i]))
                cdi_buttons |= g_btn_map[i];
        if (trig_l2())
            cdi_buttons |= g_btn_map[6];
        if (trig_r2())
            cdi_buttons |= g_btn_map[7];
    }
    analog[0][0] = g_pad.lx;
    analog[0][1] = g_pad.ly;
    analog[1][0] = g_pad.rx;
    analog[1][1] = g_pad.ry;

    /* hold L1 (alone) to slow down for fine aiming */
    const int precision = 0; /* (until v52: L1 held slowed the pointer; the shoulder buttons are left alone now) */

    /* The D-pad moves the pointer too. Holding a direction speeds it up, from 5 counts per frame to three
     * times that after half a second. */
    {
        static int hold[4];
        const int dirs[4] = {pad_btn(CDIE_PAD_LEFT), pad_btn(CDIE_PAD_RIGHT), pad_btn(CDIE_PAD_UP), pad_btn(CDIE_PAD_DOWN)};
        int delta[4] = {0, 0, 0, 0};
        for (int i = 0; i < 4; i++) {
            hold[i] = (dirs[i] && !abs_mode) ? (hold[i] < 25 ? hold[i] + 1 : 25) : 0;
            if (hold[i])
                delta[i] = (int)(5.0f * (1.0f + 2.0f * (float)hold[i] / 25.0f) * (float)g_ptr_speed / 100.0f * (precision ? PTR_PRECISION : 1.0f) + 0.5f);
        }
        mouse_dx += delta[1] - delta[0];
        mouse_dy += delta[3] - delta[2];
    }

    /* Either stick drives the pointer (CD-i pointing device). Beyond a dead zone the
     * speed follows a curve: gentle near the centre for aiming, fast at full tilt.
     * The fractions are carried over, so slow movements are smooth, not stepped. */
    if (!abs_mode) {
        static float acc[2];
        float sp[2] = {0, 0};
        const float precision_k = precision ? PTR_PRECISION : 1.0f;
        for (int st = 0; st < 2; st++)
            for (int a = 0; a < 2; a++) {
                float v = (float)analog[st][a] / 32767.0f;
                float mg = v < 0 ? -v : v;
                if (mg <= 0.08f)
                    continue;
                mg = (mg - 0.08f) / 0.92f;
                if (mg > 1.0f)
                    mg = 1.0f;
                float c = 0.35f * mg + 0.65f * mg * mg;
                sp[a] += (v < 0 ? -c : c) * PTR_MAX_COUNTS * (float)g_ptr_speed / 100.0f * precision_k;
            }
        for (int a = 0; a < 2; a++) {
            acc[a] += sp[a];
            int whole = (int)acc[a];
            acc[a] -= (float)whole;
            if (a == 0) mouse_dx += whole; else mouse_dy += whole;
        }
    } else {
        /* absolute: both sticks added, a small dead zone, then the position follows the tilt */
        float u[2] = {0, 0};
        for (int st = 0; st < 2; st++)
            for (int a = 0; a < 2; a++) {
                float v = (float)analog[st][a] / 32767.0f;
                float mg = v < 0 ? -v : v;
                if (mg <= 0.10f)
                    continue;
                mg = (mg - 0.10f) / 0.90f;
                u[a] += v < 0 ? -mg : mg;
            }
        const float gain = (float)g_ptr_speed / 100.0f;
        for (int a = 0; a < 2; a++) {
            u[a] *= gain;
            u[a] = u[a] < -1.0f ? -1.0f : u[a] > 1.0f ? 1.0f : u[a];
        }
        /* The aim point: the centre of the screen, moved by a desktop mouse (the desktop build); the sticks
         * aim around it. With none of those it stays in the centre, as before. */
        static float aim_x = -1, aim_y = -1;
        if (aim_x < 0 || !est_valid) {
            aim_x = (PTR_DEV_W - 1) * 0.5f;
            aim_y = (g_ptr_yrange - 1) * 0.5f;
        }
        aim_x += (float)ext_dx;
        aim_y += (float)ext_dy;
        aim_x = aim_x < 0 ? 0 : aim_x > PTR_DEV_W - 1 ? PTR_DEV_W - 1 : aim_x;
        aim_y = aim_y < 0 ? 0 : aim_y > g_ptr_yrange - 1 ? g_ptr_yrange - 1 : aim_y;
        if (!est_valid) { /* find out where the pointer is: push it into the corner */
            mouse_dx -= PTR_DEV_W + 300;
            mouse_dy -= g_ptr_yrange + 300;
            est_valid = 1;
        } else {
            float fx = aim_x + u[0] * 0.5f * (PTR_DEV_W - 1), fy = aim_y + u[1] * 0.5f * (g_ptr_yrange - 1);
            fx = fx < 0 ? 0 : fx > PTR_DEV_W - 1 ? PTR_DEV_W - 1 : fx;
            fy = fy < 0 ? 0 : fy > g_ptr_yrange - 1 ? g_ptr_yrange - 1 : fy;
            int tx = (int)(fx + 0.5f);
            int ty = (int)(fy + 0.5f);
            int dx = tx - est_x, dy = ty - est_y;
            if (tx <= 1) dx -= 48;                 /* overshoot at the edges: re-synchronises */
            if (tx >= PTR_DEV_W - 2) dx += 48;
            if (ty <= 1) dy -= 48;
            if (ty >= g_ptr_yrange - 2) dy += 48;
            mouse_dx += dx;
            mouse_dy += dy;
        }
    }
    /* The core turns the D-pad into a fixed 2-unit step per frame on the CD-i's pointer. The host
     * moves the pointer itself (faster, with its own speed setting, and the absolute mode needs
     * to know every move), so by default the D-pad is not handed over as buttons. */
    if (!g_dpad_digital)
        m &= ~(BIT(RETRO_DEVICE_ID_JOYPAD_UP) | BIT(RETRO_DEVICE_ID_JOYPAD_DOWN) |
               BIT(RETRO_DEVICE_ID_JOYPAD_LEFT) | BIT(RETRO_DEVICE_ID_JOYPAD_RIGHT));
    /* The CD-i's buttons from every source (pad and mouse), as the RetroPad buttons this core reads them
     * from: Y = button 1, B = button 2, A = button 3. */
    {
        int all = cdi_buttons | mouse_buttons;
        if (all & 1) m |= BIT(RETRO_DEVICE_ID_JOYPAD_Y);
        if (all & 2) m |= BIT(RETRO_DEVICE_ID_JOYPAD_B);
        if (all & 4) m |= BIT(RETRO_DEVICE_ID_JOYPAD_A);
    }
    if (g_booting) {
        m = 0;
        mouse_dx = mouse_dy = 0;
        mouse_buttons = 0;
        if (g_boot_phase == 2 && g_replay_i < g_click_n) {
            const long rel = g_boot_frames - g_replay_at;
            while (g_replay_i < g_click_n && g_click[g_replay_i].off < rel)
                g_replay_i++;
            if (g_replay_i < g_click_n && g_click[g_replay_i].off == rel) {
                mouse_dx = g_click[g_replay_i].dx;
                mouse_dy = g_click[g_replay_i].dy;
                m = g_click[g_replay_i].mask;
                g_replay_i++;
            }
        }
    } else if (g_rec_on) {
        const long rel = e_frames - g_rec_start;
        const unsigned btn = m & (BIT(RETRO_DEVICE_ID_JOYPAD_Y) | BIT(RETRO_DEVICE_ID_JOYPAD_B) | BIT(RETRO_DEVICE_ID_JOYPAD_A));
        if ((mouse_dx || mouse_dy || m) && g_rec_n < CLICK_MAX)
            g_rec[g_rec_n++] = (click_ev){(int)rel, mouse_dx, mouse_dy, m};
        if (btn && g_rec_click < 0)
            g_rec_click = (int)rel;
        if (g_rec_click >= 0 && rel > g_rec_click + 25) { /* the click and its release are in: wait for the game */
            g_rec_on = 0;
            g_rec_pending = 1;
            g_rec_until = e_frames + 15 * 50;
        } else if (rel > 120 * 50 || g_rec_n >= CLICK_MAX) {
            g_rec_on = 0; /* nothing clicked in two minutes: not this */
        }
    }
    if (g_rec_pending && e_frames > g_rec_until) {
        g_rec_pending = 0; /* no new picture followed: that click was something else */
        elogf("[host] auto play: that click was not followed by a new picture: not kept");
    }
    pad_mask = m;
    { /* the first button changes of each game, as they arrive and as the core gets them: buttons that do nothing
       * in a game can be told apart from buttons that never reach it */
        static uint32_t last_in = 0xffffffffu;
        static unsigned last_m = 0xffffffffu;
        if (g_pad_log_left > 0 && ((g_pad.buttons & ~0xf000u) != last_in || (m & ~0xf0u) != last_m)) {
            g_pad_log_left--;
            elogf("[host] pad: buttons %x (l2 %d r2 %d) -> core %x (Y=button 1, B=2, A=3; %s)", g_pad.buttons, g_pad.l2,
                  g_pad.r2, m, g_booting ? "starting" : "running");
        }
        last_in = g_pad.buttons & ~0xf000u;
        last_m = m & ~0xf0u;
    }
    {
        static int announced;
        if (!announced && (m & ~(BIT(RETRO_DEVICE_ID_JOYPAD_UP) | BIT(RETRO_DEVICE_ID_JOYPAD_DOWN) |
                                 BIT(RETRO_DEVICE_ID_JOYPAD_LEFT) | BIT(RETRO_DEVICE_ID_JOYPAD_RIGHT)))) {
            announced = 1;
            elogf("[host] first button given to the game (pad %x, core mask %x)", g_pad.buttons, m);
        }
    }
}

static int16_t input_state_cb(unsigned port, unsigned device, unsigned index,
                              unsigned id)
{
    if (port != 0)
        return 0;
    switch (device & RETRO_DEVICE_MASK) {
    case RETRO_DEVICE_JOYPAD:
        if (id == RETRO_DEVICE_ID_JOYPAD_MASK)
            return (int16_t)pad_mask;
        return (pad_mask >> id) & 1;
    case RETRO_DEVICE_ANALOG:
        if (index > 1 || id > 1)
            return 0;
        return analog[index][id];
    case RETRO_DEVICE_MOUSE: {
        int v;
        switch (id) {
        case RETRO_DEVICE_ID_MOUSE_X:
            v = mouse_dx;
            mouse_dx = 0;
            est_x = est_x + v < 0 ? 0 : est_x + v > PTR_DEV_W - 1 ? PTR_DEV_W - 1 : est_x + v;
            return (int16_t)v;
        case RETRO_DEVICE_ID_MOUSE_Y:
            v = mouse_dy;
            mouse_dy = 0;
            est_y = est_y + v < 0 ? 0 : est_y + v > g_ptr_yrange - 1 ? g_ptr_yrange - 1 : est_y + v;
            return (int16_t)v;
        case RETRO_DEVICE_ID_MOUSE_LEFT:   return mouse_buttons & 1;
        case RETRO_DEVICE_ID_MOUSE_RIGHT:  return (mouse_buttons >> 1) & 1;
        case RETRO_DEVICE_ID_MOUSE_MIDDLE: return (mouse_buttons >> 2) & 1;
        }
        return 0;
    }
    }
    return 0;
}
/* ---------------------------------------------------------- environment */

static bool env_cb(unsigned cmd, void *data)
{
    switch (cmd) {
    case RETRO_ENVIRONMENT_GET_CAN_DUPE:
        *(bool *)data = true;
        return true;
    case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT: {
        enum retro_pixel_format f = *(enum retro_pixel_format *)data;
        if (f == RETRO_PIXEL_FORMAT_XRGB8888 || f == RETRO_PIXEL_FORMAT_RGB565 ||
            f == RETRO_PIXEL_FORMAT_0RGB1555) {
            g_fmt = f;
            if (g_debug)
                elogf("[host] pixel format %d\n", (int)f);
            return true;
        }
        return false;
    }
    case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:
        *(const char **)data = g_system_dir;
        return true;
    case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY:
        *(const char **)data = g_save_dir;
        return true;
    case RETRO_ENVIRONMENT_GET_CORE_ASSETS_DIRECTORY:
        *(const char **)data = g_system_dir;
        return true;
    case RETRO_ENVIRONMENT_GET_LIBRETRO_PATH:
        *(const char **)data = g_core_path;
        return true;
    case RETRO_ENVIRONMENT_GET_LOG_INTERFACE:
        ((struct retro_log_callback *)data)->log = log_cb;
        return true;
    case RETRO_ENVIRONMENT_GET_LANGUAGE:
        *(unsigned *)data = RETRO_LANGUAGE_ENGLISH;
        return true;
    case RETRO_ENVIRONMENT_GET_INPUT_BITMASKS:
        return true;
    case RETRO_ENVIRONMENT_GET_VARIABLE: {
        struct retro_variable *v = data;
        for (int i = 0; i < g_nopts; i++)
            if (v->key && !strcmp(v->key, g_opts[i].key)) {
                v->value = g_opts[i].val;
                return true;
            }
        return false; /* not overridden: the core uses its default */
    }
    case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE:
        *(bool *)data = false;
        return true;
    case RETRO_ENVIRONMENT_SET_GEOMETRY: {
        const struct retro_game_geometry *g = data;
        g_aspect = g->aspect_ratio;
        return true;
    }
    case RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO: {
        g_av = *(const struct retro_system_av_info *)data;
        g_aspect = g_av.geometry.aspect_ratio;
        return true;
    }
    case RETRO_ENVIRONMENT_SET_MESSAGE: {
        const struct retro_message *m = data;
        elogf("[core msg] %s\n", m->msg);
        return true;
    }
    case RETRO_ENVIRONMENT_SET_VARIABLES:
    case RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS:
    case RETRO_ENVIRONMENT_SET_CONTROLLER_INFO:
    case RETRO_ENVIRONMENT_SET_PERFORMANCE_LEVEL:
    case RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME:
    case RETRO_ENVIRONMENT_SET_ROTATION:
    case RETRO_ENVIRONMENT_SET_MEMORY_MAPS:
        return true; /* accepted, nothing to do */
    case RETRO_ENVIRONMENT_SET_DISK_CONTROL_INTERFACE:
    case RETRO_ENVIRONMENT_SET_DISK_CONTROL_EXT_INTERFACE:
        /* both start with the same functions; the extended one only adds paths and labels */
        if (data) {
            memcpy(&g_disk, data, sizeof g_disk);
            g_have_disk = 1;
        } else {
            g_have_disk = 0;
        }
        return true;
    default:
        return false;
    }
}

/* ----------------------------------------------------------------- NVRAM */

/* The CD-i keeps saved games in battery-backed NVRAM. The core writes it to
 * <save dir>/same_cdi/nvram/<disc name>/ when it shuts down, but an app that is
 * just closed never gets there, so it is also written regularly while running. */

static bool nvram_flush(void)
{
    if (!core.retro_flush_nvram)
        return false;
    core.retro_flush_nvram();
    return true;
}

static char g_game_name_for_states[200];
static void slot_path(int slot, const char *ext, char *out, size_t n)
{
    snprintf(out, n, "%s/states/%.180s.slot%d.%s", g_save_dir, g_game_name_for_states, slot, ext);
}

static bool slot_save(int slot, char *msg, size_t mn)
{
    char p[1400], dir[1200];
    size_t sz = core.retro_serialize_size();
    if (!sz) {
        snprintf(msg, mn, "this game cannot be saved as a state");
        return false;
    }
    void *buf = malloc(sz);
    if (!buf || !core.retro_serialize(buf, sz)) {
        free(buf);
        snprintf(msg, mn, "saving the state failed");
        return false;
    }
    snprintf(dir, sizeof dir, "%s/states", g_save_dir);
    mkdirs(dir);
    slot_path(slot, "state", p, sizeof p);
    char tmp[1500];
    snprintf(tmp, sizeof tmp, "%s.part", p);
    FILE *f = fopen(tmp, "wb");
    bool ok = f && fwrite(buf, 1, sz, f) == sz;
    if (f)
        ok = (fclose(f) == 0) && ok;
    free(buf);
    if (!ok || rename(tmp, p) != 0) {
        remove(tmp);
        snprintf(msg, mn, "cannot write the state (disk full?)");
        return false;
    }
    /* which disc was in the drive: a state of disc 2 must be loaded with disc 2 inserted */
    slot_path(slot, "disc", p, sizeof p);
    if (g_have_disk && g_disk.get_num_images && g_disk.get_num_images() > 1) {
        f = fopen(p, "w");
        if (f) {
            fprintf(f, "%u\n", g_disk.get_image_index());
            fclose(f);
        }
    } else {
        remove(p);
    }
    snprintf(msg, mn, "saved to slot %d", slot);
    elogf("[host] state saved to slot %d (%zu bytes)\n", slot, sz);
    return true;
}

static bool disc_insert(unsigned idx, char *msg, size_t mn)
{
    if (!g_have_disk || !g_disk.get_num_images || idx >= g_disk.get_num_images()) {
        snprintf(msg, mn, "this game has one disc");
        return false;
    }
    if (g_disk.get_image_index() == idx && !g_disk.get_eject_state()) {
        snprintf(msg, mn, "disc %u is already in", idx + 1);
        return true;
    }
    if (!g_disk.set_eject_state(true) || !g_disk.set_image_index(idx) || !g_disk.set_eject_state(false)) {
        snprintf(msg, mn, "could not change the disc");
        return false;
    }
    snprintf(msg, mn, "disc %u inserted", idx + 1);
    elogf("[host] disc %u inserted\n", idx + 1);
    return true;
}

static bool slot_load(int slot, char *msg, size_t mn)
{
    char p[1400];
    slot_path(slot, "state", p, sizeof p);
    FILE *f = fopen(p, "rb");
    if (!f) {
        snprintf(msg, mn, "slot %d is empty", slot);
        return false;
    }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    void *buf = sz > 0 ? malloc((size_t)sz) : NULL;
    bool ok = buf && fread(buf, 1, (size_t)sz, f) == (size_t)sz;
    fclose(f);
    if (ok) {
        char dp[1400];
        unsigned want;
        slot_path(slot, "disc", dp, sizeof dp);
        FILE *df = fopen(dp, "r");
        if (df) {
            char dm[100];
            if (fscanf(df, "%u", &want) == 1 && g_have_disk && g_disk.get_image_index() != want)
                disc_insert(want, dm, sizeof dm);
            fclose(df);
        }
        ok = core.retro_unserialize(buf, (size_t)sz);
    }
    free(buf);
    snprintf(msg, mn, ok ? "slot %d loaded" : "slot %d could not be loaded (from another version?)", slot);
    elogf("[host] %s\n", msg);
    return ok;
}
typedef struct {
    char path[1024];
    char name[200];  /* title shown on screen */
    char base[200];  /* file name without extension, as on disk */
    char norm[200];  /* base, lower case, without (tags), for loose matching */
    char cover[1400]; /* matching cover image, or empty */
    char drive[16];   /* external drive the game is on ("usb0"), else empty */
    char nas[1100];   /* path on the NAS export if the game lives there (path is then the local copy), else empty */
    uint64_t nas_bytes;
    int ndisc;               /* a game on more than one disc: how many */
    char (*discs)[1100];     /* ... and each disc's path (local) or path on the NAS export, in disc order */
} rom_t;

typedef struct {
    char path[1024];
    char dir[1024];
    char base[200];
    char norm[200];
    int in_covers;
} img_t;

static rom_t *g_roms;
static bool game_is_here(const rom_t *r); /* defined with the NAS / 7z code below */
static int g_nroms, g_roms_cap;
static img_t *g_imgs;
static int g_nimgs, g_imgs_cap;

static bool has_ext(const char *name, const char *ext)
{
    size_t n = strlen(name), e = strlen(ext);
    return n > e + 1 && name[n - e - 1] == '.' && !strcasecmp(name + n - e, ext);
}

static bool is_image_name(const char *name)
{
    return has_ext(name, "png") || has_ext(name, "jpg") || has_ext(name, "jpeg") ||
           has_ext(name, "bmp");
}

static void strip_ext(char *s)
{
    char *dot = strrchr(s, '.');
    if (dot)
        *dot = 0;
}

/* "Burn_Cycle (USA) [!].chd" and "burn cycle" should find the same cover:
 * lower case, '_' as space, trailing (...) and [...] groups removed. */
static void norm_name(const char *in, char *out, size_t n)
{
    char tmp[256];
    size_t len = 0;
    for (; *in && len < sizeof tmp - 1; in++)
        tmp[len++] = *in == '_' ? ' ' : (char)tolower((unsigned char)*in);
    tmp[len] = 0;
    for (;;) {
        while (len && tmp[len - 1] == ' ')
            tmp[--len] = 0;
        char close = len ? tmp[len - 1] : 0;
        char open = close == ')' ? '(' : close == ']' ? '[' : 0;
        char *p = open ? strrchr(tmp, open) : NULL;
        if (!p || p == tmp)
            break;
        *p = 0;
        len = (size_t)(p - tmp);
    }
    snprintf(out, n, "%.*s", (int)n - 1, tmp); /* cut to the destination size */
}

static void rom_add(const char *full, const char *file)
{
    if (g_nroms == g_roms_cap) {
        int cap = g_roms_cap ? g_roms_cap * 2 : 64;
        rom_t *n = realloc(g_roms, (size_t)cap * sizeof *n);
        if (!n)
            return;
        g_roms = n;
        g_roms_cap = cap;
    }
    rom_t *r = &g_roms[g_nroms++];
    memset(r, 0, sizeof *r);
    snprintf(r->path, sizeof r->path, "%s", full);
    snprintf(r->base, sizeof r->base, "%.*s", (int)sizeof r->base - 1, file); /* long names are cut */
    strip_ext(r->base);
    snprintf(r->name, sizeof r->name, "%s", r->base);
    for (char *c = r->name; *c; c++)
        if (*c == '_')
            *c = ' ';
    norm_name(r->base, r->norm, sizeof r->norm);
}

static void img_add(const char *full, const char *dir, const char *file, int in_covers)
{
    if (g_nimgs == g_imgs_cap) {
        int cap = g_imgs_cap ? g_imgs_cap * 2 : 128;
        img_t *n = realloc(g_imgs, (size_t)cap * sizeof *n);
        if (!n)
            return;
        g_imgs = n;
        g_imgs_cap = cap;
    }
    img_t *i = &g_imgs[g_nimgs++];
    memset(i, 0, sizeof *i);
    snprintf(i->path, sizeof i->path, "%s", full);
    snprintf(i->dir, sizeof i->dir, "%s", dir);
    snprintf(i->base, sizeof i->base, "%.*s", (int)sizeof i->base - 1, file);
    strip_ext(i->base);
    norm_name(i->base, i->norm, sizeof i->norm);
    i->in_covers = in_covers;
}

/* A game is a .cue, .chd or .iso file; the .bin tracks of a .cue are not listed.
 * Folders are searched two levels deep. Images found on the way are remembered
 * as possible covers (and, with roms == false, only images are collected). */
static void scan_dir(const char *dir, int depth, bool roms)
{
    DIR *d = opendir(dir);
    if (!d)
        return;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.')
            continue;
        char full[1024];
        snprintf(full, sizeof full, "%s/%s", dir, e->d_name);
        struct stat st;
        if (stat(full, &st) != 0)
            continue;
        if (S_ISDIR(st.st_mode)) {
            if (depth < 2)
                scan_dir(full, depth + 1, roms);
        } else if (is_image_name(e->d_name)) {
            img_add(full, dir, e->d_name, !roms);
        } else if (roms && (has_ext(e->d_name, "cue") || has_ext(e->d_name, "chd") ||
                            has_ext(e->d_name, "iso") || has_ext(e->d_name, "7z"))) {
            rom_add(full, e->d_name);
        }
    }
    closedir(d);
}

static int rom_cmp(const void *a, const void *b)
{
    return strcasecmp(((const rom_t *)a)->name, ((const rom_t *)b)->name);
}

static bool is_cover_word(const char *base)
{
    return !strcasecmp(base, "cover") || !strcasecmp(base, "folder") ||
           !strcasecmp(base, "front") || !strcasecmp(base, "box");
}

/* Cover lookup, best match first:
 *   covers/<file name>.png|jpg      the covers folder, exact file name
 *   <game folder>/<file name>.png   next to the game
 *   same title with case, '_' and (tags) ignored: covers folder, then next to the game
 *   <game folder>/cover.jpg         (also folder, front, box) - only when that folder
 *                                   holds a single game, otherwise it would be ambiguous */
static void resolve_covers(const char *roms_dir)
{
    for (int r = 0; r < g_nroms; r++) {
        rom_t *rom = &g_roms[r];
        char rdir[1024];
        snprintf(rdir, sizeof rdir, "%s", rom->path);
        char *slash = strrchr(rdir, '/');
        if (slash)
            *slash = 0;
        const size_t dlen = strlen(rdir);
        int games_here = 0;
        for (int k = 0; k < g_nroms; k++) {
            const char *p = g_roms[k].path;
            games_here += !strncmp(p, rdir, dlen) && p[dlen] == '/' && !strchr(p + dlen + 1, '/');
        }
        const bool single_in_folder = strcmp(rdir, roms_dir) != 0 && games_here == 1;
        int best = 0;
        rom->cover[0] = 0;
        for (int i = 0; i < g_nimgs; i++) {
            const img_t *img = &g_imgs[i];
            const bool same_norm = *rom->norm && !strcmp(img->norm, rom->norm);
            int score = 0;
            if (img->in_covers) {
                if (!strcasecmp(img->base, rom->base)) score = 100;
                else if (same_norm) score = 80;
            } else if (!strcmp(img->dir, rdir)) {
                if (!strcasecmp(img->base, rom->base)) score = 90;
                else if (same_norm) score = 75;
                else if (single_in_folder && is_cover_word(img->base)) score = 50;
            }
            if (score > best) {
                best = score;
                snprintf(rom->cover, sizeof rom->cover, "%s", img->path);
            }
        }
    }
}

static void nas_scan(void);
static void drives_scan(void);

/* "(Disc 2)" or "(Disc 2 of 3)" in a name: the disc number, and where the tag starts (with its leading space) and ends.
 * 0 if there is none. */
static int disc_tag(const char *s, int *start, int *end)
{
    for (const char *p = strchr(s, '('); p; p = strchr(p + 1, '(')) {
        if (!strncasecmp(p, "(Disc ", 6) && isdigit((unsigned char)p[6])) {
            const char *close = strchr(p, ')');
            if (!close)
                return 0;
            *start = (int)(p - s);
            if (*start > 0 && s[*start - 1] == ' ')
                (*start)--;
            *end = (int)(close - s) + 1;
            return atoi(p + 6);
        }
    }
    return 0;
}

/* Discs of one game: same folder (or NAS folder / drive), same name up to "(Disc N)". The first disc's entry stays,
 * named without the disc tag and holding every disc's path; the others leave the list. The tags after "(Disc N)"
 * may differ: "(Disc 1) (The Game)" and "(Disc 2) (Bonus Disc)" belong to the same release. */
static void group_discs(void)
{
    int *num = calloc((size_t)(g_nroms > 0 ? g_nroms : 1), sizeof *num);
    char (*key)[1400] = calloc((size_t)(g_nroms > 0 ? g_nroms : 1), sizeof *key);
    char *gone = calloc((size_t)(g_nroms > 0 ? g_nroms : 1), 1);
    if (!num || !key || !gone) {
        free(num); free(key); free(gone);
        return;
    }
    for (int i = 0; i < g_nroms; i++) {
        int a, b;
        num[i] = has_ext(g_roms[i].path, "7z") ? 0 : disc_tag(g_roms[i].base, &a, &b); /* a .7z is unpacked on its own */
        if (!num[i])
            continue;
        const char *where = g_roms[i].nas[0] ? g_roms[i].nas : g_roms[i].path;
        const char *slash = strrchr(where, '/');
        snprintf(key[i], sizeof key[i], "%s|%.*s|%.*s", g_roms[i].nas[0] ? "nas" : "local",
                 slash ? (int)(slash - where) : 0, where, a, g_roms[i].base);
    }
    for (int i = 0; i < g_nroms; i++) {
        if (!num[i] || gone[i])
            continue;
        int members[16], nm = 0;
        for (int j = i; j < g_nroms && nm < 16; j++)
            if (num[j] && !gone[j] && !strcmp(key[j], key[i]))
                members[nm++] = j;
        if (nm < 2)
            continue;
        for (int x = 0; x < nm; x++) /* disc order */
            for (int y = x + 1; y < nm; y++)
                if (num[members[y]] < num[members[x]]) {
                    int t = members[x]; members[x] = members[y]; members[y] = t;
                }
        rom_t *r = &g_roms[members[0]];
        r->discs = calloc((size_t)nm, sizeof *r->discs);
        if (!r->discs)
            continue;
        r->ndisc = nm;
        for (int x = 0; x < nm; x++) {
            const rom_t *m = &g_roms[members[x]];
            snprintf(r->discs[x], sizeof r->discs[x], "%s", m->nas[0] ? m->nas : m->path);
            if (x > 0)
                gone[members[x]] = 1;
        }
        int a, b;
        if (disc_tag(r->name, &a, &b)) /* "7th Guest, The (Europe) (Disc 1) (The Game)" -> "... (Europe) (The Game)" */
            memmove(r->name + a, r->name + b, strlen(r->name + b) + 1);
    }
    int out = 0;
    for (int i = 0; i < g_nroms; i++)
        if (!gone[i])
            g_roms[out++] = g_roms[i];
    g_nroms = out;
    free(num); free(key); free(gone);
}

/* the NAS scan's state, shown in the launcher (the scan itself is further down) */
static volatile int g_nas_state;   /* 0 not set up, 1 connecting, 2 connected, 3 failed */
static volatile int g_nas_changed; /* a scan has finished and the launcher has not merged it yet */
static char g_nas_msg[500];        /* one line for the launcher header */
static pl_mutex *g_nas_mtx;

static void roms_scan(const char *dir, const char *covers_dir)
{
    for (int i = 0; i < g_nroms; i++) {
        free(g_roms[i].discs);
        g_roms[i].discs = NULL;
    }
    g_nroms = 0;
    g_nimgs = 0;
    scan_dir(dir, 0, true);
    if (covers_dir && *covers_dir)
        scan_dir(covers_dir, 1, false);
    drives_scan(); /* games on USB / external drives */
    nas_scan(); /* games on the NAS, if one is set up (nfs.cfg) */
    group_discs(); /* "(Disc 1)", "(Disc 2)" ... of one game become one entry */
    if (g_nroms > 1)
        qsort(g_roms, (size_t)g_nroms, sizeof *g_roms, rom_cmp);
    resolve_covers(dir);
}

/* The last game played is remembered so the cursor starts on it. */
static void last_game_path(char *out, size_t n)
{
    snprintf(out, n, "%s/last_game.txt", g_save_dir);
}

static int last_game_index(void)
{
    char path[1100], line[1024];
    last_game_path(path, sizeof path);
    FILE *f = fopen(path, "r");
    if (!f)
        return 0;
    if (!fgets(line, sizeof line, f))
        line[0] = 0;
    fclose(f);
    line[strcspn(line, "\r\n")] = 0;
    for (int i = 0; i < g_nroms; i++)
        if (!strcmp(g_roms[i].path, line))
            return i;
    return 0;
}

static void last_game_save(const char *game)
{
    char path[1100];
    last_game_path(path, sizeof path);
    FILE *f = fopen(path, "w");
    if (f) {
        fprintf(f, "%s\n", game);
        fclose(f);
    }
}

/* ------------------------------------------------- automatic cover download */

/* Matching a game file name to the libretro-thumbnails "Philips - CD-i" box art
 * list (host/cdi_boxarts.h). This is a C port of tools/fetch_covers.py; the two
 * should agree, so change them together. */

#define MAX_TAGS 8
typedef struct {
    char keys[4][96]; /* several spellings of the title that count as equal */
    int nkeys;
    char tags[MAX_TAGS][48]; /* lower case, without brackets */
    int ntags;
    int disc; /* 0 = none */
} parsed_t;

static parsed_t *g_art_parsed; /* one per cdi_boxart_list[] entry, built on first use */

static void squish(const char *in, char *out, size_t n)
{
    size_t o = 0;
    for (; *in && o + 4 < n; in++) {
        unsigned char c = (unsigned char)*in;
        if (c == '&') {
            memcpy(out + o, "and", 3);
            o += 3;
        } else if (isalnum(c) && c < 128) {
            out[o++] = (char)tolower(c);
        }
    }
    out[o] = 0;
}

/* Redump style "Apprentice, The" -> "The Apprentice" */
static void fix_article(const char *title, char *out, size_t n)
{
    static const char *const arts[] = {"The", "A", "An", "Le", "La", "Les", "L'", "Der", "Die",
                                       "Das", "De", "Het", "Il", "El"};
    const char *comma = strrchr(title, ',');
    if (comma) {
        const char *a = comma + 1;
        while (*a == ' ')
            a++;
        for (size_t i = 0; i < sizeof arts / sizeof *arts; i++)
            if (!strcasecmp(a, arts[i])) {
                char rest[160];
                snprintf(rest, sizeof rest, "%.*s", (int)(comma - title), title);
                if (arts[i][strlen(arts[i]) - 1] == '\'')
                    snprintf(out, n, "%s%s", arts[i], rest); /* L'Affaire */
                else
                    snprintf(out, n, "%s %s", arts[i], rest);
                return;
            }
    }
    snprintf(out, n, "%s", title);
}

static void add_key(parsed_t *p, const char *k)
{
    if (!*k)
        return;
    for (int i = 0; i < p->nkeys; i++)
        if (!strcmp(p->keys[i], k))
            return;
    if (p->nkeys < 4)
        snprintf(p->keys[p->nkeys++], sizeof p->keys[0], "%s", k);
}

/* name: a game file base name or a box art file name (the extension, if any, is
 * the caller's business: pass strip_png for box art names). */
static void parse_name(const char *name, bool strip_png, parsed_t *p)
{
    char stem[256], title[256], t[256], words[256], k[96];
    memset(p, 0, sizeof *p);
    snprintf(stem, sizeof stem, "%s", name);
    size_t sl = strlen(stem);
    if (strip_png && sl > 4 && !strcasecmp(stem + sl - 4, ".png"))
        stem[sl - 4] = 0;

    /* tags: every (...) and [...] group; title: the text before the first one */
    snprintf(title, sizeof title, "%s", stem);
    for (char *c = title; *c; c++)
        if (*c == '(' || *c == '[') {
            *c = 0;
            break;
        }
    for (char *e = title + strlen(title); e > title && e[-1] == ' ';)
        *--e = 0;
    for (const char *c = stem; *c;) {
        char open = *c, close = open == '(' ? ')' : open == '[' ? ']' : 0;
        if (close && p->ntags < MAX_TAGS) {
            const char *end = strchr(c + 1, close);
            if (end) {
                int len = (int)(end - c - 1);
                if (len > 47) len = 47;
                for (int i = 0; i < len; i++)
                    p->tags[p->ntags][i] = (char)tolower((unsigned char)c[1 + i]);
                p->tags[p->ntags][len] = 0;
                p->ntags++;
                c = end + 1;
                continue;
            }
        }
        c++;
    }
    for (int i = 0; i < p->ntags; i++) {
        const char *d = strstr(p->tags[i], "disc");
        if (d) {
            d += 4;
            while (*d == ' ') d++;
            if (isdigit((unsigned char)*d)) {
                p->disc = atoi(d);
                break;
            }
        }
    }

    for (char *c = title; *c; c++)
        if (*c == '_')
            *c = ' ';
    fix_article(title, t, sizeof t);
    squish(t, k, sizeof k);
    add_key(p, k);

    /* the same without the word "the": 'Link - The Faces of Evil' ~ 'Link Faces of Evil' */
    words[0] = 0;
    char cur[64];
    int cl = 0;
    for (const char *c = t;; c++) {
        unsigned char ch = (unsigned char)*c;
        if (ch && (isalnum(ch) || ch == '\'') && ch < 128) {
            if (cl < 63) cur[cl++] = (char)tolower(ch);
        } else {
            cur[cl] = 0;
            if (cl && strcmp(cur, "the") != 0)
                strncat(words, cur, sizeof words - strlen(words) - 1);
            cl = 0;
            if (!ch)
                break;
        }
    }
    squish(words, k, sizeof k);
    add_key(p, k);

    /* 'TheApprentice' ~ 'Apprentice' */
    for (int i = 0, n = p->nkeys; i < n; i++)
        if (!strncmp(p->keys[i], "the", 3) && strlen(p->keys[i]) > 6)
            add_key(p, p->keys[i] + 3);
}

static int lcs_len(const char *a, const char *b)
{
    int la = (int)strlen(a), lb = (int)strlen(b);
    if (la > 95 || lb > 95)
        return 0;
    int prev[96] = {0}, cur[96] = {0};
    for (int i = 1; i <= la; i++) {
        for (int j = 1; j <= lb; j++)
            cur[j] = a[i - 1] == b[j - 1] ? prev[j - 1] + 1 : (prev[j] > cur[j - 1] ? prev[j] : cur[j - 1]);
        memcpy(prev, cur, sizeof prev);
    }
    return prev[lb];
}

/* The digits of a key, in order: "crimepatrol2drugwars" -> "2". Two titles with different numbers are
 * different games (a sequel, another year of an award show), whatever else they share. */
static void key_digits(const char *k, char *out, size_t n)
{
    size_t o = 0;
    for (; *k && o + 1 < n; k++)
        if (*k >= '0' && *k <= '9')
            out[o++] = *k;
    out[o] = 0;
}

static double title_score(const parsed_t *r, const parsed_t *c)
{
    for (int i = 0; i < r->nkeys; i++)
        for (int j = 0; j < c->nkeys; j++)
            if (!strcmp(r->keys[i], c->keys[j]))
                return 100.0;
    double best = 0;
    for (int i = 0; i < r->nkeys; i++)
        for (int j = 0; j < c->nkeys; j++) {
            const char *a = r->keys[i], *b = c->keys[j];
            int la = (int)strlen(a), lb = (int)strlen(b);
            if (!la || !lb)
                continue;
            /* The game's name is the start of the cover's ('Zombie Dinos' ~ 'Zombie Dinos from Planet Zeltoid'): a
             * match. The reverse - the cover's name is the start of the game's - is a sequel or a spin-off
             * ('Crime Patrol 2', 'Mad Dog McCree III'), so the cover is not that game's. */
            const bool rom_is_prefix = la >= 6 && lb > la && !strncmp(b, a, (size_t)la) && la * 100 / lb >= 35;
            if (la > lb && lb >= 3 && !strncmp(a, b, (size_t)lb))
                continue;
            /* cheap exit: the best the longest common subsequence could give is 2*min/(la+lb) */
            if (!rom_is_prefix && 200.0 * (la < lb ? la : lb) / (la + lb) < 60.0)
                continue;
            char da[48], db[48];
            key_digits(a, da, sizeof da);
            key_digits(b, db, sizeof db);
            if (strcmp(da, db) != 0)
                continue; /* different numbers: a different game */
            double s = rom_is_prefix ? 82.0 : 200.0 * lcs_len(a, b) / (la + lb);
            if (s > best)
                best = s;
        }
    return best;
}

static bool has_tag_word(const parsed_t *p, const char *w)
{
    for (int i = 0; i < p->ntags; i++)
        if (strstr(p->tags[i], w))
            return true;
    return false;
}

/* Best picture for a game name in a list of libretro-thumbnails names (box art, screenshots, logos); returns its
 * index (or -1) and the plain title score (0-100) in *raw. Region order: Europe, USA, World. *cache holds the
 * list's names parsed, made on first use. */
static int match_names(const char *game, double *raw, int count, const char *(*name_at)(int), parsed_t **cache)
{
    static const char *const prefer[] = {"europe", "usa", "world"};
    static const char *const bad[] = {"beta", "demo", "proto", "sample", "promo", "unl", "preview"};
    if (!*cache) {
        *cache = calloc((size_t)count, sizeof **cache);
        if (!*cache)
            return -1;
        for (int i = 0; i < count; i++)
            parse_name(name_at(i), true, &(*cache)[i]);
    }
    parsed_t r;
    parse_name(game, false, &r);
    int best = -1;
    double best_total = -1e9;
    if (raw)
        *raw = 0;
    for (int i = 0; i < count; i++) {
        const parsed_t *c = &(*cache)[i];
        double s = title_score(&r, c);
        if (s < 60)
            continue;
        double bonus = 0;
        if (r.disc)
            bonus += c->disc == r.disc ? 3 : -3;
        else if (c->disc > 1)
            bonus -= 3;
        for (int k = 0; k < 3; k++)
            if (has_tag_word(c, prefer[k])) {
                bonus += 2.0 - 0.3 * k;
                break;
            }
        for (int k = 0; k < 3; k++)
            if (has_tag_word(&r, prefer[k]) && has_tag_word(c, prefer[k])) {
                bonus += 4;
                break;
            }
        for (int t = 0; t < c->ntags; t++) {
            for (size_t b = 0; b < sizeof bad / sizeof *bad; b++)
                if (strstr(c->tags[t], bad[b]))
                    bonus -= 4;
            if (!strcmp(c->tags[t], "alt"))
                bonus -= 1;
        }
        double total = s + bonus / 10.0;
        if (total > best_total) {
            best_total = total;
            best = i;
            if (raw)
                *raw = s;
        }
    }
    return best;
}

static const char *boxart_name(int i) { return cdi_boxart_list[i].name; }

/* Best box art for a game name; returns the index into cdi_boxart_list[] (or -1) and the plain title score */
static int match_boxart(const char *game, double *raw)
{
    return match_names(game, raw, CDI_BOXARTS_COUNT, boxart_name, &g_art_parsed);
}

/* --- SHA-1, to check a download against the list: the list holds each picture's git
 *     blob id, so what arrives must be exactly that file, however it got here --- */

typedef struct { uint32_t h[5]; uint64_t len; unsigned char buf[64]; size_t n; } sha1_t;

static uint32_t rol32(uint32_t v, int b) { return (v << b) | (v >> (32 - b)); }

static void sha1_block(sha1_t *c, const unsigned char *p)
{
    uint32_t w[80], a = c->h[0], b = c->h[1], cc = c->h[2], d = c->h[3], e = c->h[4];
    for (int i = 0; i < 16; i++)
        w[i] = (uint32_t)p[i * 4] << 24 | (uint32_t)p[i * 4 + 1] << 16 | (uint32_t)p[i * 4 + 2] << 8 | p[i * 4 + 3];
    for (int i = 16; i < 80; i++)
        w[i] = rol32(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    for (int i = 0; i < 80; i++) {
        uint32_t f, k;
        if (i < 20) { f = (b & cc) | (~b & d); k = 0x5A827999u; }
        else if (i < 40) { f = b ^ cc ^ d; k = 0x6ED9EBA1u; }
        else if (i < 60) { f = (b & cc) | (b & d) | (cc & d); k = 0x8F1BBCDCu; }
        else { f = b ^ cc ^ d; k = 0xCA62C1D6u; }
        uint32_t t = rol32(a, 5) + f + e + k + w[i];
        e = d; d = cc; cc = rol32(b, 30); b = a; a = t;
    }
    c->h[0] += a; c->h[1] += b; c->h[2] += cc; c->h[3] += d; c->h[4] += e;
}

static void sha1_init(sha1_t *c)
{
    c->h[0] = 0x67452301u; c->h[1] = 0xEFCDAB89u; c->h[2] = 0x98BADCFEu;
    c->h[3] = 0x10325476u; c->h[4] = 0xC3D2E1F0u;
    c->len = 0;
    c->n = 0;
}

static void sha1_update(sha1_t *c, const unsigned char *d, size_t len)
{
    c->len += len;
    while (len) {
        size_t take = 64 - c->n < len ? 64 - c->n : len;
        memcpy(c->buf + c->n, d, take);
        c->n += take; d += take; len -= take;
        if (c->n == 64) {
            sha1_block(c, c->buf);
            c->n = 0;
        }
    }
}

static void sha1_final(sha1_t *c, unsigned char out[20])
{
    uint64_t bits = c->len * 8;
    unsigned char pad = 0x80;
    sha1_update(c, &pad, 1);
    pad = 0;
    while (c->n != 56)
        sha1_update(c, &pad, 1);
    unsigned char lenb[8];
    for (int i = 0; i < 8; i++)
        lenb[i] = (unsigned char)(bits >> (56 - 8 * i));
    sha1_update(c, lenb, 8);
    for (int i = 0; i < 5; i++) {
        out[i * 4] = (unsigned char)(c->h[i] >> 24);
        out[i * 4 + 1] = (unsigned char)(c->h[i] >> 16);
        out[i * 4 + 2] = (unsigned char)(c->h[i] >> 8);
        out[i * 4 + 3] = (unsigned char)c->h[i];
    }
}

/* Does `data` have this git blob id? */
static bool blob_hash_ok(const unsigned char want[20], const unsigned char *data, size_t len)
{
    char head[40];
    unsigned char out[20];
    sha1_t c;
    sha1_init(&c);
    sha1_update(&c, (const unsigned char *)head, (size_t)snprintf(head, sizeof head, "blob %zu", len) + 1); /* + the NUL */
    sha1_update(&c, data, len);
    sha1_final(&c, out);
    return !memcmp(out, want, 20);
}

/* Does `data` have the git blob id the list gives for box art `idx`? */
static bool boxart_hash_ok(int idx, const unsigned char *data, size_t len)
{
    char head[40];
    unsigned char out[20];
    sha1_t c;
    sha1_init(&c);
    sha1_update(&c, (const unsigned char *)head, (size_t)snprintf(head, sizeof head, "blob %zu", len) + 1); /* + the NUL */
    sha1_update(&c, data, len);
    sha1_final(&c, out);
    return !memcmp(out, cdi_boxart_list[idx].sha1, 20);
}

/* --- Getting the pictures. Two ways, tried in this order:
 *   1. the system's HTTPS (the console's libSceHttp2, the way PS5SX2 and the SDK sample
 *      do it; desktop builds use the curl command). It checks certificates properly, but
 *      in a homebrew process it often cannot work at all (it fails with errno 2, probably
 *      because a file it needs is not visible there).
 *   2. our own small HTTPS client (host/tlsget.c: BSD sockets + BearSSL). It does not
 *      validate certificates, which is safe only because every picture is then checked
 *      against its known hash (host/cdi_boxarts.h). --- */

#define COVER_MIN_SCORE 80.0

static int g_dns_diag_off __attribute__((unused)); /* set when the NAS code reached the network first: skip the cover server DNS look-ups */

/* Two places the same pictures can be fetched from. They live on different domains, so
 * a DNS filter that blocks one may let the other through. */
static const char *const cv_prefix[] = {
    "https://raw.githubusercontent.com/libretro-thumbnails/Philips_-_CD-i/master/Named_Boxarts/",
    "https://thumbnails.libretro.com/Philips%20-%20CD-i/Named_Boxarts/",
};
static const char *const cv_hostname[] = {"raw.githubusercontent.com", "thumbnails.libretro.com"};

static char g_covers_dir[1024] = DEFAULT_COVERS_DIR;
static bool g_download_ok = true;     /* --no-download turns it off */
static pl_thread *cv_thread;
static volatile int cv_quit;
static volatile int cv_total, cv_done, cv_failed;
static volatile int cv_net_down;      /* no plan could reach a server */
static volatile int cv_matched;       /* the worker has matched the games to box art */
static char cv_msg[200];              /* what went wrong, for the status line */
static int cv_plan_good = -1;         /* the plan that worked last, tried first next time */
static bool cv_secure_only;           /* /data/ps5-cdi/cover_secure_only: only the system's checked HTTPS */
static int *cv_cand;                  /* per game: box art index to fetch, or -1 */
static FILE *cv_logf;

/* Everything the downloader does is logged here and in <save dir>/cover_log.txt,
 * so a failure can be read afterwards (over FTP) instead of guessed at. */
static void cvlog(const char *fmt, ...)
{
    char line[400];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    elogf("[host] covers: %s\n", line);
    if (cv_logf) {
        fprintf(cv_logf, "%s\n", line);
        fflush(cv_logf);
    }
}

/* the steps of each download, in the log: a request that stops the program shows where (tlsget.h) */
static void cv_trace(const char *step)
{
    elogf("[host] covers: download: %s", step);
}

static bool parse_ipv4(const char *s, unsigned char ip[4])
{
    unsigned a, b, c, d;
    char tail;
    if (sscanf(s, "%u.%u.%u.%u%c", &a, &b, &c, &d, &tail) == 4 && a < 256 && b < 256 && c < 256 && d < 256) {
        ip[0] = (unsigned char)a; ip[1] = (unsigned char)b; ip[2] = (unsigned char)c; ip[3] = (unsigned char)d;
        return true;
    }
    return false;
}

/* This console's address on the local network: a UDP socket "connected" to an outside address (nothing is sent) */
static int local_ipv4(unsigned char out[4])
{
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0)
        return -1;
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_port = htons(53);
    inet_pton(AF_INET, "1.1.1.1", &sa.sin_addr);
    int r = -1;
    if (connect(fd, (struct sockaddr *)&sa, sizeof sa) == 0) {
        struct sockaddr_in me;
        socklen_t len = sizeof me;
        if (getsockname(fd, (struct sockaddr *)&me, &len) == 0 && me.sin_addr.s_addr != 0) {
            memcpy(out, &me.sin_addr, 4);
            r = 0;
        }
    }
    close(fd);
    return r;
}


/* ------------------------------------------------------------- the network: names and HTTPS */

/* A PS5 title cannot resolve names with the system (the Jellyfin title found the same), so names are looked up
 * here with a plain DNS query over UDP: the server in <data>/dns.txt if there is one, else the router
 * (192.168.1.1), then 1.1.1.1 and 8.8.8.8. Desktop builds use the system's resolver. HTTPS is our own client
 * (tlsget.c, BearSSL): it does not check certificates, which is safe only because every picture is then checked
 * against its known hash (cdi_boxarts.h). */

static pl_mutex *g_net_mtx;
static volatile int g_nas_busy;

static void http_term(void) {}
static void http_abort(void) {}

#ifdef PS5
static int dns_ask(struct in_addr server, const char *name, unsigned char out[4])
{
    unsigned char q[300];
    size_t ql = 0;
    static unsigned short id = 0x5ce1;
    id++;
    q[ql++] = (unsigned char)(id >> 8); q[ql++] = (unsigned char)id;
    q[ql++] = 0x01; q[ql++] = 0x00; /* recursion desired */
    q[ql++] = 0; q[ql++] = 1;       /* one question */
    memset(q + ql, 0, 6); ql += 6;
    for (const char *p = name; *p;) {
        const char *dot = strchr(p, '.');
        size_t len = dot ? (size_t)(dot - p) : strlen(p);
        if (len == 0 || len > 63 || ql + len + 6 > sizeof q)
            return -1;
        q[ql++] = (unsigned char)len;
        memcpy(q + ql, p, len);
        ql += len;
        p += len;
        if (*p == '.')
            p++;
    }
    q[ql++] = 0;
    q[ql++] = 0; q[ql++] = 1; /* type A */
    q[ql++] = 0; q[ql++] = 1; /* class IN */
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0)
        return -1;
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_port = htons(53);
    sa.sin_addr = server;
    int ok = -1;
    for (int attempt = 0; attempt < 2 && ok != 0 && !cv_quit; attempt++) {
        if (sendto(fd, q, ql, 0, (struct sockaddr *)&sa, sizeof sa) != (ssize_t)ql)
            break;
        struct pollfd pf = {fd, POLLIN, 0};
        if (poll(&pf, 1, 1500) != 1)
            continue;
        unsigned char r[1500];
        ssize_t n = recv(fd, r, sizeof r, 0);
        if (n < 12 || r[0] != q[0] || r[1] != q[1] || (r[3] & 15) != 0)
            continue;
        size_t pos = 12;
        const int qd = (r[4] << 8) | r[5], an = (r[6] << 8) | r[7];
#define SKIP_NAME()                                                     \
    while (pos < (size_t)n) {                                           \
        unsigned char b = r[pos];                                       \
        if (b == 0) { pos++; break; }                                   \
        if ((b & 0xC0) == 0xC0) { pos += 2; break; }                    \
        pos += (size_t)b + 1;                                           \
    }
        for (int i = 0; i < qd; i++) {
            SKIP_NAME();
            pos += 4;
        }
        for (int i = 0; i < an && pos + 10 <= (size_t)n; i++) {
            SKIP_NAME();
            if (pos + 10 > (size_t)n)
                break;
            const int type = (r[pos] << 8) | r[pos + 1], len = (r[pos + 8] << 8) | r[pos + 9];
            pos += 10;
            if (type == 1 && len == 4 && pos + 4 <= (size_t)n) {
                memcpy(out, r + pos, 4);
                ok = 0;
                break;
            }
            pos += (size_t)len;
        }
#undef SKIP_NAME
    }
    close(fd);
    return ok;
}

static int net_resolve(const char *host, unsigned char ip[4])
{
    if (parse_ipv4(host, ip))
        return 0;
    char servers[5][64];
    int ns = 0;
    char path[1400], line[128];
    setting_path(path, sizeof path, g_data_dir, "dns.txt");
    FILE *f = fopen(path, "r");
    if (f) {
        while (ns < 2 && fgets(line, sizeof line, f)) {
            line[strcspn(line, " \t\r\n#")] = 0;
            unsigned char t[4];
            if (parse_ipv4(line, t))
                snprintf(servers[ns++], sizeof servers[0], "%s", line);
        }
        fclose(f);
    }
    /* the router: most are at .1 of the console's own network (10.0.3.1 for 10.0.3.21); 192.168.1.1 if the
     * console's address is not known. Then public servers. */
    unsigned char me[4];
    if (local_ipv4(me) == 0)
        snprintf(servers[ns++], sizeof servers[0], "%u.%u.%u.1", me[0], me[1], me[2]);
    else
        snprintf(servers[ns++], sizeof servers[0], "192.168.1.1");
    snprintf(servers[ns++], sizeof servers[0], "1.1.1.1");
    snprintf(servers[ns++], sizeof servers[0], "8.8.8.8");
    for (int i = 0; i < ns && !cv_quit; i++) {
        struct in_addr a;
        if (inet_pton(AF_INET, servers[i], &a) != 1)
            continue;
        if (dns_ask(a, host, ip) == 0) {
            elogf("[host] dns %s: %u.%u.%u.%u (asked %s)", host, ip[0], ip[1], ip[2], ip[3], servers[i]);
            return 0;
        }
        elogf("[host] dns %s: no answer from %s", host, servers[i]);
    }
    return -1;
}

static int net_init(char *why, size_t wn)
{
    (void)why; (void)wn;
    return 1; /* a title's network is up already */
}

static const char *test_map_url(const char *url, char *mapped, size_t mn, char *why, size_t wn)
{
    (void)mapped; (void)mn; (void)why; (void)wn;
    return url;
}
#else
static int net_resolve(const char *host, unsigned char ip[4])
{
    if (parse_ipv4(host, ip))
        return 0;
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, NULL, &hints, &res) != 0 || !res)
        return -1;
    memcpy(ip, &((struct sockaddr_in *)res->ai_addr)->sin_addr, 4);
    freeaddrinfo(res);
    return 0;
}

static int net_init(char *why, size_t wn)
{
    (void)why; (void)wn;
    return 1;
}

/* Test hooks (desktop only): CDIHOST_TEST_BLOCK=<text> makes any URL containing it fail,
 * CDIHOST_TEST_URLMAP="<from>|<to>" rewrites a URL prefix. */
static const char *test_map_url(const char *url, char *mapped, size_t mn, char *why, size_t wn)
{
    const char *block = getenv("CDIHOST_TEST_BLOCK");
    if (block && strstr(url, block)) {
        snprintf(why, wn, "blocked for the test");
        return NULL;
    }
    /* "from|to", or several: "from|to;from|to" */
    for (const char *map = getenv("CDIHOST_TEST_URLMAP"); map && *map;) {
        const char *semi = strchr(map, ';');
        const size_t len = semi ? (size_t)(semi - map) : strlen(map);
        const char *bar = memchr(map, '|', len);
        if (bar && !strncmp(url, map, (size_t)(bar - map))) {
            snprintf(mapped, mn, "%.*s%s", (int)(map + len - bar - 1), bar + 1, url + (bar - map));
            return mapped;
        }
        map = semi ? semi + 1 : NULL;
    }
    return url;
}
#endif

/* HTTPS GET with our own client. Returns the HTTP status (200: *out holds the body), or -1 with `why` filled in. */
static int own_get(const char *url, unsigned char **out, size_t *len, char *why, size_t wn)
{
    char mapped[900];
    *out = NULL;
    *len = 0;
    if (cv_quit || !net_init(why, wn))
        return -1;
    if (!(url = test_map_url(url, mapped, sizeof mapped, why, wn)))
        return -1;
    return tls_https_get(url, net_resolve, &cv_quit, 30000, out, len, why, wn);
}
static void url_encode(const char *in, char *out, size_t n)
{
    size_t o = 0;
    for (; *in && o + 4 < n; in++) {
        unsigned char c = (unsigned char)*in;
        if (isalnum(c) || c == '-' || c == '.' || c == '_' || c == '~')
            out[o++] = (char)c;
        else
            o += (size_t)snprintf(out + o, n - o, "%%%02X", c);
    }
    out[o] = 0;
}

/* ------------------------------------------------------------- cover pictures, read in the background */

/* The program says which games are in view (cdie_cover_window); a thread reads their pictures, nearest the
 * centre first, scales them down to at most th_max pixels a side and leaves them for the program to take
 * (cdie_cover_take). A picture taken is the program's; cdie_cover_forget makes it wanted again. */
typedef struct {
    int state; /* 0 not loaded, 1 loading, 2 ready to be taken, 3 no usable cover, 4 taken */
    uint8_t *px;
    int w, h;
} thumb_t;

static thumb_t *g_th;
static int g_th_n;
static pl_mutex *th_mtx;
static pl_cond *th_cond;
static pl_thread *th_thread;
static int th_lo, th_hi = -1, th_center, th_max = 512;
/* the covers wanted, nearest the centre first (cdie_cover_wanted: a filtered wheel's neighbours are not next to
 * each other in the list); when set, it takes the place of th_lo..th_hi */
static int *th_want, th_nwant, th_want_on;
static bool th_quit;
static unsigned th_epoch; /* bumped whenever a picture finishes */

/* Area-average a w x h RGBA image down to dw x dh (never up). */
static uint8_t *make_thumb(const unsigned char *src, int w, int h, int dw, int dh)
{
    uint8_t *out = malloc((size_t)dw * dh * 4);
    if (!out)
        return NULL;
    for (int y = 0; y < dh; y++) {
        int y0 = (int)((long)y * h / dh), y1 = (int)((long)(y + 1) * h / dh);
        if (y1 <= y0) y1 = y0 + 1;
        for (int x = 0; x < dw; x++) {
            int x0 = (int)((long)x * w / dw), x1 = (int)((long)(x + 1) * w / dw);
            if (x1 <= x0) x1 = x0 + 1;
            unsigned long r = 0, g = 0, b = 0, a = 0;
            int count = 0;
            for (int yy = y0; yy < y1; yy++) {
                const unsigned char *p = src + ((size_t)yy * w + x0) * 4;
                for (int xx = x0; xx < x1; xx++, p += 4, count++) {
                    r += p[0]; g += p[1]; b += p[2]; a += p[3];
                }
            }
            uint8_t *d = out + ((size_t)y * dw + x) * 4;
            d[0] = (uint8_t)(r / count); d[1] = (uint8_t)(g / count); d[2] = (uint8_t)(b / count); d[3] = (uint8_t)(a / count);
        }
    }
    return out;
}

static uint8_t *load_thumb(const char *path, int *ow, int *oh)
{
    int w, h, n;
    unsigned char *img = stbi_load(path, &w, &h, &n, 4);
    if (!img)
        return NULL;
    int tw = w, ths = h;
    if (w > th_max || h > th_max) {
        if (w >= h) {
            tw = th_max;
            ths = (int)((long)h * th_max / w);
        } else {
            ths = th_max;
            tw = (int)((long)w * th_max / h);
        }
    }
    if (tw < 1) tw = 1;
    if (ths < 1) ths = 1;
    uint8_t *px;
    if (tw == w && ths == h) {
        px = malloc((size_t)w * h * 4);
        if (px)
            memcpy(px, img, (size_t)w * h * 4);
    } else {
        px = make_thumb(img, w, h, tw, ths);
    }
    stbi_image_free(img);
    *ow = tw;
    *oh = ths;
    return px;
}

static int th_loop(void *arg)
{
    (void)arg;
    for (;;) {
        pl_lock(th_mtx);
        int pick = -1;
        while (pick < 0) {
            if (th_quit) {
                pl_unlock(th_mtx);
                return 0;
            }
            int bd = 1 << 30;
            if (th_want_on) {
                for (int k = 0; k < th_nwant && pick < 0; k++)
                    if (th_want[k] >= 0 && th_want[k] < g_th_n && g_th[th_want[k]].state == 0)
                        pick = th_want[k];
            } else {
                for (int i = th_lo; i <= th_hi; i++)
                    if (i >= 0 && i < g_th_n && g_th[i].state == 0) {
                        int d = abs(i - th_center);
                        if (d < bd) { bd = d; pick = i; }
                    }
            }
            if (pick < 0)
                pl_cond_wait_ms(th_cond, th_mtx, 250);
        }
        char path[1400];
        snprintf(path, sizeof path, "%s", g_roms[pick].cover);
        g_th[pick].state = path[0] ? 1 : 3;
        pl_unlock(th_mtx);

        uint8_t *px = NULL;
        int w = 0, h = 0;
        if (path[0])
            px = load_thumb(path, &w, &h);

        pl_lock(th_mtx);
        if (path[0] && g_th[pick].state == 1) {
            if (px) {
                g_th[pick].px = px;
                g_th[pick].w = w;
                g_th[pick].h = h;
                g_th[pick].state = 2;
                px = NULL;
            } else {
                g_th[pick].state = 3;
                elogf("[host] cannot read cover %s", path);
            }
        }
        free(px);
        th_epoch++;
        pl_unlock(th_mtx);
    }
}

static void th_start(void)
{
    g_th_n = g_nroms;
    g_th = calloc((size_t)(g_nroms > 0 ? g_nroms : 1), sizeof *g_th);
    if (!th_mtx)
        th_mtx = pl_mutex_create();
    if (!th_cond)
        th_cond = pl_cond_create();
    th_quit = false;
    th_lo = 0;
    th_hi = -1;
    th_thread = pl_thread_create(th_loop, "covers", NULL);
}

static void th_stop(void)
{
    if (!th_thread)
        return;
    pl_lock(th_mtx);
    th_quit = true;
    pl_cond_signal(th_cond);
    pl_unlock(th_mtx);
    pl_thread_wait(th_thread);
    th_thread = NULL;
    for (int i = 0; i < g_th_n; i++)
        free(g_th[i].px);
    free(g_th);
    g_th = NULL;
    g_th_n = 0;
}

void cdie_cover_wanted(const int *idx, int n, int max_side)
{
    if (!th_mtx)
        return;
    pl_lock(th_mtx);
    int *w = realloc(th_want, (size_t)(n > 0 ? n : 1) * sizeof *w);
    if (w) {
        th_want = w;
        th_nwant = n;
        if (n > 0)
            memcpy(th_want, idx, (size_t)n * sizeof *idx);
        th_want_on = 1;
        th_center = n > 0 ? idx[0] : 0;
    }
    if (max_side >= 32 && max_side <= 4096)
        th_max = max_side;
    pl_cond_signal(th_cond);
    pl_unlock(th_mtx);
}

void cdie_cover_window(int centre, int radius, int max_side)
{
    if (!th_mtx)
        return;
    pl_lock(th_mtx);
    th_want_on = 0;
    th_center = centre;
    th_lo = centre - radius;
    th_hi = centre + radius;
    if (max_side >= 32 && max_side <= 4096)
        th_max = max_side;
    pl_cond_signal(th_cond);
    pl_unlock(th_mtx);
}

int cdie_cover_take(int *index, int *w, int *h, uint8_t **rgba)
{
    int got = 0;
    if (!th_mtx)
        return 0;
    pl_lock(th_mtx);
    int pick = -1, bd = 1 << 30;
    for (int i = 0; i < g_th_n; i++)
        if (g_th[i].state == 2) {
            int d = abs(i - th_center);
            if (d < bd) { bd = d; pick = i; }
        }
    if (pick >= 0) {
        *index = pick;
        *w = g_th[pick].w;
        *h = g_th[pick].h;
        *rgba = g_th[pick].px;
        g_th[pick].px = NULL;
        g_th[pick].state = 4;
        got = 1;
    }
    pl_unlock(th_mtx);
    return got;
}

void cdie_cover_forget(int index)
{
    if (!th_mtx)
        return;
    pl_lock(th_mtx);
    if (index >= 0 && index < g_th_n && g_th[index].state == 4)
        g_th[index].state = 0;
    pl_unlock(th_mtx);
}

/* A picture the program carries itself (its logo): PNG/JPEG bytes to RGBA, top row first, freed with cdie_free() */
uint8_t *cdie_decode_image(const void *data, int len, int *w, int *h)
{
    int n = 0;
    *w = *h = 0;
    if (!data || len <= 0)
        return NULL;
    return stbi_load_from_memory((const stbi_uc *)data, len, w, h, &n, 4);
}

void cdie_free(void *p)
{
    free(p);
}

/* One picture, from either of the two servers (the one that worked last first). An HTTP answer (200, 404...) ends
 * the search; only failures to connect move on. *unchecked is always 1: our client does not check certificates,
 * so the caller checks the picture against its hash. */
static int fetch_cover(const char *art, unsigned char **out, size_t *len, int *unchecked)
{
    const int np = 2;
    const int start = cv_plan_good >= 0 && cv_plan_good < np ? cv_plan_good : 0;
    char enc[600], url[800], why[200], last_why[160] = "";
    int saw404 = 0;
    url_encode(art, enc, sizeof enc);
    *unchecked = 1;
    for (int k = 0; k < np && !cv_quit; k++) {
        int pi = (start + k) % np;
        snprintf(url, sizeof url, "%s%s", cv_prefix[pi], enc);
        why[0] = 0;
        int status = own_get(url, out, len, why, sizeof why);
        if (status > 0 && status != 200 && status != 404)
            snprintf(why, sizeof why, "HTTP status %d", status); /* e.g. a filter's block page: try the other server */
        cvlog("%s: %s", cv_hostname[pi], status == 200 ? "200 ok" : status == 404 ? "404 not found" : why);
        if (status == 200) {
            cv_plan_good = pi;
            return status;
        }
        free(*out);
        *out = NULL;
        if (status == 404) {
            saw404 = 1;
            if (pi == 0) { /* the list comes from this very repository: it is really gone */
                cv_plan_good = pi;
                return 404;
            }
        }
        snprintf(last_why, sizeof last_why, "%s", why);
    }
    if (saw404)
        return 404;
    snprintf(cv_msg, sizeof cv_msg, "%s", last_why[0] ? last_why : "no connection");
    return -1;
}

/* A 404 is remembered for two weeks so a missing picture is not asked for again. */
static void missing_path(const rom_t *r, char *out, size_t n)
{
    snprintf(out, n, "%s/.missing/%s.txt", g_covers_dir, r->base);
}

static bool recently_missing(const rom_t *r)
{
    char p[1300];
    struct stat st;
    missing_path(r, p, sizeof p);
    return stat(p, &st) == 0 && time(NULL) - st.st_mtime < 14 * 24 * 3600;
}

static bool write_atomic(const char *path, const unsigned char *data, size_t len)
{
    char tmp[1300];
    snprintf(tmp, sizeof tmp, "%s.part", path);
    FILE *f = fopen(tmp, "wb");
    if (!f)
        return false;
    bool ok = fwrite(data, 1, len, f) == len;
    ok = (fclose(f) == 0) && ok;
    if (!ok || rename(tmp, path) != 0) {
        remove(tmp);
        return false;
    }
    return true;
}

static int cv_loop(void *arg)
{
    (void)arg;
    for (;;) {
        /* the pending game nearest the selection goes first */
        int pick = -1, bd = 1 << 30;
        pl_lock(th_mtx);
        for (int i = 0; i < g_nroms; i++)
            if (cv_cand[i] >= 0 && !g_roms[i].cover[0]) {
                int d = abs(i - th_center);
                if (d < bd) { bd = d; pick = i; }
            }
        pl_unlock(th_mtx);
        if (pick < 0 || cv_quit)
            break;

        const int art_idx = cv_cand[pick];
        const char *art = cdi_boxart_list[art_idx].name;
        unsigned char *data;
        size_t len;
        int unchecked = 0;
        int status = fetch_cover(art, &data, &len, &unchecked);
#ifndef PS5
        if (status == 200 && data && len > 20 && getenv("CDIHOST_TEST_CORRUPT"))
            data[16] ^= 1; /* test: damage the picture in transit */
#endif
        if (status == 200) { /* the picture must be the one in the list */
            if (boxart_hash_ok(art_idx, data, len)) {
                cvlog("%s: hash matches the list", g_roms[pick].base);
            } else if (unchecked) {
                cvlog("%s: REJECTED - hash differs from the list and the connection was not checked", g_roms[pick].base);
                free(data);
                data = NULL;
                status = -2;
            } else {
                cvlog("%s: hash differs from the list (picture updated upstream?), accepted over a checked connection",
                      g_roms[pick].base);
            }
        }
        bool saved = false;
        char path[1400];
        const bool path_ok = snprintf(path, sizeof path, "%s/%s.png", g_covers_dir, g_roms[pick].base) < (int)sizeof path;
        if (path_ok && status == 200 && len > 8 && !memcmp(data, "\x89PNG", 4))
            saved = write_atomic(path, data, len);
        free(data);
        cvlog("%s <- %s: %d%s", g_roms[pick].base, art, status, saved ? " (saved)" : "");

        if (status == -1 && !cv_quit) { /* nothing reachable: give up for this session */
            cv_net_down = 1;
            cvlog("giving up: %s", cv_msg);
            notify("cdihost: covers: %s", cv_msg);
            pl_lock(th_mtx);
            th_epoch++;
            pl_unlock(th_mtx);
            break;
        }
        if (status == 404) {
            char m[1300], dir[1100];
            missing_path(&g_roms[pick], m, sizeof m);
            snprintf(dir, sizeof dir, "%s/.missing", g_covers_dir);
            mkdir(dir, 0777);
            write_atomic(m, (const unsigned char *)"404\n", 4);
        }
        pl_lock(th_mtx);
        cv_cand[pick] = -1; /* done with it, whatever the outcome */
        if (saved) {
            snprintf(g_roms[pick].cover, sizeof g_roms[pick].cover, "%s", path);
            if (g_th && g_th[pick].state == 3)
                g_th[pick].state = 0; /* now there is something to load */
        } else {
            cv_failed++;
        }
        cv_done++;
        th_epoch++;
        pl_cond_signal(th_cond);
        pl_unlock(th_mtx);
    }
    http_term();
    return 0;
}

/* The worker thread first works out which games lack a cover and have a box art match (a second or so for a
 * big library, so not while the launcher opens), then downloads them. */
static int cv_run(void *arg)
{
    (void)arg;
    for (int i = 0; i < g_nroms && !cv_quit; i++) {
        cv_cand[i] = -1;
        if (g_roms[i].cover[0] || recently_missing(&g_roms[i]))
            continue;
        double raw;
        int a = match_boxart(g_roms[i].base, &raw);
        if (a >= 0 && raw >= COVER_MIN_SCORE) {
            cv_cand[i] = a;
            cv_total++;
        }
    }
    cv_matched = 1;
    if (cv_total == 0 || cv_quit)
        return 0;
    cvlog("%d cover(s) to download%s", cv_total, cv_secure_only ? " (secure connections only)" : "");
    tls_trace = cv_trace;
    return cv_loop(NULL);
}

static void cv_start(void)
{
    cv_quit = cv_total = cv_done = cv_failed = cv_net_down = cv_matched = 0;
    cv_msg[0] = 0;
    if (!g_download_ok || g_nroms == 0)
        return;
    cv_secure_only = false;
#ifdef PS5
    cv_secure_only = access("/data/ps5-cdi/cover_secure_only", F_OK) == 0;
#else
    cv_secure_only = getenv("CDIHOST_SECURE_ONLY") != NULL;
#endif
    cv_cand = malloc((size_t)g_nroms * sizeof *cv_cand);
    if (!cv_cand)
        return;
    for (int i = 0; i < g_nroms; i++)
        cv_cand[i] = -1;
    if (cv_logf)
        fclose(cv_logf);
    {
        char lp[1100];
        snprintf(lp, sizeof lp, "%s/cover_log.txt", g_save_dir);
        cv_logf = fopen(lp, "w");
    }
    cv_thread = pl_thread_create(cv_run, "covers-net", NULL);
}

static void cv_stop(void)
{
    if (cv_thread) {
        cv_quit = 1;
        http_abort();
        pl_thread_wait(cv_thread);
        cv_thread = NULL;
    }
    free(cv_cand);
    cv_cand = NULL;
    if (cv_logf) {
        fclose(cv_logf);
        cv_logf = NULL;
    }
}

/* ------------------------------------------------------------- a game card's extras
 * When a game's card opens, the program asks for its extras:
 *   - a screenshot, the card's backdrop, and the game's logo, from libretro-thumbnails (Named_Snaps, Named_Logos),
 *     each checked against its known hash as box art is (host/cdi_extras.h, tools/gen_extras_index.py);
 *   - what Wikipedia says about it: its article's lead section and the scores of its review table
 *     (host/gameinfo.c), only from an article that is about a video game.
 * Everything is kept in <covers>/extras/ (snaps/, logos/, info/), so a card opens with them at once the next time;
 * what was not found is not asked for again for two weeks. One game at a time, on a thread of its own: the newest
 * wish wins, and a game whose card was closed meanwhile is dropped. */
#include "cdi_extras.h"
#include "gameinfo.h"

static pl_mutex *ex_mtx;
static pl_cond *ex_cond;
static pl_thread *ex_thread;
static volatile int ex_quit;
static int ex_want = -1;           /* the game wanted (engine index), -1 none */
static int ex_done_for = -1;       /* the last game finished */
static char ex_want_title[256];    /* its title as the list of CD-i games spells it */
static struct { int index, kind, w, h; uint8_t *rgba; } ex_pics[4];
static int ex_npics;
static cdie_extras_info ex_info = {.index = -1};
static parsed_t *ex_snap_parsed, *ex_logo_parsed;

static const char *snap_name(int i) { return cdi_snap_list[i].name; }
static const char *logo_name(int i) { return cdi_logo_list[i].name; }

static void exlog(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void exlog(const char *fmt, ...)
{
    char line[600];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    elogf("[host] extras: %s\n", line);
}

static bool ex_still_wanted(int idx)
{
    pl_lock(ex_mtx);
    const bool yes = ex_want == idx && !ex_quit;
    pl_unlock(ex_mtx);
    return yes;
}

/* HTTPS GET with our own client, the way the covers are fetched; 200: *out holds the body */
static int ex_get(const char *url, unsigned char **out, size_t *len, char *why, size_t wn)
{
    char mapped[1200];
    *out = NULL;
    *len = 0;
    if (!g_download_ok) { /* Settings: no downloads (what is cached still shows) */
        snprintf(why, wn, "downloads are off");
        return -1;
    }
    if (ex_quit || !net_init(why, wn))
        return -1;
    if (!(url = test_map_url(url, mapped, sizeof mapped, why, wn)))
        return -1;
    return tls_https_get(url, net_resolve, &ex_quit, 30000, out, len, why, wn);
}

static bool read_file(const char *path, unsigned char **out, size_t *len)
{
    *out = NULL;
    *len = 0;
    FILE *f = fopen(path, "rb");
    if (!f)
        return false;
    fseek(f, 0, SEEK_END);
    const long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n <= 0 || n > (32L << 20) || !(*out = malloc((size_t)n + 1))) {
        fclose(f);
        return false;
    }
    *len = fread(*out, 1, (size_t)n, f);
    (*out)[*len] = 0;
    fclose(f);
    return *len == (size_t)n;
}

static bool fresh_marker(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0 && time(NULL) - st.st_mtime < 14 * 24 * 3600;
}

/* One picture of the game (kind 0 the screenshot, 1 the logo): from the cache, else downloaded and checked */
static void ex_picture(int idx, const char *base, int kind)
{
    const char *folder = kind ? "Named_Logos" : "Named_Snaps";
    char dir[1200], path[1500], miss[1500];
    snprintf(dir, sizeof dir, "%s/extras/%s", g_covers_dir, kind ? "logos" : "snaps");
    mkdir(dir, 0777);
    snprintf(path, sizeof path, "%s/%s.png", dir, base);
    snprintf(miss, sizeof miss, "%s/%s.none", dir, base);
    unsigned char *data = NULL;
    size_t len = 0;
    if (!read_file(path, &data, &len)) {
        if (fresh_marker(miss))
            return;
        double raw = 0;
        const int count = kind ? CDI_LOGOS_COUNT : CDI_SNAPS_COUNT;
        const int m = match_names(base, &raw, count, kind ? logo_name : snap_name, kind ? &ex_logo_parsed : &ex_snap_parsed);
        if (m < 0 || raw < COVER_MIN_SCORE) {
            exlog("%s: no %s matches", base, kind ? "logo" : "screenshot");
            write_atomic(miss, (const unsigned char *)"none\n", 5);
            return;
        }
        const char *name = kind ? cdi_logo_list[m].file : cdi_snap_list[m].file;
        const unsigned char *sha = kind ? cdi_logo_list[m].sha1 : cdi_snap_list[m].sha1;
        static const char *const prefix[] = {"https://raw.githubusercontent.com/libretro-thumbnails/Philips_-_CD-i/master/",
                                             "https://thumbnails.libretro.com/Philips%20-%20CD-i/"};
        char enc[700], url[1000], why[200];
        url_encode(name, enc, sizeof enc);
        int status = -1;
        for (int k = 0; k < 2 && status != 200 && status != 404; k++) {
            snprintf(url, sizeof url, "%s%s/%s", prefix[k], folder, enc);
            why[0] = 0;
            status = ex_get(url, &data, &len, why, sizeof why);
            exlog("%s: %s %s: %s", base, kind ? "logo" : "screenshot", k ? "libretro" : "github",
                  status == 200 ? "200 ok" : status > 0 ? "not there" : why);
            if (status != 200) {
                free(data);
                data = NULL;
            }
        }
        if (status == 404) {
            write_atomic(miss, (const unsigned char *)"404\n", 4);
            return;
        }
        if (status != 200 || !data)
            return; /* no network: asked again next time */
        if (!blob_hash_ok(sha, data, len) || len < 8 || memcmp(data, "\x89PNG", 4)) {
            exlog("%s: %s REJECTED - not the picture the list names", base, kind ? "logo" : "screenshot");
            free(data);
            return;
        }
        write_atomic(path, data, len);
    }
    int w = 0, h = 0, n = 0;
    uint8_t *rgba = stbi_load_from_memory(data, (int)len, &w, &h, &n, 4);
    free(data);
    if (!rgba || w <= 0 || h <= 0) {
        stbi_image_free(rgba);
        return;
    }
    pl_lock(ex_mtx);
    if (ex_want == idx && ex_npics < (int)(sizeof ex_pics / sizeof ex_pics[0])) {
        ex_pics[ex_npics].index = idx;
        ex_pics[ex_npics].kind = kind;
        ex_pics[ex_npics].w = w;
        ex_pics[ex_npics].h = h;
        ex_pics[ex_npics].rgba = rgba;
        ex_npics++;
        rgba = NULL;
    }
    pl_unlock(ex_mtx);
    stbi_image_free(rgba);
}

/* Wikipedia's article on `title`, if it is about a video game: fills about, reviews and page. 1 found, 0 not a
 * game or no such page, -1 no answer (the network) */
static int ex_wiki_page(const char *title, cdie_extras_info *info)
{
    char enc[800], url[1400], why[200];
    gi_url_encode(title, enc, sizeof enc);
    snprintf(url, sizeof url,
             "https://en.wikipedia.org/w/api.php?action=query&format=json&formatversion=2&redirects=1"
             "&prop=extracts%%7Cdescription%%7Crevisions&exintro=1&explaintext=1&rvprop=content&rvslots=main&titles=%s",
             enc);
    unsigned char *body = NULL;
    size_t len = 0;
    const int status = ex_get(url, &body, &len, why, sizeof why);
    if (status != 200 || !body) {
        exlog("wikipedia \"%s\": %s", title, status > 0 ? "HTTP error" : why);
        free(body);
        return status > 0 ? 0 : -1;
    }
    const char *j = (const char *)body;
    int found = 0;
    if (!gi_json_true(j, len, "missing") && !gi_json_true(j, len, "invalid")) {
        char desc[300], page[256];
        char *extract = malloc(8192), *content = malloc(len + 1);
        if (extract && content) {
            gi_json_string(j, len, "description", desc, sizeof desc);
            gi_json_string(j, len, "title", page, sizeof page);
            gi_json_string(j, len, "extract", extract, 8192);
            if (extract[0] && gi_is_game(desc, extract)) {
                gi_json_string(j, len, "content", content, len + 1);
                gi_clean_extract(extract, 700);
                snprintf(info->about, sizeof info->about, "%s", extract);
                gi_reviews(content, info->reviews, sizeof info->reviews, 4);
                snprintf(info->source, sizeof info->source, "From Wikipedia's article \"%s\" (CC BY-SA 4.0)", page);
                found = 1;
            }
            exlog("wikipedia \"%s\": %s%s%s", title, found ? "about a game" : "not about a game",
                  found && info->reviews[0] ? ", reviews: " : "", found ? info->reviews : "");
        }
        free(extract);
        free(content);
    } else {
        exlog("wikipedia \"%s\": no such article", title);
    }
    free(body);
    return found;
}

/* The article: by the title the list of CD-i games gives, then by the shown name, then Wikipedia's own search */
static int ex_wiki(const char *title, const char *name, cdie_extras_info *info)
{
    int r = title[0] ? ex_wiki_page(title, info) : 0;
    if (r != 0)
        return r;
    if (strcasecmp(title, name) != 0 && (r = ex_wiki_page(name, info)) != 0)
        return r;
    char q[400], enc[900], url[1400], why[200];
    snprintf(q, sizeof q, "%s CD-i", title[0] ? title : name);
    gi_url_encode(q, enc, sizeof enc);
    snprintf(url, sizeof url,
             "https://en.wikipedia.org/w/api.php?action=query&format=json&formatversion=2&list=search&srnamespace=0"
             "&srlimit=3&srsearch=%s", enc);
    unsigned char *body = NULL;
    size_t len = 0;
    if (ex_get(url, &body, &len, why, sizeof why) != 200 || !body) {
        free(body);
        return -1;
    }
    char found[3][256];
    const int n = gi_search_titles((const char *)body, len, found, 3);
    free(body);
    for (int i = 0; i < n; i++)
        if ((r = ex_wiki_page(found[i], info)) != 0)
            return r;
    return 0;
}

static void ex_info_cache_path(const char *base, char *out, size_t n)
{
    snprintf(out, n, "%s/extras/info/%s.txt", g_covers_dir, base);
}

/* the cache file: "page=", "about=", "reviews=" lines (a line's text has no line breaks) */
static bool ex_info_load(const char *base, cdie_extras_info *info)
{
    char path[1500];
    ex_info_cache_path(base, path, sizeof path);
    struct stat st;
    if (stat(path, &st) != 0)
        return false;
    FILE *f = fopen(path, "r");
    if (!f)
        return false;
    char line[2048];
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        if (!strncmp(line, "about=", 6))
            snprintf(info->about, sizeof info->about, "%s", line + 6);
        else if (!strncmp(line, "reviews=", 8))
            snprintf(info->reviews, sizeof info->reviews, "%s", line + 8);
        else if (!strncmp(line, "source=", 7))
            snprintf(info->source, sizeof info->source, "%s", line + 7);
    }
    fclose(f);
    /* nothing found is asked again after two weeks */
    return info->about[0] || time(NULL) - st.st_mtime < 14 * 24 * 3600;
}

static void ex_info_save(const char *base, const cdie_extras_info *info)
{
    char dir[1200], path[1500], text[4096];
    snprintf(dir, sizeof dir, "%s/extras/info", g_covers_dir);
    mkdir(dir, 0777);
    ex_info_cache_path(base, path, sizeof path);
    const int n = snprintf(text, sizeof text, "about=%s\nreviews=%s\nsource=%s\n", info->about, info->reviews, info->source);
    if (n > 0 && (size_t)n < sizeof text)
        write_atomic(path, (const unsigned char *)text, (size_t)n);
}

static void ex_work(int idx, const char *base, const char *name, const char *title)
{
    char dir[1200];
    snprintf(dir, sizeof dir, "%s/extras", g_covers_dir);
    mkdir(g_covers_dir, 0777);
    mkdir(dir, 0777);
    /* the words first: they are small, and the card has room for them at once */
    cdie_extras_info info;
    memset(&info, 0, sizeof info);
    info.index = idx;
    if (!ex_info_load(base, &info)) {
        memset(&info, 0, sizeof info);
        info.index = idx;
        const int r = ex_wiki(title, name, &info);
        if (r >= 0)
            ex_info_save(base, &info); /* found or not (not: asked again in two weeks); no network: next time */
        info.state = r < 0 ? 2 : 1;
    } else {
        info.state = 1;
    }
    pl_lock(ex_mtx);
    if (ex_want == idx)
        ex_info = info;
    pl_unlock(ex_mtx);
    if (!ex_still_wanted(idx))
        return;
    ex_picture(idx, base, 0);
    if (!ex_still_wanted(idx))
        return;
    ex_picture(idx, base, 1);
}

static int ex_run(void *arg)
{
    (void)arg;
    for (;;) {
        pl_lock(ex_mtx);
        while (!ex_quit && (ex_want < 0 || ex_want == ex_done_for))
            pl_cond_wait(ex_cond, ex_mtx);
        if (ex_quit) {
            pl_unlock(ex_mtx);
            break;
        }
        const int idx = ex_want;
        char title[256];
        snprintf(title, sizeof title, "%s", ex_want_title);
        pl_unlock(ex_mtx);
        char base[200] = "", name[200] = "";
        pl_lock(th_mtx);
        if (idx < g_nroms) {
            snprintf(base, sizeof base, "%s", g_roms[idx].base);
            snprintf(name, sizeof name, "%s", g_roms[idx].name);
        }
        pl_unlock(th_mtx);
        if (base[0])
            ex_work(idx, base, name, title);
        pl_lock(ex_mtx);
        ex_done_for = idx;
        pl_unlock(ex_mtx);
    }
    return 0;
}

void cdie_extras_want(int index, const char *title)
{
    if (!ex_mtx) {
        ex_mtx = pl_mutex_create();
        ex_cond = pl_cond_create();
    }
    pl_lock(ex_mtx);
    if (index != ex_want) {
        ex_want = index;
        snprintf(ex_want_title, sizeof ex_want_title, "%s", title ? title : "");
        ex_done_for = -1;
        ex_info.index = -1;
        /* pictures of a game no longer wanted are dropped */
        int keep = 0;
        for (int i = 0; i < ex_npics; i++) {
            if (ex_pics[i].index == index)
                ex_pics[keep++] = ex_pics[i];
            else
                stbi_image_free(ex_pics[i].rgba);
        }
        ex_npics = keep;
        pl_cond_signal(ex_cond);
    }
    const bool start = index >= 0 && !ex_thread; /* (with downloads off it still reads the cache) */
    pl_unlock(ex_mtx);
    if (start) {
        ex_quit = 0;
        ex_thread = pl_thread_create(ex_run, "extras-net", NULL);
    }
}

int cdie_extras_info_get(int index, cdie_extras_info *out)
{
    if (!ex_mtx || index < 0)
        return 0;
    pl_lock(ex_mtx);
    const int have = ex_info.index == index;
    if (have)
        *out = ex_info;
    pl_unlock(ex_mtx);
    return have;
}

int cdie_extras_take(int *index, int *kind, int *w, int *h, uint8_t **rgba)
{
    if (!ex_mtx)
        return 0;
    pl_lock(ex_mtx);
    const int have = ex_npics > 0;
    if (have) {
        *index = ex_pics[0].index;
        *kind = ex_pics[0].kind;
        *w = ex_pics[0].w;
        *h = ex_pics[0].h;
        *rgba = ex_pics[0].rgba;
        memmove(ex_pics, ex_pics + 1, (size_t)(ex_npics - 1) * sizeof ex_pics[0]);
        ex_npics--;
    }
    pl_unlock(ex_mtx);
    return have;
}

static void ex_stop(void)
{
    if (!ex_thread)
        return;
    pl_lock(ex_mtx);
    ex_quit = 1;
    pl_cond_signal(ex_cond);
    pl_unlock(ex_mtx);
    pl_thread_wait(ex_thread);
    ex_thread = NULL;
    pl_lock(ex_mtx);
    for (int i = 0; i < ex_npics; i++)
        stbi_image_free(ex_pics[i].rgba);
    ex_npics = 0;
    ex_want = ex_done_for = -1;
    pl_unlock(ex_mtx);
}

/* ------------------------------------------------- games on USB / external drives */

/* A drive plugged into the console shows up under /mnt/usb0 ... /mnt/usb7 (extended storage: /mnt/ext0,
 * /mnt/ext1), formatted exFAT or FAT32. On each one the first of these folders that exists is searched
 * (three levels deep): ps5-cdi/roms, ps5-cdi, cdi, CDi, CD-i, roms/cdi, roms. Games found there are played
 * from the drive itself (it is random-access storage, so there is no copy); a .7z is unpacked into the
 * cache as usual. A game with the same file name that is already in the roms folder wins.
 * <options dir>/drives.cfg can change this, one key=value per line (# starts a comment):
 *     folder=Games/CDi      folders to look for on each drive; given once or more, they replace the list above
 *     extra=/mnt/ext0/cdi   another full path to search (as many as you like)
 *     enabled=0             do not look at external drives at all
 * The launcher rescans with L3, for a drive plugged in after the app started. */

static int g_drv_enabled = 1;
static char g_drv_folders[8][128];
static int g_drv_nfolders;
static char g_drv_extra[8][512];
static int g_drv_nextra;

static void drives_cfg_load(const char *dir)
{
    char path[1400], line[700];
    snprintf(path, sizeof path, "%s/drives.cfg", dir);
    FILE *f = fopen(path, "r");
    if (!f)
        return;
    while (fgets(line, sizeof line, f)) {
        char *c = line;
        while (*c == ' ' || *c == '\t')
            c++;
        if (*c == '#' || *c == '\n' || *c == '\r' || !*c)
            continue;
        c[strcspn(c, "\r\n")] = 0;
        char *eq = strchr(c, '=');
        if (!eq)
            continue;
        *eq = 0;
        const char *k = c, *v = eq + 1;
        if (!strcmp(k, "folder"))
            while (*v == '/')
                v++; /* folder names are relative to the drive */
        if (!strcmp(k, "folder") && g_drv_nfolders < 8 && *v)
            snprintf(g_drv_folders[g_drv_nfolders++], sizeof g_drv_folders[0], "%s", v);
        else if (!strcmp(k, "extra") && g_drv_nextra < 8 && *v)
            snprintf(g_drv_extra[g_drv_nextra++], sizeof g_drv_extra[0], "%s", v);
        else if (!strcmp(k, "enabled"))
            g_drv_enabled = atoi(v) != 0;
    }
    fclose(f);
}

static bool is_dir_path(const char *p)
{
    struct stat st;
    return stat(p, &st) == 0 && S_ISDIR(st.st_mode);
}

/* The drives to look at: label and mount point. */
static int drive_list(char labels[][16], char mounts[][512], int max)
{
    int n = 0;
    const char *test = getenv("CDIHOST_TEST_MOUNTS"); /* tests: mount points separated by ':' */
#ifdef CDIHOST_TEST_DUMP
    { /* tests: from the second scan on, CDIHOST_TEST_MOUNTS_AFTER replaces the list (a drive plugged in later) */
        static int calls;
        if (calls++ > 0 && getenv("CDIHOST_TEST_MOUNTS_AFTER"))
            test = getenv("CDIHOST_TEST_MOUNTS_AFTER");
    }
#endif
    if (test) {
        char tmp[2000];
        snprintf(tmp, sizeof tmp, "%s", test);
        for (char *tok = strtok(tmp, ":"); tok && n < max; tok = strtok(NULL, ":")) {
            const char *b = strrchr(tok, '/');
            snprintf(mounts[n], 512, "%s", tok);
            snprintf(labels[n], 16, "%.15s", b ? b + 1 : tok);
            n++;
        }
        return n;
    }
#ifdef PS5
    for (int i = 0; i < 8 && n < max; i++) {
        snprintf(mounts[n], 512, "/mnt/usb%d", i);
        snprintf(labels[n], 16, "usb%d", i);
        if (is_dir_path(mounts[n]))
            n++;
    }
    for (int i = 0; i < 2 && n < max; i++) {
        snprintf(mounts[n], 512, "/mnt/ext%d", i);
        snprintf(labels[n], 16, "ext%d", i);
        if (is_dir_path(mounts[n]))
            n++;
    }
#else
    /* a desktop: the usual places removable drives appear, /media/<user>/<name> and /run/media/<user>/<name> */
    const char *roots[2] = {"/media", "/run/media"};
    for (int r = 0; r < 2; r++) {
        DIR *u = opendir(roots[r]);
        struct dirent *eu;
        while (u && (eu = readdir(u)) && n < max) {
            if (eu->d_name[0] == '.')
                continue;
            char userdir[512];
            snprintf(userdir, sizeof userdir, "%s/%s", roots[r], eu->d_name);
            DIR *d = opendir(userdir);
            struct dirent *e;
            while (d && (e = readdir(d)) && n < max) {
                if (e->d_name[0] == '.')
                    continue;
                snprintf(mounts[n], 512, "%.200s/%.250s", userdir, e->d_name);
                snprintf(labels[n], 16, "%.15s", e->d_name);
                if (is_dir_path(mounts[n]))
                    n++;
            }
            if (d)
                closedir(d);
        }
        if (u)
            closedir(u);
    }
#endif
    return n;
}

/* Scans one folder, then marks what it found with the drive's label and drops duplicates. */
static void drive_scan_folder(const char *folder, const char *label)
{
    const int n0 = g_nroms;
    scan_dir(folder, 0, true);
    int out = n0;
    for (int i = n0; i < g_nroms; i++) {
        bool dup = false;
        for (int k = 0; k < n0 && !dup; k++) /* a game already in the list wins (local first) */
            dup = !strcmp(g_roms[k].base, g_roms[i].base);
        for (int k = n0; k < out && !dup; k++)
            dup = !strcmp(g_roms[k].base, g_roms[i].base);
        if (dup)
            continue;
        snprintf(g_roms[i].drive, sizeof g_roms[i].drive, "%s", label);
        if (out != i)
            g_roms[out] = g_roms[i];
        out++;
    }
    g_nroms = out;
}

static void drives_scan(void)
{
    static const char *const defaults[] = {"ps5-cdi/roms", "ps5-cdi", "cdi", "CDi", "CD-i", "roms/cdi", "roms"};
    char labels[16][16], mounts[16][512];
    if (!g_drv_enabled)
        return;
    const int before = g_nroms;
    const int nd = drive_list(labels, mounts, 16);
    for (int d = 0; d < nd; d++) {
        char folder[800] = "";
        const int nf = g_drv_nfolders ? g_drv_nfolders : (int)(sizeof defaults / sizeof *defaults);
        for (int f = 0; f < nf; f++) {
            snprintf(folder, sizeof folder, "%.500s/%.200s", mounts[d], g_drv_nfolders ? g_drv_folders[f] : defaults[f]);
            if (is_dir_path(folder))
                break;
            folder[0] = 0;
        }
        if (!folder[0]) {
            elogf("[host] drive %s (%s): no games folder (looked for %s)\n", labels[d], mounts[d],
                    g_drv_nfolders ? "the folders in drives.cfg" : "ps5-cdi/roms, ps5-cdi, cdi, CDi, CD-i, roms/cdi, roms");
            continue;
        }
        const int b = g_nroms;
        drive_scan_folder(folder, labels[d]);
        elogf("[host] drive %s: %d game(s) in %s\n", labels[d], g_nroms - b, folder);
    }
    for (int x = 0; x < g_drv_nextra; x++)
        if (is_dir_path(g_drv_extra[x])) {
            const int b = g_nroms;
            drive_scan_folder(g_drv_extra[x], "ext");
            elogf("[host] extra folder: %d game(s) in %s\n", g_nroms - b, g_drv_extra[x]);
        }
    if (g_nroms > before)
        notify("cdihost: %d game(s) found on external drives", g_nroms - before);
}

/* ------------------------------------------------- games on a NAS (NFS) and packed games (7z) */

/* Files the settings screen writes (nfs.cfg, dns.txt, no_cover_download). They belong in the data folder, but
 * when that folder was made by something else (FTP, or the payload, as another user) the title may not be
 * allowed to write there; then they go in the saves folder, which the title always writes. A copy in saves wins
 * when both exist. */
static void setting_path(char *out, size_t n, const char *dir, const char *name)
{
    char s[1400];
    snprintf(s, sizeof s, "%s/%s", g_save_dir, name);
    if (access(s, F_OK) == 0)
        snprintf(out, n, "%s", s);
    else
        snprintf(out, n, "%s/%s", dir, name);
}

static int setting_write_in(const char *dir, const char *name, const char *text, char *why, size_t wn)
{
    char path[1400], tmp[1500];
    snprintf(path, sizeof path, "%s/%s", dir, name);
    snprintf(tmp, sizeof tmp, "%s.part", path);
    chmod(dir, 0777); /* (only works when the title made it; harmless otherwise) */
    FILE *f = fopen(tmp, "w");
    if (f) {
        const int ok = fputs(text, f) >= 0;
        if (fclose(f) == 0 && ok && rename(tmp, path) == 0) {
            chmod(path, 0666);
            return 0;
        }
        snprintf(why, wn, "%s: %s", path, strerror(errno));
        remove(tmp);
    } else {
        snprintf(why, wn, "%s: %s", tmp, strerror(errno));
    }
    f = fopen(path, "w"); /* the folder is closed to the title, but the file itself may be open to it */
    if (f) {
        const int ok = fputs(text, f) >= 0;
        if (fclose(f) == 0 && ok)
            return 0;
    }
    return -1;
}

static char g_last_error[300]; /* (defined below) */
/* Writes <dir>/<name>, or <saves>/<name> when the data folder cannot be written. 0 = written. */
static int setting_write(const char *dir, const char *name, const char *text)
{
    char why[1600] = "", why2[1600] = "", saved[1400];
    snprintf(saved, sizeof saved, "%s/%s", g_save_dir, name);
    if (setting_write_in(dir, name, text, why, sizeof why) == 0) {
        remove(saved); /* an older copy in saves would win over this one */
        return 0;
    }
    if (setting_write_in(g_save_dir, name, text, why2, sizeof why2) == 0) {
        elogf("[host] could not write %s (%s): written to %s instead", name, why, saved);
        return 0;
    }
    snprintf(g_last_error, sizeof g_last_error, "could not write %s (%s; %s)", name, why, why2);
    elogf("[host] %s", g_last_error);
    return -1;
}

static void setting_remove(const char *dir, const char *name)
{
    char p[1400];
    snprintf(p, sizeof p, "%s/%s", dir, name);
    remove(p);
    snprintf(p, sizeof p, "%s/%s", g_save_dir, name);
    remove(p);
}

/* <options dir>/nfs.cfg, one key=value per line (# starts a comment):
 *     server=192.168.1.20          IPv4 address or name of the NAS
 *     export=/volume1/games        the NFS export (as `showmount -e` lists it)
 *     path=cdi                     folder inside the export to search (optional)
 *     uid=1000  gid=1000           user and group to act as (optional)
 *     cache_gb=10                  how much local space copied/unpacked games may use (default 10)
 *     cache_dir=/data/ps5-cdi/cache
 * The games found there (.cue, .chd, .iso, .7z) are listed with the local ones, tagged NAS. A game on
 * the NAS is copied to the cache folder when it is started (a CHD needs random access, which a network
 * share cannot give the core), with a progress screen; a copy that is already complete is reused.
 * A .7z (on the NAS or in the roms folder) is unpacked into the cache folder the same way and the
 * .cue / .chd / .iso inside is played. Games not played for the longest are deleted to stay under
 * cache_gb. The cache settings in nfs.cfg also apply to .7z files when there is no NAS. */

typedef struct {
    int enabled;
    char server[128], export_path[256], path[512];
    int uid, gid, cache_gb;
    int version, nfsport, mountport, reserved, timeout_s; /* see nfs.cfg */
    int backup;                                            /* backup_saves=1: copy saves/ to the share */
} nas_cfg_t;
static nas_cfg_t g_nas = {.uid = -1, .gid = -1, .cache_gb = 10, .reserved = 1, .timeout_s = 10};
static char g_cache_dir[1024] = DEFAULT_CACHE_DIR;

static int g_nas_off; /* nfs.cfg says enabled=0: kept, but not used */

static void nas_cfg_load(const char *dir)
{
    char path[1400], line[600];
    setting_path(path, sizeof path, dir, "nfs.cfg");
    const nas_cfg_t defaults = {.uid = -1, .gid = -1, .cache_gb = 10, .reserved = 1, .timeout_s = 10};
    g_nas = defaults; /* read again from nothing: the settings screen rewrites the file */
    g_nas_off = 0;
    FILE *f = fopen(path, "r");
    if (!f)
        return;
    while (fgets(line, sizeof line, f)) {
        char *c = line;
        while (*c == ' ' || *c == '\t')
            c++;
        if (*c == '#' || *c == '\n' || *c == '\r' || !*c)
            continue;
        c[strcspn(c, "\r\n")] = 0;
        char *eq = strchr(c, '=');
        if (!eq)
            continue;
        *eq = 0;
        const char *k = c, *v = eq + 1;
        if (!strcmp(k, "server")) snprintf(g_nas.server, sizeof g_nas.server, "%s", v);
        else if (!strcmp(k, "export")) snprintf(g_nas.export_path, sizeof g_nas.export_path, "%s", v);
        else if (!strcmp(k, "path")) {
            while (*v == '/') v++;
            snprintf(g_nas.path, sizeof g_nas.path, "%s", v);
            for (size_t n = strlen(g_nas.path); n && g_nas.path[n - 1] == '/'; ) g_nas.path[--n] = 0;
        }
        else if (!strcmp(k, "uid")) g_nas.uid = atoi(v);
        else if (!strcmp(k, "gid")) g_nas.gid = atoi(v);
        else if (!strcmp(k, "cache_gb")) g_nas.cache_gb = atoi(v) > 0 ? atoi(v) : 10;
        else if (!strcmp(k, "version")) g_nas.version = !strcmp(v, "3") ? 3 : !strcmp(v, "4") ? 4 : 0;
        else if (!strcmp(k, "nfsport")) g_nas.nfsport = atoi(v);
        else if (!strcmp(k, "mountport")) g_nas.mountport = atoi(v);
        else if (!strcmp(k, "reserved_port")) g_nas.reserved = atoi(v) != 0;
        else if (!strcmp(k, "backup_saves")) g_nas.backup = atoi(v) != 0;
        else if (!strcmp(k, "timeout")) g_nas.timeout_s = atoi(v) >= 2 && atoi(v) <= 60 ? atoi(v) : 10;
        else if (!strcmp(k, "cache_dir")) snprintf(g_cache_dir, sizeof g_cache_dir, "%s", v);
        else if (!strcmp(k, "enabled")) g_nas_off = atoi(v) == 0;
    }
    fclose(f);
    g_nas.enabled = g_nas.server[0] && g_nas.export_path[0] && !g_nas_off;
}

/* --- the local copies: sizes, pruning --- */

static int rm_tree(const char *dir, int depth)
{
    DIR *d = opendir(dir);
    struct dirent *e;
    if (!d)
        return -1;
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
            continue;
        char p[1400];
        struct stat st;
        snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
        if (lstat(p, &st) != 0)
            continue;
        if (S_ISDIR(st.st_mode)) {
            if (depth < 3)
                rm_tree(p, depth + 1);
        } else {
            remove(p);
        }
    }
    closedir(d);
    return rmdir(dir);
}

static uint64_t tree_bytes(const char *dir, int depth)
{
    DIR *d = opendir(dir);
    struct dirent *e;
    uint64_t total = 0;
    if (!d)
        return 0;
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
            continue;
        char p[1400];
        struct stat st;
        snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
        if (stat(p, &st) != 0)
            continue;
        if (S_ISDIR(st.st_mode)) {
            if (depth < 3)
                total += tree_bytes(p, depth + 1);
        } else {
            total += (uint64_t)st.st_size;
        }
    }
    closedir(d);
    return total;
}

/* Deletes the games played longest ago until `need` more bytes fit under the limit. */
static void cache_prune(uint64_t need, const char *keep)
{
    uint64_t limit = (uint64_t)g_nas.cache_gb << 30;
#ifdef CDIHOST_TEST_DUMP
    if (getenv("CDIHOST_TEST_CACHE_BYTES") && atoll(getenv("CDIHOST_TEST_CACHE_BYTES")) > 0)
        limit = (uint64_t)atoll(getenv("CDIHOST_TEST_CACHE_BYTES")); /* test only: a tiny cache */
#endif
    for (int round = 0; round < 200; round++) {
        DIR *d = opendir(g_cache_dir);
        struct dirent *e;
        uint64_t total = need;
        char oldest[1400] = "";
        time_t oldest_t = 0;
        if (!d)
            return;
        while ((e = readdir(d))) {
            if (e->d_name[0] == '.')
                continue;
            char p[1400], u[1500];
            struct stat st;
            snprintf(p, sizeof p, "%s/%s", g_cache_dir, e->d_name);
            if (stat(p, &st) != 0 || !S_ISDIR(st.st_mode))
                continue;
            total += tree_bytes(p, 0);
            if (!strcmp(e->d_name, keep))
                continue;
            snprintf(u, sizeof u, "%s/.used", p);
            time_t t = stat(u, &st) == 0 ? st.st_mtime : 0; /* never played: oldest of all */
            if (!oldest[0] || t < oldest_t) {
                snprintf(oldest, sizeof oldest, "%s", p);
                oldest_t = t;
            }
        }
        closedir(d);
        if (total <= limit || !oldest[0])
            return;
        elogf("[host] cache: removing %s to make room\n", oldest);
        rm_tree(oldest, 0);
    }
}

static void mkdirs_for(const char *file)
{
    char d[1300];
    snprintf(d, sizeof d, "%s", file);
    char *s = strrchr(d, '/');
    if (s) {
        *s = 0;
        mkdirs(d);
    }
}

/* The disc image to start in an unpacked folder: a .cue if there is one, else .chd, else .iso. */
static void find_playable(const char *dir, int depth, char *best, int *best_rank)
{
    DIR *d = opendir(dir);
    struct dirent *e;
    if (!d)
        return;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.')
            continue;
        char p[1400];
        struct stat st;
        snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
        if (stat(p, &st) != 0)
            continue;
        if (S_ISDIR(st.st_mode)) {
            if (depth < 2)
                find_playable(p, depth + 1, best, best_rank);
            continue;
        }
        int rank = has_ext(e->d_name, "cue") ? 0 : has_ext(e->d_name, "chd") ? 1 : has_ext(e->d_name, "iso") ? 2 : 9;
        if (rank < *best_rank || (rank == *best_rank && rank < 9 && strcmp(p, best) < 0)) {
            *best_rank = rank;
            snprintf(best, 1100, "%.1099s", p);
        }
    }
    closedir(d);
}

/* The "(Disc N)" images in an unpacked folder, in disc order (a .cue is preferred over the .bin it names). */
static int find_discs(const char *dir, int depth, char out[][1100], int max)
{
    DIR *d = opendir(dir);
    struct dirent *e;
    int n = 0, nums[16] = {0};
    if (!d)
        return 0;
    while ((e = readdir(d)) && n < max && n < 16) {
        if (e->d_name[0] == '.')
            continue;
        char p[1400];
        snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
        if (!(has_ext(e->d_name, "cue") || has_ext(e->d_name, "chd") || has_ext(e->d_name, "iso")))
            continue;
        int a, b, num = disc_tag(e->d_name, &a, &b);
        if (!num)
            continue;
        snprintf(out[n], 1100, "%.1099s", p);
        nums[n++] = num;
    }
    closedir(d);
    (void)depth;
    for (int x = 0; x < n; x++)
        for (int y = x + 1; y < n; y++)
            if (nums[y] < nums[x]) {
                int t = nums[x]; nums[x] = nums[y]; nums[y] = t;
                char tmp[1100];
                memcpy(tmp, out[x], 1100); memcpy(out[x], out[y], 1100); memcpy(out[y], tmp, 1100);
            }
    return n;
}

/* --- preparing one game: copy from the NAS and/or unpack, with progress --- */

typedef struct {
    char remote[1100], local[1300];
    uint64_t size;
} nas_file_t;

typedef struct {
    int idx;
    nas_file_t files[48];
    int nfiles, cur;
    uint64_t total;
    volatile uint64_t done;
    volatile int cancel;
    volatile int state; /* 0 running, 1 ready, 2 failed, 3 cancelled */
    char title[80], phase[200], why[300];
    uint64_t resumed;   /* bytes that were already in the cache */
    char launch[1100];  /* what to start when ready */
} job_t;

static void job_fail(job_t *j, const char *msg)
{
    snprintf(j->why, sizeof j->why, "%.299s", msg);
    j->state = 2;
}

static void unpacked_marker(const char *dir, char *out, size_t n)
{
    snprintf(out, n, "%s/.unpacked", dir);
}

/* what identifies a version of an archive: its size and, when it is a local file, its time */
static void archive_stamp(uint64_t size, time_t mtime, char *out, size_t n)
{
    snprintf(out, n, "%llu:%lld\n", (unsigned long long)size, (long long)mtime);
}

static bool unpacked_ok(const char *dir, uint64_t size, time_t mtime)
{
    char m[1500], want[80], have[80] = "";
    unpacked_marker(dir, m, sizeof m);
    FILE *f = fopen(m, "r");
    if (!f)
        return false;
    if (!fgets(have, sizeof have, f))
        have[0] = 0;
    fclose(f);
    archive_stamp(size, mtime, want, sizeof want);
    return !strcmp(have, want);
}

/* Unpacks `archive` into `dir` (a game's folder in the cache). */
static void job_unpack(job_t *j, const char *archive, const char *dir, uint64_t arc_size, time_t arc_mtime,
                       const char *keep, bool remove_archive)
{
    char why[300] = "";
    if (unpacked_ok(dir, arc_size, arc_mtime)) { /* done before */
        if (remove_archive)
            remove(archive);
        return;
    }
    snprintf(j->phase, sizeof j->phase, "Reading the archive");
    arc7z *a = arc7z_open(archive, why, sizeof why);
    if (!a) {
        job_fail(j, why);
        return;
    }
    const uint64_t total = arc7z_total(a);
    cache_prune(total, keep);
#ifdef HAVE_STATVFS
    {
        struct statvfs sv;
        if (statvfs(g_cache_dir, &sv) == 0 && (uint64_t)sv.f_bavail * sv.f_frsize < total + (64u << 20)) {
            arc7z_close(a);
            job_fail(j, "not enough free space on the console to unpack this game");
            return;
        }
    }
#endif
    char marker[1500];
    unpacked_marker(dir, marker, sizeof marker);
    remove(marker);
    snprintf(j->title, sizeof j->title, "Unpacking");
    j->total = total;
    j->done = 0;
    j->resumed = 0;
    int rc = arc7z_extract(a, dir, &j->done, &j->cancel, j->phase, sizeof j->phase, why, sizeof why);
    arc7z_close(a);
    if (rc == 1) {
        rm_tree(dir, 0); /* an unpacking that was stopped is not worth keeping */
        j->state = 3;
        return;
    }
    if (rc != 0) {
        rm_tree(dir, 0);
        job_fail(j, why);
        return;
    }
    char stamp[80];
    archive_stamp(arc_size, arc_mtime, stamp, sizeof stamp);
    FILE *m = fopen(marker, "w");
    if (m) {
        fputs(stamp, m);
        fclose(m);
    }
    if (remove_archive)
        remove(archive);
}

/* --- NAS (needs libnfs) --- */

#ifdef HAVE_NFS
#include "nfsclient.h"

static void nas_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static nfsx *nas_connect(char *why, size_t wn)
{
    unsigned char ip[4];
    char ips[32];
    g_dns_diag_off = 1;
#ifdef PS5
    if (!net_init(why, wn))
        return NULL;
#endif
    if (net_resolve(g_nas.server, ip) != 0) {
        snprintf(why, wn, "cannot find %s on the network", g_nas.server);
        return NULL;
    }
    snprintf(ips, sizeof ips, "%u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);
    /* is anything there? (clear messages, and no minute-long wait inside libnfs for an address that never answers) */
    if (nfsx_probe(ips, g_nas.nfsport > 0 ? g_nas.nfsport : 2049, g_nas.timeout_s * 1000, why, wn) != 0)
        return NULL;
    nfsx_opts o = {g_nas.version, g_nas.nfsport, g_nas.mountport, g_nas.reserved, g_nas.uid, g_nas.gid, g_nas.timeout_s * 1000};
    nfsx *n = nfsx_open(ips, g_nas.export_path, &o, why, wn);
    if (!n) {
        /* A title may not be allowed a reserved source port (below 1024), and some shares insist on one: try the
         * other way before giving up. Both answers go in nas_log.txt. */
        char why2[500];
        nas_log("  with the reserved source port %s: %s", o.reserved ? "on" : "off", why);
        o.reserved = !o.reserved;
        n = nfsx_open(ips, g_nas.export_path, &o, why2, sizeof why2);
        if (n)
            nas_log("  mounted with the reserved source port %s (Settings > NAS > Connect from a reserved port: set it %s)",
                    o.reserved ? "on" : "off", o.reserved ? "on" : "off");
        else
            nas_log("  with the reserved source port %s: %s", o.reserved ? "on" : "off", why2);
    }
    return n;
}

/* --- the list of games on the NAS: a background thread reads it, the launcher merges what it found --- */

typedef struct {
    char rel[1100];
    uint64_t size;
} nas_entry_t;

static nas_entry_t *g_nas_cache; /* what the last successful scan found */
static int g_nas_ncache;
static pl_thread *g_nas_thread;
static FILE *g_nas_log;

static void nas_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void nas_log(const char *fmt, ...)
{
    char line[600];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    elogf("[host] NAS: %s\n", line);
    if (g_nas_log) {
        fprintf(g_nas_log, "%s\n", line);
        fflush(g_nas_log);
    }
}

/* A short line for the launcher from a long failure text (all of it is in nas_log.txt). */
static void nas_short_reason(const char *why, char *out, size_t n)
{
    const char *r = NULL;
    if (strstr(why, "no answer from"))
        r = "no answer (NAS off, wrong address, or a firewall)";
    else if (strstr(why, "refused the connection"))
        r = "refused (NFS not running there, or blocked)";
    else if (strstr(why, "MNT3ERR_ACCES"))
        r = "the export refuses this console (allowed clients? insecure?)";
    else if (strstr(why, "MNT3ERR_NOENT") || strstr(why, "NOENT"))
        r = "export path not found (see showmount -e)";
    else if (strstr(why, "Program is not available"))
        r = "NFS version not offered (try version=4)";
    else if (strstr(why, "cannot find"))
        r = "cannot find the server's address";
    snprintf(out, n, "NAS: %s%s", r ? r : "cannot mount, see saves/nas_log.txt", r ? " (saves/nas_log.txt)" : "");
}

typedef struct {
    nas_entry_t *v;
    int n, cap;
} nas_collect_t;

static void nas_collect_cb(const char *rel, int is_dir, uint64_t size, void *user)
{
    nas_collect_t *c = user;
    if (is_dir || !(has_ext(rel, "cue") || has_ext(rel, "chd") || has_ext(rel, "iso") || has_ext(rel, "7z")))
        return;
    if (c->n == c->cap) {
        int nc = c->cap ? c->cap * 2 : 128;
        nas_entry_t *nv = realloc(c->v, (size_t)nc * sizeof *nv);
        if (!nv)
            return;
        c->v = nv;
        c->cap = nc;
    }
    snprintf(c->v[c->n].rel, sizeof c->v[c->n].rel, "%s", rel);
    c->v[c->n].size = size;
    c->n++;
}

static int nas_scan_thread(void *arg)
{
    (void)arg;
    char why[500];
    nas_collect_t col = {0};
    g_nas_busy = 1;
    nas_log("connecting to %s, export %s, folder '%s' (NFS %s, ports %s, reserved source port %s)", g_nas.server,
            g_nas.export_path, g_nas.path, g_nas.version ? (g_nas.version == 3 ? "3" : "4") : "3 then 4",
            g_nas.nfsport || g_nas.mountport ? "fixed" : "from the portmapper", g_nas.reserved ? "on" : "off");
    nfsx *n = nas_connect(why, sizeof why);
    if (!n) {
        nas_log("FAILED: %s", why);
        pl_lock(g_nas_mtx);
        nas_short_reason(why, g_nas_msg, sizeof g_nas_msg);
        g_nas_state = 3;
        pl_unlock(g_nas_mtx);
        g_nas_busy = 0;
        return 0;
    }
    nas_log("mounted (NFS v%d), reading the folder '%s'", nfsx_version(n), g_nas.path);
    int r = nfsx_scan(n, g_nas.path, 3, nas_collect_cb, &col);
    char diag[300] = "";
    if (r < 0) {
        /* What does the share look like from here? Walk the path one folder at a time and log what each level
         * holds, so a wrong name, a permission problem or a folder on another drive shows up in nas_log.txt. */
        char err[300];
        snprintf(err, sizeof err, "%s", nfsx_error(n));
        nas_log("cannot open '%s': %s", g_nas.path, err);
        char listing[1500], prefix[600] = "";
        int nl = nfsx_list(n, "", listing, sizeof listing);
        if (nl < 0)
            nas_log("  the top of the share cannot be read either: %s", nfsx_error(n));
        else
            nas_log("  top of the share (%d entries): %s", nl, listing);
        const char *c = g_nas.path;
        while (*c) {
            const char *slash = strchr(c, '/');
            size_t len = slash ? (size_t)(slash - c) : strlen(c);
            char part[300];
            snprintf(part, sizeof part, "%.*s", (int)len, c);
            char next[600];
            snprintf(next, sizeof next, "%s%s%s", prefix, prefix[0] ? "/" : "", part);
            nl = nfsx_list(n, next, listing, sizeof listing);
            if (nl < 0) {
                snprintf(err, sizeof err, "%s", nfsx_error(n));
                nas_log("  '%s': cannot open: %s", next, err);
                if (strstr(err, "ACCES") || strstr(err, "ermission"))
                    snprintf(diag, sizeof diag, "no permission to open '%.80s'", next);
                else if (strstr(err, "NOENT") || strstr(err, "o such"))
                    snprintf(diag, sizeof diag, "'%.80s' does not exist on the share", next);
                else
                    snprintf(diag, sizeof diag, "cannot open '%.80s'", next);
                break;
            }
            nas_log("  '%s' (%d entries): %s", next, nl, listing);
            if (nl == 0 && !slash)
                snprintf(diag, sizeof diag, "'%.80s' is empty over NFS (another drive or mount?)", next);
            snprintf(prefix, sizeof prefix, "%s", next);
            if (!slash)
                break;
            c = slash + 1;
        }
    }
    nfsx_close(n);
    pl_lock(g_nas_mtx);
    if (r < 0) {
        nas_log("FAILED: cannot read the folder '%s'%s%s", g_nas.path, diag[0] ? ": " : "", diag);
        snprintf(g_nas_msg, sizeof g_nas_msg, "NAS: %s (saves/nas_log.txt)", diag[0] ? diag : "the folder is not there");
        g_nas_state = 3;
        free(col.v);
    } else {
        free(g_nas_cache);
        g_nas_cache = col.v;
        g_nas_ncache = col.n;
        snprintf(g_nas_msg, sizeof g_nas_msg, "NAS: %d game file(s) found", col.n);
        nas_log("%d game file(s) found", col.n);
        g_nas_state = 2;
        g_nas_changed = 1;
    }
    pl_unlock(g_nas_mtx);
    g_nas_busy = 0;
    return 0;
}

/* Starts (or restarts) the background scan. The launcher shows its progress; nothing waits for it. */
static void nas_start(const char *save_dir)
{
    if (!g_nas.enabled || g_nas_state == 1)
        return;
    if (!g_nas_mtx)
        g_nas_mtx = pl_mutex_create();
    char lp[1300];
    snprintf(lp, sizeof lp, "%s/nas_log.txt", save_dir);
    if (g_nas_log)
        fclose(g_nas_log);
    g_nas_log = fopen(lp, "w");
    pl_lock(g_nas_mtx);
    snprintf(g_nas_msg, sizeof g_nas_msg, "NAS: connecting to %s ...", g_nas.server);
    g_nas_state = 1;
    pl_unlock(g_nas_mtx);
    g_nas_thread = pl_thread_create(nas_scan_thread, "nas-scan", NULL);
    if (g_nas_thread)
        pl_thread_detach(g_nas_thread);
    else {
        g_nas_state = 3;
        snprintf(g_nas_msg, sizeof g_nas_msg, "NAS: cannot start the scan");
    }
}

/* Adds what the last scan found to the game list (no network access here). */
static void nas_scan(void)
{
    if (!g_nas.enabled || !g_nas_mtx)
        return;
    pl_lock(g_nas_mtx);
    for (int i = 0; i < g_nas_ncache; i++) {
        const char *rel = g_nas_cache[i].rel;
        const char *file = strrchr(rel, '/');
        file = file ? file + 1 : rel;
        char base[200];
        snprintf(base, sizeof base, "%.199s", file);
        strip_ext(base);
        bool dup = false;
        for (int k = 0; k < g_nroms && !dup; k++) /* a local copy, or the same game seen twice, wins */
            dup = !strcmp(g_roms[k].base, base);
        if (dup)
            continue;
        char local[1024];
        snprintf(local, sizeof local, "%.500s/%.199s/%.300s", g_cache_dir, base, file);
        rom_add(local, file);
        snprintf(g_roms[g_nroms - 1].nas, sizeof g_roms[g_nroms - 1].nas, "%s", rel);
        g_roms[g_nroms - 1].nas_bytes = g_nas_cache[i].size;
    }
    g_nas_changed = 0;
    pl_unlock(g_nas_mtx);
}

/* the file names a .cue refers to (FILE "name" BINARY ...), relative to the cue's folder */
static int cue_files(const char *text, char names[][400], int max)
{
    int n = 0;
    for (const char *line = text; *line && n < max;) {
        const char *eol = strpbrk(line, "\r\n");
        size_t len = eol ? (size_t)(eol - line) : strlen(line);
        const char *p = line;
        while (p < line + len && (*p == ' ' || *p == '\t'))
            p++;
        if (line + len - p > 5 && !strncasecmp(p, "FILE", 4) && (p[4] == ' ' || p[4] == '\t')) {
            const char *q1 = memchr(p, '"', (size_t)(line + len - p));
            const char *q2 = q1 ? memchr(q1 + 1, '"', (size_t)(line + len - q1 - 1)) : NULL;
            if (q1 && q2 && q2 > q1 + 1) {
                snprintf(names[n], 400, "%.*s", (int)(q2 - q1 - 1), q1 + 1);
                int dup = 0;
                for (int i = 0; i < n; i++)
                    if (!strcmp(names[i], names[n]))
                        dup = 1;
                if (!dup && !strstr(names[n], ".."))
                    n++;
            }
        }
        if (!eol)
            break;
        line = eol + 1;
        while (*line == '\r' || *line == '\n')
            line++;
    }
    return n;
}

/* Copies the game's file(s) from the NAS into its cache folder. `local_dir` is that folder. */
static int job_nas_copy(job_t *j, const rom_t *r, const char *local_dir)
{
    char why[300] = "";
    char remote_dir[1100];
    snprintf(j->phase, sizeof j->phase, "Connecting to %s", g_nas.server);
    nfsx *n = nas_connect(why, sizeof why);
    if (!n) {
        job_fail(j, why);
        return -1;
    }
    /* the files this game is made of: each disc (one for most games), and for a .cue the files it names */
    j->nfiles = 0;
    const int nd = r->ndisc > 1 ? r->ndisc : 1;
    for (int dsk = 0; dsk < nd; dsk++) {
        const char *rel = r->ndisc > 1 ? r->discs[dsk] : r->nas;
        snprintf(remote_dir, sizeof remote_dir, "%s", rel);
        char *sl = strrchr(remote_dir, '/');
        if (sl) *sl = 0; else remote_dir[0] = 0;
        const char *file = strrchr(rel, '/');
        file = file ? file + 1 : rel;
        if (j->nfiles >= 47) {
            job_fail(j, "this game is made of too many files");
            nfsx_close(n);
            return -1;
        }
        snprintf(j->files[j->nfiles].remote, sizeof j->files[0].remote, "%s", rel);
        snprintf(j->files[j->nfiles].local, sizeof j->files[0].local, "%.1000s/%.250s", local_dir, file);
        j->nfiles++;
        if (has_ext(file, "cue")) {
            unsigned char *cue;
            size_t clen;
            static char names[47][400];
            if (nfsx_read_all(n, rel, 1 << 20, &cue, &clen) != 0) {
                job_fail(j, "cannot read the .cue file on the NAS");
                nfsx_close(n);
                return -1;
            }
            int nn = cue_files((const char *)cue, names, 47);
            free(cue);
            if (nn == 0) {
                job_fail(j, "the .cue file names no data files");
                nfsx_close(n);
                return -1;
            }
            for (int i = 0; i < nn && j->nfiles < 48; i++) {
                nas_file_t *f = &j->files[j->nfiles++];
                snprintf(f->remote, sizeof f->remote, "%.600s%s%.399s", remote_dir, remote_dir[0] ? "/" : "", names[i]);
                snprintf(f->local, sizeof f->local, "%.800s/%.399s", local_dir, names[i]);
            }
        }
    }
    for (int i = 0; i < j->nfiles; i++) {
        int64_t sz = nfsx_size(n, j->files[i].remote);
        if (sz < 0) {
            char m[400];
            snprintf(m, sizeof m, "not found on the NAS: %.350s", j->files[i].remote);
            job_fail(j, m);
            nfsx_close(n);
            return -1;
        }
        j->files[i].size = (uint64_t)sz;
        j->total += (uint64_t)sz;
    }

    /* what is already here */
    uint64_t missing = 0;
    for (int i = 0; i < j->nfiles; i++) {
        struct stat st;
        if (stat(j->files[i].local, &st) == 0 && (uint64_t)st.st_size == j->files[i].size)
            j->resumed += j->files[i].size;
        else
            missing += j->files[i].size;
    }
    j->done = j->resumed;
    mkdirs(g_cache_dir);
    if (missing) {
        cache_prune(missing, r->base);
#ifdef HAVE_STATVFS
        struct statvfs sv;
        if (statvfs(g_cache_dir, &sv) == 0 && (uint64_t)sv.f_bavail * sv.f_frsize < missing + (64u << 20)) {
            job_fail(j, "not enough free space on the console for this game");
            nfsx_close(n);
            return -1;
        }
#endif
    }

    for (int i = 0; i < j->nfiles; i++) {
        struct stat st;
        j->cur = i;
        if (stat(j->files[i].local, &st) == 0 && (uint64_t)st.st_size == j->files[i].size)
            continue;
        mkdirs_for(j->files[i].local);
        const char *base = strrchr(j->files[i].remote, '/');
        snprintf(j->phase, sizeof j->phase, "%s  (file %d of %d)", base ? base + 1 : j->files[i].remote, i + 1, j->nfiles);
        int rc = nfsx_copy(n, j->files[i].remote, j->files[i].local, &j->done, &j->cancel, why, sizeof why);
        if (rc == 1) {
            j->state = 3;
            nfsx_close(n);
            return -1;
        }
        if (rc != 0) {
            job_fail(j, why);
            nfsx_close(n);
            return -1;
        }
    }
    nfsx_close(n);
    return 0;
}
#else /* no NFS support built in */
static void nas_scan(void) {}
static void nas_start(const char *save_dir) { (void)save_dir; }
static int job_nas_copy(job_t *j, const rom_t *r, const char *local_dir)
{
    (void)r; (void)local_dir;
    job_fail(j, "this build has no NAS support (see README: games on a NAS)");
    return -1;
}
#endif

/* A list of the game's discs for the core (an .m3u: one disc image per line). Disc 1 starts; the menu swaps. */
static bool write_m3u(const char *m3u, char paths[][1100], int n)
{
    char tmp[1300];
    mkdirs_for(m3u);
    snprintf(tmp, sizeof tmp, "%s.part", m3u);
    FILE *f = fopen(tmp, "w");
    if (!f)
        return false;
    for (int i = 0; i < n; i++)
        fprintf(f, "%s\n", paths[i]);
    bool ok = fclose(f) == 0;
    if (!ok || rename(tmp, m3u) != 0) {
        remove(tmp);
        return false;
    }
    return true;
}

static int prepare_job(void *arg)
{
    job_t *j = arg;
    const rom_t *r = &g_roms[j->idx];
    char local_dir[1100];
    const bool is_arc = has_ext(r->path, "7z");
    snprintf(local_dir, sizeof local_dir, "%.800s/%.199s", g_cache_dir, r->base);
    snprintf(j->title, sizeof j->title, "Copying from the NAS");

    if (r->nas[0]) {
        if (is_arc && unpacked_ok(local_dir, r->nas_bytes, 0)) {
            /* the unpacked game is here already: nothing to copy */
        } else if (job_nas_copy(j, r, local_dir) != 0) {
            return 0;
        }
    }
    if (is_arc) {
        struct stat st;
        const bool have_archive = stat(r->path, &st) == 0;
        if (!have_archive && !unpacked_ok(local_dir, r->nas[0] ? r->nas_bytes : 0, 0)) {
            job_fail(j, "the archive is not there any more");
            return 0;
        }
        /* a NAS archive is identified by its size, a local one by size and time */
        job_unpack(j, r->path, local_dir, r->nas[0] ? r->nas_bytes : (uint64_t)st.st_size,
                   r->nas[0] ? 0 : st.st_mtime, r->base, r->nas[0] != 0);
        if (j->state != 0)
            return 0;
    }
    /* what to start */
    int rank = 9;
    j->launch[0] = 0;
    if (is_arc) {
        find_playable(local_dir, 0, j->launch, &rank);
        if (rank < 9) { /* more than one "(Disc N)" image inside: give the core a disc list */
            static char found[16][1100];
            int nf = find_discs(local_dir, 0, found, 16);
            if (nf > 1) {
                snprintf(j->launch, sizeof j->launch, "%.800s/%.190s.m3u", local_dir, r->name);
                if (!write_m3u(j->launch, found, nf)) {
                    job_fail(j, "cannot write the disc list");
                    return 0;
                }
            }
        }
        if (rank == 9) {
            rm_tree(local_dir, 0); /* nothing playable in there: do not keep it */
            job_fail(j, "no .cue, .chd or .iso file found inside the archive");
            return 0;
        }
    } else if (r->ndisc > 1) { /* a game on several discs, copied from the NAS: the core gets a disc list */
        static char local_discs[16][1100];
        for (int d = 0; d < r->ndisc && d < 16; d++) {
            const char *file = strrchr(r->discs[d], '/');
            snprintf(local_discs[d], sizeof local_discs[d], "%.800s/%.290s", local_dir, file ? file + 1 : r->discs[d]);
        }
        snprintf(j->launch, sizeof j->launch, "%.800s/%.190s.m3u", local_dir, r->name);
        if (!write_m3u(j->launch, local_discs, r->ndisc < 16 ? r->ndisc : 16)) {
            job_fail(j, "cannot write the disc list");
            return 0;
        }
    } else {
        snprintf(j->launch, sizeof j->launch, "%s", r->path);
    }
    char used[1300];
    snprintf(used, sizeof used, "%s/.used", local_dir);
    FILE *u = fopen(used, "w");
    if (u) {
        fprintf(u, "%ld\n", (long)time(NULL));
        fclose(u);
    }
    j->state = 1;
    return 0;
}
/* Is the game for list entry `idx` already on the console, ready to start? */
static bool game_is_here(const rom_t *r)
{
    struct stat st;
    if (has_ext(r->path, "7z")) {
        char dir[1100];
        snprintf(dir, sizeof dir, "%.800s/%.199s", g_cache_dir, r->base);
        return stat(dir, &st) == 0 && unpacked_ok(dir, r->nas[0] ? r->nas_bytes : (stat(r->path, &st) == 0 ? (uint64_t)st.st_size : 0),
                                                  r->nas[0] ? 0 : (stat(r->path, &st) == 0 ? st.st_mtime : 0));
    }
    return stat(r->path, &st) == 0;
}
/* key=value lines (# starts a comment) that are handed to the core as its options. */
static int load_options_file(const char *path)
{
    FILE *f = fopen(path, "r");
    char line[256];
    int n = 0;
    if (!f)
        return 0;
    while (fgets(line, sizeof line, f)) {
        char *c = line;
        while (*c == ' ' || *c == '\t')
            c++;
        if (*c == '#' || *c == '\n' || *c == '\r' || !*c)
            continue;
        c[strcspn(c, "\r\n")] = 0;
        opt_set(c);
        n++;
    }
    fclose(f);
    return n;
}

static bool opt_is_set(const char *key)
{
    for (int i = 0; i < g_nopts; i++)
        if (!strcmp(g_opts[i].key, key))
            return true;
    return false;
}

/* Options from <dir>/options.cfg, then <dir>/options/<game file name>.cfg, which wins.
 * With no explicit video standard, a game tagged (USA) and not (Europe) is run as NTSC:
 * the CD-i player is a PAL one by default, and digital video games can care. */
static void apply_game_options(const char *dir, const char *game_path)
{
    char path[1400], base[256];
    if (g_game_name[0]) {
        snprintf(base, sizeof base, "%.199s", g_game_name);
    } else {
        const char *slash = strrchr(game_path, '/');
        snprintf(base, sizeof base, "%.255s", slash ? slash + 1 : game_path);
        strip_ext(base);
        snprintf(g_game_name, sizeof g_game_name, "%.199s", base);
    }

    snprintf(path, sizeof path, "%s/options.cfg", dir);
    int n = load_options_file(path);
    snprintf(path, sizeof path, "%s/options/%s.cfg", dir, base);
    n += load_options_file(path);
    if (n)
        notify("cdihost: %d option(s) read from the options files", n);

    /* settings for this program rather than for the core */
    for (int i = 0; i < g_nopts; i++) {
        if (!strcmp(g_opts[i].key, "host_audio_latency_ms"))
            g_audio_ms = atoi(g_opts[i].val);
        else if (!strcmp(g_opts[i].key, "host_audio_samples"))
            g_audio_samples = atoi(g_opts[i].val);
        else if (!strcmp(g_opts[i].key, "host_pointer_speed"))
            g_ptr_speed = atoi(g_opts[i].val);
        else if (!strncmp(g_opts[i].key, "host_button_", 12)) {
            static const char *const names[8] = {"cross", "circle", "square", "triangle", "l1", "r1", "l2", "r2"};
            for (int b = 0; b < 8; b++)
                if (!strcmp(g_opts[i].key + 12, names[b])) {
                    int mask = 0;
                    for (const char *c = g_opts[i].val; *c; c++)
                        if (*c >= '1' && *c <= '3')
                            mask |= 1 << (*c - '1');
                    g_btn_map[b] = mask;
                }
        }
        else if (!strcmp(g_opts[i].key, "host_smooth_video"))
            g_smooth = atoi(g_opts[i].val) != 0;
        else if (!strcmp(g_opts[i].key, "host_smooth_strength"))
            g_smooth_strength = atoi(g_opts[i].val) < 0 ? 0 : atoi(g_opts[i].val) > 100 ? 100 : atoi(g_opts[i].val);
        else if (!strcmp(g_opts[i].key, "host_dpad_digital"))
            g_dpad_digital = atoi(g_opts[i].val) != 0;
        else if (!strcmp(g_opts[i].key, "host_pointer_mode"))
            g_ptr_mode = !strcmp(g_opts[i].val, "absolute");
        else if (!strcmp(g_opts[i].key, "host_pointer_y_range"))
            g_ptr_yrange = atoi(g_opts[i].val);
        else if (!strcmp(g_opts[i].key, "host_fast_start"))
            g_fast_start = atoi(g_opts[i].val) != 0;
        else if (!strcmp(g_opts[i].key, "host_fast_start_max"))
            g_fast_max_s = atoi(g_opts[i].val);
        else if (!strcmp(g_opts[i].key, "host_fast_start_settle"))
            g_fast_settle = atoi(g_opts[i].val);
        else if (!strcmp(g_opts[i].key, "host_fast_start_ignore"))
            g_fast_ignore = atoi(g_opts[i].val);
        else if (!strcmp(g_opts[i].key, "host_auto_play"))
            g_auto_play = atoi(g_opts[i].val) != 0;
    }
    if (g_fast_max_s < 1 || g_fast_max_s > 120)
        g_fast_max_s = 30;
    if (g_fast_settle < 1 || g_fast_settle > 500)
        g_fast_settle = 40;
    if (g_fast_ignore < 0 || g_fast_ignore > 3000)
        g_fast_ignore = 100;
    if (g_audio_ms < 20 || g_audio_ms > 300)
        g_audio_ms = 30;
    if (g_audio_samples != 256 && g_audio_samples != 1024)
        g_audio_samples = 512;
    if (g_ptr_speed < 30 || g_ptr_speed > 800)
        g_ptr_speed = 100;
    if (g_ptr_yrange < 100 || g_ptr_yrange > 1024)
        g_ptr_yrange = 560;

    if (!opt_is_set("same_cdi_video_standard")) {
        parsed_t p;
        parse_name(base, false, &p);
        if (has_tag_word(&p, "usa") && !has_tag_word(&p, "europe")) {
            opt_set("same_cdi_video_standard=ntsc");
            notify("cdihost: NTSC video standard (the file name says USA)");
        }
    }
}

static void audio_cfg_path(char *out, size_t n)
{
    snprintf(out, n, "%s/audio.cfg", g_save_dir);
}

static void audio_cfg_load(void)
{
    char path[1100];
    int ms;
    audio_cfg_path(path, sizeof path);
    FILE *f = fopen(path, "r");
    if (!f)
        return;
    if (fscanf(f, "%d", &ms) == 1 && ms >= 20 && ms <= 300)
        g_audio_ms = ms;
    fclose(f);
}

/* Pointer speed and mode are remembered per game: <save dir>/pointer/<game>.cfg ("speed mode"). The old
 * single file <save dir>/pointer.cfg still supplies the speed for games with no file of their own, but
 * not the mode: absolute aiming set up for one shooter must not follow you into the next game. */
static void ptr_cfg_path(char *out, size_t n)
{
    snprintf(out, n, "%s/pointer/%s.cfg", g_save_dir, g_game_name);
}

static void ptr_cfg_load(void)
{
    char path[1400];
    int sp, mode = -1, n;
    FILE *f;
    snprintf(path, sizeof path, "%s/pointer.cfg", g_save_dir);
    if ((f = fopen(path, "r"))) { /* older versions: one file for every game, speed only */
        if (fscanf(f, "%d", &sp) == 1 && sp >= 30 && sp <= 800)
            g_ptr_speed = sp;
        fclose(f);
    }
    ptr_cfg_path(path, sizeof path);
    if ((f = fopen(path, "r"))) {
        int smooth = -1;
        n = fscanf(f, "%d %d %d", &sp, &mode, &smooth);
        if (n >= 1 && sp >= 30 && sp <= 800)
            g_ptr_speed = sp;
        if (n >= 2 && (mode == 0 || mode == 1))
            g_ptr_mode = mode;
        if (n == 3 && (smooth == 0 || smooth == 1))
            g_smooth = smooth;
        fclose(f);
    }
}

static void ptr_cfg_save(void)
{
    char path[1400], dir[1200];
    snprintf(dir, sizeof dir, "%s/pointer", g_save_dir);
    mkdirs(dir);
    ptr_cfg_path(path, sizeof path);
    FILE *f = fopen(path, "w");
    if (f) {
        fprintf(f, "%d %d %d\n", g_ptr_speed, g_ptr_mode, g_smooth ? 1 : 0);
        fclose(f);
    }
}

static void audio_cfg_save(void)
{
    char path[1100];
    audio_cfg_path(path, sizeof path);
    FILE *f = fopen(path, "w");
    if (f) {
        fprintf(f, "%d\n", g_audio_ms);
        fclose(f);
    }
}

/* The Mono-I player has a Digital Video Cartridge fitted, but its driver ROM (vmpega.rom)
 * is optional in the core. Without it, games that need digital video stop with "digital
 * video capability missing". Say so up front instead of leaving it to the game. */
/* Is a file in the BIOS zip, or loose in its folder? (a zip lists its file names in plain text; the console's
 * libc has no memmem) */
static bool bios_has(const char *file)
{
    char path[1400];
    snprintf(path, sizeof path, "%s/same_cdi/bios/cdimono1/%s", g_system_dir, file);
    if (access(path, R_OK) == 0)
        return true;
    snprintf(path, sizeof path, "%s/same_cdi/bios/cdimono1.zip", g_system_dir);
    FILE *f = fopen(path, "rb");
    if (!f)
        return false;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = n > 0 && n < (64L << 20) ? malloc((size_t)n) : NULL;
    bool found = false;
    const size_t fl = strlen(file);
    if (buf && fread(buf, 1, (size_t)n, f) == (size_t)n)
        for (long i = 0; i + (long)fl <= n && !found; i++)
            found = buf[i] == file[0] && !memcmp(buf + i, file, fl);
    free(buf);
    fclose(f);
    return found;
}

/* The player's BIOS: one of the main ROMs the core can boot (cdi220b.rom is what it uses by default) */
static bool bios_present(void)
{
    return bios_has("cdi220b.rom") || bios_has("cdi200.rom") || bios_has("cdi220.rom");
}

/* The Mono-I player has a Digital Video Cartridge fitted, but its driver ROM (vmpega.rom)
 * is optional in the core. Without it, games that need digital video stop with "digital
 * video capability missing". Say so up front instead of leaving it to the game. */
static void check_dvc_rom(void)
{
    if (bios_has("vmpega.rom")) {
        elogf("[host] digital video ROM (vmpega.rom) found");
        return;
    }
    elogf("[host] vmpega.rom is not in the cdimono1 BIOS set");
    notify("cdihost: vmpega.rom not found next to the BIOS: games that use digital video will say 'digital video capability missing'");
}

/* ===================================================================== the emulator thread */

/* The core is used from this one thread only: loading, running, saving states, swapping discs and unloading all
 * happen here, between frames. The program asks for those through emu_cmd(), which waits for the answer. */
enum { CMD_NONE, CMD_LOAD, CMD_UNLOAD, CMD_SAVE, CMD_LOADSTATE, CMD_DISC, CMD_RESET, CMD_QUIT };

static struct {
    int cmd, arg, result, done;
    char path[1100];
    char msg[300];
} g_cmd;
static pl_mutex *g_cmd_mtx;
static pl_cond *g_cmd_cond;
static pl_thread *g_emu_thread;
static volatile int g_emu_ready;   /* 1 core up, -1 failed */
static volatile int g_loaded, g_paused;
static struct retro_system_info g_si;
static void *g_rom;                /* the game's data, for a core that wants it in memory */
static int g_core_up;              /* retro_init has run and retro_deinit has not */
static char g_last_error[300];
static int g_nvram_interval = 20;  /* seconds between NVRAM writes */

/* what the options files and per-game settings change: put back before the next game */
static struct { char key[64]; char val[64]; } g_opts0[MAX_OPTS];
static int g_nopts0, g_audio_ms0, g_ptr_speed0, g_ptr_mode0, g_ptr_yrange0, g_smooth0, g_smooth_strength0, g_dpad0, g_btn0[8];
static float g_aspect0;

/* pacing and statistics (emulator thread) */
static long e_frames;
static double e_next_run_ms, e_lvl_ema, e_run_ms;
static unsigned long e_panic_runs;
static double e_stat_t0;
static long e_stat_frames0;
static double e_stat_run0;
static unsigned long e_stat_under0, e_stat_panic0, e_stat_vid0;
static void sound_stats_log(double secs);
static double e_last_fps, e_last_ms;
static uint32_t e_nvram_tick;

static int emu_cmd(int cmd, int arg, const char *path, char *msg, size_t mn)
{
    if (!g_emu_thread || g_emu_ready != 1)
        return -1;
    pl_lock(g_cmd_mtx);
    while (g_cmd.cmd != CMD_NONE)
        pl_cond_wait(g_cmd_cond, g_cmd_mtx);
    g_cmd.cmd = cmd;
    g_cmd.arg = arg;
    g_cmd.done = 0;
    g_cmd.msg[0] = 0;
    snprintf(g_cmd.path, sizeof g_cmd.path, "%s", path ? path : "");
    pl_cond_broadcast(g_cmd_cond);
    while (!g_cmd.done)
        pl_cond_wait(g_cmd_cond, g_cmd_mtx);
    const int r = g_cmd.result;
    if (msg && mn)
        snprintf(msg, mn, "%s", g_cmd.msg);
    g_cmd.cmd = CMD_NONE;
    pl_cond_broadcast(g_cmd_cond);
    pl_unlock(g_cmd_mtx);
    return r;
}

static void settings_restore(void)
{
    memcpy(g_opts, g_opts0, sizeof g_opts0);
    g_nopts = g_nopts0;
    g_audio_ms = g_audio_ms0; g_ptr_speed = g_ptr_speed0; g_ptr_mode = g_ptr_mode0;
    g_ptr_yrange = g_ptr_yrange0; g_smooth = g_smooth0; g_smooth_strength = g_smooth_strength0; g_dpad_digital = g_dpad0;
    g_fast_start = g_fast_start0; g_fast_max_s = g_fast_max_s0; g_fast_settle = g_fast_settle0; g_fast_ignore = g_fast_ignore0; g_auto_play = g_auto_play0;
    memcpy(g_btn_map, g_btn0, sizeof g_btn0);
    g_aspect = g_aspect0;
}

/* Loads a game (emulator thread). 0 = running. */
static int emu_load(const char *game_path, char *msg, size_t mn)
{
    settings_restore();
    apply_game_options(g_options_dir, game_path);
    snprintf(g_game_name_for_states, sizeof g_game_name_for_states, "%s", g_game_name);
    audio_cfg_load();
    ptr_cfg_load();
    elogf("[host] settings for '%s': pointer %s %d%%, buttons cross=%d circle=%d square=%d triangle=%d r2=%d, video smoothing %s",
          g_game_name, g_ptr_mode ? "absolute" : "relative", g_ptr_speed, g_btn_map[0], g_btn_map[1], g_btn_map[2],
          g_btn_map[3], g_btn_map[7], g_smooth ? "on" : "off");
    check_dvc_rom();

    if (!g_core_up) { /* this core cannot load a second game: it is shut down after each game and started again */
        core.retro_init();
        g_core_up = 1;
    }
    struct retro_game_info gi;
    memset(&gi, 0, sizeof gi);
    gi.path = game_path;
    free(g_rom);
    g_rom = NULL;
    if (!g_si.need_fullpath) {
        FILE *f = fopen(game_path, "rb");
        if (!f) {
            snprintf(msg, mn, "cannot open %s", game_path);
            return -1;
        }
        fseek(f, 0, SEEK_END);
        gi.size = (size_t)ftell(f);
        fseek(f, 0, SEEK_SET);
        g_rom = malloc(gi.size ? gi.size : 1);
        if (!g_rom || fread(g_rom, 1, gi.size, f) != gi.size) {
            fclose(f);
            snprintf(msg, mn, "cannot read %s", game_path);
            return -1;
        }
        fclose(f);
        gi.data = g_rom;
    }
    est_valid = 0; /* (the disc tray interface is given once, at retro_init: keep it) */
    if (!core.retro_load_game(&gi)) {
        snprintf(msg, mn, "the core could not load the game (is the BIOS in %s/same_cdi/bios?)", g_system_dir);
        notify("cdihost: load game failed (BIOS in %s?)", g_system_dir);
        return -1;
    }
    core.retro_get_system_av_info(&g_av);
    if (g_aspect <= 0)
        g_aspect = g_av.geometry.aspect_ratio;
    const double fps = g_av.timing.fps > 1 ? g_av.timing.fps : 60.0;
    const unsigned rate = g_av.timing.sample_rate > 1 ? (unsigned)g_av.timing.sample_rate : 44100;
    elogf("av: %ux%u @ %.3f fps, audio %u Hz, aspect %.3f", g_av.geometry.base_width, g_av.geometry.base_height, fps,
          rate, g_aspect);
    audio_clear();
    video_forget();
    pl_lock(g_audio_mtx);
    g_core_rate = rate;
    pl_unlock(g_audio_mtx);
    notify("cdihost: running %ux%u, %.0f fps, audio %u Hz, latency %d ms, pointer %s %d%%, smoothing %s",
           g_av.geometry.base_width, g_av.geometry.base_height, fps, rate, g_audio_ms,
           g_ptr_mode ? "absolute" : "relative", g_ptr_speed, g_smooth ? "on" : "off");
    if (!core.retro_flush_nvram)
        notify("cdihost: this core has no NVRAM flush - saved games are only kept after a clean exit");
    e_frames = 0;
    e_lvl_ema = 0;
    e_panic_runs = 0;
    e_run_ms = 0;
    e_stat_t0 = pc_ms();
    e_stat_frames0 = 0;
    e_stat_run0 = 0;
    e_stat_under0 = underruns;
    e_stat_panic0 = 0;
    e_stat_vid0 = vid_frames;
    e_nvram_tick = pl_ticks();
    g_input_hold = 1; /* the button that started the game is still down */
    if (!g_rw_cfg_read)
        rw_cfg_load();
    rw_clear();
    g_rewinding = 0;
    g_pad_log_left = 40;
    g_paused = 0;
    g_audio_quiet = 0;
    g_boot_skip = 0;
    g_boot_frames = 0;
    g_boot_last_change = 0;
    g_boot_changes = 0;
    g_boot_w = g_boot_h = 0;
    g_boot_t0 = pc_ms();
    g_boot_phase = 1;
    g_replay_i = 0;
    g_rec_on = g_rec_pending = 0;
    g_vmode_w = g_vmode_h = 0;
    g_play_from = -1;
    click_load();
    {
        char lp[1400];
        g_learn_frames = 0;
        snprintf(lp, sizeof lp, "%s/start/%.190s.txt", g_save_dir, g_game_name_for_states);
        FILE *lf = fopen(lp, "r");
        if (lf) {
            if (fscanf(lf, "%ld", &g_learn_frames) != 1 || g_learn_frames < 0)
                g_learn_frames = 0;
            fclose(lf);
        }
    }
    g_booting = g_fast_start;
    if (g_resume_req) { /* Continue: the automatic state, straight away */
        char sm[200];
        g_booting = 0;
        if (slot_load(0, sm, sizeof sm)) {
            elogf("[host] continued from the automatic save");
            g_play_from = 0;
            est_valid = 0;
        } else {
            elogf("[host] continue: %s: starting the game instead", sm);
            g_booting = g_fast_start;
        }
        g_resume_req = 0;
    }
    if (g_booting)
        elogf("[host] fast start: up to %d s of CD-i time, shown %d frames after the screen mode settles%s%s", g_fast_max_s,
              g_fast_settle, g_auto_play && g_click_n ? ", Play CD-i clicked by itself" : "",
              g_learn_frames ? ", this game's start learned" : "");
    else if (g_play_from < 0)
        g_play_from = 0;
    g_pt_t0 = g_booting ? -1 : pc_ms();
    g_loaded = 1;
    snprintf(msg, mn, "running");
    return 0;
}

static void rw_free(void);
static void keep_backup(void);
static void emu_unload(void)
{
    if (!g_loaded)
        return;
    rw_free(); /* (the steps of this game go with it) */
    g_rewinding = 0;
    if (!g_booting && g_play_from >= 0 && e_frames - g_play_from > 10 * 50) { /* played for a while: keep where it got to */
        char sm[200];
        if (slot_save(0, sm, sizeof sm))
            elogf("[host] automatic save for Continue");
    }
    if (g_pt_t0 >= 0) {
        pt_add(g_game_name_for_states, (long)((pc_ms() - g_pt_t0) / 1000.0));
        g_pt_t0 = -1;
    }
    g_loaded = 0;
    g_booting = 0;
    g_rec_on = g_rec_pending = 0;
    g_audio_quiet = 1;
    nvram_flush(); /* the core also saves when it unloads; this is the belt to its braces */
    core.retro_unload_game();
    /* SAME_CDI does not run a second game after retro_unload_game (it stays paused, and retro_load_game does not
     * start a new machine): shut the core down here, and the next game starts it again (emu_load) */
    core.retro_deinit();
    g_core_up = 0;
    free(g_rom);
    g_rom = NULL;
    audio_clear();
    video_forget();
    settings_restore();
#ifdef PS5
    open_modes(g_save_dir, 0); /* the NVRAM and states it wrote: reachable over FTP (a title has no umask to set) */
#endif
    g_game_name[0] = 0;
    est_valid = 0;
    keep_backup(); /* the game's saves and any settings changed: into the safe copy */
    elogf("done: %ld frames, %lu underruns, %lu overruns", e_frames, underruns, overruns);
    notify("cdihost: back to the game list");
}

static void emu_handle(void)
{
    char msg[300] = "";
    int r = 0;
    if (g_cmd.cmd == CMD_LOADSTATE || g_cmd.cmd == CMD_DISC || g_cmd.cmd == CMD_RESET || g_cmd.cmd == CMD_SAVE)
        boot_end("a menu command"); /* what the player does from the menu is done to the game they can see */
    switch (g_cmd.cmd) {
    case CMD_LOAD:
        r = emu_load(g_cmd.path, msg, sizeof msg);
        break;
    case CMD_UNLOAD:
        emu_unload();
        break;
    case CMD_SAVE:
        r = g_loaded && slot_save(g_cmd.arg, msg, sizeof msg) ? 0 : -1;
        if (!g_loaded) snprintf(msg, sizeof msg, "no game is running");
        break;
    case CMD_LOADSTATE:
        rw_clear();
        r = g_loaded && slot_load(g_cmd.arg, msg, sizeof msg) ? 0 : -1;
        if (!g_loaded) snprintf(msg, sizeof msg, "no game is running");
        break;
    case CMD_DISC:
        rw_clear();
        r = g_loaded && disc_insert((unsigned)g_cmd.arg, msg, sizeof msg) ? 0 : -1;
        break;
    case CMD_RESET:
        rw_clear();
        if (g_loaded) {
            core.retro_reset();
            snprintf(msg, sizeof msg, "game reset");
        }
        break;
    case CMD_QUIT:
        emu_unload();
        break;
    }
    if (msg[0] && g_cmd.cmd != CMD_LOAD)
        notify("cdihost: %s", msg);
    snprintf(g_cmd.msg, sizeof g_cmd.msg, "%s", msg);
    g_cmd.result = r;
}

static void click_path(char *out, size_t n)
{
    snprintf(out, n, "%s/start_click.txt", g_save_dir);
}

static void click_load(void)
{
    char p[1200];
    click_path(p, sizeof p);
    g_click_n = 0;
    FILE *f = fopen(p, "r");
    if (!f)
        return;
    click_ev e;
    while (g_click_n < CLICK_MAX && fscanf(f, "%d %d %d %u", &e.off, &e.dx, &e.dy, &e.mask) == 4)
        g_click[g_click_n++] = e;
    fclose(f);
}

static void rec_start(void)
{
    g_rec_n = 0;
    g_rec_click = -1;
    g_rec_start = e_frames;
    g_rec_on = 1;
    g_rec_pending = 0;
    elogf("[host] auto play: recording the Play CD-i click");
}

/* the recording: from its first move, the quiet stretches shortened to half a second, then saved */
static void rec_commit(void)
{
    g_rec_pending = 0;
    if (g_rec_n == 0)
        return;
    char p[1200], tmp[1300];
    click_path(p, sizeof p);
    snprintf(tmp, sizeof tmp, "%s.part", p);
    FILE *f = fopen(tmp, "w");
    if (!f)
        return;
    int off = 0, prev = g_rec[0].off;
    for (int i = 0; i < g_rec_n; i++) {
        int gap = g_rec[i].off - prev;
        if (gap > 25)
            gap = 25;
        off += i ? gap : 0;
        prev = g_rec[i].off;
        fprintf(f, "%d %d %d %u\n", off, g_rec[i].dx, g_rec[i].dy, g_rec[i].mask);
    }
    const int ok = fclose(f) == 0;
    if (ok && rename(tmp, p) == 0) {
        chmod(p, 0666);
        elogf("[host] auto play: the Play CD-i click is learned (%d steps): the next games start by themselves", g_rec_n);
        notify("cdihost: Play CD-i learned: from now on games start by themselves");
    } else {
        remove(tmp);
    }
}

/* a game's own picture showed at this boot frame: next time the fast start runs exactly that far */
static void learn_save(long frames)
{
    char dir[1200], p[1400];
    snprintf(dir, sizeof dir, "%s/start", g_save_dir);
    mkdirs(dir);
    snprintf(p, sizeof p, "%s/%.190s.txt", dir, g_game_name_for_states);
    FILE *f = fopen(p, "w");
    if (f) {
        fprintf(f, "%ld\n", frames);
        fclose(f);
        chmod(p, 0666);
    }
}

/* The picture during the fast start: note each change of the screen mode (with how bright the picture is, for the log) */
static void boot_watch(unsigned w, unsigned h, const void *data, size_t pitch)
{
    if (w == g_boot_w && h == g_boot_h)
        return;
    /* the player's first second or two can switch modes on its way to its own start screen: those do not count */
    if (g_boot_w && g_boot_frames >= g_fast_ignore) {
        g_boot_changes++;
        g_boot_last_change = g_boot_frames;
    }
    unsigned long sum = 0, n = 0;
    if (g_fmt == RETRO_PIXEL_FORMAT_XRGB8888)
        for (unsigned y = 0; y < h; y += 8)
            for (unsigned x = 0; x < w; x += 8, n++) {
                const uint32_t v = ((const uint32_t *)((const uint8_t *)data + y * pitch))[x];
                sum += ((v >> 16) & 255) + ((v >> 8) & 255) + (v & 255);
            }
    elogf("[host] fast start: frame %ld (%.1f s of CD-i time): screen %ux%u (was %ux%u), brightness %lu",
          g_boot_frames, g_boot_frames / (g_av.timing.fps > 1 ? g_av.timing.fps : 50.0), w, h, g_boot_w, g_boot_h,
          n ? sum / (n * 3) : 0);
    g_boot_w = w;
    g_boot_h = h;
}

static void boot_end(const char *why)
{
    if (!g_booting)
        return;
    g_booting = 0;
    const double fps = g_av.timing.fps > 1 ? g_av.timing.fps : 50.0;
    const double wall = pc_ms() - g_boot_t0;
    elogf("[host] fast start done (%s): %ld frames = %.1f s of CD-i time in %.1f s (%.1fx), %ld screen mode change(s)",
          why, g_boot_frames, g_boot_frames / fps, wall / 1000.0, wall > 0 ? g_boot_frames / fps * 1000.0 / wall : 0,
          g_boot_changes);
    audio_clear(); /* primed afresh: the game is heard from here */
    e_next_run_ms = pc_ms();
    est_valid = 0;      /* the pointer moved without the absolute mode's knowledge */
    g_play_from = e_frames;
    g_pt_t0 = pc_ms();
    g_vmode_w = g_boot_w; /* what shows now is the mode the next change is measured from */
    g_vmode_h = g_boot_h;
}

/* The fast start: frames back to back for a slice of time (so commands still get through), until the game shows */
static void boot_step(void)
{
    const double fps = g_av.timing.fps > 1 ? g_av.timing.fps : 50.0;
    const double t0 = pc_ms();
    while (g_booting && pc_ms() - t0 < 25.0) {
        core.retro_run();
        g_boot_frames++;
        e_frames++;
        const int settled = g_boot_changes && g_boot_frames - g_boot_last_change >= g_fast_settle;
        if (g_boot_skip) {
            if (g_boot_phase == 2 || !g_click_n || !g_auto_play)
                learn_save(g_boot_frames); /* shown where the player wanted it */
            boot_end("a button was pressed");
        } else if (g_learn_frames && g_boot_frames >= g_learn_frames && (g_boot_phase == 2 || !g_click_n || !g_auto_play)) {
            boot_end("this game's learned start");
        } else if (g_boot_phase == 1 && settled) {
            if (g_auto_play && g_click_n) { /* the player's start screen: click Play CD-i, and go on to the game */
                g_boot_phase = 2;
                g_replay_at = g_boot_frames + 10;
                g_phase2_at = g_boot_frames;
                g_replay_i = 0;
                g_boot_changes = 0;
                elogf("[host] fast start: the player's start screen at frame %ld: clicking Play CD-i", g_boot_frames);
            } else {
                boot_end("the player's start screen");
                g_play_from = -1; /* not the game yet */
                if (g_auto_play)
                    rec_start(); /* the click made there is recorded */
            }
        } else if (g_boot_phase == 2 && settled) {
            learn_save(g_boot_frames);
            boot_end("the game's own picture");
        } else if (g_boot_phase == 2 && g_boot_frames - g_phase2_at > 25 * (long)fps) {
            boot_end("no new picture after the click: the player's start screen is shown");
            g_play_from = -1;
            rec_start(); /* a new click is recorded */
        } else if (g_boot_frames >= (long)((g_fast_max_s + (g_boot_phase == 2 ? 25 : 0)) * fps)) {
            boot_end("time limit");
        }
    }
}

/* ---- rewind (v59) ----
 * Every RW_EVERY frames the core's state is taken (retro_serialize) and kept as its difference from the one before:
 * the two XORed, as runs of zero 8-byte words and the words that changed (a CD-i's memory changes little in a fifth
 * of a second, so a 5.5 MB state is usually a few kilobytes). Going back applies the newest difference to the
 * current state (XOR undoes itself), loads it and shows one frame of it, step after step while L2 is held. */
#define RW_EVERY 10          /* a step: 0.2 s at 50 Hz */
#define RW_MAX 150           /* steps kept: 30 s */
#define RW_BUDGET (128u << 20)
#define RW_STEP_FRAMES 2     /* going back: a step every two frames (5x speed) */
static volatile int g_rw_held;
static uint64_t *rw_cur, *rw_new;
static uint8_t *rw_enc;
static size_t rw_size, rw_words;
typedef struct { uint8_t *d; size_t n; } rw_delta;
static rw_delta rw_ring[RW_MAX];
static int rw_head, rw_count, rw_have_cur, rw_wait;
static size_t rw_bytes;
static long rw_last_frame;
static double rw_ms_ema;

static void rw_clear(void)
{
    for (int i = 0; i < RW_MAX; i++) {
        free(rw_ring[i].d);
        rw_ring[i].d = NULL;
        rw_ring[i].n = 0;
    }
    rw_head = rw_count = 0;
    rw_bytes = 0;
    rw_have_cur = 0;
    rw_last_frame = e_frames;
}

static void rw_free(void)
{
    rw_clear();
    free(rw_cur);
    free(rw_new);
    free(rw_enc);
    rw_cur = rw_new = NULL;
    rw_enc = NULL;
    rw_size = rw_words = 0;
}

static void rw_cfg_path(char *out, size_t n)
{
    snprintf(out, n, "%s/rewind.cfg", g_save_dir);
}

static void rw_cfg_load(void)
{
    char p[1200];
    rw_cfg_path(p, sizeof p);
    FILE *f = fopen(p, "r");
    g_rw_enabled = 0;
    if (f) {
        int v;
        if (fscanf(f, "%d", &v) == 1)
            g_rw_enabled = v != 0;
        fclose(f);
    }
    g_rw_cfg_read = 1;
}

static void rw_cfg_save(void)
{
    char p[1200];
    rw_cfg_path(p, sizeof p);
    FILE *f = fopen(p, "w");
    if (f) {
        fprintf(f, "%d\n", g_rw_enabled);
        fclose(f);
        chmod(p, 0666);
    }
}

/* a ^ b as (zero words, changed words, the changed words...) runs; returns the length */
static size_t rw_encode(const uint64_t *a, const uint64_t *b, size_t words, uint8_t *out)
{
    size_t o = 0, i = 0;
    while (i < words) {
        uint32_t z = 0, l = 0;
        while (i + z < words && a[i + z] == b[i + z] && z < 0xffffffffu)
            z++;
        size_t j = i + z;
        while (j + l < words && a[j + l] != b[j + l] && l < 0xffffffffu)
            l++;
        memcpy(out + o, &z, 4);
        memcpy(out + o + 4, &l, 4);
        o += 8;
        for (uint32_t k = 0; k < l; k++) {
            const uint64_t x = a[j + k] ^ b[j + k];
            memcpy(out + o, &x, 8);
            o += 8;
        }
        i = j + l;
    }
    return o;
}

static void rw_apply(uint64_t *cur, const uint8_t *enc, size_t n)
{
    size_t o = 0, i = 0;
    while (o + 8 <= n) {
        uint32_t z, l;
        memcpy(&z, enc + o, 4);
        memcpy(&l, enc + o + 4, 4);
        o += 8;
        i += z;
        for (uint32_t k = 0; k < l && o + 8 <= n && i < rw_words; k++, i++, o += 8) {
            uint64_t x;
            memcpy(&x, enc + o, 8);
            cur[i] ^= x;
        }
    }
}

/* after a frame: take the state when a step has passed */
static void rw_capture(void)
{
    if (!g_rw_enabled || g_booting || e_frames - rw_last_frame < RW_EVERY)
        return;
    rw_last_frame = e_frames;
    const double t0 = pc_ms();
    const size_t sz = core.retro_serialize_size();
    if (!sz)
        return;
    if (sz != rw_size) {
        rw_free();
        rw_words = (sz + 7) / 8;
        rw_cur = calloc(rw_words, 8);
        rw_new = calloc(rw_words, 8);
        rw_enc = malloc(rw_words * 8 * 2 + 16);
        if (!rw_cur || !rw_new || !rw_enc) {
            rw_free();
            g_rw_enabled = 0;
            elogf("[host] rewind: not enough memory for %zu-byte states: off", sz);
            return;
        }
        rw_size = sz;
    }
    if (!core.retro_serialize(rw_new, sz))
        return;
    if (rw_have_cur) {
        const size_t n = rw_encode(rw_new, rw_cur, rw_words, rw_enc);
        uint8_t *d = malloc(n ? n : 1);
        if (d) {
            memcpy(d, rw_enc, n);
            while (rw_count && (rw_count == RW_MAX || rw_bytes + n > RW_BUDGET)) { /* the oldest step goes */
                const int old = (rw_head - rw_count + RW_MAX) % RW_MAX;
                rw_bytes -= rw_ring[old].n;
                free(rw_ring[old].d);
                rw_ring[old].d = NULL;
                rw_count--;
            }
            rw_ring[rw_head].d = d;
            rw_ring[rw_head].n = n;
            rw_head = (rw_head + 1) % RW_MAX;
            rw_count++;
            rw_bytes += n;
        }
    }
    uint64_t *t = rw_cur;
    rw_cur = rw_new;
    rw_new = t;
    rw_have_cur = 1;
    const double ms = pc_ms() - t0;
    rw_ms_ema = rw_ms_ema > 0 ? rw_ms_ema * 0.9 + ms * 0.1 : ms;
    static int told;
    if (!told && rw_count == 50) {
        told = 1;
        elogf("[host] rewind: %zu-byte states, %.1f ms to take one, %d steps in %.1f MB", rw_size, rw_ms_ema, rw_count,
              rw_bytes / 1048576.0);
    }
}

/* L2 held: one step back every RW_STEP_FRAMES frames. Returns 1 when it ran the frame itself. */
static int rw_back(void)
{
    if (!g_rw_enabled || !g_rw_held || !rw_have_cur || g_booting) {
        if (g_rewinding) { /* let go: on from here, heard again */
            g_rewinding = 0;
            audio_clear();
            rw_last_frame = e_frames;
            e_next_run_ms = pc_ms();
        }
        return 0;
    }
    if (!g_rewinding) {
        g_rewinding = 1;
        rw_wait = 0;
    }
    if (rw_wait > 0 || !rw_count) { /* between steps, or at the oldest: where it is, again (the picture stays) */
        rw_wait--;
        core.retro_unserialize(rw_cur, rw_size);
        core.retro_run();
        return 1;
    }
    rw_wait = RW_STEP_FRAMES - 1;
    rw_head = (rw_head - 1 + RW_MAX) % RW_MAX;
    rw_apply(rw_cur, rw_ring[rw_head].d, rw_ring[rw_head].n);
    rw_bytes -= rw_ring[rw_head].n;
    free(rw_ring[rw_head].d);
    rw_ring[rw_head].d = NULL;
    rw_count--;
    core.retro_unserialize(rw_cur, rw_size);
    core.retro_run();                         /* show it */
    core.retro_unserialize(rw_cur, rw_size); /* and stay there */
    return 1;
}

void cdie_rewind(int held)
{
    g_rw_held = held != 0;
}

int cdie_rewinding(void)
{
    return g_rewinding;
}

/* One step of the clock: makes a frame when it is due. Frames are scheduled by the clock, evenly (every 1/fps s);
 * the sound buffer only steers that clock by up to 3 % (a real audio output is within 0.1 % of the clock), so the
 * sound never runs dry or piles up. */
static void emu_step(void)
{
    double fps = g_av.timing.fps > 1 ? g_av.timing.fps : 60.0;
    const unsigned rate = g_core_rate ? g_core_rate : OUT_RATE;
    const size_t target = (size_t)((double)g_audio_ms * rate / 1000.0);
    const size_t buffered = audio_buffered();
    const double period_ms = 1000.0 / fps;
    const double level_ms = (double)buffered * 1000.0 / (double)rate;
    const double clock_now = pc_ms();
    int do_run = 0;
    if (!g_audio_live) {
        do_run = 1; /* priming: make frames until the sound buffer has what it needs */
        if (buffered >= target) {
            pl_lock(g_audio_mtx);
            g_audio_live = 1;
            pl_unlock(g_audio_mtx);
            e_next_run_ms = clock_now + period_ms;
            e_lvl_ema = level_ms;
        }
    } else if (clock_now >= e_next_run_ms) {
        do_run = 1;
        /* steer: sound buffered before this frame, smoothed, against what was asked for (g_audio_ms) */
        e_lvl_ema += 0.1 * (level_ms - e_lvl_ema);
        /* The output takes the sound in chunks, so the buffer swings by about one chunk and a bit around its
         * average; aim a chunk and a half above g_audio_ms so that the low points stay safe. */
        const double want_ms = (double)g_audio_ms + 1.5 * g_chunk_ema * 1000.0 / (double)OUT_RATE;
        double adj = (e_lvl_ema - want_ms) * 0.0005; /* +10 ms too much sound = 0.5 % slower */
        adj = adj < -0.03 ? -0.03 : adj > 0.03 ? 0.03 : adj;
        e_next_run_ms += period_ms * (1.0 + adj);
        if (clock_now - e_next_run_ms > 4.0 * period_ms) /* far behind (a hiccup): do not try to catch up */
            e_next_run_ms = clock_now + period_ms;
    } else if (level_ms < 2.0) {
        do_run = 1; /* the sound is about to run dry: make a frame now */
        e_panic_runs++;
    }
    if (do_run) {
        const double r0 = pc_ms();
        if (!rw_back()) {
            core.retro_run();
            rw_capture();
        }
        e_run_ms += pc_ms() - r0;
        e_frames++;
    } else {
        const double rem = e_next_run_ms - clock_now;
        if (rem > 2.0)
            pl_delay(1);
        else if (rem > 0.3)
            pl_delay(0);
    }
    const uint32_t now = pl_ticks();
    if (g_nvram_interval > 0 && now - e_nvram_tick >= (uint32_t)g_nvram_interval * 1000u) {
        e_nvram_tick = now;
        nvram_flush();
    }
    const double tnow = pc_ms();
    static double stat_ms = -1;
    if (stat_ms < 0)
        stat_ms = getenv("CDIE_TEST_PERF_MS") ? atof(getenv("CDIE_TEST_PERF_MS")) : 10000.0;
    if (tnow - e_stat_t0 >= stat_ms) {
        const double secs = (tnow - e_stat_t0) / 1000.0;
        const long df = e_frames - e_stat_frames0;
        e_last_fps = (double)df / secs;
        e_last_ms = df ? (e_run_ms - e_stat_run0) / (double)df : 0;
        static unsigned long audio_in0;
        elogf("perf: emulation %.1f fps (%.1f ms/frame), frames out %.1f/s, sound in %.0f/s, audio underruns +%lu, early frames +%lu",
              e_last_fps, e_last_ms, (double)(vid_frames - e_stat_vid0) / secs, (double)(g_audio_in - audio_in0) / secs,
              underruns - e_stat_under0, e_panic_runs - e_stat_panic0);
        audio_in0 = g_audio_in;
        sound_stats_log(secs);
        e_stat_t0 = tnow;
        e_stat_frames0 = e_frames;
        e_stat_run0 = e_run_ms;
        e_stat_under0 = underruns;
        e_stat_panic0 = e_panic_runs;
        e_stat_vid0 = vid_frames;
    }
}

/* What the CD-i's sound chip did since the last report (patches/0004): sound from memory (the "audio map": most
 * games' music loops and effects) and from the disc (speech, music), and whether its output ran dry part way, which
 * is heard as a gap. Our own output's underruns are on the perf line. Logged only when something played. */
static void sound_stats_log(double secs)
{
    static unsigned long last[16];
    if (!core.retro_cdi_audio_stats)
        return;
    unsigned long *st = core.retro_cdi_audio_stats();
    unsigned long d[16];
    for (int i = 0; i < 16; i++)
        d[i] = st[i] - last[i];
    const unsigned long most = st[7];
    st[7] = 0; /* the most waiting: since the last report */
    memcpy(last, st, sizeof last);
    last[7] = 0;
    if (!d[3] && !d[4])
        return;
    elogf("sound: from memory %lu sectors (%lu starts, %lu ends, %lu cut off by disc sound), from disc %lu sectors; "
          "ran dry %lu times (%.0f ms silent, %.1f/s), dropped %lu, most waiting %lu, rate changes %lu",
          d[3], d[6], d[9], d[5], d[4], d[0], (double)d[1] / 37.8, (double)d[0] / secs, d[2], most, d[8]);
}

static int emu_main(void *arg)
{
    (void)arg;
    if (!core_open(g_core_path) || core.retro_api_version() != RETRO_API_VERSION) {
        snprintf(g_last_error, sizeof g_last_error, "the CD-i core could not be opened");
        g_emu_ready = -1;
        return 0;
    }
    /* Callbacks must be registered before retro_init(). */
    core.retro_set_environment(env_cb);
    core.retro_set_video_refresh(video_cb);
    core.retro_set_audio_sample(audio_sample_cb);
    core.retro_set_audio_sample_batch(audio_batch_cb);
    core.retro_set_input_poll(input_poll_cb);
    core.retro_set_input_state(input_state_cb);
    core.retro_init();
    g_core_up = 1;
    memset(&g_si, 0, sizeof g_si);
    core.retro_get_system_info(&g_si);
    elogf("core: %s %s (need_fullpath=%d)", g_si.library_name, g_si.library_version, g_si.need_fullpath);
    g_emu_ready = 1;

    int quit = 0;
    while (!quit) {
        pl_lock(g_cmd_mtx);
        if (g_cmd.cmd != CMD_NONE && !g_cmd.done) {
            quit = g_cmd.cmd == CMD_QUIT;
            pl_unlock(g_cmd_mtx); /* the command runs without the lock: the program only waits on it */
            emu_handle();
            pl_lock(g_cmd_mtx);
            g_cmd.done = 1;
            pl_cond_broadcast(g_cmd_cond);
            pl_unlock(g_cmd_mtx);
            if (!g_paused && g_loaded)
                e_next_run_ms = pc_ms(); /* the clock stood still while the command ran */
            continue;
        }
        if (!g_loaded || g_paused) {
            pl_cond_wait_ms(g_cmd_cond, g_cmd_mtx, 20);
            pl_unlock(g_cmd_mtx);
            continue;
        }
        pl_unlock(g_cmd_mtx);
        if (g_booting)
            boot_step();
        else
            emu_step();
    }
    if (g_core_up)
        core.retro_deinit();
    g_core_up = 0;
    return 0;
}

/* ===================================================================== the program's side */

static unsigned g_lib_serial = 1;
static volatile int g_rescan_req;
static int g_inited;

/* the game being made ready */
static job_t *g_job;
static pl_thread *g_job_thread;
static int g_job_index = -1;
static int g_job_state = -1; /* what cdie_prepare_poll reports when there is no thread */
static char g_launch[1100];
static uint32_t g_job_last;
static uint64_t g_job_last_done;
static double g_job_mbps, g_job_eta;

static void lib_rescan_now(void)
{
    cv_stop();
    th_stop();
    roms_scan(g_roms_dir, g_covers_dir);
    th_start();
    cv_start();
    g_lib_serial++;
    notify("cdihost: %d game(s)", g_nroms);
}

/* What goes where, for whoever opens the folder over FTP; written once (and again when it is deleted). */
static void write_readme(const char *d)
{
    char p[1200], bios[1100];
    snprintf(p, sizeof p, "%s/READ ME.txt", d);
    if (access(p, F_OK) == 0)
        return;
    snprintf(bios, sizeof bios, "%s/same_cdi/bios", g_system_dir);
    mkdirs(bios);
    FILE *f = fopen(p, "w");
    if (!f)
        return;
    fprintf(f,
        "Seedee5 / ps5-cdi - what goes where (this folder is %s)\r\n"
        "\r\n"
        "roms/      your CD-i games: .chd, .cue + .bin, .iso, or .7z. Folders inside are fine (two levels).\r\n"
        "           Games in several discs: name them \"Game (Disc 1)\", \"Game (Disc 2)\"... and they are listed once.\r\n"
        "system/same_cdi/bios/   the BIOS: cdimono1.zip (the SAME_CDI / MAME set). Not included: you supply it.\r\n"
        "           vmpega.rom inside it (or next to it in a cdimono1 folder) makes digital video (FMV) games work.\r\n"
        "covers/    box art, named like the game (Game (Europe).png or .jpg). Missing ones are downloaded.\r\n"
        "nfs.cfg    a NAS share with your games (NFS). Easiest: Settings > NAS > Find NAS on my network.\r\n"
        "dns.txt    optional: the address of the DNS server to use for cover downloads (else your router).\r\n"
        "options.cfg, options/<game>.cfg   core and host options (see the project's README).\r\n"
        "saves/     saved games (NVRAM), save states, settings and the logs (title_log.txt, nas_log.txt, cover_log.txt).\r\n"
        "cache/     games copied from the NAS or unpacked from .7z; the oldest go when it is full (cache_gb in nfs.cfg).\r\n",
        d);
    fclose(f);
    chmod(p, 0666);
}


/* ------------------------------------------------- a safe copy of what matters, outside the app folder */

/* Everything Seedee5 keeps is in its data folder, inside the app's own folder. A store that updates the app
 * by replacing that folder (ProsperoStore moves the old one to /data/prosperostore/previous/PPSA99142) takes it
 * all away. So the small, precious part (settings, NAS settings, saved games, the BIOS) is also copied to the
 * title's own storage, /download0, which belongs to the title ID rather than to the folder and so survives a
 * replaced folder. When the data folder turns up new (its marker is gone) and the copy has things it lacks, they
 * are put back. Not copied: games, the NAS cache, covers (downloaded again) and save states (big; /download0 is
 * small). Desktop builds: only with CDIE_KEEP_DIR set (tests). */
#define KEEP_MARKER ".seedee5-data"
#define KEEP_FILE_MAX (64L << 20)
#define KEEP_TOTAL_MAX (180LL << 20)
static char g_keep_dir[600];
static char g_keep_note[600];
static int g_keep_restored;
static long long g_keep_total;

static const char *keep_root(void)
{
    const char *env = getenv("CDIE_KEEP_DIR");
    if (env && *env)
        return env;
#ifdef PS5
    return "/download0/seedee5";
#else
    return NULL;
#endif
}

/* what is worth keeping: rel is the path inside the data folder */
static int keep_wanted(const char *rel, int is_dir)
{
    const char *base = strrchr(rel, '/');
    base = base ? base + 1 : rel;
    if (base[0] == '.' && strcmp(base, KEEP_MARKER) != 0)
        return 0;
    if (!strcmp(rel, KEEP_MARKER))
        return 0;
    if (is_dir) /* the folders gone into: saves (but not its states) and system */
        return !strcmp(rel, "saves") || !strcmp(rel, "system") ||
               ((!strncmp(rel, "saves/", 6) || !strncmp(rel, "system/", 7)) && strcmp(rel, "saves/states") != 0);
    if (!strchr(rel, '/')) /* the data folder's own settings: nfs.cfg, options.cfg, dns.txt, games_info.txt ... */
        return strcmp(base, "READ ME.txt") != 0 && strstr(base, "_log") == NULL && strstr(base, ".tmp") == NULL;
    if (strstr(base, "_log.txt") || strstr(base, ".tmp"))
        return 0;
    return !strncmp(rel, "saves/", 6) || !strncmp(rel, "system/", 7);
}

static int keep_copy(const char *from, const char *to)
{
    FILE *in = fopen(from, "rb");
    if (!in)
        return -1;
    char tmp[1300];
    snprintf(tmp, sizeof tmp, "%s.tmp", to);
    FILE *out = fopen(tmp, "wb");
    if (!out) {
        fclose(in);
        return -1;
    }
    char buf[64 * 1024];
    size_t n;
    int ok = 1;
    while ((n = fread(buf, 1, sizeof buf, in)) > 0)
        if (fwrite(buf, 1, n, out) != n) {
            ok = 0;
            break;
        }
    ok = ok && !ferror(in);
    fclose(in);
    ok = (fclose(out) == 0) && ok;
    if (!ok || rename(tmp, to) != 0) {
        remove(tmp);
        return -1;
    }
    chmod(to, 0666);
    return 0;
}

/* copies the wanted files of from (inside the data folder at rel) to to; only_missing: never replace a file */
static void keep_walk(const char *from_root, const char *to_root, const char *rel, int only_missing, int *copied,
                      int depth)
{
    if (depth > 6)
        return;
    char dir[1100];
    snprintf(dir, sizeof dir, "%s%s%s", from_root, rel[0] ? "/" : "", rel);
    DIR *d = opendir(dir);
    if (!d)
        return;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
            continue;
        char sub[600], src[1300], dst[1300];
        snprintf(sub, sizeof sub, "%s%s%s", rel, rel[0] ? "/" : "", e->d_name);
        snprintf(src, sizeof src, "%s/%s", from_root, sub);
        snprintf(dst, sizeof dst, "%s/%s", to_root, sub);
        struct stat a, b;
        if (stat(src, &a) != 0)
            continue;
        if (S_ISDIR(a.st_mode)) {
            if (keep_wanted(sub, 1)) {
                mkdirs(dst);
                keep_walk(from_root, to_root, sub, only_missing, copied, depth + 1);
            }
            continue;
        }
        if (!S_ISREG(a.st_mode) || !keep_wanted(sub, 0) || a.st_size > KEEP_FILE_MAX)
            continue;
        if (!only_missing) { /* (the copy stays well inside /download0's room) */
            if (g_keep_total + a.st_size > KEEP_TOTAL_MAX)
                continue;
            g_keep_total += a.st_size;
        }
        const int there = stat(dst, &b) == 0;
        if (there && (only_missing || (b.st_size == a.st_size && b.st_mtime >= a.st_mtime)))
            continue;
        if (keep_copy(src, dst) == 0)
            (*copied)++;
    }
    closedir(d);
}

static int count_entries(const char *dir)
{
    DIR *d = opendir(dir);
    int n = 0;
    if (!d)
        return 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL)
        if (e->d_name[0] != '.')
            n++;
    closedir(d);
    return n;
}

/* at start, before anything reads the settings: a data folder without its marker gets back what the copy has */
static void keep_restore(void)
{
    const char *root = keep_root();
    if (!root)
        return;
    snprintf(g_keep_dir, sizeof g_keep_dir, "%s", root);
    mkdirs(g_keep_dir);
    char marker[1100], had[1100];
    snprintf(marker, sizeof marker, "%s/" KEEP_MARKER, g_data_dir);
    snprintf(had, sizeof had, "%s/" KEEP_MARKER, g_keep_dir);
    struct stat st;
    if (stat(marker, &st) != 0 && stat(had, &st) == 0) {
        int n = 0;
        keep_walk(g_keep_dir, g_data_dir, "", 1, &n, 0);
        g_keep_restored = n;
        /* the copy's marker says how many games the old folder had in roms/ */
        int games = 0;
        FILE *f = fopen(had, "r");
        if (f) {
            if (fscanf(f, "games=%d", &games) != 1)
                games = 0;
            fclose(f);
        }
        if (n > 0 || games > 0) {
            if (games > 0 && count_entries(g_roms_dir) == 0)
                snprintf(g_keep_note, sizeof g_keep_note,
                         "The app folder was replaced (an update?). Settings and saved games were put back. Your "
                         "games are not: if a store updated Seedee5, they are in /data/prosperostore/previous/PPSA99142/"
                         "data/roms. Copy them back to data/roms over FTP.");
            else
                snprintf(g_keep_note, sizeof g_keep_note,
                         "The app folder was replaced (an update?). Your settings and saved games were put back.");
        }
    }
    else if (stat(marker, &st) != 0) /* a new data folder and no copy: a first start, or an update from 1.21 or older */
        snprintf(g_keep_note, sizeof g_keep_note,
                 "Updated with a store from 1.21 or older? Your earlier games, saves and settings are in "
                 "/data/prosperostore/previous/PPSA99142/data: copy that folder's contents back to data/ over FTP.");
    FILE *f = fopen(marker, "w");
    if (f) {
        fputs("Seedee5's data folder. Settings and saved games are also kept in the title's own storage.\n", f);
        fclose(f);
        chmod(marker, 0666);
    }
}

/* after start and after each game: the copy brought up to date (new or changed files only) */
static void keep_backup(void)
{
    if (!g_keep_dir[0])
        return;
    int n = 0;
    g_keep_total = 0;
    keep_walk(g_data_dir, g_keep_dir, "", 0, &n, 0);
    char had[1100];
    snprintf(had, sizeof had, "%s/" KEEP_MARKER, g_keep_dir);
    FILE *f = fopen(had, "w");
    if (f) {
        fprintf(f, "games=%d\n", count_entries(g_roms_dir));
        fclose(f);
    }
    if (n)
        elogf("[host] safe copy: %d file(s) brought up to date in %s", n, g_keep_dir);
}

const char *cdie_keep_note(void)
{
    return g_keep_note;
}

int cdie_init(const cdie_config *cfg)
{
    if (g_inited)
        return 0;
    g_log_mtx = pl_mutex_create();
    g_log_fn = cfg->log;
    g_notice_fn = cfg->notice;
    g_audio_mtx = pl_mutex_create();
    g_vid_mtx = pl_mutex_create();
    g_pad_mtx = pl_mutex_create();
    g_cmd_mtx = pl_mutex_create();
    g_cmd_cond = pl_cond_create();
    g_net_mtx = pl_mutex_create();
    if (!th_mtx)
        th_mtx = pl_mutex_create();
    if (!th_cond)
        th_cond = pl_cond_create();
    g_audio_quiet = 1;

    const char *d = cfg->data_dir && *cfg->data_dir ? cfg->data_dir : ".";
    if (cfg->core_path && *cfg->core_path)
        snprintf(g_core_path, sizeof g_core_path, "%s", cfg->core_path);
    snprintf(g_data_dir, sizeof g_data_dir, "%s", d);
    snprintf(g_system_dir, sizeof g_system_dir, "%s/system", d);
    snprintf(g_save_dir, sizeof g_save_dir, "%s/saves", d);
    snprintf(g_roms_dir, sizeof g_roms_dir, "%s/roms", d);
    snprintf(g_covers_dir, sizeof g_covers_dir, "%s/covers", d);
    snprintf(g_cache_dir, sizeof g_cache_dir, "%s/cache", d);
    snprintf(g_options_dir, sizeof g_options_dir, "%s", d);
    mkdirs(g_system_dir);
    mkdirs(g_save_dir);
    keep_restore(); /* (a replaced app folder: the safe copy's files back first) */
    {
        char lp[1200];
        snprintf(lp, sizeof lp, "%s/cdi_log.txt", g_save_dir);
        FILE *lf = fopen(lp, "w"); /* each start begins it again */
        if (lf) {
            chmod(lp, 0666);
            pl_lock(g_log_mtx);
            g_log_file = lf;
            pl_unlock(g_log_mtx);
        } else {
            fprintf(stderr, "[host] cannot write %s: %s\n", lp, strerror(errno));
        }
    }
    mkdirs(g_roms_dir);
    mkdirs(g_covers_dir);
#ifdef PS5
    open_modes(g_save_dir, 0);
    open_modes(g_covers_dir, 0);
    open_modes(g_cache_dir, 0);
#endif
    view_load();
    write_readme(d);
    g_download_ok = !cfg->no_download;
    {
        char p[1200];
        setting_path(p, sizeof p, d, "no_cover_download");
        if (access(p, F_OK) == 0)
            g_download_ok = false; /* a file with this name switches the download off */
    }
    elogf("[host] data folder %s", d);

    /* the settings every game starts from */
    memcpy(g_opts0, g_opts, sizeof g_opts0);
    g_nopts0 = g_nopts;
    g_audio_ms0 = g_audio_ms; g_ptr_speed0 = g_ptr_speed; g_ptr_mode0 = g_ptr_mode; g_ptr_yrange0 = g_ptr_yrange;
    g_smooth0 = g_smooth; g_smooth_strength0 = g_smooth_strength; g_dpad0 = g_dpad_digital;
    g_fast_start0 = g_fast_start; g_fast_max_s0 = g_fast_max_s; g_fast_settle0 = g_fast_settle; g_fast_ignore0 = g_fast_ignore; g_auto_play0 = g_auto_play;
    memcpy(g_btn0, g_btn_map, sizeof g_btn0);
    g_aspect0 = g_aspect;

    pt_load();
    nas_cfg_load(g_options_dir);
    drives_cfg_load(g_options_dir);
    if (g_keep_dir[0])
        elogf("[host] safe copy of the settings and saved games: %s", g_keep_dir);
    if (g_keep_restored)
        elogf("[host] %d file(s) put back from the safe copy", g_keep_restored);
    roms_scan(g_roms_dir, g_covers_dir);
    keep_backup();
    nas_start(g_save_dir); /* in the background: the NAS games appear when they have been read */
    th_start();
    cv_start();
    notify("cdihost: %d game(s) in %s", g_nroms, g_roms_dir);

    g_emu_thread = pl_thread_create(emu_main, "emulator", NULL);
    if (!g_emu_thread) {
        snprintf(g_last_error, sizeof g_last_error, "cannot start the emulator thread");
        return -1;
    }
    while (g_emu_ready == 0)
        pl_delay(2);
    g_inited = 1;
    return g_emu_ready == 1 ? 0 : -1;
}

static void net_stop(void);

static pl_thread *g_bk_thread; /* (saves on the NAS: below) */
void cdie_shutdown(void)
{
    if (!g_inited)
        return;
    if (g_job) {
        g_job->cancel = 1;
        pl_thread_wait(g_job_thread);
        g_job_thread = NULL;
        free(g_job);
        g_job = NULL;
    }
    net_stop();
    const int was_loaded = g_loaded;
    if (g_emu_ready == 1)
        emu_cmd(CMD_QUIT, 0, NULL, NULL, 0);
    if (was_loaded && g_nas.backup && g_nas.enabled && !g_nas_off)
        cdie_backup_start(0); /* closing from a game: its saves to the NAS too (waited for, below) */
    if (g_bk_thread) {
        pl_thread_wait(g_bk_thread);
        g_bk_thread = NULL;
    }
    pl_thread_wait(g_emu_thread);
    g_emu_thread = NULL;
    ex_stop();
    cv_stop();
    th_stop();
#ifdef PS5
    open_modes(g_save_dir, 0);
    open_modes(g_covers_dir, 0);
#endif
    pl_lock(g_log_mtx);
    if (g_log_file)
        fclose(g_log_file);
    g_log_file = NULL;
    pl_unlock(g_log_mtx);
    g_inited = 0;
}

/* ---- the list ---- */

static void nas_restart_pending(void);

unsigned cdie_lib_update(void)
{
    nas_restart_pending();
    if ((g_nas_changed || g_rescan_req) && !g_job_thread) {
        g_rescan_req = 0;
        lib_rescan_now();
    }
    return g_lib_serial;
}

int cdie_lib_count(void)
{
    return g_nroms;
}

int cdie_lib_game(int i, cdie_game *g)
{
    if (i < 0 || i >= g_nroms)
        return -1;
    const rom_t *r = &g_roms[i];
    memset(g, 0, sizeof *g);
    snprintf(g->name, sizeof g->name, "%s", r->name);
    snprintf(g->path, sizeof g->path, "%s", r->path);
    snprintf(g->drive, sizeof g->drive, "%s", r->drive);
    const unsigned char c0 = (unsigned char)r->name[0];
    g->letter = isalpha(c0) ? (char)toupper(c0) : '#';
    g->source = r->nas[0] ? CDIE_SRC_NAS : r->drive[0] ? CDIE_SRC_DRIVE : CDIE_SRC_LOCAL;
    g->discs = r->ndisc > 1 ? r->ndisc : 1;
    g->packed = has_ext(r->path, "7z");
    g->here = (!r->nas[0] && !g->packed) || game_is_here(r);
    pl_lock(th_mtx);
    g->has_cover = r->cover[0] != 0;
    pl_unlock(th_mtx);
    return 0;
}

int cdie_lib_find(const char *path)
{
    for (int i = 0; i < g_nroms; i++)
        if (!strcmp(g_roms[i].path, path))
            return i;
    return -1;
}

int cdie_lib_last(void)
{
    return last_game_index();
}

void cdie_lib_rescan(void)
{
    nas_start(g_save_dir); /* read the NAS again too (merged when it arrives) */
    g_rescan_req = 1;
}

int cdie_lib_status(char *nas, size_t nn, char *covers, size_t cn)
{
    int st = 0;
    if (nas && nn) {
        nas[0] = 0;
        if (g_nas_state != 0 && g_nas_mtx) {
            pl_lock(g_nas_mtx);
            snprintf(nas, nn, "%s", g_nas_msg);
            st = g_nas_state;
            pl_unlock(g_nas_mtx);
        }
    }
    if (covers && cn) {
        covers[0] = 0;
        if (cv_total > 0 && (cv_net_down || cv_done < cv_total)) {
            if (cv_net_down)
                snprintf(covers, cn, "Covers: %s", cv_msg);
            else
                snprintf(covers, cn, "Downloading covers  %d / %d", cv_done, cv_total);
        }
    }
    return st;
}

void cdie_covers_retry(void)
{
    if (!g_download_ok)
        return;
    cv_stop();
    cv_plan_good = -1;
    cv_start();
}

/* ---- getting a game ready ---- */

int cdie_prepare(int idx)
{
    if (idx < 0 || idx >= g_nroms || g_job_thread)
        return -1;
    free(g_job);
    g_job = NULL;
    g_job_index = idx;
    g_launch[0] = 0;
    const rom_t *r = &g_roms[idx];
    if (!r->nas[0] && !has_ext(r->path, "7z")) { /* ready as it is */
        snprintf(g_launch, sizeof g_launch, "%s", r->path);
        if (r->ndisc > 1) { /* local discs: the list goes into the save folder */
            char m3u[1300];
            snprintf(m3u, sizeof m3u, "%.800s/discs/%.190s.m3u", g_save_dir, r->name);
            if (write_m3u(m3u, r->discs, r->ndisc))
                snprintf(g_launch, sizeof g_launch, "%.1099s", m3u);
        }
        g_job_state = 1;
        return 0;
    }
    g_job = calloc(1, sizeof *g_job);
    if (!g_job)
        return -1;
    g_job->idx = idx;
    g_job_state = 0;
    g_job_last = pl_ticks();
    g_job_last_done = 0;
    g_job_mbps = g_job_eta = 0;
    g_job_thread = pl_thread_create(prepare_job, "prepare-game", g_job);
    if (!g_job_thread) {
        free(g_job);
        g_job = NULL;
        g_job_state = -1;
        return -1;
    }
    return 0;
}

int cdie_prepare_poll(cdie_job *out)
{
    memset(out, 0, sizeof *out);
    out->state = g_job_state;
    if (!g_job)
        return out->state;
    job_t *j = g_job;
    const uint32_t now = pl_ticks();
    if (now - g_job_last >= 500) {
        const double dt = (double)(now - g_job_last) / 1000.0;
        const uint64_t d = j->done;
        const double inst = d >= g_job_last_done ? (double)(d - g_job_last_done) / 1048576.0 / dt : 0; /* a new stage starts at 0 */
        g_job_mbps = g_job_mbps > 0 ? g_job_mbps * 0.6 + inst * 0.4 : inst;
        g_job_last_done = d;
        g_job_last = now;
        g_job_eta = g_job_mbps > 0.05 && j->total > j->done ? (double)(j->total - j->done) / 1048576.0 / g_job_mbps : 0;
    }
    snprintf(out->title, sizeof out->title, "%s", j->title);
    snprintf(out->phase, sizeof out->phase, "%s", j->phase);
    snprintf(out->why, sizeof out->why, "%s", j->why);
    out->done = j->done;
    out->total = j->total;
    out->resumed = j->resumed;
    out->mbps = g_job_mbps;
    out->eta_s = g_job_eta;
    out->state = j->state;
    if (j->state != 0 && g_job_thread) { /* finished: collect the thread */
        pl_thread_wait(g_job_thread);
        g_job_thread = NULL;
        if (j->state == 1)
            snprintf(g_launch, sizeof g_launch, "%s", j->launch);
        else if (j->state == 2) {
            notify("cdihost: %s", j->why);
            elogf("[host] game preparation failed: %s", j->why);
        }
        g_job_state = j->state;
    }
    return out->state;
}

void cdie_prepare_cancel(void)
{
    if (g_job)
        g_job->cancel = 1;
}

static int start_path(const char *path)
{
    char msg[300];
    if (!bios_present()) {
        snprintf(g_last_error, sizeof g_last_error,
                 "The CD-i BIOS is missing. Put cdimono1.zip (the MAME / RetroArch SAME_CDI set) in %s/same_cdi/bios, "
                 "or let Settings find it on your NAS.", g_system_dir);
        elogf("[host] %s", g_last_error);
        return -1;
    }
    if (g_loaded)
        emu_cmd(CMD_UNLOAD, 0, NULL, NULL, 0);
    const int r = emu_cmd(CMD_LOAD, 0, path, msg, sizeof msg);
    if (r != 0)
        snprintf(g_last_error, sizeof g_last_error, "%s", msg[0] ? msg : "the game could not be started");
    return r == 0 ? 0 : -1;
}

int cdie_start(void)
{
    if (g_job_state != 1 || !g_launch[0] || g_job_index < 0 || g_job_index >= g_nroms) {
        snprintf(g_last_error, sizeof g_last_error, "the game is not ready");
        return -1;
    }
    const rom_t *r = &g_roms[g_job_index];
    snprintf(g_game_name, sizeof g_game_name, "%.199s", r->base);
    last_game_save(r->path);
    notify("cdihost: loading %s", r->name);
    elogf("[host] selected %s", g_launch);
    g_job_state = -1;
    return start_path(g_launch);
}

/* Time played (v58): saves/playtime.txt, a line a game: "<seconds> <last played, Unix time> <game>". Counted from the
 * moment the game itself shows to when it is left. */
typedef struct { char base[200]; long secs; long last; } playtime_t;
static playtime_t *g_pt;
static int g_npt;

static void pt_load(void)
{
    char p[1200], line[400];
    snprintf(p, sizeof p, "%s/playtime.txt", g_save_dir);
    free(g_pt);
    g_pt = NULL;
    g_npt = 0;
    FILE *f = fopen(p, "r");
    if (!f)
        return;
    while (fgets(line, sizeof line, f)) {
        long a, b;
        int used = 0;
        if (sscanf(line, "%ld %ld %n", &a, &b, &used) < 2 || !used)
            continue;
        line[strcspn(line, "\r\n")] = 0;
        playtime_t *n = realloc(g_pt, (size_t)(g_npt + 1) * sizeof *n);
        if (!n)
            break;
        g_pt = n;
        snprintf(g_pt[g_npt].base, sizeof g_pt[g_npt].base, "%s", line + used);
        g_pt[g_npt].secs = a;
        g_pt[g_npt].last = b;
        g_npt++;
    }
    fclose(f);
}

static void pt_add(const char *base, long secs)
{
    if (!base[0] || secs <= 0)
        return;
    int i = 0;
    while (i < g_npt && strcmp(g_pt[i].base, base))
        i++;
    if (i == g_npt) {
        playtime_t *n = realloc(g_pt, (size_t)(g_npt + 1) * sizeof *n);
        if (!n)
            return;
        g_pt = n;
        snprintf(g_pt[i].base, sizeof g_pt[i].base, "%s", base);
        g_pt[i].secs = 0;
        g_npt++;
    }
    g_pt[i].secs += secs;
    g_pt[i].last = (long)time(NULL);
    char p[1200], tmp[1300];
    snprintf(p, sizeof p, "%s/playtime.txt", g_save_dir);
    snprintf(tmp, sizeof tmp, "%s.part", p);
    FILE *f = fopen(tmp, "w");
    if (!f)
        return;
    for (int k = 0; k < g_npt; k++)
        fprintf(f, "%ld %ld %s\n", g_pt[k].secs, g_pt[k].last, g_pt[k].base);
    if (fclose(f) == 0 && rename(tmp, p) == 0)
        chmod(p, 0666);
    else
        remove(tmp);
}

int cdie_play_info(int idx, long *seconds, long *last)
{
    *seconds = *last = 0;
    if (idx < 0 || idx >= g_nroms)
        return 0;
    for (int i = 0; i < g_npt; i++)
        if (!strcmp(g_pt[i].base, g_roms[idx].base)) {
            *seconds = g_pt[i].secs;
            *last = g_pt[i].last;
            return 1;
        }
    return 0;
}

/* The CD-i buttons on the pad's face buttons, for every game (v57): options.cfg's host_button_cross, _circle,
 * _square and _triangle. A game's own options/<game>.cfg still wins. Masks: 1 = button 1, 2 = button 2, 3 = both. */
static const char *const k_face_keys[4] = {"host_button_cross", "host_button_circle", "host_button_square", "host_button_triangle"};

static int parse_mask(const char *v)
{
    int mask = 0;
    for (; *v; v++)
        if (*v >= '1' && *v <= '3')
            mask |= 1 << (*v - '1');
    return mask & 3;
}

void cdie_buttons_get(int map[4])
{
    static const int defaults[4] = {1, 2, 1, 2};
    char p[1200], line[300];
    memcpy(map, defaults, sizeof defaults);
    snprintf(p, sizeof p, "%s/options.cfg", g_options_dir);
    FILE *f = fopen(p, "r");
    if (!f)
        return;
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        char *eq = strchr(line, '=');
        if (!eq)
            continue;
        *eq = 0;
        for (int i = 0; i < 4; i++)
            if (!strcmp(line, k_face_keys[i]))
                map[i] = !strcmp(eq + 1, "none") ? 0 : parse_mask(eq + 1);
    }
    fclose(f);
}

int cdie_buttons_set(const int map[4])
{
    char p[1200], line[300];
    char *text = malloc(65536);
    if (!text)
        return -1;
    size_t k = 0;
    snprintf(p, sizeof p, "%s/options.cfg", g_options_dir);
    FILE *f = fopen(p, "r");
    if (f) { /* every other line as it was */
        while (fgets(line, sizeof line, f) && k + sizeof line < 65536) {
            int ours = 0;
            for (int i = 0; i < 4; i++)
                ours |= !strncmp(line, k_face_keys[i], strlen(k_face_keys[i])) && line[strlen(k_face_keys[i])] == '=';
            if (!ours) {
                k += (size_t)snprintf(text + k, 65536 - k, "%s", line);
                if (k && text[k - 1] != '\n')
                    text[k++] = '\n', text[k] = 0;
            }
        }
        fclose(f);
    }
    static const char *const vals[4] = {"none", "1", "2", "12"};
    for (int i = 0; i < 4; i++)
        k += (size_t)snprintf(text + k, 65536 - k, "%s=%s\n", k_face_keys[i], vals[map[i] & 3]);
    const int r = setting_write(g_options_dir, "options.cfg", text);
    free(text);
    if (r == 0)
        elogf("[host] buttons: cross %d, circle %d, square %d, triangle %d", map[0], map[1], map[2], map[3]);
    return r;
}

/* Continue (v56): the next cdie_start loads the game's automatic save (made when it was last left) instead of
 * starting it; it goes straight to where it was, without the player's start-up. */
void cdie_continue_next(int on)
{
    g_resume_req = on != 0;
}

int cdie_continue_info(int idx, char *when, size_t n)
{
    char p[1400];
    struct stat st;
    if (when && n)
        when[0] = 0;
    if (idx < 0 || idx >= g_nroms)
        return 0;
    snprintf(p, sizeof p, "%s/states/%.180s.slot0.state", g_save_dir, g_roms[idx].base);
    if (stat(p, &st) != 0)
        return 0;
    if (when && n) {
        struct tm tmv;
        time_t t = st.st_mtime;
        localtime_r(&t, &tmv);
        strftime(when, n, "%d %b %H:%M", &tmv);
    }
    return 1;
}

int cdie_start_file(const char *path)
{
    g_game_name[0] = 0; /* named after the file */
    return start_path(path);
}

const char *cdie_last_error(void)
{
    return g_last_error;
}

/* ---- saves on the NAS (v59): saves/ copied to <share>/cdi-wheel-saves/ ----
 * After each game (when nfs.cfg says backup_saves=1), every file in the saves folder that is newer than its copy on
 * the share is copied there: the CD-i's memory (NVRAM, the games' own saved games), the save states, Continue,
 * favourites, time played and the settings kept there. Restore copies back the files on the share that are newer
 * than the console's (or missing on it). Logs, half-written files and the generated disc lists stay behind. */
#define BACKUP_DIR "cdi-wheel-saves"
static pl_thread *g_bk_thread;
static volatile int g_bk_state = -1;     /* -1 idle, 0 working, 1 done, 2 failed */
static volatile int g_bk_restore;
static char g_bk_msg[300];
static int g_bk_files, g_bk_failed;

static int bk_skip(const char *rel)
{
    const char *base = strrchr(rel, '/');
    base = base ? base + 1 : rel;
    const size_t n = strlen(base);
    return !strncmp(rel, "discs/", 6) || (n > 5 && !strcmp(base + n - 5, ".part")) ||
           (n > 8 && !strcmp(base + n - 8, "_log.txt")) || base[0] == '.';
}

#ifdef HAVE_NFS
static void bk_up_dir(nfsx *n, const char *local_dir, const char *rel)
{
    char path[1400];
    snprintf(path, sizeof path, "%s%s%s", local_dir, rel[0] ? "/" : "", rel);
    DIR *d = opendir(path);
    if (!d)
        return;
    struct dirent *e;
    while ((e = readdir(d)) && g_bk_failed < 3) {
        if (e->d_name[0] == '.')
            continue;
        char r2[1000], full[1500];
        snprintf(r2, sizeof r2, "%s%s%s", rel, rel[0] ? "/" : "", e->d_name);
        snprintf(full, sizeof full, "%s/%s", local_dir, r2);
        struct stat st;
        if (stat(full, &st) != 0 || bk_skip(r2))
            continue;
        if (S_ISDIR(st.st_mode)) {
            bk_up_dir(n, local_dir, r2);
            continue;
        }
        char remote[1100], why[300];
        snprintf(remote, sizeof remote, BACKUP_DIR "/%s", r2);
        if (nfsx_mtime(n, remote) >= (int64_t)st.st_mtime)
            continue; /* the share's copy is as new */
        char dir[1100];
        snprintf(dir, sizeof dir, "%s", remote);
        char *sl = strrchr(dir, '/');
        if (sl) {
            *sl = 0;
            nfsx_mkdirs(n, dir);
        }
        if (nfsx_put(n, full, remote, why, sizeof why) == 0) {
            g_bk_files++;
        } else {
            g_bk_failed++;
            snprintf(g_bk_msg, sizeof g_bk_msg, "%s", why);
            elogf("[host] saves backup: %s", why);
        }
    }
    closedir(d);
}

typedef struct { char rel[1000]; uint64_t size; } bk_entry;
typedef struct { bk_entry *v; int n, cap; } bk_list;
static void bk_collect(const char *path, int is_dir, uint64_t size, void *user)
{
    bk_list *l = user;
    if (is_dir || l->n >= 20000)
        return;
    const size_t pre = strlen(BACKUP_DIR) + 1;
    if (strncmp(path, BACKUP_DIR "/", pre) && !(path[0] == '/' && !strncmp(path + 1, BACKUP_DIR "/", pre)))
        return;
    const char *rel = path[0] == '/' ? path + 1 + pre : path + pre;
    if (bk_skip(rel))
        return;
    if (l->n == l->cap) {
        int nc = l->cap ? l->cap * 2 : 64;
        bk_entry *nv = realloc(l->v, (size_t)nc * sizeof *nv);
        if (!nv)
            return;
        l->v = nv;
        l->cap = nc;
    }
    snprintf(l->v[l->n].rel, sizeof l->v[0].rel, "%s", rel);
    l->v[l->n].size = size;
    l->n++;
}

static int bk_thread(void *arg)
{
    (void)arg;
    char why[300];
    g_bk_files = g_bk_failed = 0;
    g_bk_msg[0] = 0;
    nfsx *n = nas_connect(why, sizeof why);
    if (!n) {
        snprintf(g_bk_msg, sizeof g_bk_msg, "the NAS cannot be reached: %s", why);
        g_bk_state = 2;
        return 0;
    }
    if (!g_bk_restore) {
        if (nfsx_mkdirs(n, BACKUP_DIR) != 0) {
            snprintf(g_bk_msg, sizeof g_bk_msg, "cannot make %s on the share (is it read-only for the console?)", BACKUP_DIR);
            nfsx_close(n);
            g_bk_state = 2;
            return 0;
        }
        bk_up_dir(n, g_save_dir, "");
        if (g_bk_failed)
            g_bk_state = 2;
        else {
            snprintf(g_bk_msg, sizeof g_bk_msg, "%d file(s) copied to the NAS (%s)", g_bk_files, BACKUP_DIR);
            g_bk_state = 1;
        }
    } else {
        bk_list l = {0};
        nfsx_scan(n, BACKUP_DIR, 8, bk_collect, &l);
        for (int i = 0; i < l.n && g_bk_failed < 3; i++) {
            char local[1500], remote[1100];
            snprintf(local, sizeof local, "%s/%s", g_save_dir, l.v[i].rel);
            snprintf(remote, sizeof remote, BACKUP_DIR "/%s", l.v[i].rel);
            struct stat st;
            if (stat(local, &st) == 0 && (int64_t)st.st_mtime >= nfsx_mtime(n, remote))
                continue; /* the console's is as new */
            mkdirs_for(local);
            volatile uint64_t done = 0;
            volatile int cancel = 0;
            if (nfsx_copy(n, remote, local, &done, &cancel, why, sizeof why) == 0) {
                chmod(local, 0666);
                g_bk_files++;
            } else {
                g_bk_failed++;
                snprintf(g_bk_msg, sizeof g_bk_msg, "%s", why);
            }
        }
        free(l.v);
        if (g_bk_failed)
            g_bk_state = 2;
        else {
            if (l.n)
                snprintf(g_bk_msg, sizeof g_bk_msg, "%d file(s) brought back from the NAS", g_bk_files);
            else
                snprintf(g_bk_msg, sizeof g_bk_msg, "no saves on the NAS yet (%s)", BACKUP_DIR);
            g_bk_state = 1;
            if (g_bk_files) {
                pt_load(); /* time played may have come back */
                g_lib_serial++;
            }
        }
    }
    nfsx_close(n);
    elogf("[host] saves %s: %s", g_bk_restore ? "restore" : "backup", g_bk_msg);
    return 0;
}
#endif

int cdie_backup_start(int restore)
{
#ifdef HAVE_NFS
    if (g_bk_thread) {
        if (g_bk_state == 0)
            return -1; /* one at a time */
        pl_thread_wait(g_bk_thread);
        g_bk_thread = NULL;
    }
    if (!g_nas.server[0] || !g_nas.export_path[0])
        return -1;
    g_bk_restore = restore;
    g_bk_state = 0;
    g_bk_thread = pl_thread_create(bk_thread, "saves-backup", NULL);
    if (!g_bk_thread) {
        g_bk_state = -1;
        return -1;
    }
    return 0;
#else
    (void)restore;
    return -1;
#endif
}

int cdie_backup_poll(char *msg, size_t n)
{
    if (msg && n)
        snprintf(msg, n, "%s", g_bk_state == 0 ? (g_bk_restore ? "bringing saves back from the NAS ..." : "copying saves to the NAS ...") : g_bk_msg);
    return g_bk_state;
}

void cdie_stop(void)
{
    if (g_loaded) {
        emu_cmd(CMD_UNLOAD, 0, NULL, NULL, 0);
        if (g_nas.backup && g_nas.enabled && !g_nas_off)
            cdie_backup_start(0); /* the game's memory and states, to the NAS */
    }
    g_paused = 0;
}

int cdie_running(void)
{
    return g_loaded;
}

const char *cdie_game_name(void)
{
    return g_game_name;
}

/* ---- while a game runs ---- */

void cdie_pause(int on)
{
    if (on && !g_paused) {
        g_paused = 1;
        g_audio_quiet = 1;
    } else if (!on && g_paused) {
        g_input_hold = 1;
        g_audio_quiet = 0;
        g_paused = 0;
        pl_lock(g_cmd_mtx);
        e_next_run_ms = pc_ms(); /* the clock stood still while paused */
        pl_cond_broadcast(g_cmd_cond);
        pl_unlock(g_cmd_mtx);
    }
}

int cdie_paused(void)
{
    return g_paused;
}

int cdie_slot_info(int slot, char *out, size_t n)
{
    char p[1400];
    struct stat st;
    slot_path(slot, "state", p, sizeof p);
    if (stat(p, &st) != 0) {
        snprintf(out, n, "empty");
        return 0;
    }
    char when[32];
    struct tm tmv;
    time_t t = st.st_mtime;
    localtime_r(&t, &tmv);
    strftime(when, sizeof when, "%d %b %H:%M", &tmv);
    snprintf(out, n, "saved %s", when);
    return 1;
}

int cdie_slot_save(int slot, char *msg, size_t n)
{
    return emu_cmd(CMD_SAVE, slot, NULL, msg, n);
}

int cdie_slot_load(int slot, char *msg, size_t n)
{
    return emu_cmd(CMD_LOADSTATE, slot, NULL, msg, n);
}

int cdie_disc_count(void)
{
    if (!g_loaded || !g_have_disk || !g_disk.get_num_images)
        return 1;
    const unsigned nd = g_disk.get_num_images();
    return nd > 1 ? (int)nd : 1;
}

int cdie_disc_current(void)
{
    if (!g_loaded || !g_have_disk || !g_disk.get_image_index)
        return 0;
    return (int)g_disk.get_image_index();
}

int cdie_disc_insert(int disc, char *msg, size_t n)
{
    return emu_cmd(CMD_DISC, disc, NULL, msg, n);
}

void cdie_reset(void)
{
    emu_cmd(CMD_RESET, 0, NULL, NULL, 0);
}

void cdie_fit_crop(void)
{
    g_autofit_req = 1; /* done on the next frame */
}

/* ---- settings ---- */

enum { ST_PTRMODE, ST_PTRSPEED, ST_SMOOTH, ST_LATENCY, ST_SCREEN, ST_CROP, ST_REWIND, ST_COUNT };
static const char *const k_ptr_names[] = {"relative", "absolute (aim)"};
static const char *const k_onoff[] = {"off", "on"};
static const char *const k_screen[] = {"4:3", "16:9 (fill)"};
static const char *const k_crop[] = {"off (whole frame)", "manual", "auto"};

int cdie_setting_count(void)
{
    return ST_COUNT;
}

int cdie_setting_get(int i, cdie_setting *s)
{
    memset(s, 0, sizeof *s);
    switch (i) {
    case ST_PTRMODE: s->label = "Pointer"; s->value = g_ptr_mode; s->max = 1; s->step = 1; s->names = k_ptr_names; break;
    case ST_PTRSPEED: s->label = "Pointer speed"; s->value = g_ptr_speed; s->min = 30; s->max = 800; s->step = 10; s->unit = "%"; break;
    case ST_SMOOTH: s->label = "Frame blending (even motion)"; s->value = g_smooth ? 1 : 0; s->max = 1; s->step = 1; s->names = k_onoff; break;
    case ST_LATENCY: s->label = "Sound delay"; s->value = g_audio_ms; s->min = 20; s->max = 150; s->step = 5; s->unit = " ms"; break;
    case ST_SCREEN: s->label = "Screen"; s->value = g_view_mode; s->max = 1; s->step = 1; s->names = k_screen; break;
    case ST_CROP: s->label = "Crop"; s->value = g_crop_mode; s->max = 2; s->step = 1; s->names = k_crop; break;
    case ST_REWIND: s->label = "Rewind (hold L2)"; s->value = g_rw_enabled; s->max = 1; s->step = 1; s->names = k_onoff; break;
    default: return -1;
    }
    return 0;
}

void cdie_setting_set(int i, int v)
{
    cdie_setting s;
    if (cdie_setting_get(i, &s) != 0)
        return;
    if (s.names) { /* a choice wraps around */
        if (v < s.min) v = s.max;
        if (v > s.max) v = s.min;
    } else {
        v = v < s.min ? s.min : v > s.max ? s.max : v;
    }
    switch (i) {
    case ST_PTRMODE: g_ptr_mode = v; est_valid = 0; ptr_cfg_save(); break;
    case ST_PTRSPEED: g_ptr_speed = v; ptr_cfg_save(); break;
    case ST_SMOOTH: g_smooth = v; ptr_cfg_save(); break;
    case ST_LATENCY: g_audio_ms = v; audio_cfg_save(); break;
    case ST_SCREEN: g_view_mode = v; view_save(); break;
    case ST_REWIND:
        g_rw_enabled = v;
        rw_cfg_save();
        if (!v)
            g_rw_held = 0; /* (the steps are let go of with the game, or the next time it starts) */
        break;
    case ST_CROP:
        if (v == CROP_AUTO) /* start from the manual crop: no jump */
            for (int k = 0; k < 4; k++)
                g_auto[k] = g_crop[g_view_mode ? 1 : 0][k];
        g_crop_mode = v;
        view_save();
        break;
    }
}

void cdie_stats(char *out, size_t n)
{
    snprintf(out, n, "%.1f fps, %.1f ms a frame, sound %d ms, underruns %lu", e_last_fps, e_last_ms,
             (int)(audio_buffered() * 1000 / (g_core_rate ? g_core_rate : OUT_RATE)), underruns);
}

/* ===================================================================== settings: BIOS, NAS, covers, DNS */

void cdie_bios_status(cdie_bios *out)
{
    memset(out, 0, sizeof *out);
    out->bios = bios_present();
    out->dvc = bios_has("vmpega.rom");
    snprintf(out->folder, sizeof out->folder, "%s/same_cdi/bios", g_system_dir);
    snprintf(out->roms, sizeof out->roms, "%s", g_roms_dir);
    snprintf(out->data, sizeof out->data, "%s", g_data_dir);
#ifdef HAVE_NFS
    out->nfs = 1;
#endif
}

void cdie_nas_get(cdie_nas_cfg *out)
{
    memset(out, 0, sizeof *out);
    out->enabled = g_nas.enabled;
    snprintf(out->server, sizeof out->server, "%s", g_nas.server);
    snprintf(out->share, sizeof out->share, "%s", g_nas.export_path);
    snprintf(out->path, sizeof out->path, "%s", g_nas.path);
    out->version = g_nas.version;
    out->reserved = g_nas.reserved;
    out->cache_gb = g_nas.cache_gb;
    out->backup = g_nas.backup;
}

static volatile int g_nas_restart; /* the settings changed while a scan was running: start again when it ends */

int cdie_nas_set(const cdie_nas_cfg *in)
{
    char text[4000];
    size_t k = 0;
#define ADD(...) k += (size_t)snprintf(text + k, k < sizeof text ? sizeof text - k : 0, __VA_ARGS__)
    const char *p = in->path;
    while (*p == '/')
        p++;
    ADD("# Written by the settings screen. One key=value per line; see the README for the rest.\n");
    ADD("enabled=%d\nserver=%s\nexport=%s\npath=%s\n", in->enabled ? 1 : 0, in->server, in->share, p);
    if (in->version == 3 || in->version == 4)
        ADD("version=%d\n", in->version);
    ADD("reserved_port=%d\ncache_gb=%d\n", in->reserved ? 1 : 0, in->cache_gb >= 1 ? in->cache_gb : 10);
    ADD("backup_saves=%d\n", in->backup ? 1 : 0);
    /* what the screen does not show, as it was */
    if (g_nas.uid >= 0) ADD("uid=%d\n", g_nas.uid);
    if (g_nas.gid >= 0) ADD("gid=%d\n", g_nas.gid);
    if (g_nas.nfsport > 0) ADD("nfsport=%d\n", g_nas.nfsport);
    if (g_nas.mountport > 0) ADD("mountport=%d\n", g_nas.mountport);
    if (g_nas.timeout_s != 10) ADD("timeout=%d\n", g_nas.timeout_s);
    {
        char def[1100];
        snprintf(def, sizeof def, "%s/cache", g_data_dir);
        if (strcmp(g_cache_dir, def) != 0)
            ADD("cache_dir=%s\n", g_cache_dir);
    }
#undef ADD
    if (k >= sizeof text || setting_write(g_options_dir, "nfs.cfg", text) != 0)
        return -1;
    elogf("[host] NAS settings saved: %s %s:%s folder '%s'", in->enabled ? "on" : "OFF", in->server, in->share, p);
    if (g_nas_state == 1) { /* a scan is reading the old settings: start again once it ends */
        g_nas_restart = 1;
        return 0;
    }
    nas_cfg_load(g_options_dir);
#ifdef HAVE_NFS
    if (g_nas_mtx) {
        pl_lock(g_nas_mtx);
        free(g_nas_cache); /* the old share's games go */
        g_nas_cache = NULL;
        g_nas_ncache = 0;
        g_nas_msg[0] = 0;
        g_nas_state = 0;
        pl_unlock(g_nas_mtx);
    }
#endif
    nas_start(g_save_dir);
    g_rescan_req = 1;
    return 0;
}

/* called from cdie_lib_update: settings saved during a scan */
static void nas_restart_pending(void)
{
    if (!g_nas_restart || g_nas_state == 1)
        return;
    g_nas_restart = 0;
    nas_cfg_load(g_options_dir);
#ifdef HAVE_NFS
    pl_lock(g_nas_mtx);
    free(g_nas_cache);
    g_nas_cache = NULL;
    g_nas_ncache = 0;
    g_nas_state = 0;
    pl_unlock(g_nas_mtx);
#endif
    nas_start(g_save_dir);
    g_rescan_req = 1;
}

int cdie_covers_on(void)
{
    return g_download_ok;
}

void cdie_covers_set(int on)
{
    if (on)
        setting_remove(g_data_dir, "no_cover_download");
    else
        setting_write(g_data_dir, "no_cover_download", "(this file switches the cover downloads off)\n");
    cv_stop();
    g_download_ok = on != 0;
    if (on) {
        cv_plan_good = -1;
        cv_start();
    }
}

int cdie_local_ip(char *out, size_t n)
{
    unsigned char me[4];
    if (local_ipv4(me) != 0) {
        snprintf(out, n, "%s", "");
        return -1;
    }
    snprintf(out, n, "%u.%u.%u.%u", me[0], me[1], me[2], me[3]);
    return 0;
}

void cdie_dns_get(char *out, size_t n)
{
    char p[1200], line[128];
    out[0] = 0;
    setting_path(p, sizeof p, g_data_dir, "dns.txt");
    FILE *f = fopen(p, "r");
    if (!f)
        return;
    if (fgets(line, sizeof line, f)) {
        line[strcspn(line, " \t\r\n#")] = 0;
        unsigned char ip[4];
        if (parse_ipv4(line, ip))
            snprintf(out, n, "%s", line);
    }
    fclose(f);
}

void cdie_dns_set(const char *ip)
{
    char line[64];
    unsigned char a[4];
    if (!ip || !*ip || !parse_ipv4(ip, a)) {
        setting_remove(g_data_dir, "dns.txt"); /* automatic: the router, then public servers */
        return;
    }
    snprintf(line, sizeof line, "%s\n", ip);
    setting_write(g_data_dir, "dns.txt", line);
}

/* ---- network jobs for the settings screen: finding NFS servers, their shares, folders, the BIOS ---- */

static struct {
    pl_thread *thread;
    volatile int state; /* -1 idle, 0 working, 1 done, 2 failed */
    volatile int cancel;
    int what;
    char ip[64], share[300], path[700];
    char msg[300];
    cdie_net_item *items;
    int nitems, cap;
    volatile uint64_t done, total;
} g_net = {.state = -1};
static pl_mutex *g_net_job_mtx;

static void net_say(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void net_say(const char *fmt, ...)
{
    va_list ap;
    pl_lock(g_net_job_mtx);
    va_start(ap, fmt);
    vsnprintf(g_net.msg, sizeof g_net.msg, fmt, ap);
    va_end(ap);
    pl_unlock(g_net_job_mtx);
}

static void net_add(const char *name, const char *path, int kind, int count)
{
    pl_lock(g_net_job_mtx);
    if (g_net.nitems == g_net.cap) {
        int cap = g_net.cap ? g_net.cap * 2 : 64;
        cdie_net_item *n = realloc(g_net.items, (size_t)cap * sizeof *n);
        if (!n) {
            pl_unlock(g_net_job_mtx);
            return;
        }
        g_net.items = n;
        g_net.cap = cap;
    }
    cdie_net_item *it = &g_net.items[g_net.nitems++];
    memset(it, 0, sizeof *it);
    snprintf(it->name, sizeof it->name, "%s", name);
    snprintf(it->path, sizeof it->path, "%s", path ? path : name);
    it->kind = kind;
    it->count = count;
    pl_unlock(g_net_job_mtx);
}

static int item_cmp(const void *a, const void *b)
{
    const cdie_net_item *x = a, *y = b;
    if (x->kind != y->kind)
        return x->kind - y->kind;
    return strcasecmp(x->name, y->name);
}

#ifdef HAVE_NFS

static void count_share(const char *share, void *user)
{
    (void)share;
    (*(int *)user)++;
}

static void add_share(const char *share, void *user)
{
    (void)user;
    net_add(share, share, CDIE_NET_SHARE, 0);
}

/* Asks every machine the broadcasts reach for its NFS port (the portmapper's GETPORT, over UDP): the console's
 * own /24 is scanned anyway, but a NAS on the same network with a wider mask (10.0.0.8 beside a console at
 * 10.0.3.21 on a /16) only answers this. Fills addrs (4 bytes each), returns how many. */
static int bcast_find(const unsigned char me[4], unsigned char addrs[][4], int max)
{
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0)
        return 0;
    int yes = 1;
    setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &yes, sizeof yes);
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
    const uint32_t xid = 0x5ee5d000u ^ (uint32_t)(pc_ms());
    const uint32_t call[14] = {xid, 0, 2, 100000, 2, 3, 0, 0, 0, 0, 100003, 3, 6, 0}; /* GETPORT nfs v3 tcp */
    unsigned char pkt[56];
    for (int i = 0; i < 14; i++) {
        pkt[i * 4] = (unsigned char)(call[i] >> 24);
        pkt[i * 4 + 1] = (unsigned char)(call[i] >> 16);
        pkt[i * 4 + 2] = (unsigned char)(call[i] >> 8);
        pkt[i * 4 + 3] = (unsigned char)call[i];
    }
    /* everyone on the segment, then the console's /24, /16 and /8 as directed broadcasts (harmless when wrong) */
    const unsigned char to[4][4] = {{255, 255, 255, 255}, {me[0], me[1], me[2], 255}, {me[0], me[1], 255, 255},
                                    {me[0], 255, 255, 255}};
    for (int round = 0; round < 2; round++)
        for (int t = 0; t < 4; t++) {
            struct sockaddr_in sa;
            memset(&sa, 0, sizeof sa);
            sa.sin_family = AF_INET;
            sa.sin_port = htons(111);
            memcpy(&sa.sin_addr, to[t], 4);
            sendto(fd, pkt, sizeof pkt, 0, (struct sockaddr *)&sa, sizeof sa);
        }
    int n = 0;
    const double until = pc_ms() + 1500.0;
    while (n < max && !g_net.cancel) {
        const int wait = (int)(until - pc_ms());
        if (wait <= 0)
            break;
        struct pollfd pf = {fd, POLLIN, 0};
        if (poll(&pf, 1, wait) <= 0)
            break;
        unsigned char r[256];
        struct sockaddr_in from;
        socklen_t fl = sizeof from;
        const ssize_t got = recvfrom(fd, r, sizeof r, 0, (struct sockaddr *)&from, &fl);
        if (got < 24)
            continue;
#define U32(o) ((uint32_t)r[o] << 24 | (uint32_t)r[(o) + 1] << 16 | (uint32_t)r[(o) + 2] << 8 | r[(o) + 3])
        /* a reply to our call, accepted (any port: a NAS with only NFS 4 still runs the portmapper) */
        if (U32(0) != xid || U32(4) != 1 || U32(8) != 0)
            continue;
        const uint32_t vlen = U32(16);
        if (vlen > 400 || 20 + ((vlen + 3) & ~3u) + 4 > (uint32_t)got || U32(20 + ((vlen + 3) & ~3u)) != 0)
            continue;
#undef U32
        unsigned char a[4];
        memcpy(a, &from.sin_addr, 4);
        if (!memcmp(a, me, 4))
            continue;
        int dup = 0;
        for (int i = 0; i < n; i++)
            dup |= !memcmp(addrs[i], a, 4);
        if (!dup)
            memcpy(addrs[n++], a, 4);
    }
    close(fd);
    return n;
}

/* every address of the console's /24 network, 64 at a time, on the NFS port; and a broadcast for the rest */
static void net_find(void)
{
    unsigned char me[4];
    if (local_ipv4(me) != 0) {
        unsigned char guess[4];
        if (g_nas.server[0] && parse_ipv4(g_nas.server, guess))
            memcpy(me, guess, 4);
        else {
            net_say("This console's network address is not known (is it connected?)");
            g_net.state = 2;
            return;
        }
    }
    int found[256] = {0}, nfound = 0;
    g_net.total = 254;
    net_say("Asking the network for NFS servers ...");
    unsigned char bc[32][4];
    const int nbc = bcast_find(me, bc, 32);
    elogf("[host] NAS search from %u.%u.%u.%u: %d answer(s) to the broadcast", me[0], me[1], me[2], me[3], nbc);
    for (int base = 1; base <= 254 && !g_net.cancel; base += 64) {
        int fds[64], hosts[64], n = 0;
        net_say("Looking for NFS servers on %u.%u.%u.x ... (%d of 254)", me[0], me[1], me[2], base - 1);
        for (int h = base; h < base + 64 && h <= 254; h++) {
            if (h == me[3])
                continue;
            int fd = socket(AF_INET, SOCK_STREAM, 0);
            if (fd < 0)
                continue;
            fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
            struct sockaddr_in sa;
            memset(&sa, 0, sizeof sa);
            sa.sin_family = AF_INET;
            sa.sin_port = htons(2049);
            unsigned char ip[4] = {me[0], me[1], me[2], (unsigned char)h};
            memcpy(&sa.sin_addr, ip, 4);
            int r = connect(fd, (struct sockaddr *)&sa, sizeof sa);
            if (r == 0) {
                found[nfound++] = h;
                close(fd);
                continue;
            }
            if (errno != EINPROGRESS) {
                close(fd);
                continue;
            }
            fds[n] = fd;
            hosts[n] = h;
            n++;
        }
        /* up to 0.8 s for this batch to answer */
        const double until = pc_ms() + 800.0;
        int left = n;
        while (left > 0 && !g_net.cancel) {
            struct pollfd pf[64];
            int map[64], m = 0;
            for (int i = 0; i < n; i++)
                if (fds[i] >= 0) {
                    pf[m].fd = fds[i];
                    pf[m].events = POLLOUT;
                    pf[m].revents = 0;
                    map[m++] = i;
                }
            const int wait = (int)(until - pc_ms());
            if (wait <= 0 || poll(pf, (unsigned)m, wait) <= 0)
                break;
            for (int k = 0; k < m; k++) {
                if (!pf[k].revents)
                    continue;
                const int i = map[k];
                int err = 0;
                socklen_t len = sizeof err;
                getsockopt(fds[i], SOL_SOCKET, SO_ERROR, &err, &len);
                if (err == 0)
                    found[nfound++] = hosts[i];
                close(fds[i]);
                fds[i] = -1;
                left--;
            }
        }
        for (int i = 0; i < n; i++)
            if (fds[i] >= 0)
                close(fds[i]);
        g_net.done = (uint64_t)(base + 63 > 254 ? 254 : base + 63);
    }
    if (g_net.cancel) {
        g_net.state = 2;
        net_say("Stopped");
        return;
    }
    /* the broadcast's answers outside the /24 just scanned, when their NFS port opens */
    unsigned char other[32][4];
    int nother = 0;
    for (int i = 0; i < nbc && !g_net.cancel; i++) {
        if (!memcmp(bc[i], me, 3))
            continue; /* (the scan has those) */
        char ip[32], why[200];
        snprintf(ip, sizeof ip, "%u.%u.%u.%u", bc[i][0], bc[i][1], bc[i][2], bc[i][3]);
        net_say("Checking %s ...", ip);
        if (nfsx_probe(ip, 2049, 1500, why, sizeof why) == 0)
            memcpy(other[nother++], bc[i], 4);
        else
            elogf("[host] NAS search: %s answered the broadcast but not on the NFS port (%s)", ip, why);
    }
    const int total_found = nfound + nother;
    for (int i = 0; i < total_found && !g_net.cancel; i++) {
        char ip[32], why[200];
        if (i < nfound)
            snprintf(ip, sizeof ip, "%u.%u.%u.%d", me[0], me[1], me[2], found[i]);
        else
            snprintf(ip, sizeof ip, "%u.%u.%u.%u", other[i - nfound][0], other[i - nfound][1], other[i - nfound][2],
                     other[i - nfound][3]);
        net_say("Found %s, asking for its shares ...", ip);
        int shares = 0;
        if (nfsx_exports(ip, 3, count_share, &shares, why, sizeof why) < 0)
            shares = -1;
        net_add(ip, ip, CDIE_NET_SERVER, shares);
    }
    pl_lock(g_net_job_mtx);
    qsort(g_net.items, (size_t)g_net.nitems, sizeof *g_net.items, item_cmp);
    pl_unlock(g_net_job_mtx);
    if (!total_found)
        net_say("No NFS server found from %u.%u.%u.%u. Is NFS on, on the NAS? On another network? Enter its address",
                me[0], me[1], me[2], me[3]);
    else
        net_say("%d NFS server(s) found", total_found);
    g_net.state = 1;
}

static nfsx *net_open(char *why, size_t wn)
{
    nfsx_opts o = {g_nas.version, g_nas.nfsport, g_nas.mountport, g_nas.reserved, g_nas.uid, g_nas.gid,
                   g_nas.timeout_s * 1000};
    if (nfsx_probe(g_net.ip, g_nas.nfsport > 0 ? g_nas.nfsport : 2049, 4000, why, wn) != 0)
        return NULL;
    nfsx *n = nfsx_open(g_net.ip, g_net.share, &o, why, wn);
    if (!n && o.reserved) { /* a title may not be allowed a reserved port: try without it */
        char why2[300];
        o.reserved = 0;
        n = nfsx_open(g_net.ip, g_net.share, &o, why2, sizeof why2);
        if (n)
            elogf("[host] NAS: mounted only without a reserved source port (set reserved_port=0)");
    }
    return n;
}

typedef struct {
    int games;
} folder_t;

static void folder_cb(const char *path, int is_dir, uint64_t size, void *user)
{
    (void)size;
    folder_t *f = user;
    const char *name = strrchr(path, '/');
    name = name ? name + 1 : path;
    if (is_dir)
        net_add(name, path, CDIE_NET_FOLDER_ITEM, 0);
    else if (has_ext(name, "cue") || has_ext(name, "chd") || has_ext(name, "iso") || has_ext(name, "7z"))
        f->games++;
}

static void net_folder(void)
{
    char why[500];
    net_say("Opening %s:%s ...", g_net.ip, g_net.share);
    nfsx *n = net_open(why, sizeof why);
    if (!n) {
        net_say("%s", why);
        g_net.state = 2;
        return;
    }
    folder_t f = {0};
    if (nfsx_scan(n, g_net.path, 0, folder_cb, &f) < 0) {
        net_say("Cannot open the folder '%s': %s", g_net.path[0] ? g_net.path : "/", nfsx_error(n));
        nfsx_close(n);
        g_net.state = 2;
        return;
    }
    nfsx_close(n);
    pl_lock(g_net_job_mtx);
    qsort(g_net.items, (size_t)g_net.nitems, sizeof *g_net.items, item_cmp);
    pl_unlock(g_net_job_mtx);
    g_net.done = (uint64_t)f.games;
    net_say("%d game file%s in this folder, %d folder%s", f.games, f.games == 1 ? "" : "s", g_net.nitems,
            g_net.nitems == 1 ? "" : "s");
    g_net.state = 1;
}

static void bios_cb(const char *path, int is_dir, uint64_t size, void *user)
{
    (void)size;
    unsigned long *seen = user;
    (*seen)++;
    if ((*seen & 255) == 0)
        net_say("Looking for cdimono1.zip ... %lu files and folders so far", *seen);
    if (is_dir)
        return;
    const char *name = strrchr(path, '/');
    name = name ? name + 1 : path;
    if (!strcasecmp(name, "cdimono1.zip"))
        net_add(name, path, CDIE_NET_FILE, (int)(size >> 10));
}

static void net_bios_find(void)
{
    char why[500];
    net_say("Opening %s:%s ...", g_net.ip, g_net.share);
    nfsx *n = net_open(why, sizeof why);
    if (!n) {
        net_say("%s", why);
        g_net.state = 2;
        return;
    }
    unsigned long seen = 0;
    nfsx_scan_until(n, g_net.path, 6, bios_cb, &seen, &g_net.cancel);
    nfsx_close(n);
    if (g_net.cancel) {
        net_say("Stopped after %lu files and folders", seen);
        g_net.state = 2;
        return;
    }
    if (g_net.nitems)
        net_say("Found %d cdimono1.zip on the share: pick one to copy", g_net.nitems);
    else
        net_say("No cdimono1.zip on the share (%lu files and folders looked at)", seen);
    g_net.state = 1;
}

static void net_bios_copy(void)
{
    char why[500], dir[1200], dest[1300], part[1400];
    net_say("Opening %s:%s ...", g_net.ip, g_net.share);
    nfsx *n = net_open(why, sizeof why);
    if (!n) {
        net_say("%s", why);
        g_net.state = 2;
        return;
    }
    snprintf(dir, sizeof dir, "%s/same_cdi/bios", g_system_dir);
    mkdirs(dir);
    snprintf(dest, sizeof dest, "%s/cdimono1.zip", dir);
    snprintf(part, sizeof part, "%s.part", dest);
    const int64_t size = nfsx_size(n, g_net.path);
    g_net.total = size > 0 ? (uint64_t)size : 0;
    net_say("Copying %s ...", g_net.path);
    const int r = nfsx_copy(n, g_net.path, part, &g_net.done, &g_net.cancel, why, sizeof why);
    nfsx_close(n);
    if (r != 0 || rename(part, dest) != 0) {
        remove(part);
        net_say(r == 1 ? "Stopped" : "The copy failed: %s", why);
        g_net.state = 2;
        return;
    }
    chmod(dest, 0666);
    net_say(bios_present() ? "The BIOS is in place%s" : "Copied, but the file does not hold the CD-i BIOS ROMs%s",
            bios_present() && !bios_has("vmpega.rom") ? " (without vmpega.rom: no digital video)" : "");
    elogf("[host] BIOS copied from %s:%s/%s", g_net.ip, g_net.share, g_net.path);
    g_net.state = bios_present() ? 1 : 2;
}
#endif

static int net_job(void *arg)
{
    (void)arg;
#ifdef HAVE_NFS
    switch (g_net.what) {
    case CDIE_NET_FIND: net_find(); break;
    case CDIE_NET_SHARES: {
        char why[300];
        net_say("Asking %s for its shares ...", g_net.ip);
        const int r = nfsx_exports(g_net.ip, 5, add_share, NULL, why, sizeof why);
        if (r < 0) {
            net_say("%s", why);
            g_net.state = 2;
        } else {
            pl_lock(g_net_job_mtx);
            qsort(g_net.items, (size_t)g_net.nitems, sizeof *g_net.items, item_cmp);
            pl_unlock(g_net_job_mtx);
            if (r)
                net_say("%d share(s) on %s", r, g_net.ip);
            else
                net_say("%s offers no shares to this console", g_net.ip);
            g_net.state = 1;
        }
        break;
    }
    case CDIE_NET_FOLDER: net_folder(); break;
    case CDIE_NET_BIOS_FIND: net_bios_find(); break;
    case CDIE_NET_BIOS_COPY: net_bios_copy(); break;
    default: g_net.state = 2; break;
    }
#else
    net_say("This build has no NAS support");
    g_net.state = 2;
#endif
    return 0;
}

int cdie_net_start(int what, const char *ip, const char *share, const char *path)
{
    if (!g_net_job_mtx)
        g_net_job_mtx = pl_mutex_create();
    if (g_net.thread) {
        if (g_net.state == 0)
            return -1; /* one at a time */
        pl_thread_wait(g_net.thread);
        g_net.thread = NULL;
    }
    pl_lock(g_net_job_mtx);
    g_net.nitems = 0;
    g_net.what = what;
    g_net.cancel = 0;
    g_net.done = g_net.total = 0;
    snprintf(g_net.ip, sizeof g_net.ip, "%s", ip ? ip : "");
    snprintf(g_net.share, sizeof g_net.share, "%s", share ? share : "");
    snprintf(g_net.path, sizeof g_net.path, "%s", path ? path : "");
    g_net.msg[0] = 0;
    g_net.state = 0;
    pl_unlock(g_net_job_mtx);
    g_net.thread = pl_thread_create(net_job, "settings-net", NULL);
    if (!g_net.thread) {
        g_net.state = 2;
        return -1;
    }
    return 0;
}

int cdie_net_poll(char *msg, size_t n, uint64_t *done, uint64_t *total)
{
    if (!g_net_job_mtx)
        return -1;
    pl_lock(g_net_job_mtx);
    if (msg && n)
        snprintf(msg, n, "%s", g_net.msg);
    pl_unlock(g_net_job_mtx);
    if (done)
        *done = g_net.done;
    if (total)
        *total = g_net.total;
    return g_net.state;
}

int cdie_net_items(cdie_net_item *out, int max)
{
    if (!g_net_job_mtx)
        return 0;
    pl_lock(g_net_job_mtx);
    const int n = g_net.nitems < max ? g_net.nitems : max;
    if (n > 0)
        memcpy(out, g_net.items, (size_t)n * sizeof *out);
    pl_unlock(g_net_job_mtx);
    return n;
}

void cdie_net_cancel(void)
{
    g_net.cancel = 1;
}

static void net_stop(void)
{
    g_net.cancel = 1;
    if (g_net.thread) {
        pl_thread_wait(g_net.thread);
        g_net.thread = NULL;
    }
}
