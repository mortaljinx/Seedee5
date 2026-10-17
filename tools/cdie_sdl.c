/*
 * cdie_sdl - a desktop test program for the CD-i engine (engine/cdie.h).
 *
 * It does with SDL what the PS5 title does with Vulkan and the UI kit: pulls the sound, shows the two newest
 * frames blended (the previous one, then the newest at the blend's opacity, as the title's GPU does), passes
 * the keyboard or a pad in, and drives the engine's list, preparation and menu functions. It has no list
 * screen: the game is chosen on the command line.
 *
 *   cdie_sdl <data dir> <core.so> --list
 *   cdie_sdl <data dir> <core.so> --game N            the N-th game of the list (1 = first)
 *   cdie_sdl <data dir> <core.so> --file game.cue
 *   options: --frames N (stop after N screen refreshes)  --headless (no window, no sound device: tests)
 *            --dump PREFIX (write PREFIX-<refresh>.ppm at the refreshes in CDIE_TEST_DUMPAT)
 *            --no-download
 *
 * Keys: arrows = D-pad, Z X A S = Cross Circle Square Triangle, Q W = L1 R1, Enter = Options,
 *       Tab = pause (menu), F5 / F7 = save / load slot 1, F3 = next disc, F10 = reset, Esc = quit.
 * Test actions (CDIE_TEST_ACTIONS="<refresh>:<action> ..."): save<N> load<N> disc<N> pause unpause reset stop
 *       start<N> (game N of the list) fit set<i>=<v> press<mask>.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <SDL2/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cdie.h"

static FILE *g_logf;

static void log_line(const char *line)
{
    fprintf(stderr, "%s\n", line);
    if (g_logf) {
        fprintf(g_logf, "%s\n", line);
        fflush(g_logf);
    }
}

static void notice(const char *line)
{
    fprintf(stderr, "*** %s\n", line);
}

static void SDLCALL audio_cb(void *ud, Uint8 *stream, int len)
{
    (void)ud;
    cdie_audio((int16_t *)stream, len / 4);
}

static void write_ppm(const char *path, const uint8_t *rgba, int w, int h)
{
    FILE *f = fopen(path, "wb");
    if (!f)
        return;
    fprintf(f, "P6\n%d %d\n255\n", w, h);
    for (int i = 0; i < w * h; i++)
        fwrite(rgba + (size_t)i * 4, 1, 3, f);
    fclose(f);
}

static int prepare_and_start(int idx)
{
    cdie_game g;
    if (cdie_lib_game(idx, &g) != 0) {
        fprintf(stderr, "no game %d\n", idx + 1);
        return -1;
    }
    fprintf(stderr, "[test] preparing %s (%s)\n", g.name, g.here ? "here" : "needs copying/unpacking");
    if (cdie_prepare(idx) != 0)
        return -1;
    cdie_job j;
    int last_pct = -1;
    while (cdie_prepare_poll(&j) == 0) {
        int pct = j.total ? (int)(j.done * 100 / j.total) : 0;
        if (pct / 25 != last_pct / 25) {
            fprintf(stderr, "[test] %s: %d%% (%s)\n", j.title, pct, j.phase);
            last_pct = pct;
        }
        SDL_Delay(20);
    }
    if (j.state != 1) {
        fprintf(stderr, "[test] not ready: state %d %s\n", j.state, j.why);
        return -1;
    }
    if (getenv("CDIE_TEST_CONTINUE"))
        cdie_continue_next(1);
    if (cdie_start() != 0) {
        fprintf(stderr, "[test] start failed: %s\n", cdie_last_error());
        return -1;
    }
    fprintf(stderr, "[test] running %s\n", cdie_game_name());
    return 0;
}

static void do_action(const char *a)
{
    char msg[300] = "";
    int n;
    if (sscanf(a, "save%d", &n) == 1) {
        int r = cdie_slot_save(n, msg, sizeof msg);
        fprintf(stderr, "[test] save %d -> %d: %s\n", n, r, msg);
    } else if (sscanf(a, "load%d", &n) == 1) {
        int r = cdie_slot_load(n, msg, sizeof msg);
        fprintf(stderr, "[test] load %d -> %d: %s\n", n, r, msg);
    } else if (sscanf(a, "disc%d", &n) == 1) {
        int r = cdie_disc_insert(n - 1, msg, sizeof msg);
        fprintf(stderr, "[test] disc %d of %d -> %d: %s (now %d)\n", n, cdie_disc_count(), r, msg, cdie_disc_current() + 1);
    } else if (!strcmp(a, "pause")) {
        cdie_pause(1);
        fprintf(stderr, "[test] paused\n");
    } else if (!strcmp(a, "unpause")) {
        cdie_pause(0);
        fprintf(stderr, "[test] going on\n");
    } else if (!strcmp(a, "reset")) {
        cdie_reset();
        fprintf(stderr, "[test] reset\n");
    } else if (!strcmp(a, "stop")) {
        cdie_stop();
        fprintf(stderr, "[test] stopped (running %d)\n", cdie_running());
    } else if (sscanf(a, "start%d", &n) == 1) {
        prepare_and_start(n - 1);
    } else if (!strcmp(a, "rw1") || !strcmp(a, "rw0")) {
        cdie_rewind(a[2] == '1');
        fprintf(stderr, "[test] rewind %s\n", a[2] == '1' ? "held" : "let go");
    } else if (!strcmp(a, "fit")) {
        cdie_fit_crop();
    } else if (!strncmp(a, "set", 3)) {
        int i, v;
        if (sscanf(a + 3, "%d=%d", &i, &v) == 2) {
            cdie_setting_set(i, v);
            cdie_setting s;
            cdie_setting_get(i, &s);
            fprintf(stderr, "[test] %s = %d\n", s.label, s.value);
        }
    }
}

int main(int argc, char **argv)
{
    if (argc < 4) {
        fprintf(stderr, "usage: %s <data dir> <core.so> --list | --game N | --file PATH [--frames N] [--headless] [--dump PREFIX]\n", argv[0]);
        return 2;
    }
    const char *data = argv[1], *core = argv[2], *file = NULL, *dump = NULL;
    int game = -1, list = 0, headless = 0, no_download = 0;
    long max_frames = -1;
    for (int i = 3; i < argc; i++) {
        if (!strcmp(argv[i], "--list")) list = 1;
        else if (!strcmp(argv[i], "--game") && i + 1 < argc) game = atoi(argv[++i]) - 1;
        else if (!strcmp(argv[i], "--file") && i + 1 < argc) file = argv[++i];
        else if (!strcmp(argv[i], "--frames") && i + 1 < argc) max_frames = atol(argv[++i]);
        else if (!strcmp(argv[i], "--headless")) headless = 1;
        else if (!strcmp(argv[i], "--dump") && i + 1 < argc) dump = argv[++i];
        else if (!strcmp(argv[i], "--no-download")) no_download = 1;
    }
    if (getenv("CDIE_TEST_LOG"))
        g_logf = fopen(getenv("CDIE_TEST_LOG"), "w");

    cdie_config cfg = {0};
    cfg.data_dir = data;
    cfg.core_path = core;
    cfg.log = log_line;
    cfg.notice = notice;
    cfg.no_download = no_download;
    if (cdie_init(&cfg) != 0) {
        fprintf(stderr, "engine: %s\n", cdie_last_error());
        return 1;
    }
    if (list) {
        /* the NAS is read in the background: give it a moment (CDIE_TEST_LIST_WAIT_MS) */
        const int wait = getenv("CDIE_TEST_LIST_WAIT_MS") ? atoi(getenv("CDIE_TEST_LIST_WAIT_MS")) : 0;
        for (int t = 0; t < wait; t += 50) {
            cdie_lib_update();
            SDL_Delay(50);
        }
        cdie_lib_update();
        char nas[300], cov[300];
        int st = cdie_lib_status(nas, sizeof nas, cov, sizeof cov);
        for (int i = 0; i < cdie_lib_count(); i++) {
            cdie_game g;
            cdie_lib_game(i, &g);
            printf("%3d  %c  %-40s  %-5s %s%s discs %d  cover %s\n", i + 1, g.letter, g.name,
                   g.source == CDIE_SRC_NAS ? "NAS" : g.source == CDIE_SRC_DRIVE ? g.drive : "local",
                   g.packed ? "7z " : "", g.here ? "here" : "away", g.discs, g.has_cover ? "yes" : "no");
        }
        printf("%d game(s); last played %d; NAS state %d '%s'; covers '%s'\n", cdie_lib_count(), cdie_lib_last() + 1, st, nas, cov);
        /* the cover pictures, as the title takes them for the wheel (CDIE_TEST_COVERS=<max side>) */
        if (getenv("CDIE_TEST_COVERS")) {
            cdie_cover_window(0, 8, atoi(getenv("CDIE_TEST_COVERS")));
            int got = 0;
            for (int t = 0; t < 100 && got < cdie_lib_count(); t++) {
                int idx, w, h;
                uint8_t *px;
                while (cdie_cover_take(&idx, &w, &h, &px)) {
                    printf("cover %d: %dx%d, first pixel %u,%u,%u,%u\n", idx + 1, w, h, px[0], px[1], px[2], px[3]);
                    cdie_free(px);
                    got++;
                    if (idx == 0 && getenv("CDIE_TEST_FORGET")) { /* let go of it: it should come back */
                        cdie_cover_forget(idx);
                        unsetenv("CDIE_TEST_FORGET");
                    }
                }
                SDL_Delay(20);
            }
        }
        cdie_shutdown();
        return 0;
    }

    if (SDL_Init((headless ? 0 : SDL_INIT_VIDEO | SDL_INIT_AUDIO) | SDL_INIT_EVENTS | SDL_INIT_GAMECONTROLLER) != 0) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    SDL_Window *win = NULL;
    SDL_Renderer *ren = NULL;
    SDL_Texture *tex[2] = {NULL, NULL}; /* [0] previous frame, [1] newest */
    unsigned tex_serial[2] = {0, 0};
    int tex_w = 0, tex_h = 0;
    SDL_AudioDeviceID adev = 0;
    if (!headless) {
        win = SDL_CreateWindow("cdie_sdl", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 960, 540, SDL_WINDOW_RESIZABLE);
        ren = win ? SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC) : NULL;
        if (!ren) {
            fprintf(stderr, "no renderer: %s\n", SDL_GetError());
            return 1;
        }
        SDL_AudioSpec want, have;
        SDL_zero(want);
        want.freq = 48000;
        want.format = AUDIO_S16SYS;
        want.channels = 2;
        want.samples = 512;
        want.callback = audio_cb;
        adev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
        if (adev)
            SDL_PauseAudioDevice(adev, 0);
    }
    SDL_GameController *pad = NULL;
    for (int i = 0; i < SDL_NumJoysticks() && !pad; i++)
        if (SDL_IsGameController(i))
            pad = SDL_GameControllerOpen(i);

    int ok = file ? cdie_start_file(file) : game >= 0 ? prepare_and_start(game) : -1;
    if (ok != 0) {
        fprintf(stderr, "nothing to run (%s)\n", cdie_last_error());
        cdie_shutdown();
        return 1;
    }

    /* headless: a pretend 60 Hz screen, and a pretend sound output that takes 512 frames every 10.67 ms */
    const double t0 = cdie_now_ms();
    double next_audio = t0;
    int16_t abuf[512 * 2];
    long refresh = 0;
    int running = 1;
    unsigned shown_serial = 0;
    long new_frames = 0;
    double blend_sum = 0;
    int blend_n = 0;
    const char *dumpat = getenv("CDIE_TEST_DUMPAT");
    while (running && (max_frames < 0 || refresh < max_frames)) {
        SDL_Event e;
        cdie_pad p = {0};
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT)
                running = 0;
            else if (e.type == SDL_KEYDOWN && !e.key.repeat) {
                char msg[200];
                switch (e.key.keysym.sym) {
                case SDLK_ESCAPE: running = 0; break;
                case SDLK_TAB: cdie_pause(!cdie_paused()); break;
                case SDLK_F5: cdie_slot_save(1, msg, sizeof msg); break;
                case SDLK_F7: cdie_slot_load(1, msg, sizeof msg); break;
                case SDLK_F3: cdie_disc_insert((cdie_disc_current() + 1) % cdie_disc_count(), msg, sizeof msg); break;
                case SDLK_F10: cdie_reset(); break;
                }
            }
        }
        if (!headless) {
            const Uint8 *k = SDL_GetKeyboardState(NULL);
            if (k[SDL_SCANCODE_UP]) p.buttons |= CDIE_PAD_UP;
            if (k[SDL_SCANCODE_DOWN]) p.buttons |= CDIE_PAD_DOWN;
            if (k[SDL_SCANCODE_LEFT]) p.buttons |= CDIE_PAD_LEFT;
            if (k[SDL_SCANCODE_RIGHT]) p.buttons |= CDIE_PAD_RIGHT;
            if (k[SDL_SCANCODE_Z]) p.buttons |= CDIE_PAD_CROSS;
            if (k[SDL_SCANCODE_X]) p.buttons |= CDIE_PAD_CIRCLE;
            if (k[SDL_SCANCODE_A]) p.buttons |= CDIE_PAD_SQUARE;
            if (k[SDL_SCANCODE_S]) p.buttons |= CDIE_PAD_TRIANGLE;
            if (k[SDL_SCANCODE_Q]) p.buttons |= CDIE_PAD_L1;
            if (k[SDL_SCANCODE_W]) p.buttons |= CDIE_PAD_R1;
            if (k[SDL_SCANCODE_RETURN]) p.buttons |= CDIE_PAD_OPTIONS;
        }
        if (pad) {
            static const struct { int b; uint32_t m; } map[] = {
                {SDL_CONTROLLER_BUTTON_A, CDIE_PAD_CROSS}, {SDL_CONTROLLER_BUTTON_B, CDIE_PAD_CIRCLE},
                {SDL_CONTROLLER_BUTTON_X, CDIE_PAD_SQUARE}, {SDL_CONTROLLER_BUTTON_Y, CDIE_PAD_TRIANGLE},
                {SDL_CONTROLLER_BUTTON_LEFTSHOULDER, CDIE_PAD_L1}, {SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, CDIE_PAD_R1},
                {SDL_CONTROLLER_BUTTON_LEFTSTICK, CDIE_PAD_L3}, {SDL_CONTROLLER_BUTTON_RIGHTSTICK, CDIE_PAD_R3},
                {SDL_CONTROLLER_BUTTON_START, CDIE_PAD_OPTIONS}, {SDL_CONTROLLER_BUTTON_BACK, CDIE_PAD_TOUCH},
                {SDL_CONTROLLER_BUTTON_DPAD_UP, CDIE_PAD_UP}, {SDL_CONTROLLER_BUTTON_DPAD_DOWN, CDIE_PAD_DOWN},
                {SDL_CONTROLLER_BUTTON_DPAD_LEFT, CDIE_PAD_LEFT}, {SDL_CONTROLLER_BUTTON_DPAD_RIGHT, CDIE_PAD_RIGHT},
            };
            for (size_t i = 0; i < sizeof map / sizeof map[0]; i++)
                if (SDL_GameControllerGetButton(pad, (SDL_GameControllerButton)map[i].b))
                    p.buttons |= map[i].m;
            p.lx = SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_LEFTX);
            p.ly = SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_LEFTY);
            p.rx = SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_RIGHTX);
            p.ry = SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_RIGHTY);
            p.l2 = (uint8_t)(SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_TRIGGERLEFT) >> 7);
            p.r2 = (uint8_t)(SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) >> 7);
        }
        { /* test: CDIE_TEST_PAD="<refresh>:<button mask> ..." holds those buttons from that refresh on */
            static uint32_t held;
            const char *spec = getenv("CDIE_TEST_PAD");
            for (const char *c = spec; c && *c;) {
                long f;
                unsigned m;
                int nn = 0;
                if (sscanf(c, "%ld:%u%n", &f, &m, &nn) == 2 && nn > 0) {
                    if (f == refresh) held = m;
                    c += nn;
                } else {
                    c++;
                }
            }
            p.buttons |= held;
        }
        { /* test: CDIE_TEST_STICK="<refresh>:<lx>,<ly> ..." holds the left stick there from that refresh on */
            static int16_t sx, sy;
            const char *spec = getenv("CDIE_TEST_STICK");
            for (const char *c = spec; c && *c;) {
                long f;
                int x, y, nn = 0;
                if (sscanf(c, "%ld:%d,%d%n", &f, &x, &y, &nn) == 3 && nn > 0) {
                    if (f == refresh) { sx = (int16_t)x; sy = (int16_t)y; }
                    c += nn;
                } else {
                    c++;
                }
            }
            if (sx || sy) { p.lx = sx; p.ly = sy; }
        }
        cdie_input(&p);
        { /* test actions at given refreshes */
            const char *spec = getenv("CDIE_TEST_ACTIONS");
            for (const char *c = spec; c && *c;) {
                long f;
                char act[64];
                int nn = 0;
                if (sscanf(c, "%ld:%63s%n", &f, act, &nn) == 2 && nn > 0) {
                    if (f == refresh)
                        do_action(act);
                    c += nn;
                } else {
                    c++;
                }
            }
        }

        /* the pictures: upload a frame only when it is new, keep the previous one as the other texture */
        cdie_image a, b;
        const double now = cdie_now_ms();
        const float w = cdie_video_blend(now);
        blend_sum += w;
        blend_n++;
        int nimg = cdie_video_lock(&a, &b);
        if (nimg >= 1 && a.serial != shown_serial) {
            new_frames++;
            shown_serial = a.serial;
            if (!headless) {
                if (a.w != tex_w || a.h != tex_h) {
                    for (int i = 0; i < 2; i++) {
                        if (tex[i])
                            SDL_DestroyTexture(tex[i]);
                        tex[i] = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STREAMING, a.w, a.h);
                        SDL_SetTextureBlendMode(tex[i], SDL_BLENDMODE_BLEND);
                        tex_serial[i] = 0;
                    }
                    tex_w = a.w;
                    tex_h = a.h;
                }
                if (nimg == 2 && tex_serial[1] == b.serial) { /* the old newest becomes the previous one */
                    SDL_Texture *t = tex[0]; tex[0] = tex[1]; tex[1] = t;
                    tex_serial[0] = tex_serial[1];
                } else if (nimg == 2 && b.w == a.w && b.h == a.h) {
                    SDL_UpdateTexture(tex[0], NULL, b.rgba, b.pitch);
                    tex_serial[0] = b.serial;
                }
                SDL_UpdateTexture(tex[1], NULL, a.rgba, a.pitch);
                tex_serial[1] = a.serial;
            }
            if (dump && dumpat) { /* CDIE_TEST_DUMPAT="<refresh> ..." */
                for (const char *c = dumpat; *c;) {
                    char *end;
                    long f = strtol(c, &end, 10);
                    if (end == c) break;
                    if (f == refresh) {
                        char path[600];
                        snprintf(path, sizeof path, "%s-%ld.ppm", dump, refresh);
                        write_ppm(path, a.rgba, a.w, a.h);
                        fprintf(stderr, "[test] wrote %s (%dx%d, serial %u)\n", path, a.w, a.h, a.serial);
                    }
                    c = end;
                    while (*c == ' ' || *c == ',') c++;
                }
            }
        }
        cdie_video_unlock();

        if (!headless) {
            int ow, oh;
            SDL_GetRendererOutputSize(ren, &ow, &oh);
            SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);
            SDL_RenderClear(ren);
            if (tex[1]) {
                cdie_view v;
                cdie_video_view(tex_w, tex_h, &v);
                SDL_Rect src = {v.x, v.y, v.w, v.h}, dst;
                if (v.wide) {
                    dst = (SDL_Rect){0, 0, ow, oh};
                } else {
                    int dw = oh * 4 / 3, dh = oh;
                    if (dw > ow) { dw = ow; dh = ow * 3 / 4; }
                    dst = (SDL_Rect){(ow - dw) / 2, (oh - dh) / 2, dw, dh};
                }
                if (tex_serial[0] && w < 1.0f) {
                    SDL_SetTextureAlphaMod(tex[0], 255);
                    SDL_RenderCopy(ren, tex[0], &src, &dst);
                }
                SDL_SetTextureAlphaMod(tex[1], (Uint8)(w * 255.0f + 0.5f));
                SDL_RenderCopy(ren, tex[1], &src, &dst);
            }
            SDL_RenderPresent(ren); /* waits for the display (vsync) */
        } else {
            /* pretend sound output and screen */
            double tnow = cdie_now_ms();
            while (tnow >= next_audio) {
                cdie_audio(abuf, 512);
                next_audio += 512.0 * 1000.0 / 48000.0;
            }
            const double next_refresh = t0 + (double)(refresh + 1) * 1000.0 / 60.0;
            tnow = cdie_now_ms();
            if (next_refresh > tnow)
                SDL_Delay((Uint32)(next_refresh - tnow));
        }
        cdie_lib_update();
        refresh++;
    }
    char st[200];
    cdie_stats(st, sizeof st);
    fprintf(stderr, "[test] %ld refreshes, %ld new frames, mean blend %.3f; %s\n", refresh, new_frames,
            blend_n ? blend_sum / blend_n : 0.0, st);
    if (adev)
        SDL_CloseAudioDevice(adev);
    cdie_shutdown();
    SDL_Quit();
    return 0;
}
