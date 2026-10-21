// OS library replacements. Everything not here runs as recompiled game code; these are the parts
// that touch the CPU's interrupt state, the scheduler, timers, the RTC/SRAM or module linking.
#include "runtime/runtime.h"
#include "runtime/sched.h"

#include <cstdlib>

namespace rt {

namespace {
constexpr u32 kEE = 0x8000;
u64 arg64(const Ctx& c, int hiReg) { return u64(c.r[hiReg]) << 32 | c.r[hiReg + 1]; }
}  // namespace

// ---- console output ----
HLE(OSReport) {
    const std::string s = guestFormat(c, c.r[3], 4, 1);
    log("%s", s.c_str());
}

// ---- interrupt enable state (MSR[EE] of the running context) ----
HLE(OSDisableInterrupts) {
    const u32 old = (c.msr & kEE) ? 1 : 0;
    c.msr &= ~kEE;
    c.r[3] = old;
}
HLE(OSEnableInterrupts) {
    const u32 old = (c.msr & kEE) ? 1 : 0;
    c.msr |= kEE;
    c.r[3] = old;
    c.poll();
}
HLE(OSRestoreInterrupts) {
    const u32 old = (c.msr & kEE) ? 1 : 0;
    if (c.r[3]) c.msr |= kEE;
    else c.msr &= ~kEE;
    c.r[3] = old;
    if (c.msr & kEE) c.poll();
}

// ---- hardware init the host does not need (called from OSInit) ----
HLE(EXIInit) {}
HLE(SIInit) {}
HLE(__OSInitAudioSystem) {}
// Memory protection/BAT setup runs in real mode through an rfi trampoline: nothing to do natively.
HLE(__OSInitMemoryProtection) {}
HLE(RealMode) {}

// ---- threads ----
HLE(__OSThreadInit) {
    callOriginal(c, "__OSThreadInit");  // builds the default thread and sets __OSCurrentThread
    sched::adoptBootThread(rd32(sched::kCurrentThread), c);
}
HLE(OSCreateThread) {
    c.r[3] = sched::create(c.r[3], c.r[4], c.r[5], c.r[6], c.r[7], s32(c.r[8]), c.r[9]) ? 1 : 0;
}
HLE(OSResumeThread) { c.r[3] = u32(sched::resume(c.r[3])); }
HLE(OSSuspendThread) { c.r[3] = u32(sched::suspend(c.r[3])); }
HLE(OSSleepThread) { sched::sleepOn(c.r[3]); }
HLE(OSWakeupThread) { sched::wakeup(c.r[3]); }
HLE(OSExitThread) { sched::exitThread(c.r[3]); }
HLE(OSCancelThread) { sched::cancel(c.r[3]); }
HLE(OSJoinThread) {
    u32 val = 0;
    const u32 out = c.r[4];
    c.r[3] = sched::join(c.r[3], &val) ? 1 : 0;
    if (out) wr32(out, val);
}
HLE(OSSetThreadPriority) {
    if (s32(c.r[4]) < 0 || s32(c.r[4]) > 31) {
        c.r[3] = 0;
        return;
    }
    sched::setPriority(c.r[3], s32(c.r[4]));
    c.r[3] = 1;
}
HLE(OSDisableScheduler) {
    sched::disableScheduler();
    c.r[3] = 0;
}
HLE(OSEnableScheduler) {
    sched::enableScheduler();
    c.r[3] = 0;
}
HLE(__OSReschedule) {}
HLE(SelectThread) {}
HLE(UnsetRun) {}
HLE(SetEffectivePriority) {}

// ---- alarms (OSTime arguments arrive in odd/even register pairs) ----
HLE(OSSetAlarm) {  // (alarm r3, tick r5:r6, handler r7)
    sched::setAlarm(c.r[3], timebase() + arg64(c, 5), 0, c.r[7]);
}
HLE(OSSetPeriodicAlarm) {  // (alarm r3, start r5:r6, period r7:r8, handler r9)
    const u64 start = arg64(c, 5), period = arg64(c, 7);
    u64 first = start;
    const u64 now = timebase();
    if (period && first <= now) first += ((now - first) / period + 1) * period;
    sched::setAlarm(c.r[3], first, period, c.r[9]);
}
HLE(OSCancelAlarm) { sched::cancelAlarm(c.r[3]); }

// ---- SRAM (normally read over EXI from the RTC chip): a valid, English, stereo setup ----
namespace {
u32 s_sram = 0, s_sramEx = 0;
void sramInit() {
    if (s_sram) return;
    s_sram = hleAlloc(0x40);
    s_sramEx = hleAlloc(0x40);
    wr8(s_sram + 0x12, 0);     // language: English
    wr8(s_sram + 0x13, 0x04);  // flags: stereo sound
}
}  // namespace
HLE(__OSInitSram) { sramInit(); }
HLE(__OSLockSram) {
    sramInit();
    c.r[3] = s_sram;
}
HLE(__OSLockSramEx) {
    sramInit();
    c.r[3] = s_sramEx;
}
HLE(__OSUnlockSram) { c.r[3] = 1; }
HLE(__OSUnlockSramEx) { c.r[3] = 1; }
HLE(__OSSyncSram) { c.r[3] = 1; }

// ---- REL modules: the SDK's OSLink applies the relocations; then the recompiled code of the
// module is registered at the addresses the game loaded it to. ----
HLE(OSLink) {
    const u32 module = c.r[3];
    callOriginal(c, "OSLink");
    if (!c.r[3]) return;
    const u32 id = rd32(module);
    const u32 nsec = rd32(module + 0x0C);
    const u32 info = rd32(module + 0x10);  // absolute once linked
    if (id >= 256 || nsec > 32) fatal("OSLink: bad module header at %08X", module);
    for (u32 i = 0; i < nsec; i++) g_relBase[id][i] = rd32(info + i * 8) & ~1u;
    registerRelModule(id);
}
HLE(OSUnlink) {
    const u32 id = rd32(c.r[3]);
    if (id < 256) unregisterRelModule(id);
    callOriginal(c, "OSUnlink");
}

// ---- reset ----
HLE(OSResetSystem) {
    log("[os] OSResetSystem(%u, %08X, %u): exiting\n", c.r[3], c.r[4], c.r[5]);
    std::exit(0);
}

}  // namespace rt
