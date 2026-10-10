// Apps: programs that have sound open, their Windows volume, and one-click
// capture into Echo with per-output sends.
#include <windows.h>
#include <shellapi.h>

#include <cstdio>

#include "imgui.h"
#include "ui/App.h"
#include "ui/Icons.h"
#include "ui/Pop.h"
#include "ui/Theme.h"
#include "ui/Widgets.h"

namespace echo::ui {

void EchoApp::drawApps() {
    appsVisible_ = true;
    const Palette& p = pal();
    const float sc = uiScale();
    pop::PageTitle("Apps", "Programs show up here while they have sound open. Capture one to put it in your mic or headphones.");
    if (!backend_->processLoopbackSupported())
        pop::Banner(ICON_WARNING, "Capturing single apps needs Windows 10 version 2004 or newer.", p.warnTint, p.warn);
    ImGui::Dummy(ImVec2(0, 2.0f * sc));

    auto sessions = backend_->sessions().sessions();
    if (now_ - lastAppOutputRefresh_ > 1.0 && appRouting_ && appRouting_->available()) {
        lastAppOutputRefresh_ = now_;
        appOutputCache_.clear();
        for (const auto& s : sessions)
            if (!s.systemSounds) appOutputCache_[s.pid] = appRouting_->appOutput(s.pid);
    }
    if (sessions.empty()) {
        pop::BeginCard("##noapps", 0.0f, 0.0f, true);
        pop::CardTitle("No apps have sound open right now");
        pop::Caption("Start playing something in an app (a game, a browser tab, music) and it appears here.");
        pop::EndCard();
    }
    const auto renders = backend_->deviceManager().devices(win::Flow::Render, true);
    int row = 0;
    int systemCount = 0;
    for (const auto& s : sessions) {
        if (s.systemSounds) {
            ++systemCount;
            continue;
        }
        ImGui::PushID(row++);
        StripModel* captured = nullptr;
        if (!s.systemSounds)
            for (auto& st : mc_->session().strips)
                if (st.source.kind == SourceKind::ProcessLoopback && _stricmp(st.source.processName.c_str(), s.exeName.c_str()) == 0)
                    captured = &st;
        pop::BeginCard("##app", 0.0f, 0.0f, captured == nullptr, 18.0f, 14.0f);
        const float inner = ImGui::GetContentRegionAvail().x;
        const float rightW = 330.0f * sc;
        const float midW = 260.0f * sc;
        const float leftW = std::max(160.0f * sc, inner - rightW - midW - 40.0f * sc);

        // App: icon, name, level.
        const ImVec2 rp = ImGui::GetCursorScreenPos();
        const float isz = 40.0f * sc;
        if (ImTextureID tex = textures_.iconForExe(s.exePath)) {
            ImGui::GetWindowDrawList()->AddRectFilled(rp, ImVec2(rp.x + isz, rp.y + isz), p.segBg, 10.0f * sc);
            ImGui::SetCursorScreenPos(ImVec2(rp.x + 6.0f * sc, rp.y + 6.0f * sc));
            ImGui::Image(ImTextureRef(tex), ImVec2(isz - 12.0f * sc, isz - 12.0f * sc));
            ImGui::SetCursorScreenPos(rp);
            ImGui::Dummy(ImVec2(isz, isz));
        } else {
            pop::IconBox(s.systemSounds ? ICON_VOLUME : ICON_APPS, p.segBg, 40.0f);
        }
        ImGui::SameLine(0, 12.0f * sc);
        ImGui::BeginGroup();
        ImGui::PushFont(fonts().semibold, 15.0f);
        TextEllipsis(s.displayName, leftW - isz - 12.0f * sc);
        ImGui::PopFont();
        ImGui::PushFont(fonts().regular, 12.0f);
        ImGui::TextColored(toVec4(captured ? p.goodInk : p.textDim), "%s",
                           s.systemSounds ? "Windows system sounds" : captured ? "In Echo" : s.active ? "Playing" : "Open, quiet");
        ImGui::PopFont();
        pop::BarMeter(gainToDb(s.peak), std::min(200.0f * sc, leftW - isz - 12.0f * sc), 8.0f * sc);
        ImGui::EndGroup();

        // Windows: volume, mute, which device it plays on.
        ImGui::SameLine(leftW + 20.0f * sc + ImGui::GetStyle().WindowPadding.x);
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() - 2.0f * sc);
        ImGui::BeginGroup();
        ImGui::PushFont(fonts().regular, 12.0f);
        ImGui::TextColored(toVec4(p.textDim), "In Windows · plays on %s", s.deviceName.c_str());
        ImGui::PopFont();
        float vol = s.volume;
        if (pop::Slider("##vol", &vol, 0.0f, 1.0f, 1.0f, midW - 96.0f * sc, p.primary))
            backend_->sessions().setVolume(s.pid, s.deviceId, vol);
        Tip("This app's volume in Windows (the same as Windows' volume mixer).");
        ImGui::SameLine(0, 6.0f * sc);
        char pct[16];
        std::snprintf(pct, sizeof(pct), "%.0f%%", s.volume * 100.0f);
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 3.0f * sc);
        ImGui::PushFont(fonts().regular, 13.0f);
        ImGui::TextUnformatted(pct);
        ImGui::PopFont();
        ImGui::SameLine(0, 6.0f * sc);
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() - 3.0f * sc);
        bool muted = s.muted;
        if (pop::Chip(muted ? "Muted##m" : "Mute##m", muted)) backend_->sessions().setMute(s.pid, s.deviceId, !muted);
        Tip("Mutes the app in Windows. This also silences Echo's copy of it.");
        ImGui::EndGroup();

        // Echo: capture, or where the captured app goes.
        ImGui::SameLine(inner - rightW + ImGui::GetStyle().WindowPadding.x + 18.0f * sc);
        ImGui::BeginGroup();
        if (s.systemSounds) {
            pop::Hint("System sounds can't be captured on their own.");
        } else if (captured) {
            for (const auto& b : mc_->session().buses) {
                SendParams& sp = captured->rt->sends[b.slot];
                const bool on = sp.enabled.load();
                const std::string lbl = (b.kind == BusKind::Virtual ? "In your mic (" : "In " + b.name + " (") + busLabel(b) + ")##s" + std::to_string(b.id);
                if (pop::SendPill(lbl.c_str(), on, b.kind == BusKind::Virtual, 0.0f, sp.blocked.load())) mc_->setSend(captured->id, b.id, !on);
                ImGui::SameLine(0, 6.0f * sc);
            }
            const uint32_t id = captured->id;
            if (pop::SmallButton("Remove", pop::Btn::Quiet)) defer([this, id]() { mc_->removeStrip(id); });
        } else {
            ImGui::BeginDisabled(!backend_->processLoopbackSupported());
            if (pop::Button("Capture", pop::Btn::Primary)) {
                SourceRef r;
                r.kind = SourceKind::ProcessLoopback;
                r.processId = s.pid;
                r.processName = s.exeName;
                const uint32_t id = mc_->addStrip(StripGroup::Application, r, s.displayName);
                if (id) toast("Capturing " + s.displayName + ". Choose where it goes with its buttons.", 1);
            }
            ImGui::EndDisabled();
        }
        if (!s.systemSounds && appRouting_ && appRouting_->available()) {
            ImGui::SameLine(0, 4.0f * sc);
            if (ImGui::SmallButton(ICON_MORE)) ImGui::OpenPopup("##appmenu");
            Tip("Which device Windows plays this app on (experimental)");
            if (ImGui::BeginPopup("##appmenu")) {
                const std::string cur = appOutputCache_[s.pid];
                ImGui::TextDisabled("Windows output for %s (experimental)", s.displayName.c_str());
                ImGui::Separator();
                std::string chosen, chosenName = "the default output";
                bool pick = false;
                if (ImGui::MenuItem("System default", nullptr, cur.empty())) pick = true;
                for (const auto& d : renders) {
                    const std::string n = d.name + (d.virtualKind == win::VirtualKind::Cable ? "  [virtual cable]" : "");
                    if (ImGui::MenuItem(n.c_str(), nullptr, d.idUtf8 == cur)) {
                        chosen = d.idUtf8;
                        chosenName = d.name;
                        pick = true;
                    }
                }
                if (pick) {
                    std::string err;
                    if (appRouting_->setAppOutput(s.pid, chosen, &err)) {
                        appOutputCache_[s.pid] = chosen;
                        toast("Windows now plays " + s.displayName + " on " + chosenName, 1);
                    } else {
                        toast("Could not change the app's output: " + err, 3);
                    }
                }
                ImGui::EndPopup();
            }
        }
        ImGui::EndGroup();
        pop::EndCard();
        ImGui::PopID();
    }

    if (systemCount > 0) {
        ImGui::Dummy(ImVec2(0, 2.0f * sc));
        char title[64];
        std::snprintf(title, sizeof(title), "Windows system sounds (%d)", systemCount);
        if (ImGui::TreeNode(title)) {
            pop::Hint("Notification sounds, one entry per output device. They can't be captured on their own.");
            int i = 0;
            for (const auto& s : sessions) {
                if (!s.systemSounds) continue;
                ImGui::PushID(1000 + i++);
                ImGui::AlignTextToFramePadding();
                ImGui::TextUnformatted(s.deviceName.c_str());
                ImGui::SameLine(360.0f * sc);
                float vol = s.volume;
                if (pop::Slider("##svol", &vol, 0.0f, 1.0f, 1.0f, 200.0f * sc, p.primary))
                    backend_->sessions().setVolume(s.pid, s.deviceId, vol);
                ImGui::SameLine();
                bool muted = s.muted;
                if (pop::Chip(muted ? "Muted##sm" : "Mute##sm", muted)) backend_->sessions().setMute(s.pid, s.deviceId, !muted);
                ImGui::PopID();
            }
            ImGui::TreePop();
        }
    }
    ImGui::Dummy(ImVec2(0, 4.0f * sc));
    pop::Banner(ICON_INFO,
                "Echo copies an app's sound; the app also keeps playing through Windows as usual, so you normally leave "
                "your headphones off here. Muting an app in Windows also silences Echo's copy. To hear an app only "
                "through Echo, use the app's ... menu to play it on a device you don't listen to.",
                p.infoTint, p.link);
    if (ImGui::TreeNode("How app capture works")) {
        ImGui::BulletText("Capture records what the app plays (including its child processes), on every device it uses.");
        ImGui::BulletText("Echo never captures itself, so capturing \"all apps\" can't feed back.");
        ImGui::BulletText("Some protected video streams may arrive as silence.");
        ImGui::BulletText("The app's Windows output uses an undocumented Windows interface (experimental). Some apps only "
                          "switch after they restart playback. Choose \"System default\" to undo.");
        if (pop::SmallButton("Open Windows' app volume settings"))
            ShellExecuteW(nullptr, L"open", L"ms-settings:apps-volume", nullptr, nullptr, SW_SHOWNORMAL);
        ImGui::TreePop();
    }
}

} // namespace echo::ui
