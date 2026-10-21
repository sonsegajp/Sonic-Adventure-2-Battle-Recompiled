#pragma once
// GX bridge (src/runtime/gx.cpp): the game's GX command stream -> Aurora, frame by frame.
#include "runtime/ctx.h"

#include <vector>

namespace rt {

void gxWrite(u32 value, int size);           // a write to the write-gather pipe (0xCC008000)
void gxNoteCpRegister(u32 off, u32 value);   // CP register writes (FIFO base)
void gxEndFrame();                           // VISetNextFrameBuffer: hand the frame to the renderer
bool gxTakeFrame(std::vector<u8>& out, int waitMs);  // render thread: next frame's command stream
u64 gxFrames();

}  // namespace rt
