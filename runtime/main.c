// Entry point: load the relocated LE image into guest memory and run the recompiled cstart.
#include "rt.h"
#include "sys.h"
#include "../gen/image_info.h"
#include <stdio.h>
#include <stdlib.h>

static void load_image(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) rt_fatal("cannot open %s", path);
    uint32_t hdr[2];
    while (fread(hdr, 4, 2, f) == 2) {
        if (fread(MEM + hdr[0], 1, hdr[1], f) != hdr[1]) rt_fatal("short image");
    }
    fclose(f);
}

int main(int argc, char **argv) {
    extern int rt_verbose; rt_verbose = getenv("QUAR_VERBOSE") != 0;
    rt_skip_copyprot = !(getenv("QUAR_COPYPROT") && atoi(getenv("QUAR_COPYPROT")));
    MEM = calloc(1, MEM_SIZE + 16);
    const char *img = getenv("QUAR_IMAGE") ? getenv("QUAR_IMAGE") : "gen/image.bin";
    load_image(img);
    sys_init(argc, argv);
    ESP = GUEST_ESP;
    PUSH32(MAGIC_FARRET);
    rt_dispatch_tail_noret(GUEST_ENTRY);
    fprintf(stderr, "guest returned from entry\n");
    sys_shutdown();
    return 0;
}
