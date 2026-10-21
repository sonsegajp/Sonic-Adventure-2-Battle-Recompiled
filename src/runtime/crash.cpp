// Crash reporting: an unhandled exception (access violation, ...) prints the exception, the host
// address (with the module it belongs to) and what every guest thread was doing.
#include "runtime/runtime.h"
#include "runtime/sched.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <cstdio>

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
    std::fprintf(stdout, "\nCRASH: exception %08lX at %p (%s+%llX)", r->ExceptionCode, r->ExceptionAddress, name,
                 (unsigned long long)(pc - reinterpret_cast<uintptr_t>(mod)));
    if (r->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && r->NumberParameters >= 2) {
        const auto addr = r->ExceptionInformation[1];
        const auto mem = reinterpret_cast<uintptr_t>(g_mem);
        std::fprintf(stdout, ", %s %llX", r->ExceptionInformation[0] ? "writing" : "reading", (unsigned long long)addr);
        if (addr >= mem && addr < mem + kHostMemSize) std::fprintf(stdout, " (guest memory +%llX)", (unsigned long long)(addr - mem));
    }
    std::fprintf(stdout, "\n");
    std::fflush(stdout);
    dumpAllThreads();
    sched::debugDump();
    std::fflush(stdout);
    return EXCEPTION_EXECUTE_HANDLER;
}
}  // namespace

void installCrashHandler() { SetUnhandledExceptionFilter(onCrash); }

}  // namespace rt
