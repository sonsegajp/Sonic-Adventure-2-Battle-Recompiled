// Mod host: address space, heap, native thunks and the PE32 loader (see modhost.h).
#include "modhost/modhost.h"

#include "runtime/runtime.h"

#include "modhost/threads.h"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <set>

namespace mh {

namespace {
constexpr u32 kHeapBase = 0x20000000, kHeapEnd = 0x70000000;
constexpr u32 kStackBase = 0x7FE00000, kStackTop = 0x7FF00000;
constexpr u32 kTeb = 0x7FFDE000, kPeb = 0x7FFD0000;  // the main thread's TEB: 2 pages (threads.cpp)
constexpr u32 kSystemModules = 0x7C000000;           // handles standing for system DLLs, 64 KB apart

bool inWindow(u32 a, u32 size) {
    const u64 e = u64(a) + size;
    return (e > kPcImageBase && a < kPcImageEnd) || e > kGcBase;
}
}  // namespace

Host& host() {
    static Host* s_host = new Host;  // never destroyed: mod code may run until the process ends
    return *s_host;
}

Host::Host() : heapTop_(kHeapBase) {
    mem.foreign = this;
    mem.map(kStackBase, kStackTop - kStackBase);
    mem.map(kTeb, 0x2000);
    mem.map(kPeb, 0x1000);
    mem.w32(kTeb + 0x00, 0xFFFFFFFF);  // end of the SEH chain
    mem.w32(kTeb + 0x04, kStackTop);
    mem.w32(kTeb + 0x08, kStackBase);
    mem.w32(kTeb + 0x18, kTeb);
    mem.w32(kTeb + 0x20, 0x1000);  // process id
    mem.w32(kTeb + 0x24, 0x1004);  // thread id
    mem.w32(kTeb + 0x30, kPeb);
    mem.w32(kTeb + 0x2C, kTeb + 0x1800);  // static TLS pointers (threads.cpp)
    cpu.fsBase = kTeb;
    cpu.r[Cpu::ESP] = kStackTop - 16;
}

// ---- thunks ----

u32 Host::thunk(const std::string& name, NativeCtx fn, u32 ctx) {
    if (const auto it = thunkByName_.find(name); it != thunkByName_.end()) return kThunkBase + 16 * it->second;
    const u32 i = u32(thunks_.size());
    thunks_.push_back({name, nullptr, fn, ctx});
    thunkByName_[name] = i;
    return kThunkBase + 16 * i;
}

u32 Host::virtualVariable(u32 pcAddr, u32 size) {
    if (const auto it = virtualVars.find(pcAddr); it != virtualVars.end()) return it->second.second;
    const u32 a = alloc(size);
    for (u32 i = 0; i < size; i++) mem.w8(a + i, 0);
    virtualVars[pcAddr] = {size, a};
    return a;
}

u32 Host::thunk(const std::string& name, Native fn) {
    if (const auto it = thunkByName_.find(name); it != thunkByName_.end()) return kThunkBase + 16 * it->second;
    const u32 i = u32(thunks_.size());
    thunks_.push_back({name, fn});
    thunkByName_[name] = i;
    return kThunkBase + 16 * i;
}

void Host::execThunk(Cpu& c, u32 index) {
    if (index >= thunks_.size()) {
        char b[64];
        std::snprintf(b, sizeof b, "jump to %08X, where nothing is", c.eip);
        throw ModFault(b);
    }
    const Thunk& t = thunks_[index];
    if (t.fnCtx) return t.fnCtx(c, t.ctx);
    if (!t.fn) {
        static std::set<u32> s_logged;
        if (s_logged.insert(index).second)
            rt::log("[modhost] %s called %s, which the mod host does not provide (returned 0)\n", currentMod.c_str(),
                    t.name.c_str());
        c.r[Cpu::EAX] = 0;
        c.ret();
        return;
    }
    t.fn(c);
}

// ---- heap: first fit over a free list, 16-byte granules ----

u32 Host::alloc(u32 size) {
    size = (std::max(size, 1u) + 15) & ~15u;
    for (auto it = free_.begin(); it != free_.end(); ++it) {
        if (it->second < size) continue;
        const u32 a = it->first, rest = it->second - size;
        free_.erase(it);
        if (rest) free_[a + size] = rest;
        used_[a] = size;
        return a;
    }
    if (heapTop_ + size > kHeapEnd) {
        rt::log("[modhost] heap exhausted (%u bytes asked)\n", size);
        return 0;
    }
    const u32 a = heapTop_;
    heapTop_ += size;
    mem.map(a, size);
    used_[a] = size;
    return a;
}

void Host::free(u32 addr) {
    if (!addr) return;
    const auto it = used_.find(addr);
    if (it == used_.end()) {
        rt::log("[modhost] free of %08X, which is not a heap block\n", addr);
        return;
    }
    u32 a = addr, size = it->second;
    used_.erase(it);
    // Coalesce with the neighbours.
    if (const auto next = free_.find(a + size); next != free_.end()) size += next->second, free_.erase(next);
    if (auto prev = free_.lower_bound(a); prev != free_.begin()) {
        --prev;
        if (prev->first + prev->second == a) {
            a = prev->first, size += prev->second;
            free_.erase(prev);
        }
    }
    free_[a] = size;
}

u32 Host::blockSize(u32 addr) const {
    const auto it = used_.find(addr);
    return it == used_.end() ? 0 : it->second;
}

u32 Host::allocString(const std::string& s) {
    const u32 a = alloc(u32(s.size() + 1));
    mem.write(a, s.c_str(), u32(s.size() + 1));
    return a;
}

// ---- PE32 loader ----

Image* Host::load(const std::filesystem::path& dll) {
    std::ifstream f(dll, std::ios::binary);
    std::vector<u8> d((std::istreambuf_iterator<char>(f)), {});
    const std::string name = dll.filename().string();
    auto le16 = [&](size_t o) { return u32(d[o] | d[o + 1] << 8); };
    auto le32 = [&](size_t o) { return u32(d[o] | d[o + 1] << 8 | d[o + 2] << 16 | u32(d[o + 3]) << 24); };
    if (d.size() < 0x200 || d[0] != 'M' || d[1] != 'Z') {
        rt::log("[modhost] %s: not a Windows DLL\n", name.c_str());
        return nullptr;
    }
    const size_t pe = le32(0x3C);
    if (pe + 0xF8 > d.size() || le32(pe) != 0x4550 || le16(pe + 4) != 0x14C || le16(pe + 0x18) != 0x10B) {
        rt::log("[modhost] %s: not a 32-bit x86 DLL\n", name.c_str());
        return nullptr;
    }
    const u32 nSections = le16(pe + 6), optSize = le16(pe + 0x14);
    const size_t opt = pe + 0x18;
    const u32 prefBase = le32(opt + 0x1C), imageSize = le32(opt + 0x38), headerSize = le32(opt + 0x3C);
    auto dir = [&](int i) { return std::pair<u32, u32>(le32(opt + 0x60 + 8 * i), le32(opt + 0x64 + 8 * i)); };

    // Place it: the preferred base when that range is free, else the next free range.
    auto rangeFree = [&](u32 b) {
        if (inWindow(b, imageSize) || b < 0x00010000) return false;
        for (u32 a = b; a < b + imageSize; a += 0x1000)
            if (mem.mapped(a)) return false;
        return true;
    };
    u32 base = prefBase;
    if (!rangeFree(base)) {
        base = nextImage_;
        while (!rangeFree(base)) base += 0x10000;
    }
    nextImage_ = std::max(nextImage_, (base + imageSize + 0xFFFF) & ~0xFFFFu);
    mem.map(base, imageSize);
    mem.write(base, d.data(), std::min<u32>(headerSize, u32(d.size())));
    const size_t sec = opt + optSize;
    std::vector<std::pair<u32, u32>> code;
    for (u32 i = 0; i < nSections; i++) {
        const size_t s = sec + 40 * i;
        const u32 va = le32(s + 12), rawSize = le32(s + 16), rawPtr = le32(s + 20);
        const u32 vsize = le32(s + 8);
        const u32 n = std::min(rawSize, vsize ? vsize : rawSize);
        if (rawPtr + n <= d.size()) mem.write(base + va, d.data() + rawPtr, n);
        if (le32(s + 36) & 0x20000020) code.push_back({base + va, base + va + std::max(vsize, rawSize)});  // code, executable
    }

    // Relocations.
    if (base != prefBase) {
        const auto [rva, size] = dir(5);
        if (!rva) {
            rt::log("[modhost] %s: its base %08X is taken and it has no relocations\n", name.c_str(), prefBase);
            return nullptr;
        }
        const u32 delta = base - prefBase;
        for (u32 b = 0; b + 8 <= size;) {
            const u32 page = mem.r32(base + rva + b), blockSize = mem.r32(base + rva + b + 4);
            if (blockSize < 8) break;
            for (u32 e = 8; e + 2 <= blockSize; e += 2) {
                const u16 entry = mem.r16(base + rva + b + e);
                if ((entry >> 12) == 3) {
                    const u32 a = base + page + (entry & 0xFFF);
                    mem.w32(a, mem.r32(a) + delta);
                } else if (entry >> 12) {
                    rt::log("[modhost] %s: relocation type %d not supported\n", name.c_str(), entry >> 12);
                }
            }
            b += blockSize;
        }
    }

    // Imports: every entry binds to a native stand-in (or a stub that reports the call).
    if (const auto [rva, size] = dir(1); rva) {
        for (u32 desc = base + rva;; desc += 20) {
            const u32 ilt = mem.r32(desc), nameRva = mem.r32(desc + 12), iat = mem.r32(desc + 16);
            if (!nameRva && !iat) break;
            const std::string dllName = mem.cstr(base + nameRva);
            for (u32 i = 0;; i++) {
                const u32 entry = mem.r32(base + (ilt ? ilt : iat) + 4 * i);
                if (!entry) break;
                std::string fn;
                if (entry & 0x80000000u) fn = "#" + std::to_string(entry & 0xFFFF);
                else fn = mem.cstr(base + entry + 2);
                mem.w32(base + iat + 4 * i, thunk(dllName + "!" + fn, win32Import(dllName, fn)));
            }
        }
    }

    auto img = std::make_unique<Image>();
    img->name = name;
    img->base = base;
    img->size = imageSize;
    img->entry = le32(opt + 0x10) ? base + le32(opt + 0x10) : 0;
    if (const auto [rva, size] = dir(0); rva) {
        const u32 e = base + rva;
        const u32 nNames = mem.r32(e + 24), funcs = mem.r32(e + 28), names = mem.r32(e + 32), ords = mem.r32(e + 36);
        for (u32 i = 0; i < nNames; i++) {
            const std::string sym = mem.cstr(base + mem.r32(base + names + 4 * i));
            img->exports[sym] = base + mem.r32(base + funcs + 4 * mem.r16(base + ords + 2 * i));
        }
    }
    img->path = dll.string();
    img->code = std::move(code);
    // Thread-local storage: the template for every thread, and callbacks run with DllMain's.
    if (const auto [rva, size] = dir(9); rva) {
        const u32 t = base + rva;
        tlsAddImage(mem.r32(t), mem.r32(t + 4), mem.r32(t + 16), mem.r32(t + 8));
        if (u32 cb = mem.r32(t + 12))
            for (u32 fn; (fn = mem.r32(cb)) != 0; cb += 4) img->tlsCallbacks.push_back(fn);
    }
    rt::log("[modhost] loaded %s at %08X (%u KB)%s\n", name.c_str(), base, imageSize / 1024,
            base != prefBase ? " (relocated)" : "");

    Image* p = img.get();
    images_.push_back(std::move(img));
    Cpu& c = cur();
    for (u32 cb : p->tlsCallbacks) c.call(cb, {base, 1, 0});
    if (p->entry && !c.call(p->entry, {base, 1, 0})) {  // DllMain(DLL_PROCESS_ATTACH)
        rt::log("[modhost] %s: DllMain failed\n", name.c_str());
        return nullptr;
    }
    return p;
}

}  // namespace mh

namespace mh {

Image* Host::imageByName(const std::string& lowerName) {
    for (auto& img : images_) {
        std::string n = img->name;
        for (char& ch : n) ch = char(std::tolower(u8(ch)));
        if (n == lowerName) return img.get();
    }
    return nullptr;
}

Image* Host::imageAt(u32 addr) {
    for (auto& img : images_)
        if (addr >= img->base && addr < img->base + img->size) return img.get();
    return nullptr;
}

bool Host::isModCode(u32 addr) {
    if (addr < 0x00010000 || inWindow(addr, 1)) return false;
    if (const Image* img = imageAt(addr))
        for (const auto& [s, e] : img->code)
            if (addr >= s && addr < e) return true;
    return false;
}

void Host::threadAttach(Cpu& c) {
    for (auto& img : images_)
        for (u32 cb : img->tlsCallbacks) c.call(cb, {img->base, 2, 0});
}

namespace {
bool isSystemDll(const std::string& n) {
    static const char* const k[] = {"kernel32.dll", "kernelbase.dll", "ntdll.dll", "user32.dll", "gdi32.dll",
                                    "advapi32.dll", "shell32.dll", "ole32.dll", "ucrtbase.dll", "msvcrt.dll",
                                    "vcruntime140.dll", "msvcp140.dll", "d3d9.dll", "d3dx9_43.dll", "winmm.dll",
                                    "dinput8.dll", "xinput1_3.dll", "xinput9_1_0.dll", "version.dll"};
    for (const char* s : k)
        if (n == s) return true;
    return n.rfind("api-ms-win-", 0) == 0;
}
}  // namespace

u32 Host::systemModule(const std::string& lowerName, bool create) {
    if (const auto it = systemModules_.find(lowerName); it != systemModules_.end()) return it->second;
    if (!create && !isSystemDll(lowerName)) return 0;
    const u32 h = kSystemModules + 0x10000 * u32(systemModules_.size());
    systemModules_[lowerName] = h;
    return h;
}

std::string Host::systemModuleName(u32 handle) const {
    for (const auto& [n, h] : systemModules_)
        if (h == handle) return n;
    return {};
}

u32 Host::systemSymbol(const std::string& dll, const std::string& name) {
    Native fn = win32Import(dll, name);
    return fn ? thunk(dll + "!" + name, fn) : 0;
}

}  // namespace mh
