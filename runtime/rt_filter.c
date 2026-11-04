// Output filters: how the game's 320x200 (or 320x240) frame is scaled to the window.
//   Sharp    nearest neighbour (GPU)
//   Smooth   bilinear (GPU)
//   Crisp    "sharp bilinear": integer nearest upscale on the CPU, then bilinear for the leftover fraction,
//            so pixels stay square-edged but all the same size at any window size
//   CRT      scanlines with a gaussian beam profile, slight horizontal softness, aperture-grille mask
//   Scale4x  AdvMAME/EPX edge smoothing applied twice (4x), then bilinear
// The CPU filters render into a streaming texture sized for the window's real pixel size.
#ifdef USE_SDL
#include "disc.h"
#include <math.h>
#include <stdint.h>
#include <string.h>

static SDL_Texture *ftex;
static int fw, fh;

static uint32_t *lock_out(SDL_Renderer *ren, int w, int h, int *pitch) {
    if (!ftex || w != fw || h != fh) {
        if (ftex) SDL_DestroyTexture(ftex);
        ftex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_XRGB8888, SDL_TEXTUREACCESS_STREAMING, w, h);
        if (!ftex) return 0;
        SDL_SetTextureScaleMode(ftex, SDL_SCALEMODE_LINEAR);
        fw = w; fh = h;
    }
    void *px; int p;
    if (!SDL_LockTexture(ftex, NULL, &px, &p)) return 0;
    *pitch = p / 4;
    return px;
}

// ---------------------------------------------------------------- Crisp
static void crisp(SDL_Renderer *ren, const uint32_t *fb, int w, int h, SDL_FRect r) {
    int kx = (int)(r.w / w), ky = (int)(r.h / h);
    if (kx < 1) kx = 1; if (kx > 6) kx = 6;        // beyond 6x the bilinear remainder is invisible
    if (ky < 1) ky = 1; if (ky > 6) ky = 6;
    int pitch; uint32_t *o = lock_out(ren, w * kx, h * ky, &pitch);
    if (!o) return;
    for (int y = 0; y < h; y++) {
        uint32_t *row = o + (size_t)y * ky * pitch;
        const uint32_t *s = fb + y * w;
        for (int x = 0; x < w; x++) for (int i = 0; i < kx; i++) row[x * kx + i] = s[x];
        for (int j = 1; j < ky; j++) memcpy(row + (size_t)j * pitch, row, (size_t)w * kx * 4);
    }
    SDL_UnlockTexture(ftex);
}

// ---------------------------------------------------------------- Scale4x
static void scale2x(const uint32_t *s, int w, int h, uint32_t *d) {
    for (int y = 0; y < h; y++) {
        const uint32_t *up = s + (y ? y - 1 : y) * w, *mid = s + y * w, *dn = s + (y < h - 1 ? y + 1 : y) * w;
        uint32_t *d0 = d + (size_t)y * 2 * w * 2, *d1 = d0 + w * 2;
        for (int x = 0; x < w; x++) {
            uint32_t B = up[x], D = mid[x ? x - 1 : x], E = mid[x], F = mid[x < w - 1 ? x + 1 : x], H = dn[x];
            if (B != H && D != F) {
                d0[2 * x] = D == B ? D : E; d0[2 * x + 1] = B == F ? F : E;
                d1[2 * x] = D == H ? D : E; d1[2 * x + 1] = H == F ? F : E;
            } else d0[2 * x] = d0[2 * x + 1] = d1[2 * x] = d1[2 * x + 1] = E;
        }
    }
}
static void scale4x(SDL_Renderer *ren, const uint32_t *fb, int w, int h) {
    static uint32_t mid[640 * 480], out[1280 * 960];
    scale2x(fb, w, h, mid);
    scale2x(mid, w * 2, h * 2, out);
    int pitch; uint32_t *o = lock_out(ren, w * 4, h * 4, &pitch);
    if (!o) return;
    for (int y = 0; y < h * 4; y++) memcpy(o + (size_t)y * pitch, out + (size_t)y * w * 4, (size_t)w * 16);
    SDL_UnlockTexture(ftex);
}

// ---------------------------------------------------------------- CRT
#define CRT_MAXW 1920
#define CRT_MAXH 1600
static void crt(SDL_Renderer *ren, const uint32_t *fb, int w, int h, SDL_FRect r) {
    int ow = (int)r.w, oh = (int)r.h;               // one texel per real pixel so scanlines don't alias
    if (ow > CRT_MAXW) ow = CRT_MAXW; if (oh > CRT_MAXH) oh = CRT_MAXH;
    if (ow < w) ow = w; if (oh < h) oh = h;
    static int lw, lh, lsrcw, lsrch;
    static int16_t hx[CRT_MAXW]; static uint8_t hf[CRT_MAXW];   // horizontal: left source pixel, weight of right
    static int16_t vy[CRT_MAXH]; static uint16_t vw0[CRT_MAXH], vw1[CRT_MAXH];
    static uint8_t mask[CRT_MAXW][3];
    static uint32_t hrow[240][CRT_MAXW];
    if (ow != lw || oh != lh || w != lsrcw || h != lsrch) {
        lw = ow; lh = oh; lsrcw = w; lsrch = h;
        for (int x = 0; x < ow; x++) {
            double u = (x + 0.5) * w / ow - 0.5; int i = (int)floor(u); double t = u - i;
            t = t * t * (3 - 2 * t);                // ease: sharper than linear, softer than nearest
            if (i < 0) { i = 0; t = 0; } if (i >= w - 1) { i = w - 2; t = u >= w - 1 ? 1 : t; }
            hx[x] = (int16_t)i; hf[x] = (uint8_t)(t * 255 + 0.5);
        }
        // aperture grille: needs ~3 real pixels per triad to read as a mask rather than colour fringes
        int m = ow >= w * 3;
        for (int x = 0; x < ow; x++) for (int c = 0; c < 3; c++)
            mask[x][c] = !m ? 255 : (x % 3 == 2 - c) ? 255 : 200;   // c: 0=b 1=g 2=r in XRGB
        // beam profile: gaussian per source line, gained back up to the original average brightness
        // (bright lines then clip, which reads as a slight bloom).
        double sigma = oh >= h * 3 ? 0.30 : 0.42;   // little room per line: soften so lines don't flicker
        double gain = 1.0 / (sigma * 2.5066) * (m ? 255.0 / 218 : 1.0);
        for (int y = 0; y < oh; y++) {
            double v = (y + 0.5) * h / oh - 0.5; int i = (int)floor(v); double d = v - i;
            double a = exp(-d * d / (2 * sigma * sigma)), b = exp(-(1 - d) * (1 - d) / (2 * sigma * sigma));
            if (i < 0) { i = 0; a = b; b = 0; } if (i >= h - 1) { i = h - 2; b = a; a = 0; }  // edges
            vy[y] = (int16_t)i; vw0[y] = (uint16_t)(a * gain * 256); vw1[y] = (uint16_t)(b * gain * 256);
        }
    }
    for (int y = 0; y < h; y++) {
        const uint32_t *s = fb + y * w;
        uint32_t *d = hrow[y];
        for (int x = 0; x < ow; x++) {
            uint32_t p = s[hx[x]], q = s[hx[x] + 1], f = hf[x], g = 255 - f;
            uint32_t rb = ((p & 0xff00ff) * g + (q & 0xff00ff) * f) >> 8 & 0xff00ff;
            uint32_t gg = ((p & 0x00ff00) * g + (q & 0x00ff00) * f) >> 8 & 0x00ff00;
            d[x] = rb | gg;
        }
    }
    int pitch; uint32_t *o = lock_out(ren, ow, oh, &pitch);
    if (!o) return;
    for (int y = 0; y < oh; y++) {
        const uint32_t *a = hrow[vy[y]], *b = hrow[vy[y] + 1];
        uint32_t w0 = vw0[y], w1 = vw1[y];
        uint32_t *d = o + (size_t)y * pitch;
        for (int x = 0; x < ow; x++) {
            uint32_t pa = a[x], pb = b[x], px = 0;
            for (int c = 0; c < 3; c++) {
                uint32_t v = (((pa >> (c * 8)) & 255) * w0 + ((pb >> (c * 8)) & 255) * w1) >> 8;
                v = v * mask[x][c] >> 8;
                px |= (v > 255 ? 255 : v) << (c * 8);
            }
            d[x] = px;
        }
    }
    SDL_UnlockTexture(ftex);
}

// ---------------------------------------------------------------- entry
// Draws fb (w x h, XRGB) over the whole logical area. tex is the game's 320x240 streaming texture.
void filter_draw(SDL_Renderer *ren, SDL_Texture *tex, const uint32_t *fb, int w, int h) {
    SDL_FRect r;
    if (!SDL_GetRenderLogicalPresentationRect(ren, &r) || r.w < 1 || r.h < 1) { int ww, hh; SDL_GetCurrentRenderOutputSize(ren, &ww, &hh); r.w = ww; r.h = hh; }
    switch (cfg.filter) {
    case FILTER_CRISP:   crisp(ren, fb, w, h, r); break;
    case FILTER_CRT:     crt(ren, fb, w, h, r); break;
    case FILTER_SCALE4X: scale4x(ren, fb, w, h); break;
    default: {
        SDL_SetTextureScaleMode(tex, cfg.filter == FILTER_SMOOTH ? SDL_SCALEMODE_LINEAR : SDL_SCALEMODE_NEAREST);
        SDL_UpdateTexture(tex, NULL, fb, w * 4);
        SDL_FRect src = {0, 0, (float)w, (float)h};
        SDL_RenderTexture(ren, tex, &src, NULL);
        return;
    }
    }
    if (ftex) SDL_RenderTexture(ren, ftex, NULL, NULL);
}
#endif
