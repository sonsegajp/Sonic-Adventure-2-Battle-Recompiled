// The game module: sa2b_game.dll, the game's code translated by the recompiler from the player's
// own disc - built by the launcher on first run (it is never shipped) or, for development, by the
// CMake target sa2b_game. Loaded first thing; its tables and the state its code owns are reached
// through g_game and the pointers below (src/runtime/ctx.h, modules.h).
#include "runtime/modules.h"
#include "runtime/runtime.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <cstdio>
#include <string>

namespace rt {

const GameModule* g_game = nullptr;

namespace {
// stand-ins until the module is loaded (a flag set from the command line is carried over)
volatile u32 s_irqPending = 0;
bool s_traceOn = false;
u32 s_relBase[256][32];
}  // namespace
volatile u32* g_irqPendingPtr = &s_irqPending;
bool* g_traceOnPtr = &s_traceOn;
u32 (*g_relBasePtr)[32] = s_relBase;

// The module next to the program; false (after telling the player, unless `quiet`) when it is
// missing or was built for another version of the runtime.
bool loadGameModule(bool quiet) {
    std::string error;
    auto fail = [&] {
        std::fprintf(stderr, "%s\n", error.c_str());
        if (!quiet) MessageBoxA(nullptr, error.c_str(), "Sonic Adventure 2 Battle: Recompiled", MB_OK | MB_ICONINFORMATION);
        return false;
    };
    wchar_t exe[MAX_PATH];
    const DWORD n = GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring path(exe, n);
    path = path.substr(0, path.find_last_of(L"\\/") + 1) + L"sa2b_game.dll";
    HMODULE m = LoadLibraryW(path.c_str());
    if (!m) {
        error = "The game has not been set up on this computer yet. Start SA2BLauncher.exe: on its first run it builds the game "
                "from your disc.";
        return fail();
    }
    using Fn = const GameModule* (*)();
    const auto get = reinterpret_cast<Fn>(reinterpret_cast<void*>(GetProcAddress(m, "sa2b_game_module")));
    const GameModule* g = get ? get() : nullptr;
    if (!g || g->abi != kGameAbi) {
        error = "The game was set up by another version of this program. Start SA2BLauncher.exe to set it up again.";
        return fail();
    }
    *g->irqPending = s_irqPending;
    *g->traceOn = s_traceOn;
    g_irqPendingPtr = g->irqPending;
    g_traceOnPtr = g->traceOn;
    g_relBasePtr = g->relBase;
    g_game = g;
    return true;
}

// Once guest memory exists: the module's copy of its base.
void gameModuleMemory() {
    if (g_game) *g_game->mem = g_mem;
}

}  // namespace rt
