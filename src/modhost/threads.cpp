// Mod threads, the x86 lock and thread-local storage (see threads.h).
#include "modhost/threads.h"

#include "modhost/handles.h"
#include "modhost/modhost.h"

#include "runtime/runtime.h"

#include <condition_variable>
#include <map>
#include <mutex>
#include <thread>
#include <vector>

namespace mh {

namespace {

// ---- the lock ----
struct GilState {
    std::mutex m;
    std::condition_variable cv;
    std::thread::id owner;
    int depth = 0;
    int waiting = 0;
} g;

// ---- address space for thread stacks and TEBs ----
constexpr u32 kStackRegion = 0x58000000, kStackRegionEnd = 0x5FF00000;
constexpr u32 kTebRegion = 0x7F800000, kTebRegionEnd = 0x7FDE0000;
constexpr u32 kTebSize = 0x2000;  // the TEB page, then a page of per-thread scratch
constexpr u32 kTlsArray = 0x800;  // in the scratch page: the static TLS pointers (64)
constexpr u32 kMainTid = 0x1004;

struct XThread {
    std::unique_ptr<Cpu> cpu;
    u32 id = 0, teb = 0, start = 0, arg = 0;
    bool suspended = false, done = false;
    u32 exitCode = 0;
};
struct ThreadObject : KObject {
    std::shared_ptr<XThread> t;
    bool signaled() override { return t->done; }
};

std::vector<u32> s_tebs;  // every thread's TEB (the main thread's first)
u32 s_nextStack = kStackRegion, s_nextTeb = kTebRegion, s_nextTid = kMainTid;

thread_local Cpu* t_cpu = nullptr;
thread_local u32 t_tid = kMainTid;
thread_local u32 t_teb = 0;

struct TlsImage {
    u32 start, end, zeroFill, index;
};
std::vector<TlsImage> s_tlsImages;
u64 s_tlsSlots = 0;  // TlsAlloc bitmap (64 slots)

struct ThreadExit {
    u32 code;
};

void tlsSetupThread(u32 teb, size_t first) {
    Memory& m = host().mem;
    for (size_t i = first; i < s_tlsImages.size(); i++) {
        const TlsImage& ti = s_tlsImages[i];
        const u32 size = ti.end - ti.start + ti.zeroFill;
        const u32 block = host().alloc(size + 16);
        for (u32 k = 0; k < ti.end - ti.start; k++) m.w8(block + k, m.r8(ti.start + k));
        for (u32 k = ti.end - ti.start; k < size; k++) m.w8(block + k, 0);
        m.w32(teb + 0x1000 + kTlsArray + 4 * ti.index, block);
    }
}

u32 tebCreate(u32 tid, u32 stackBase, u32 stackTop) {
    Memory& m = host().mem;
    if (s_nextTeb + kTebSize > kTebRegionEnd) return 0;
    const u32 teb = s_nextTeb;
    s_nextTeb += kTebSize;
    m.map(teb, kTebSize);
    m.w32(teb + 0x00, 0xFFFFFFFF);  // end of the SEH chain
    m.w32(teb + 0x04, stackTop);
    m.w32(teb + 0x08, stackBase);
    m.w32(teb + 0x18, teb);
    m.w32(teb + 0x20, 0x1000);
    m.w32(teb + 0x24, tid);
    m.w32(teb + 0x2C, teb + 0x1000 + kTlsArray);
    m.w32(teb + 0x30, m.r32(s_tebs.empty() ? teb + 0x30 : s_tebs[0] + 0x30));
    s_tebs.push_back(teb);
    tlsSetupThread(teb, 0);
    return teb;
}

void threadMain(std::shared_ptr<XThread> t) {
    t_cpu = t->cpu.get();
    t_tid = t->id;
    t_teb = t->teb;
    Gil gil;
    gilWait([&] { return !t->suspended; });
    Host& h = host();
    try {
        h.threadAttach(*t->cpu);
        t->exitCode = t->cpu->call(t->start, {t->arg});
    } catch (const ThreadExit& e) {
        t->exitCode = e.code;
    } catch (const ModFault& e) {
        rt::log("[modhost] a mod thread (%X) stopped: %s\n", t->id, e.what());
    }
    t->done = true;
    gilNotify();
}

}  // namespace

Cpu& cur() { return t_cpu ? *t_cpu : host().cpu; }
u32 curThreadId() { return t_tid; }
u32 curTeb() { return t_teb ? t_teb : (s_tebs.empty() ? 0 : s_tebs[0]); }

void threadsInit() {
    // The main thread (the game's): its TEB was made by the Host.
    if (!s_tebs.empty()) return;
    s_tebs.push_back(host().cpu.fsBase);
    host().mem.w32(host().cpu.fsBase + 0x2C, host().cpu.fsBase + 0x1000 + kTlsArray);
    t_teb = host().cpu.fsBase;
}

// ---- the lock ----

void gilLock() {
    std::unique_lock<std::mutex> l(g.m);
    const auto me = std::this_thread::get_id();
    if (g.depth && g.owner == me) {
        g.depth++;
        return;
    }
    g.waiting++;
    g.cv.wait(l, [] { return g.depth == 0; });
    g.waiting--;
    g.owner = me;
    g.depth = 1;
}

void gilUnlock() {
    std::lock_guard<std::mutex> l(g.m);
    if (--g.depth == 0) {
        g.owner = {};
        g.cv.notify_all();
    }
}

bool gilWait(const std::function<bool()>& ready, u32 ms) {
    std::unique_lock<std::mutex> l(g.m);
    const auto me = std::this_thread::get_id();
    const int depth = g.depth;
    if (ready()) return true;
    g.depth = 0;
    g.owner = {};
    g.waiting++;
    g.cv.notify_all();
    auto runnable = [&] { return g.depth == 0 && ready(); };
    bool ok = true;
    if (ms == kInfinite) {
        g.cv.wait(l, runnable);
    } else {
        ok = g.cv.wait_until(l, std::chrono::steady_clock::now() + std::chrono::milliseconds(ms), runnable);
        if (!ok) g.cv.wait(l, [] { return g.depth == 0; });
    }
    g.waiting--;
    g.owner = me;
    g.depth = depth;
    return ok;
}

void gilNotify() {
    std::lock_guard<std::mutex> l(g.m);
    g.cv.notify_all();
}

void gilYield() {
    int depth;
    {
        std::lock_guard<std::mutex> l(g.m);
        if (!g.waiting) return;
        depth = g.depth;
        g.depth = 0;
        g.owner = {};
        g.cv.notify_all();
    }
    std::this_thread::yield();
    std::unique_lock<std::mutex> l(g.m);
    g.waiting++;
    g.cv.wait(l, [] { return g.depth == 0; });
    g.waiting--;
    g.owner = std::this_thread::get_id();
    g.depth = depth;
}

// ---- threads ----

u32 threadCreate(u32 start, u32 arg, u32 stackSize, bool suspended, u32* id) {
    Memory& m = host().mem;
    stackSize = std::max<u32>(0x40000, (stackSize + 0xFFFF) & ~0xFFFFu);
    if (s_nextStack + stackSize > kStackRegionEnd) return 0;
    const u32 stackBase = s_nextStack;
    s_nextStack += stackSize + 0x10000;  // a guard gap
    m.map(stackBase, stackSize);
    auto t = std::make_shared<XThread>();
    t->cpu = std::make_unique<Cpu>(m);
    t->id = s_nextTid += 4;
    t->teb = tebCreate(t->id, stackBase, stackBase + stackSize);
    if (!t->teb) return 0;
    t->cpu->fsBase = t->teb;
    t->cpu->r[Cpu::ESP] = stackBase + stackSize - 16;
    t->start = start, t->arg = arg, t->suspended = suspended;
    auto obj = std::make_shared<ThreadObject>();
    obj->t = t;
    const u32 handle = handleNew(obj);
    if (id) *id = t->id;
    rt::log("[modhost] %s starts a thread (%X) at %08X\n", host().currentMod.c_str(), t->id, start);
    std::thread(threadMain, t).detach();
    return handle;
}

void threadResume(u32 handle) {
    if (auto o = handleAs<ThreadObject>(handle)) {
        o->t->suspended = false;
        gilNotify();
    }
}

bool threadDone(u32 handle, u32* exitCode) {
    auto o = handleAs<ThreadObject>(handle);
    if (!o) return true;
    if (exitCode) *exitCode = o->t->done ? o->t->exitCode : 259;  // STILL_ACTIVE
    return o->t->done;
}

void threadExit(u32 code) { throw ThreadExit{code}; }

// ---- TLS ----

void tlsAddImage(u32 start, u32 end, u32 zeroFill, u32 indexAddr) {
    const u32 index = u32(s_tlsImages.size());
    s_tlsImages.push_back({start, end, zeroFill, index});
    host().mem.w32(indexAddr, index);
    for (u32 teb : s_tebs) tlsSetupThread(teb, index);
}

u32 tlsSlotAlloc() {
    for (u32 i = 0; i < 64; i++)
        if (!(s_tlsSlots >> i & 1)) {
            s_tlsSlots |= u64(1) << i;
            for (u32 teb : s_tebs) host().mem.w32(teb + 0xE10 + 4 * i, 0);
            return i;
        }
    return 0xFFFFFFFF;  // TLS_OUT_OF_INDEXES
}
void tlsSlotFree(u32 slot) {
    if (slot < 64) s_tlsSlots &= ~(u64(1) << slot);
}
u32 tlsSlotGet(u32 slot) { return slot < 64 ? host().mem.r32(curTeb() + 0xE10 + 4 * slot) : 0; }
void tlsSlotSet(u32 slot, u32 v) {
    if (slot < 64) host().mem.w32(curTeb() + 0xE10 + 4 * slot, v);
}
u32 tebScratch(u32 offset) { return curTeb() + 0x1000 + offset; }

}  // namespace mh
