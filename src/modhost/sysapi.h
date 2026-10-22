// Shared by the Windows and C runtime stand-ins (kernel32.cpp, crt.cpp, win32.cpp).
#pragma once

#include "modhost/modhost.h"
#include "modhost/threads.h"

#include <string>

namespace mh {

struct ImportEntry {
    const char* name;
    Native fn;
};

inline Memory& M() { return host().mem; }
inline void retc(Cpu& c, u32 v) { c.r[Cpu::EAX] = v, c.ret(); }                        // cdecl
inline void rets(Cpu& c, u32 v, int nargs) { c.r[Cpu::EAX] = v, c.ret(4 * u32(nargs)); }  // stdcall
inline void retc64(Cpu& c, u64 v) { c.r[Cpu::EAX] = u32(v), c.r[Cpu::EDX] = u32(v >> 32), c.ret(); }
// A double argument (two stack dwords from argument slot n).
inline double argDouble(Cpu& c, int n) {
    const u64 b = u64(c.arg(n)) | u64(c.arg(n + 1)) << 32;
    double d;
    std::memcpy(&d, &b, 8);
    return d;
}
inline float argFloat(Cpu& c, int n) {
    const u32 b = c.arg(n);
    float f;
    std::memcpy(&f, &b, 4);
    return f;
}
inline u64 arg64(Cpu& c, int n) { return u64(c.arg(n)) | u64(c.arg(n + 1)) << 32; }
// x87 results (cdecl float/double returns).
inline void retFloat(Cpu& c, double v) { c.fpush(v), c.ret(); }

// ---- guest strings ----
std::string utf8(const std::u16string& s);
std::u16string utf16(const std::string& s);
inline std::string wideArg(u32 addr) { return addr ? utf8(M().wstr(addr)) : std::string(); }
inline std::string strArg(u32 addr) { return addr ? M().cstr(addr) : std::string(); }
// Writes a string with its terminator; returns the characters written (without it). `cap`:
// buffer size in characters, including the terminator.
u32 putStr(u32 addr, const std::string& s, u32 cap = 0xFFFFFFFF);
u32 putWide(u32 addr, const std::u16string& s, u32 cap = 0xFFFFFFFF);

// ---- paths: the PC game's view (Windows paths under its folder) <-> host files ----
// The game folder mods see (GetModuleFileName, the current directory) is a virtual C:\SA2B,
// which is the host's working directory.
constexpr const char* kGameDir = "C:\\SA2B";
std::string hostPath(const std::string& guestPath);
std::string guestPath(const std::string& hostPath);  // absolute, Windows style

// ---- errors ----
void setLastError(u32 e);
u32 lastError();
void setErrno(int e);

// ---- printf (crt.cpp): formats with guest arguments read from `va` onwards ----
std::string guestFormat(const std::string& fmt, u32 va, bool wide);

// Stand-in tables (kernel32.cpp, crt.cpp).
extern const ImportEntry kKernel32[];
extern const size_t kKernel32Count;
extern const ImportEntry kCrt[];
extern const size_t kCrtCount;

}  // namespace mh
