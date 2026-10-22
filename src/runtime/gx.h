#pragma once
// GX bridge (src/runtime/gx.cpp): the game's GX command stream -> Aurora, frame by frame.
#include "runtime/ctx.h"

#include <memory>
#include <vector>

namespace rt {

void gxWrite(u32 value, int size);           // a write to the write-gather pipe (0xCC008000)
void gxNoteCpRegister(u32 off, u32 value);   // CP register writes (FIFO base)
void gxEndFrame();                           // VISetNextFrameBuffer: hand the frame to the renderer
u64 gxFrames();

// ---- frames for the renderer ----
// Each game frame stands alone: `preamble` sets the whole GX state as the frame began, so a frame
// can be rendered any number of times, in any order, or skipped. `patches` are its values that
// move from frame to frame (matrices, lights, the projection, vertex positions sent with the draw,
// vertex arrays the CPU rewrites), each named by what drew it: the game object (task) whose
// display code was running, and the place in that code's output - so the renderer can draw the
// frame again with them blended toward the previous frame's (src/runtime/gxinterp.cpp).
struct GxPatch {
    enum Kind : u8 { PosMtx, NrmMtx, TexMtx, Light, Proj, Vertex, Array } kind;
    u8 fmt = 0;        // Vertex/Array: component format (GX_F32 4, GX_S16 3, GX_U16 2...), comps in `comps`
    u8 comps = 0;      // Vertex/Array: position components (2 or 3)
    u8 shift = 0;      // Vertex/Array: fixed-point shift
    bool flat = false; // used by a 2D draw (sprites, text, menus): not blended - they have no lasting
                       // names (glyphs and sprites come and go), and pairing them by order wobbles
    u32 at = 0;        // offset in cmds of the first value (Array: of the pointer in the ARRAYBASE command)
    u32 count = 0;     // XF words; vertices; array bytes
    u32 stride = 0;    // Vertex: vertex size; Array: element stride; XF kinds: the XF address
    u32 posOff = 0;    // Vertex: position offset in a vertex
    u8* data = nullptr;  // Array: the snapshot the command points at
    u64 key = 0;       // what drew it (see gx.cpp: tags): the object and the model part it drew
    u64 key2 = 0;      // the fallback: the object and the value's place in what it drew
    u64 group = 0;     // the object and model part, without which copy of it (0: no part)
    u32 occ = 0;       // which copy of that part the object drew (copies pair by position)
};
struct GxFrame {
    std::vector<u8> preamble, cmds;
    std::vector<GxPatch> patches;
    std::vector<std::unique_ptr<u8[]>> snapshots;  // array data the commands point into
    u64 index = 0;      // game frame number
    double time = 0;    // when the game finished it (seconds, steady clock)
    double vi = 0;      // the retrace before that (when the game began it), same clock
};
// Render thread: the frames finished since the last call, oldest first. Waits up to waitMs for one
// when there are none.
bool gxTakeFrames(std::vector<std::shared_ptr<GxFrame>>& out, int waitMs);

// Game thread (Ctx::call): the task system enters / leaves a task's display (or exec) function.
void gxTaskEnter(u32 task, u32 fn);
void gxTaskLeave();
// The task system's "function being run" variable (r13-0x7878): set to a task's exec / display
// function just before the call.
constexpr u32 kTaskFnVar = 0x803ADA48;

// ---- in-between frames (src/runtime/gxinterp.cpp), render thread ----
void gxInterpPush(std::shared_ptr<GxFrame> f);  // a new game frame (paired with the one before)
void gxInterpReset();
const GxFrame* gxInterpNewest();
double gxInterpNewestTime();  // when the newest frame is shown (its retrace)
double gxInterpDelay();       // how far behind the display runs (a frame, and the slowest recent frame's work)
double gxInterpShowTime(double now);  // the time to show at `now` (that far behind, moving smoothly)
// The commands to draw at `when` (GxFrame::time's clock; a frame is due one frame after the game
// finished it): the frame due then, its moving values blended from the one before. Blended vertex
// arrays are added to `keep` (alive until Aurora has used them). nullptr: no frames yet.
std::shared_ptr<GxFrame> gxInterpBuild(double when, std::vector<u8>& out, std::vector<std::unique_ptr<u8[]>>& keep, float* shownT);
double gxNow();  // GxFrame::time's clock

}  // namespace rt
