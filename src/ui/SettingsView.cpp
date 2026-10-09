// Settings: audio engine, appearance, startup, data and privacy.
#include <windows.h>
#include <shellapi.h>

#include "Version.h"
#include "core/Text.h"
#include "imgui.h"
#include "session/Paths.h"
#include "ui/App.h"
#include "ui/Icons.h"
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

} // namespace

void EchoApp::drawSettings() {
    if (!ImGui::Begin("Settings")) {
        ImGui::End();
        return;
    }
    const Palette& p = pal();
    Session& s = mc_->session();
    if (!pendingEngineInit_) {
        pendingEngine_ = s.engine;
        pendingEngineInit_ = true;
    }
    if (ImGui::BeginTabBar("##settabs")) {
        if (ImGui::BeginTabItem("Audio engine")) {
            const int rates[] = {44100, 48000, 88200, 96000};
            ImGui::SetNextItemWidth(180 * dpi_);
            if (ImGui::BeginCombo("Sample rate", (std::to_string(pendingEngine_.sampleRate) + " Hz").c_str())) {
                for (int r : rates)
                    if (ImGui::Selectable((std::to_string(r) + " Hz").c_str(), r == pendingEngine_.sampleRate))
                        pendingEngine_.sampleRate = r;
                ImGui::EndCombo();
            }
            Tip("Internal processing rate. Devices running at other rates are resampled automatically. 48 kHz is "
                "recommended (Discord, games and most headsets use it).");
            const int blocks[] = {64, 128, 256, 480, 512, 1024, 2048};
            char cur[48];
            std::snprintf(cur, sizeof(cur), "%d samples (%.1f ms)", pendingEngine_.blockSize,
                          pendingEngine_.blockSize * 1000.0 / pendingEngine_.sampleRate);
            ImGui::SetNextItemWidth(180 * dpi_);
            if (ImGui::BeginCombo("Buffer size", cur)) {
                for (int b : blocks) {
                    char lbl[48];
                    std::snprintf(lbl, sizeof(lbl), "%d samples (%.1f ms)", b, b * 1000.0 / pendingEngine_.sampleRate);
                    if (ImGui::Selectable(lbl, b == pendingEngine_.blockSize)) pendingEngine_.blockSize = b;
                }
                ImGui::EndCombo();
            }
            Tip("Smaller buffers lower latency but need more CPU headroom. If you hear crackles, increase it.");
            int q = static_cast<int>(pendingEngine_.resamplerQuality);
            const char* qs[] = {"Low (16 taps)", "Normal (32 taps)", "High (64 taps)"};
            ImGui::SetNextItemWidth(180 * dpi_);
            if (ImGui::Combo("Resampler quality", &q, qs, 3)) pendingEngine_.resamplerQuality = static_cast<ResamplerQuality>(q);
            ImGui::Checkbox("Low-latency shared mode (Windows 10+ minimum device period)", &pendingEngine_.lowLatencyShared);
            Tip("Asks Windows for the smallest shared-mode period the driver supports. Not all drivers allow it.");
            ImGui::Checkbox("Feedback protection (mute sends that would create a loop)", &pendingEngine_.feedbackProtection);
            const bool changed = pendingEngine_.sampleRate != s.engine.sampleRate || pendingEngine_.blockSize != s.engine.blockSize ||
                                 pendingEngine_.resamplerQuality != s.engine.resamplerQuality ||
                                 pendingEngine_.lowLatencyShared != s.engine.lowLatencyShared ||
                                 pendingEngine_.feedbackProtection != s.engine.feedbackProtection;
            ImGui::BeginDisabled(!changed);
            if (ImGui::Button(ICON_CHECK " Apply and restart audio")) {
                const uint32_t clock = s.engine.clockBusId;
                s.engine = pendingEngine_;
                s.engine.clockBusId = clock;
                mc_->restart();
                toast("Audio engine restarted with the new settings", 1);
            }
            ImGui::EndDisabled();
            ImGui::SameLine();
            if (changed && ImGui::Button("Revert")) pendingEngine_ = s.engine;
            ImGui::Separator();
            ImGui::TextWrapped("Gain staging: keep input peaks around -12 dBFS (yellow region rarely, red never). Each "
                               "output bus has a safety limiter (Lim) that stops clipping; if its LIM indicator is "
                               "often active, lower your input gain or fader instead.");
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Appearance")) {
            int theme = s.ui.theme;
            const char* themes[] = {"Dark", "Midnight", "Light"};
            ImGui::SetNextItemWidth(180 * dpi_);
            if (ImGui::Combo("Theme", &theme, themes, 3)) {
                s.ui.theme = theme;
                setDpiScale(dpi_);
            }
            float sc = s.ui.uiScale;
            ImGui::SetNextItemWidth(180 * dpi_);
            if (ImGui::SliderFloat("Interface size", &sc, 0.75f, 2.0f, "%.2fx")) s.ui.uiScale = sc;
            if (ImGui::IsItemDeactivatedAfterEdit()) setDpiScale(dpi_);
            Tip("Scales on top of Windows display scaling.");
            ImGui::Checkbox("Compact mixer strips", &s.ui.compactStrips);
            if (ImGui::Checkbox("Show tooltips", &s.ui.showTooltips)) setTooltipsEnabled(s.ui.showTooltips);
            ImGui::TextDisabled("Window layout is saved automatically. View > Reset window layout restores the default.");
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Startup & data")) {
            bool startup = startupEnabled();
            if (ImGui::Checkbox("Start Echo when I sign in to Windows (minimized)", &startup)) setStartup(startup);
            Tip("Adds/removes an entry under your user's Run registry key. No administrator rights needed.");
            ImGui::Separator();
            ImGui::TextDisabled("Settings: %s", wideToUtf8(paths::appDataDir()).c_str());
            if (ImGui::Button(ICON_FOLDER " Open settings folder"))
                ShellExecuteW(nullptr, L"open", paths::appDataDir().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            ImGui::SameLine();
            if (ImGui::Button(ICON_FOLDER " Open log folder"))
                ShellExecuteW(nullptr, L"open", (paths::localDataDir() + L"\\logs").c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            ImGui::Separator();
            ImGui::TextColored(toVec4(p.good), ICON_CHECK " Privacy");
            ImGui::TextWrapped("Echo has no accounts, subscriptions, ads or telemetry and makes no network connections. "
                               "Audio is processed only on this computer and is recorded only when you press Record. "
                               "Optional ONNX Runtime (for AI voices) is loaded only if you install it; Echo turns off "
                               "its optional trace events.");
            ImGui::Separator();
            if (ImGui::Button("Run setup wizard again")) openWizard();
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    ImGui::End();
}

} // namespace echo::ui
