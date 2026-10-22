// KERNEL32 (and the bits of USER32 and D3DX mods use) for mod code: processes and modules,
// memory, threads and synchronisation, files through the game's virtual folder, strings, time.
// Waits release the x86 lock (threads.h) so other mod threads run meanwhile.
#include "modhost/handles.h"
#include "modhost/sysapi.h"

#include "runtime/runtime.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <map>
#include <random>

namespace fs = std::filesystem;

namespace mh {

// ---- handles ----

namespace {
std::map<u32, std::shared_ptr<KObject>> s_handles;
u32 s_nextHandle = 0x100;
}  // namespace

u32 handleNew(std::shared_ptr<KObject> o) {
    const u32 h = s_nextHandle;
    s_nextHandle += 4;
    s_handles[h] = std::move(o);
    return h;
}
std::shared_ptr<KObject> handleGet(u32 h) {
    const auto it = s_handles.find(h);
    return it == s_handles.end() ? nullptr : it->second;
}
bool handleClose(u32 h) { return s_handles.erase(h) > 0; }

// ---- strings ----

std::string utf8(const std::u16string& s) {
    std::string o;
    for (size_t i = 0; i < s.size(); i++) {
        u32 cp = s[i];
        if (cp >= 0xD800 && cp < 0xDC00 && i + 1 < s.size() && s[i + 1] >= 0xDC00 && s[i + 1] < 0xE000)
            cp = 0x10000 + ((cp - 0xD800) << 10) + (s[++i] - 0xDC00);
        if (cp < 0x80) o += char(cp);
        else if (cp < 0x800) o += char(0xC0 | cp >> 6), o += char(0x80 | (cp & 0x3F));
        else if (cp < 0x10000) o += char(0xE0 | cp >> 12), o += char(0x80 | (cp >> 6 & 0x3F)), o += char(0x80 | (cp & 0x3F));
        else
            o += char(0xF0 | cp >> 18), o += char(0x80 | (cp >> 12 & 0x3F)), o += char(0x80 | (cp >> 6 & 0x3F)),
                o += char(0x80 | (cp & 0x3F));
    }
    return o;
}
std::u16string utf16(const std::string& s) {
    std::u16string o;
    for (size_t i = 0; i < s.size();) {
        const u8 b = u8(s[i]);
        u32 cp;
        int n;
        if (b < 0x80) cp = b, n = 1;
        else if ((b >> 5) == 6) cp = b & 0x1F, n = 2;
        else if ((b >> 4) == 14) cp = b & 0x0F, n = 3;
        else if ((b >> 3) == 30) cp = b & 0x07, n = 4;
        else cp = b, n = 1;  // not UTF-8: Latin-1
        if (n > 1) {
            if (i + size_t(n) > s.size()) cp = b, n = 1;
            else
                for (int k = 1; k < n; k++) cp = cp << 6 | (u8(s[i + size_t(k)]) & 0x3F);
        }
        i += size_t(n);
        if (cp >= 0x10000) o += char16_t(0xD800 + ((cp - 0x10000) >> 10)), o += char16_t(0xDC00 + ((cp - 0x10000) & 0x3FF));
        else o += char16_t(cp);
    }
    return o;
}
u32 putStr(u32 addr, const std::string& s, u32 cap) {
    if (!addr || !cap) return 0;
    const u32 n = std::min<u32>(u32(s.size()), cap - 1);
    M().write(addr, s.data(), n);
    M().w8(addr + n, 0);
    return n;
}
u32 putWide(u32 addr, const std::u16string& s, u32 cap) {
    if (!addr || !cap) return 0;
    const u32 n = std::min<u32>(u32(s.size()), cap - 1);
    for (u32 i = 0; i < n; i++) M().w16(addr + 2 * i, u16(s[i]));
    M().w16(addr + 2 * n, 0);
    return n;
}

// ---- paths ----

std::string hostPath(const std::string& g) {
    std::string p = g;
    for (char& ch : p)
        if (ch == '\\') ch = '/';
    // The virtual game folder is the working directory.
    const std::string root = "c:/sa2b";
    std::string lower = p.substr(0, root.size());
    for (char& ch : lower) ch = char(std::tolower(u8(ch)));
    if (lower == root && (p.size() == root.size() || p[root.size()] == '/')) {
        p = p.size() > root.size() + 1 ? p.substr(root.size() + 1) : ".";
    }
    while (p.rfind("./", 0) == 0) p = p.substr(2);
    // The PC game's data (resource/gd_PC) is the disc's, with mods' replacements first.
    std::string pl = p;
    for (char& ch : pl) ch = char(std::tolower(u8(ch)));
    // The PC game's save folder: mods' own saves live with the game's (saves/pc).
    if (pl.rfind("resource/gd_pc/savedata", 0) == 0) {
        std::error_code ec;
        fs::create_directories("saves/pc/SAVEDATA", ec);
        return "saves/pc/SAVEDATA" + p.substr(23);
    }
    if (pl.rfind("resource/gd_pc/", 0) == 0) {
        const std::string rel = p.substr(15);
        const std::string mod = rt::modFile(rel);
        if (!mod.empty()) return mod;
        std::error_code ec;
        if (fs::exists("orig/disc/" + rel, ec)) return "orig/disc/" + rel;
    }
    return p;
}

std::string guestPath(const std::string& h) {
    std::error_code ec;
    fs::path abs = fs::absolute(h, ec);
    fs::path rel = fs::relative(abs, fs::current_path(ec), ec);
    std::string r = rel.generic_string();
    if (r.empty() || r == ".") return kGameDir;
    if (r.rfind("..", 0) == 0) r = abs.generic_string();  // outside the game folder
    else r = std::string(kGameDir) + "/" + r;
    for (char& ch : r)
        if (ch == '/') ch = '\\';
    return r;
}

// ---- errors ----

void setLastError(u32 e) { M().w32(curTeb() + 0x34, e); }
u32 lastError() { return M().r32(curTeb() + 0x34); }
void setErrno(int e) { M().w32(tebScratch(0), u32(e)); }

namespace {

constexpr u32 kPseudoProcess = 0xFFFFFFFF, kPseudoThread = 0xFFFFFFFE;
constexpr u32 kInvalidHandle = 0xFFFFFFFF;
constexpr u32 ERROR_FILE_NOT_FOUND = 2, ERROR_PATH_NOT_FOUND = 3, ERROR_INVALID_HANDLE = 6, ERROR_NO_MORE_FILES = 18,
              ERROR_FILE_EXISTS = 80, ERROR_ALREADY_EXISTS = 183, ERROR_TIMEOUT = 1460, ERROR_PROC_NOT_FOUND = 127,
              ERROR_MOD_NOT_FOUND = 126, ERROR_NOT_SUPPORTED = 50;

// ---- kernel objects ----
struct EventObject : KObject {
    bool manual, state;
    EventObject(bool m, bool s) : manual(m), state(s) {}
    bool signaled() override { return state; }
    void consume() override {
        if (!manual) state = false;
    }
};
struct SemaphoreObject : KObject {
    s32 count, max;
    SemaphoreObject(s32 c, s32 m) : count(c), max(m) {}
    bool signaled() override { return count > 0; }
    void consume() override { count--; }
};
struct NeverObject : KObject {};  // a handle to something that never ends (the current thread...)
struct FileObject : KObject {
    std::FILE* f = nullptr;
    std::string path;
    bool dir = false;
    ~FileObject() override {
        if (f) std::fclose(f);
    }
};
struct FindObject : KObject {
    std::vector<fs::directory_entry> entries;
    std::vector<std::string> names;
    size_t next = 0;
};

// ---- time ----
u64 fileTimeNow() {  // 100 ns since 1601
    using namespace std::chrono;
    const u64 unix100ns = u64(duration_cast<nanoseconds>(system_clock::now().time_since_epoch()).count() / 100);
    return unix100ns + 116444736000000000ull;
}
u64 fileTimeOf(fs::file_time_type t) {
    using namespace std::chrono;
    const auto sys = time_point_cast<system_clock::duration>(t - fs::file_time_type::clock::now() + system_clock::now());
    return u64(duration_cast<nanoseconds>(sys.time_since_epoch()).count() / 100) + 116444736000000000ull;
}

// ---- process, modules ----

void GetCurrentProcess(Cpu& c) { rets(c, kPseudoProcess, 0); }
void GetCurrentProcessId(Cpu& c) { rets(c, 0x1000, 0); }
void GetCurrentThread(Cpu& c) { rets(c, kPseudoThread, 0); }
void GetCurrentThreadId(Cpu& c) { rets(c, curThreadId(), 0); }
void OpenProcess(Cpu& c) { rets(c, kPseudoProcess, 3); }
void IsDebuggerPresent(Cpu& c) { rets(c, 0, 0); }
void SetUnhandledExceptionFilter(Cpu& c) { rets(c, 0, 1); }
void UnhandledExceptionFilter(Cpu& c) { rets(c, 0, 1); }
void IsProcessorFeaturePresent(Cpu& c) {
    const u32 f = c.arg(0);  // 6: SSE, 10: SSE2 (no fast-fail, no SSE3+)
    rets(c, f == 6 || f == 10 ? 1 : 0, 1);
}
void TerminateProcess(Cpu& c) { throw ModFault("TerminateProcess(" + std::to_string(c.arg(1)) + ")"); }
void ExitProcess(Cpu& c) { throw ModFault("ExitProcess(" + std::to_string(c.arg(0)) + ")"); }

u32 moduleHandle(const std::string& name) {
    if (name.empty()) return kPcImageBase;  // the game's executable
    std::string n = name;
    for (char& ch : n) ch = char(std::tolower(u8(ch)));
    if (n.find('.') == std::string::npos) n += ".dll";
    const size_t slash = n.find_last_of("/\\");
    if (slash != std::string::npos) n = n.substr(slash + 1);
    if (n == "sonic2app.exe") return kPcImageBase;
    if (Image* img = host().imageByName(n)) return img->base;
    return host().systemModule(n);
}
void GetModuleHandleA(Cpu& c) {
    const u32 h = moduleHandle(strArg(c.arg(0)));
    if (!h) setLastError(ERROR_MOD_NOT_FOUND);
    rets(c, h, 1);
}
void GetModuleHandleW(Cpu& c) {
    const u32 h = moduleHandle(wideArg(c.arg(0)));
    if (!h) setLastError(ERROR_MOD_NOT_FOUND);
    rets(c, h, 1);
}
void getModuleHandleEx(Cpu& c, bool wide) {
    const u32 flags = c.arg(0), name = c.arg(1), out = c.arg(2);
    u32 h = 0;
    if (flags & 4) {  // FROM_ADDRESS
        if (Image* img = host().imageAt(name)) h = img->base;
        else if (name >= kPcImageBase && name < kPcImageEnd) h = kPcImageBase;
    } else {
        h = moduleHandle(wide ? wideArg(name) : strArg(name));
    }
    if (out) M().w32(out, h);
    rets(c, h ? 1 : 0, 3);
}
void GetModuleHandleExA(Cpu& c) { getModuleHandleEx(c, false); }
void GetModuleHandleExW(Cpu& c) { getModuleHandleEx(c, true); }
std::string moduleFileName(u32 h) {
    if (!h || h == kPcImageBase) return std::string(kGameDir) + "\\sonic2app.exe";
    if (Image* img = host().imageAt(h)) return guestPath(img->path);
    return std::string("C:\\Windows\\System32\\") + host().systemModuleName(h);
}
void GetModuleFileNameA(Cpu& c) {
    const std::string n = moduleFileName(c.arg(0));
    const u32 cap = c.arg(2);
    const u32 len = putStr(c.arg(1), n, cap);
    rets(c, len, 3);
}
void GetModuleFileNameW(Cpu& c) {
    const std::u16string n = utf16(moduleFileName(c.arg(0)));
    const u32 cap = c.arg(2);
    rets(c, putWide(c.arg(1), n, cap), 3);
}
void GetProcAddress(Cpu& c) {
    const u32 h = c.arg(0), name = c.arg(1);
    const std::string sym = name < 0x10000 ? "#" + std::to_string(name) : M().cstr(name);
    u32 a = 0;
    if (Image* img = host().imageAt(h)) a = img->symbol(sym);
    else if (const std::string dll = host().systemModuleName(h); !dll.empty()) a = host().systemSymbol(dll, sym);
    if (!a) setLastError(ERROR_PROC_NOT_FOUND);
    rets(c, a, 2);
}
u32 loadLibrary(const std::string& name) {
    const u32 h = moduleHandle(name);
    if (h) return h;
    // A DLL of the mod's own: load it from where the path says (or the mod's folder).
    const std::string p = hostPath(name);
    std::error_code ec;
    if (fs::exists(p, ec))
        if (Image* img = host().load(p)) return img->base;
    if (host().currentModDir.size() && fs::exists(host().currentModDir + "/" + p, ec))
        if (Image* img = host().load(host().currentModDir + "/" + p)) return img->base;
    // A system DLL: the stand-ins answer for it.
    std::string n = name;
    for (char& ch : n) ch = char(std::tolower(u8(ch)));
    if (n.find('.') == std::string::npos) n += ".dll";
    return host().systemModule(n, true);
}
void LoadLibraryA(Cpu& c) { rets(c, loadLibrary(strArg(c.arg(0))), 1); }
void LoadLibraryW(Cpu& c) { rets(c, loadLibrary(wideArg(c.arg(0))), 1); }
void LoadLibraryExA(Cpu& c) { rets(c, loadLibrary(strArg(c.arg(0))), 3); }
void LoadLibraryExW(Cpu& c) { rets(c, loadLibrary(wideArg(c.arg(0))), 3); }
void FreeLibrary(Cpu& c) { rets(c, 1, 1); }
void DisableThreadLibraryCalls(Cpu& c) { rets(c, 1, 1); }

// ---- memory ----

u32 s_nextVirtual = 0x40000000;
constexpr u32 kVirtualEnd = 0x50000000;
void VirtualAlloc(Cpu& c) {
    u32 addr = c.arg(0);
    const u32 size = (c.arg(1) + 0xFFF) & ~0xFFFu;
    bool free = addr && (addr < kPcImageBase || addr >= kPcImageEnd) && addr < kGcBase;
    if (free) {
        addr &= ~0xFFFu;
        if (c.arg(2) & 0x2000)  // MEM_RESERVE: the whole range must be free
            for (u32 a = addr; a < addr + size && free; a += 0x1000) free = !M().mapped(a);
    }
    if (!free) {
        if (s_nextVirtual + size > kVirtualEnd) return setLastError(8), rets(c, 0, 4);
        addr = s_nextVirtual;
        s_nextVirtual += (size + 0xFFFF) & ~0xFFFFu;
    }
    M().map(addr, size);
    rets(c, addr, 4);
}
void VirtualFree(Cpu& c) { rets(c, 1, 3); }
void VirtualProtect(Cpu& c) {
    if (c.arg(3)) M().w32(c.arg(3), 0x40);  // PAGE_EXECUTE_READWRITE
    rets(c, 1, 4);
}
void VirtualQuery(Cpu& c) {
    const u32 a = c.arg(0), mbi = c.arg(1);
    const u32 page = a & ~0xFFFu;
    u32 base = page, size = 0x1000, state = 0x10000 /* MEM_FREE */, protect = 1, type = 0;
    if (a >= kPcImageBase && a < kPcImageEnd) {
        base = kPcImageBase, size = kPcImageEnd - kPcImageBase, state = 0x1000, protect = 0x20, type = 0x1000000;
    } else if (Image* img = host().imageAt(a)) {
        base = img->base, size = img->size, state = 0x1000, protect = 0x40, type = 0x1000000;
    } else if (M().mapped(a)) {
        state = 0x1000, protect = 4, type = 0x20000;
    }
    M().w32(mbi + 0, a >= kPcImageBase && a < kPcImageEnd ? page : base);
    M().w32(mbi + 4, base);
    M().w32(mbi + 8, protect);
    M().w32(mbi + 12, size);
    M().w32(mbi + 16, state);
    M().w32(mbi + 20, protect);
    M().w32(mbi + 24, type);
    rets(c, 28, 3);
}
void FlushInstructionCache(Cpu& c) { rets(c, 1, 3); }
// Aligned dwords as dwords: a pointer written into the game (WriteData of a function pointer) is
// translated whole.
void copyMemory(u32 dst, u32 src, u32 n) {
    for (u32 i = 0; i < n;) {
        if (n - i >= 4 && !((dst + i) & 3)) M().w32(dst + i, M().r32(src + i)), i += 4;
        else M().w8(dst + i, M().r8(src + i)), i++;
    }
}
void WriteProcessMemory(Cpu& c) {
    const u32 dst = c.arg(1), src = c.arg(2), n = c.arg(3), written = c.arg(4);
    copyMemory(dst, src, n);
    if (written) M().w32(written, n);  // translated when the mod code is done (commitPatches)
    rets(c, 1, 5);
}
void ReadProcessMemory(Cpu& c) {
    const u32 src = c.arg(1), dst = c.arg(2), n = c.arg(3), got = c.arg(4);
    copyMemory(dst, src, n);
    if (got) M().w32(got, n);
    rets(c, 1, 5);
}
void LocalFree(Cpu& c) {
    host().free(c.arg(0));
    rets(c, 0, 1);
}
void LocalAlloc(Cpu& c) {
    const u32 p = host().alloc(c.arg(1));
    if (c.arg(0) & 0x40)
        for (u32 i = 0; i < c.arg(1); i++) M().w8(p + i, 0);
    rets(c, p, 2);
}
void GetProcessHeap(Cpu& c) { rets(c, 0x00F00000, 0); }
void HeapAlloc(Cpu& c) {
    const u32 p = host().alloc(c.arg(2));
    if (c.arg(1) & 8)
        for (u32 i = 0; i < c.arg(2); i++) M().w8(p + i, 0);
    rets(c, p, 3);
}
void HeapFree(Cpu& c) {
    host().free(c.arg(2));
    rets(c, 1, 3);
}
void HeapReAlloc(Cpu& c) {
    const u32 old = c.arg(2), n = c.arg(3);
    const u32 p = host().alloc(n);
    const u32 keep = std::min(n, host().blockSize(old));
    for (u32 i = 0; i < keep; i++) M().w8(p + i, M().r8(old + i));
    host().free(old);
    rets(c, p, 4);
}
void HeapSize(Cpu& c) { rets(c, host().blockSize(c.arg(2)), 3); }
void GetNativeSystemInfo(Cpu& c) {
    const u32 p = c.arg(0);
    M().w16(p + 0, 0);  // x86
    M().w16(p + 2, 0);
    M().w32(p + 4, 0x1000);
    M().w32(p + 8, 0x10000);
    M().w32(p + 12, 0x7FFEFFFF);
    M().w32(p + 16, 0xF);
    M().w32(p + 20, 4);
    M().w32(p + 24, 586);
    M().w32(p + 28, 0x10000);
    M().w16(p + 32, 6);
    M().w16(p + 34, 0x3A09);
    rets(c, 0, 1);
}
void GetProcessAffinityMask(Cpu& c) {
    if (c.arg(1)) M().w32(c.arg(1), 0xF);
    if (c.arg(2)) M().w32(c.arg(2), 0xF);
    rets(c, 1, 3);
}
void SetProcessAffinityMask(Cpu& c) { rets(c, 1, 2); }
void GetProcessTimes(Cpu& c) {
    for (int i = 1; i <= 4; i++)
        if (c.arg(i)) M().w64(c.arg(i), i == 1 ? fileTimeNow() : 0);
    rets(c, 1, 5);
}
void GetThreadTimes(Cpu& c) { GetProcessTimes(c); }

// ---- threads ----

void CreateThread(Cpu& c) {
    u32 id = 0;
    const u32 h = threadCreate(c.arg(2), c.arg(3), c.arg(1), c.arg(4) & 4, &id);
    if (c.arg(5)) M().w32(c.arg(5), id);
    rets(c, h, 6);
}
void ExitThread(Cpu& c) { threadExit(c.arg(0)); }
void GetExitCodeThread(Cpu& c) {
    u32 code = 0;
    threadDone(c.arg(0), &code);
    M().w32(c.arg(1), code);
    rets(c, 1, 2);
}
void ResumeThread(Cpu& c) {
    threadResume(c.arg(0));
    rets(c, 1, 1);
}
void SuspendThread(Cpu& c) { rets(c, 0, 1); }
void GetThreadPriority(Cpu& c) { rets(c, 0, 1); }
void SetThreadPriority(Cpu& c) { rets(c, 1, 2); }
void GetThreadContext(Cpu& c) { rets(c, (setLastError(ERROR_NOT_SUPPORTED), 0), 2); }
void SetThreadContext(Cpu& c) { rets(c, (setLastError(ERROR_NOT_SUPPORTED), 0), 2); }
void Sleep(Cpu& c) {
    const u32 ms = c.arg(0);
    if (ms) gilWait([] { return false; }, ms);
    else gilYield();
    rets(c, 0, 1);
}
void SleepEx(Cpu& c) {
    if (c.arg(0)) gilWait([] { return false; }, c.arg(0));
    rets(c, 0, 2);
}
void SwitchToThread(Cpu& c) {
    gilYield();
    rets(c, 1, 0);
}
void GetCurrentProcessorNumber(Cpu& c) { rets(c, 0, 0); }

// TLS (TlsAlloc and fiber-local storage, which is the same here)
void TlsAlloc(Cpu& c) { rets(c, tlsSlotAlloc(), 0); }
void TlsFree(Cpu& c) {
    tlsSlotFree(c.arg(0));
    rets(c, 1, 1);
}
void TlsGetValue(Cpu& c) {
    setLastError(0);
    rets(c, tlsSlotGet(c.arg(0)), 1);
}
void TlsSetValue(Cpu& c) {
    tlsSlotSet(c.arg(0), c.arg(1));
    rets(c, 1, 2);
}
void FlsAlloc(Cpu& c) { rets(c, tlsSlotAlloc(), 1); }

// ---- synchronisation ----

std::shared_ptr<KObject> waitable(u32 h) {
    if (h == kPseudoThread || h == kPseudoProcess) return std::make_shared<NeverObject>();
    return handleGet(h);
}
void WaitForSingleObject(Cpu& c) {
    auto o = waitable(c.arg(0));
    if (!o) return setLastError(ERROR_INVALID_HANDLE), rets(c, 0xFFFFFFFF, 2);
    if (!gilWait([&] { return o->signaled(); }, c.arg(1))) return rets(c, 0x102, 2);  // WAIT_TIMEOUT
    o->consume();
    rets(c, 0, 2);
}
void WaitForSingleObjectEx(Cpu& c) {
    auto o = waitable(c.arg(0));
    if (!o) return setLastError(ERROR_INVALID_HANDLE), rets(c, 0xFFFFFFFF, 3);
    if (!gilWait([&] { return o->signaled(); }, c.arg(1))) return rets(c, 0x102, 3);
    o->consume();
    rets(c, 0, 3);
}
void WaitForMultipleObjects(Cpu& c) {
    const u32 n = c.arg(0), list = c.arg(1), all = c.arg(2), ms = c.arg(3);
    std::vector<std::shared_ptr<KObject>> os;
    for (u32 i = 0; i < n; i++) {
        auto o = waitable(M().r32(list + 4 * i));
        if (!o) return setLastError(ERROR_INVALID_HANDLE), rets(c, 0xFFFFFFFF, 4);
        os.push_back(o);
    }
    u32 which = 0;
    const bool ok = gilWait(
        [&] {
            if (all) return std::all_of(os.begin(), os.end(), [](auto& o) { return o->signaled(); });
            for (u32 i = 0; i < n; i++)
                if (os[i]->signaled()) return which = i, true;
            return false;
        },
        ms);
    if (!ok) return rets(c, 0x102, 4);
    if (all)
        for (auto& o : os) o->consume();
    else os[which]->consume();
    rets(c, all ? 0 : which, 4);
}
void CreateEventA(Cpu& c) { rets(c, handleNew(std::make_shared<EventObject>(c.arg(1) != 0, c.arg(2) != 0)), 4); }
void CreateEventExW(Cpu& c) {  // flags: 1 manual reset, 2 initially set
    rets(c, handleNew(std::make_shared<EventObject>(c.arg(2) & 1, c.arg(2) & 2)), 4);
}
void SetEvent(Cpu& c) {
    if (auto e = handleAs<EventObject>(c.arg(0))) e->state = true, gilNotify();
    rets(c, 1, 1);
}
void ResetEvent(Cpu& c) {
    if (auto e = handleAs<EventObject>(c.arg(0))) e->state = false;
    rets(c, 1, 1);
}
void CreateSemaphoreA(Cpu& c) { rets(c, handleNew(std::make_shared<SemaphoreObject>(s32(c.arg(1)), s32(c.arg(2)))), 4); }
void CreateSemaphoreExW(Cpu& c) { rets(c, handleNew(std::make_shared<SemaphoreObject>(s32(c.arg(1)), s32(c.arg(2)))), 6); }
void ReleaseSemaphore(Cpu& c) {
    auto s = handleAs<SemaphoreObject>(c.arg(0));
    if (!s) return rets(c, 0, 3);
    if (c.arg(2)) M().w32(c.arg(2), u32(s->count));
    s->count = std::min(s->max, s->count + s32(c.arg(1)));
    gilNotify();
    rets(c, 1, 3);
}
void CloseHandle(Cpu& c) { rets(c, handleClose(c.arg(0)) || c.arg(0) >= 0xFFFFFFFE ? 1 : 0, 1); }
void DuplicateHandle(Cpu& c) {
    const u32 src = c.arg(1), out = c.arg(3);
    std::shared_ptr<KObject> o = src == kPseudoThread || src == kPseudoProcess ? std::make_shared<NeverObject>() : handleGet(src);
    if (!o) return setLastError(ERROR_INVALID_HANDLE), rets(c, 0, 7);
    if (out) M().w32(out, handleNew(o));
    rets(c, 1, 7);
}

// CRITICAL_SECTION: DebugInfo, LockCount, RecursionCount, OwningThread, LockSemaphore, SpinCount
void InitializeCriticalSection(Cpu& c) {
    for (u32 i = 0; i < 24; i += 4) M().w32(c.arg(0) + i, 0);
    M().w32(c.arg(0) + 4, 0xFFFFFFFF);
    rets(c, 0, 1);
}
void InitializeCriticalSectionAndSpinCount(Cpu& c) {
    for (u32 i = 0; i < 24; i += 4) M().w32(c.arg(0) + i, 0);
    M().w32(c.arg(0) + 4, 0xFFFFFFFF);
    rets(c, 1, 2);
}
void InitializeCriticalSectionEx(Cpu& c) {
    for (u32 i = 0; i < 24; i += 4) M().w32(c.arg(0) + i, 0);
    M().w32(c.arg(0) + 4, 0xFFFFFFFF);
    rets(c, 1, 3);
}
void DeleteCriticalSection(Cpu& c) { rets(c, 0, 1); }
void enterCs(u32 cs) {
    const u32 me = curThreadId();
    if (M().r32(cs + 12) == me) {
        M().w32(cs + 8, M().r32(cs + 8) + 1);
        return;
    }
    gilWait([&] { return M().r32(cs + 12) == 0; });
    M().w32(cs + 12, me);
    M().w32(cs + 8, 1);
}
void leaveCs(u32 cs) {
    const u32 n = M().r32(cs + 8) - 1;
    M().w32(cs + 8, n);
    if (!n) {
        M().w32(cs + 12, 0);
        gilNotify();
    }
}
void EnterCriticalSection(Cpu& c) {
    enterCs(c.arg(0));
    rets(c, 0, 1);
}
void LeaveCriticalSection(Cpu& c) {
    leaveCs(c.arg(0));
    rets(c, 0, 1);
}
void TryEnterCriticalSection(Cpu& c) {
    const u32 cs = c.arg(0), owner = M().r32(cs + 12);
    if (owner && owner != curThreadId()) return rets(c, 0, 1);
    enterCs(cs);
    rets(c, 1, 1);
}

// SRWLOCK: 0 free, 0xFFFFFFFF held exclusively, else the number of shared holders.
void srwExclusive(u32 l) {
    gilWait([&] { return M().r32(l) == 0; });
    M().w32(l, 0xFFFFFFFF);
}
void srwShared(u32 l) {
    gilWait([&] { return M().r32(l) != 0xFFFFFFFF; });
    M().w32(l, M().r32(l) + 1);
}
void srwRelease(u32 l) {
    const u32 v = M().r32(l);
    M().w32(l, v == 0xFFFFFFFF || v == 0 ? 0 : v - 1);
    gilNotify();
}
void InitializeSRWLock(Cpu& c) {
    M().w32(c.arg(0), 0);
    rets(c, 0, 1);
}
void AcquireSRWLockExclusive(Cpu& c) {
    srwExclusive(c.arg(0));
    rets(c, 0, 1);
}
void AcquireSRWLockShared(Cpu& c) {
    srwShared(c.arg(0));
    rets(c, 0, 1);
}
void ReleaseSRWLock(Cpu& c) {
    srwRelease(c.arg(0));
    rets(c, 0, 1);
}
void TryAcquireSRWLockExclusive(Cpu& c) {
    if (M().r32(c.arg(0))) return rets(c, 0, 1);
    M().w32(c.arg(0), 0xFFFFFFFF);
    rets(c, 1, 1);
}
void TryAcquireSRWLockShared(Cpu& c) {
    if (M().r32(c.arg(0)) == 0xFFFFFFFF) return rets(c, 0, 1);
    M().w32(c.arg(0), M().r32(c.arg(0)) + 1);
    rets(c, 1, 1);
}
// CONDITION_VARIABLE: a generation count; waking moves it on (waiters may wake spuriously).
void InitializeConditionVariable(Cpu& c) {
    M().w32(c.arg(0), 0);
    rets(c, 0, 1);
}
void WakeConditionVariable(Cpu& c) {
    M().w32(c.arg(0), M().r32(c.arg(0)) + 1);
    gilNotify();
    rets(c, 0, 1);
}
void SleepConditionVariableSRW(Cpu& c) {
    const u32 cv = c.arg(0), lock = c.arg(1), ms = c.arg(2), shared = c.arg(3) & 1;
    const u32 gen = M().r32(cv);
    srwRelease(lock);
    const bool ok = gilWait([&] { return M().r32(cv) != gen; }, ms);
    if (shared) srwShared(lock);
    else srwExclusive(lock);
    if (!ok) setLastError(ERROR_TIMEOUT);
    rets(c, ok, 4);
}
void SleepConditionVariableCS(Cpu& c) {
    const u32 cv = c.arg(0), cs = c.arg(1), ms = c.arg(2);
    const u32 gen = M().r32(cv), depth = M().r32(cs + 8);
    M().w32(cs + 8, 1);
    leaveCs(cs);
    const bool ok = gilWait([&] { return M().r32(cv) != gen; }, ms);
    enterCs(cs);
    M().w32(cs + 8, depth);
    if (!ok) setLastError(ERROR_TIMEOUT);
    rets(c, ok, 3);
}
void InitOnceExecuteOnce(Cpu& c) {
    const u32 once = c.arg(0), fn = c.arg(1), param = c.arg(2), ctx = c.arg(3);
    if (M().r32(once) == 0) {
        M().w32(once, 1);
        const u32 ok = c.call(fn, {once, param, ctx});
        M().w32(once, ok ? 2 : 0);
        return rets(c, ok, 4);
    }
    gilWait([&] { return M().r32(once) != 1; });
    rets(c, 1, 4);
}

// ---- files ----

u32 fileAttributes(const fs::path& p) {
    std::error_code ec;
    const auto st = fs::status(p, ec);
    if (ec || !fs::exists(st)) return 0xFFFFFFFF;
    return fs::is_directory(st) ? 0x10 : 0x20;
}
void CreateFileW_(Cpu& c, bool wide) {
    const std::string name = wide ? wideArg(c.arg(0)) : strArg(c.arg(0));
    const u32 access = c.arg(1), disp = c.arg(4), flags = c.arg(5);
    const std::string p = hostPath(name);
    std::error_code ec;
    auto o = std::make_shared<FileObject>();
    o->path = p;
    const bool exists = fs::exists(p, ec);
    if (fs::is_directory(p, ec)) {
        if (!(flags & 0x02000000)) return setLastError(5), rets(c, kInvalidHandle, 7);  // access denied
        o->dir = true;
        return rets(c, handleNew(o), 7);
    }
    const bool write = access & 0xC0000000 & 0x40000000;
    std::string mode;
    switch (disp) {
    case 1: if (exists) return setLastError(ERROR_FILE_EXISTS), rets(c, kInvalidHandle, 7); mode = "w+b"; break;
    case 2: mode = "w+b"; break;
    case 3: if (!exists) return setLastError(ERROR_FILE_NOT_FOUND), rets(c, kInvalidHandle, 7); mode = write ? "r+b" : "rb"; break;
    case 4: mode = exists ? (write ? "r+b" : "rb") : "w+b"; break;
    case 5: if (!exists) return setLastError(ERROR_FILE_NOT_FOUND), rets(c, kInvalidHandle, 7); mode = "w+b"; break;
    default: return setLastError(87), rets(c, kInvalidHandle, 7);
    }
    o->f = std::fopen(p.c_str(), mode.c_str());
    if (!o->f) return setLastError(ERROR_PATH_NOT_FOUND), rets(c, kInvalidHandle, 7);
    setLastError(disp == 4 && exists ? ERROR_ALREADY_EXISTS : 0);
    rets(c, handleNew(o), 7);
}
void CreateFileW(Cpu& c) { CreateFileW_(c, true); }
void CreateFileA(Cpu& c) { CreateFileW_(c, false); }
void ReadFile(Cpu& c) {
    auto o = handleAs<FileObject>(c.arg(0));
    const u32 buf = c.arg(1), n = c.arg(2), got = c.arg(3);
    if (!o || !o->f) return setLastError(ERROR_INVALID_HANDLE), rets(c, 0, 5);
    std::vector<u8> tmp(n);
    const size_t r = std::fread(tmp.data(), 1, n, o->f);
    M().write(buf, tmp.data(), u32(r));
    if (got) M().w32(got, u32(r));
    rets(c, 1, 5);
}
void WriteFile(Cpu& c) {
    auto o = handleAs<FileObject>(c.arg(0));
    const u32 buf = c.arg(1), n = c.arg(2), put = c.arg(3);
    if (!o || !o->f) return setLastError(ERROR_INVALID_HANDLE), rets(c, 0, 5);
    std::vector<u8> tmp(n);
    M().read(buf, tmp.data(), n);
    const size_t w = std::fwrite(tmp.data(), 1, n, o->f);
    if (put) M().w32(put, u32(w));
    rets(c, 1, 5);
}
s64 fileSize(std::FILE* f) {
    const long pos = std::ftell(f);
    std::fseek(f, 0, SEEK_END);
    const long size = std::ftell(f);
    std::fseek(f, pos, SEEK_SET);
    return size;
}
void GetFileSize(Cpu& c) {
    auto o = handleAs<FileObject>(c.arg(0));
    if (!o || !o->f) return rets(c, 0xFFFFFFFF, 2);
    if (c.arg(1)) M().w32(c.arg(1), 0);
    rets(c, u32(fileSize(o->f)), 2);
}
void GetFileSizeEx(Cpu& c) {
    auto o = handleAs<FileObject>(c.arg(0));
    if (!o || !o->f) return rets(c, 0, 2);
    M().w64(c.arg(1), u64(fileSize(o->f)));
    rets(c, 1, 2);
}
void SetFilePointer(Cpu& c) {
    auto o = handleAs<FileObject>(c.arg(0));
    if (!o || !o->f) return rets(c, 0xFFFFFFFF, 4);
    s64 off = s32(c.arg(1));
    if (c.arg(2)) off = s64(u64(c.arg(1)) | u64(M().r32(c.arg(2))) << 32);
    std::fseek(o->f, long(off), int(c.arg(3)));
    const long pos = std::ftell(o->f);
    if (c.arg(2)) M().w32(c.arg(2), 0);
    rets(c, u32(pos), 4);
}
void SetFilePointerEx(Cpu& c) {
    auto o = handleAs<FileObject>(c.arg(0));
    if (!o || !o->f) return rets(c, 0, 5);
    std::fseek(o->f, long(s64(arg64(c, 1))), int(c.arg(4)));
    if (c.arg(3)) M().w64(c.arg(3), u64(std::ftell(o->f)));
    rets(c, 1, 5);
}
void GetFileType(Cpu& c) { rets(c, handleAs<FileObject>(c.arg(0)) ? 1 : 2, 1); }
void FlushFileBuffers(Cpu& c) {
    if (auto o = handleAs<FileObject>(c.arg(0)); o && o->f) std::fflush(o->f);
    rets(c, 1, 1);
}
void GetFileInformationByHandle(Cpu& c) {
    auto o = handleAs<FileObject>(c.arg(0));
    const u32 p = c.arg(1);
    if (!o) return setLastError(ERROR_INVALID_HANDLE), rets(c, 0, 2);
    std::error_code ec;
    const u64 t = fileTimeOf(fs::last_write_time(o->path, ec));
    const u64 size = o->dir ? 0 : u64(fs::file_size(o->path, ec));
    M().w32(p + 0, o->dir ? 0x10 : 0x20);
    for (int i = 0; i < 3; i++) M().w64(p + 4 + 8 * u32(i), t);
    M().w32(p + 28, 0x5A2B);  // volume serial
    M().w32(p + 32, u32(size >> 32));
    M().w32(p + 36, u32(size));
    M().w32(p + 40, 1);
    const u64 id = std::hash<std::string>{}(fs::absolute(o->path, ec).generic_string());
    M().w32(p + 44, u32(id >> 32));
    M().w32(p + 48, u32(id));
    rets(c, 1, 2);
}
void GetFileAttributesW(Cpu& c) {
    const u32 a = fileAttributes(hostPath(wideArg(c.arg(0))));
    if (a == 0xFFFFFFFF) setLastError(ERROR_FILE_NOT_FOUND);
    rets(c, a, 1);
}
void GetFileAttributesA(Cpu& c) {
    const u32 a = fileAttributes(hostPath(strArg(c.arg(0))));
    if (a == 0xFFFFFFFF) setLastError(ERROR_FILE_NOT_FOUND);
    rets(c, a, 1);
}
void GetFileAttributesExW(Cpu& c) {
    const std::string p = hostPath(wideArg(c.arg(0)));
    const u32 a = fileAttributes(p), out = c.arg(2);
    if (a == 0xFFFFFFFF) return setLastError(ERROR_FILE_NOT_FOUND), rets(c, 0, 3);
    std::error_code ec;
    const u64 t = fileTimeOf(fs::last_write_time(p, ec));
    const u64 size = a & 0x10 ? 0 : u64(fs::file_size(p, ec));
    M().w32(out, a);
    for (int i = 0; i < 3; i++) M().w64(out + 4 + 8 * u32(i), t);
    M().w32(out + 28, u32(size >> 32));
    M().w32(out + 32, u32(size));
    rets(c, 1, 3);
}
void DeleteFileW(Cpu& c) {
    std::error_code ec;
    rets(c, fs::remove(hostPath(wideArg(c.arg(0))), ec) ? 1 : (setLastError(ERROR_FILE_NOT_FOUND), 0), 1);
}
void RemoveDirectoryW(Cpu& c) {
    std::error_code ec;
    rets(c, fs::remove(hostPath(wideArg(c.arg(0))), ec) ? 1 : (setLastError(ERROR_PATH_NOT_FOUND), 0), 1);
}
void CreateDirectoryW(Cpu& c) {
    std::error_code ec;
    rets(c, fs::create_directory(hostPath(wideArg(c.arg(0))), ec) ? 1 : (setLastError(ERROR_ALREADY_EXISTS), 0), 2);
}
void CreateDirectoryA(Cpu& c) {
    std::error_code ec;
    rets(c, fs::create_directory(hostPath(strArg(c.arg(0))), ec) ? 1 : (setLastError(ERROR_ALREADY_EXISTS), 0), 2);
}
void MoveFileExW(Cpu& c) {
    std::error_code ec;
    fs::rename(hostPath(wideArg(c.arg(0))), hostPath(wideArg(c.arg(1))), ec);
    rets(c, ec ? 0 : 1, 3);
}
void CopyFileW(Cpu& c) {
    std::error_code ec;
    fs::copy_file(hostPath(wideArg(c.arg(0))), hostPath(wideArg(c.arg(1))),
                  c.arg(2) ? fs::copy_options::none : fs::copy_options::overwrite_existing, ec);
    rets(c, ec ? 0 : 1, 3);
}
void CreateHardLinkW(Cpu& c) { rets(c, (setLastError(ERROR_NOT_SUPPORTED), 0), 3); }
void GetDiskFreeSpaceExW(Cpu& c) {
    for (int i = 1; i <= 3; i++)
        if (c.arg(i)) M().w64(c.arg(i), 64ull << 30);
    rets(c, 1, 4);
}
void GetTempPathW(Cpu& c) { rets(c, putWide(c.arg(1), utf16(std::string(kGameDir) + "\\temp\\"), c.arg(0)), 2); }
void GetCurrentDirectoryA(Cpu& c) { rets(c, putStr(c.arg(1), kGameDir, c.arg(0)), 2); }
void GetCurrentDirectoryW(Cpu& c) { rets(c, putWide(c.arg(1), utf16(kGameDir), c.arg(0)), 2); }
std::string fullPath(const std::string& p) {
    if (p.size() > 1 && p[1] == ':') return p;
    if (!p.empty() && (p[0] == '\\' || p[0] == '/')) return std::string("C:") + p;
    std::string r = std::string(kGameDir) + "\\" + p;
    for (char& ch : r)
        if (ch == '/') ch = '\\';
    return r;
}
void GetFullPathNameW(Cpu& c) {
    const std::u16string full = utf16(fullPath(wideArg(c.arg(0))));
    const u32 cap = c.arg(1);
    if (cap <= full.size()) return rets(c, u32(full.size() + 1), 4);
    putWide(c.arg(2), full, cap);
    if (c.arg(3)) {  // the file part
        const size_t slash = full.find_last_of(u"\\/");
        M().w32(c.arg(3), slash == std::u16string::npos ? c.arg(2) : c.arg(2) + 2 * u32(slash + 1));
    }
    rets(c, u32(full.size()), 4);
}
void GetFullPathNameA(Cpu& c) {
    const std::string full = fullPath(strArg(c.arg(0)));
    const u32 cap = c.arg(1);
    if (cap <= full.size()) return rets(c, u32(full.size() + 1), 4);
    putStr(c.arg(2), full, cap);
    if (c.arg(3)) {
        const size_t slash = full.find_last_of("\\/");
        M().w32(c.arg(3), slash == std::string::npos ? c.arg(2) : c.arg(2) + u32(slash + 1));
    }
    rets(c, u32(full.size()), 4);
}

// FindFirstFile: the directory's entries (with . and ..) matching the pattern's wildcards.
bool wildMatch(const char* p, const char* s) {
    for (; *p; p++, s++) {
        if (*p == '*') {
            while (p[1] == '*') p++;
            if (!p[1]) return true;
            for (; *s; s++)
                if (wildMatch(p + 1, s)) return true;
            return false;
        }
        if (!*s) return false;
        if (*p != '?' && std::tolower(u8(*p)) != std::tolower(u8(*s))) return false;
    }
    return !*s;
}
std::shared_ptr<FindObject> findOpen(const std::string& pattern) {
    std::string pat = pattern;
    for (char& ch : pat)
        if (ch == '\\') ch = '/';
    const size_t slash = pat.find_last_of('/');
    const std::string dir = slash == std::string::npos ? "." : pat.substr(0, slash);
    std::string mask = slash == std::string::npos ? pat : pat.substr(slash + 1);
    if (mask == "*.*") mask = "*";
    auto o = std::make_shared<FindObject>();
    const std::string hdir = hostPath(dir.empty() ? "/" : dir);
    std::error_code ec;
    if (!fs::is_directory(hdir, ec)) return nullptr;
    if (wildMatch(mask.c_str(), ".")) o->entries.push_back(fs::directory_entry(hdir)), o->names.push_back(".");
    if (wildMatch(mask.c_str(), "..")) o->entries.push_back(fs::directory_entry(hdir)), o->names.push_back("..");
    for (const auto& e : fs::directory_iterator(hdir, ec)) {
        const std::string n = e.path().filename().string();
        if (wildMatch(mask.c_str(), n.c_str())) o->entries.push_back(e), o->names.push_back(n);
    }
    return o->entries.empty() ? nullptr : o;
}
void fillFindData(u32 p, const fs::directory_entry& e, const std::string& name, bool wide) {
    std::error_code ec;
    const bool dir = e.is_directory(ec);
    const u64 t = fileTimeOf(e.last_write_time(ec));
    const u64 size = dir ? 0 : u64(e.file_size(ec));
    M().w32(p + 0, dir ? 0x10 : 0x20);
    for (int i = 0; i < 3; i++) M().w64(p + 4 + 8 * u32(i), t);
    M().w32(p + 28, u32(size >> 32));
    M().w32(p + 32, u32(size));
    M().w32(p + 36, 0);
    M().w32(p + 40, 0);
    if (wide) {
        putWide(p + 44, utf16(name), 260);
        M().w16(p + 44 + 520, 0);
    } else {
        putStr(p + 44, name, 260);
        M().w8(p + 44 + 260, 0);
    }
}
void findFirst(Cpu& c, bool wide) {
    auto o = findOpen(wide ? wideArg(c.arg(0)) : strArg(c.arg(0)));
    if (!o) return setLastError(ERROR_FILE_NOT_FOUND), rets(c, kInvalidHandle, 2);
    fillFindData(c.arg(1), o->entries[0], o->names[0], wide);
    o->next = 1;
    rets(c, handleNew(o), 2);
}
void findNext(Cpu& c, bool wide) {
    auto o = handleAs<FindObject>(c.arg(0));
    if (!o || o->next >= o->entries.size()) return setLastError(ERROR_NO_MORE_FILES), rets(c, 0, 2);
    fillFindData(c.arg(1), o->entries[o->next], o->names[o->next], wide);
    o->next++;
    rets(c, 1, 2);
}
void FindFirstFileA(Cpu& c) { findFirst(c, false); }
void FindFirstFileW(Cpu& c) { findFirst(c, true); }
void FindNextFileA(Cpu& c) { findNext(c, false); }
void FindNextFileW(Cpu& c) { findNext(c, true); }
void FindFirstFileExW(Cpu& c) {  // (name, info level, data, search op, filter, flags)
    auto o = findOpen(wideArg(c.arg(0)));
    if (!o) return setLastError(ERROR_FILE_NOT_FOUND), rets(c, kInvalidHandle, 6);
    fillFindData(c.arg(2), o->entries[0], o->names[0], true);
    o->next = 1;
    rets(c, handleNew(o), 6);
}
void FindClose(Cpu& c) { rets(c, handleClose(c.arg(0)) ? 1 : 0, 1); }

// ---- strings and errors ----

void MultiByteToWideChar(Cpu& c) {
    const u32 src = c.arg(2), dst = c.arg(4), cap = c.arg(5);
    const s32 len = s32(c.arg(3));
    std::string s;
    if (len < 0) s = M().cstr(src, 1 << 20) + std::string(1, '\0');
    else {
        s.resize(size_t(len));
        M().read(src, s.data(), u32(len));
    }
    const std::u16string w = utf16(s);
    if (!cap) return rets(c, u32(w.size()), 6);
    const u32 n = std::min<u32>(u32(w.size()), cap);
    for (u32 i = 0; i < n; i++) M().w16(dst + 2 * i, u16(w[i]));
    rets(c, n, 6);
}
void WideCharToMultiByte(Cpu& c) {
    const u32 src = c.arg(2), dst = c.arg(4), cap = c.arg(5);
    const s32 len = s32(c.arg(3));
    std::u16string w;
    if (len < 0) w = M().wstr(src, 1 << 20) + std::u16string(1, u'\0');
    else
        for (s32 i = 0; i < len; i++) w += char16_t(M().r16(src + 2 * u32(i)));
    std::string s = c.arg(0) == 65001 ? utf8(w) : std::string();
    if (c.arg(0) != 65001)  // ANSI code pages: Latin-1, '?' outside it
        for (char16_t ch : w) s += ch < 0x100 ? char(ch) : '?';
    if (c.arg(7)) M().w32(c.arg(7), 0);
    if (!cap) return rets(c, u32(s.size()), 8);
    const u32 n = std::min<u32>(u32(s.size()), cap);
    M().write(dst, s.data(), n);
    rets(c, n, 8);
}
void GetLastError(Cpu& c) { rets(c, lastError(), 0); }
void SetLastError(Cpu& c) {
    setLastError(c.arg(0));
    rets(c, 0, 1);
}
void FormatMessageA(Cpu& c) {  // (flags, source, id, lang, buffer, size, args)
    const u32 flags = c.arg(0), buf = c.arg(4), size = c.arg(5);
    const std::string msg = "Error " + std::to_string(c.arg(2)) + ".\r\n";
    if (flags & 0x100) {  // ALLOCATE_BUFFER
        const u32 p = host().allocString(msg);
        M().w32(buf, p);
        return rets(c, u32(msg.size()), 7);
    }
    rets(c, putStr(buf, msg, size), 7);
}
void FormatMessageW(Cpu& c) {
    const u32 flags = c.arg(0), buf = c.arg(4), size = c.arg(5);
    const std::u16string msg = utf16("Error " + std::to_string(c.arg(2)) + ".\r\n");
    if (flags & 0x100) {
        const u32 p = host().alloc(2 * u32(msg.size() + 1));
        putWide(p, msg);
        M().w32(buf, p);
        return rets(c, u32(msg.size()), 7);
    }
    rets(c, putWide(buf, msg, size), 7);
}
void OutputDebugStringA(Cpu& c) {
    rt::log("[%s] %s\n", host().currentMod.c_str(), strArg(c.arg(0)).c_str());
    rets(c, 0, 1);
}
void OutputDebugStringW(Cpu& c) {
    rt::log("[%s] %s\n", host().currentMod.c_str(), wideArg(c.arg(0)).c_str());
    rets(c, 0, 1);
}
void lstrlenA(Cpu& c) { rets(c, c.arg(0) ? u32(M().cstr(c.arg(0), 1 << 20).size()) : 0, 1); }
void lstrlenW(Cpu& c) { rets(c, c.arg(0) ? u32(M().wstr(c.arg(0), 1 << 20).size()) : 0, 1); }
void GetEnvironmentVariableA(Cpu& c) { rets(c, (setLastError(203), 0), 3); }
void GetEnvironmentVariableW(Cpu& c) { rets(c, (setLastError(203), 0), 3); }
void GetCommandLineA(Cpu& c) {
    static u32 s = 0;
    if (!s) s = host().allocString(std::string(kGameDir) + "\\sonic2app.exe");
    rets(c, s, 0);
}

// ---- time ----

void GetSystemTimeAsFileTime(Cpu& c) {
    M().w64(c.arg(0), fileTimeNow());
    rets(c, 0, 1);
}
void QueryPerformanceCounter(Cpu& c) {
    M().w64(c.arg(0), u64(std::chrono::steady_clock::now().time_since_epoch().count()));
    rets(c, 1, 1);
}
void QueryPerformanceFrequency(Cpu& c) {
    M().w64(c.arg(0), u64(std::chrono::steady_clock::period::den / std::chrono::steady_clock::period::num));
    rets(c, 1, 1);
}
u64 msNow() {
    using namespace std::chrono;
    return u64(duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}
void GetTickCount(Cpu& c) { rets(c, u32(msNow()), 0); }
void GetTickCount64(Cpu& c) {
    const u64 t = msNow();
    c.r[Cpu::EDX] = u32(t >> 32);
    rets(c, u32(t), 0);
}
void writeSystemTime(u32 p, std::time_t t, int ms) {
    std::tm tmv{};
#ifdef _WIN32
    gmtime_s(&tmv, &t);
#else
    gmtime_r(&t, &tmv);
#endif
    const u16 v[8] = {u16(tmv.tm_year + 1900), u16(tmv.tm_mon + 1), u16(tmv.tm_wday), u16(tmv.tm_mday),
                      u16(tmv.tm_hour), u16(tmv.tm_min), u16(tmv.tm_sec), u16(ms)};
    for (int i = 0; i < 8; i++) M().w16(p + 2 * u32(i), v[i]);
}
void FileTimeToSystemTime(Cpu& c) {
    const u64 ft = M().r64(c.arg(0));
    const u64 unix100 = ft - 116444736000000000ull;
    writeSystemTime(c.arg(1), std::time_t(unix100 / 10000000), int(unix100 / 10000 % 1000));
    rets(c, 1, 2);
}
void GetSystemTime(Cpu& c) {
    const u64 ft = fileTimeNow() - 116444736000000000ull;
    writeSystemTime(c.arg(0), std::time_t(ft / 10000000), int(ft / 10000 % 1000));
    rets(c, 0, 1);
}
void GetLocalTime(Cpu& c) { GetSystemTime(c); }
void SetSystemTime(Cpu& c) { rets(c, (setLastError(5), 0), 1); }
void GetSystemTimeAdjustment(Cpu& c) {
    M().w32(c.arg(0), 156250);
    M().w32(c.arg(1), 156250);
    M().w32(c.arg(2), 1);
    rets(c, 1, 3);
}
void InitializeSListHead(Cpu& c) {
    M().w64(c.arg(0), 0);
    rets(c, 0, 1);
}

// ---- exceptions: Win32 structured exception dispatch through the fs:[0] chain ----
// EXCEPTION_RECORD on the stack, then each registered handler is asked (phase one); the C++
// runtime's handler unwinds and continues itself. Nobody handling it stops the mod code.
void RaiseException(Cpu& c) {
    const u32 code = c.arg(0), flags = c.arg(1), n = std::min<u32>(c.arg(2), 15), args = c.arg(3);
    const u32 retAddr = M().r32(c.r[Cpu::ESP]);
    const CpuRegs at = c.save();
    // Records below the current stack.
    u32 sp = (c.r[Cpu::ESP] - 0x400) & ~15u;
    const u32 rec = sp, ctx = sp + 0x60;
    M().w32(rec + 0, code);
    M().w32(rec + 4, flags & 1);
    M().w32(rec + 8, 0);
    M().w32(rec + 12, retAddr);
    M().w32(rec + 16, n);
    for (u32 i = 0; i < n; i++) M().w32(rec + 20 + 4 * i, M().r32(args + 4 * i));
    // CONTEXT (x86): flags, then the integer registers and control at their offsets.
    for (u32 i = 0; i < 0x2CC; i += 4) M().w32(ctx + i, 0);
    M().w32(ctx + 0, 0x1007F);
    M().w32(ctx + 0x9C, at.r[Cpu::EDI]);
    M().w32(ctx + 0xA0, at.r[Cpu::ESI]);
    M().w32(ctx + 0xA4, at.r[Cpu::EBX]);
    M().w32(ctx + 0xA8, at.r[Cpu::EDX]);
    M().w32(ctx + 0xAC, at.r[Cpu::ECX]);
    M().w32(ctx + 0xB0, at.r[Cpu::EAX]);
    M().w32(ctx + 0xB4, at.r[Cpu::EBP]);
    M().w32(ctx + 0xB8, retAddr);
    M().w32(ctx + 0xC4, at.r[Cpu::ESP] + 4 + 16);  // as after the stdcall return
    c.r[Cpu::ESP] = sp - 16;
    const u32 dispatcher = sp - 8;
    for (u32 frame = M().r32(c.fsBase); frame != 0xFFFFFFFF && frame;) {
        const u32 handler = M().r32(frame + 4);
        const u32 r = c.call(handler, {rec, frame, ctx, dispatcher});
        if (r == 0) {  // ExceptionContinueExecution: resume with the (maybe changed) context
            c.restore(at);
            c.r[Cpu::EDI] = M().r32(ctx + 0x9C), c.r[Cpu::ESI] = M().r32(ctx + 0xA0), c.r[Cpu::EBX] = M().r32(ctx + 0xA4);
            c.r[Cpu::EDX] = M().r32(ctx + 0xA8), c.r[Cpu::ECX] = M().r32(ctx + 0xAC), c.r[Cpu::EAX] = M().r32(ctx + 0xB0);
            c.r[Cpu::EBP] = M().r32(ctx + 0xB4);
            c.eip = M().r32(ctx + 0xB8);
            c.r[Cpu::ESP] = M().r32(ctx + 0xC4);
            return;
        }
        frame = M().r32(frame);
    }
    char b[96];
    std::snprintf(b, sizeof b, "unhandled exception %08X raised at %08X", code, retAddr);
    throw ModFault(b);
}
// RtlUnwind(targetFrame, targetIp, record, returnValue): calls the handlers above the target
// with the unwinding flag, unlinks them, then continues at targetIp (or returns).
void RtlUnwind(Cpu& c) {
    const u32 target = c.arg(0), targetIp = c.arg(1), recIn = c.arg(2), value = c.arg(3);
    const u32 retAddr = M().r32(c.r[Cpu::ESP]);
    const u32 sp = (c.r[Cpu::ESP] - 0x200) & ~15u;
    const u32 rec = recIn ? recIn : sp;
    if (!recIn) {
        for (u32 i = 0; i < 80; i += 4) M().w32(rec + i, 0);
        M().w32(rec, 0xC0000027);  // STATUS_UNWIND
    }
    M().w32(rec + 4, M().r32(rec + 4) | (target ? 2 : 6));  // UNWINDING (| EXIT_UNWIND)
    const CpuRegs at = c.save();
    c.r[Cpu::ESP] = sp - 0x40;
    for (u32 frame = M().r32(c.fsBase); frame != 0xFFFFFFFF && frame && frame != target;) {
        const u32 next = M().r32(frame);
        c.call(M().r32(frame + 4), {rec, frame, 0, sp - 0x20});
        M().w32(c.fsBase, next);
        frame = next;
    }
    c.restore(at);
    c.r[Cpu::EAX] = value;
    c.ret(16);
    if (targetIp) c.eip = targetIp;
    (void)retAddr;
}

// ---- USER32 ----
void GetAsyncKeyState(Cpu& c) { rets(c, 0, 1); }
void GetKeyState(Cpu& c) { rets(c, 0, 1); }
void MessageBoxA(Cpu& c) {
    rt::log("[%s] message box \"%s\": %s\n", host().currentMod.c_str(), strArg(c.arg(2)).c_str(), strArg(c.arg(1)).c_str());
    rets(c, 1, 4);  // IDOK
}
void MessageBoxW(Cpu& c) {
    rt::log("[%s] message box \"%s\": %s\n", host().currentMod.c_str(), wideArg(c.arg(2)).c_str(), wideArg(c.arg(1)).c_str());
    rets(c, 1, 4);
}
void wsprintfW(Cpu& c) {  // cdecl varargs
    const std::u16string s = utf16(guestFormat(wideArg(c.arg(1)), c.r[Cpu::ESP] + 12, true));
    retc(c, putWide(c.arg(0), s, 1024));
}
void wsprintfA(Cpu& c) { retc(c, putStr(c.arg(0), guestFormat(strArg(c.arg(1)), c.r[Cpu::ESP] + 12, false), 1024)); }
void GetActiveWindow(Cpu& c) { rets(c, 0x10010, 0); }
void FindWindowA(Cpu& c) { rets(c, 0x10010, 2); }

// ---- D3DX ----
void D3DXMatrixOrthoRH(Cpu& c) {  // (out, w, h, zn, zf)
    const u32 out = c.arg(0);
    const float w = argFloat(c, 1), h = argFloat(c, 2), zn = argFloat(c, 3), zf = argFloat(c, 4);
    const float m[16] = {2 / w, 0, 0, 0, 0, 2 / h, 0, 0, 0, 0, 1 / (zn - zf), 0, 0, 0, zn / (zn - zf), 1};
    for (int i = 0; i < 16; i++) {
        u32 b;
        std::memcpy(&b, &m[i], 4);
        M().w32(out + 4 * u32(i), b);
    }
    rets(c, out, 5);
}

}  // namespace

const ImportEntry kKernel32[] = {
    {"GetCurrentProcess", GetCurrentProcess}, {"GetCurrentProcessId", GetCurrentProcessId},
    {"GetCurrentThread", GetCurrentThread}, {"GetCurrentThreadId", GetCurrentThreadId}, {"OpenProcess", OpenProcess},
    {"IsDebuggerPresent", IsDebuggerPresent}, {"SetUnhandledExceptionFilter", SetUnhandledExceptionFilter},
    {"UnhandledExceptionFilter", UnhandledExceptionFilter}, {"IsProcessorFeaturePresent", IsProcessorFeaturePresent},
    {"TerminateProcess", TerminateProcess}, {"ExitProcess", ExitProcess},
    {"GetModuleHandleA", GetModuleHandleA}, {"GetModuleHandleW", GetModuleHandleW},
    {"GetModuleHandleExA", GetModuleHandleExA}, {"GetModuleHandleExW", GetModuleHandleExW},
    {"GetModuleFileNameA", GetModuleFileNameA}, {"GetModuleFileNameW", GetModuleFileNameW},
    {"GetProcAddress", GetProcAddress}, {"LoadLibraryA", LoadLibraryA}, {"LoadLibraryW", LoadLibraryW},
    {"LoadLibraryExA", LoadLibraryExA}, {"LoadLibraryExW", LoadLibraryExW}, {"FreeLibrary", FreeLibrary},
    {"DisableThreadLibraryCalls", DisableThreadLibraryCalls},
    {"VirtualAlloc", VirtualAlloc}, {"VirtualFree", VirtualFree}, {"VirtualProtect", VirtualProtect},
    {"VirtualQuery", VirtualQuery}, {"FlushInstructionCache", FlushInstructionCache},
    {"WriteProcessMemory", WriteProcessMemory}, {"ReadProcessMemory", ReadProcessMemory},
    {"LocalFree", LocalFree}, {"LocalAlloc", LocalAlloc}, {"GetProcessHeap", GetProcessHeap}, {"HeapAlloc", HeapAlloc},
    {"HeapFree", HeapFree}, {"HeapReAlloc", HeapReAlloc}, {"HeapSize", HeapSize},
    {"GetNativeSystemInfo", GetNativeSystemInfo}, {"GetSystemInfo", GetNativeSystemInfo},
    {"GetProcessAffinityMask", GetProcessAffinityMask}, {"SetProcessAffinityMask", SetProcessAffinityMask},
    {"GetProcessTimes", GetProcessTimes}, {"GetThreadTimes", GetThreadTimes},
    {"CreateThread", CreateThread}, {"ExitThread", ExitThread}, {"GetExitCodeThread", GetExitCodeThread},
    {"ResumeThread", ResumeThread}, {"SuspendThread", SuspendThread}, {"GetThreadPriority", GetThreadPriority},
    {"SetThreadPriority", SetThreadPriority}, {"GetThreadContext", GetThreadContext},
    {"SetThreadContext", SetThreadContext}, {"Sleep", Sleep}, {"SleepEx", SleepEx}, {"SwitchToThread", SwitchToThread},
    {"GetCurrentProcessorNumber", GetCurrentProcessorNumber},
    {"TlsAlloc", TlsAlloc}, {"TlsFree", TlsFree}, {"TlsGetValue", TlsGetValue}, {"TlsSetValue", TlsSetValue},
    {"FlsAlloc", FlsAlloc}, {"FlsFree", TlsFree}, {"FlsGetValue", TlsGetValue}, {"FlsSetValue", TlsSetValue},
    {"WaitForSingleObject", WaitForSingleObject}, {"WaitForSingleObjectEx", WaitForSingleObjectEx},
    {"WaitForMultipleObjects", WaitForMultipleObjects}, {"CreateEventA", CreateEventA}, {"CreateEventW", CreateEventA},
    {"CreateEventExW", CreateEventExW}, {"SetEvent", SetEvent}, {"ResetEvent", ResetEvent},
    {"CreateSemaphoreA", CreateSemaphoreA}, {"CreateSemaphoreW", CreateSemaphoreA},
    {"CreateSemaphoreExW", CreateSemaphoreExW}, {"ReleaseSemaphore", ReleaseSemaphore}, {"CloseHandle", CloseHandle},
    {"DuplicateHandle", DuplicateHandle},
    {"InitializeCriticalSection", InitializeCriticalSection},
    {"InitializeCriticalSectionAndSpinCount", InitializeCriticalSectionAndSpinCount},
    {"InitializeCriticalSectionEx", InitializeCriticalSectionEx}, {"DeleteCriticalSection", DeleteCriticalSection},
    {"EnterCriticalSection", EnterCriticalSection}, {"LeaveCriticalSection", LeaveCriticalSection},
    {"TryEnterCriticalSection", TryEnterCriticalSection},
    {"InitializeSRWLock", InitializeSRWLock}, {"AcquireSRWLockExclusive", AcquireSRWLockExclusive},
    {"AcquireSRWLockShared", AcquireSRWLockShared}, {"ReleaseSRWLockExclusive", ReleaseSRWLock},
    {"ReleaseSRWLockShared", ReleaseSRWLock}, {"TryAcquireSRWLockExclusive", TryAcquireSRWLockExclusive},
    {"TryAcquireSRWLockShared", TryAcquireSRWLockShared},
    {"InitializeConditionVariable", InitializeConditionVariable}, {"WakeConditionVariable", WakeConditionVariable},
    {"WakeAllConditionVariable", WakeConditionVariable}, {"SleepConditionVariableSRW", SleepConditionVariableSRW},
    {"SleepConditionVariableCS", SleepConditionVariableCS}, {"InitOnceExecuteOnce", InitOnceExecuteOnce},
    {"CreateFileA", CreateFileA}, {"CreateFileW", CreateFileW}, {"ReadFile", ReadFile}, {"WriteFile", WriteFile},
    {"GetFileSize", GetFileSize}, {"GetFileSizeEx", GetFileSizeEx}, {"SetFilePointer", SetFilePointer},
    {"SetFilePointerEx", SetFilePointerEx}, {"GetFileType", GetFileType}, {"FlushFileBuffers", FlushFileBuffers},
    {"GetFileInformationByHandle", GetFileInformationByHandle}, {"GetFileAttributesA", GetFileAttributesA},
    {"GetFileAttributesW", GetFileAttributesW}, {"GetFileAttributesExW", GetFileAttributesExW},
    {"DeleteFileW", DeleteFileW}, {"RemoveDirectoryW", RemoveDirectoryW}, {"CreateDirectoryW", CreateDirectoryW},
    {"CreateDirectoryA", CreateDirectoryA}, {"MoveFileExW", MoveFileExW}, {"CopyFileW", CopyFileW},
    {"CreateHardLinkW", CreateHardLinkW}, {"GetDiskFreeSpaceExW", GetDiskFreeSpaceExW}, {"GetTempPathW", GetTempPathW},
    {"GetCurrentDirectoryA", GetCurrentDirectoryA}, {"GetCurrentDirectoryW", GetCurrentDirectoryW},
    {"GetFullPathNameA", GetFullPathNameA}, {"GetFullPathNameW", GetFullPathNameW},
    {"FindFirstFileA", FindFirstFileA}, {"FindFirstFileW", FindFirstFileW}, {"FindFirstFileExW", FindFirstFileExW},
    {"FindNextFileA", FindNextFileA}, {"FindNextFileW", FindNextFileW}, {"FindClose", FindClose},
    {"MultiByteToWideChar", MultiByteToWideChar}, {"WideCharToMultiByte", WideCharToMultiByte},
    {"GetLastError", GetLastError}, {"SetLastError", SetLastError}, {"FormatMessageA", FormatMessageA},
    {"FormatMessageW", FormatMessageW}, {"OutputDebugStringA", OutputDebugStringA},
    {"OutputDebugStringW", OutputDebugStringW}, {"lstrlenA", lstrlenA}, {"lstrlenW", lstrlenW},
    {"GetEnvironmentVariableA", GetEnvironmentVariableA}, {"GetEnvironmentVariableW", GetEnvironmentVariableW},
    {"GetCommandLineA", GetCommandLineA},
    {"GetSystemTimeAsFileTime", GetSystemTimeAsFileTime}, {"GetSystemTimePreciseAsFileTime", GetSystemTimeAsFileTime},
    {"QueryPerformanceCounter", QueryPerformanceCounter}, {"QueryPerformanceFrequency", QueryPerformanceFrequency},
    {"GetTickCount", GetTickCount}, {"GetTickCount64", GetTickCount64}, {"FileTimeToSystemTime", FileTimeToSystemTime},
    {"GetSystemTime", GetSystemTime}, {"GetLocalTime", GetLocalTime}, {"SetSystemTime", SetSystemTime},
    {"GetSystemTimeAdjustment", GetSystemTimeAdjustment}, {"InitializeSListHead", InitializeSListHead},
    {"RaiseException", RaiseException}, {"RtlUnwind", RtlUnwind},
    {"GetAsyncKeyState", GetAsyncKeyState}, {"GetKeyState", GetKeyState}, {"MessageBoxA", MessageBoxA},
    {"MessageBoxW", MessageBoxW}, {"wsprintfW", wsprintfW}, {"wsprintfA", wsprintfA},
    {"GetActiveWindow", GetActiveWindow}, {"FindWindowA", FindWindowA},
    {"D3DXMatrixOrthoRH", D3DXMatrixOrthoRH},
};
const size_t kKernel32Count = std::size(kKernel32);

}  // namespace mh
