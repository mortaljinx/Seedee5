/*
 * testcore - tiny libretro core: colour gradient + 440 Hz tone + save states.
 * Lets you verify the host (video, audio pacing, input, states) without any
 * BIOS or game. Build: make testcore.so
 */
#include <math.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "libretro.h"

#define W 320
#define H 240
static int RATE = 44100;  /* TESTCORE_RATE: SDL's dummy audio device only keeps time exactly when samples*1000/rate is a whole number of ms, e.g. 51200 with 512 samples */
static int FPS = 60;      /* TESTCORE_FPS=50 pretends to be a PAL machine */
static int bar_mode;      /* TESTCORE_BAR=1: a white bar moving 4 pixels a frame on black, for measuring smoothness */
static int shell_mode;    /* TESTCORE_SHELL=1: a pretend CD-i player: 100 frames of start-up, then a start screen of
                             another size with a pointer (the mouse) and a "Play" box; button 1 (Y) on the box
                             starts the "game" (the first size again, the usual picture) */
static int sh_px = 60, sh_py = 60, sh_started;
static uint32_t shfb[352 * 240];
static int image_mode;    /* TESTCORE_IMAGE=<file.ppm>: a still picture (binary P6, 320x240), for the picture filters */

static retro_environment_t env;
static retro_video_refresh_t video;
static retro_audio_sample_batch_t audio;
static retro_input_poll_t poll_cb;
static retro_input_state_t input;
static uint32_t fb[W * H];
static uint32_t frame;
static double phase;

void retro_set_environment(retro_environment_t cb) { env = cb; }
void retro_set_video_refresh(retro_video_refresh_t cb) { video = cb; }
void retro_set_audio_sample(retro_audio_sample_t cb) { (void)cb; }
void retro_set_audio_sample_batch(retro_audio_sample_batch_t cb) { audio = cb; }
void retro_set_input_poll(retro_input_poll_t cb) { poll_cb = cb; }
void retro_set_input_state(retro_input_state_t cb) { input = cb; }
void retro_init(void) {}
void retro_deinit(void) {}
unsigned retro_api_version(void) { return RETRO_API_VERSION; }
void retro_set_controller_port_device(unsigned p, unsigned d) { (void)p; (void)d; }
void retro_reset(void) { frame = 0; sh_started = 0; sh_px = sh_py = 60; }

void retro_get_system_info(struct retro_system_info *i)
{
    memset(i, 0, sizeof *i);
    i->library_name = "testcore";
    i->library_version = "0";
    i->valid_extensions = "bin";
    i->need_fullpath = true; /* ignores the game, never reads a file */
}

void retro_get_system_av_info(struct retro_system_av_info *i)
{
    if (getenv("TESTCORE_FPS") && atoi(getenv("TESTCORE_FPS")) > 0)
        FPS = atoi(getenv("TESTCORE_FPS"));
    bar_mode = getenv("TESTCORE_BAR") != NULL;
    shell_mode = getenv("TESTCORE_SHELL") != NULL;
    if (getenv("TESTCORE_IMAGE")) {
        FILE *f = fopen(getenv("TESTCORE_IMAGE"), "rb");
        int w = 0, h = 0, mx = 0;
        if (f && fscanf(f, "P6 %d %d %d", &w, &h, &mx) == 3 && w == W && h == H && fgetc(f) != EOF) {
            for (int i = 0; i < W * H; i++) {
                int r = fgetc(f), g = fgetc(f), b = fgetc(f);
                fb[i] = ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
            }
            image_mode = 1;
        }
        if (f)
            fclose(f);
    }
    if (getenv("TESTCORE_RATE") && atoi(getenv("TESTCORE_RATE")) > 8000)
        RATE = atoi(getenv("TESTCORE_RATE"));
    memset(i, 0, sizeof *i);
    i->geometry.base_width = i->geometry.max_width = W;
    i->geometry.base_height = i->geometry.max_height = H;
    i->geometry.aspect_ratio = 4.0f / 3.0f;
    i->timing.fps = FPS;
    i->timing.sample_rate = RATE;
}

bool retro_load_game(const struct retro_game_info *g)
{
    (void)g;
    frame = 0;
    sh_started = 0;
    sh_px = sh_py = 60;
    enum retro_pixel_format f = RETRO_PIXEL_FORMAT_XRGB8888;
    return env(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &f);
}

void retro_unload_game(void) {}

void retro_run(void)
{
    poll_cb();
    int boost = input(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B) ? 128 : 0;
    if (shell_mode && !sh_started) {
        const int mx = input(0, RETRO_DEVICE_MOUSE, 0, RETRO_DEVICE_ID_MOUSE_X);
        const int my = input(0, RETRO_DEVICE_MOUSE, 0, RETRO_DEVICE_ID_MOUSE_Y);
        const int b1 = input(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_Y);
        if (frame < 100) {
            memset(fb, 0, sizeof fb);
            video(fb, W, H, W * sizeof(uint32_t));
        } else {
            sh_px += mx / 4; /* the CD-i pointer moves a quarter of the counts, as an example */
            sh_py += my / 4;
            if (sh_px < 0) sh_px = 0;
            if (sh_px > 351) sh_px = 351;
            if (sh_py < 0) sh_py = 0;
            if (sh_py > 239) sh_py = 239;
            for (int y = 0; y < 240; y++)
                for (int x = 0; x < 352; x++) {
                    uint32_t c = 0x00203070;
                    if (x >= 200 && x < 260 && y >= 150 && y < 180) c = 0x00E0E0E0; /* Play */
                    if (abs(x - sh_px) < 3 && abs(y - sh_py) < 3) c = 0x00FFFF00;
                    shfb[y * 352 + x] = c;
                }
            if (b1 && getenv("TESTCORE_DEBUG")) fprintf(stderr, "testcore: click at %d,%d\n", sh_px, sh_py);
            if (b1 && sh_px >= 200 && sh_px < 260 && sh_py >= 150 && sh_py < 180)
                sh_started = 1;
            video(shfb, 352, 240, 352 * sizeof(uint32_t));
        }
        static int16_t sbuf[(96000 / 10) * 2];
        memset(sbuf, 0, sizeof sbuf);
        audio(sbuf, RATE / FPS);
        frame++;
        return;
    }
    if (image_mode) {
        /* the still picture, as loaded */
    } else if (bar_mode) {
        memset(fb, 0, sizeof fb);
        const int bx = (int)((frame * 4) % (W - 8));
        for (int y = 0; y < H; y++)
            for (int x = bx; x < bx + 8; x++)
                fb[y * W + x] = 0x00FFFFFF;
    } else {
        for (int y = 0; y < H; y++)
            for (int x = 0; x < W; x++)
                fb[y * W + x] = ((uint32_t)((x + frame) & 255) << 16) |
                                ((uint32_t)(y & 255) << 8) | (uint32_t)boost;
    }
    video(fb, W, H, W * sizeof(uint32_t));

    static int16_t buf[(96000 / 10) * 2];
    for (int i = 0; i < RATE / FPS; i++) {
        int16_t s = (int16_t)(sin(phase) * 6000);
        phase += 2 * 3.14159265358979 * 440.0 / RATE;
        buf[i * 2] = buf[i * 2 + 1] = s;
    }
    audio(buf, RATE / FPS);
    frame++;
}

size_t retro_serialize_size(void) { return sizeof frame + 3 * sizeof(int); }
bool retro_serialize(void *d, size_t n)
{
    if (n < retro_serialize_size()) return false;
    int v[3] = {sh_px, sh_py, sh_started};
    memcpy(d, &frame, sizeof frame);
    memcpy((char *)d + sizeof frame, v, sizeof v);
    return true;
}
bool retro_unserialize(const void *d, size_t n)
{
    if (n < retro_serialize_size()) return false;
    int v[3];
    memcpy(&frame, d, sizeof frame);
    memcpy(v, (const char *)d + sizeof frame, sizeof v);
    sh_px = v[0]; sh_py = v[1]; sh_started = v[2];
    return true;
}
void retro_cheat_reset(void) {}
void retro_cheat_set(unsigned i, bool e, const char *c) { (void)i; (void)e; (void)c; }
bool retro_load_game_special(unsigned t, const struct retro_game_info *g, size_t n) { (void)t; (void)g; (void)n; return false; }
unsigned retro_get_region(void) { return RETRO_REGION_NTSC; }
void *retro_get_memory_data(unsigned id) { (void)id; return NULL; }
size_t retro_get_memory_size(unsigned id) { (void)id; return 0; }
