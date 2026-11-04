# Changelog

## 0.1.0 — first release

The whole game runs natively: the DOS executable is translated to C, with no emulator.

- **Launcher**: pick your disc image (.cue/.bin, .nrg or .iso, or drag it onto the window); it checks the
  disc is the right version, installs the game's files on first start, and plays the CD music straight
  from the image. Works with mouse, keyboard and controller.
- **Settings**: window or fullscreen (F11), picture filters (Sharp, Smooth, Crisp, CRT, Scale4x; F10),
  4:3 or square pixels, music and effects volume, 70 Hz frame limit, optional copy-protection screen.
- **Sound**: sound effects and CD music.
- **Controllers**: Xbox, PlayStation, Switch Pro and other SDL-supported pads, with menu and driving layouts.
- **Low CPU use**: the game's busy-waits sleep instead of spinning.
- **Platforms**: Windows x64, macOS 11+ (universal), Linux x86-64.

Known limitations:
- Only the version of the game whose `SKIPIT.EXE` is 572,501 bytes is supported.
- The Windows and Linux builds have been checked by automated runs (Wine, a container), not yet by players
  on real machines; the macOS build's Intel half hasn't been run. Reports welcome.
- No analog steering: the stick acts like the arrow keys, as in the original.
