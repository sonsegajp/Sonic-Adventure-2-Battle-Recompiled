#include "runtime/coop.h"
#include "runtime/runtime.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <mutex>
#include <thread>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

namespace rt::coop {

namespace {
Settings s_settings;
std::mutex s_settingsLock;
bool s_active = false;
coopnet::Link s_link;
std::vector<std::string> s_args;  // the command line, for starting again in co-op
constexpr const char* kSettingsFile = "coop.ini";
const char* const kColourKeys[kColourSlots] = {"sonic",    "sonic_effects", "shadow",        "shadow_effects",
                                                "tails",    "eggman",        "knuckles",      "rouge",
                                                "cyclone_blue", "cyclone_yellow", "walker_gray", "walker_red"};

std::string defaultName() {
    const char* u = std::getenv("USERNAME");
    return u && *u ? std::string(u).substr(0, 24) : "Player";
}

void loadSettings() {
    s_settings.name = defaultName();
    std::ifstream f(kSettingsFile);
    for (std::string l; std::getline(f, l);) {
        if (!l.empty() && l.back() == '\r') l.pop_back();
        const size_t eq = l.find('=');
        if (eq == std::string::npos) continue;
        const std::string k = l.substr(0, eq), v = l.substr(eq + 1);
        if (k == "name" && !v.empty()) s_settings.name = v.substr(0, 24);
        else if (k == "relay" && !v.empty()) s_settings.relay = v;
        else if (k == "discord") s_settings.discord = v != "0";
        else if (k == "discord_app_id") s_settings.discordAppId = v;
        else if (k == "last_code") s_settings.lastCode = v;
        else
            for (int i = 0; i < kColourSlots; i++)  // colour_<slot>=RRGGBB (else as the game has it)
                if (k == std::string("colour_") + kColourKeys[i])
                    s_settings.colours[i] = v.size() == 6 ? int(std::strtol(v.c_str(), nullptr, 16)) : -1;
    }
}
}  // namespace

Settings& settings() { return s_settings; }

void saveSettings() {
    std::lock_guard g(s_settingsLock);
    std::ofstream f(kSettingsFile);
    f << "name=" << s_settings.name << "\nrelay=" << s_settings.relay << "\ndiscord=" << (s_settings.discord ? 1 : 0)
      << "\ndiscord_app_id=" << s_settings.discordAppId << "\nlast_code=" << s_settings.lastCode << "\n";
    for (int i = 0; i < kColourSlots; i++) {
        char v[8] = "game";
        if (s_settings.colours[i] >= 0) std::snprintf(v, sizeof v, "%06X", s_settings.colours[i] & 0xFFFFFF);
        f << "colour_" << kColourKeys[i] << "=" << v << "\n";
    }
}

coopnet::Relay relay() {
    coopnet::Relay r;
    const std::string& v = s_settings.relay;
    const size_t colon = v.rfind(':');
    r.host = colon == std::string::npos ? v : v.substr(0, colon);
    if (colon != std::string::npos) r.port = uint16_t(std::atoi(v.c_str() + colon + 1));
    if (r.host.empty()) r.host = coopnet::Relay{}.host;
    if (!r.port) r.port = coopnet::Relay{}.port;
    return r;
}

void init(int argc, char** argv) {
    loadSettings();
    bool host = false;
    std::string code;
    for (int i = 1; i < argc; i++) {
        const std::string a = argv[i];
        if ((a == "--coop-host" || a == "--coop-join") && i + 1 < argc) {
            host = a == "--coop-host";
            code = coopnet::cleanCode(argv[++i]);
            continue;
        }
        if (a == "--coop-name" && i + 1 < argc) {
            s_settings.name = std::string(argv[++i]).substr(0, 24);
            continue;
        }
        s_args.push_back(a);
    }
    if (code.empty()) return;
    s_active = true;
    // co-op's own memory card (read by the EXI layer when the card is first used)
    _putenv("SA2_CARD=saves/coop/MemoryCardA.raw");
    _putenv("SA2_CARD_FORMATTED=1");  // (a new one starts formatted: no format prompt in co-op)
    if (!host) s_settings.lastCode = code, saveSettings();
    s_link.start(host, code, s_settings.name, relay());
    log("[coop] %s %s through %s; saves: saves/coop\n", host ? "hosting" : "joining", code.c_str(), s_settings.relay.c_str());
    std::thread([] {  // the link's changes, in the log
        coopnet::State last = coopnet::State::Off;
        int lastPing = -1;
        for (;;) {
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
            const coopnet::State st = s_link.state();
            if (st != last) {
                log("[coop] %s%s%s\n", coopnet::stateName(st), st == coopnet::State::Connected ? ": " : "",
                    st == coopnet::State::Connected ? s_link.partnerName().c_str() : s_link.error().c_str());
                last = st;
            }
            const int ping = s_link.pingMs();
            if (st == coopnet::State::Connected && ping >= 0 && lastPing < 0) log("[coop] ping %d ms\n", ping);
            lastPing = ping;
        }
    }).detach();
}

bool active() { return s_active; }
coopnet::Link& link() { return s_link; }

void hostOrJoin(bool host, const std::string& code) {
    if (!host) s_settings.lastCode = code;
    saveSettings();
    if (s_active) {
        s_link.start(host, code, s_settings.name, relay());
        return;
    }
    // Start again as a co-op session: the same program and options, plus the co-op ones.
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    auto wide = [](const std::string& s) {
        std::wstring w(size_t(MultiByteToWideChar(CP_UTF8, 0, s.c_str(), int(s.size()), nullptr, 0)), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, s.c_str(), int(s.size()), w.data(), int(w.size()));
        return w;
    };
    std::wstring cmd = L"\"" + std::wstring(exe) + L"\"";
    for (const std::string& a : s_args) cmd += L" \"" + wide(a) + L"\"";
    cmd += host ? L" --coop-host " : L" --coop-join ";
    cmd += wide(code);
    STARTUPINFOW si{sizeof si};
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(exe, cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
        log("[coop] couldn't start the game again in co-op (error %lu)\n", GetLastError());
        return;
    }
    CloseHandle(pi.hThread), CloseHandle(pi.hProcess);
    log("[coop] starting again as a co-op session (%s %s, process %lu)\n", host ? "hosting" : "joining", code.c_str(), pi.dwProcessId);
    // (the launcher, waiting on this process, takes the exit code as "carry on with the new one")
    std::_Exit(kRestartedInCoop);
}

}  // namespace rt::coop
