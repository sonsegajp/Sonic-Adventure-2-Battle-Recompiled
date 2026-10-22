// x86-32 interpreter for PC mod code (the DLLs SA Mod Manager loads into the PC version). Portable
// C++: mods run the same wherever the game is built. The mod's 4 GB address space holds its images,
// heap and stack in host memory; two windows reach the game:
//   0x80000000-0x81FFFFFF  the GameCube's memory (big-endian; accesses are byte-swapped by size)
//   0x00400000-0x02FFFFFF  the PC executable's image, which does not exist here: reads, writes,
//                          jumps and calls into it go to the bridge (src/modhost/bridge.cpp), which
//                          translates them to the GameCube game (data, functions, code patches)
// Native code (Windows/C runtime stand-ins, the bridge) is reached through thunk addresses.
#pragma once

#include <cstdint>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace mh {

using u8 = uint8_t;
using u16 = uint16_t;
using u32 = uint32_t;
using u64 = uint64_t;
using s8 = int8_t;
using s16 = int16_t;
using s32 = int32_t;
using s64 = int64_t;

constexpr u32 kGcBase = 0x80000000, kGcEnd = 0x82000000;
constexpr u32 kPcImageBase = 0x00400000, kPcImageEnd = 0x03000000;
constexpr u32 kThunkBase = 0xFFF00000;  // native entry points, 16 bytes apart
constexpr u32 kReturnSentinel = 0xFFFFFFF0;

struct Cpu;

// Mod code did something the host cannot go on with (an instruction it does not know, abort(), an
// uncaught exception...): the mod code is stopped where the host entered it, the game goes on.
struct ModFault : std::runtime_error {
    using std::runtime_error::runtime_error;
};

// Whatever lies outside host memory: the GameCube window, the PC image and the thunks.
struct Foreign {
    virtual ~Foreign() = default;
    virtual u64 read(u32 addr, int size) = 0;              // PC image reads
    virtual void write(u32 addr, u64 v, int size) = 0;     // PC image writes (code patches...)
    virtual void execPc(Cpu& cpu, u32 addr) = 0;           // eip entered the PC image (a game function)
    virtual void execGc(Cpu& cpu, u32 addr) = 0;           // eip entered the GameCube window (a function of the game's)
    virtual void execThunk(Cpu& cpu, u32 index) = 0;       // eip entered a native thunk
    // Pointers crossing the GameCube window (aligned 32-bit values): what mod code stores there
    // (a function of its own becomes one the game can call) and what it loads.
    virtual u32 toGc(u32 v) = 0;
    virtual u32 fromGc(u32 v) = 0;
};

class Memory {
public:
    Memory();
    // Host-backed memory (mod images, heap, stack). Pages are 4 KB, zero-filled.
    void map(u32 addr, u32 size);
    bool mapped(u32 addr) const { return pages_[addr >> 12] != nullptr; }
    u8* host(u32 addr) { return pages_[addr >> 12] ? pages_[addr >> 12].get() + (addr & 0xFFF) : nullptr; }

    u8 r8(u32 a);
    u16 r16(u32 a);
    u32 r32(u32 a);
    u64 r64(u32 a);
    void w8(u32 a, u8 v);
    void w16(u32 a, u16 v);
    void w32(u32 a, u32 v);
    void w64(u32 a, u64 v);
    void read(u32 a, void* dst, u32 n);
    void write(u32 a, const void* src, u32 n);
    std::string cstr(u32 a, size_t max = 4096);
    std::u16string wstr(u32 a, size_t max = 4096);

    Foreign* foreign = nullptr;

private:
    u64 slow(u32 a, int size);
    void slowWrite(u32 a, u64 v, int size);
    std::vector<std::unique_ptr<u8[]>> pages_;
};

union Xmm {
    u8 b[16];
    u16 w[8];
    u32 d[4];
    u64 q[2];
    float f[4];
    double f64[2];
};

// The registers (everything but the memory): saved around calls into the game, which may run
// other mod code.
struct CpuRegs {
    u32 r[8]{};
    u32 eip = 0;
    // Flags kept as separate booleans; packed for pushfd/lahf.
    bool cf = false, pf = false, af = false, zf = false, sf = false, of = false, df = false;
    u32 fsBase = 0;
    // x87 (64-bit doubles: mod code only needs float/double precision)
    double st[8]{};
    int top = 0;
    u16 fpuCw = 0x27F, fpuSw = 0;
    u8 fpuTags = 0xFF;  // 1 bit per physical register: empty
    Xmm xmm[8]{};
    u32 mxcsr = 0x1F80;
};

struct Cpu : CpuRegs {
    enum { EAX, ECX, EDX, EBX, ESP, EBP, ESI, EDI };
    Memory& mem;
    u64 steps = 0;
    u32 insn = 0;  // the instruction being run (for reports)
    u32 trail[16]{};  // the last control transfers' targets (for reports)
    u32 trailPos = 0;

    explicit Cpu(Memory& m) : mem(m) {}
    CpuRegs save() const { return *this; }
    void restore(const CpuRegs& s) { static_cast<CpuRegs&>(*this) = s; }

    u32 eflags() const;
    void setEflags(u32 v);
    void push(u32 v) { r[ESP] -= 4, mem.w32(r[ESP], v); }
    u32 pop() { const u32 v = mem.r32(r[ESP]); r[ESP] += 4; return v; }
    u32 arg(int n) { return mem.r32(r[ESP] + 4 + 4 * n); }  // at a native thunk: n-th stack argument
    // Native thunk return: pops the return address and `argBytes` (stdcall), sets eip.
    void ret(u32 argBytes = 0) {
        eip = pop();
        r[ESP] += argBytes;
    }
    // x87 stack
    double& ST(int i) { return st[(top + i) & 7]; }
    void fpush(double v) {
        top = (top - 1) & 7;
        st[top] = v;
        fpuTags &= ~(1 << top);
    }
    double fpop() {
        const double v = st[top];
        fpuTags |= 1 << top;
        top = (top + 1) & 7;
        return v;
    }

    // Runs code at `entry` as a cdecl/stdcall call with these stack arguments until it returns;
    // returns eax. Reentrant (game code it calls may call back into mod code).
    u32 call(u32 entry, std::initializer_list<u32> args = {});
    // Called every 2^20 instructions of a long run (lets other mod threads in).
    static inline void (*onSlice)() = nullptr;
    void step();
    std::string where() const;
};

}  // namespace mh
