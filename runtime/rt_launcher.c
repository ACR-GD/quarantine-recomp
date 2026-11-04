// Launcher: pick the disc image, change settings, start the game. Mouse, keyboard and controller.
#ifdef USE_SDL
#include "disc.h"
#include <SDL3/SDL.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

#define W 640
#define H 480

typedef struct { Uint8 r, g, b, a; } col;
static const col BG = {12, 10, 8, 255}, AMBER = {242, 177, 52, 255}, DIM = {150, 140, 120, 255},
                 TEXT = {225, 220, 210, 255}, GREEN = {120, 210, 110, 255}, RED = {235, 95, 80, 255},
                 GREY = {90, 85, 78, 255};

static SDL_Window *win;
static SDL_Renderer *ren;

// ---------------------------------------------------------------- drawing
static void text(float x, float y, float scale, col c, const char *s) {
    SDL_SetRenderScale(ren, scale, scale);
    SDL_SetRenderDrawColor(ren, c.r, c.g, c.b, c.a);
    SDL_RenderDebugText(ren, x / scale, y / scale, s);
    SDL_SetRenderScale(ren, 1, 1);
}
static float text_w(const char *s, float scale) { return (float)strlen(s) * 8 * scale; }
static void text_c(float y, float scale, col c, const char *s) { text((W - text_w(s, scale)) / 2, y, scale, c, s); }
static void rect(float x, float y, float w, float h, col c) {
    SDL_SetRenderDrawColor(ren, c.r, c.g, c.b, c.a);
    SDL_FRect r = {x, y, w, h}; SDL_RenderFillRect(ren, &r);
}
// Word-wrap s into lines of at most n characters, centered.
static void text_wrap_c(float y, float scale, col c, const char *s, int n) {
    char line[128]; const char *p = s;
    while (*p) {
        int len = (int)strlen(p);
        if (len > n) { len = n; while (len > 0 && p[len] != ' ') len--; if (!len) len = n; }
        snprintf(line, sizeof line, "%.*s", len, p);
        text_c(y, scale, c, line);
        y += 10 * scale; p += len; while (*p == ' ') p++;
    }
}
static void background(void) {
    SDL_SetRenderDrawColor(ren, BG.r, BG.g, BG.b, 255);
    SDL_RenderClear(ren);
    SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);
    for (int i = 0; i < 6; i++) rect(0, 118 + i * 2, W, 1, (col){242, 177, 52, (Uint8)(60 - i * 10)});
    text_c(40, 6, AMBER, "QUARANTINE");
    text_c(98, 1, DIM, "1994 DOS game - native recompilation");
}

// ---------------------------------------------------------------- state
enum { PAGE_MAIN, PAGE_SETTINGS };
static int page, sel, quit, start;
static char status[512];
static int status_ok;
static float mx = -1, my = -1;

static SDL_Mutex *dlg_mx;
static char dlg_path[1024];
static int dlg_ready;

static void check_disc(void) {
    char err[512];
    if (disc_open(cfg.disc, err, sizeof err) == 0) {
        status_ok = 1;
        if (disc_kind() == SRC_FOLDER) snprintf(status, sizeof status, "Extracted game folder - %s",
                                                disc_has_music() ? "music found" : "no music tracks");
        else snprintf(status, sizeof status, "Quarantine CD found - %s",
                      disc_has_music() ? "with CD music" : "no music tracks in this image");
    } else { status_ok = 0; snprintf(status, sizeof status, "%s", err); }
}

static void SDLCALL dialog_cb(void *u, const char *const *files, int filter) {
    (void)u; (void)filter;
    if (!files || !files[0]) return;                // error or cancelled
    SDL_LockMutex(dlg_mx);
    snprintf(dlg_path, sizeof dlg_path, "%s", files[0]); dlg_ready = 1;
    SDL_UnlockMutex(dlg_mx);
}
static void choose_disc(void) {
    static const SDL_DialogFileFilter filters[] = {
        {"CD images (.cue, .nrg, .iso)", "cue;nrg;iso;bin"}, {"All files", "*"} };
    char dir[1024] = "";
    if (cfg.disc[0]) { snprintf(dir, sizeof dir, "%s", cfg.disc); char *s = strrchr(dir, '/'), *b = strrchr(dir, '\\'); if (b > s) s = b; if (s) *s = 0; }
    SDL_ShowOpenFileDialog(dialog_cb, 0, win, filters, 2, dir[0] ? dir : 0, false);
}
static void use_path(const char *p) {
    snprintf(cfg.disc, sizeof cfg.disc, "%s", p);
    check_disc();
    if (status_ok) cfg_save();
}

// ---------------------------------------------------------------- items
typedef struct { const char *label; int *val; int lo, hi, step; const char *const *names; } setting;
static const char *const n_display[] = {"Window", "Fullscreen"}, *const n_scale[] = {"Sharp", "Smooth", "Crisp", "CRT", "Scale4x"},
                  *const n_aspect[] = {"4:3 (CRT)", "Square pixels"},
                  *const n_limit[] = {"Off", "70 Hz"}, *const n_cp[] = {"Skip", "Show"},
                  *const n_launch[] = {"Skip", "Show"};
static setting settings[] = {
    {"Display", &cfg.fullscreen, 0, 1, 1, n_display},
    {"Filter", &cfg.filter, 0, FILTER_COUNT - 1, 1, n_scale},
    {"Aspect ratio", &cfg.square, 0, 1, 1, n_aspect},
    {"Music volume", &cfg.music, 0, 100, 10, 0},
    {"Effects volume", &cfg.sfx, 0, 100, 10, 0},
    {"Frame limit", &cfg.framecap, 0, 1, 1, n_limit},
    {"Copy protection", &cfg.copyprot, 0, 1, 1, n_cp},
    {"Launcher at startup", &cfg.launcher, 0, 1, 1, n_launch},
};
#define NSET (int)(sizeof settings / sizeof settings[0])
static int nitems(void) { return page == PAGE_MAIN ? 4 : NSET + 1; }
static float item_y(int i) { return page == PAGE_MAIN ? 170 + i * 44 : 150 + i * 30; }
static float item_h(void) { return page == PAGE_MAIN ? 40 : 28; }

static void apply_setting(int i) {
    if (settings[i].val == &cfg.fullscreen) SDL_SetWindowFullscreen(win, cfg.fullscreen);
    cfg_save();
}
static void adjust(int i, int dir) {          // dir: +1/-1, 0 = cycle
    setting *s = &settings[i];
    int v = *s->val + (dir ? dir : 1) * s->step;
    if (dir == 0 && v > s->hi) v = s->lo;
    *s->val = v < s->lo ? s->lo : v > s->hi ? s->hi : v;
    apply_setting(i);
}

static void activate(int i) {
    if (page == PAGE_MAIN) {
        if (i == 0) { if (status_ok) start = 1; else choose_disc(); }
        else if (i == 1) choose_disc();
        else if (i == 2) { page = PAGE_SETTINGS; sel = 0; }
        else quit = 1;
    } else {
        if (i == NSET) { page = PAGE_MAIN; sel = 2; }
        else adjust(i, 0);
    }
}
static void back(void) { if (page == PAGE_SETTINGS) { page = PAGE_MAIN; sel = 2; } else quit = 1; }
static void move(int d) { int n = nitems(); sel = (sel + d + n) % n; }

static int nlt;
static void test_step(void);
static void draw(void) {
    background();
    SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);
    if (page == PAGE_MAIN) {
        const char *base = "(none)";
        if (cfg.disc[0]) { base = cfg.disc; for (const char *c = cfg.disc; *c; c++) if (*c == '/' || *c == '\\') base = c + 1; }
        char disc[200]; snprintf(disc, sizeof disc, "Disc image: %.40s", base);
        const char *labels[] = {"Start game", disc, "Settings", "Quit"};
        for (int i = 0; i < 4; i++) {
            float y = item_y(i);
            if (i == sel) rect(60, y - 8, W - 120, item_h() - 4, (col){242, 177, 52, 40});
            col c = i == 0 && !status_ok ? GREY : i == sel ? AMBER : TEXT;
            text_c(y, 2, c, labels[i]);
        }
        text_wrap_c(352, 1, status_ok ? GREEN : RED, status, 70);
        if (cfg.disc[0]) {
            char p[100]; int l = (int)strlen(cfg.disc);
            snprintf(p, sizeof p, l > 76 ? "...%s" : "%s", l > 76 ? cfg.disc + l - 73 : cfg.disc);
            text_c(380, 1, GREY, p);
        }
        if (!status_ok) text_wrap_c(404, 1, DIM, "Choose a rip of the Quarantine CD: a .cue sheet (with its .bin), a Nero .nrg, or an .iso (no music). You can also drop it on this window.", 72);
    } else {
        text_c(130, 1, DIM, "SETTINGS");
        for (int i = 0; i <= NSET; i++) {
            float y = item_y(i) + 14;
            if (i == sel) rect(60, y - 7, W - 120, item_h() - 2, (col){242, 177, 52, 40});
            col c = i == sel ? AMBER : TEXT;
            if (i == NSET) { text_c(y + 6, 2, c, "Back"); continue; }
            setting *s = &settings[i];
            char v[64];
            if (s->names) snprintf(v, sizeof v, "%s", s->names[*s->val]); else snprintf(v, sizeof v, "%d%%", *s->val);
            text(90, y, 1.5f, c, s->label);
            char vv[80]; snprintf(vv, sizeof vv, "< %s >", v);
            text(W - 90 - text_w(vv, 1.5f), y, 1.5f, c, vv);
        }
    }
    text_c(452, 1, GREY, "Arrows/D-pad move   Enter/A select   Esc/B back   Left/Right change");
    text(W - 8 - text_w("v" QUAR_VERSION, 1), 468, 1, DIM, "v" QUAR_VERSION);
    if (nlt) test_step();
    SDL_RenderPresent(ren);
}

// ---------------------------------------------------------------- install progress
static void progress(int done, int total, void *u) {
    (void)u;
    if (done % 8 && done != total) return;
    SDL_PumpEvents();
    background();
    text_c(220, 2, TEXT, "Installing game files");
    rect(120, 270, 400, 14, (col){60, 50, 35, 255});
    rect(120, 270, total ? 400.0f * done / total : 0, 14, AMBER);
    char s[64]; snprintf(s, sizeof s, "%d / %d", done, total);
    text_c(300, 1, DIM, s);
    SDL_RenderPresent(ren);
}

// QUAR_LAUNCHER_TEST="ms:action,..." for automated checks. Actions: up down left right enter esc,
// shot=<file.bmp> (save what is on screen), path=<disc image> (as if chosen in the file dialog).
static struct { Uint64 t; char a[600]; } lt[64];
static int plt;
static Uint64 lt0;
static void key(SDL_Keycode k) { SDL_Event e = {0}; e.type = SDL_EVENT_KEY_DOWN; e.key.key = k; SDL_PushEvent(&e); }
static void test_step(void) {
    while (plt < nlt && SDL_GetTicks() - lt0 >= lt[plt].t) {
        const char *a = lt[plt++].a;
        if (!strcmp(a, "up")) key(SDLK_UP); else if (!strcmp(a, "down")) key(SDLK_DOWN);
        else if (!strcmp(a, "left")) key(SDLK_LEFT); else if (!strcmp(a, "right")) key(SDLK_RIGHT);
        else if (!strcmp(a, "enter")) key(SDLK_RETURN); else if (!strcmp(a, "esc")) key(SDLK_ESCAPE);
        else if (!strncmp(a, "path=", 5)) { SDL_LockMutex(dlg_mx); snprintf(dlg_path, sizeof dlg_path, "%s", a + 5); dlg_ready = 1; SDL_UnlockMutex(dlg_mx); }
        else if (!strncmp(a, "shot=", 5)) {
            SDL_Surface *sf = SDL_RenderReadPixels(ren, NULL);
            if (sf) { SDL_SaveBMP(sf, a + 5); SDL_DestroySurface(sf); }
        }
    }
}

int launcher_run(SDL_Window *w, SDL_Renderer *r, const char *initial_error) {
    if (getenv("QUAR_LAUNCHER_TEST") && !nlt) {
        char *s = SDL_strdup(getenv("QUAR_LAUNCHER_TEST")), *save = 0;
        for (char *tok = SDL_strtok_r(s, ",", &save); tok && nlt < 64; tok = SDL_strtok_r(0, ",", &save)) {
            char *c = strchr(tok, ':'); if (!c) continue;
            *c = 0; lt[nlt].t = SDL_strtoull(tok, 0, 10); snprintf(lt[nlt].a, sizeof lt[nlt].a, "%s", c + 1); nlt++;
        }
        SDL_free(s); lt0 = SDL_GetTicks();
    }
    win = w; ren = r;
    if (!dlg_mx) dlg_mx = SDL_CreateMutex();
    SDL_SetRenderLogicalPresentation(ren, W, H, SDL_LOGICAL_PRESENTATION_LETTERBOX);
    SDL_InitSubSystem(SDL_INIT_GAMEPAD);
    page = PAGE_MAIN; sel = 0; quit = start = 0;
    check_disc();
    if (!status_ok && initial_error && *initial_error && cfg.disc[0]) snprintf(status, sizeof status, "%s", initial_error);
    if (!status_ok) sel = 1;
    int axis_held = 0;
    while (!quit) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            SDL_ConvertEventToRenderCoordinates(ren, &e);
            switch (e.type) {
            case SDL_EVENT_QUIT: quit = 1; break;
            case SDL_EVENT_DROP_FILE: if (e.drop.data) use_path(e.drop.data); break;
            case SDL_EVENT_KEY_DOWN:
                switch (e.key.key) {
                case SDLK_UP: move(-1); break;
                case SDLK_DOWN: move(1); break;
                case SDLK_LEFT: if (page == PAGE_SETTINGS && sel < NSET) adjust(sel, -1); break;
                case SDLK_RIGHT: if (page == PAGE_SETTINGS && sel < NSET) adjust(sel, 1); break;
                case SDLK_RETURN: case SDLK_KP_ENTER: case SDLK_SPACE: activate(sel); break;
                case SDLK_ESCAPE: case SDLK_BACKSPACE: back(); break;
                case SDLK_F11: cfg.fullscreen ^= 1; SDL_SetWindowFullscreen(win, cfg.fullscreen); cfg_save(); break;
                }
                break;
            case SDL_EVENT_GAMEPAD_ADDED: SDL_OpenGamepad(e.gdevice.which); break;
            case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
                switch (e.gbutton.button) {
                case SDL_GAMEPAD_BUTTON_DPAD_UP: move(-1); break;
                case SDL_GAMEPAD_BUTTON_DPAD_DOWN: move(1); break;
                case SDL_GAMEPAD_BUTTON_DPAD_LEFT: if (page == PAGE_SETTINGS && sel < NSET) adjust(sel, -1); break;
                case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: if (page == PAGE_SETTINGS && sel < NSET) adjust(sel, 1); break;
                case SDL_GAMEPAD_BUTTON_SOUTH: case SDL_GAMEPAD_BUTTON_START: activate(sel); break;
                case SDL_GAMEPAD_BUTTON_EAST: case SDL_GAMEPAD_BUTTON_BACK: back(); break;
                }
                break;
            case SDL_EVENT_GAMEPAD_AXIS_MOTION:
                if (e.gaxis.axis == SDL_GAMEPAD_AXIS_LEFTY || e.gaxis.axis == SDL_GAMEPAD_AXIS_LEFTX) {
                    int v = e.gaxis.value, d = v > 20000 ? 1 : v < -20000 ? -1 : 0;
                    if (d && !axis_held) {
                        if (e.gaxis.axis == SDL_GAMEPAD_AXIS_LEFTY) move(d);
                        else if (page == PAGE_SETTINGS && sel < NSET) adjust(sel, d);
                    }
                    axis_held = d != 0;
                }
                break;
            case SDL_EVENT_MOUSE_MOTION:
                mx = e.motion.x; my = e.motion.y;
                for (int i = 0; i < nitems(); i++) {
                    float y = page == PAGE_MAIN ? item_y(i) - 8 : item_y(i) + 14 - 7;
                    if (my >= y && my < y + item_h() && mx >= 60 && mx < W - 60) sel = i;
                }
                break;
            case SDL_EVENT_MOUSE_BUTTON_DOWN:
                if (e.button.button == SDL_BUTTON_LEFT) {
                    float y = page == PAGE_MAIN ? item_y(sel) - 8 : item_y(sel) + 14 - 7;
                    if (e.button.y >= y && e.button.y < y + item_h() && e.button.x >= 60 && e.button.x < W - 60) {
                        if (page == PAGE_SETTINGS && sel < NSET && settings[sel].names == 0)
                            adjust(sel, e.button.x < W / 2 ? -1 : 1);       // volumes: left half lowers
                        else activate(sel);
                    }
                }
                break;
            case SDL_EVENT_MOUSE_WHEEL:
                if (page == PAGE_SETTINGS && sel < NSET) adjust(sel, e.wheel.y > 0 ? 1 : -1);
                break;
            }
        }
        SDL_LockMutex(dlg_mx);
        if (dlg_ready) { dlg_ready = 0; use_path(dlg_path); if (status_ok) sel = 0; }
        SDL_UnlockMutex(dlg_mx);
        if (start) {
            char dest[1100], err[512] = "";
            snprintf(dest, sizeof dest, "%sdata", cfg_dir());
            if (disc_install(dest, progress, 0, err, sizeof err) == 0) { cfg_save(); return 1; }
            snprintf(status, sizeof status, "Install failed: %s", err); status_ok = 0; start = 0;
        }
        draw();
        SDL_Delay(16);
    }
    return 0;
}
#endif
