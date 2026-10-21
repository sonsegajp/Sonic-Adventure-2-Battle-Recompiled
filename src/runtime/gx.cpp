// GX bridge: the game's own (recompiled) GX library writes the hardware command stream to the
// write-gather pipe (0xCC008000). This file parses that stream command by command and hands it to
// Aurora's command processor, rewriting the commands that carry console addresses into Aurora's
// pointer-carrying GX_AURORA forms:
//   CP 0xA0-0xAF array bases      -> GX_AURORA_LOAD_ARRAYBASE (host pointer into guest memory)
//   BP image3 (texture address)   -> GX_AURORA_LOAD_TEXOBJ (content-hashed by Aurora)
//   BP 0x64/0x65 TLUT loads       -> GX_AURORA_LOAD_TLUT
//   BP 0x49/0x4A/0x4B/0x4D + 0x52 -> GX_AURORA_LOAD_COPY_SRC/DST/DEST + the copy
//   CALL_DL                       -> the display list's commands, inline
// Draw-done (BP 0x45) and token (BP 0x47/0x48) raise the PE interrupts as soon as they are written.
// While the CPU FIFO is redirected away from the GP FIFO (GXBeginDisplayList), writes go to guest
// memory at the PI write pointer instead, like the hardware.
// Frames are handed to the main (render) thread at VISetNextFrameBuffer, at most one frame ahead.
#include "runtime/gx.h"
#include "runtime/runtime.h"
#include "runtime/sched.h"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <set>
#include <vector>

namespace rt {

extern std::atomic<u64> g_relLinkFrame[256];  // src/runtime/dispatch.cpp
u32 mmioGet16(u32 off);
void mmioSet16(u32 off, u32 v);

namespace {
enum : u8 {
    CMD_NOP = 0x00,
    CMD_LOAD_CP = 0x08,
    CMD_LOAD_XF = 0x10,
    CMD_INDX_A = 0x20,
    CMD_INDX_B = 0x28,
    CMD_INDX_C = 0x30,
    CMD_INDX_D = 0x38,
    CMD_CALL_DL = 0x40,
    CMD_METRICS = 0x44,
    CMD_INVAL_VC = 0x48,
    CMD_AURORA = 0x50,
    CMD_LOAD_BP = 0x61,
};
enum : u16 {
    AUR_ARRAYBASE = 0x0010,
    AUR_LOAD_TEXOBJ = 0x0030,
    AUR_LOAD_TLUT = 0x0031,
    AUR_COPY_SRC = 0x0035,
    AUR_COPY_DST = 0x0036,
    AUR_COPY_DEST = 0x0037,
};
enum : u32 { IRQ_PE_TOKEN = 18, IRQ_PE_FINISH = 19 };

// ---- output stream (Aurora format, big-endian) ----
std::vector<u8> s_out;
void put8(u32 v) { s_out.push_back(u8(v)); }
void put16(u32 v) {
    put8(v >> 8);
    put8(v);
}
void put32(u32 v) {
    put16(v >> 16);
    put16(v);
}
void put64(u64 v) {
    put32(u32(v >> 32));
    put32(u32(v));
}
void putBytes(const u8* p, size_t n) { s_out.insert(s_out.end(), p, p + n); }
void aurora(u16 sub) {
    put8(CMD_AURORA);
    put16(sub);
}
u64 hostAddr(u32 physAddr) { return u64(reinterpret_cast<uintptr_t>(hostp(0x80000000u | (physAddr & 0x01FFFFFF)))); }

// ---- CP vertex format state (for command lengths) ----
u32 s_vcdLo = 0, s_vcdHi = 0, s_vatA[8], s_vatB[8], s_vatC[8];
const u8 kComp[8] = {1, 1, 2, 2, 4, 0, 0, 0};
const u8 kClr[8] = {2, 3, 4, 2, 3, 4, 0, 0};
u32 vertexSize(u32 fmt) {
    const u32 lo = s_vcdLo, hi = s_vcdHi, a = s_vatA[fmt], b = s_vatB[fmt], c = s_vatC[fmt];
    u32 size = 0;
    for (u32 i = 0; i < 9; i++) size += (lo >> i) & 1;  // position/normal + 8 texture matrix indices
    auto attr = [&](u32 type, u32 direct) { size += type == 1 ? direct : type == 2 ? 1 : type == 3 ? 2 : 0; };
    attr((lo >> 9) & 3, ((a & 1) ? 3 : 2) * kComp[(a >> 1) & 7]);
    if (u32 t = (lo >> 11) & 3) {
        const bool nbt = (a >> 9) & 1;
        if (t == 1) size += (nbt ? 9 : 3) * kComp[(a >> 10) & 7];
        else size += (t == 2 ? 1 : 2) * ((nbt && (a >> 31)) ? 3 : 1);
    }
    attr((lo >> 13) & 3, kClr[(a >> 14) & 7]);
    attr((lo >> 15) & 3, kClr[(a >> 18) & 7]);
    const u32 tex[8][2] = {{(a >> 21) & 1, (a >> 22) & 7}, {b & 1, (b >> 1) & 7},          {(b >> 9) & 1, (b >> 10) & 7},
                           {(b >> 18) & 1, (b >> 19) & 7}, {(b >> 27) & 1, (b >> 28) & 7}, {(c >> 5) & 1, (c >> 6) & 7},
                           {(c >> 14) & 1, (c >> 15) & 7}, {(c >> 23) & 1, (c >> 24) & 7}};
    for (u32 i = 0; i < 8; i++) attr((hi >> (2 * i)) & 3, (tex[i][0] ? 2 : 1) * kComp[tex[i][1]]);
    return size;
}

u32 s_arraysSet = 0;  // CP arrays whose base has been loaded (bit = array index)

// Debugging: SA2_SKIP_DRAWS=i,j,... drops those draws (counted from 0 in every frame).
u32 s_drawIndex = 0;
const std::set<u32> s_skipDraws = [] {
    std::set<u32> s;
    for (const char* p = std::getenv("SA2_SKIP_DRAWS"); p && *p;) {
        char* end;
        s.insert(u32(std::strtoul(p, &end, 10)));
        p = *end == ',' ? end + 1 : end;
        if (end == p && *p) break;
    }
    return s;
}();

// ---- snapshots ----
// Aurora reads array data when the render thread processes the frame, after the game has moved on
// (and, since draw-done is signalled at once, possibly rewritten its per-frame vertex and matrix
// buffers). So the arrays a draw uses are copied when the draw is issued, into memory that lives
// until the frame has been rendered.
struct Arena {
    std::vector<std::unique_ptr<u8[]>> chunks;
    size_t used = 0, cap = 0;
    u8* alloc(size_t n) {
        n = (n + 31) & ~size_t(31);
        if (used + n > cap) {
            cap = std::max<size_t>(n, size_t(1) << 20);
            chunks.emplace_back(new u8[cap]);
            used = 0;
        }
        u8* p = chunks.back().get() + used;
        used += n;
        return p;
    }
};
Arena s_arena;  // snapshots of the frame being built

// ---- vertex/matrix arrays ----
// The hardware has no array sizes, but Aurora copies each array a draw reads into a per-frame
// storage buffer, so a base is sent only once a draw uses it, as a snapshot of the extent the draws
// have reached so far; it is resent larger (doubling) when a later draw indexes further. Every new
// base and every frame starts over (s_arraySent = 0).
u32 s_arrayBase[16], s_arrayStride[16], s_arraySent[16];

void needArray(u32 i, u32 bytes) {
    if (bytes <= s_arraySent[i]) return;
    const u32 limit = kHostMemSize - (s_arrayBase[i] & 0x01FFFFFF);
    const u32 size = std::min((std::max(bytes, s_arraySent[i] * 2) + 63) & ~63u, limit);
    s_arraySent[i] = size;
    u8* copy = s_arena.alloc(size);
    std::memcpy(copy, hostp(0x80000000u | (s_arrayBase[i] & 0x01FFFFFF)), size);
    aurora(u16(AUR_ARRAYBASE | i));
    put64(u64(reinterpret_cast<uintptr_t>(copy)));
    put32(size);
    put8(0);  // big-endian
}

// Reads the indices of a draw's vertices and makes sure every array they index is sent big enough.
void sizeArrays(const u8* p, u32 count, u32 fmt) {
    const u32 lo = s_vcdLo, hi = s_vcdHi, a = s_vatA[fmt], b = s_vatB[fmt], c = s_vatC[fmt];
    struct Op {
        u8 skip, idxSize, array, elem;
    };
    Op ops[24];
    u32 nops = 0, mtx = 0;
    for (u32 i = 0; i < 9; i++) mtx += (lo >> i) & 1;
    if (mtx) ops[nops++] = {u8(mtx), 0, 0, 0};
    auto attr = [&](u32 type, u32 array, u32 elem, u32 indices = 1) {
        if (type == 1) ops[nops++] = {u8(elem), 0, 0, 0};
        else if (type >= 2)
            for (u32 k = 0; k < indices; k++) ops[nops++] = {0, u8(type == 2 ? 1 : 2), u8(array), u8(elem)};
    };
    attr((lo >> 9) & 3, 0, ((a & 1) ? 3 : 2) * kComp[(a >> 1) & 7]);
    const bool nbt = (a >> 9) & 1, nbt3 = nbt && (a >> 31);
    const u32 nrmType = (lo >> 11) & 3;
    if (nrmType == 1) attr(1, 1, (nbt ? 9 : 3) * kComp[(a >> 10) & 7]);
    else attr(nrmType, 1, (nbt && !nbt3 ? 9 : 3) * kComp[(a >> 10) & 7], nbt3 ? 3 : 1);
    attr((lo >> 13) & 3, 2, kClr[(a >> 14) & 7]);
    attr((lo >> 15) & 3, 3, kClr[(a >> 18) & 7]);
    const u32 tex[8][2] = {{(a >> 21) & 1, (a >> 22) & 7}, {b & 1, (b >> 1) & 7},          {(b >> 9) & 1, (b >> 10) & 7},
                           {(b >> 18) & 1, (b >> 19) & 7}, {(b >> 27) & 1, (b >> 28) & 7}, {(c >> 5) & 1, (c >> 6) & 7},
                           {(c >> 14) & 1, (c >> 15) & 7}, {(c >> 23) & 1, (c >> 24) & 7}};
    for (u32 i = 0; i < 8; i++) attr((hi >> (2 * i)) & 3, 4 + i, (tex[i][0] ? 2 : 1) * kComp[tex[i][1]]);

    static const bool s_check = std::getenv("SA2_GX_CHECK") != nullptr;
    if (s_check) {
        u32 total = 0;
        for (u32 k = 0; k < nops; k++) total += ops[k].skip ? ops[k].skip : ops[k].idxSize;
        static int s_bad = 0;
        if (total != vertexSize(fmt) && s_bad++ < 10)
            log("[gx] layout mismatch: ops %u bytes, vertexSize %u (fmt %u VCD %08X/%08X VAT %08X %08X %08X)\n", total,
                vertexSize(fmt), fmt, lo, hi, a, b, c);
    }
    s32 maxIdx[12];
    u32 elemOf[12] = {};
    for (s32& m : maxIdx) m = -1;
    for (u32 v = 0; v < count; v++) {
        for (u32 k = 0; k < nops; k++) {
            const Op& o = ops[k];
            if (o.skip) {
                p += o.skip;
                continue;
            }
            const u32 idx = o.idxSize == 1 ? p[0] : u32(p[0]) << 8 | p[1];
            p += o.idxSize;
            // An all-ones position index makes the hardware skip the vertex; it reads nothing.
            if (o.array == 0 && idx == (o.idxSize == 1 ? 0xFFu : 0xFFFFu)) continue;
            if (s32(idx) > maxIdx[o.array]) maxIdx[o.array] = s32(idx);
            elemOf[o.array] = o.elem;
        }
    }
    for (u32 i = 0; i < 12; i++)
        if (maxIdx[i] >= 0) needArray(i, u32(maxIdx[i]) * s_arrayStride[i] + elemOf[i]);
}

// Indexed attributes in the current vertex descriptor whose array was never given a base.
u32 missingArrays() {
    u32 need = 0;
    for (u32 i = 0; i < 4; i++)  // position, normal, color0, color1
        if (((s_vcdLo >> (9 + 2 * i)) & 3) >= 2) need |= 1u << i;
    for (u32 i = 0; i < 8; i++)
        if (((s_vcdHi >> (2 * i)) & 3) >= 2) need |= 1u << (4 + i);
    return need & ~s_arraysSet;
}

// ---- BP state the translation needs ----
u32 s_texImage0[8], s_texMode1[8], s_tlutRegion[8];
u32 s_tlutSrc = 0;
u32 s_copySrcTL = 0, s_copySrcWH = 0, s_copyDest = 0, s_copyStride = 0;
u64 s_frames = 0;
u32 s_displayCopies = 0;  // display copies since the last frame handoff

int texMapOf(u32 reg, u32 base) {  // maps 0-3 at base 0x80.., 4-7 at 0xA0..
    if (reg >= 0x80 + base && reg < 0x84 + base) return int(reg - 0x80 - base);
    if (reg >= 0xA0 + base && reg < 0xA4 + base) return int(reg - 0xA0 - base + 4);
    return -1;
}

u32 texBytes(u32 fmt, u32 w, u32 h) {
    // Block sizes (width x height of a 32-byte tile) per GX texture format.
    u32 bw = 8, bh = 8, bytes = 32;
    switch (fmt) {
    case 0x0: case 0x8: case 0xE: bw = 8; bh = 8; break;          // I4, C4, CMPR
    case 0x1: case 0x2: case 0x9: bw = 8; bh = 4; break;          // I8, IA4, C8
    case 0x3: case 0x4: case 0x5: case 0xA: bw = 4; bh = 4; break; // IA8, RGB565, RGB5A3, C14X2
    case 0x6: bw = 4; bh = 4; bytes = 64; break;                   // RGBA8
    default: break;
    }
    return ((w + bw - 1) / bw) * ((h + bh - 1) / bh) * bytes;
}

// TLUT slots: TMEM offsets (512-byte units past 0x80000) of the SDK's default regions, GX_TLUT0-15
// at 0xC0000 + 0x2000*n and GX_BIGTLUT0-3 at 0xE0000 + 0x8000*n.
u32 tlutSlot(u32 tmem) {
    if (tmem >= 0x300) return 16 + (tmem - 0x300) / 0x40;
    return tmem >= 0x200 ? (tmem - 0x200) / 0x10 : 0;
}
struct TlutLoad {
    u32 src = 0, count = 0;
};
TlutLoad s_tluts[20];

void emitTlut(u32 slot, u32 format) {
    if (slot >= 20 || !s_tluts[slot].count) return;
    aurora(AUR_LOAD_TLUT);
    put8(slot);
    put64(hostAddr(s_tluts[slot].src));
    put32(format);
    put16(s_tluts[slot].count);
    put32(0);  // object id 0: content-hashed
    put32(0);
}

void loadTexture(int map, u32 image3) {
    const u32 img0 = s_texImage0[map];
    const u32 w = (img0 & 0x3FF) + 1, h = ((img0 >> 10) & 0x3FF) + 1, fmt = (img0 >> 20) & 0xF;
    const u32 maxLod = (s_texMode1[map] >> 8) & 0xFF;
    const u32 slot = tlutSlot(s_tlutRegion[map] & 0x3FF);
    if (fmt == 0x8 || fmt == 0x9 || fmt == 0xA) emitTlut(slot, (s_tlutRegion[map] >> 10) & 3);  // CI: palette + its format
    aurora(AUR_LOAD_TEXOBJ);
    put8(u32(map));
    put64(hostAddr((image3 & 0x00FFFFFF) << 5));
    put32(w);
    put32(h);
    put32(fmt);
    put32(slot);
    put8(maxLod > 0 ? 1 : 0);
    put32(0);  // object id 0: Aurora caches by content
    put32(0);
    (void)texBytes;
}

// ---- widescreen: which commands draw to the screen, and where 2D draws sit ----
// Aurora (patches/aurora/0004) widens perspective views spanning the screen and keeps everything
// else at its aspect, moved toward the edge it is near. The bridge marks each stretch of commands
// between EFB copies as drawing to the screen (1) or into a texture (0: decided when the copy that
// ends it turns out to be a small texture copy - shadow and reflection passes), and classifies each
// 2D (orthographic) draw by where it lands on the 640-wide logical screen.
enum : u16 { AUR_WIDE_SEGMENT = 0x0060, AUR_WIDE_2D = 0x0061 };
size_t s_segmentAt = SIZE_MAX;  // offset in s_out of the current segment's mode byte
int s_last2D = -1;
u32 s_xf[0x1058];           // XF registers and matrix memory, as the game wrote them
u32 s_cpMtxA = 0;           // CP matrix index A: the default position matrix
u32 s_genMode = 0;          // BP GENMODE (number of textures in bits 0-3)
u32 s_tex0 = 0;             // address of the texture in map 0
std::set<u32> s_copyDests;  // texture copy destinations this frame (screen effects draw from them)

void beginSegment() {
    aurora(AUR_WIDE_SEGMENT);
    s_segmentAt = s_out.size();
    put8(1);
    s_last2D = -1;
}

float xff(u32 a) { return __builtin_bit_cast(float, s_xf[a]); }

// Where a draw lands, for widescreen placement: its screen extent (logical units) and eye-space
// depth range. SA2 draws its 2D (HUD, menus, title art) with Ninja's sprite routines under the
// game's perspective projection: flat quads in screen pixels, at a depth given by the sprite's
// priority, through Ninja's 2D matrix [-a 0 320a 0 | 0 a -240a 0 | 0 0 1 0], which takes pixels at
// any depth to the eye space that projects them back onto those pixels. Such sprites and
// orthographic draws are 2D: full-screen art and backdrop tiles cover the screen, fades stretch,
// the HUD goes to the edges, the rest stays centred. Everything else is 3D, placed by Aurora from
// its viewport.
enum : int { WIDE_CENTRE, WIDE_LEFT, WIDE_RIGHT, WIDE_STRETCH, WIDE_3D, WIDE_COVER };
std::set<float> s_backdropDepths[2];  // depths of backdrop tiles in the previous and this frame
u32 s_zmode = 0;  // BP ZMODE (bit 0: depth test)
u64 s_wideLogFrame = ~0ull, s_wideLogTo = 0;  // one frame, or 2D draws over a range of frames

// The gameplay HUD (score, time, rings, lives, character gauges) is drawn by f_80061CBC, which the
// stage display routine calls from 8001E768 - and, while paused, the paused one from 8001E180.
// Only its sprites move to the screen edges; menus are laid out as one 4:3 composition (panels,
// highlights and text in separate sprites) and stay centred, as on the PC version.
constexpr u32 kHudReturns[] = {0x8001E76C, 0x8001E184};
// The stage display routines (running and paused), seen from the code they call.
constexpr u32 kStageDisplayReturns[] = {0x8001F740, 0x8001FC38};

bool drawingHud(float minx, float maxx) {
    const Ctx* c = sched::currentCtx();
    if (!c) return false;
    u32 sp = c->r[1], chain[12] = {};
    int n = 0;
    bool inStage = false;
    for (; n < 12 && (sp >> 25) == 0x40; n++) {  // the back chain's LR save words
        if (!(sp = rd32(sp))) break;
        const u32 ret = chain[n] = rd32(sp + 4);
        for (u32 r : kHudReturns)
            if (ret == r) return true;
        for (u32 r : kStageDisplayReturns) inStage = inStage || ret == r;
    }
    // Other 2D in a stage (trick ratings, prompts, boss gauges...) stays centred; each distinct
    // drawing path is logged once, so the ones that belong at the edges can be found and added.
    if (inStage && n >= 2) {
        static std::set<u64> s_seen;
        if (s_seen.size() < 64 && s_seen.insert(u64(chain[0]) << 32 | chain[1]).second)
            log("[wide] in-stage 2D at x %.0f..%.0f drawn from %08X %08X %08X %08X\n", minx, maxx, chain[0], chain[1],
                n > 2 ? chain[2] : 0, n > 3 ? chain[3] : 0);
    }
    return false;
}

bool ninja2DMatrix(u32 m) {
    const float a = xff(m + 5);
    if (a == 0.f || std::fabs(xff(m) + a) > 1e-4f * std::fabs(a) || xff(m + 10) != 1.f) return false;
    for (u32 z : {1u, 3u, 4u, 7u, 8u, 9u, 11u})
        if (xff(m + z) != 0.f) return false;
    return std::fabs(xff(m + 2) - 320.f * a) <= 1e-3f * std::fabs(320.f * a) &&
           std::fabs(xff(m + 6) + 240.f * a) <= 1e-3f * std::fabs(240.f * a);
}

void place2D(const u8* p, u32 count, u32 fmt) {
    const u32 lo = s_vcdLo, a = s_vatA[fmt];
    const u32 posType = (lo >> 9) & 3;
    if (!posType) return;
    const bool ortho = s_xf[0x1026] == 1;
    u32 pre = 0;  // matrix index bytes before the position
    for (u32 i = 0; i < 9; i++) pre += (lo >> i) & 1;
    const u32 vsize = vertexSize(fmt);
    const u32 comps = (a & 1) ? 3 : 2, cfmt = (a >> 1) & 7, shift = (a >> 4) & 31;
    const float scale = 1.f / float(1u << shift);
    auto comp = [&](const u8* q, u32 i) -> float {
        switch (cfmt) {
        case 0: return q[i] * scale;
        case 1: return s8(q[i]) * scale;
        case 2: return u16(q[i * 2] << 8 | q[i * 2 + 1]) * scale;
        case 3: return s16(q[i * 2] << 8 | q[i * 2 + 1]) * scale;
        default: return __builtin_bit_cast(float, u32(q[i * 4]) << 24 | u32(q[i * 4 + 1]) << 16 | u32(q[i * 4 + 2]) << 8 | q[i * 4 + 3]);
        }
    };
    const float p0 = xff(0x1020), p1 = xff(0x1021), p2 = xff(0x1022), p3 = xff(0x1023);
    const float sx = xff(0x101A), ox = xff(0x101D) - 342.f, sy = xff(0x101B), oy = xff(0x101E) - 342.f;
    float minx = 1e9f, maxx = -1e9f, miny = 1e9f, maxy = -1e9f, minz = 1e9f, maxz = -1e9f;
    bool ninja2D = true;
    for (u32 v = 0; v < count && v < 256; v++) {
        const u8* vp = p + v * vsize;
        const u32 m = ((lo & 1) ? vp[0] : (s_cpMtxA & 0x3F)) * 4;  // matrix rows are 4 words
        if (m + 11 >= 0x100) continue;
        const u8* pos = vp + pre;
        if (posType >= 2) {
            const u32 idx = posType == 2 ? pos[0] : u32(pos[0]) << 8 | pos[1];
            pos = hostp(0x80000000u | ((s_arrayBase[0] + idx * s_arrayStride[0]) & 0x01FFFFFF));
        }
        ninja2D = ninja2D && ninja2DMatrix(m);
        const float x = comp(pos, 0), y = comp(pos, 1), z = comps == 3 ? comp(pos, 2) : 0.f;
        const float ex = xff(m) * x + xff(m + 1) * y + xff(m + 2) * z + xff(m + 3);
        const float ey = xff(m + 4) * x + xff(m + 5) * y + xff(m + 6) * z + xff(m + 7);
        const float ez = xff(m + 8) * x + xff(m + 9) * y + xff(m + 10) * z + xff(m + 11);
        float ndcX, ndcY;
        if (ortho) ndcX = p0 * ex + p1, ndcY = p2 * ey + p3;
        else if (ez < -1e-6f) ndcX = (p0 * ex + p1 * ez) / -ez, ndcY = (p2 * ey + p3 * ez) / -ez;
        else continue;  // behind the eye
        const float screenX = ndcX * sx + ox, screenY = ndcY * sy + oy;
        minx = std::min(minx, screenX);
        maxx = std::max(maxx, screenX);
        miny = std::min(miny, screenY);
        maxy = std::max(maxy, screenY);
        minz = std::min(minz, ez);
        maxz = std::max(maxz, ez);
    }
    int place = WIDE_3D;
    if (minx <= maxx) {
        const bool flat = maxz - minz <= 1e-3f * std::max(1.f, -minz);
        const bool sprite = ortho || (flat && ninja2D);
        const float w = maxx - minx, h = maxy - miny, cx = (minx + maxx) * 0.5f;
        if (sprite) {
            if (minx <= 16.f && maxx >= 624.f)  // full screen: art fills it, fades and screen effects stretch
                place = (s_genMode & 0xF) && !s_copyDests.count(s_tex0) ? WIDE_COVER : WIDE_STRETCH;
            // Backdrops: the farthest layers (sprite priority, as depth, 5000 or more). Nearer layers
            // carry things placed on them (the stage select's map and its stage icons) and stay
            // centred with the rest.
            else if (minz <= -5000.f && h >= 120.f && minx >= -1.f && maxx <= 641.f && (minx <= 1.f || maxx >= 639.f)) {
                place = WIDE_COVER;  // a tall piece against an edge: a tile of a backdrop
                s_backdropDepths[1].insert(minz);
            } else if (minz <= -5000.f && w >= 64.f && h >= 64.f && (s_backdropDepths[0].count(minz) || s_backdropDepths[1].count(minz)))
                place = WIDE_COVER;  // a tile inside a backdrop: Ninja draws them all at one priority (depth)
            else if (!drawingHud(minx, maxx)) place = WIDE_CENTRE;
            else if (cx < 192.f) place = WIDE_LEFT;
            else if (cx > 448.f) place = WIDE_RIGHT;
            else place = WIDE_CENTRE;
        }
        if (s_frames == s_wideLogFrame || (s_frames >= s_wideLogFrame && s_frames <= s_wideLogTo && flat)) {
            const u32 m = ((lo & 1) ? p[0] : (s_cpMtxA & 0x3F)) * 4;
            std::string chain;  // the guest code drawing it: LR, then the back chain's LR save words
            if (const Ctx* c = sched::currentCtx()) {
                char b[16];
                std::snprintf(b, sizeof b, " %08X", c->lr);
                chain += b;
                u32 sp = c->r[1];
                for (int i = 0; i < 8 && (sp >> 25) == 0x40; i++) {
                    if (!(sp = rd32(sp))) break;
                    std::snprintf(b, sizeof b, " %08X", rd32(sp + 4));
                    chain += b;
                }
            }
            log("[wide] %llu %s draw: x %.0f..%.0f y %.0f..%.0f z %.2f..%.2f, %u verts, mtx %u [%.4g %.4g %.4g %.4g | %.4g %.4g %.4g %.4g | %.4g %.4g %.4g %.4g], zmode %x, tex %u%s -> %d; stack%s\n",
                (unsigned long long)s_frames, ortho ? "ortho" : "persp", minx, maxx, miny, maxy, minz, maxz, count, m / 4,
                xff(m), xff(m + 1), xff(m + 2), xff(m + 3), xff(m + 4), xff(m + 5), xff(m + 6), xff(m + 7), xff(m + 8),
                xff(m + 9), xff(m + 10), xff(m + 11), s_zmode, s_genMode & 0xF, s_copyDests.count(s_tex0) ? " (copy)" : "", place,
                chain.c_str());
        }
    }
    if (place != s_last2D) {
        aurora(AUR_WIDE_2D);
        put8(u8(place));
        s_last2D = place;
    }
}

void translateBp(u32 v) {
    const u32 reg = v >> 24, val = v & 0xFFFFFF;
    int map;
    if (reg == 0x00) s_genMode = val;
    if (reg == 0x40) s_zmode = val;
    if (texMapOf(reg, 0x14) == 0) s_tex0 = (val & 0xFFFFFF) << 5;
    if ((map = texMapOf(reg, 0x08)) >= 0) s_texImage0[map] = val;
    if ((map = texMapOf(reg, 0x04)) >= 0) s_texMode1[map] = val;
    if ((map = texMapOf(reg, 0x18)) >= 0) s_tlutRegion[map] = val;
    if ((map = texMapOf(reg, 0x14)) >= 0) {  // image3: the texture's address -> load it
        put8(CMD_LOAD_BP);
        put32(v);
        loadTexture(map, val);
        return;
    }
    switch (reg) {
    case 0x45:  // PE_DONE: everything before it has been "drawn"
        raiseInterrupt(IRQ_PE_FINISH, [] { mmioSet16(0x100A, mmioGet16(0x100A) | 0x8); });
        break;
    case 0x47:  // PE token (no interrupt)
        mmioSet16(0x100E, val & 0xFFFF);
        break;
    case 0x48:  // PE token with interrupt
        mmioSet16(0x100E, val & 0xFFFF);
        raiseInterrupt(IRQ_PE_TOKEN, [] { mmioSet16(0x100A, mmioGet16(0x100A) | 0x4); });
        break;
    case 0x49: s_copySrcTL = val; break;
    case 0x4A: s_copySrcWH = val; break;
    case 0x4B: s_copyDest = val; break;
    case 0x4D: s_copyStride = val; break;
    case 0x64: s_tlutSrc = val; break;
    case 0x65: {  // TLUT load: from s_tlutSrc in main memory into a TMEM region
        const u32 slot = tlutSlot(val & 0x3FF);
        if (slot < 20) s_tluts[slot] = {(s_tlutSrc & 0x1FFFFF) << 5, ((val >> 10) & 0x7FF) * 16};
        emitTlut(slot, 1);  // RGB565 until a texture names the format
        break;
    }
    case 0x52: {  // EFB copy
        if ((val >> 14) & 1) s_displayCopies++;
        if (!((val >> 14) & 1)) {  // texture copy (display copies just present the EFB)
            aurora(AUR_COPY_SRC);
            put32(s_copySrcTL & 0x3FF);
            put32((s_copySrcTL >> 10) & 0x3FF);
            put32((s_copySrcWH & 0x3FF) + 1);
            put32(((s_copySrcWH >> 10) & 0x3FF) + 1);
            const bool half = (val >> 9) & 1;
            const u32 fmt = ((val >> 4) & 7) | (((val >> 3) & 1) << 3);
            aurora(AUR_COPY_DST);
            put32(((s_copySrcWH & 0x3FF) + 1) >> (half ? 1 : 0));
            put32((((s_copySrcWH >> 10) & 0x3FF) + 1) >> (half ? 1 : 0));
            put32(fmt);
            put8(half ? 1 : 0);
            aurora(AUR_COPY_DEST);
            put64(hostAddr((s_copyDest & 0x1FFFFF) << 5));
            s_copyDests.insert((s_copyDest & 0x1FFFFF) << 5);
            // A small copy ends a render-to-texture pass: its segment keeps plain mapping.
            const u32 w = (s_copySrcWH & 0x3FF) + 1, h = ((s_copySrcWH >> 10) & 0x3FF) + 1;
            if ((w < 400 || h < 300) && s_segmentAt < s_out.size()) s_out[s_segmentAt] = 0;
        }
        put8(CMD_LOAD_BP);
        put32(v);
        beginSegment();
        return;
    }
    default: break;
    }
    put8(CMD_LOAD_BP);
    put32(v);
}

void translateCp(u32 reg, u32 v) {
    if (reg == 0x50) s_vcdLo = v;
    else if (reg == 0x60) s_vcdHi = v;
    else if (reg >= 0x70 && reg < 0x78) s_vatA[reg & 7] = v;
    else if (reg >= 0x80 && reg < 0x88) s_vatB[reg & 7] = v;
    else if (reg >= 0x90 && reg < 0x98) s_vatC[reg & 7] = v;
    else if (reg == 0x30) s_cpMtxA = v;
    if (reg >= 0xA0 && reg < 0xB0) {  // array base: physical address, sent when a draw uses it
        s_arraysSet |= 1u << (reg & 0xF);
        s_arrayBase[reg & 0xF] = v;
        s_arraySent[reg & 0xF] = 0;
        return;
    }
    if (reg >= 0xB0 && reg < 0xC0) {  // array stride
        s_arrayStride[reg & 0xF] = v & 0xFF;
        s_arraySent[reg & 0xF] = 0;
    }
    put8(CMD_LOAD_CP);
    put8(reg);
    put32(v);
}

u32 be32(const u8* p) { return u32(p[0]) << 24 | u32(p[1]) << 16 | u32(p[2]) << 8 | p[3]; }
u32 be16(const u8* p) { return u32(p[0]) << 8 | p[1]; }

// Translates whole commands from [p, p+n); returns the bytes consumed (a partial command stays).
size_t parse(const u8* p, size_t n, int depth);

size_t command(const u8* p, size_t n, int depth) {
    if (n < 1) return 0;
    const u8 op = p[0];
    switch (op) {
    case CMD_NOP:
    case CMD_INVAL_VC:
    case CMD_METRICS:
        put8(op);
        return 1;
    case CMD_LOAD_CP:
        if (n < 6) return 0;
        translateCp(p[1], be32(p + 2));
        return 6;
    case CMD_LOAD_XF: {
        if (n < 5) return 0;
        const u32 count = (be16(p + 1)) + 1;
        const size_t len = 5 + 4 * size_t(count);
        if (n < len) return 0;
        const u32 addr = be16(p + 3);
        for (u32 i = 0; i < count; i++)
            if (addr + i < 0x1058) s_xf[addr + i] = be32(p + 5 + i * 4);
        putBytes(p, len);
        return len;
    }
    case CMD_INDX_A: case CMD_INDX_B: case CMD_INDX_C: case CMD_INDX_D: {
        // Indexed XF load: (index, length-1 << 12 | XF address) from arrays 12-15.
        if (n < 5) return 0;
        const u32 array = 12 + (op - CMD_INDX_A) / 8;
        needArray(array, be16(p + 1) * s_arrayStride[array] + ((be16(p + 3) >> 12) + 1) * 4);
        const u32 src = s_arrayBase[array] + be16(p + 1) * s_arrayStride[array], xa = be16(p + 3) & 0xFFF;
        for (u32 i = 0; i <= (be16(p + 3) >> 12u); i++)
            if (xa + i < 0x1058) s_xf[xa + i] = rd32(0x80000000u | ((src + i * 4) & 0x01FFFFFF));
        putBytes(p, 5);
        return 5;
    }
    case CMD_CALL_DL: {
        if (n < 9) return 0;
        const u32 addr = be32(p + 1), size = be32(p + 5);
        if (depth < 4) parse(hostp(0x80000000u | (addr & 0x01FFFFFF)), size, depth + 1);
        return 9;
    }
    case CMD_LOAD_BP:
        if (n < 5) return 0;
        translateBp(be32(p + 1));
        return 5;
    default:
        if (op >= 0x80 && op < 0xC0) {
            if (n < 3) return 0;
            if (const u32 miss = missingArrays()) {
                static int s_warned = 0;
                if (s_warned++ < 5) log("[gx] draw %02X uses arrays %03X with no base (VCD %08X/%08X)\n", op, miss, s_vcdLo, s_vcdHi);
            }
            const u32 count = be16(p + 1);
            const size_t len = 3 + size_t(count) * vertexSize(op & 7);
            if (n < len) return 0;
            if (s_skipDraws.count(s_drawIndex++)) return len;  // SA2_SKIP_DRAWS (debugging)
            // Too few vertices for one primitive draws nothing on the hardware. The game's model
            // code emits such draws (e.g. empty triangle strips); Aurora would turn them into a
            // bogus strip that the next draw merges into, drawing its triangles as a strip.
            const u32 prim = op & 0xF8;
            const u32 minCount = prim == 0x80 || prim == 0x88 ? 4 : prim >= 0xA8 && prim < 0xB8 ? 2 : prim == 0xB8 ? 1 : 3;
            if (count < minCount) return len;
            sizeArrays(p + 3, be16(p + 1), op & 7);
            place2D(p + 3, count, op & 7);
            putBytes(p, len);
            return len;
        }
        static int s_bad = 0;
        if (s_bad++ < 8) log("[gx] unknown command %02X (depth %d)\n", op, depth);
        return n;  // resync impossible: drop the rest
    }
}

size_t parse(const u8* p, size_t n, int depth) {
    size_t done = 0;
    while (done < n) {
        const size_t c = command(p + done, n - done, depth);
        if (!c) break;
        done += c;
    }
    return done;
}

// ---- write-gather pipe input ----
std::vector<u8> s_in;
size_t s_inPos = 0;
u32 s_gpFifoBase = 0;  // CP FIFO base: writes go to the GPU while the CPU FIFO is the same one

bool redirectedToMemory() {
    const u32 piBase = rd32(0xCC00300C) & 0x03FFFFFF;
    return s_gpFifoBase && piBase != s_gpFifoBase;
}

// ---- frame handoff to the render thread ----
std::mutex s_frameLock;
std::condition_variable s_frameCv;
std::vector<u8> s_ready;
std::vector<std::unique_ptr<u8[]>> s_readySnapshots;
bool s_haveReady = false;
u32 s_readyFrames = 0;  // game frames in the pending (merged) frame
}  // namespace

void gxWrite(u32 v, int size) {
    if (redirectedToMemory()) {  // display list recording: the CPU FIFO points into RAM
        u32 wp = rd32(0xCC003014) & 0x03FFFFFF;
        for (int i = size - 1; i >= 0; i--) *hostp(0x80000000u | (wp++ & 0x01FFFFFF)) = u8(v >> (8 * i));
        const u32 end = rd32(0xCC003010) & 0x03FFFFFF;
        if (wp >= end && end) wp |= 0x04000000;  // overflow ("wrap") flag
        wr32(0xCC003014, wp);
        return;
    }
    for (int i = size - 1; i >= 0; i--) s_in.push_back(u8(v >> (8 * i)));
    s_inPos += parse(s_in.data() + s_inPos, s_in.size() - s_inPos, 0);
    if (s_inPos > (1u << 16)) {
        s_in.erase(s_in.begin(), s_in.begin() + ptrdiff_t(s_inPos));
        s_inPos = 0;
    }
}

void gxNoteCpRegister(u32 off, u32 v) {
    // CP FIFO base lo/hi (0xCC000020/22): remember which buffer the GP reads.
    if (off == 0x20) s_gpFifoBase = (s_gpFifoBase & 0xFFFF0000) | (v & 0xFFFF);
    else if (off == 0x22) s_gpFifoBase = (s_gpFifoBase & 0xFFFF) | ((v & 0x3FF) << 16);
}

void gxEndFrame() {
    s_frames++;
    if (s_out.size() > (2u << 20) || s_displayCopies > 1)
        log("[gx] frame %llu: %zu bytes, %u display copies\n", (unsigned long long)s_frames, s_out.size(), s_displayCopies);
    s_displayCopies = 0;
    std::unique_lock lk(s_frameLock);
    // The game never waits for the display: it runs at the console's 60 Hz whatever the monitor
    // does (some of its timing follows the music clock, some the frame count; holding frames back
    // pulled them apart in cutscenes). Only when the renderer has fallen several frames behind
    // (a stall, a minimised window) is the game held, briefly, with the guest CPU released.
    if (s_haveReady && s_readyFrames >= 4) {
        lk.unlock();
        sched::withCpuReleased([&] {
            std::unique_lock w(s_frameLock);
            s_frameCv.wait_for(w, std::chrono::milliseconds(50), [] { return !s_haveReady; });
        });
        lk.lock();
    }
    // Never drop a frame's commands: they carry state (GXInit's registers, vertex formats...) later
    // frames rely on. A frame the renderer has not taken yet is extended instead.
    if (s_haveReady) {
        s_ready.insert(s_ready.end(), s_out.begin(), s_out.end());
        s_readyFrames++;
    } else {
        s_ready.swap(s_out);
        s_readyFrames = 1;
    }
    s_out.clear();
    for (auto& chunk : s_arena.chunks) s_readySnapshots.push_back(std::move(chunk));
    s_arena = {};
    std::fill(std::begin(s_arraySent), std::end(s_arraySent), 0u);  // next frame snapshots afresh
    s_drawIndex = 0;
    s_copyDests.clear();
    s_backdropDepths[0].swap(s_backdropDepths[1]);
    s_backdropDepths[1].clear();
    beginSegment();  // the next frame starts drawing to the screen
    // SA2_WIDE_LOG=<game frame> or r<module>:<frames after it linked>: log that frame's placements;
    // <from>-<to>: those frames' flat draws (2D candidates).
    if (const char* wl = std::getenv("SA2_WIDE_LOG")) {
        if (wl[0] == 'r') {
            const u32 mod = u32(std::atoi(wl + 1));
            const char* c = std::strchr(wl, ':');
            const u64 link = g_relLinkFrame[mod & 255].load();
            if (link && c) s_wideLogFrame = link - 1 + std::strtoull(c + 1, nullptr, 10);
            if (const char* d = c ? std::strchr(c, '-') : nullptr; link && d) s_wideLogTo = link - 1 + std::strtoull(d + 1, nullptr, 10);
        } else {
            s_wideLogFrame = std::strtoull(wl, nullptr, 10);
            if (const char* d = std::strchr(wl, '-')) s_wideLogTo = std::strtoull(d + 1, nullptr, 10);
        }
    }
    s_haveReady = true;
    s_frameCv.notify_all();
}

bool gxTakeFrame(std::vector<u8>& out, std::vector<std::unique_ptr<u8[]>>& snapshots, int waitMs) {
    std::unique_lock lk(s_frameLock);
    if (!s_frameCv.wait_for(lk, std::chrono::milliseconds(waitMs), [] { return s_haveReady; })) return false;
    out.swap(s_ready);
    s_ready.clear();
    snapshots.clear();  // the previous frame's: Aurora is done with it
    snapshots.swap(s_readySnapshots);
    s_haveReady = false;
    s_frameCv.notify_all();
    return true;
}

u64 gxFrames() { return s_frames; }

}  // namespace rt
