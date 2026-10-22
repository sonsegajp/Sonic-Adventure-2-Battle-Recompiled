// Threads of mod code. Each has its own x86 Cpu, stack and thread environment block (TEB); all
// share the one address space. All x86 code runs under one lock (the "GIL", as in Python): one
// thread executes mod code at a time, and a thread waiting (Sleep, events, locks, joins) lets
// the others run. The game thread takes the lock whenever it runs mod code (Init, hooks, frames).
#pragma once

#include "modhost/x86.h"

#include <chrono>
#include <functional>

namespace mh {

constexpr u32 kInfinite = 0xFFFFFFFFu;

// The Cpu of the calling host thread (the game thread's is host().cpu).
Cpu& cur();
u32 curThreadId();
u32 curTeb();

// ---- the lock over all x86 execution (recursive per host thread) ----
void gilLock();
void gilUnlock();
struct Gil {
    Gil() { gilLock(); }
    ~Gil() { gilUnlock(); }
    Gil(const Gil&) = delete;
    Gil& operator=(const Gil&) = delete;
};
// Waits until `ready()` (evaluated with the lock held) or `ms` elapse, letting other threads run
// meanwhile; returns ready(). The caller holds the lock before and after.
bool gilWait(const std::function<bool()>& ready, u32 ms = kInfinite);
void gilNotify();  // something a waiter may be waiting for changed
void gilYield();   // lets other threads run (long-running threads call it now and then)

// ---- threads ----
// Starts start(arg) on a new thread; returns its handle (0 on failure) and id.
u32 threadCreate(u32 start, u32 arg, u32 stackSize, bool suspended, u32* id);
void threadResume(u32 handle);
bool threadDone(u32 handle, u32* exitCode = nullptr);
[[noreturn]] void threadExit(u32 code);  // from a stand-in on a mod thread

// ---- thread-local storage ----
// Static TLS (__declspec(thread)) of a loaded image: its template, given an index, set up in
// every thread (now and later).
void tlsAddImage(u32 start, u32 end, u32 zeroFill, u32 indexAddr);
u32 tlsSlotAlloc();
void tlsSlotFree(u32 slot);
u32 tlsSlotGet(u32 slot);
void tlsSlotSet(u32 slot, u32 v);
// Per-thread scratch words in the TEB's second page (errno, strtok state...).
u32 tebScratch(u32 offset);

// Runs code on the main thread's Cpu at startup (DllMain, TLS callbacks, Init).
void threadsInit();

}  // namespace mh
