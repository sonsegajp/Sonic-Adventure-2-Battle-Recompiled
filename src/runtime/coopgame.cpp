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
#include "runtime/gx.h"
#include "runtime/interp.h"
#include "runtime/runtime.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <iterator>
#include <mutex>
#include <array>
#include <atomic>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace rt {
extern std::atomic<bool> g_relLinked[256];  // src/runtime/dispatch.cpp
}

namespace rt::coop {

namespace {
constexpr u32 kLoadCharacters = 0x8001DEA8;
constexpr u32 kPlayerSetup = 0x8001D74C;  // per-player setup LoadCharacters runs after each loader
constexpr u32 kCurrentLevel = 0x803AD820;
constexpr u32 kLastBoss = 66;  // Final Hazard
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
// The sender finished the stage ("the goal", below).
constexpr u8 kMsgFinished = coopnet::kFirstGameMessage + 3;
struct FinishedMsg {
    u16 level;
    u8 pad_[2];
};
// The treasure hunt as the sender has it ("treasure hunts", below): sent on a change and every half
// second (a lost one is made up for).
constexpr u8 kMsgTreasure = coopnet::kFirstGameMessage + 4;
struct TreasureMsg {
    u16 level;
    u8 hasPicks;     // the host's: the pieces it hid (picks valid)
    u8 found;        // bit k: piece k found
    u32 visit;       // the host's count of hunts begun (a found mask from an earlier one is not this one's)
    u8 picks[0x50];  // EmeManObj2 +0xC..+0x5C: the two spare entries and Piece1..3
};
// The sender's character colours (settings().colours): on a change and every two seconds.
constexpr u8 kMsgColours = coopnet::kFirstGameMessage + 5;
struct ColoursMsg {
    s32 colours[kColourSlots];
};
// The boss's health in the sender's game ("bosses", below): on a change and every quarter second.
constexpr u8 kMsgBoss = coopnet::kFirstGameMessage + 6;
struct BossMsg {
    u16 level;
    u8 pad_[2];
    float health;
};
// A chat line (T: coopmenu.cpp), with the sender's name in their character's colour.
constexpr u8 kMsgChat = coopnet::kFirstGameMessage + 7;
struct ChatMsg {
    u32 colour;
    char name[25];
    char text[159];
};

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
// The partner's player slot: 1 ("player 2"); 2 in a rival stage, whose rival (the computer's) the
// stage loads as player 2 itself and drives by slot 1.
int s_p2 = 1;
// This game's own player's slot: 0 - but 1 for the joiner in the last boss (Super Shadow there: the
// stage has both supers in slots 0 and 1, and one game plays each).
int s_p1 = 0;
std::atomic<int> s_partnerCharacter{-1};  // the partner's character now (their last frame's)
bool s_lastBossHooked = false;  // (the last boss's supers' code hooked: lastBossFrame)
bool s_rivalPainted = false;  // (the rival's grey planned: it is loaded after the players)

double nowS() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
u32 player(u32 table, int n) { return rd32(table + u32(n) * 4); }
void setObjectsFrame();  // (shared objects, below)
void setObjectsReset();
void beforePlayer1();  // (damage, below)
void afterPlayer1();
void checkHitOnPartner();
void hostPadArrived(const PadMsg& p);  // (player 1 drives, below)
void partnerFinished(u16 level);       // (the goal, below)
void partnerInLevel(u16 level);
void goalFrame(Ctx& c);
void treasureArrived(const TreasureMsg& t);  // (treasure hunts, below)
void treasureFrame(Ctx& c);
void partnerColours(const ColoursMsg& m);  // (colours, below)
void coloursFrame();
void partnerBoss(const BossMsg& m);  // (bosses, below)
void bossFrame(Ctx& c);
bool bossStage(u32 level);
void colourPlayers();


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
        if (type == kMsgFinished && data.size() >= sizeof(FinishedMsg)) {
            FinishedMsg f;
            std::memcpy(&f, data.data(), sizeof f);
            partnerFinished(f.level);
            continue;
        }
        if (type == kMsgTreasure && data.size() >= sizeof(TreasureMsg)) {
            TreasureMsg t;
            std::memcpy(&t, data.data(), sizeof t);
            treasureArrived(t);
            continue;
        }
        if (type == kMsgColours && data.size() >= sizeof(ColoursMsg)) {
            ColoursMsg m;
            std::memcpy(&m, data.data(), sizeof m);
            partnerColours(m);
            continue;
        }
        if (type == kMsgChat && data.size() >= sizeof(ChatMsg)) {
            ChatMsg m;
            std::memcpy(&m, data.data(), sizeof m);
            m.name[sizeof m.name - 1] = m.text[sizeof m.text - 1] = 0;
            chatArrived(m.name, m.colour & 0xFFFFFF, m.text);
            audioBeep();
            continue;
        }
        if (type == kMsgBoss && data.size() >= sizeof(BossMsg)) {
            BossMsg m;
            std::memcpy(&m, data.data(), sizeof m);
            partnerBoss(m);
            continue;
        }
        if (type != kMsgPlayer || data.size() < sizeof(PlayerMsg)) continue;
        PlayerMsg m;
        std::memcpy(&m, data.data(), sizeof m);
        partnerInLevel(m.level);
        s_partnerCharacter = m.character;
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
    const u32 e = player(kMainCharObj1, s_p1), p = player(kMainCharObj2, s_p1);
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
    const u32 pad = kControllers + kControllerSize * u32(s_p1);  // player 1's controller, as the character read it
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
    const u32 e = player(kMainCharObj1, s_p2), p = player(kMainCharObj2, s_p2);
    for (int i = 0; i < 3; i++) {
        wr32(e + 8 + 4 * i, u32(m.rot[i])), wr32(e + 0x14 + 4 * i, bits32(m.pos[i]));
        wr32(p + 0x64 + 4 * i, bits32(m.speed[i]));
    }
    const u32 pad = kControllers + kControllerSize * u32(s_p2);
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
// How far apart the two stand side by side: further with a walker (twice a character's width).
float spacing() {
    const u32 a = player(kMainCharObj2, s_p1), b = player(kMainCharObj2, s_p2);
    return (a && rd8(a + 1) >= 6) || (b && rd8(b + 1) >= 6) ? 34.f : 18.f;
}
// The stage over, each game puts its player on the stage's end spot to pose: there the partner
// (where they stand in their game - the same spot) is shown beside this game's player, to its
// right, not inside it. Facing Rotation.y = a, forward is (cos a, 0, -sin a).
bool stageOver();  // (the goal, below)
bool rivalStage(u32 level);
void besideAtTheEnd(u32 e2) {
    const u32 e1 = player(kMainCharObj1, s_p1);
    if (!e1 || !stageOver()) return;
    float d2 = 0;
    for (int i = 0; i < 3; i += 2) {
        const float d = flt(rd32(e2 + 0x14 + 4 * u32(i))) - flt(rd32(e1 + 0x14 + 4 * u32(i)));
        d2 += d * d;
    }
    const float apart = spacing();
    if (d2 > apart * apart * 1.7f) return;  // (apart already)
    const float a = float(rd32(e1 + 0xC) & 0xFFFF) * (6.2831853f / 65536.f);
    wr32(e2 + 0x14, bits32(flt(rd32(e1 + 0x14)) + apart * std::sin(a)));
    wr32(e2 + 0x18, rd32(e1 + 0x18));
    wr32(e2 + 0x1C, bits32(flt(rd32(e1 + 0x1C)) + apart * std::cos(a)));
    wr32(e2 + 0xC, rd32(e1 + 0xC));  // (turned the same way: both to the camera)
}
void afterPlayer2(const PlayerMsg& m) {
    const u32 e = player(kMainCharObj1, s_p2), p = player(kMainCharObj2, s_p2);
    saveOwn(e, p);
    for (int i = 0; i < 3; i++) wr32(e + 8 + 4 * i, u32(m.rot[i])), wr32(e + 0x14 + 4 * i, bits32(m.pos[i]));
    wr8(e, m.action), wr8(e + 1, m.nextAction), wr16(e + 4, u16(m.status));
    for (u32 i = 0; i < sizeof m.animInfo; i++) wr8(p + 0x174 + i, m.animInfo[i]);
    wr32(p + 0x184, bits32(m.nframe));  // (the frame, blended between packets)
    besideAtTheEnd(e);
}

// The stages have one start, where the game puts player 1 as the stage begins: there the joiner's
// steps to the side (spacing(); else the two stand inside each other). Facing Rotation.y = a,
// forward is (cos a, 0, -sin a).
u32 s_startFrames = 0;
void startBeside() {
    if (link().host() || rd16(kCurrentLevel) == kLastBoss || ++s_startFrames > 2) return;
    const u32 e = player(kMainCharObj1, s_p1);
    const float a = float(rd32(e + 0xC) & 0xFFFF) * (6.2831853f / 65536.f);
    const float step = spacing() / 2;  // (over two frames)
    wr32(e + 0x14, bits32(flt(rd32(e + 0x14)) + step * std::sin(a)));
    wr32(e + 0x1C, bits32(flt(rd32(e + 0x1C)) + step * std::cos(a)));
}

// Each character's main function (shared: Sonic and Shadow run the same code): after player 1's,
// it is sent; player 2's runs as above.
void characterMain(Ctx& c, u32 entry) {
    const u32 obj = c.r[3];
    if (obj == player(kMainCharacter, s_p2)) {
        const u32 e = player(kMainCharObj1, s_p2), p = player(kMainCharObj2, s_p2);
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
            const u32 e1 = player(kMainCharObj1, s_p1);
            log("[coop] frame %u: player 1 at %.0f %.0f %.0f, partner at %.0f %.0f %.0f, action %u, anim %d\n", m.frame,
                flt(rd32(e1 + 0x14)), flt(rd32(e1 + 0x18)), flt(rd32(e1 + 0x1C)), m.pos[0], m.pos[1], m.pos[2], m.action, m.anim);
            if (const u32 r = s_p2 == 2 ? player(kMainCharObj1, 1) : 0)
                log("[coop]   the rival at %.0f %.0f %.0f; player 2 drawn at %.0f %.0f %.0f\n", flt(rd32(r + 0x14)), flt(rd32(r + 0x18)),
                    flt(rd32(r + 0x1C)), flt(rd32(e + 0x14)), flt(rd32(e + 0x18)), flt(rd32(e + 0x1C)));
        }
        return;
    }
    const bool p1 = obj == player(kMainCharacter, s_p1);
    if (p1) startBeside(), beforePlayer1();
    interpOriginal(c, entry);
    if (p1) afterPlayer1(), sendPlayer1(), setObjectsFrame(), checkHitOnPartner(), treasureFrame(c), bossFrame(c), goalFrame(c), coloursFrame();
}
bool rivalStage(u32 level);  // (below)
// ---- the partner's name tag: where their head is on the screen, as they are drawn ----
// The camera's view is Ninja's current matrix (pointer at 803AE0C4, 3x4) as a display function
// starts; the projection is the one the game has set (gxProject).
std::mutex s_tagLock;
float s_tagX = 0, s_tagY = 0;
double s_tagAt = -100;
void partnerNameTag() {
    const u32 e = player(kMainCharObj1, s_p2), m = rd32(0x803AE0C4);
    if (!e || !m) return;
    const int ch = s_partnerCharacter.load();
    const float above = ch == 6 || ch == 7 ? 26.f : 13.f;  // (from the body's centre to over the head; a walker's taller)
    const float w[3] = {flt(rd32(e + 0x14)), flt(rd32(e + 0x18)) + above, flt(rd32(e + 0x1C))};
    float eye[3];
    for (int r = 0; r < 3; r++)
        eye[r] = flt(rd32(m + u32(r) * 16)) * w[0] + flt(rd32(m + u32(r) * 16 + 4)) * w[1] + flt(rd32(m + u32(r) * 16 + 8)) * w[2] +
                 flt(rd32(m + u32(r) * 16 + 12));
    float x, y;
    if (!gxProject(eye, x, y)) return;
    std::lock_guard g(s_tagLock);
    s_tagX = x, s_tagY = y, s_tagAt = nowS();
    static const bool s_log = std::getenv("SA2_COOP_LOG") != nullptr;
    static u32 s_n = 0;
    if (s_log && s_n++ % 120 == 0) log("[coop] name tag at %.0f, %.0f\n", x, y);
}
// The last boss's player manager (its data: Boss_Last2D.rel section 6 +0x4A8; byte 0 the super in
// play, byte 1 its state - 1: a swap to the other asked for, 2-4 the swap), or 0.
u32 lastBossManager() { return rd16(kCurrentLevel) == kLastBoss && g_relLinked[89] ? rd32(g_relBase[89][6] + 0x4A8) : 0; }
void characterDisplay(Ctx& c, u32 entry) {
    if (c.r[3] == player(kMainCharacter, s_p2) && !s_player2Shown) return;  // not drawn until the partner is here
    // A rival stage: the rival (the computer's, slot 1) drawn in grey, the players in their colours -
    // painted as drawn, the rival being the same character as one of them (the same textures).
    int painted = -1;
    for (int n = 0; s_p2 == 2 && n < 3 && painted < 0; n++)
        if (c.r[3] == player(kMainCharacter, n)) painted = n;
    if (c.r[3] == player(kMainCharacter, s_p2)) partnerNameTag();
    // The last boss draws only the super it has active (its manager's byte 0): the partner's, the
    // other one, is drawn as the active one.
    const u32 manager = c.r[3] == player(kMainCharacter, s_p2) ? lastBossManager() : 0;
    if (manager) wr8(manager, u8(s_p2));
    if (painted >= 0) drawPainted(painted);
    interpOriginal(c, entry);
    if (painted >= 0) drawPainted(-1);
    if (manager) wr8(manager, u8(s_p1));
}

// A function hooked wherever it is: main.dol's by its address, a REL's (the last boss's supers are
// the stage's own) by its module and section-1 offset.
void hookAt(u32 addr, InterpHandler fn) {
    if (addr < 0x80400000u) return void(interpReplace(0, addr, fn));
    u32 base = 0, module = 0;
    for (u32 m = 1; m < 256; m++)
        if (g_relLinked[m] && g_relBase[m][1] && g_relBase[m][1] <= addr && g_relBase[m][1] > base) base = g_relBase[m][1], module = m;
    if (module) interpReplace(module, 0x01000000u | (addr - base), fn);
}
void hookCharacter(int n) {
    const u32 obj = player(kMainCharacter, n);
    if (!obj) return;
    const u32 main = rd32(obj + 0x10), display = rd32(obj + 0x14);
    if (main && s_hookedMain.insert(main).second) hookAt(main, characterMain);
    if (display && s_hookedDisplay.insert(display).second) hookAt(display, characterDisplay);
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
constexpr u32 kRingCount = 0x801CC1D0;  // s16 per player (PC 0174B028: the block at 801CC168 is PC's at 0174AFC0)
double s_lastHitSent = -100;
s16 s_ringsKept = -1;

void checkHitOnPartner() {
    if (!s_player2Shown) return;
    const u32 e1 = player(kMainCharObj1, s_p1), e2 = player(kMainCharObj1, s_p2);
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

bool waitingAtGoal();  // (the goal, below)
void damagePlayer(Ctx& c, u32 entry) {
    if (c.r[3] && c.r[3] == player(kMainCharObj1, s_p2)) return void(c.r[3] = 0);  // (the partner: not here)
    if (c.r[3] && c.r[3] == player(kMainCharObj1, s_p1) && waitingAtGoal()) return void(c.r[3] = 0);
    interpOriginal(c, entry);
}
void killPlayer(Ctx& c, u32 entry) {
    if (c.r[3] == u32(s_p2) && player(kMainCharacter, s_p2)) return;  // (the partner)
    if (c.r[3] == u32(s_p1) && waitingAtGoal()) return;
    interpOriginal(c, entry);
}
// Around player 1's update: a hit from the partner becomes its own knockback, its rings kept.
void beforePlayer1() {
    if (!s_hitPending) return;
    s_hitPending = false;
    const u32 e1 = player(kMainCharObj1, s_p1);
    if (!e1 || (rd16(e1 + 4) & 0x4)) return;
    s_ringsKept = s16(rd16(kRingCount + 2 * u32(s_p1)));
    if (s_ringsKept <= 0) wr16(kRingCount + 2 * u32(s_p1), 1);
    wr16(e1 + 4, u16(rd16(e1 + 4) | 0x4));
}
void afterPlayer1() {
    if (s_ringsKept < 0) return;
    wr16(kRingCount + 2 * u32(s_p1), u16(s_ringsKept));
    s_ringsKept = -1;
}

// ---- the goal: both finish before either goes on ----
// AwardWin (8001C810, r3 = the player) ends the stage for a player: it turns their controller off
// (ControllerEnabled, 801CC1A7 + n, by 80022E24), stops the timer (801CC182, by 80062C4C), marks
// the stage won and brings up the results. In co-op the first to finish is only marked finished -
// stopped where they are the same way (controller off, timer stopped), unhurt - and the partner is
// told; once both have finished, both games award the stage. Player 2 reaching a goal in this game
// ends nothing: the partner's own game tells when they finish.
constexpr u32 kAwardWin = 0x8001C810;
constexpr u32 kTimerStopped = 0x801CC182, kControllerEnabled = 0x801CC1A7;
bool s_coopLevel = false;    // this stage has the partner as player 2
bool s_finished = false;     // this game's player has reached the goal
bool s_awarded = false;      // ... and the stage was awarded
bool s_partnerDone = false;  // the partner has finished s_partnerDoneLevel
u16 s_partnerDoneLevel = 0;
bool s_releasing = false;    // (AwardWin let through to the game's own)

bool waitingAtGoal() { return s_coopLevel && s_finished && !s_awarded; }
bool stageOver() { return s_coopLevel && s_finished; }

void partnerFinished(u16 level) {
    if (s_partnerDone && s_partnerDoneLevel == level) return;
    s_partnerDone = true, s_partnerDoneLevel = level;
    log("[coop] the partner finished level %u\n", level);
    if (!s_finished) notify(link().partnerName() + " reached the goal");
}
void partnerInLevel(u16 level) {
    if (s_partnerDone && level != s_partnerDoneLevel) s_partnerDone = false;  // (they went on: a new stage)
}

void award(Ctx& c, u32 entry) {
    s_releasing = true;
    if (entry) interpOriginal(c, entry);
    else guestCall(c, kAwardWin, {u32(s_p1)});
    s_releasing = false;
    s_awarded = true, s_partnerDone = false;
    log("[coop] stage awarded\n");
}
// Whether to award now: the partner has finished this stage too - or is gone (no waiting forever).
bool bothDone() {
    return (s_partnerDone && s_partnerDoneLevel == rd16(kCurrentLevel)) || link().state() != coopnet::State::Connected;
}

void awardWin(Ctx& c, u32 entry) {
    if (!s_coopLevel || s_releasing) return interpOriginal(c, entry);
    if (c.r[3] == u32(s_p2) && rd16(kCurrentLevel) != kLastBoss) return;  // (the partner)
    if (c.r[3] != u32(s_p1) && rd16(kCurrentLevel) != kLastBoss) return interpOriginal(c, entry);  // (a rival's win: the game's own)
    if (!s_finished) {
        s_finished = true;
        const FinishedMsg f{u16(rd16(kCurrentLevel)), {}};
        link().send(kMsgFinished, &f, sizeof f);
        log("[coop] reached the goal of level %u\n", f.level);
    }
    if (!s_awarded && bothDone()) award(c, entry);
}
// Once a frame in the stage (with player 1's update): holding at the goal, or awarding.
u32 s_stageFrames = 0;  // frames of player 1's update since the stage loaded
void goalFrame(Ctx& c) {
    // SA2_COOP_TEST_CHAT=<n>:<text> (test runs): a chat line sent n frames into the stage.
    static const char* s_testChat = std::getenv("SA2_COOP_TEST_CHAT");
    if (s_testChat && u32(std::atoi(s_testChat)) == s_stageFrames + 1 && std::strchr(s_testChat, ':')) sendChat(std::strchr(s_testChat, ':') + 1);
    // SA2_COOP_TEST_GOAL=<n> (test runs): player 1 reaches the goal n frames into the stage.
    static const u32 s_testGoal = std::getenv("SA2_COOP_TEST_GOAL") ? u32(std::atoi(std::getenv("SA2_COOP_TEST_GOAL"))) : 0;
    if (++s_stageFrames == s_testGoal && !s_finished) guestCall(c, kAwardWin, {u32(s_p1)});
    // the same boss: beaten in the partner's game, beaten here
    if (!s_finished && s_coopLevel && bossStage(rd16(kCurrentLevel)) && s_partnerDone && s_partnerDoneLevel == rd16(kCurrentLevel)) {
        log("[coop] the boss was beaten in the partner's game\n");
        guestCall(c, kAwardWin, {u32(s_p1)});
    }
    if (!waitingAtGoal()) return;
    if (bothDone()) return award(c, 0);
    wr8(kControllerEnabled + u32(s_p1), 0), wr8(kTimerStopped, 1);
}
}  // namespace

namespace {
// ---- bosses: one boss's health for both ----
// Each boss stage's REL has the same health gauge, made by a function given a pointer to the boss's
// health (a float; Big Foot's: m82 1:5409C, called with the boss's work + 0x28): hooked there, it
// gives the boss's health in this game. Both games send theirs and each takes the lower, so a hit in
// either game (the partner's player 2 hitting it here as well) takes it down in both, and never
// twice. A boss beaten in the partner's game is beaten here too (the goal, above).
struct BossGauge {
    u32 module, addr;  // the gauge's maker: REL module, section << 24 | offset
};
const BossGauge kBossGauges[] = {{82, 0x0105409C}, {83, 0x0104C618}, {84, 0x0104A1E0}, {85, 0x01056378},
                                 {86, 0x01055084}, {87, 0x0103DCD4}, {88, 0x01053E54},
                                 // the rivals' (in their stages' RELs)
                                 {19, 0x01022984}, {20, 0x0103361C}, {29, 0x0102CF50}, {33, 0x0102CB08}, {42, 0x01020470},
                                 {89, 0x0101B3E8}};  // (Final Hazard's)
u32 s_bossHealth = 0;  // the boss's health in this game (0: none yet)
u16 s_bossLevel = 0xFFFF;
float s_partnerHealth = -1;
u16 s_partnerHealthLevel = 0xFFFF;
float s_healthSent = -1;
u32 s_bossFrames = 0;

// Big Foot, Hot Shot, Flying Dog, King Boom Boo, Egg Golem (both), Biolizard, Final Hazard - and the rivals.
bool bossStage(u32 level) { return (level >= 60 && level <= 67) || rivalStage(level); }

void bossGauge(Ctx& c, u32 entry) {
    const u32 health = c.r[4];
    interpOriginal(c, entry);
    if (!s_coopLevel || (health & 0xFE000000) != 0x80000000) return;
    s_bossHealth = health, s_bossLevel = u16(rd16(kCurrentLevel)), s_healthSent = -1;
    log("[coop] the boss's health: %.1f (at %08X)\n", flt(rd32(health)), health);
}
void partnerBoss(const BossMsg& m) { s_partnerHealth = m.health, s_partnerHealthLevel = m.level; }
// Once a frame in the stage (with player 1's update).
void bossFrame(Ctx& c) {
    (void)c;
    const u16 level = u16(rd16(kCurrentLevel));
    if (!s_bossHealth || s_bossLevel != level) return;
    float health = flt(rd32(s_bossHealth));
    // SA2_COOP_TEST_BOSS=<n> (test runs): the boss here loses 3 of its health n frames into the stage.
    static const u32 s_testBoss = std::getenv("SA2_COOP_TEST_BOSS") ? u32(std::atoi(std::getenv("SA2_COOP_TEST_BOSS"))) : 0;
    if (s_testBoss && s_stageFrames == s_testBoss) health = std::max(health - 3, 0.f), wr32(s_bossHealth, bits32(health));
    // (never 0 from the partner's: only a hit kills a boss - its health at 0 alone does nothing - so
    // it is left a sliver for the next hit here; the partner's kill ends it here anyway)
    const float partner = std::max(s_partnerHealth, 0.01f);
    if (s_partnerHealthLevel == level && s_partnerHealth >= 0 && partner < health) {
        log("[coop] the boss's health: %.1f -> %.1f (the partner's)\n", health, s_partnerHealth);
        wr32(s_bossHealth, bits32(partner));
        health = partner;
    }
    if (health == s_healthSent && ++s_bossFrames % 15) return;
    const BossMsg m{level, {}, health};
    link().send(kMsgBoss, &m, sizeof m);
    s_healthSent = health;
}

// ---- colours (coopcolour.cpp): yours on your player 1, the partner's on player 2 ----
std::atomic<bool> s_coloursDirty{true};  // yours changed: redone and sent
s32 s_partnerColours[kColourSlots] = {-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1};
bool s_partnerColoursDirty = false;
u32 s_coloursFrames = 0;

void colourPlayers() {
    recolourPlayer(s_p1, settings().colours);
    if (!player(kMainCharObj2, s_p2)) return;
    recolourPlayer(s_p2, s_partnerColours);
    if (s_p2 == 2)  // (a rival stage: see characterDisplay)
        planDrawn(0, settings().colours), planDrawn(1, nullptr), planDrawn(2, s_partnerColours);
}
void partnerColours(const ColoursMsg& m) {
    if (std::memcmp(m.colours, s_partnerColours, sizeof s_partnerColours)) {
        std::memcpy(s_partnerColours, m.colours, sizeof s_partnerColours);
        s_partnerColoursDirty = true;
    }
}
// Once a frame in the stage (with player 1's update).
void coloursFrame() {
    if (s_p2 == 2 && !s_rivalPainted && player(kMainCharObj2, 1)) colourPlayers(), s_rivalPainted = true;
    const bool mine = s_coloursDirty.exchange(false);
    if (mine) recolourPlayer(s_p1, settings().colours);
    if (mine && s_p2 == 2) planDrawn(0, settings().colours);
    if (s_partnerColoursDirty && s_coopLevel) colourPlayers(), s_partnerColoursDirty = false;
    if (!mine && ++s_coloursFrames % 120) return;
    ColoursMsg m;
    std::memcpy(m.colours, settings().colours, sizeof m.colours);
    link().send(kMsgColours, &m, sizeof m);
}
}  // namespace

void coloursChanged() { s_coloursDirty = true; }

// A character's colour for names: the player's own for it (the character's main area: Sonic's
// blue, Shadow's stripes...), else the character's. The walkers take their pilots'; the supers gold.
unsigned characterColour(int character, const int* colours) {
    static const int kSlot[11] = {0, 2, 4, 5, 6, 7, 4, 5, -1, -1, -1};
    static const unsigned kOwn[11] = {0x4070FF, 0xE83030, 0xF0B030, 0xE83030, 0xF04040, 0xF4F4F4, 0xF0B030, 0xE83030,
                                      0xF080C0, 0xF8E040, 0xF8E8A0};
    if (character < 0 || character > 10) return 0xFFFFFF;
    const int slot = kSlot[character];
    return slot >= 0 && colours[slot] >= 0 ? unsigned(colours[slot]) : kOwn[character];
}
unsigned myNameColour() {
    const u32 obj2 = s_coopLevel ? player(kMainCharObj2, s_p1) : 0;
    return characterColour(obj2 ? int(rd8(obj2 + 1)) : int(rd32(kCurrentCharacter)), settings().colours);
}
void sendChat(const std::string& text) {
    ChatMsg m{};
    m.colour = myNameColour();
    std::snprintf(m.name, sizeof m.name, "%s", settings().name.c_str());
    std::snprintf(m.text, sizeof m.text, "%s", text.c_str());
    link().send(kMsgChat, &m, sizeof m);
    chatArrived(m.name, m.colour, m.text);
    audioBeep();
}
bool nameTag(float& x, float& y, unsigned& colour) {
    {
        std::lock_guard g(s_tagLock);
        if (nowS() - s_tagAt > 0.25) return false;  // (not drawn lately: not on the screen, or gone)
        x = s_tagX, y = s_tagY;
    }
    colour = characterColour(s_partnerCharacter.load(), s_partnerColours);
    return true;
}

std::string gameStatus() {
    if (!waitingAtGoal()) return {};
    return "Waiting for " + link().partnerName() + " to reach the goal";
}

namespace {
// ---- treasure hunts: one hunt for both ----
// The emerald manager (EmeManObj2 at *803ADA88, its main 8008E4AC) hides three pieces as the stage
// starts (actions 2-3, in one call: picked at random from the stage's hiding places) and then hunts
// them (action 4). Piece1..3 are at +0x2C, 0x10 each: id, sub, -, -, position; a found piece's id is -2, and with
// all three found the manager awards the stage. GotPiece (8008C988: id, sub, player) finds one: id
// -2, the icon, the sound and the finder's voice, the score.
// In co-op both games hunt the host's pieces - the joiner's manager waits before hiding for them and
// takes them over its own - and a piece found in either game is found in both: the other game finds
// it as player 2's (the partner's voice). A life lost (the stage restarting) hides the same pieces,
// those found staying found; a new visit to the stage (from the menus) hides new ones.
constexpr u32 kEmeraldManager = 0x803ADA88, kEmeraldManagerMain = 0x8008E4AC, kGotPiece = 0x8008C988;
u16 s_picksLevel = 0xFFFF;  // the stage s_picks are for (0xFFFF: none)
u8 s_picks[0x50];
u32 s_visit = 0;            // the host's hunts begun (the joiner: the one its picks are from)
u8 s_foundShared = 0;       // bit k: piece k found, in either game
u8 s_foundSent = 0xFF;      // as last sent
u32 s_treasureFrames = 0;

u32 emeraldManager() { return rd32(kEmeraldManager); }
s8 pieceId(u32 m, int k) { return s8(rd8(m + 0x2C + 0x10 * u32(k))); }

void treasureArrived(const TreasureMsg& t) {
    if (link().host()) {
        if (t.visit == s_visit && t.level == s_picksLevel) s_foundShared |= t.found & 7;
        return;
    }
    if (!t.hasPicks) return;
    if (t.visit != s_visit || t.level != s_picksLevel) {
        std::memcpy(s_picks, t.picks, sizeof s_picks);
        s_picksLevel = t.level, s_visit = t.visit, s_foundShared = 0;
        log("[coop] the host's pieces for level %u: %d/%d %d/%d %d/%d\n", t.level, s8(t.picks[0x20]), s8(t.picks[0x21]),
            s8(t.picks[0x30]), s8(t.picks[0x31]), s8(t.picks[0x40]), s8(t.picks[0x41]));
    }
    s_foundShared |= t.found & 7;
}

void emeraldManagerMain(Ctx& c, u32 entry) {
    const u32 m = emeraldManager();
    // (action 2 sets 3 and hides them in the same call; 3 is the hiding itself)
    if (!s_coopLevel || !m || (s8(rd8(m)) != 2 && s8(rd8(m)) != 3)) return interpOriginal(c, entry);
    const u16 level = u16(rd16(kCurrentLevel));
    const bool have = s_picksLevel == level;
    if (!link().host() && !have && link().state() == coopnet::State::Connected) return;  // (waits for the host's)
    interpOriginal(c, entry);
    if (s8(rd8(m)) != 4) return;  // (still hiding them)
    if (have) {  // the host's (or, after a restart, the same as before)
        for (u32 i = 0; i < sizeof s_picks; i++) wr8(m + 0xC + i, s_picks[i]);
        for (int k = 0; k < 3; k++)
            if ((s_foundShared >> k & 1) && pieceId(m, k) != -2) {
                wr8(m + 0x2C + 0x10 * u32(k), 0xFE), wr8(m + 0x2D + 0x10 * u32(k), 0);
                wr8(m + 5, rd8(m + 5) - 1);  // (pieces left)
            }
    } else if (link().host()) {  // new ones
        for (u32 i = 0; i < sizeof s_picks; i++) s_picks[i] = u8(rd8(m + 0xC + i));
        s_picksLevel = level, s_visit++, s_foundShared = 0;
    }
    s_foundSent = 0xFF;
    log("[coop] pieces hidden in level %u: %d/%d %d/%d %d/%d (%s)\n", level, pieceId(m, 0), s8(rd8(m + 0x2D)), pieceId(m, 1),
        s8(rd8(m + 0x3D)), pieceId(m, 2), s8(rd8(m + 0x4D)), have ? "the host's" : "this game's");
}

// Once a frame in the stage (with player 1's update).
void treasureFrame(Ctx& c) {
    const u32 m = emeraldManager();
    if (!m || s8(rd8(m)) < 4 || s_picksLevel != rd16(kCurrentLevel)) return;
    // SA2_COOP_TEST_PIECE=<n> (test runs): player 1 finds the first piece left n frames into the stage.
    static const u32 s_testPiece = std::getenv("SA2_COOP_TEST_PIECE") ? u32(std::atoi(std::getenv("SA2_COOP_TEST_PIECE"))) : 0;
    if (s_testPiece && s_stageFrames == s_testPiece)
        for (int k = 0; k < 3; k++)
            if (pieceId(m, k) != -2) {
                guestCall(c, kGotPiece, {u32(s32(pieceId(m, k))), u32(s32(s8(rd8(m + 0x2D + 0x10 * u32(k))))), 0});
                break;
            }
    u8 here = 0;
    for (int k = 0; k < 3; k++) here |= u8((pieceId(m, k) == -2) << k);
    for (int k = 0; k < 3; k++)  // found in the other game: found here too, as the partner's
        if ((s_foundShared & ~here) >> k & 1) {
            log("[coop] the partner found piece %d (%d/%d)\n", k, pieceId(m, k), s8(rd8(m + 0x2D + 0x10 * u32(k))));
            guestCall(c, kGotPiece, {u32(s32(pieceId(m, k))), u32(s32(s8(rd8(m + 0x2D + 0x10 * u32(k))))), 1});
            if (pieceId(m, k) == -2) here |= u8(1 << k);
        }
    s_foundShared |= here;
    if (s_foundShared == s_foundSent && ++s_treasureFrames % 30) return;
    TreasureMsg t{};
    t.level = u16(s_picksLevel), t.hasPicks = link().host(), t.found = s_foundShared, t.visit = s_visit;
    std::memcpy(t.picks, s_picks, sizeof t.picks);
    link().send(kMsgTreasure, &t, sizeof t);
    s_foundSent = s_foundShared;
}
// Back in the menus: the next visit to a stage is a new hunt.
void treasureMenus() {
    static bool s_inMenus = false;
    const bool menus = g_relLinked[91];
    if (menus && !s_inMenus) s_picksLevel = 0xFFFF, s_foundShared = 0;
    s_inMenus = menus;
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
std::unordered_map<u32, std::array<u8, 12>> s_exitPress;      // per screen: the host's last press on it
u64 s_padCalls = 0, s_retryAt = 0;                            // (pressing it again: see padFilter)
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

// SA2_TEST_STAGE=<from>:<to>:<character> (test runs): the stage select's <from> starts stage <to>
// as <character> (StageSelectLevels, advertiseD.rel section 5 +0x6E64: 33 of level, character,
// column, row).
void testStage() {
    // SA2_TEST_LEVEL=<level>:<character>: as the menus close (advertiseD.rel unlinked), the stage to
    // load becomes that one (the stage select keeps rival battles from being chosen another way).
    static const char* s_level = std::getenv("SA2_TEST_LEVEL");
    static bool s_menus = false;
    int lv, chr;
    if (s_level && s_menus && !g_relLinked[91] && std::sscanf(s_level, "%d:%d", &lv, &chr) == 2)
        wr16(kCurrentLevel, u16(lv)), wr32(kCurrentCharacter, u32(chr)), log("[coop] test: level %d as %d\n", lv, chr);
    s_menus = g_relLinked[91];
    static const char* s_spec = std::getenv("SA2_TEST_STAGE");
    int from, to, ch;
    if (!s_spec || !g_relLinked[91] || std::sscanf(s_spec, "%d:%d:%d", &from, &to, &ch) != 3) return;
    const u32 t = g_relBase[91][5] + 0x6E64;
    for (u32 i = 0; i < 33; i++)
        if (s32(rd32(t + i * 16)) == from) wr32(t + i * 16, u32(to)), wr32(t + i * 16 + 4, u32(ch));
}

// The one-player action stages, the bosses and the rival battles: not the test levels, the
// two-player courses, the last boss (66: for now), the karts or the Chao World.
bool coopStage(u32 level) {
    static const std::set<u32> k = {3,  4,  5,  6,  7,  8,  9,  10, 11, 12, 13, 14, 16, 17, 18, 21, 22,
                                    23, 24, 25, 26, 27, 28, 30, 31, 32, 34, 35, 36, 37, 38, 40, 43, 44};
    return k.count(level) != 0 || bossStage(level);
}
// Sonic vs Shadow 1 and 2, Tails vs Eggman 1 and 2, Knuckles vs Rouge: the stage loads the rival.
bool rivalStage(u32 level) { return level == 19 || level == 20 || level == 29 || level == 33 || level == 42; }

void loadCharacters(Ctx& c, u32 entry) {
    const u32 level = rd16(kCurrentLevel);
    const bool here = coopStage(level) && !(rd8(0x801CC186) & 0xFF);  // (not in 2P battle)
    u32 local = rd32(kCurrentCharacter);
    s_p1 = 0;
    if (level == kLastBoss) {  // (both supers are the stage's own: see lastBossManager)
        interpOriginal(c, entry);
        s_coopLevel = here;
        s_finished = s_awarded = false, s_stageFrames = 0, s_bossHealth = 0, s_lastBossHooked = false;
        s_p1 = link().host() ? 0 : 1, s_p2 = 1 - s_p1;
        log("[coop] the last boss: this game plays slot %d, the partner slot %d\n", s_p1, s_p2);
        return;
    }
    if (here && !link().host() && counterpart(local) >= 0 && local != s_lastSwapped) {
        local = u32(counterpart(local));  // the joiner plays the other side's character
        wr32(kCurrentCharacter, local);
        s_lastSwapped = local;
    }
    interpOriginal(c, entry);
    s_coopLevel = here && counterpart(local) >= 0;
    s_finished = s_awarded = false, s_stageFrames = 0, s_startFrames = 0, s_bossHealth = 0, s_rivalPainted = false;
    if (!s_coopLevel) return;
    const u32 partner = u32(counterpart(local));
    s_p2 = rivalStage(level) ? 2 : 1;
    if (s_p2 == 1) {
        wr32(kCurrentCharacter2P, partner);
        guestCall(c, kLoaders[partner], {1});
        guestCall(c, kPlayerSetup, {1});
    } else {  // (no per-player setup: its tables are players 1 and 2's; the partner's place is theirs)
        guestCall(c, kLoaders[partner], {2});
    }
    hookCharacter(0), hookCharacter(s_p2);
    setObjectsReset();
    colourPlayers();
    s_ownValid = false;
    if (const u32 e = player(kMainCharObj1, s_p2))
        for (int i = 0; i < 3; i++) s_spawn[i] = flt(rd32(e + 0x14 + 4 * i));
    log("[coop] level %u: player 1 %u, the partner's %u in slot %d (%08X)\n", level, local, partner, s_p2, player(kMainCharacter, s_p2));
}

// Where a stage puts its player - at the start (LoadStartPosition, 8001DB4C), at the end
// (LoadEndPosition, 8001CFE4, which tries missions 2 and 3's first: 8001D29C) and 8001D954 - comes
// from tables by character (SonicStartArray, MechTailsEndArray...), which have the stages the
// character plays: the joiner's (the other side's) is looked up as the stage's own character (else
// the Cyclone posed at 0,0,0 - up in Iron Gate's ceiling).
constexpr u32 kStagePlaces[] = {0x8001DB4C, 0x8001CFE4, 0x8001D29C, 0x8001D954};
int s_asStage = 0;  // (inside one already: the character is the stage's)
void asStageCharacter(Ctx& c, u32 entry) {
    const u32 obj2 = player(kMainCharObj2, 0);
    if (s_asStage || !s_coopLevel || link().host() || c.r[3] != 0 || !obj2) return interpOriginal(c, entry);
    const u8 mine = u8(rd8(obj2 + 1));
    const int stage = counterpart(mine);
    if (stage < 0) return interpOriginal(c, entry);
    wr8(obj2 + 1, u8(stage));
    s_asStage++;
    interpOriginal(c, entry);
    s_asStage--;
    wr8(obj2 + 1, mine);
    static const bool s_log = std::getenv("SA2_COOP_LOG") != nullptr;
    if (s_log) log("[coop] %08X as character %d (not %u)\n", entry, stage, mine);
}
}  // namespace

// The rival stages' own set-up loads the rival as the story has it: the other side's character
// from player 1's (CurrentCharacter, through 8001DDA8). The joiner's player 1 being the other side
// already, its game is told the story's (the host's) there, so both fight the same rival.
const BossGauge kRivalSetups[] = {{19, 0x01000E78}, {20, 0x010016D0}, {29, 0x0100002C}, {33, 0x0100390C}, {42, 0x01002CF0}};
void rivalSetup(Ctx& c, u32 entry) {
    const u32 mine = rd32(kCurrentCharacter);
    const bool tell = s_coopLevel && !link().host() && counterpart(mine) >= 0;
    if (tell) wr32(kCurrentCharacter, u32(counterpart(mine)));
    interpOriginal(c, entry);
    if (tell) wr32(kCurrentCharacter, mine);
    log("[coop] the rival: %d\n", player(kMainCharObj2, 1) ? int(rd8(player(kMainCharObj2, 1) + 1)) : -1);
}

// ---- the last boss: Super Sonic and Super Shadow, one for each player ----
// The stage loads both supers (slots 0 and 1) and its player manager (m89 1:F3F8) plays one at a
// time: the one in play has the controller (the other's is turned off), and after the boss is hit it
// swaps them (its state 1, then the swap's own scene). In co-op each game keeps its own super in play
// - the host Super Sonic, the joiner Super Shadow - and never swaps; the other super is the partner.
void lastBossManagerMain(Ctx& c, u32 entry) {
    const u32 m = s_coopLevel ? rd32(c.r[3] + 0x40) : 0;
    if (m) {
        if (s8(rd8(m + 1)) == 1) wr8(m + 1, 0), log("[coop] the last boss: no swap\n");
        wr8(m, u8(s_p1));
        // Each player's rings are their own: as the fight starts (its frame count, +8, still low) the
        // joiner's Super Shadow is given what Super Sonic has - the stage gives the second super what
        // the first leaves of one shared pool, none, and the manager ends a super out of rings at once.
        if (s_p1 == 1 && s16(rd16(m + 8)) < 30 && !rd16(kRingCount + 2)) wr16(kRingCount + 2, rd16(kRingCount));
    }
    interpOriginal(c, entry);
    if (m) wr8(m, u8(s_p1));
}
// Each frame (PADRead): the supers' own code hooked once they are there (they come after LoadCharacters).
void lastBossFrame() {
    if (!s_coopLevel || s_lastBossHooked || rd16(kCurrentLevel) != kLastBoss) return;
    const u32 a = player(kMainCharObj2, 0), b = player(kMainCharObj2, 1);
    if (!a || !b || rd8(a + 1) != 9 || rd8(b + 1) != 10) return;
    hookCharacter(0), hookCharacter(1);
    s_lastBossHooked = true, s_ownValid = false;
    log("[coop] the last boss: the supers are hooked\n");
}

// Not in co-op: your colours on your own character, as each stage loads it (and as you change them:
// padFilter).
void soloInit() {
    interpReplace(0, kLoadCharacters, [](Ctx& c, u32 entry) {
        interpOriginal(c, entry);
        recolourPlayer(0, settings().colours);
    });
}

// From coop::init, in a co-op session.
void gameInit() {
    interpReplace(89, 0x0100F3F8, lastBossManagerMain);
    for (const BossGauge& r : kRivalSetups) interpReplace(r.module, r.addr, rivalSetup);
    for (u32 a : kStagePlaces) interpReplace(0, a, asStageCharacter);
    interpReplace(0, kLoadCharacters, loadCharacters);
    interpReplace(0, kDamagePlayer, damagePlayer);
    interpReplace(0, kKillPlayer, killPlayer);
    interpReplace(0, kAwardWin, awardWin);
    interpReplace(0, kEmeraldManagerMain, emeraldManagerMain);
    for (const BossGauge& g : kBossGauges) interpReplace(g.module, g.addr, bossGauge);
}

// PADRead (src/runtime/hle_hw.cpp), every frame: see "player 1 drives the menus" above.
void padFilter(u8 pads[4][12]) {
    if (!active()) {  // (alone: colours changed in the F1 menu, on the character now)
        if (inGameplay() && player(kMainCharObj2, 0) && s_coloursDirty.exchange(false)) recolourPlayer(0, settings().colours);
        return;
    }
    treasureMenus();
    testStage();
    lastBossFrame();
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
        if (pad[0] | pad[1]) s_exitPress[mine] = pad, s_retryAt = 0;  // (the latest press on this screen)
        s_hostPads.pop_front();
    } else if (mine && !s_hostPads.empty()) {
        // The host has left this screen and this game is still on it: the press that took the host
        // off came while this game was not ready yet (a cutscene still loading, a menu still
        // appearing) - it is pressed again, half a second apart, until this game moves on too.
        const auto it = s_exitPress.find(mine);
        if (it != s_exitPress.end()) {
            if (s_retryAt == 0) s_retryAt = s_padCalls + 30;
            if (s_padCalls >= s_retryAt && s_padCalls < s_retryAt + 8) pad = it->second;
            if (s_padCalls >= s_retryAt + 30) s_retryAt = s_padCalls;
        }
    }
    s_padCalls++;
    static const bool s_log = std::getenv("SA2_COOP_LOG") != nullptr;
    if (s_log && (pad[0] | pad[1]))
        log("[coop] host pad %02X%02X on screen %08X (queue %zu, next screen %08X)\n", pad[0], pad[1], mine, s_hostPads.size(),
            s_hostPads.empty() ? 0u : s_hostPads.front().screen);
    std::memcpy(pads[0], pad.data(), 10);
    pads[0][10] = 0;  // (connected)
}

}  // namespace rt::coop
