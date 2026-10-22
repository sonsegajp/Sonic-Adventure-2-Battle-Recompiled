#include "modhost/x86dec.h"

namespace mh {

namespace {

// Immediate kinds of one-byte opcodes: 0 none, 1 imm8, 2 imm16/32 (operand size), 3 imm16,
// 4 imm16+imm8, 5 moffs32, 6 far pointer, 7 rel8, 8 rel32. ModRM presence separately.
struct Op1 {
    bool modrm;
    u8 imm;
};
Op1 op1(u8 op) {
    if (op < 0x40) {
        if ((op & 7) < 4) return {true, 0};
        if ((op & 7) == 4) return {false, 1};
        if ((op & 7) == 5) return {false, 2};
        return {false, 0};
    }
    if (op < 0x60) return {false, 0};
    switch (op) {
    case 0x62: case 0x63: return {true, 0};
    case 0x68: return {false, 2};
    case 0x69: return {true, 2};
    case 0x6A: return {false, 1};
    case 0x6B: return {true, 1};
    case 0x80: case 0x82: case 0x83: return {true, 1};
    case 0x81: return {true, 2};
    case 0x9A: case 0xEA: return {false, 6};
    case 0xA0: case 0xA1: case 0xA2: case 0xA3: return {false, 5};
    case 0xA8: return {false, 1};
    case 0xA9: return {false, 2};
    case 0xC0: case 0xC1: case 0xC6: return {true, 1};
    case 0xC7: return {true, 2};
    case 0xC2: case 0xCA: return {false, 3};
    case 0xC4: case 0xC5: return {true, 0};
    case 0xC8: return {false, 4};
    case 0xCD: case 0xD4: case 0xD5: return {false, 1};
    case 0xE4: case 0xE5: case 0xE6: case 0xE7: return {false, 1};
    case 0xE0: case 0xE1: case 0xE2: case 0xE3: case 0xEB: return {false, 7};
    case 0xE8: case 0xE9: return {false, 8};
    default: break;
    }
    if (op >= 0x70 && op <= 0x7F) return {false, 7};
    if (op >= 0x84 && op <= 0x8F) return {true, 0};
    if (op >= 0xB0 && op <= 0xB7) return {false, 1};
    if (op >= 0xB8 && op <= 0xBF) return {false, 2};
    if (op >= 0xD0 && op <= 0xD3) return {true, 0};
    if (op >= 0xD8 && op <= 0xDF) return {true, 0};
    if (op == 0xF6 || op == 0xF7 || op == 0xFE || op == 0xFF) return {true, 0};
    return {false, 0};
}

}  // namespace

s32 X86Insn::immSigned() const {
    if (immSize == 1) return s32(s8(u8(imm)));
    if (immSize == 2) return s32(s16(u16(imm)));
    return s32(imm);
}

bool X86Insn::sameOperation(const X86Insn& o) const {
    return op == o.op && twoByte == o.twoByte && opsz == o.opsz && hasModrm == o.hasModrm && (!hasModrm || reg == o.reg);
}

bool x86Decode(const u8* b, int avail, u32 addr, X86Insn& x) {
    x = X86Insn{};
    int i = 0;
    auto need = [&](int n) { return i + n <= avail; };
    for (;; i++) {
        if (!need(1)) return false;
        const u8 p = b[i];
        if (p == 0x66) x.opsz = true;
        else if (p == 0xF2 || p == 0xF3 || p == 0xF0 || p == 0x2E || p == 0x3E || p == 0x26 || p == 0x36 ||
                 p == 0x64 || p == 0x65) {
        } else break;
    }
    u8 immKind = 0;
    x.op = b[i++];
    if (x.op == 0x0F) {
        if (!need(1)) return false;
        x.twoByte = true;
        x.op = b[i++];
        const u8 o = x.op;
        if (o >= 0x80 && o <= 0x8F) immKind = 8;
        else if (o == 0x31 || o == 0xA2 || o == 0x0B || o == 0x05 || o == 0x77 || (o >= 0xC8 && o <= 0xCF)) {
        } else if (o == 0x38 || o == 0x3A) {  // three-byte opcodes
            if (!need(1)) return false;
            i++;
            x.hasModrm = true;
            if (o == 0x3A) immKind = 1;
        } else {
            x.hasModrm = true;
            if ((o >= 0x70 && o <= 0x73) || o == 0xA4 || o == 0xAC || o == 0xBA || o == 0xC2 || o == 0xC4 ||
                o == 0xC5 || o == 0xC6)
                immKind = 1;
        }
        if (o >= 0x80 && o <= 0x8F) x.flow = X86Insn::Jcc, x.cc = o & 15;
    } else {
        const Op1 d = op1(x.op);
        x.hasModrm = d.modrm;
        immKind = d.imm;
        if (x.op >= 0x70 && x.op <= 0x7F) x.flow = X86Insn::Jcc, x.cc = x.op & 15;
        else if (x.op == 0xEB || x.op == 0xE9) x.flow = X86Insn::Jmp;
        else if (x.op == 0xE8) x.flow = X86Insn::Call;
        else if (x.op == 0xC3 || x.op == 0xC2) x.flow = X86Insn::Ret;
    }
    if (x.hasModrm) {
        if (!need(1)) return false;
        const u8 m = b[i++];
        x.mod = m >> 6, x.reg = (m >> 3) & 7, x.rm = m & 7;
        if (x.mod != 3) {
            x.mem = true;
            if (x.rm == 4) {
                if (!need(1)) return false;
                const u8 sib = b[i++];
                x.scale = 1 << (sib >> 6);
                x.index = ((sib >> 3) & 7) == 4 ? -1 : (sib >> 3) & 7;
                x.base = sib & 7;
                if (x.base == 5 && x.mod == 0) x.base = -1, x.dispSize = 4;
            } else if (x.rm == 5 && x.mod == 0) {
                x.base = -1, x.dispSize = 4;
            } else {
                x.base = x.rm;
            }
            if (x.mod == 1) x.dispSize = 1;
            else if (x.mod == 2) x.dispSize = 4;
            if (x.dispSize) {
                if (!need(x.dispSize)) return false;
                x.dispOff = i;
                if (x.dispSize == 1) x.disp = s8(b[i]);
                else x.disp = s32(u32(b[i]) | u32(b[i + 1]) << 8 | u32(b[i + 2]) << 16 | u32(b[i + 3]) << 24);
                i += x.dispSize;
            }
        }
        if (x.op == 0xFF && !x.twoByte && (x.reg == 2 || x.reg == 4)) {
            x.flow = x.reg == 2 ? X86Insn::Call : X86Insn::Jmp;
            x.indirect = true;
        }
        if (!x.twoByte && (x.op == 0xF6 || x.op == 0xF7) && x.reg < 2) immKind = x.op == 0xF6 ? 1 : 2;
    }
    auto takeImm = [&](int n) {
        if (!need(n)) return false;
        x.immOff = i, x.immSize = n;
        x.imm = 0;
        for (int k = 0; k < n; k++) x.imm |= u32(b[i + k]) << (8 * k);
        i += n;
        return true;
    };
    switch (immKind) {
    case 1: if (!takeImm(1)) return false; break;
    case 2: if (!takeImm(x.opsz ? 2 : 4)) return false; break;
    case 3: if (!takeImm(2)) return false; break;
    case 4: if (!takeImm(2) || !need(1)) return false; i++; break;
    case 5: if (!takeImm(4)) return false; break;
    case 6: if (!takeImm(4) || !need(2)) return false; i += 2; break;
    case 7:
        if (!takeImm(1)) return false;
        x.target = addr + u32(i) + u32(s32(s8(u8(x.imm))));
        break;
    case 8:
        if (!takeImm(4)) return false;
        x.target = addr + u32(i) + x.imm;
        break;
    default: break;
    }
    if (x.op == 0xE0 || x.op == 0xE1 || x.op == 0xE2 || x.op == 0xE3) x.flow = X86Insn::None;  // loop/jecxz
    x.len = i;
    return true;
}

}  // namespace mh
