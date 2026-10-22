// Mod host: runs the PC version's code mods (32-bit Windows DLLs) unchanged, inside the x86
// interpreter (x86.h), in one shared address space as they would share the PC game's process:
//   0x10000000-  mod images (at their preferred base when free, else relocated)
//   0x20000000-  heap
//   0x7FE00000-  stack (1 MB), then the thread environment block
// The Windows and C runtime functions mods import are native stand-ins (win32.cpp). What mods do
// to the PC executable - code patches, reads and writes of its variables, calls of its functions -
// lands in the PC image window and is translated to the GameCube game by the bridge (bridge.cpp).
#pragma once

#include "modhost/x86.h"

#include <cstdio>
#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace mh {

using Native = void (*)(Cpu&);
using NativeCtx = void (*)(Cpu&, u32 ctx);  // one function behind many thunks

struct Image {
    std::string name, path;
    u32 base = 0, size = 0, entry = 0;
    std::vector<u32> tlsCallbacks;
    std::map<std::string, u32> exports;
    std::vector<std::pair<u32, u32>> code;  // its executable sections [start, end)
    u32 symbol(const std::string& name) const {
        const auto it = exports.find(name);
        return it == exports.end() ? 0 : it->second;
    }
};

// A byte range the mods wrote into the PC executable's code (WriteProcessMemory, or plain stores
// after VirtualProtect), in the order written.
struct CodePatch {
    u32 addr;
    std::vector<u8> bytes;
    std::string mod;
};

class Host final : public Foreign {
public:
    Host();
    Memory mem;
    Cpu cpu{mem};

    // ---- native entry points ----
    u32 thunk(const std::string& name, Native fn);
    u32 thunk(const std::string& name, NativeCtx fn, u32 ctx);
    const std::string& thunkName(u32 index) const { return thunks_[index].name; }

    // ---- heap ----
    u32 alloc(u32 size);
    void free(u32 addr);
    u32 blockSize(u32 addr) const;  // 0 if not a heap block
    u32 allocString(const std::string& s);

    // ---- images ----
    Image* load(const std::filesystem::path& dll);  // maps, relocates, binds, runs DllMain
    Image* imageByName(const std::string& lowerName);
    Image* imageAt(u32 addr);  // the loaded image containing addr
    bool isModCode(u32 addr);  // in an executable section of a loaded image
    void threadAttach(Cpu& c);  // a new thread: the images' TLS callbacks (DLL_THREAD_ATTACH)
    // System DLLs (kernel32, user32...): handles that stand for them; GetProcAddress on one gives
    // the stand-in (0 if there is none). `create`: any DLL name gets a handle (LoadLibrary).
    u32 systemModule(const std::string& lowerName, bool create = false);
    std::string systemModuleName(u32 handle) const;
    u32 systemSymbol(const std::string& dll, const std::string& name);
    std::string currentMod;     // the mod whose code runs (for logs, patches)
    std::string currentModDir;  // its folder (host path)

    // ---- the PC executable (bridge.cpp) ----
    // The PC executable's original image, when the user has it (SA2_PC_EXE): only used to answer
    // mods reading code they did not patch.
    bool loadPcImage(const std::filesystem::path& exe);
    int pcImageByte(u32 addr) const;  // -1 when unknown
    bool loadPcMap(const std::filesystem::path& file);
    // Translates the PC code patches made since the last call (called when mod code is done:
    // after a mod's Init, after each run of mod code from the game).
    void commitPatches();
    // Every PC address mod code touches (build/modhost_pcrefs.tsv): what the PC <-> GameCube map
    // must cover. kind: r (read), w (write: patch or variable), x (call/jump), t (translated)
    void noteRef(char kind, u32 pcAddr, int size);
    std::vector<CodePatch> patches;
    std::map<u32, u8> pcOverlay;  // every byte written into the PC image, by address
    std::vector<u32> dirty;       // PC code addresses written since the last commit
    // All or nothing: what each mod changed, to take back a mod the bridge cannot fully translate
    // (a half-translated mod is worse than none). untranslated: its patches with no GameCube place.
    struct Undo {
        std::map<u32, int> overlay;  // PC address -> byte before the mod's first write (-1: none)
        std::map<u32, u8> game;      // GameCube address -> byte before the mod's first write
        std::vector<u64> relWrites;  // its writes into REL modules' variables (keys of relData)
        std::set<u32> unknown;       // PC bytes it patched that have no GameCube place
        std::set<u32> failed;        // PC instructions whose patch the bridge cannot translate
        u32 untranslated() const { return u32(unknown.size() + failed.size()); }
        std::vector<u32> watched;    // PC functions whose entries it wrote
    };
    std::map<std::string, Undo> undo;
    // Mods' writes into variables of REL modules, by (module << 40 | section << 32 | offset): the
    // module's data comes from the disc again whenever it is linked, so they are written again.
    std::map<u64, u8> relData;
    void rollback(const std::string& mod);

    // ---- PC-only variables (the renderer, the window...): backed by mod-side memory ----
    // Reads and writes of PC [pcAddr, pcAddr+size) go to the returned x86 address instead.
    u32 virtualVariable(u32 pcAddr, u32 size);
    std::map<u32, std::pair<u32, u32>> virtualVars;  // PC start -> (size, x86 address)

    // ---- C runtime state used by the stand-ins ----
    std::map<u32, FILE*> files;
    std::vector<u32> atexit;

    // Foreign
    u64 read(u32 addr, int size) override;
    void write(u32 addr, u64 v, int size) override;
    void execPc(Cpu& cpu, u32 addr) override;
    void execGc(Cpu& cpu, u32 addr) override;
    void execThunk(Cpu& cpu, u32 index) override;
    u32 toGc(u32 v) override;
    u32 fromGc(u32 v) override;

private:
    struct Thunk {
        std::string name;
        Native fn;
        NativeCtx fnCtx = nullptr;
        u32 ctx = 0;
    };
    std::vector<Thunk> thunks_;
    std::map<std::string, u32> thunkByName_;
    std::map<u32, u32> free_;  // free heap blocks: address -> size
    std::map<u32, u32> used_;  // allocated blocks: address -> size
    u32 heapTop_;
    u32 nextImage_ = 0x10000000;
    std::vector<std::unique_ptr<Image>> images_;
    std::map<std::string, u32> systemModules_;
    std::vector<u8> pcImage_;  // from 0x00400000
};

Host& host();
// Starts a PC code mod: loads its DLL, applies its SA2ModInfo tables, runs its Init. False if it
// cannot run.
bool runModDll(const std::filesystem::path& dir, const std::string& name, const std::string& dllFile, u32 modIndex);
// The stand-in for an imported function, by name (nullptr if there is none): win32.cpp.
Native win32Import(const std::string& dll, const std::string& name);
// The PC renderer and its Direct3D 9 device, as mods reach them (d3d9.cpp).
void d3d9Init();

}  // namespace mh
