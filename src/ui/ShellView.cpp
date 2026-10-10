// The window frame: a header with the logo, the screen tabs and live status
// pills, and the scrolling page area below it.
#include <algorithm>
#include <cmath>
#include <cstdio>

#include "Version.h"
#include "core/Text.h"
#include "imgui.h"
#include "ui/App.h"
#include "ui/Icons.h"
#include "ui/Pop.h"
#include "ui/Theme.h"
#include "ui/Widgets.h"

namespace echo::ui {

namespace {
const char* const kPageNames[] = {"Home", "Voices", "Mixer", "Apps", "Devices", "Recorder", "Settings"};
} // namespace

void EchoApp::drawShell() {
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    const Palette& p = pal();
    const float sc = uiScale();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGui::SetNextWindowViewport(vp->ID);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, toVec4(p.bg));
    ImGui::Begin("##echo-shell", nullptr,
                 ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse |
                     ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBringToFrontOnFocus |
                     ImGuiWindowFlags_NoNavFocus | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse |
                     ImGuiWindowFlags_NoSavedSettings);
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(3);

    drawHeader();

    // Page area: centred, at most 1280 design pixels wide.
    const float avail = ImGui::GetContentRegionAvail().x;
    const float padX = std::max(28.0f * sc, (avail - 1280.0f * sc) * 0.5f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(padX, 24.0f * sc));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(16.0f * sc, 16.0f * sc));
    char id[32];
    std::snprintf(id, sizeof(id), "##page%d", static_cast<int>(page_));
    ImGui::BeginChild(id, ImVec2(0, 0), ImGuiChildFlags_AlwaysUseWindowPadding);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(10.0f * sc, 8.0f * sc));
    switch (page_) {
    case Page::Home: drawHome(); break;
    case Page::Voices: drawVoiceLab(); break;
    case Page::Mixer: drawMixer(); break;
    case Page::Apps: drawApps(); break;
    case Page::Devices: drawDevices(); break;
    case Page::Recorder: drawRecorder(); break;
    case Page::Settings: drawSettings(); break;
    default: break;
    }
    ImGui::Dummy(ImVec2(0, 12.0f * sc));
    ImGui::PopStyleVar();
    ImGui::EndChild();
    ImGui::PopStyleVar(2);
    ImGui::End();
}

void EchoApp::drawHeader() {
    const Palette& p = pal();
    const float sc = uiScale();
    const float h = 64.0f * sc;
    ImGui::PushStyleColor(ImGuiCol_ChildBg, toVec4(p.surface));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(24.0f * sc, 14.0f * sc));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(4.0f * sc, 0.0f));
    ImGui::BeginChild("##header", ImVec2(0, h), ImGuiChildFlags_AlwaysUseWindowPadding,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    {
        const ImVec2 wp = ImGui::GetWindowPos(), ws = ImGui::GetWindowSize();
        dl->AddLine(ImVec2(wp.x, wp.y + ws.y - 0.75f * sc), ImVec2(wp.x + ws.x, wp.y + ws.y - 0.75f * sc), p.line, 1.5f * sc);
    }

    // Logo: accent square with a sound-wave mark, then the name.
    const ImVec2 lp = ImGui::GetCursorScreenPos();
    const float logo = 32.0f * sc;
    const float cy = lp.y + 18.0f * sc;
    if (ImGui::InvisibleButton("##logo", ImVec2(logo + 70.0f * sc, 36.0f * sc))) navigate(Page::Home);
    const ImVec2 l0(lp.x, cy - logo * 0.5f), l1(lp.x + logo, cy + logo * 0.5f);
    dl->AddRectFilled(l0, l1, p.accent, 10.0f * sc);
    dl->AddRect(l0, l1, p.outline, 10.0f * sc, 1.5f * sc);
    {
        // Five bars of a waveform.
        const float bars[] = {0.25f, 0.55f, 0.85f, 0.55f, 0.25f};
        const float bx = l0.x + 7.0f * sc, step = 4.5f * sc;
        for (int i = 0; i < 5; ++i) {
            const float hh = logo * 0.5f * bars[i];
            dl->AddLine(ImVec2(bx + i * step, cy - hh * 0.5f - 1.0f * sc), ImVec2(bx + i * step, cy + hh * 0.5f + 1.0f * sc),
                        p.onAccent, 2.4f * sc);
        }
    }
    ImGui::PushFont(fonts().semibold, 19.0f);
    dl->AddText(ImVec2(l1.x + 10.0f * sc, cy - ImGui::GetFontSize() * 0.55f), p.text, ECHO_APP_NAME);
    ImGui::PopFont();

    // Screen tabs.
    ImGui::SameLine(0, 18.0f * sc);
    for (int i = 0; i < static_cast<int>(Page::Count); ++i) {
        if (i) ImGui::SameLine(0, 2.0f * sc);
        if (pop::NavTab(kPageNames[i], static_cast<int>(page_) == i)) navigate(static_cast<Page>(i));
    }

    // Status pills, right-aligned.
    struct Pill {
        std::string text;
        ImU32 bg, fg, dot;
        Page target;
        std::string tip;
    };
    std::vector<Pill> pills;
    Engine& e = mc_->engine();
    if (mc_->recorder().active()) {
        const bool blink = std::fmod(now_, 1.0) < 0.6;
        pills.push_back({"Recording " + formatDuration(mc_->recorder().status().seconds), p.badTint, p.badInk,
                         blink ? p.bad : p.badTint, Page::Recorder, "Ctrl+R stops recording"});
    }
    if (!mc_->feedbackWarnings().empty())
        pills.push_back({"Feedback blocked", p.warnTint, p.warnInk, p.warn, Page::Mixer, mc_->feedbackWarnings().front().text});
    char engineInfo[160];
    uint64_t drops = 0;
    if (const GraphSnapshot* g = e.current()) {
        for (int i = 0; i < g->numSources; ++i) drops += g->sources[i]->underruns.load() + g->sources[i]->overruns.load();
        for (int i = 0; i < g->numSinks; ++i) drops += g->sinks[i]->underruns.load() + g->sinks[i]->overruns.load();
    }
    std::snprintf(engineInfo, sizeof(engineInfo), "%d Hz, %d samples (%.1f ms)\nProcessing load %.1f%%\nDropouts: %llu",
                  e.sampleRate(), e.blockSize(), e.blockSeconds() * 1000.0, e.stats().cpuLoad.load() * 100.0f,
                  static_cast<unsigned long long>(drops));
    const HotkeySettings& hk = mc_->session().hotkeys;
    const bool talkMode = hk.talkMode != TalkMode::Off && !hk.talkKey.empty();
    if (!e.running()) {
        pills.push_back({"Audio stopped", p.badTint, p.badInk, p.bad, Page::Settings, "Start it in Settings > Audio"});
    } else if (mc_->microphonesMuted()) {
        pills.push_back({"Mic muted", p.segBg, p.text2, p.textDim, Page::Home,
                         hk.muteKey.empty() ? std::string(engineInfo) : "Press " + keyBindingName(hk.muteKey) + " to unmute\n" + engineInfo});
    } else if (talkMode && hotkeys_ && !hotkeys_->gateOpen()) {
        pills.push_back({hk.talkMode == TalkMode::PushToTalk ? "Push to talk · mic off" : "Push to mute · muted", p.segBg,
                         p.text2, p.textDim, Page::Home,
                         std::string(hk.talkMode == TalkMode::PushToTalk ? "Hold " : "Release ") + keyBindingName(hk.talkKey) + "\n" + engineInfo});
    } else {
        pills.push_back({"Live", p.goodTint, p.goodInk, p.meterGreen, Page::Home, engineInfo});
    }
    float total = 0.0f;
    for (const auto& pl : pills) total += pop::StatusPillWidth(pl.text.c_str()) + 8.0f * sc;
    const float right = ImGui::GetWindowWidth() - ImGui::GetStyle().WindowPadding.x;
    ImGui::SameLine(std::max(ImGui::GetCursorPosX() + 12.0f * sc, right - total + 8.0f * sc));
    const float pillY = ImGui::GetCursorPosY() + 3.0f * sc;
    for (size_t i = 0; i < pills.size(); ++i) {
        if (i) ImGui::SameLine(0, 8.0f * sc);
        ImGui::SetCursorPosY(pillY);
        if (pop::StatusPill(pills[i].text.c_str(), pills[i].bg, pills[i].fg, pills[i].dot)) navigate(pills[i].target);
        if (!pills[i].tip.empty()) Tip(pills[i].tip.c_str());
    }
    ImGui::EndChild();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();
}

} // namespace echo::ui
