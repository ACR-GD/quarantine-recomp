// User configuration: <pref dir>/config.ini, plain key=value lines.
// The pref dir is SDL's per-user app folder, e.g. ~/Library/Application Support/quarantine-recomp/Quarantine/
// on macOS, ~/.local/share/quarantine-recomp/Quarantine/ on Linux, %APPDATA%\quarantine-recomp\Quarantine\ on Windows.
#include "disc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifdef USE_SDL
#include <SDL3/SDL.h>
#endif

config_t cfg = { .disc = "", .fullscreen = 0, .filter = FILTER_SHARP, .square = 0, .music = 100, .sfx = 100,
                 .framecap = 1, .copyprot = 0, .launcher = 1 };

const char *cfg_dir(void) {
    static char dir[1024];
    if (!dir[0]) {
        if (getenv("QUAR_CONFIG_DIR")) snprintf(dir, sizeof dir, "%s/", getenv("QUAR_CONFIG_DIR"));
#ifdef USE_SDL
        else { char *p = SDL_GetPrefPath("quarantine-recomp", "Quarantine"); if (p) { snprintf(dir, sizeof dir, "%s", p); SDL_free(p); } }
#endif
        if (!dir[0]) snprintf(dir, sizeof dir, "./");
        mkdir(dir, 0755);
    }
    return dir;
}

static struct { const char *key; int *val; int lo, hi; } ints[] = {
    {"fullscreen", &cfg.fullscreen, 0, 1}, {"filter", &cfg.filter, 0, FILTER_COUNT - 1}, {"square_pixels", &cfg.square, 0, 1},
    {"music_volume", &cfg.music, 0, 100}, {"effects_volume", &cfg.sfx, 0, 100}, {"frame_limit", &cfg.framecap, 0, 1},
    {"copy_protection", &cfg.copyprot, 0, 1}, {"show_launcher", &cfg.launcher, 0, 1},
};

void cfg_load(void) {
    char p[1100]; snprintf(p, sizeof p, "%sconfig.ini", cfg_dir());
    FILE *f = fopen(p, "r"); if (!f) return;
    char line[1200];
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        char *eq = strchr(line, '='); if (!eq || line[0] == '#') continue;
        *eq = 0; const char *k = line, *v = eq + 1;
        if (!strcmp(k, "smooth") && atoi(v)) cfg.filter = FILTER_SMOOTH;   // older config files
        if (!strcmp(k, "disc")) snprintf(cfg.disc, sizeof cfg.disc, "%s", v);
        for (size_t i = 0; i < sizeof ints / sizeof ints[0]; i++)
            if (!strcmp(k, ints[i].key)) {
                int x = atoi(v); *ints[i].val = x < ints[i].lo ? ints[i].lo : x > ints[i].hi ? ints[i].hi : x;
            }
    }
    fclose(f);
}

void cfg_save(void) {
    char p[1100], tmp[1110]; snprintf(p, sizeof p, "%sconfig.ini", cfg_dir()); snprintf(tmp, sizeof tmp, "%s.tmp", p);
    FILE *f = fopen(tmp, "w"); if (!f) return;
    fprintf(f, "# Quarantine (recompiled) settings. Edited by the launcher; safe to edit by hand.\n");
    fprintf(f, "disc=%s\n", cfg.disc);
    for (size_t i = 0; i < sizeof ints / sizeof ints[0]; i++) fprintf(f, "%s=%d\n", ints[i].key, *ints[i].val);
    if (fclose(f) == 0) rename(tmp, p);
}
