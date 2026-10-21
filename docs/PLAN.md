# SA2B Recompiled — plan

Native PC port of Sonic Adventure 2: Battle (GameCube, USA **GSNE8P**) built from the user's own disc.
Same architecture as Luigi's Mansion Recompiled (rob646464/luigis-mansion-recompiled), all code our own:

1. **Recompiler** (`src/recompiler`, C++): translates `main.dol` and all 76 `*.rel` modules into C++.
   One C++ function per guest function; in-function branches are `goto`; `bl` is a direct call;
   `bctr` through a jump table is a `switch`; other indirect calls go through an O(1) dispatch table.
   No symbol map exists for SA2B, so function discovery is our own (call targets, relocations,
   pointers in data, control-flow closure, jump tables).
2. **Runtime** (`src/runtime`): guest MEM1 (big-endian), `Ctx` registers, paired singles/GQR,
   one host thread per OSThread with a single "CPU" baton, interrupts delivered at safe points.
3. **SDK replaced by name** (no hardware emulation): OS, DVD, VI, PAD/SI, AI/AX/DSP/AR/ARQ, CARD, GX.
   SDK functions are identified by our own signature matcher against Nintendo's shipped
   Wind Waker `framework.map` (tools/sdkmatch.py) → `config/GSNE8P/names.txt`.
4. **GX → native renderer** via Aurora (encounter/aurora, MIT, WebGPU/Dawn + SDL3), the graphics
   layer LM uses; the write-gather pipe (0xCC008000) feeds its FIFO.
5. **REL modules**: the game loads a REL into its own heap and calls OSLink; our OSLink applies the
   relocations exactly like the SDK, records the section bases, and registers the module's
   recompiled functions. REL code addresses its own data through those runtime bases, never fixed
   addresses.
6. **60 fps everywhere**: native speed removes all GameCube slowdown; 30 fps-locked parts
   (event/cutscene engine, any 2P/menu caps) get 60 Hz logic where safe, otherwise frame
   interpolation from tagged objects (LM approach). Uncapped framerate later.
7. **Distribution**: the exe contains the recompiler; first run reads the user's disc, recompiles,
   and builds the game module with a bundled clang + lld. Nothing derived from the game is shipped.

Never: Dolphin/GekkoRecompiled/RecompCore runtimes, interpreter fallback, committed game data.

## Milestones
- M0 disc/DOL/REL readers; function discovery with coverage stats; decoder vs capstone oracle.
- M1 recompiler emits every DOL + REL function; all of it compiles.
- M2 runtime boots `__start` → `main` with OS/thread HLE.
- M3 DVD + PRS/REL loading; GX via Aurora → title screen on screen.
- M4 pad input, audio (AX + CRI ADX), Sofdec movies → playable stages.
- M5 first-run setup + bundled clang; 60 fps work; widescreen/resolution.
