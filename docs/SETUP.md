# First-run setup: building the game from your disc

The release contains **no game code**: none of Sonic Adventure 2: Battle's code, translated or not.
It contains the runtime (`sa2b.exe`, our own code, which stands in for the GameCube's hardware), the
launcher, and the recompiler. The game's code is translated from **your own disc** on **your own
computer** the first time you press **PLAY**, and kept next to the program as `sa2b_game.dll`.

This page describes what happens during that setup, what it needs, how your disc is checked, and
how the pieces fit together.

## At a glance

| | |
|---|---|
| When | The first **PLAY**; again after an update of the program, or when you choose another disc image |
| How long | About 10 minutes with 8 processor threads, 20 or more with 4 (almost all of it compiling) |
| Disk space | About 1 GB while it works (in `%LOCALAPPDATA%\SA2BRecompiled\build`, removed afterwards); the result is about 290 MB |
| Memory | Up to about 300 MB per processor thread used |
| Internet | Not needed: the compiler is bundled |
| Result | `sa2b_game.dll` and `sa2b_game.txt` (what it was built from) next to `sa2b.exe`; `setup.log` in the data folder |

The launcher shows the setup on its **SETUP** page: the five steps, each marked done or with its
percentage, a bar for the current step (with what it is working on, such as "120 of 273 files"), and
a bar for the whole setup with the time so far and an estimate of the time left. **CANCEL** stops it
at any point; nothing half-built is kept, and the next **PLAY** starts over. When it finishes, the
game starts.

![The SETUP page, compiling](launcher_setup.png)

## The five steps

### 1. Checking your disc

The whole disc image is read once and its SHA-1 computed. The recompiler and the runtime are made for
one exact game: **Sonic Adventure 2: Battle (USA), game ID `GSNE8P`**, as dumped by Redump:

| | |
|---|---|
| Size | 1,459,978,240 bytes |
| CRC32 | `024f4f8f` |
| MD5 | `9ef5fadf4b8756af820df997468a5a16` |
| SHA-1 | `f8cc51128df2a3b0da9d6f3a234c895a4cd7dc22` |

A disc image that is not a 1:1 copy of that dump (a trimmed or scrubbed image, or one from another
dump tool) can still hold exactly the same game. So when the disc's SHA-1 differs, the setup goes on
to step 2 and checks the **program** instead: the SHA-1 of `main.dol` and every code module, as
extracted (each file's name, a NUL byte, its size as 8 little-endian bytes, then its contents, in
name order, `main.dol` first):

| | |
|---|---|
| Program SHA-1 | `e23d2e3b41d2b051b3360fb1b55f9af6a96d6c37` (`main.dol` and 95 code modules) |

If that matches, the setup continues (and says so); if it does not, it stops with a message: the
recompiler's function names and the runtime's fixes are tied to this exact program, and another
version (the European or Japanese release, a modified game) would not work.

To check your disc image yourself, in PowerShell:

```powershell
Get-FileHash -Algorithm SHA1 "Sonic Adventure 2 - Battle (USA).iso"
```

or in a command prompt: `certutil -hashfile "Sonic Adventure 2 - Battle (USA).iso" SHA1`.

### 2. Extracting the game's program

Read from the disc image directly (nothing is unpacked to disk):

- `main.dol`, the main program (its size is where its last section ends);
- every `.rel` file: the code modules the game links at run time (each stage, boss, menu, ...);
- every `.prs` file that unpacks to a REL module: the Chao World modules (`ChaoMain.prs` and the
  others) are kept PRS-compressed on the disc. Only the first 512 bytes of each `.prs` file are read
  to tell (decompressed to the 64-byte REL header); the modules themselves are then decompressed in
  full and named `<name>_prs.rel`, as `tools/extract.py` names them.

The program's SHA-1 (above) is computed here.

### 3. Recompiling it to C++

The recompiler (`src/recompiler`, linked into the launcher) runs in the launcher's process:

1. **Analysis** finds every function in `main.dol` and the modules: from the entry points, calls,
   tail calls, jump tables, function pointers in data and gaps between known functions.
2. **Names**: the SDK functions in `config/GSNE8P/names.txt` (OS, GX, DVD, audio, memory card, ...)
   get a hook slot, which the runtime fills with its own implementation.
3. **Emission**: one C++ function per guest function (registers in a `Ctx`, memory through
   big-endian helpers), written in files of a manageable size: `main.dol` in 24, each module in
   pieces of about 60,000 instructions, plus headers and function tables. About 270 MB of C++ in
   272 source files.

### 4. Compiling the game

Every file is compiled with the **bundled clang** (`tools/llvm/clang.exe`, LLVM 19.1.5), as many at
once as the processor has threads (fewer if memory is short), the largest files first so the last
ones finish together. The compilers run at below-normal priority, so the computer stays usable, and
inside a job object, so they end with the launcher whatever happens to it.

```
clang --target=x86_64-pc-windows-msvc -std=c++20 -O2 -g0 -ffreestanding -nostdinc
      -fno-exceptions -fno-rtti -fno-threadsafe-statics -DSA2B_GAME_DLL -Iinclude -Igen -w
      -c gen/<file>.cpp -o obj/<file>.obj
```

The module is **freestanding**: it uses no C or C++ library and no headers but the runtime's
`ctx.h`, `modules.h` and `abi.h` (embedded in the launcher and written to the work folder). The few
functions a compiler may call on its own are in `src/game/glue.cpp`: `memcpy`, `memmove`, `memset`
(`rep movsb`/`stosb`), `memcmp`, `sqrt` (`sqrtsd`), `fma` (forwarded to the runtime's C library),
`__chkstk` (the stack probe for frames over a page) and `_fltused`. That is why no Windows SDK or
Visual Studio is needed on your computer.

The bar follows the amount of source compiled (bytes, not files: the files differ a lot in size).

### 5. Linking the game

```
lld-link /dll /noentry /nodefaultlib /machine:x64 /opt:ref /opt:noicf
         /out:sa2b_game.dll @objs.rsp sa2b.lib
```

`sa2b.lib` is the runtime's import library: the module calls the runtime's functions in `sa2b.exe`
(memory-mapped hardware, indirect calls, the time base, traps, ...). The linker does not report its
progress; the bar creeps on over about the time it takes (about a second).

Then `sa2b_game.dll` is copied next to `sa2b.exe`, `sa2b_game.txt` records what it was built from,
and the work folder is removed.

## When it runs again

`sa2b_game.txt` holds the program's version, the module interface version, and the disc image's
path, size, date and SHA-1. PLAY builds the module again when:

- `sa2b_game.dll` is missing;
- the program was updated (a new version's runtime may expect differently translated code);
- the disc image's path, size or date changed. If its SHA-1 is still the same (the file was moved
  or renamed), step 1 is all that runs.

The runtime checks the module too: one built for another interface version is refused, with a
message to start the launcher.

## How the pieces fit together

```
SA2BLauncher.exe  --builds-->  sa2b_game.dll  <--loads--  sa2b.exe
  recompiler (in process)       translated game code       runtime: CPU helpers, interrupts,
  bundled clang + lld-link      + src/game/glue.cpp        threads, DVD, audio, memory card,
  embedded headers/names                                   GX -> Aurora (WebGPU), window, input
```

- `sa2b.exe` starts by loading `sa2b_game.dll` from its own folder and asking it for its
  `GameModule` (`src/runtime/modules.h`): the function tables (main.dol's, each module's), the
  SDK hook slots, the run-time override flags, and pointers to the state the translated code uses
  all the time - its copy of the guest memory base, the pending-interrupt flag, the call-trace flag
  and the REL section bases. Those live in the module, so the hot paths stay plain global accesses;
  the runtime reaches them through the pointers.
- The runtime's functions the module calls are exported from `sa2b.exe` (`RT_API` in
  `src/runtime/ctx.h`) and imported through `sa2b.lib`.
- `SA2B_GAME_ABI` (`src/runtime/abi.h`) is raised whenever that interface changes.

## Files

| File | What |
|---|---|
| `tools/llvm/clang.exe`, `lld-link.exe` | The official LLVM 19.1.5 Windows release's compiler and linker, unchanged (`LLVM-19.1.5-win64.exe`, SHA-256 `9b29856202d82047b504a9e501d66080bed0eaff3f1127191f4f361e0e00c770`). Apache License 2.0 with LLVM Exceptions: `tools/llvm/LICENSE.TXT` |
| `sa2b.lib` | The runtime's import library, for linking the module |
| `sa2b_game.dll` | The game's code, made by the setup (never in the download) |
| `sa2b_game.txt` | What `sa2b_game.dll` was built from |
| `setup.log` | The last setup: each step's time, the disc's and program's SHA-1, compiler or linker errors |

## If it stops

The page shows why, and `setup.log` has the details. The usual causes:

- *This disc image is not the Sonic Adventure 2: Battle (USA) disc...*: another version or a modified
  game. Check its SHA-1 as above.
- *The game's folder can't be written to*: the program is in a protected folder such as Program
  Files. Move it to a folder of your own.
- *Couldn't write the recompiled code*: the drive with `%LOCALAPPDATA%` needs about 1 GB free.
- *Files the setup needs are missing*: the download was not extracted in full (`tools/llvm`,
  `sa2b.lib`).

## For developers

The developer build makes the same module with CMake, from `build/gen` (`sa2recompile ... --out
build/gen`): the `sa2b_game` target, compiled with the same freestanding flags by Visual Studio's
clang-cl. The launcher's setup is the release path; to try it on a developer build, put the LLVM
release's `clang.exe` and `lld-link.exe` in `build/llvm`.

`SA2BLauncher.exe --shot <file.bmp> setup` runs a real setup in a hidden window and saves a picture
of the page a third of the way into compiling (then stops it).
