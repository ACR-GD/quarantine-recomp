# Quarantine (1994, DOS) — static recompilation

The DOS CD version's `SKIPIT.EXE` (Watcom C/C++32, DOS/4GW) is translated instruction-by-instruction
into C and linked against a small runtime that emulates DOS, DPMI, VGA Mode X, the PIT/PIC, the keyboard
and an MSCDEX CD-ROM drive with CD audio. It's native code on macOS/Linux/Windows; no emulator is involved.

## Layout
- `extracted/` — data pulled from `Quarantine.nrg` (`cd/` files, `track02–12.wav` CD audio)
- `tools/le.py` — LE loader (objects, fixups, rebasing)
- `tools/recomp.py` — the recompiler: function discovery + x86 → C emitter, writes `gen/`
- `tools/difftest.py` — differential test of every instruction form against Unicorn
  (`tools/.venv/bin/python -I tools/difftest.py extracted/cd/SKIPIT.EXE`)
- `runtime/` — `rt.h` (CPU state, lazy flags, ALU), `rt_core.c` (dispatch, interrupts, string ops, x87),
  `rt_sys.c` (DOS/DPMI/BIOS, VGA, ports, SDL frontend), `rt_cd.c` (MSCDEX + CD audio),
  `rt_snd.c` (HMI SOS digital audio: native mixer behind the hooked SOS API)
- `analysis/` — Ghidra project and exports

## Build & run
    make gen          # regenerate C from SKIPIT.EXE
    make -j12 OPT=-O2
    ./quarantine      # run from this directory

Environment knobs: `QUAR_DATA` (game files, default `extracted/cd`), `QUAR_SAVE` (writable dir, default `save`),
`QUAR_CDAUDIO` (default `extracted`), `QUAR_HEADLESS=1`, `QUAR_DUMP=<dir>` (frame dumps),
`QUAR_KEYS="ms:scancode,..."` (scripted input), `QUAR_VERBOSE=1`, `QUAR_MAXFRAMES=n`,
`QUAR_COPYPROT=1` (show the manual-lookup copy-protection screen; skipped by default),
`QUAR_NOSOUND=1` (mute sound effects), `QUAR_SNDDUMP=file.wav` (headless: write the effects mix to a WAV),
`QUAR_NOVSYNC=1` (don't pace page flips to 70 Hz), `QUAR_WATCH=hexaddr` (log a guest dword twice a second).
Debug build with call tracing and stack checks: `make OPT="-O1 -DRT_DEBUG"`.

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
