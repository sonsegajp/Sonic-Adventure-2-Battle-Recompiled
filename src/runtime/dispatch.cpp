// Indirect calls: guest code address -> native function, in O(1). Two-level table over the
// physical address (cached/uncached mirrors resolve alike); pages are allocated on demand.
#include "runtime/modules.h"
#include "runtime/runtime.h"

#include <atomic>
#include <cstdlib>

namespace rt {

namespace {
constexpr u32 kPageBits = 12;
GuestFn* s_pages[kHostMemSize >> kPageBits];

GuestFn* slot(u32 addr, bool create) {
    const u32 p = phys(addr);
    GuestFn*& page = s_pages[p >> kPageBits];
    if (!page) {
        if (!create) return nullptr;
        page = new GuestFn[1u << (kPageBits - 2)]();
    }
    return &page[(p & ((1u << kPageBits) - 1)) >> 2];
}
}  // namespace

void dispatchRegister(u32 addr, GuestFn fn) { *slot(addr, true) = fn; }

void dispatchUnregister(u32 addr) {
    if (GuestFn* s = slot(addr, false)) *s = nullptr;
}

GuestFn dispatchLookup(u32 addr) {
    GuestFn* s = slot(addr, false);
    return s ? *s : nullptr;
}

void registerDol() {
    for (u32 i = 0; i < kDolFuncCount; i++) dispatchRegister(kDolFuncs[i].addr, kDolFuncs[i].fn);
}

static const RelModuleCode* relCode(u32 id) {
    for (u32 i = 0; i < kRelModuleCount; i++)
        if (kRelModules[i].id == id) return &kRelModules[i];
    return nullptr;
}

void registerRelModule(u32 id) {
    const RelModuleCode* m = relCode(id);
    if (!m) fatal("OSLink: no recompiled code for REL module %u", id);
    for (u32 i = 0; i < m->count; i++) {
        const u32 a = m->funcs[i].addr;
        dispatchRegister(g_relBase[id][a >> 24] + (a & 0xFFFFFF), m->funcs[i].fn);
    }
    log("[rel] linked module %u (%s): %u functions, text at %08X\n", id, m->file, m->count, g_relBase[id][1]);
}

void unregisterRelModule(u32 id) {
    // Like the console, an unlinked module's code stays callable at its old addresses until
    // something else is linked there (the game calls into a module after OSUnlink, e.g. eventD's
    // callbacks); a module linked later at the same addresses simply overwrites these entries.
    const RelModuleCode* m = relCode(id);
    if (m) log("[rel] unlinked module %u (%s)\n", id, m->file);
    extern std::atomic<int> g_traceBurst;
    if (const char* n = std::getenv("SA2_TRACE_AFTER_UNLINK")) g_traceBurst = std::atoi(n);
}

void Ctx::call(u32 addr) {
    if (GuestFn fn = dispatchLookup(addr)) [[likely]] {
        fn(*this);
        return;
    }
    if (addr == 0) fatal("call to null (lr=%08X r3=%08X r12=%08X)", lr, r[3], r[12]);
    fatal("call to %08X: no function starts there (lr=%08X)", addr, lr);
}

}  // namespace rt
