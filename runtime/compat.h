// Small portability layer: the runtime is written against POSIX; on Windows (MinGW-w64) the few
// missing pieces are provided here (runtime/compat.c).
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <fcntl.h>
#include <sys/types.h>
#ifndef O_BINARY
#define O_BINARY 0
#endif

void host_sleep_ns(uint64_t ns);          // precise-ish sleep (1 ms timer resolution on Windows)

#ifdef _WIN32
#include <io.h>
#include <direct.h>
#define mkdir(p, mode) _mkdir(p)
ssize_t pread(int fd, void *buf, size_t n, off_t off);
char *realpath(const char *path, char *resolved);
#define FNM_CASEFOLD 16
int fnmatch(const char *pat, const char *s, int flags);
#else
#include <fnmatch.h>
#endif
