// Co-op in the stage (docs/COOP.md): your partner as player 2 of your game.
//
// LoadCharacters (8001DEA8) loads player 1 - and, in 2P battle, player 2 - with the character's
// loader (LoadSonic...) and the per-player setup (8001D74C). In a co-op session it also loads the
// partner's character as player 2, without 2P mode (no split screen, no battle rules). P1 (the
// host) plays the story's character; the joiner's game swaps its player 1 to the other side's
// (Sonic <-> Shadow, Tails <-> Eggman, Knuckles <-> Rouge).
//
// Every frame each game sends its player 1 (position, turn, speed, animation, and the controller as
// the character read it). Player 2 runs its own update from the partner's state and controller - so
// it rides its board, makes its effects and plays its sounds and voice itself - and is then set to
// the partner's position and pose for drawing. Until the partner is in the same stage, player 2
// neither runs nor is drawn.
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
#include <array>
#include <atomic>
#include <set>
#include <string>
#include <vector>

namespace rt {
extern std::atomic<bool> g_relLinked[256];  // src/runtime/dispatch.cpp
}

namespace rt::coop {

namespace {
constexpr u32 kLoadCharacters = 0x8001DEA8;
constexpr u32 kPlayerSetup = 0x8001D74C;  // per-player setup LoadCharacters runs after each loader
constexpr u32 kCurrentLevel = 0x803AD820;
constexpr u32 kCurrentCharacter = 0x803AD828, kCurrentCharacter2P = 0x803AD82C;
constexpr u32 kMainCharObj1 = 0x801E7768;  // EntityData1*[]: Action +0, Status +4, Rotation +8, Position +0x14
constexpr u32 kMainCharObj2 = 0x801E7728;  // CharObj2Base*[]: CharID +1, AnimInfo +0x174 (Current +0x178, nframe +0x184)
constexpr u32 kMainCharacter = 0x801E7788; // ObjectMaster*[]: MainSub +0x10, DisplaySub +0x14
// The players' controllers as the characters read them (Ninja PDS_PERIPHERAL, 0x34 each; filled from
// PADRead by f_80119A04 every frame, before the characters run): support +4, on +8, off +0xC, press
// +0x10, release +0x14, r +0x18, l +0x1A, x1 y1 x2 y2 +0x1C..0x22, info +0x30 (type 0x101: there).
constexpr u32 kControllers = 0x80357E60, kControllerSize = 0x34;
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
    float speed[3];        // CharObj2Base Speed (+0x64)
    u32 on, press, release;  // the controller as the character read it
    s16 stick[4];          // x1 y1 x2 y2
    u16 trigger[2];        // r l
    u8 nextAction, pad_[3];
    u8 animInfo[0x24];     // CharObj2Base AnimInfo (+0x174) as is: modes, animations, blend, frame
};

// The host's pad, for the joiner's menus and cutscenes ("player 1 drives", below).
constexpr u8 kMsgPad = coopnet::kFirstGameMessage + 1;
struct PadMsg {
    u32 seq;            // of the newest of the four
    u8 inGameplay;      // the host's game is in gameplay (its pad then plays its own stage)
    u8 pad_[3];
    u8 pads[4][12];     // PADStatus of port 1, frames seq-3 .. seq (a lost packet loses no press)
    u32 screens[4];     // the screen each was on (screenKey)
};
// A hit on the other player ("damage", below).
constexpr u8 kMsgHit = coopnet::kFirstGameMessage + 2;
struct HitMsg {
    float from[3];  // where the hitter was
};
bool s_hitPending = false;

// The partner's frames, oldest first, played back a little behind the newest (a jitter buffer):
// packets arrive unevenly, frames are shown evenly, and a lost one is blended over.
std::mutex s_lock;
std::deque<PlayerMsg> s_buffer;
double s_partnerAt = -100;  // when the last one came
u32 s_pendingPress = 0, s_pendingRelease = 0;  // the partner's button edges not given to player 2 yet
bool s_player2Shown = false;  // player 2 ran this frame as the partner (else it is not drawn)
double s_cursor = -1;       // the partner's frame shown (fractional)
constexpr double kBehind = 2;  // frames behind the newest (about 33 ms)
u32 s_frame = 0;
u32 s_lastSwapped = ~0u;  // the joiner's player 1 as last swapped (not swapped again on a restart)
std::set<u32> s_hookedMain, s_hookedDisplay;
float s_spawn[3];  // where player 2 was set down (kept there while hidden: no falling out of the stage unseen)

double nowS() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
u32 player(u32 table, int n) { return rd32(table + u32(n) * 4); }
void setObjectsFrame();  // (shared objects, below)
void setObjectsReset();
void beforePlayer1();  // (damage, below)
void afterPlayer1();
void checkHitOnPartner();
void hostPadArrived(const PadMsg& p);  // (player 1 drives, below)


void drainMessages() {
    u8 type;
    std::vector<u8> data;
    while (link().receive(type, data)) {
        if (type == kMsgPad && data.size() >= sizeof(PadMsg)) {
            PadMsg p;
            std::memcpy(&p, data.data(), sizeof p);
            hostPadArrived(p);
            continue;
        }
        if (type == kMsgHit) {
            s_hitPending = true;
            continue;
        }
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
        s_pendingPress |= m.press, s_pendingRelease |= m.release;  // (edges are never lost to the buffer)
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
    for (int i = 0; i < 3; i++) {
        m.rot[i] = s32(rd32(e + 8 + 4 * i)), m.pos[i] = flt(rd32(e + 0x14 + 4 * i));
        m.speed[i] = flt(rd32(p + 0x64 + 4 * i));
    }
    const u32 pad = kControllers;  // player 1's controller, as the character read it this frame
    m.on = rd32(pad + 8), m.press = rd32(pad + 0x10), m.release = rd32(pad + 0x14);
    for (int i = 0; i < 4; i++) m.stick[i] = s16(rd16(pad + 0x1C + 2 * i));
    m.trigger[0] = u16(rd16(pad + 0x18)), m.trigger[1] = u16(rd16(pad + 0x1A));
    m.nextAction = u8(rd8(e + 1));
    for (u32 i = 0; i < sizeof m.animInfo; i++) m.animInfo[i] = u8(rd8(p + 0x174 + i));
    link().send(kMsgPlayer, &m, sizeof m);
}

// Player 2 runs its own update - so it rides its board, makes its effects, plays its sounds and
// voice - from the partner's state and the partner's controller: before it runs, their position,
// turn and speed and their buttons and stick (in player 2's controller slot); after, their position
// and pose again, so it is drawn exactly where and as they are. (Its action is left to its own
// state machine: forcing one - a grind without its rail - could make the code write through a null
// pointer.) Until the partner is in this stage it does not run and is not drawn.
void beforePlayer2(const PlayerMsg& m) {
    const u32 e = player(kMainCharObj1, 1), p = player(kMainCharObj2, 1);
    for (int i = 0; i < 3; i++) {
        wr32(e + 8 + 4 * i, u32(m.rot[i])), wr32(e + 0x14 + 4 * i, bits32(m.pos[i]));
        wr32(p + 0x64 + 4 * i, bits32(m.speed[i]));
    }
    const u32 pad = kControllers + kControllerSize;
    u32 press, release;
    {
        std::lock_guard g(s_lock);
        press = m.press | s_pendingPress, release = m.release | s_pendingRelease;
        s_pendingPress = s_pendingRelease = 0;
    }
    wr32(pad + 4, 0x001F07FE);  // supports the GameCube pad's buttons and sticks
    wr32(pad + 8, m.on), wr32(pad + 0xC, ~m.on), wr32(pad + 0x10, press), wr32(pad + 0x14, release);
    wr16(pad + 0x18, m.trigger[0]), wr16(pad + 0x1A, m.trigger[1]);
    for (int i = 0; i < 4; i++) wr16(pad + 0x1C + 2 * i, u16(m.stick[i]));
    if (const u32 info = rd32(pad + 0x30)) wr32(info, 0x101);  // a pad is there
}
// What player 2 is between its own updates - its action, status and animation state as its own
// code left them - kept apart from what is shown (the partner's): put back before it runs, so its
// state machine and animation blending go on undisturbed and never fight the partner's pose (they
// did: the model jerked between the two every frame).
struct OwnState {
    u8 action, nextAction;
    u16 status;
    u8 anim[0x24];
};
OwnState s_own;
bool s_ownValid = false;
void restoreOwn(u32 e, u32 p) {
    if (!s_ownValid) return;
    wr8(e, s_own.action), wr8(e + 1, s_own.nextAction), wr16(e + 4, s_own.status);
    for (u32 i = 0; i < sizeof s_own.anim; i++) wr8(p + 0x174 + i, s_own.anim[i]);
}
void saveOwn(u32 e, u32 p) {
    s_own.action = u8(rd8(e)), s_own.nextAction = u8(rd8(e + 1)), s_own.status = u16(rd16(e + 4));
    for (u32 i = 0; i < sizeof s_own.anim; i++) s_own.anim[i] = u8(rd8(p + 0x174 + i));
    s_ownValid = true;
}
// After its update: shown as the partner is - where, turned how, doing what (the action and status
// pick the jump ball, the board and the like), posed how (their whole animation state: animations,
// blend, frame).
void afterPlayer2(const PlayerMsg& m) {
    const u32 e = player(kMainCharObj1, 1), p = player(kMainCharObj2, 1);
    saveOwn(e, p);
    for (int i = 0; i < 3; i++) wr32(e + 8 + 4 * i, u32(m.rot[i])), wr32(e + 0x14 + 4 * i, bits32(m.pos[i]));
    wr8(e, m.action), wr8(e + 1, m.nextAction), wr16(e + 4, u16(m.status));
    for (u32 i = 0; i < sizeof m.animInfo; i++) wr8(p + 0x174 + i, m.animInfo[i]);
    wr32(p + 0x184, bits32(m.nframe));  // (the frame, blended between packets)
}

// Each character's main function (shared: Sonic and Shadow run the same code): after player 1's,
// it is sent; player 2's runs as above.
void characterMain(Ctx& c, u32 entry) {
    const u32 obj = c.r[3];
    if (obj == player(kMainCharacter, 1)) {
        const u32 e = player(kMainCharObj1, 1), p = player(kMainCharObj2, 1);
        PlayerMsg m;
        s_player2Shown = e && p && partnerHere(m);
        if (!s_player2Shown) {  // not here yet: kept where it was set down, not run
            if (e)
                for (int i = 0; i < 3; i++) wr32(e + 0x14 + 4 * i, bits32(s_spawn[i]));
            return;
        }
        restoreOwn(e, p);
        beforePlayer2(m);
        interpOriginal(c, entry);
        afterPlayer2(m);
        static const bool s_log = std::getenv("SA2_COOP_LOG") != nullptr;  // (where both are, once a second)
        if (s_log && m.frame % 60 == 0) {
            const u32 e1 = player(kMainCharObj1, 0);
            log("[coop] frame %u: player 1 at %.0f %.0f %.0f, partner at %.0f %.0f %.0f, action %u, anim %d\n", m.frame,
                flt(rd32(e1 + 0x14)), flt(rd32(e1 + 0x18)), flt(rd32(e1 + 0x1C)), m.pos[0], m.pos[1], m.pos[2], m.action, m.anim);
        }
        return;
    }
    const bool p1 = obj == player(kMainCharacter, 0);
    if (p1) beforePlayer1();
    interpOriginal(c, entry);
    if (p1) afterPlayer1(), sendPlayer1(), setObjectsFrame(), checkHitOnPartner();
}
void characterDisplay(Ctx& c, u32 entry) {
    if (c.r[3] == player(kMainCharacter, 1) && !s_player2Shown) return;  // not drawn until the partner is here
    interpOriginal(c, entry);
}

void hookCharacter(int n) {
    const u32 obj = player(kMainCharacter, n);
    if (!obj) return;
    const u32 main = rd32(obj + 0x10), display = rd32(obj + 0x14);
    if (main && s_hookedMain.insert(main).second) interpReplace(0, main, characterMain);
    if (display && s_hookedDisplay.insert(display).second) interpReplace(0, display, characterDisplay);
}

// ---- shared objects: rings, item boxes and enemies come back 10 s after they are taken ----
// The stage's SET objects (801F5968, 16 bytes each, count at 803ADA14): flags at +2 (0x8000: may
// spawn - cleared when the object is taken for good, by 800653E0), the SET entry at +8 (its ID at +0
// indexes the stage's object list, 803ADA2C: { count, entries of 0x10: ... name at +0xC }). Giving
// 0x8000 back makes the stage's own spawner bring the object back once it is in range again.
constexpr u32 kSetObjects = 0x801F5968, kSetObjectCount = 0x803ADA14, kObjectList = 0x803ADA2C;
constexpr double kRespawnSeconds = 10;
std::vector<u16> s_setFlags;      // last frame's flags
std::vector<double> s_setTaken;   // when each was taken (0: not waiting to come back)
std::vector<s8> s_setRespawns;    // by object list entry: -1 unknown, 0 stays taken, 1 comes back

std::string objectName(u32 id) {
    const u32 head = rd32(kObjectList);
    if (!head || id >= rd32(head)) return {};
    const u32 name = rd32(rd32(head + 4) + id * 0x10 + 0xC);
    std::string s;
    for (u32 i = 0; name && i < 32; i++) {
        const char ch = char(rd8(name + i));
        if (!ch) break;
        s += ch;
    }
    return s;
}
bool respawns(u32 id) {
    if (id >= 4096) return false;
    if (s_setRespawns.size() <= id) s_setRespawns.resize(id + 1, -1);
    if (s_setRespawns[id] < 0) {
        const std::string n = objectName(id);
        const bool ring = n.find("RING") != std::string::npos, box = n.find("ITEM") != std::string::npos;
        const bool enemy = n.size() > 2 && n[0] == 'E' && (n[1] == ' ' || n[1] == '_');
        s_setRespawns[id] = s8(ring || box || enemy);
    }
    return s_setRespawns[id] > 0;
}
void setObjectsReset() {
    s_setFlags.clear(), s_setTaken.clear(), s_setRespawns.clear();
    const u32 head = rd32(kObjectList);
    std::string names;
    for (u32 i = 0; head && i < rd32(head) && i < 256; i++) names += std::to_string(i) + ":" + objectName(i) + " ";
    log("[coop] stage objects: %s\n", names.c_str());
}
// Once a frame in the stage (with player 1's update).
void setObjectsFrame() {
    const u32 count = std::min<u32>(rd16(kSetObjectCount), 0x600);
    s_setFlags.resize(count, 0), s_setTaken.resize(count, 0);
    const double now = nowS();
    for (u32 i = 0; i < count; i++) {
        const u32 a = kSetObjects + i * 16;
        const u16 f = u16(rd16(a + 2));
        if ((s_setFlags[i] & 0x8000) && !(f & 0x8000)) {
            const u32 entry = rd32(a + 8);
            if (entry && respawns(rd16(entry) & 0xFFF)) s_setTaken[i] = now;
        }
        if (s_setTaken[i] > 0 && now - s_setTaken[i] >= kRespawnSeconds) {
            wr16(a + 2, f | 0x8000);
            s_setTaken[i] = 0;
        }
        s_setFlags[i] = u16(rd16(a + 2));
    }
}

// ---- damage: each game decides its own player's ----
// Player 2 is the partner and is hurt or killed only in their own game: here DamagePlayer
// (80055734, which applies Status 0x4 Hurt: knockback, rings) does nothing to it and KillPlayer
// (8003BB7C) leaves it alone - the one-player game took either for its player's (a hit on the
// partner killed you). A hit between players - an attack (Status 0x400) or a spin (0x100) that
// reaches them - is told to the one hit, whose game knocks its player back: their own Hurt, with
// their rings kept (and never at none, so it never kills).
constexpr u32 kDamagePlayer = 0x80055734, kKillPlayer = 0x8003BB7C;
constexpr u32 kRingCount = 0x801CC168;  // s16 per player
double s_lastHitSent = -100;
s16 s_ringsKept = -1;

void checkHitOnPartner() {
    if (!s_player2Shown) return;
    const u32 e1 = player(kMainCharObj1, 0), e2 = player(kMainCharObj1, 1);
    if (!e1 || !e2 || !(rd16(e1 + 4) & 0x0500)) return;
    float d2 = 0;
    for (int i = 0; i < 3; i++) {
        const float d = flt(rd32(e1 + 0x14 + 4 * i)) - flt(rd32(e2 + 0x14 + 4 * i));
        d2 += d * d;
    }
    if (d2 > 18.f * 18.f || nowS() - s_lastHitSent < 1.0) return;
    HitMsg h;
    for (int i = 0; i < 3; i++) h.from[i] = flt(rd32(e1 + 0x14 + 4 * i));
    link().send(kMsgHit, &h, sizeof h);
    s_lastHitSent = nowS();
}

void damagePlayer(Ctx& c, u32 entry) {
    if (c.r[3] && c.r[3] == player(kMainCharObj1, 1)) return void(c.r[3] = 0);  // (the partner: not here)
    interpOriginal(c, entry);
}
void killPlayer(Ctx& c, u32 entry) {
    if (c.r[3] == 1 && player(kMainCharacter, 1)) return;  // (the partner)
    interpOriginal(c, entry);
}
// Around player 1's update: a hit from the partner becomes its own knockback, its rings kept.
void beforePlayer1() {
    if (!s_hitPending) return;
    s_hitPending = false;
    const u32 e1 = player(kMainCharObj1, 0);
    if (!e1 || (rd16(e1 + 4) & 0x4)) return;
    s_ringsKept = s16(rd16(kRingCount));
    if (s_ringsKept <= 0) wr16(kRingCount, 1);
    wr16(e1 + 4, u16(rd16(e1 + 4) | 0x4));
}
void afterPlayer1() {
    if (s_ringsKept < 0) return;
    wr16(kRingCount, u16(s_ringsKept));
    s_ringsKept = -1;
}

// ---- player 1 (the host) drives the menus and cutscenes ----
// Outside gameplay (menus, events, loading, results) the joiner's game reads the host's pad instead
// of its own, frame by frame and in order - so the host's choices and skips happen in both games and
// the joiner can make none. The host's game waits at the title (its buttons do nothing outside
// gameplay) until the joiner is connected, so both start together.
constexpr u32 kGameState = 0x803AD818;  // 0x10 playing, 0x11 paused
u32 s_padSeq = 0;
u8 s_myPads[4][12];
u32 s_myScreens[4];

struct HostPad {
    u32 seq, screen;
    std::array<u8, 12> pad;
};
std::deque<HostPad> s_hostPads;  // in order, not applied yet
u32 s_hostNext = 0;                                           // the next to apply (0: none yet)
bool s_hostInGameplay = false;

bool inGameplay() {
    const u32 mode = rd32(kGameState);
    return mode == 0x10 || mode == 0x11;
}
// Which screen the game is on, the same in both games: the menus' (advertiseD.rel, module 91:
// CurrentMenu, CurrentSubMenu), an event (eventD.rel, module 80), or anything else (loading).
u32 screenKey() {
    if (g_relLinked[91]) {
        const u32 at = g_relBase[91][6] + 0x62054;
        return 0x10000000u | (rd32(at) & 0xFFF) << 12 | (rd32(at + 4) & 0xFFF);
    }
    if (g_relLinked[80]) return 0x20000000u;
    return 0;
}
void hostPadArrived(const PadMsg& p) {
    s_hostInGameplay = p.inGameplay != 0;
    for (u32 k = 0; k < 4; k++) {
        const u32 seq = p.seq - 3 + k;
        if (p.seq < 3 + k - 3 || (s_hostNext && seq < s_hostNext)) continue;
        auto it = s_hostPads.begin();
        while (it != s_hostPads.end() && it->seq < seq) ++it;
        if (it != s_hostPads.end() && it->seq == seq) continue;
        HostPad h{seq, p.screens[k], {}};
        std::memcpy(h.pad.data(), p.pads[k], 12);
        s_hostPads.insert(it, h);
    }
    if (!s_hostNext && !s_hostPads.empty()) s_hostNext = s_hostPads.front().seq;
    while (s_hostPads.size() > 3600) s_hostPads.pop_front();  // (a minute)
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
    setObjectsReset();
    s_ownValid = false;
    if (const u32 e = player(kMainCharObj1, 1))
        for (int i = 0; i < 3; i++) s_spawn[i] = flt(rd32(e + 0x14 + 4 * i));
    log("[coop] level %u: player 1 %u, the partner's %u as player 2 (%08X)\n", level, local, partner, player(kMainCharacter, 1));
}
}  // namespace

// From coop::init, in a co-op session.
void gameInit() {
    interpReplace(0, kLoadCharacters, loadCharacters);
    interpReplace(0, kDamagePlayer, damagePlayer);
    interpReplace(0, kKillPlayer, killPlayer);
}

// PADRead (src/runtime/hle_hw.cpp), every frame: see "player 1 drives the menus" above.
void padFilter(u8 pads[4][12]) {
    if (!active()) return;
    const bool connected = link().state() == coopnet::State::Connected;
    if (link().host()) {
        std::memmove(s_myPads[0], s_myPads[1], 36);
        std::memcpy(s_myPads[3], pads[0], 12);
        std::memmove(s_myScreens, s_myScreens + 1, 12);
        s_myScreens[3] = screenKey();
        PadMsg m{};
        m.seq = ++s_padSeq;
        m.inGameplay = inGameplay();
        std::memcpy(m.pads, s_myPads, sizeof m.pads);
        std::memcpy(m.screens, s_myScreens, sizeof m.screens);
        if (connected) link().send(kMsgPad, &m, sizeof m);
        else if (!m.inGameplay) std::memset(pads[0], 0, 10);  // (waiting for the partner: buttons, sticks, triggers off)
        return;
    }
    drainMessages();
    if (inGameplay()) return;  // the joiner plays their own stage
    // The host's pad, in order, as the host had it on this same screen: the games reach screens at
    // different moments (loading, logos), so a frame waits until this game shows its screen too;
    // frames from screens this game has gone past are dropped. Waiting, nothing is pressed.
    const u32 mine = screenKey();
    while (!s_hostPads.empty() && s_hostPads.front().seq < s_hostNext) s_hostPads.pop_front();
    size_t at = 0;
    if (!mine) at = !s_hostPads.empty() && s_hostPads.front().screen == 0 ? 0 : s_hostPads.size();  // (an unnamed screen
    else                                                                                             //  never skips ahead)
        while (at < s_hostPads.size() && s_hostPads[at].screen != mine) at++;
    std::array<u8, 12> pad{};
    if (at < s_hostPads.size()) {
        s_hostPads.erase(s_hostPads.begin(), s_hostPads.begin() + ptrdiff_t(at));  // (screens gone past)
        pad = s_hostPads.front().pad;
        s_hostNext = s_hostPads.front().seq + 1;
        s_hostPads.pop_front();
    }
    static const bool s_log = std::getenv("SA2_COOP_LOG") != nullptr;
    if (s_log && (pad[0] | pad[1]))
        log("[coop] host pad %02X%02X on screen %08X (queue %zu, next screen %08X)\n", pad[0], pad[1], mine, s_hostPads.size(),
            s_hostPads.empty() ? 0u : s_hostPads.front().screen);
    std::memcpy(pads[0], pad.data(), 10);
    pads[0][10] = 0;  // (connected)
}

}  // namespace rt::coop
