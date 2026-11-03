# Quarantine (1994, DOS) — static recompilation

The DOS CD version's `SKIPIT.EXE` (Watcom C/C++32, DOS/4GW) is translated instruction-by-instruction
into C and linked against a small runtime that emulates DOS, DPMI, VGA Mode X, the PIT/PIC, the keyboard
and an MSCDEX CD-ROM drive with CD audio. The result is a native program; no emulator is involved.

**This repository contains no game data.** You need your own copy of the Quarantine DOS CD-ROM; the
game's executable is translated on your machine during setup.

## Layout
- `extracted/` — your CD's files (`cd/`) and audio tracks (`track02–12.wav`), made by `tools/extract.py`
- `tools/extract.py` — CD image extractor (.cue/.bin or .nrg; ISO 9660 + CD audio to WAV)
- `tools/le.py` — LE loader (objects, fixups, rebasing)
- `tools/recomp.py` — the recompiler: function discovery + x86 → C emitter, writes `gen/`
- `tools/difftest.py` — differential test of every instruction form against Unicorn
  (`tools/.venv/bin/python -I tools/difftest.py extracted/cd/SKIPIT.EXE`)
- `runtime/` — `rt.h` (CPU state, lazy flags, ALU), `rt_core.c` (dispatch, interrupts, string ops, x87),
  `rt_sys.c` (DOS/DPMI/BIOS, VGA, ports, SDL frontend), `rt_cd.c` (MSCDEX + CD audio),
  `rt_snd.c` (HMI SOS digital audio: native mixer behind the hooked SOS API), `rt_pad.c` (controllers)
- `analysis/` — Ghidra project and exports

## Setup
Tested on macOS (Apple Silicon). Linux should work with the same steps but hasn't been tried; Windows is
untested (MSYS2 with SDL3 and clang is the likely route).

**1. Install the tools**

| | |
|---|---|
| macOS | `brew install sdl3 python` and the Xcode command-line tools (`xcode-select --install`) |
| Debian/Ubuntu | `sudo apt install clang make python3 python3-venv pkg-config` plus SDL3 (`libsdl3-dev` on recent releases, otherwise build SDL3 from source) |

The translator needs the `capstone` disassembler for Python:

    python3 -m venv .venv && .venv/bin/pip install capstone

**2. Image your CD.** Any rip that keeps the audio tracks works; the music is CD audio.
`tools/extract.py` reads a cue sheet with its `.bin` file(s) (e.g. from ImgBurn, cdrdao or
`bchunk`-style rips) or a Nero `.nrg` image. A plain `.iso` is not enough: it has no music.

**3. Extract the image** into `extracted/` (game files in `extracted/cd/`, music in `extracted/trackNN.wav`):

    python3 tools/extract.py /path/to/Quarantine.cue      # or .nrg

**4. Translate and build**

    make gen PYTHON=.venv/bin/python     # translate SKIPIT.EXE into C (gen/), ~5 s
    make -j8 OPT=-O2                     # build ./quarantine

If SDL3 isn't found, point the build at it: `make SDL_PREFIX=/path/to/sdl3`. To use gcc: `make CC=gcc`.

**5. Play**

    ./quarantine                         # run from the repository directory

The manual-lookup copy protection is skipped (`QUAR_COPYPROT=1` brings it back). Saved games and any file
the game modifies go to `save/`; `extracted/` is never written to.

Environment knobs: `QUAR_DATA` (game files, default `extracted/cd`), `QUAR_SAVE` (writable dir, default `save`),
`QUAR_CDAUDIO` (default `extracted`), `QUAR_HEADLESS=1`, `QUAR_DUMP=<dir>` (frame dumps),
`QUAR_KEYS="ms:scancode,..."` (scripted input), `QUAR_VERBOSE=1`, `QUAR_MAXFRAMES=n`,
`QUAR_COPYPROT=1` (show the manual-lookup copy-protection screen; skipped by default),
`QUAR_NOSOUND=1` (mute sound effects), `QUAR_SNDDUMP=file.wav` (headless: write the effects mix to a WAV),
`QUAR_NOVSYNC=1` (don't pace page flips to 70 Hz), `QUAR_WATCH=hexaddr` (log a guest dword twice a second).
Debug build with call tracing and stack checks: `make OPT="-O1 -DRT_DEBUG"`.

## Controls
Keyboard as in the original. Any SDL3-supported controller (Xbox, PlayStation, Switch Pro, 8BitDo, ...) also
works and can be plugged in at any time:

| Input | Menus | Driving |
|---|---|---|
| Left stick / D-pad | arrows | steer, accelerate, brake |
| RT / LT | – | accelerate / brake-reverse |
| A | Enter | fire (Ctrl) |
| B | Esc | nitro (Tab) |
| X | – | cycle weapon (Alt) |
| Y | Space | special item (Space) |
| LB / RB | – | E / Z |
| Start | Enter | game menu (Esc) |
| Back / Select | Esc | map (M) |
| L3 / R3 | – | horn (Enter) / cycle camera (F1–F3) |

The driving column applies while the game's in-game key handler is running (hooked at 0x12cb64). Each button
releases the key it pressed, even if the context changed meanwhile. `QUAR_PADTEST="ms:input:value,..."` drives
a virtual pad for testing (see `runtime/rt_pad.c`).

## Translation model notes
- Objects are loaded 0x100000 above their preferred bases (the data object would otherwise overlap VGA RAM).
- Code also lives in the data object (hand-written Mode X routines); it is discovered through branches.
- The span renderers patch a `ret` into unrolled loops; those loops become multi-entry "region" functions
  that check for the patched `0xC3` at every patch point.
- A shadow return stack turns `push addr; ret` tricks (Watcom `int386x`) into dispatches.
- `iret` to the IRQ return marker longjmps back to the delivery point, which handles `_chain_intr`.
- `HOOKS` in `recomp.py` inserts C at a function's entry. It is used once: the copy-protection screen
  (0x147c44) returns "correct" (EAX=1) immediately unless `QUAR_COPYPROT=1`.
- Sound effects: the SOS library would load a driver from `HMIDRV.386` at run time (code the recompiler
  never sees), so its API entry points (init/detect/driver, start/stop/continue sample, set pitch, and the
  driver's timer event) are hooked and served by `rt_snd.c`. Sample-done callbacks (used to stream VOC files
  in 4 KB chunks) run on the main thread from the IRQ check, like the driver's ISR would. A `SOUND.DAT`
  selecting a card is written to the save directory on first run (the CD's copy says "no sound").
- CPU use: the game never waits for retrace and would flip pages ~2000 times a second. Page flips (CRTC
  start-address writes) are paced to the VGA's 70 Hz; the game clamps its own frame-time estimate at 70 fps,
  so game speed is unchanged. Pure polling loops (wait for key, wait N ticks, FLI frame delays, wait for the
  voice stream) are listed in `IDLE` in recomp.py and sleep 0.5 ms per pass. Because the frame's idle time
  now sits in the flip, keyboard interrupts are held until mid-frame: the menus call `kbhit()` (which
  consumes the key) at the top of their loop and check Enter/arrows later in the frame.
