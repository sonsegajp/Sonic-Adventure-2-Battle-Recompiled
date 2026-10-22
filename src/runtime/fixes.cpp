// Changes to the game's own code for widescreen, applied while the module they are in is linked
// (instructions run through the interpreter with host code in their place, src/runtime/interp.h).
//
// Stage Select (advertiseD.rel, module 91): the map is 1920 units wide, 18 tiles, and scrolls
// between four pages (x 0, -320, -640, -1280). The tile loop (+D290) draws only tiles whose left
// edge lies in [-256, 896] - enough for 640 units of screen; and the first and last pages put the
// map's own edges at the 4:3 screen's edges. Wider, the screen shows `extra` units more on each
// side: the tile window grows by that much, and the page targets (+F274..+F310, the scroll's
// approach targets) stay that far inside the map, so no side shows past it. The stage icons are
// placed on the map by the same scroll, so they stay on their spots.
#include "runtime/interp.h"
#include "runtime/runtime.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <vector>

namespace rt {

std::atomic<float> g_wideExtra{0.f};  // logical 640x480 units shown beyond each side of the 4:3 screen

namespace {

struct Patch {
    u32 module, section, offset, original;
    InterpHook hook;
};
std::vector<u32> s_applied[256];  // addresses patched per module

std::vector<Patch> patches() {
    auto extra = [] { return std::max(0.f, g_wideExtra.load()); };
    auto target = [extra](s32 page) {
        return [extra, page](Ctx& c, u32 pc) {
            const s32 e = s32(std::ceil(extra()));
            c.r[4] = u32(std::clamp(page, -1280 + e, -e));  // inside the map: [-1280, 0] narrowed by the extra
            return pc + 4;
        };
    };
    return {
        // tile window: lfs f0, -256 / lfs f0, 896 (the loads of the limits)
        {91, 1, 0xD31C, 0xC0030000, [extra](Ctx& c, u32 pc) {
             c.f[0] = c.ps1[0] = -256.0 - extra();
             return pc + 4;
         }},
        {91, 1, 0xD36C, 0xC0030000, [extra](Ctx& c, u32 pc) {
             c.f[0] = c.ps1[0] = 896.0 + extra();
             return pc + 4;
         }},
        // page targets: li r4, 0 / -320 / -640 / -1280
        {91, 1, 0xF274, 0x38800000, target(0)},
        {91, 1, 0xF2A8, 0x3880FEC0, target(-320)},
        {91, 1, 0xF2DC, 0x3880FD80, target(-640)},
        {91, 1, 0xF310, 0x3880FB00, target(-1280)},
    };
}

}  // namespace

void gameFixesLinked(u32 module) {
    static const std::vector<Patch> s_patches = patches();
    static const std::vector<u32> s_words = [] {  // their hook words, made once
        std::vector<u32> w;
        for (const Patch& p : s_patches) w.push_back(interpHookWord(p.hook));
        return w;
    }();
    for (size_t i = 0; i < s_patches.size(); i++) {
        const Patch& p = s_patches[i];
        if (p.module != module) continue;
        const u32 a = g_relBase[module][p.section] + p.offset;
        if (rd32(a) != p.original) {
            log("[fixes] module %u +%X: %08X, not the expected %08X - left alone\n", module, p.offset, rd32(a), p.original);
            continue;
        }
        interpPatch(a, s_words[i]);
        interpOverrideAt(a);
        s_applied[module & 255].push_back(a);
    }
}

void gameFixesUnlinked(u32 module) {
    for (u32 a : s_applied[module & 255]) interpUnpatch(a);
    s_applied[module & 255].clear();
}

// Culling (main.dol). Ninja's screen setup (f_801126B0) keeps the screen's edges as pixel offsets
// from its centre at 803578C8 (right, +319) and 803578CC (left, -320); its model clip
// (f_8012192C: bounding sphere against the view volume) and the like drop whatever projects past
// them - with the wider screen, things in the strips at the sides vanished or popped in. The
// edges move out by what the screen shows beyond the 4:3 picture, whenever the setup runs (at the
// end of it: 801127A0, its epilogue's first instruction) and when the window's shape changes.
namespace {
constexpr u32 kClipRight = 0x803578C8, kClipLeft = 0x803578CC;
float s_clipRight = 319.f, s_clipLeft = -320.f;  // the game's own edges, last set
bool s_clipSeen = false;
float s_clipExtra = 0.f;  // the extra they were moved out by

void widenClip() {
    s_clipExtra = std::max(0.f, g_wideExtra.load());
    wrf32(kClipRight, s_clipRight + s_clipExtra);
    wrf32(kClipLeft, s_clipLeft - s_clipExtra);
}
}  // namespace

void gameFixesInit() {
    constexpr u32 kAt = 0x801127A0, kWord = 0x80010024;  // lwz r0, 0x24(r1)
    if (rd32(kAt) != kWord) {
        log("[fixes] main.dol %08X: %08X, not the expected %08X - culling left at 4:3\n", kAt, rd32(kAt), kWord);
        return;
    }
    interpPatch(kAt, interpHookWord([](Ctx&, u32) {
        s_clipRight = rdf32(kClipRight), s_clipLeft = rdf32(kClipLeft), s_clipSeen = true;
        widenClip();
        return kInterpOriginal;
    }));
    interpOverrideAt(kAt);
}

// Each game frame (on the game's thread): a new window shape moves the edges again.
void gameFixesFrame() {
    if (s_clipSeen && std::max(0.f, g_wideExtra.load()) != s_clipExtra) widenClip();
}

}  // namespace rt
