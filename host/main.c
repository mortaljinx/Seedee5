/*
 * cdihost - minimal libretro host (SDL2).
 *
 * Phase 1/4 of the ps5-cdi plan. Loads any libretro core (.so) and runs a
 * game with it. The PS5 build replaces only the "platform" parts of this
 * file (window, audio device, input polling); the libretro glue stays.
 *
 * Pacing is audio-driven: we call retro_run() only when the audio ring
 * buffer is running low, so video follows the audio clock.
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
#include <errno.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>
#include <SDL2/SDL.h>
#include "libretro.h"
#include "font8x16.h"
#include "cdi_boxarts.h"
#include "tlsget.h"
#include "arc7z.h"
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <time.h>

#include <ctype.h>
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#pragma GCC diagnostic ignored "-Wsign-compare"
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_ONLY_BMP
#define STBI_MAX_DIMENSIONS 8192
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#include "stb_image.h"
#pragma GCC diagnostic pop

/* PS5 (and RENDER_THREAD test builds): draw on a separate thread with our own
 * scaler straight into the window surface. Desktop builds keep SDL's renderer. */
#define USE_RENDER_THREAD 1

/* On the PS5 there is no console to read, so progress and errors are also
 * shown as on-screen notifications (same call the SDK's hello_world uses). */
#ifdef PS5
typedef struct {
    char useless1[45];
    char message[3075];
} notify_request_t;
int sceKernelSendNotificationRequest(int, notify_request_t *, size_t, int);

static void notify(const char *fmt, ...)
{
    notify_request_t req;
    va_list ap;
    memset(&req, 0, sizeof req);
    va_start(ap, fmt);
    vsnprintf(req.message, sizeof req.message, fmt, ap);
    va_end(ap);
    fprintf(stderr, "[notify] %s\n", req.message);
    sceKernelSendNotificationRequest(0, &req, sizeof req, 0);
}
#else
static void notify(const char *fmt, ...) { (void)fmt; }
#endif

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
} core;

#ifdef STATIC_CORE
/* PS5 build: the core is linked into the ELF, no dlopen. */
extern void retro_flush_nvram(void) __attribute__((weak));
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
    return true;
}
#else
#define LOAD(sym)                                                          \
    do {                                                                   \
        *(void **)&core.sym = dlsym(core.handle, #sym);                    \
        if (!core.sym) {                                                   \
            fprintf(stderr, "core is missing symbol %s\n", #sym);          \
            return false;                                                  \
        }                                                                  \
    } while (0)

static bool core_open(const char *path)
{
    core.handle = dlopen(path, RTLD_LAZY | RTLD_LOCAL);
    if (!core.handle) {
        fprintf(stderr, "dlopen failed: %s\n", dlerror());
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
    return true;
}
#endif /* STATIC_CORE */

/* -------------------------------------------------------------- globals */

static char g_core_path[1024];
#ifdef PS5
static char g_system_dir[1024] = "/data/ps5-cdi/system";
static char g_save_dir[1024] = "/data/ps5-cdi/saves";
#else
static char g_system_dir[1024] = "system";
static char g_save_dir[1024] = "saves";
#endif

static SDL_Window *win;
#ifndef USE_RENDER_THREAD
static SDL_Renderer *ren;
static SDL_Texture *tex;
static unsigned tex_w, tex_h;
#endif
static enum retro_pixel_format g_fmt = RETRO_PIXEL_FORMAT_0RGB1555;
static float g_aspect;
static struct retro_system_av_info g_av;
static bool g_running = true;
static bool g_debug;
static volatile int g_nvram_req;       /* set by the L1+R1+Square shortcut */
static volatile sig_atomic_t g_signal; /* set when the system asks us to stop */
static volatile int g_view_mode;      /* 0 = 4:3, 1 = 16:9 (fill the screen) */

/* Live input readout (L2 + R1 + Square): what the pad reports and what is sent to the core. */
static volatile int g_dbg_overlay;
static struct {
    int ax[6];            /* raw axes: left x/y, right x/y, L2, R2 */
    int arrows[4];        /* the D-pad as the console reports it: left right up down */
    unsigned mask;        /* libretro button mask */
    int cx, cy;           /* counts handed to the core in the last frame */
    int tot_x, tot_y;     /* ... and in total since the readout was switched on */
    int est_x, est_y;     /* where the host believes the pointer is */
    int mode, speed, yrange;
    int btn;              /* CD-i buttons held: bit 0 = button 1, bit 1 = button 2 */
} g_dbg;
static void draw_text(SDL_Surface *s, int x, int y, int sc, Uint32 col, const char *t, int max_chars);
/* The CD-i frame carries dark margins of its own, and they differ from screen
 * to screen, so there is no single crop that fits everything. Crops are in
 * tenths of a percent of the frame: left, right, top, bottom.
 *   crop mode 0 = off (whole frame)
 *   crop mode 1 = manual: a fixed crop for each aspect (4:3 and 16:9)
 *   crop mode 2 = auto: follows the picture on screen */
enum { CROP_OFF = 0, CROP_MANUAL = 1, CROP_AUTO = 2 };
static const int CROP_DEF[2][4] = {
    {0, 0, 75, 185},   /* 4:3  - values tuned on the console */
    {30, 30, 75, 135}, /* 16:9 - values tuned on the console */
};
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
        fprintf(stderr, "bad --opt '%s' (want key=value)\n", kv);
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

static SDL_AudioDeviceID adev;
static bool audio_started;
/* Sound is played from a buffer that is topped up one video frame at a time. The
 * buffer's low-water mark is g_audio_ms (the sound is on average about half a frame
 * later than that, plus the device's own buffer): lower = sound earlier, but too low
 * and it runs dry (the "audio underruns" in the perf report). */
static int g_audio_ms = 30;
static int g_audio_samples = 512; /* the device's buffer: 256, 512 or 1024 */
static void audio_cfg_save(void);

/* Pointer (the CD-i's mouse) speed from the pad. 100 % means the sticks move the pointer
 * at up to PTR_MAX_COUNTS per video frame at full tilt; small movements are slow and fine
 * (a curve), and the D-pad, which the console also reports as arrow keys, speeds up the
 * longer it is held. L2 + R1 + D-pad up/down changes the percentage. */
#define PTR_MAX_COUNTS 14.0f
#define PTR_PRECISION 0.4f /* holding L1 slows the pointer to this fraction, for fine aiming */
static int g_ptr_speed = 100;
static void ptr_cfg_save(void);
static int g_pad_adjusting; /* L2 + R1 held: the D-pad is changing a setting, not moving the pointer */

/* Pointer modes. 0 = relative: the sticks push the pointer along (what it did so far).
 * 1 = absolute: the stick position IS the pointer position, like a light gun - tilt right
 * and the crosshair is at the right edge at once; release and it returns to the centre.
 * The CD-i keeps an absolute pointer position (768 x 560 units) inside the player, so the
 * host tracks where it thinks that is (est_x/est_y, updated from every count it hands
 * over) and sends the difference to the target. Pushing to an edge overshoots on purpose,
 * which re-synchronises the estimate. L2 + R1 + Triangle switches modes. */
#define PTR_DEV_W 768
static int g_ptr_mode;
static char g_game_name[200]; /* the game as the list names it (a .7z's own name, not the file inside), for per-game settings */

/* The CD-i's buttons. The emulated player has two (button 1 and button 2; a third is ignored). In this
 * core they come from the RetroPad's Y (button 1) and B (button 2), not from mouse clicks, which are
 * dropped. Each pad button below is mapped to a mask: 1 = button 1, 2 = button 2, 3 = both together,
 * 0 = none. Order: Cross, Circle, Square, Triangle, L1, R1, L2, R2. Per game in options.cfg:
 * host_button_cross=1 (or 2, 12 for both, none), and circle, square, triangle, l1, r1, l2, r2. */
static int g_btn_map[8] = {1, 2, 1, 2, 0, 0, 0, 1};
static int g_dpad_digital; /* options.cfg host_dpad_digital=1: also hand the D-pad to the core as buttons (a fixed 2-unit step per frame) */
static int g_ptr_yrange = 560; /* options.cfg host_pointer_y_range: try 280 if the pointer reaches the bottom too early */
static int est_x, est_y, est_valid;

/* ----------------------------------------------------------------- misc */

static void mkdirs(const char *path)
{
    char tmp[1024];
    snprintf(tmp, sizeof tmp, "%s", path);
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = 0;
            mkdir(tmp, 0755);
            *p = '/';
        }
    }
    mkdir(tmp, 0755);
}

static void log_cb(enum retro_log_level level, const char *fmt, ...)
{
    static const char *names[] = {"DEBUG", "INFO", "WARN", "ERROR"};
    if (level == RETRO_LOG_DEBUG)
        return;
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "[core %s] ", names[level <= 3 ? level : 3]);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
}

/* ---------------------------------------------------------------- audio */

#define RB_FRAMES 16384 /* stereo frames, power of two */
static int16_t rb[RB_FRAMES * 2];
static size_t rb_r, rb_w, rb_count;
static unsigned long underruns, overruns;

static void SDLCALL audio_cb(void *ud, Uint8 *stream, int len)
{
    (void)ud; /* SDL already holds the device lock here */
    int16_t *out = (int16_t *)stream;
    size_t want = (size_t)len / (sizeof(int16_t) * 2);
    size_t n = want < rb_count ? want : rb_count;
    for (size_t i = 0; i < n; i++) {
        out[i * 2] = rb[rb_r * 2];
        out[i * 2 + 1] = rb[rb_r * 2 + 1];
        rb_r = (rb_r + 1) & (RB_FRAMES - 1);
    }
    rb_count -= n;
    if (n < want) {
        memset(out + n * 2, 0, (want - n) * sizeof(int16_t) * 2);
        underruns++;
    }
}

static size_t audio_batch_cb(const int16_t *data, size_t frames)
{
    if (!adev)
        return frames;
    SDL_LockAudioDevice(adev);
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
    SDL_UnlockAudioDevice(adev);
    return frames;
}

static void audio_sample_cb(int16_t l, int16_t r)
{
    int16_t s[2] = {l, r};
    audio_batch_cb(s, 1);
}

static bool audio_open(unsigned rate)
{
    SDL_AudioSpec want, have;
    SDL_zero(want);
    want.freq = (int)rate;
    want.format = AUDIO_S16SYS;
    want.channels = 2;
    want.samples = (Uint16)g_audio_samples;
    want.callback = audio_cb;
    adev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    if (!adev) {
        fprintf(stderr, "audio open failed: %s\n", SDL_GetError());
        notify("cdihost: audio open failed: %s", SDL_GetError());
        return false;
    }
    SDL_PauseAudioDevice(adev, 1); /* unpause once the buffer is primed */
    return true;
}

static size_t audio_buffered(void)
{
    if (!adev)
        return 0;
    SDL_LockAudioDevice(adev);
    size_t n = rb_count;
    SDL_UnlockAudioDevice(adev);
    return n;
}

/* ---------------------------------------------------------- in-game menu */

/* The touchpad (Tab on a keyboard) pauses the game and opens this menu. The main thread owns the menu; the render
 * thread only draws a copy of it over the paused picture. */
typedef struct {
    char label[40];
    char value[56];
    int kind; /* MI_* */
} menu_item_t;

enum { MI_RESUME, MI_SAVE, MI_LOAD, MI_DISC, MI_PTRMODE, MI_PTRSPEED, MI_SMOOTH, MI_LATENCY, MI_VIEW, MI_RESET, MI_LIST, MI_QUIT };

static struct {
    volatile int open;
    int sel, n;
    menu_item_t it[16];
    char title[100];
    char msg[100];
} g_menu;
static SDL_mutex *g_menu_mtx;
static volatile int g_menu_req;   /* the touchpad was pressed while playing */
static volatile int g_input_hold; /* after the menu closes: ignore the pad until its buttons are released */
static volatile int g_vid_redraw; /* draw the last picture again (the menu changed while the game is paused) */
static volatile int g_vid_reset;  /* the next picture repaints the whole screen (after the launcher drew on it) */

/* The core's disc tray (libretro disc control), given to us when a game is loaded. */
static struct retro_disk_control_callback g_disk;
static int g_have_disk;

/* ---------------------------------------------------------------- video */

static double pc_ms(void) /* a monotonic clock in milliseconds */
{
    return (double)SDL_GetPerformanceCounter() * 1000.0 / (double)SDL_GetPerformanceFrequency();
}

/* Video smoothing: the console shows 60 pictures a second, a PAL CD-i makes 50. Showing each frame until the
 * next one arrives repeats one picture in six, and scrolling stutters. With smoothing, the picture shown at
 * every refresh is a blend of the two newest frames, weighted by where that refresh falls between them. It is
 * switched off by itself when the display rate and the frame rate match (an NTSC game at 60). The cost is
 * about one frame (20 ms) of extra delay and a slight softness on fast motion. L2 + R1 + L3 toggles it.
 * options.cfg: host_smooth_video=0 or 1, host_smooth_strength=0..100 (how much of the blend is used). */
static volatile int g_smooth = 1;
static int g_smooth_strength = 100;

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
    fprintf(stderr, "[host] video %ux%u fmt %d nonblack %zu | px(0,0)=%08x px(centre)=%08x | frames %lu dupes %lu\n",
            w, h, (int)g_fmt, nonzero, r0[0], rc[w / 2], g_frames_in, g_dupes);
}

/* Which part of the frame is shown (src) and where on screen (dst).
 * Two outputs only: a 4:3 box centred on the screen, or the full 16:9 screen.
 * The picture (cropped, if crop is on) is stretched to fill that box. */
static void compute_view(int fw, int fh, int ow, int oh, SDL_Rect *src, SDL_Rect *dst)
{
    const volatile int *c = g_crop[g_view_mode ? 1 : 0];
    if (g_crop_mode == CROP_AUTO)
        c = g_auto;
    if (g_crop_mode != CROP_OFF) {
        src->x = fw * c[0] / 1000;
        src->y = fh * c[2] / 1000;
        src->w = fw - src->x - fw * c[1] / 1000;
        src->h = fh - src->y - fh * c[3] / 1000;
    } else {
        src->x = 0; src->y = 0; src->w = fw; src->h = fh;
    }
    if (src->w < 16 || src->h < 16) { /* never crop the picture away */
        src->x = 0; src->y = 0; src->w = fw; src->h = fh;
    }
    if (g_view_mode == 1) {
        dst->x = 0; dst->y = 0; dst->w = ow; dst->h = oh;
    } else {
        int dw = oh * 4 / 3, dh = oh;
        if (dw > ow) {
            dw = ow;
            dh = ow * 3 / 4;
        }
        dst->x = (ow - dw) / 2; dst->y = (oh - dh) / 2; dst->w = dw; dst->h = dh;
    }
}

#ifndef USE_RENDER_THREAD
static Uint32 sdl_format(void)
{
    switch (g_fmt) {
    case RETRO_PIXEL_FORMAT_XRGB8888: return SDL_PIXELFORMAT_ARGB8888;
    case RETRO_PIXEL_FORMAT_RGB565:   return SDL_PIXELFORMAT_RGB565;
    default:                          return SDL_PIXELFORMAT_RGB555;
    }
}

/* Uploads a frame into the streaming texture (desktop renderer path). */
static void texture_update(const void *data, unsigned w, unsigned h, size_t pitch)
{
    if (!tex || w != tex_w || h != tex_h) {
        if (tex)
            SDL_DestroyTexture(tex);
        tex = SDL_CreateTexture(ren, sdl_format(), SDL_TEXTUREACCESS_STREAMING,
                                (int)w, (int)h);
        if (!tex) {
            fprintf(stderr, "texture failed: %s\n", SDL_GetError());
            return;
        }
        tex_w = w;
        tex_h = h;
    }
    SDL_UpdateTexture(tex, NULL, data, (int)pitch);
    debug_scan(data, w, h, pitch);
}

static void render(void)
{
    SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);
    SDL_RenderClear(ren);
    if (tex) {
        int ow, oh;
        SDL_Rect src, r;
        SDL_GetRendererOutputSize(ren, &ow, &oh);
        compute_view((int)tex_w, (int)tex_h, ow, oh, &src, &r);
        SDL_RenderCopy(ren, tex, &src, &r);
    }
    SDL_RenderPresent(ren);
}
#endif /* !USE_RENDER_THREAD */

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

#ifdef USE_RENDER_THREAD
/* Drawing runs on its own thread: presenting a frame on the PS5 waits for the
 * display's vertical sync, and that wait must not stall the emulation. The
 * emulator hands over the newest frame; if the drawing thread is still busy,
 * the older pending frame is simply replaced (automatic frame skipping). */
typedef struct {
    uint8_t *data;
    size_t cap;
    unsigned w, h;
    size_t pitch;
    double t; /* when the emulator made this frame (pc_ms) */
} frame_t;

static frame_t frame_in, frame_out; /* swapped under vid_mtx */
static SDL_mutex *vid_mtx;
static SDL_cond *vid_cond;
static bool vid_ready, vid_quit, vid_init_done;
static SDL_Thread *vid_thread;
static unsigned long vid_dropped;
static volatile unsigned long vid_presented;
static volatile double vid_present_ms;

static void video_cb(const void *data, unsigned w, unsigned h, size_t pitch)
{
    if (!data || !w || !h) { /* NULL = duplicate previous frame */
        g_dupes++;
        return;
    }
    g_frames_in++;
    if (g_autofit_req)
        autofit(data, w, h, pitch);
    if (g_crop_mode == CROP_AUTO)
        auto_track(data, w, h, pitch);
    size_t need = pitch * h;
    SDL_LockMutex(vid_mtx);
    if (frame_in.cap < need) {
        free(frame_in.data);
        frame_in.data = malloc(need);
        frame_in.cap = frame_in.data ? need : 0;
    }
    if (frame_in.data) {
        memcpy(frame_in.data, data, need);
        frame_in.w = w;
        frame_in.h = h;
        frame_in.pitch = pitch;
        frame_in.t = pc_ms();
        if (vid_ready)
            vid_dropped++;
        vid_ready = true;
        SDL_CondSignal(vid_cond);
    }
    SDL_UnlockMutex(vid_mtx);
}

static bool vid_ok;
static int *xmap;
static int xmap_cap;

/* Scales one frame straight into the window surface and updates only the
 * picture area. Nearest-neighbour; each distinct source row is converted once
 * and then copied for the destination rows that repeat it. */
static void draw_frame(const uint8_t *data, unsigned fw, unsigned fh, size_t pitch)
{
    SDL_Surface *surf = SDL_GetWindowSurface(win);
    if (!surf || !data || g_fmt != RETRO_PIXEL_FORMAT_XRGB8888)
        return;
    const int ow = surf->w, oh = surf->h;
    SDL_Rect src, dst;
    compute_view((int)fw, (int)fh, ow, oh, &src, &dst);
    if (dst.w < 1 || dst.h < 1)
        return;

    static SDL_Rect last_dst;
    static int full_updates; /* two buffers on the PS5: refresh both after a layout change */
    static int said;
    if (g_vid_reset || dst.x != last_dst.x || dst.y != last_dst.y || dst.w != last_dst.w ||
        dst.h != last_dst.h) {
        g_vid_reset = 0;
        SDL_FillRect(surf, NULL, SDL_MapRGB(surf->format, 0, 0, 0));
        last_dst = dst;
        full_updates = 2;
    }
    if (!said) {
        said = 1;
        notify("cdihost: screen %dx%d, frame %ux%u, aspect %.3f, drawn %dx%d at %d,%d",
               ow, oh, fw, fh, g_aspect, dst.w, dst.h, dst.x, dst.y);
    }

    if (xmap_cap < dst.w) {
        free(xmap);
        xmap = malloc((size_t)dst.w * sizeof *xmap);
        xmap_cap = xmap ? dst.w : 0;
        if (!xmap)
            return;
    }
    for (int x = 0; x < dst.w; x++)
        xmap[x] = src.x + (int)(((long)x * src.w) / dst.w);

    const int swap = surf->format->format == SDL_PIXELFORMAT_ABGR8888; /* PS5: R and B swap */
    int prev_sy = -1;
    uint32_t *prev_row = NULL;
    for (int y = 0; y < dst.h; y++) {
        int sy = src.y + (int)(((long)y * src.h) / dst.h);
        uint32_t *d = (uint32_t *)((uint8_t *)surf->pixels + (size_t)(dst.y + y) * surf->pitch) + dst.x;
        if (sy == prev_sy && prev_row) {
            memcpy(d, prev_row, (size_t)dst.w * 4);
        } else {
            const uint32_t *srow = (const uint32_t *)(data + (size_t)sy * pitch);
            if (swap) {
                for (int x = 0; x < dst.w; x++) {
                    uint32_t px = srow[xmap[x]];
                    d[x] = 0xFF000000u | (px & 0x0000FF00u) | ((px >> 16) & 0xFFu) |
                           ((px & 0xFFu) << 16);
                }
            } else {
                for (int x = 0; x < dst.w; x++)
                    d[x] = 0xFF000000u | srow[xmap[x]];
            }
            prev_sy = sy;
            prev_row = d;
        }
    }

    if (g_menu.open && g_menu_mtx) { /* the in-game menu, over the paused picture */
        static menu_item_t items[16];
        char title[100], msg[100];
        int n, sel;
        SDL_LockMutex(g_menu_mtx);
        n = g_menu.n;
        sel = g_menu.sel;
        memcpy(items, g_menu.it, sizeof items);
        snprintf(title, sizeof title, "%s", g_menu.title);
        snprintf(msg, sizeof msg, "%s", g_menu.msg);
        SDL_UnlockMutex(g_menu_mtx);
        const int sc = oh >= 900 ? 2 : 1, lh = 26 * sc;
        const int bw = 60 * 8 * sc + 40 * sc, bh = (n + 4) * lh + 30 * sc;
        const int bx = (ow - bw) / 2, by = (oh - bh) / 2 > 0 ? (oh - bh) / 2 : 0;
        const Uint32 bg = SDL_MapRGB(surf->format, 14, 16, 28), edge = SDL_MapRGB(surf->format, 90, 170, 255);
        const Uint32 fg = SDL_MapRGB(surf->format, 235, 235, 240), dimc = SDL_MapRGB(surf->format, 150, 158, 180);
        const Uint32 selbg = SDL_MapRGB(surf->format, 40, 70, 120), msgc = SDL_MapRGB(surf->format, 120, 200, 140);
        SDL_Rect r = {bx - 3 * sc, by - 3 * sc, bw + 6 * sc, bh + 6 * sc};
        SDL_FillRect(surf, &r, edge);
        r = (SDL_Rect){bx, by, bw, bh};
        SDL_FillRect(surf, &r, bg);
        int y = by + 14 * sc;
        draw_text(surf, bx + 20 * sc, y, sc, edge, title, 58);
        y += lh + 6 * sc;
        for (int i = 0; i < n; i++) {
            if (i == sel) {
                SDL_Rect hl = {bx + 10 * sc, y - 4 * sc, bw - 20 * sc, lh};
                SDL_FillRect(surf, &hl, selbg);
            }
            draw_text(surf, bx + 20 * sc, y, sc, fg, items[i].label, 30);
            if (items[i].value[0]) {
                char v[64];
                snprintf(v, sizeof v, "%s%s%s", i == sel ? "< " : "", items[i].value, i == sel ? " >" : "");
                draw_text(surf, bx + bw - 20 * sc - (int)strlen(v) * 8 * sc, y, sc, i == sel ? fg : dimc, v, 40);
            }
            y += lh;
        }
        y += 6 * sc;
        if (msg[0])
            draw_text(surf, bx + 20 * sc, y, sc, msgc, msg, 58);
        y += lh;
        draw_text(surf, bx + 20 * sc, y, sc, dimc, "Cross: choose   Left/Right: change   Circle: back", 58);
        full_updates = full_updates < 1 ? 1 : full_updates; /* the box may cover the black bars too */
#ifdef CDIHOST_TEST_DUMP
        if (getenv("CDIHOST_TEST_MENUDUMP")) {
            FILE *f = fopen(getenv("CDIHOST_TEST_MENUDUMP"), "wb");
            if (f) {
                fprintf(f, "P6\n%d %d\n255\n", ow, oh);
                for (int yy = 0; yy < oh; yy++)
                    for (int x = 0; x < ow; x++) {
                        Uint32 px = ((Uint32 *)((Uint8 *)surf->pixels + yy * surf->pitch))[x];
                        if (swap) { fputc(px & 255, f); fputc((px >> 8) & 255, f); fputc((px >> 16) & 255, f); }
                        else { fputc((px >> 16) & 255, f); fputc((px >> 8) & 255, f); fputc(px & 255, f); }
                    }
                fclose(f);
            }
        }
#endif
    }
    if (g_dbg_overlay) {
        const Uint32 bg = SDL_MapRGB(surf->format, 0, 0, 0), fg = SDL_MapRGB(surf->format, 255, 255, 255);
        const Uint32 hi = SDL_MapRGB(surf->format, 255, 90, 255);
        const int sc = oh >= 900 ? 2 : 1, lh = 18 * sc;
        char line[96];
        SDL_Rect box = {dst.x + 8, dst.y + 8, 62 * 8 * sc, lh * 7 + 8};
        if (box.w > dst.w - 16) box.w = dst.w - 16;
        SDL_FillRect(surf, &box, bg);
        int y = box.y + 4;
        snprintf(line, sizeof line, "left stick %6d %6d   right stick %6d %6d", g_dbg.ax[0], g_dbg.ax[1], g_dbg.ax[2], g_dbg.ax[3]);
        draw_text(surf, box.x + 6, y, sc, fg, line, 62); y += lh;
        snprintf(line, sizeof line, "triggers L2 %6d R2 %6d   buttons %04x", g_dbg.ax[4], g_dbg.ax[5], g_dbg.mask);
        draw_text(surf, box.x + 6, y, sc, fg, line, 62); y += lh;
        snprintf(line, sizeof line, "CD-i buttons: 1[%c] 2[%c]   d-pad arrows L%d R%d U%d D%d", (g_dbg.btn & 1) ? 'X' : ' ', (g_dbg.btn & 2) ? 'X' : ' ',
                 g_dbg.arrows[0], g_dbg.arrows[1], g_dbg.arrows[2], g_dbg.arrows[3]);
        draw_text(surf, box.x + 6, y, sc, fg, line, 62); y += lh;
        snprintf(line, sizeof line, "pointer mode %s  speed %d%%  y range %d", g_dbg.mode ? "ABSOLUTE" : "relative", g_dbg.speed, g_dbg.yrange);
        draw_text(surf, box.x + 6, y, sc, fg, line, 62); y += lh;
        snprintf(line, sizeof line, "counts sent last frame: x %d  y %d", g_dbg.cx, g_dbg.cy);
        draw_text(surf, box.x + 6, y, sc, fg, line, 62); y += lh;
        snprintf(line, sizeof line, "counts sent in total:   x %d  y %d", g_dbg.tot_x, g_dbg.tot_y);
        draw_text(surf, box.x + 6, y, sc, fg, line, 62); y += lh;
        snprintf(line, sizeof line, "host thinks the pointer is at %d,%d", g_dbg.est_x, g_dbg.est_y);
        draw_text(surf, box.x + 6, y, sc, fg, line, 62);
        /* a cross where the host believes the pointer is: compare it with the game's own crosshair */
        int px = dst.x + (int)((long)g_dbg.est_x * dst.w / 768);
        int py = dst.y + (int)((long)g_dbg.est_y * dst.h / (g_dbg.yrange > 0 ? g_dbg.yrange : 560));
        SDL_Rect h = {px - 14 * sc, py - sc, 28 * sc, 2 * sc}, v = {px - sc, py - 14 * sc, 2 * sc, 28 * sc};
        SDL_FillRect(surf, &h, hi);
        SDL_FillRect(surf, &v, hi);
    }
#ifdef CDIHOST_TEST_DUMP
    {
        static int n;
        if (++n == 90) {
            FILE *f = fopen("/tmp/cdihost_dump.ppm", "wb");
            if (f) {
                fprintf(f, "P6\n%d %d\n255\n", ow, oh);
                for (int y = 0; y < oh; y++)
                    for (int x = 0; x < ow; x++) {
                        uint32_t px = ((uint32_t *)((uint8_t *)surf->pixels + (size_t)y * surf->pitch))[x];
                        uint8_t rgb[3];
                        SDL_GetRGB(px, surf->format, &rgb[0], &rgb[1], &rgb[2]);
                        fwrite(rgb, 1, 3, f);
                    }
                fclose(f);
            }
        }
    }
#endif
    if (full_updates > 0) {
        full_updates--;
        SDL_UpdateWindowSurface(win);
    } else {
        SDL_UpdateWindowSurfaceRects(win, &dst, 1);
    }
}

/* a = a*(256-w) + b*w, per channel, for xRGB pixels */
static void blend_row(uint32_t *out, const uint32_t *a, const uint32_t *b, size_t n, unsigned w)
{
    const unsigned iw = 256 - w;
    for (size_t i = 0; i < n; i++) {
        const uint32_t pa = a[i], pb = b[i];
        const uint32_t rb = (((pa & 0x00FF00FFu) * iw + (pb & 0x00FF00FFu) * w) >> 8) & 0x00FF00FFu;
        const uint32_t g = (((pa & 0x0000FF00u) * iw + (pb & 0x0000FF00u) * w) >> 8) & 0x0000FF00u;
        out[i] = rb | g;
    }
}

static int render_thread(void *arg)
{
    (void)arg;
    vid_ok = SDL_GetWindowSurface(win) != NULL;
    SDL_LockMutex(vid_mtx);
    vid_init_done = true;
    SDL_CondSignal(vid_cond);
    SDL_UnlockMutex(vid_mtx);
    if (!vid_ok)
        return 1;

    /* smoothing state: the two newest frames and when they arrived */
    frame_t hist_a = {0}, hist_b = {0};
    uint8_t *mix = NULL;
    size_t mix_cap = 0;
    double t_est = 0, period = 20.0, ref_period = 16.7, last_arrival = 0, last_loop = 0;
    int have_b = 0;

    for (;;) {
        const int smooth = g_smooth;
        SDL_LockMutex(vid_mtx);
        if (!smooth)
            while (!vid_ready && !vid_quit && !g_vid_redraw)
                SDL_CondWait(vid_cond, vid_mtx);
        g_vid_redraw = 0;
        if (vid_quit) {
            SDL_UnlockMutex(vid_mtx);
            break;
        }
        const bool got = vid_ready;
        if (got) {
            frame_t t = frame_in; /* take the newest frame, give back the old buffer */
            frame_in = frame_out;
            frame_out = t;
            vid_ready = false;
        }
        SDL_UnlockMutex(vid_mtx);

        Uint64 t0 = SDL_GetPerformanceCounter();
        const double now = pc_ms();
        if (!smooth) {
            have_b = 0; /* history is stale after a stretch without smoothing */
            debug_scan(frame_out.data, frame_out.w, frame_out.h, frame_out.pitch);
            draw_frame(frame_out.data, frame_out.w, frame_out.h, frame_out.pitch);
        } else {
            if (got) {
                frame_t tmp = hist_a; /* the newest frame becomes B, the old B becomes A */
                hist_a = hist_b;
                hist_b = frame_out;
                frame_out = tmp;
                /* when the emulator made the frame, not when this thread noticed it (it only looks once per
                 * refresh, up to 16 ms late) */
                if (have_b >= 1) {
                    const double gap = hist_b.t - last_arrival;
                    if (gap > 5.0 && gap < 80.0)
                        period += 0.05 * (gap - period);
                }
                t_est = hist_b.t;
                last_arrival = hist_b.t;
                have_b = have_b < 2 ? have_b + 1 : 2;
            }
            if (have_b == 0 || !hist_b.data) {
                SDL_Delay(2);
                continue;
            }
            const double loop_dt = now - last_loop;
            if (last_loop > 0 && loop_dt > 5.0 && loop_dt < 60.0)
                ref_period += 0.05 * (loop_dt - ref_period);
            last_loop = now;

            /* Show the scene as it was (display lag) = one refresh + one frame ago. The newest frame B is `age` old
             * now (0 to one frame), and the scene at that moment lies between A and B exactly at p = age / frame
             * time, whatever the phase between the frames and the refreshes. Cost: about one frame of delay. */
            double p = (now - t_est) / period;
            p = p < 0 ? 0 : p > 1 ? 1 : p;
            const bool rates_match = (period - ref_period < 1.2 && ref_period - period < 1.2);
            const bool usable = have_b == 2 && hist_a.data && hist_a.w == hist_b.w && hist_a.h == hist_b.h &&
                                now - last_arrival < 3.0 * period;
            if (!usable || rates_match)
                p = 1.0;
            else if (g_smooth_strength < 100) {
                const double nearest = p < 0.5 ? 0.0 : 1.0;
                p = nearest + (p - nearest) * (double)g_smooth_strength / 100.0;
            }
            unsigned w256 = (unsigned)(p * 256.0 + 0.5);
            if (w256 > 256) w256 = 256;

            const frame_t *show = &hist_b;
            frame_t mixf;
            if (w256 < 256 && usable) {
                const size_t need = (size_t)hist_b.w * hist_b.h * 4;
                if (mix_cap < need) {
                    free(mix);
                    mix = malloc(need);
                    mix_cap = mix ? need : 0;
                }
                if (mix) {
                    for (unsigned y = 0; y < hist_b.h; y++)
                        blend_row((uint32_t *)(mix + (size_t)y * hist_b.w * 4),
                                  (const uint32_t *)(hist_a.data + (size_t)y * hist_a.pitch),
                                  (const uint32_t *)(hist_b.data + (size_t)y * hist_b.pitch), hist_b.w, w256);
                    mixf.data = mix; mixf.cap = mix_cap; mixf.w = hist_b.w; mixf.h = hist_b.h; mixf.pitch = (size_t)hist_b.w * 4;
                    show = &mixf;
                }
            }
            debug_scan(show->data, show->w, show->h, show->pitch);
#ifdef CDIHOST_TEST_STICK
            { /* test only: log where a white bar is in what is shown (CDIHOST_TEST_BARLOG=<file>) */
                const char *lp = getenv("CDIHOST_TEST_BARLOG");
                if (lp) {
                    static FILE *lf;
                    if (!lf) lf = fopen(lp, "w");
                    double sum = 0, wsum = 0;
                    const uint32_t *row = (const uint32_t *)(show->data + (size_t)(show->h / 2) * show->pitch);
                    for (unsigned x = 0; x < show->w; x++) { double v = (double)(row[x] & 0xFF); sum += v * x; wsum += v; }
                    if (lf) fprintf(lf, "%.2f %.3f %.3f %u\n", now, wsum > 0 ? sum / wsum : -1.0, p, w256);
                    if (lf) fflush(lf);
                }
            }
#endif
            draw_frame(show->data, show->w, show->h, show->pitch);
#ifndef PS5
            { /* no display to wait for on a desktop test: pretend to be a 60 Hz one */
                const double spent = pc_ms() - now;
                if (spent < 16.6 && !getenv("CDIHOST_TEST_NOPACE"))
                    SDL_Delay((Uint32)(16.6 - spent));
            }
#endif
        }
        vid_present_ms += (double)(SDL_GetPerformanceCounter() - t0) * 1000.0 /
                          (double)SDL_GetPerformanceFrequency();
        vid_presented++;
    }
    free(mix);
    return 0;
}

static bool video_thread_start(void)
{
    vid_mtx = SDL_CreateMutex();
    vid_cond = SDL_CreateCond();
    vid_thread = SDL_CreateThread(render_thread, "render", NULL);
    if (!vid_thread)
        return false;
    SDL_LockMutex(vid_mtx);
    while (!vid_init_done)
        SDL_CondWait(vid_cond, vid_mtx);
    SDL_UnlockMutex(vid_mtx);
    return vid_ok;
}

static void video_thread_stop(void)
{
    if (!vid_thread)
        return;
    SDL_LockMutex(vid_mtx);
    vid_quit = true;
    SDL_CondSignal(vid_cond);
    SDL_UnlockMutex(vid_mtx);
    SDL_WaitThread(vid_thread, NULL);
    /* ready for another start (back to the game list, then another game) */
    vid_thread = NULL;
    vid_quit = false;
    vid_init_done = false;
    vid_ready = false;
    SDL_DestroyCond(vid_cond);
    SDL_DestroyMutex(vid_mtx);
    vid_cond = NULL;
    vid_mtx = NULL;
}
#else
static void video_cb(const void *data, unsigned w, unsigned h, size_t pitch)
{
    if (!data || !w || !h) { /* NULL = duplicate previous frame */
        g_dupes++;
        return;
    }
    g_frames_in++;
    if (g_autofit_req)
        autofit(data, w, h, pitch);
    if (g_crop_mode == CROP_AUTO)
        auto_track(data, w, h, pitch);
    texture_update(data, w, h, pitch);
}
#endif

/* ---------------------------------------------------------------- input */

static unsigned pad_mask;
static int kb_buttons;

/* Test builds can press pad buttons without a pad: CDIHOST_TEST_SDLBTN="<frame>:<names joined by +>" with
 * the names A B X Y L1 R1 L2 R2 (A = Cross, B = Circle, X = Square, Y = Triangle). */
#ifdef CDIHOST_TEST_STICK
static unsigned g_test_sdl;
static int g_test_sdl_frame;
static void test_sdl_update(void)
{
    const char *spec = getenv("CDIHOST_TEST_SDLBTN");
    for (const char *c = spec; c && *c;) {
        int f, n = 0;
        if (sscanf(c, "%d:%n", &f, &n) == 1 && n > 0) {
            c += n;
            unsigned bits = 0;
            while (*c && *c != ' ') {
                if (!strncmp(c, "L1", 2)) { bits |= 1u << 4; c += 2; }
                else if (!strncmp(c, "R1", 2)) { bits |= 1u << 5; c += 2; }
                else if (!strncmp(c, "L2", 2)) { bits |= 1u << 6; c += 2; }
                else if (!strncmp(c, "R2", 2)) { bits |= 1u << 7; c += 2; }
                else if (*c == 'A') { bits |= 1u << 0; c++; }
                else if (*c == 'B') { bits |= 1u << 1; c++; }
                else if (*c == 'X') { bits |= 1u << 2; c++; }
                else if (*c == 'Y') { bits |= 1u << 3; c++; }
                else c++;
            }
            if (g_test_sdl_frame == f) g_test_sdl = bits;
        } else {
            c++;
        }
    }
    g_test_sdl_frame++;
}
#endif

static int mouse_dx, mouse_dy, mouse_buttons;
static int16_t analog[2][2]; /* [left/right][x/y] */
#ifdef PS5
/* The PS5 SDL port exposes the pad as a raw joystick whose button/axis order
 * matches SDL's game-controller enums (A=0 B=1 X=2 Y=3 ... axes 0..5), so we
 * read it directly and do not depend on a controller mapping. */
static SDL_Joystick *pad;
static int pad_present(void) { return pad != NULL; }
static int pad_btn(int b) { return SDL_JoystickGetButton(pad, b); }
static int pad_axis(int a) { return SDL_JoystickGetAxis(pad, a); }

static void pad_close(void)
{
    if (pad) {
        SDL_JoystickClose(pad);
        pad = NULL;
    }
}

static void pad_open(void)
{
    static int said_none;
    int n = SDL_NumJoysticks();

    if (pad)
        return;
    for (int i = 0; i < n; i++) {
        pad = SDL_JoystickOpen(i);
        if (pad) {
            notify("cdihost: pad %d of %d opened: %s (%d buttons, %d axes)", i + 1, n,
                   SDL_JoystickName(pad), SDL_JoystickNumButtons(pad),
                   SDL_JoystickNumAxes(pad));
            return;
        }
        notify("cdihost: pad open failed: %s", SDL_GetError());
    }
    if (!said_none) {
        said_none = 1;
        notify("cdihost: no pad found yet (%d joysticks)", n);
    }
}
#else
static SDL_GameController *pad;
static int pad_present(void) { return pad != NULL; }
static int pad_btn(int b) { return SDL_GameControllerGetButton(pad, (SDL_GameControllerButton)b); }
static int pad_axis(int a) { return SDL_GameControllerGetAxis(pad, (SDL_GameControllerAxis)a); }

static void pad_close(void)
{
    if (pad) {
        SDL_GameControllerClose(pad);
        pad = NULL;
    }
}

static void pad_open(void)
{
    if (pad)
        return;
    for (int i = 0; i < SDL_NumJoysticks(); i++) {
        if (SDL_IsGameController(i)) {
            pad = SDL_GameControllerOpen(i);
            if (pad) {
                fprintf(stderr, "gamepad: %s\n", SDL_GameControllerName(pad));
                return;
            }
        }
    }
}
#endif

#define BIT(id) (1u << (id))

/* Pad access used for the CD-i's buttons: in test builds the pretend presses count too. */
static int pad_btn_t(int b)
{
#ifdef CDIHOST_TEST_STICK
    static const int idx[] = {SDL_CONTROLLER_BUTTON_A, SDL_CONTROLLER_BUTTON_B, SDL_CONTROLLER_BUTTON_X,
                              SDL_CONTROLLER_BUTTON_Y, SDL_CONTROLLER_BUTTON_LEFTSHOULDER,
                              SDL_CONTROLLER_BUTTON_RIGHTSHOULDER};
    for (int i = 0; i < 6; i++)
        if (idx[i] == b && ((g_test_sdl >> i) & 1))
            return 1;
#endif
    return pad_present() && pad_btn(b);
}

static int pad_axis_t(int a)
{
#ifdef CDIHOST_TEST_STICK
    if (a == SDL_CONTROLLER_AXIS_TRIGGERLEFT && ((g_test_sdl >> 6) & 1)) return 32767;
    if (a == SDL_CONTROLLER_AXIS_TRIGGERRIGHT && ((g_test_sdl >> 7) & 1)) return 32767;
#endif
    return pad_present() ? pad_axis(a) : 0;
}

static void input_poll_cb(void)
{
    const Uint8 *k = SDL_GetKeyboardState(NULL);
    unsigned m = 0;

    if (k[SDL_SCANCODE_UP])     m |= BIT(RETRO_DEVICE_ID_JOYPAD_UP);
    if (k[SDL_SCANCODE_DOWN])   m |= BIT(RETRO_DEVICE_ID_JOYPAD_DOWN);
    if (k[SDL_SCANCODE_LEFT])   m |= BIT(RETRO_DEVICE_ID_JOYPAD_LEFT);
    if (k[SDL_SCANCODE_RIGHT])  m |= BIT(RETRO_DEVICE_ID_JOYPAD_RIGHT);
    if (k[SDL_SCANCODE_Z])      m |= BIT(RETRO_DEVICE_ID_JOYPAD_B);
    if (k[SDL_SCANCODE_X])      m |= BIT(RETRO_DEVICE_ID_JOYPAD_A);
    if (k[SDL_SCANCODE_A])      m |= BIT(RETRO_DEVICE_ID_JOYPAD_Y);
    if (k[SDL_SCANCODE_S])      m |= BIT(RETRO_DEVICE_ID_JOYPAD_X);
    if (k[SDL_SCANCODE_Q])      m |= BIT(RETRO_DEVICE_ID_JOYPAD_L);
    if (k[SDL_SCANCODE_W])      m |= BIT(RETRO_DEVICE_ID_JOYPAD_R);
    if (k[SDL_SCANCODE_RETURN]) m |= BIT(RETRO_DEVICE_ID_JOYPAD_START);
    if (k[SDL_SCANCODE_RSHIFT]) m |= BIT(RETRO_DEVICE_ID_JOYPAD_SELECT);

    /* Keyboard-only pointer: arrows move it, Z/X/A = buttons 1/2/3. The console reports
     * the D-pad as arrow keys too. Holding a direction speeds the pointer up, from 8
     * counts per frame to three times that after half a second. */
    {
        static int hold[4];
        const int precision = pad_present() && pad_btn(SDL_CONTROLLER_BUTTON_LEFTSHOULDER) &&
                              !pad_btn(SDL_CONTROLLER_BUTTON_RIGHTSHOULDER) &&
                              pad_axis(SDL_CONTROLLER_AXIS_TRIGGERLEFT) <= 8000;
        const int dirs[4] = {k[SDL_SCANCODE_LEFT], k[SDL_SCANCODE_RIGHT], k[SDL_SCANCODE_UP], k[SDL_SCANCODE_DOWN]};
        int delta[4] = {0, 0, 0, 0};
        for (int i = 0; i < 4; i++) {
            hold[i] = (dirs[i] && !g_pad_adjusting && !g_ptr_mode) ? (hold[i] < 25 ? hold[i] + 1 : 25) : 0;
            if (hold[i])
                delta[i] = (int)(5.0f * (1.0f + 2.0f * (float)hold[i] / 25.0f) * (float)g_ptr_speed / 100.0f * (precision ? PTR_PRECISION : 1.0f) + 0.5f);
        }
        mouse_dx += delta[1] - delta[0];
        mouse_dy += delta[3] - delta[2];
    }
    kb_buttons = (k[SDL_SCANCODE_Z] ? 1 : 0) | (k[SDL_SCANCODE_X] ? 2 : 0) |
                 (k[SDL_SCANCODE_A] ? 4 : 0);

#ifdef CDIHOST_TEST_STICK
    {
        static int bfno;
        static int bmask;
        const char *spec = getenv("CDIHOST_TEST_BUTTONS");
        for (const char *c = spec; c && *c;) {
            int f, b, n = 0;
            if (sscanf(c, "%d:%d%n", &f, &b, &n) == 2 && n > 0) {
                if (bfno == f) bmask = b;
                c += n;
            } else {
                c++;
            }
        }
        kb_buttons |= bmask;
        bfno++;
    }
#endif
#ifdef CDIHOST_TEST_STICK
    test_sdl_update();
#endif
    memset(analog, 0, sizeof analog);
    int cdi_buttons = 0; /* bit 0 = CD-i button 1, bit 1 = button 2, bit 2 = button 3 */
#ifdef CDIHOST_TEST_STICK
    if (1) {
#else
    if (pad_present()) {
#endif
        /* SDL names face buttons by position (A=south); libretro names them
         * after the SNES layout (B=south), hence the crossover. */
        struct { SDL_GameControllerButton sdl; int id; } map[] = {
            /* the face buttons are handled below: they are the CD-i's buttons (g_btn_map) */
            {SDL_CONTROLLER_BUTTON_START, RETRO_DEVICE_ID_JOYPAD_START},
            {SDL_CONTROLLER_BUTTON_LEFTSHOULDER, RETRO_DEVICE_ID_JOYPAD_L},
            {SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, RETRO_DEVICE_ID_JOYPAD_R},
            {SDL_CONTROLLER_BUTTON_DPAD_UP, RETRO_DEVICE_ID_JOYPAD_UP},
            {SDL_CONTROLLER_BUTTON_DPAD_DOWN, RETRO_DEVICE_ID_JOYPAD_DOWN},
            {SDL_CONTROLLER_BUTTON_DPAD_LEFT, RETRO_DEVICE_ID_JOYPAD_LEFT},
            {SDL_CONTROLLER_BUTTON_DPAD_RIGHT, RETRO_DEVICE_ID_JOYPAD_RIGHT},
        };
        for (size_t i = 0; i < sizeof map / sizeof map[0]; i++)
            if (pad_btn(map[i].sdl))
                m |= BIT(map[i].id);

        /* the pad's buttons as CD-i buttons (see g_btn_map) */
        {
            static const SDL_GameControllerButton face[6] = {
                SDL_CONTROLLER_BUTTON_A, SDL_CONTROLLER_BUTTON_B, SDL_CONTROLLER_BUTTON_X,
                SDL_CONTROLLER_BUTTON_Y, SDL_CONTROLLER_BUTTON_LEFTSHOULDER, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER};
            for (int i = 0; i < 6; i++)
                if (pad_btn_t(face[i]))
                    cdi_buttons |= g_btn_map[i];
            if (pad_axis_t(SDL_CONTROLLER_AXIS_TRIGGERLEFT) > 8000)
                cdi_buttons |= g_btn_map[6];
            if (pad_axis_t(SDL_CONTROLLER_AXIS_TRIGGERRIGHT) > 8000)
                cdi_buttons |= g_btn_map[7];
        }

        analog[0][0] = pad_axis(SDL_CONTROLLER_AXIS_LEFTX);
        analog[0][1] = pad_axis(SDL_CONTROLLER_AXIS_LEFTY);
        analog[1][0] = pad_axis(SDL_CONTROLLER_AXIS_RIGHTX);
        analog[1][1] = pad_axis(SDL_CONTROLLER_AXIS_RIGHTY);
        g_dbg.ax[0] = analog[0][0]; g_dbg.ax[1] = analog[0][1]; g_dbg.ax[2] = analog[1][0]; g_dbg.ax[3] = analog[1][1];
        g_dbg.ax[4] = pad_axis(SDL_CONTROLLER_AXIS_TRIGGERLEFT); g_dbg.ax[5] = pad_axis(SDL_CONTROLLER_AXIS_TRIGGERRIGHT);

#ifdef CDIHOST_TEST_STICK
        { /* test only: CDIHOST_TEST_STICKS="<frame>:<x>,<y> ..." sets the left stick from that frame on */
            static int frame_no;
            static float tx, ty;
            const char *spec = getenv("CDIHOST_TEST_STICKS");
            for (const char *c = spec; c && *c;) {
                int f, n = 0;
                float x, y;
                if (sscanf(c, "%d:%f,%f%n", &f, &x, &y, &n) == 3 && n > 0) {
                    if (frame_no == f) { tx = x; ty = y; }
                    c += n;
                } else {
                    c++;
                }
            }
            analog[0][0] = (int16_t)(tx * 32767.0f);
            analog[0][1] = (int16_t)(ty * 32767.0f);
            analog[1][0] = analog[1][1] = 0;
            frame_no++;
        }
#endif
        /* Either stick drives the pointer (CD-i pointing device). Beyond a dead zone the
         * speed follows a curve: gentle near the centre for aiming, fast at full tilt.
         * The fractions are carried over, so slow movements are smooth, not stepped. */
        if (!g_ptr_mode) {
            static float acc[2];
            float sp[2] = {0, 0};
            /* hold L1 (alone) to slow down for fine aiming */
            const float precision_k = (pad_btn(SDL_CONTROLLER_BUTTON_LEFTSHOULDER) &&
                                       !pad_btn(SDL_CONTROLLER_BUTTON_RIGHTSHOULDER) &&
                                       pad_axis(SDL_CONTROLLER_AXIS_TRIGGERLEFT) <= 8000) ? PTR_PRECISION : 1.0f;
            for (int st = 0; st < 2; st++)
                for (int a = 0; a < 2; a++) {
                    float v = (float)analog[st][a] / 32767.0f;
                    float m = v < 0 ? -v : v;
                    if (m <= 0.08f)
                        continue;
                    m = (m - 0.08f) / 0.92f;
                    if (m > 1.0f)
                        m = 1.0f;
                    float c = 0.35f * m + 0.65f * m * m;
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
                    float m = v < 0 ? -v : v;
                    if (m <= 0.10f)
                        continue;
                    m = (m - 0.10f) / 0.90f;
                    u[a] += v < 0 ? -m : m;
                }
            const float gain = (float)g_ptr_speed / 100.0f;
            for (int a = 0; a < 2; a++) {
                u[a] *= gain;
                u[a] = u[a] < -1.0f ? -1.0f : u[a] > 1.0f ? 1.0f : u[a];
            }
            if (!est_valid) { /* find out where the pointer is: push it into the corner */
                mouse_dx -= PTR_DEV_W + 300;
                mouse_dy -= g_ptr_yrange + 300;
                est_valid = 1;
            } else {
                int tx = (int)((u[0] + 1.0f) * 0.5f * (PTR_DEV_W - 1) + 0.5f);
                int ty = (int)((u[1] + 1.0f) * 0.5f * (g_ptr_yrange - 1) + 0.5f);
                int dx = tx - est_x, dy = ty - est_y;
                if (tx <= 1) dx -= 48;                 /* overshoot at the edges: re-synchronises */
                if (tx >= PTR_DEV_W - 2) dx += 48;
                if (ty <= 1) dy -= 48;
                if (ty >= g_ptr_yrange - 2) dy += 48;
                mouse_dx += dx;
                mouse_dy += dy;
            }
        }
    }
#ifdef CDIHOST_TEST_STICK
    {
        static int fno;
        static unsigned bits;
        const char *spec = getenv("CDIHOST_TEST_DPAD");
        for (const char *c = spec; c && *c;) {
            int f, n = 0;
            unsigned b;
            if (sscanf(c, "%d:%u%n", &f, &b, &n) == 2 && n > 0) {
                if (fno == f) bits = b;
                c += n;
            } else {
                c++;
            }
        }
        m = (m & ~(BIT(RETRO_DEVICE_ID_JOYPAD_UP) | BIT(RETRO_DEVICE_ID_JOYPAD_DOWN) | BIT(RETRO_DEVICE_ID_JOYPAD_LEFT) | BIT(RETRO_DEVICE_ID_JOYPAD_RIGHT))) | bits;
        { /* CDIHOST_TEST_PAD="<frame>:<RetroPad bit mask>": face buttons etc. (B=1 Y=2 SELECT=4 START=8 A=256 X=512 L=1024 R=2048) */
            static int pfno;
            static unsigned pbits;
            const char *pspec = getenv("CDIHOST_TEST_PAD");
            for (const char *c = pspec; c && *c;) {
                int f, n = 0;
                unsigned b;
                if (sscanf(c, "%d:%u%n", &f, &b, &n) == 2 && n > 0) {
                    if (pfno == f) pbits = b;
                    c += n;
                } else {
                    c++;
                }
            }
            m |= pbits;
            pfno++;
        }
        fno++;
    }
#endif
    /* The core turns the D-pad into a fixed 2-unit step per frame on the CD-i's pointer. The host
     * moves the pointer itself (faster, with its own speed setting, and the absolute mode needs
     * to know every move), so by default the D-pad is not handed over as buttons. */
    if (!g_dpad_digital)
        m &= ~(BIT(RETRO_DEVICE_ID_JOYPAD_UP) | BIT(RETRO_DEVICE_ID_JOYPAD_DOWN) |
               BIT(RETRO_DEVICE_ID_JOYPAD_LEFT) | BIT(RETRO_DEVICE_ID_JOYPAD_RIGHT));
    /* The CD-i's buttons from every source (pad, keys, mouse), as the RetroPad buttons this core reads them
     * from: Y = button 1, B = button 2, A = button 3. */
    {
        int all = cdi_buttons | kb_buttons | mouse_buttons;
        /* the touchpad opens the menu (it is not given to the game) */
        {
            static int prev_touch = 1;
            const int touch = pad_present() && pad_btn(SDL_CONTROLLER_BUTTON_BACK);
            if (touch && !prev_touch)
                g_menu_req = 1;
            prev_touch = touch;
        }
        /* just after the menu closed: the button that closed it is still down; give the game nothing until the pad is
         * let go, or that press would reach the game */
        if (g_input_hold) {
            if (all || (m & ~(BIT(RETRO_DEVICE_ID_JOYPAD_UP) | BIT(RETRO_DEVICE_ID_JOYPAD_DOWN) |
                              BIT(RETRO_DEVICE_ID_JOYPAD_LEFT) | BIT(RETRO_DEVICE_ID_JOYPAD_RIGHT))))
                all = 0, m = 0;
            else
                g_input_hold = 0;
        }
        if (all & 1) m |= BIT(RETRO_DEVICE_ID_JOYPAD_Y);
        if (all & 2) m |= BIT(RETRO_DEVICE_ID_JOYPAD_B);
        if (all & 4) m |= BIT(RETRO_DEVICE_ID_JOYPAD_A);
        g_dbg.btn = all & 7;
    }
    pad_mask = m;
    g_dbg.mask = m;
    g_dbg.arrows[0] = k[SDL_SCANCODE_LEFT]; g_dbg.arrows[1] = k[SDL_SCANCODE_RIGHT];
    g_dbg.arrows[2] = k[SDL_SCANCODE_UP]; g_dbg.arrows[3] = k[SDL_SCANCODE_DOWN];
    g_dbg.mode = g_ptr_mode; g_dbg.speed = g_ptr_speed; g_dbg.yrange = g_ptr_yrange;

    {   /* pad shortcuts:
         *   R3                 4:3 <-> 16:9     (L2 + R3 = reset the whole view)
         *   L3                 crop: manual -> auto -> off
         *   L2 + L3            fit the manual crop to the picture on screen
         *   L2 + D-pad         manual crop size  (left/right = sides, up/down = top and bottom)
         *   L1 + L2 + D-pad    move the cropped window
         *   L1 + R1 + Square   save NVRAM now
         *   L1 + R1 + Options  quit */
        static int prev_l3, prev_r3, prev_d[4];
        int have = pad_present();
        int l3 = have && pad_btn(SDL_CONTROLLER_BUTTON_LEFTSTICK);
        int r3 = have && pad_btn(SDL_CONTROLLER_BUTTON_RIGHTSTICK);
        int l1 = have && pad_btn(SDL_CONTROLLER_BUTTON_LEFTSHOULDER);
        int r1 = have && pad_btn(SDL_CONTROLLER_BUTTON_RIGHTSHOULDER);
        int l2 = have && pad_axis(SDL_CONTROLLER_AXIS_TRIGGERLEFT) > 8000;
        int d[4] = {
            have && pad_btn(SDL_CONTROLLER_BUTTON_DPAD_LEFT),
            have && pad_btn(SDL_CONTROLLER_BUTTON_DPAD_RIGHT),
            have && pad_btn(SDL_CONTROLLER_BUTTON_DPAD_UP),
            have && pad_btn(SDL_CONTROLLER_BUTTON_DPAD_DOWN),
        };
        const int step = 5; /* half a percent */
        int changed = 0;

        if (r3 && !prev_r3 && !(l2 && r1)) {
            if (l2) {
                g_view_mode = 0;
                g_crop_mode = CROP_MANUAL;
                for (int m = 0; m < 2; m++)
                    for (int i = 0; i < 4; i++)
                        g_crop[m][i] = CROP_DEF[m][i];
                notify("cdihost: view reset to defaults (4:3, manual crop)");
            } else {
                g_view_mode = !g_view_mode;
                notify("cdihost: %s", view_name());
            }
            changed = 1;
        }
        if (l3 && !prev_l3 && !(l2 && r1)) {
            if (l2) {
                g_autofit_req = 1; /* handled on the next frame */
            } else {
                static const char *const names[3] = {"off (whole frame)", "manual",
                                                     "auto (follows the picture)"};
                int next = g_crop_mode == CROP_MANUAL ? CROP_AUTO
                         : g_crop_mode == CROP_AUTO ? CROP_OFF : CROP_MANUAL;
                if (next == CROP_AUTO) /* start from the manual crop: no jump */
                    for (int i = 0; i < 4; i++)
                        g_auto[i] = g_crop[g_view_mode ? 1 : 0][i];
                g_crop_mode = next;
                notify("cdihost: crop %s", names[next]);
                changed = 1;
            }
        }
        g_pad_adjusting = l2 && r1;
        {
            static int prev_l3s;
            if (l2 && r1 && l3 && !prev_l3s) { /* L2 + R1 + L3: video smoothing on / off */
                g_smooth = !g_smooth;
                ptr_cfg_save();
                notify("cdihost: video smoothing %s", g_smooth ? "ON (blends frames; smoother motion, about one frame more delay)" : "OFF");
            }
            prev_l3s = l3;
        }
        {
            static int prev_r3b;
            int r3b = have && pad_btn(SDL_CONTROLLER_BUTTON_RIGHTSTICK);
            if (l2 && r1 && r3b && !prev_r3b && g_ptr_speed != 100) {
                g_ptr_speed = 100;
                ptr_cfg_save();
                notify("cdihost: pointer speed back to 100%%");
            }
            prev_r3b = r3b;
        }
        {
            static int prev_sq2;
            int sq2 = have && pad_btn(SDL_CONTROLLER_BUTTON_X);
            if (l2 && r1 && sq2 && !prev_sq2) {
                g_dbg_overlay = !g_dbg_overlay;
                g_dbg.tot_x = g_dbg.tot_y = 0;
                notify("cdihost: input readout %s", g_dbg_overlay ? "on" : "off");
            }
            prev_sq2 = sq2;
        }
        {
            static int prev_tri;
            int tri = have && pad_btn(SDL_CONTROLLER_BUTTON_Y);
            if (l2 && r1 && tri && !prev_tri) {
                g_ptr_mode = !g_ptr_mode;
                est_valid = 0;
                ptr_cfg_save();
                notify("cdihost: pointer %s", g_ptr_mode ? "ABSOLUTE (stick position = crosshair position)"
                                                         : "RELATIVE (the stick moves the crosshair)");
            }
            prev_tri = tri;
        }
        if (l2 && r1 && !l1) { /* L2 + R1 + D-pad up/down: pointer faster / slower */
            int sp = g_ptr_speed;
            if (d[2] && !prev_d[2]) sp += 10;
            if (d[3] && !prev_d[3]) sp -= 10;
            sp = sp < 30 ? 30 : sp > 800 ? 800 : sp;
            if (sp != g_ptr_speed) {
                g_ptr_speed = sp;
                ptr_cfg_save();
                notify("cdihost: pointer speed %d%% (up = faster, down = slower)", sp);
            }
        }
        if (l2 && r1 && !l1) { /* L2 + R1 + D-pad left/right: sound earlier / later */
            int ms = g_audio_ms;
            if (d[0] && !prev_d[0]) ms -= 5;
            if (d[1] && !prev_d[1]) ms += 5;
            ms = ms < 20 ? 20 : ms > 150 ? 150 : ms; /* below a PAL frame (20 ms) the sound runs dry */
            if (ms != g_audio_ms) {
                g_audio_ms = ms;
                audio_cfg_save();
                notify("cdihost: audio latency %d ms (left = sound earlier, right = later)", ms);
            }
        }
        if (l2 && !l3 && !r1) {
            int *cur = (int *)g_crop[g_view_mode ? 1 : 0];
            int L = cur[0], R = cur[1], T = cur[2], B = cur[3];
            int dl = d[0] && !prev_d[0], dr = d[1] && !prev_d[1];
            int du = d[2] && !prev_d[2], dd = d[3] && !prev_d[3];
            if (l1) { /* move: shrink one side, grow the other */
                if (dl) { L -= step; R += step; }
                if (dr) { L += step; R -= step; }
                if (du) { T -= step; B += step; }
                if (dd) { T += step; B -= step; }
            } else {  /* size: both sides together */
                if (dl) { L -= step; R -= step; }
                if (dr) { L += step; R += step; }
                if (du) { T -= step; B -= step; }
                if (dd) { T += step; B += step; }
            }
            L = L < 0 ? 0 : L > 300 ? 300 : L;
            R = R < 0 ? 0 : R > 300 ? 300 : R;
            T = T < 0 ? 0 : T > 300 ? 300 : T;
            B = B < 0 ? 0 : B > 300 ? 300 : B;
            if (L != cur[0] || R != cur[1] || T != cur[2] || B != cur[3]) {
                cur[0] = L; cur[1] = R; cur[2] = T; cur[3] = B;
                g_crop_mode = CROP_MANUAL;
                notify("cdihost: %s crop left %.1f%% right %.1f%% top %.1f%% bottom %.1f%%",
                       view_name(), L / 10.0, R / 10.0, T / 10.0, B / 10.0);
                changed = 1;
            }
        }
        if (changed)
            view_save();
        prev_l3 = l3;
        prev_r3 = r3;
        for (int i = 0; i < 4; i++)
            prev_d[i] = d[i];
        {
            static int prev_save;
            int save = have && l1 && pad_btn(SDL_CONTROLLER_BUTTON_RIGHTSHOULDER) &&
                       pad_btn(SDL_CONTROLLER_BUTTON_X);
            if (save && !prev_save)
                g_nvram_req = 1; /* done in the main loop, not inside the frame */
            prev_save = save;
        }
        if (have && l1 && pad_btn(SDL_CONTROLLER_BUTTON_RIGHTSHOULDER) &&
            pad_btn(SDL_CONTROLLER_BUTTON_START))
            g_running = false;
    }
    {
        static int announced;
        if (!announced && (m || kb_buttons)) {
            announced = 1;
            notify("cdihost: first input seen (mask %x, clicks %x)", m, kb_buttons);
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
            g_dbg.cx = v; g_dbg.tot_x += v; g_dbg.est_x = est_x;
            return (int16_t)v;
        case RETRO_DEVICE_ID_MOUSE_Y:
            v = mouse_dy;
            mouse_dy = 0;
            est_y = est_y + v < 0 ? 0 : est_y + v > g_ptr_yrange - 1 ? g_ptr_yrange - 1 : est_y + v;
            g_dbg.cy = v; g_dbg.tot_y += v; g_dbg.est_y = est_y;
            return (int16_t)v;
        case RETRO_DEVICE_ID_MOUSE_LEFT:   return (mouse_buttons | kb_buttons) & 1;
        case RETRO_DEVICE_ID_MOUSE_RIGHT:  return ((mouse_buttons | kb_buttons) >> 1) & 1;
        case RETRO_DEVICE_ID_MOUSE_MIDDLE: return ((mouse_buttons | kb_buttons) >> 2) & 1;
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
                fprintf(stderr, "[host] pixel format %d\n", (int)f);
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
        fprintf(stderr, "[core msg] %s\n", m->msg);
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

static void on_signal(int sig)
{
    g_signal = sig;
}

static bool nvram_flush(void)
{
    if (!core.retro_flush_nvram)
        return false;
    core.retro_flush_nvram();
    return true;
}

/* ----------------------------------------------------------- save state */

static void state_path(char *out, size_t n)
{
    snprintf(out, n, "%s/quick.state", g_save_dir);
}

static void state_save(void)
{
    size_t sz = core.retro_serialize_size();
    if (!sz) {
        fprintf(stderr, "core does not support save states\n");
        return;
    }
    void *buf = malloc(sz);
    char p[1100];
    state_path(p, sizeof p);
    if (buf && core.retro_serialize(buf, sz)) {
        FILE *f = fopen(p, "wb");
        if (f) {
            fwrite(buf, 1, sz, f);
            fclose(f);
            fprintf(stderr, "state saved (%zu bytes)\n", sz);
        }
    } else {
        fprintf(stderr, "serialize failed\n");
    }
    free(buf);
}

static void state_load(void)
{
    char p[1100];
    state_path(p, sizeof p);
    FILE *f = fopen(p, "rb");
    if (!f) {
        fprintf(stderr, "no saved state\n");
        return;
    }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    void *buf = malloc((size_t)sz);
    if (buf && fread(buf, 1, (size_t)sz, f) == (size_t)sz) {
        if (core.retro_unserialize(buf, (size_t)sz))
            fprintf(stderr, "state loaded\n");
        else
            fprintf(stderr, "unserialize failed\n");
    }
    free(buf);
    fclose(f);
}

/* ------------------------------------------------- in-game menu: actions */

enum { MA_NONE, MA_RESUME, MA_SAVE, MA_LOAD, MA_DISC, MA_RESET, MA_LIST, MA_QUIT };

static int g_slot = 1;            /* save state slot shown in the menu, 1 to 4 */
static int g_mprev[3];            /* Cross, Circle, touchpad: down at the previous menu step */
static Uint8 g_mprev_keys[6];
static int g_back_to_list;        /* the menu asked to leave the game for the game list */
static int g_disc_pick;           /* disc chosen in the menu (0-based) */
static bool g_menu_has_list;      /* the game was started from the game list (so "Back to game list" makes sense) */
static char g_game_name_for_states[200];

static void slot_path(int slot, const char *ext, char *out, size_t n)
{
    snprintf(out, n, "%s/states/%.180s.slot%d.%s", g_save_dir, g_game_name_for_states, slot, ext);
}

static void slot_info(int slot, char *out, size_t n)
{
    char p[1400];
    struct stat st;
    slot_path(slot, "state", p, sizeof p);
    if (stat(p, &st) != 0) {
        snprintf(out, n, "slot %d: empty", slot);
        return;
    }
    char when[32];
    struct tm tmv;
    time_t t = st.st_mtime;
    localtime_r(&t, &tmv);
    strftime(when, sizeof when, "%d %b %H:%M", &tmv);
    snprintf(out, n, "slot %d: %s", slot, when);
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
    fprintf(stderr, "[host] state saved to slot %d (%zu bytes)\n", slot, sz);
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
    fprintf(stderr, "[host] disc %u inserted\n", idx + 1);
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
    fprintf(stderr, "[host] %s\n", msg);
    return ok;
}

/* Fills the menu's lines from the current settings. */
static void menu_build(void)
{
    SDL_LockMutex(g_menu_mtx);
    int n = 0;
    menu_item_t *it = g_menu.it;
#define ITEM(k, l, ...) do { it[n].kind = (k); snprintf(it[n].label, sizeof it[n].label, "%s", (l)); snprintf(it[n].value, sizeof it[n].value, __VA_ARGS__); n++; } while (0)
    char info[50];
    ITEM(MI_RESUME, "Resume", "%s", "");
    slot_info(g_slot, info, sizeof info);
    ITEM(MI_SAVE, "Save state", "%s", info);
    ITEM(MI_LOAD, "Load state", "%s", info);
    const unsigned nd = g_have_disk && g_disk.get_num_images ? g_disk.get_num_images() : 0;
    if (nd > 1)
        ITEM(MI_DISC, "Disc", "insert %d of %u%s", g_disc_pick + 1, nd,
             (unsigned)g_disc_pick == g_disk.get_image_index() ? " (in)" : "");
    ITEM(MI_PTRMODE, "Pointer", "%s", g_ptr_mode ? "absolute (aim)" : "relative");
    ITEM(MI_PTRSPEED, "Pointer speed", "%d%%", g_ptr_speed);
    ITEM(MI_SMOOTH, "Smooth video", "%s", g_smooth ? "on" : "off");
    ITEM(MI_LATENCY, "Sound delay", "%d ms", g_audio_ms);
    ITEM(MI_VIEW, "Screen", "%s", g_view_mode ? "16:9 (fill)" : "4:3");
    ITEM(MI_RESET, "Reset game", "%s", "");
    if (g_menu_has_list)
        ITEM(MI_LIST, "Back to the game list", "%s", "");
    ITEM(MI_QUIT, "Quit", "%s", "");
#undef ITEM
    g_menu.n = n;
    if (g_menu.sel >= n)
        g_menu.sel = n - 1;
    SDL_UnlockMutex(g_menu_mtx);
    g_vid_redraw = 1;
    if (vid_cond) {
        SDL_LockMutex(vid_mtx);
        SDL_CondSignal(vid_cond);
        SDL_UnlockMutex(vid_mtx);
    }
}

static void menu_message(const char *msg)
{
    SDL_LockMutex(g_menu_mtx);
    snprintf(g_menu.msg, sizeof g_menu.msg, "%s", msg);
    SDL_UnlockMutex(g_menu_mtx);
}

static void menu_open(void)
{
    if (!g_menu_mtx)
        g_menu_mtx = SDL_CreateMutex();
    SDL_LockMutex(g_menu_mtx);
    snprintf(g_menu.title, sizeof g_menu.title, "Paused - %.80s", g_game_name);
    g_menu.msg[0] = 0;
    g_menu.sel = 0;
    SDL_UnlockMutex(g_menu_mtx);
    if (g_have_disk && g_disk.get_image_index)
        g_disc_pick = (int)g_disk.get_image_index();
    /* whatever is held right now (the touchpad that opened the menu) does not count as a new press */
    g_mprev[0] = g_mprev[1] = g_mprev[2] = 1;
    memset(g_mprev_keys, 1, sizeof g_mprev_keys);
    g_menu.open = 1;
    menu_build();
}

static void menu_close(void)
{
    g_menu.open = 0;
    g_input_hold = 1;
    g_vid_redraw = 1;
    g_vid_reset = 1; /* repaint the whole screen: the menu box is gone */
}

/* One pass of menu input. Returns an MA_* action for the main loop (which owns the core and the clock). */
static int menu_step(void)
{
    static Uint32 next_repeat;
    static int held = -1; /* the direction being held, for auto-repeat */
    int up = 0, down = 0, left = 0, right = 0, cross = 0, back = 0;
#ifdef CDIHOST_TEST_STICK
    { /* test only: CDIHOST_TEST_MENUKEYS: one key per step: u d l r x (Cross) o (Circle) */
        static const char *k;
        static Uint32 nexttime;
        if (!k) k = getenv("CDIHOST_TEST_MENUKEYS");
        if (k && *k && SDL_GetTicks() >= nexttime) {
            switch (*k++) {
            case 'u': up = 1; break; case 'd': down = 1; break; case 'l': left = 1; break;
            case 'r': right = 1; break; case 'x': cross = 1; break; case 'o': back = 1; break;
            }
            nexttime = SDL_GetTicks() + 60;
        }
    }
#endif
    if (pad_present()) {
        const int dir[4] = {pad_btn(SDL_CONTROLLER_BUTTON_DPAD_UP) || pad_axis(SDL_CONTROLLER_AXIS_LEFTY) < -20000,
                            pad_btn(SDL_CONTROLLER_BUTTON_DPAD_DOWN) || pad_axis(SDL_CONTROLLER_AXIS_LEFTY) > 20000,
                            pad_btn(SDL_CONTROLLER_BUTTON_DPAD_LEFT) || pad_axis(SDL_CONTROLLER_AXIS_LEFTX) < -20000,
                            pad_btn(SDL_CONTROLLER_BUTTON_DPAD_RIGHT) || pad_axis(SDL_CONTROLLER_AXIS_LEFTX) > 20000};
        int now_dir = -1;
        for (int i = 0; i < 4; i++)
            if (dir[i])
                now_dir = i;
        const Uint32 t = SDL_GetTicks();
        if (now_dir != held) {
            held = now_dir;
            next_repeat = t + 350;
            if (now_dir >= 0) {
                up |= now_dir == 0; down |= now_dir == 1; left |= now_dir == 2; right |= now_dir == 3;
            }
        } else if (now_dir >= 0 && t >= next_repeat) {
            next_repeat = t + 110;
            up |= now_dir == 0; down |= now_dir == 1; left |= now_dir == 2; right |= now_dir == 3;
        }
        const int c = pad_btn(SDL_CONTROLLER_BUTTON_A), ci = pad_btn(SDL_CONTROLLER_BUTTON_B),
                  tp = pad_btn(SDL_CONTROLLER_BUTTON_BACK);
        if (c && !g_mprev[0]) cross = 1;
        if ((ci && !g_mprev[1]) || (tp && !g_mprev[2])) back = 1;
        g_mprev[0] = c; g_mprev[1] = ci; g_mprev[2] = tp;
    }
#ifndef PS5
    {
        const Uint8 *k = SDL_GetKeyboardState(NULL);
        Uint8 *pk = g_mprev_keys;
        const Uint8 now[6] = {k[SDL_SCANCODE_UP], k[SDL_SCANCODE_DOWN], k[SDL_SCANCODE_LEFT], k[SDL_SCANCODE_RIGHT],
                              (Uint8)(k[SDL_SCANCODE_RETURN] || k[SDL_SCANCODE_Z]), (Uint8)(k[SDL_SCANCODE_ESCAPE] || k[SDL_SCANCODE_TAB])};
        if (now[0] && !pk[0]) up = 1;
        if (now[1] && !pk[1]) down = 1;
        if (now[2] && !pk[2]) left = 1;
        if (now[3] && !pk[3]) right = 1;
        if (now[4] && !pk[4]) cross = 1;
        if (now[5] && !pk[5]) back = 1;
        memcpy(pk, now, 6);
    }
#endif
    if (!(up || down || left || right || cross || back))
        return MA_NONE;
    if (back)
        return MA_RESUME;

    SDL_LockMutex(g_menu_mtx);
    if (up) g_menu.sel = g_menu.sel > 0 ? g_menu.sel - 1 : g_menu.n - 1;
    if (down) g_menu.sel = g_menu.sel < g_menu.n - 1 ? g_menu.sel + 1 : 0;
    const int kind = g_menu.it[g_menu.sel].kind;
    SDL_UnlockMutex(g_menu_mtx);

    const int d = right - left;
    int action = MA_NONE;
    if (d) {
        switch (kind) {
        case MI_SAVE: case MI_LOAD: g_slot = (g_slot - 1 + d + 4) % 4 + 1; break;
        case MI_DISC: {
            const int nd = (int)g_disk.get_num_images();
            g_disc_pick = (g_disc_pick + d + nd) % nd;
            break;
        }
        case MI_PTRMODE: g_ptr_mode = !g_ptr_mode; est_valid = 0; ptr_cfg_save(); break;
        case MI_PTRSPEED: g_ptr_speed += 10 * d; g_ptr_speed = g_ptr_speed < 30 ? 30 : g_ptr_speed > 800 ? 800 : g_ptr_speed; ptr_cfg_save(); break;
        case MI_SMOOTH: g_smooth = !g_smooth; ptr_cfg_save(); break;
        case MI_LATENCY: g_audio_ms += 5 * d; g_audio_ms = g_audio_ms < 20 ? 20 : g_audio_ms > 150 ? 150 : g_audio_ms; audio_cfg_save(); break;
        case MI_VIEW: g_view_mode = !g_view_mode; g_vid_reset = 1; view_save(); break;
        }
    }
    if (cross) {
        switch (kind) {
        case MI_RESUME: action = MA_RESUME; break;
        case MI_SAVE: action = MA_SAVE; break;
        case MI_LOAD: action = MA_LOAD; break;
        case MI_DISC: action = MA_DISC; break;
        case MI_RESET: action = MA_RESET; break;
        case MI_LIST: action = MA_LIST; break;
        case MI_QUIT: action = MA_QUIT; break;
        case MI_PTRMODE: case MI_SMOOTH: case MI_VIEW:
            /* Cross on an on/off line flips it too */
            if (kind == MI_PTRMODE) { g_ptr_mode = !g_ptr_mode; est_valid = 0; ptr_cfg_save(); }
            else if (kind == MI_SMOOTH) { g_smooth = !g_smooth; ptr_cfg_save(); }
            else { g_view_mode = !g_view_mode; g_vid_reset = 1; view_save(); }
            break;
        }
    }
    menu_build();
    return action;
}

/* ----------------------------------------------------------------- main */

/* ------------------------------------------------------------ game library */

#ifdef PS5
#define DEFAULT_ROMS_DIR "/data/ps5-cdi/roms"
#define DEFAULT_COVERS_DIR "/data/ps5-cdi/covers"
#define DEFAULT_CACHE_DIR "/data/ps5-cdi/cache"
#else
#define DEFAULT_ROMS_DIR "roms"
#define DEFAULT_COVERS_DIR "covers"
#define DEFAULT_CACHE_DIR "cache"
#endif

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
static SDL_mutex *g_nas_mtx;

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

/* --- drawing (straight into the window surface, 8x16 font scaled up) --- */

static int sh_r, sh_g, sh_b, sh_a; /* pixel layout of the window surface */

static Uint32 rgb(int r, int g, int b)
{
    return ((Uint32)r << sh_r) | ((Uint32)g << sh_g) | ((Uint32)b << sh_b) | (0xFFu << sh_a);
}

static void fill_rect(SDL_Surface *s, int x, int y, int w, int h, Uint32 c)
{
    SDL_Rect r = {x, y, w, h};
    SDL_FillRect(s, &r, c);
}

static void frame_rect(SDL_Surface *s, int x, int y, int w, int h, int t, Uint32 c)
{
    fill_rect(s, x - t, y - t, w + 2 * t, t, c);
    fill_rect(s, x - t, y + h, w + 2 * t, t, c);
    fill_rect(s, x - t, y, t, h, c);
    fill_rect(s, x + w, y, t, h, c);
}

static void draw_text(SDL_Surface *s, int x, int y, int sc, Uint32 col, const char *t, int max_chars)
{
    if (y < 0 || y + 16 * sc > s->h)
        return;
    for (int n = 0; *t && n < max_chars; t++, n++, x += 8 * sc) {
        if (x < 0 || x + 8 * sc > s->w)
            break;
        unsigned c = (unsigned char)*t;
        const unsigned char *g = font8x16[(c < 0x20 || c > 0x7E) ? '?' - 0x20 : c - 0x20];
        for (int gy = 0; gy < 16; gy++)
            for (int gx = 0; gx < 8; gx++)
                if (g[gy] & (0x80 >> gx))
                    for (int dy = 0; dy < sc; dy++) {
                        Uint32 *row = (Uint32 *)((Uint8 *)s->pixels +
                                                 (size_t)(y + gy * sc + dy) * s->pitch);
                        for (int dx = 0; dx < sc; dx++)
                            row[x + gx * sc + dx] = col;
                    }
    }
}

/* Text cut to max_chars with "..." when it does not fit. */
static void draw_text_fit(SDL_Surface *s, int x, int y, int sc, Uint32 col, const char *t, int max_chars)
{
    char buf[256];
    size_t n = strlen(t);
    if ((int)n <= max_chars || max_chars < 4) {
        draw_text(s, x, y, sc, col, t, max_chars);
        return;
    }
    snprintf(buf, sizeof buf, "%.*s...", max_chars - 3, t);
    draw_text(s, x, y, sc, col, buf, max_chars);
}

/* --- cover thumbnails: decoded on a worker thread, kept in an LRU-ish cache --- */

typedef struct {
    int state; /* 0 not loaded, 1 loading, 2 ready, 3 no usable cover */
    uint32_t *px;
    int w, h;
} thumb_t;

static thumb_t *g_th;
static SDL_mutex *th_mtx;
static SDL_cond *th_cond;
static SDL_Thread *th_thread;
static int th_lo, th_hi, th_center, th_max = 360;
static bool th_quit;
static unsigned th_epoch; /* bumped whenever a thumbnail finishes */
#define THUMB_CACHE 72

/* Area-average a w x h RGBA image down to dw x dh (never up), compositing any
 * transparency over the menu background, into the surface's pixel layout. */
static uint32_t *make_thumb(const unsigned char *src, int w, int h, int dw, int dh)
{
    uint32_t *out = malloc((size_t)dw * dh * sizeof *out);
    if (!out)
        return NULL;
    for (int y = 0; y < dh; y++) {
        int y0 = (int)((long)y * h / dh), y1 = (int)((long)(y + 1) * h / dh);
        if (y1 <= y0) y1 = y0 + 1;
        for (int x = 0; x < dw; x++) {
            int x0 = (int)((long)x * w / dw), x1 = (int)((long)(x + 1) * w / dw);
            if (x1 <= x0) x1 = x0 + 1;
            unsigned long r = 0, g = 0, b = 0;
            int count = 0;
            for (int yy = y0; yy < y1; yy++) {
                const unsigned char *p = src + ((size_t)yy * w + x0) * 4;
                for (int xx = x0; xx < x1; xx++, p += 4, count++) {
                    int a = p[3];
                    r += (unsigned)(p[0] * a + 20 * (255 - a)) / 255;
                    g += (unsigned)(p[1] * a + 20 * (255 - a)) / 255;
                    b += (unsigned)(p[2] * a + 28 * (255 - a)) / 255;
                }
            }
            out[(size_t)y * dw + x] = rgb((int)(r / count), (int)(g / count), (int)(b / count));
        }
    }
    return out;
}

static uint32_t *load_thumb(const char *path, int *ow, int *oh)
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
    uint32_t *px = make_thumb(img, w, h, tw, ths);
    stbi_image_free(img);
    *ow = tw;
    *oh = ths;
    return px;
}

/* caller holds th_mtx */
static void th_evict(void)
{
    int ready = 0;
    for (int i = 0; i < g_nroms; i++)
        ready += g_th[i].state == 2;
    while (ready > THUMB_CACHE) {
        int far = -1, fd = -1;
        for (int i = 0; i < g_nroms; i++) {
            if (g_th[i].state != 2 || (i >= th_lo && i <= th_hi))
                continue;
            int d = abs(i - th_center);
            if (d > fd) { fd = d; far = i; }
        }
        if (far < 0)
            break;
        free(g_th[far].px);
        g_th[far].px = NULL;
        g_th[far].state = 0;
        ready--;
    }
}

static int th_loop(void *arg)
{
    (void)arg;
    for (;;) {
        SDL_LockMutex(th_mtx);
        int pick = -1;
        while (pick < 0) {
            if (th_quit) {
                SDL_UnlockMutex(th_mtx);
                return 0;
            }
            th_evict();
            int bd = 1 << 30;
            for (int i = th_lo; i <= th_hi; i++)
                if (i >= 0 && i < g_nroms && g_th[i].state == 0) {
                    int d = abs(i - th_center);
                    if (d < bd) { bd = d; pick = i; }
                }
            if (pick < 0)
                SDL_CondWaitTimeout(th_cond, th_mtx, 250);
        }
        char path[1400];
        snprintf(path, sizeof path, "%s", g_roms[pick].cover);
        g_th[pick].state = path[0] ? 1 : 3;
        SDL_UnlockMutex(th_mtx);

        uint32_t *px = NULL;
        int w = 0, h = 0;
        if (path[0])
            px = load_thumb(path, &w, &h);

        SDL_LockMutex(th_mtx);
        if (path[0]) {
            if (px) {
                g_th[pick].px = px;
                g_th[pick].w = w;
                g_th[pick].h = h;
                g_th[pick].state = 2;
            } else {
                g_th[pick].state = 3;
                fprintf(stderr, "[host] cannot read cover %s\n", path);
            }
        }
        th_epoch++;
        SDL_UnlockMutex(th_mtx);
    }
}

static void th_start(void)
{
    g_th = calloc((size_t)(g_nroms > 0 ? g_nroms : 1), sizeof *g_th);
    th_mtx = SDL_CreateMutex();
    th_cond = SDL_CreateCond();
    th_quit = false;
    th_lo = 0;
    th_hi = -1;
    th_thread = SDL_CreateThread(th_loop, "covers", NULL);
}

static void th_stop(void)
{
    if (!th_thread)
        return;
    SDL_LockMutex(th_mtx);
    th_quit = true;
    SDL_CondSignal(th_cond);
    SDL_UnlockMutex(th_mtx);
    SDL_WaitThread(th_thread, NULL);
    th_thread = NULL;
    for (int i = 0; i < g_nroms; i++)
        free(g_th[i].px);
    free(g_th);
    g_th = NULL;
    SDL_DestroyCond(th_cond);
    SDL_DestroyMutex(th_mtx);
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

/* Best box art for a game name; returns the index into cdi_boxart_list[] (or -1) and the
 * plain title score (0-100) in *raw. Region order: Europe, USA, World. */
static int match_boxart(const char *game, double *raw)
{
    static const char *const prefer[] = {"europe", "usa", "world"};
    static const char *const bad[] = {"beta", "demo", "proto", "sample", "promo", "unl", "preview"};
    if (!g_art_parsed) {
        g_art_parsed = calloc(CDI_BOXARTS_COUNT, sizeof *g_art_parsed);
        if (!g_art_parsed)
            return -1;
        for (int i = 0; i < CDI_BOXARTS_COUNT; i++)
            parse_name(cdi_boxart_list[i].name, true, &g_art_parsed[i]);
    }
    parsed_t r;
    parse_name(game, false, &r);
    int best = -1;
    double best_total = -1e9;
    if (raw)
        *raw = 0;
    for (int i = 0; i < CDI_BOXARTS_COUNT; i++) {
        const parsed_t *c = &g_art_parsed[i];
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
static SDL_Thread *cv_thread;
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
    fprintf(stderr, "[host] covers: %s\n", line);
    if (cv_logf) {
        fprintf(cv_logf, "%s\n", line);
        fflush(cv_logf);
    }
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

#ifdef PS5
int sceNetInit(void);
int sceNetPoolCreate(const char *name, int size, int flags);
int sceNetPoolDestroy(int pool);
int sceNetResolverCreate(const char *name, int memid, int flags);
int sceNetResolverStartNtoa(int rid, const char *hostname, void *addr, int timeout, int retry, int flags);
int sceNetResolverAbort(int rid, int flags);
int sceNetResolverDestroy(int rid);
int sceSslInit(size_t pool_size);
int sceSslTerm(int ctx);
int sceHttp2Init(int net_pool, int ssl_ctx, size_t pool_size, int max_requests);
int sceHttp2Term(int ctx);
int sceHttp2CreateTemplate(int ctx, const char *user_agent, int http_version, int auto_proxy);
int sceHttp2DeleteTemplate(int tmpl);
int sceHttp2CreateRequestWithURL(int tmpl, const char *method, const char *url, uint64_t content_length);
int sceHttp2DeleteRequest(int req);
int sceHttp2SendRequest(int req, const void *data, size_t size);
int sceHttp2GetStatusCode(int req, int *status);
int sceHttp2ReadData(int req, void *data, size_t size);
int sceHttp2SetResolveTimeOut(int id, uint32_t usec);
int sceHttp2SetConnectTimeOut(int id, uint32_t usec);
int sceHttp2SetSendTimeOut(int id, uint32_t usec);
int sceHttp2SetRecvTimeOut(int id, uint32_t usec);
int sceHttp2SetTimeOut(int id, uint32_t usec);
int sceHttp2SetAutoRedirect(int id, int enable);
int sceHttp2AbortRequest(int req);
int sceNetCtlInit(void);
void sceNetCtlTerm(void);
int sceNetCtlGetState(int *state);

static struct {
    int net_tried, net_ok, tried, ok, netctl, pool, ssl, ctx, tmpl;
    int offline_hint; /* NetCtl says there is no IP address: use short timeouts */
    volatile int active, rid; /* request / resolver in flight, so the main thread can abort them */
} hp = {.pool = -1, .ssl = -1, .ctx = -1, .tmpl = -1, .active = -1, .rid = -1};

/* Name -> IPv4 address with the console's own resolver (which works in a homebrew
 * process, unlike its HTTPS). */
static int net_resolve(const char *host, unsigned char ip[4])
{
    if (parse_ipv4(host, ip))
        return 0;
    if (hp.pool < 0)
        return -1;
    int rid = sceNetResolverCreate("cdihost", hp.pool, 0);
    if (rid < 0)
        return -1;
    hp.rid = rid;
    unsigned char addr[16] = {0};
    int r = sceNetResolverStartNtoa(rid, host, addr, 0, 0, 0);
    hp.rid = -1;
    sceNetResolverDestroy(rid);
    if (r < 0 || (addr[0] == 0 && addr[1] == 0 && addr[2] == 0 && addr[3] == 0))
        return -1;
    memcpy(ip, addr, 4);
    return 0;
}

/* Resolves a name and logs the answer: this is what tells a blocked DNS name from a
 * TLS or routing problem. */
static void dns_check(const char *host)
{
    unsigned char ip[4];
    if (net_resolve(host, ip) == 0)
        cvlog("dns %s: %u.%u.%u.%u", host, ip[0], ip[1], ip[2], ip[3]);
    else
        cvlog("dns %s: FAILED", host);
}

/* The network: state, pool, and a look at what the names resolve to. */
static SDL_mutex *g_net_mtx; /* created in main() */
static volatile int g_nas_busy; /* the NAS thread is using the network: the covers thread must not shut it down */

static int net_init_locked(char *why, size_t wn);
static int net_init(char *why, size_t wn)
{
    if (g_net_mtx)
        SDL_LockMutex(g_net_mtx);
    int r = net_init_locked(why, wn);
    if (g_net_mtx)
        SDL_UnlockMutex(g_net_mtx);
    return r;
}

static int net_init_locked(char *why, size_t wn)
{
    if (hp.net_tried)
        return hp.net_ok;
    hp.net_tried = 1;
    /* Is there a network at all? An offline console must not sit waiting on the resolver
     * (PS5SX2 saw a first request that never came back). 3 = IP address obtained. This
     * only shortens the timeouts: a wrong answer here must not stop the attempt. */
    int nc = sceNetCtlInit();
    hp.netctl = nc == 0;
    int state[4] = {-1, 0, 0, 0};
    int gs = sceNetCtlGetState(state);
    hp.offline_hint = gs == 0 && state[0] >= 0 && state[0] < 3;
    cvlog("netctl: init %#x, state %d (%#x)%s", (unsigned)nc, state[0], (unsigned)gs,
          hp.offline_hint ? " - no IP address, using short timeouts" : "");
    int net = sceNetInit(); /* an error here only means it was up already */
    hp.pool = sceNetPoolCreate("cdihost", 64 * 1024, 0);
    cvlog("net %#x pool %#x", (unsigned)net, (unsigned)hp.pool);
    hp.net_ok = hp.pool >= 0;
    if (!hp.net_ok) {
        snprintf(why, wn, "network could not start (pool %#x)", (unsigned)hp.pool);
        return 0;
    }
    if (!g_dns_diag_off)
        for (size_t i = 0; i < sizeof cv_hostname / sizeof *cv_hostname; i++)
            dns_check(cv_hostname[i]);
    return 1;
}

/* The console's own HTTPS stack. */
static int http_init(char *why, size_t wn)
{
    if (!net_init(why, wn))
        return 0;
    if (hp.tried)
        return hp.ok;
    hp.tried = 1;
    hp.ssl = sceSslInit(256 * 1024);
    hp.ctx = hp.ssl >= 0 ? sceHttp2Init(hp.pool, hp.ssl, 256 * 1024, 1) : -1;
    hp.tmpl = hp.ctx >= 0 ? sceHttp2CreateTemplate(hp.ctx, "ps5-cdi/1.0", 3, 1) : -1;
    cvlog("system https: ssl %#x http2 %#x template %#x", (unsigned)hp.ssl, (unsigned)hp.ctx, (unsigned)hp.tmpl);
    hp.ok = hp.tmpl >= 0;
    if (!hp.ok)
        snprintf(why, wn, "system HTTPS could not start (ssl %#x http2 %#x)", (unsigned)hp.ssl, (unsigned)hp.ctx);
    return hp.ok;
}

static void http_term(void)
{
    if (g_nas_busy)
        return; /* the NAS scan is on the network right now; the next start-up finds it still set up */
    if (hp.tmpl >= 0) sceHttp2DeleteTemplate(hp.tmpl);
    if (hp.ctx >= 0) sceHttp2Term(hp.ctx);
    if (hp.ssl >= 0) sceSslTerm(hp.ssl);
    if (hp.pool >= 0) sceNetPoolDestroy(hp.pool);
    if (hp.netctl) sceNetCtlTerm();
    hp.net_tried = hp.net_ok = hp.tried = hp.ok = hp.netctl = 0;
    hp.pool = hp.ssl = hp.ctx = hp.tmpl = -1;
}

static void http_abort(void)
{
    int req = hp.active, rid = hp.rid;
    if (req >= 0)
        sceHttp2AbortRequest(req);
    if (rid >= 0)
        sceNetResolverAbort(rid, 0);
}

/* The system's HTTPS. Returns the HTTP status (200: *out holds the body), or -1 with
 * `why` filled in. */
static int sys_get(const char *url, unsigned char **out, size_t *len, char *why, size_t wn)
{
    *out = NULL;
    *len = 0;
    if (cv_quit || !http_init(why, wn))
        return -1;
    int req = sceHttp2CreateRequestWithURL(hp.tmpl, "GET", url, 0);
    if (req < 0) {
        snprintf(why, wn, "request not created %#x", (unsigned)req);
        return -1;
    }
    const uint32_t t = hp.offline_hint ? 5u * 1000 * 1000 : 10u * 1000 * 1000;
    sceHttp2SetResolveTimeOut(req, t);
    sceHttp2SetConnectTimeOut(req, t);
    sceHttp2SetSendTimeOut(req, t);
    sceHttp2SetRecvTimeOut(req, t);
    sceHttp2SetTimeOut(req, hp.offline_hint ? 8u * 1000 * 1000 : 20u * 1000 * 1000);
    sceHttp2SetAutoRedirect(req, 1);
    hp.active = req;
    int status = -1;
    errno = 0;
    int sent = cv_quit ? -1 : sceHttp2SendRequest(req, NULL, 0);
    if (sent != 0) {
        snprintf(why, wn, "send failed %#x (errno %d)", (unsigned)sent, errno);
    } else {
        int got = sceHttp2GetStatusCode(req, &status);
        if (got != 0) {
            snprintf(why, wn, "no status %#x", (unsigned)got);
            status = -1;
        } else if (status == 200) {
            size_t cap = 256 * 1024, n = 0;
            unsigned char *buf = malloc(cap);
            while (buf) {
                if (cap - n < 65536) {
                    cap *= 2;
                    unsigned char *nb = cap > (16u << 20) ? NULL : realloc(buf, cap);
                    if (!nb) { free(buf); buf = NULL; break; }
                    buf = nb;
                }
                int r = sceHttp2ReadData(req, buf + n, cap - n);
                if (r < 0) {
                    snprintf(why, wn, "read failed %#x after %zu bytes", (unsigned)r, n);
                    free(buf);
                    buf = NULL;
                    break;
                }
                if (r == 0)
                    break;
                n += (size_t)r;
            }
            if (buf) {
                *out = buf;
                *len = n;
            } else {
                status = -1;
            }
        }
    }
    hp.active = -1;
    sceHttp2DeleteRequest(req);
    return status;
}

/* Our own client: the network must be up, but not the system's HTTPS. */
static int own_get(const char *url, unsigned char **out, size_t *len, char *why, size_t wn)
{
    if (cv_quit || !net_init(why, wn))
        return -1;
    return tls_https_get(url, net_resolve, &cv_quit, 30000, out, len, why, wn);
}
#else
static SDL_mutex *g_net_mtx;
static volatile int g_nas_busy;
static void http_term(void) {}
static void http_abort(void) {}

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

/* Test hooks (desktop only): CDIHOST_TEST_BLOCK=<text> makes any URL containing it fail,
 * CDIHOST_TEST_URLMAP="<from>|<to>" rewrites a URL prefix, CDIHOST_TEST_NO_SYSTEM_HTTPS
 * makes the system HTTPS fail the way it does on the console. */
static const char *test_map_url(const char *url, char *mapped, size_t mn, char *why, size_t wn)
{
    const char *block = getenv("CDIHOST_TEST_BLOCK");
    if (block && strstr(url, block)) {
        snprintf(why, wn, "blocked for the test");
        return NULL;
    }
    const char *map = getenv("CDIHOST_TEST_URLMAP");
    if (map) {
        const char *bar = strchr(map, '|');
        if (bar && !strncmp(url, map, (size_t)(bar - map))) {
            snprintf(mapped, mn, "%s%s", bar + 1, url + (bar - map));
            return mapped;
        }
    }
    return url;
}

/* Desktop stand-in for the system's HTTPS: the curl command. */
static int sys_get(const char *url, unsigned char **out, size_t *len, char *why, size_t wn)
{
    char tmp[1100], cmd[2600], mapped[900];
    *out = NULL;
    *len = 0;
    if (getenv("CDIHOST_TEST_NO_SYSTEM_HTTPS")) {
        snprintf(why, wn, "send failed 0x8095f00c (errno 2) [simulated]");
        return -1;
    }
    if (!(url = test_map_url(url, mapped, sizeof mapped, why, wn)))
        return -1;
    snprintf(tmp, sizeof tmp, "%s/.download.part", g_covers_dir);
    snprintf(cmd, sizeof cmd, "curl -sL --max-time 20 -o '%s' -w '%%{http_code}' '%s' 2>/dev/null", tmp, url);
    FILE *p = popen(cmd, "r");
    if (!p) {
        snprintf(why, wn, "cannot run curl");
        return -1;
    }
    char code[16] = {0};
    if (!fgets(code, sizeof code, p))
        code[0] = 0;
    pclose(p);
    int status = atoi(code);
    if (status <= 0) {
        remove(tmp);
        snprintf(why, wn, "curl could not connect");
        return -1;
    }
    if (status == 200) {
        FILE *f = fopen(tmp, "rb");
        if (f) {
            fseek(f, 0, SEEK_END);
            long n = ftell(f);
            fseek(f, 0, SEEK_SET);
            unsigned char *buf = n > 0 ? malloc((size_t)n) : NULL;
            if (buf && fread(buf, 1, (size_t)n, f) == (size_t)n) {
                *out = buf;
                *len = (size_t)n;
            } else {
                free(buf);
                snprintf(why, wn, "download incomplete");
                status = -1;
            }
            fclose(f);
        }
    }
    remove(tmp);
    return status;
}

static int own_get(const char *url, unsigned char **out, size_t *len, char *why, size_t wn)
{
    char mapped[900];
    *out = NULL;
    *len = 0;
    if (cv_quit)
        return -1;
    if (!(url = test_map_url(url, mapped, sizeof mapped, why, wn)))
        return -1;
    return tls_https_get(url, net_resolve, &cv_quit, 30000, out, len, why, wn);
}
#endif

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

/* One picture. The plans (how, and from which server) are tried in turn, starting with
 * the one that worked last:
 *   0, 1  the system's HTTPS: raw.githubusercontent.com, then thumbnails.libretro.com
 *   2, 3  our own HTTPS client, the same two servers: no certificate checks, so the
 *         picture is accepted only if it matches its known hash
 * /data/ps5-cdi/cover_secure_only limits it to plans 0 and 1. An HTTP answer (200,
 * 404...) ends the search; only failures to connect move on. *unchecked says whether
 * the picture came over a connection that was not certificate-checked. */
static int fetch_cover(const char *art, unsigned char **out, size_t *len, int *unchecked)
{
    static const struct { int own, host; } plans[] = {{0, 0}, {0, 1}, {1, 0}, {1, 1}};
    const int np = cv_secure_only ? 2 : 4;
    const int start = cv_plan_good >= 0 && cv_plan_good < np ? cv_plan_good : 0;
    char enc[600], url[800], why[200], sys_why[160] = "", own_why[160] = "";
    int saw404 = 0;
    url_encode(art, enc, sizeof enc);
    *unchecked = 0;
    for (int k = 0; k < np && !cv_quit; k++) {
        int pi = (start + k) % np;
        snprintf(url, sizeof url, "%s%s", cv_prefix[plans[pi].host], enc);
        why[0] = 0;
        int status = plans[pi].own ? own_get(url, out, len, why, sizeof why) : sys_get(url, out, len, why, sizeof why);
        if (status > 0 && status != 200 && status != 404)
            snprintf(why, sizeof why, "HTTP status %d", status); /* e.g. a filter's block page: try the next plan */
        cvlog("plan %d (%s, %s): %s", pi, plans[pi].own ? "own HTTPS" : "system HTTPS", cv_hostname[plans[pi].host],
              status == 200 ? "200 ok" : status == 404 ? "404 not found" : why);
        if (status == 200) {
            cv_plan_good = pi;
            *unchecked = plans[pi].own;
            return status;
        }
        free(*out);
        *out = NULL;
        if (status == 404) {
            saw404 = 1;
            if (plans[pi].host == 0) { /* the list comes from this very repository: it is really gone */
                cv_plan_good = pi;
                return 404;
            }
        }
        if (plans[pi].own)
            snprintf(own_why, sizeof own_why, "%s", why);
        else
            snprintf(sys_why, sizeof sys_why, "%s", why);
    }
    if (saw404)
        return 404;
    if (own_why[0] && sys_why[0])
        snprintf(cv_msg, sizeof cv_msg, "own HTTPS: %s | system: %s", own_why, sys_why);
    else
        snprintf(cv_msg, sizeof cv_msg, "%s", own_why[0] ? own_why : sys_why[0] ? sys_why : "no connection");
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
        SDL_LockMutex(th_mtx);
        for (int i = 0; i < g_nroms; i++)
            if (cv_cand[i] >= 0 && !g_roms[i].cover[0]) {
                int d = abs(i - th_center);
                if (d < bd) { bd = d; pick = i; }
            }
        SDL_UnlockMutex(th_mtx);
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
            SDL_LockMutex(th_mtx);
            th_epoch++;
            SDL_UnlockMutex(th_mtx);
            break;
        }
        if (status == 404) {
            char m[1300], dir[1100];
            missing_path(&g_roms[pick], m, sizeof m);
            snprintf(dir, sizeof dir, "%s/.missing", g_covers_dir);
            mkdir(dir, 0777);
            write_atomic(m, (const unsigned char *)"404\n", 4);
        }
        SDL_LockMutex(th_mtx);
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
        SDL_CondSignal(th_cond);
        SDL_UnlockMutex(th_mtx);
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
    cv_thread = SDL_CreateThread(cv_run, "covers-net", NULL);
}

static void cv_stop(void)
{
    if (cv_thread) {
        cv_quit = 1;
        http_abort();
        SDL_WaitThread(cv_thread, NULL);
        cv_thread = NULL;
    }
    free(cv_cand);
    cv_cand = NULL;
    if (cv_logf) {
        fclose(cv_logf);
        cv_logf = NULL;
    }
}

/* Draws a thumbnail scaled to dw x dh at (dx, dy); bilinear unless 1:1. */
static void blit_thumb(SDL_Surface *s, const thumb_t *t, int dx, int dy, int dw, int dh)
{
    if (dw < 1 || dh < 1)
        return;
    const int y_from = dy < 0 ? -dy : 0, y_to = dy + dh > s->h ? s->h - dy : dh;
    const int x_from = dx < 0 ? -dx : 0, x_to = dx + dw > s->w ? s->w - dx : dw;
    for (int y = y_from; y < y_to; y++) {
        Uint32 *dst = (Uint32 *)((Uint8 *)s->pixels + (size_t)(dy + y) * s->pitch) + dx;
        if (dw == t->w && dh == t->h) {
            memcpy(dst + x_from, t->px + (size_t)y * t->w + x_from, (size_t)(x_to - x_from) * 4);
            continue;
        }
        /* source position of this row's centre, 16.16 fixed point */
        long fy = (((long)y * 2 + 1) * t->h * 32768L) / dh - 32768L;
        if (fy < 0) fy = 0;
        int sy = (int)(fy >> 16), wy = (int)(fy & 0xFFFF);
        int sy1 = sy + 1 < t->h ? sy + 1 : sy;
        const uint32_t *r0 = t->px + (size_t)sy * t->w, *r1 = t->px + (size_t)sy1 * t->w;
        for (int x = x_from; x < x_to; x++) {
            long fx = (((long)x * 2 + 1) * t->w * 32768L) / dw - 32768L;
            if (fx < 0) fx = 0;
            int sx = (int)(fx >> 16), wx = (int)(fx & 0xFFFF);
            int sx1 = sx + 1 < t->w ? sx + 1 : sx;
            uint32_t p00 = r0[sx], p10 = r0[sx1], p01 = r1[sx], p11 = r1[sx1];
            Uint32 o = 0xFFu << sh_a;
            const int sh[3] = {sh_r, sh_g, sh_b};
            for (int c = 0; c < 3; c++) {
                long a = (long)((p00 >> sh[c]) & 255) * (65536 - wx) + (long)((p10 >> sh[c]) & 255) * wx;
                long b = (long)((p01 >> sh[c]) & 255) * (65536 - wx) + (long)((p11 >> sh[c]) & 255) * wx;
                long v = (a * (65536 - wy) + b * wy) >> 32;
                o |= (Uint32)v << sh[c];
            }
            dst[x] = o;
        }
    }
}

/* --- list view --- */

static void menu_draw(SDL_Surface *s, int sel, int top, const char *roms_dir)
{
    const int sc = s->h >= 900 ? 2 : 1;
    const int line = 20 * sc;
    const int mx = s->w / 12;
    const int y0 = 110 * sc;
    const int rows = (s->h - y0 - 70 * sc) / line;
    const int maxc = (s->w - 2 * mx) / (8 * sc);
    const Uint32 bg = rgb(20, 20, 28), bar = rgb(42, 90, 160), title = rgb(90, 170, 255);
    const Uint32 text = rgb(200, 200, 210), hi = rgb(255, 255, 255), dim = rgb(130, 130, 145);
    char buf[256];

    fill_rect(s, 0, 0, s->w, s->h, bg);
    draw_text(s, mx, 24 * sc, sc + 1, title, "CD-i", maxc);
    if (g_nroms > 0) {
        snprintf(buf, sizeof buf, "%d / %d", sel + 1, g_nroms);
        draw_text(s, s->w - mx - (int)strlen(buf) * 8 * sc, 36 * sc, sc, dim, buf, maxc);
    }
    snprintf(buf, sizeof buf, "Games in %s", roms_dir);
    draw_text(s, mx, 78 * sc, sc, dim, buf, maxc);
    for (int i = 0; i < rows && top + i < g_nroms; i++) {
        int y = y0 + i * line;
        if (top + i == sel)
            fill_rect(s, mx - 10 * sc, y - 2 * sc, s->w - 2 * mx + 20 * sc, line, bar);
        draw_text_fit(s, mx, y, sc, top + i == sel ? hi : text, g_roms[top + i].name, maxc);
    }
    draw_text(s, mx, s->h - 44 * sc, sc, dim,
              "Cross: play   D-pad/stick: choose   L1/R1: page   L2/R2: letter   Square: covers   L3: rescan   Options: quit",
              maxc);
}

/* --- cover grid --- */

typedef struct {
    int cols, rows, cell, gap, x0, y0;
} grid_t;

static grid_t grid_geom(const SDL_Surface *s)
{
    grid_t g;
    const int mx = s->w / 16;
    g.gap = s->h / 34;
    g.cols = (s->w * 10 > s->h * 15) ? 5 : 4;
    g.cell = (s->w - 2 * mx - (g.cols - 1) * g.gap) / g.cols;
    const int top = s->h * 14 / 100, bottom = s->h * 23 / 100;
    const int avail = s->h - top - bottom;
    g.rows = (avail + g.gap) / (g.cell + g.gap);
    if (g.rows < 1)
        g.rows = 1;
    const int used = g.rows * g.cell + (g.rows - 1) * g.gap;
    g.x0 = mx;
    g.y0 = top + (avail > used ? (avail - used) / 2 : 0);
    return g;
}

/* Tile for a game without a (readable) cover: its title on a plain panel. */
static void draw_placeholder(SDL_Surface *s, int x, int y, int w, int h, const char *name, int sc,
                             bool selected)
{
    fill_rect(s, x, y, w, h, selected ? rgb(52, 62, 92) : rgb(38, 42, 58));
    frame_rect(s, x + 6 * sc, y + 6 * sc, w - 12 * sc, h - 12 * sc, sc, rgb(80, 90, 120));
    const int maxc = (w - 28 * sc) / (8 * sc);
    const int max_lines = (h - 28 * sc) / (18 * sc);
    char line[128];
    const char *p = name;
    int n = 0;
    int lines_total = 0;
    /* count lines first so the block can be centred */
    for (const char *q = name; *q && lines_total < max_lines;) {
        int len = 0, brk = -1;
        while (q[len] && len < maxc) {
            if (q[len] == ' ') brk = len;
            len++;
        }
        if (q[len] && brk > 0) len = brk;
        q += len;
        while (*q == ' ') q++;
        lines_total++;
    }
    int ty = y + (h - lines_total * 18 * sc) / 2;
    while (*p && n < max_lines) {
        int len = 0, brk = -1;
        while (p[len] && len < maxc) {
            if (p[len] == ' ') brk = len;
            len++;
        }
        if (p[len] && brk > 0) len = brk;
        snprintf(line, sizeof line, "%.*s", len, p);
        int tw = (int)strlen(line) * 8 * sc;
        draw_text(s, x + (w - tw) / 2, ty, sc, rgb(215, 215, 225), line, maxc);
        ty += 18 * sc;
        p += len;
        while (*p == ' ') p++;
        n++;
    }
}

static void grid_draw(SDL_Surface *s, int sel, int top_row, const char *roms_dir)
{
    const grid_t g = grid_geom(s);
    const int sc = s->h >= 900 ? 2 : 1;
    const int mx = s->w / 16;
    const int maxc = (s->w - 2 * mx) / (8 * sc);
    const Uint32 bg = rgb(20, 20, 28), title = rgb(90, 170, 255), hi = rgb(255, 255, 255);
    const Uint32 dim = rgb(130, 130, 145), sel_col = rgb(255, 255, 255), shadow = rgb(12, 12, 18);
    char buf[256];

    fill_rect(s, 0, 0, s->w, s->h, bg);
    draw_text(s, mx, 24 * sc, sc + 1, title, "CD-i", maxc);
    if (g_nroms > 0) {
        snprintf(buf, sizeof buf, "%d / %d", sel + 1, g_nroms);
        draw_text(s, s->w - mx - (int)strlen(buf) * 8 * sc, 36 * sc, sc, dim, buf, maxc);
    }
    if (cv_total > 0 && (cv_net_down || cv_done < cv_total)) {
        if (cv_net_down) {
            snprintf(buf, sizeof buf, "Covers: %s", cv_msg);
            draw_text_fit(s, mx, 36 * sc + 22 * sc, sc, rgb(230, 170, 90), buf, maxc - 12);
            snprintf(buf, sizeof buf, "Triangle: retry");
        } else {
            snprintf(buf, sizeof buf, "Downloading covers  %d / %d", cv_done, cv_total);
        }
        draw_text(s, s->w - mx - (int)strlen(buf) * 8 * sc, 36 * sc + 22 * sc, sc, dim, buf, maxc);
    }
    if (g_nas_state != 0) { /* the NAS, one line under the covers line */
        char nb[520];
        SDL_LockMutex(g_nas_mtx);
        snprintf(nb, sizeof nb, "%s", g_nas_msg);
        const int st = g_nas_state;
        SDL_UnlockMutex(g_nas_mtx);
        const int small = sc > 1 ? sc - 1 : 1; /* a smaller font, in the gap between the title and the tiles */
        draw_text_fit(s, mx, 48 * sc, small, st == 3 ? rgb(230, 170, 90) : st == 2 ? rgb(120, 200, 140) : dim, nb,
                      (s->w - 2 * mx) / (8 * small) - 34);
    }
    if (g_nroms == 0) {
        draw_text(s, mx, s->h / 3, sc, hi, "No games found.", maxc);
        snprintf(buf, sizeof buf, "Copy .cue, .chd or .iso files into %s", roms_dir);
        draw_text_fit(s, mx, s->h / 3 + 24 * sc, sc, dim, buf, maxc);
        draw_text_fit(s, mx, s->h / 3 + 44 * sc, sc, dim,
                      "Covers (.png or .jpg) go next to the game or in the covers folder.", maxc);
        return;
    }

    SDL_LockMutex(th_mtx);
    th_lo = (top_row - 1) * g.cols;
    th_hi = (top_row + g.rows + 1) * g.cols - 1;
    th_center = sel;
    SDL_CondSignal(th_cond);

    int sel_x = 0, sel_y = 0, have_sel = 0;
    for (int pass = 0; pass < 2; pass++) { /* selected tile last, so it overlaps its neighbours */
        for (int row = 0; row < g.rows; row++)
            for (int col = 0; col < g.cols; col++) {
                int idx = (top_row + row) * g.cols + col;
                if (idx >= g_nroms)
                    continue;
                if ((idx == sel) != (pass == 1))
                    continue;
                const int cx = g.x0 + col * (g.cell + g.gap);
                const int cy = g.y0 + row * (g.cell + g.gap);
                const thumb_t *t = &g_th[idx];
                int dw = g.cell, dh = g.cell, dx = cx, dy = cy;
                if (t->state == 2) { /* fit the cover inside the cell, keep its shape */
                    if ((long)t->w * g.cell >= (long)t->h * g.cell) {
                        dw = g.cell;
                        dh = (int)((long)t->h * g.cell / t->w);
                        if (dh > g.cell) { dh = g.cell; dw = (int)((long)t->w * g.cell / t->h); }
                    }
                    if (t->h > t->w) {
                        dh = g.cell;
                        dw = (int)((long)t->w * g.cell / t->h);
                    }
                    dx = cx + (g.cell - dw) / 2;
                    dy = cy + (g.cell - dh) / 2;
                }
                if (idx == sel) { /* selected: 12% bigger, with a border and a shadow */
                    int gw = dw * 112 / 100, gh = dh * 112 / 100;
                    dx -= (gw - dw) / 2;
                    dy -= (gh - dh) / 2;
                    dw = gw;
                    dh = gh;
                    fill_rect(s, dx + 5 * sc, dy + 5 * sc, dw, dh, shadow);
                    have_sel = 1;
                    sel_x = dx;
                    sel_y = dy;
                }
                if (t->state == 2)
                    blit_thumb(s, t, dx, dy, dw, dh);
                else
                    draw_placeholder(s, dx, dy, dw, dh, g_roms[idx].name, sc, idx == sel);
                if (g_roms[idx].nas[0] || has_ext(g_roms[idx].path, "7z") || g_roms[idx].drive[0]) { /* tag: NAS, packed, or on a USB drive */
                    const char *tag = g_roms[idx].nas[0] ? "NAS" : has_ext(g_roms[idx].path, "7z") ? "7z" : "USB";
                    const int bw = 3 * 8 * sc + 8 * sc, bh = 16 * sc + 6 * sc;
                    fill_rect(s, dx + 4 * sc, dy + 4 * sc, bw, bh,
                              g_roms[idx].nas[0] ? rgb(20, 70, 140) : has_ext(g_roms[idx].path, "7z") ? rgb(90, 60, 20) : rgb(20, 100, 60));
                    draw_text(s, dx + 8 * sc, dy + 7 * sc, sc, rgb(255, 255, 255), tag, 3);
                }
                if (idx == sel)
                    frame_rect(s, dx, dy, dw, dh, 3 * sc, sel_col);
            }
    }
    (void)have_sel; (void)sel_x; (void)sel_y;
    SDL_UnlockMutex(th_mtx);

    const char *file = strrchr(g_roms[sel].path, '/');
    const int tsc = sc + 1;                         /* title: 3x font at 1080p */
    const int hint_y = s->h - 20 * sc - 16 * sc;    /* last line */
    const int file_y = hint_y - 22 * sc - 16 * sc;  /* file name above the hints */
    const int title_y = file_y - 12 * sc - 16 * tsc; /* title above the file name */
    draw_text_fit(s, mx, title_y, tsc, hi, g_roms[sel].name, (s->w - 2 * mx) / (8 * tsc));
    if (g_roms[sel].drive[0] && !has_ext(g_roms[sel].path, "7z")) {
        char info[1300];
        snprintf(info, sizeof info, "%s: %s", g_roms[sel].drive, file ? file + 1 : g_roms[sel].path);
        draw_text_fit(s, mx, file_y, sc, dim, info, maxc);
    } else if (g_roms[sel].nas[0] || has_ext(g_roms[sel].path, "7z")) {
        char info[1300];
        const bool here = game_is_here(&g_roms[sel]);
        const bool arc = has_ext(g_roms[sel].path, "7z");
        const char *what = g_roms[sel].nas[0] ? g_roms[sel].nas : (file ? file + 1 : g_roms[sel].path);
        snprintf(info, sizeof info, "%s%s   %s", g_roms[sel].nas[0] ? "NAS: " : g_roms[sel].drive[0] ? "7z archive on USB: " : "7z archive: ", what,
                 here ? (arc ? "(already unpacked on the console)" : "(a copy is already on the console)")
                      : (g_roms[sel].nas[0] ? (arc ? "(copied and unpacked when started)" : "(copied to the console when started)")
                                            : "(unpacked when started)"));
        draw_text_fit(s, mx, file_y, sc, dim, info, maxc);
    } else {
        draw_text_fit(s, mx, file_y, sc, dim, file ? file + 1 : g_roms[sel].path, maxc);
    }
    draw_text(s, mx, hint_y, sc, dim,
              "Cross: play   D-pad/stick: choose   L1/R1: page   L2/R2: letter   Square: list   L3: rescan   Options: quit",
              maxc);
}

/* Returns the chosen game, or -1 to quit. `script` (tests only) feeds keys:
 * l r u d = left/right/up/down, n p = page down/up, v = list/covers, E = play, Q = quit. */
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
            fprintf(stderr, "[host] drive %s (%s): no games folder (looked for %s)\n", labels[d], mounts[d],
                    g_drv_nfolders ? "the folders in drives.cfg" : "ps5-cdi/roms, ps5-cdi, cdi, CDi, CD-i, roms/cdi, roms");
            continue;
        }
        const int b = g_nroms;
        drive_scan_folder(folder, labels[d]);
        fprintf(stderr, "[host] drive %s: %d game(s) in %s\n", labels[d], g_nroms - b, folder);
    }
    for (int x = 0; x < g_drv_nextra; x++)
        if (is_dir_path(g_drv_extra[x])) {
            const int b = g_nroms;
            drive_scan_folder(g_drv_extra[x], "ext");
            fprintf(stderr, "[host] extra folder: %d game(s) in %s\n", g_nroms - b, g_drv_extra[x]);
        }
    if (g_nroms > before)
        notify("cdihost: %d game(s) found on external drives", g_nroms - before);
}

/* ------------------------------------------------- games on a NAS (NFS) and packed games (7z) */

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
} nas_cfg_t;
static nas_cfg_t g_nas = {.uid = -1, .gid = -1, .cache_gb = 10, .reserved = 1, .timeout_s = 10};
static char g_cache_dir[1024] = DEFAULT_CACHE_DIR;

static void nas_cfg_load(const char *dir)
{
    char path[1400], line[600];
    snprintf(path, sizeof path, "%s/nfs.cfg", dir);
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
        else if (!strcmp(k, "timeout")) g_nas.timeout_s = atoi(v) >= 2 && atoi(v) <= 60 ? atoi(v) : 10;
        else if (!strcmp(k, "cache_dir")) snprintf(g_cache_dir, sizeof g_cache_dir, "%s", v);
    }
    fclose(f);
    g_nas.enabled = g_nas.server[0] && g_nas.export_path[0];
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
        fprintf(stderr, "[host] cache: removing %s to make room\n", oldest);
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
    return nfsx_open(ips, g_nas.export_path, &o, why, wn);
}

/* --- the list of games on the NAS: a background thread reads it, the launcher merges what it found --- */

typedef struct {
    char rel[1100];
    uint64_t size;
} nas_entry_t;

static nas_entry_t *g_nas_cache; /* what the last successful scan found */
static int g_nas_ncache;
static SDL_Thread *g_nas_thread;
static FILE *g_nas_log;

static void nas_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void nas_log(const char *fmt, ...)
{
    char line[600];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    fprintf(stderr, "[host] NAS: %s\n", line);
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
        SDL_LockMutex(g_nas_mtx);
        nas_short_reason(why, g_nas_msg, sizeof g_nas_msg);
        g_nas_state = 3;
        SDL_UnlockMutex(g_nas_mtx);
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
    SDL_LockMutex(g_nas_mtx);
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
    SDL_UnlockMutex(g_nas_mtx);
    g_nas_busy = 0;
    return 0;
}

/* Starts (or restarts) the background scan. The launcher shows its progress; nothing waits for it. */
static void nas_start(const char *save_dir)
{
    if (!g_nas.enabled || g_nas_state == 1)
        return;
    if (!g_nas_mtx)
        g_nas_mtx = SDL_CreateMutex();
    char lp[1300];
    snprintf(lp, sizeof lp, "%s/nas_log.txt", save_dir);
    if (g_nas_log)
        fclose(g_nas_log);
    g_nas_log = fopen(lp, "w");
    SDL_LockMutex(g_nas_mtx);
    snprintf(g_nas_msg, sizeof g_nas_msg, "NAS: connecting to %s ...", g_nas.server);
    g_nas_state = 1;
    SDL_UnlockMutex(g_nas_mtx);
    g_nas_thread = SDL_CreateThread(nas_scan_thread, "nas-scan", NULL);
    if (g_nas_thread)
        SDL_DetachThread(g_nas_thread);
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
    SDL_LockMutex(g_nas_mtx);
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
    SDL_UnlockMutex(g_nas_mtx);
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

static void draw_copy_screen(SDL_Surface *s, const job_t *j, double mbps, double eta)
{
    const int sc = s->h >= 900 ? 2 : 1;
    const int mx = 60 * sc;
    const int maxc = (s->w - 2 * mx) / (8 * sc);
    char line[300];
    fill_rect(s, 0, 0, s->w, s->h, rgb(16, 18, 30));
    draw_text(s, mx, 24 * sc, sc + 1, rgb(90, 170, 255), j->title, maxc);
    draw_text_fit(s, mx, 120 * sc, sc + 1, rgb(255, 255, 255), g_roms[j->idx].name, maxc / 2);
    const int bx = mx, by = 200 * sc, bw = s->w - 2 * mx, bh = 28 * sc;
    fill_rect(s, bx, by, bw, bh, rgb(40, 44, 66));
    const uint64_t done = j->done;
    int fw = j->total ? (int)((double)bw * (double)done / (double)j->total) : 0;
    if (fw > bw) fw = bw;
    fill_rect(s, bx, by, fw, bh, rgb(90, 170, 255));
    snprintf(line, sizeof line, "%llu of %llu MB", (unsigned long long)(done >> 20), (unsigned long long)(j->total >> 20));
    draw_text(s, mx, by + bh + 14 * sc, sc, rgb(255, 255, 255), line, maxc);
    if (mbps > 0.05) {
        int secs = (int)eta;
        snprintf(line, sizeof line, "%.1f MB/s     about %d:%02d left", mbps, secs / 60, secs % 60);
        draw_text(s, mx, by + bh + 38 * sc, sc, rgb(160, 168, 190), line, maxc);
    }
    draw_text_fit(s, mx, by + bh + 70 * sc, sc, rgb(160, 168, 190), j->phase, maxc);
    if (j->resumed)
        draw_text(s, mx, by + bh + 94 * sc, sc, rgb(160, 168, 190), "(part of this game was already here)", maxc);
    draw_text(s, mx, s->h - 36 * sc, sc, rgb(160, 168, 190), "Circle: cancel", maxc);
}

static void draw_message_screen(SDL_Surface *s, const char *title, const char *msg)
{
    const int sc = s->h >= 900 ? 2 : 1;
    const int mx = 60 * sc;
    const int maxc = (s->w - 2 * mx) / (8 * sc);
    fill_rect(s, 0, 0, s->w, s->h, rgb(16, 18, 30));
    draw_text(s, mx, 24 * sc, sc + 1, rgb(230, 170, 90), title, maxc);
    draw_text_fit(s, mx, 120 * sc, sc, rgb(255, 255, 255), msg, maxc);
    draw_text(s, mx, s->h - 36 * sc, sc, rgb(160, 168, 190), "Cross: back to the list", maxc);
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

/* Makes game `idx` playable. 0 = ready (launch holds what to start), 1 = cancelled, -1 = failed (a
 * message was shown). Games that are neither on the NAS nor packed are ready at once. */
static int game_prepare(int idx, char *launch, size_t launch_len)
{
    const rom_t *r = &g_roms[idx];
    if (!r->nas[0] && !has_ext(r->path, "7z")) {
        snprintf(launch, launch_len, "%s", r->path);
        if (r->ndisc > 1) { /* local discs: the list goes into the save folder */
            char m3u[1300];
            snprintf(m3u, sizeof m3u, "%.800s/discs/%.190s.m3u", g_save_dir, r->name);
            if (write_m3u(m3u, r->discs, r->ndisc))
                snprintf(launch, launch_len, "%.*s", (int)launch_len - 1, m3u);
        }
        return 0;
    }
    SDL_Surface *s = SDL_GetWindowSurface(win);
    if (!s || s->format->BytesPerPixel != 4)
        return -1;
    job_t *j = calloc(1, sizeof *j);
    if (!j)
        return -1;
    j->idx = idx;
    SDL_Thread *th = SDL_CreateThread(prepare_job, "prepare-game", j);
    if (!th) {
        free(j);
        return -1;
    }
    Uint32 last = SDL_GetTicks();
    uint64_t last_done = 0;
    double mbps = 0, eta = 0;
    int dumped = 0;
    while (j->state == 0) {
        SDL_Event e;
        while (SDL_PollEvent(&e))
            if (e.type == SDL_QUIT)
                j->cancel = 1;
        if (g_signal || pad_btn(SDL_CONTROLLER_BUTTON_B))
            j->cancel = 1;
        Uint32 now = SDL_GetTicks();
        if (now - last >= 500) {
            double dt = (double)(now - last) / 1000.0;
            uint64_t d = j->done;
            double inst = d >= last_done ? (double)(d - last_done) / 1048576.0 / dt : 0; /* a new stage starts at 0 */
            mbps = mbps > 0 ? mbps * 0.6 + inst * 0.4 : inst;
            last_done = d;
            last = now;
            eta = mbps > 0.05 && j->total > j->done ? (double)(j->total - j->done) / 1048576.0 / mbps : 0;
        }
        draw_copy_screen(s, j, mbps, eta);
        SDL_UpdateWindowSurface(win);
#ifdef CDIHOST_TEST_DUMP
        if (!dumped && j->total && j->done * 2 > j->total) {
            dumped = 1;
            FILE *f = fopen("/tmp/cdihost_copy.ppm", "wb");
            if (f) {
                fprintf(f, "P6\n%d %d\n255\n", s->w, s->h);
                for (int y = 0; y < s->h; y++)
                    for (int x = 0; x < s->w; x++) {
                        Uint32 px = ((Uint32 *)((Uint8 *)s->pixels + y * s->pitch))[x];
                        fputc((px >> sh_r) & 255, f); fputc((px >> sh_g) & 255, f); fputc((px >> sh_b) & 255, f);
                    }
                fclose(f);
            }
        }
#endif
        SDL_Delay(50);
    }
    SDL_WaitThread(th, NULL);
    (void)dumped;
    int state = j->state;
    if (state == 2) {
        notify("cdihost: %s", j->why);
        fprintf(stderr, "[host] game preparation failed: %s\n", j->why);
        draw_message_screen(s, "Could not get the game ready", j->why);
        SDL_UpdateWindowSurface(win);
        Uint32 until = SDL_GetTicks() + 10000;
        while (SDL_GetTicks() < until && !g_signal) {
            SDL_Event e;
            while (SDL_PollEvent(&e)) {}
            if (pad_btn(SDL_CONTROLLER_BUTTON_A))
                break;
            SDL_Delay(50);
        }
    }
    if (state == 1)
        snprintf(launch, launch_len, "%s", j->launch);
    free(j);
    return state == 1 ? 0 : state == 3 ? 1 : -1;
}

static char g_resel[1100]; /* the game that was selected when the list was rescanned */
static const char *g_script_rest; /* test hook: where the menu script stopped, for the next time the menu opens */

static int run_menu(const char *roms_dir, int sel, const char *script)
{
    if (script && g_script_rest)
        script = g_script_rest;
    SDL_Surface *s = SDL_GetWindowSurface(win);
    if (!s || s->format->BytesPerPixel != 4) {
        notify("cdihost: cannot draw the game list (no window surface)");
        return -1;
    }
    sh_r = s->format->Rshift; sh_g = s->format->Gshift; sh_b = s->format->Bshift;
    sh_a = s->format->Ashift;
    if (s->format->Amask == 0) { /* no alpha channel: use the unused byte */
        Uint32 used = s->format->Rmask | s->format->Gmask | s->format->Bmask;
        for (int b = 0; b < 32; b += 8)
            if (!((used >> b) & 0xFF)) { sh_a = b; break; }
    }
    const grid_t g = grid_geom(s);
    th_max = g.cell * 115 / 100;
    if (th_max < 64) th_max = 64;
    const int sc = s->h >= 900 ? 2 : 1;
    const int list_rows = (s->h - 110 * sc - 70 * sc) / (20 * sc);

    int grid_view = 1, top = 0, top_row = 0, dirty = 1, script_waited = 0;
    int prev_cross = 1, prev_l1 = 1, prev_r1 = 1, prev_opt = 1, prev_sq = 1, prev_tri = 1, prev_l3 = 1, prev_l2 = 1, prev_r2 = 1;
    Uint32 next_move = 0;
    unsigned drawn_epoch = 0;

    th_start();
    cv_start();
    for (;;) {
        int dx = 0, dy = 0, page = 0, go = 0, quit = 0, toggle = 0, retry = 0, rescan = 0, auto_rescan = 0, jump = 0;
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT)
                quit = 1;
            else if (e.type == SDL_JOYDEVICEADDED || e.type == SDL_CONTROLLERDEVICEADDED)
                pad_open();
            else if (e.type == SDL_JOYDEVICEREMOVED || e.type == SDL_CONTROLLERDEVICEREMOVED)
                pad_close();
#ifndef PS5 /* the console's SDL also turns pad presses into key events: pad only there */
            else if (e.type == SDL_KEYDOWN) {
                switch (e.key.keysym.sym) {
                case SDLK_LEFT:     dx = -1; break;
                case SDLK_RIGHT:    dx = 1; break;
                case SDLK_UP:       dy = -1; break;
                case SDLK_DOWN:     dy = 1; break;
                case SDLK_PAGEUP:   page = -1; break;
                case SDLK_PAGEDOWN: page = 1; break;
                case SDLK_RETURN:   go = 1; break;
                case SDLK_TAB:      toggle = 1; break;
                case SDLK_ESCAPE:   quit = 1; break;
                }
            }
#endif
        }
        if (script) { /* test hook: one key per pass, after the covers in view have loaded */
            for (int w = 0; w < 400 && !dirty; w++) {
                SDL_LockMutex(th_mtx);
                int pending = 0;
                for (int i = th_lo; i <= th_hi; i++)
                    if (i >= 0 && i < g_nroms && g_th[i].state < 2 && g_roms[i].cover[0] &&
                        g_th[i].state != 3)
                        pending = 1;
                SDL_UnlockMutex(th_mtx);
                if (!pending)
                    break;
                SDL_Delay(5);
            }
            switch (*script) {
            case 'l': dx = -1; break;
            case 'r': dx = 1; break;
            case 'u': dy = -1; break;
            case 'd': dy = 1; break;
            case 'n': page = 1; break;
            case 'f': jump = 1; break;
            case 'b': jump = -1; break;
            case 'p': page = -1; break;
            case 'v': toggle = 1; break;
            case 'R': retry = 1; break;
            case 'S': rescan = 1; break;
            case 'E': go = 1; break;
            case 'Q': quit = 1; break;
            default: break;
            }
            if (*script) {
                if (*script != 'S' && !getenv("CDIHOST_TEST_NOWAIT"))
                    for (int w = 0; w < 3000 && g_nas_state == 1; w++) /* test hook: let a running NAS scan finish first */
                        SDL_Delay(10);
                script++;
                g_script_rest = script;
            } else if (!script_waited) {
                script_waited = 1; /* let the downloads finish, then show the result once */
#ifdef CDIHOST_TEST_DUMP
                if (getenv("CDIHOST_TEST_FINAL_WAIT_MS")) /* test only: give the thumbnail loader time before the last picture */
                    SDL_Delay((Uint32)atoi(getenv("CDIHOST_TEST_FINAL_WAIT_MS")));
#endif
                for (int w = 0; w < 24000 && cv_thread && (!cv_matched || cv_done < cv_total) && !cv_net_down; w++)
                    SDL_Delay(5);
                for (int w = 0; w < 1000; w++) {
                    SDL_LockMutex(th_mtx);
                    int pending = 0;
                    for (int i = 0; i < g_nroms; i++)
                        if (g_th[i].state < 2 && g_roms[i].cover[0] && g_th[i].state != 3 && i >= th_lo && i <= th_hi)
                            pending = 1;
                    SDL_UnlockMutex(th_mtx);
                    if (!pending)
                        break;
                    SDL_Delay(5);
                }
                dirty = 1;
            } else {
                quit = 1;
            }
        }

        if (pad_present()) {
            Uint32 now = SDL_GetTicks();
            int pdx = (pad_btn(SDL_CONTROLLER_BUTTON_DPAD_RIGHT) || pad_axis(SDL_CONTROLLER_AXIS_LEFTX) > 16000) -
                      (pad_btn(SDL_CONTROLLER_BUTTON_DPAD_LEFT) || pad_axis(SDL_CONTROLLER_AXIS_LEFTX) < -16000);
            int pdy = (pad_btn(SDL_CONTROLLER_BUTTON_DPAD_DOWN) || pad_axis(SDL_CONTROLLER_AXIS_LEFTY) > 16000) -
                      (pad_btn(SDL_CONTROLLER_BUTTON_DPAD_UP) || pad_axis(SDL_CONTROLLER_AXIS_LEFTY) < -16000);
            if ((pdx || pdy) && now >= next_move) { /* hold to repeat: slow start, then faster */
                dx = pdx;
                dy = pdy;
                next_move = now + (next_move == 0 ? 350 : 90);
            } else if (!pdx && !pdy) {
                next_move = 0;
            }
            int cross = pad_btn(SDL_CONTROLLER_BUTTON_A);
            int l1 = pad_btn(SDL_CONTROLLER_BUTTON_LEFTSHOULDER);
            int r1 = pad_btn(SDL_CONTROLLER_BUTTON_RIGHTSHOULDER);
            int opt = pad_btn(SDL_CONTROLLER_BUTTON_START);
            int sq = pad_btn(SDL_CONTROLLER_BUTTON_X);
            int tri = pad_btn(SDL_CONTROLLER_BUTTON_Y);
            if (tri && !prev_tri) retry = 1;
            prev_tri = tri;
            int l3 = pad_btn(SDL_CONTROLLER_BUTTON_LEFTSTICK);
            if (l3 && !prev_l3) rescan = 1;
            prev_l3 = l3;
            if (cross && !prev_cross) go = 1;
            if (l1 && !prev_l1) page = -1;
            if (r1 && !prev_r1) page = 1;
            int l2t = pad_axis(SDL_CONTROLLER_AXIS_TRIGGERLEFT) > 8000, r2t = pad_axis(SDL_CONTROLLER_AXIS_TRIGGERRIGHT) > 8000;
            if (l2t && !prev_l2) jump = -1;
            if (r2t && !prev_r2) jump = 1;
            prev_l2 = l2t; prev_r2 = r2t;
            if (opt && !prev_opt) quit = 1;
            if (sq && !prev_sq) toggle = 1;
            prev_cross = cross; prev_l1 = l1; prev_r1 = r1; prev_opt = opt; prev_sq = sq;
        }

        if (quit || g_signal) {
            cv_stop();
            th_stop();
            return -1;
        }
        if (go && g_nroms > 0) {
            cv_stop();
            th_stop();
            return sel;
        }
        if (g_nas_changed) { /* a NAS scan finished: merge it (leave the list, scan, come back to the same game) */
            rescan = 1;
            auto_rescan = 1;
        }
        if (rescan) { /* look for games again (a drive plugged in after the start): leave, the caller scans and returns */
            if (g_nroms > 0)
                snprintf(g_resel, sizeof g_resel, "%s", g_roms[sel].path);
            if (!auto_rescan)
                nas_start(g_save_dir); /* L3: read the NAS again too (the result is merged when it arrives) */
            cv_stop();
            th_stop();
            return -2;
        }
        if (toggle) {
            grid_view = !grid_view;
            dirty = 1;
        }
        if (retry && g_download_ok) { /* try the downloads again, e.g. after changing the DNS */
            cv_stop();
            cv_plan_good = -1;
            cv_start();
            dirty = 1;
        }
        if (g_nroms > 0 && jump) { /* first game of the next / previous letter (digits and symbols share one group) */
            #define LETTER_CLASS(i) (isalpha((unsigned char)g_roms[i].name[0]) ? toupper((unsigned char)g_roms[i].name[0]) : '#')
            int cls = LETTER_CLASS(sel), t = sel;
            if (jump > 0) {
                while (t < g_nroms && LETTER_CLASS(t) == cls)
                    t++;
                if (t >= g_nroms)
                    t = 0; /* past the end: back to the start */
            } else {
                int start = sel;
                while (start > 0 && LETTER_CLASS(start - 1) == cls)
                    start--;
                if (start < sel) {
                    t = start; /* first press: the start of this letter */
                } else if (start > 0) {
                    int pc = LETTER_CLASS(start - 1);
                    t = start - 1;
                    while (t > 0 && LETTER_CLASS(t - 1) == pc)
                        t--;
                } else {
                    t = g_nroms - 1;
                    int lc = LETTER_CLASS(t);
                    while (t > 0 && LETTER_CLASS(t - 1) == lc)
                        t--; /* before the start: the last letter */
                }
            }
            #undef LETTER_CLASS
            if (t != sel) {
                sel = t;
                dirty = 1;
            }
        }
        if (g_nroms > 0 && (dx || dy || page)) {
            int old = sel;
            if (grid_view) {
                if (dx) sel += dx;
                if (dy) {
                    int ns = sel + dy * g.cols;
                    if (ns >= g_nroms && dy > 0 && sel / g.cols < (g_nroms - 1) / g.cols)
                        ns = g_nroms - 1; /* a shorter last row: go to its end */
                    if (ns >= 0 && ns < g_nroms)
                        sel = ns;
                }
                if (page) sel += page * g.rows * g.cols;
            } else {
                if (dy) {
                    sel += dy;
                    if (sel < 0) sel = g_nroms - 1; /* wrap in the list */
                    if (sel >= g_nroms) sel = 0;
                }
                if (page) sel += page * (list_rows > 1 ? list_rows - 1 : 1);
            }
            if (sel < 0) sel = 0;
            if (sel >= g_nroms) sel = g_nroms - 1;
            if (sel != old)
                dirty = 1;
        }
        if (g_nroms > 0) { /* keep the selection on screen */
            int row = sel / g.cols;
            if (row < top_row) top_row = row;
            if (row >= top_row + g.rows) top_row = row - g.rows + 1;
            if (sel < top) top = sel;
            if (sel >= top + list_rows) top = sel - list_rows + 1;
        }
        SDL_LockMutex(th_mtx);
        if (th_epoch != drawn_epoch && grid_view)
            dirty = 1; /* a cover finished loading */
        SDL_UnlockMutex(th_mtx);
        if (dirty) {
            SDL_LockMutex(th_mtx);
            drawn_epoch = th_epoch;
            SDL_UnlockMutex(th_mtx);
            if (grid_view)
                grid_draw(s, sel, top_row, roms_dir);
            else
                menu_draw(s, sel, top, roms_dir);
#ifdef CDIHOST_TEST_DUMP
            FILE *f = fopen("/tmp/cdihost_menu.ppm", "wb");
            if (f) {
                fprintf(f, "P6\n%d %d\n255\n", s->w, s->h);
                for (int y = 0; y < s->h; y++)
                    for (int x = 0; x < s->w; x++) {
                        Uint32 px = ((Uint32 *)((Uint8 *)s->pixels + (size_t)y * s->pitch))[x];
                        Uint8 rgbv[3];
                        SDL_GetRGB(px, s->format, &rgbv[0], &rgbv[1], &rgbv[2]);
                        fwrite(rgbv, 1, 3, f);
                    }
                fclose(f);
            }
#endif
            SDL_UpdateWindowSurface(win);
            dirty = 0;
        }
        SDL_Delay(script ? 1 : 16);
    }
}

/* ------------------------------------------------- core options and the digital video ROM */

#ifdef PS5
#define DEFAULT_OPTIONS_DIR "/data/ps5-cdi"
#else
#define DEFAULT_OPTIONS_DIR "."
#endif

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
    }
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
static void check_dvc_rom(void)
{
    char path[1400];
    snprintf(path, sizeof path, "%s/same_cdi/bios/cdimono1/vmpega.rom", g_system_dir);
    if (access(path, R_OK) == 0) {
        fprintf(stderr, "[host] digital video ROM found: %s\n", path);
        return;
    }
    snprintf(path, sizeof path, "%s/same_cdi/bios/cdimono1.zip", g_system_dir);
    FILE *f = fopen(path, "rb");
    if (f) { /* a zip lists its file names in plain text */
        fseek(f, 0, SEEK_END);
        long n = ftell(f);
        fseek(f, 0, SEEK_SET);
        char *buf = n > 0 && n < (64L << 20) ? malloc((size_t)n) : NULL;
        bool found = false;
        if (buf && fread(buf, 1, (size_t)n, f) == (size_t)n) /* (the console's libc has no memmem) */
            for (long i = 0; i + 10 <= n && !found; i++)
                found = buf[i] == 'v' && !memcmp(buf + i, "vmpega.rom", 10);
        free(buf);
        fclose(f);
        if (found)
            return;
    }
    snprintf(path, sizeof path, "%s/same_cdi/bios/cdimono1", g_system_dir);
    struct stat st;
    if (stat(path, &st) == 0 || f) {
        fprintf(stderr, "[host] vmpega.rom is not in the cdimono1 BIOS set\n");
        notify("cdihost: vmpega.rom not found next to the BIOS: games that use digital video will say 'digital video capability missing'");
    }
}

static void usage(const char *argv0)
{
    fprintf(stderr,
        "usage: %s <core.so> <game | -> [options]\n"
        "  game \"-\"         pick from the game list in the roms folder\n"
        "  --roms DIR       roms folder (default %s)\n"
        "  --covers DIR     covers folder (default %s)\n"
        "  --no-download    do not fetch missing covers from the internet\n"
        "  --options DIR    folder with options.cfg and options/<game>.cfg (default %s)\n"
        "  --list           print the games found in the roms folder and exit\n"
        "  --system DIR     system/BIOS dir (default ./system)\n"
        "  --save DIR       save dir (default ./saves)\n"
        "  --scale N        initial window scale (default 2)\n"
        "  --window WxH     exact window size, e.g. 1920x1080\n"
        "  --nvram-interval S  write NVRAM every S seconds (default 20, 0 = only at exit)\n"
        "  --device NAME    port 0 device: joypad | mouse | analog\n"
        "  --fullscreen     start fullscreen\n"
        "  --frames N       exit after N frames (for automated tests)\n"
        "  --debug          print video/audio diagnostics\n"
        "  --autofit        fit the crop to the first frame's picture\n"
        "  --opt KEY=VALUE  set a core option (mouse is enabled by default)\n"
        "keys: arrows=dpad Z/X/A/S=B/A/Y/X Q/W=L/R Enter=Start RShift=Select\n"
        "      F2 mouse capture  F5/F7 save/load state  F10 reset  F11 fullscreen  Esc quit\n",
        argv0, DEFAULT_ROMS_DIR, DEFAULT_COVERS_DIR, DEFAULT_OPTIONS_DIR);
}

int main(int argc, char **argv)
{
#ifdef STATIC_CORE
    /* A payload ELF is launched without arguments: use built-in defaults. */
    static char *dflt_argv[] = {(char *)"cdihost", (char *)"(static core)",
                                (char *)"-", NULL}; /* "-" = pick from the game list */
    if (argc < 3) {
        argc = 3;
        argv = dflt_argv;
    }
#endif
    notify("cdihost: started");
    if (argc < 3) {
        usage(argv[0]);
        return 2;
    }
    const char *game_path = argv[2]; /* "-" = choose from the game list */
    const char *roms_dir = DEFAULT_ROMS_DIR;
    const char *covers_dir = DEFAULT_COVERS_DIR;
    const char *menu_script = NULL;   /* tests only */
    const char *match_name = NULL;    /* tests: print the box art a name matches, then exit */
    const char *options_dir = DEFAULT_OPTIONS_DIR;
    bool list_only = false;
    int scale = 2;
    int win_w = 0, win_h = 0; /* desktop only: explicit window size */
    int nvram_interval = 20;  /* seconds between NVRAM writes, 0 = only at exit */
    bool fullscreen = false;
    long max_frames = -1;
    int device = -1;

    snprintf(g_core_path, sizeof g_core_path, "%s", argv[1]);
    for (int i = 3; i < argc; i++) {
        if (!strcmp(argv[i], "--system") && i + 1 < argc)
            snprintf(g_system_dir, sizeof g_system_dir, "%s", argv[++i]);
        else if (!strcmp(argv[i], "--save") && i + 1 < argc)
            snprintf(g_save_dir, sizeof g_save_dir, "%s", argv[++i]);
        else if (!strcmp(argv[i], "--scale") && i + 1 < argc)
            scale = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--window") && i + 1 < argc)
            sscanf(argv[++i], "%dx%d", &win_w, &win_h);
        else if (!strcmp(argv[i], "--roms") && i + 1 < argc)
            roms_dir = argv[++i];
        else if (!strcmp(argv[i], "--covers") && i + 1 < argc)
            covers_dir = argv[++i];
        else if (!strcmp(argv[i], "--options") && i + 1 < argc)
            options_dir = argv[++i];
        else if (!strcmp(argv[i], "--no-download"))
            g_download_ok = false;
        else if (!strcmp(argv[i], "--match") && i + 1 < argc)
            match_name = argv[++i];
        else if (!strcmp(argv[i], "--list"))
            list_only = true;
        else if (!strcmp(argv[i], "--menu-script") && i + 1 < argc)
            menu_script = argv[++i];
        else if (!strcmp(argv[i], "--nvram-interval") && i + 1 < argc)
            nvram_interval = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--frames") && i + 1 < argc)
            max_frames = atol(argv[++i]);
        else if (!strcmp(argv[i], "--fullscreen"))
            fullscreen = true;
        else if (!strcmp(argv[i], "--debug"))
            g_debug = true;
        else if (!strcmp(argv[i], "--autofit"))
            g_autofit_req = 1; /* crop to the picture on the first frame */
        else if (!strcmp(argv[i], "--opt") && i + 1 < argc)
            opt_set(argv[++i]);
        else if (!strcmp(argv[i], "--device") && i + 1 < argc) {
            const char *d = argv[++i];
            device = !strcmp(d, "mouse") ? RETRO_DEVICE_MOUSE
                   : !strcmp(d, "analog") ? RETRO_DEVICE_ANALOG
                   : RETRO_DEVICE_JOYPAD;
        } else {
            usage(argv[0]);
            return 2;
        }
    }
#ifdef AUTO_EXIT_FRAMES
    if (max_frames < 0)
        max_frames = AUTO_EXIT_FRAMES; /* test builds end by themselves */
#endif
    mkdirs(g_system_dir);
    mkdirs(g_save_dir);
    view_load();
    if (match_name) {
        double raw;
        int a = match_boxart(match_name, &raw);
        printf("%s\n", a >= 0 ? cdi_boxart_list[a].name : "(none)");
        printf("%.1f\n", a >= 0 ? raw : 0.0);
        return 0;
    }
    snprintf(g_covers_dir, sizeof g_covers_dir, "%s", covers_dir);
#ifdef PS5
    if (access("/data/ps5-cdi/no_cover_download", F_OK) == 0)
        g_download_ok = false; /* a file with this name switches the download off */
#endif
    signal(SIGTERM, on_signal);
    signal(SIGINT, on_signal);
    signal(SIGHUP, on_signal);

    const bool use_menu = !strcmp(game_path, "-");
    if (use_menu || list_only) {
        mkdirs(roms_dir);
        mkdirs(covers_dir);
        nas_cfg_load(options_dir);
        drives_cfg_load(options_dir);
        roms_scan(roms_dir, covers_dir);
        if (!g_net_mtx)
            g_net_mtx = SDL_CreateMutex();
        nas_start(g_save_dir); /* in the background: the launcher opens at once, the NAS games appear when they are read */
        if (list_only) {
            /* the NAS is read in the background: for a one-off listing, wait for it and merge what it found */
            for (int w = 0; w < 6000 && g_nas_state == 1; w++)
                SDL_Delay(10);
            if (g_nas_changed)
                roms_scan(roms_dir, covers_dir);
            if (g_nas_state == 3)
                printf("%s\n", g_nas_msg);
            for (int i = 0; i < g_nroms; i++) {
                const char *c = strrchr(g_roms[i].cover, '/');
                printf("%3d  %-34s  cover: %-24s  %s\n", i + 1, g_roms[i].name,
                       g_roms[i].cover[0] ? (c ? c + 1 : g_roms[i].cover) : "-", g_roms[i].path);
            }
            printf("%d game(s) in %s\n", g_nroms, roms_dir);
            return 0;
        }
        notify("cdihost: %d game(s) in %s", g_nroms, roms_dir);
    }

    if (!core_open(g_core_path))
        return 1;
    if (core.retro_api_version() != RETRO_API_VERSION) {
        fprintf(stderr, "core API version mismatch\n");
        return 1;
    }

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_EVENTS) != 0) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        notify("cdihost: SDL_Init failed: %s", SDL_GetError());
        return 1;
    }
    if (SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER) != 0)
        fprintf(stderr, "no gamepad support: %s\n", SDL_GetError());
    fprintf(stderr, "SDL video driver: %s\n", SDL_GetCurrentVideoDriver());
    notify("cdihost: SDL ok, video driver %s", SDL_GetCurrentVideoDriver());

    /* Callbacks must be registered before retro_init(). */
    core.retro_set_environment(env_cb);
    core.retro_set_video_refresh(video_cb);
    core.retro_set_audio_sample(audio_sample_cb);
    core.retro_set_audio_sample_batch(audio_batch_cb);
    core.retro_set_input_poll(input_poll_cb);
    core.retro_set_input_state(input_state_cb);
    core.retro_init();

    struct retro_system_info si;
    memset(&si, 0, sizeof si);
    core.retro_get_system_info(&si);
    fprintf(stderr, "core: %s %s (need_fullpath=%d)\n", si.library_name,
            si.library_version, si.need_fullpath);

    /* Window first: the core may call video_cb during retro_load_game(). */
#ifdef PS5
    (void)scale;
    (void)win_w;
    (void)win_h;
    (void)fullscreen;
    win = SDL_CreateWindow("cdihost", 0, 0, 1920, 1080, 0);
#else
    win = SDL_CreateWindow("cdihost", SDL_WINDOWPOS_CENTERED,
                           SDL_WINDOWPOS_CENTERED, win_w > 0 ? win_w : 384 * scale,
                           win_h > 0 ? win_h : 288 * scale,
                           SDL_WINDOW_RESIZABLE |
                               (fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0));
#ifndef USE_RENDER_THREAD
    ren = win ? SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED) : NULL;
    if (!ren && win)
        ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_SOFTWARE);
#endif
#endif
    pad_open();
    char launch_path[1100] = "";
    /* what the options files and per-game settings change: put back before the next game */
    static struct { char key[64]; char val[64]; } opts0[MAX_OPTS];
    const int nopts0 = g_nopts;
    memcpy(opts0, g_opts, sizeof opts0);
    const int audio_ms0 = g_audio_ms, audio_samples0 = g_audio_samples, ptr_speed0 = g_ptr_speed, ptr_mode0 = g_ptr_mode,
              ptr_yrange0 = g_ptr_yrange, smooth0 = g_smooth, smooth_strength0 = g_smooth_strength, dpad0 = g_dpad_digital;
    int btn0[8];
    memcpy(btn0, g_btn_map, sizeof btn0);
    const double aspect0 = g_aspect;
    g_menu_has_list = use_menu && win;
pick_game:
    if (use_menu && win) {
        const char *chosen = NULL;
        /* The NAS list arrives in the background, after this point: with no local games the launcher must still open
         * and wait for it (it shows "No games found" and the NAS status until the games are merged). */
        if (g_nroms > 0 || g_nas_state != 0) {
            int idx = run_menu(roms_dir, last_game_index(), menu_script);
            while (idx == -2) { /* rescan: scan again, then open the list on the game that was selected */
                roms_scan(roms_dir, covers_dir);
                notify("cdihost: %d game(s) after rescanning", g_nroms);
                int again = 0;
                for (int i = 0; i < g_nroms; i++)
                    if (!strcmp(g_roms[i].path, g_resel))
                        again = i;
                idx = run_menu(roms_dir, again, menu_script);
            }
            while (idx >= 0) { /* a game on the NAS or in a .7z is made ready first; cancelled or failed: back to the list */
                int prep = game_prepare(idx, launch_path, sizeof launch_path);
                if (prep == 0)
                    break;
                if (g_signal)
                    idx = -1;
                else
                    idx = run_menu(roms_dir, idx, menu_script);
                while (idx == -2) {
                    roms_scan(roms_dir, covers_dir);
                    int again = 0;
                    for (int i = 0; i < g_nroms; i++)
                        if (!strcmp(g_roms[i].path, g_resel))
                            again = i;
                    idx = run_menu(roms_dir, again, menu_script);
                }
            }
            if (idx < 0) { /* quit from the list: nothing is loaded yet, so just leave */
                notify("cdihost: bye");
                core.retro_deinit();
                SDL_Quit();
                return 0;
            }
            chosen = launch_path;
            snprintf(g_game_name, sizeof g_game_name, "%.199s", g_roms[idx].base);
            last_game_save(g_roms[idx].path);
            notify("cdihost: loading %s", g_roms[idx].name);
            fprintf(stderr, "[host] selected %s\n", chosen);
        } else {
#ifdef PS5
            const char *legacy = "/data/ps5-cdi/game.cue"; /* the single-game layout used so far */
            if (access(legacy, R_OK) == 0) {
                chosen = legacy;
                notify("cdihost: roms folder is empty, using %s", legacy);
            }
#endif
            if (!chosen) {
                run_menu(roms_dir, 0, menu_script); /* shows the "no games" message until quit */
                core.retro_deinit();
                SDL_Quit();
                return 0;
            }
        }
        game_path = chosen;
    }
#ifdef USE_RENDER_THREAD
    g_vid_reset = 1; /* the launcher drew on the screen: start from black */
    const bool video_ok = win && video_thread_start();
#else
    const bool video_ok = ren != NULL;
#endif
    if (!video_ok) {
        fprintf(stderr, "window/renderer: %s\n", SDL_GetError());
        notify("cdihost: window/renderer failed: %s", SDL_GetError());
        return 1;
    }

    apply_game_options(options_dir, game_path);
    snprintf(g_game_name_for_states, sizeof g_game_name_for_states, "%s", g_game_name);
    audio_cfg_load();
    ptr_cfg_load();
    fprintf(stderr, "[host] settings for '%s': pointer %s %d%%, buttons cross=%d circle=%d square=%d triangle=%d r2=%d, video smoothing %s\n", g_game_name,
            g_ptr_mode ? "absolute" : "relative", g_ptr_speed, g_btn_map[0], g_btn_map[1], g_btn_map[2], g_btn_map[3], g_btn_map[7],
            g_smooth ? "on" : "off");
#ifdef CDIHOST_TEST_STICK
    if (getenv("CDIHOST_TEST_PTR_SAVE")) {
        int sp, md;
        if (sscanf(getenv("CDIHOST_TEST_PTR_SAVE"), "%d:%d", &sp, &md) == 2) { g_ptr_speed = sp; g_ptr_mode = md; ptr_cfg_save(); }
    }
#endif
    check_dvc_rom();

    struct retro_game_info gi;
    memset(&gi, 0, sizeof gi);
    gi.path = game_path;
    void *rom = NULL;
    if (!si.need_fullpath) {
        FILE *f = fopen(game_path, "rb");
        if (!f) {
            perror(game_path);
            return 1;
        }
        fseek(f, 0, SEEK_END);
        gi.size = (size_t)ftell(f);
        fseek(f, 0, SEEK_SET);
        rom = malloc(gi.size);
        if (!rom || fread(rom, 1, gi.size, f) != gi.size) {
            fprintf(stderr, "read failed\n");
            return 1;
        }
        fclose(f);
        gi.data = rom;
    }
    if (!core.retro_load_game(&gi)) {
        fprintf(stderr, "retro_load_game failed (check BIOS in %s)\n", g_system_dir);
        notify("cdihost: load game failed (BIOS in %s?)", g_system_dir);
        return 1;
    }

    core.retro_get_system_av_info(&g_av);
    if (g_aspect <= 0)
        g_aspect = g_av.geometry.aspect_ratio;
    double fps = g_av.timing.fps > 1 ? g_av.timing.fps : 60.0;
    unsigned rate = g_av.timing.sample_rate > 1 ? (unsigned)g_av.timing.sample_rate : 44100;
    fprintf(stderr, "av: %ux%u @ %.3f fps, audio %u Hz, aspect %.3f\n",
            g_av.geometry.base_width, g_av.geometry.base_height, fps, rate, g_aspect);

    if (!audio_open(rate))
        return 1;
    notify("cdihost: running %ux%u, %.0f fps, audio %u Hz, latency %d ms, pointer %s %d%%, smoothing %s",
           g_av.geometry.base_width, g_av.geometry.base_height, fps, rate, g_audio_ms,
           g_ptr_mode ? "absolute" : "relative", g_ptr_speed, g_smooth ? "on" : "off");
    if (core.retro_flush_nvram)
        notify("cdihost: NVRAM autosave on (every %d s)", nvram_interval);
    else
        notify("cdihost: this core has no NVRAM flush - saved games are only kept after a clean exit");
    if (device >= 0)
        core.retro_set_controller_port_device(0, (unsigned)device);
    pad_open();

    size_t target = (size_t)((double)g_audio_ms * rate / 1000.0);
    long frames = 0, last_frames = 0;
    double next_run_ms = 0, lvl_ema = 0;
    unsigned long panic_runs = 0;
    Uint32 last_tick = SDL_GetTicks();

    Uint32 nvram_tick = last_tick;

    /* performance report (on-screen notification every 10 s on the PS5) */
    Uint32 stat_tick = last_tick;
    long stat_frames0 = 0;
    double run_ms = 0, stat_run0 = 0;
    unsigned long stat_under0 = 0, stat_panic0 = 0;
#ifdef USE_RENDER_THREAD
    unsigned long stat_pres0 = 0;
    double stat_pms0 = 0;
#endif

    while (g_running) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            switch (e.type) {
            case SDL_QUIT:
                g_running = false;
                break;
            case SDL_CONTROLLERDEVICEADDED:
            case SDL_JOYDEVICEADDED:
                pad_open();
                break;
            case SDL_CONTROLLERDEVICEREMOVED:
            case SDL_JOYDEVICEREMOVED:
                pad_close();
                break;
            case SDL_MOUSEMOTION:
                if (SDL_GetRelativeMouseMode()) {
                    mouse_dx += e.motion.xrel;
                    mouse_dy += e.motion.yrel;
                }
                break;
            case SDL_MOUSEBUTTONDOWN:
            case SDL_MOUSEBUTTONUP: {
                int bit = e.button.button == SDL_BUTTON_LEFT ? 1
                        : e.button.button == SDL_BUTTON_RIGHT ? 2
                        : e.button.button == SDL_BUTTON_MIDDLE ? 4 : 0;
                if (e.type == SDL_MOUSEBUTTONDOWN) mouse_buttons |= bit;
                else mouse_buttons &= ~bit;
                break;
            }
            case SDL_KEYDOWN:
                if (e.key.repeat)
                    break;
                switch (e.key.keysym.sym) {
#ifndef PS5
                case SDLK_ESCAPE: g_running = false; break;
#endif
                case SDLK_F2:
                    SDL_SetRelativeMouseMode(!SDL_GetRelativeMouseMode());
                    break;
                case SDLK_TAB: g_menu_req = 1; break;
                case SDLK_F5:  state_save(); break;
                case SDLK_F7:  state_load(); break;
                case SDLK_F10: core.retro_reset(); break;
                case SDLK_F11:
                    SDL_SetWindowFullscreen(win, (SDL_GetWindowFlags(win) &
                        SDL_WINDOW_FULLSCREEN_DESKTOP) ? 0 : SDL_WINDOW_FULLSCREEN_DESKTOP);
                    break;
                }
                break;
            }
        }

        if (g_signal)
            g_running = false; /* clean shutdown writes the NVRAM */
#ifdef CDIHOST_TEST_STICK
        {
            static long menu_at_done = -1; /* test only: open the menu once at this frame (CDIHOST_TEST_MENUAT, comma list) */
            const char *ma = getenv("CDIHOST_TEST_MENUAT");
            for (const char *c = ma; c && *c && !g_menu.open;) {
                long f = strtol(c, (char **)&c, 10);
                if (f == frames && frames != menu_at_done) {
                    menu_at_done = frames;
                    g_menu_req = 1;
                }
                while (*c == ',')
                    c++;
                if (*c && !isdigit((unsigned char)*c))
                    break;
            }
        }
#endif
        if (g_menu_req && !g_menu.open && audio_started && g_running) {
            g_menu_req = 0;
            if (adev)
                SDL_PauseAudioDevice(adev, 1); /* silence while paused */
            menu_open();
        }
        g_menu_req = 0;
        if (g_menu.open) { /* paused: the menu has the pad; the core does not run */
            char msg[100] = "";
            const int a = menu_step();
#ifdef CDIHOST_TEST_STICK
            if (a != MA_NONE) fprintf(stderr, "[menu] action %d (selected line %d: %s)\n", a, g_menu.sel, g_menu.it[g_menu.sel].label);
#endif
            if (a == MA_SAVE) {
                slot_save(g_slot, msg, sizeof msg);
                menu_message(msg);
                menu_build();
                notify("cdihost: %s", msg);
            } else if (a == MA_LOAD) {
                bool ok = slot_load(g_slot, msg, sizeof msg);
                notify("cdihost: %s", msg);
                menu_message(msg);
                menu_build();
                if (ok)
                    goto resume_game;
            } else if (a == MA_DISC) {
                disc_insert((unsigned)g_disc_pick, msg, sizeof msg);
                notify("cdihost: %s", msg);
                menu_message(msg);
                menu_build();
            } else if (a == MA_RESET) {
                core.retro_reset();
                notify("cdihost: game reset");
                goto resume_game;
            } else if (a == MA_LIST) {
                g_back_to_list = 1;
                menu_close();
                g_running = false;
            } else if (a == MA_QUIT) {
                menu_close();
                g_running = false;
            } else if (a == MA_RESUME) {
            resume_game:
                menu_close();
                if (adev)
                    SDL_PauseAudioDevice(adev, 0);
                next_run_ms = pc_ms(); /* the clock stood still while paused */
            }
            SDL_Delay(15);
            continue;
        }
        if (g_nvram_req) {
            g_nvram_req = 0;
            notify(nvram_flush() ? "cdihost: NVRAM saved" : "cdihost: cannot save NVRAM (core has no flush)");
        }
        if (g_av.timing.fps > 1)
            fps = g_av.timing.fps; /* core may change fps at runtime */
        target = (size_t)((double)g_audio_ms * rate / 1000.0);

        /* Frames are scheduled by the clock, evenly (every 1/fps s); the audio buffer only steers that clock
         * by up to 3 % (a real audio device is within 0.1 % of the clock), so the sound never runs dry or piles up. Before this, a frame was made whenever the
         * audio buffer dipped below a mark, which happens in steps of the audio device's buffer (10 or 21 ms):
         * frames came at uneven times and fast scrolling looked choppy even at a steady 50 fps. */
        size_t buffered = audio_buffered();
        const double period_ms = 1000.0 / fps;
        const double level_ms = (double)buffered * 1000.0 / (double)rate;
        const double clock_now = pc_ms();
        int do_run = 0;
        if (!audio_started) {
            do_run = 1; /* priming: make frames until the audio buffer has what it needs */
            if (!adev || buffered >= target) {
                if (adev)
                    SDL_PauseAudioDevice(adev, 0);
                audio_started = true;
                next_run_ms = clock_now + period_ms;
                lvl_ema = level_ms;
            }
        } else if (clock_now >= next_run_ms) {
            do_run = 1;
            /* steer: audio buffered before this frame, smoothed, against what was asked for (g_audio_ms) */
            lvl_ema += 0.1 * (level_ms - lvl_ema);
            /* The device takes the sound in chunks (g_audio_samples), so the buffer swings by about one chunk and a
             * bit around its average; aim a chunk and a half above g_audio_ms so that the low points stay safe. */
            const double want_ms = (double)g_audio_ms + 1.5 * (double)g_audio_samples * 1000.0 / (double)rate;
            double adj = (lvl_ema - want_ms) * 0.0005; /* +10 ms too much sound = 0.5 % slower */
            adj = adj < -0.03 ? -0.03 : adj > 0.03 ? 0.03 : adj;
            next_run_ms += period_ms * (1.0 + adj);
            if (clock_now - next_run_ms > 4.0 * period_ms) /* far behind (a hiccup): do not try to catch up */
                next_run_ms = clock_now + period_ms;
        } else if (adev && level_ms < 2.0) {
            do_run = 1; /* the sound is about to run dry: make a frame now */
            panic_runs++;
        }
#ifdef CDIHOST_TEST_STICK
        if (do_run && getenv("CDIHOST_TEST_PACELOG")) {
            static FILE *pf;
            if (!pf) pf = fopen(getenv("CDIHOST_TEST_PACELOG"), "w");
            if (pf) { fprintf(pf, "%.2f level=%.1f ema=%.1f scheduled=%d\n", clock_now, level_ms, lvl_ema, clock_now >= next_run_ms - period_ms * 1.5); fflush(pf); }
        }
#endif
        if (do_run) {
            Uint64 r0 = SDL_GetPerformanceCounter();
#ifdef CDIHOST_TEST_STICK
            { const char *rl = getenv("CDIHOST_TEST_RUNLOG"); if (rl) { static FILE *rf; if (!rf) rf = fopen(rl, "w"); if (rf) { fprintf(rf, "%.3f\n", pc_ms()); fflush(rf); } } }
#endif
            core.retro_run();
            run_ms += (double)(SDL_GetPerformanceCounter() - r0) * 1000.0 /
                      (double)SDL_GetPerformanceFrequency();
            frames++;
#ifndef USE_RENDER_THREAD
            render();
#endif
            if (max_frames >= 0 && frames >= max_frames)
                g_running = false;
        } else {
            const double rem = next_run_ms - clock_now;
            if (rem > 2.0)
                SDL_Delay(1);
            else if (rem > 0.3)
                SDL_Delay(0);
        }

        Uint32 now = SDL_GetTicks();
        if (nvram_interval > 0 && now - nvram_tick >= (Uint32)nvram_interval * 1000u) {
            nvram_tick = now;
            if (nvram_flush() && g_debug)
                fprintf(stderr, "[host] NVRAM written\n");
        }
        if (now - last_tick >= 1000) {
            char title[160];
            snprintf(title, sizeof title,
                     "cdihost - %ld fps | audio buf %zu | underruns %lu overruns %lu",
                     (frames - last_frames) * 1000 / (long)(now - last_tick),
                     buffered, underruns, overruns);
            SDL_SetWindowTitle(win, title);
            last_frames = frames;
            last_tick = now;
        }

        if (now - stat_tick >= 10000) {
            double secs = (double)(now - stat_tick) / 1000.0;
            long df = frames - stat_frames0;
            double emu_fps = (double)df / secs;
            double emu_ms = df ? (run_ms - stat_run0) / (double)df : 0;
#ifdef USE_RENDER_THREAD
            unsigned long dp = vid_presented - stat_pres0;
            double dpm = vid_present_ms - stat_pms0;
            notify("perf: emulation %.1f fps (%.1f ms/frame), screen %.1f fps (%.1f ms/frame), dropped %lu, audio underruns +%lu, early frames +%lu",
                   emu_fps, emu_ms, (double)dp / secs, dp ? dpm / (double)dp : 0.0,
                   vid_dropped, underruns - stat_under0, panic_runs - stat_panic0);
            stat_pres0 = vid_presented;
            stat_pms0 = vid_present_ms;
#else
            notify("perf: emulation %.1f fps (%.1f ms/frame), audio underruns +%lu, early frames +%lu",
                   emu_fps, emu_ms, underruns - stat_under0, panic_runs - stat_panic0);
#endif
            stat_tick = now;
            stat_frames0 = frames;
            stat_run0 = run_ms;
            stat_under0 = underruns;
            stat_panic0 = panic_runs;
        }
    }

    fprintf(stderr, "done: %ld frames, %lu underruns, %lu overruns\n", frames,
            underruns, overruns);
    notify("cdihost: done, %ld frames, %lu underruns", frames, underruns);
#ifdef USE_RENDER_THREAD
    video_thread_stop();
#endif
    nvram_flush(); /* the core also saves when it unloads; this is the belt to its braces */
    core.retro_unload_game();
    if (g_back_to_list && !g_signal) { /* from the menu: unload this game, show the list again, start the next one */
        g_back_to_list = 0;
        if (adev) {
            SDL_CloseAudioDevice(adev);
            adev = 0;
        }
        rb_r = rb_w = rb_count = 0;
        audio_started = false;
        free(rom);
        rom = NULL;
        memcpy(g_opts, opts0, sizeof opts0);
        g_nopts = nopts0;
        g_audio_ms = audio_ms0; g_audio_samples = audio_samples0; g_ptr_speed = ptr_speed0; g_ptr_mode = ptr_mode0;
        g_ptr_yrange = ptr_yrange0; g_smooth = smooth0; g_smooth_strength = smooth_strength0; g_dpad_digital = dpad0;
        memcpy(g_btn_map, btn0, sizeof btn0);
        g_aspect = aspect0;
        g_game_name[0] = 0;
        g_have_disk = 0;
        /* SAME_CDI does not run a second game after retro_unload_game (it stays paused, and retro_load_game does not
         * start a new machine): shut the core down and start it again. retro_init gives the disc tray back. */
        core.retro_deinit();
        core.retro_init();
        g_running = true;
        est_valid = 0;
        notify("cdihost: back to the game list");
        fprintf(stderr, "[host] back to the game list\n");
        goto pick_game;
    }
    core.retro_deinit();
    if (adev)
        SDL_CloseAudioDevice(adev);
    free(rom);
    SDL_Quit();
    return 0;
}
