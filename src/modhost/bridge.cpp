// The bridge: what mod code does to the PC executable, which does not exist here, done to the
// GameCube game instead. With the correspondences of config/<game>/pcmap*.txt:
//   - variables: reads and writes of PC globals reach the GameCube's (byte-swapped by element);
//   - functions: calls of PC functions call the GameCube's, arguments moved between the PC
//     conventions (registers, stack) and the GameCube's (r3..., f1...);
//   - whole functions: a jump to mod code written at a PC function's start replaces it, and is
//     looked at whenever the game calls the function, so a mod restoring the original bytes to
//     call the original (FunctionHook) works as on PC;
//   - code: a patched PC instruction is compared with the original, and the difference applied
//     to the GameCube instruction(s) doing the same - a new immediate, displacement, memory
//     operand or branch condition rewrites the GameCube instruction; a branch or call redirected
//     to mod code becomes a hook that runs that x86 code there. Functions with rewritten
//     instructions run in the interpreter (src/runtime/interp.cpp).
// Patches in REL modules are applied while the module is linked.
#include "modhost/modhost.h"
#include "modhost/pcmap.h"
#include "modhost/threads.h"
#include "modhost/x86dec.h"

#include "runtime/interp.h"
#include "runtime/runtime.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <fstream>
#include <set>
#include <unordered_map>
#include <utility>

namespace mh {

using rt::Ctx;

namespace {

PcMap s_map;
// Debugging the map (SA2_BRIDGE_SKIP=data,entry,insn; SA2_BRIDGE_RANGE=<from>-<to>): only some of
// what mods change is translated, to find a wrong entry by halves.
bool bridgeSkips(const char* kind, u32 pc) {
    static const std::string skip = std::getenv("SA2_BRIDGE_SKIP") ? std::getenv("SA2_BRIDGE_SKIP") : "";
    static u32 lo = 0, hi = 0xFFFFFFFF;
    static bool init = [] {
        if (const char* r = std::getenv("SA2_BRIDGE_RANGE")) {
            char* e;
            lo = u32(std::strtoul(r, &e, 16));
            if (*e == '-') hi = u32(std::strtoul(e + 1, nullptr, 16));
        }
        return true;
    }();
    (void)init;
    return skip.find(kind) != std::string::npos || pc < lo || pc >= hi;
}
constexpr u32 kHookReturn = 0x02FFFFF0;  // the return address mod code replacing a function returns to

// ---- GameCube register state saved around mod code inserted mid-function ----
struct GcRegs {
    u32 r[32];
    double f[32], ps1[32];
    u8 cr[8];
    u32 lr, ctr, ca, so, ov, fpscr;
};
GcRegs saveGc(const Ctx& c) {
    GcRegs s;
    std::memcpy(s.r, c.r, sizeof s.r);
    std::memcpy(s.f, c.f, sizeof s.f);
    std::memcpy(s.ps1, c.ps1, sizeof s.ps1);
    std::memcpy(s.cr, c.cr, sizeof s.cr);
    s.lr = c.lr, s.ctr = c.ctr, s.ca = c.ca, s.so = c.so, s.ov = c.ov, s.fpscr = c.fpscr;
    return s;
}
void restoreGc(Ctx& c, const GcRegs& s) {
    std::memcpy(c.r, s.r, sizeof s.r);
    std::memcpy(c.f, s.f, sizeof s.f);
    std::memcpy(c.ps1, s.ps1, sizeof s.ps1);
    std::memcpy(c.cr, s.cr, sizeof s.cr);
    c.lr = s.lr, c.ctr = s.ctr, c.ca = s.ca, c.so = s.so, c.ov = s.ov, c.fpscr = s.fpscr;
}

u32 gcRegValue(const Ctx& c, int reg) { return reg < 32 ? c.r[reg] : rt::bits32(float(c.f[reg - 32])); }
void setGcReg(Ctx& c, int reg, u32 v) {
    if (reg < 32) c.r[reg] = v;
    else c.f[reg - 32] = c.ps1[reg - 32] = double(rt::flt(v));
}

// ---- arguments between the conventions ----
// A function the map does not describe: r3..r10 are its arguments, passed as stack arguments
// (cdecl: harmless when it takes fewer), its result in eax / r3.

// Integer arguments and results are passed through Host::toGc / fromGc: a pointer to a function
// of the mod becomes one the game can call, and back.
// The game calls x86 code: the x86 stack and registers from the GameCube registers.
void argsToX86(const Ctx& c, Cpu& x, const FuncMap* sig) {
    Host& h = host();
    if (!sig || !sig->known) {
        for (int r = 10; r >= 3; r--) x.push(h.fromGc(c.r[r]));
        return;
    }
    int nStack = 0;
    for (const auto& [gr, src] : sig->args) nStack = std::max(nStack, src >= 100 ? src - 100 + 1 : 0);
    std::vector<u32> stack(size_t(nStack), 0);
    for (const auto& [gr, src] : sig->args) {
        const u32 v = gr < 32 ? h.fromGc(c.r[gr]) : gcRegValue(c, gr);
        if (src >= 100) stack[size_t(src - 100)] = v;
        else x.r[src] = v;
    }
    for (int i = nStack; i-- > 0;) x.push(stack[size_t(i)]);
}
// x86 code calls the game: the GameCube registers from the x86 stack (above the return address).
void argsToGc(Cpu& x, Ctx& c, const FuncMap* sig) {
    Host& h = host();
    if (!sig || !sig->known) {
        for (int r = 3; r <= 10; r++) c.r[r] = h.toGc(x.arg(r - 3));
        return;
    }
    for (const auto& [gr, src] : sig->args) {
        const u32 v = src >= 100 ? x.arg(src - 100) : x.r[src];
        setGcReg(c, gr, gr < 32 ? h.toGc(v) : v);
    }
}
int retKind(const FuncMap* sig) { return !sig || !sig->known ? 1 : sig->ret; }

// ---- running mod code from the game ----

struct Frame {
    bool call;      // entered as a call: ends when it returns to retPc; else a jump into PC code
    u32 retPc = 0;
    bool done = false;
    u32 exitPc = 0;
};
std::vector<Frame> s_frames;
std::vector<Ctx*> s_ctx;     // the GameCube context the mod code runs for (innermost last)
std::vector<u32> s_sitePc;   // the GameCube instruction it runs at

// Runs the x86 code at cpu.eip until it goes back into PC code (the frame's end); returns where.
// A fault in mod code (an instruction the interpreter does not know, abort()...) ends the frame as
// if the code had returned; it is reported once per place.
u32 runFrame(Ctx& c, u32 sitePc, Frame fr) {
    Host& h = host();
    s_frames.push_back(fr);
    s_ctx.push_back(&c);
    s_sitePc.push_back(sitePc);
    try {
        u64 n = 0;
        while (!s_frames.back().done) {
            h.cpu.step();
            if (!(++n & 0xFFFFF) && n >= 0x10000000) throw ModFault("runs forever");
        }
    } catch (const ModFault& e) {
        static std::set<u32> s_reported;
        if (s_reported.insert(sitePc).second)
            rt::log("[modhost] %s: mod code run at %08X stopped: %s (%s)\n", h.currentMod.c_str(), sitePc, e.what(),
                    h.cpu.where().c_str());
        s_frames.back().done = true;
        s_frames.back().exitPc = s_frames.back().call ? s_frames.back().retPc : 0;
    }
    const u32 exitPc = s_frames.back().exitPc;
    s_frames.pop_back();
    s_ctx.pop_back();
    s_sitePc.pop_back();
    return exitPc;
}

// Mod code called at GameCube instruction `pc` as the function `sig` describes (its arguments from
// the GameCube registers, its result back to them); x86 registers from GameCube ones by `regs`.
void runCall(Ctx& c, u32 pc, u32 entry, u32 retPc, const FuncMap* sig, const std::map<int, int>& regs) {
    Gil gil;
    Host& h = host();
    Cpu& x = h.cpu;
    const CpuRegs outer = x.save();
    for (const auto& [xr, gr] : regs) x.r[xr] = gcRegValue(c, gr);
    if (sig) argsToX86(c, x, sig);
    x.push(retPc);
    x.eip = entry;
    runFrame(c, pc, {true, retPc});
    const u32 eax = x.r[Cpu::EAX];
    const double st0 = x.top != outer.top ? x.ST(0) : 0.0;
    x.restore(outer);
    if (sig) {
        const int rk = retKind(sig);
        if (rk == 1) c.r[3] = h.toGc(eax);
        else if (rk == 2) c.f[1] = c.ps1[1] = st0;
    }
    h.commitPatches();
}

const GcSite* firstLoaded(const std::vector<GcSite>& sites, u32* at) {
    for (const GcSite& s : sites)
        if ((*at = s.at.resolve())) return &s;
    return nullptr;
}

// Where PC code at `pcAddr` continues in the GameCube code (0 if unknown).
u32 gcContinuation(u32 pcAddr) {
    const auto it = s_map.insns.find(pcAddr);
    if (it == s_map.insns.end()) return 0;
    u32 a;
    const GcSite* s = firstLoaded(it->second.sites, &a);
    if (!s) return 0;
    if (s->kind == GcSite::After) return a + 4;
    if (s->kind == GcSite::Taken) {
        const u32 w = rt::rd32(a);
        return (w >> 26) == 18 ? a + u32(s32(w << 6) >> 6 & ~3) : a + u32(s32(s16(u16(w & 0xFFFC))));
    }
    return a;
}

// Mod code jumped to from GameCube instruction `pc`; returns where the game continues: the
// GameCube place of the PC code the mod code jumps back to.
u32 runJump(Ctx& c, u32 pc, u32 entry, const std::map<int, int>& regs) {
    Gil gil;
    Host& h = host();
    Cpu& x = h.cpu;
    const CpuRegs outer = x.save();
    const GcRegs gc = saveGc(c);
    for (const auto& [xr, gr] : regs) x.r[xr] = gcRegValue(c, gr);
    const CpuRegs atEntry = x.save();
    x.eip = entry;
    const u32 exitPc = runFrame(c, pc, {false});
    // The game's registers as they were, plus what the mod code left in registers the code it
    // returns to reads.
    restoreGc(c, gc);
    if (const auto it = s_map.insns.find(exitPc); it != s_map.insns.end())
        for (const auto& [xr, gr] : it->second.regs) {
            // Only values that are the game's: given to the mod code from that same register, or
            // set by it (an x86 register the mod code never wrote holds nothing of the game's).
            const auto in = regs.find(xr);
            if ((in != regs.end() && in->second == gr) || x.r[xr] != atEntry.r[xr]) setGcReg(c, gr, x.r[xr]);
        }
    x.restore(outer);
    h.commitPatches();
    const u32 to = gcContinuation(exitPc);
    if (!to) {
        rt::log("[modhost] %s: mod code jumped to PC %08X, which has no GameCube place in pcmap.txt\n", h.currentMod.c_str(), exitPc);
        return pc + 4;
    }
    return to;
}

// ---- whole functions ----

// What the PC bytes at a function's start make of it now: a jump to elsewhere (its target), or 0
// (the function itself). Only mod writes count: unwritten bytes are the original.
u32 entryTarget(u32 pcFunc) {
    const Host& h = host();
    const auto b0 = h.pcOverlay.find(pcFunc);
    if (b0 == h.pcOverlay.end() || b0->second != 0xE9) return 0;
    u32 rel = 0;
    for (u32 i = 0; i < 4; i++) {
        const auto b = h.pcOverlay.find(pcFunc + 1 + i);
        if (b == h.pcOverlay.end()) return 0;
        rel |= u32(b->second) << (8 * i);
    }
    return pcFunc + 5 + rel;
}

std::set<u32> s_replaced;  // PC functions whose GameCube copies look at their entry bytes

// The game called a function a mod may have replaced (`f`), entered at `entry`.
void enterFunction(Ctx& c, const FuncMap& f, u32 entry) {
    const u32 t = entryTarget(f.pc);
    if (!t) return rt::interpOriginal(c, entry);
    static std::set<u32> s_seen;
    if (s_seen.insert(f.pc).second)
        rt::log("[modhost] %s replaces PC %08X (GameCube %08X, by %08X)%s\n", host().currentMod.c_str(), f.pc, entry, t,
                f.note.c_str());
    if (t >= kPcImageBase && t < kPcImageEnd) {  // redirected to another game function
        const FuncMap* g = s_map.funcAt(t);
        const u32 ga = g ? g->gc() : 0;
        if (!ga) return rt::interpOriginal(c, entry);
        return c.call(ga);
    }
    runCall(c, entry, t, kHookReturn, &f, {});
}

void watchEntry(const FuncMap& f) {
    if (bridgeSkips("entry", f.pc)) return;
    host().undo[host().currentMod].watched.push_back(f.pc);
    if (!s_replaced.insert(f.pc).second) return;
    const FuncMap* fp = &f;
    int n = 0;
    for (const GcAddr& g : f.gcs)
        n += rt::interpReplace(g.module, g.module ? (g.section << 24 | g.addr) : g.addr,
                               [fp](Ctx& c, u32 entry) { enterFunction(c, *fp, entry); });
    if (!n) rt::log("[modhost] PC %08X: none of its GameCube copies is a function\n", f.pc);
}

// ---- functions of mods the game calls: trampolines ----
// A function of a mod the game is to call (a task's callback, a table's entry, an operand a mod
// changed) needs a GameCube address: one of these, which runs the x86 function with its arguments
// as the PC function it stands in for takes them (cdecl r3.. when that is not known).
const FuncMap kUndescribed = [] {
    FuncMap f;
    f.known = false;
    return f;
}();
constexpr u32 kMaxTramps = 2048;
struct Tramp {
    u32 entry;
    const FuncMap* sig;
};
Tramp s_tramps[kMaxTramps];
u32 s_nTramps = 0, s_trampBase = 0;  // trampoline i is at s_trampBase + 4 * i
std::map<u32, u32> s_trampOf;        // x86 entry -> its trampoline

template <u32 N>
void trampFn(Ctx& c) {
    const Tramp& t = s_tramps[N];
    runCall(c, s_trampBase + 4 * N, t.entry, kHookReturn, t.sig, {});
}
template <u32... I>
constexpr std::array<rt::GuestFn, sizeof...(I)> trampFns(std::integer_sequence<u32, I...>) {
    return {&trampFn<I>...};
}
constexpr auto kTrampFns = trampFns(std::make_integer_sequence<u32, kMaxTramps>{});

u32 trampolineFor(u32 entry, const FuncMap* sig) {
    if (const auto it = s_trampOf.find(entry); it != s_trampOf.end()) return it->second;
    if (s_nTramps == kMaxTramps) {
        static bool s_warned = false;
        if (!s_warned) rt::log("[modhost] out of trampolines (%u): mod functions given to the game\n", kMaxTramps), s_warned = true;
        return 0;
    }
    if (!s_trampBase) {
        s_trampBase = rt::hleAlloc(4 * kMaxTramps, 32);
        for (u32 i = 0; i < kMaxTramps; i++) rt::wr32(s_trampBase + 4 * i, 0x4E800020);  // blr, for a reader
    }
    const u32 n = s_nTramps++;
    s_tramps[n] = {entry, sig ? sig : &kUndescribed};
    const u32 a = s_trampBase + 4 * n;
    rt::dispatchRegister(a, kTrampFns[n]);
    s_trampOf[entry] = a;
    return a;
}
const Tramp* trampAt(u32 a) {
    if (!s_trampBase || a < s_trampBase || a >= s_trampBase + 4 * s_nTramps || (a & 3)) return nullptr;
    return &s_tramps[(a - s_trampBase) / 4];
}

// GameCube function (as linked now) -> its PC function; rebuilt when modules link.
std::unordered_map<u32, const FuncMap*> s_gcFuncs;
bool s_gcFuncsValid = false;
const FuncMap* funcAtGc(u32 a) {
    if (!s_gcFuncsValid) {
        s_gcFuncs.clear();
        for (const auto& [pc, f] : s_map.funcs)
            for (const GcAddr& g : f.gcs)
                if (const u32 r = g.resolve()) s_gcFuncs.emplace(r, &f);
        s_gcFuncsValid = true;
    }
    const auto it = s_gcFuncs.find(a);
    return it == s_gcFuncs.end() ? nullptr : it->second;
}
// The GameCube copy mod code last read a pointer to (a function in several modules): written back,
// it is that copy again.
std::unordered_map<u32, u32> s_readBack;  // PC function -> GameCube address
u32 gcOf(const FuncMap& f) {
    if (const auto it = s_readBack.find(f.pc); it != s_readBack.end() && funcAtGc(it->second) == &f) return it->second;
    return f.gc();
}

// x86 code calls the game's function at GameCube `g` (`sig`: its arguments; `what` for logs).
void callGame(Cpu& x, u32 g, const FuncMap* sig, u32 what) {
    Host& h = host();
    if (s_ctx.empty() || !g) {
        static std::set<u32> s_logged;
        if (s_logged.insert(what).second)
            rt::log("[modhost] %s called game function %08X %s; returned 0\n", h.currentMod.c_str(), what,
                    s_ctx.empty() ? "outside of the game's code (at load)" : "whose module is not loaded");
        x.r[Cpu::EAX] = 0;
        x.ret(sig->pop);
        return;
    }
    Ctx& c = *s_ctx.back();
    argsToGc(x, c, sig);
    const CpuRegs saved = x.save();
    c.lr = s_sitePc.back() + 4;
    c.call(g);
    x.restore(saved);
    const int rk = retKind(sig);
    if (rk == 1) x.r[Cpu::EAX] = h.fromGc(c.r[3]);
    else if (rk == 2) x.fpush(c.f[1]);
    x.ret(sig->pop);
}

// ---- translation of patched instructions ----

struct Applied {
    std::vector<u8> bytes;  // the PC bytes translated
    std::vector<std::pair<u32, u32>> sites;  // GameCube words patched (address, module)
};
std::map<u32, Applied> s_applied;  // by PC instruction address

u32 branchTarget(u32 at, u32 w) {
    if ((w >> 26) == 18) return (w & 2) ? u32(s32(w << 6) >> 6 & ~3) : at + u32(s32(w << 6) >> 6 & ~3);
    const u32 bd = u32(s32(s16(u16(w & 0xFFFC))));
    return (w & 2) ? bd : at + bd;
}

// Outcomes of a compare: 1 less, 2 greater, 4 equal, 8 unordered.
enum : int { LT = 1, GT = 2, EQ = 4, UN = 8 };

// The outcomes for which a PC conditional jump is taken (-1 if not a compare this knows).
// `sign`: 1 signed, 2 unsigned, 0 either.
int x86Taken(int cc, int fpuMask, int& sign) {
    sign = 0;
    if (fpuMask >= 0) {  // fnstsw ax; test ah, mask; j(n)e / j(n)p
        int set = 0;
        const int ah[4] = {0x01, 0x00, 0x40, 0x45};  // less, greater, equal, unordered
        for (int o = 0; o < 4; o++) {
            const int r = ah[o] & fpuMask;
            const bool zf = r == 0;
            int bits = 0;
            for (int k = 0; k < 8; k++) bits += (r >> k) & 1;
            const bool pf = !(bits & 1);
            bool t;
            switch (cc) {
            case 4: t = zf; break;
            case 5: t = !zf; break;
            case 10: t = pf; break;
            case 11: t = !pf; break;
            default: return -1;
            }
            if (t) set |= 1 << o;
        }
        return set;
    }
    switch (cc) {
    case 2: sign = 2; return LT;
    case 3: sign = 2; return GT | EQ;
    case 4: return EQ;
    case 5: return LT | GT;
    case 6: sign = 2; return LT | EQ;
    case 7: sign = 2; return GT;
    case 12: sign = 1; return LT;
    case 13: sign = 1; return GT | EQ;
    case 14: sign = 1; return LT | EQ;
    case 15: sign = 1; return GT;
    default: return -1;
    }
}

// The outcomes for which a GameCube bc is taken (-1 if it is not a plain CR-bit test).
int gcTaken(u32 w) {
    const u32 bo = (w >> 21) & 31, bi = (w >> 16) & 31;
    if ((bo & 0x14) != 0x04) return -1;  // always, or decrements CTR
    const int bit = 1 << (bi & 3);       // lt gt eq so/un
    return (bo & 8) ? bit : (15 & ~bit);
}
// A bc with the same target and CR field taken for `set` (-1 if one bit cannot express it).
// `intCompare`: unordered never happens, so sets are compared without it.
s64 gcBranchFor(u32 w, int set, bool intCompare) {
    const int care = intCompare ? 7 : 15;
    for (int bit = 0; bit < 4; bit++)
        for (int sense = 0; sense < 2; sense++) {
            const int s = sense ? (1 << bit) : (15 & ~(1 << bit));
            if ((s & care) != (set & care)) continue;
            const u32 bo = sense ? 12 : 4;
            const u32 bi = (((w >> 16) & 31) & ~3u) | u32(bit);
            return (w & ~0x03FF0000u) | (bo << 21) | (bi << 16);
        }
    return -1;
}

class Translator {
public:
    Translator(const InsnMap& m, const u8* cur, int curLen) : m_(m), cur_(cur), curLen_(curLen) {}
    void run();
    std::vector<std::pair<u32, u32>> sites;  // patched GameCube words (address, module)
    bool pending = false;                    // some GameCube places are in modules not linked now

private:
    const InsnMap& m_;
    const u8* cur_;
    int curLen_;
    X86Insn o_, p_;

    void fail(const char* why) {
        host().undo[host().currentMod].failed.insert(m_.pc);
        static int s_logged = 0;
        if (s_logged++ < 2000)
            rt::log("[modhost] %s: PC %08X:%s - can't translate: %s\n", host().currentMod.c_str(), m_.pc, m_.note.c_str(), why);
    }
    // The site's address now, or 0 (its module is not linked: done when it is).
    u32 at(const GcSite& s) {
        const u32 a = s.at.resolve();
        if (!a) pending = true;
        return a;
    }
    void patch(const GcSite& s, u32 a, u32 word) {
        rt::interpPatch(a, word);
        rt::interpOverrideAt(a);
        sites.push_back({a, s.at.module});
    }
    void hook(const GcSite& s, u32 a, rt::InterpHook fn) { patch(s, a, rt::interpHookWord(std::move(fn))); }

    void insertedCall(u32 entry);
    void redirectCall(u32 entry);
    void redirectJump(u32 entry, bool conditional);
    void condition();
    void operands();
    void forceBranch(bool taken);
    void nopped();
    void functionPointer();
    void memoryMoved();
    const std::vector<GcSite>& immSites() const { return m_.immSites.empty() ? m_.sites : m_.immSites; }
};

// x86 instructions by what they do, for the NOP rules.
bool flagsOnly(const X86Insn& x) {  // cmp, test: only the flags change
    if (x.twoByte) return false;
    if (x.op >= 0x38 && x.op <= 0x3D) return true;
    if ((x.op == 0x80 || x.op == 0x81 || x.op == 0x83) && x.reg == 7) return true;
    if (x.op == 0x84 || x.op == 0x85 || x.op == 0xA8 || x.op == 0xA9) return true;
    return (x.op == 0xF6 || x.op == 0xF7) && x.reg == 0;
}
bool storesOnly(const X86Insn& x) {  // a plain store to memory (mov, fst/fstp, movss/movsd)
    if (!x.mem) return false;
    if (x.twoByte) return x.op == 0x11;
    if (x.op == 0x88 || x.op == 0x89 || x.op == 0xC6 || x.op == 0xC7) return true;
    return (x.op == 0xD9 || x.op == 0xDD || x.op == 0xDB) && (x.reg == 2 || x.reg == 3);
}

// A GameCube D-form load or store done on other memory: x86 memory (`x86`) or the game's at `ea`.
// Returns false for an instruction that is not a plain one.
bool accessAt(Ctx& c, u32 w, u32 ea, bool x86) {
    const u32 op = w >> 26, d = (w >> 21) & 31;
    Memory& m = host().mem;
    auto r32 = [&](u32 a) { return x86 ? m.r32(a) : rt::rd32(a); };
    auto r16 = [&](u32 a) { return x86 ? u32(m.r16(a)) : u32(rt::rd16(a)); };
    auto r8 = [&](u32 a) { return x86 ? u32(m.r8(a)) : u32(rt::rd8(a)); };
    auto w32 = [&](u32 a, u32 v) { x86 ? m.w32(a, v) : rt::wr32(a, v); };
    switch (op) {
    case 32: c.r[d] = r32(ea); return true;
    case 34: c.r[d] = r8(ea); return true;
    case 40: c.r[d] = r16(ea); return true;
    case 42: c.r[d] = u32(s32(s16(u16(r16(ea))))); return true;
    case 48: c.f[d] = c.ps1[d] = double(rt::flt(r32(ea))); return true;
    case 50: {
        const u64 v = x86 ? m.r64(ea) : (u64(rt::rd32(ea)) << 32 | rt::rd32(ea + 4));
        c.f[d] = rt::dbl(v);
        return true;
    }
    case 36: w32(ea, c.r[d]); return true;
    case 38: x86 ? m.w8(ea, u8(c.r[d])) : rt::wr8(ea, u8(c.r[d])); return true;
    case 44: x86 ? m.w16(ea, u16(c.r[d])) : rt::wr16(ea, u16(c.r[d])); return true;
    case 52: w32(ea, rt::bits32(float(c.f[d]))); return true;
    case 54: {
        u64 v;
        const double f = c.f[d];
        std::memcpy(&v, &f, 8);
        if (x86) m.w64(ea, v);
        else rt::wr32(ea, u32(v >> 32)), rt::wr32(ea + 4, u32(v));
        return true;
    }
    default: return false;
    }
}

void Translator::run() {
    if (!x86Decode(m_.bytes.data(), int(m_.bytes.size()), m_.pc, o_)) return fail("the original instruction does not decode");
    if (!x86Decode(cur_, curLen_, m_.pc, p_)) return fail("the patched instruction does not decode");
    const bool modCode = p_.target < kPcImageBase || p_.target >= kPcImageEnd;
    if (p_.flow == X86Insn::Call && !p_.indirect && o_.flow != X86Insn::Call) {
        // A call put in front of the instruction, which the mod moved after it.
        if (curLen_ < p_.len + o_.len || std::memcmp(cur_ + p_.len, m_.bytes.data(), size_t(o_.len)))
            return fail("a call replaces the instruction (the original is not after it)");
        if (!modCode) return fail("a call of game code was inserted");
        return insertedCall(p_.target);
    }
    if (p_.flow == X86Insn::Call && o_.flow == X86Insn::Call && !p_.indirect && !o_.indirect) {
        if (p_.target == o_.target) return;
        return redirectCall(p_.target);
    }
    if (p_.flow == X86Insn::Jmp && !p_.indirect && (o_.flow == X86Insn::Jmp || o_.flow == X86Insn::None)) {
        if (o_.flow == X86Insn::Jmp && p_.target == o_.target) return;
        if (!modCode) return fail("a jump was redirected to other game code");
        return redirectJump(p_.target, false);
    }
    bool allNops = true;
    for (int j = 0; j < o_.len; j++) allNops = allNops && cur_[j] == 0x90;
    if (o_.flow == X86Insn::Jcc && (allNops || (p_.flow == X86Insn::Jmp && !p_.indirect && p_.target == o_.target))) {
        return forceBranch(!allNops);
    }
    if (allNops) return nopped();
    if (p_.flow == X86Insn::Jcc && o_.flow == X86Insn::Jcc) {
        if (p_.target != o_.target) {
            if (p_.cc != o_.cc) return fail("both the condition and the target of a jump changed");
            if (!modCode) return fail("a jump was redirected to other game code");
            return redirectJump(p_.target, true);
        }
        if (p_.cc != o_.cc) return condition();
        return;
    }
    if (p_.len == o_.len && p_.sameOperation(o_)) return operands();
    fail("the instruction became a different one");
}

void Translator::insertedCall(u32 entry) {
    const u32 retPc = m_.pc + u32(p_.len);
    const std::map<int, int> regs = m_.regs;
    for (const GcSite& s : m_.sites) {
        const u32 a = at(s);
        if (!a) continue;
        const u32 orig = rt::rd32(a);
        auto call = [=](Ctx& c, u32 pc) {
            const GcRegs gc = saveGc(c);
            runCall(c, pc, entry, retPc, nullptr, regs);
            restoreGc(c, gc);
        };
        switch (s.kind) {
        case GcSite::Insn:
            hook(s, a, [=](Ctx& c, u32 pc) { return call(c, pc), rt::kInterpOriginal; });
            break;
        case GcSite::After:
            hook(s, a, [=](Ctx& c, u32 pc) {
                const u32 next = rt::interpStep(c, pc, orig);
                call(c, pc);
                return next;
            });
            break;
        case GcSite::Taken: {
            const u32 target = branchTarget(a, orig);
            hook(s, a, [=](Ctx& c, u32 pc) {  // when taken: the mod code runs on the way
                const u32 next = rt::interpStep(c, pc, orig);
                if (next == target) call(c, pc);
                return next;
            });
            break;
        }
        }
    }
}

void Translator::redirectCall(u32 entry) {
    // The callee's description (arguments, result): the original callee's, the same call site.
    const FuncMap* sig = s_map.funcAt(o_.target);
    const bool toGame = entry >= kPcImageBase && entry < kPcImageEnd;
    const FuncMap* target = toGame ? s_map.funcAt(entry) : nullptr;
    if (toGame && !target) return fail("a call was redirected to a game function the map does not describe");
    const u32 retPc = m_.pc + u32(o_.len);
    const std::map<int, int> regs = m_.regs;
    for (const GcSite& s : m_.sites) {
        const u32 a = at(s);
        if (!a) continue;
        const u32 w = rt::rd32(a);
        if (s.kind != GcSite::Insn || (w >> 26) != 18 || !(w & 1)) {
            fail("its GameCube place is not a bl");
            continue;
        }
        if (toGame) {  // another game function: the same arguments, its GameCube code
            hook(s, a, [=](Ctx& c, u32 pc) {
                c.lr = pc + 4;
                if (const u32 g = target->gc()) c.call(g);
                return pc + 4;
            });
            continue;
        }
        hook(s, a, [=](Ctx& c, u32 pc) {
            c.lr = pc + 4;
            runCall(c, pc, entry, retPc, sig ? sig : &kUndescribed, regs);
            return pc + 4;
        });
    }
}

void Translator::redirectJump(u32 entry, bool conditional) {
    const std::map<int, int> regs = m_.regs;
    const bool inv = m_.inverted;
    for (const GcSite& s : m_.sites) {
        const u32 a = at(s);
        if (!a) continue;
        const u32 orig = rt::rd32(a);
        if (s.kind == GcSite::After) {
            hook(s, a, [=](Ctx& c, u32 pc) {
                const u32 next = rt::interpStep(c, pc, orig);
                return next == rt::kInterpReturn ? next : runJump(c, pc, entry, regs);
            });
            continue;
        }
        const u32 op = orig >> 26;
        if (s.kind != GcSite::Insn || (op != 16 && op != 18) || (orig & 1)) {
            fail("its GameCube place is not a branch");
            continue;
        }
        if (!conditional || op == 18) {
            hook(s, a, [=](Ctx& c, u32 pc) { return runJump(c, pc, entry, regs); });
            continue;
        }
        hook(s, a, [=](Ctx& c, u32 pc) {
            const u32 next = rt::interpStep(c, pc, orig);
            const bool gcTook = next != pc + 4;
            return gcTook != inv ? runJump(c, pc, entry, regs) : next;
        });
    }
}

void Translator::condition() {
    int so = 0, sp = 0;
    const int before = x86Taken(o_.cc, m_.fpuMask, so), after = x86Taken(p_.cc, m_.fpuMask, sp);
    if (before < 0 || after < 0) return fail("a jump condition this does not know");
    if (so && sp && so != sp) return fail("a signed compare became unsigned (or back)");
    const bool intCompare = m_.fpuMask < 0;
    const int care = intCompare ? 7 : 15;
    for (const GcSite& s : m_.sites) {
        const u32 a = at(s);
        if (!a) continue;
        const u32 w = rt::rd32(a);
        const int g = (w >> 26) == 16 && s.kind == GcSite::Insn ? gcTaken(w) : -1;
        if (g < 0) {
            fail("its GameCube place is not a conditional branch");
            continue;
        }
        const int want0 = m_.inverted ? (15 & ~before) : before;
        if ((g & care) != (want0 & care)) {
            rt::log("[modhost] PC %08X: GameCube branch %08X tests %X, pcmap.txt says %X - entry wrong?\n", m_.pc, a, g, want0);
            continue;
        }
        const int want = m_.inverted ? (15 & ~after) : after;
        const s64 nw = gcBranchFor(w, want, intCompare);
        if (nw < 0) {
            fail("the new condition needs more than one condition bit");
            continue;
        }
        patch(s, a, u32(nw));
    }
}

void Translator::operands() {
    const bool immChanged = p_.immSize && p_.imm != o_.imm;
    const bool memChanged = p_.mem != o_.mem || p_.base != o_.base || p_.index != o_.index || p_.scale != o_.scale || p_.disp != o_.disp;
    if (immChanged && memChanged) return fail("both its immediate and its memory operand changed");
    if (!immChanged && !memChanged) {
        if (p_.mod != o_.mod || p_.rm != o_.rm) return fail("a register operand changed");
        return;
    }
    if (immChanged && o_.immSize == 4 && s_map.funcAt(o_.imm)) return functionPointer();
    if (memChanged && o_.mem && p_.mem && o_.base < 0 && o_.index < 0 && p_.base < 0 && p_.index < 0) return memoryMoved();
    for (const GcSite& s : immChanged ? immSites() : m_.sites) {
        if (s.kind != GcSite::Insn) {
            fail("an operand change needs a GameCube instruction, not an edge");
            continue;
        }
        const u32 a = at(s);
        if (!a) continue;
        const u32 w = rt::rd32(a);
        const u32 op = w >> 26;
        if (immChanged) {
            const bool isSigned = op == 14 || op == 11 || op == 7 || op == 8 || op == 12 || op == 13;
            const bool isUnsigned = op == 10 || op == 24 || op == 26 || op == 28;
            if (!isSigned && !isUnsigned) {
                fail("its GameCube place has no immediate");
                continue;
            }
            const s32 had = isSigned ? s32(s16(u16(w))) : s32(w & 0xFFFF);
            if (had != o_.immSigned()) {
                rt::log("[modhost] PC %08X: GameCube %08X has immediate %d, the PC instruction %d - entry wrong?\n", m_.pc, a,
                        had, o_.immSigned());
                continue;
            }
            const s32 v = p_.immSigned();
            if (isSigned ? (v < -32768 || v > 32767) : (v < 0 || v > 0xFFFF)) {
                fail("the new immediate does not fit the GameCube instruction");
                continue;
            }
            patch(s, a, (w & 0xFFFF0000u) | (u32(v) & 0xFFFF));
            continue;
        }
        if (op < 32 || op > 55) {
            fail("its GameCube place is not a load or store");
            continue;
        }
        if (p_.index >= 0) {
            fail("the new memory operand has an index register");
            continue;
        }
        u32 ra = (w >> 16) & 31;
        if (p_.base != o_.base) {
            const auto r = m_.regs.find(p_.base);
            if (p_.base < 0 || r == m_.regs.end() || r->second >= 32) {
                fail("the new memory operand's base register has no GameCube register (regs= in pcmap.txt)");
                continue;
            }
            ra = u32(r->second);
        } else if (s32(s16(u16(w))) != o_.disp) {
            rt::log("[modhost] PC %08X: GameCube %08X has displacement %d, the PC instruction %d - entry wrong?\n", m_.pc, a,
                    s32(s16(u16(w))), o_.disp);
            continue;
        }
        if (p_.disp < -32768 || p_.disp > 32767) {
            fail("the new displacement does not fit the GameCube instruction");
            continue;
        }
        patch(s, a, (w & ~0x001FFFFFu) | (ra << 16) | (u32(p_.disp) & 0xFFFF));
    }
}

// A conditional jump made unconditional (`taken`) or never taken (nops).
void Translator::forceBranch(bool taken) {
    const bool gcTaken = taken != m_.inverted;
    for (const GcSite& s : m_.sites) {
        const u32 a = at(s);
        if (!a) continue;
        const u32 w = rt::rd32(a);
        if (s.kind != GcSite::Insn || (w >> 26) != 16 || (w & 1)) {
            fail("its GameCube place is not a conditional branch");
            continue;
        }
        patch(s, a, gcTaken ? (w & ~0x03E00000u) | (20u << 21) : 0x60000000u);  // bc always / nop
    }
}

// An instruction filled with nops: a compare (the jump after it is what matters), a store or a
// call goes at the GameCube instruction too.
void Translator::nopped() {
    if (flagsOnly(o_)) return;
    const bool call = o_.flow == X86Insn::Call, store = storesOnly(o_);
    if (!call && !store) return fail("an instruction other than a store, call or compare was removed");
    for (const GcSite& s : m_.sites) {
        const u32 a = at(s);
        if (!a) continue;
        const u32 w = rt::rd32(a), op = w >> 26;
        const bool ok = s.kind == GcSite::Insn &&
                        (call ? op == 18 && (w & 1) : op == 36 || op == 38 || op == 44 || op == 52 || op == 54);
        if (!ok) {
            fail(call ? "its GameCube place is not a bl" : "its GameCube place is not a plain store");
            continue;
        }
        patch(s, a, 0x60000000u);
    }
}

// A function's address in the code (a callback set, an argument) changed: the GameCube
// instruction forming it forms the new one's (a trampoline for mod code).
void Translator::functionPointer() {
    const FuncMap* was = s_map.funcAt(o_.imm);
    const u32 v = p_.imm;
    const FuncMap* game = v >= kPcImageBase && v < kPcImageEnd ? s_map.funcAt(v) : nullptr;
    if (!game && !host().isModCode(v)) return fail("a function's address became something else than a function");
    const u32 tramp = game ? 0 : trampolineFor(v, was);
    if (!game && !tramp) return fail("no trampoline left");
    for (const GcSite& s : immSites()) {
        const u32 a = at(s);
        if (!a) continue;
        const u32 w = rt::rd32(a), op = w >> 26;
        if (s.kind != GcSite::Insn || (op != 14 && op != 24)) {
            fail("its GameCube place does not form the address (addi / ori)");
            continue;
        }
        const u32 rd = op == 14 ? (w >> 21) & 31 : (w >> 16) & 31;
        hook(s, a, [=](Ctx& c, u32 pc) {
            c.r[rd] = game ? gcOf(*game) : tramp;
            return pc + 4;
        });
    }
}

// A variable's address in the code changed (the mod's own float, table...): the GameCube load or
// store reads or writes there - in mod memory, or the game variable of another PC address.
void Translator::memoryMoved() {
    const u32 from = u32(o_.disp), to = u32(p_.disp);
    const bool x86 = to < kPcImageBase || to >= kPcImageEnd;
    const DataMap* dTo = x86 ? nullptr : s_map.dataAt(to);
    if (!x86 && !dTo) return fail("a variable's address became one of a PC variable the map does not describe");
    const DataMap* dFrom = s_map.dataAt(from);
    for (const GcSite& s : m_.sites) {
        const u32 a = at(s);
        if (!a) continue;
        const u32 w = rt::rd32(a), op = w >> 26;
        if (s.kind != GcSite::Insn || !(op == 32 || op == 34 || op == 36 || op == 38 || op == 40 || op == 42 || op == 44 ||
                                        op == 48 || op == 50 || op == 52 || op == 54)) {
            fail("its GameCube place is not a plain load or store");
            continue;
        }
        hook(s, a, [=](Ctx& c, u32 pc) {
            const u32 ra = (w >> 16) & 31;
            const u32 ea = (ra ? c.r[ra] : 0) + u32(s32(s16(u16(w))));
            // where in the variable: from the GameCube address of the original, when the map has it
            u32 off = 0;
            if (dFrom)
                if (const u32 base = dFrom->gc.resolve()) off = ea - (base + (from - dFrom->pc));
            Gil gil;
            if (x86) accessAt(c, w, to + off, true);
            else if (const u32 base = dTo->gc.resolve()) accessAt(c, w, base + (to - dTo->pc) + off, false);
            return pc + 4;
        });
    }
}

// In a function a mod replaced (a jump to its code at the start): code that no longer runs (the rest
// of the function filled with nops, typically).
bool deadCode(u32 a) {
    const FuncMap* f = s_map.funcCovering(a);
    if (!f || a < f->pc + 5) return false;
    const u32 t = entryTarget(f->pc);
    return t && (t < kPcImageBase || t >= kPcImageEnd);
}

void translate(const InsnMap& m) {
    Host& h = host();
    h.undo[h.currentMod].failed.erase(m.pc);
    const int olen = int(m.bytes.size());
    u8 cur[24];
    bool over[24];
    for (int j = 0; j < 24; j++) {
        const u32 a = m.pc + u32(j);
        const auto it = h.pcOverlay.find(a);
        over[j] = it != h.pcOverlay.end();
        if (over[j]) cur[j] = it->second;
        else if (j < olen) cur[j] = m.bytes[size_t(j)];
        else cur[j] = u8(h.pcImageByte(a) < 0 ? 0xCC : h.pcImageByte(a));
    }
    // The instruction as it is now spans its original bytes, or more when a longer one replaced it.
    X86Insn now;
    const int span = std::max(olen, x86Decode(cur, 24, m.pc, now) ? now.len : 0);
    bool patched = false;
    for (int j = 0; j < span; j++) patched = patched || (over[j] && (j >= olen || cur[j] != m.bytes[size_t(j)]));
    std::vector<u8> key(cur, cur + 24);  // with the bytes after it (a call inserted before it)
    for (int j = 0; j < 24; j++) key.push_back(over[j]);
    Applied& prev = s_applied[m.pc];
    if (key == prev.bytes) return;
    for (const auto& [a, mod] : prev.sites) rt::interpUnpatch(a);
    prev = Applied{key, {}};
    if (!patched || bridgeSkips("insn", m.pc)) return;  // back to the original
    Translator t(m, cur, 24);
    t.run();
    prev.sites = t.sites;
    if (t.pending) prev.bytes.clear();  // not complete: again when a module links
    if (!t.sites.empty()) {
        static int s_logged = 0;
        if (s_logged++ < 100)
            rt::log("[modhost] %s: PC %08X%s -> %zu GameCube instruction(s) from %08X\n", h.currentMod.c_str(), m.pc,
                    m.note.c_str(), t.sites.size(), t.sites[0].first);
    }
}

// A REL module came or went: its patched instructions are applied again or taken away.
void onRelLink(u32 module, bool linked) {
    Gil gil;
    s_gcFuncsValid = false;
    if (!linked) {
        for (auto& [pc, ap] : s_applied) {
            bool had = false;
            for (const auto& [a, mod] : ap.sites)
                if (mod == module) rt::interpUnpatch(a), had = true;
            if (had) {
                ap.sites.erase(std::remove_if(ap.sites.begin(), ap.sites.end(), [&](auto& s) { return s.second == module; }),
                               ap.sites.end());
                ap.bytes.clear();
            }
        }
        return;
    }
    for (auto& [pc, ap] : s_applied) {
        if (!ap.bytes.empty()) continue;
        const auto it = s_map.insns.find(pc);
        if (it != s_map.insns.end()) translate(it->second);
    }
    // The module's variables, as mods left them.
    const Host& h = host();
    for (auto it = h.relData.lower_bound(u64(module) << 40); it != h.relData.end() && (it->first >> 40) == module; ++it) {
        const u32 sec = u32(it->first >> 32) & 0xFF, off = u32(it->first);
        if (const u32 base = rt::g_relBase[module][sec]) rt::wr8(base + off, it->second);
    }
}

}  // namespace

// ---- Host: the PC image window ----

bool Host::loadPcMap(const std::filesystem::path& file) {
    rt::g_relLinkHook = onRelLink;
    return s_map.load(file);
}

bool Host::loadPcImage(const std::filesystem::path& exe) {
    std::ifstream f(exe, std::ios::binary);
    std::vector<u8> d((std::istreambuf_iterator<char>(f)), {});
    auto le16 = [&](size_t o) { return u32(d[o] | d[o + 1] << 8); };
    auto le32 = [&](size_t o) { return u32(d[o] | d[o + 1] << 8 | d[o + 2] << 16 | u32(d[o + 3]) << 24); };
    if (d.size() < 0x400 || d[0] != 'M' || d[1] != 'Z') return false;
    const size_t pe = le32(0x3C), opt = pe + 0x18;
    if (le32(pe) != 0x4550 || le32(opt + 0x1C) != kPcImageBase) return false;
    pcImage_.assign(std::min<u32>(le32(opt + 0x38), kPcImageEnd - kPcImageBase), 0);
    const u32 nSections = le16(pe + 6);
    const size_t sec = opt + le16(pe + 0x14);
    for (u32 i = 0; i < nSections; i++) {
        const size_t s = sec + 40 * i;
        const u32 va = le32(s + 12), rawSize = le32(s + 16), rawPtr = le32(s + 20);
        if (va >= pcImage_.size()) continue;
        const u32 n = std::min<u32>(rawSize, u32(pcImage_.size()) - va);
        if (rawPtr + n <= d.size()) std::memcpy(pcImage_.data() + va, d.data() + rawPtr, n);
    }
    rt::log("[modhost] PC executable image loaded from %s\n", exe.string().c_str());
    return true;
}

int Host::pcImageByte(u32 a) const { return a - kPcImageBase < pcImage_.size() ? pcImage_[a - kPcImageBase] : -1; }

void Host::noteRef(char kind, u32 pcAddr, int size) {
    static std::set<std::pair<u32, char>> s_seen;
    static std::FILE* s_out = nullptr;
    if (!s_seen.insert({pcAddr, kind}).second) return;
    if (!s_out) s_out = std::fopen("build/modhost_pcrefs.tsv", "w");
    if (!s_out) return;
    std::fprintf(s_out, "%c\t%08X\t%d\t%s\t%08X\n", kind, pcAddr, size, currentMod.c_str(), cur().insn);
    std::fflush(s_out);
}

// A PC-only variable backed by mod-side memory (virtualVariable): its x86 address, else 0.
static u32 virtualAt(Host& h, u32 a) {
    auto it = h.virtualVars.upper_bound(a);
    if (it == h.virtualVars.begin()) return 0;
    --it;
    return a < it->first + it->second.first ? it->second.second + (a - it->first) : 0;
}

u64 Host::read(u32 addr, int size) {
    if (const u32 v = virtualAt(*this, addr)) {
        u64 r = 0;
        for (int i = 0; i < size; i++) r |= u64(mem.r8(v + u32(i))) << (8 * i);
        return r;
    }
    noteRef('r', addr, size);
    const DataMap* pd = size == 4 ? s_map.dataAt(addr) : nullptr;
    const bool ptr = pd && pd->ptr && pd->elem == 4 && (addr - pd->pc) % 4 == 0;
    u64 v = 0;
    for (int i = 0; i < size; i++) {
        const u32 a = addr + u32(i);
        u8 b = 0;
        if (const DataMap* d = s_map.dataAt(a)) {
            const u32 off = a - d->pc, start = off / d->elem * d->elem;
            const u32 at = start + (d->elem - 1 - (off - start));
            if (const u32 base = d->gc.resolve()) b = u8(rt::rd8(base + at));
            else if (const auto it = relData.find(u64(d->gc.module) << 40 | u64(d->gc.section) << 32 | (d->gc.addr + at));
                     it != relData.end())
                b = it->second;  // the module is not loaded: what a mod wrote there
        } else if (const auto it = pcOverlay.find(a); it != pcOverlay.end()) {
            b = it->second;
        } else if (const InsnMap* m = s_map.insnCovering(a)) {
            b = m->bytes[a - m->pc];
        } else if (a - kPcImageBase < pcImage_.size()) {
            b = pcImage_[a - kPcImageBase];
        } else if (!s_map.funcAt(a & ~0xFu) && !s_map.funcAt(a)) {
            static std::set<u32> s_warned;
            if (s_warned.size() < 64 && s_warned.insert(a & ~0xFu).second)
                rt::log("[modhost] %s read PC address %08X, which has no translation yet\n", currentMod.c_str(), a);
        }
        v |= u64(b) << (8 * i);
    }
    return ptr ? fromGc(u32(v)) : v;
}

void Host::write(u32 addr, u64 v, int size) {
    if (const u32 x = virtualAt(*this, addr)) {
        for (int i = 0; i < size; i++) mem.w8(x + u32(i), u8(v >> (8 * i)));
        return;
    }
    noteRef('w', addr, size);
    if (size == 4)  // a pointer into a table of them: one the game can use
        if (const DataMap* d = s_map.dataAt(addr); d && d->ptr && d->elem == 4 && (addr - d->pc) % 4 == 0) v = toGc(u32(v));
    for (int i = 0; i < size; i++) {
        const u32 a = addr + u32(i);
        const u8 b = u8(v >> (8 * i));
        Undo& u = undo[currentMod];
        if (const DataMap* d = s_map.dataAt(a)) {  // a variable of the game
            const u32 off = a - d->pc, start = off / d->elem * d->elem;
            const u32 at = start + (d->elem - 1 - (off - start));
            if (bridgeSkips("data", a)) continue;
            if (d->gc.module) {
                const u64 key = u64(d->gc.module) << 40 | u64(d->gc.section) << 32 | (d->gc.addr + at);
                relData[key] = b;
                u.relWrites.push_back(key);
            }
            if (const u32 base = d->gc.resolve()) {
                u.game.emplace(base + at, u8(rt::rd8(base + at)));
                rt::wr8(base + at, b);
            }
            continue;
        }
        if (!u.overlay.count(a)) {
            const auto it = pcOverlay.find(a);
            u.overlay[a] = it == pcOverlay.end() ? -1 : it->second;
        }
        pcOverlay[a] = b;
        dirty.push_back(a);
        if (!patches.empty() && patches.back().mod == currentMod && patches.back().addr + patches.back().bytes.size() == a)
            patches.back().bytes.push_back(b);
        else
            patches.push_back({a, {b}, currentMod});
    }
}

void Host::commitPatches() {
    if (dirty.empty()) return;
    std::set<u32> entries;
    std::set<u32> unknown;
    for (u32 a : dirty) {
        bool known = s_map.isPadding(a);
        // A function's first bytes: whether a mod replaces it is looked at on every call.
        for (u32 f = a >= 4 ? a - 4 : 0; f <= a; f++)
            if (const FuncMap* fm = s_map.funcAt(f)) watchEntry(*fm), known = true;
        // Entries whose instruction holds a, or whose start was patched with a longer instruction.
        for (auto it = s_map.insns.lower_bound(a >= 15 ? a - 15 : 0); it != s_map.insns.end() && it->first <= a; ++it)
            if (a < it->first + it->second.bytes.size() || pcOverlay.count(it->first)) entries.insert(it->first), known = true;
        Undo& u = undo[currentMod];
        if (!known && !deadCode(a)) unknown.insert(a), u.unknown.insert(a);
        else u.unknown.erase(a);
    }
    dirty.clear();
    for (u32 pc : entries) translate(s_map.insns.at(pc));
    // What a later patch made dead code (the function replaced since) needs nothing any more.
    for (auto& [mod, u] : undo) {
        for (auto it = u.unknown.begin(); it != u.unknown.end();) it = deadCode(*it) ? u.unknown.erase(it) : std::next(it);
        for (auto it = u.failed.begin(); it != u.failed.end();) it = deadCode(*it) ? u.failed.erase(it) : std::next(it);
    }
    // Report untranslated patches by run of bytes.
    for (auto it = unknown.begin(); it != unknown.end();) {
        u32 first = *it, last = first;
        while (++it != unknown.end() && *it == last + 1) last = *it;
        static int s_logged = 0;
        if (s_logged++ < 2000)
            rt::log("[modhost] %s patched PC %08X-%08X: no GameCube place known (pcmap.txt)\n", currentMod.c_str(), first, last);
    }
}

void Host::rollback(const std::string& mod) {
    const auto it = undo.find(mod);
    if (it == undo.end()) return;
    for (const auto& [a, b] : it->second.overlay) {
        if (b < 0) pcOverlay.erase(a);
        else pcOverlay[a] = u8(b);
        dirty.push_back(a);
    }
    for (const auto& [g, b] : it->second.game) rt::wr8(g, b);
    for (u64 k : it->second.relWrites) relData.erase(k);
    // Functions whose entries no mod writes now run their own code again.
    for (u32 f : it->second.watched) {
        bool written = false;
        for (u32 i = 0; i < 5; i++) written = written || pcOverlay.count(f + i);
        const FuncMap* fm = s_map.funcAt(f);
        if (written || !fm || !s_replaced.erase(f)) continue;
        for (const GcAddr& g : fm->gcs) rt::interpUnreplace(g.module, g.module ? (g.section << 24 | g.addr) : g.addr);
    }
    const std::string was = currentMod;
    currentMod = mod;
    commitPatches();  // back to the originals: the translations go
    currentMod = was;
    undo.erase(it);
}

void Host::execPc(Cpu& x, u32 addr) {
    noteRef('x', addr, 0);
    if (const FuncMap* f = s_map.funcAt(addr)) {
        // A mod replaced it: its replacement, entered as the PC would.
        if (const u32 t = entryTarget(addr); t && (t < kPcImageBase || t >= kPcImageEnd)) {
            x.eip = t;
            return;
        }
        // The game function, in the GameCube game.
        return callGame(x, gcOf(*f), f, addr);
    }
    // Back into the game's code: the end of the mod code the game ran.
    if (!s_frames.empty()) {
        Frame& fr = s_frames.back();
        if (fr.call && addr != fr.retPc)
            rt::log("[modhost] %s: mod code called at a PC call returned to %08X instead of %08X\n", currentMod.c_str(), addr, fr.retPc);
        fr.done = true;
        fr.exitPc = addr;
        return;
    }
    static int s_warned = 0;
    if (s_warned++ < 32)
        rt::log("[modhost] %s called PC function %08X, which pcmap.txt does not describe (returned 0)\n", currentMod.c_str(), addr);
    x.r[Cpu::EAX] = 0;
    x.ret();
}

void Host::execGc(Cpu& x, u32 addr) {
    if (const Tramp* t = trampAt(addr)) {  // a function of a mod after all
        x.eip = t->entry;
        return;
    }
    const FuncMap* f = funcAtGc(addr);
    callGame(x, addr, f ? f : &kUndescribed, addr);
}

u32 Host::toGc(u32 v) {
    if (v >= kPcImageBase && v < kPcImageEnd) {
        if (const FuncMap* f = s_map.funcAt(v)) {
            const u32 g = gcOf(*f);
            return g ? g : v;
        }
        if (const DataMap* d = s_map.dataAt(v))
            if (const u32 base = d->gc.resolve()) return base + (v - d->pc);
        return v;
    }
    if (v >= 0x00010000 && (v < kGcBase || v >= kGcEnd) && isModCode(v)) {
        const u32 t = trampolineFor(v, nullptr);
        return t ? t : v;
    }
    return v;
}

u32 Host::fromGc(u32 v) {
    if (v < kGcBase || v >= kGcEnd) return v;
    if (const Tramp* t = trampAt(v)) return t->entry;
    if (const FuncMap* f = funcAtGc(v)) {  // a function of the game: the PC's, as mod code compares and calls it
        s_readBack[f->pc] = v;
        return f->pc;
    }
    return v;
}

}  // namespace mh
