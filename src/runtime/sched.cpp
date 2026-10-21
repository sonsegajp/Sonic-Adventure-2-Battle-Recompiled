#include "runtime/sched.h"

#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>

namespace rt::sched {

struct Thread {
    u32 guest = 0;
    Ctx* ctx = nullptr;
    std::unique_ptr<Ctx> own;
    std::condition_variable cv;
    bool started = false;
    bool cancelled = false;
    u32 entry = 0;
    u64 seq = 0;  // FIFO order among equal priorities
};

namespace {
std::mutex g_m;
Thread* g_running = nullptr;                // owner of the guest CPU; nullptr = idle
std::unordered_map<u32, Thread*> g_threads;  // by guest OSThread address
int g_schedDisable = 0;
u64 g_seq = 0;
Thread g_irq;  // pseudo thread that owns the CPU while interrupts run on an idle CPU
Ctx g_irqCtx;
u32 g_irqStackTop = 0;
std::deque<std::function<void(Ctx&)>> g_irqq;
thread_local Thread* t_self = nullptr;
thread_local bool t_inIrq = false;

u16 stateOf(u32 g) { return u16(rd16(g + T_STATE)); }
void setState(u32 g, u16 s) { wr16(g + T_STATE, s); }
s32 suspendOf(u32 g) { return s32(rd32(g + T_SUSPEND)); }
s32 prioOf(const Thread* t) { return s32(rd32(t->guest + T_PRIORITY)); }
bool runnable(const Thread* t) {
    if (t->cancelled) return false;
    const u16 s = stateOf(t->guest);
    return (s == S_READY || s == S_RUNNING) && suspendOf(t->guest) <= 0;
}

// Highest-priority runnable thread (FIFO among equals). With `yielding`, the current thread only
// keeps the CPU when nothing of equal or higher priority is ready.
Thread* best(Thread* cur, bool yielding) {
    Thread* b = nullptr;
    for (auto& [g, t] : g_threads) {
        if (!runnable(t) || (yielding && t == cur)) continue;
        if (!b || prioOf(t) < prioOf(b) || (prioOf(t) == prioOf(b) && t->seq < b->seq)) b = t;
    }
    if (yielding && cur && runnable(cur) && (!b || prioOf(cur) < prioOf(b))) b = cur;
    return b;
}

void threadMain(Thread* t);

void giveCpu(Thread* next) {
    g_running = next;
    if (!next) return;
    setState(next->guest, S_RUNNING);
    wr32(kCurrentThread, next->guest);
    wr32(kCurrentContext, phys(next->guest));
    if (!next->started) {
        next->started = true;
        std::thread(threadMain, next).detach();
    } else {
        next->cv.notify_one();
    }
}

// Waits until `self` owns the CPU again. A cancelled thread unwinds instead.
void waitTurn(std::unique_lock<std::mutex>& lk, Thread* self) {
    self->cv.wait(lk, [&] { return g_running == self || self->cancelled; });
    if (self->cancelled && g_running != self) {
        lk.unlock();
        throw ThreadExit{};
    }
}

// Runs queued interrupts; the caller has made g_running something only it uses.
void serveQueue(std::unique_lock<std::mutex>& lk) {
    while (!g_irqq.empty()) {
        auto fn = std::move(g_irqq.front());
        g_irqq.pop_front();
        lk.unlock();
        const bool was = t_inIrq;
        t_inIrq = true;
        g_irqCtx.r[1] = g_irqStackTop - 0x100;
        g_irqCtx.msr = 0;  // handlers run with external interrupts off
        fn(g_irqCtx);
        t_inIrq = was;
        lk.lock();
    }
    g_irqPending = 0;
}

// The current owner gives up the CPU: hand it to the best thread, or serve interrupts while idle.
void reschedule(std::unique_lock<std::mutex>& lk) {
    for (;;) {
        if (Thread* next = best(nullptr, false)) {
            giveCpu(next);
            return;
        }
        if (g_irqq.empty()) {
            g_running = nullptr;
            return;
        }
        g_running = &g_irq;
        serveQueue(lk);
    }
}

// After something became ready: switch if a higher-priority thread can run now.
void maybePreempt(std::unique_lock<std::mutex>& lk) {
    Thread* self = t_self;
    if (t_inIrq || g_schedDisable || !self || g_running != self) return;
    Thread* b = best(self, false);
    if (b && b != self && prioOf(b) < prioOf(self)) {
        setState(self->guest, S_READY);
        self->seq = ++g_seq;
        giveCpu(b);
        waitTurn(lk, self);
    }
}

void threadMain(Thread* t) {
    t_self = t;
    {
        std::unique_lock lk(g_m);
        waitTurn(lk, t);
    }
    try {
        Ctx& c = *t->ctx;
        c.call(t->entry);
        exitThread(c.r[3]);
    } catch (const ThreadExit&) {
    }
}

void queueAppend(u32 queue, u32 g) {
    const u32 tail = rd32(queue + 4);
    wr32(g + T_LINK, 0);
    wr32(g + T_LINK + 4, tail);
    if (tail) wr32(tail + T_LINK, g);
    else wr32(queue, g);
    wr32(queue + 4, g);
}

void wakeupLocked(u32 queue) {
    for (u32 g = rd32(queue); g;) {
        const u32 next = rd32(g + T_LINK);
        wr32(g + T_LINK, 0);
        wr32(g + T_LINK + 4, 0);
        wr32(g + T_QUEUE, 0);
        if (stateOf(g) == S_WAITING) setState(g, S_READY);
        if (auto it = g_threads.find(g); it != g_threads.end()) it->second->seq = ++g_seq;
        g = next;
    }
    wr32(queue, 0);
    wr32(queue + 4, 0);
}
}  // namespace

Thread* current() { return t_self; }
bool inInterrupt() { return t_inIrq; }
Ctx* currentCtx() { return t_inIrq ? &g_irqCtx : (t_self ? t_self->ctx : nullptr); }

void adoptBootThread(u32 guest, Ctx& ctx) {
    std::unique_lock lk(g_m);
    auto* t = new Thread;
    t->guest = guest;
    t->ctx = &ctx;
    t->started = true;
    t->seq = ++g_seq;
    g_threads[guest] = t;
    t_self = t;
    g_running = t;
    ctx.thread = t;
    setState(guest, S_RUNNING);
    wr32(kCurrentThread, guest);
    // Interrupt context: own stack, same small-data bases as the game.
    g_irqStackTop = hleAlloc(0x10000, 32) + 0x10000;
    g_irqCtx.r[2] = ctx.r[2];
    g_irqCtx.r[13] = ctx.r[13];
    g_irqCtx.thread = &g_irq;
    log("[sched] boot thread %08X (priority %d)\n", guest, prioOf(t));
}

bool create(u32 g, u32 entry, u32 arg, u32 stackTop, u32 stackSize, s32 prio, u32 attr) {
    if (prio < 0 || prio > 31) return false;
    std::unique_lock lk(g_m);
    std::memset(hostp(g), 0, T_STACKEND + 4);
    setState(g, S_READY);
    wr16(g + T_ATTR, attr & 1);
    wr32(g + T_SUSPEND, 1);
    wr32(g + T_PRIORITY, u32(prio));
    wr32(g + T_BASE, u32(prio));
    wr32(g + T_STACKBASE, stackTop);
    wr32(g + T_STACKEND, stackTop - stackSize);
    wr32(stackTop - stackSize, 0xDEADBABE);
    const u32 sp = ((stackTop & ~7u) - 8);
    wr32(sp, 0);
    wr32(sp + 4, 0);
    wr32(g + 4, sp);  // context.gpr[1]
    wr32(g + 12, arg);
    if (auto it = g_threads.find(g); it != g_threads.end()) it->second->cancelled = true;  // reused OSThread
    auto* t = new Thread;
    t->guest = g;
    t->own = std::make_unique<Ctx>();
    t->ctx = t->own.get();
    t->ctx->r[1] = sp;
    t->ctx->r[2] = g_irqCtx.r[2];
    t->ctx->r[3] = arg;
    t->ctx->r[13] = g_irqCtx.r[13];
    t->ctx->thread = t;
    t->entry = entry;
    t->seq = ++g_seq;
    g_threads[g] = t;
    return true;
}

s32 resume(u32 g) {
    std::unique_lock lk(g_m);
    const s32 s = suspendOf(g);
    if (s > 0) {
        wr32(g + T_SUSPEND, u32(s - 1));
        if (auto it = g_threads.find(g); it != g_threads.end() && s - 1 == 0) it->second->seq = ++g_seq;
        maybePreempt(lk);
    }
    return s;
}

s32 suspend(u32 g) {
    std::unique_lock lk(g_m);
    const s32 s = suspendOf(g);
    wr32(g + T_SUSPEND, u32(s + 1));
    Thread* self = t_self;
    if (self && self->guest == g && !t_inIrq) {
        setState(g, S_READY);
        reschedule(lk);
        waitTurn(lk, self);
    }
    return s;
}

void sleepOn(u32 queue) {
    std::unique_lock lk(g_m);
    Thread* self = t_self;
    if (!self || t_inIrq) fatal("OSSleepThread outside a thread (queue %08X)", queue);
    setState(self->guest, S_WAITING);
    wr32(self->guest + T_QUEUE, queue);
    queueAppend(queue, self->guest);
    reschedule(lk);
    waitTurn(lk, self);
}

void wakeup(u32 queue) {
    std::unique_lock lk(g_m);
    wakeupLocked(queue);
    maybePreempt(lk);
}

void yield() {
    std::unique_lock lk(g_m);
    Thread* self = t_self;
    if (!self || t_inIrq) return;
    Thread* b = best(self, true);
    if (b && b != self) {
        setState(self->guest, S_READY);
        self->seq = ++g_seq;
        giveCpu(b);
        waitTurn(lk, self);
    }
}

void exitThread(u32 val) {
    std::unique_lock lk(g_m);
    Thread* self = t_self;
    const u32 g = self->guest;
    wr32(g + T_VAL, val);
    setState(g, (rd16(g + T_ATTR) & 1) ? 0 : S_MORIBUND);
    wakeupLocked(g + T_JOIN);
    self->cancelled = true;
    reschedule(lk);
    lk.unlock();
    throw ThreadExit{};
}

void cancel(u32 g) {
    std::unique_lock lk(g_m);
    if (t_self && t_self->guest == g) {
        lk.unlock();
        exitThread(0);
    }
    auto it = g_threads.find(g);
    setState(g, (rd16(g + T_ATTR) & 1) ? 0 : S_MORIBUND);
    wakeupLocked(g + T_JOIN);
    if (it != g_threads.end()) {
        it->second->cancelled = true;
        it->second->cv.notify_one();
    }
    maybePreempt(lk);
}

bool join(u32 g, u32* val) {
    for (;;) {
        {
            std::unique_lock lk(g_m);
            if (stateOf(g) == S_MORIBUND || stateOf(g) == 0) {
                if (val) *val = rd32(g + T_VAL);
                setState(g, 0);
                return true;
            }
        }
        sleepOn(g + T_JOIN);
    }
}

void setPriority(u32 g, s32 prio) {
    std::unique_lock lk(g_m);
    wr32(g + T_PRIORITY, u32(prio));
    wr32(g + T_BASE, u32(prio));
    maybePreempt(lk);
}

void disableScheduler() {
    std::unique_lock lk(g_m);
    g_schedDisable++;
}

void enableScheduler() {
    std::unique_lock lk(g_m);
    if (g_schedDisable > 0 && --g_schedDisable == 0) maybePreempt(lk);
}

// ---- interrupts ----
void postIrq(std::function<void(Ctx&)> fn) {
    std::unique_lock lk(g_m);
    g_irqq.push_back(std::move(fn));
    g_irqPending = 1;
    if (g_running == nullptr) {
        g_running = &g_irq;
        serveQueue(lk);
        reschedule(lk);
    }
}

void serviceInterrupts(Ctx& c) {
    if (!(c.msr & 0x8000) || t_inIrq) return;
    std::unique_lock lk(g_m);
    Thread* self = t_self;
    if (!self || g_running != self || g_irqq.empty()) {
        if (g_irqq.empty()) g_irqPending = 0;
        return;
    }
    g_running = &g_irq;
    serveQueue(lk);
    g_running = self;
    if (!g_schedDisable) {
        Thread* b = best(self, false);
        if (b && b != self && prioOf(b) < prioOf(self)) {
            setState(self->guest, S_READY);
            self->seq = ++g_seq;
            giveCpu(b);
            waitTurn(lk, self);
        }
    }
}

void withCpuReleased(const std::function<void()>& fn) {
    Thread* self = t_self;
    if (!self || t_inIrq) {
        fn();
        return;
    }
    {
        std::unique_lock lk(g_m);
        setState(self->guest, S_WAITING);
        reschedule(lk);
    }
    fn();
    std::unique_lock lk(g_m);
    setState(self->guest, S_READY);
    self->seq = ++g_seq;
    if (g_running == nullptr) {
        reschedule(lk);
    } else {
        // Ask the running thread to look at the run queue at its next safe point.
        g_irqq.push_back([](Ctx&) {});
        g_irqPending = 1;
    }
    waitTurn(lk, self);
}

// ---- alarms: a host timer thread posts each due alarm as an interrupt ----
namespace {
struct AlarmEntry {
    u32 alarm, handler;
    u64 period;
};
std::mutex g_am;
std::condition_variable g_acv;
std::multimap<u64, AlarmEntry> g_alarms;  // key: time base tick
bool g_timerStarted = false;

void timerMain() {
    std::unique_lock lk(g_am);
    for (;;) {
        if (g_alarms.empty()) {
            g_acv.wait(lk);
            continue;
        }
        const u64 now = timebase();
        auto it = g_alarms.begin();
        if (it->first > now) {
            const u64 waitNs = (it->first - now) * 1000000000ull / kTimebaseHz;
            g_acv.wait_for(lk, std::chrono::nanoseconds(waitNs));
            continue;
        }
        const AlarmEntry e = it->second;
        const u64 when = it->first;
        g_alarms.erase(it);
        if (e.period) g_alarms.emplace(when + e.period, e);
        lk.unlock();
        postIrq([e](Ctx& c) {
            if (!e.period) wr32(e.alarm, 0);  // one-shot: handler cleared like OSAlarm does
            c.r[3] = e.alarm;
            c.r[4] = phys(rd32(kCurrentThread));
            c.call(e.handler);
        });
        lk.lock();
    }
}
}  // namespace

void setAlarm(u32 alarm, u64 when, u64 period, u32 handler) {
    std::unique_lock lk(g_am);
    for (auto it = g_alarms.begin(); it != g_alarms.end();)
        it = it->second.alarm == alarm ? g_alarms.erase(it) : std::next(it);
    wr32(alarm, handler);
    wr32(alarm + 8, u32(when >> 32));
    wr32(alarm + 12, u32(when));
    g_alarms.emplace(when, AlarmEntry{alarm, handler, period});
    if (!g_timerStarted) {
        g_timerStarted = true;
        std::thread(timerMain).detach();
    }
    g_acv.notify_one();
}

void cancelAlarm(u32 alarm) {
    std::unique_lock lk(g_am);
    for (auto it = g_alarms.begin(); it != g_alarms.end();)
        it = it->second.alarm == alarm ? g_alarms.erase(it) : std::next(it);
    wr32(alarm, 0);
}

}  // namespace rt::sched

namespace rt::sched {
void debugDump() {
    std::unique_lock lk(g_m);
    log("[sched] running %08X, %zu interrupts queued, pending flag %u, scheduler-disable %d\n",
        g_running ? g_running->guest : 0u, g_irqq.size(), g_irqPending, g_schedDisable);
    for (auto& [g, t] : g_threads)
        log("  thread %08X state %u suspend %d prio %d msr.EE %u started %d cancelled %d\n", g, stateOf(g), suspendOf(g),
            prioOf(t), (t->ctx->msr >> 15) & 1, t->started, t->cancelled);
}
}  // namespace rt::sched

namespace rt {
void raiseInterrupt(u32 id, void (*before)()) {
    sched::postIrq([id, before](Ctx& c) {
        if (before) before();
        const u32 h = rd32(kInterruptTable + 4 * id);
        if (!h) return;
        c.r[3] = id;
        c.r[4] = phys(rd32(sched::kCurrentThread));
        c.call(h);
    });
}
}  // namespace rt
