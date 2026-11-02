// HMI SOS digital-audio emulation (high level).
//
// The game links the SOS 4 library, which normally loads a real driver from HMIDRV.386 at run time.
// That driver is data to the recompiler, so instead the SOS API entry points are hooked (see HOOKS in
// tools/recomp.py) and implemented here with a small software mixer.
//
// _SOS_START_SAMPLE layout as the game fills it:
//   +00 sample ptr (offset) +04 selector  +08 size  +0C loop count (0xFFFF = forever)
//   +10 volume (0..7FFF)    +14 callback offset  +18 callback selector
//   +1C flags (0x100 volume, 0x400 pitch shift, 0x4000 looping)  +2C pitch add (16.16, 1.0 = 11000 Hz)
// Samples are 8-bit unsigned mono.
#include "rt.h"
#include "sys.h"
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <time.h>
#ifdef USE_SDL
#include <SDL3/SDL.h>
#endif
extern int rt_verbose;

#define OUT_RATE   22050
#define BASE_RATE  11000
#define NVOICES    16
#define LOOKAHEAD  (OUT_RATE / 10)       // fire the "processed" callback ~100 ms before a buffer ends
#define FAKE_TIMER_HANDLE 0x7ff0
#define F_VOLUME   0x0100
#define F_PITCH    0x0400
#define F_LOOPING  0x4000

typedef struct { uint8_t *data; uint32_t size; } buf_t;
typedef struct {
    int active, waiting;              // waiting: buffer ran out, callback fired, no continuation yet
    buf_t cur, next;
    uint64_t pos;                     // 32.32 position in cur
    uint32_t pitch, flags, cb_off;
    uint16_t loops, vol, cb_sel;
    int cb_fired;                     // processed-callback already raised for cur
    int cb_action;                    // pending callback action (-1 none): 0 processed, 2 done
    uint32_t serial;
} voice_t;

static voice_t vc[NVOICES];
static pthread_mutex_t mx = PTHREAD_MUTEX_INITIALIZER;
static int have_dev, muted;
static uint32_t serial;
static double hl_t0;
static FILE *dump;            // QUAR_SNDDUMP=file.wav (headless only): raw mixer output
static uint32_t dump_frames;
static uint64_t out_frames, out_nonzero;

static void wav_header(FILE *f, uint32_t frames) {
    uint32_t h[11] = {0x46464952, 36 + frames * 2, 0x45564157, 0x20746d66, 16, 0x00010001, OUT_RATE, OUT_RATE * 2, 0x00100002, 0x61746164, frames * 2};
    fseek(f, 0, SEEK_SET); fwrite(h, 4, 11, f); fseek(f, 0, SEEK_END); fflush(f);
}
void snd_shutdown(void) {
    if (rt_verbose) fprintf(stderr, "[sfx: %llu frames mixed, %llu non-silent, device %d]\n", (unsigned long long)out_frames, (unsigned long long)out_nonzero, have_dev); if (dump) { wav_header(dump, dump_frames); fclose(dump); dump = 0; } }

static buf_t copy_sample(uint32_t sp) {
    uint32_t lin = RD32(sp) + sel_base(RD16(sp + 4));
    uint32_t size = RD32(sp + 8);
    if (size > 0x400000) size = 0;
    buf_t b = { 0, size };
    if (size) { b.data = malloc(size); memcpy(b.data, MEM + lin, size); }
    return b;
}
static void free_buf(buf_t *b) { free(b->data); b->data = 0; b->size = 0; }
static void voice_off(voice_t *v) { free_buf(&v->cur); free_buf(&v->next); v->active = v->waiting = 0; }

// Mix n mono frames into out (int32 accumulator). Called with mx held.
static void mix(int32_t *out, int n) {
    for (int k = 0; k < NVOICES; k++) {
        voice_t *v = &vc[k];
        if (!v->active || v->waiting) continue;
        uint64_t step = (v->flags & F_PITCH) ? ((uint64_t)v->pitch * BASE_RATE << 16) / OUT_RATE
                                              : ((uint64_t)BASE_RATE << 32) / OUT_RATE;
        if (!step) step = 1;
        int vol = (v->flags & F_VOLUME) ? v->vol : 0x7fff;
        for (int i = 0; i < n; i++) {
            uint32_t ip = (uint32_t)(v->pos >> 32);
            if (ip >= v->cur.size) {
                if (v->next.data) {                          // seamless continuation
                    free_buf(&v->cur); v->cur = v->next; v->next.data = 0; v->next.size = 0;
                    v->pos = 0; v->cb_fired = 0; ip = 0;
                } else if ((v->flags & F_LOOPING) && v->loops) {
                    if (v->loops != 0xffff) v->loops--;
                    v->pos -= (uint64_t)v->cur.size << 32; ip = (uint32_t)(v->pos >> 32);
                    if (ip >= v->cur.size) { v->pos = 0; ip = 0; }
                } else {
                    if (v->cb_off) { v->waiting = 1; v->cb_action = 2; irq_pending = 1; }
                    else voice_off(v);
                    break;
                }
                if (!v->cur.size) { voice_off(v); break; }
            }
            uint32_t fr = (uint32_t)(v->pos >> 16) & 0xffff;
            int s0 = v->cur.data[ip] - 128;
            int s1 = ip + 1 < v->cur.size ? v->cur.data[ip + 1] - 128 : s0;
            int s = (s0 * 256 + (((s1 - s0) * (int)fr) >> 8));
            out[i] += (s * vol) >> 15;
            v->pos += step;
        }
        // raise the "processed" callback shortly before the buffer runs out, so the game can queue the next one
        if (v->active && !v->waiting && v->cb_off && !v->cb_fired && !v->next.data && !(v->flags & F_LOOPING)) {
            uint64_t left = ((uint64_t)v->cur.size << 32) - v->pos;
            if (left / step < LOOKAHEAD) { v->cb_fired = 1; v->cb_action = 0; irq_pending = 1; }
        }
    }
}

static void render(int16_t *dst, int n) {
    int32_t acc[1024];
    while (n > 0) {
        int m = n > 1024 ? 1024 : n;
        memset(acc, 0, sizeof(int32_t) * m);
        pthread_mutex_lock(&mx); mix(acc, m); pthread_mutex_unlock(&mx);
        for (int i = 0; i < m; i++) {
            int32_t s = muted ? 0 : acc[i] * 3 / 4;
            dst[i] = s > 32767 ? 32767 : s < -32768 ? -32768 : (int16_t)s;
            out_nonzero += dst[i] != 0;
        }
        out_frames += m; dst += m; n -= m;
    }
}

#ifdef USE_SDL
static void SDLCALL audio_cb(void *ud, SDL_AudioStream *s, int additional, int total) {
    (void)ud; (void)total;
    static int16_t buf[2048];
    while (additional > 0) {
        int n = additional / 2 > 2048 ? 2048 : additional / 2;
        if (n <= 0) break;
        render(buf, n);
        SDL_PutAudioStreamData(s, buf, n * 2);
        additional -= n * 2;
    }
}
#endif

static double now_s(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec * 1e-9; }

void snd_start(int headless) {
    muted = getenv("QUAR_NOSOUND") != 0;
    hl_t0 = now_s();
    if (headless && getenv("QUAR_SNDDUMP")) { dump = fopen(getenv("QUAR_SNDDUMP"), "wb"); if (dump) wav_header(dump, 0); }
#ifdef USE_SDL
    if (!headless && SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        SDL_AudioSpec spec = {SDL_AUDIO_S16LE, 1, OUT_RATE};
        SDL_AudioStream *st = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, audio_cb, 0);
        if (st) { SDL_ResumeAudioStreamDevice(st); have_dev = 1; }
        else fprintf(stderr, "sound: %s\n", SDL_GetError());
    }
#endif
    (void)headless;
}

// Main thread, from the IRQ check: advance time when there is no audio device, and run pending
// sample callbacks (far cdecl: callback(hDriver, action, hSample)) the way the driver's ISR would.
void snd_service(void) {
    if (!have_dev) {
        double t = now_s();
        int n = (int)((t - hl_t0) * OUT_RATE);
        if (n > 0) {
            if (n > OUT_RATE) n = OUT_RATE;
            hl_t0 += n / (double)OUT_RATE;
            int16_t out[1024];
            while (n > 0) {
                int m = n > 1024 ? 1024 : n;
                render(out, m);
                if (dump) { fwrite(out, 2, m, dump); dump_frames += m; }
                n -= m;
            }
        }
    }
    for (int k = 0; k < NVOICES; k++) {
        pthread_mutex_lock(&mx);
        voice_t *v = &vc[k];
        int act = v->cb_action; uint32_t off = v->cb_off; uint16_t sel = v->cb_sel;
        v->cb_action = -1;
        pthread_mutex_unlock(&mx);
        if (act < 0) continue;
        if (off) {
            uint32_t saveR[8]; memcpy(saveR, R, sizeof saveR);
            uint16_t saveS[6]; memcpy(saveS, S, sizeof saveS);
            uint32_t saveSB[6]; memcpy(saveSB, SB, sizeof saveSB);
            uint32_t fl = get_eflags(), sdf = DF, sif = IF;
            PUSH32(k); PUSH32(act); PUSH32(0);
            rt_farcall(off, sel, 0);
            memcpy(R, saveR, sizeof saveR); memcpy(S, saveS, sizeof saveS); memcpy(SB, saveSB, sizeof saveSB);
            set_eflags(fl); DF = sdf; IF = sif;
        }
        pthread_mutex_lock(&mx);
        if (act == 2 && vc[k].waiting) voice_off(&vc[k]);   // done and not continued: free the slot
        pthread_mutex_unlock(&mx);
    }
}

static int start_sample(uint32_t sp) {
    pthread_mutex_lock(&mx);
    int k;
    for (k = 0; k < NVOICES; k++) if (!vc[k].active && vc[k].cb_action < 0) break;
    if (k == NVOICES) {                        // steal the oldest non-looping voice
        uint32_t best = 0xffffffffu; int bk = -1;
        for (int j = 0; j < NVOICES; j++)
            if (!(vc[j].flags & F_LOOPING) && !vc[j].cb_off && vc[j].serial < best) { best = vc[j].serial; bk = j; }
        if (bk < 0) { pthread_mutex_unlock(&mx); return -1; }
        k = bk; voice_off(&vc[k]);
    }
    voice_t *v = &vc[k];
    v->cur = copy_sample(sp); v->next = (buf_t){0, 0};
    v->pos = 0; v->loops = RD16(sp + 0x0c); v->vol = RD16(sp + 0x10);
    v->cb_off = RD32(sp + 0x14); v->cb_sel = RD16(sp + 0x18);
    v->flags = RD32(sp + 0x1c); v->pitch = RD32(sp + 0x2c);
    v->cb_fired = 0; v->cb_action = -1; v->waiting = 0; v->serial = ++serial;
    v->active = v->cur.size != 0;
    if (rt_verbose) fprintf(stderr, "sos start #%d size %u pitch %08x flags %04x vol %04x cb %08x\n",
                            k, v->cur.size, v->pitch, v->flags, v->vol, v->cb_off);
    pthread_mutex_unlock(&mx);
    return k;
}

static void continue_sample(int k, uint32_t sp) {
    if (k < 0 || k >= NVOICES) return;
    pthread_mutex_lock(&mx);
    voice_t *v = &vc[k];
    buf_t b = copy_sample(sp);
    if (rt_verbose) fprintf(stderr, "sos continue #%d size %u (%s)\n", k, b.size, v->active && !v->waiting ? "queued" : "restart");
    if (v->active && !v->waiting) { free_buf(&v->next); v->next = b; }
    else {                                     // ran dry already: restart on the same handle
        free_buf(&v->cur); free_buf(&v->next);
        v->cur = b; v->pos = 0; v->cb_fired = 0; v->active = b.size != 0;
        v->waiting = 0;
    }
    pthread_mutex_unlock(&mx);
}

static uint32_t arg(int i) { return RD32(SB[2] + ESP + 4 + 4 * i); }   // stack args after the return address

// Called at the entry of hooked SOS functions. Returns 1 if handled (EAX holds the result).
int snd_hle(uint32_t fn) {
    switch (fn) {
    case 0x155194:                  // sosDIGIInitSystem
    case 0x15532d:                  // sosDIGIDetectInit
    case 0x155b9c:                  // sosDIGIDetectFindHardware
    case 0x1552c2:                  // sosDIGIDetectUnInit
    case 0x1561d5:                  // sosTIMER/driver prep
    case 0x15623d:                  // sosDIGIUnInitSystem
    case 0x156acf:                  // sosDIGIUnInitDriver
        EAX = 0; return 1;
    case 0x15626b: {                // sosDIGIInitDriver(id, hw far*, cfg far*, hDriver far*)
        uint32_t hp = arg(2) + sel_base((uint16_t)arg(3));
        WR32(hp, 0);
        EAX = 0; return 1;
    }
    case 0x154208:                  // sosTIMERRegisterEvent(rate, callback far, handle far*)
        if (EBX != 0) return 0;     // the game's own timer events go to the real code
        WR32(arg(0) + sel_base((uint16_t)arg(1)), FAKE_TIMER_HANDLE);
        EAX = 0; return 1;
    case 0x15452a:                  // sosTIMERRemoveEvent(handle)
        if ((EAX & 0xffff) != FAKE_TIMER_HANDLE) return 0;
        EAX = 0; return 1;
    case 0x156fb8:                  // sosDIGIStartSample(hDriver, sample far*) -> handle
        EAX = (uint32_t)start_sample(EBX + sel_base((uint16_t)ECX)); return 1;
    case 0x1575d1: {                // sosDIGIStopSample(hDriver, handle)
        int k = (int)EDX;
        if (k >= 0 && k < NVOICES) { pthread_mutex_lock(&mx); voice_off(&vc[k]); vc[k].cb_action = -1; pthread_mutex_unlock(&mx); }
        EAX = 0; return 1;
    }
    case 0x157637:                  // sosDIGIContinueSample(hDriver, handle, sample far*)
        continue_sample((int)EDX, EBX + sel_base((uint16_t)ECX));
        EAX = 0; return 1;
    case 0x157e7d: {                // sosDIGISetPitch(hDriver, handle, pitch)
        int k = (int)EDX;
        if (k >= 0 && k < NVOICES) { pthread_mutex_lock(&mx); vc[k].pitch = EBX; vc[k].flags |= F_PITCH; pthread_mutex_unlock(&mx); }
        EAX = 0; return 1;
    }
    }
    return 0;
}

void snd_init(void) { for (int k = 0; k < NVOICES; k++) vc[k].cb_action = -1; }
