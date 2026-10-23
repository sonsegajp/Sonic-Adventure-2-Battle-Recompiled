// Online co-op (docs/COOP.md): the session's link to the partner, its settings (coop.ini in the data
// folder) and the F1 menu (coopmenu.cpp). A co-op session plays on its own memory card
// (saves/coop/MemoryCardA.raw): the main game's saves are never touched by co-op.
#pragma once

#include "net/coopnet.h"

#include <string>

namespace rt::coop {

struct Settings {
    std::string name;                         // shown to the partner (and on Discord)
    std::string relay = "198.71.52.82:8901";  // the relay server (host:port)
    bool discord = true;                      // Discord Rich Presence
    std::string discordAppId;                 // the Discord application it shows as (empty: off)
    std::string lastCode;                     // the last code joined (the menu offers it again)
    // Colours (coopcolour.cpp), by slot: Sonic's blue, his jump ball and trails, Shadow's red stripes,
    // his jump ball and trails, Tails' fur, Eggman's suit, Knuckles' red, Rouge's white fur, the
    // Cyclone's blue and yellow, the Egg Walker's gray and red: 0xRRGGBB, or -1 as the game has it.
    int colours[12] = {-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1};
};
constexpr int kColourSlots = 12;
Settings& settings();
void saveSettings();

// At start: --coop-host <code> / --coop-join <code> [--coop-name <name>] make this a co-op session
// (its own memory card) and start the link. Must run before the memory card is first used.
void init(int argc, char** argv);
bool active();  // a co-op session (on the co-op card)
coopnet::Link& link();
coopnet::Relay relay();  // settings().relay, parsed
// Hosting or joining from the menu: at once in a co-op session; else the game starts again as one
// (with the same options), so the main game's card is never used for co-op. The old process exits
// with kRestartedInCoop (the launcher then waits for the new one).
void hostOrJoin(bool host, const std::string& code);
constexpr int kRestartedInCoop = 0x5A2B;

void gameInit();  // the stage side (coopgame.cpp): player 2 = the partner
void soloInit();  // ... not in co-op: only your colours, on your own character
void coloursChanged();  // coopgame.cpp: settings().colours changed (any thread): yours redone and sent
void recolourPlayer(int n, const int* colours);  // coopcolour.cpp (game thread): player n's character in these colours
void planDrawn(int n, const int* colours);  // ... painted as drawn instead (colours nullptr: grey), around drawPainted(n)
void drawPainted(int n);                    // player n is being drawn (-1: none)
std::string gameStatus();  // coopgame.cpp: a line shown while the game holds for the partner (else empty)
void overlay();  // render thread, each frame between aurora_begin_frame and aurora_end_frame: F1 menu, notices
void notify(const std::string& text);  // a notice in the corner for a few seconds (any thread)

// Chat (T opens it: coopmenu.cpp; messages over the link: coopgame.cpp).
void sendChat(const std::string& text);  // coopgame.cpp (any thread): to the partner, and into the chat
void chatArrived(const std::string& name, unsigned colour, const std::string& text);  // coopmenu.cpp (any thread)
bool typing();                           // coopmenu.cpp: the chat is open (the game gets no buttons)
unsigned myNameColour();                 // coopgame.cpp: 0xRRGGBB of your character now (your colour for it)
// The partner's name tag (coopgame.cpp): where above their head on the game's 640x480 screen, its
// colour - false when they are not on screen.
bool nameTag(float& x, float& y, unsigned& colour);

}  // namespace rt::coop

namespace rt {
void audioBeep();  // src/runtime/audio.cpp: a short chime over the game's sound (chat)
}

namespace rt::discord {
void start();               // Rich Presence on its own thread (if enabled and an application id is set)
std::string status();       // for the menu
}  // namespace rt::discord
