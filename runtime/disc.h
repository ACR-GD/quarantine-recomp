// Game source (disc image or extracted folder), user configuration, and the launcher.
#pragma once
#include <stdint.h>
#include <stddef.h>

// ---------------------------------------------------------------- disc (rt_disc.c)
// One CD-audio track: raw 44.1 kHz 16-bit stereo PCM at [offset, offset+bytes) of a host file.
typedef struct { char path[1024]; uint64_t offset, bytes; int swap; } cd_track_src;

enum { SRC_NONE, SRC_IMAGE, SRC_FOLDER };
// Open a .cue/.nrg/.iso image or a folder made by tools/extract.py (cd/ + trackNN.wav).
// Checks that it holds the SKIPIT.EXE this build was translated from. 0 = ok, else err is set.
int disc_open(const char *path, char *err, size_t errn);
int disc_kind(void);
int disc_has_music(void);
const char *disc_data_dir(void);          // folder with the game files (installed copy or the folder)
// Copy the data track's files into dest (skipped if dest already holds this build's files).
int disc_install(const char *dest, void (*progress)(int done, int total, void *u), void *u, char *err, size_t errn);
int disc_is_installed(const char *dest);
int disc_audio_tracks(cd_track_src *out, int max);   // tracks 2.. in order
uint32_t disc_data_sectors(void);
void disc_close(void);

// ---------------------------------------------------------------- config (rt_config.c)
typedef struct {
    char disc[1024];      // disc image or extracted folder
    int fullscreen;       // 0/1
    int filter;           // FILTER_* (rt_filter.c)
    int square;           // 0 = 4:3 like a CRT, 1 = square pixels
    int music, sfx;       // volume 0..100
    int framecap;         // pace page flips to 70 Hz
    int copyprot;         // 1 = show the manual-lookup screen
    int launcher;         // show the launcher at startup
} config_t;
extern config_t cfg;
enum { FILTER_SHARP, FILTER_SMOOTH, FILTER_CRISP, FILTER_CRT, FILTER_SCALE4X, FILTER_COUNT };
const char *cfg_dir(void);                // per-user folder (config.ini, data/, save/), ends with '/'
void cfg_load(void);
void cfg_save(void);

// ---------------------------------------------------------------- launcher (rt_launcher.c)
#ifdef USE_SDL
#include <SDL3/SDL.h>
// Runs until the player starts the game (returns 1) or quits (0). On return with 1, cfg.disc is open
// (disc_open succeeded) and its files are installed.
int launcher_run(SDL_Window *win, SDL_Renderer *ren, const char *initial_error);
// Scale the game frame (w x h, XRGB) to the logical presentation area with cfg.filter.
void filter_draw(SDL_Renderer *ren, SDL_Texture *tex, const uint32_t *fb, int w, int h);
#endif

// ---------------------------------------------------------------- hooks into the runtime
void cd_init_tracks(const cd_track_src *t, int n, uint32_t data_sectors);
void cd_set_volume(int pct);
void snd_set_volume(int pct);
