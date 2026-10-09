// Applications: per-app audio sessions with volume/mute/level, and one-click
// per-application capture into the mixer.
#include <windows.h>
#include <shellapi.h>

#include "imgui.h"
#include "ui/App.h"
#include "ui/Icons.h"
#include "ui/Theme.h"
#include "ui/Widgets.h"

namespace echo::ui {

void EchoApp::drawApps() {
    if (!ImGui::Begin("Applications")) {
        ImGui::End();
        return;
    }
    appsVisible_ = true;
    const Palette& p = pal();
    if (!backend_->processLoopbackSupported())
        ImGui::TextColored(toVec4(p.warn), ICON_WARNING " Per-application capture requires Windows 10 version 2004 or newer.");
    ImGui::TextWrapped("Applications currently using audio. 'Capture' adds the app as an input strip, so you can "
                       "process it and send it to any bus (for example music into your virtual microphone).");
    if (ImGui::CollapsingHeader("How app capture works and its limits")) {
        ImGui::BulletText("Capture records what the app renders (including its child processes), on every device it uses.");
        ImGui::BulletText("The app keeps playing on its own output too. To hear it only through Echo, use 'App output' "
                          "to send the app to a device you don't listen to (for example a virtual cable), then "
                          "capture the app or that cable in Echo.");
        ImGui::BulletText("'App output' changes the same per-app setting as Windows Settings > Sound > Volume mixer. "
                          "It uses an undocumented Windows interface (experimental). Some apps only switch after they "
                          "restart playback. Choose 'System default' to undo.");
        ImGui::BulletText("Muting the app in the Windows volume mixer also silences Echo's capture of it "
                          "(verified on this Windows build by echo_hwtest).");
        ImGui::BulletText("DRM-protected streams (some video services) may be delivered as silence by Windows.");
        ImGui::BulletText("Echo never captures itself, so capturing 'all apps' cannot feed back.");
        if (ImGui::Button("Open Windows app volume & device settings"))
            ShellExecuteW(nullptr, L"open", L"ms-settings:apps-volume", nullptr, nullptr, SW_SHOWNORMAL);
    }
    ImGui::Separator();
    auto sessions = backend_->sessions().sessions();
    if (sessions.empty()) {
        ImGui::TextDisabled("No applications have an audio session right now. Start playing audio in an app.");
        ImGui::End();
        return;
    }
    if (now_ - lastAppOutputRefresh_ > 1.0 && appRouting_ && appRouting_->available()) {
        lastAppOutputRefresh_ = now_;
        appOutputCache_.clear();
        for (const auto& s : sessions)
            if (!s.systemSounds) appOutputCache_[s.pid] = appRouting_->appOutput(s.pid);
    }
    const auto renders = backend_->deviceManager().devices(win::Flow::Render, true);
    if (ImGui::BeginTable("##apps", 7, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_ScrollY |
                                           ImGuiTableFlags_Resizable)) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Application", ImGuiTableColumnFlags_WidthStretch, 2.0f);
        ImGui::TableSetupColumn("Playing on", ImGuiTableColumnFlags_WidthStretch, 1.6f);
        ImGui::TableSetupColumn("Level", ImGuiTableColumnFlags_WidthFixed, 110 * dpi_);
        ImGui::TableSetupColumn("Volume", ImGuiTableColumnFlags_WidthFixed, 150 * dpi_);
        ImGui::TableSetupColumn("Mute", ImGuiTableColumnFlags_WidthFixed, 50 * dpi_);
        ImGui::TableSetupColumn("App output", ImGuiTableColumnFlags_WidthStretch, 1.4f);
        ImGui::TableSetupColumn("In Echo", ImGuiTableColumnFlags_WidthFixed, 130 * dpi_);
        ImGui::TableHeadersRow();
        int row = 0;
        for (const auto& s : sessions) {
            ImGui::PushID(row++);
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            const float isz = ImGui::GetFontSize() * 1.3f;
            ImTextureID tex = textures_.iconForExe(s.exePath);
            if (tex) ImGui::Image(ImTextureRef(tex), ImVec2(isz, isz));
            else ImGui::Dummy(ImVec2(isz, isz));
            ImGui::SameLine();
            Led(s.active, p.good, ImGui::GetFontSize() * 0.25f);
            ImGui::SameLine();
            ImGui::TextUnformatted(s.displayName.c_str());
            if (!s.exeName.empty() && s.exeName != s.displayName) {
                ImGui::SameLine();
                ImGui::TextDisabled("%s", s.exeName.c_str());
            }
            ImGui::TableNextColumn();
            TextEllipsis(s.deviceName, ImGui::GetContentRegionAvail().x, p.textDim);
            ImGui::TableNextColumn();
            MiniMeter(gainToDb(s.peak), ImVec2(100 * dpi_, ImGui::GetFontSize() * 0.6f));
            ImGui::TableNextColumn();
            float vol = s.volume * 100.0f;
            ImGui::SetNextItemWidth(-1);
            if (ImGui::SliderFloat("##vol", &vol, 0.0f, 100.0f, "%.0f%%"))
                backend_->sessions().setVolume(s.pid, s.deviceId, vol / 100.0f);
            Tip("Windows per-app volume for this app on this device");
            ImGui::TableNextColumn();
            bool muted = s.muted;
            if (ImGui::Checkbox("##mute", &muted)) backend_->sessions().setMute(s.pid, s.deviceId, muted);
            Tip("Windows per-app mute (also silences Echo's capture of this app)");
            ImGui::TableNextColumn();
            if (s.systemSounds || !appRouting_ || !appRouting_->available()) {
                if (!s.systemSounds && ImGui::SmallButton("Windows settings"))
                    ShellExecuteW(nullptr, L"open", L"ms-settings:apps-volume", nullptr, nullptr, SW_SHOWNORMAL);
            } else {
                const std::string cur = appOutputCache_[s.pid];
                std::string label = "System default";
                for (const auto& d : renders)
                    if (d.idUtf8 == cur) label = d.name;
                ImGui::SetNextItemWidth(-1);
                if (ImGui::BeginCombo("##appout", label.c_str())) {
                    std::string chosen;
                    bool pick = false;
                    if (ImGui::Selectable("System default", cur.empty())) {
                        pick = true;
                    }
                    for (const auto& d : renders) {
                        std::string n = d.name + (d.virtualKind == win::VirtualKind::Cable ? "  [virtual cable]" : "");
                        if (ImGui::Selectable(n.c_str(), d.idUtf8 == cur)) {
                            chosen = d.idUtf8;
                            pick = true;
                        }
                    }
                    if (pick) {
                        std::string err;
                        if (appRouting_->setAppOutput(s.pid, chosen, &err)) {
                            appOutputCache_[s.pid] = chosen;
                            toast("Sent " + s.displayName + " to " + (chosen.empty() ? std::string("the system default output") : label), 1);
                        } else {
                            toast("Could not change the app's output: " + err, 3);
                        }
                    }
                    ImGui::EndCombo();
                }
                Tip("Experimental: which output device this app plays to (same as Windows Volume mixer).");
            }
            ImGui::TableNextColumn();
            if (s.systemSounds) {
                ImGui::TextDisabled("system");
            } else {
                bool captured = false;
                for (const auto& st : mc_->session().strips)
                    if (st.source.kind == SourceKind::ProcessLoopback && _stricmp(st.source.processName.c_str(), s.exeName.c_str()) == 0)
                        captured = true;
                if (captured) {
                    ImGui::TextColored(toVec4(p.good), ICON_CHECK " Captured");
                } else {
                    ImGui::BeginDisabled(!backend_->processLoopbackSupported());
                    if (ImGui::SmallButton(ICON_ADD " Capture")) {
                        SourceRef r;
                        r.kind = SourceKind::ProcessLoopback;
                        r.processId = s.pid;
                        r.processName = s.exeName;
                        const uint32_t id = mc_->addStrip(StripGroup::Application, r, s.displayName);
                        if (id) toast("Capturing " + s.displayName + ". Route it with the bus buttons on its strip.", 1);
                    }
                    ImGui::EndDisabled();
                }
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    ImGui::End();
}

} // namespace echo::ui
