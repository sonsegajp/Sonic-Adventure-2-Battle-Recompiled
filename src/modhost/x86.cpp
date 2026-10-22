// x86-32 interpreter (see x86.h). Covers what MSVC-built 32-bit mod DLLs use: the integer
// instruction set, x87, SSE/SSE2 (scalar and packed), string instructions, cpuid. Flags are
// computed eagerly; speed is no concern for mod code (hooks run a few hundred instructions).
#include "modhost/x86.h"

#include "runtime/runtime.h"

#include <cmath>
#include <cstdio>
#include <stdexcept>

namespace mh {

// ---- memory ----

Memory::Memory() : pages_(1u << 20) {}

void Memory::map(u32 addr, u32 size) {
    if (!size) return;
    for (u32 p = addr >> 12; p <= (addr + size - 1) >> 12; p++)
        if (!pages_[p]) pages_[p] = std::make_unique<u8[]>(4096);
}

// The GameCube window: big-endian memory. An access is split into the naturally aligned fields
// it covers (at most 4 bytes: 8-byte accesses are pairs of floats or ints far more often than
// doubles), each byte-swapped on its own: a dword at +0x12 is the two halfwords +0x12 and +0x14,
// as on PC.
namespace {
template <typename F>
void gcFields(u32 a, int size, F&& f) {
    for (int done = 0; done < size;) {
        int n = 4;
        while (n > 1 && (((a + u32(done)) & u32(n - 1)) || done + n > size)) n >>= 1;
        f(done, n);
        done += n;
    }
}
}  // namespace

u64 Memory::slow(u32 a, int size) {
    if (a >= kGcBase && a < kGcEnd) {
        u64 v = 0;
        gcFields(a, size, [&](int off, int n) {
            const u32 at = a + u32(off);
            u64 f = n == 4 ? rt::rd32(at) : n == 2 ? rt::rd16(at) : rt::rd8(at);
            if (n == 4 && foreign) f = foreign->fromGc(u32(f));
            v |= f << (8 * off);
        });
        return v;
    }
    if (a >= kPcImageBase && a < kPcImageEnd && foreign) return foreign->read(a, size);
    // Straddles a host page boundary, or unmapped.
    u64 v = 0;
    for (int i = 0; i < size; i++) {
        const u32 x = a + u32(i);
        if (u8* h = host(x)) v |= u64(*h) << (8 * i);
        else {
            static int s_warned = 0;
            if (s_warned++ < 20) rt::log("[modhost] read of unmapped %08X\n", x);
            return v;
        }
    }
    return v;
}

void Memory::slowWrite(u32 a, u64 v, int size) {
    if (a >= kGcBase && a < kGcEnd) {
        gcFields(a, size, [&](int off, int n) {
            const u32 at = a + u32(off), f = u32(v >> (8 * off));
            if (n == 4) rt::wr32(at, foreign ? foreign->toGc(f) : f);
            else if (n == 2) rt::wr16(at, f & 0xFFFF);
            else rt::wr8(at, f & 0xFF);
        });
        return;
    }
    if (a >= kPcImageBase && a < kPcImageEnd && foreign) return foreign->write(a, v, size);
    for (int i = 0; i < size; i++) {
        const u32 x = a + u32(i);
        if (u8* h = host(x)) *h = u8(v >> (8 * i));
        else {
            static int s_warned = 0;
            if (s_warned++ < 20) rt::log("[modhost] write to unmapped %08X\n", x);
            return;
        }
    }
}

#define FAST_READ(T, a)                                                         \
    if (((a) & 0xFFF) <= 0x1000 - sizeof(T)) {                                  \
        if (const auto& pg = pages_[(a) >> 12]) {                               \
            T v;                                                                \
            std::memcpy(&v, pg.get() + ((a) & 0xFFF), sizeof(T));             \
            return v;                                                           \
        }                                                                       \
    }
#define FAST_WRITE(T, a, v)                                                     \
    if (((a) & 0xFFF) <= 0x1000 - sizeof(T)) {                                  \
        if (const auto& pg = pages_[(a) >> 12]) {                               \
            const T t = T(v);                                                   \
            std::memcpy(pg.get() + ((a) & 0xFFF), &t, sizeof(T));             \
            return;                                                             \
        }                                                                       \
    }

u8 Memory::r8(u32 a) { FAST_READ(u8, a) return u8(slow(a, 1)); }
u16 Memory::r16(u32 a) { FAST_READ(u16, a) return u16(slow(a, 2)); }
u32 Memory::r32(u32 a) { FAST_READ(u32, a) return u32(slow(a, 4)); }
u64 Memory::r64(u32 a) { FAST_READ(u64, a) return slow(a, 8); }
void Memory::w8(u32 a, u8 v) { FAST_WRITE(u8, a, v) slowWrite(a, v, 1); }
void Memory::w16(u32 a, u16 v) { FAST_WRITE(u16, a, v) slowWrite(a, v, 2); }
void Memory::w32(u32 a, u32 v) { FAST_WRITE(u32, a, v) slowWrite(a, v, 4); }
void Memory::w64(u32 a, u64 v) { FAST_WRITE(u64, a, v) slowWrite(a, v, 8); }

void Memory::read(u32 a, void* dst, u32 n) {
    u8* d = static_cast<u8*>(dst);
    for (u32 i = 0; i < n; i++) d[i] = r8(a + i);
}
void Memory::write(u32 a, const void* src, u32 n) {
    const u8* s = static_cast<const u8*>(src);
    for (u32 i = 0; i < n; i++) w8(a + i, s[i]);
}
std::string Memory::cstr(u32 a, size_t max) {
    std::string s;
    for (size_t i = 0; i < max; i++) {
        const u8 c = r8(a + u32(i));
        if (!c) break;
        s += char(c);
    }
    return s;
}
std::u16string Memory::wstr(u32 a, size_t max) {
    std::u16string s;
    for (size_t i = 0; i < max; i++) {
        const u16 c = r16(a + u32(2 * i));
        if (!c) break;
        s += char16_t(c);
    }
    return s;
}

// ---- cpu ----

u32 Cpu::eflags() const {
    return 2u | (cf ? 1u : 0) | (pf ? 4u : 0) | (af ? 0x10u : 0) | (zf ? 0x40u : 0) | (sf ? 0x80u : 0) |
           (df ? 0x400u : 0) | (of ? 0x800u : 0) | 0x200u;
}
void Cpu::setEflags(u32 v) {
    cf = v & 1, pf = v & 4, af = v & 0x10, zf = v & 0x40, sf = v & 0x80, df = v & 0x400, of = v & 0x800;
}

std::string Cpu::where() const {
    char b[160];
    std::snprintf(b, sizeof b, "eip %08X eax %08X ecx %08X edx %08X ebx %08X esp %08X ebp %08X esi %08X edi %08X", eip,
                  r[EAX], r[ECX], r[EDX], r[EBX], r[ESP], r[EBP], r[ESI], r[EDI]);
    std::string s = b;
    s += "; came from";
    for (u32 i = 1; i <= 8; i++) {
        char t[12];
        std::snprintf(t, sizeof t, " %08X", trail[(trailPos - i) & 15]);
        s += t;
    }
    return s;
}

u32 Cpu::call(u32 entry, std::initializer_list<u32> args) {
    const u32 savedEip = eip;
    std::vector<u32> a(args);
    for (size_t i = a.size(); i-- > 0;) push(a[i]);
    const u32 sp = r[ESP];
    push(kReturnSentinel);
    eip = entry;
    while (eip != kReturnSentinel) {
        step();
        if (!(steps & 0xFFFFF) && onSlice) onSlice();
    }
    // cdecl leaves the arguments for the caller; stdcall popped them already.
    r[ESP] = sp + 4 * u32(a.size());
    eip = savedEip;
    return r[EAX];
}

namespace {

bool parity(u32 v) {
    v &= 0xFF;
    v ^= v >> 4;
    v ^= v >> 2;
    v ^= v >> 1;
    return !(v & 1);
}

struct Unimplemented : ModFault {
    using ModFault::ModFault;
};

}  // namespace

// One instruction. Organised as a classic decoder: prefixes, opcode, ModRM operands.
void Cpu::step() {
    steps++;
    if (eip >= kThunkBase && eip < kReturnSentinel) {
        mem.foreign->execThunk(*this, (eip - kThunkBase) >> 4);
        return;
    }
    if (eip >= kPcImageBase && eip < kPcImageEnd) {
        mem.foreign->execPc(*this, eip);
        return;
    }
    if (eip >= kGcBase && eip < kGcEnd) {
        mem.foreign->execGc(*this, eip);
        return;
    }
    const u32 start = eip;
    if (start != insn + 0 && (start < insn || start > insn + 15)) trail[trailPos++ & 15] = start;
    insn = start;
    auto f8 = [&] { return mem.r8(eip++); };
    auto f16 = [&] { const u16 v = mem.r16(eip); eip += 2; return v; };
    auto f32 = [&] { const u32 v = mem.r32(eip); eip += 4; return v; };

    bool opsz = false, rep = false, repne = false;
    u32 seg = 0;  // fs base when overridden
    u8 op;
    for (;;) {
        op = f8();
        if (op == 0x66) opsz = true;
        else if (op == 0xF3) rep = true;
        else if (op == 0xF2) repne = true;
        else if (op == 0x64) seg = fsBase;
        else if (op == 0x65 || op == 0x2E || op == 0x3E || op == 0x26 || op == 0x36 || op == 0xF0) {
        } else break;
    }
    const int osz = opsz ? 2 : 4;

    // ---- ModRM ----
    u8 mrm = 0;
    u32 ea = 0;
    bool isReg = false;
    auto decode = [&] {
        mrm = f8();
        const u8 mod = mrm >> 6, rm = mrm & 7;
        if (mod == 3) {
            isReg = true;
            return;
        }
        isReg = false;
        u32 a = 0;
        if (rm == 4) {
            const u8 sib = f8();
            const u8 ss = sib >> 6, idx = (sib >> 3) & 7, base = sib & 7;
            if (base == 5 && mod == 0) a = f32();
            else a = r[base];
            if (idx != 4) a += r[idx] << ss;
        } else if (rm == 5 && mod == 0) {
            a = f32();
        } else {
            a = r[rm];
        }
        if (mod == 1) a += u32(s32(s8(f8())));
        else if (mod == 2) a += f32();
        ea = a + seg;
    };
    auto regN = [&] { return (mrm >> 3) & 7; };
    auto rmN = [&] { return mrm & 7; };
    // 8-bit registers: AL CL DL BL AH CH DH BH
    auto getR8 = [&](int n) -> u8 { return n < 4 ? u8(r[n]) : u8(r[n - 4] >> 8); };
    auto setR8 = [&](int n, u8 v) {
        if (n < 4) r[n] = (r[n] & ~0xFFu) | v;
        else r[n - 4] = (r[n - 4] & ~0xFF00u) | (u32(v) << 8);
    };
    auto getR = [&](int n, int size) -> u32 { return size == 1 ? getR8(n) : size == 2 ? (r[n] & 0xFFFF) : r[n]; };
    auto setR = [&](int n, int size, u32 v) {
        if (size == 1) setR8(n, u8(v));
        else if (size == 2) r[n] = (r[n] & 0xFFFF0000u) | (v & 0xFFFF);
        else r[n] = v;
    };
    auto rdRM = [&](int size) -> u32 {
        if (isReg) return getR(rmN(), size);
        return size == 1 ? mem.r8(ea) : size == 2 ? mem.r16(ea) : mem.r32(ea);
    };
    auto wrRM = [&](int size, u32 v) {
        if (isReg) return setR(rmN(), size, v);
        if (size == 1) mem.w8(ea, u8(v));
        else if (size == 2) mem.w16(ea, u16(v));
        else mem.w32(ea, v);
    };
    auto immN = [&](int size) -> u32 { return size == 1 ? f8() : size == 2 ? f16() : f32(); };

    // ---- flags ----
    auto signBit = [](int size) { return size == 1 ? 0x80u : size == 2 ? 0x8000u : 0x80000000u; };
    auto maskOf = [](int size) { return size == 1 ? 0xFFu : size == 2 ? 0xFFFFu : 0xFFFFFFFFu; };
    auto szp = [&](u32 v, int size) {
        v &= maskOf(size);
        zf = v == 0;
        sf = v & signBit(size);
        pf = parity(v);
    };
    // ALU: 0 add, 1 or, 2 adc, 3 sbb, 4 and, 5 sub, 6 xor, 7 cmp
    auto alu = [&](int o, u32 a, u32 b, int size) -> u32 {
        const u32 m = maskOf(size), sb = signBit(size);
        a &= m, b &= m;
        u32 res = 0;
        switch (o) {
        case 0:
        case 2: {
            const u64 c = (o == 2 && cf) ? 1 : 0;
            const u64 full = u64(a) + b + c;
            res = u32(full) & m;
            cf = full > m;
            of = (~(a ^ b) & (a ^ res)) & sb;
            af = ((a ^ b ^ res) & 0x10) != 0;
            break;
        }
        case 3:
        case 5:
        case 7: {
            const u64 c = (o == 3 && cf) ? 1 : 0;
            res = u32(u64(a) - b - c) & m;
            cf = u64(a) < u64(b) + c;
            of = ((a ^ b) & (a ^ res)) & sb;
            af = ((a ^ b ^ res) & 0x10) != 0;
            break;
        }
        case 1: res = a | b, cf = of = false; break;
        case 4: res = a & b, cf = of = false; break;
        case 6: res = a ^ b, cf = of = false; break;
        }
        szp(res, size);
        return res;
    };
    auto cond = [&](int cc) -> bool {
        bool v;
        switch (cc >> 1) {
        case 0: v = of; break;
        case 1: v = cf; break;
        case 2: v = zf; break;
        case 3: v = cf || zf; break;
        case 4: v = sf; break;
        case 5: v = pf; break;
        case 6: v = sf != of; break;
        default: v = zf || (sf != of); break;
        }
        return (cc & 1) ? !v : v;
    };
    auto shift = [&](int o, u32 v, u32 count, int size) -> u32 {
        const u32 m = maskOf(size);
        const int bits = size * 8;
        count &= 31;
        if (!count) return v & m;
        v &= m;
        u32 res = v;
        switch (o) {
        case 0:  // rol
            count %= bits;
            res = ((v << count) | (v >> ((bits - count) % bits))) & m;
            cf = res & 1;
            of = ((res >> (bits - 1)) & 1) ^ cf;
            return res;
        case 1:  // ror
            count %= bits;
            res = ((v >> count) | (v << ((bits - count) % bits))) & m;
            cf = (res >> (bits - 1)) & 1;
            of = ((res >> (bits - 1)) ^ (res >> (bits - 2))) & 1;
            return res;
        case 2: {  // rcl
            for (u32 i = 0; i < count; i++) {
                const bool nc = (res >> (bits - 1)) & 1;
                res = ((res << 1) | (cf ? 1 : 0)) & m;
                cf = nc;
            }
            of = ((res >> (bits - 1)) & 1) ^ cf;
            return res;
        }
        case 3: {  // rcr
            for (u32 i = 0; i < count; i++) {
                const bool nc = res & 1;
                res = (res >> 1) | (cf ? (1u << (bits - 1)) : 0);
                cf = nc;
            }
            of = ((res >> (bits - 1)) ^ (res >> (bits - 2))) & 1;
            return res;
        }
        case 4:
        case 6:  // shl
            cf = count <= u32(bits) ? (v >> (bits - count)) & 1 : false;
            res = (count < 32 ? v << count : 0) & m;
            of = ((res >> (bits - 1)) & 1) ^ cf;
            break;
        case 5:  // shr
            cf = (v >> (count - 1)) & 1;
            res = count < 32 ? v >> count : 0;
            of = (v >> (bits - 1)) & 1;
            break;
        case 7: {  // sar
            const s32 sv = size == 1 ? s32(s8(v)) : size == 2 ? s32(s16(v)) : s32(v);
            cf = (sv >> (count - 1)) & 1;
            res = u32(sv >> (count > 31 ? 31 : count)) & m;
            of = false;
            break;
        }
        }
        szp(res, size);
        return res;
    };
    auto relJump = [&](s32 rel) { eip += u32(rel); };

    // ---- x87 helpers ----
    auto fcomFlags = [&](double a, double b) {
        // C0 (bit 8) less, C2 (bit 10) unordered, C3 (bit 14) equal
        fpuSw &= ~0x4500;
        if (std::isnan(a) || std::isnan(b)) fpuSw |= 0x4500;
        else if (a < b) fpuSw |= 0x0100;
        else if (a == b) fpuSw |= 0x4000;
    };
    auto fcomiFlags = [&](double a, double b) {
        of = sf = af = false;
        if (std::isnan(a) || std::isnan(b)) zf = pf = cf = true;
        else zf = a == b, pf = false, cf = a < b;
    };
    auto statusWord = [&]() -> u16 { return u16((fpuSw & ~0x3800) | ((top & 7) << 11)); };
    auto roundFpu = [&](double v) -> double {
        switch ((fpuCw >> 10) & 3) {
        case 0: return std::nearbyint(v);
        case 1: return std::floor(v);
        case 2: return std::ceil(v);
        default: return std::trunc(v);
        }
    };
    auto roundSse = [&](double v) -> double {
        switch ((mxcsr >> 13) & 3) {
        case 0: return std::nearbyint(v);
        case 1: return std::floor(v);
        case 2: return std::ceil(v);
        default: return std::trunc(v);
        }
    };
    auto toInt32 = [](double v) -> u32 {
        if (std::isnan(v) || v >= 2147483648.0 || v < -2147483648.0) return 0x80000000u;
        return u32(s32(v));
    };

    auto unimpl = [&](const char* what) {
        char b[96];
        std::snprintf(b, sizeof b, "%s at %08X (op %02X)", what, start, op);
        throw Unimplemented(b);
    };

    // ---- SSE operand helpers ----
    auto xmmRM = [&](Xmm& out, int bytes) {  // reads 4/8/16 bytes from xmm reg or memory
        if (isReg) {
            out = xmm[rmN()];
            return;
        }
        std::memset(&out, 0, sizeof out);
        if (bytes == 4) out.d[0] = mem.r32(ea);
        else if (bytes == 8) out.q[0] = mem.r64(ea);
        else out.q[0] = mem.r64(ea), out.q[1] = mem.r64(ea + 8);
    };
    auto storeXmm = [&](const Xmm& v, int bytes) {
        if (isReg) {
            if (bytes == 16) xmm[rmN()] = v;
            else if (bytes == 8) xmm[rmN()].q[0] = v.q[0];
            else xmm[rmN()].d[0] = v.d[0];
            return;
        }
        if (bytes == 4) mem.w32(ea, v.d[0]);
        else if (bytes == 8) mem.w64(ea, v.q[0]);
        else mem.w64(ea, v.q[0]), mem.w64(ea + 8, v.q[1]);
    };

    try {
        switch (op) {
        // ---- ALU r/m, reg / reg, r/m / acc, imm ----
        case 0x00: case 0x01: case 0x02: case 0x03: case 0x04: case 0x05:
        case 0x08: case 0x09: case 0x0A: case 0x0B: case 0x0C: case 0x0D:
        case 0x10: case 0x11: case 0x12: case 0x13: case 0x14: case 0x15:
        case 0x18: case 0x19: case 0x1A: case 0x1B: case 0x1C: case 0x1D:
        case 0x20: case 0x21: case 0x22: case 0x23: case 0x24: case 0x25:
        case 0x28: case 0x29: case 0x2A: case 0x2B: case 0x2C: case 0x2D:
        case 0x30: case 0x31: case 0x32: case 0x33: case 0x34: case 0x35:
        case 0x38: case 0x39: case 0x3A: case 0x3B: case 0x3C: case 0x3D: {
            const int o = op >> 3, form = op & 7;
            const int size = (form & 1) ? osz : 1;
            if (form >= 4) {  // al/eax, imm
                const u32 res = alu(o, getR(EAX, size), immN(size), size);
                if (o != 7) setR(EAX, size, res);
                break;
            }
            decode();
            if (form < 2) {  // r/m, reg
                const u32 res = alu(o, rdRM(size), getR(regN(), size), size);
                if (o != 7) wrRM(size, res);
            } else {  // reg, r/m
                const u32 res = alu(o, getR(regN(), size), rdRM(size), size);
                if (o != 7) setR(regN(), size, res);
            }
            break;
        }
        case 0x0F: {
            const u8 op2 = f8();
            switch (op2) {
            case 0x0B: unimpl("ud2"); break;
            case 0x1F: decode(); break;  // nop r/m
            case 0x01:
                if (f8() == 0xD0) r[EAX] = 7, r[EDX] = 0;  // xgetbv: x87/SSE/AVX state enabled
                else unimpl("0F 01");
                break;
            case 0x31: {  // rdtsc
                const u64 t = steps * 4;
                r[EAX] = u32(t), r[EDX] = u32(t >> 32);
                break;
            }
            case 0xA2: {  // cpuid: a plain SSE2 machine
                const u32 leaf = r[EAX];
                if (leaf == 0) r[EAX] = 1, r[EBX] = 0x756E6547, r[EDX] = 0x49656E69, r[ECX] = 0x6C65746E;
                else if (leaf == 1) r[EAX] = 0x000306A9, r[EBX] = 0, r[ECX] = 0, r[EDX] = 0x07808111;  // fpu tsc cx8 cmov mmx fxsr sse sse2
                else r[EAX] = r[EBX] = r[ECX] = r[EDX] = 0;
                break;
            }
            case 0x40: case 0x41: case 0x42: case 0x43: case 0x44: case 0x45: case 0x46: case 0x47:
            case 0x48: case 0x49: case 0x4A: case 0x4B: case 0x4C: case 0x4D: case 0x4E: case 0x4F: {
                decode();
                const u32 v = rdRM(osz);
                if (cond(op2 & 15)) setR(regN(), osz, v);
                break;
            }
            case 0x80: case 0x81: case 0x82: case 0x83: case 0x84: case 0x85: case 0x86: case 0x87:
            case 0x88: case 0x89: case 0x8A: case 0x8B: case 0x8C: case 0x8D: case 0x8E: case 0x8F: {
                const s32 rel = s32(f32());
                if (cond(op2 & 15)) relJump(rel);
                break;
            }
            case 0x90: case 0x91: case 0x92: case 0x93: case 0x94: case 0x95: case 0x96: case 0x97:
            case 0x98: case 0x99: case 0x9A: case 0x9B: case 0x9C: case 0x9D: case 0x9E: case 0x9F:
                decode();
                wrRM(1, cond(op2 & 15) ? 1 : 0);
                break;
            case 0xA3: case 0xAB: case 0xB3: case 0xBB: {  // bt/bts/btr/btc r/m, reg
                decode();
                const u32 bit = getR(regN(), osz);
                u32 a = ea;
                if (!isReg) a += u32(s32(bit) >> 5) * 4;
                const u32 v = isReg ? getR(rmN(), osz) : mem.r32(a);
                const u32 m = 1u << (bit & (isReg ? osz * 8 - 1 : 31));
                cf = v & m;
                u32 nv = v;
                if (op2 == 0xAB) nv |= m;
                else if (op2 == 0xB3) nv &= ~m;
                else if (op2 == 0xBB) nv ^= m;
                if (op2 != 0xA3) {
                    if (isReg) setR(rmN(), osz, nv);
                    else mem.w32(a, nv);
                }
                break;
            }
            case 0xBA: {  // bt group, imm
                decode();
                const int o = regN();
                const u32 v = rdRM(osz);
                const u32 m = 1u << (f8() & (osz * 8 - 1));
                cf = v & m;
                if (o == 5) wrRM(osz, v | m);
                else if (o == 6) wrRM(osz, v & ~m);
                else if (o == 7) wrRM(osz, v ^ m);
                break;
            }
            case 0xA4: case 0xA5: case 0xAC: case 0xAD: {  // shld/shrd
                decode();
                const u32 count = ((op2 & 1) ? r[ECX] : f8()) & 31;
                if (!count) break;
                const u32 dst = rdRM(osz), src = getR(regN(), osz);
                const int bits = osz * 8;
                u32 res;
                if (op2 < 0xA8) {
                    res = (dst << count) | (src >> (bits - count));
                    cf = (dst >> (bits - count)) & 1;
                } else {
                    res = (dst >> count) | (src << (bits - count));
                    cf = (dst >> (count - 1)) & 1;
                }
                szp(res, osz);
                wrRM(osz, res);
                break;
            }
            case 0xAF: {  // imul reg, r/m
                decode();
                const s64 a = osz == 2 ? s16(getR(regN(), 2)) : s32(r[regN()]);
                const s64 b = osz == 2 ? s16(rdRM(2)) : s32(rdRM(4));
                const s64 p = a * b;
                setR(regN(), osz, u32(p));
                cf = of = osz == 2 ? p != s16(p) : p != s32(p);
                break;
            }
            case 0xB0: case 0xB1: {  // cmpxchg
                const int size = op2 & 1 ? osz : 1;
                decode();
                const u32 v = rdRM(size);
                alu(7, getR(EAX, size), v, size);
                if (zf) wrRM(size, getR(regN(), size));
                else setR(EAX, size, v);
                break;
            }
            case 0xB6: decode(); setR(regN(), osz, rdRM(1)); break;
            case 0xB7: decode(); setR(regN(), osz, rdRM(2)); break;
            case 0xBE: decode(); setR(regN(), osz, u32(s32(s8(rdRM(1))))); break;
            case 0xBF: decode(); setR(regN(), osz, u32(s32(s16(rdRM(2))))); break;
            case 0xBC: case 0xBD: {  // bsf/bsr
                decode();
                const u32 v = rdRM(osz);
                zf = v == 0;
                if (v) {
                    u32 i = 0;
                    if (op2 == 0xBC) while (!(v >> i & 1)) i++;
                    else for (i = 31; !(v >> i & 1); i--) {}
                    setR(regN(), osz, i);
                }
                break;
            }
            case 0xC0: case 0xC1: {  // xadd
                const int size = op2 & 1 ? osz : 1;
                decode();
                const u32 d = rdRM(size), s = getR(regN(), size);
                const u32 sum = alu(0, d, s, size);
                setR(regN(), size, d);
                wrRM(size, sum);
                break;
            }
            case 0xC8: case 0xC9: case 0xCA: case 0xCB: case 0xCC: case 0xCD: case 0xCE: case 0xCF: {
                const u32 v = r[op2 & 7];
                r[op2 & 7] = (v >> 24) | ((v >> 8) & 0xFF00) | ((v << 8) & 0xFF0000) | (v << 24);
                break;
            }
            // ---- SSE / SSE2 ----
            case 0x10: case 0x11: {  // movups/movupd/movss/movsd
                decode();
                const int bytes = rep ? 4 : repne ? 8 : 16;
                if (op2 == 0x10) {
                    // Loads from memory clear the upper lanes; register moves of a scalar keep them.
                    Xmm v;
                    xmmRM(v, bytes);
                    if (bytes == 16) xmm[regN()] = v;
                    else {
                        if (!isReg) xmm[regN()] = Xmm{};
                        if (bytes == 4) xmm[regN()].d[0] = v.d[0];
                        else xmm[regN()].q[0] = v.q[0];
                    }
                } else {
                    storeXmm(xmm[regN()], bytes);
                }
                break;
            }
            case 0x12: case 0x13: case 0x16: case 0x17: {  // movlps/movhps (and movhlps/movlhps)
                decode();
                const bool hi = op2 >= 0x16;
                if (op2 == 0x12 || op2 == 0x16) {
                    if (isReg) {
                        if (hi) xmm[regN()].q[1] = xmm[rmN()].q[0];  // movlhps
                        else xmm[regN()].q[0] = xmm[rmN()].q[1];     // movhlps
                    } else {
                        xmm[regN()].q[hi] = mem.r64(ea);
                    }
                } else {
                    mem.w64(ea, xmm[regN()].q[hi]);
                }
                break;
            }
            case 0x14: case 0x15: {  // unpcklps/unpckhps (pd with 66)
                decode();
                Xmm s;
                xmmRM(s, 16);
                Xmm& d = xmm[regN()];
                Xmm o = d;
                if (opsz) {
                    if (op2 == 0x14) d.q[1] = s.q[0];
                    else d.q[0] = o.q[1], d.q[1] = s.q[1];
                } else if (op2 == 0x14) {
                    d.d[0] = o.d[0], d.d[1] = s.d[0], d.d[2] = o.d[1], d.d[3] = s.d[1];
                } else {
                    d.d[0] = o.d[2], d.d[1] = s.d[2], d.d[2] = o.d[3], d.d[3] = s.d[3];
                }
                break;
            }
            case 0x28: case 0x29: {  // movaps/movapd
                decode();
                if (op2 == 0x28) xmmRM(xmm[regN()], 16);
                else storeXmm(xmm[regN()], 16);
                break;
            }
            case 0x2A: {  // cvtsi2ss/cvtsi2sd (cvtpi2ps unsupported)
                decode();
                const s32 v = s32(rdRM(4));
                if (rep) xmm[regN()].f[0] = float(v);
                else if (repne) xmm[regN()].f64[0] = double(v);
                else unimpl("cvtpi2ps");
                break;
            }
            case 0x2C: case 0x2D: {  // cvt(t)ss2si / cvt(t)sd2si
                decode();
                Xmm v;
                xmmRM(v, rep ? 4 : 8);
                double d = rep ? double(v.f[0]) : v.f64[0];
                d = op2 == 0x2C ? std::trunc(d) : roundSse(d);
                setR(regN(), 4, toInt32(d));
                break;
            }
            case 0x2E: case 0x2F: {  // (u)comiss / (u)comisd
                decode();
                Xmm v;
                xmmRM(v, opsz ? 8 : 4);
                if (opsz) fcomiFlags(xmm[regN()].f64[0], v.f64[0]);
                else fcomiFlags(xmm[regN()].f[0], v.f[0]);
                break;
            }
            case 0x51: case 0x58: case 0x59: case 0x5C: case 0x5D: case 0x5E: case 0x5F: {
                decode();
                const bool ss = rep, sd = repne, pd = opsz && !rep && !repne;
                Xmm s;
                xmmRM(s, ss ? 4 : sd ? 8 : 16);
                Xmm& d = xmm[regN()];
                auto f = [&](double a, double b) -> double {
                    switch (op2) {
                    case 0x51: return std::sqrt(b);
                    case 0x58: return a + b;
                    case 0x59: return a * b;
                    case 0x5C: return a - b;
                    case 0x5D: return (a < b) ? a : b;
                    case 0x5E: return a / b;
                    default: return (a > b) ? a : b;
                    }
                };
                if (ss) d.f[0] = float(f(d.f[0], s.f[0]));
                else if (sd) d.f64[0] = f(d.f64[0], s.f64[0]);
                else if (pd) for (int i = 0; i < 2; i++) d.f64[i] = f(d.f64[i], s.f64[i]);
                else for (int i = 0; i < 4; i++) d.f[i] = float(f(d.f[i], s.f[i]));
                break;
            }
            case 0x54: case 0x55: case 0x56: case 0x57: {  // and/andn/or/xor ps/pd
                decode();
                Xmm s;
                xmmRM(s, 16);
                Xmm& d = xmm[regN()];
                for (int i = 0; i < 2; i++) {
                    if (op2 == 0x54) d.q[i] &= s.q[i];
                    else if (op2 == 0x55) d.q[i] = ~d.q[i] & s.q[i];
                    else if (op2 == 0x56) d.q[i] |= s.q[i];
                    else d.q[i] ^= s.q[i];
                }
                break;
            }
            case 0x5A: {  // cvtss2sd / cvtsd2ss / cvtps2pd / cvtpd2ps
                decode();
                Xmm s;
                Xmm& d = xmm[regN()];
                if (rep) xmmRM(s, 4), d.f64[0] = double(s.f[0]);
                else if (repne) xmmRM(s, 8), d.f[0] = float(s.f64[0]);
                else if (opsz) xmmRM(s, 16), d.f[0] = float(s.f64[0]), d.f[1] = float(s.f64[1]), d.q[1] = 0;
                else xmmRM(s, 8), d.f64[0] = double(s.f[0]), d.f64[1] = double(s.f[1]);
                break;
            }
            case 0x5B: {  // cvtdq2ps / cvtps2dq (66) / cvttps2dq (F3)
                decode();
                Xmm s;
                xmmRM(s, 16);
                Xmm& d = xmm[regN()];
                for (int i = 0; i < 4; i++) {
                    if (opsz) d.d[i] = toInt32(roundSse(s.f[i]));
                    else if (rep) d.d[i] = toInt32(std::trunc(s.f[i]));
                    else d.f[i] = float(s32(s.d[i]));
                }
                break;
            }
            case 0xE6: {  // cvtdq2pd (F3) / cvttpd2dq (66) / cvtpd2dq (F2)
                decode();
                Xmm s;
                xmmRM(s, rep ? 8 : 16);
                Xmm& d = xmm[regN()];
                if (rep) d.f64[1] = double(s32(s.d[1])), d.f64[0] = double(s32(s.d[0]));
                else {
                    const u32 a = toInt32(opsz ? std::trunc(s.f64[0]) : roundSse(s.f64[0]));
                    const u32 b = toInt32(opsz ? std::trunc(s.f64[1]) : roundSse(s.f64[1]));
                    d.d[0] = a, d.d[1] = b, d.q[1] = 0;
                }
                break;
            }
            case 0xC2: {  // cmpps/cmpss/cmppd/cmpsd
                decode();
                Xmm s;
                xmmRM(s, rep ? 4 : repne ? 8 : 16);
                const u8 pred = f8() & 7;
                Xmm& d = xmm[regN()];
                auto cmp = [&](double a, double b) -> bool {
                    const bool un = std::isnan(a) || std::isnan(b);
                    switch (pred) {
                    case 0: return a == b;
                    case 1: return a < b;
                    case 2: return a <= b;
                    case 3: return un;
                    case 4: return !(a == b);
                    case 5: return !(a < b);
                    case 6: return !(a <= b);
                    default: return !un;
                    }
                };
                if (rep) d.d[0] = cmp(d.f[0], s.f[0]) ? ~0u : 0;
                else if (repne) d.q[0] = cmp(d.f64[0], s.f64[0]) ? ~0ull : 0;
                else if (opsz) for (int i = 0; i < 2; i++) d.q[i] = cmp(d.f64[i], s.f64[i]) ? ~0ull : 0;
                else for (int i = 0; i < 4; i++) d.d[i] = cmp(d.f[i], s.f[i]) ? ~0u : 0;
                break;
            }
            case 0xC6: {  // shufps/shufpd
                decode();
                Xmm s;
                xmmRM(s, 16);
                const u8 imm = f8();
                Xmm& d = xmm[regN()];
                Xmm o = d;
                if (opsz) d.q[0] = o.q[imm & 1], d.q[1] = s.q[(imm >> 1) & 1];
                else
                    d.d[0] = o.d[imm & 3], d.d[1] = o.d[(imm >> 2) & 3], d.d[2] = s.d[(imm >> 4) & 3],
                    d.d[3] = s.d[(imm >> 6) & 3];
                break;
            }
            case 0x6E: decode(); xmm[regN()] = Xmm{}; xmm[regN()].d[0] = rdRM(4); break;  // movd xmm, r/m
            case 0x7E:
                decode();
                if (rep) {  // movq xmm, xmm/m64
                    Xmm s;
                    xmmRM(s, 8);
                    xmm[regN()] = Xmm{};
                    xmm[regN()].q[0] = s.q[0];
                } else {
                    wrRM(4, xmm[regN()].d[0]);  // movd r/m, xmm
                }
                break;
            case 0xD6: decode(); storeXmm(xmm[regN()], 8); if (isReg) xmm[rmN()].q[1] = 0; break;  // movq xmm/m64, xmm
            case 0x6F: case 0x7F: {  // movdqa/movdqu (66/F3); mmx forms unsupported
                if (!opsz && !rep) unimpl("mmx movq");
                decode();
                if (op2 == 0x6F) xmmRM(xmm[regN()], 16);
                else storeXmm(xmm[regN()], 16);
                break;
            }
            case 0x70: {  // pshufd (66), pshuflw (F2), pshufhw (F3)
                decode();
                Xmm s;
                xmmRM(s, 16);
                const u8 imm = f8();
                Xmm& d = xmm[regN()];
                if (opsz) for (int i = 0; i < 4; i++) d.d[i] = s.d[(imm >> (2 * i)) & 3];
                else if (repne) { d = s; for (int i = 0; i < 4; i++) d.w[i] = s.w[(imm >> (2 * i)) & 3]; }
                else { d = s; for (int i = 0; i < 4; i++) d.w[4 + i] = s.w[4 + ((imm >> (2 * i)) & 3)]; }
                break;
            }
            case 0x60: case 0x61: case 0x62: case 0x6C: case 0x68: case 0x69: case 0x6A: case 0x6D: {  // punpck
                decode();
                Xmm s;
                xmmRM(s, 16);
                Xmm& d = xmm[regN()];
                Xmm o = d, res{};
                const bool hi = op2 >= 0x68;
                const int el = (op2 == 0x60 || op2 == 0x68) ? 1 : (op2 == 0x61 || op2 == 0x69) ? 2 : (op2 == 0x62 || op2 == 0x6A) ? 4 : 8;
                const int n = 16 / el / 2;
                for (int i = 0; i < n; i++) {
                    std::memcpy(res.b + (2 * i) * el, o.b + (hi ? 8 : 0) + i * el, size_t(el));
                    std::memcpy(res.b + (2 * i + 1) * el, s.b + (hi ? 8 : 0) + i * el, size_t(el));
                }
                d = res;
                break;
            }
            case 0x72: case 0x73: case 0x71: {  // shifts by immediate (66)
                decode();
                const int o = regN();
                const u8 n = f8();
                Xmm& d = xmm[rmN()];
                if (op2 == 0x73 && o == 3) {  // psrldq
                    Xmm t{};
                    for (int i = 0; i + n < 16; i++) t.b[i] = d.b[i + n];
                    d = t;
                } else if (op2 == 0x73 && o == 7) {  // pslldq
                    Xmm t{};
                    for (int i = n; i < 16; i++) t.b[i] = d.b[i - n];
                    d = t;
                } else if (op2 == 0x73) {
                    for (int i = 0; i < 2; i++) d.q[i] = n > 63 ? 0 : o == 2 ? d.q[i] >> n : d.q[i] << n;
                } else if (op2 == 0x72) {
                    for (int i = 0; i < 4; i++)
                        d.d[i] = o == 2 ? (n > 31 ? 0 : d.d[i] >> n) : o == 4 ? u32(s32(d.d[i]) >> (n > 31 ? 31 : n)) : (n > 31 ? 0 : d.d[i] << n);
                } else {
                    for (int i = 0; i < 8; i++)
                        d.w[i] = o == 2 ? (n > 15 ? 0 : d.w[i] >> n) : o == 4 ? u16(s16(d.w[i]) >> (n > 15 ? 15 : n)) : u16(n > 15 ? 0 : d.w[i] << n);
                }
                break;
            }
            case 0xD4: case 0xFB: case 0xFE: case 0xFA: case 0xEF: case 0xEB: case 0xDB: case 0xDF:
            case 0x74: case 0x75: case 0x76: case 0xFC: case 0xF8: case 0xFD: case 0xF9: case 0xD7: {
                decode();
                Xmm s;
                xmmRM(s, 16);
                Xmm& d = xmm[regN()];
                switch (op2) {
                case 0xD4: for (int i = 0; i < 2; i++) d.q[i] += s.q[i]; break;  // paddq
                case 0xFB: for (int i = 0; i < 2; i++) d.q[i] -= s.q[i]; break;  // psubq
                case 0xFE: for (int i = 0; i < 4; i++) d.d[i] += s.d[i]; break;  // paddd
                case 0xFA: for (int i = 0; i < 4; i++) d.d[i] -= s.d[i]; break;  // psubd
                case 0xFD: for (int i = 0; i < 8; i++) d.w[i] += s.w[i]; break;  // paddw
                case 0xF9: for (int i = 0; i < 8; i++) d.w[i] -= s.w[i]; break;  // psubw
                case 0xFC: for (int i = 0; i < 16; i++) d.b[i] += s.b[i]; break;  // paddb
                case 0xF8: for (int i = 0; i < 16; i++) d.b[i] -= s.b[i]; break;  // psubb
                case 0xEF: for (int i = 0; i < 2; i++) d.q[i] ^= s.q[i]; break;  // pxor
                case 0xEB: for (int i = 0; i < 2; i++) d.q[i] |= s.q[i]; break;  // por
                case 0xDB: for (int i = 0; i < 2; i++) d.q[i] &= s.q[i]; break;  // pand
                case 0xDF: for (int i = 0; i < 2; i++) d.q[i] = ~d.q[i] & s.q[i]; break;  // pandn
                case 0x74: for (int i = 0; i < 16; i++) d.b[i] = d.b[i] == s.b[i] ? 0xFF : 0; break;
                case 0x75: for (int i = 0; i < 8; i++) d.w[i] = d.w[i] == s.w[i] ? 0xFFFF : 0; break;
                case 0x76: for (int i = 0; i < 4; i++) d.d[i] = d.d[i] == s.d[i] ? ~0u : 0; break;
                case 0xD7: {  // pmovmskb reg, xmm
                    u32 m = 0;
                    for (int i = 0; i < 16; i++) m |= u32(xmm[rmN()].b[i] >> 7) << i;
                    r[regN()] = m;
                    break;
                }
                }
                break;
            }
            case 0x50: {  // movmskps/pd
                decode();
                u32 m = 0;
                if (opsz) m = u32(xmm[rmN()].q[0] >> 63) | u32(xmm[rmN()].q[1] >> 63) << 1;
                else for (int i = 0; i < 4; i++) m |= (xmm[rmN()].d[i] >> 31) << i;
                r[regN()] = m;
                break;
            }
            case 0xAE: {  // ldmxcsr/stmxcsr/fences
                const u8 b = mem.r8(eip);
                if (b >= 0xE8) { eip++; break; }  // lfence/mfence/sfence
                decode();
                if (regN() == 2) mxcsr = mem.r32(ea);
                else if (regN() == 3) mem.w32(ea, mxcsr);
                else unimpl("0F AE");
                break;
            }
            case 0x18: decode(); break;  // prefetch
            default: unimpl("0F opcode");
            }
            break;
        }
        // ---- inc/dec/push/pop reg ----
        case 0x40: case 0x41: case 0x42: case 0x43: case 0x44: case 0x45: case 0x46: case 0x47: {
            const bool c = cf;
            setR(op & 7, osz, alu(0, getR(op & 7, osz), 1, osz));
            cf = c;
            break;
        }
        case 0x48: case 0x49: case 0x4A: case 0x4B: case 0x4C: case 0x4D: case 0x4E: case 0x4F: {
            const bool c = cf;
            setR(op & 7, osz, alu(5, getR(op & 7, osz), 1, osz));
            cf = c;
            break;
        }
        case 0x50: case 0x51: case 0x52: case 0x53: case 0x54: case 0x55: case 0x56: case 0x57: push(r[op & 7]); break;
        case 0x58: case 0x59: case 0x5A: case 0x5B: case 0x5C: case 0x5D: case 0x5E: case 0x5F: r[op & 7] = pop(); break;
        case 0x60: {  // pushad
            const u32 sp = r[ESP];
            for (int i = 0; i < 8; i++) push(i == ESP ? sp : r[i]);
            break;
        }
        case 0x61:
            for (int i = 7; i >= 0; i--) {
                const u32 v = pop();
                if (i != ESP) r[i] = v;
            }
            break;
        case 0x68: push(f32()); break;
        case 0x6A: push(u32(s32(s8(f8())))); break;
        case 0x69: case 0x6B: {  // imul reg, r/m, imm
            decode();
            const s64 a = osz == 2 ? s16(rdRM(2)) : s32(rdRM(4));
            const s64 b = op == 0x6B ? s8(f8()) : osz == 2 ? s16(f16()) : s32(f32());
            const s64 p = a * b;
            setR(regN(), osz, u32(p));
            cf = of = osz == 2 ? p != s16(p) : p != s32(p);
            break;
        }
        case 0x70: case 0x71: case 0x72: case 0x73: case 0x74: case 0x75: case 0x76: case 0x77:
        case 0x78: case 0x79: case 0x7A: case 0x7B: case 0x7C: case 0x7D: case 0x7E: case 0x7F: {
            const s32 rel = s8(f8());
            if (cond(op & 15)) relJump(rel);
            break;
        }
        case 0x80: case 0x81: case 0x82: case 0x83: {
            const int size = op == 0x81 || op == 0x83 ? osz : 1;
            decode();
            const int o = regN();
            const u32 imm = op == 0x83 ? u32(s32(s8(f8()))) : immN(size);
            const u32 res = alu(o, rdRM(size), imm, size);
            if (o != 7) wrRM(size, res);
            break;
        }
        case 0x84: case 0x85: {
            const int size = op & 1 ? osz : 1;
            decode();
            alu(4, rdRM(size), getR(regN(), size), size);
            break;
        }
        case 0x86: case 0x87: {
            const int size = op & 1 ? osz : 1;
            decode();
            const u32 a = rdRM(size), b = getR(regN(), size);
            wrRM(size, b);
            setR(regN(), size, a);
            break;
        }
        case 0x88: case 0x89: { const int size = op & 1 ? osz : 1; decode(); wrRM(size, getR(regN(), size)); break; }
        case 0x8A: case 0x8B: { const int size = op & 1 ? osz : 1; decode(); setR(regN(), size, rdRM(size)); break; }
        case 0x8D: decode(); setR(regN(), osz, ea - seg); break;
        case 0x8F: { decode(); const u32 v = pop(); wrRM(osz, v); break; }
        case 0x90: break;
        case 0x91: case 0x92: case 0x93: case 0x94: case 0x95: case 0x96: case 0x97: {
            const u32 a = getR(EAX, osz);
            setR(EAX, osz, getR(op & 7, osz));
            setR(op & 7, osz, a);
            break;
        }
        case 0x98:
            if (opsz) setR(EAX, 2, u32(s32(s8(r[EAX]))));
            else r[EAX] = u32(s32(s16(r[EAX])));
            break;
        case 0x99:
            if (opsz) setR(EDX, 2, (r[EAX] & 0x8000) ? 0xFFFF : 0);
            else r[EDX] = (r[EAX] & 0x80000000u) ? 0xFFFFFFFFu : 0;
            break;
        case 0x9B: break;  // fwait
        case 0x9C: push(eflags()); break;
        case 0x9D: setEflags(pop()); break;
        case 0x9E: setEflags((eflags() & ~0xFFu) | ((r[EAX] >> 8) & 0xD5)); break;  // sahf
        case 0x9F: r[EAX] = (r[EAX] & ~0xFF00u) | ((eflags() & 0xD7) << 8); break;   // lahf
        case 0xA0: r[EAX] = (r[EAX] & ~0xFFu) | mem.r8(f32() + seg); break;
        case 0xA1: setR(EAX, osz, osz == 2 ? mem.r16(f32() + seg) : mem.r32(f32() + seg)); break;
        case 0xA2: mem.w8(f32() + seg, u8(r[EAX])); break;
        case 0xA3: { const u32 a = f32() + seg; if (osz == 2) mem.w16(a, u16(r[EAX])); else mem.w32(a, r[EAX]); break; }
        case 0xA4: case 0xA5: case 0xAA: case 0xAB: case 0xAC: case 0xAD: case 0xA6: case 0xA7: case 0xAE: case 0xAF: {
            const int size = (op & 1) ? osz : 1;
            const s32 d = df ? -size : size;
            auto once = [&]() -> bool {  // false: stop a repe/repne
                switch (op) {
                case 0xA4: case 0xA5: {  // movs
                    const u32 v = size == 1 ? mem.r8(r[ESI]) : size == 2 ? mem.r16(r[ESI]) : mem.r32(r[ESI]);
                    if (size == 1) mem.w8(r[EDI], u8(v));
                    else if (size == 2) mem.w16(r[EDI], u16(v));
                    else mem.w32(r[EDI], v);
                    r[ESI] += u32(d), r[EDI] += u32(d);
                    return true;
                }
                case 0xAA: case 0xAB:  // stos
                    if (size == 1) mem.w8(r[EDI], u8(r[EAX]));
                    else if (size == 2) mem.w16(r[EDI], u16(r[EAX]));
                    else mem.w32(r[EDI], r[EAX]);
                    r[EDI] += u32(d);
                    return true;
                case 0xAC: case 0xAD:  // lods
                    setR(EAX, size, size == 1 ? mem.r8(r[ESI]) : size == 2 ? mem.r16(r[ESI]) : mem.r32(r[ESI]));
                    r[ESI] += u32(d);
                    return true;
                case 0xA6: case 0xA7: {  // cmps
                    const u32 a = size == 1 ? mem.r8(r[ESI]) : size == 2 ? mem.r16(r[ESI]) : mem.r32(r[ESI]);
                    const u32 b = size == 1 ? mem.r8(r[EDI]) : size == 2 ? mem.r16(r[EDI]) : mem.r32(r[EDI]);
                    alu(7, a, b, size);
                    r[ESI] += u32(d), r[EDI] += u32(d);
                    return rep ? zf : repne ? !zf : true;
                }
                default: {  // scas
                    const u32 b = size == 1 ? mem.r8(r[EDI]) : size == 2 ? mem.r16(r[EDI]) : mem.r32(r[EDI]);
                    alu(7, getR(EAX, size), b, size);
                    r[EDI] += u32(d);
                    return rep ? zf : repne ? !zf : true;
                }
                }
            };
            if (rep || repne) {
                while (r[ECX]) {
                    r[ECX]--;
                    if (!once()) break;
                }
            } else {
                once();
            }
            break;
        }
        case 0xA8: alu(4, r[EAX], f8(), 1); break;
        case 0xA9: alu(4, getR(EAX, osz), immN(osz), osz); break;
        case 0xB0: case 0xB1: case 0xB2: case 0xB3: case 0xB4: case 0xB5: case 0xB6: case 0xB7: setR8(op & 7, f8()); break;
        case 0xB8: case 0xB9: case 0xBA: case 0xBB: case 0xBC: case 0xBD: case 0xBE: case 0xBF: setR(op & 7, osz, immN(osz)); break;
        case 0xC0: case 0xC1: case 0xD0: case 0xD1: case 0xD2: case 0xD3: {
            const int size = op & 1 ? osz : 1;
            decode();
            const u32 count = (op == 0xC0 || op == 0xC1) ? f8() : (op == 0xD0 || op == 0xD1) ? 1 : (r[ECX] & 0xFF);
            wrRM(size, shift(regN(), rdRM(size), count, size));
            break;
        }
        case 0xC2: { const u16 n = f16(); eip = pop(); r[ESP] += n; break; }
        case 0xC3: eip = pop(); break;
        case 0xC6: case 0xC7: {
            const int size = op & 1 ? osz : 1;
            decode();
            wrRM(size, immN(size));
            break;
        }
        case 0xC8: {  // enter
            const u16 n = f16();
            f8();
            push(r[EBP]);
            r[EBP] = r[ESP];
            r[ESP] -= n;
            break;
        }
        case 0xC9: r[ESP] = r[EBP]; r[EBP] = pop(); break;
        case 0xCC: unimpl("int3 (breakpoint in mod code)"); break;
        case 0xCD: unimpl("int"); break;
        case 0xE8: { const s32 rel = s32(f32()); push(eip); relJump(rel); break; }
        case 0xE9: { const s32 rel = s32(f32()); relJump(rel); break; }
        case 0xEB: { const s32 rel = s8(f8()); relJump(rel); break; }
        case 0xE3: { const s32 rel = s8(f8()); if (!r[ECX]) relJump(rel); break; }  // jecxz
        case 0xF5: cf = !cf; break;
        case 0xF8: cf = false; break;
        case 0xF9: cf = true; break;
        case 0xFC: df = false; break;
        case 0xFD: df = true; break;
        case 0xF6: case 0xF7: {
            const int size = op & 1 ? osz : 1;
            decode();
            const int o = regN();
            switch (o) {
            case 0: case 1: alu(4, rdRM(size), immN(size), size); break;
            case 2: wrRM(size, ~rdRM(size)); break;
            case 3: { const u32 v = rdRM(size); const u32 res = alu(5, 0, v, size); wrRM(size, res); cf = v != 0; break; }
            case 4: case 5: {  // mul / imul (one operand)
                const u32 v = rdRM(size);
                if (size == 1) {
                    const u32 p = o == 4 ? (r[EAX] & 0xFF) * v : u32(s32(s8(r[EAX])) * s32(s8(v)));
                    setR(EAX, 2, p);
                    cf = of = o == 4 ? (p >> 8) != 0 : s16(p) != s8(p);
                } else if (size == 2) {
                    const u32 p = o == 4 ? (r[EAX] & 0xFFFF) * v : u32(s32(s16(r[EAX])) * s32(s16(v)));
                    setR(EAX, 2, p), setR(EDX, 2, p >> 16);
                    cf = of = o == 4 ? (p >> 16) != 0 : s32(p) != s16(p);
                } else {
                    const u64 p = o == 4 ? u64(r[EAX]) * v : u64(s64(s32(r[EAX])) * s64(s32(v)));
                    r[EAX] = u32(p), r[EDX] = u32(p >> 32);
                    cf = of = o == 4 ? r[EDX] != 0 : s64(p) != s64(s32(p));
                }
                break;
            }
            case 6: case 7: {  // div / idiv
                const u32 v = rdRM(size);
                if (!v) unimpl("divide by zero");
                if (size == 4) {
                    const u64 n = u64(r[EDX]) << 32 | r[EAX];
                    if (o == 6) r[EAX] = u32(n / v), r[EDX] = u32(n % v);
                    else r[EAX] = u32(s64(n) / s32(v)), r[EDX] = u32(s64(n) % s32(v));
                } else if (size == 2) {
                    const u32 n = (r[EDX] & 0xFFFF) << 16 | (r[EAX] & 0xFFFF);
                    if (o == 6) setR(EAX, 2, n / v), setR(EDX, 2, n % v);
                    else setR(EAX, 2, u32(s32(n) / s16(v))), setR(EDX, 2, u32(s32(n) % s16(v)));
                } else {
                    const u32 n = r[EAX] & 0xFFFF;
                    if (o == 6) setR8(0, u8(n / v)), setR8(4, u8(n % v));
                    else setR8(0, u8(s16(n) / s8(v))), setR8(4, u8(s16(n) % s8(v)));
                }
                break;
            }
            }
            break;
        }
        case 0xFE: case 0xFF: {
            const int size = op & 1 ? osz : 1;
            decode();
            const int o = regN();
            switch (o) {
            case 0: case 1: { const bool c = cf; wrRM(size, alu(o == 0 ? 0 : 5, rdRM(size), 1, size)); cf = c; break; }
            case 2: { const u32 t = rdRM(4); push(eip); eip = t; break; }
            case 4: eip = rdRM(4); break;
            case 6: push(rdRM(4)); break;
            default: unimpl("FF group");
            }
            break;
        }
        // ---- x87 ----
        case 0xD8: case 0xD9: case 0xDA: case 0xDB: case 0xDC: case 0xDD: case 0xDE: case 0xDF: {
            decode();
            const int o = regN();
            auto arith = [&](int k, double& d, double s) {  // fadd fmul fcom fcomp fsub fsubr fdiv fdivr
                switch (k) {
                case 0: d = d + s; break;
                case 1: d = d * s; break;
                case 4: d = d - s; break;
                case 5: d = s - d; break;
                case 6: d = d / s; break;
                case 7: d = s / d; break;
                }
            };
            if (!isReg) {
                switch (op) {
                case 0xD8: case 0xDC: case 0xDA: case 0xDE: {  // m32fp / m64fp / m32int / m16int
                    double s;
                    if (op == 0xD8) { u32 b = mem.r32(ea); float f; std::memcpy(&f, &b, 4); s = f; }
                    else if (op == 0xDC) { u64 b = mem.r64(ea); std::memcpy(&s, &b, 8); }
                    else if (op == 0xDA) s = double(s32(mem.r32(ea)));
                    else s = double(s16(mem.r16(ea)));
                    if (o == 2 || o == 3) { fcomFlags(ST(0), s); if (o == 3) fpop(); }
                    else arith(o, ST(0), s);
                    break;
                }
                case 0xD9:
                    if (o == 0) { u32 b = mem.r32(ea); float f; std::memcpy(&f, &b, 4); fpush(f); }
                    else if (o == 2 || o == 3) { const float f = float(ST(0)); u32 b; std::memcpy(&b, &f, 4); mem.w32(ea, b); if (o == 3) fpop(); }
                    else if (o == 5) fpuCw = mem.r16(ea);
                    else if (o == 7) mem.w16(ea, fpuCw);
                    else if (o == 4 || o == 6) {}  // fldenv/fnstenv: ignored
                    else unimpl("D9 mem");
                    break;
                case 0xDB:
                    if (o == 0) fpush(double(s32(mem.r32(ea))));
                    else if (o == 1) { mem.w32(ea, toInt32(std::trunc(ST(0)))); fpop(); }  // fisttp
                    else if (o == 2 || o == 3) { mem.w32(ea, toInt32(roundFpu(ST(0)))); if (o == 3) fpop(); }
                    else if (o == 5) {  // fld m80
                        u8 b[10];
                        mem.read(ea, b, 10);
                        u64 mant; std::memcpy(&mant, b, 8);
                        const u16 se = u16(b[8] | b[9] << 8);
                        const int e = (se & 0x7FFF) - 16383;
                        double v = std::ldexp(double(mant) / 9223372036854775808.0, e);
                        fpush((se & 0x8000) ? -v : v);
                    } else if (o == 7) {  // fstp m80
                        const double v = fpop();
                        int e;
                        const double m = std::frexp(std::fabs(v), &e);
                        const u64 mant = v == 0 ? 0 : u64(m * 18446744073709551616.0);
                        const u16 se = u16((v == 0 ? 0 : (e - 1 + 16383)) | (std::signbit(v) ? 0x8000 : 0));
                        u8 b[10];
                        std::memcpy(b, &mant, 8);
                        b[8] = u8(se), b[9] = u8(se >> 8);
                        mem.write(ea, b, 10);
                    } else unimpl("DB mem");
                    break;
                case 0xDD:
                    if (o == 0) { u64 b = mem.r64(ea); double d; std::memcpy(&d, &b, 8); fpush(d); }
                    else if (o == 1) { const double v = std::trunc(fpop()); const s64 i = s64(v); mem.w64(ea, u64(i)); }
                    else if (o == 2 || o == 3) { u64 b; const double d = ST(0); std::memcpy(&b, &d, 8); mem.w64(ea, b); if (o == 3) fpop(); }
                    else if (o == 7) mem.w16(ea, statusWord());
                    else if (o == 4 || o == 6) {}  // frstor/fnsave: ignored
                    else unimpl("DD mem");
                    break;
                case 0xDF:
                    if (o == 0) fpush(double(s16(mem.r16(ea))));
                    else if (o == 5) fpush(double(s64(mem.r64(ea))));
                    else if (o == 7) { mem.w64(ea, u64(s64(roundFpu(fpop())))); }
                    else if (o == 1) { mem.w16(ea, u16(s16(std::trunc(fpop())))); }
                    else if (o == 2 || o == 3) { mem.w16(ea, u16(s16(roundFpu(ST(0))))); if (o == 3) fpop(); }
                    else if (o == 4) unimpl("fbld");
                    else unimpl("DF mem");
                    break;
                }
                break;
            }
            const int i = rmN();
            switch (op) {
            case 0xD8:
                if (o == 2 || o == 3) { fcomFlags(ST(0), ST(i)); if (o == 3) fpop(); }
                else arith(o, ST(0), ST(i));
                break;
            case 0xDC:  // st(i) = st(i) op st(0)  (sub/div operands reversed vs. D8)
                if (o == 2 || o == 3) { fcomFlags(ST(0), ST(i)); if (o == 3) fpop(); }
                else arith(o == 4 ? 5 : o == 5 ? 4 : o == 6 ? 7 : o == 7 ? 6 : o, ST(i), ST(0));
                break;
            case 0xDE:
                if (o == 3 && i == 1) { fcomFlags(ST(0), ST(1)); fpop(); fpop(); }  // fcompp
                else { arith(o == 4 ? 5 : o == 5 ? 4 : o == 6 ? 7 : o == 7 ? 6 : o, ST(i), ST(0)); fpop(); }
                break;
            case 0xD9:
                if (o == 0) fpush(ST(i));
                else if (o == 1) std::swap(ST(0), ST(i));
                else if (mrm == 0xD0) {}  // fnop
                else if (mrm == 0xE0) ST(0) = -ST(0);
                else if (mrm == 0xE1) ST(0) = std::fabs(ST(0));
                else if (mrm == 0xE4) fcomFlags(ST(0), 0.0);
                else if (mrm == 0xE5) { fpuSw &= ~0x4700; if (std::isnan(ST(0))) fpuSw |= 0x0100; else fpuSw |= 0x0400; }
                else if (mrm == 0xE8) fpush(1.0);
                else if (mrm == 0xE9) fpush(3.321928094887362);
                else if (mrm == 0xEA) fpush(1.4426950408889634);
                else if (mrm == 0xEB) fpush(3.141592653589793);
                else if (mrm == 0xEC) fpush(0.30102999566398120);
                else if (mrm == 0xED) fpush(0.6931471805599453);
                else if (mrm == 0xEE) fpush(0.0);
                else if (mrm == 0xF0) ST(0) = std::exp2(ST(0)) - 1;
                else if (mrm == 0xF1) { const double y = ST(1) * std::log2(ST(0)); fpop(); ST(0) = y; }
                else if (mrm == 0xF2) { ST(0) = std::tan(ST(0)); fpush(1.0); }
                else if (mrm == 0xF3) { const double y = std::atan2(ST(1), ST(0)); fpop(); ST(0) = y; }
                else if (mrm == 0xF8) ST(0) = std::fmod(ST(0), ST(1));
                else if (mrm == 0xF5) ST(0) = std::remainder(ST(0), ST(1));
                else if (mrm == 0xFA) ST(0) = std::sqrt(ST(0));
                else if (mrm == 0xFB) { const double v = ST(0); ST(0) = std::sin(v); fpush(std::cos(v)); }
                else if (mrm == 0xFC) ST(0) = roundFpu(ST(0));
                else if (mrm == 0xFD) ST(0) = std::ldexp(ST(0), int(std::trunc(ST(1))));
                else if (mrm == 0xFE) ST(0) = std::sin(ST(0));
                else if (mrm == 0xFF) ST(0) = std::cos(ST(0));
                else if (mrm == 0xF6) top = (top - 1) & 7;
                else if (mrm == 0xF7) top = (top + 1) & 7;
                else unimpl("D9 reg");
                break;
            case 0xDA:
                if (mrm == 0xE9) { fcomFlags(ST(0), ST(1)); fpop(); fpop(); }  // fucompp
                else {  // fcmovb/e/be/u
                    const bool c = o == 0 ? cf : o == 1 ? zf : o == 2 ? (cf || zf) : pf;
                    if (c) ST(0) = ST(i);
                }
                break;
            case 0xDB:
                if (mrm == 0xE2) fpuSw &= 0x7F00;    // fnclex
                else if (mrm == 0xE3) fpuCw = 0x37F, fpuSw = 0, top = 0, fpuTags = 0xFF;  // fninit
                else if (o == 5 || o == 6) fcomiFlags(ST(0), ST(i));  // fucomi/fcomi
                else {  // fcmovnb/ne/nbe/nu
                    const bool c = o == 0 ? !cf : o == 1 ? !zf : o == 2 ? !(cf || zf) : !pf;
                    if (c) ST(0) = ST(i);
                }
                break;
            case 0xDD:
                if (o == 0) fpuTags |= 1 << ((top + i) & 7);  // ffree
                else if (o == 2) ST(i) = ST(0);
                else if (o == 3) { ST(i) = ST(0); fpop(); }
                else if (o == 4 || o == 5) { fcomFlags(ST(0), ST(i)); if (o == 5) fpop(); }
                else unimpl("DD reg");
                break;
            case 0xDF:
                if (mrm == 0xE0) r[EAX] = (r[EAX] & 0xFFFF0000u) | statusWord();  // fnstsw ax
                else if (o == 5 || o == 6) { fcomiFlags(ST(0), ST(i)); fpop(); }  // fucomip/fcomip
                else unimpl("DF reg");
                break;
            }
            break;
        }
        default: unimpl("opcode");
        }
    } catch (const Unimplemented& e) {
        rt::log("[modhost] unimplemented instruction: %s; %s\n", e.what(), where().c_str());
        throw;
    }
}

}  // namespace mh
