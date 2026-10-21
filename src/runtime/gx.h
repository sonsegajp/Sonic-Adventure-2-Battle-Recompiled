#pragma once
// GX bridge (src/runtime/gx.cpp): the game's GX command stream -> Aurora, frame by frame.
#include "runtime/ctx.h"

#include <memory>
#include <vector>

namespace rt {

void gxWrite(u32 value, int size);           // a write to the write-gather pipe (0xCC008000)
void gxNoteCpRegister(u32 off, u32 value);   // CP register writes (FIFO base)
void gxEndFrame();                           // VISetNextFrameBuffer: hand the frame to the renderer
// Render thread: the next frame's command stream, and the snapshot memory it points into (keep it
// until Aurora has processed the frame; the previous frame's is released by the next call).
bool gxTakeFrame(std::vector<u8>& out, std::vector<std::unique_ptr<u8[]>>& snapshots, int waitMs);
u64 gxFrames();

}  // namespace rt
