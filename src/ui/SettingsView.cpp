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
        std::snprintf(line, sizeof(line), "%s · %d Hz · %s buffer · %s", e.running() ? "Running" : "Stopped", e.sampleRate(),
                      bufferSizeName(e.blockSize(), e.sampleRate()).c_str(), mc_->clockDescription().c_str());
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
        const std::string cur = bufferSizeName(pendingEngine_.blockSize, pendingEngine_.sampleRate);
        settingRow("Buffer size", "Smaller means less delay but needs more CPU. If you hear crackles, go bigger.", comboW);
        if (ImGui::BeginCombo("##block", cur.c_str())) {
            for (int b : blocks) {
                ImGui::PushID(b);
                if (ImGui::Selectable(bufferSizeName(b, pendingEngine_.sampleRate).c_str(), b == pendingEngine_.blockSize))
                    pendingEngine_.blockSize = b;
                ImGui::PopID();
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
        {
            // A small preview of every theme: its background, a card with
            // text, and the accent colour.
            const ThemeColors mine{s.ui.customBackground, s.ui.customSurface, s.ui.customText};
            const float avail = ImGui::GetContentRegionAvail().x, gap = 12.0f * sc;
            const int cols = pop::columnsFor(avail, 118.0f * sc, gap);
            const float tw = (avail - gap * (cols - 1)) / cols, ph = 62.0f * sc, th = ph + 30.0f * sc;
            for (int t = 0; t < kThemeCount; ++t) {
                if (t % cols) ImGui::SameLine(0, gap);
                ImGui::PushID(t);
                const ImVec2 pos = ImGui::GetCursorScreenPos();
                if (ImGui::InvisibleButton("##theme", ImVec2(tw, th))) {
                    s.ui.theme = t;
                    setDpiScale(dpi_);
                }
                const bool hov = ImGui::IsItemHovered();
                if (hov) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
                ImGui::PopID();
                ImDrawList* dl = ImGui::GetWindowDrawList();
                const float r = 10.0f * sc;
                auto preview = [&](const ThemeColors& c, ImVec2 a, ImVec2 b) {
                    dl->AddRectFilled(a, b, hex(c.background), r);
                    const ImVec2 ca(a.x + 10.0f * sc, a.y + 10.0f * sc), cb(b.x - 10.0f * sc, b.y - 8.0f * sc);
                    if (cb.x > ca.x + 8.0f * sc) {
                        dl->AddRectFilled(ca, cb, hex(c.surface), 6.0f * sc);
                        const float w = cb.x - ca.x;
                        dl->AddRectFilled(ImVec2(ca.x + 8.0f * sc, ca.y + 9.0f * sc), ImVec2(ca.x + w * 0.62f, ca.y + 14.0f * sc),
                                          hex(c.text), 2.0f * sc);
                        dl->AddRectFilled(ImVec2(ca.x + 8.0f * sc, ca.y + 20.0f * sc), ImVec2(ca.x + w * 0.42f, ca.y + 24.0f * sc),
                                          withAlpha(hex(c.text), 0.45f), 2.0f * sc);
                    }
                };
                const ImVec2 pa = pos, pb(pos.x + tw, pos.y + ph);
                if (t == kThemeSystem) {
                    // Half light, half dark.
                    dl->PushClipRect(pa, ImVec2(pa.x + tw * 0.5f, pb.y), true);
                    preview(themeColors(kThemeLight, mine), pa, pb);
                    dl->PopClipRect();
                    dl->PushClipRect(ImVec2(pa.x + tw * 0.5f, pa.y), pb, true);
                    preview(themeColors(kThemeDark, mine), pa, pb);
                    dl->PopClipRect();
                } else {
                    preview(themeColors(t, mine), pa, pb);
                }
                dl->AddCircleFilled(ImVec2(pb.x - 22.0f * sc, pb.y - 18.0f * sc), 6.0f * sc, p.accent, 16);
                const bool on = s.ui.theme == t;
                dl->AddRect(pa, pb, on ? p.accent : hov ? p.border : p.lineSoft, r, (on ? 2.5f : 1.5f) * sc);
                ImGui::PushFont(on ? fonts().semibold : fonts().regular, 13.0f);
                dl->AddText(ImVec2(pa.x + 2.0f * sc, pb.y + 6.0f * sc), on ? p.text : p.text2, themeName(t));
                ImGui::PopFont();
            }
        }
        if (s.ui.theme == kThemeSystem) pop::Hint("Follows Windows: Settings > Personalization > Colors > app mode.");
        if (s.ui.theme == kThemeCustom) {
            // The user's own theme: three colours, the rest follows from them.
            ImGui::Dummy(ImVec2(0, 4.0f * sc));
            pop::Label("Your colours");
            auto colorRow = [&](const char* label, uint32_t* rgb) {
                float c[3] = {((*rgb >> 16) & 0xFF) / 255.0f, ((*rgb >> 8) & 0xFF) / 255.0f, (*rgb & 0xFF) / 255.0f};
                ImGui::SetNextItemWidth(220.0f * sc);
                if (ImGui::ColorEdit3(label, c, ImGuiColorEditFlags_DisplayHex | ImGuiColorEditFlags_NoOptions)) {
                    auto b = [](float v) { return static_cast<uint32_t>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f); };
                    *rgb = (b(c[0]) << 16) | (b(c[1]) << 8) | b(c[2]);
                    setDpiScale(dpi_);
                }
                if (ImGui::IsItemDeactivatedAfterEdit()) mc_->markDirty();
            };
            colorRow("Background", &s.ui.customBackground);
            colorRow("Cards", &s.ui.customSurface);
            colorRow("Text", &s.ui.customText);
            if (contrastRatio(s.ui.customText, s.ui.customSurface) < 4.5f)
                pop::TextColoredWrapped(p.warn, "The text may be hard to read on these cards; try a lighter or darker text colour.");
            if (pop::SmallButton("Start from a theme...")) ImGui::OpenPopup("##startfrom");
            if (ImGui::BeginPopup("##startfrom")) {
                for (int t = 0; t < kThemeCount; ++t) {
                    if (t == kThemeCustom || t == kThemeSystem) continue;
                    if (ImGui::Selectable(themeName(t))) {
                        const ThemeColors c = themeColors(t, ThemeColors{});
                        s.ui.customBackground = c.background;
                        s.ui.customSurface = c.surface;
                        s.ui.customText = c.text;
                        setDpiScale(dpi_);
                        mc_->markDirty();
                    }
                }
                ImGui::EndPopup();
            }
        }
        pop::Divider();
        pop::Label("Accent colour");
        {
            const float d = 34.0f * sc;
            for (int i = 0; i <= kAccentCount; ++i) {
                const bool custom = i == kAccentCount;
                const char* name = custom ? "custom" : kAccentNames[i];
                if (i) ImGui::SameLine(0, 10.0f * sc);
                const ImVec2 pos = ImGui::GetCursorScreenPos();
                ImGui::PushID(i);
                if (ImGui::InvisibleButton("##accent", ImVec2(d + 8.0f * sc, d + 8.0f * sc))) {
                    s.ui.accent = name;
                    setDpiScale(dpi_);
                    if (custom) ImGui::OpenPopup("##customaccent");
                }
                Tip(custom ? "Your own colour" : kAccentLabels[i]);
                if (custom && ImGui::BeginPopup("##customaccent")) {
                    float c[3] = {((s.ui.customAccent >> 16) & 0xFF) / 255.0f, ((s.ui.customAccent >> 8) & 0xFF) / 255.0f,
                                  (s.ui.customAccent & 0xFF) / 255.0f};
                    if (ImGui::ColorPicker3("##picker", c, ImGuiColorEditFlags_DisplayHex | ImGuiColorEditFlags_NoSidePreview)) {
                        auto b = [](float v) { return static_cast<uint32_t>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f); };
                        s.ui.customAccent = (b(c[0]) << 16) | (b(c[1]) << 8) | b(c[2]);
                        setDpiScale(dpi_);
                    }
                    if (ImGui::IsItemDeactivatedAfterEdit()) mc_->markDirty();
                    ImGui::EndPopup();
                }
                ImGui::PopID();
                ImDrawList* dl = ImGui::GetWindowDrawList();
                const ImVec2 c(pos.x + d * 0.5f + 4.0f * sc, pos.y + d * 0.5f + 4.0f * sc);
                if (s.ui.accent == name) dl->AddCircle(c, d * 0.5f + 4.0f * sc, p.outline, 32, 2.0f * sc);
                if (custom) {
                    // A colour wheel, filled with the chosen colour in the middle.
                    for (int k = 0; k < 12; ++k) {
                        const float a0 = k * 6.2831853f / 12.0f, a1 = (k + 1) * 6.2831853f / 12.0f;
                        float rr, gg, bb;
                        ImGui::ColorConvertHSVtoRGB(k / 12.0f, 0.75f, 0.95f, rr, gg, bb);
                        dl->PathArcTo(c, d * 0.5f, a0, a1, 6);
                        dl->PathLineTo(c);
                        dl->PathFillConvex(ImGui::ColorConvertFloat4ToU32(ImVec4(rr, gg, bb, 1.0f)));
                    }
                    dl->AddCircleFilled(c, d * 0.27f, accentColor("custom", s.ui.customAccent), 24);
                } else {
                    dl->AddCircleFilled(c, d * 0.5f, accentColor(name), 32);
                }
                dl->AddCircle(c, d * 0.5f, p.outline, 32, 1.5f * sc);
            }
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
        pop::EndCard();

        ImGui::Dummy(ImVec2(0, 4.0f * sc));
        pop::BeginCard("##resetcard");
        pop::CardTitle("Reset to defaults");
        pop::Caption("Put Echo's settings back the way they were when you installed it. You choose what to reset; your "
                     "presets, recordings, AI models and sound files are always kept.");
        if (pop::Button("Reset settings...", pop::Btn::Danger)) openResetPopup_ = true;
        pop::EndCard();

        // The confirmation, with what to reset (everything ticked).
        static ResetParts parts;
        if (openResetPopup_) {
            parts = ResetParts{};
            ImGui::OpenPopup("Reset settings");
            openResetPopup_ = false;
        }
        ImGui::SetNextWindowSize(ImVec2(600.0f * sc, 0.0f));
        if (ImGui::BeginPopupModal("Reset settings", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar)) {
            pop::CardTitle("Reset settings to the defaults?");
            pop::Caption("Choose what goes back to how it was when Echo was installed.");
            ImGui::Dummy(ImVec2(0, 4.0f * sc));
            const float w = ImGui::GetContentRegionAvail().x;
            pop::CheckRow("##rmixer", "Inputs, outputs and routing",
                          "Your inputs and outputs as at the start; voices and effects removed",
                          &parts.mixer, w);
            pop::CheckRow("##rkeys", "Hotkeys", "Push to talk, the mute key and voice hotkeys are cleared", &parts.hotkeys, w);
            pop::CheckRow("##rboard", "Soundboard", "Echo's built-in sounds, no keys (sounds you added stay in the folder)",
                          &parts.soundboard, w);
            pop::CheckRow("##raudio", "Audio quality and recording", "Sample rate, buffer size, quality, recording folder and format",
                          &parts.audio, w);
            pop::CheckRow("##rlook", "Look and behaviour",
                          "Theme, colour, size, tooltips, running when closed, starting with Windows",
                          &parts.look, w);
            pop::Hint("Always kept: your presets, recordings, AI models and sound files. This can't be undone; use Export "
                      "settings first if you might want them back.");
            ImGui::Dummy(ImVec2(0, 4.0f * sc));
            ImGui::BeginDisabled(!parts.any());
            if (pop::Button("Reset", pop::Btn::Danger)) {
                if (parts.look) setStartup(false);
                resetToDefaults(parts);
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndDisabled();
            ImGui::SameLine();
            if (pop::Button("Cancel")) ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }
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
        ImGui::BulletText("Audio is processed only on this computer, and nothing is uploaded.");
        ImGui::BulletText("Echo goes online only when you click Install VB-CABLE, Install the AI engine or Download ContentVec.");
        ImGui::BulletText("Nothing is recorded unless you press Record.");
        ImGui::BulletText("AI voices use Microsoft's ONNX Runtime only if you install it, with voices you add yourself.");
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
