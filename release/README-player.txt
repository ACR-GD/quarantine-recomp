QUARANTINE (1994) - native port by static recompilation
https://github.com/ACR-GD/quarantine-recomp

This is an unofficial, fan-made native version of the 1994 DOS game Quarantine. It runs the original
game's code, translated to run directly on your computer, with no emulator.

THIS DOWNLOAD CONTAINS NO GAME FILES. You need your own copy of the Quarantine CD-ROM for DOS.


WHAT YOU NEED
-------------
A disc image of the Quarantine DOS CD (GameTek, 1994), made from your own disc, in one of these formats:
  - a .cue sheet with its .bin file(s)   (ImgBurn, cdrdao, CDRWIN...)   -> game + CD music
  - a Nero .nrg image                                                  -> game + CD music
  - an .iso                                                            -> game only, no music
The music is played from the disc's audio tracks, so choose a format that keeps them (.cue/.bin or
.nrg). This build works with the version of the game whose SKIPIT.EXE is 572,501 bytes; the launcher
tells you if your disc is a different version.


STARTING THE GAME
-----------------
Windows:  unzip, then run Quarantine.exe.
          If SmartScreen says "Windows protected your PC", click "More info" then "Run anyway"
          (the program isn't signed with a paid certificate).
macOS:    unzip, then move Quarantine.app to Applications (optional).
          The first time, right-click (or Control-click) Quarantine.app and choose Open, then Open again.
          If macOS says the app "is damaged" or can't be checked, open System Settings > Privacy &
          Security, scroll down and click "Open Anyway". Works on Apple silicon and Intel Macs,
          macOS 11 or later.
Linux:    extract, then run ./Quarantine from the extracted folder (x86-64).

The launcher opens:
  1. Choose "Disc image" and pick your .cue / .nrg / .iso (or drag the file onto the window).
     The launcher checks the disc and shows a green line when it's the right one.
  2. Choose "Start game". The first time, the game's files (about 60 MB) are copied from the disc
     image to your user folder. The disc image itself is only read, never changed.
Keep the disc image where it is: the CD music is played from it.

Next time, "Start game" is already selected. To skip the launcher entirely, set "Launcher at startup"
to Skip in Settings; hold Shift while starting the game to get it back.


SETTINGS (launcher > Settings)
------------------------------
Display          Window or Fullscreen (F11 switches during play)
Filter           how the 320x200 picture is enlarged (F10 switches during play):
                   Sharp    - big square pixels
                   Smooth   - blurred
                   Crisp    - sharp pixels, all the same size at any window size
                   CRT      - scanlines and glow, like a monitor of the time (best in fullscreen)
                   Scale4x  - smooths jagged edges
Aspect ratio     4:3 (CRT) as the game was shown in 1994, or square pixels
Music volume / Effects volume
Frame limit      70 Hz like the original VGA; turn off only if the game seems slow
Copy protection  Skip (default) or Show the original manual-lookup screen
Launcher at startup


CONTROLS
--------
The keyboard works as in the original game (see the game's manual and in-game help).
Game controllers (Xbox, PlayStation, Switch Pro, 8BitDo and others) work too, and can be plugged in
at any time:

                     Menus        Driving
  Left stick/D-pad   arrows       steer, accelerate, brake
  RT / LT            -            accelerate / brake and reverse
  A                  Enter        fire
  B                  Esc          nitro
  X                  -            next weapon
  Y                  Space        special item
  LB / RB            -            E / Z keys
  Start              Enter        game menu
  Back / Select      Esc          map
  L3 / R3            -            horn / change camera

F10  next picture filter        F11  fullscreen on/off


WHERE YOUR FILES ARE
--------------------
Settings (config.ini), the installed game files (data) and saved games (save) are in:
  Windows:  %APPDATA%\quarantine-recomp\Quarantine\
  macOS:    ~/Library/Application Support/quarantine-recomp/Quarantine/
  Linux:    ~/.local/share/quarantine-recomp/Quarantine/
To start over, delete that folder. Back up "save" to keep your saved games.


TROUBLESHOOTING
---------------
"This isn't the Quarantine CD" / "different version": this build only accepts the version of
  SKIPIT.EXE it was made from (572,501 bytes). Other releases of the game need their own build.
"Select the .cue file that goes with this .bin": pick the .cue sheet, not the .bin.
No music: .iso images have no audio tracks; use a .cue/.bin or .nrg rip instead.
Game runs too fast or slow: make sure Frame limit is set to 70 Hz.


ABOUT
-----
Quarantine (c) 1994 Imagexcel / GameTek. This project is not affiliated with or endorsed by the
rights holders, and contains none of the game's files: the program loads the game from your disc.
Uses SDL3 (zlib license, https://libsdl.org).
