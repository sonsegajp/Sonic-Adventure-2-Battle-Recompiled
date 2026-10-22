// Mods. A mod is a folder in mods/ with a mod.ini, laid out as for the PC version's mod loader
// (SA Mod Manager), so PC mods are installed by dropping their folder in:
//   - its files replace the game's (the folder's gd_PC/ tree; the DVD layer asks modFile());
//   - its code, a 32-bit x86 DLL patching the PC executable, runs unchanged in the mod host
//     (src/modhost): an x86 interpreter, so the same DLL works wherever the game is built. What it
//     does to the PC executable is translated to the GameCube game by the host's bridge, with the
//     correspondences of config/GSNE8P/pcmap.txt.
#include "runtime/runtime.h"

#include "modhost/modhost.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

namespace rt {

namespace {

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
std::vector<std::filesystem::path> s_assetDirs;  // enabled mods' gd_PC folders, highest priority first

}  // namespace

void modsInit() {
    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path root = std::getenv("SA2_MODS") ? fs::path(std::getenv("SA2_MODS")) : fs::path("mods");
    if (!fs::is_directory(root, ec)) return;
    // enabled.txt (written by the launcher): the folders turned on, one a line; without it, all.
    std::vector<std::string> enabled;
    bool listed = false;
    if (std::ifstream f(root / "enabled.txt"); f) {
        listed = true;
        for (std::string l; std::getline(f, l);)
            if (!(l = trim(l)).empty()) enabled.push_back(l);
    }
    std::vector<fs::path> dirs;
    for (const auto& e : fs::directory_iterator(root, ec))
        if (e.is_directory() && fs::exists(e.path() / "mod.ini") &&
            (!listed || std::find(enabled.begin(), enabled.end(), e.path().filename().string()) != enabled.end()))
            dirs.push_back(e.path());
    std::sort(dirs.begin(), dirs.end());
    // What the mod host translates PC code mods with; the PC executable, when the user has it,
    // answers mods reading code they do not patch.
    // The generated map first; the hand-written one's entries replace its.
    bool pcMap = false;
    for (const char* f : {"config/GSNE8P/pcmap_auto.txt", "config/GSNE8P/pcmap.txt"})
        if (fs::exists(f, ec)) pcMap = mh::host().loadPcMap(f) || pcMap;
    if (!pcMap) log("[mods] no pcmap.txt: PC code mods load but nothing they patch is translated\n");
    if (const char* exe = std::getenv("SA2_PC_EXE")) mh::host().loadPcImage(exe);
    u32 modIndex = 0;
    for (const auto& dir : dirs) {
        const Ini info = readIni(dir / "mod.ini");
        const std::string name = iniGet(info, "Name", dir.filename().string());
        const std::string dll = iniGet(info, "DLLFile", "");
        const bool hosted = !dll.empty() && mh::runModDll(dir, name, dll, modIndex);
        if (fs::is_directory(dir / "gd_PC", ec)) s_assetDirs.insert(s_assetDirs.begin(), dir / "gd_PC");
        log("[mods] %s (%s)%s%s\n", name.c_str(), dir.filename().string().c_str(),
            fs::is_directory(dir / "gd_PC", ec) ? ", files" : "",
            dll.empty() ? "" : hosted ? ", code: DLL running" : ", code: DLL failed to start");
        modIndex++;
    }
}

namespace {
std::map<std::string, std::string> s_replaced;  // disc path (lower case) -> host file
std::string fileKey(std::string p) {
    for (char& ch : p) ch = ch == '\\' ? '/' : char(std::tolower(u8(ch)));
    while (!p.empty() && p[0] == '/') p.erase(0, 1);
    if (p.rfind("resource/gd_pc/", 0) == 0) p.erase(0, 15);
    return p;
}
}  // namespace

void modReplaceFile(const std::string& gamePath, const std::string& hostFile) {
    if (hostFile.empty()) s_replaced.erase(fileKey(gamePath));
    else s_replaced[fileKey(gamePath)] = hostFile;
}

// A mod's replacement for a game file (path as on the disc), or empty.
std::string modFile(const std::string& discPath) {
    namespace fs = std::filesystem;
    std::error_code ec;
    if (const auto it = s_replaced.find(fileKey(discPath)); it != s_replaced.end()) return it->second;
    for (const auto& dir : s_assetDirs) {
        const fs::path p = dir / discPath;
        if (fs::is_regular_file(p, ec)) return p.string();
    }
    return {};
}

}  // namespace rt
