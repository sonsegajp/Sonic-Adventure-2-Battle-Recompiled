// Sampling profiler (SA2_PROFILE=<seconds>): every millisecond the host threads that run guest
// code are stopped for a moment and their instruction pointer recorded; every <seconds> the
// hottest functions are printed (names from the PDB: generated code appears as f_XXXXXXXX). Samples
// of threads that are waiting (instruction pointer outside sa2b.exe) are not counted.
#include "runtime/runtime.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <dbghelp.h>
#include <timeapi.h>

#include <algorithm>
#include <cstdlib>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#pragma comment(lib, "dbghelp.lib")
#pragma comment(lib, "winmm.lib")

namespace rt {

namespace {
std::mutex s_lock;
std::vector<HANDLE> s_threads;
bool s_enabled = std::getenv("SA2_PROFILE") != nullptr;

// SA2_PROFILE_WAITS: also record where waiting threads wait (their innermost frames in sa2b.exe).
const bool s_waits = std::getenv("SA2_PROFILE_WAITS") != nullptr;

void symInit() {
    static bool s_done = false;
    if (s_done) return;
    s_done = true;
    SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
    SymInitialize(GetCurrentProcess(), nullptr, TRUE);
}

std::string symName(u64 addr) {
    alignas(SYMBOL_INFO) char buf[sizeof(SYMBOL_INFO) + 256];
    auto* sym = reinterpret_cast<SYMBOL_INFO*>(buf);
    sym->SizeOfStruct = sizeof(SYMBOL_INFO);
    sym->MaxNameLen = 255;
    DWORD64 disp = 0;
    return SymFromAddr(GetCurrentProcess(), addr, &disp, sym) ? std::string(sym->Name) : "?";
}

void reportWaits(std::map<std::vector<u64>, u32>& waits, u64 total) {
    std::map<std::string, u32> byChain;
    for (auto& [chain, n] : waits) {
        std::string key;
        for (u64 a : chain)
            if (a) key += (key.empty() ? "" : " < ") + symName(a);
        byChain[key] += n;
    }
    std::vector<std::pair<u32, std::string>> top;
    for (auto& [k, n] : byChain) top.push_back({n, k});
    std::sort(top.rbegin(), top.rend());
    log("[profile] %llu waiting samples; top wait sites:\n", (unsigned long long)total);
    for (size_t i = 0; i < top.size() && i < 15; i++)
        log("[profile] %5.1f%%  %s\n", 100.0 * top[i].first / double(total), top[i].second.c_str());
    waits.clear();
}

void report(std::unordered_map<u64, u32>& hits, u64 total) {
    symInit();
    std::map<std::string, u32> byName;
    for (auto& [rip, n] : hits) byName[symName(rip)] += n;
    std::vector<std::pair<u32, std::string>> top;
    for (auto& [name, n] : byName) top.push_back({n, name});
    std::sort(top.rbegin(), top.rend());
    log("[profile] %llu samples in guest-running threads; top functions:\n", (unsigned long long)total);
    for (size_t i = 0; i < top.size() && i < 40; i++)
        log("[profile] %5.1f%%  %s\n", 100.0 * top[i].first / double(total), top[i].second.c_str());
    // The hottest single addresses, with source lines where the PDB has them (runtime code).
    std::vector<std::pair<u32, u64>> rips;
    for (auto& [rip, n] : hits) rips.push_back({n, rip});
    std::sort(rips.rbegin(), rips.rend());
    for (size_t i = 0; i < rips.size() && i < 12; i++) {
        IMAGEHLP_LINE64 line{sizeof(IMAGEHLP_LINE64)};
        DWORD disp = 0;
        const bool have = SymGetLineFromAddr64(GetCurrentProcess(), rips[i].second, &disp, &line);
        log("[profile]   %5.1f%% at %s %s:%lu\n", 100.0 * rips[i].first / double(total), symName(rips[i].second).c_str(),
            have ? line.FileName : "", have ? line.LineNumber : 0ul);
    }
    hits.clear();
}

void profilerMain(int seconds) {
    timeBeginPeriod(1);
    HMODULE exe = GetModuleHandleA(nullptr);
    const u64 lo = reinterpret_cast<u64>(exe);
    IMAGE_NT_HEADERS* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(reinterpret_cast<u8*>(exe) + reinterpret_cast<IMAGE_DOS_HEADER*>(exe)->e_lfanew);
    const u64 hi = lo + nt->OptionalHeader.SizeOfImage;
    std::unordered_map<u64, u32> hits;
    std::map<std::vector<u64>, u32> waits;
    u64 total = 0, waitTotal = 0;
    if (s_waits) symInit();
    auto last = GetTickCount64();
    for (;;) {
        Sleep(1);
        std::vector<HANDLE> threads;
        {
            std::lock_guard g(s_lock);
            threads = s_threads;
        }
        for (HANDLE h : threads) {
            CONTEXT ctx{};
            ctx.ContextFlags = s_waits ? CONTEXT_FULL : CONTEXT_CONTROL;
            if (SuspendThread(h) == DWORD(-1)) continue;
            const bool ok = GetThreadContext(h, &ctx);
            u64 chain[6] = {};
            if (ok && s_waits && (ctx.Rip < lo || ctx.Rip >= hi)) {
                // Waiting: walk the stack (while the thread is stopped) to the first frames in sa2b.exe.
                STACKFRAME64 f{};
                f.AddrPC = {ctx.Rip, 0, AddrModeFlat};
                f.AddrFrame = {ctx.Rbp, 0, AddrModeFlat};
                f.AddrStack = {ctx.Rsp, 0, AddrModeFlat};
                int n = 0;
                for (int depth = 0; depth < 48 && n < 6; depth++) {
                    if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, GetCurrentProcess(), h, &f, &ctx, nullptr,
                                     SymFunctionTableAccess64, SymGetModuleBase64, nullptr) || !f.AddrPC.Offset)
                        break;
                    if (f.AddrPC.Offset >= lo && f.AddrPC.Offset < hi) chain[n++] = f.AddrPC.Offset;
                }
            }
            ResumeThread(h);
            if (!ok) continue;
            if (ctx.Rip < lo || ctx.Rip >= hi) {  // waiting, or in a system DLL
                if (chain[0]) {
                    waits[std::vector<u64>(chain, chain + 6)]++;
                    waitTotal++;
                }
                continue;
            }
            hits[ctx.Rip]++;
            total++;
        }
        if (GetTickCount64() - last >= u64(seconds) * 1000) {
            report(hits, total);
            if (s_waits) reportWaits(waits, waitTotal);
            total = waitTotal = 0;
            last = GetTickCount64();
        }
    }
}
}  // namespace

// Called by every host thread that may run guest code.
void profileRegisterThread() {
    if (!s_enabled) return;
    HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, GetCurrentThreadId());
    if (!h) return;
    std::lock_guard g(s_lock);
    s_threads.push_back(h);
    static bool s_started = false;
    if (!s_started) {
        s_started = true;
        const int seconds = std::max(1, std::atoi(std::getenv("SA2_PROFILE")));
        std::thread(profilerMain, seconds).detach();
    }
}

}  // namespace rt
