// Developer entry point (sa2b): load main.dol into guest memory, set up the boot block the IPL
// would leave behind, and run the recompiled __start.
//   sa2b --dol <main.dol> --iso <disc image> [--trace]
#include "runtime/runtime.h"
#include "runtime/sched.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>

namespace rt {
void dvdOpenDisc(const std::string& path);  // src/runtime/hle_dvd.cpp
void dumpAllThreads();                       // src/runtime/cpu.cpp
void installCrashHandler();                  // src/runtime/crash.cpp
void installWatch(u32 guest, u32 len);       // src/runtime/crash.cpp
int runFrontend(int argc, char** argv, bool headless, u64 maxFrames, void (*startGuest)());  // frontend.cpp
}

using namespace rt;

static bool readFile(const std::string& p, std::vector<u8>& out) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return false;
    out.assign(std::istreambuf_iterator<char>(f), {});
    return true;
}

// The boot block (OSBootInfo at 0x80000000 and friends) as the IPL leaves it for a retail disc.
static void setupLowMem(const std::vector<u8>& discHeader) {
    std::memcpy(hostp(0x80000000), discHeader.data(), 0x20);  // game id, maker, disc, version
    wr32(0x8000001C, 0xC2339F3D);  // DVD magic
    wr32(0x80000020, 0x0D15EA5E);  // normal boot
    wr32(0x80000024, 1);           // boot info version
    wr32(0x80000028, kMem1Size);   // physical memory size
    wr32(0x8000002C, 0x00000003);  // console type: retail
    wr32(0x80000030, 0);           // arena lo: linker default
    wr32(0x80000034, 0x80000000 + kMem1Size);  // arena hi: lowered to the FST by dvdOpenDisc
    wr32(0x80000038, 0);                        // FST location, size: set by dvdOpenDisc
    wr32(0x8000003C, 0);
    wr32(0x800000CC, 0);           // TV mode: NTSC
    wr32(0x800000F0, kMem1Size);   // simulated memory size
    wr32(0x800000F8, 162000000);   // bus clock
    wr32(0x800000FC, 486000000);   // CPU clock
    wr16(0x800030E6, 0x8003);      // device code: retail
}

int main(int argc, char** argv) {
    std::string dolPath = "orig/main.dol", isoPath;
    int watchSecs = 0;
    bool headless = false;
    u64 maxFrames = 0;
    for (int i = 1; i < argc; i++) {
        const std::string a = argv[i];
        if (a == "--dol" && i + 1 < argc) dolPath = argv[++i];
        else if (a == "--iso" && i + 1 < argc) isoPath = argv[++i];
        else if (a == "--trace") g_traceOn = true;
        else if (a == "--watch" && i + 1 < argc) watchSecs = std::atoi(argv[++i]);
        else if (a == "--headless") headless = true;
        else if (a == "--frames" && i + 1 < argc) maxFrames = std::strtoull(argv[++i], nullptr, 10);
    }
    installCrashHandler();
    if (!memInit()) return std::fprintf(stderr, "out of memory\n"), 1;
    std::vector<u8> dol;
    if (!readFile(dolPath, dol)) return std::fprintf(stderr, "can't read %s\n", dolPath.c_str()), 1;
    u32 entry = 0;
    if (!loadDol(dol, entry)) return std::fprintf(stderr, "bad DOL %s\n", dolPath.c_str()), 1;
    std::vector<u8> header(0x20, 0);
    if (!isoPath.empty()) {
        std::ifstream f(isoPath, std::ios::binary);
        f.read(reinterpret_cast<char*>(header.data()), 0x20);
    } else {
        std::memcpy(header.data(), "GSNE8P", 6);
    }
    setupLowMem(header);
    if (!isoPath.empty()) dvdOpenDisc(isoPath);  // also places the FST and records it in the boot block
    registerDol();
    resolveHooks();
    if (const char* w = std::getenv("SA2_WATCH")) {  // SA2_WATCH=<hex address>[:<length>]
        const char* colon = std::strchr(w, ':');
        installWatch(u32(std::strtoul(w, nullptr, 16)), colon ? u32(std::strtoul(colon + 1, nullptr, 0)) : 4);
    }
    log("[boot] %s entry %08X\n", dolPath.c_str(), entry);
    if (watchSecs > 0) {
        g_traceOn = true;
        std::thread([watchSecs] {
            for (;;) {
                std::this_thread::sleep_for(std::chrono::seconds(watchSecs));
                dumpAllThreads();
                sched::debugDump();
            }
        }).detach();
    }
    static u32 s_entry;
    s_entry = entry;
    // The game runs on its own host thread; this one owns the window.
    return runFrontend(argc, argv, headless, maxFrames, [] {
        static Ctx boot;
        boot.msr = 0;  // __start runs with interrupts off; OSInit enables them
        boot.call(s_entry);
        log("[boot] __start returned\n");
    });
}
