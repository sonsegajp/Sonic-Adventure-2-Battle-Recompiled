// The registry of native stand-ins for the Windows and C runtime functions mod DLLs import
// (kernel32.cpp, crt.cpp), plus the Visual C++ runtime's (exceptions, type info). Each runs at a
// thunk address: arguments are on the x86 stack (cpu.arg), the result goes in eax, and it returns
// with the callee's stack cleanup (stdcall: the argument bytes; cdecl: none).
#include "modhost/sysapi.h"

#include "runtime/runtime.h"

#include <map>

namespace mh {

namespace {

[[noreturn]] void stdError(Cpu& c, const char* what) {
    throw ModFault(std::string("std::") + what + " (" + (c.arg(0) ? M().cstr(c.arg(0)) : std::string()) + ")");
}
void Xbad_alloc(Cpu& c) { stdError(c, "bad_alloc"); }
void Xout_of_range(Cpu& c) { stdError(c, "out_of_range"); }
void Xlength_error(Cpu& c) { stdError(c, "length_error"); }
void Xinvalid_argument(Cpu& c) { stdError(c, "invalid_argument"); }
void CxxThrowException(Cpu& c) { throw ModFault("a C++ exception (" + c.where() + ")"); }
void exceptionDispatch(Cpu&) { throw ModFault("C++ exception dispatch through the dynamic runtime"); }
void nop0(Cpu& c) { retc(c, 0); }
// struct __std_exception_data { const char* what; bool doFree; }
void std_exception_copy(Cpu& c) {
    const u32 from = c.arg(0), to = c.arg(1);
    const u32 what = M().r32(from);
    if (what && M().r8(from + 4)) {
        M().w32(to, host().allocString(M().cstr(what)));
        M().w8(to + 4, 1);
    } else {
        M().w32(to, what);
        M().w8(to + 4, 0);
    }
    retc(c, 0);
}
void std_exception_destroy(Cpu& c) {
    const u32 p = c.arg(0);
    if (M().r8(p + 4)) host().free(M().r32(p));
    M().w32(p, 0), M().w8(p + 4, 0);
    retc(c, 0);
}

const ImportEntry kVcRuntime[] = {
    {"?_Xbad_alloc@std@@YAXXZ", Xbad_alloc},
    {"?_Xout_of_range@std@@YAXPBD@Z", Xout_of_range},
    {"?_Xlength_error@std@@YAXPBD@Z", Xlength_error},
    {"?_Xinvalid_argument@std@@YAXPBD@Z", Xinvalid_argument},
    {"_CxxThrowException", CxxThrowException},
    {"__std_type_info_destroy_list", nop0},
    {"__std_exception_copy", std_exception_copy},
    {"__std_exception_destroy", std_exception_destroy},
    {"__CxxFrameHandler3", exceptionDispatch},
    {"__CxxFrameHandler4", exceptionDispatch},
    {"_except_handler4_common", exceptionDispatch},
};

const std::map<std::string, Native>& table() {
    static const std::map<std::string, Native> t = [] {
        std::map<std::string, Native> m;
        for (size_t i = 0; i < kKernel32Count; i++) m[kKernel32[i].name] = kKernel32[i].fn;
        for (size_t i = 0; i < kCrtCount; i++) m[kCrt[i].name] = kCrt[i].fn;
        for (const ImportEntry& e : kVcRuntime) m[e.name] = e.fn;
        return m;
    }();
    return t;
}

}  // namespace

Native win32Import(const std::string&, const std::string& name) {
    const auto it = table().find(name);
    return it == table().end() ? nullptr : it->second;
}

}  // namespace mh
