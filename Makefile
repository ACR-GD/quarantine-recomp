# Quarantine static recompilation build.
CC      = clang
OPT    ?= -O1
CFLAGS  = $(OPT) -g -std=c11 -D_DEFAULT_SOURCE -D_DARWIN_C_SOURCE -DUSE_SDL -Wno-unused-label -Wno-unused-variable \
          -Wno-parentheses-equality -Wno-unused-value -fno-strict-aliasing -fwrapv \
          -I/opt/homebrew/opt/sdl3/include
LDLIBS  = -L/opt/homebrew/opt/sdl3/lib -lSDL3 -lm -lpthread

GEN_SRC = $(wildcard gen/*.c)
RT_SRC  = runtime/rt_core.c runtime/rt_sys.c runtime/rt_cd.c runtime/rt_snd.c runtime/rt_pad.c runtime/main.c
OBJ     = $(patsubst %.c,build/%.o,$(GEN_SRC) $(RT_SRC))

quarantine: $(OBJ)
	$(CC) -o $@ $(OBJ) $(LDLIBS)

build/%.o: %.c runtime/rt.h runtime/sys.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

gen: tools/recomp.py tools/le.py
	python3 -I tools/recomp.py extracted/cd/SKIPIT.EXE gen

clean:
	rm -rf build quarantine
.PHONY: gen clean
