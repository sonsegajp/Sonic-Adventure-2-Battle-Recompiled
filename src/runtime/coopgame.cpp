// Co-op in the stage (docs/COOP.md): your partner as player 2 of your game.
//
// LoadCharacters (8001DEA8) loads player 1 - and, in 2P battle, player 2 - with the character's
// loader (LoadSonic...) and the per-player setup (8001D74C). In a co-op session it also loads the
// partner's character as player 2, without 2P mode (no split screen, no battle rules). P1 (the
// host) plays the story's character; the joiner's game swaps its player 1 to the other side's
// (Sonic <-> Shadow, Tails <-> Eggman, Knuckles <-> Rouge).
//
// Every frame each game sends its player 1 (position, rotation, action, animation) and, after its
// player 2's own update, writes the partner's into it - so player 2 is drawn where the partner is,
// as they are. Until the partner is in the same stage, player 2 is not drawn.
//
// Addresses: SA2 PC's (SA2ModLoader) through the PC <-> GameCube function map (pcmap_auto.txt).
#include "runtime/coop.h"
#include "runtime/interp.h"
#include "runtime/runtime.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <deque>
#include <iterator>
#include <mutex>
#include <set>

namespace rt::coop {

namespace {
constexpr u32 kLoadCharacters = 0x8001DEA8;
constexpr u32 kPlayerSetup = 0x8001D74C;  // per-player setup LoadCharacters runs after each loader
constexpr u32 kCurrentLevel = 0x803AD820;
constexpr u32 kCurrentCharacter = 0x803AD828, kCurrentCharacter2P = 0x803AD82C;
constexpr u32 kMainCharObj1 = 0x801E7768;  // EntityData1*[]: Action +0, Status +4, Rotation +8, Position +0x14
constexpr u32 kMainCharObj2 = 0x801E7728;  // CharObj2Base*[]: CharID +1, AnimInfo +0x174 (Current +0x178, nframe +0x184)
constexpr u32 kMainCharacter = 0x801E7788; // ObjectMaster*[]: MainSub +0x10, DisplaySub +0x14
// Loaders by character id: Sonic, Shadow, Tails, Eggman, Knuckles, Rouge, Tails' and Eggman's walkers.
constexpr u32 kLoaders[8] = {0x800874C0, 0x80086E3C, 0x800A9930, 0x800D92E4, 0x800A2360, 0x800A1F00, 0x800B5AC0, 0x800B5D2C};
int counterpart(u32 c) {
    static const int k[8] = {1, 0, 3, 2, 5, 4, 7, 6};
    return c < 8 ? k[c] : -1;
}

constexpr u8 kMsgPlayer = coopnet::kFirstGameMessage;  // a player's state, every frame
struct PlayerMsg {
    u32 frame;
    u16 level;
    u8 character, action;
    s16 status, anim;
    float nframe;
    float pos[3];
    s32 rot[3];
};

// The partner's frames, oldest first, played back a little behind the newest (a jitter buffer):
// packets arrive unevenly, frames are shown evenly, and a lost one is blended over.
std::mutex s_lock;
std::deque<PlayerMsg> s_buffer;
double s_partnerAt = -100;  // when the last one came
double s_cursor = -1;       // the partner's frame shown (fractional)
constexpr double kBehind = 2;  // frames behind the newest (about 33 ms)
u32 s_frame = 0;
u32 s_lastSwapped = ~0u;  // the joiner's player 1 as last swapped (not swapped again on a restart)
std::set<u32> s_hookedMain, s_hookedDisplay;
float s_spawn[3];  // where player 2 was set down (kept there while hidden: no falling out of the stage unseen)

double nowS() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
u32 player(u32 table, int n) { return rd32(table + u32(n) * 4); }

void drainMessages() {
    u8 type;
    std::vector<u8> data;
    while (link().receive(type, data)) {
        if (type != kMsgPlayer || data.size() < sizeof(PlayerMsg)) continue;
        PlayerMsg m;
        std::memcpy(&m, data.data(), sizeof m);
        std::lock_guard g(s_lock);
        // the partner's game started over (its frame count with it): start the buffer over
        if (!s_buffer.empty() && m.frame + 120 < s_buffer.back().frame) s_buffer.clear(), s_cursor = -1;
        auto it = s_buffer.end();
        while (it != s_buffer.begin() && std::prev(it)->frame > m.frame) --it;
        if (it != s_buffer.begin() && std::prev(it)->frame == m.frame) continue;  // (a duplicate)
        if (s_cursor >= 0 && m.frame + 2 < s_cursor) continue;                     // (too late to show)
        s_buffer.insert(it, m);
        s_partnerAt = nowS();
        while (s_buffer.size() > 64) s_buffer.pop_front();
    }
}

// Whether the partner is in this stage now (else player 2 stays hidden); `m` gets what to show
// this frame.
bool partnerHere(PlayerMsg& m) {
    drainMessages();
    std::lock_guard g(s_lock);
    if (link().state() != coopnet::State::Connected || nowS() - s_partnerAt > 1.0 || s_buffer.empty() ||
        s_buffer.back().level != rd16(kCurrentLevel))
        return false;
    // where to be: kBehind frames behind the newest, moving a frame per frame and easing toward it
    const double target = double(s_buffer.back().frame) - kBehind;
    if (s_cursor < 0 || std::fabs(s_cursor - target) > 12) s_cursor = target;
    else s_cursor += 1 + std::clamp((target - s_cursor) * 0.1, -0.5, 0.5);
    s_cursor = std::min(s_cursor, double(s_buffer.back().frame));
    while (s_buffer.size() > 2 && s_buffer[1].frame <= s_cursor - 1) s_buffer.pop_front();
    // the frames on either side of the cursor, blended
    const PlayerMsg* a = &s_buffer.front();
    const PlayerMsg* b = a;
    for (const PlayerMsg& x : s_buffer) {
        if (x.frame <= s_cursor) a = &x;
        if (x.frame >= s_cursor) {
            b = &x;
            break;
        }
    }
    m = *a;
    if (b->frame > a->frame) {
        const float t = float((s_cursor - a->frame) / double(b->frame - a->frame));
        for (int i = 0; i < 3; i++) {
            m.pos[i] = a->pos[i] + (b->pos[i] - a->pos[i]) * t;
            m.rot[i] = a->rot[i] + s32(float(s16(u16(b->rot[i] - a->rot[i]))) * t);  // (angles: 0x10000 a turn)
        }
        if (a->anim == b->anim) m.nframe = a->nframe + (b->nframe - a->nframe) * t;
    }
    return true;
}

void sendPlayer1() {
    const u32 e = player(kMainCharObj1, 0), p = player(kMainCharObj2, 0);
    if (!e || !p || link().state() != coopnet::State::Connected) return;
    PlayerMsg m{};
    m.frame = ++s_frame;
    m.level = u16(rd16(kCurrentLevel));
    m.character = u8(rd8(p + 1));
    m.action = u8(rd8(e));
    m.status = s16(rd16(e + 4));
    m.anim = s16(rd16(p + 0x178));
    m.nframe = flt(rd32(p + 0x184));
    for (int i = 0; i < 3; i++) m.rot[i] = s32(rd32(e + 8 + 4 * i)), m.pos[i] = flt(rd32(e + 0x14 + 4 * i));
    link().send(kMsgPlayer, &m, sizeof m);
}

void applyToPlayer2() {
    const u32 e = player(kMainCharObj1, 1), p = player(kMainCharObj2, 1);
    if (!e || !p) return;
    PlayerMsg m;
    if (!partnerHere(m)) {
        for (int i = 0; i < 3; i++) wr32(e + 0x14 + 4 * i, bits32(s_spawn[i]));
        return;
    }
    for (int i = 0; i < 3; i++) wr32(e + 8 + 4 * i, u32(m.rot[i])), wr32(e + 0x14 + 4 * i, bits32(m.pos[i]));
    // the pose: the partner's animation at their frame (its own state machine keeps running underneath)
    wr16(p + 0x176, u16(m.anim));  // Next
    wr16(p + 0x178, u16(m.anim));  // Current
    wr32(p + 0x184, bits32(m.nframe));
}

// Each character's main function (shared: Sonic and Shadow run the same code): after player 1's,
// it is sent. Player 2's does not run at all: it is only what the partner is - their position,
// turn and pose - and takes no part in the stage (no collisions: it collects no rings, hits no
// enemy, ends no stage at the goal; those are shared by messages instead).
void characterMain(Ctx& c, u32 entry) {
    const u32 obj = c.r[3];
    if (obj == player(kMainCharacter, 1)) return applyToPlayer2();
    interpOriginal(c, entry);
    if (obj == player(kMainCharacter, 0)) sendPlayer1();
}
void characterDisplay(Ctx& c, u32 entry) {
    PlayerMsg m;
    if (c.r[3] == player(kMainCharacter, 1) && !partnerHere(m)) return;  // not drawn until the partner is here
    interpOriginal(c, entry);
}

void hookCharacter(int n) {
    const u32 obj = player(kMainCharacter, n);
    if (!obj) return;
    const u32 main = rd32(obj + 0x10), display = rd32(obj + 0x14);
    if (main && s_hookedMain.insert(main).second) interpReplace(0, main, characterMain);
    if (display && s_hookedDisplay.insert(display).second) interpReplace(0, display, characterDisplay);
}

// The action stages (not the test levels, bosses, karts or the Chao World - for now).
bool coopStage(u32 level) { return level >= 3 && level < 60; }

void loadCharacters(Ctx& c, u32 entry) {
    const u32 level = rd16(kCurrentLevel);
    const bool here = coopStage(level) && !(rd8(0x801CC186) & 0xFF);  // (not in 2P battle)
    u32 local = rd32(kCurrentCharacter);
    if (here && !link().host() && counterpart(local) >= 0 && local != s_lastSwapped) {
        local = u32(counterpart(local));  // the joiner plays the other side's character
        wr32(kCurrentCharacter, local);
        s_lastSwapped = local;
    }
    interpOriginal(c, entry);
    if (!here || counterpart(local) < 0) return;
    const u32 partner = u32(counterpart(local));
    wr32(kCurrentCharacter2P, partner);
    guestCall(c, kLoaders[partner], {1});
    guestCall(c, kPlayerSetup, {1});
    hookCharacter(0), hookCharacter(1);
    if (const u32 e = player(kMainCharObj1, 1))
        for (int i = 0; i < 3; i++) s_spawn[i] = flt(rd32(e + 0x14 + 4 * i));
    log("[coop] level %u: player 1 %u, the partner's %u as player 2 (%08X)\n", level, local, partner, player(kMainCharacter, 1));
}
}  // namespace

// From coop::init, in a co-op session.
void gameInit() { interpReplace(0, kLoadCharacters, loadCharacters); }

}  // namespace rt::coop
