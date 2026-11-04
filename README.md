# Quarantine (1994, DOS) — static recompilation

The DOS CD version's `SKIPIT.EXE` (Watcom C/C++32, DOS/4GW) is translated instruction-by-instruction
into C and linked against a small runtime that emulates DOS, DPMI, VGA Mode X, the PIT/PIC, the keyboard
and an MSCDEX CD-ROM drive with CD audio. The result is a native program; no emulator is involved.

**This repository and the downloads contain no game data.** You need your own copy of the Quarantine DOS
CD-ROM; the program loads the game from your disc image.

## Download and play
Get the latest build for your system from the [Releases page](https://github.com/ACR-GD/quarantine-recomp/releases):

| System | File | Notes |
|---|---|---|
| Windows 10/11 (x64) | `Quarantine-<version>-Windows-x64.zip` | run `Quarantine.exe`; on the SmartScreen warning choose *More info → Run anyway* |
| macOS 11+ (Apple silicon and Intel) | `Quarantine-<version>-macOS.zip` | first launch: right-click `Quarantine.app` → *Open* (or *System Settings → Privacy & Security → Open Anyway*) |
| Linux x86-64 (glibc 2.35+) | `Quarantine-<version>-Linux-x64.tar.gz` | run `./Quarantine` in the extracted folder |

**You need a disc image of the Quarantine DOS CD** made from your own disc:

| Format | Game | CD music |
|---|---|---|
| `.cue` + `.bin` (ImgBurn, cdrdao, CDRWIN, ...) | yes | yes |
| Nero `.nrg` | yes | yes |
| `.iso` | yes | no (an ISO holds only the data track) |

The build accepts the version of the game whose `SKIPIT.EXE` is 572,501 bytes (the launcher checks it and
says so if your disc is a different version).

**First start.** The launcher opens: choose **Disc image** and pick your `.cue`/`.nrg`/`.iso` (or drop it on
the window), then **Start game**. On the first start the game's files (60 MB) are copied from the image into
your user folder; the music is played straight from the image, so leave it where it is. The image is only
read, never changed.

**Settings** (in the launcher): window/fullscreen (also F11 in game), picture filter (also F10 in game),
4:3 or square pixels, music and effects volume, the 70 Hz frame limit, copy protection, and whether the
launcher shows at startup (if it's off, hold Shift while starting, or run with `--launcher`).

Picture filters: **Sharp** (nearest neighbour), **Smooth** (bilinear), **Crisp** (integer upscale, then
bilinear for the remainder: square pixels of equal size at any window size), **CRT** (scanlines, beam
bloom, aperture-grille mask; best in fullscreen or a large window), **Scale4x** (AdvMAME edge smoothing).

Settings, the installed data and saved games live in the per-user folder:
Windows `%APPDATA%\quarantine-recomp\Quarantine\`,
macOS `~/Library/Application Support/quarantine-recomp/Quarantine/`,
Linux `~/.local/share/quarantine-recomp/Quarantine/`
(`config.ini`, `data/`, `save/`). Delete the folder to start over.

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

## Building from source
The translator runs on your machine against your copy of the game, so the generated C (`gen/`) is never
part of the repository.

**1. Install the tools**

| | |
|---|---|
| macOS | `brew install sdl3 python` and the Xcode command-line tools (`xcode-select --install`) |
| Debian/Ubuntu | `sudo apt install clang make python3 python3-venv pkg-config` plus SDL3 (`libsdl3-dev` on recent releases, otherwise build SDL3 from source) |
| Windows | build with the release script below (MinGW-w64 in Docker), or MSYS2 with `mingw-w64-x86_64-SDL3` |

The translator needs the `capstone` disassembler for Python:

    python3 -m venv .venv && .venv/bin/pip install capstone

**2. Translate and build.** The build needs only the game's executable from the disc:

    python3 tools/extract.py --exe-only /path/to/Quarantine.cue   # writes extracted/cd/SKIPIT.EXE
    make gen PYTHON=.venv/bin/python     # translate SKIPIT.EXE into C (gen/), ~5 s
    make -j8 OPT=-O2                     # build ./quarantine
    ./quarantine

If SDL3 isn't found, point the build at it: `make SDL_PREFIX=/path/to/sdl3`. To use gcc: `make CC=gcc`.
A repository checkout with a full `extracted/` folder (`python3 tools/extract.py <image>`, which also writes
the music as WAV files) uses it automatically instead of asking for a disc image.

**Release packages** (written to `dist/`, after step 2):

    release/build-macos.sh     # universal Quarantine.app with SDL3.framework (needs Xcode tools)
    release/build-windows.sh   # Quarantine.exe + SDL3.dll, cross-compiled with MinGW-w64 in Docker
    release/build-linux.sh     # x86-64 binary + libSDL3.so.0, built on Ubuntu 22.04 in Docker

They download SDL3 (version pinned in the scripts) into `release/cache/`. The version number comes from
`VERSION` and shows in the launcher's corner. `release/README-player.txt` is the readme inside the packages.

**Layout**
- `tools/extract.py` — CD image extractor (.cue/.bin or .nrg; ISO 9660 + CD audio to WAV; `--exe-only` for the build)
- `tools/le.py` — LE loader (objects, fixups, rebasing)
- `tools/recomp.py` — the recompiler: function discovery + x86 → C emitter, writes `gen/`
- `tools/difftest.py` — differential test of every instruction form against Unicorn
  (`tools/.venv/bin/python -I tools/difftest.py extracted/cd/SKIPIT.EXE`)
- `runtime/` — `rt.h` (CPU state, lazy flags, ALU), `rt_core.c` (dispatch, interrupts, string ops, x87),
  `rt_sys.c` (DOS/DPMI/BIOS, VGA, ports, SDL frontend), `rt_cd.c` (MSCDEX + CD audio),
  `rt_snd.c` (HMI SOS digital audio: native mixer behind the hooked SOS API), `rt_pad.c` (controllers),
  `rt_disc.c` (reads .cue/.bin, .nrg, .iso images directly), `rt_le.c` (loads SKIPIT.EXE from the player's
  files at startup), `rt_config.c` (config.ini), `rt_launcher.c` (launcher), `rt_filter.c` (picture filters),
  `compat.c` (Windows shims)
- `release/` — packaging scripts
- `analysis/` — Ghidra project and exports (not in the repository)

**Environment knobs** (for development; they override the launcher): `QUAR_DATA` (use this folder of game
files as is), `QUAR_SAVE` (save folder), `QUAR_CDAUDIO` (music folder with `QUAR_DATA`), `QUAR_CONFIG_DIR`
(settings folder), `QUAR_LAUNCHER_TEST` (scripted launcher input, see `rt_launcher.c`), `QUAR_HEADLESS=1`,
`QUAR_DUMP=<dir>` (frame dumps), `QUAR_KEYS="ms:scancode,..."` (scripted input), `QUAR_SHOT="frame:file.bmp"`
(window screenshot), `QUAR_VERBOSE=1`, `QUAR_MAXFRAMES=n`, `QUAR_COPYPROT=1` (show the manual-lookup
copy-protection screen; skipped by default), `QUAR_NOSOUND=1` (mute sound effects), `QUAR_SNDDUMP=file.wav`
(headless: write the effects mix to a WAV), `QUAR_NOVSYNC=1` (don't pace page flips to 70 Hz),
`QUAR_WATCH=hexaddr` (log a guest dword twice a second). Debug build with call tracing and stack checks:
`make OPT="-O1 -DRT_DEBUG"`.

## Translation model notes
- `SKIPIT.EXE` is read from the player's game files at startup and relocated by `rt_le.c` (checked against
  the FNV-1a hash recorded at translation time), so binaries carry none of the original program's bytes.
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
