// Hardware registers (0xCCxxxxxx) that recompiled SDK code still touches. Unclaimed registers act
// as plain storage, and the first access of each is logged so remaining hardware use is visible.
#include "runtime/runtime.h"

#include <mutex>
#include <unordered_set>

namespace rt {

namespace {
u8 s_regs[0x10000];  // 0xCC000000-0xCC00FFFF
std::mutex s_seenLock;
std::unordered_set<u32> s_seen;
u64 s_wgpBytes = 0;

void noteFirst(u32 a, bool write, u32 v) {
    std::lock_guard g(s_seenLock);
    if (s_seen.insert(a | (write ? 0x80000000u : 0)).second)
        log("[mmio] first %s %08X%s\n", write ? "write" : "read ", a, write ? (" = " + std::to_string(v)).c_str() : "");
}
}  // namespace

u32 dspRead16(u32 off);
void dspWrite16(u32 off, u32 v);

u32 mmio_rd(u32 a, int size) {
    const u32 o = a & 0xFFFF;
    if (o >= 0x5000 && o < 0x500C) {  // DSP mailbox/control (src/runtime/dsp.cpp)
        if (size == 4) {
            const u32 hi = dspRead16(o);
            return hi << 16 | dspRead16(o + 2);
        }
        return dspRead16(o & ~1u);
    }
    noteFirst(a & ~3u, false, 0);
    switch (size) {
    case 1: return s_regs[o];
    case 2: return u32(s_regs[o]) << 8 | s_regs[(o + 1) & 0xFFFF];
    default: return u32(s_regs[o]) << 24 | u32(s_regs[o + 1]) << 16 | u32(s_regs[o + 2]) << 8 | s_regs[o + 3];
    }
}

void mmio_wr(u32 a, u32 v, int size) {
    const u32 o = a & 0xFFFF;
    if ((o & 0xFFF8) == 0x8000) {  // GX write-gather pipe: the GX FIFO (src/runtime/gx.cpp, later)
        s_wgpBytes += u32(size);
        return;
    }
    if (o >= 0x5000 && o < 0x500C) {
        if (size == 4) {
            dspWrite16(o, v >> 16);
            dspWrite16(o + 2, v & 0xFFFF);
        } else {
            dspWrite16(o & ~1u, v);
        }
        return;
    }
    noteFirst(a & ~3u, true, v);
    switch (size) {
    case 1: s_regs[o] = u8(v); break;
    case 2:
        s_regs[o] = u8(v >> 8);
        s_regs[(o + 1) & 0xFFFF] = u8(v);
        break;
    default:
        s_regs[o] = u8(v >> 24);
        s_regs[o + 1] = u8(v >> 16);
        s_regs[o + 2] = u8(v >> 8);
        s_regs[o + 3] = u8(v);
        break;
    }
}

u64 mmioFifoBytes() { return s_wgpBytes; }

}  // namespace rt

namespace rt {
// Host-side access to the register file (device models raise status bits before interrupts).
u32 mmioGet16(u32 off) { return u32(s_regs[off & 0xFFFF]) << 8 | s_regs[(off + 1) & 0xFFFF]; }
void mmioSet16(u32 off, u32 v) {
    s_regs[off & 0xFFFF] = u8(v >> 8);
    s_regs[(off + 1) & 0xFFFF] = u8(v);
}
}  // namespace rt
