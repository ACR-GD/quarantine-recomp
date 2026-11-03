// Entry point: load the relocated LE image into guest memory and run the recompiled cstart.
#include "rt.h"
#include "sys.h"
#include "../gen/image_info.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// The relocated objects of SKIPIT.EXE, embedded by tools/recomp.py (gen/image_data.c).
extern const uint8_t GUEST_IMAGE[];
extern const uint32_t GUEST_IMAGE_SIZE;
static void load_image(void) {
    for (uint32_t p = 0; p + 8 <= GUEST_IMAGE_SIZE;) {
        uint32_t base, size;
        memcpy(&base, GUEST_IMAGE + p, 4); memcpy(&size, GUEST_IMAGE + p + 4, 4); p += 8;
        memcpy(MEM + base, GUEST_IMAGE + p, size); p += size;
    }
}

int main(int argc, char **argv) {
    extern int rt_verbose; rt_verbose = getenv("QUAR_VERBOSE") != 0;
    MEM = calloc(1, MEM_SIZE + 16);
    load_image();
    sys_init(argc, argv);
    ESP = GUEST_ESP;
    PUSH32(MAGIC_FARRET);
    rt_dispatch_tail_noret(GUEST_ENTRY);
    fprintf(stderr, "guest returned from entry\n");
    sys_shutdown();
    return 0;
}
