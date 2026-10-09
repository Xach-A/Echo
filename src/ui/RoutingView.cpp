// Routing: matrix of input strips x output buses, scene presets, feedback
// warnings, and a live signal-flow diagram.
#include <cmath>
#include <cstdio>

#include "imgui.h"
#include "session/Presets.h"
#include "ui/App.h"
#include "ui/Icons.h"
#include "ui/Theme.h"
#include "ui/Widgets.h"

namespace echo::ui {

void EchoApp::drawRouting() {
    if (!ImGui::Begin("Routing")) {
        ImGui::End();
        return;
    }
    const Palette& p = pal();
    Session& s = mc_->session();

    // Scenes.
    ImGui::TextDisabled("Routing scenes:");
    for (int i = 0; i < static_cast<int>(ScenePreset::Count); ++i) {
        ImGui::SameLine();
        const auto sp = static_cast<ScenePreset>(i);
        if (ImGui::Button(scenePresetName(sp))) toast(applyScenePreset(*mc_, sp), 1);
        Tip(scenePresetDescription(sp));
    }
    for (const auto& w : mc_->feedbackWarnings()) ImGui::TextColored(toVec4(p.warn), ICON_WARNING " %s", w.text.c_str());

    if (ImGui::BeginTabBar("##routingtabs")) {
        if (ImGui::BeginTabItem("Matrix")) {
            if (s.strips.empty() || s.buses.empty()) {
                ImGui::TextDisabled("Add inputs and buses in the Mixer to route audio.");
            } else if (ImGui::BeginTable("##matrix", static_cast<int>(s.buses.size()) + 1,
                                         ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollX |
                                             ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit)) {
                ImGui::TableSetupScrollFreeze(1, 1);
                ImGui::TableSetupColumn("Input  \\  Output", ImGuiTableColumnFlags_WidthFixed, 200 * dpi_);
                for (const auto& b : s.buses) {
                    const std::string h = busLabel(b) + " " + b.name;
                    ImGui::TableSetupColumn(h.c_str(), ImGuiTableColumnFlags_WidthFixed, 112 * dpi_);
                }
                ImGui::TableHeadersRow();
                for (auto& st : s.strips) {
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::TextColored(toVec4(st.color), "%s", ICON_AUDIO);
                    ImGui::SameLine();
                    ImGui::TextUnformatted(st.name.c_str());
                    if (!st.online) {
                        ImGui::SameLine();
                        ImGui::TextColored(toVec4(p.bad), ICON_WARNING);
                        Tip(st.status.c_str());
                    }
                    for (const auto& b : s.buses) {
                        ImGui::TableNextColumn();
                        ImGui::PushID(static_cast<int>(st.id * 100 + b.id));
                        SendParams& sp = st.rt->sends[b.slot];
                        bool on = sp.enabled.load();
                        const bool blocked = sp.blocked.load();
                        char lbl[48];
                        if (on) std::snprintf(lbl, sizeof(lbl), "%s%+.0f dB%s", blocked ? ICON_WARNING " " : "",
                                              sp.gainDb.load(), sp.preFader.load() ? " pre" : "");
                        else std::snprintf(lbl, sizeof(lbl), "off");
                        const ImU32 col = blocked ? p.bad : (b.kind == BusKind::Physical ? p.busPhysical : p.busVirtual);
                        if (ToggleButton(lbl, &on, col, ImVec2(100 * dpi_, 0))) mc_->setSend(st.id, b.id, on);
                        if (ImGui::IsItemHovered())
                            ImGui::SetTooltip("%s -> %s\n%sRight-click: gain and pre/post fader", st.name.c_str(),
                                              b.name.c_str(), blocked ? "Blocked: feedback loop\n" : "");
                        if (ImGui::BeginPopupContextItem()) {
                            float g = sp.gainDb.load();
                            ImGui::SetNextItemWidth(180 * dpi_);
                            if (ImGui::SliderFloat("Send gain", &g, -60.0f, 12.0f, "%+.1f dB")) sp.gainDb = g;
                            bool pre = sp.preFader.load();
                            if (ImGui::Checkbox("Pre-fader", &pre)) sp.preFader = pre;
                            ImGui::EndPopup();
                        }
                        ImGui::PopID();
                    }
                }
                ImGui::EndTable();
            }
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Signal flow")) {
            drawFlowDiagram(ImGui::GetContentRegionAvail());
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    ImGui::End();
}

void EchoApp::drawFlowDiagram(ImVec2 size) {
    const Palette& p = pal();
    Session& s = mc_->session();
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##flow", ImVec2(std::max(size.x, 600.0f), std::max(size.y, 240.0f)));
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float W = std::max(size.x, 600.0f), H = std::max(size.y, 240.0f);
    const float colX[4] = {origin.x + W * 0.02f, origin.x + W * 0.27f, origin.x + W * 0.56f, origin.x + W * 0.80f};
    const float nodeW = W * 0.18f, nodeH = ImGui::GetFontSize() * 1.8f;
    auto nodeY = [&](int i, int n) {
        const float gap = std::min(nodeH * 1.6f, (H - nodeH) / std::max(1, n));
        const float total = gap * (n - 1) + nodeH;
        return origin.y + (H - total) * 0.5f + i * gap;
    };
    auto drawNode = [&](float x, float y, const std::string& text, ImU32 col, float levelDb, bool online) {
        dl->AddRectFilled(ImVec2(x, y), ImVec2(x + nodeW, y + nodeH), p.panelAlt, 5.0f);
        dl->AddRect(ImVec2(x, y), ImVec2(x + nodeW, y + nodeH), online ? col : p.bad, 5.0f, 1.5f);
        // Live level bar along the bottom edge.
        const float f = std::clamp((levelDb + 60.0f) / 60.0f, 0.0f, 1.0f);
        dl->AddRectFilled(ImVec2(x + 2, y + nodeH - 4), ImVec2(x + 2 + (nodeW - 4) * f, y + nodeH - 2),
                          levelDb > -3.0f ? p.bad : p.good);
        ImGui::PushClipRect(ImVec2(x, y), ImVec2(x + nodeW - 4, y + nodeH), true);
        dl->AddText(ImVec2(x + 8, y + (nodeH - ImGui::GetFontSize()) * 0.5f - 1), p.text, text.c_str());
        ImGui::PopClipRect();
    };
    const int ns = static_cast<int>(s.strips.size()), nb = static_cast<int>(s.buses.size());
    std::vector<ImVec2> stripOut(ns), stripIn(ns), busIn(nb), busOut(nb);
    for (int i = 0; i < ns; ++i) {
        StripModel& st = s.strips[i];
        const float y = nodeY(i, ns);
        std::string src;
        switch (st.source.kind) {
        case SourceKind::CaptureDevice: src = st.source.deviceId.empty() ? "Default mic" : st.source.deviceName; break;
        case SourceKind::ProcessLoopback: src = st.source.processName; break;
        case SourceKind::SystemExcludingEcho: src = "System audio"; break;
        case SourceKind::RenderLoopback: src = st.source.deviceName + " loopback"; break;
        case SourceKind::ToneGenerator: src = "Test signal"; break;
        default: src = "(none)"; break;
        }
        const auto& m = stripMeters(st.id);
        drawNode(colX[0], y, src, st.color, m.in.levelDb[0], st.online);
        std::string label = st.name;
        if (!st.chain.empty()) label += "  [" + std::to_string(st.chain.size()) + " fx]";
        drawNode(colX[1], y, label, st.color, std::max(m.out.levelDb[0], m.out.levelDb[1]), st.online);
        dl->AddLine(ImVec2(colX[0] + nodeW, y + nodeH * 0.5f), ImVec2(colX[1], y + nodeH * 0.5f), withAlpha(st.color, 0.7f), 2.0f);
        stripOut[i] = ImVec2(colX[1] + nodeW, y + nodeH * 0.5f);
    }
    for (int b = 0; b < nb; ++b) {
        BusModel& bus = s.buses[b];
        const float y = nodeY(b, nb);
        const auto& m = busMeters(bus.id);
        drawNode(colX[2], y, busLabel(bus) + " " + bus.name, bus.color, std::max(m.levelDb[0], m.levelDb[1]), bus.online);
        const std::string dev = bus.output.deviceId.empty() ? (bus.kind == BusKind::Physical ? "Default output" : "(no device)")
                                                            : bus.output.deviceName;
        drawNode(colX[3], y, dev, bus.kind == BusKind::Physical ? p.busPhysical : p.busVirtual, std::max(m.levelDb[0], m.levelDb[1]),
                 bus.online);
        dl->AddLine(ImVec2(colX[2] + nodeW, y + nodeH * 0.5f), ImVec2(colX[3], y + nodeH * 0.5f), withAlpha(bus.color, 0.7f), 2.0f);
        busIn[b] = ImVec2(colX[2], y + nodeH * 0.5f);
    }
    // Sends, brighter when signal is flowing.
    for (int i = 0; i < ns; ++i) {
        const StripModel& st = s.strips[i];
        const auto& m = stripMeters(st.id);
        const float lvl = std::max(m.out.levelDb[0], m.out.levelDb[1]);
        const float act = std::clamp((lvl + 60.0f) / 50.0f, 0.15f, 1.0f);
        for (int b = 0; b < nb; ++b) {
            const SendParams& sp = st.rt->sends[s.buses[b].slot];
            if (!sp.enabled.load()) continue;
            const ImVec2 a = stripOut[i], c = busIn[b];
            const float dx = (c.x - a.x) * 0.5f;
            const ImU32 col = sp.blocked.load() ? p.bad : withAlpha(st.color, act);
            dl->AddBezierCubic(a, ImVec2(a.x + dx, a.y), ImVec2(c.x - dx, c.y), c, col, sp.blocked.load() ? 1.5f : 1.0f + 2.0f * act);
        }
    }
    const float hy = origin.y + 2;
    dl->AddText(ImVec2(colX[0], hy), p.textDim, "SOURCES");
    dl->AddText(ImVec2(colX[1], hy), p.textDim, "INPUT STRIPS");
    dl->AddText(ImVec2(colX[2], hy), p.textDim, "BUSES");
    dl->AddText(ImVec2(colX[3], hy), p.textDim, "DEVICES");
}

} // namespace echo::ui
