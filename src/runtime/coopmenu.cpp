// The F1 menu (Dear ImGui, which Aurora draws over the game): a bar along the top with Co-op,
// Player and Discord. Notices (partner joined, left, ...) show in the corner even while it is closed.
#include "runtime/coop.h"
#include "runtime/runtime.h"

#include <aurora/aurora.h>
#include <imgui.h>

#include <SDL3/SDL.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <atomic>
#include <cstring>
#include <deque>
#include <iterator>
#include <mutex>
#include <string>

namespace rt::coop {

namespace {
struct Notice {
    std::string text;
    double until = 0;
};
Notice s_notice;
std::mutex s_noticeLock;  // (notices come from the game thread too)
double nowS() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
}  // namespace

void notify(const std::string& t) {
    std::lock_guard g(s_noticeLock);
    s_notice = {t, nowS() + 5};
}

// ---- chat: T opens a line to type, Enter sends it, Esc closes; the last lines show bottom left ----
namespace {
struct ChatLine {
    std::string name, text;
    unsigned colour;
    double at;
};
std::mutex s_chatLock;
std::deque<ChatLine> s_chat;
std::atomic<bool> s_typing{false};
ImU32 imColour(unsigned rgb, float alpha) {
    return IM_COL32(rgb >> 16 & 255, rgb >> 8 & 255, rgb & 255, int(alpha * 255));
}
}  // namespace

void chatArrived(const std::string& name, unsigned colour, const std::string& text) {
    log("[chat] %s (%06X): %s\n", name.c_str(), colour, text.c_str());
    std::lock_guard g(s_chatLock);
    s_chat.push_back({name, text, colour, nowS()});
    while (s_chat.size() > 50) s_chat.pop_front();
}
bool typing() { return s_typing; }

namespace {
void chatOverlay(ImGuiIO& io, bool menuOpen) {
    static bool s_t = false, s_opening = false;
    static char s_line[150] = "";
    const bool* keys = SDL_GetKeyboardState(nullptr);
    const bool t = keys[SDL_SCANCODE_T];
    // (opened the frame after T: so the T itself is not typed into the line)
    if (s_opening) s_typing = true, s_opening = false, s_line[0] = 0;
    if (t && !s_t && !s_typing && !menuOpen && !io.WantTextInput && link().state() == coopnet::State::Connected) s_opening = true;
    s_t = t;
    const double now = nowS();
    std::deque<ChatLine> lines;
    {
        std::lock_guard g(s_chatLock);
        for (size_t i = s_chat.size() > 8 ? s_chat.size() - 8 : 0; i < s_chat.size(); i++)
            if (s_typing || now - s_chat[i].at < 12) lines.push_back(s_chat[i]);
    }
    if (lines.empty() && !s_typing) return;
    const float width = std::min(io.DisplaySize.x * 0.45f, ImGui::GetFontSize() * 34);
    ImGui::SetNextWindowPos(ImVec2(16, io.DisplaySize.y * 0.80f), ImGuiCond_Always, ImVec2(0, 1));
    ImGui::SetNextWindowSize(ImVec2(width, 0));
    ImGui::SetNextWindowBgAlpha(s_typing ? 0.6f : 0.25f);
    ImGui::Begin("##chat", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav | (s_typing ? 0 : ImGuiWindowFlags_NoInputs));
    for (const ChatLine& l : lines) {
        const float alpha = s_typing ? 1.f : std::clamp(float(12 - (now - l.at)) / 2.f, 0.f, 1.f);  // (fading the last 2 s)
        ImGui::PushStyleColor(ImGuiCol_Text, imColour(l.colour, alpha));
        ImGui::TextUnformatted((l.name + ":").c_str());
        ImGui::PopStyleColor();
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Text, imColour(0xFFFFFF, alpha));
        ImGui::PushTextWrapPos(0);
        ImGui::TextUnformatted(l.text.c_str());
        ImGui::PopTextWrapPos();
        ImGui::PopStyleColor();
    }
    if (s_typing) {
        ImGui::PushStyleColor(ImGuiCol_Text, imColour(myNameColour(), 1));
        ImGui::TextUnformatted((settings().name + ":").c_str());
        ImGui::PopStyleColor();
        ImGui::SameLine();
        ImGui::SetNextItemWidth(-1);
        if (ImGui::IsWindowAppearing() || !ImGui::IsAnyItemActive()) ImGui::SetKeyboardFocusHere();
        if (ImGui::InputText("##line", s_line, sizeof s_line, ImGuiInputTextFlags_EnterReturnsTrue)) {
            if (s_line[0]) sendChat(s_line);
            s_typing = false;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_Escape)) s_typing = false;
    }
    ImGui::End();
}

// The partner's name above their head (coopgame.cpp: nameTag), from the game's 640x480 screen to
// the window: the 4:3 picture at the window's height, centred (widescreen shows more at the sides).
void nameTagOverlay(ImGuiIO& io) {
    float x, y;
    unsigned colour;
    if (!nameTag(x, y, colour)) return;
    const float scale = std::min(io.DisplaySize.x / 640.f, io.DisplaySize.y / 480.f);
    const float wx = io.DisplaySize.x * 0.5f + (x - 320.f) * scale, wy = io.DisplaySize.y * 0.5f + (y - 240.f) * scale;
    const std::string name = link().partnerName();
    ImFont* font = ImGui::GetFont();
    const float size = ImGui::GetFontSize() * 1.1f;
    const ImVec2 ts = font->CalcTextSizeA(size, 1e9f, 0, name.c_str());
    const ImVec2 at(wx - ts.x * 0.5f, wy - ts.y);
    if (at.x < -ts.x || at.x > io.DisplaySize.x || at.y < -ts.y || at.y > io.DisplaySize.y) return;
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    dl->AddText(font, size, ImVec2(at.x + 2, at.y + 2), IM_COL32(0, 0, 0, 170), name.c_str());  // (a shadow)
    dl->AddText(font, size, at, imColour(colour, 1), name.c_str());
}
}  // namespace

namespace {

// Changes of the link worth telling, whether the menu is open or not.
void watchLink() {
    static coopnet::State s_last = coopnet::State::Off;
    const coopnet::State st = link().state();
    if (st == s_last) return;
    if (st == coopnet::State::Connected) notify(link().partnerName() + " joined");
    else if (st == coopnet::State::Lost) notify("Your partner is not responding");
    else if (st == coopnet::State::Waiting && s_last == coopnet::State::Connected) notify("Your partner left");
    else if (st == coopnet::State::Failed) notify("Co-op: " + link().error());
    s_last = st;
}

void coopMenu() {
    coopnet::Link& l = link();
    const coopnet::State st = l.state();
    ImGui::TextDisabled("Online co-op through the relay server");
    ImGui::Separator();
    if (active() && st != coopnet::State::Off) {
        ImGui::Text("%s", coopnet::stateName(st));
        if (st == coopnet::State::Connected) {
            ImGui::Text("Partner: %s", l.partnerName().c_str());
            if (l.pingMs() >= 0) ImGui::Text("Ping: %d ms", l.pingMs());
        }
        if (st == coopnet::State::Failed && !l.error().empty()) ImGui::TextWrapped("%s", l.error().c_str());
        const std::string code = l.code();
        ImGui::Text("%s code: %s", l.host() ? "Your" : "Joined", code.c_str());
        if (l.host()) {
            ImGui::SameLine();
            if (ImGui::SmallButton("Copy")) ImGui::SetClipboardText(code.c_str());
        }
        if (ImGui::MenuItem("Leave")) l.stop();
        return;
    }
    if (ImGui::MenuItem("Host a game")) hostOrJoin(true, coopnet::newCode());
    ImGui::Separator();
    static char s_code[16] = "";
    static bool s_init = false;
    if (!s_init) std::snprintf(s_code, sizeof s_code, "%s", settings().lastCode.c_str()), s_init = true;
    ImGui::Text("Join with your partner's code:");
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 7);
    ImGui::InputText("##code", s_code, sizeof s_code, ImGuiInputTextFlags_CharsUppercase);
    ImGui::SameLine();
    const std::string code = coopnet::cleanCode(s_code);
    if (code.empty()) ImGui::BeginDisabled();
    if (ImGui::Button("Join")) hostOrJoin(false, code);
    if (code.empty()) ImGui::EndDisabled();
    if (!active()) {
        ImGui::Separator();
        ImGui::TextDisabled("Co-op plays on its own save (saves\\coop), apart from your game:");
        ImGui::TextDisabled("hosting or joining starts the game again in co-op.");
    }
}

// Colours (coopcolour.cpp): each area as the game has it or a colour of your own - one of the
// presets or any from the picker. Your partner sees them too.
void colourMenu() {
    struct Slot {
        const char* group;  // (a heading when it changes)
        const char* what;
        int game;           // about the game's own (where a new own colour starts)
    };
    static const Slot kSlots[kColourSlots] = {
        {"Sonic", "Blue", 0x1E3CE0},          {"Sonic", "Jump ball and trails", 0x1E3CE0},
        {"Shadow", "Red stripes", 0xD01818},  {"Shadow", "Jump ball and trails", 0xE0A020},
        {"Tails", "Fur", 0xF0B030},           {"Dr. Eggman", "Suit", 0xD01818},
        {"Knuckles", "Red", 0xD82020},        {"Rouge", "White fur", 0xF0F0F0},
        {"Tails' Cyclone", "Blue", 0x3040C0}, {"Tails' Cyclone", "Yellow", 0xF0C020},
        {"Eggman's Egg Walker", "Gray", 0xA0A0A0}, {"Eggman's Egg Walker", "Red", 0xD01818},
    };
    struct Preset {
        const char* name;
        int rgb;
    };
    static const Preset kPresets[] = {{"Red", 0xE02020},   {"Orange", 0xF08020}, {"Yellow", 0xF0D020}, {"Green", 0x20C040},
                                      {"Cyan", 0x20C8E0},  {"Blue", 0x2050F0},   {"Purple", 0x8030E0}, {"Pink", 0xF060C0},
                                      {"White", 0xF0F0F0}, {"Black", 0x303030}};
    auto vec = [](int rgb) { return ImVec4(float(rgb >> 16 & 255) / 255, float(rgb >> 8 & 255) / 255, float(rgb & 255) / 255, 1); };
    ImGui::Text("Colours");
    if (active()) ImGui::TextDisabled("Your partner sees your colours too.");
    int* colours = settings().colours;
    bool changed = false, done = false;
    for (int i = 0; i < kColourSlots; i++) {
        if (!i || std::strcmp(kSlots[i].group, kSlots[i - 1].group)) ImGui::SeparatorText(kSlots[i].group);
        ImGui::PushID(i);
        bool own = colours[i] >= 0;
        if (ImGui::Checkbox(kSlots[i].what, &own)) colours[i] = own ? kSlots[i].game : -1, changed = done = true;
        if (own) {
            ImGui::Indent();
            float c[3] = {float(colours[i] >> 16 & 255) / 255, float(colours[i] >> 8 & 255) / 255, float(colours[i] & 255) / 255};
            if (ImGui::ColorEdit3("##pick", c, ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_PickerHueWheel)) {
                colours[i] = int(c[0] * 255 + 0.5f) << 16 | int(c[1] * 255 + 0.5f) << 8 | int(c[2] * 255 + 0.5f);
                changed = true;
            }
            done |= ImGui::IsItemDeactivatedAfterEdit();
            const float sz = ImGui::GetFrameHeight();
            for (int k = 0; k < int(std::size(kPresets)); k++) {  // (ids by number: "Blue" is a slot's name too)
                const Preset& p = kPresets[k];
                ImGui::SameLine();
                ImGui::PushID(100 + k);
                if (ImGui::ColorButton("##preset", vec(p.rgb), ImGuiColorEditFlags_NoTooltip, ImVec2(sz, sz))) colours[i] = p.rgb, changed = done = true;
                ImGui::PopID();
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", p.name);
            }
            ImGui::Unindent();
        }
        ImGui::PopID();
    }
    if (changed) coloursChanged();
    if (done) saveSettings();
}

void playerMenu() {
    static char s_name[32] = "";
    static bool s_init = false;
    if (!s_init) std::snprintf(s_name, sizeof s_name, "%s", settings().name.c_str()), s_init = true;
    ImGui::Text("Your name");
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12);
    if (ImGui::InputText("##name", s_name, 25) && s_name[0]) settings().name = s_name;
    if (ImGui::IsItemDeactivatedAfterEdit()) saveSettings();
    ImGui::Separator();
    colourMenu();
}

void discordMenu() {
    if (ImGui::Checkbox("Show what I'm playing on Discord", &settings().discord)) saveSettings();
    ImGui::Text("Status: %s", discord::status().c_str());
    static char s_id[32] = "";
    static bool s_init = false;
    if (!s_init) std::snprintf(s_id, sizeof s_id, "%s", settings().discordAppId.c_str()), s_init = true;
    ImGui::Text("Application id");
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12);
    if (ImGui::InputText("##appid", s_id, sizeof s_id, ImGuiInputTextFlags_CharsDecimal)) settings().discordAppId = s_id;
    if (ImGui::IsItemDeactivatedAfterEdit()) saveSettings();
}
}  // namespace

void overlay() {
    if (!ImGui::GetCurrentContext()) return;
    static bool s_open = false, s_f1 = false;
    const bool down = SDL_GetKeyboardState(nullptr)[SDL_SCANCODE_F1];
    if (down && !s_f1) s_open = !s_open;
    s_f1 = down;
    ImGuiIO& io = ImGui::GetIO();
    io.MouseDrawCursor = s_open;
    // readable at any window size (ImGui's font is drawn for about 720 lines)
    io.FontGlobalScale = std::max(1.f, io.DisplaySize.y / 760.f);
    watchLink();
    Notice notice;
    {
        std::lock_guard g(s_noticeLock);
        notice = s_notice;
    }
    if (nowS() < notice.until) {
        ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x - 16, s_open ? 48.f : 16.f), ImGuiCond_Always, ImVec2(1, 0));
        ImGui::SetNextWindowBgAlpha(0.7f);
        ImGui::Begin("##notice", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoSavedSettings |
                         ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav);
        ImGui::TextUnformatted(notice.text.c_str());
        ImGui::End();
    }
    // What the game is holding for (the partner still on the way to the goal), centred low.
    if (const std::string held = gameStatus(); !held.empty()) {
        ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.78f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
        ImGui::SetNextWindowBgAlpha(0.7f);
        ImGui::Begin("##held", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoSavedSettings |
                         ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav);
        ImGui::TextUnformatted(held.c_str());
        ImGui::End();
    }
    if (active()) nameTagOverlay(io), chatOverlay(io, s_open);
    if (!s_open) return;
    if (ImGui::BeginMainMenuBar()) {
        if (ImGui::BeginMenu("Co-op")) coopMenu(), ImGui::EndMenu();
        if (ImGui::BeginMenu("Player")) playerMenu(), ImGui::EndMenu();
        if (ImGui::BeginMenu("Discord")) discordMenu(), ImGui::EndMenu();
        ImGui::TextDisabled("  F1 closes this menu");
        if (active()) {
            const coopnet::State st = link().state();
            const std::string right = st == coopnet::State::Connected ? "Co-op with " + link().partnerName() : coopnet::stateName(st);
            ImGui::SameLine(ImGui::GetWindowWidth() - ImGui::CalcTextSize(right.c_str()).x - 24);
            ImGui::TextUnformatted(right.c_str());
        }
        ImGui::EndMainMenuBar();
    }
}

}  // namespace rt::coop
