// Entry point: set up the host (sys_init picks the game files and loads SKIPIT.EXE from them, rt_le.c),
// then run the recompiled cstart.
#include "rt.h"
#include "sys.h"
#include "../gen/image_info.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef USE_SDL
#include <SDL3/SDL_main.h>     // provides WinMain on Windows
#endif

int main(int argc, char **argv) {
    extern int rt_verbose; rt_verbose = getenv("QUAR_VERBOSE") != 0;
    MEM = calloc(1, MEM_SIZE + 16);
    sys_init(argc, argv);
    ESP = GUEST_ESP;
    PUSH32(MAGIC_FARRET);
    rt_dispatch_tail_noret(GUEST_ENTRY);
    fprintf(stderr, "guest returned from entry\n");
    sys_shutdown();
    return 0;
}
