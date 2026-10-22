// Starting a PC code mod as SA Mod Manager's loader does: load its DLL, apply the tables in its
// SA2ModInfo export, call its Init(path, helperFunctions, index), keep its per-frame exports.
#include "modhost/modhost.h"
#include "modhost/sysapi.h"
#include "modhost/threads.h"

#include "runtime/runtime.h"

#include <cstdio>
#include <cstdlib>
#include <iterator>

namespace mh {

namespace {

// HelperFunctions (SA2ModInfo.h): the loader's services, by slot. Slot 0 is the version.
const char* const kHelperSlots[] = {
    nullptr, "RegisterStartPosition", "ClearStartPositionList", "Register2PIntroPosition",
    "Clear2PIntroPositionList", "GetMainSavePath", "GetChaoSavePath", "RegisterEndPosition", "ClearEndPositionList",
    "RegisterMission23EndPosition", "ClearMission23EndPositionList", "HookExport", "GetReplaceablePath",
    "ReplaceFile", "SetWindowTitle", "SetDebugFontSize", "SetDebugFontColor", "DisplayDebugString",
    "DisplayDebugStringFormatted", "DisplayDebugNumber", nullptr /* LoaderSettings */, nullptr /* Mods */,
    "RegisterVoice", "ReplaceTexture", "UnreplaceFile", "PushInterpolationFix", "PopInterpolationFix",
    "GetFileModIndex", "ReplaceFileAtIndex",
};
constexpr int kHelperVersion = 16;

u32 s_savePath = 0;
void getSavePath(Cpu& c) { c.r[Cpu::EAX] = s_savePath, c.ret(); }
void nop(Cpu& c) { c.r[Cpu::EAX] = 0, c.ret(); }
// File replacement: the mod's file (a path from the game folder) stands for a game file.
void replaceFile(Cpu& c) {
    rt::modReplaceFile(c.mem.cstr(c.arg(0)), hostPath(c.mem.cstr(c.arg(1))));
    c.r[Cpu::EAX] = 0, c.ret();
}
void unreplaceFile(Cpu& c) {
    rt::modReplaceFile(c.mem.cstr(c.arg(0)), "");
    c.r[Cpu::EAX] = 0, c.ret();
}
void getReplaceablePath(Cpu& c) {  // the replacement's path when there is one (a static buffer)
    static u32 s_buf = 0;
    if (!s_buf) s_buf = host().alloc(1024);
    const std::string game = c.mem.cstr(c.arg(0));
    const std::string repl = rt::modFile(game);
    if (repl.empty()) return c.r[Cpu::EAX] = c.arg(0), c.ret();
    putStr(s_buf, guestPath(repl), 1024);
    c.r[Cpu::EAX] = s_buf, c.ret();
}
void minusOne(Cpu& c) { c.r[Cpu::EAX] = 0xFFFFFFFF, c.ret(); }

Native helperImpl(const std::string& name) {
    if (name == "GetMainSavePath" || name == "GetChaoSavePath") return getSavePath;
    if (name == "GetReplaceablePath") return getReplaceablePath;
    if (name == "ReplaceFile" || name == "ReplaceFileAtIndex") return replaceFile;
    if (name == "UnreplaceFile") return unreplaceFile;
    if (name == "GetFileModIndex") return minusOne;
    if (name == "RegisterVoice" || name == "ReplaceTexture" || name == "HookExport" || name == "DisplayDebugString" ||
        name == "DisplayDebugStringFormatted" || name == "DisplayDebugNumber")
        return nop;
    if (name == "SetWindowTitle" || name == "SetDebugFontSize" || name == "SetDebugFontColor" ||
        name == "PushInterpolationFix" || name == "PopInterpolationFix")
        return nop;
    return nullptr;
}

u32 helperFunctions() {
    static u32 s_helpers = 0;
    if (s_helpers) return s_helpers;
    Host& h = host();
    s_savePath = h.allocString("resource/gd_PC/SAVEDATA");
    s_helpers = h.alloc(4 * u32(std::size(kHelperSlots)));
    h.mem.w32(s_helpers, kHelperVersion);
    const u32 settings = h.alloc(0x100);
    for (u32 i = 0; i < 0x100; i += 4) h.mem.w32(settings + i, 0);
    for (size_t i = 1; i < std::size(kHelperSlots); i++) {
        u32 v = 0;
        if (kHelperSlots[i]) v = h.thunk(std::string("HelperFunctions.") + kHelperSlots[i], helperImpl(kHelperSlots[i]));
        else if (i == 20) v = settings;
        h.mem.w32(s_helpers + 4 * u32(i), v);
    }
    return s_helpers;
}

bool startMod(Host& h, const std::filesystem::path& dir, const std::string& name, const std::string& dllFile, u32 modIndex);

}  // namespace

bool runModDll(const std::filesystem::path& dir, const std::string& name, const std::string& dllFile, u32 modIndex) {
    Host& h = host();
    Gil gil;
    threadsInit();
    Cpu::onSlice = gilYield;
    d3d9Init();
    h.currentMod = name;
    h.currentModDir = dir.string();
    bool ok;
    try {
        ok = startMod(h, dir, name, dllFile, modIndex);
    } catch (const ModFault& e) {
        rt::log("[modhost] %s stopped while starting: %s (%s)\n", name.c_str(), e.what(), h.cpu.where().c_str());
        h.commitPatches();
        ok = false;
    }
    // All or nothing: a mod whose changes are not all translated is taken back whole.
    const u32 missing = h.undo.count(name) ? h.undo[name].untranslated() : 0;
    if (!ok || (missing && !std::getenv("SA2_MOD_PARTIAL"))) {
        if (missing) {
            rt::log("[modhost] %s: %u of its changes to the game have no GameCube translation yet; the mod is not "
                    "applied (the game runs without it)\n", name.c_str(), missing);
            const Host::Undo& u = h.undo[name];
            std::string list;
            for (auto it = u.unknown.begin(); it != u.unknown.end();) {
                const u32 first = *it;
                u32 last = first;
                while (++it != u.unknown.end() && *it == last + 1) last = *it;
                char b[32];
                std::snprintf(b, sizeof b, " %X-%X", first, last);
                list += b;
            }
            rt::log("[modhost] %s: no GameCube place:%s\n", name.c_str(), list.c_str());
            list.clear();
            for (u32 pc : u.failed) {
                char b[16];
                std::snprintf(b, sizeof b, " %X", pc);
                list += b;
            }
            rt::log("[modhost] %s: not translatable:%s\n", name.c_str(), list.c_str());
        }
        h.rollback(name);
        return false;
    }
    return true;
}

namespace {
bool startMod(Host& h, const std::filesystem::path& dir, const std::string& name, const std::string& dllFile, u32 modIndex) {
    const size_t firstPatch = h.patches.size();
    Image* img = h.load(dir / dllFile);
    if (!img) return false;

    // SA2ModInfo: version, then (in old mods) Init and tables of patches the loader applies.
    const u32 info = img->symbol("SA2ModInfo");
    if (!info) {
        rt::log("[modhost] %s: no SA2ModInfo export, not a mod DLL\n", name.c_str());
        return false;
    }
    const u32 version = h.mem.r32(info);
    u32 init = img->symbol("Init");
    if (version < 1 || version > 32) {
        rt::log("[modhost] %s: unknown mod version %u\n", name.c_str(), version);
        return false;
    }
    if (!init) init = h.mem.r32(info + 4);
    {  // the Patches / Jumps / Calls / Pointers tables (old mods; empty in newer ones)
        auto table = [&](u32 off) { return std::pair<u32, u32>(h.mem.r32(info + off), h.mem.r32(info + off + 4)); };
        for (auto [p, n] = table(8); p && n--; p += 12) {
            const u32 dst = h.mem.r32(p), src = h.mem.r32(p + 4), size = h.mem.r32(p + 8);
            for (u32 i = 0; i < size; i++) h.mem.w8(dst + i, h.mem.r8(src + i));
        }
        for (int k = 0; k < 2; k++)  // jumps, calls: E9/E8 rel32
            for (auto [p, n] = table(16 + 8 * k); p && n--; p += 8) {
                const u32 at = h.mem.r32(p), to = h.mem.r32(p + 4);
                h.mem.w8(at, k ? 0xE8 : 0xE9);
                h.mem.w32(at + 1, to - (at + 5));
            }
        for (auto [p, n] = table(32); p && n--; p += 8) h.mem.w32(h.mem.r32(p), h.mem.r32(p + 4));
    }
    if (init) {
        std::string path = dir.string();
        for (char& ch : path)
            if (ch == '/') ch = '\\';
        const u32 pathArg = h.allocString(path);
        h.cpu.call(init, {pathArg, helperFunctions(), modIndex});
    }
    h.commitPatches();

    u32 bytes = 0;
    for (size_t i = firstPatch; i < h.patches.size(); i++) bytes += u32(h.patches[i].bytes.size());
    if (std::getenv("SA2_MOD_DUMP")) {  // every patch with its bytes (for the map work)
        if (std::FILE* f = std::fopen(("build/modhost_patches_" + name + ".tsv").c_str(), "w")) {
            for (size_t i = firstPatch; i < h.patches.size(); i++) {
                std::fprintf(f, "%08X\t", h.patches[i].addr);
                for (u8 b : h.patches[i].bytes) std::fprintf(f, "%02X", b);
                std::fprintf(f, "\n");
            }
            std::fclose(f);
        }
    }
    rt::log("[modhost] %s: Init done; it patched %u byte(s) of the PC executable in %u place(s)\n", name.c_str(), bytes,
            u32(h.patches.size() - firstPatch));
    return true;
}
}  // namespace

}  // namespace mh
