# Quarantine static recompilation build.
CC      = clang
OPT    ?= -O1
PYTHON ?= python3

# SDL3: pkg-config if available (Linux, MSYS2), else Homebrew's prefix.
ifeq ($(shell pkg-config --exists sdl3 2>/dev/null && echo yes),yes)
SDL_CFLAGS := $(shell pkg-config --cflags sdl3)
SDL_LIBS   := $(shell pkg-config --libs sdl3)
else
SDL_PREFIX ?= $(shell brew --prefix sdl3 2>/dev/null || echo /opt/homebrew/opt/sdl3)
SDL_CFLAGS := -I$(SDL_PREFIX)/include
SDL_LIBS   := -L$(SDL_PREFIX)/lib -lSDL3
endif

CFLAGS  = $(OPT) -g -std=c11 -D_DEFAULT_SOURCE -D_GNU_SOURCE -D_DARWIN_C_SOURCE -DUSE_SDL -Wno-unused-label \
          -Wno-unused-variable -Wno-parentheses-equality -Wno-unused-value -fno-strict-aliasing -fwrapv \
          $(SDL_CFLAGS)
LDLIBS  = $(SDL_LIBS) -lm -lpthread

GEN_SRC = $(wildcard gen/*.c)
RT_SRC  = runtime/rt_core.c runtime/rt_sys.c runtime/rt_cd.c runtime/rt_snd.c runtime/rt_pad.c runtime/rt_disc.c runtime/rt_config.c runtime/rt_launcher.c runtime/main.c
OBJ     = $(patsubst %.c,build/%.o,$(GEN_SRC) $(RT_SRC))

quarantine: $(OBJ)
	$(CC) -o $@ $(OBJ) $(LDLIBS)

build/%.o: %.c runtime/rt.h runtime/sys.h runtime/disc.h gen/image_info.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

gen: tools/recomp.py tools/le.py
	$(PYTHON) -I tools/recomp.py extracted/cd/SKIPIT.EXE gen

clean:
	rm -rf build quarantine
.PHONY: gen clean
