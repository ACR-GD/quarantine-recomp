// Game controller support (SDL3 gamepad API: Xbox, PlayStation, Switch Pro, 8BitDo, ...).
//
// The game only knows the keyboard (its joystick support needs a game-port and calibration), so pad
// inputs become key presses. The same button means different things in menus and while driving; the
// game's per-frame driving key handler (0x12cb64, hooked in recomp.py) tells us which one applies.
// A key is chosen when an input becomes active and released with the same key, so switching context
// while holding a button never leaves keys stuck or fires something unintended.
//
//                 menus            driving
//   left stick    arrows           steer / accelerate / brake
//   d-pad         arrows           steer / accelerate / brake
//   RT / LT       -                accelerate (Up) / brake-reverse (Down)
//   A             Enter            fire (Ctrl)
//   B             Esc              nitro (Tab)
//   X             -                cycle weapon (Alt)
//   Y             Space            special item (Space)
//   LB / RB       -                E / Z
//   Start         Enter            menu (Esc)
//   Back/Select   Esc              map (M)
//   L3            -                horn (Enter)
//   R3            -                cycle camera (F1/F2/F3)
//
// QUAR_PADTEST="ms:input:value,..." attaches a virtual gamepad and drives it from a script, for testing
// without a controller (inputs: a b x y back start lb rb l3 r3 up down left right lx ly lt rt).
#include "rt.h"
#include "sys.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef USE_SDL
#include <SDL3/SDL.h>
#endif
extern int rt_verbose;
void sys_key(uint8_t sc, int down);

static double last_game_frame = -1;
void pad_game_frame(void) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    last_game_frame = t.tv_sec + t.tv_nsec * 1e-9;
}

#ifdef USE_SDL
enum { K_BTN, K_AXN, K_AXP };     // button, axis below -threshold, axis above +threshold
#define CAMERA 0xff               // special: cycle F1/F2/F3
typedef struct { const char *name; int kind, id; float thr; uint8_t menu, game; } map_t;
static const map_t maps[] = {
    {"a",     K_BTN, SDL_GAMEPAD_BUTTON_SOUTH,          0, 0x1c, 0x1d},
    {"b",     K_BTN, SDL_GAMEPAD_BUTTON_EAST,           0, 0x01, 0x0f},
    {"x",     K_BTN, SDL_GAMEPAD_BUTTON_WEST,           0, 0,    0x38},
    {"y",     K_BTN, SDL_GAMEPAD_BUTTON_NORTH,          0, 0x39, 0x39},
    {"lb",    K_BTN, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER,  0, 0,    0x12},
    {"rb",    K_BTN, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, 0, 0,    0x2c},
    {"start", K_BTN, SDL_GAMEPAD_BUTTON_START,          0, 0x1c, 0x01},
    {"back",  K_BTN, SDL_GAMEPAD_BUTTON_BACK,           0, 0x01, 0x32},
    {"l3",    K_BTN, SDL_GAMEPAD_BUTTON_LEFT_STICK,     0, 0,    0x1c},
    {"r3",    K_BTN, SDL_GAMEPAD_BUTTON_RIGHT_STICK,    0, 0,    CAMERA},
    {"up",    K_BTN, SDL_GAMEPAD_BUTTON_DPAD_UP,        0, 0x48, 0x48},
    {"down",  K_BTN, SDL_GAMEPAD_BUTTON_DPAD_DOWN,      0, 0x50, 0x50},
    {"left",  K_BTN, SDL_GAMEPAD_BUTTON_DPAD_LEFT,      0, 0x4b, 0x4b},
    {"right", K_BTN, SDL_GAMEPAD_BUTTON_DPAD_RIGHT,     0, 0x4d, 0x4d},
    {"lx-",   K_AXN, SDL_GAMEPAD_AXIS_LEFTX,       0.40f, 0x4b, 0x4b},
    {"lx+",   K_AXP, SDL_GAMEPAD_AXIS_LEFTX,       0.40f, 0x4d, 0x4d},
    {"ly-",   K_AXN, SDL_GAMEPAD_AXIS_LEFTY,       0.50f, 0x48, 0x48},
    {"ly+",   K_AXP, SDL_GAMEPAD_AXIS_LEFTY,       0.50f, 0x50, 0x50},
    {"rt",    K_AXP, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, 0.25f, 0,  0x48},
    {"lt",    K_AXP, SDL_GAMEPAD_AXIS_LEFT_TRIGGER,  0.25f, 0,  0x50},
};
#define NMAPS (int)(sizeof maps / sizeof maps[0])

static SDL_Gamepad *pad;
static int active[NMAPS];         // input currently active
static uint8_t latched[NMAPS];    // key chosen when it became active
static uint8_t held[128];         // how many active inputs hold each key
static int camera;
static double next_scan;

// virtual pad for scripted tests
static SDL_Joystick *vjoy;
static struct { double t; char in[8]; float v; } script[256];
static int nscript, spos;
static double t0;

static double now_s(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec * 1e-9; }

static void press(uint8_t sc, int down) {
    if (!sc) return;
    if (down) { if (held[sc]++ == 0) sys_key(sc, 1); }
    else if (held[sc] && --held[sc] == 0) sys_key(sc, 0);
}

static void release_all(void) {
    for (int i = 0; i < NMAPS; i++) if (active[i]) { press(latched[i], 0); active[i] = 0; }
}

void pad_init(void) {
    if (!SDL_InitSubSystem(SDL_INIT_GAMEPAD)) { fprintf(stderr, "gamepad: %s\n", SDL_GetError()); return; }
    const char *ps = getenv("QUAR_PADTEST");
    if (ps) {
        SDL_VirtualJoystickDesc d; SDL_INIT_INTERFACE(&d);
        d.type = SDL_JOYSTICK_TYPE_GAMEPAD; d.naxes = SDL_GAMEPAD_AXIS_COUNT; d.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
        d.name = "Quarantine test pad";
        SDL_JoystickID id = SDL_AttachVirtualJoystick(&d);
        vjoy = id ? SDL_OpenJoystick(id) : 0;
        if (!vjoy) fprintf(stderr, "virtual pad: %s\n", SDL_GetError());
        char *s = strdup(ps), *tok;
        for (tok = strtok(s, ","); tok && nscript < 256; tok = strtok(0, ",")) {
            unsigned ms; char in[8]; float v;
            if (sscanf(tok, "%u:%7[^:]:%f", &ms, in, &v) == 3) {
                script[nscript].t = ms / 1000.0; strcpy(script[nscript].in, in); script[nscript].v = v; nscript++;
            }
        }
        free(s);
    }
    t0 = now_s();
}

static void run_script(double t) {
    static const char *btn[] = {"a", "b", "x", "y", "back", 0, "start", "l3", "r3", "lb", "rb", "up", "down", "left", "right"};
    while (vjoy && spos < nscript && t - t0 >= script[spos].t) {
        const char *in = script[spos].in; float v = script[spos].v;
        if (!strcmp(in, "lx")) SDL_SetJoystickVirtualAxis(vjoy, SDL_GAMEPAD_AXIS_LEFTX, (Sint16)(v * 32767));
        else if (!strcmp(in, "ly")) SDL_SetJoystickVirtualAxis(vjoy, SDL_GAMEPAD_AXIS_LEFTY, (Sint16)(v * 32767));
        else if (!strcmp(in, "lt")) SDL_SetJoystickVirtualAxis(vjoy, SDL_GAMEPAD_AXIS_LEFT_TRIGGER, (Sint16)(v * 32767));
        else if (!strcmp(in, "rt")) SDL_SetJoystickVirtualAxis(vjoy, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, (Sint16)(v * 32767));
        else for (int b = 0; b < (int)(sizeof btn / sizeof btn[0]); b++)
            if (btn[b] && !strcmp(btn[b], in)) SDL_SetJoystickVirtualButton(vjoy, b, v != 0);
        spos++;
    }
}

// Called once per presented frame (70 Hz).
void pad_poll(void) {
    double t = now_s();
    run_script(t);
    SDL_UpdateGamepads();
    if (pad && !SDL_GamepadConnected(pad)) {
        if (rt_verbose) fprintf(stderr, "pad: disconnected\n");
        release_all(); SDL_CloseGamepad(pad); pad = 0;
    }
    if (!pad && t >= next_scan) {
        next_scan = t + 1.0;
        int n = 0; SDL_JoystickID *ids = SDL_GetGamepads(&n);
        if (ids && n > 0) {
            pad = SDL_OpenGamepad(ids[0]);
            if (pad) fprintf(stderr, "[controller: %s]\n", SDL_GetGamepadName(pad));
        }
        SDL_free(ids);
    }
    if (!pad) return;
    int driving = t - last_game_frame < 0.25;
    for (int i = 0; i < NMAPS; i++) {
        const map_t *m = &maps[i];
        int on;
        if (m->kind == K_BTN) on = SDL_GetGamepadButton(pad, (SDL_GamepadButton)m->id);
        else {
            float v = SDL_GetGamepadAxis(pad, (SDL_GamepadAxis)m->id) / 32767.0f;
            // hysteresis: a slightly lower threshold to stay active
            float thr = active[i] ? m->thr * 0.8f : m->thr;
            on = m->kind == K_AXN ? v < -thr : v > thr;
        }
        if (on == active[i]) continue;
        active[i] = on;
        if (on) {
            uint8_t sc = driving ? m->game : m->menu;
            if (sc == CAMERA) { sc = 0x3b + camera; camera = (camera + 1) % 3; }
            latched[i] = sc;
            if (rt_verbose) fprintf(stderr, "pad: %.2fs %s -> key %02x (%s)\n", t - t0, m->name, sc, driving ? "driving" : "menu");
        }
        press(latched[i], on);
    }
}
#else
void pad_init(void) {}
void pad_poll(void) {}
#endif
