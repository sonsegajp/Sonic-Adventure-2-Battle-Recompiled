// Character colours (docs/COOP.md): each colour area - Sonic's blue, Shadow's red stripes, Tails'
// fur, Eggman's suit, Knuckles' red, Rouge's white fur, Sonic's and Shadow's jump ball and trails,
// the Cyclone's blue and yellow, the Egg Walker's gray and red - made white (its brightness kept, so
// the shading stays) and tinted with the player's colour, in the textures as they sit in memory
// (Aurora hashes texture contents: the new ones are drawn from the next frame).
//
// A character's textures: its CharObj2's NJS_TEXLIST (at +0x394 Sonic and Shadow, +0x3B0 Tails,
// +0x360 Eggman, +0x3F0/+0x3F4 Knuckles and Rouge, +0x448/+0x444 the walkers): { NJS_TEXNAME*, count }
// of { name, attr, manage }; manage +0xC the texture (+4 GX format, +0x18 size with the mipmaps, +0x20
// the data), +0x10 how many lists use it (one two characters share - Sonic's and Shadow's - is left
// as it is). In the order of the disc's archives (SONICTEX: 10 is s_wind, the trail; TWALKTEX, 31:
// 3-4 are Tails' own; EWALKTEX, 47: 3-10 Eggman's own). The jump ball's glow and the trails are
// S_EFFTEX's and SH_EFFTEX's first 27 (ball, dash 1-23, charge), loaded into the lists at 803AD4C8
// and 803AD4D0 (by 8008BDC4 and 8008BD60, from LoadSonic and LoadShadow). The GX format is CMPR:
// 4x4 blocks of two RGB565 colours and 2-bit indices.
#include "runtime/coop.h"
#include "runtime/gx.h"
#include "runtime/runtime.h"

#include <algorithm>
#include <cmath>
#include <deque>
#include <memory>
#include <unordered_map>
#include <vector>

namespace rt::coop {

namespace {
constexpr u32 kMainCharObj2 = 0x801E7728;
constexpr u32 kSonicEffects = 0x803AD4C8, kShadowEffects = 0x803AD4D0;

enum Slot : int {
    SonicBlue, SonicEffects, ShadowStripes, ShadowEffects, TailsFur, EggmanSuit, KnucklesRed, RougeFur,
    CycloneBlue, CycloneYellow, WalkerGray, WalkerRed,
};
static_assert(WalkerRed + 1 == kColourSlots);

// What of a texture is a slot's area.
struct Area {
    enum Kind { Hue, White, Gray, Coloured } kind;
    float hueLo = 0, hueHi = 0;  // degrees (lo > hi: around red)
    float satMin = 0;
};
const Area kAreas[kColourSlots] = {
    {Area::Hue, 195, 255, 0.30f},  // Sonic: blue
    {Area::Coloured},              // Sonic's ball and trails
    {Area::Hue, 345, 12, 0.55f},   // Shadow: red stripes
    {Area::Coloured},              // Shadow's ball and trails
    {Area::Hue, 20, 55, 0.40f},    // Tails: fur
    {Area::Hue, 345, 12, 0.55f},   // Eggman: suit
    {Area::Hue, 345, 12, 0.50f},   // Knuckles: red
    {Area::White},                 // Rouge: white fur
    {Area::Hue, 205, 265, 0.35f},  // the Cyclone: blue
    {Area::Hue, 38, 65, 0.50f},    // the Cyclone: yellow
    {Area::Gray},                  // the Egg Walker: gray
    {Area::Hue, 345, 15, 0.50f},   // the Egg Walker: red
};

float smooth(float lo, float hi, float x) {
    const float t = std::clamp((x - lo) / (hi - lo), 0.f, 1.f);
    return t * t * (3 - 2 * t);
}
// How much of an area a colour is (0..1).
float weight(const Area& a, float r, float g, float b) {
    const float v = std::max({r, g, b}), mn = std::min({r, g, b});
    const float s = v > 0 ? (v - mn) / v : 0;
    switch (a.kind) {
    case Area::White: return smooth(0.45f, 0.6f, v) * (1 - smooth(0.12f, 0.25f, s));
    case Area::Gray: return smooth(0.08f, 0.2f, v) * (1 - smooth(0.12f, 0.22f, s));
    case Area::Coloured: return smooth(0.15f, 0.3f, s) * smooth(0.03f, 0.1f, v);  // (a white core stays white)
    case Area::Hue: break;
    }
    if (v - mn < 1e-4f) return 0;
    float h;
    if (v == r) h = 60 * std::fmod((g - b) / (v - mn), 6.f);
    else if (v == g) h = 60 * ((b - r) / (v - mn) + 2);
    else h = 60 * ((r - g) / (v - mn) + 4);
    if (h < 0) h += 360;
    auto dist = [](float x, float y) { const float d = std::fabs(x - y); return std::min(d, 360 - d); };
    const bool inside = a.hueLo <= a.hueHi ? h >= a.hueLo && h <= a.hueHi : h >= a.hueLo || h <= a.hueHi;
    const float out = inside ? 0 : std::min(dist(h, a.hueLo), dist(h, a.hueHi));  // (degrees outside)
    return (1 - smooth(0, 12, out)) * smooth(a.satMin - 0.1f, a.satMin + 0.05f, s) * smooth(0.06f, 0.18f, v);
}

// A texture's areas and their tints.
struct Paint {
    bool grey = false;  // all of it in grey (a rival)
    int count = 0;
    const Area* area[2];
    float tint[2][3];
    void add(int slot, int rgb) {
        if (rgb < 0 || count == 2) return;
        area[count] = &kAreas[slot];
        tint[count][0] = float(rgb >> 16 & 255) / 255, tint[count][1] = float(rgb >> 8 & 255) / 255, tint[count][2] = float(rgb & 255) / 255;
        count++;
    }
};
// One colour: each area's part of it (weighed on the colour as it was) turned to its brightness times
// the area's tint.
u16 paint565(const Paint& p, u16 c) {
    const float r0 = float(c >> 11) / 31, g0 = float(c >> 5 & 63) / 63, b0 = float(c & 31) / 31;
    if (p.grey) {
        const float y = std::clamp(0.30f * r0 + 0.59f * g0 + 0.11f * b0, 0.f, 1.f);
        return u16(std::lround(y * 31) << 11 | std::lround(y * 63) << 5 | std::lround(y * 31));
    }
    const float v = std::max({r0, g0, b0});
    float r = r0, g = g0, b = b0;
    for (int i = 0; i < p.count; i++) {
        const float w = weight(*p.area[i], r0, g0, b0);
        if (w <= 0) continue;
        r += (v * p.tint[i][0] - r0) * w, g += (v * p.tint[i][1] - g0) * w, b += (v * p.tint[i][2] - b0) * w;
    }
    r = std::clamp(r, 0.f, 1.f), g = std::clamp(g, 0.f, 1.f), b = std::clamp(b, 0.f, 1.f);
    return u16(std::lround(r * 31) << 11 | std::lround(g * 63) << 5 | std::lround(b * 31));
}

// A CMPR block (8 bytes): colours 0 and 1, then 16 2-bit indices. Colour 0 > colour 1: four colours
// (2 and 3 between them); else three and transparent - so the order is kept as it was (swapping
// the colours and their indices if the new ones came out the other way).
void paintBlock(const Paint& pt, u8* p) {
    const u16 c0 = u16(p[0] << 8 | p[1]), c1 = u16(p[2] << 8 | p[3]);
    u16 n0 = paint565(pt, c0), n1 = paint565(pt, c1);
    if (n0 == c0 && n1 == c1) return;
    u32 idx = u32(p[4]) << 24 | u32(p[5]) << 16 | u32(p[6]) << 8 | p[7];
    if (c0 > c1) {
        if (n0 < n1) std::swap(n0, n1), idx ^= 0x55555555u;  // (0 <-> 1, 2 <-> 3)
        else if (n0 == n1) n0 == 0xFFFF ? n1-- : n0++;       // (equal would be three colours)
    } else if (n0 > n1) {
        std::swap(n0, n1);
        for (int k = 0; k < 16; k++)  // (0 <-> 1; 2, the middle, and 3, transparent, stay)
            if ((idx >> (2 * k) & 3) < 2) idx ^= 1u << (2 * k);
    }
    p[0] = u8(n0 >> 8), p[1] = u8(n0), p[2] = u8(n1 >> 8), p[3] = u8(n1);
    p[4] = u8(idx >> 24), p[5] = u8(idx >> 16), p[6] = u8(idx >> 8), p[7] = u8(idx);
}

// Each texture's own data, by address: the colours always start from it. `written` tells our own
// painting from a new texture loaded at the same place (a new stage).
struct Saved {
    std::vector<u8> original;
    u64 written = 0;
};
std::unordered_map<u32, Saved> s_saved;

u64 hashOf(const u8* p, size_t n) {
    u64 h = 1469598103934665603ull;
    for (size_t i = 0; i < n; i++) h = (h ^ p[i]) * 1099511628211ull;
    return h;
}

void paintTexture(u32 manage, const Paint& paint) {
    if (!manage || rd32(manage + 0x10) > 1) return;  // (shared with another character)
    const u32 tex = rd32(manage + 0xC);
    if (!tex) return;
    const u32 format = rd32(tex + 4), size = rd32(tex + 0x18), data = rd32(tex + 0x20);
    if (format != 0xE || !size || size > 0x40000 || size % 8 || (data & 0xFE000000) != 0x80000000) return;
    u8* p = hostp(data);
    if (s_saved.size() > 512) s_saved.clear();
    Saved& s = s_saved[data];
    if (s.original.size() != size || s.written != hashOf(p, size)) s.original.assign(p, p + size);  // (new: as loaded)
    std::copy(s.original.begin(), s.original.end(), p);
    for (u32 o = 0; paint.count && o < size; o += 8) paintBlock(paint, p + o);
    s.written = hashOf(p, size);
}

// ---- painting as drawn ----
// A texture two players share (a rival stage: the rival and the partner are the same character) is
// painted as each draws it instead: while one draws (drawPainted), its textures are handed to the
// renderer as painted copies (g_texSubstitute, src/runtime/gx.cpp) - the rival in grey, the partner
// in their colours. A copy is made again only when the texture itself changes.
struct Planned {
    Paint paint;
    u32 size = 0;
};
std::unordered_map<u32, Planned> s_plans[8];  // by player slot: texture data -> its paint
int s_drawing = -1;
struct Copy {
    u64 source = 0, paint = 0;  // (what it was made from)
    std::unique_ptr<u8[]> data;
};
std::unordered_map<u64, Copy> s_copies;      // by slot << 32 | data address
std::deque<std::unique_ptr<u8[]>> s_retired;  // replaced copies, kept while frames using them may be drawn

u64 paintKey(const Paint& p) {
    u64 h = p.grey ? 0x9E3779B97F4A7C15ull : 1469598103934665603ull;
    for (int i = 0; i < p.count; i++) {
        h = (h ^ u64(p.area[i] - kAreas)) * 1099511628211ull;
        for (float t : p.tint[i]) h = (h ^ u64(t * 255)) * 1099511628211ull;
    }
    return h;
}
const u8* substitute(u32 addr, u32 format, u32, u32) {
    if (s_drawing < 0 || format != 0xE) return nullptr;
    const auto it = s_plans[s_drawing].find(addr);
    if (it == s_plans[s_drawing].end() || !it->second.size) return nullptr;
    const Planned& pl = it->second;
    const u8* src = hostp(addr);
    const u64 source = hashOf(src, pl.size), key = paintKey(pl.paint);
    Copy& c = s_copies[u64(s_drawing) << 32 | addr];
    if (!c.data || c.source != source || c.paint != key) {
        if (c.data) s_retired.push_back(std::move(c.data));
        while (s_retired.size() > 64) s_retired.pop_front();
        c.data.reset(new u8[pl.size]);
        std::memcpy(c.data.get(), src, pl.size);
        for (u32 o = 0; o < pl.size; o += 8) paintBlock(pl.paint, c.data.get() + o);
        c.source = source, c.paint = key;
    }
    return c.data.get();
}
void planTexture(int slot, u32 manage, const Paint& paint) {
    if (!manage || !(paint.count || paint.grey)) return;
    if (!paint.grey && rd32(manage + 0x10) <= 1) return;  // (its own: painted in memory - recolourPlayer)
    const u32 tex = rd32(manage + 0xC);
    if (!tex) return;
    const u32 format = rd32(tex + 4), size = rd32(tex + 0x18), data = rd32(tex + 0x20);
    if (format != 0xE || !size || size > 0x40000 || size % 8 || (data & 0xFE000000) != 0x80000000) return;
    s_plans[slot][data] = {paint, size};
}

// A texture list: texture i painted with what `paintOf(i, count)` gives.
template <class F> void paintList(u32 list, F paintOf) {
    if (!list || !rd32(list)) return;
    const u32 count = std::min<u32>(rd32(list + 4), 64);
    for (u32 i = 0; i < count; i++) paintTexture(rd32(rd32(list) + i * 12 + 8), paintOf(i, count));
}
}  // namespace

namespace {
// Player n's textures, each with the paint the colours give it, handed to `use(manage, paint)`.
template <class Use> void eachTexture(int n, const int* c, Use use) {
    const u32 obj2 = rd32(kMainCharObj2 + u32(n) * 4);
    if (!obj2) return;
    auto one = [&](int slot) { Paint p; if (c) p.add(slot, c[slot]); else p.grey = true; return p; };
    auto two = [&](int a, int b) { Paint p; if (c) p.add(a, c[a]), p.add(b, c[b]); else p.grey = true; return p; };
    auto list = [&](u32 l, auto paintOf) {
        if (!l || !rd32(l)) return;
        const u32 count = std::min<u32>(rd32(l + 4), 64);
        for (u32 i = 0; i < count; i++) use(rd32(rd32(l) + i * 12 + 8), paintOf(i, count));
    };
    const auto all = [](const Paint& p) { return [p](u32, u32) { return p; }; };
    switch (rd8(obj2 + 1)) {
    case 0:  // Sonic: s_wind (10) is the trail's
        list(rd32(obj2 + 0x394), [&](u32 i, u32) { return one(i == 10 ? SonicEffects : SonicBlue); });
        if (c) list(kSonicEffects, [&](u32 i, u32) { return i < 27 ? one(SonicEffects) : Paint{}; });
        break;
    case 1:
        list(rd32(obj2 + 0x394), all(one(ShadowStripes)));
        if (c) list(kShadowEffects, [&](u32 i, u32) { return i < 27 ? one(ShadowEffects) : Paint{}; });
        break;
    case 2: list(rd32(obj2 + 0x3B0), all(one(TailsFur))); break;
    case 3: list(rd32(obj2 + 0x360), all(one(EggmanSuit))); break;
    case 4: list(rd32(obj2 + 0x3F0), all(one(KnucklesRed))), list(rd32(obj2 + 0x3F4), all(one(KnucklesRed))); break;
    case 5: list(rd32(obj2 + 0x3F0), all(one(RougeFur))), list(rd32(obj2 + 0x3F4), all(one(RougeFur))); break;
    case 6:  // the Cyclone (TWALKTEX: 3-4 Tails' own) and Tails' own list
        for (u32 off : {0x448u, 0x444u})
            list(rd32(obj2 + off), [&](u32 i, u32 count) {
                return count == 31 && (i < 3 || i > 4) ? two(CycloneBlue, CycloneYellow) : one(TailsFur);
            });
        break;
    case 7:  // the Egg Walker (EWALKTEX: 3-10 Eggman's own) and Eggman's own list
        for (u32 off : {0x448u, 0x444u})
            list(rd32(obj2 + off), [&](u32 i, u32 count) {
                return count == 47 && (i < 3 || i > 10) ? two(WalkerGray, WalkerRed) : one(EggmanSuit);
            });
        break;
    }
}
}  // namespace

// Player n's character in the colours given (by slot: 0xRRGGBB, or -1 as the game has it), in memory.
void recolourPlayer(int n, const int* colours) {
    eachTexture(n, colours, [](u32 manage, const Paint& p) { paintTexture(manage, p); });
}
// ... or as drawn (colours nullptr: all grey), the textures it shares (its own are painted in memory):
// drawPainted(n) around its drawing.
void planDrawn(int n, const int* colours) {
    if (n < 0 || n >= 8) return;
    s_plans[n].clear();
    eachTexture(n, colours, [n](u32 manage, const Paint& p) { planTexture(n, manage, p); });
}
void drawPainted(int n) {
    s_drawing = n >= 0 && n < 8 && !s_plans[n].empty() ? n : -1;
    g_texSubstitute = s_drawing >= 0 ? substitute : nullptr;
}

}  // namespace rt::coop
