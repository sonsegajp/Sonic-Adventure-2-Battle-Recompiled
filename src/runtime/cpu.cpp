// CPU helpers the generated code calls out of line: quantized paired-single loads/stores, SPRs,
// MSR, traps, time base, string loads; plus logging, fatal errors and call tracing.
#include "runtime/runtime.h"
#include "runtime/sched.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <atomic>
#include <mutex>
#include <vector>

namespace rt {

volatile u32 g_irqPending = 0;
bool g_traceOn = false;

// ---- logging ----
static std::mutex s_logLock;
void vlog(const char* fmt, va_list ap) {
    std::lock_guard g(s_logLock);
    std::vfprintf(stdout, fmt, ap);
    std::fflush(stdout);
}
void log(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vlog(fmt, ap);
    va_end(ap);
}

// ---- tracing: last functions entered, per host thread. Rings are registered so the watchdog can
// show what every thread is doing. ----
namespace {
struct Ring {
    u32 a[64];
    u32 pos;
    u64 calls;
};
std::mutex s_ringsLock;
std::vector<Ring*> s_rings;
thread_local Ring* t_ring = nullptr;
Ring* myRing() {
    if (!t_ring) {
        t_ring = new Ring{};
        std::lock_guard g(s_ringsLock);
        s_rings.push_back(t_ring);
    }
    return t_ring;
}
void dumpRing(const Ring* r, u32 n) {
    for (u32 i = 64 - n; i < 64; i++) {
        const u32 a = r->a[(r->pos + i) & 63];
        if (!a) continue;
        const char* nm = guestName(a);
        std::fprintf(stdout, "    %08X %s\n", a, nm ? nm : "");
    }
}
}  // namespace
std::atomic<int> g_traceBurst{0};  // print this many upcoming function entries (any thread)
void trace_enter(u32 addr, Ctx& c) {
    Ring* r = myRing();
    r->a[r->pos++ & 63] = addr;
    r->calls++;
    if (g_traceBurst.load(std::memory_order_relaxed) > 0 && g_traceBurst.fetch_sub(1) > 0) {
        const char* n = guestName(addr);
        log("[trace] %08X %s (lr %08X r3 %08X)\n", addr, n ? n : "", c.lr, c.r[3]);
    }
}
static void dumpTrace() {
    std::fprintf(stdout, "recent guest functions on this thread (oldest first):\n");
    if (t_ring) dumpRing(t_ring, 64);
}
void dumpAllThreads() {
    std::lock_guard g(s_ringsLock);
    std::fprintf(stdout, "[watch] %zu host threads ran guest code\n", s_rings.size());
    for (size_t i = 0; i < s_rings.size(); i++) {
        std::fprintf(stdout, "  thread %zu: %llu calls, last:\n", i, (unsigned long long)s_rings[i]->calls);
        dumpRing(s_rings[i], 12);
    }
    std::fflush(stdout);
}

[[noreturn]] void fatal(const char* fmt, ...) {
    {
        std::lock_guard g(s_logLock);
        std::fprintf(stdout, "\nFATAL: ");
        va_list ap;
        va_start(ap, fmt);
        std::vfprintf(stdout, fmt, ap);
        va_end(ap);
        std::fprintf(stdout, "\n");
        if (g_traceOn) dumpTrace();
        std::fflush(stdout);
    }
    std::_Exit(1);
}

// ---- time base ----
static const auto s_t0 = std::chrono::steady_clock::now();
u64 timebase() {
    const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - s_t0).count();
    return u64(ns) * kTimebaseHz / 1000000000ull;
}
u32 Ctx::timebaseLo() { return u32(timebase()); }
u32 Ctx::timebaseHi() { return u32(timebase() >> 32); }

// ---- paired-single quantization (GQR: load type 16-18, load scale 24-29, store type 0-2,
// store scale 8-13; types 0 float, 4 u8, 5 u16, 6 s8, 7 s16; scale is 6-bit signed) ----
namespace {
int scaleOf(u32 v) { return (v & 0x20) ? int(v) - 64 : int(v); }
double dequant(u32 ea, u32 type, int scale, u32& size) {
    switch (type) {
    case 4: size = 1; return std::ldexp(double(rd8(ea)), -scale);
    case 5: size = 2; return std::ldexp(double(rd16(ea)), -scale);
    case 6: size = 1; return std::ldexp(double(s8(rd8(ea))), -scale);
    case 7: size = 2; return std::ldexp(double(s16(rd16(ea))), -scale);
    default: size = 4; return double(flt(rd32(ea)));
    }
}
void quant(u32 ea, u32 type, int scale, double v, u32& size) {
    auto clampTo = [](double x, double lo, double hi) { return x < lo ? lo : (x > hi ? hi : x); };
    switch (type) {
    case 4: size = 1; wr8(ea, u32(clampTo(std::ldexp(v, scale), 0, 255))); return;
    case 5: size = 2; wr16(ea, u32(clampTo(std::ldexp(v, scale), 0, 65535))); return;
    case 6: size = 1; wr8(ea, u32(s32(clampTo(std::ldexp(v, scale), -128, 127)))); return;
    case 7: size = 2; wr16(ea, u32(s32(clampTo(std::ldexp(v, scale), -32768, 32767)))); return;
    default: size = 4; wr32(ea, bits32(float(v))); return;
    }
}
}  // namespace

void Ctx::psqLoad(int d, u32 ea, bool w, int i) {
    const u32 g = gqr[i];
    const u32 type = (g >> 16) & 7;
    const int scale = scaleOf((g >> 24) & 0x3F);
    u32 size;
    f[d] = dequant(ea, type, scale, size);
    ps1[d] = w ? 1.0 : dequant(ea + size, type, scale, size);
}

void Ctx::psqStore(int s, u32 ea, bool w, int i) {
    const u32 g = gqr[i];
    const u32 type = g & 7;
    const int scale = scaleOf((g >> 8) & 0x3F);
    u32 size;
    quant(ea, type, scale, f[s], size);
    if (!w) quant(ea + size, type, scale, ps1[s], size);
}

// ---- special registers ----
void Ctx::mtspr(u32 n, u32 v) {
    spr[n & 1023] = v;
    // Locked-cache DMA: DMA_U (922) = memory address | length high, DMA_L (923) = cache address |
    // load bit (16) | length low (bits 2-3) | trigger (2). Done at once; LCQueueWait sees it idle.
    if (n == 923 && (v & 2)) {
        const u32 u = spr[922];
        u32 blocks = ((u & 0x1F) << 2) | ((v >> 2) & 3);
        if (!blocks) blocks = 128;
        const u32 mem = u & ~31u, lc = v & ~31u, len = blocks * 32;
        if (v & 0x10) __builtin_memcpy(hostp(lc), hostp(mem), len);
        else __builtin_memcpy(hostp(mem), hostp(lc), len);
        spr[923] = v & ~2u;
    }
}
u32 Ctx::mfspr(u32 n) {
    switch (n) {
    case 22: return u32(-1);           // DEC: never expires (alarms are host timers)
    case 287: return 0x00083214;       // PVR: Gekko
    default: return spr[n & 1023];
    }
}
void Ctx::mtmsr(u32 v) {
    const bool wasOff = !(msr & 0x8000);
    msr = v;
    if (wasOff && (msr & 0x8000)) poll();
}

void Ctx::syscall() {}  // sc: PPCSync in the SDK; nothing to synchronise natively
void Ctx::trap(u32 pc) { fatal("trap at %08X (lr=%08X)", pc, lr); }
void Ctx::unimplemented(u32 pc, u32 insn) { fatal("unimplemented instruction %08X at %08X (lr=%08X)", insn, pc, lr); }

void Ctx::lswi(int d, u32 ea, u32 n) {
    // Four bytes per register from rD on, big-endian; the last register is zero-padded.
    for (u32 i = 0; i < n; i++) {
        const u32 reg = (d + i / 4) & 31;
        if (i % 4 == 0) r[reg] = 0;
        r[reg] |= rd8(ea + i) << (24 - 8 * (i % 4));
    }
}
void Ctx::stswi(int s, u32 ea, u32 n) {
    for (u32 i = 0; i < n; i++) wr8(ea + i, (r[(s + i / 4) & 31] >> (24 - 8 * (i % 4))) & 0xFF);
}

// ---- interrupt safe points ----
void service_irq(Ctx& c) { sched::serviceInterrupts(c); }

}  // namespace rt
