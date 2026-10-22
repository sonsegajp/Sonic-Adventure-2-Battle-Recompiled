Sonic Adventure 2 Battle: Recompiled
====================================

A native PC version of Sonic Adventure 2: Battle (GameCube, USA), made by static recompilation.
Nothing of the game is included - neither its data nor its code. You need your own copy of the
game, a USA disc image (GSNE8P, .iso/.gcm); the game's code is built from it on your computer.

Playing
  1. Run SA2BLauncher.exe.
  2. CHOOSE DISC and pick your disc image.
  3. PLAY. The first time, the launcher builds the game from your disc (the SETUP page shows each
     step's progress): about 10 minutes with 8 processor threads, 20 or more with 4. It needs about
     1 GB of free space while it works, and keeps the result here as sa2b_game.dll. It does this
     again only after an update or when you choose another disc image.
  4. SETTINGS for display, frame rate, volume and loading.

  A gamepad works directly. Keyboard: WASD stick, Space A, Shift B, F X, C Y, Enter START, Q/E L/R,
  IJKL C-stick, arrows D-pad. F11 toggles full screen.

  Saves: saves\MemoryCardA.raw in this folder (a GameCube memory card image, as Dolphin's).
  The last session's log: sa2b.log. The last setup's: setup.log.

Your disc image
  The setup checks it for you. It is made for the Redump dump of the USA disc:
    size   1,459,978,240 bytes
    CRC32  024f4f8f
    SHA-1  f8cc51128df2a3b0da9d6f3a234c895a4cd7dc22
  To check it yourself, in PowerShell:  Get-FileHash -Algorithm SHA1 "<your disc image>"
  A trimmed or otherwise different image of the same disc is accepted too when the game's program
  on it is identical. Keep the program in a folder you can write to (not Program Files).

HD textures (optional)
  On the launcher's HD TEXTURES page: Install the upscaler (Python, PyTorch and the HDcube model,
  downloaded into %LOCALAPPDATA%\SA2BRecompiled: about 3 GB with an NVIDIA card), then Make HD
  textures. About 10-20 minutes with a graphics card; it can run while you play.

What is in this folder
  SA2BLauncher.exe   the launcher, with the recompiler built in
  sa2b.exe, *.dll    the runtime: our own code standing in for the GameCube (graphics, sound, ...)
  sa2b.lib           the runtime's interface, for linking the game's code
  tools\llvm         the compiler and linker the setup uses (LLVM 19.1.5, unchanged)
  tools\hd           the HD texture builder's scripts

Source code, issues, how the setup works: https://github.com/sonsegajp/Sonic-Adventure-2-Battle-Recompiled
  (docs/SETUP.md)

Sonic Adventure 2: Battle is (c) SEGA. This project is not affiliated with or endorsed by SEGA or
Nintendo.
