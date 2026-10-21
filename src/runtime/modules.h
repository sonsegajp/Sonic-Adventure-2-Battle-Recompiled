#pragma once
// Tables the recompiler emits for the game module: main.dol's functions and, per REL, the
// functions at their section-relative addresses ((section << 24) | offset). OSLink registers a
// REL's functions at their real addresses once it knows where the game loaded the module.
#include "runtime/ctx.h"

namespace rt {

struct FuncEntry {
    u32 addr;
    GuestFn fn;
};

struct NameEntry {
    u32 addr;
    const char* name;
};

struct HookEntry {
    const char* name;
    u32 addr;
    GuestFn orig;  // the translated body
};

struct RelModuleCode {
    u32 id;
    const char* file;
    const FuncEntry* funcs;
    u32 count;
};

extern const FuncEntry kDolFuncs[];
extern const u32 kDolFuncCount;
extern const NameEntry kDolNames[];
extern const u32 kDolNameCount;
extern const HookEntry kHookTable[];
extern const u32 kHookCount;
extern HleFn g_hooks[];
extern const RelModuleCode kRelModules[];
extern const u32 kRelModuleCount;

}  // namespace rt
