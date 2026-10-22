// SA2B launcher: picks the disc, sets the game's options, builds HD textures, turns mods on and
// off, and starts sa2b. It looks like the game's own menus because it is drawn with them - the
// title art, the bokeh lights, the menu bars and the message font are read from the user's disc
// when it starts (nothing of the game ships with it). Its own button words are prerendered images
// (labels.inc, tools/launcher/mklabels.py).
//
// Files, in the data folder (the first of the working folder, the launcher's folder and the one two
// above it that has saves/ or launcher.ini; else the launcher's): launcher.ini (settings),
// mods/enabled.txt (mods turned on), textures/GSNE8P (the HD texture pack), setup.log.
//
// The game's code is not shipped: on the first PLAY (and after an update or a change of disc) the
// SETUP page builds it from the player's disc (setup.h, docs/SETUP.md).
#include "launcher/disc.h"
#include "launcher/setup.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <mutex>
#include <random>
#include <string>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

namespace fs = std::filesystem;

namespace launcher {

struct LabelImage {
    const char* id;
    int w, h;
    const unsigned char* rle;
    unsigned size;
};
#include "launcher/labels.inc"

namespace {

constexpr float kW = 1280, kH = 720;  // layout space
constexpr bool kMods = false;         // mod support is off for now: no MODS page, the game never gets --mods

// Paths as UTF-8 (settings file, messages) and back.
fs::path pathU8(const std::string& s) { return fs::path(std::u8string(s.begin(), s.end())); }
std::string strU8(const fs::path& p) {
    const std::u8string u = p.u8string();
    return std::string(u.begin(), u.end());
}

// ---- settings ----
struct Config {
    std::string disc;
    bool fullscreen = false;
    int winW = 1920, winH = 1080;
    bool widescreen = true;
    bool fixed60 = false;  // false: in-between frames on high-refresh displays
    int volume = 100;
    bool fastDisc = false;
    bool hdTextures = true;

    void load(const fs::path& p) {
        std::ifstream f(p);
        std::string line;
        while (std::getline(f, line)) {
            const size_t eq = line.find('=');
            if (eq == std::string::npos) continue;
            const std::string k = line.substr(0, eq), v = line.substr(eq + 1);
            if (k == "disc") disc = v;
            else if (k == "fullscreen") fullscreen = v == "1";
            else if (k == "window") winW = std::atoi(v.c_str()), winH = v.find('x') != std::string::npos ? std::atoi(v.c_str() + v.find('x') + 1) : winH;
            else if (k == "widescreen") widescreen = v == "1";
            else if (k == "framerate") fixed60 = v == "60";
            else if (k == "volume") volume = std::clamp(std::atoi(v.c_str()), 0, 100);
            else if (k == "discloading") fastDisc = v == "instant";
            else if (k == "hdtextures") hdTextures = v == "1";
        }
    }
    void save(const fs::path& p) const {
        std::ofstream f(p);
        f << "disc=" << disc << "\nfullscreen=" << fullscreen << "\nwindow=" << winW << "x" << winH << "\nwidescreen=" << widescreen
          << "\nframerate=" << (fixed60 ? "60" : "auto") << "\nvolume=" << volume << "\ndiscloading=" << (fastDisc ? "instant" : "console")
          << "\nhdtextures=" << hdTextures << "\n";
    }
};

const int kSizes[][2] = {{1280, 720}, {1600, 900}, {1920, 1080}, {2560, 1440}, {3840, 2160}};

// ---- drawing ----
struct Tex {
    SDL_Texture* t = nullptr;
    float w = 0, h = 0;
    explicit operator bool() const { return t != nullptr; }
};

SDL_Renderer* g_ren = nullptr;

Tex makeTex(const Image& img, bool linear = true) {
    Tex out;
    if (img.w <= 0 || img.h <= 0) return out;
    out.t = SDL_CreateTexture(g_ren, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STATIC, img.w, img.h);
    if (!out.t) return out;
    SDL_UpdateTexture(out.t, nullptr, img.rgba.data(), img.w * 4);
    SDL_SetTextureBlendMode(out.t, SDL_BLENDMODE_BLEND);
    SDL_SetTextureScaleMode(out.t, linear ? SDL_SCALEMODE_LINEAR : SDL_SCALEMODE_NEAREST);
    out.w = float(img.w), out.h = float(img.h);
    return out;
}
void freeTex(Tex& t) {
    if (t.t) SDL_DestroyTexture(t.t);
    t = {};
}

void draw(const Tex& t, SDL_FRect src, SDL_FRect dst, float r = 1, float g = 1, float b = 1, float a = 1, SDL_BlendMode blend = SDL_BLENDMODE_BLEND) {
    if (!t) return;
    SDL_SetTextureColorModFloat(t.t, r, g, b);
    SDL_SetTextureAlphaModFloat(t.t, std::clamp(a, 0.f, 1.f));
    SDL_SetTextureBlendMode(t.t, blend);
    SDL_RenderTexture(g_ren, t.t, &src, &dst);
}
void draw(const Tex& t, SDL_FRect dst, float r = 1, float g = 1, float b = 1, float a = 1, SDL_BlendMode blend = SDL_BLENDMODE_BLEND) {
    draw(t, {0, 0, t.w, t.h}, dst, r, g, b, a, blend);
}

// A rectangle with a colour per corner (top left, top right, bottom right, bottom left).
void gradient(SDL_FRect r, SDL_FColor tl, SDL_FColor tr, SDL_FColor br, SDL_FColor bl) {
    const SDL_Vertex v[4] = {{{r.x, r.y}, tl, {0, 0}}, {{r.x + r.w, r.y}, tr, {0, 0}}, {{r.x + r.w, r.y + r.h}, br, {0, 0}}, {{r.x, r.y + r.h}, bl, {0, 0}}};
    const int idx[6] = {0, 1, 2, 0, 2, 3};
    SDL_SetRenderDrawBlendMode(g_ren, SDL_BLENDMODE_BLEND);
    SDL_RenderGeometry(g_ren, nullptr, v, 4, idx, 6);
}
void fill(SDL_FRect r, SDL_FColor c) { gradient(r, c, c, c, c); }

// ---- images built here ----
// Coverage mask -> coloured label: light top-to-bottom gradient, dark outline, drop shadow.
Image buildLabel(const std::vector<uint8_t>& mask, int w, int h, int outline, int shadow) {
    const int pad = outline + shadow + 2;
    Image img;
    img.w = w + 2 * pad, img.h = h + 2 * pad;
    std::vector<float> m(size_t(img.w) * img.h, 0.f), dil(m.size(), 0.f);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) m[size_t(y + pad) * img.w + x + pad] = mask[size_t(y) * w + x] / 255.f;
    for (int y = 0; y < img.h; y++)
        for (int x = 0; x < img.w; x++) {
            float v = 0;
            for (int dy = -outline; dy <= outline; dy++)
                for (int dx = -outline; dx <= outline; dx++) {
                    if (dx * dx + dy * dy > outline * outline + outline) continue;
                    const int sx = x + dx, sy = y + dy;
                    if (sx >= 0 && sy >= 0 && sx < img.w && sy < img.h) v = std::max(v, m[size_t(sy) * img.w + sx]);
                }
            dil[size_t(y) * img.w + x] = v;
        }
    img.rgba.assign(size_t(img.w) * img.h * 4, 0);
    for (int y = 0; y < img.h; y++)
        for (int x = 0; x < img.w; x++) {
            float r = 0, g = 0, b = 0, a = 0;
            auto over = [&](float cr, float cg, float cb, float ca) {
                r = cr * ca + r * (1 - ca), g = cg * ca + g * (1 - ca), b = cb * ca + b * (1 - ca), a = ca + a * (1 - ca);
            };
            const int sx = x - shadow, sy = y - shadow;
            if (sx >= 0 && sy >= 0) over(0.02f, 0.03f, 0.10f, 0.75f * dil[size_t(sy) * img.w + sx]);
            over(0.04f, 0.05f, 0.16f, dil[size_t(y) * img.w + x]);
            const float t = float(y - pad) / float(std::max(1, h));
            const float shade = 1.f - 0.26f * std::clamp(t, 0.f, 1.f);
            over(shade, shade * 1.01f > 1 ? 1 : shade * 1.01f, std::min(1.f, shade * 1.06f), m[size_t(y) * img.w + x]);
            uint8_t* p = &img.rgba[(size_t(y) * img.w + x) * 4];
            // stored premultiplied-free: colour divided back out of the coverage
            p[0] = uint8_t(a > 0 ? std::clamp(r / a, 0.f, 1.f) * 255 : 0);
            p[1] = uint8_t(a > 0 ? std::clamp(g / a, 0.f, 1.f) * 255 : 0);
            p[2] = uint8_t(a > 0 ? std::clamp(b / a, 0.f, 1.f) * 255 : 0);
            p[3] = uint8_t(std::clamp(a, 0.f, 1.f) * 255);
        }
    return img;
}

Image labelImage(const char* id) {
    for (const LabelImage& l : kLabels) {
        if (std::strcmp(l.id, id) != 0) continue;
        std::vector<uint8_t> mask;
        mask.reserve(size_t(l.w) * l.h);
        for (unsigned i = 0; i + 1 < l.size; i += 2) mask.insert(mask.end(), size_t(l.rle[i]) + 1, l.rle[i + 1]);
        mask.resize(size_t(l.w) * l.h);
        return buildLabel(mask, l.w, l.h, 4, 5);
    }
    return {};
}

// A soft ring of light (used for the bokeh before a disc gives the game's own).
Image softRing(float r, float g, float b) {
    Image img;
    img.w = img.h = 64;
    img.rgba.resize(64 * 64 * 4);
    for (int y = 0; y < 64; y++)
        for (int x = 0; x < 64; x++) {
            const float d = std::hypot(x - 31.5f, y - 31.5f) / 30.f;
            const float ring = std::exp(-std::pow((d - 0.86f) / 0.1f, 2.f)), body = d < 0.9f ? 0.35f : 0.f;
            const float a = std::clamp(std::max(ring, body * (1 - std::pow(1 - d, 3.f))), 0.f, 1.f) * (d < 1.05f ? 1.f : 0.f);
            uint8_t* p = &img.rgba[size_t(y * 64 + x) * 4];
            p[0] = uint8_t(r * 255), p[1] = uint8_t(g * 255), p[2] = uint8_t(b * 255), p[3] = uint8_t(a * 255);
        }
    return img;
}

// ---- the message font, as a texture atlas: each glyph 3x, smoothed, with a dark outline ----
struct Text {
    static constexpr int kS = 3, kPad = 2, kCell = (Font::kCell + 2 * kPad) * kS;
    Tex atlas;
    int left[Font::kCount] = {}, width[Font::kCount] = {};

    bool build(const Font& f) {
        const int cols = 16, rows = (Font::kCount + cols - 1) / cols;
        Image img;
        img.w = cols * kCell, img.h = rows * kCell;
        img.rgba.assign(size_t(img.w) * img.h * 4, 0);
        std::vector<float> cov(size_t(kCell) * kCell), soft(cov.size()), ring(cov.size());
        for (int g = 0; g < Font::kCount; g++) {
            int lo = Font::kCell, hi = -1;
            std::fill(cov.begin(), cov.end(), 0.f);
            for (int y = 0; y < Font::kCell; y++)
                for (int x = 0; x < Font::kCell; x++)
                    if (f.pixel(g, x, y)) {
                        lo = std::min(lo, x), hi = std::max(hi, x);
                        for (int sy = 0; sy < kS; sy++)
                            for (int sx = 0; sx < kS; sx++) cov[size_t((y + kPad) * kS + sy) * kCell + (x + kPad) * kS + sx] = 1.f;
                    }
            left[g] = hi < 0 ? 0 : lo;
            width[g] = hi < 0 ? 8 : hi - lo + 1;
            for (int y = 0; y < kCell; y++)  // round the blocky 3x pixels off a little
                for (int x = 0; x < kCell; x++) {
                    float s = 0;
                    int n = 0;
                    for (int dy = -1; dy <= 1; dy++)
                        for (int dx = -1; dx <= 1; dx++) {
                            const int sx = x + dx, sy = y + dy;
                            if (sx >= 0 && sy >= 0 && sx < kCell && sy < kCell) s += cov[size_t(sy) * kCell + sx], n++;
                        }
                    soft[size_t(y) * kCell + x] = s / float(n);
                }
            for (int y = 0; y < kCell; y++)
                for (int x = 0; x < kCell; x++) {
                    float v = 0;
                    for (int dy = -4; dy <= 4; dy++)
                        for (int dx = -4; dx <= 4; dx++) {
                            if (dx * dx + dy * dy > 18) continue;
                            const int sx = x + dx, sy = y + dy;
                            if (sx >= 0 && sy >= 0 && sx < kCell && sy < kCell) v = std::max(v, soft[size_t(sy) * kCell + sx]);
                        }
                    ring[size_t(y) * kCell + x] = v;
                }
            const int ox = (g % cols) * kCell, oy = (g / cols) * kCell;
            for (int y = 0; y < kCell; y++)
                for (int x = 0; x < kCell; x++) {
                    const float fg = soft[size_t(y) * kCell + x], ol = ring[size_t(y) * kCell + x];
                    const float a = fg + ol * 0.85f * (1 - fg);
                    const float c = a > 0 ? fg / a : 0;
                    uint8_t* p = &img.rgba[(size_t(oy + y) * img.w + ox + x) * 4];
                    p[0] = p[1] = p[2] = uint8_t(c * 255), p[3] = uint8_t(std::clamp(a, 0.f, 1.f) * 255);
                }
        }
        atlas = makeTex(img);
        return bool(atlas);
    }

    static std::vector<int> glyphs(const std::string& s) {  // UTF-8 -> glyph indices (Windows-1252)
        std::vector<int> out;
        for (size_t i = 0; i < s.size();) {
            uint32_t c = uint8_t(s[i]);
            int n = c < 0x80 ? 1 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4;
            if (n > 1) {
                c &= 0x3F >> (n - 1);
                for (int k = 1; k < n && i + k < s.size(); k++) c = c << 6 | (uint8_t(s[i + k]) & 0x3F);
            }
            i += size_t(n);
            static const std::map<uint32_t, uint32_t> kMap = {{0x20AC, 0x80}, {0x2026, 0x85}, {0x2018, 0x91}, {0x2019, 0x92},
                                                              {0x201C, 0x93}, {0x201D, 0x94}, {0x2013, 0x96}, {0x2014, 0x97}};
            if (auto it = kMap.find(c); it != kMap.end()) c = it->second;
            out.push_back(c >= 0x20 && c <= 0xFF ? int(c - 0x20) : '?' - 0x20);
        }
        return out;
    }
    float advance(int g) const { return float(width[g] + 2); }
    float measure(const std::string& s, float scale) const {
        float w = 0;
        for (int g : glyphs(s)) w += advance(g);
        return w * scale;
    }
    // align: 0 left, 1 centre, 2 right; y is the top of the 24-pixel line
    void draw(const std::string& s, float x, float y, float scale, SDL_FColor col, int align = 0) const {
        if (!atlas) return;
        if (align) x -= measure(s, scale) * (align == 1 ? 0.5f : 1.f);
        for (int g : glyphs(s)) {
            const float sx = float((g % 16) * kCell + (left[g] + kPad - 1) * kS), sy = float((g / 16) * kCell);
            const float cw = float((width[g] + 2) * kS);
            const SDL_FRect src{sx, sy, cw, float(kCell)};
            const SDL_FRect dst{x - scale, y - kPad * scale, cw / kS * scale, float(kCell) / kS * scale};
            launcher::draw(atlas, src, dst, col.r, col.g, col.b, col.a);
            x += advance(g) * scale;
        }
    }
    // Word-wrapped to a width, at most maxLines (the last cut short with "..."); returns the lines' count.
    int drawWrapped(const std::string& s, float x, float y, float w, float scale, SDL_FColor col, int align = 0, float lineH = 30,
                    size_t maxLines = 99) const {
        std::vector<std::string> lines;
        std::string line, word;
        auto flushWord = [&] {
            const std::string t = line.empty() ? word : line + " " + word;
            if (!line.empty() && measure(t, scale) > w) lines.push_back(line), line = word;
            else line = t;
            word.clear();
        };
        for (char ch : s) {
            if (ch == ' ') flushWord();
            else if (ch == '\n') flushWord(), lines.push_back(line), line.clear();
            else word += ch;
        }
        flushWord();
        if (!line.empty()) lines.push_back(line);
        if (lines.size() > maxLines) {
            lines.resize(maxLines);
            std::string& last = lines.back();
            while (!last.empty() && measure(last + "...", scale) > w) last.pop_back();
            last += "...";
        }
        for (size_t i = 0; i < lines.size(); i++)
            draw(lines[i], align == 1 ? x + w / 2 : align == 2 ? x + w : x, y + float(i) * lineH * scale, scale, col, align);
        return int(lines.size());
    }
};

// ---- the game process ----
struct GameProcess {
#ifdef _WIN32
    HANDLE proc = nullptr;
#endif
    bool running() {
#ifdef _WIN32
        return proc && WaitForSingleObject(proc, 0) == WAIT_TIMEOUT;
#else
        return false;
#endif
    }
    // Starts exe with args in dir; false with a reason.
    bool start(const fs::path& exe, const std::vector<std::string>& args, const fs::path& dir, std::string& error) {
#ifdef _WIN32
        auto wide = [](const std::string& s) {
            std::wstring w(size_t(MultiByteToWideChar(CP_UTF8, 0, s.c_str(), int(s.size()), nullptr, 0)), L'\0');
            MultiByteToWideChar(CP_UTF8, 0, s.c_str(), int(s.size()), w.data(), int(w.size()));
            return w;
        };
        std::wstring cmd = L"\"" + exe.wstring() + L"\"";
        for (const std::string& a : args) cmd += L" \"" + wide(a) + L"\"";
        STARTUPINFOW si{sizeof si};
        PROCESS_INFORMATION pi{};
        if (!CreateProcessW(exe.wstring().c_str(), cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, dir.wstring().c_str(), &si, &pi))
            return error = "couldn't start " + exe.filename().string() + " (error " + std::to_string(GetLastError()) + ")", false;
        CloseHandle(pi.hThread);
        if (proc) CloseHandle(proc);
        proc = pi.hProcess;
        return true;
#else
        (void)exe, (void)args, (void)dir;
        return error = "starting the game is only written for Windows so far", false;
#endif
    }
    // After it ended: its exit code.
    unsigned long exitCode() {
#ifdef _WIN32
        DWORD c = 0;
        if (proc) GetExitCodeProcess(proc, &c);
        return c;
#else
        return 0;
#endif
    }
};

// ---- mods: folders in mods/ with a mod.ini; the ones turned on are listed in mods/enabled.txt ----
struct Mod {
    std::string folder, name, author, description;
    bool on = false;
};
std::vector<Mod> scanMods(const fs::path& dir) {
    std::vector<Mod> out;
    std::error_code ec;
    std::vector<std::string> enabled;
    {
        std::ifstream f(dir / "enabled.txt");
        std::string l;
        while (std::getline(f, l))
            if (!l.empty()) enabled.push_back(l.back() == '\r' ? l.substr(0, l.size() - 1) : l);
    }
    for (const auto& e : fs::directory_iterator(dir, ec)) {
        if (!e.is_directory() || !fs::exists(e.path() / "mod.ini", ec)) continue;
        Mod m;
        m.folder = e.path().filename().string();
        m.name = m.folder;
        std::ifstream f(e.path() / "mod.ini");
        std::string l;
        while (std::getline(f, l)) {
            if (!l.empty() && l.back() == '\r') l.pop_back();
            const size_t eq = l.find('=');
            if (eq == std::string::npos) continue;
            std::string k = l.substr(0, eq);
            for (char& c : k) c = char(std::tolower(uint8_t(c)));
            if (k == "name") m.name = l.substr(eq + 1);
            else if (k == "author") m.author = l.substr(eq + 1);
            else if (k == "description") m.description = l.substr(eq + 1);
        }
        m.on = std::find(enabled.begin(), enabled.end(), m.folder) != enabled.end();
        out.push_back(m);
    }
    std::sort(out.begin(), out.end(), [](const Mod& a, const Mod& b) { return a.folder < b.folder; });
    return out;
}
void saveMods(const fs::path& dir, const std::vector<Mod>& mods) {
    std::ofstream f(dir / "enabled.txt");
    for (const Mod& m : mods)
        if (m.on) f << m.folder << "\n";
}

// ---- HD texture builder status (tools/hd/build_pack.py writes <pack>/status.txt) ----
struct HdStatus {
    std::string state;  // "", running, paused, finished, failed
    long done = 0, total = 0, eta = 0;
    std::string note;
    bool fresh = false;  // written in the last 15 s (a builder is alive)
};
HdStatus readHd(const fs::path& pack) {
    HdStatus s;
    std::error_code ec;
    std::ifstream f(pack / "status.txt");
    if (!f) return s;
    f >> s.state >> s.done >> s.total >> s.eta;
    std::getline(f, s.note);
    if (!s.note.empty() && s.note[0] == ' ') s.note.erase(0, 1);
    const auto t = fs::last_write_time(pack / "status.txt", ec);
    s.fresh = !ec && fs::file_time_type::clock::now() - t < std::chrono::seconds(15);
    return s;
}

std::string fmtTime(long s) {
    char b[32];
    if (s >= 3600) std::snprintf(b, sizeof b, "%ld h %02ld min", s / 3600, s / 60 % 60);
    else std::snprintf(b, sizeof b, "%ld min", std::max(1L, s / 60));
    return b;
}
std::string fmtClock(float seconds) {  // time so far, m:ss
    char b[32];
    const long s = long(seconds);
    std::snprintf(b, sizeof b, "%ld:%02ld", s / 60, s % 60);
    return b;
}

// ---- the app ----
enum class Screen { Main, Settings, Hd, Mods, Setup };

struct Bokeh {
    float x, y, vx, vy, size, phase;
    int kind;
};

struct App {
    SDL_Window* win = nullptr;
    fs::path exeDir, dataDir;
    Config cfg;
    Disc disc;
    Font font;
    Text text;
    std::string discError;
    // textures: from the disc (cleared when it changes) and our own
    Tex pano, logo, bars, bokeh[3];
    Tex ownBokeh[3];
    std::map<std::string, Tex> labels;
    std::vector<Bokeh> lights;
    Screen screen = Screen::Main;
    int sel = 0;
    float t = 0, screenT = 0;  // time, time on this screen
    float selGlow[16] = {};
    std::string status;  // a message in the footer (launch errors, ...)
    float statusT = -100;
    GameProcess game;
    bool gameRunning = false;
    std::vector<Mod> mods;
    GameProcess hdJob, setupJob;
    Setup build;             // the game's code from the disc (SETUP page)
    bool needBuild = false;  // Setup::needed for the current disc
    std::mutex dialogLock;
    std::string dialogResult;
    bool dialogDone = false;
    float mouseX = -1, mouseY = -1;
    bool quit = false;

    fs::path packDir() const { return dataDir / "textures" / "GSNE8P"; }

    void say(const std::string& s) { status = s, statusT = t; }

    void loadDisc() {
        for (Tex* x : {&pano, &logo, &bars, &bokeh[0], &bokeh[1], &bokeh[2]}) freeTex(*x);
        freeTex(text.atlas);
        discError.clear();
        if (cfg.disc.empty()) return;
        std::string err;
        if (!disc.open(pathU8(cfg.disc), err)) {
            discError = "wrongdisc";
            say("That disc image can't be used: " + err + ".");
            return;
        }
        if (disc.gameId() != "GSNE8P") {
            discError = "wrongdisc";
            return;
        }
        Bytes raw;
        if (disc.read("alphaTitle_e.prs", raw)) {
            std::vector<Image> t = gvrTextures(prsDecompress(raw));
            if (t.size() >= 12) {
                // the title's art, as its screen draws it: a 640-wide column of two 640x512 halves
                // one above the other (each a 512x512 and, to its right, two 128x256 strips),
                // scrolling upwards round and round
                Image p;
                p.w = 640, p.h = 1024;
                p.rgba.assign(size_t(p.w) * p.h * 4, 0);
                auto blit = [&](const Image& s, int ox, int oy) {
                    for (int y = 0; y < s.h && oy + y < p.h; y++)
                        std::memcpy(&p.rgba[(size_t(oy + y) * p.w + ox) * 4], &s.rgba[size_t(y) * s.w * 4], size_t(std::min(s.w, p.w - ox)) * 4);
                };
                blit(t[3], 0, 0), blit(t[4], 512, 0), blit(t[5], 512, 256);
                blit(t[6], 0, 512), blit(t[7], 512, 512), blit(t[8], 512, 768);
                pano = makeTex(p);
                logo = makeTex(t[2]);
                for (int k = 0; k < 3; k++) bokeh[k] = makeTex(t[9 + k]);
            }
        }
        if (disc.read("newMenu_e.prs", raw)) {
            std::vector<Image> t = gvrTextures(prsDecompress(raw));
            if (t.size() > 6) bars = makeTex(t[6]);
        }
        if (font.load(disc)) text.build(font);
        needBuild = Setup::needed(exeDir, pathU8(cfg.disc));
    }
    bool discOk() const { return disc.isOpen() && discError.empty() && text.atlas; }

    // ---- menus ----
    struct Item {
        std::string label;                     // a label image id, or "" for a text row
        std::function<std::string()> name;     // text rows: the setting's name
        std::function<std::string()> value;    // text rows: its value
        std::function<void(int)> change;       // left/right
        std::function<void()> activate;        // A / Enter / click
        std::function<std::string()> help;
    };
    std::vector<Item> items() {
        std::vector<Item> v;
        auto label = [&](const char* id, std::function<void()> act, std::string help) {
            v.push_back({id, nullptr, nullptr, nullptr, act, [help] { return help; }});
        };
        if (screen == Screen::Main) {
            if (!discOk()) {
                label("choosedisc", [this] { chooseDisc(); }, "");
                label("quit", [this] { quit = true; }, "");
                return v;
            }
            label("play", [this] { launch(); },
                  needBuild ? "Start the game. First the game is built from your disc, once: about 10 to 20 minutes." : "Start the game.");
            label("settings", [this] { go(Screen::Settings); }, "Display, frame rate, sound and disc options.");
            label("hdtex", [this] { go(Screen::Hd); }, "Make high-resolution textures from your disc with an upscaling model.");
            if (kMods) label("mods", [this] { mods = scanMods(dataDir / "mods"), go(Screen::Mods); }, "Turn installed mods on or off.");
            label("quit", [this] { quit = true; }, "Close the launcher.");
        } else if (screen == Screen::Settings) {
            auto row = [&](std::string n, std::function<std::string()> val, std::function<void(int)> ch, std::string help, std::function<void()> act = nullptr) {
                v.push_back({"", [n] { return n; }, val, ch, act ? act : std::function<void()>([ch] { ch(1); }), [help] { return help; }});
            };
            row("Display", [this] { return cfg.fullscreen ? "Full screen" : "Window"; }, [this](int) { cfg.fullscreen = !cfg.fullscreen; },
                "Play in a window or on the whole screen.");
            row("Window size", [this] { return std::to_string(cfg.winW) + " x " + std::to_string(cfg.winH); },
                [this](int d) {
                    int i = 0;
                    for (int k = 0; k < 5; k++)
                        if (kSizes[k][0] == cfg.winW && kSizes[k][1] == cfg.winH) i = k;
                    i = (i + d + 5) % 5;
                    cfg.winW = kSizes[i][0], cfg.winH = kSizes[i][1];
                },
                "The window's size. Full screen always uses your display's resolution.");
            row("Aspect ratio", [this] { return cfg.widescreen ? "Widescreen 16:9" : "Original 4:3"; }, [this](int) { cfg.widescreen = !cfg.widescreen; },
                "Widescreen shows more of the world to the sides; 4:3 is the GameCube's picture.");
            row("Frame rate", [this] { return cfg.fixed60 ? "60 fps" : "Match display"; }, [this](int) { cfg.fixed60 = !cfg.fixed60; },
                "Match display draws in-between frames on 120 and 144 Hz screens. 60 fps shows the game's own frames only.");
            row("Volume", [this] { return std::to_string(cfg.volume) + "%"; }, [this](int d) { cfg.volume = std::clamp(cfg.volume + 10 * d, 0, 100); },
                "How loud the game plays.");
            row("Disc loading", [this] { return cfg.fastDisc ? "Instant" : "Like the console"; }, [this](int) { cfg.fastDisc = !cfg.fastDisc; },
                "Like the console keeps the original loading times. Instant loads at once.");
            row("HD textures", [this] { return !packReady() ? "Not made yet" : cfg.hdTextures ? "On" : "Off"; },
                [this](int) {
                    if (packReady()) cfg.hdTextures = !cfg.hdTextures;
                },
                "Use the high-resolution textures made on the HD TEXTURES page.");
            row("Disc", [this] { return strU8(pathU8(cfg.disc).filename()); }, [this](int) { chooseDisc(); },
                "The disc image the game is played from. Press to choose another.", [this] { chooseDisc(); });
            label("back", [this] { cfg.save(dataDir / "launcher.ini"), go(Screen::Main, 1); }, "Back to the main menu. Settings are saved.");
        } else if (screen == Screen::Mods) {
            for (size_t i = 0; i < mods.size(); i++) {
                v.push_back({"", [this, i] { return mods[i].name; }, [this, i] { return mods[i].on ? "On" : "Off"; },
                             [this, i](int) { mods[i].on = !mods[i].on, saveMods(dataDir / "mods", mods); }, nullptr,
                             [this, i] {
                                 const Mod& m = mods[i];
                                 return (m.author.empty() ? "" : "By " + m.author + ". ") + m.description;
                             }});
                v.back().activate = [c = v.back().change] { c(1); };
            }
            label("back", [this] { go(Screen::Main, 3); }, "Back to the main menu.");
        } else if (screen == Screen::Hd) {
            const HdStatus s = readHd(packDir());
            const bool alive = hdJob.running() || (s.fresh && (s.state == "running" || s.state == "paused"));
            if (!upscalerReady()) {  // first: the upscaler itself (tools/hd/setup.ps1)
                if (!setupJob.running())
                    v.push_back({"", [] { return std::string("Install the upscaler"); }, [] { return std::string(""); }, nullptr, [this] { startSetup(); },
                                 [] { return std::string("Downloads Python, PyTorch and the HDcube model into your AppData folder (about 3 GB with an NVIDIA card)."); }});
            } else if (!alive && s.state != "finished") {
                v.push_back({"", [] { return std::string("Make HD textures"); }, [] { return std::string(""); }, nullptr, [this] { startHd(); },
                             [] { return std::string("Upscales every texture on your disc. It works in the background, even while you play; the game uses them once all are done."); }});
            } else if (alive) {
                const bool paused = s.state == "paused";
                v.push_back({"", [paused] { return std::string(paused ? "Resume" : "Pause"); }, [] { return std::string(""); }, nullptr,
                             [this, paused] { setHdPause(!paused); },
                             [] { return std::string("Pausing frees your computer; the work done so far is kept."); }});
            }
            if (packReady())
                v.push_back({"", [] { return std::string("Use them"); }, [this] { return std::string(cfg.hdTextures ? "On" : "Off"); },
                             [this](int) { cfg.hdTextures = !cfg.hdTextures, cfg.save(dataDir / "launcher.ini"); },
                             [this] { cfg.hdTextures = !cfg.hdTextures, cfg.save(dataDir / "launcher.ini"); },
                             [] { return std::string("Play with the HD textures or the originals."); }});
            label("back", [this] { go(Screen::Main, 2); }, "Back to the main menu.");
        } else if (screen == Screen::Setup) {
            const Setup::Status s = build.status();
            if (s.running) {
                label("cancel", [this] { build.cancel(); }, "Stop building. Nothing is kept: it starts over the next time you press PLAY.");
            } else if (!s.done) {
                label("tryagain", [this] { startBuild(); }, "Start the setup again.");
                label("back", [this] { go(Screen::Main, 0); }, "Back to the main menu.");
            }
        }
        return v;
    }

    bool packReady() const {
        std::error_code ec;
        return fs::exists(packDir() / "pack.txt", ec);
    }

    void go(Screen s, int select = 0) {
        screen = s, sel = select, screenT = 0;
        std::fill(std::begin(selGlow), std::end(selGlow), 0.f);
    }

    void chooseDisc() {
        static const SDL_DialogFileFilter filters[] = {{"GameCube disc image", "iso;gcm"}, {"All files", "*"}};
        SDL_ShowOpenFileDialog(
            [](void* ud, const char* const* list, int) {
                App* a = static_cast<App*>(ud);
                std::lock_guard g(a->dialogLock);
                a->dialogResult = list && list[0] ? list[0] : "";
                a->dialogDone = true;
            },
            this, win, filters, 2, cfg.disc.empty() ? nullptr : cfg.disc.c_str(), false);
    }

    void startBuild() {
        go(Screen::Setup);
        build.start(exeDir, pathU8(cfg.disc), dataDir / "setup.log");
    }
    // Each frame on the SETUP page: once the build is done, the game starts.
    void pollBuild() {
        if (screen != Screen::Setup || build.running()) return;
        const Setup::Status s = build.status();
        if (!s.done) return;
        needBuild = Setup::needed(exeDir, pathU8(cfg.disc));
        go(Screen::Main, 0);
        if (!needBuild) launch();
    }

    void launch() {
        const fs::path exe = exeDir / "sa2b.exe";
        std::error_code ec;
        if (!fs::exists(exe, ec)) return say("sa2b.exe isn't next to the launcher.");
        needBuild = Setup::needed(exeDir, pathU8(cfg.disc));
        if (needBuild) return startBuild();
        std::vector<std::string> a = {"--iso", cfg.disc, "--volume", std::to_string(cfg.volume)};
        if (cfg.fullscreen) a.push_back("--fullscreen");
        else a.push_back("--window"), a.push_back(std::to_string(cfg.winW) + "x" + std::to_string(cfg.winH));
        if (!cfg.widescreen) a.push_back("--no-widescreen");
        if (cfg.fixed60) a.push_back("--no-interp");
        if (cfg.fastDisc) a.push_back("--fast-disc");
        if (cfg.hdTextures && packReady()) a.push_back("--hd-textures"), a.push_back(strU8(packDir()));
        if (kMods && std::any_of(mods.begin(), mods.end(), [](const Mod& m) { return m.on; })) a.push_back("--mods");
        cfg.save(dataDir / "launcher.ini");
        std::string err;
        if (!game.start(exe, a, dataDir, err)) return say("The game didn't start: " + err + ".");
        gameRunning = true;
        SDL_HideWindow(win);
    }

    // The upscaler: Python with PyTorch and the HDcube model, which tools/hd/setup.ps1 installs in
    // %LOCALAPPDATA%\SA2BRecompiled\upscaler (a short path: PyTorch's deep folders) - or, building
    // from source, a venv and models in tools/hd.
    fs::path hdDir() const { return dataDir / "tools" / "hd"; }
    static fs::path upscalerHome() {
        const char* la = std::getenv("LOCALAPPDATA");
        return la ? pathU8(la) / "SA2BRecompiled" / "upscaler" : fs::path();
    }
    fs::path python() const {
        std::error_code ec;
        const fs::path own = upscalerHome() / "python" / "python.exe";
        return !upscalerHome().empty() && fs::exists(own, ec) ? own : hdDir() / "venv" / "Scripts" / "python.exe";
    }
    fs::path models() const {
        std::error_code ec;
        const fs::path own = upscalerHome() / "models" / "Models" / "HDcube";
        return !upscalerHome().empty() && fs::exists(own, ec) ? own : hdDir() / "models" / "Models" / "HDcube";
    }
    bool upscalerReady() const {
        std::error_code ec;
        return fs::exists(python(), ec) && fs::exists(hdDir() / "build_pack.py", ec) && fs::exists(models() / "4xHDCube4Compact.pth", ec) &&
               fs::exists(models() / "4xHDcube-Alpha.pth", ec);
    }
    void startSetup() {
        std::error_code ec;
        const fs::path script = hdDir() / "setup.ps1";
        if (!fs::exists(script, ec)) return say("tools/hd/setup.ps1 is missing.");
        const char* root = std::getenv("SystemRoot");
        const fs::path ps = fs::path(root ? root : "C:/Windows") / "System32" / "WindowsPowerShell" / "v1.0" / "powershell.exe";
        fs::remove(hdDir() / "setup_status.txt", ec);
        std::string err;
        if (!setupJob.start(ps, {"-NoProfile", "-ExecutionPolicy", "Bypass", "-File", strU8(script)}, dataDir, err))
            say("The upscaler's setup didn't start: " + err + ".");
    }

    void startHd() {
        const fs::path py = python(), script = hdDir() / "build_pack.py";
        std::error_code ec;
        if (!upscalerReady()) return say("The upscaler isn't installed yet.");
        fs::create_directories(packDir(), ec);
        fs::remove(packDir() / "pause", ec);
        std::string err;
        if (!hdJob.start(py,
                         {strU8(script), "--iso", cfg.disc, "--out", strU8(packDir()), "--model", strU8(models() / "4xHDCube4Compact.pth"),
                          "--alpha-model", strU8(models() / "4xHDcube-Alpha.pth")},
                         dataDir, err))
            say("The texture builder didn't start: " + err + ".");
    }
    void setHdPause(bool pause) {
        std::error_code ec;
        if (pause) std::ofstream(packDir() / "pause") << "1";
        else fs::remove(packDir() / "pause", ec);
    }

    // ---- input ----
    void move(int d) {
        const int n = int(items().size());
        if (n) sel = (sel + d + n) % n;
    }
    void left_right(int d) {
        if (screen == Screen::Setup) return move(d);  // (its buttons are side by side)
        auto v = items();
        if (sel < int(v.size()) && v[size_t(sel)].change) v[size_t(sel)].change(d);
    }
    void accept() {
        auto v = items();
        if (sel < int(v.size()) && v[size_t(sel)].activate) v[size_t(sel)].activate();
    }
    void cancel() {
        if (screen == Screen::Settings) cfg.save(dataDir / "launcher.ini"), go(Screen::Main, 1);
        else if (screen == Screen::Hd) go(Screen::Main, 2);
        else if (screen == Screen::Mods) go(Screen::Main, 3);
        else if (screen == Screen::Setup) {
            if (build.running()) build.cancel();
            else go(Screen::Main, 0);
        }
    }

    // Where each item is drawn (layout space), for drawing and the mouse.
    std::vector<SDL_FRect> layout() {
        const auto v = items();
        std::vector<SDL_FRect> r;
        if (screen == Screen::Main) {
            const float x = discOk() ? 700 : 400, w = 480, h = 66, step = 84;
            const float y0 = (discOk() ? 380 : 330) - float(v.size()) * step / 2;
            for (size_t i = 0; i < v.size(); i++) r.push_back({x, y0 + float(i) * step, w, h});
        } else if (screen == Screen::Setup) {  // buttons side by side under the progress
            const float w = 340, h = 56, gap = 30, total = float(v.size()) * w + float(v.size() > 0 ? v.size() - 1 : 0) * gap;
            for (size_t i = 0; i < v.size(); i++) r.push_back({(kW - total) / 2 + float(i) * (w + gap), 536, w, h});
        } else {
            const float x = 240, w = 800, step = 49, y0 = screen == Screen::Hd ? 330.f : 130.f;
            for (size_t i = 0; i < v.size(); i++) {
                const bool lab = !v[i].label.empty();
                r.push_back({lab ? x + w / 2 - 170 : x, y0 + float(i) * step + (lab ? 12 : 0), lab ? 340 : w, lab ? 56.f : 44.f});
            }
        }
        return r;
    }

    // ---- frame ----
    void drawBackground(float dt) {
        fill({0, 0, kW, kH}, {0.01f, 0.01f, 0.04f, 1});
        if (pano) {  // the menus' darkened title art: the full width, scrolling up 120 units a second
            const float sc = kW / pano.w, ph = pano.h * sc;
            const float y = -std::fmod(t * 120.f * sc, ph);
            for (float yy = y; yy < kH; yy += ph) draw(pano, {0, yy, kW, ph}, 0.08f, 0.08f, 0.08f, 1);  // as dark as the main menu shows it
        }
        gradient({0, kH - 160, kW, 160}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0.45f}, {0, 0, 0, 0.45f});
        for (Bokeh& b : lights) {
            b.x += b.vx * dt, b.y += b.vy * dt;
            if (b.y < -b.size) b.y = kH + b.size, b.x = float(std::rand() % int(kW));
            if (b.x < -b.size) b.x = kW + b.size;
            if (b.x > kW + b.size) b.x = -b.size;
            const Tex& tx = bokeh[b.kind] ? bokeh[b.kind] : ownBokeh[b.kind];
            const float a = 0.35f + 0.25f * std::sin(b.phase + t * 0.8f);
            draw(tx, {b.x - b.size / 2, b.y - b.size / 2, b.size, b.size}, 1, 1, 1, a, SDL_BLENDMODE_ADD);
        }
    }

    Tex& label(const std::string& id) {
        Tex& tx = labels[id];
        if (!tx) tx = makeTex(labelImage(id.c_str()));
        return tx;
    }

    void drawBar(SDL_FRect r, bool selected, float glow, float appear) {
        const float slide = (1 - appear) * 40;
        r.x += slide;
        const float a = appear;
        if (bars) {
            const float k = selected ? 1.f : 0.78f;
            draw(bars, {0, 0, 256, 41}, r, k, k, k, a);
            if (glow > 0) draw(bars, {0, 0, 256, 41}, r, 0.5f, 0.8f, 1, glow * (0.22f + 0.1f * std::sin(t * 5)) * a, SDL_BLENDMODE_ADD);
        } else {  // no disc yet: a bar of our own
            gradient(r, {0.05f, 0.45f, 0.95f, 0.9f * a}, {0.10f, 0.18f, 0.55f, 0.9f * a}, {0.06f, 0.12f, 0.45f, 0.9f * a}, {0.03f, 0.30f, 0.80f, 0.9f * a});
            SDL_SetRenderDrawColorFloat(g_ren, 0.3f, 0.95f, 1, (selected ? 1.f : 0.5f) * a);
            SDL_RenderRect(g_ren, &r);
        }
    }

    void drawLabelIn(const std::string& id, SDL_FRect r, bool selected, float appear) {
        Tex& tx = label(id);
        if (!tx) return;
        float s = r.h * 0.6f / (tx.h - 52);  // (the image's padding, outline and shadow around the words)
        s = std::min(s, (r.w - 30) / tx.w);
        const float w = tx.w * s, h = tx.h * s, slide = (1 - appear) * 40;
        const SDL_FRect d{r.x + slide + (r.w - w) / 2, r.y + (r.h - h) / 2 + 1, w, h};
        if (selected) draw(tx, d, 1, 1, 1, appear);
        else draw(tx, d, 0.16f, 0.24f, 0.55f, appear);
    }

    void frame(float dt) {
        frameNoPresent(dt);
        SDL_RenderPresent(g_ren);
    }
    void frameNoPresent(float dt) {
        t += dt, screenT += dt;
        SDL_SetRenderDrawColor(g_ren, 0, 0, 0, 255);
        SDL_RenderClear(g_ren);
        drawBackground(dt);

        const auto v = items();
        if (sel >= int(v.size())) sel = std::max(0, int(v.size()) - 1);
        const auto rects = layout();

        // the logo: large beside the main menu, small in the corner elsewhere
        if (logo) {
            static float k = 1;
            const float target = screen == Screen::Main ? 1.f : 0.f;
            k += (target - k) * std::min(1.f, dt * 8);
            const float size = 230 + 300 * k;
            const float x = 40 + (130 - 40) * k - 40 * (1 - k), y = 10 + 90 * k - 20 * (1 - k);
            draw(logo, {x, y, size, size});
        }
        // page titles
        const char* title = screen == Screen::Settings ? "settings"
                            : screen == Screen::Hd     ? "hdtex"
                            : screen == Screen::Mods   ? "mods"
                            : screen == Screen::Setup  ? "setup"
                                                       : nullptr;
        if (title) {
            Tex& tx = label(title);
            const float s = 0.62f, a = std::min(1.f, screenT * 5);
            draw(tx, {kW - 60 - tx.w * s, 52 - (1 - a) * 20, tx.w * s, tx.h * s}, 1, 1, 1, a);
        }

        for (size_t i = 0; i < v.size(); i++) {
            const bool on = int(i) == sel;
            selGlow[i % 16] += ((on ? 1.f : 0.f) - selGlow[i % 16]) * std::min(1.f, dt * 12);
            const float appear = std::clamp((screenT - 0.05f * float(i)) * 6, 0.f, 1.f);
            const float ease = 1 - (1 - appear) * (1 - appear);
            const SDL_FRect r = rects[i];
            if (!v[i].label.empty()) {
                drawBar(r, on, selGlow[i % 16], ease);
                drawLabelIn(v[i].label, r, on, ease);
                continue;
            }
            // a setting: inset bar, name on the left, value with arrows on the right
            SDL_FRect rr = r;
            rr.x += (1 - ease) * 40;
            if (bars) {
                draw(bars, {0, 43, 256, 41}, rr, on ? 1.f : 0.7f, on ? 1.f : 0.7f, on ? 1.f : 0.75f, ease);
                if (on) draw(bars, {0, 0, 256, 41}, rr, 0.6f, 0.85f, 1, selGlow[i % 16] * (0.35f + 0.1f * std::sin(t * 5)) * ease, SDL_BLENDMODE_ADD);
            }
            const SDL_FColor white{1, 1, 1, ease}, dim{0.72f, 0.80f, 1.0f, ease};
            const float ty = rr.y + (rr.h - 24) / 2;
            text.draw(v[i].name ? v[i].name() : "", rr.x + 26, ty, 1.0f, on ? white : dim);
            const std::string val = v[i].value ? v[i].value() : "";
            if (!val.empty()) {
                const bool arrows = bool(v[i].change);
                const float right = rr.x + rr.w - 26;
                if (arrows && on) {
                    const float nudge = 2 * std::sin(t * 6);
                    text.draw(">", right + nudge, ty, 1.0f, {1, 0.85f, 0.3f, ease}, 2);
                    text.draw("<", right - 34 - text.measure(val, 1.0f) - 18 - nudge, ty, 1.0f, {1, 0.85f, 0.3f, ease}, 0);
                }
                text.draw(val, right - (arrows ? 34 : 0), ty, 1.0f, on ? SDL_FColor{1, 0.93f, 0.55f, ease} : dim, 2);
            }
        }

        if (screen == Screen::Hd) drawHdInfo();
        if (screen == Screen::Setup) drawSetupInfo();
        if (screen == Screen::Mods && mods.empty())
            text.drawWrapped("No mods installed. Mods are folders with a mod.ini, put in: " + strU8(dataDir / "mods"), 240, 300, 800, 1.0f, {1, 1, 1, 1}, 1);

        // footer: what the selected item does, or a message
        fill({0, kH - 108, kW, 108}, {0, 0, 0, 0.45f});
        gradient({0, kH - 109, kW, 2}, {0.1f, 0.6f, 1, 0}, {0.1f, 0.9f, 1, 0.8f}, {0.1f, 0.9f, 1, 0.8f}, {0.1f, 0.6f, 1, 0});
        if (!discOk()) {
            Tex& msg = label(discError.empty() ? "nodisc" : discError);
            const float s = std::min(0.5f, (kW - 80) / msg.w);
            draw(msg, {(kW - msg.w * s) / 2, kH - 70, msg.w * s, msg.h * s});
        } else {
            const bool recent = t - statusT < 8;
            const std::string help = recent ? status : sel < int(v.size()) && v[size_t(sel)].help ? v[size_t(sel)].help() : "";
            text.drawWrapped(help, 60, kH - 96, kW - 120, 0.92f, recent ? SDL_FColor{1, 0.85f, 0.4f, 1} : SDL_FColor{1, 1, 1, 1}, 1, 29, 2);
            text.draw("Disc: " + strU8(pathU8(cfg.disc).filename()), kW / 2, kH - 30, 0.7f, {0.6f, 0.68f, 0.85f, 1}, 1);
        }
    }

    // A bar in the menus' style: the inset bar as the track, the lit one filled to k.
    void progressBar(SDL_FRect track, float k, float glow = 0) {
        k = std::clamp(k, 0.f, 1.f);
        if (!bars) return;
        draw(bars, {0, 43, 256, 41}, track, 0.6f, 0.6f, 0.7f, 1);
        draw(bars, {0, 0, 256.f * k, 41}, {track.x, track.y, track.w * k, track.h});
        if (glow > 0) draw(bars, {0, 0, 256.f * k, 41}, {track.x, track.y, track.w * k, track.h}, 0.5f, 0.8f, 1, glow, SDL_BLENDMODE_ADD);
    }

    // The SETUP page: the five steps, the current one's bar and the whole build's.
    void drawSetupInfo() {
        const Setup::Status s = build.status();
        const SDL_FColor white{1, 1, 1, 1}, dim{0.62f, 0.70f, 0.90f, 1}, gold{1, 0.85f, 0.4f, 1}, green{0.55f, 1, 0.6f, 1}, red{1, 0.5f, 0.45f, 1};
        const char* head = s.running ? "Building the game from your disc" : s.done ? "The game is ready" : s.cancelled ? "Cancelled" : "The setup stopped";
        text.draw(head, kW / 2, 112, 1.1f, white, 1);
        text.drawWrapped("No game code comes with this program: the recompiler translates the program on your own disc into native code, "
                         "here on your computer. It is done once.",
                         240, 150, 800, 0.8f, dim, 1, 28, 2);
        // the steps: done, under way (its percentage) or still to come
        for (int i = 0; i < Setup::kSteps; i++) {
            const float y = 222 + float(i) * 32;
            const bool past = s.done || i < s.step, now = i == s.step && !s.done;
            const SDL_FColor c = past ? green : now ? (s.running ? gold : s.cancelled ? dim : red) : dim;
            text.draw(std::to_string(i + 1) + ".  " + Setup::stepName(i), 330, y, 0.9f, c);
            std::string state;
            if (past) state = "Done";
            else if (now && s.running) state = std::to_string(int(s.stepFrac * 100)) + "%";
            else if (now) state = s.cancelled ? "Cancelled" : "Stopped";
            text.draw(state, 950, y, 0.9f, c, 2);
        }
        if (s.failed) {
            text.drawWrapped(s.error, 240, 392, 800, 0.85f, red, 1, 28, 4);
            text.draw("setup.log has the details.", kW / 2, 496, 0.75f, dim, 1);
            return;
        }
        if (!s.running && !s.done) return;
        // this step's bar, with what it is on
        const std::string detail = std::string(Setup::stepName(s.step)) + (s.detail.empty() ? "" : ": " + s.detail);
        text.draw(detail, 290, 388, 0.75f, dim);
        progressBar({290, 414, 700, 16}, s.stepFrac);
        // and the whole build's
        progressBar({290, 442, 700, 30}, s.overall, 0.12f + 0.08f * std::sin(t * 4));
        std::string line = std::to_string(int(s.overall * 100)) + "%   " + fmtClock(s.elapsed);
        if (s.eta >= 0) line += "   about " + fmtTime(long(s.eta)) + " left";
        text.draw(line, kW / 2, 480, 0.8f, white, 1);
        if (!s.note.empty()) text.drawWrapped(s.note, 240, 506, 800, 0.7f, dim, 1, 24, 1);
    }

    void drawHdInfo() {
        if (!upscalerReady()) {  // the setup's progress (setup_status.txt: state, percent, note)
            std::string state, note;
            int pct = 0;
            std::ifstream f(hdDir() / "setup_status.txt");
            f >> state >> pct;
            std::getline(f, note);
            if (!note.empty() && note[0] == ' ') note.erase(0, 1);
            const bool running = setupJob.running();
            std::string line = running ? (note.empty() ? "Starting..." : note)
                             : state == "failed" ? "The setup stopped: " + note + " Try again, or check your connection."
                                                  : "To make HD textures, the upscaler is installed first: a private copy of Python and PyTorch, and the "
                                                    "HDcube model by Venomalia, downloaded from its author.";
            text.drawWrapped(line, 240, 150, 800, 1.0f, {1, 1, 1, 1}, 1, 30, 3);
            if (running) {
                const SDL_FRect track{290, 250, 700, 28};
                if (bars) draw(bars, {0, 43, 256, 41}, track, 0.6f, 0.6f, 0.7f, 1);
                const float k = std::clamp(pct / 100.f, 0.f, 1.f);
                if (bars) draw(bars, {0, 0, 256.f * k, 41}, {track.x, track.y, track.w * k, track.h});
            }
            return;
        }
        const HdStatus s = readHd(packDir());
        const bool alive = hdJob.running() || (s.fresh && (s.state == "running" || s.state == "paused"));
        std::string line;
        if (s.state == "finished" || (packReady() && !alive)) line = "Your HD textures are ready: " + std::to_string(s.total) + " textures, upscaled 4x.";
        else if (alive && s.total) {
            line = std::string(s.state == "paused" ? "Paused" : "Upscaling") + ": " + std::to_string(s.done) + " of " + std::to_string(s.total) + " textures";
            if (s.state != "paused" && s.eta > 0) line += ", about " + fmtTime(s.eta) + " left";
            line += ".";
        } else if (alive) line = s.note.empty() ? "Starting..." : s.note;
        else if (s.state == "failed") line = "The last build stopped: " + s.note;
        else line = "Not made yet. Every texture is upscaled 4x with the HDCube model: about 10 to 20 minutes with a graphics card, a few hours on the processor alone. It takes about 3 GB.";
        const float y = 150;
        text.drawWrapped(line, 240, y, 800, 1.0f, {1, 1, 1, 1}, 1, 30, 3);
        if (alive && s.total) {
            const SDL_FRect track{290, y + 100, 700, 28};
            if (bars) draw(bars, {0, 43, 256, 41}, track, 0.6f, 0.6f, 0.7f, 1);
            const float k = float(s.done) / float(s.total);
            if (bars) draw(bars, {0, 0, 256.f * k, 41}, {track.x, track.y, track.w * k, track.h});
        }
    }

    void init() {
        for (int k = 0; k < 3; k++) {
            static const float c[3][3] = {{0.3f, 0.9f, 1}, {1, 0.35f, 0.55f}, {0.45f, 0.5f, 1}};
            ownBokeh[k] = makeTex(softRing(c[k][0], c[k][1], c[k][2]));
        }
        std::mt19937 rng(1234);
        std::uniform_real_distribution<float> u(0, 1);
        for (int i = 0; i < 34; i++)
            lights.push_back({u(rng) * kW, u(rng) * kH, (u(rng) - 0.5f) * 14, -6 - u(rng) * 16, 30 + u(rng) * u(rng) * 150, u(rng) * 6.28f, int(u(rng) * 3) % 3});
        cfg.load(dataDir / "launcher.ini");
        mods = scanMods(dataDir / "mods");
        loadDisc();
    }

    void pollDialog() {
        std::string picked;
        {
            std::lock_guard g(dialogLock);
            if (!dialogDone) return;
            dialogDone = false;
            picked = dialogResult;
        }
        if (picked.empty()) return;
        cfg.disc = picked;
        cfg.save(dataDir / "launcher.ini");
        loadDisc();
        if (discOk()) go(Screen::Main, 0);
    }
};

fs::path findDataDir(const fs::path& exeDir) {
    std::error_code ec;
    for (const fs::path& d : {fs::current_path(ec), exeDir, exeDir.parent_path().parent_path()})
        if (!d.empty() && (fs::exists(d / "launcher.ini", ec) || fs::is_directory(d / "saves", ec))) return d;
    return exeDir;
}

}  // namespace
}  // namespace launcher

int main(int argc, char** argv) {
    using namespace launcher;
    // --shot <file.bmp> [<page: main|settings|hd|mods>] (checking the look): draws that page in a
    // hidden window for two seconds, saves it and exits.
    // --build-only: runs the setup (building sa2b_game.dll from the chosen disc) in a hidden window and
    // exits, 0 when the module was built.
    const char* shot = nullptr;
    std::string shotPage = "main";
    bool buildOnly = false;
    for (int i = 1; i < argc; i++)
        if (!std::strcmp(argv[i], "--shot") && i + 1 < argc) {
            shot = argv[++i];
            if (i + 1 < argc && argv[i + 1][0] != '-') shotPage = argv[++i];
        } else if (!std::strcmp(argv[i], "--build-only")) {
            buildOnly = true;
        }
    SDL_SetHint(SDL_HINT_WINDOWS_INTRESOURCE_ICON, "1");  // the icon in the program (res/sa2b.rc)
    SDL_SetHint(SDL_HINT_WINDOWS_INTRESOURCE_ICON_SMALL, "1");
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) return 1;
    static App app;
    app.exeDir = pathU8(SDL_GetBasePath() ? SDL_GetBasePath() : ".");
    app.dataDir = findDataDir(app.exeDir);
    int w = 1280, h = 720;
    if (const SDL_DisplayMode* dm = SDL_GetCurrentDisplayMode(SDL_GetPrimaryDisplay()); dm && dm->h >= 1080) w = 1600, h = 900;
    if (shot) w = 1280, h = 720;
    if (!SDL_CreateWindowAndRenderer("Sonic Adventure 2: Battle", w, h,
                                     SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY | (shot || buildOnly ? SDL_WINDOW_HIDDEN : 0), &app.win, &g_ren))
        return 1;
    SDL_SetRenderVSync(g_ren, 1);
    SDL_SetRenderLogicalPresentation(g_ren, int(kW), int(kH), SDL_LOGICAL_PRESENTATION_LETTERBOX);
    app.init();
    if (buildOnly) {
        if (!app.discOk()) return 2;
        app.startBuild();
        while (app.build.running()) {
            app.frame(1 / 60.f);
            SDL_Delay(16);
        }
        const bool built = app.build.status().done;
        SDL_Quit();
        return built ? 0 : 1;
    }
    if (shot) {
        app.go(shotPage == "settings" ? Screen::Settings : shotPage == "hd" ? Screen::Hd : shotPage == "mods" ? Screen::Mods : Screen::Main);
        if (shotPage == "setup") {  // a real build, taken a third of the way into compiling (then stopped)
            app.startBuild();
            for (;;) {
                const Setup::Status s = app.build.status();
                if (!s.running || (s.step == Setup::Compile && s.stepFrac > 0.33f)) break;
                app.frame(1 / 60.f);
                SDL_Delay(16);
            }
        }
        for (int i = 0; i < 120; i++) {
            app.frame(1 / 60.f);
        }
        SDL_SetRenderLogicalPresentation(g_ren, int(kW), int(kH), SDL_LOGICAL_PRESENTATION_LETTERBOX);
        app.frameNoPresent(1 / 60.f);
        if (SDL_Surface* s = SDL_RenderReadPixels(g_ren, nullptr)) {
            SDL_SaveBMP(s, shot);
            SDL_DestroySurface(s);
        }
        SDL_Quit();
        return 0;
    }

    Uint64 last = SDL_GetTicksNS();
    float stickRepeat = 0;
    int stickDir = 0;
    bool stickFirst = true;
    while (!app.quit) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (app.gameRunning) continue;
            SDL_ConvertEventToRenderCoordinates(g_ren, &e);
            switch (e.type) {
            case SDL_EVENT_QUIT: app.quit = true; break;
            case SDL_EVENT_KEY_DOWN:
                switch (e.key.key) {
                case SDLK_UP: app.move(-1); break;
                case SDLK_DOWN: app.move(1); break;
                case SDLK_LEFT: app.left_right(-1); break;
                case SDLK_RIGHT: app.left_right(1); break;
                case SDLK_RETURN:
                case SDLK_KP_ENTER:
                case SDLK_SPACE:
                    if (!e.key.repeat) app.accept();
                    break;
                case SDLK_ESCAPE:
                case SDLK_BACKSPACE:
                    if (!e.key.repeat) app.cancel();
                    break;
                default: break;
                }
                break;
            case SDL_EVENT_GAMEPAD_ADDED: SDL_OpenGamepad(e.gdevice.which); break;
            case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
                switch (e.gbutton.button) {
                case SDL_GAMEPAD_BUTTON_DPAD_UP: app.move(-1); break;
                case SDL_GAMEPAD_BUTTON_DPAD_DOWN: app.move(1); break;
                case SDL_GAMEPAD_BUTTON_DPAD_LEFT: app.left_right(-1); break;
                case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: app.left_right(1); break;
                case SDL_GAMEPAD_BUTTON_SOUTH:
                case SDL_GAMEPAD_BUTTON_START: app.accept(); break;
                case SDL_GAMEPAD_BUTTON_EAST: app.cancel(); break;
                default: break;
                }
                break;
            case SDL_EVENT_MOUSE_MOTION: {
                app.mouseX = e.motion.x, app.mouseY = e.motion.y;
                const auto r = app.layout();
                for (size_t i = 0; i < r.size(); i++)
                    if (e.motion.x >= r[i].x && e.motion.x < r[i].x + r[i].w && e.motion.y >= r[i].y && e.motion.y < r[i].y + r[i].h) app.sel = int(i);
                break;
            }
            case SDL_EVENT_MOUSE_BUTTON_DOWN: {
                const auto r = app.layout();
                for (size_t i = 0; i < r.size(); i++)
                    if (e.button.x >= r[i].x && e.button.x < r[i].x + r[i].w && e.button.y >= r[i].y && e.button.y < r[i].y + r[i].h) {
                        app.sel = int(i);
                        if (e.button.button == SDL_BUTTON_RIGHT) app.left_right(-1);
                        else app.accept();
                    }
                if (e.button.button == SDL_BUTTON_X1) app.cancel();
                break;
            }
            default: break;
            }
        }
        const Uint64 now = SDL_GetTicksNS();
        const float dt = std::min(0.1f, float(now - last) / 1e9f);
        last = now;
        app.pollDialog();
        app.pollBuild();
        if (app.gameRunning) {
            if (app.game.running()) {
                SDL_Delay(100);
                continue;
            }
            app.gameRunning = false;
            const unsigned long code = app.game.exitCode();
            if (code != 0) app.say("The game closed unexpectedly (code " + std::to_string(code) + "). Its log is sa2b.log.");
            SDL_ShowWindow(app.win);
            SDL_RaiseWindow(app.win);
            last = SDL_GetTicksNS();
        }
        // the left stick moves the cursor, repeating while held
        int dir = 0;
        int count = 0;
        if (SDL_JoystickID* pads = SDL_GetGamepads(&count)) {
            for (int i = 0; i < count; i++)
                if (SDL_Gamepad* g = SDL_GetGamepadFromID(pads[i])) {
                    const int y = SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_LEFTY), x = SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_LEFTX);
                    if (y < -20000) dir = -1;
                    else if (y > 20000) dir = 1;
                    else if (x < -20000) dir = -2;
                    else if (x > 20000) dir = 2;
                }
            SDL_free(pads);
        }
        if (dir != stickDir) stickDir = dir, stickRepeat = 0, stickFirst = true;
        if (dir) {
            stickRepeat -= dt;
            if (stickRepeat <= 0) {
                if (dir == -1 || dir == 1) app.move(dir);
                else app.left_right(dir / 2);
                stickRepeat = stickFirst ? 0.35f : 0.12f;
                stickFirst = false;
            }
        }
        app.frame(dt);
    }
    app.cfg.save(app.dataDir / "launcher.ini");
    app.build.cancel();  // (a build under way stops; nothing of it is kept)
    SDL_Quit();
    return 0;
}
