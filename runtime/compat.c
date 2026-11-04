// See compat.h.
#include "compat.h"
#include <time.h>
#ifdef USE_SDL
#include <SDL3/SDL.h>
#endif

void host_sleep_ns(uint64_t ns) {
#if defined(_WIN32) && defined(USE_SDL)
    SDL_DelayNS(ns);                       // uses a high-resolution waitable timer on Windows 10+
#else
    struct timespec ts = { (time_t)(ns / 1000000000u), (long)(ns % 1000000000u) };
    nanosleep(&ts, 0);
#endif
}

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <ctype.h>
#include <stdlib.h>

ssize_t pread(int fd, void *buf, size_t n, off_t off) {
    HANDLE h = (HANDLE)_get_osfhandle(fd);
    if (h == INVALID_HANDLE_VALUE) return -1;
    OVERLAPPED ov = {0};
    uint64_t o = (uint64_t)off;
    ov.Offset = (DWORD)o; ov.OffsetHigh = (DWORD)(o >> 32);
    DWORD got = 0;
    if (!ReadFile(h, buf, (DWORD)n, &got, &ov)) return GetLastError() == ERROR_HANDLE_EOF ? 0 : -1;
    return (ssize_t)got;
}

char *realpath(const char *path, char *resolved) { return _fullpath(resolved, path, 1024); }

// DOS-style wildcard match (* and ?), optionally case-insensitive.
int fnmatch(const char *p, const char *s, int flags) {
    int ci = flags & FNM_CASEFOLD;
    const char *star = 0, *ss = 0;
    while (*s) {
        if (*p == '*') { star = ++p; ss = s; continue; }
        if (*p == '?' || (ci ? tolower((unsigned char)*p) == tolower((unsigned char)*s) : *p == *s)) { p++; s++; continue; }
        if (star) { p = star; s = ++ss; continue; }
        return 1;
    }
    while (*p == '*') p++;
    return *p ? 1 : 0;
}
#endif
