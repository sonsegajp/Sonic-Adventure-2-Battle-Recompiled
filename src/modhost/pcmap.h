// The PC executable <-> GameCube game correspondences the bridge translates with
// (config/<game>/pcmap.txt, written by hand, over pcmap_auto.txt, made by tools/pcmatch). Facts
// about the two builds of the game, not about any mod: every mod touching a listed place is
// translated by the same entry.
#pragma once

#include "modhost/x86.h"

#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace mh {

// A GameCube code address: in main.dol, or in a REL module (where it is only while the module is
// linked): "m<id>:<section>:<offset>".
struct GcAddr {
    u32 module = 0;  // 0: main.dol
    u32 section = 0;
    u32 addr = 0;    // main.dol: the address; REL: the offset in the section
    u32 resolve() const;  // the address now (0 if the module is not linked)
    bool parse(const std::string& s);
};

// Where a PC location is in the GameCube code: an instruction, the fall-through after an
// instruction ("X+"), or the taken edge of a branch ("X>") - the GameCube compiler merges and
// duplicates code, so one PC instruction can be several of these.
struct GcSite {
    enum Kind { Insn, After, Taken } kind = Insn;
    GcAddr at;
};

// x86 register numbers as in Cpu (EAX..EDI); GameCube registers as r0..r31, f0..f31 (+32).
struct InsnMap {
    u32 pc = 0;
    std::vector<u8> bytes;  // the PC instruction's original encoding
    std::vector<GcSite> sites;
    std::vector<GcSite> immSites;  // where its immediate is, when another instruction ("imm=")
    std::map<int, int> regs;  // x86 register -> GameCube register holding the same value there
    bool inverted = false;    // the GameCube branch is taken when the PC one is not
    int fpuMask = -1;         // the PC branch tests `fnstsw ax; test ah, mask` (x87 compare)
    std::string note;
};

// A function of both: its GameCube copies, how a call's arguments and result correspond.
struct FuncMap {
    u32 pc = 0;
    std::vector<GcAddr> gcs;
    // GameCube argument registers (r3..r10, f1..) from x86 sources: a register (0-7) or a stack
    // argument (100 + n: the n-th dword above the return address).
    std::vector<std::pair<int, int>> args;
    bool known = true;   // false: not described ('?'): r3.. are passed as stack arguments, eax back
    int ret = 0;         // 0 none, 1 eax <-> r3, 2 st0 <-> f1
    u32 pop = 0;         // bytes of arguments the callee pops (stdcall, thiscall, fastcall)
    u32 len = 0;         // the PC function's bytes (0: not known)
    std::string note;
    u32 gc() const;      // the first copy that is loaded now (0 if none)
};

// A variable: PC bytes [pc, pc+size) are GameCube bytes [gc, gc+size), in elements of `elem`
// bytes (byte-swapped each). In a REL module, it exists only while the module is linked.
struct DataMap {
    u32 pc = 0, size = 0, elem = 4;
    bool ptr = false;  // its elements are pointers (translated, not just byte-swapped)
    GcAddr gc;
    std::string note;
};

struct PcMap {
    std::map<u32, InsnMap> insns;  // by PC address
    std::map<u32, FuncMap> funcs;
    std::map<u32, DataMap> data;   // by PC start
    std::map<u32, u32> padding;    // PC start -> length: bytes no code runs (patches there do nothing)

    bool load(const std::filesystem::path& file);  // later files' entries replace earlier ones
    const InsnMap* insnCovering(u32 pc) const;     // the entry whose instruction contains pc
    const DataMap* dataAt(u32 pc) const;
    const FuncMap* funcAt(u32 pc) const;           // the function starting at pc
    const FuncMap* funcCovering(u32 pc) const;     // the function whose bytes hold pc (len known)
    bool isPadding(u32 pc) const;
};

}  // namespace mh
