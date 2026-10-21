// Video retrace, ARAM, audio DMA pacing and controllers. The SDK's own interrupt handlers stay
// native: a device "interrupt" sets the status bit the handler checks and calls the handler the
// game registered in the OS interrupt table (__OSSetInterruptHandler, table at 0x80003040).
#include "runtime/gx.h"
#include "runtime/runtime.h"
#include "runtime/sched.h"

#include <atomic>
#include <chrono>
#include <cstring>
#include <memory>
#include <thread>

namespace rt {

u32 mmioGet16(u32 off);
void mmioSet16(u32 off, u32 v);

namespace {
enum : u32 { IRQ_DSP_AI = 5, IRQ_DSP_ARAM = 6, IRQ_PI_VI = 24 };


// ---- VI: retrace at the NTSC field rate ----
std::atomic<bool> s_viRunning{false};
std::atomic<u32> s_xfb{0};
std::atomic<u64> s_frames{0};

void viThread() {
    profileRegisterThread();
    using clock = std::chrono::steady_clock;
    const auto period = std::chrono::nanoseconds(1000000000ll * 1001 / 60000);  // 59.94 Hz
    auto next = clock::now() + period;
    for (;;) {
        std::this_thread::sleep_until(next);
        next += period;
        raiseInterrupt(IRQ_PI_VI, [] { mmioSet16(0x2030, mmioGet16(0x2030) | 0x8000); });
        if (++s_frames % 600 == 0) log("[vi] %llu retraces, xfb %08X\n", (unsigned long long)s_frames.load(), s_xfb.load());
    }
}

// ---- ARAM: 16 MiB of host memory; DMA completes at once, the interrupt follows ----
std::unique_ptr<u8[]> s_aram;
constexpr u32 kAramSize = 0x1000000;
}  // namespace

u8* aramPtr(u32 a) { return &s_aram[a & (kAramSize - 1)]; }
u64 viRetraces() { return s_frames.load(); }

HLE(VIInit) {
    callOriginal(c, "VIInit");
    bool expected = false;
    if (s_viRunning.compare_exchange_strong(expected, true)) std::thread(viThread).detach();
}
HLE(VISetNextFrameBuffer) {
    s_xfb = c.r[3];
    callOriginal(c, "VISetNextFrameBuffer");
    gxEndFrame();  // the frame's GX commands go to the renderer
}

HLE(__ARChecksize) {
    if (!s_aram) s_aram.reset(new u8[kAramSize]());
    wr32(c.r[13] - 0x743C, kAramSize);  // __AR_Size
}
HLE(ARStartDMA) {  // (type, main memory, aram, length): type 0 main->ARAM, 1 ARAM->main
    if (!s_aram) s_aram.reset(new u8[kAramSize]());
    const u32 type = c.r[3], mem = c.r[4], aram = c.r[5], len = c.r[6];
    for (u32 i = 0; i < len; i++) {
        if (type == 0) *aramPtr(aram + i) = *hostp(mem + i);
        else *hostp(mem + i) = *aramPtr(aram + i);
    }
    raiseInterrupt(IRQ_DSP_ARAM);
}

// ---- AI: the DMA "plays" in real time; each finished block raises the AI DMA interrupt, whose
// handler calls the game's AIRegisterDMACallback (the audio frame tick) ----
void audioPushAi(u32 addr, u32 bytes);  // src/runtime/audio.cpp
namespace {
std::atomic<bool> s_aiRunning{false};
void aiThread() {
    profileRegisterThread();
    using clock = std::chrono::steady_clock;
    auto next = clock::now();
    while (s_aiRunning) {
        const u32 blocks = mmioGet16(0x5036) & 0x7FFF;
        const u32 bytes = (blocks ? blocks : 1) * 32;
        if (blocks && (mmioGet16(0x5036) & 0x8000)) {  // the block now playing goes to the host
            const u32 src = ((mmioGet16(0x5030) & 0x3FF) << 16 | mmioGet16(0x5032)) & ~31u;
            audioPushAi(0x80000000u | src, bytes);
        }
        next += std::chrono::nanoseconds(u64(bytes / 4) * 1000000000ull / 32000);
        std::this_thread::sleep_until(next);
        if (mmioGet16(0x5036) & 0x8000)
            raiseInterrupt(IRQ_DSP_AI, [] { mmioSet16(0x500A, mmioGet16(0x500A) | 0x0008); });
    }
}
}  // namespace

HLE(AIInit) {
    // Hook the DMA and streaming interrupts up the way AIInit does; skip its sample-counter
    // calibration (it busy-waits on hardware).
    guestCall(c, guestSymbol("__OSSetInterruptHandler"), {IRQ_DSP_AI, guestSymbol("__AIDHandler")});
    guestCall(c, guestSymbol("__OSSetInterruptHandler"), {8, guestSymbol("__AISHandler")});
}
HLE(AIStartDMA) {
    callOriginal(c, "AIStartDMA");
    bool expected = false;
    if (s_aiRunning.compare_exchange_strong(expected, true)) std::thread(aiThread).detach();
}

// ---- PAD: port 1 is a standard controller (input comes from the host later), 2-4 are empty ----
HLE(PADInit) { c.r[3] = 1; }
HLE(PADReset) { c.r[3] = 1; }
HLE(PADRecalibrate) { c.r[3] = 1; }
HLE(PADControlMotor) {}
void padSnapshot(u8 out[4][12]);  // src/runtime/frontend.cpp (host controllers via Aurora)
HLE(PADRead) {
    u8 pads[4][12];
    padSnapshot(pads);
    u32 mask = 0;
    for (u32 i = 0; i < 4; i++) {
        std::memcpy(hostp(c.r[3] + i * 12), pads[i], 12);
        if (s8(pads[i][10]) == 0) mask |= 0x80000000u >> i;
    }
    c.r[3] = mask;
}

}  // namespace rt
