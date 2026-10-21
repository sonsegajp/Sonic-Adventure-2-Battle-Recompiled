// Mods. A mod is a folder in mods/ with a mod.ini, laid out as for the PC version's mod loader
// (SA Mod Manager), so PC mods are installed by dropping their folder in:
//   - its files replace the game's (the folder's gd_PC/ tree; the DVD layer asks modFile());
//   - its code, a 32-bit x86 DLL patching the PC executable, cannot run here (nor on any other
//     platform the game is built for). Code mods are ported instead, as native code compiled into
//     the program: a port is switched on when its mod's folder is present and reads the mod's own
//     config.ini, so the PC mod's options work unchanged. Ports hook the recompiled game through
//     patch points (config/GSNE8P/patchpoints.txt): native code that runs before a given
//     instruction, with the game's registers, and may skip it.
#include "runtime/runtime.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <random>
#include <string>
#include <vector>

namespace rt {

using PatchFn = bool (*)(Ctx&);
extern const u32 kPatchPointAddrs[];  // generated (dol_table.cpp)
extern const u32 kPatchPointCount;
extern PatchFn g_patchPoints[];

namespace {

bool registerPatch(u32 addr, PatchFn fn) {
    for (u32 i = 0; i < kPatchPointCount; i++)
        if (kPatchPointAddrs[i] == addr) {
            g_patchPoints[i] = fn;
            return true;
        }
    log("[mods] no patch point at %08X (add it to config/GSNE8P/patchpoints.txt and regenerate)\n", addr);
    return false;
}

// ---- ini files (mod.ini, config.ini): [section] key=value, keys compared without case ----
using Ini = std::map<std::string, std::string>;  // "section.key" -> value, lower-case keys

std::string lower(std::string s) {
    for (char& ch : s) ch = char(std::tolower(u8(ch)));
    return s;
}
std::string trim(const std::string& s) {
    const size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
}
Ini readIni(const std::filesystem::path& p) {
    Ini ini;
    std::ifstream f(p);
    std::string section, line;
    while (std::getline(f, line)) {
        line = trim(line);
        if (line.empty() || line[0] == ';' || line[0] == '#') continue;
        if (line.front() == '[' && line.back() == ']') {
            section = lower(trim(line.substr(1, line.size() - 2)));
            continue;
        }
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string key = lower(trim(line.substr(0, eq)));
        ini[section.empty() ? key : section + "." + key] = trim(line.substr(eq + 1));
    }
    return ini;
}
std::string iniGet(const Ini& ini, const std::string& key, const std::string& def) {
    const auto it = ini.find(lower(key));
    return it == ini.end() || it->second.empty() ? def : it->second;
}
bool iniBool(const Ini& ini, const std::string& key, bool def) {
    const std::string v = lower(iniGet(ini, key, def ? "true" : "false"));
    return v == "true" || v == "1" || v == "yes" || v == "on";
}

// ---- HedgePanel (SPEEPSHighway): tweaks to Sonic and Shadow, ported from the PC mod ----
// Each patch of the PC mod was read in the PC executable and matched to the GameCube code it
// changes; the GameCube compiler duplicated some of that code, so a PC patch can be several here.
namespace hedgepanel {

std::mt19937 s_rng{std::random_device{}()};
constexpr u32 kCurrentLevel = 0x803AD820;  // s16 (r13 - 0x7AA0)

// No Low Enemy Bounces: whenever Sonic bounces off an enemy, the jump timer is reset to the value
// that gives the high bounce. (The PC mod writes 32 bits at +0x12, the timer and the field after
// it, and only when that pair is above 0x23 - done the same here.)
bool noLowBounce(Ctx& c) {
    const u32 p = c.r[29];
    if (s32(rd16(p + 0x14) << 16 | rd16(p + 0x12)) > 0x23) {
        wr16(p + 0x12, 0x23);
        wr16(p + 0x14, 0);
    }
    return false;
}

// Bounce From Light Attack: at the light attack's end, the bounce action (6) and animation (0x67)
// instead of falling (0xA, 0xF), with the bounce's speed field (+0xDC rather than +0xD8).
bool lightAttackAction(Ctx& c) { return c.r[0] = 6, false; }
bool lightAttackAnim(Ctx& c) { return c.r[0] = 0x67, false; }
bool lightAttackSpeed(Ctx& c) {  // replaces lfs f1, 0xD8(r31)
    c.f[1] = c.ps1[1] = rdf32(c.r[31] + 0xDC);
    return true;
}

// Randomized Homing Sounds: one of five voice clips per homing attack (the PC mod's sets, the
// original one twice for Sonic, once for Shadow plus 0x300A twice).
bool sonicHomingVoice(Ctx& c) {
    static const u32 ids[5] = {0x3013, 0x301B, 0x301C, 0x301B, 0x301F};
    return c.r[3] = ids[s_rng() % 5], false;
}
bool shadowHomingVoice(Ctx& c) {
    static const u32 ids[5] = {0x300A, 0x300E, 0x3009, 0x3008, 0x300A};
    return c.r[3] = ids[s_rng() % 5], false;
}

// Homing Attack At Low Heights: the height difference is compared with the player's field +0x94
// instead of 8 units.
bool lowHoming(Ctx& c) {
    c.f[0] = c.ps1[0] = rdf32(c.r[31] + 0x94);
    return false;
}

// Fast Somersault: the somersault keeps the player's speed (its speed stores are skipped); the
// default mode also turns it into a slide (animation 0x5D for 0x60).
bool keepSpeed(Ctx&) { return true; }
bool slideAnim(Ctx& c) { return c.r[0] = 0x5D, false; }

// Modern Style Grinding: leaning on a rail never makes the player fall off or hang from its side.
bool grindNoFall(Ctx& c) { return c.r[0] = 0, false; }  // the lean timer reads as below the limit
bool grindLeanAnim(Ctx& c) {                             // the hanging animation goes to +0x172
    wr16(c.r[29] + 0x172, c.r[0]);
    return true;
}
bool grindLeanReset(Ctx& c) {  // the lean timer resets on the opposite side of the 0.6 test
    c.setCrbit(0, !c.crbit(0));
    return false;
}

// Automatic Magic Hands Prompt: with no other prompt, Sonic's action prompt is Magic Hands (0x4A);
// catching then uses action 0x19.
bool magicHandsPrompt(Ctx& c) {
    if (s16(rd16(kCurrentLevel)) == 90) return false;  // not in Chao World
    const u32 p = c.r[30];
    if (rd8(p + 1) == 0 && rd8(p + 0xD) == 0) wr8(p + 0xD, 0x4A);
    return false;
}
bool magicHandsCatch(Ctx& c) { return c.r[0] = 0x19, false; }

// No Magic Orbs While Running: an enemy caught while moving is dropped.
template <int Twp, int Pwp>
bool runningDrop(Ctx& c) {
    const u32 t = c.r[Twp], p = c.r[Pwp];
    if (rdf32(p + 0x64) > 0.0) {
        wr8(t, 1);
        wr8(t + 1, 1);
        wr16(t + 4, u16(rd16(t + 4) - 0x800));
        wr32(p + 0x94, 0);
    }
    return false;
}

void enable(const Ini& cfg) {
    int n = 0;
    auto on = [&](std::initializer_list<u32> addrs, PatchFn fn) {
        for (u32 a : addrs) n += registerPatch(a, fn);
    };
    const std::string somersault = lower(iniGet(cfg, "Options.FastSomersault", "Enabled"));
    if (somersault != "disabled") {
        on({0x80077B2C, 0x80077E18, 0x800780F8, 0x800783C0, 0x80078638, 0x80078808, 0x80078A9C, 0x80077F5C,
            0x800779E8, 0x80077CC8, 0x80077FB4, 0x8007827C, 0x800786F8, 0x80077BE8, 0x80077ED4, 0x800781E4,
            0x800784AC, 0x80078670, 0x80078840},
           keepSpeed);
        if (somersault == "enabled") on({0x80078A6C}, slideAnim);
    }
    if (iniBool(cfg, "Options.ModernStyleGrinding", true)) {
        on({0x8007F9BC}, grindNoFall);
        on({0x800754A0, 0x800754B4, 0x800754C8, 0x800754DC, 0x8007557C, 0x80075590, 0x800755A4, 0x800755B8}, grindLeanAnim);
        on({0x80075320}, grindLeanReset);
    }
    if (iniBool(cfg, "Options.RandomizedHomingSounds", true)) {
        on({0x800802B8, 0x800805D8, 0x800808A0}, sonicHomingVoice);
        on({0x80080294, 0x800805B4, 0x8008087C}, shadowHomingVoice);
    }
    if (iniBool(cfg, "Options.RemoveHomingHeightLimit", true)) on({0x800801E8, 0x80080508, 0x800807D0}, lowHoming);
    if (iniBool(cfg, "Options.NoLowEnemyBounces", true)) on({0x80081130}, noLowBounce);
    if (iniBool(cfg, "Options.BounceFromLightAttack", true)) {
        on({0x8007F5EC}, lightAttackAction);
        on({0x8007F5F8}, lightAttackAnim);
        on({0x8007F60C}, lightAttackSpeed);
    }
    if (iniBool(cfg, "Options.AutoMagicHandsPrompt", true)) {
        on({0x80010CA8}, magicHandsPrompt);
        on({0x800796E4, 0x8007A238, 0x80095F10, 0x8009648C}, magicHandsCatch);
    }
    if (iniBool(cfg, "Options.RunningMagicHandsDrop", false)) {
        on({0x8007973C, 0x8007A290}, runningDrop<28, 30>);
        on({0x80095F64}, runningDrop<29, 31>);
        on({0x800964E4}, runningDrop<30, 31>);
    }
    log("[mods] HedgePanel: native port active (%d patch points)\n", n);
}

}  // namespace hedgepanel

// Native ports of PC code mods, by the Name in their mod.ini.
struct Port {
    const char* name;
    void (*enable)(const Ini& config);
};
const Port kPorts[] = {
    {"HedgePanel", hedgepanel::enable},
};

std::vector<std::filesystem::path> s_assetDirs;  // enabled mods' gd_PC folders, highest priority first

}  // namespace

void modsInit() {
    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path root = std::getenv("SA2_MODS") ? fs::path(std::getenv("SA2_MODS")) : fs::path("mods");
    if (!fs::is_directory(root, ec)) return;
    std::vector<fs::path> dirs;
    for (const auto& e : fs::directory_iterator(root, ec))
        if (e.is_directory() && fs::exists(e.path() / "mod.ini")) dirs.push_back(e.path());
    std::sort(dirs.begin(), dirs.end());
    for (const auto& dir : dirs) {
        const Ini info = readIni(dir / "mod.ini");
        const std::string name = iniGet(info, "Name", dir.filename().string());
        const std::string dll = iniGet(info, "DLLFile", "");
        bool ported = false;
        for (const Port& p : kPorts)
            if (lower(name) == lower(p.name)) {
                p.enable(readIni(dir / "config.ini"));
                ported = true;
            }
        if (fs::is_directory(dir / "gd_PC", ec)) s_assetDirs.insert(s_assetDirs.begin(), dir / "gd_PC");
        log("[mods] %s (%s)%s%s\n", name.c_str(), dir.filename().string().c_str(),
            fs::is_directory(dir / "gd_PC", ec) ? ", files" : "",
            dll.empty() ? "" : ported ? ", code: native port" : ", code: not ported yet (its files still load)");
    }
}

// A mod's replacement for a game file (path as on the disc), or empty.
std::string modFile(const std::string& discPath) {
    namespace fs = std::filesystem;
    std::error_code ec;
    for (const auto& dir : s_assetDirs) {
        const fs::path p = dir / discPath;
        if (fs::is_regular_file(p, ec)) return p.string();
    }
    return {};
}

}  // namespace rt
