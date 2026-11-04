// Loads SKIPIT.EXE (a DOS/4GW "LE" executable) from the player's game files into guest memory:
// objects at their preferred bases + GUEST_REBASE, with internal fixups applied. Same result as
// tools/le.py, so the release binary carries no bytes of the original program.
#include "rt.h"
#include "../gen/image_info.h"
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static uint32_t u32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static uint16_t u16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
// fixups may reach past the object's end into the page slack: those bytes are dropped (as tools/le.py does)
static uint32_t lim;
static void putn(uint32_t a, uint32_t v, int n) { for (int i = 0; i < n; i++) if (a + i < lim) MEM[a + i] = (uint8_t)(v >> 8 * i); }
static void put32(uint32_t a, uint32_t v) { putn(a, v, 4); }
static void put16(uint32_t a, uint16_t v) { putn(a, v, 2); }

typedef struct { uint32_t vsize, base, flags, pidx, pcnt; } le_obj;

// Returns 0 on success, else a message.
const char *le_load(const uint8_t *d, size_t n, uint32_t rebase) {
#define NEED(off, len) do { if ((size_t)(off) + (len) > n) return "truncated executable"; } while (0)
    NEED(0x40, 4);
    uint32_t o = u32(d + 0x3c);
    if (o + 0xc4 > n || d[o] != 'L' || d[o + 1] != 'E') return "not an LE executable";
    const uint8_t *h = d + o;
    uint32_t npages = u32(h + 0x14), pagesize = u32(h + 0x28), lastpage = u32(h + 0x2c);
    uint32_t objtab = u32(h + 0x40), nobj = u32(h + 0x44);
    uint32_t fixpage = u32(h + 0x68), fixrec = u32(h + 0x6c), datapages = u32(h + 0x80);
    if (nobj == 0 || nobj > 16 || pagesize == 0) return "bad LE header";
    le_obj ob[16];
    NEED(o + objtab, nobj * 24);
    for (uint32_t i = 0; i < nobj; i++) {
        const uint8_t *e = d + o + objtab + i * 24;
        ob[i] = (le_obj){u32(e), u32(e + 4) + rebase, u32(e + 8), u32(e + 12), u32(e + 16)};
        if ((uint64_t)ob[i].base + ob[i].vsize + pagesize > MEM_SIZE) return "object outside guest memory";
        memset(MEM + ob[i].base, 0, ob[i].vsize);
        for (uint32_t k = 0; k < ob[i].pcnt; k++) {
            uint32_t p = ob[i].pidx + k, start = datapages + (p - 1) * pagesize;
            uint32_t len = p == npages ? lastpage : pagesize;
            NEED(start, len);
            memcpy(MEM + ob[i].base + k * pagesize, d + start, len);
        }
    }
    NEED(o + fixpage, (npages + 1) * 4);
    for (uint32_t p = 0; p < npages; p++) {
        le_obj *src = 0;
        for (uint32_t i = 0; i < nobj; i++) if (ob[i].pidx <= p + 1 && p + 1 < ob[i].pidx + ob[i].pcnt) src = &ob[i];
        if (!src) continue;
        uint32_t pbase = src->base + (p + 1 - src->pidx) * pagesize;
        lim = src->base + src->vsize;
        size_t i = o + fixrec + u32(d + o + fixpage + 4 * p), end = o + fixrec + u32(d + o + fixpage + 4 * (p + 1));
        if (end > n) return "truncated fixups";
        while (i < end) {
            uint8_t s = d[i], flg = d[i + 1]; i += 2;
            int cnt = -1; int16_t srcoff = 0;
            if (s & 0x20) cnt = d[i++]; else { srcoff = (int16_t)u16(d + i); i += 2; }
            if (flg & 3) return "unsupported import fixup";
            uint32_t tobj; if (flg & 0x40) { tobj = u16(d + i); i += 2; } else tobj = d[i++];
            if (tobj < 1 || tobj > nobj) return "bad fixup target";
            int styp = s & 0xf; uint32_t tgt = 0;
            if (styp != 2) { if (flg & 0x10) { tgt = u32(d + i); i += 4; } else { tgt = u16(d + i); i += 2; } }
            uint32_t val = ob[tobj - 1].base + tgt;
            int k = cnt < 0 ? 1 : cnt;
            for (int j = 0; j < k; j++) {
                int16_t so = cnt < 0 ? srcoff : (int16_t)u16(d + i);
                if (cnt >= 0) i += 2;
                if (so < 0) continue;
                uint32_t a = pbase + (uint32_t)so;
                switch (styp) {
                case 7: put32(a, val); break;                                   // 32-bit offset
                case 8: put32(a, val - (a + 4)); break;                         // 32-bit self-relative
                case 5: put16(a, (uint16_t)val); break;                         // 16-bit offset
                case 2: put16(a, (uint16_t)tobj); break;                        // selector
                case 6: put32(a, val); put16(a + 4, (uint16_t)tobj); break;     // 16:32 pointer
                case 3: put16(a, (uint16_t)val); put16(a + 2, (uint16_t)tobj); break;  // 16:16 pointer
                }
            }
        }
    }
    return 0;
#undef NEED
}

// Finds SKIPIT.EXE in dir (any letter case), checks it is the translated version, loads it.
void guest_load_exe(const char *dir) {
    char path[1200] = "";
    DIR *dp = opendir(dir);
    if (dp) {
        struct dirent *e;
        while ((e = readdir(dp))) if (!strcasecmp(e->d_name, "SKIPIT.EXE")) { snprintf(path, sizeof path, "%s/%s", dir, e->d_name); break; }
        closedir(dp);
    }
    if (!path[0]) rt_fatal("SKIPIT.EXE not found in %s", dir);
    FILE *f = fopen(path, "rb");
    if (!f) rt_fatal("can't open %s", path);
    uint8_t *buf = malloc(GUEST_EXE_SIZE + 1);
    size_t n = fread(buf, 1, GUEST_EXE_SIZE + 1, f);
    fclose(f);
    uint64_t hsh = 0xcbf29ce484222325ull;
    for (size_t i = 0; i < n; i++) hsh = (hsh ^ buf[i]) * 0x100000001b3ull;
    if (n != GUEST_EXE_SIZE || hsh != GUEST_EXE_HASH) rt_fatal("%s is not the version of Quarantine this build was made from", path);
    const char *err = le_load(buf, n, GUEST_REBASE);
    free(buf);
    if (err) rt_fatal("loading %s: %s", path, err);
}
