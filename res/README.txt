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

Online co-op
  CO-OP on the launcher (or F1 in the game): host and send your partner the code, or join with
  theirs. Each of you plays on your own screen: the host the story's character, the joiner the
  other side's (Sonic/Shadow, Tails/Eggman, Knuckles/Rouge). The host drives the menus and
  cutscenes; co-op has its own save (saves\coop). In the game: F1 for the co-op menu (colours,
  name, Discord), T to chat.

What is in this folder
  SA2BLauncher.exe   the launcher, with the recompiler built in
  sa2b.exe, *.dll    the runtime: our own code standing in for the GameCube (graphics, sound, ...)
  sa2b.lib           the runtime's interface, for linking the game's code
  tools\llvm         the compiler and linker the setup uses (LLVM 19.1.5, unchanged)

Source code, issues, how the setup works: https://github.com/sonsegajp/Sonic-Adventure-2-Battle-Recompiled
  (docs/SETUP.md)

Sonic Adventure 2: Battle is (c) SEGA. This project is not affiliated with or endorsed by SEGA or
Nintendo.
