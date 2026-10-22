// The Microsoft C runtime (UCRT) for mod code: stdio (with the printf family), strings and
// conversions, ctype and locale, math (x87 and SSE2 conventions), time, setjmp/longjmp, files
// and directories through the game's virtual folder, threads, sorting, the heap.
#include "modhost/sysapi.h"

#include "runtime/runtime.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <map>
#include <random>

namespace fs = std::filesystem;

namespace mh {

// ---- printf ----

std::string guestFormat(const std::string& f, u32 va, bool wide) {
    std::string out;
    auto next32 = [&] { const u32 v = M().r32(va); va += 4; return v; };
    auto next64 = [&] { const u64 v = M().r64(va); va += 8; return v; };
    char buf[512];
    for (size_t i = 0; i < f.size();) {
        const char ch = f[i++];
        if (ch != '%') {
            out += ch;
            continue;
        }
        if (i < f.size() && f[i] == '%') {
            out += '%', i++;
            continue;
        }
        std::string spec = "%";
        while (i < f.size() && std::strchr("-+ #0", f[i]) && f[i]) spec += f[i++];
        if (i < f.size() && f[i] == '*') {
            s32 w = s32(next32());
            if (w < 0) spec += '-', w = -w;
            spec += std::to_string(w), i++;
        } else
            while (i < f.size() && std::isdigit(u8(f[i]))) spec += f[i++];
        if (i < f.size() && f[i] == '.') {
            spec += f[i++];
            if (i < f.size() && f[i] == '*') {
                const s32 p = s32(next32());
                spec += std::to_string(p < 0 ? 0 : p), i++;
            } else
                while (i < f.size() && std::isdigit(u8(f[i]))) spec += f[i++];
        }
        // Length: 8 = 64-bit, 1 = char, 2 = short, 'w' = wide, 'h' = narrow (strings/chars)
        int len = 4;
        char strKind = 0;
        if (f.compare(i, 3, "I64") == 0) len = 8, i += 3;
        else if (f.compare(i, 3, "I32") == 0) i += 3;
        else if (f.compare(i, 2, "ll") == 0) len = 8, i += 2;
        else if (f.compare(i, 2, "hh") == 0) len = 1, i += 2;
        else if (i < f.size() && f[i] == 'h') len = 2, strKind = 'h', i++;
        else if (i < f.size() && (f[i] == 'l' || f[i] == 'w')) strKind = 'w', i++;
        else if (i < f.size() && (f[i] == 'j')) len = 8, i++;
        else if (i < f.size() && std::strchr("ztIL", f[i]) && f[i]) i++;
        if (i >= f.size()) break;
        const char conv = f[i++];
        switch (conv) {
        case 'd': case 'i': {
            s64 v = len == 8 ? s64(next64()) : s64(s32(next32()));
            if (len == 2) v = s16(v);
            if (len == 1) v = s8(v);
            std::snprintf(buf, sizeof buf, (spec + "lld").c_str(), (long long)v);
            out += buf;
            break;
        }
        case 'u': case 'o': case 'x': case 'X': {
            u64 v = len == 8 ? next64() : u64(next32());
            if (len == 2) v = u16(v);
            if (len == 1) v = u8(v);
            std::snprintf(buf, sizeof buf, (spec + "ll" + conv).c_str(), (unsigned long long)v);
            out += buf;
            break;
        }
        case 'c': case 'C': {
            const u32 v = next32();
            const bool w = conv == 'C' ? !wide || strKind == 'w' : (wide ? strKind != 'h' : strKind == 'w');
            const std::string s = w ? utf8(std::u16string(1, char16_t(v & 0xFFFF))) : std::string(1, char(v));
            std::snprintf(buf, sizeof buf, (spec + "s").c_str(), s.c_str());
            out += buf;
            break;
        }
        case 's': case 'S': case 'Z': {
            const u32 p = next32();
            const bool w = conv == 'S' ? (wide ? strKind == 'w' : strKind != 'h') : (wide ? strKind != 'h' : strKind == 'w');
            std::string s = !p ? "(null)" : w ? wideArg(p) : M().cstr(p, 1 << 20);
            std::vector<char> big(s.size() + 512);
            std::snprintf(big.data(), big.size(), (spec + "s").c_str(), s.c_str());
            out += big.data();
            break;
        }
        case 'f': case 'F': case 'e': case 'E': case 'g': case 'G': case 'a': case 'A': {
            const u64 b = next64();
            double d;
            std::memcpy(&d, &b, 8);
            std::snprintf(buf, sizeof buf, (spec + conv).c_str(), d);
            out += buf;
            break;
        }
        case 'p': std::snprintf(buf, sizeof buf, "%08X", next32()); out += buf; break;
        case 'n': M().w32(next32(), u32(out.size())); break;
        default: out += '%', out += conv; break;
        }
    }
    return out;
}

namespace {

// ---- FILE ----
struct CFile {
    std::FILE* f = nullptr;
    int console = 0;  // 1 stdout, 2 stderr, 3 stdin
    std::string line; // console output not yet logged
    std::string path;
};
std::map<u32, CFile> s_files;     // guest FILE* -> stream
std::map<int, u32> s_fdFiles;     // fd -> guest FILE*
int s_nextFd = 3;
u32 s_iob[3];

CFile* cfile(u32 h) {
    const auto it = s_files.find(h);
    return it == s_files.end() ? nullptr : &it->second;
}
u32 newFile(std::FILE* f, const std::string& path) {
    const u32 h = host().alloc(32);
    s_files[h] = CFile{f, 0, {}, path};
    return h;
}
int fdOf(u32 h) {
    for (const auto& [fd, fh] : s_fdFiles)
        if (fh == h) return fd;
    if (h == s_iob[0]) return 0;
    if (h == s_iob[1]) return 1;
    if (h == s_iob[2]) return 2;
    const int fd = s_nextFd++;
    s_fdFiles[fd] = h;
    return fd;
}
u32 fileOfFd(int fd) {
    if (fd >= 0 && fd < 3) return s_iob[fd];
    const auto it = s_fdFiles.find(fd);
    return it == s_fdFiles.end() ? 0 : it->second;
}
void consoleWrite(CFile& f, const std::string& s) {
    f.line += s;
    for (size_t nl; (nl = f.line.find('\n')) != std::string::npos;) {
        rt::log("[%s] %s\n", host().currentMod.c_str(), f.line.substr(0, nl).c_str());
        f.line.erase(0, nl + 1);
    }
}
size_t fileWrite(u32 h, const void* data, size_t n) {
    CFile* f = cfile(h);
    if (!f) return 0;
    if (f->console) {
        consoleWrite(*f, std::string(static_cast<const char*>(data), n));
        return n;
    }
    return f->f ? std::fwrite(data, 1, n, f->f) : 0;
}
std::string openMode(const std::string& m) {  // drop the MS-only letters (ccs=, t, N, S...)
    std::string r;
    for (char ch : m) {
        if (ch == ',') break;
        if (std::strchr("rwa+b", ch) && ch) r += ch;
    }
    if (r.find('b') == std::string::npos) r += 'b';
    return r;
}
u32 openFile(const std::string& name, const std::string& mode) {
    const std::string p = hostPath(name);
    std::FILE* f = std::fopen(p.c_str(), openMode(mode).c_str());
    if (!f) {
        setErrno(2);  // ENOENT
        return 0;
    }
    return newFile(f, p);
}

void fopen_(Cpu& c) { retc(c, openFile(strArg(c.arg(0)), strArg(c.arg(1)))); }
void _wfopen(Cpu& c) { retc(c, openFile(wideArg(c.arg(0)), wideArg(c.arg(1)))); }
void _fsopen(Cpu& c) { retc(c, openFile(strArg(c.arg(0)), strArg(c.arg(1)))); }
void fopen_s(Cpu& c) {
    const u32 h = openFile(strArg(c.arg(1)), strArg(c.arg(2)));
    M().w32(c.arg(0), h);
    retc(c, h ? 0 : 2);
}
void _wfopen_s(Cpu& c) {
    const u32 h = openFile(wideArg(c.arg(1)), wideArg(c.arg(2)));
    M().w32(c.arg(0), h);
    retc(c, h ? 0 : 2);
}
void fclose_(Cpu& c) {
    const u32 h = c.arg(0);
    CFile* f = cfile(h);
    if (!f || f->console) return retc(c, f ? 0 : u32(-1));
    if (f->f) std::fclose(f->f);
    s_files.erase(h);
    for (auto it = s_fdFiles.begin(); it != s_fdFiles.end();)
        it = it->second == h ? s_fdFiles.erase(it) : std::next(it);
    host().free(h);
    retc(c, 0);
}
void fflush_(Cpu& c) {
    if (CFile* f = cfile(c.arg(0)); f && f->f) std::fflush(f->f);
    retc(c, 0);
}
void fread_(Cpu& c) {
    const u32 buf = c.arg(0), size = c.arg(1), count = c.arg(2);
    CFile* f = cfile(c.arg(3));
    if (!f || !f->f || !size) return retc(c, 0);
    std::vector<u8> tmp(size_t(size) * count);
    const size_t got = std::fread(tmp.data(), size, count, f->f);
    M().write(buf, tmp.data(), u32(got * size));
    retc(c, u32(got));
}
void fwrite_(Cpu& c) {
    const u32 buf = c.arg(0), size = c.arg(1), count = c.arg(2);
    if (!size) return retc(c, 0);
    std::vector<u8> tmp(size_t(size) * count);
    M().read(buf, tmp.data(), u32(tmp.size()));
    retc(c, u32(fileWrite(c.arg(3), tmp.data(), tmp.size()) / size));
}
void fgets_(Cpu& c) {
    const u32 buf = c.arg(0), n = c.arg(1);
    CFile* f = cfile(c.arg(2));
    std::string line(n + 1, '\0');
    if (!f || !f->f || !n || !std::fgets(line.data(), int(n), f->f)) return retc(c, 0);
    M().write(buf, line.c_str(), u32(std::strlen(line.c_str()) + 1));
    retc(c, buf);
}
void fputs_(Cpu& c) {
    const std::string s = strArg(c.arg(0));
    fileWrite(c.arg(1), s.data(), s.size());
    retc(c, 0);
}
void fputc_(Cpu& c) {
    const char ch = char(c.arg(0));
    retc(c, fileWrite(c.arg(1), &ch, 1) ? u8(ch) : u32(-1));
}
void fgetc_(Cpu& c) {
    CFile* f = cfile(c.arg(0));
    retc(c, f && f->f ? u32(std::fgetc(f->f)) : u32(-1));
}
void ungetc_(Cpu& c) {
    CFile* f = cfile(c.arg(1));
    retc(c, f && f->f ? u32(std::ungetc(int(c.arg(0)), f->f)) : u32(-1));
}
void getwc_(Cpu& c) {  // UTF-16LE units
    CFile* f = cfile(c.arg(0));
    if (!f || !f->f) return retc(c, 0xFFFF);
    const int a = std::fgetc(f->f), b = std::fgetc(f->f);
    retc(c, a < 0 || b < 0 ? 0xFFFF : u32(a | b << 8));
}
void putwc_(Cpu& c) {
    const u8 b[2] = {u8(c.arg(0)), u8(c.arg(0) >> 8)};
    retc(c, fileWrite(c.arg(1), b, 2) == 2 ? (c.arg(0) & 0xFFFF) : 0xFFFF);
}
void ungetwc_(Cpu& c) {
    CFile* f = cfile(c.arg(1));
    if (f && f->f) std::fseek(f->f, -2, SEEK_CUR);
    retc(c, c.arg(0) & 0xFFFF);
}
void feof_(Cpu& c) {
    CFile* f = cfile(c.arg(0));
    retc(c, f && f->f ? (std::feof(f->f) ? 1 : 0) : 1);
}
void ferror_(Cpu& c) {
    CFile* f = cfile(c.arg(0));
    retc(c, f && f->f ? (std::ferror(f->f) ? 1 : 0) : 1);
}
void fseek_(Cpu& c) {
    CFile* f = cfile(c.arg(0));
    retc(c, f && f->f ? u32(std::fseek(f->f, long(s32(c.arg(1))), int(c.arg(2)))) : u32(-1));
}
void ftell_(Cpu& c) {
    CFile* f = cfile(c.arg(0));
    retc(c, f && f->f ? u32(std::ftell(f->f)) : u32(-1));
}
void _fseeki64(Cpu& c) {
    CFile* f = cfile(c.arg(0));
    retc(c, f && f->f ? u32(std::fseek(f->f, long(s64(arg64(c, 1))), int(c.arg(3)))) : u32(-1));
}
void _ftelli64(Cpu& c) {
    CFile* f = cfile(c.arg(0));
    retc64(c, f && f->f ? u64(s64(std::ftell(f->f))) : ~0ull);
}
void setvbuf_(Cpu& c) { retc(c, 0); }
void nop0(Cpu& c) { retc(c, 0); }
void ensureIob() {
    if (!s_iob[0])
        for (int k = 0; k < 3; k++) {
            s_iob[k] = host().alloc(32);
            s_files[s_iob[k]] = CFile{nullptr, k == 0 ? 3 : k, {}, {}};
        }
}
void __acrt_iob_func(Cpu& c) {
    ensureIob();
    retc(c, c.arg(0) < 3 ? s_iob[c.arg(0)] : 0);
}
void _fileno(Cpu& c) { retc(c, u32(fdOf(c.arg(0)))); }

// fds: <io.h> on top of the same streams
int openFd(const std::string& name, u32 flags) {
    const bool rw = flags & 2, wr = flags & 1, append = flags & 8, create = flags & 0x100, trunc = flags & 0x200;
    const std::string p = hostPath(name);
    std::error_code ec;
    const bool exists = fs::exists(p, ec);
    if (!exists && !create) return setErrno(2), -1;
    if (exists && create && (flags & 0x400)) return setErrno(17), -1;  // EEXIST
    std::string mode = trunc || (!exists && create) ? (rw ? "w+b" : wr ? "wb" : "w+b")
                                                    : append ? (rw ? "a+b" : "ab")
                                                    : rw || wr ? "r+b" : "rb";
    std::FILE* f = std::fopen(p.c_str(), mode.c_str());
    if (!f) return setErrno(2), -1;
    const int fd = s_nextFd++;
    s_fdFiles[fd] = newFile(f, p);
    return fd;
}
void _open(Cpu& c) { retc(c, u32(openFd(strArg(c.arg(0)), c.arg(1)))); }
void _wopen(Cpu& c) { retc(c, u32(openFd(wideArg(c.arg(0)), c.arg(1)))); }
void _close(Cpu& c) {
    const u32 h = fileOfFd(int(c.arg(0)));
    CFile* f = cfile(h);
    if (!f) return retc(c, u32(-1));
    if (!f->console) {
        if (f->f) std::fclose(f->f);
        s_files.erase(h);
        host().free(h);
    }
    s_fdFiles.erase(int(c.arg(0)));
    retc(c, 0);
}
void _read(Cpu& c) {
    CFile* f = cfile(fileOfFd(int(c.arg(0))));
    if (!f || !f->f) return retc(c, u32(-1));
    std::vector<u8> tmp(c.arg(2));
    const size_t got = std::fread(tmp.data(), 1, tmp.size(), f->f);
    M().write(c.arg(1), tmp.data(), u32(got));
    retc(c, u32(got));
}
void _write(Cpu& c) {
    std::vector<u8> tmp(c.arg(2));
    M().read(c.arg(1), tmp.data(), u32(tmp.size()));
    retc(c, u32(fileWrite(fileOfFd(int(c.arg(0))), tmp.data(), tmp.size())));
}
void _lseeki64(Cpu& c) {
    CFile* f = cfile(fileOfFd(int(c.arg(0))));
    if (!f || !f->f) return retc64(c, ~0ull);
    std::fseek(f->f, long(s64(arg64(c, 1))), int(c.arg(3)));
    retc64(c, u64(std::ftell(f->f)));
}
void _chsize_s(Cpu& c) {
    CFile* f = cfile(fileOfFd(int(c.arg(0))));
    if (!f || !f->f) return retc(c, 9);
    std::fflush(f->f);
    std::error_code ec;
    fs::resize_file(f->path, arg64(c, 1), ec);
    retc(c, ec ? 22 : 0);
}
void _fdopen(Cpu& c) { retc(c, fileOfFd(int(c.arg(0)))); }
void _setmode(Cpu& c) { retc(c, 0x8000); }
void _get_osfhandle(Cpu& c) {
    setErrno(9);
    retc(c, 0xFFFFFFFF);
}

// printf family (UCRT's: options (64-bit), destination, ..., format, locale, va_list)
u32 writeOut(u32 buf, u32 cap, const std::string& s, bool wide, bool truncateOk, bool* fit) {
    if (wide) {
        const std::u16string w = utf16(s);
        *fit = w.size() < cap;
        if (*fit || truncateOk) putWide(buf, w, cap);
        return u32(w.size());
    }
    *fit = s.size() < cap;
    if (*fit || truncateOk) putStr(buf, s, cap);
    return u32(s.size());
}
void vsprintf_(Cpu& c, bool wide) {  // (opts, opts, buf, count, fmt, locale, va)
    const u64 opts = arg64(c, 0);
    const u32 buf = c.arg(2), cap = c.arg(3);
    const std::string s = guestFormat(wide ? wideArg(c.arg(4)) : strArg(c.arg(4)), c.arg(6), wide);
    if (!buf) return retc(c, u32(wide ? utf16(s).size() : s.size()));
    bool fit;
    const u32 n = writeOut(buf, cap, s, wide, true, &fit);
    if (!fit && !(opts & 2)) return retc(c, u32(-1));  // legacy _vsnprintf: -1 when it does not fit
    retc(c, n);
}
void __stdio_common_vsprintf(Cpu& c) { vsprintf_(c, false); }
void __stdio_common_vswprintf(Cpu& c) { vsprintf_(c, true); }
void vsprintf_s_(Cpu& c, bool wide) {  // (opts, opts, buf, count, fmt, locale, va)
    const u32 buf = c.arg(2), cap = c.arg(3);
    const std::string s = guestFormat(wide ? wideArg(c.arg(4)) : strArg(c.arg(4)), c.arg(6), wide);
    bool fit;
    const u32 n = writeOut(buf, cap, s, wide, false, &fit);
    if (!fit) {
        if (cap) wide ? M().w16(buf, 0) : M().w8(buf, 0);
        setErrno(34);
        return retc(c, u32(-1));
    }
    retc(c, n);
}
void __stdio_common_vsprintf_s(Cpu& c) { vsprintf_s_(c, false); }
void __stdio_common_vswprintf_s(Cpu& c) { vsprintf_s_(c, true); }
void vsnprintf_s_(Cpu& c, bool wide) {  // (opts, opts, buf, count, max, fmt, locale, va)
    const u32 buf = c.arg(2), cap = c.arg(3), max = c.arg(4);
    const std::string s = guestFormat(wide ? wideArg(c.arg(5)) : strArg(c.arg(5)), c.arg(7), wide);
    const u32 limit = max == 0xFFFFFFFF ? cap : std::min(cap, max + 1);
    bool fit;
    const u32 n = writeOut(buf, limit, s, wide, true, &fit);
    if (!fit) return retc(c, u32(-1));
    retc(c, n);
}
void __stdio_common_vsnprintf_s(Cpu& c) { vsnprintf_s_(c, false); }
void __stdio_common_vsnwprintf_s(Cpu& c) { vsnprintf_s_(c, true); }
void __stdio_common_vfprintf(Cpu& c) {  // (opts, opts, FILE*, fmt, locale, va)
    const std::string s = guestFormat(strArg(c.arg(3)), c.arg(5), false);
    fileWrite(c.arg(2), s.data(), s.size());
    retc(c, u32(s.size()));
}
void __stdio_common_vfwprintf(Cpu& c) {
    const std::string s = guestFormat(wideArg(c.arg(3)), c.arg(5), true);
    fileWrite(c.arg(2), s.data(), s.size());
    retc(c, u32(s.size()));
}
void printf_(Cpu& c) {  // old-style cdecl varargs printf
    const std::string s = guestFormat(strArg(c.arg(0)), c.r[Cpu::ESP] + 8, false);
    ensureIob();
    fileWrite(s_iob[1], s.data(), s.size());
    retc(c, u32(s.size()));
}
void sprintf_(Cpu& c) { retc(c, putStr(c.arg(0), guestFormat(strArg(c.arg(1)), c.r[Cpu::ESP] + 12, false))); }

// ---- strings ----
std::string cs(u32 a) { return M().cstr(a, 1 << 20); }
std::u16string ws(u32 a) { return M().wstr(a, 1 << 20); }
void strlen_(Cpu& c) { retc(c, u32(cs(c.arg(0)).size())); }
void strnlen_(Cpu& c) {
    u32 n = 0;
    while (n < c.arg(1) && M().r8(c.arg(0) + n)) n++;
    retc(c, n);
}
int cmpN(u32 a, u32 b, u32 n, bool fold) {
    for (u32 i = 0; i < n; i++) {
        int x = M().r8(a + i), y = M().r8(b + i);
        if (fold) x = std::tolower(x), y = std::tolower(y);
        if (x != y) return x < y ? -1 : 1;
        if (!x) return 0;
    }
    return 0;
}
void strcmp_(Cpu& c) { retc(c, u32(cmpN(c.arg(0), c.arg(1), 0xFFFFFFFF, false))); }
void strncmp_(Cpu& c) { retc(c, u32(cmpN(c.arg(0), c.arg(1), c.arg(2), false))); }
void _stricmp(Cpu& c) { retc(c, u32(cmpN(c.arg(0), c.arg(1), 0xFFFFFFFF, true))); }
void _strnicmp(Cpu& c) { retc(c, u32(cmpN(c.arg(0), c.arg(1), c.arg(2), true))); }
void strcpy_(Cpu& c) {
    putStr(c.arg(0), cs(c.arg(1)));
    retc(c, c.arg(0));
}
void strcat_(Cpu& c) {
    putStr(c.arg(0), cs(c.arg(0)) + cs(c.arg(1)));
    retc(c, c.arg(0));
}
void strncpy_(Cpu& c) {
    const u32 d = c.arg(0), s = c.arg(1), n = c.arg(2);
    bool end = false;
    for (u32 i = 0; i < n; i++) {
        const u8 ch = end ? 0 : M().r8(s + i);
        if (!ch) end = true;
        M().w8(d + i, ch);
    }
    retc(c, d);
}
void strncat_(Cpu& c) {
    std::string s = cs(c.arg(1));
    if (s.size() > c.arg(2)) s.resize(c.arg(2));
    putStr(c.arg(0), cs(c.arg(0)) + s);
    retc(c, c.arg(0));
}
void strcpy_s(Cpu& c) {
    const std::string s = cs(c.arg(2));
    if (s.size() >= c.arg(1)) return (c.arg(1) ? M().w8(c.arg(0), 0) : void()), retc(c, 34);
    putStr(c.arg(0), s);
    retc(c, 0);
}
void strcat_s(Cpu& c) {
    const std::string s = cs(c.arg(0)) + cs(c.arg(2));
    if (s.size() >= c.arg(1)) return retc(c, 34);
    putStr(c.arg(0), s);
    retc(c, 0);
}
void strncpy_s(Cpu& c) {  // (dst, dstSize, src, count)
    std::string s = cs(c.arg(2));
    const u32 count = c.arg(3);
    if (count != 0xFFFFFFFF && s.size() > count) s.resize(count);
    if (s.size() >= c.arg(1)) {
        if (count == 0xFFFFFFFF) return putStr(c.arg(0), s, c.arg(1)), retc(c, 80);  // STRUNCATE
        if (c.arg(1)) M().w8(c.arg(0), 0);
        return retc(c, 34);
    }
    putStr(c.arg(0), s);
    retc(c, 0);
}
void strchr_(Cpu& c) {
    const u32 s = c.arg(0);
    const u8 ch = u8(c.arg(1));
    for (u32 i = 0;; i++) {
        const u8 v = M().r8(s + i);
        if (v == ch) return retc(c, s + i);
        if (!v) return retc(c, 0);
    }
}
void strrchr_(Cpu& c) {
    const u32 s = c.arg(0);
    const u8 ch = u8(c.arg(1));
    u32 last = 0;
    for (u32 i = 0;; i++) {
        const u8 v = M().r8(s + i);
        if (v == ch) last = s + i;
        if (!v) return retc(c, last);
    }
}
void strstr_(Cpu& c) {
    const std::string h = cs(c.arg(0)), n = cs(c.arg(1));
    const size_t at = h.find(n);
    retc(c, at == std::string::npos ? 0 : c.arg(0) + u32(at));
}
void strspn_(Cpu& c) {
    const std::string s = cs(c.arg(0));
    const size_t at = s.find_first_not_of(cs(c.arg(1)));
    retc(c, u32(at == std::string::npos ? s.size() : at));
}
void strcspn_(Cpu& c) {
    const std::string s = cs(c.arg(0));
    const size_t at = s.find_first_of(cs(c.arg(1)));
    retc(c, u32(at == std::string::npos ? s.size() : at));
}
void strpbrk_(Cpu& c) {
    const size_t at = cs(c.arg(0)).find_first_of(cs(c.arg(1)));
    retc(c, at == std::string::npos ? 0 : c.arg(0) + u32(at));
}
void _strdup(Cpu& c) { retc(c, c.arg(0) ? host().allocString(cs(c.arg(0))) : 0); }
void strxfrm_(Cpu& c) {
    const std::string s = cs(c.arg(1));
    if (c.arg(2) > s.size()) putStr(c.arg(0), s);
    retc(c, u32(s.size()));
}
void memchr_(Cpu& c) {
    for (u32 i = 0; i < c.arg(2); i++)
        if (M().r8(c.arg(0) + i) == u8(c.arg(1))) return retc(c, c.arg(0) + i);
    retc(c, 0);
}
void memcpy_(Cpu& c) {
    const u32 d = c.arg(0), s = c.arg(1), n = c.arg(2);
    if (d < s || d >= s + n)
        for (u32 i = 0; i < n; i++) M().w8(d + i, M().r8(s + i));
    else
        for (u32 i = n; i-- > 0;) M().w8(d + i, M().r8(s + i));
    retc(c, d);
}
void memset_(Cpu& c) {
    const u32 d = c.arg(0), n = c.arg(2);
    const u8 v = u8(c.arg(1));
    for (u32 i = 0; i < n; i++) M().w8(d + i, v);
    retc(c, d);
}
void memcmp_(Cpu& c) {
    for (u32 i = 0; i < c.arg(2); i++) {
        const u8 x = M().r8(c.arg(0) + i), y = M().r8(c.arg(1) + i);
        if (x != y) return retc(c, x < y ? u32(-1) : 1);
    }
    retc(c, 0);
}
// wide
void wcslen_(Cpu& c) { retc(c, u32(ws(c.arg(0)).size())); }
void wcsnlen_(Cpu& c) {
    u32 n = 0;
    while (n < c.arg(1) && M().r16(c.arg(0) + 2 * n)) n++;
    retc(c, n);
}
int wcmp(u32 a, u32 b, u32 n, bool fold) {
    for (u32 i = 0; i < n; i++) {
        u32 x = M().r16(a + 2 * i), y = M().r16(b + 2 * i);
        if (fold && x < 0x80) x = u32(std::tolower(int(x)));
        if (fold && y < 0x80) y = u32(std::tolower(int(y)));
        if (x != y) return x < y ? -1 : 1;
        if (!x) return 0;
    }
    return 0;
}
void wcscmp_(Cpu& c) { retc(c, u32(wcmp(c.arg(0), c.arg(1), 0xFFFFFFFF, false))); }
void wcsncmp_(Cpu& c) { retc(c, u32(wcmp(c.arg(0), c.arg(1), c.arg(2), false))); }
void _wcsicmp(Cpu& c) { retc(c, u32(wcmp(c.arg(0), c.arg(1), 0xFFFFFFFF, true))); }
void wcscpy_(Cpu& c) {
    putWide(c.arg(0), ws(c.arg(1)));
    retc(c, c.arg(0));
}
void wcscat_(Cpu& c) {
    putWide(c.arg(0), ws(c.arg(0)) + ws(c.arg(1)));
    retc(c, c.arg(0));
}
void wcscpy_s(Cpu& c) {
    const std::u16string s = ws(c.arg(2));
    if (s.size() >= c.arg(1)) return retc(c, 34);
    putWide(c.arg(0), s);
    retc(c, 0);
}
void wcscat_s(Cpu& c) {
    const std::u16string s = ws(c.arg(0)) + ws(c.arg(2));
    if (s.size() >= c.arg(1)) return retc(c, 34);
    putWide(c.arg(0), s);
    retc(c, 0);
}
void wcsxfrm_(Cpu& c) {
    const std::u16string s = ws(c.arg(1));
    if (c.arg(2) > s.size()) putWide(c.arg(0), s);
    retc(c, u32(s.size()));
}
void wcschr_(Cpu& c) {
    for (u32 i = 0;; i++) {
        const u16 v = M().r16(c.arg(0) + 2 * i);
        if (v == u16(c.arg(1))) return retc(c, c.arg(0) + 2 * i);
        if (!v) return retc(c, 0);
    }
}
void wcsrchr_(Cpu& c) {
    u32 last = 0;
    for (u32 i = 0;; i++) {
        const u16 v = M().r16(c.arg(0) + 2 * i);
        if (v == u16(c.arg(1))) last = c.arg(0) + 2 * i;
        if (!v) return retc(c, last);
    }
}
void _wcsdup(Cpu& c) {
    const std::u16string s = ws(c.arg(0));
    const u32 p = host().alloc(2 * u32(s.size() + 1));
    putWide(p, s);
    retc(c, p);
}

// ---- ctype ----
void isdigit_(Cpu& c) { retc(c, std::isdigit(int(c.arg(0) & 0xFF)) ? 4 : 0); }
void isspace_(Cpu& c) { retc(c, std::isspace(int(c.arg(0) & 0xFF)) ? 8 : 0); }
void isxdigit_(Cpu& c) { retc(c, std::isxdigit(int(c.arg(0) & 0xFF)) ? 0x80 : 0); }
void isalpha_(Cpu& c) { retc(c, std::isalpha(int(c.arg(0) & 0xFF)) ? 0x103 : 0); }
void isalnum_(Cpu& c) { retc(c, std::isalnum(int(c.arg(0) & 0xFF)) ? 0x107 : 0); }
void isupper_(Cpu& c) { retc(c, std::isupper(int(c.arg(0) & 0xFF)) ? 1 : 0); }
void islower_(Cpu& c) { retc(c, std::islower(int(c.arg(0) & 0xFF)) ? 2 : 0); }
void isprint_(Cpu& c) { retc(c, std::isprint(int(c.arg(0) & 0xFF)) ? 0x157 : 0); }
void ispunct_(Cpu& c) { retc(c, std::ispunct(int(c.arg(0) & 0xFF)) ? 0x10 : 0); }
void tolower_(Cpu& c) { retc(c, u32(std::tolower(int(c.arg(0) & 0xFF))) | (c.arg(0) & ~0xFFu)); }
void toupper_(Cpu& c) { retc(c, u32(std::toupper(int(c.arg(0) & 0xFF))) | (c.arg(0) & ~0xFFu)); }
void towlower_(Cpu& c) { retc(c, c.arg(0) < 0x80 ? u32(std::tolower(int(c.arg(0)))) : (c.arg(0) & 0xFFFF)); }
void towupper_(Cpu& c) { retc(c, c.arg(0) < 0x80 ? u32(std::toupper(int(c.arg(0)))) : (c.arg(0) & 0xFFFF)); }
u32 ctypeMask(u32 ch) {
    if (ch >= 0x80) return 0x100 | 0x40;  // letters beyond ASCII: alpha, printable
    u32 m = 0;
    if (std::isupper(int(ch))) m |= 1;
    if (std::islower(int(ch))) m |= 2;
    if (std::isdigit(int(ch))) m |= 4;
    if (std::isspace(int(ch))) m |= 8;
    if (std::ispunct(int(ch))) m |= 0x10;
    if (std::iscntrl(int(ch))) m |= 0x20;
    if (ch == ' ') m |= 0x40;
    if (std::isxdigit(int(ch))) m |= 0x80;
    if (std::isalpha(int(ch))) m |= 0x100;
    return m;
}
void iswctype_(Cpu& c) { retc(c, ctypeMask(c.arg(0) & 0xFFFF) & c.arg(1)); }
void wctype_(Cpu& c) {
    static const std::map<std::string, u32> k = {{"alnum", 0x107}, {"alpha", 0x103}, {"cntrl", 0x20}, {"digit", 4},
                                                  {"graph", 0x117}, {"lower", 2}, {"print", 0x157}, {"punct", 0x10},
                                                  {"space", 8}, {"upper", 1}, {"xdigit", 0x80}, {"blank", 0x40}};
    const auto it = k.find(cs(c.arg(0)));
    retc(c, it == k.end() ? 0 : it->second);
}
void __pctype_func(Cpu& c) {
    static u32 table = 0;
    if (!table) {
        table = host().alloc(2 * 257);
        M().w16(table, 0);
        for (u32 ch = 0; ch < 256; ch++) M().w16(table + 2 + 2 * ch, u16(ch < 0x80 ? ctypeMask(ch) : 0));
    }
    retc(c, table + 2);
}

// ---- conversions ----
template <typename F>
void strtoX(Cpu& c, F f, bool is64) {  // (str, end*, base)
    const u32 s = c.arg(0);
    const std::string str = cs(s);
    char* end = nullptr;
    const auto v = f(str.c_str(), &end, int(c.arg(2)));
    if (c.arg(1)) M().w32(c.arg(1), s + u32(end - str.c_str()));
    if (is64) retc64(c, u64(v));
    else retc(c, u32(v));
}
void strtol_(Cpu& c) {
    strtoX(c, [](const char* s, char** e, int b) {
        const long long v = std::strtoll(s, e, b);
        return v > 0x7FFFFFFF ? 0x7FFFFFFFLL : v < -0x80000000LL ? -0x80000000LL : v;
    }, false);
}
void strtoul_(Cpu& c) { strtoX(c, [](const char* s, char** e, int b) { return std::strtoull(s, e, b); }, false); }
void strtoll_(Cpu& c) { strtoX(c, [](const char* s, char** e, int b) { return std::strtoll(s, e, b); }, true); }
void strtoull_(Cpu& c) { strtoX(c, [](const char* s, char** e, int b) { return std::strtoull(s, e, b); }, true); }
void atoi_(Cpu& c) { retc(c, u32(std::atoi(cs(c.arg(0)).c_str()))); }
void _wtoi(Cpu& c) { retc(c, u32(std::atoi(utf8(ws(c.arg(0))).c_str()))); }
void _atoi64(Cpu& c) { retc64(c, u64(std::atoll(cs(c.arg(0)).c_str()))); }
void atof_(Cpu& c) { retFloat(c, std::atof(cs(c.arg(0)).c_str())); }
void strtod_(Cpu& c) {
    const std::string str = cs(c.arg(0));
    char* end;
    const double v = std::strtod(str.c_str(), &end);
    if (c.arg(1)) M().w32(c.arg(1), c.arg(0) + u32(end - str.c_str()));
    retFloat(c, v);
}
void strtof_(Cpu& c) { strtod_(c); }
void wcstol_(Cpu& c) {
    const std::string str = utf8(ws(c.arg(0)));
    char* end;
    const long v = std::strtol(str.c_str(), &end, int(c.arg(2)));
    if (c.arg(1)) M().w32(c.arg(1), c.arg(0) + 2 * u32(end - str.c_str()));  // ASCII digits: one unit each
    retc(c, u32(v));
}
void wcstod_(Cpu& c) {
    const std::string str = utf8(ws(c.arg(0)));
    char* end;
    const double v = std::strtod(str.c_str(), &end);
    if (c.arg(1)) M().w32(c.arg(1), c.arg(0) + 2 * u32(end - str.c_str()));
    retFloat(c, v);
}
// Multibyte: the "C" locale, bytes as Latin-1 code points (UTF-8 aware for > 0x7F).
void mbrtowc_(Cpu& c) {  // (wc*, s, n, state)
    if (!c.arg(1)) return retc(c, 0);
    const u8 b = M().r8(c.arg(1));
    if (c.arg(0)) M().w16(c.arg(0), b);
    retc(c, b ? 1 : 0);
}
void mbrlen_(Cpu& c) { retc(c, c.arg(0) && M().r8(c.arg(0)) ? 1 : 0); }
void wcrtomb_(Cpu& c) {  // (s, wc, state)
    if (c.arg(0)) M().w8(c.arg(0), u8(c.arg(1) < 0x100 ? c.arg(1) : '?'));
    retc(c, 1);
}
void mbsrtowcs_(Cpu& c) {  // (dst, src**, len, state)
    const u32 srcp = c.arg(1), src = M().r32(srcp), dst = c.arg(0), len = c.arg(2);
    const std::string s = cs(src);
    u32 n = 0;
    for (; n < s.size() && (!dst || n < len); n++)
        if (dst) M().w16(dst + 2 * n, u8(s[n]));
    if (dst) {
        if (n < len) M().w16(dst + 2 * n, 0), M().w32(srcp, 0);
        else M().w32(srcp, src + n);
    }
    retc(c, n);
}
void wcstombs_(Cpu& c) {
    const std::u16string w = ws(c.arg(1));
    std::string s;
    for (char16_t ch : w) s += ch < 0x100 ? char(ch) : '?';
    if (c.arg(0)) putStr(c.arg(0), s, c.arg(2) + 1);
    retc(c, u32(s.size()));
}
void mbstowcs_(Cpu& c) {
    const std::string s = cs(c.arg(1));
    if (c.arg(0)) {
        const u32 n = std::min<u32>(u32(s.size()), c.arg(2));
        for (u32 i = 0; i < n; i++) M().w16(c.arg(0) + 2 * i, u8(s[i]));
        if (n < c.arg(2)) M().w16(c.arg(0) + 2 * n, 0);
    }
    retc(c, u32(s.size()));
}

// ---- locale ----
void setlocale_(Cpu& c) {
    static u32 s = 0;
    if (!s) s = host().allocString("C");
    retc(c, s);
}
void localeconv_(Cpu& c) {
    static u32 lc = 0;
    if (!lc) {
        lc = host().alloc(80);
        const u32 dot = host().allocString("."), empty = host().allocString("");
        for (u32 i = 0; i < 80; i += 4) M().w32(lc + i, empty);
        M().w32(lc, dot);
        for (u32 i = 0x24; i < 0x30; i++) M().w8(lc + i, 0x7F);  // the char fields: CHAR_MAX
    }
    retc(c, lc);
}
void one(Cpu& c) { retc(c, 1); }

// ---- math: x87 (arguments on the stack, the result in ST0) and the SSE2 library forms ----
template <double (*F)(double)>
void math1(Cpu& c) { retFloat(c, F(argDouble(c, 0))); }
template <double (*F)(double, double)>
void math2(Cpu& c) { retFloat(c, F(argDouble(c, 0), argDouble(c, 2))); }
template <double (*F)(double)>
void math1f(Cpu& c) { retFloat(c, double(float(F(double(argFloat(c, 0)))))); }
template <double (*F)(double, double)>
void math2f(Cpu& c) { retFloat(c, double(float(F(double(argFloat(c, 0)), double(argFloat(c, 1)))))); }
template <double (*F)(double)>
void ci1(Cpu& c) {  // _CIsin...: the argument in ST0
    c.ST(0) = F(c.ST(0));
    c.ret();
}
template <double (*F)(double, double)>
void ci2(Cpu& c) {  // _CIpow...: ST1 op ST0
    const double b = c.fpop();
    c.ST(0) = F(c.ST(0), b);
    c.ret();
}
template <double (*F)(double)>
void sse1(Cpu& c) {  // __libm_sse2_sin...: xmm0 in, xmm0 out
    c.xmm[0].f64[0] = F(c.xmm[0].f64[0]);
    c.ret();
}
template <double (*F)(double, double)>
void sse2(Cpu& c) {
    c.xmm[0].f64[0] = F(c.xmm[0].f64[0], c.xmm[1].f64[0]);
    c.ret();
}
double dsin(double x) { return std::sin(x); }
double dcos(double x) { return std::cos(x); }
double dtan(double x) { return std::tan(x); }
double dasin(double x) { return std::asin(x); }
double dacos(double x) { return std::acos(x); }
double datan(double x) { return std::atan(x); }
double dsqrt(double x) { return std::sqrt(x); }
double dexp(double x) { return std::exp(x); }
double dlog(double x) { return std::log(x); }
double dlog10(double x) { return std::log10(x); }
double dfloor(double x) { return std::floor(x); }
double dceil(double x) { return std::ceil(x); }
double dround(double x) { return std::round(x); }
double dfabs(double x) { return std::fabs(x); }
double dpow(double x, double y) { return std::pow(x, y); }
double dfmod(double x, double y) { return std::fmod(x, y); }
double datan2(double y, double x) { return std::atan2(y, x); }
void _ftol2(Cpu& c) {  // ST0 -> edx:eax, truncated
    const s64 v = s64(std::trunc(c.fpop()));
    c.r[Cpu::EAX] = u32(v), c.r[Cpu::EDX] = u32(u64(v) >> 32);
    c.ret();
}
void _ftol2_sse(Cpu& c) { _ftol2(c); }

// ---- time ----
void _time64(Cpu& c) {
    const s64 t = s64(std::time(nullptr));
    if (c.arg(0)) M().w64(c.arg(0), u64(t));
    retc64(c, u64(t));
}
void _time32(Cpu& c) {
    const u32 t = u32(std::time(nullptr));
    if (c.arg(0)) M().w32(c.arg(0), t);
    retc(c, t);
}
u32 putTm(const std::tm& t) {
    const u32 p = tebScratch(0x20);
    const s32 v[9] = {t.tm_sec, t.tm_min, t.tm_hour, t.tm_mday, t.tm_mon, t.tm_year, t.tm_wday, t.tm_yday, t.tm_isdst};
    for (int i = 0; i < 9; i++) M().w32(p + 4 * u32(i), u32(v[i]));
    return p;
}
std::tm getTm(u32 p) {
    std::tm t{};
    s32 v[9];
    for (int i = 0; i < 9; i++) v[i] = s32(M().r32(p + 4 * u32(i)));
    t.tm_sec = v[0], t.tm_min = v[1], t.tm_hour = v[2], t.tm_mday = v[3], t.tm_mon = v[4], t.tm_year = v[5];
    t.tm_wday = v[6], t.tm_yday = v[7], t.tm_isdst = v[8];
    return t;
}
void localtime_(Cpu& c, bool local) {
    const std::time_t t = std::time_t(s64(M().r64(c.arg(0))));
    std::tm tmv{};
#ifdef _WIN32
    local ? localtime_s(&tmv, &t) : gmtime_s(&tmv, &t);
#else
    local ? localtime_r(&t, &tmv) : gmtime_r(&t, &tmv);
#endif
    retc(c, putTm(tmv));
}
void _localtime64(Cpu& c) { localtime_(c, true); }
void _gmtime64(Cpu& c) { localtime_(c, false); }
void _mktime64(Cpu& c) {
    std::tm t = getTm(c.arg(0));
    retc64(c, u64(s64(std::mktime(&t))));
}
void strftime_(Cpu& c) {
    const std::tm t = getTm(c.arg(3));
    std::vector<char> buf(c.arg(1) + 1);
    const size_t n = std::strftime(buf.data(), buf.size(), cs(c.arg(2)).c_str(), &t);
    if (n && n < c.arg(1)) putStr(c.arg(0), std::string(buf.data(), n));
    retc(c, n < c.arg(1) ? u32(n) : 0);
}
void wcsftime_(Cpu& c) {
    const std::tm t = getTm(c.arg(3));
    std::vector<char> buf(c.arg(1) * 4 + 1);
    const size_t n = std::strftime(buf.data(), buf.size(), utf8(ws(c.arg(2))).c_str(), &t);
    const std::u16string w = utf16(std::string(buf.data(), n));
    if (w.size() >= c.arg(1)) return retc(c, 0);
    putWide(c.arg(0), w);
    retc(c, u32(w.size()));
}
void clock_(Cpu& c) {
    static const auto t0 = std::chrono::steady_clock::now();
    retc(c, u32(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count()));
}

// ---- setjmp / longjmp (MSVC _JUMP_BUFFER: ebp ebx edi esi esp eip registration trylevel...) ----
void _setjmp3(Cpu& c) {
    const u32 b = c.arg(0);
    M().w32(b + 0, c.r[Cpu::EBP]);
    M().w32(b + 4, c.r[Cpu::EBX]);
    M().w32(b + 8, c.r[Cpu::EDI]);
    M().w32(b + 12, c.r[Cpu::ESI]);
    M().w32(b + 16, c.r[Cpu::ESP] + 4);
    M().w32(b + 20, M().r32(c.r[Cpu::ESP]));
    M().w32(b + 24, M().r32(c.fsBase));
    M().w32(b + 28, 0xFFFFFFFF);
    M().w32(b + 32, 0x56433230);  // 'VC20'
    M().w32(b + 36, 0);
    retc(c, 0);
}
void longjmp_(Cpu& c) {
    const u32 b = c.arg(0), v = c.arg(1);
    c.r[Cpu::EBP] = M().r32(b + 0);
    c.r[Cpu::EBX] = M().r32(b + 4);
    c.r[Cpu::EDI] = M().r32(b + 8);
    c.r[Cpu::ESI] = M().r32(b + 12);
    c.r[Cpu::ESP] = M().r32(b + 16);
    c.eip = M().r32(b + 20);
    M().w32(c.fsBase, M().r32(b + 24));
    c.r[Cpu::EAX] = v ? v : 1;
}

// ---- files and directories (<io.h>, <direct.h>, <sys/stat.h>) ----
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
struct CFind {
    std::vector<std::pair<std::string, fs::directory_entry>> list;
    size_t next = 0;
};
std::map<u32, CFind> s_finds;
u32 s_nextFind = 0x1000;
void fillFind32(u32 p, const std::string& name, const fs::directory_entry& e, bool wide) {
    std::error_code ec;
    const bool dir = e.is_directory(ec);
    const u32 t = u32(std::time(nullptr));
    M().w32(p + 0, dir ? 0x10 : 0x20);
    M().w32(p + 4, t), M().w32(p + 8, t), M().w32(p + 12, t);
    M().w32(p + 16, dir ? 0 : u32(e.file_size(ec)));
    if (wide) putWide(p + 20, utf16(name), 260);
    else putStr(p + 20, name, 260);
}
void findfirst_(Cpu& c, bool wide) {
    std::string pat = wide ? wideArg(c.arg(0)) : strArg(c.arg(0));
    for (char& ch : pat)
        if (ch == '\\') ch = '/';
    const size_t slash = pat.find_last_of('/');
    const std::string dir = slash == std::string::npos ? "." : pat.substr(0, slash);
    std::string mask = slash == std::string::npos ? pat : pat.substr(slash + 1);
    if (mask == "*.*") mask = "*";
    CFind f;
    const std::string hdir = hostPath(dir);
    std::error_code ec;
    if (wildMatch(mask.c_str(), ".")) f.list.push_back({".", fs::directory_entry(hdir)});
    if (wildMatch(mask.c_str(), "..")) f.list.push_back({"..", fs::directory_entry(hdir)});
    for (const auto& e : fs::directory_iterator(hdir, ec)) {
        const std::string n = e.path().filename().string();
        if (wildMatch(mask.c_str(), n.c_str())) f.list.push_back({n, e});
    }
    if (f.list.empty() || ec) return setErrno(2), retc(c, 0xFFFFFFFF);
    fillFind32(c.arg(1), f.list[0].first, f.list[0].second, wide);
    f.next = 1;
    const u32 h = s_nextFind++;
    s_finds[h] = std::move(f);
    retc(c, h);
}
void findnext_(Cpu& c, bool wide) {
    const auto it = s_finds.find(c.arg(0));
    // the end: ENOENT, and the Windows error MinGW's readdir checks to tell the end from a failure
    if (it == s_finds.end() || it->second.next >= it->second.list.size())
        return setErrno(2), setLastError(18), retc(c, 0xFFFFFFFF);
    auto& [name, e] = it->second.list[it->second.next++];
    fillFind32(c.arg(1), name, e, wide);
    retc(c, 0);
}
void _wfindfirst32(Cpu& c) { findfirst_(c, true); }
void _wfindnext32(Cpu& c) { findnext_(c, true); }
void _findfirst32(Cpu& c) { findfirst_(c, false); }
void _findnext32(Cpu& c) { findnext_(c, false); }
void _findclose(Cpu& c) { retc(c, s_finds.erase(c.arg(0)) ? 0 : u32(-1)); }
bool stat64(const std::string& p, u32 st) {
    std::error_code ec;
    const auto s = fs::status(p, ec);
    if (ec || !fs::exists(s)) return setErrno(2), false;
    const bool dir = fs::is_directory(s);
    for (u32 i = 0; i < 56; i += 4) M().w32(st + i, 0);
    M().w16(st + 6, dir ? 0x41FF : 0x81B6);
    M().w16(st + 8, 1);
    M().w64(st + 24, dir ? 0 : u64(fs::file_size(p, ec)));
    const u64 t = u64(std::time(nullptr));
    M().w64(st + 32, t), M().w64(st + 40, t), M().w64(st + 48, t);
    return true;
}
void _wstat64(Cpu& c) { retc(c, stat64(hostPath(wideArg(c.arg(0))), c.arg(1)) ? 0 : u32(-1)); }
void _stat64(Cpu& c) { retc(c, stat64(hostPath(strArg(c.arg(0))), c.arg(1)) ? 0 : u32(-1)); }
void _fstat64(Cpu& c) {
    CFile* f = cfile(fileOfFd(int(c.arg(0))));
    retc(c, f && stat64(f->path, c.arg(1)) ? 0 : u32(-1));
}
void mkdir_(Cpu& c, bool wide) {
    std::error_code ec;
    const std::string p = hostPath(wide ? wideArg(c.arg(0)) : strArg(c.arg(0)));
    const bool ok = fs::create_directory(p, ec);
    if (!ok) setErrno(fs::exists(p) ? 17 : 2);  // EEXIST, or ENOENT (no parent)
    retc(c, ok ? 0 : u32(-1));
}
void _wmkdir(Cpu& c) { mkdir_(c, true); }
void _mkdir(Cpu& c) { mkdir_(c, false); }
void access_(Cpu& c, bool wide) {
    std::error_code ec;
    retc(c, fs::exists(hostPath(wide ? wideArg(c.arg(0)) : strArg(c.arg(0))), ec) ? 0 : (setErrno(2), u32(-1)));
}
void _waccess(Cpu& c) { access_(c, true); }
void _access(Cpu& c) { access_(c, false); }
void remove_(Cpu& c, bool wide) {
    std::error_code ec;
    retc(c, fs::remove(hostPath(wide ? wideArg(c.arg(0)) : strArg(c.arg(0))), ec) ? 0 : u32(-1));
}
void _wremove(Cpu& c) { remove_(c, true); }
void remove__(Cpu& c) { remove_(c, false); }
void rename_(Cpu& c, bool wide) {
    std::error_code ec;
    fs::rename(hostPath(wide ? wideArg(c.arg(0)) : strArg(c.arg(0))), hostPath(wide ? wideArg(c.arg(1)) : strArg(c.arg(1))), ec);
    retc(c, ec ? u32(-1) : 0);
}
void _wrename(Cpu& c) { rename_(c, true); }
void rename__(Cpu& c) { rename_(c, false); }
void _wgetcwd(Cpu& c) {
    const u32 buf = c.arg(0) ? c.arg(0) : host().alloc(2 * std::max<u32>(c.arg(1), 260));
    putWide(buf, utf16(kGameDir), std::max<u32>(c.arg(1), 260));
    retc(c, buf);
}
void _getcwd(Cpu& c) {
    const u32 buf = c.arg(0) ? c.arg(0) : host().alloc(std::max<u32>(c.arg(1), 260));
    putStr(buf, kGameDir, std::max<u32>(c.arg(1), 260));
    retc(c, buf);
}
std::string fullPathOf(const std::string& p) {
    if (p.size() > 1 && p[1] == ':') return p;
    std::string r = std::string(kGameDir) + "\\" + p;
    for (char& ch : r)
        if (ch == '/') ch = '\\';
    return r;
}
void _wfullpath(Cpu& c) {
    const std::u16string w = utf16(fullPathOf(wideArg(c.arg(1))));
    const u32 buf = c.arg(0) ? c.arg(0) : host().alloc(2 * u32(w.size() + 1));
    if (c.arg(0) && w.size() >= c.arg(2)) return retc(c, 0);
    putWide(buf, w);
    retc(c, buf);
}
void _fullpath(Cpu& c) {
    const std::string s = fullPathOf(strArg(c.arg(1)));
    const u32 buf = c.arg(0) ? c.arg(0) : host().alloc(u32(s.size() + 1));
    if (c.arg(0) && s.size() >= c.arg(2)) return retc(c, 0);
    putStr(buf, s);
    retc(c, buf);
}

// ---- runtime ----
void _errno(Cpu& c) { retc(c, tebScratch(0)); }
void __doserrno(Cpu& c) { retc(c, tebScratch(4)); }
void _get_errno(Cpu& c) {
    M().w32(c.arg(0), M().r32(tebScratch(0)));
    retc(c, 0);
}
void _set_errno(Cpu& c) {
    setErrno(int(c.arg(0)));
    retc(c, 0);
}
void strerror_(Cpu& c) {
    const u32 buf = tebScratch(0x100);
    putStr(buf, std::string("error ") + std::to_string(c.arg(0)), 128);
    retc(c, buf);
}
void abort_(Cpu&) { throw ModFault("abort()"); }
void exit_(Cpu& c) { throw ModFault("exit(" + std::to_string(c.arg(0)) + ")"); }
void _assert_(Cpu& c) {
    throw ModFault("assertion failed: " + strArg(c.arg(0)) + " (" + strArg(c.arg(1)) + ":" + std::to_string(c.arg(2)) + ")");
}
void _wassert(Cpu& c) {
    throw ModFault("assertion failed: " + wideArg(c.arg(0)) + " (" + wideArg(c.arg(1)) + ":" + std::to_string(c.arg(2)) + ")");
}
void system_(Cpu& c) { retc(c, u32(-1)); }
void getenv_(Cpu& c) { retc(c, 0); }
void _beginthreadex(Cpu& c) {  // (security, stack, start, arg, flags, *id)
    u32 id = 0;
    const u32 h = threadCreate(c.arg(2), c.arg(3), c.arg(1), c.arg(4) & 4, &id);
    if (c.arg(5)) M().w32(c.arg(5), id);
    retc(c, h);
}
void _endthreadex(Cpu& c) { threadExit(c.arg(0)); }
void _purecall(Cpu&) { throw ModFault("pure virtual function call"); }
void terminate_(Cpu&) { throw ModFault("std::terminate"); }

// startup
void initterm(Cpu& c) {
    const u32 first = c.arg(0), last = c.arg(1);
    for (u32 p = first; p < last; p += 4)
        if (const u32 fn = M().r32(p)) c.call(fn);
    retc(c, 0);
}
void initterm_e(Cpu& c) {
    const u32 first = c.arg(0), last = c.arg(1);
    for (u32 p = first; p < last; p += 4)
        if (const u32 fn = M().r32(p))
            if (const u32 r = c.call(fn)) return retc(c, r);
    retc(c, 0);
}
void initialize_onexit_table(Cpu& c) {
    for (u32 i = 0; i < 12; i += 4) M().w32(c.arg(0) + i, 0);
    retc(c, 0);
}
void register_onexit_function(Cpu& c) {
    host().atexit.push_back(c.arg(1));
    retc(c, 0);
}
void atexit_(Cpu& c) {
    host().atexit.push_back(c.arg(0));
    retc(c, 0);
}
void invalid_parameter(Cpu&) { throw ModFault("invalid parameter to a C runtime function"); }

// ---- utility ----
u32 s_randSeed = 1;  // the Microsoft C runtime's rand(): the PC sequence (seed 1 unless srand)
void rand_(Cpu& c) {
    s_randSeed = s_randSeed * 214013 + 2531011;
    retc(c, (s_randSeed >> 16) & 0x7FFF);
}
void srand_(Cpu& c) {
    s_randSeed = c.arg(0);
    retc(c, 0);
}
void rand_s_(Cpu& c) {
    static std::mt19937 g{std::random_device{}()};
    M().w32(c.arg(0), g());
    retc(c, 0);
}
void qsort_(Cpu& c) {  // (base, n, width, cmp): sorts an index list with the mod's comparator
    const u32 base = c.arg(0), n = c.arg(1), w = c.arg(2), cmp = c.arg(3);
    std::vector<u32> idx(n);
    for (u32 i = 0; i < n; i++) idx[i] = i;
    std::stable_sort(idx.begin(), idx.end(), [&](u32 a, u32 b) { return s32(c.call(cmp, {base + a * w, base + b * w})) < 0; });
    std::vector<u8> tmp(size_t(n) * w);
    for (u32 i = 0; i < n; i++) M().read(base + idx[i] * w, tmp.data() + size_t(i) * w, w);
    M().write(base, tmp.data(), u32(tmp.size()));
    retc(c, 0);
}
void bsearch_(Cpu& c) {  // (key, base, n, width, cmp)
    u32 lo = 0, hi = c.arg(2);
    while (lo < hi) {
        const u32 mid = (lo + hi) / 2, at = c.arg(1) + mid * c.arg(3);
        const s32 r = s32(c.call(c.arg(4), {c.arg(0), at}));
        if (!r) return retc(c, at);
        if (r < 0) hi = mid;
        else lo = mid + 1;
    }
    retc(c, 0);
}
void abs_(Cpu& c) { retc(c, u32(std::abs(s32(c.arg(0))))); }

// ---- heap ----
void malloc_(Cpu& c) { retc(c, host().alloc(c.arg(0))); }
void calloc_(Cpu& c) {
    const u32 n = c.arg(0) * c.arg(1), p = host().alloc(n);
    for (u32 i = 0; i < n; i++) M().w8(p + i, 0);
    retc(c, p);
}
void free_(Cpu& c) {
    host().free(c.arg(0));
    retc(c, 0);
}
void realloc_(Cpu& c) {
    const u32 old = c.arg(0), n = c.arg(1);
    if (!old) return retc(c, host().alloc(n));
    if (!n) return host().free(old), retc(c, 0);
    const u32 p = host().alloc(n), keep = std::min(n, host().blockSize(old));
    for (u32 i = 0; i < keep; i++) M().w8(p + i, M().r8(old + i));
    host().free(old);
    retc(c, p);
}
void _msize(Cpu& c) { retc(c, host().blockSize(c.arg(0))); }
void _aligned_malloc(Cpu& c) {  // (size, align): the real block's address is kept before the result
    const u32 align = std::max<u32>(c.arg(1), 4), raw = host().alloc(c.arg(0) + align + 4);
    const u32 p = (raw + 4 + align - 1) & ~(align - 1);
    M().w32(p - 4, raw);
    retc(c, p);
}
void _aligned_free(Cpu& c) {
    if (c.arg(0)) host().free(M().r32(c.arg(0) - 4));
    retc(c, 0);
}

}  // namespace

const ImportEntry kCrt[] = {
    // stdio
    {"fopen", fopen_}, {"_wfopen", _wfopen}, {"_fsopen", _fsopen}, {"_wfsopen", _wfopen}, {"fopen_s", fopen_s},
    {"_wfopen_s", _wfopen_s}, {"fclose", fclose_}, {"fflush", fflush_}, {"fread", fread_}, {"fwrite", fwrite_},
    {"fgets", fgets_}, {"fputs", fputs_}, {"fputc", fputc_}, {"putc", fputc_}, {"fgetc", fgetc_}, {"getc", fgetc_},
    {"ungetc", ungetc_}, {"getwc", getwc_}, {"fgetwc", getwc_}, {"putwc", putwc_}, {"fputwc", putwc_},
    {"ungetwc", ungetwc_}, {"feof", feof_}, {"ferror", ferror_}, {"fseek", fseek_}, {"ftell", ftell_},
    {"_fseeki64", _fseeki64}, {"_ftelli64", _ftelli64}, {"setvbuf", setvbuf_}, {"_lock_file", nop0},
    {"_unlock_file", nop0}, {"__acrt_iob_func", __acrt_iob_func}, {"_fileno", _fileno}, {"_open", _open},
    {"_wopen", _wopen}, {"_close", _close}, {"_read", _read}, {"_write", _write}, {"_lseeki64", _lseeki64},
    {"_chsize_s", _chsize_s}, {"_fdopen", _fdopen}, {"_setmode", _setmode}, {"_get_osfhandle", _get_osfhandle},
    {"__stdio_common_vsprintf", __stdio_common_vsprintf}, {"__stdio_common_vswprintf", __stdio_common_vswprintf},
    {"__stdio_common_vsprintf_s", __stdio_common_vsprintf_s},
    {"__stdio_common_vswprintf_s", __stdio_common_vswprintf_s},
    {"__stdio_common_vsnprintf_s", __stdio_common_vsnprintf_s},
    {"__stdio_common_vsnwprintf_s", __stdio_common_vsnwprintf_s},
    {"__stdio_common_vfprintf", __stdio_common_vfprintf}, {"__stdio_common_vfprintf_s", __stdio_common_vfprintf},
    {"__stdio_common_vfwprintf", __stdio_common_vfwprintf}, {"printf", printf_}, {"sprintf", sprintf_},
    // strings
    {"strlen", strlen_}, {"strnlen", strnlen_}, {"strcmp", strcmp_}, {"strncmp", strncmp_}, {"_stricmp", _stricmp},
    {"_strnicmp", _strnicmp}, {"strcoll", strcmp_}, {"strcpy", strcpy_}, {"strcat", strcat_}, {"strncpy", strncpy_},
    {"strncat", strncat_}, {"strcpy_s", strcpy_s}, {"strcat_s", strcat_s}, {"strncpy_s", strncpy_s},
    {"strchr", strchr_}, {"strrchr", strrchr_}, {"strstr", strstr_}, {"strspn", strspn_}, {"strcspn", strcspn_},
    {"strpbrk", strpbrk_}, {"_strdup", _strdup}, {"strxfrm", strxfrm_}, {"memchr", memchr_}, {"memcpy", memcpy_},
    {"memmove", memcpy_}, {"memset", memset_}, {"memcmp", memcmp_},
    {"wcslen", wcslen_}, {"wcsnlen", wcsnlen_}, {"wcscmp", wcscmp_}, {"wcsncmp", wcsncmp_}, {"_wcsicmp", _wcsicmp},
    {"wcscoll", wcscmp_}, {"wcscpy", wcscpy_}, {"wcscat", wcscat_}, {"wcscpy_s", wcscpy_s}, {"wcscat_s", wcscat_s},
    {"wcsxfrm", wcsxfrm_}, {"wcschr", wcschr_}, {"wcsrchr", wcsrchr_}, {"_wcsdup", _wcsdup},
    // ctype, conversions, locale
    {"isdigit", isdigit_}, {"isspace", isspace_}, {"isxdigit", isxdigit_}, {"isalpha", isalpha_},
    {"isalnum", isalnum_}, {"isupper", isupper_}, {"islower", islower_}, {"isprint", isprint_},
    {"ispunct", ispunct_}, {"tolower", tolower_}, {"toupper", toupper_}, {"towlower", towlower_},
    {"towupper", towupper_}, {"iswctype", iswctype_}, {"wctype", wctype_}, {"__pctype_func", __pctype_func},
    {"strtol", strtol_}, {"strtoul", strtoul_}, {"strtoll", strtoll_}, {"strtoull", strtoull_}, {"atoi", atoi_},
    {"atol", atoi_}, {"_wtoi", _wtoi}, {"_atoi64", _atoi64}, {"atof", atof_}, {"strtod", strtod_},
    {"strtof", strtof_}, {"wcstol", wcstol_}, {"wcstod", wcstod_}, {"mbrtowc", mbrtowc_}, {"mbrlen", mbrlen_},
    {"wcrtomb", wcrtomb_}, {"mbsrtowcs", mbsrtowcs_}, {"wcstombs", wcstombs_}, {"mbstowcs", mbstowcs_},
    {"setlocale", setlocale_}, {"localeconv", localeconv_}, {"___mb_cur_max_func", one},
    {"___lc_codepage_func", nop0},
    // math
    {"sin", math1<dsin>}, {"cos", math1<dcos>}, {"tan", math1<dtan>}, {"asin", math1<dasin>},
    {"acos", math1<dacos>}, {"atan", math1<datan>}, {"sqrt", math1<dsqrt>}, {"exp", math1<dexp>},
    {"log", math1<dlog>}, {"log10", math1<dlog10>}, {"floor", math1<dfloor>}, {"ceil", math1<dceil>},
    {"round", math1<dround>}, {"fabs", math1<dfabs>}, {"pow", math2<dpow>}, {"fmod", math2<dfmod>},
    {"atan2", math2<datan2>}, {"sinf", math1f<dsin>}, {"cosf", math1f<dcos>}, {"tanf", math1f<dtan>},
    {"sqrtf", math1f<dsqrt>}, {"floorf", math1f<dfloor>}, {"ceilf", math1f<dceil>}, {"roundf", math1f<dround>},
    {"acosf", math1f<dacos>}, {"asinf", math1f<dasin>}, {"atanf", math1f<datan>}, {"powf", math2f<dpow>},
    {"fmodf", math2f<dfmod>}, {"atan2f", math2f<datan2>},
    {"_CIsin", ci1<dsin>}, {"_CIcos", ci1<dcos>}, {"_CItan", ci1<dtan>}, {"_CIasin", ci1<dasin>},
    {"_CIacos", ci1<dacos>}, {"_CIatan", ci1<datan>}, {"_CIsqrt", ci1<dsqrt>}, {"_CIexp", ci1<dexp>},
    {"_CIlog", ci1<dlog>}, {"_CIlog10", ci1<dlog10>}, {"_CIpow", ci2<dpow>}, {"_CIfmod", ci2<dfmod>},
    {"_CIatan2", ci2<datan2>}, {"__libm_sse2_sin", sse1<dsin>}, {"__libm_sse2_cos", sse1<dcos>},
    {"__libm_sse2_tan", sse1<dtan>}, {"__libm_sse2_asin", sse1<dasin>}, {"__libm_sse2_acos", sse1<dacos>},
    {"__libm_sse2_atan", sse1<datan>}, {"__libm_sse2_exp", sse1<dexp>}, {"__libm_sse2_log", sse1<dlog>},
    {"__libm_sse2_log10", sse1<dlog10>}, {"__libm_sse2_pow", sse2<dpow>}, {"__libm_sse2_atan2", sse2<datan2>},
    {"_ftol2", _ftol2}, {"_ftol2_sse", _ftol2_sse}, {"__setusermatherr", nop0},
    // time
    {"_time64", _time64}, {"_time32", _time32}, {"time", _time32}, {"_localtime64", _localtime64},
    {"_gmtime64", _gmtime64}, {"_mktime64", _mktime64}, {"strftime", strftime_}, {"wcsftime", wcsftime_},
    {"_wutime64", nop0}, {"clock", clock_},
    // setjmp
    {"_setjmp3", _setjmp3}, {"longjmp", longjmp_},
    // files and directories
    {"_wfindfirst32", _wfindfirst32}, {"_wfindnext32", _wfindnext32}, {"_findfirst32", _findfirst32},
    {"_findnext32", _findnext32}, {"_findclose", _findclose}, {"_wstat64", _wstat64}, {"_stat64", _stat64},
    {"_fstat64", _fstat64}, {"_wmkdir", _wmkdir}, {"_mkdir", _mkdir}, {"_wchdir", nop0}, {"_chdir", nop0},
    {"_wchmod", nop0}, {"_waccess", _waccess}, {"_access", _access}, {"_wremove", _wremove}, {"remove", remove__},
    {"_wunlink", _wremove}, {"_unlink", remove__}, {"_wrename", _wrename}, {"rename", rename__},
    {"_wgetcwd", _wgetcwd}, {"_getcwd", _getcwd}, {"_wfullpath", _wfullpath}, {"_fullpath", _fullpath},
    // runtime
    {"_errno", _errno}, {"__doserrno", __doserrno}, {"_get_errno", _get_errno}, {"_set_errno", _set_errno},
    {"strerror", strerror_}, {"abort", abort_}, {"exit", exit_}, {"_exit", exit_}, {"_assert", _assert_},
    {"_wassert", _wassert}, {"system", system_}, {"getenv", getenv_}, {"_beginthreadex", _beginthreadex},
    {"_endthreadex", _endthreadex}, {"_purecall", _purecall}, {"terminate", terminate_},
    {"_initterm", initterm}, {"_initterm_e", initterm_e}, {"_initialize_onexit_table", initialize_onexit_table},
    {"_register_onexit_function", register_onexit_function}, {"_crt_atexit", atexit_}, {"atexit", atexit_},
    {"_execute_onexit_table", nop0}, {"_cexit", nop0}, {"_c_exit", nop0}, {"_seh_filter_dll", nop0},
    {"_configure_narrow_argv", nop0}, {"_initialize_narrow_environment", nop0},
    {"_invalid_parameter_noinfo", invalid_parameter}, {"_invalid_parameter_noinfo_noreturn", invalid_parameter},
    {"_callnewh", nop0},
    // utility, heap
    {"rand", rand_}, {"srand", srand_}, {"rand_s", rand_s_}, {"qsort", qsort_}, {"bsearch", bsearch_},
    {"abs", abs_}, {"labs", abs_}, {"malloc", malloc_}, {"calloc", calloc_}, {"free", free_},
    {"realloc", realloc_}, {"_msize", _msize}, {"_aligned_malloc", _aligned_malloc},
    {"_aligned_free", _aligned_free},
};
const size_t kCrtCount = std::size(kCrt);

}  // namespace mh
