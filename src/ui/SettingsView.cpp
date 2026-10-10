// Settings: audio engine, hotkeys, appearance, startup & data, privacy,
// diagnostics and about.
#include <windows.h>
#include <shellapi.h>

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "Version.h"
#include "core/Text.h"
#include "imgui.h"
#include "session/Paths.h"
#include "session/Serialization.h"
#include "ui/App.h"
#include "ui/Icons.h"
#include "ui/Pop.h"
#include "ui/Theme.h"
#include "ui/Widgets.h"

namespace echo::ui {

namespace {

const wchar_t* kRunKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";

bool startupEnabled() {
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_READ, &k) != ERROR_SUCCESS) return false;
    const bool on = RegQueryValueExW(k, ECHO_APP_ID_W, nullptr, nullptr, nullptr, nullptr) == ERROR_SUCCESS;
    RegCloseKey(k);
    return on;
}

void setStartup(bool on) {
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_SET_VALUE, &k) != ERROR_SUCCESS) return;
    if (on) {
        wchar_t exe[MAX_PATH * 2];
        GetModuleFileNameW(nullptr, exe, MAX_PATH * 2);
        const std::wstring cmd = L"\"" + std::wstring(exe) + L"\" --minimized";
        RegSetValueExW(k, ECHO_APP_ID_W, 0, REG_SZ, reinterpret_cast<const BYTE*>(cmd.c_str()),
                       static_cast<DWORD>((cmd.size() + 1) * sizeof(wchar_t)));
    } else {
        RegDeleteValueW(k, ECHO_APP_ID_W);
    }
    RegCloseKey(k);
}

const char* const kTabNames[] = {"Audio", "Hotkeys", "Appearance", "Startup & data", "Privacy", "Diagnostics", "About"};

bool sideTab(const char* label, bool active, float width) {
    const Palette& p = pal();
    const float sc = uiScale();
    const ImVec2 size(width, 40.0f * sc);
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const bool clicked = ImGui::InvisibleButton(label, size);
    const bool hov = ImGui::IsItemHovered();
    if (hov) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 b(pos.x + size.x, pos.y + size.y);
    if (active) {
        dl->AddRectFilled(pos, b, p.surface, 10.0f * sc);
        dl->AddRect(pos, b, p.outline, 10.0f * sc, 1.5f * sc);
    } else if (hov) {
        dl->AddRectFilled(pos, b, p.segBg, 10.0f * sc);
    }
    ImGui::PushFont(active ? fonts().semibold : fonts().regular, 14.0f);
    dl->AddText(ImVec2(pos.x + 14.0f * sc, pos.y + (size.y - ImGui::GetFontSize()) * 0.5f), p.text, label);
    ImGui::PopFont();
    return clicked;
}

// Title and description on the left, a control (next item) on the right.
void settingRow(const char* title, const char* desc, float controlW) {
    const Palette& p = pal();
    const float sc = uiScale();
    const float w = ImGui::GetContentRegionAvail().x;
    const float x0 = ImGui::GetCursorPosX();
    const float y0 = ImGui::GetCursorPosY();
    ImGui::BeginGroup();
    ImGui::PushTextWrapPos(x0 + w - controlW - 20.0f * sc);
    ImGui::PushFont(fonts().semibold, 15.0f);
    ImGui::TextUnformatted(title);
    ImGui::PopFont();
    ImGui::PushFont(fonts().regular, 13.0f);
    ImGui::TextColored(toVec4(p.text2), "%s", desc);
    ImGui::PopFont();
    ImGui::PopTextWrapPos();
    ImGui::EndGroup();
    ImGui::SameLine(x0 + w - controlW);
    ImGui::SetCursorPosY(y0 + 4.0f * sc);
    ImGui::SetNextItemWidth(controlW);
}

} // namespace

void EchoApp::drawSettings() {
    const Palette& p = pal();
    const float sc = uiScale();
    Session& s = mc_->session();
    if (!pendingEngineInit_) {
        pendingEngine_ = s.engine;
        pendingEngineInit_ = true;
    }
    pop::PageTitle("Settings");
    ImGui::Dummy(ImVec2(0, 2.0f * sc));
    const float avail = ImGui::GetContentRegionAvail().x;
    const float navW = 200.0f * sc, gap = 24.0f * sc;
    ImGui::BeginGroup();
    for (int i = 0; i < static_cast<int>(SettingsTab::Count); ++i)
        if (sideTab(kTabNames[i], static_cast<int>(settingsTab_) == i, navW)) settingsTab_ = static_cast<SettingsTab>(i);
    ImGui::EndGroup();
    ImGui::SameLine(0, gap);
    const float contentW = std::min(avail - navW - gap, 820.0f * sc);
    ImGui::BeginChild("##settingsbody", ImVec2(contentW, 0), ImGuiChildFlags_AutoResizeY, ImGuiWindowFlags_NoScrollbar);

    switch (settingsTab_) {
    case SettingsTab::Audio: {
        pop::BeginCard("##engine");
        pop::CardTitle("Audio engine");
        Engine& e = mc_->engine();
        char line[200];
        std::snprintf(line, sizeof(line), "%s · %d Hz · %d samples (%.1f ms) · %s", e.running() ? "Running" : "Stopped",
                      e.sampleRate(), e.blockSize(), e.blockSeconds() * 1000.0, mc_->clockDescription().c_str());
        pop::Caption(line);
        if (pop::SmallButton(e.running() ? "Stop audio" : "Start audio", e.running() ? pop::Btn::Secondary : pop::Btn::Primary)) {
            if (e.running()) mc_->stop();
            else mc_->start();
        }
        ImGui::SameLine();
        if (pop::SmallButton("Restart audio")) mc_->restart();
        ImGui::SameLine();
        if (pop::SmallButton("Rescan devices")) mc_->rescanDevices();
        pop::EndCard();

        ImGui::Dummy(ImVec2(0, 4.0f * sc));
        pop::BeginCard("##quality", 0.0f, 0.0f, false, 20.0f, 12.0f);
        pop::CardTitle("Quality and delay");
        ImGui::Dummy(ImVec2(0, 2.0f * sc));
        const float comboW = 240.0f * sc;
        const int rates[] = {44100, 48000, 88200, 96000};
        settingRow("Sample rate", "48 kHz suits Discord, games and most headsets. Devices at other rates are converted.", comboW);
        if (ImGui::BeginCombo("##rate", (std::to_string(pendingEngine_.sampleRate) + " Hz").c_str())) {
            for (int r : rates)
                if (ImGui::Selectable((std::to_string(r) + " Hz").c_str(), r == pendingEngine_.sampleRate)) pendingEngine_.sampleRate = r;
            ImGui::EndCombo();
        }
        pop::Divider();
        const int blocks[] = {64, 128, 256, 480, 512, 1024, 2048};
        char cur[48];
        std::snprintf(cur, sizeof(cur), "%d samples (%.1f ms)", pendingEngine_.blockSize,
                      pendingEngine_.blockSize * 1000.0 / pendingEngine_.sampleRate);
        settingRow("Buffer size", "Smaller means less delay but needs more CPU. If you hear crackles, go bigger.", comboW);
        if (ImGui::BeginCombo("##block", cur)) {
            for (int b : blocks) {
                char lbl[48];
                std::snprintf(lbl, sizeof(lbl), "%d samples (%.1f ms)", b, b * 1000.0 / pendingEngine_.sampleRate);
                if (ImGui::Selectable(lbl, b == pendingEngine_.blockSize)) pendingEngine_.blockSize = b;
            }
            ImGui::EndCombo();
        }
        pop::Divider();
        int q = static_cast<int>(pendingEngine_.resamplerQuality);
        const char* qs[] = {"Low (16 taps)", "Normal (32 taps)", "High (64 taps)"};
        settingRow("Resampler quality", "Used when a device runs at a different rate.", comboW);
        if (ImGui::Combo("##resq", &q, qs, 3)) pendingEngine_.resamplerQuality = static_cast<ResamplerQuality>(q);
        pop::Divider(8.0f, 0.0f);
        pop::SwitchRow("Low-latency shared mode", "Asks Windows for the smallest buffer the driver allows. Not every driver supports it.",
                       &pendingEngine_.lowLatencyShared);
        pop::SwitchRow("Feedback protection", "Blocks a route that would loop sound back into itself.", &pendingEngine_.feedbackProtection,
                       false);
        const bool changed = pendingEngine_.sampleRate != s.engine.sampleRate || pendingEngine_.blockSize != s.engine.blockSize ||
                             pendingEngine_.resamplerQuality != s.engine.resamplerQuality ||
                             pendingEngine_.lowLatencyShared != s.engine.lowLatencyShared ||
                             pendingEngine_.feedbackProtection != s.engine.feedbackProtection;
        ImGui::Dummy(ImVec2(0, 4.0f * sc));
        ImGui::BeginDisabled(!changed);
        if (pop::Button("Apply and restart audio", changed ? pop::Btn::Primary : pop::Btn::Secondary)) {
            const uint32_t clock = s.engine.clockBusId;
            s.engine = pendingEngine_;
            s.engine.clockBusId = clock;
            mc_->restart();
            toast("Audio restarted with the new settings", 1);
        }
        ImGui::EndDisabled();
        if (changed) {
            ImGui::SameLine();
            if (pop::Button("Revert")) pendingEngine_ = s.engine;
        }
        ImGui::Dummy(ImVec2(0, 4.0f * sc));
        pop::EndCard();

        ImGui::Dummy(ImVec2(0, 4.0f * sc));
        pop::BeginCard("##mics", 0.0f, 0.0f, false, 20.0f, 6.0f);
        bool autoNs = s.autoNoiseSuppression;
        if (pop::SwitchRow("Add noise removal to new microphones", "You can still turn it off for any microphone.", &autoNs, false)) {
            s.autoNoiseSuppression = autoNs;
            mc_->markDirty();
        }
        pop::EndCard();
        ImGui::Dummy(ImVec2(0, 2.0f * sc));
        pop::Hint("Gain staging: keep input peaks around -12 dBFS. Every output has a safety limiter; if it is often "
                  "active, lower an input's gain or volume instead.");
        break;
    }
    case SettingsTab::Hotkeys:
        drawHotkeySettings();
        break;
    case SettingsTab::Appearance: {
        pop::BeginCard("##look", 0.0f, 0.0f, false, 20.0f, 14.0f);
        pop::Label("Theme");
        int theme = std::clamp(s.ui.theme, 0, 2);
        const char* themes[] = {"Light", "Dark", "Match Windows"};
        if (pop::Segmented("##theme", &theme, themes, 3, 420.0f * sc)) {
            s.ui.theme = theme;
            setDpiScale(dpi_);
        }
        pop::Hint("Match Windows follows Settings > Personalization > Colors > app mode.");
        pop::Divider();
        pop::Label("Accent colour");
        for (int i = 0; i < 4; ++i) {
            if (i) ImGui::SameLine(0, 14.0f * sc);
            const float d = 36.0f * sc;
            const ImVec2 pos = ImGui::GetCursorScreenPos();
            ImGui::PushID(i);
            if (ImGui::InvisibleButton("##accent", ImVec2(d + 8.0f * sc, d + 8.0f * sc))) {
                s.ui.accent = kAccentNames[i];
                setDpiScale(dpi_);
            }
            Tip(kAccentLabels[i]);
            ImGui::PopID();
            ImDrawList* dl = ImGui::GetWindowDrawList();
            const ImVec2 c(pos.x + d * 0.5f + 4.0f * sc, pos.y + d * 0.5f + 4.0f * sc);
            if (s.ui.accent == kAccentNames[i]) dl->AddCircle(c, d * 0.5f + 4.0f * sc, p.outline, 32, 2.0f * sc);
            dl->AddCircleFilled(c, d * 0.5f, accentColor(kAccentNames[i]), 32);
            dl->AddCircle(c, d * 0.5f, p.outline, 32, 1.5f * sc);
        }
        pop::Divider();
        float scale = s.ui.uiScale * 100.0f;
        char v[16];
        std::snprintf(v, sizeof(v), "%.0f%%", scale);
        if (pop::SliderRow("Interface size", v, "##scale", &scale, 75.0f, 200.0f, 100.0f, 420.0f * sc))
            s.ui.uiScale = std::round(scale / 5.0f) * 0.05f;
        if (ImGui::IsItemDeactivated()) setDpiScale(dpi_);
        pop::Hint("On top of Windows display scaling. Applied when you let go.");
        pop::Divider(8.0f, 0.0f);
        if (pop::SwitchRow("Show tips when hovering", nullptr, &s.ui.showTooltips, false)) setTooltipsEnabled(s.ui.showTooltips);
        pop::EndCard();
        break;
    }
    case SettingsTab::Startup: {
        pop::BeginCard("##startup", 0.0f, 0.0f, false, 20.0f, 6.0f);
        bool startup = startupEnabled();
        if (pop::SwitchRow("Start Echo when I sign in to Windows", "Starts minimized, so your virtual mic is ready.", &startup))
            setStartup(startup);
        pop::SwitchRow("Keep running next to the clock when closed",
                       "Your mic, voice and hotkeys keep working. Right-click the Echo icon to open, mute or quit.",
                       &s.ui.closeToTray, false);
        pop::EndCard();
        ImGui::Dummy(ImVec2(0, 4.0f * sc));
        pop::BeginCard("##setupagain");
        pop::CardTitle("Setup guide");
        pop::Caption("Pick your microphone, headphones and virtual mic again (F1).");
        if (pop::Button("Run setup again")) openWizard();
        pop::EndCard();
        ImGui::Dummy(ImVec2(0, 4.0f * sc));
        pop::BeginCard("##data");
        pop::CardTitle("Your settings");
        pop::Hint(("Saved automatically in " + wideToUtf8(paths::appDataDir())).c_str());
        if (pop::SmallButton("Open settings folder"))
            ShellExecuteW(nullptr, L"open", paths::appDataDir().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        ImGui::SameLine();
        if (pop::SmallButton("Open log folder"))
            ShellExecuteW(nullptr, L"open", (paths::localDataDir() + L"\\logs").c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        ImGui::SameLine();
        if (pop::SmallButton("Save now")) {
            saveNow();
            toast("Settings saved", 1);
        }
        pop::Divider();
        if (pop::SmallButton("Export settings")) {
            const std::wstring file = paths::appDataDir() + L"\\session-export.json";
            std::string err;
            if (saveJsonFile(file, sessionToJson(s), &err)) {
                toast("Exported to " + wideToUtf8(file), 1);
                ShellExecuteW(nullptr, L"open", paths::appDataDir().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            } else {
                toast("Export failed: " + err, 3);
            }
        }
        Tip("Writes session-export.json into the settings folder.");
        ImGui::SameLine();
        if (pop::SmallButton("Import settings")) {
            json j;
            std::string err;
            Session in;
            std::vector<std::string> warn;
            if (loadJsonFile(paths::appDataDir() + L"\\session-import.json", j, &err, false) && sessionFromJson(j, in, &warn)) {
                mc_->replaceSession(std::move(in));
                pendingEngineInit_ = false;
                toast("Settings imported", 1);
            } else {
                toast("Put a settings file named session-import.json in the settings folder first.", 2);
            }
        }
        Tip("Reads session-import.json from the settings folder.");
        ImGui::SameLine();
        if (pop::SmallButton("Reset to defaults...", pop::Btn::Danger)) ImGui::OpenPopup("##reset");
        if (ImGui::BeginPopupModal("##reset", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar)) {
            pop::CardTitle("Reset everything to the defaults?");
            pop::Caption("Inputs, outputs, routing and effects go back to the defaults. Your presets, recordings and models are kept.");
            if (pop::Button("Reset", pop::Btn::Primary)) {
                mc_->createDefaultSession();
                mc_->session().firstRunCompleted = true;
                pendingEngineInit_ = false;
                toast("Settings reset to the defaults", 1);
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (pop::Button("Cancel")) ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }
        pop::EndCard();
        break;
    }
    case SettingsTab::Privacy: {
        pop::BeginCard("##privacy");
        pop::IconBox(ICON_CHECK, p.goodTint, 40.0f, p.good);
        ImGui::SameLine(0, 12.0f * sc);
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 8.0f * sc);
        pop::CardTitle("Everything stays on this PC");
        ImGui::Dummy(ImVec2(0, 2.0f * sc));
        ImGui::BulletText("No accounts, subscriptions, ads or telemetry.");
        ImGui::BulletText("Echo makes no network connections. Audio is processed only on this computer.");
        ImGui::BulletText("Nothing is recorded unless you press Record.");
        ImGui::BulletText("AI voice uses ONNX Runtime only if you install it, with models you add yourself.");
        ImGui::BulletText("Logs are plain text files in your app data folder, for fixing problems.");
        pop::EndCard();
        break;
    }
    case SettingsTab::Diagnostics:
        pop::BeginCard("##diag", 0.0f, 0.0f, true);
        drawDiagnostics();
        pop::EndCard();
        break;
    case SettingsTab::About:
        drawAbout();
        break;
    default:
        break;
    }
    ImGui::EndChild();
}

} // namespace echo::ui
