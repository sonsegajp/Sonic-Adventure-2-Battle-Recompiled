// One-instruction x86-32 decoder for the bridge: what a mod changed in a PC instruction (its
// immediate, displacement, memory operand, branch condition or target).
#pragma once

#include "modhost/x86.h"

namespace mh {

struct X86Insn {
    int len = 0;
    bool opsz = false;  // 66 prefix
    u8 op = 0;          // opcode byte (after 0F for two-byte opcodes)
    bool twoByte = false;
    bool hasModrm = false;
    int mod = 0, reg = 0, rm = 0;
    // Memory operand (hasModrm && mod != 3): [base + index * scale + disp]; base/index -1 = none.
    bool mem = false;
    int base = -1, index = -1, scale = 1;
    s32 disp = 0;
    int dispOff = 0, dispSize = 0;
    u32 imm = 0;  // as encoded (zero-extended)
    int immOff = 0, immSize = 0;
    enum Flow { None, Jcc, Jmp, Call, Ret } flow = None;
    int cc = -1;      // Jcc condition (0-15, x86 encoding)
    u32 target = 0;   // Jcc / Jmp / Call destination (direct forms)
    bool indirect = false;

    s32 immSigned() const;  // the immediate as the instruction uses it (sign-extended imm8 etc.)
    bool sameOperation(const X86Insn& o) const;  // same opcode, operand size and ModRM reg field
};

// Decodes the instruction at `addr` from `bytes`; false if it is not one this decoder knows.
bool x86Decode(const u8* bytes, int avail, u32 addr, X86Insn& out);

}  // namespace mh
