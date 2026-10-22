# Sonic Adventure 2 Battle: Recompiled

A native PC version of **Sonic Adventure 2: Battle** (GameCube, USA), made by static recompilation:
the game's PowerPC code is translated to C++ ahead of time and runs natively, with its own runtime in
place of the console's hardware. No emulator.

**No game data is included.** You need your own copy of the game (USA disc image, `GSNE8P`); the
launcher reads everything from it.

## Features

- Story, stage select, Chao World, kart races, bosses, 2P battle and memory card saves
- Widescreen at any aspect ratio (16:9, 16:10, ultrawide, resizable windows), HUD kept at the edges
- Smooth motion on 120/144 Hz displays (in-between frames), the game itself at its 60 Hz
- Optional **HD textures**: every texture on your disc upscaled 4x with the HDcube model, made on
  your own computer from the launcher
- Original disc loading times by default (cutscenes are timed for them), or instant loading
- A launcher in the game's own style: drawn with the menu art, lights and font from your disc

## Playing

1. Download the latest release and unpack it anywhere.
2. Run **SA2BLauncher.exe**, choose **CHOOSE DISC** and pick your disc image (`.iso`/`.gcm`).
3. **SETTINGS** for display, frame rate, volume and loading; **PLAY** to start.

Controls: a gamepad works directly. Keyboard: WASD stick, Space A, Shift B, F X, C Y, Enter START,
Q/E L/R, IJKL C-stick, arrows D-pad. F11 toggles full screen.

Saves go to `saves/MemoryCardA.raw` next to the program (a GameCube memory card image, the same
format as Dolphin's).

### HD textures

On the launcher's **HD TEXTURES** page: **Install the upscaler** (a private copy of Python and
PyTorch, and the HDcube model from its author's release page, about 3 GB with an NVIDIA card), then
**Make HD textures**. It takes about 10-20 minutes with a graphics card, longer on the processor
alone, and can run while you play; the game uses the textures once all are done.

## Building from source

Windows, Visual Studio 2022 (clang-cl), CMake and Ninja, Python 3.

```
git clone --recursive https://github.com/sonsegajp/Sonic-Adventure-2-Battle-Recompiled
python tools/extract.py "<your disc image>"          # orig/: main.dol and the REL modules
build/windows-clang/sa2recompile.exe --dol orig/main.dol --rels orig/rel --names config/GSNE8P/names.txt --patches config/GSNE8P/patchpoints.txt --out build/gen
powershell -File tools/build.ps1                     # sa2b.exe and SA2BLauncher.exe
```

(`tools/build.ps1 -Target sa2recompile` first builds the recompiler.) Optionally,
`python tools/launcher/mkicon.py --disc "<disc image>"` gives the programs the game's own icon.

How it works: [docs/PLAN.md](docs/PLAN.md). In short, `src/recompiler` finds every function in
`main.dol` and the 76 REL modules and emits C++; `src/runtime` provides the console (CPU state,
interrupts, threads, DVD, audio DSP, memory card, video) and hands the game's GX command stream to
[Aurora](https://github.com/encounter/aurora) (WebGPU) for drawing.

## Credits and licences

- This project's code: [PolyForm Noncommercial 1.0.0](LICENSE.md).
- [Aurora](https://github.com/encounter/aurora) (MIT), with local changes in `patches/aurora`.
- SDL3, Dawn, libpng, zlib, xxHash: see `THIRD_PARTY_NOTICES.txt` in a release.
- [HDcube](https://github.com/Venomalia/HDcube) by Venomalia: downloaded from its author by the
  upscaler setup, never redistributed here.
- The launcher's button lettering is rendered from Vipnagorgialla by Typodermic Fonts (words as
  images; the font itself is not included).

Sonic Adventure 2: Battle is © SEGA. This project is not affiliated with or endorsed by SEGA or
Nintendo. Please buy the game.
