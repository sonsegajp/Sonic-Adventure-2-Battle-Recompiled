// Crash reporting: an unhandled exception (access violation, ...) prints the exception, the host
// address (with the module it belongs to) and what every guest thread was doing.
#include "runtime/runtime.h"
#include "runtime/sched.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace rt {

void dumpAllThreads();  // src/runtime/cpu.cpp

namespace {
LONG WINAPI onCrash(EXCEPTION_POINTERS* ep) {
    const auto* r = ep->ExceptionRecord;
    const auto pc = reinterpret_cast<uintptr_t>(r->ExceptionAddress);
    HMODULE mod = nullptr;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCSTR>(pc), &mod);
    char name[MAX_PATH] = "?";
    if (mod) GetModuleFileNameA(mod, name, MAX_PATH);
    // (through log(): into sa2b.log too - a game started by the launcher has no console)
    char what[160] = "";
    if (r->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && r->NumberParameters >= 2) {
        const auto addr = r->ExceptionInformation[1];
        const auto mem = reinterpret_cast<uintptr_t>(g_mem);
        std::snprintf(what, sizeof what, ", %s %llX%s", r->ExceptionInformation[0] ? "writing" : "reading", (unsigned long long)addr,
                      addr >= mem && addr < mem + kHostMemSize ? " (guest memory)" : "");
    }
    log("\nCRASH: exception %08lX at %p (%s+%llX)%s\n", r->ExceptionCode, r->ExceptionAddress, name,
        (unsigned long long)(pc - reinterpret_cast<uintptr_t>(mod)), what);
    // the host call stack (through the exception dispatch into the faulting code), as module+offset
    void* frames[48];
    const USHORT n = CaptureStackBackTrace(0, 48, frames, nullptr);
    for (USHORT i = 0; i < n; i++) {
        HMODULE m = nullptr;
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, static_cast<LPCSTR>(frames[i]), &m);
        char mn[MAX_PATH] = "?";
        if (m) GetModuleFileNameA(m, mn, MAX_PATH);
        const char* base = std::strrchr(mn, '\\');
        log("  %s+%llX\n", base ? base + 1 : mn, (unsigned long long)(reinterpret_cast<uintptr_t>(frames[i]) - reinterpret_cast<uintptr_t>(m)));
    }
    dumpAllThreads();
    sched::debugDump();
    std::fflush(stdout);
    return EXCEPTION_EXECUTE_HANDLER;
}
}  // namespace

// abort() (also reached from std::terminate, e.g. an uncaught C++ exception) fast-fails past the
// exception filter; report it the same way, with the host call stack as module+offset.
static void onAbort(int) {
    log("\nABORT on host thread %lu; host stack:\n", GetCurrentThreadId());
    void* frames[48];
    const USHORT n = CaptureStackBackTrace(1, 48, frames, nullptr);
    for (USHORT i = 0; i < n; i++) {
        HMODULE mod = nullptr;
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           static_cast<LPCSTR>(frames[i]), &mod);
        char name[MAX_PATH] = "?";
        if (mod) GetModuleFileNameA(mod, name, MAX_PATH);
        const char* base = std::strrchr(name, '\\');
        log("  %s+%llX\n", base ? base + 1 : name, (unsigned long long)(reinterpret_cast<uintptr_t>(frames[i]) - reinterpret_cast<uintptr_t>(mod)));
    }
    std::fflush(stdout);
    dumpAllThreads();
    sched::debugDump();
    std::fflush(stdout);
    std::_Exit(3);
}

void installCrashHandler() {
    SetUnhandledExceptionFilter(onCrash);
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    std::signal(SIGABRT, onAbort);
}

// Debug watchpoint (SA2_WATCH=<guest address>): the host page holding it is made read-only and
// the first write to it is reported with what the guest threads were doing, then allowed.
namespace {
uintptr_t s_watchPage = 0;
u32 s_watchGuest = 0, s_watchLen = 0;
int s_watchHits = 0, s_watchMax = 3;
thread_local bool t_rearm = false;  // this thread is single-stepping past a write to the page
LONG WINAPI onWatch(EXCEPTION_POINTERS* ep) {
    const auto* r = ep->ExceptionRecord;
    DWORD old;
    if (r->ExceptionCode == EXCEPTION_SINGLE_STEP && t_rearm) {
        t_rearm = false;
        VirtualProtect(reinterpret_cast<void*>(s_watchPage), 0x1000, PAGE_READONLY, &old);
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (r->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || r->NumberParameters < 2 || !s_watchPage) return EXCEPTION_CONTINUE_SEARCH;
    const uintptr_t addr = r->ExceptionInformation[1];
    if ((addr & ~uintptr_t(0xFFF)) != s_watchPage) return EXCEPTION_CONTINUE_SEARCH;
    const u32 guest = 0x80000000u | u32(addr - reinterpret_cast<uintptr_t>(g_mem));
    if (guest >= s_watchGuest && guest < s_watchGuest + s_watchLen && s_watchHits++ < s_watchMax) {
        std::fprintf(stdout, "\n[watch] write to %08X (watching %08X+%u) from host %p\n", guest, s_watchGuest, s_watchLen,
                     r->ExceptionAddress);
        if (const Ctx* c = sched::currentCtx()) {
            for (int i = 0; i < 32; i++) std::fprintf(stdout, "r%-2d %08X%s", i, c->r[i], i % 8 == 7 ? "\n" : "  ");
            std::fprintf(stdout, "lr %08X  ctr %08X\nguest stack:", c->lr, c->ctr);
            // Back chain: each frame's first word links to the caller's, whose LR save word is at +4.
            u32 sp = c->r[1];
            for (int i = 0; i < 16 && (sp >> 24) == 0x80 + ((sp >> 24) & 1); i++) {
                sp = rd32(sp);
                if (!sp) break;
                const u32 ret = rd32(sp + 4);
                const char* nm = guestName(ret);
                std::fprintf(stdout, " %08X%s%s", ret, nm ? ":" : "", nm ? nm : "");
            }
            std::fprintf(stdout, "\n");
        }
        dumpAllThreads();
        std::fflush(stdout);
    }
    // Let this one write through: unprotect, single-step it, protect again.
    VirtualProtect(reinterpret_cast<void*>(s_watchPage), 0x1000, PAGE_READWRITE, &old);
    ep->ContextRecord->EFlags |= 0x100;
    t_rearm = true;
    return EXCEPTION_CONTINUE_EXECUTION;
}
}  // namespace

void installWatch(u32 guest, u32 len) {
    s_watchGuest = guest;
    s_watchLen = len;
    if (const char* m = std::getenv("SA2_WATCH_HITS")) s_watchMax = std::atoi(m);
    s_watchPage = reinterpret_cast<uintptr_t>(hostp(guest)) & ~uintptr_t(0xFFF);
    AddVectoredExceptionHandler(1, onWatch);
    DWORD old;
    VirtualProtect(reinterpret_cast<void*>(s_watchPage), 0x1000, PAGE_READONLY, &old);
    log("[watch] guarding %08X\n", guest);
}

}  // namespace rt
