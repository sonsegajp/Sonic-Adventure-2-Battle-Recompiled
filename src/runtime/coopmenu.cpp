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
#include <cstring>
#include <string>

namespace rt::coop {

namespace {
struct Notice {
    std::string text;
    double until = 0;
};
Notice s_notice;
double nowS() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
void notify(const std::string& t) { s_notice = {t, nowS() + 5}; }

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

void playerMenu() {
    static char s_name[32] = "";
    static bool s_init = false;
    if (!s_init) std::snprintf(s_name, sizeof s_name, "%s", settings().name.c_str()), s_init = true;
    ImGui::Text("Your name");
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12);
    if (ImGui::InputText("##name", s_name, 25) && s_name[0]) settings().name = s_name;
    if (ImGui::IsItemDeactivatedAfterEdit()) saveSettings();
    ImGui::Separator();
    ImGui::TextDisabled("Character colors: with the co-op partner update");
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
    if (nowS() < s_notice.until) {
        ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x - 16, s_open ? 48.f : 16.f), ImGuiCond_Always, ImVec2(1, 0));
        ImGui::SetNextWindowBgAlpha(0.7f);
        ImGui::Begin("##notice", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoSavedSettings |
                         ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav);
        ImGui::TextUnformatted(s_notice.text.c_str());
        ImGui::End();
    }
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
