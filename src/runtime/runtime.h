#pragma once
// Runtime internals shared by the host side (memory, dispatch, HLE registry, logging).
#include "runtime/ctx.h"

#include <cstdarg>
#include <cstdint>
#include <string>
#include <vector>

namespace rt {

// ---- logging ----
void log(const char* fmt, ...);
void vlog(const char* fmt, va_list ap);

// ---- memory ----
bool memInit();
bool loadDol(const std::vector<u8>& dol, u32& entry);
inline u32 phys(u32 a) { return a & 0x01FFFFFF; }
// Bump allocator for host-owned guest objects (interrupt stack, HLE structures). They live past
// the end of MEM1, in the rest of the 32 MB guest window: no game code addresses that range, so
// all of MEM1 stays laid out as the IPL leaves it (the game hardcodes buffers up near its top).
constexpr u32 kHleRegion = 0x80000000 + kMem1Size;
constexpr u32 kHleRegionEnd = 0x80000000 + kHostMemSize;
u32 hleAlloc(u32 size, u32 align = 32);
void profileRegisterThread();  // src/runtime/profile.cpp (SA2_PROFILE)
inline const char* gstr(u32 a) { return reinterpret_cast<const char*>(hostp(a)); }

// ---- dispatch ----
void dispatchRegister(u32 addr, GuestFn fn);
void dispatchUnregister(u32 addr);
GuestFn dispatchLookup(u32 addr);
void registerDol();
// Registers / unregisters a REL module's functions once its section bases are known.
void registerRelModule(u32 id);
void unregisterRelModule(u32 id);

// ---- HLE / hooks ----
struct HleReg {
    HleReg(const char* name, HleFn fn);
};
#define HLE(name)                                                   \
    static void hle_##name(::rt::Ctx& c);                            \
    static ::rt::HleReg hleReg_##name(#name, &hle_##name);           \
    static void hle_##name([[maybe_unused]] ::rt::Ctx& c)
void resolveHooks();                         // fills g_hooks from the registry
void callOriginal(Ctx& c, const char* name); // runs the translated body of a hooked function
u32 guestSymbol(const char* name);           // address of a named main.dol function (0 if unknown)
const char* guestName(u32 addr);             // name of a main.dol function, or nullptr
// Calls a guest function with up to 8 integer arguments on `c`'s stack/registers; returns r3.
u32 guestCall(Ctx& c, u32 fn, std::initializer_list<u32> args = {});
inline u32 stackArg(const Ctx& c, int n) { return rd32(c.r[1] + 8 + 4 * n); }

// ---- formatting of guest printf-style calls (OSReport) ----
std::string guestFormat(Ctx& c, u32 fmtAddr, int firstGpr, int firstFpr);

// ---- device interrupts ----
// The OS keeps its handler table at 0x80003040 (__OSSetInterruptHandler writes entry `id`). Posts
// an interrupt that runs the game's handler for `id`, after `before` sets the device status bits
// the handler checks.
constexpr u32 kInterruptTable = 0x80003040;
void raiseInterrupt(u32 id, void (*before)() = nullptr);

// ---- time ----
u64 timebase();  // guest time base: 40.5 MHz (bus clock / 4) since boot
constexpr u64 kTimebaseHz = 40500000;

}  // namespace rt
