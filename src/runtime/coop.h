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
};
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
void overlay();  // render thread, each frame between aurora_begin_frame and aurora_end_frame: F1 menu, notices

}  // namespace rt::coop

namespace rt::discord {
void start();               // Rich Presence on its own thread (if enabled and an application id is set)
std::string status();       // for the menu
}  // namespace rt::discord
