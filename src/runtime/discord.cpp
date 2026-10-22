// Discord Rich Presence: what the player is doing, on their Discord profile. Spoken to the Discord
// app over its local IPC pipe (\\.\pipe\discord-ipc-N: frames of a 32-bit opcode, a 32-bit length
// and JSON - 0 HANDSHAKE, 1 FRAME, 2 CLOSE), no SDK. It needs the id of a Discord application
// (coop.ini: discord_app_id) - the name and pictures shown are that application's.
#include "runtime/coop.h"
#include "runtime/runtime.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <mutex>
#include <string>
#include <thread>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

namespace rt {
extern std::atomic<bool> g_relLinked[256];  // src/runtime/dispatch.cpp
}

namespace rt::discord {

namespace {
std::mutex s_lock;
std::string s_status = "Off";
void setStatus(const std::string& s) {
    std::lock_guard g(s_lock);
    s_status = s;
}

// ---- what to show ----
const char* levelName(u32 id) {
    static const char* const kNames[] = {
        "Basic Test", "Knuckles Test", "Sonic Test", "Green Forest", "White Jungle", "Pumpkin Hill", "Sky Rail", "Aquatic Mine",
        "Security Hall", "Prison Lane", "Metal Harbor", "Iron Gate", "Weapons Bed", "City Escape", "Radical Highway",
        "Weapons Bed", "Wild Canyon", "Mission Street", "Dry Lagoon", "Sonic vs. Shadow", "Tails vs. Eggman", "Sand Ocean",
        "Crazy Gadget", "Hidden Base", "Eternal Engine", "Death Chamber", "Egg Quarters", "Lost Colony", "Pyramid Cave",
        "Tails vs. Eggman", "Final Rush", "Green Hill", "Meteor Herd", "Knuckles vs. Rouge", "Cannon's Core", "Cannon's Core",
        "Cannon's Core", "Cannon's Core", "Cannon's Core", "Mission Street", "Final Chase", "Wild Canyon", "Sonic vs. Shadow",
        "Cosmic Wall", "Mad Space", "Sand Ocean", "Dry Lagoon", "Pyramid Race", "Hidden Base", "Pool Quest", "Planet Quest",
        "Deck Race", "Downhill Race", "Cosmic Wall", "Grind Race", "Lost Colony", "Eternal Engine", "Metal Harbor", "Iron Gate",
        "Death Chamber", "Big Foot", "Hot Shot", "Flying Dog", "King Boom Boo", "Egg Golem", "Biolizard", "Finalhazard",
        "Egg Golem"};
    if (id < sizeof kNames / sizeof *kNames) return kNames[id];
    if (id == 70) return "Route 280";
    if (id == 71) return "Kart Race";
    if (id == 90) return "Chao World";
    return nullptr;
}
const char* characterName(u32 c, const char** image) {
    static const char* const kNames[] = {"Sonic", "Shadow", "Tails", "Dr. Eggman", "Knuckles", "Rouge", "Tails", "Dr. Eggman",
                                         "Amy", "Metal Sonic", "Chao Walker", "Dark Chao Walker", "Tikal", "Chaos"};
    static const char* const kImages[] = {"sonic", "shadow", "tails", "eggman", "knuckles", "rouge", "tails", "eggman",
                                          "amy", "metalsonic", "chao", "darkchao", "tikal", "chaos"};
    if (c >= sizeof kNames / sizeof *kNames) return *image = nullptr, nullptr;
    *image = kImages[c];
    return kNames[c];
}

std::string jsonEscape(const std::string& s) {
    std::string o;
    for (char ch : s) {
        if (ch == '"' || ch == '\\') o += '\\', o += ch;
        else if (u8(ch) < 0x20) o += ' ';
        else o += ch;
    }
    return o;
}

// The activity as JSON, from the game's state (read as it runs: a stale value only shows a moment late).
std::string activity(std::time_t started) {
    const u32 mode = rd32(0x803AD818), level = rd16(0x803AD820), chr = rd32(0x803AD828);
    std::string details, state;
    const char* image = nullptr;
    const char* chrName = characterName(chr, &image);
    const char* lv = levelName(level);
    const bool inStage = (mode == 0x10 || mode == 0x11) && lv;
    if (inStage && level == 90) details = "In the Chao World";
    else if (inStage) details = std::string(lv) + (chrName ? std::string(" as ") + chrName : "");
    else if (g_relLinked[91]) details = "In the menus";
    else details = "Loading";
    coopnet::Link& link = coop::link();
    if (coop::active()) {
        const std::string partner = link.partnerName();
        state = link.state() == coopnet::State::Connected && !partner.empty() ? "Co-op with " + partner : "Co-op: waiting for a partner";
    } else {
        state = "Single player";
    }
    char ts[32];
    std::snprintf(ts, sizeof ts, "%lld", (long long)started);
    std::string a = "{\"details\":\"" + jsonEscape(details) + "\",\"state\":\"" + jsonEscape(state) + "\",\"timestamps\":{\"start\":" + ts +
                    "},\"assets\":{\"large_image\":\"logo\",\"large_text\":\"Sonic Adventure 2 Battle: Recompiled\"";
    if (inStage && image) a += ",\"small_image\":\"" + std::string(image) + "\",\"small_text\":\"" + chrName + "\"";
    a += "}";
    if (coop::active() && link.state() == coopnet::State::Connected)
        a += ",\"party\":{\"id\":\"sa2b-" + jsonEscape(link.code()) + "\",\"size\":[2,2]}";
    a += "}";
    return a;
}

// ---- the pipe ----
struct Pipe {
    HANDLE h = INVALID_HANDLE_VALUE;
    bool open() {
        for (int i = 0; i < 10 && h == INVALID_HANDLE_VALUE; i++) {
            const std::wstring name = L"\\\\.\\pipe\\discord-ipc-" + std::to_wstring(i);
            h = CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        }
        return h != INVALID_HANDLE_VALUE;
    }
    void close() {
        if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
        h = INVALID_HANDLE_VALUE;
    }
    bool write(u32 op, const std::string& json) {
        std::string f(8, '\0');
        const u32 len = u32(json.size());
        std::memcpy(f.data(), &op, 4);
        std::memcpy(f.data() + 4, &len, 4);
        f += json;
        DWORD n = 0;
        return WriteFile(h, f.data(), DWORD(f.size()), &n, nullptr) && n == f.size();
    }
    // One reply (Discord answers every frame we send).
    bool read(u32& op, std::string& json) {
        u32 hdr[2];
        DWORD n = 0;
        if (!ReadFile(h, hdr, 8, &n, nullptr) || n != 8) return false;
        op = hdr[0];
        json.assign(hdr[1], '\0');
        for (DWORD got = 0; got < hdr[1];) {
            if (!ReadFile(h, json.data() + got, hdr[1] - got, &n, nullptr) || !n) return false;
            got += n;
        }
        return true;
    }
};

void run() {
    const std::time_t started = std::time(nullptr);
    Pipe pipe;
    std::string last;
    auto lastSent = std::chrono::steady_clock::now() - std::chrono::hours(1);
    u32 nonce = 0;
    for (;;) {
        std::this_thread::sleep_for(std::chrono::seconds(2));
        const coop::Settings& s = coop::settings();
        if (!s.discord || s.discordAppId.empty()) {
            if (pipe.h != INVALID_HANDLE_VALUE) pipe.close(), last.clear();
            setStatus(!s.discord ? "Off" : "Needs the Discord application id (coop.ini: discord_app_id)");
            continue;
        }
        if (pipe.h == INVALID_HANDLE_VALUE) {
            if (!pipe.open()) {
                setStatus("Discord is not running");
                std::this_thread::sleep_for(std::chrono::seconds(8));
                continue;
            }
            u32 op = 0;
            std::string reply;
            if (!pipe.write(0, "{\"v\":1,\"client_id\":\"" + jsonEscape(s.discordAppId) + "\"}") || !pipe.read(op, reply) || op == 2) {
                setStatus("Discord refused the connection" + (reply.empty() ? std::string() : ": " + reply.substr(0, 120)));
                log("[discord] handshake failed: %s\n", reply.c_str());
                pipe.close();
                std::this_thread::sleep_for(std::chrono::seconds(30));
                continue;
            }
            setStatus("Connected");
            last.clear();
        }
        const std::string a = activity(started);
        const auto now = std::chrono::steady_clock::now();
        if (a == last && now - lastSent < std::chrono::seconds(60)) continue;
        const std::string cmd = "{\"cmd\":\"SET_ACTIVITY\",\"args\":{\"pid\":" + std::to_string(GetCurrentProcessId()) + ",\"activity\":" + a +
                                "},\"nonce\":\"" + std::to_string(++nonce) + "\"}";
        u32 op = 0;
        std::string reply;
        if (!pipe.write(1, cmd) || !pipe.read(op, reply)) {
            setStatus("Discord closed the connection");
            pipe.close();
            continue;
        }
        if (reply.find("\"evt\":\"ERROR\"") != std::string::npos) log("[discord] %s\n", reply.c_str());
        last = a, lastSent = now;
    }
}
}  // namespace

void start() {
    static bool s_started = false;
    if (s_started) return;
    s_started = true;
    std::thread(run).detach();
}

std::string status() {
    std::lock_guard g(s_lock);
    return s_status;
}

}  // namespace rt::discord
