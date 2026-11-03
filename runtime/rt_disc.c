// Reading the game straight from a CD image (.cue/.bin, Nero .nrg, .iso) or from a folder made by
// tools/extract.py. The data track's files are installed (copied) once into the user's data folder;
// CD audio is streamed from the image at run time, so nothing large is duplicated.
#include "disc.h"
#include "../gen/image_info.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>

#define MAXTRK 99

static struct {
    int kind;
    char path[1024];
    // data track (images)
    int dfd;
    uint64_t doff;           // byte offset of sector 0 of the data track
    uint32_t dsz, ddata;     // sector size, offset of the 2048 user bytes inside a sector
    uint32_t dsectors;
    // folder source
    char dir[1024];
    cd_track_src audio[MAXTRK];
    int naudio;
} D = { .dfd = -1 };

static int fail(char *err, size_t n, const char *fmt, const char *arg) {
    if (err) snprintf(err, n, fmt, arg ? arg : "");
    return -1;
}
static const char *ext_of(const char *p) { const char *d = strrchr(p, '.'), *s = strrchr(p, '/'); return d && (!s || d > s) ? d : ""; }
static int is_dir(const char *p) { struct stat st; return stat(p, &st) == 0 && S_ISDIR(st.st_mode); }
static uint64_t fsize(const char *p) { struct stat st; return stat(p, &st) == 0 ? (uint64_t)st.st_size : 0; }
static uint32_t be32(const uint8_t *b) { return (uint32_t)b[0] << 24 | b[1] << 16 | b[2] << 8 | b[3]; }
static uint64_t be64(const uint8_t *b) { return (uint64_t)be32(b) << 32 | be32(b + 4); }
static uint32_t le32(const uint8_t *b) { return b[0] | b[1] << 8 | b[2] << 16 | (uint32_t)b[3] << 24; }

// Resolve name inside dir, ignoring case (cue sheets often disagree with the file system).
static int find_ci(const char *dir, const char *name, char *out, size_t n) {
    snprintf(out, n, "%s/%s", dir, name);
    if (access(out, F_OK) == 0) return 1;
    DIR *d = opendir(dir); if (!d) return 0;
    struct dirent *e; int ok = 0;
    while ((e = readdir(d))) if (!strcasecmp(e->d_name, name)) { snprintf(out, n, "%s/%s", dir, e->d_name); ok = 1; break; }
    closedir(d);
    return ok;
}

// ---------------------------------------------------------------- data track and ISO 9660
static int read_sector(uint32_t lba, uint8_t *buf) {
    if (lba >= D.dsectors) return -1;
    return pread(D.dfd, buf, 2048, (off_t)(D.doff + (uint64_t)lba * D.dsz + D.ddata)) == 2048 ? 0 : -1;
}

typedef int (*file_cb)(const char *name, uint32_t lba, uint32_t size, void *u);
// Calls cb for every file of the root directory (the Quarantine CD has no subdirectories).
static int iso_walk(file_cb cb, void *u) {
    uint8_t pvd[2048], sec[2048];
    if (read_sector(16, pvd) || memcmp(pvd + 1, "CD001", 5)) return -1;
    uint32_t lba = le32(pvd + 156 + 2), size = le32(pvd + 156 + 10);
    for (uint32_t off = 0; off < size; off += 2048) {
        if (read_sector(lba + off / 2048, sec)) return -1;
        for (int i = 0; i < 2048;) {
            int rl = sec[i];
            if (!rl) break;                                   // rest of this sector is padding
            const uint8_t *r = sec + i; i += rl;
            int nl = r[32];
            if (nl == 1 && (r[33] == 0 || r[33] == 1)) continue;   // . and ..
            if (r[25] & 2) continue;                          // directory
            char name[64]; int k;
            for (k = 0; k < nl && k < 63 && r[33 + k] != ';'; k++) name[k] = r[33 + k];
            name[k] = 0;
            if (k && name[k - 1] == '.') name[k - 1] = 0;
            int rc = cb(name, le32(r + 2), le32(r + 10), u);
            if (rc) return rc;
        }
    }
    return 0;
}

static uint64_t fnv(uint64_t h, const uint8_t *p, size_t n) { while (n--) h = (h ^ *p++) * 0x100000001b3ull; return h; }

struct find_exe { int found; uint64_t hash; uint32_t size; };
static int exe_cb(const char *name, uint32_t lba, uint32_t size, void *u) {
    struct find_exe *f = u;
    if (strcasecmp(name, "SKIPIT.EXE")) return 0;
    uint8_t sec[2048]; uint64_t h = 0xcbf29ce484222325ull;
    for (uint32_t done = 0; done < size; done += 2048, lba++) {
        if (read_sector(lba, sec)) return -1;
        h = fnv(h, sec, size - done < 2048 ? size - done : 2048);
    }
    f->found = 1; f->hash = h; f->size = size;
    return 1;
}

// ---------------------------------------------------------------- audio byte order (some .nrg are big-endian)
static uint64_t roughness(const uint8_t *b, size_t n, int swap) {
    uint64_t s = 0; int prev = 0;
    for (size_t i = 0; i + 1 < n; i += 4) {
        int16_t v = swap ? (int16_t)(b[i] << 8 | b[i + 1]) : (int16_t)(b[i] | b[i + 1] << 8);
        if (i) s += (uint64_t)abs(v - prev);
        prev = v;
    }
    return s;
}
static int detect_swap(int fd, uint64_t off, uint64_t bytes) {
    static uint8_t probe[2352 * 50];
    size_t n = bytes < sizeof probe ? (size_t)bytes : sizeof probe;
    if (pread(fd, probe, n, (off_t)(off + bytes / 2 - n / 2)) != (ssize_t)n) return 0;
    return roughness(probe, n, 1) < roughness(probe, n, 0);
}

// ---------------------------------------------------------------- image formats
static void add_audio(const char *path, uint64_t off, uint64_t bytes, int swap) {
    if (D.naudio >= MAXTRK) return;
    cd_track_src *t = &D.audio[D.naudio++];
    snprintf(t->path, sizeof t->path, "%s", path); t->offset = off; t->bytes = bytes / 2352 * 2352; t->swap = swap;
}

static int open_nrg(const char *p, char *err, size_t en) {
    int fd = open(p, O_RDONLY); if (fd < 0) return fail(err, en, "Can't open %s", p);
    uint64_t sz = fsize(p); uint8_t f[12]; uint64_t chunks;
    if (sz < 12 || pread(fd, f, 12, (off_t)(sz - 12)) != 12) { close(fd); return fail(err, en, "Not a Nero image", 0); }
    if (!memcmp(f, "NER5", 4)) chunks = be64(f + 4);
    else if (!memcmp(f + 4, "NERO", 4)) chunks = be32(f + 8);
    else { close(fd); return fail(err, en, "Not a Nero image (no NER5/NERO footer)", 0); }
    int first_done = 0;
    for (uint64_t pos = chunks; pos + 8 <= sz;) {
        uint8_t h[8]; if (pread(fd, h, 8, (off_t)pos) != 8) break;
        uint32_t ln = be32(h + 4);
        if (!memcmp(h, "END!", 4)) break;
        if (!memcmp(h, "DAOX", 4) || !memcmp(h, "DAOI", 4)) {
            int x = !memcmp(h, "DAOX", 4), esz = x ? 42 : 30;
            uint8_t *b = malloc(ln); if (pread(fd, b, ln, (off_t)(pos + 8)) != (ssize_t)ln) { free(b); break; }
            int first = b[20], last = b[21];
            for (int t = first, q = 22; t <= last && q + esz <= (int)ln; t++, q += esz) {
                uint32_t ssz = (uint32_t)b[q + 12] << 8 | b[q + 13];
                uint64_t i1 = x ? be64(b + q + 26) : be32(b + q + 22), end = x ? be64(b + q + 34) : be32(b + q + 26);
                if (!first_done) {                       // the first track is the data track
                    D.doff = i1; D.dsz = ssz; D.ddata = ssz == 2048 ? 0 : ssz == 2336 ? 8 : 16;
                    D.dsectors = (uint32_t)((end - i1) / ssz); first_done = 1;
                } else if (ssz == 2352) add_audio(p, i1, end - i1, detect_swap(fd, i1, end - i1));
            }
            free(b);
        }
        pos += 8 + ln;
    }
    if (!first_done) { close(fd); return fail(err, en, "No track table in the Nero image", 0); }
    D.dfd = fd;
    return 0;
}

static int msf(const char *s) { int m, sec, fr; return sscanf(s, "%d:%d:%d", &m, &sec, &fr) == 3 ? (m * 60 + sec) * 75 + fr : -1; }

static int open_cue(const char *p, char *err, size_t en) {
    FILE *f = fopen(p, "r"); if (!f) return fail(err, en, "Can't open %s", p);
    char base[1024]; snprintf(base, sizeof base, "%s", p);
    char *sl = strrchr(base, '/'); if (sl) *sl = 0; else strcpy(base, ".");
    struct { char file[1024]; char type[16]; int idx0, idx1; } tr[MAXTRK]; int nt = 0;
    char cur[1024] = "", line[1200];
    while (fgets(line, sizeof line, f)) {
        char *s = line; while (isspace((unsigned char)*s)) s++;
        if (!strncasecmp(s, "FILE", 4)) {
            char *q1 = strchr(s, '"'), *q2 = q1 ? strchr(q1 + 1, '"') : 0, name[1024];
            if (q1 && q2) { snprintf(name, sizeof name, "%.*s", (int)(q2 - q1 - 1), q1 + 1); }
            else if (sscanf(s + 4, " %1023s", name) != 1) continue;
            const char *bn = strrchr(name, '/') ? strrchr(name, '/') + 1 : strrchr(name, '\\') ? strrchr(name, '\\') + 1 : name;
            if (!find_ci(base, bn, cur, sizeof cur)) { fclose(f); return fail(err, en, "The cue sheet refers to %s, which isn't next to it", bn); }
        } else if (!strncasecmp(s, "TRACK", 5) && nt < MAXTRK) {
            int n; char type[32];
            if (sscanf(s + 5, "%d %31s", &n, type) == 2) {
                snprintf(tr[nt].file, sizeof tr[nt].file, "%s", cur); snprintf(tr[nt].type, sizeof tr[nt].type, "%s", type);
                tr[nt].idx0 = tr[nt].idx1 = -1; nt++;
            }
        } else if (!strncasecmp(s, "INDEX", 5) && nt) {
            int n; char t[32];
            if (sscanf(s + 5, "%d %31s", &n, t) == 2) { if (n == 0) tr[nt - 1].idx0 = msf(t); if (n == 1) tr[nt - 1].idx1 = msf(t); }
        }
    }
    fclose(f);
    if (!nt) return fail(err, en, "No tracks in the cue sheet", 0);
    for (int k = 0; k < nt; k++) {
        uint32_t ssz = !strcasecmp(tr[k].type, "MODE1/2048") ? 2048 : !strcasecmp(tr[k].type, "MODE2/2336") ? 2336 : 2352;
        int start = tr[k].idx1 >= 0 ? tr[k].idx1 : tr[k].idx0 >= 0 ? tr[k].idx0 : 0;
        uint64_t end;
        if (k + 1 < nt && !strcmp(tr[k + 1].file, tr[k].file))
            end = (uint64_t)(tr[k + 1].idx0 >= 0 ? tr[k + 1].idx0 : tr[k + 1].idx1) * ssz;
        else end = fsize(tr[k].file) / ssz * ssz;
        uint64_t off = (uint64_t)start * ssz;
        if (end <= off) continue;
        if (k == 0) {
            if (!strcasecmp(tr[k].type, "AUDIO")) return fail(err, en, "The first track is audio; this isn't a game disc", 0);
            D.dfd = open(tr[k].file, O_RDONLY); if (D.dfd < 0) return fail(err, en, "Can't open %s", tr[k].file);
            D.doff = off; D.dsz = ssz; D.dsectors = (uint32_t)((end - off) / ssz);
            D.ddata = ssz == 2048 ? 0 : ssz == 2336 ? 8 : !strcasecmp(tr[k].type, "MODE2/2352") ? 24 : 16;
        } else if (!strcasecmp(tr[k].type, "AUDIO")) add_audio(tr[k].file, off, end - off, 0);
    }
    return D.dfd >= 0 ? 0 : fail(err, en, "No data track in the cue sheet", 0);
}

static int open_iso(const char *p, char *err, size_t en) {
    D.dfd = open(p, O_RDONLY); if (D.dfd < 0) return fail(err, en, "Can't open %s", p);
    D.doff = 0; D.dsz = 2048; D.ddata = 0; D.dsectors = (uint32_t)(fsize(p) / 2048);
    return 0;
}

// WAV made by tools/extract.py: find the "data" chunk.
static int wav_data(const char *p, uint64_t *off, uint64_t *bytes) {
    int fd = open(p, O_RDONLY); if (fd < 0) return -1;
    uint8_t h[12]; uint64_t pos = 12, sz = fsize(p); int ok = -1;
    if (pread(fd, h, 12, 0) == 12 && !memcmp(h, "RIFF", 4) && !memcmp(h + 8, "WAVE", 4))
        while (pos + 8 <= sz) {
            uint8_t c[8]; if (pread(fd, c, 8, (off_t)pos) != 8) break;
            uint32_t ln = le32(c + 4);
            if (!memcmp(c, "data", 4)) { *off = pos + 8; *bytes = ln; ok = 0; break; }
            pos += 8 + ln + (ln & 1);
        }
    close(fd);
    return ok;
}

static int open_folder(const char *p, char *err, size_t en) {
    char exe[1100];
    snprintf(D.dir, sizeof D.dir, "%s/cd", p);
    if (!find_ci(D.dir, "SKIPIT.EXE", exe, sizeof exe)) {
        snprintf(D.dir, sizeof D.dir, "%s", p);                       // the game files directly in the folder
        if (!find_ci(D.dir, "SKIPIT.EXE", exe, sizeof exe)) return fail(err, en, "No SKIPIT.EXE in %s", p);
    }
    char iso[1100]; snprintf(iso, sizeof iso, "%s/track01.iso", p);
    D.dsectors = fsize(iso) ? (uint32_t)(fsize(iso) / 2048) : 29428;
    for (int t = 2; t < MAXTRK; t++) {
        char w[1100], name[32]; snprintf(name, sizeof name, "track%02d.wav", t);
        uint64_t off, bytes;
        if (!find_ci(p, name, w, sizeof w) || wav_data(w, &off, &bytes)) break;
        add_audio(w, off, bytes, 0);
    }
    return 0;
}

static int check_folder_exe(char *err, size_t en) {
    char exe[1100]; find_ci(D.dir, "SKIPIT.EXE", exe, sizeof exe);
    FILE *f = fopen(exe, "rb"); if (!f) return fail(err, en, "Can't read %s", exe);
    uint8_t buf[65536]; size_t n; uint64_t h = 0xcbf29ce484222325ull, total = 0;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) { h = fnv(h, buf, n); total += n; }
    fclose(f);
    if (h != GUEST_EXE_HASH) return fail(err, en, "This SKIPIT.EXE is a different version from the one this build was made from", 0);
    return 0;
}

void disc_close(void) {
    if (D.dfd >= 0) close(D.dfd);
    memset(&D, 0, sizeof D); D.dfd = -1;
}

int disc_open(const char *path, char *err, size_t en) {
    disc_close();
    if (!path || !*path) return fail(err, en, "No disc image selected", 0);
    if (access(path, R_OK)) return fail(err, en, "Not found: %s", path);
    snprintf(D.path, sizeof D.path, "%s", path);
    int rc;
    const char *x = ext_of(path);
    if (is_dir(path)) { rc = open_folder(path, err, en); if (!rc) rc = check_folder_exe(err, en); if (!rc) D.kind = SRC_FOLDER; return rc; }
    if (!strcasecmp(x, ".cue")) rc = open_cue(path, err, en);
    else if (!strcasecmp(x, ".nrg")) rc = open_nrg(path, err, en);
    else if (!strcasecmp(x, ".iso")) rc = open_iso(path, err, en);
    else if (!strcasecmp(x, ".bin") || !strcasecmp(x, ".img")) {
        char cue[1100]; snprintf(cue, sizeof cue, "%.*s.cue", (int)(x - path), path);
        if (access(cue, R_OK)) return fail(err, en, "Select the .cue file that goes with this .bin", 0);
        rc = open_cue(cue, err, en);
    } else return fail(err, en, "Unsupported file type %s (use .cue, .nrg or .iso)", x);
    if (rc) { disc_close(); return rc; }
    struct find_exe fe = {0};
    if (iso_walk(exe_cb, &fe) < 0 && !fe.found) { disc_close(); return fail(err, en, "Couldn't read the CD's file system", 0); }
    if (!fe.found) { disc_close(); return fail(err, en, "This isn't the Quarantine CD (no SKIPIT.EXE)", 0); }
    if (fe.hash != GUEST_EXE_HASH) { disc_close(); return fail(err, en, "This disc has a different version of Quarantine than this build was made from", 0); }
    D.kind = SRC_IMAGE;
    return 0;
}

int disc_kind(void) { return D.kind; }
int disc_has_music(void) { return D.naudio > 0; }
uint32_t disc_data_sectors(void) { return D.dsectors ? D.dsectors : 29428; }
const char *disc_data_dir(void) { return D.kind == SRC_FOLDER ? D.dir : 0; }
int disc_audio_tracks(cd_track_src *out, int max) {
    int n = D.naudio < max ? D.naudio : max;
    memcpy(out, D.audio, sizeof *out * n);
    return n;
}

// ---------------------------------------------------------------- install
static void marker_path(const char *dest, char *out, size_t n) { snprintf(out, n, "%s/.installed", dest); }

int disc_is_installed(const char *dest) {
    char m[1100]; marker_path(dest, m, sizeof m);
    FILE *f = fopen(m, "r"); if (!f) return 0;
    unsigned long long h = 0; int ok = fscanf(f, "%llx", &h) == 1 && h == GUEST_EXE_HASH;
    fclose(f);
    return ok;
}

struct inst { const char *dest; int done, total; void (*progress)(int, int, void *); void *u; char *err; size_t en; };
static int count_cb(const char *name, uint32_t lba, uint32_t size, void *u) { (void)name; (void)lba; (void)size; ((struct inst *)u)->total++; return 0; }
static int copy_cb(const char *name, uint32_t lba, uint32_t size, void *u) {
    struct inst *in = u;
    char p[1200]; snprintf(p, sizeof p, "%s/%s", in->dest, name);
    FILE *f = fopen(p, "wb"); if (!f) { fail(in->err, in->en, "Can't write %s", p); return -1; }
    uint8_t sec[2048];
    for (uint32_t done = 0; done < size; done += 2048, lba++) {
        if (read_sector(lba, sec)) { fclose(f); fail(in->err, in->en, "Read error in the disc image", 0); return -1; }
        fwrite(sec, 1, size - done < 2048 ? size - done : 2048, f);
    }
    if (fclose(f)) { fail(in->err, in->en, "Can't write %s (disk full?)", p); return -1; }
    in->done++;
    if (in->progress) in->progress(in->done, in->total, in->u);
    return 0;
}

int disc_install(const char *dest, void (*progress)(int, int, void *), void *u, char *err, size_t en) {
    if (D.kind != SRC_IMAGE) return 0;                    // folders are used in place
    if (disc_is_installed(dest)) return 0;
    mkdir(dest, 0755);
    char m[1100]; marker_path(dest, m, sizeof m); unlink(m);
    struct inst in = { dest, 0, 0, progress, u, err, en };
    iso_walk(count_cb, &in);
    if (iso_walk(copy_cb, &in)) return -1;
    FILE *f = fopen(m, "w"); if (!f) return fail(err, en, "Can't write %s", m);
    fprintf(f, "%016llx\n", (unsigned long long)GUEST_EXE_HASH);
    fclose(f);
    return 0;
}
