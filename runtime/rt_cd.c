// MSCDEX + CD-audio emulation. Audio tracks are raw PCM ranges of host files (the disc image itself,
// or WAVs from tools/extract.py); see rt_disc.c. The TOC is rebuilt from their lengths (track 1 = data,
// 2 s pregap before track 2). With no music available, silent tracks keep the game's CD logic happy.
#include "compat.h"
#include "rt.h"
#include "sys.h"
#include "disc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <pthread.h>
#include <time.h>
#ifdef USE_SDL
#include <SDL3/SDL.h>
#endif
extern int rt_verbose;

#define CD_DRIVE 3            // D:
#define MAXTRACKS 99

static struct { uint32_t start, len; cd_track_src src; } trk[MAXTRACKS + 1];
static int NTRACKS = 1;
static uint32_t leadout;
static float gain = 1.0f;
#ifdef USE_SDL
static SDL_AudioStream *stream_h;
#endif
static pthread_mutex_t cd_mx = PTHREAD_MUTEX_INITIALIZER;
static int playing, paused;
static uint32_t play_start, play_end, cur_lba;   // LBA (HSG)
static uint64_t cur_byte;                         // byte position inside cur_lba sector stream
static int cur_fd = -1, cur_trk;
static double headless_t0;
static int have_audio;
static uint64_t pcm_bytes, pcm_nonzero;
void cd_stats(void) { if (rt_verbose) fprintf(stderr, "[cd audio: %llu PCM bytes delivered, %llu non-silent]\n", (unsigned long long)pcm_bytes, (unsigned long long)pcm_nonzero); }

static uint32_t to_msf(uint32_t lba) { lba += 150; return ((lba / 4500) << 16) | (((lba / 75) % 60) << 8) | (lba % 75); }
static uint32_t from_redbook(uint32_t rb) { return ((rb >> 16) & 0xff) * 4500 + ((rb >> 8) & 0xff) * 75 + (rb & 0xff) - 150; }
static uint8_t bcd(int v) { return (uint8_t)(((v / 10) << 4) | (v % 10)); }
static int track_of(uint32_t lba) { for (int t = NTRACKS; t >= 1; t--) if (lba >= trk[t].start) return t; return 1; }

void cd_init_tracks(const cd_track_src *src, int n, uint32_t data_sectors) {
    trk[1].start = 0; trk[1].len = data_sectors;
    uint32_t lba = data_sectors + 150;
    if (n > MAXTRACKS - 1) n = MAXTRACKS - 1;
    int silent = n == 0;
    if (silent) n = 11;                                   // no music: 11 one-minute silent tracks
    NTRACKS = n + 1;
    for (int k = 0; k < n; k++) {
        int t = k + 2;
        memset(&trk[t].src, 0, sizeof trk[t].src);
        if (!silent) trk[t].src = src[k];
        uint32_t len = silent ? 75 * 60 : (uint32_t)(src[k].bytes / 2352);
        trk[t].start = lba; trk[t].len = len; lba += len;
    }
    leadout = lba;
}
void cd_set_volume(int pct) {
    gain = pct / 100.0f;
#ifdef USE_SDL
    if (stream_h) SDL_SetAudioStreamGain(stream_h, gain);
#endif
}

// position the stream at cur_lba (call with mutex held)
static void seek_stream(void) {
    if (cur_fd >= 0) { close(cur_fd); cur_fd = -1; }
    cur_trk = track_of(cur_lba);
    if (cur_trk < 2) return;
    cur_byte = (uint64_t)(cur_lba - trk[cur_trk].start) * 2352;
    cur_fd = trk[cur_trk].src.path[0] ? open(trk[cur_trk].src.path, O_RDONLY | O_BINARY) : -2;   // -2: silent track
    if (cur_fd >= 0) lseek(cur_fd, (off_t)(trk[cur_trk].src.offset + cur_byte), SEEK_SET);
}

// fill PCM (called from the audio thread)
static int cd_read_pcm(uint8_t *buf, int n) {
    pthread_mutex_lock(&cd_mx);
    int got = 0;
    while (playing && !paused && got < n) {
        if (cur_lba >= play_end) { playing = 0; break; }
        if (cur_fd == -1) seek_stream();
        if (cur_fd == -1) { playing = 0; break; }
        uint32_t want = n - got;
        uint64_t remain = (uint64_t)(play_end - cur_lba) * 2352 - (cur_byte % 2352);
        uint64_t trk_left = (uint64_t)trk[cur_trk].len * 2352 - cur_byte;    // never read into the next track
        if (want > remain) want = (uint32_t)remain;
        if (want > trk_left) want = (uint32_t)trk_left;
        ssize_t r;
        if (cur_fd == -2) { memset(buf + got, 0, want); r = want; }
        else {
            r = want ? read(cur_fd, buf + got, want) : 0;
            if (r > 0 && trk[cur_trk].src.swap)
                for (ssize_t i = 0; i + 1 < r; i += 2) { uint8_t t = buf[got + i]; buf[got + i] = buf[got + i + 1]; buf[got + i + 1] = t; }
        }
        if (r <= 0) {                                    // end of track: continue with the next one
            cur_lba = trk[cur_trk].start + trk[cur_trk].len; cur_byte = 0;
            if (cur_fd >= 0) close(cur_fd);
            cur_fd = -1;
            if (cur_trk + 1 <= NTRACKS && cur_lba < trk[cur_trk + 1].start) cur_lba = trk[cur_trk + 1].start;   // skip pregap
            continue;
        }
        got += (int)r; cur_byte += r;
        cur_lba = trk[cur_trk].start + (uint32_t)(cur_byte / 2352);
    }
    pthread_mutex_unlock(&cd_mx);
    return got;
}

#ifdef USE_SDL
static void SDLCALL audio_cb(void *ud, SDL_AudioStream *s, int additional, int total) {
    (void)ud; (void)total;
    static uint8_t buf[2352 * 16];
    while (additional > 0) {
        int n = additional > (int)sizeof buf ? (int)sizeof buf : additional;
        int got = cd_read_pcm(buf, n);
        pcm_bytes += got; for (int i = 0; i < got; i += 2) pcm_nonzero += (buf[i] | buf[i + 1]) != 0;
        if (got < n) memset(buf + got, 0, n - got);
        SDL_PutAudioStreamData(s, buf, n);
        additional -= n;
    }
}
#endif

void cd_audio_start(int headless) {
#ifdef USE_SDL
    if (!headless && SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        SDL_AudioSpec spec = {SDL_AUDIO_S16LE, 2, 44100};
        SDL_AudioStream *st = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, audio_cb, 0);
        if (st) { stream_h = st; SDL_SetAudioStreamGain(st, gain); SDL_ResumeAudioStreamDevice(st); have_audio = 1; }
    }
#endif
    (void)headless;
}

// headless: advance the play position by wall-clock time
static void headless_advance(void) {
    if (have_audio || !playing || paused) return;
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    double now = t.tv_sec + t.tv_nsec * 1e-9;
    uint32_t adv = (uint32_t)((now - headless_t0) * 75);
    if (adv) { headless_t0 += adv / 75.0; cur_lba += adv; if (cur_lba >= play_end) playing = 0; }
}

static uint32_t far_lin(uint32_t a) { return ((RD16(a + 2) << 4) + RD16(a)); }

static void ioctl_input(uint32_t cb) {
    uint8_t code = RD8(cb);
    switch (code) {
    case 0: WR32(cb + 1, 0); break;
    case 1: WR8(cb + 1, 0); WR32(cb + 1 + 1, cur_lba); break;
    case 4: for (int k = 0; k < 4; k++) { WR8(cb + 1 + 2 * k, k); WR8(cb + 2 + 2 * k, 0xff); } break;
    case 6: WR32(cb + 1, 0x00000316u); break;
    case 7: WR16(cb + 2, 2048); break;
    case 8: WR32(cb + 1, leadout); break;
    case 9: WR8(cb + 1, 1); break;
    case 10: WR8(cb + 1, 1); WR8(cb + 2, NTRACKS); WR32(cb + 3, to_msf(leadout)); break;
    case 11: { int t = RD8(cb + 1); if (t < 1 || t > NTRACKS) t = 1;
               WR32(cb + 2, to_msf(trk[t].start)); WR8(cb + 6, t == 1 ? 0x41 : 0x01); break; }
    case 12: {
        uint32_t l = cur_lba; int t = track_of(l); uint32_t rel = l - trk[t].start; uint32_t abs = l + 150;
        WR8(cb + 1, t == 1 ? 0x41 : 0x01); WR8(cb + 2, bcd(t)); WR8(cb + 3, 1);
        WR8(cb + 4, rel / 4500); WR8(cb + 5, (rel / 75) % 60); WR8(cb + 6, rel % 75); WR8(cb + 7, 0);
        WR8(cb + 8, abs / 4500); WR8(cb + 9, (abs / 75) % 60); WR8(cb + 10, abs % 75);
        break;
    }
    case 15: WR16(cb + 1, paused ? 1 : 0); WR32(cb + 3, to_msf(play_start)); WR32(cb + 7, to_msf(play_end)); break;
    default: if (rt_verbose) fprintf(stderr, "CD ioctl input %d\n", code);
    }
}

// MSCDEX int 2Fh, AX=15xx. es = real-mode segment for buffers.
int cd_int2f(uint32_t *eax, uint32_t *ebx, uint32_t *ecx, uint32_t *edx, uint16_t es) {
    (void)edx;
    uint16_t ax = *eax & 0xffff;
    if ((ax >> 8) != 0x15) return 0;
    pthread_mutex_lock(&cd_mx);
    headless_advance();
    switch (ax) {
    case 0x1500: *ebx = (*ebx & 0xffff0000u) | 1; *ecx = (*ecx & 0xffff0000u) | CD_DRIVE; break;
    case 0x150b: *eax = (*eax & 0xffff0000u) | ((*ecx & 0xffff) == CD_DRIVE ? 0x5ad8 : 0); *ebx = (*ebx & 0xffff0000u) | 0xadad; break;
    case 0x150c: *ebx = (*ebx & 0xffff0000u) | 0x020a; break;
    case 0x150d: WR8((es << 4) + (*ebx & 0xffff), CD_DRIVE); break;
    case 0x1510: {
        uint32_t rh = (es << 4) + (*ebx & 0xffff);
        uint8_t cmd = RD8(rh + 2);
        uint16_t status = 0x0100;
        switch (cmd) {
        case 3: ioctl_input(far_lin(rh + 14)); break;
        case 12: { uint8_t c = RD8(far_lin(rh + 14)); if (c == 2) { playing = paused = 0; } break; }
        case 13: case 14: break;               // device open/close
        case 128: case 130: case 131: break;   // read long / prefetch / seek (data reads go through DOS files)
        case 132: {
            uint32_t start = RD32(rh + 14), n = RD32(rh + 18);
            if (RD8(rh + 13) == 1) start = from_redbook(start);
            play_start = start; play_end = start + n; if (play_end > leadout) play_end = leadout;
            cur_lba = start; paused = 0; playing = n > 0; seek_stream();
            struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); headless_t0 = t.tv_sec + t.tv_nsec * 1e-9;
            if (rt_verbose) fprintf(stderr, "CD play lba %u (track %d) for %u sectors\n", start, track_of(start), n);
            break;
        }
        case 133: if (playing && !paused) paused = 1; else { playing = paused = 0; } break;
        case 136: if (playing) { paused = 0; struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); headless_t0 = t.tv_sec + t.tv_nsec * 1e-9; } break;
        default: if (rt_verbose) fprintf(stderr, "CD request %d\n", cmd);
        }
        if (playing && !paused) status |= 0x0200;
        WR16(rh + 3, status);
        break;
    }
    default: break;
    }
    pthread_mutex_unlock(&cd_mx);
    return 1;
}
