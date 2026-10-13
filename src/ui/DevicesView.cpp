// Devices: which microphone and headphones Echo uses, the virtual mic
// (loopback cable) with its test, other apps' virtual devices, a test tone,
// and the full device lists.
#include <windows.h>
#include <shellapi.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>

#include "audio/VbCable.h"
#include "audio/VirtualCables.h"
#include "core/Sources.h"
#include "imgui.h"
#include "session/Paths.h"
#include "ui/App.h"
#include "ui/Icons.h"
#include "ui/Pop.h"
#include "ui/Theme.h"
#include "ui/Widgets.h"

namespace echo::ui {

void EchoApp::drawVbCableInstall() {
    const Palette& p = pal();
    const float sc = uiScale();
    const bool running = vbCableJob_.valid() && vbCableJob_.wait_for(std::chrono::seconds(0)) != std::future_status::ready;
    if (vbCableJob_.valid() && !running) vbCableLast_ = vbCableJob_.get();
    // Downloads the pack (once) on a worker, then opens VB-Audio's installer
    // there too: Windows' permission prompt can wait for the user.
    auto start = [this]() {
        vbCableProgress_.cancel = false;
        vbCableProgress_.done = 0;
        const std::wstring setup = vbCableLast_.setup;
        vbCableLast_ = {};
        vbCableJob_ = std::async(std::launch::async, [this, setup]() {
            VbCableJob job;
            job.setup = setup;
            std::error_code ec;
            if (job.setup.empty() || !std::filesystem::exists(job.setup, ec)) {
                job.setup.clear();
                if (!win::prepareVbCable(paths::localDataDir() + L"\\VB-CABLE", vbCableProgress_, &job.setup, &job.error))
                    return job;
            }
            job.opened = win::openVbCableSetup(job.setup, &job.error);
            return job;
        });
    };
    if (running) {
        const uint64_t done = vbCableProgress_.done.load();
        if (done < win::kVbCableBytes) {
            char label[64];
            std::snprintf(label, sizeof(label), "Downloading VB-CABLE: %.1f of %.1f MB", done / 1048576.0,
                          win::kVbCableBytes / 1048576.0);
            ImGui::ProgressBar(static_cast<float>(done) / static_cast<float>(win::kVbCableBytes),
                               ImVec2(std::min(ImGui::GetContentRegionAvail().x, 420.0f * sc), 0), label);
        } else {
            pop::Caption("Opening VB-Audio's installer. Windows asks for your permission.");
        }
        return;
    }
    const bool opened = vbCableLast_.opened;
    if (opened) {
        pop::Banner(ICON_CHECK, "VB-Audio's installer is open.", p.goodTint, p.good);
        pop::Caption("Click Install Driver in its window. When it's done, restart your PC. Echo finds the cable by itself.");
        if (pop::Button("Open the installer again")) start();
    } else {
        if (!vbCableLast_.error.empty()) pop::TextColoredWrapped(p.bad, vbCableLast_.error.c_str());
        if (pop::Button(ICON_DOWNLOAD "  Install VB-CABLE", pop::Btn::Primary)) start();
    }
    ImGui::SameLine();
    if (pop::Button(ICON_REFRESH "  Rescan")) mc_->rescanDevices();
    if (!opened)
        pop::Hint("Downloads VB-Audio's official installer (1.3 MB) and opens it. Windows asks for your permission.");
    pop::Hint("VB-CABLE is free (donationware) from VB-Audio. If it helps you, you can support them on their site.");
    if (pop::LinkButton("vb-audio.com/Cable")) ShellExecuteW(nullptr, L"open", win::kVbCablePage, nullptr, nullptr, SW_SHOWNORMAL);
}

namespace {

uint32_t g_toneBus = 0;

void deviceTable(EchoApp& app, bool render) {
    const Palette& p = pal();
    auto devs = app.backend().deviceManager().devices(render ? win::Flow::Render : win::Flow::Capture, false);
    if (ImGui::BeginTable(render ? "##outdevs" : "##indevs", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH)) {
        ImGui::TableSetupColumn("Device", ImGuiTableColumnFlags_WidthStretch, 2.4f);
        ImGui::TableSetupColumn("Format", ImGuiTableColumnFlags_WidthStretch, 1.4f);
        ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthStretch, 1.2f);
        ImGui::TableSetupColumn("State", ImGuiTableColumnFlags_WidthFixed, 90 * uiScale());
        ImGui::TableHeadersRow();
        for (const auto& d : devs) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(d.name.c_str());
            if (d.isDefault) {
                ImGui::SameLine();
                ImGui::TextColored(toVec4(p.link), "Windows default");
            }
            ImGui::TableNextColumn();
            if (d.active()) ImGui::TextDisabled("%s", win::formatToString(d.mixFormat).c_str());
            ImGui::TableNextColumn();
            if (d.virtualKind == win::VirtualKind::Cable) ImGui::TextColored(toVec4(p.busVirtual), "Virtual cable");
            else if (d.virtualKind == win::VirtualKind::AppBound) {
                ImGui::TextColored(toVec4(p.warnInk), "App virtual device");
                Tip(("Owned by " + d.virtualVendor + ". It only carries audio while that application is running and "
                     "cannot be used as a general-purpose cable by Echo.").c_str());
            } else ImGui::TextDisabled("Hardware");
            ImGui::TableNextColumn();
            ImGui::TextColored(toVec4(d.active() ? p.goodInk : p.textDim), "%s",
                               d.active() ? "Active" : d.state == DEVICE_STATE_UNPLUGGED ? "Unplugged" : "Disabled");
        }
        ImGui::EndTable();
    }
}

} // namespace

void EchoApp::drawDevices() {
    const Palette& p = pal();
    const float sc = uiScale();
    Session& s = mc_->session();
    pop::PageTitle("Devices", "Echo never changes your Windows default devices.");
    const float avail = ImGui::GetContentRegionAvail().x;
    const float gap = 20.0f * sc;
    const bool wide = avail > 980.0f * sc;
    const float rightW = wide ? 400.0f * sc : avail;
    const float leftW = wide ? avail - rightW - gap : avail;
    if (pop::SmallButton(ICON_REFRESH "  Rescan devices")) mc_->rescanDevices();
    ImGui::Dummy(ImVec2(0, 2.0f * sc));
    const auto devs = backend_->devices();

    // ------------------------------------------------------------- left column
    ImGui::BeginChild("##devleft", ImVec2(leftW, 0), ImGuiChildFlags_AutoResizeY, ImGuiWindowFlags_NoScrollbar);
    StripModel* mic = primaryMic();
    pop::BeginCard("##micdev");
    pop::CardTitle("Microphone input");
    pop::Caption("\"Windows default\" follows Windows when you switch microphones there.");
    ImGui::Dummy(ImVec2(0, 2.0f * sc));
    if (!mic) {
        if (pop::Button("Add a microphone input", pop::Btn::Primary)) {
            SourceRef r;
            r.kind = SourceKind::CaptureDevice;
            mc_->addStrip(StripGroup::Hardware, r, "Microphone");
        }
    } else {
        std::string defName;
        for (const auto& d : devs)
            if (!d.render && d.isDefault) defName = d.name;
        if (pop::RadioRow("##micdef", "Windows default", ("Now: " + defName).c_str(), mic->source.deviceId.empty())) {
            SourceRef r;
            r.kind = SourceKind::CaptureDevice;
            mc_->setStripSource(mic->id, r);
        }
        int i = 0;
        for (const auto& d : devs) {
            if (d.render || !d.active || d.isCable) continue;
            ImGui::PushID(i++);
            const std::string sub = d.isAppBound ? "Another app's virtual device" : (d.isDefault ? "Windows default" : "Microphone");
            if (pop::RadioRow("##mic", d.name.c_str(), sub.c_str(), mic->source.deviceId == d.id)) {
                SourceRef r;
                r.kind = SourceKind::CaptureDevice;
                r.deviceId = d.id;
                r.deviceName = d.name;
                mc_->setStripSource(mic->id, r);
            }
            ImGui::PopID();
        }
        ImGui::Dummy(ImVec2(0, 2.0f * sc));
        pop::Label("Level");
        const StripMeters& m = stripMeters(mic->id);
        pop::BarMeter(std::max(m.in.levelDb[0], m.in.levelDb[1]), ImGui::GetContentRegionAvail().x, 10.0f * sc);
        if (!mic->online) pop::Banner(ICON_WARNING, mic->status.c_str(), p.badTint, p.bad);
    }
    pop::EndCard();

    ImGui::Dummy(ImVec2(0, 4.0f * sc));
    BusModel* a1 = firstBus(BusKind::Physical);
    pop::BeginCard("##outdev");
    pop::CardTitle(a1 ? (a1->name + " (" + busLabel(*a1) + ")").c_str() : "Headphones");
    pop::Caption("Where you hear Echo. This device also sets Echo's timing.");
    ImGui::Dummy(ImVec2(0, 2.0f * sc));
    if (!a1) {
        if (pop::Button("Add a headphones output", pop::Btn::Primary)) mc_->addBus(BusKind::Physical, OutputRef{}, "Headphones");
    } else {
        std::string defName;
        for (const auto& d : devs)
            if (d.render && d.isDefault) defName = d.name;
        if (pop::RadioRow("##outdef", "Windows default", ("Now: " + defName).c_str(), a1->output.deviceId.empty()))
            mc_->setBusOutput(a1->id, OutputRef{});
        int i = 0;
        for (const auto& d : devs) {
            if (!d.render || !d.active || d.isCable) continue;
            ImGui::PushID(i++);
            std::string sub = d.isAppBound ? "Another app's virtual device" : (d.isDefault ? "Windows default" : "Output");
            if (a1->output.deviceId == d.id && a1->isClock) sub += " · timing source";
            if (pop::RadioRow("##out", d.name.c_str(), sub.c_str(), a1->output.deviceId == d.id)) {
                OutputRef o;
                o.deviceId = d.id;
                o.deviceName = d.name;
                mc_->setBusOutput(a1->id, o);
            }
            ImGui::PopID();
        }
        if (!a1->online) pop::Banner(ICON_WARNING, a1->status.c_str(), p.badTint, p.bad);
    }
    pop::EndCard();

    ImGui::Dummy(ImVec2(0, 4.0f * sc));
    pop::BeginCard("##others", 0.0f, 0.0f, true);
    pop::CardTitle("Other apps' virtual devices");
    pop::Caption("These belong to other programs. Echo sees them and leaves them alone.");
    bool anyApp = false;
    for (const auto& d : backend_->deviceManager().devices(win::Flow::Capture, false))
        if (d.virtualKind == win::VirtualKind::AppBound) {
            anyApp = true;
            pop::Divider(4.0f, 4.0f);
            ImGui::TextUnformatted(d.name.c_str());
            ImGui::PushFont(fonts().regular, 12.0f);
            ImGui::TextColored(toVec4(p.textDim), "%s", d.virtualVendor.c_str());
            ImGui::PopFont();
        }
    if (!anyApp) pop::Hint("None found.");
    pop::EndCard();

    ImGui::Dummy(ImVec2(0, 4.0f * sc));
    pop::BeginCard("##alldevs", 0.0f, 0.0f, true);
    if (ImGui::TreeNode("All devices, formats and states")) {
        pop::Label("Inputs");
        deviceTable(*this, false);
        ImGui::Dummy(ImVec2(0, 6.0f * sc));
        pop::Label("Outputs");
        deviceTable(*this, true);
        ImGui::TreePop();
    }
    pop::EndCard();
    ImGui::EndChild();

    // ------------------------------------------------------------ right column
    if (wide) ImGui::SameLine(0, gap);
    ImGui::BeginChild("##devright", ImVec2(rightW, 0), ImGuiChildFlags_AutoResizeY, ImGuiWindowFlags_NoScrollbar);
    const auto& pairs = backend_->cablePairs();
    pop::BeginCard("##vmic");
    pop::CardTitle("Virtual mic");
    if (pairs.empty() || snapshotNoCable_) {
        pop::Banner(ICON_WARNING, "No virtual audio cable is installed.", p.warnTint, p.warn);
        pop::Caption("Other apps hear Echo through a virtual audio cable. We recommend VB-CABLE, free from VB-Audio.");
        drawVbCableInstall();
    } else {
        for (const auto& c : pairs) {
            if (c.secondary) continue;
            ImGui::PushID(c.renderId.c_str());
            BusModel* user = nullptr;
            for (auto& b : s.buses)
                if (b.output.deviceId == c.renderId) user = &b;
            if (user) pop::Badge(("Used by " + user->name + " (" + busLabel(*user) + ")").c_str(), p.goodTint, p.goodInk);
            else pop::Badge("Not used yet", p.segBg, p.text2);
            // Mapping: Echo plays into ... other apps pick ...
            const ImVec2 bp = ImGui::GetCursorScreenPos();
            const float bw = ImGui::GetContentRegionAvail().x, bh = 58.0f * sc;
            ImDrawList* dl = ImGui::GetWindowDrawList();
            dl->AddRectFilled(bp, ImVec2(bp.x + bw, bp.y + bh), p.surfaceAlt, 12.0f * sc);
            ImGui::SetCursorScreenPos(ImVec2(bp.x + 14.0f * sc, bp.y + 9.0f * sc));
            ImGui::BeginGroup();
            ImGui::PushFont(fonts().regular, 11.0f);
            ImGui::TextColored(toVec4(p.textDim), "Echo plays into");
            ImGui::PopFont();
            ImGui::PushFont(fonts().semibold, 14.0f);
            TextEllipsis(c.renderName, bw * 0.5f - 44.0f * sc);
            ImGui::PopFont();
            ImGui::EndGroup();
            const float ax = bp.x + bw * 0.5f;
            dl->AddLine(ImVec2(ax - 12 * sc, bp.y + bh * 0.5f), ImVec2(ax + 10 * sc, bp.y + bh * 0.5f), p.busVirtual, 2.0f * sc);
            dl->AddTriangleFilled(ImVec2(ax + 14 * sc, bp.y + bh * 0.5f), ImVec2(ax + 6 * sc, bp.y + bh * 0.5f - 6 * sc),
                                  ImVec2(ax + 6 * sc, bp.y + bh * 0.5f + 6 * sc), p.busVirtual);
            ImGui::SetCursorScreenPos(ImVec2(ax + 24.0f * sc, bp.y + 9.0f * sc));
            ImGui::BeginGroup();
            ImGui::PushFont(fonts().regular, 11.0f);
            ImGui::TextColored(toVec4(p.textDim), "Other apps pick");
            ImGui::PopFont();
            ImGui::PushFont(fonts().semibold, 14.0f);
            TextEllipsis(c.captureName, bp.x + bw - (ax + 24.0f * sc) - 14.0f * sc);
            ImGui::PopFont();
            ImGui::EndGroup();
            ImGui::SetCursorScreenPos(ImVec2(bp.x, bp.y + bh));
            ImGui::Dummy(ImVec2(bw, 0));
            // Test result.
            if (cableTestRunning_ && cableTestRender_ == c.renderId) {
                pop::Caption("Testing...");
            } else if (cableTestDone_ && cableTestRender_ == c.renderId) {
                char msg[160];
                std::snprintf(msg, sizeof(msg), "%s: %s", cableTestResult_.ok ? "Passed" : "Failed", cableTestResult_.message.c_str());
                pop::Banner(cableTestResult_.ok ? ICON_CHECK : ICON_WARNING, msg, cableTestResult_.ok ? p.goodTint : p.badTint,
                            cableTestResult_.ok ? p.good : p.bad);
            }
            ImGui::BeginDisabled(cableTestRunning_);
            if (pop::Button("Test the virtual mic", pop::Btn::Primary)) startCableTest(c.renderId, c.captureId);
            ImGui::EndDisabled();
            Tip("Plays a short -20 dBFS tone into the cable (silent on your speakers) and checks it comes out the other side.");
            if (!user) {
                ImGui::SameLine();
                if (pop::Button("Use as virtual mic")) {
                    OutputRef o;
                    o.deviceId = c.renderId;
                    o.deviceName = c.renderName;
                    BusModel* vb = nullptr;
                    for (auto& b : s.buses)
                        if (b.kind == BusKind::Virtual && b.output.deviceId.empty()) {
                            vb = &b;
                            break;
                        }
                    if (vb) mc_->setBusOutput(vb->id, o);
                    else mc_->addBus(BusKind::Virtual, o, "Virtual Mic");
                    toast("Apps can now choose '" + c.captureName + "' as their microphone.", 1);
                }
            }
            if (pop::LinkButton("Use it as a virtual speaker instead")) {
                SourceRef r;
                r.kind = SourceKind::CaptureDevice;
                r.deviceId = c.captureId;
                r.deviceName = c.captureName;
                mc_->addStrip(StripGroup::Virtual, r, "Virtual Speaker");
                toast("Set an app's output to '" + c.renderName + "' and it arrives on the new input.", 1);
            }
            pop::Divider();
            ImGui::PopID();
        }
        pop::Hint("VB-CABLE is a free (donationware) driver by VB-Audio: https://vb-audio.com/Cable/");
    }
    if (ImGui::TreeNode("Using it in your apps")) {
        pop::Caption("In your app's voice or audio settings, set the microphone (input) to the cable's recording side, "
                     "for example \"CABLE Output\", and leave the speakers (output) on your headphones. Keep Windows' "
                     "own sound settings as they are. Speak: Echo's Virtual Mic meter and the app's meter should both "
                     "move.");
        ImGui::TreePop();
    }
    if (ImGui::TreeNode("Removing a cable driver")) {
        pop::Caption("Uninstalling Echo leaves cable drivers alone. To remove VB-CABLE, run VB-Audio's installer again "
                     "and click Remove Driver. For other cables, use their own uninstaller, or Device Manager > Sound, "
                     "video and game controllers > right-click the cable > Uninstall device (tick \"delete driver\"), "
                     "then restart Windows.");
        ImGui::TreePop();
    }
    if (ImGui::TreeNode("Test any playback / recording pair")) {
        static int r = -1, c = -1;
        auto outs = backend_->deviceManager().devices(win::Flow::Render, true);
        auto ins = backend_->deviceManager().devices(win::Flow::Capture, true);
        ImGui::SetNextItemWidth(-1);
        if (ImGui::BeginCombo("##playinto", r >= 0 && r < static_cast<int>(outs.size()) ? outs[r].name.c_str() : "Play into...")) {
            for (int i = 0; i < static_cast<int>(outs.size()); ++i)
                if (ImGui::Selectable(outs[i].name.c_str(), r == i)) r = i;
            ImGui::EndCombo();
        }
        ImGui::SetNextItemWidth(-1);
        if (ImGui::BeginCombo("##recfrom", c >= 0 && c < static_cast<int>(ins.size()) ? ins[c].name.c_str() : "Record from...")) {
            for (int i = 0; i < static_cast<int>(ins.size()); ++i)
                if (ImGui::Selectable(ins[i].name.c_str(), c == i)) c = i;
            ImGui::EndCombo();
        }
        const bool valid = r >= 0 && c >= 0 && r < static_cast<int>(outs.size()) && c < static_cast<int>(ins.size());
        ImGui::BeginDisabled(cableTestRunning_ || !valid);
        if (pop::SmallButton("Run test")) startCableTest(outs[r].idUtf8, ins[c].idUtf8);
        ImGui::EndDisabled();
        Tip("If the playback device is a speaker, the tone is audible; with a cable it is silent.");
        ImGui::SameLine();
        ImGui::BeginDisabled(!valid);
        if (pop::SmallButton("Remember as a cable")) {
            VirtualCableModel vc;
            vc.name = outs[r].name;
            vc.renderId = outs[r].idUtf8;
            vc.renderName = outs[r].name;
            vc.captureId = ins[c].idUtf8;
            vc.captureName = ins[c].name;
            vc.verified = cableTestDone_ && cableTestResult_.ok;
            s.cables.push_back(vc);
            std::vector<win::CablePair> extra;
            for (const auto& cm : s.cables)
                extra.push_back(win::CablePair{cm.renderId, cm.captureId, cm.renderName, cm.captureName, "", cm.name});
            backend_->setExtraCablePairs(extra);
            mc_->rebuildGraph();
            toast("Saved cable pair (used for feedback protection)", 1);
        }
        ImGui::EndDisabled();
        ImGui::TreePop();
    }
    pop::EndCard();

    // Test tone through a generator input routed to one output.
    ImGui::Dummy(ImVec2(0, 4.0f * sc));
    pop::BeginCard("##tone");
    pop::CardTitle("Test tone");
    pop::Caption("Start low: test tones can be loud in headphones.");
    ToneSource& t = mc_->tone();
    if (!s.bus(g_toneBus)) g_toneBus = a1 ? a1->id : (s.buses.empty() ? 0 : s.buses.front().id);
    int wave = t.wave.load();
    pop::Label("Signal");
    ImGui::SetNextItemWidth(-1);
    if (ImGui::Combo("##wave", &wave, ToneSource::kWaveNames, ToneSource::NumWaves)) t.wave = wave;
    float f = std::log10(t.frequency.load());
    char ft[32];
    std::snprintf(ft, sizeof(ft), "%.0f Hz", t.frequency.load());
    if (pop::SliderRow("Frequency", ft, "##freq", &f, std::log10(20.0f), std::log10(20000.0f), std::log10(440.0f)))
        t.frequency = std::pow(10.0f, f);
    float lvl = t.levelDb.load();
    char lt[32];
    std::snprintf(lt, sizeof(lt), "%.0f dBFS", lvl);
    if (pop::SliderRow("Level", lt, "##lvl", &lvl, -60.0f, 0.0f, -24.0f)) t.levelDb = std::round(lvl);
    pop::Label("Play on");
    const BusModel* tb = s.bus(g_toneBus);
    ImGui::SetNextItemWidth(-1);
    if (ImGui::BeginCombo("##tonebus", tb ? (tb->name + " (" + busLabel(*tb) + ")").c_str() : "No output")) {
        for (const auto& b : s.buses)
            if (ImGui::Selectable((b.name + " (" + busLabel(b) + ")").c_str(), b.id == g_toneBus)) g_toneBus = b.id;
        ImGui::EndCombo();
    }
    const bool en = t.enabled.load();
    ImGui::Dummy(ImVec2(0, 2.0f * sc));
    if (pop::Button(en ? "Stop tone" : "Play test tone", en ? pop::Btn::Primary : pop::Btn::Secondary, ImVec2(-1, 0))) {
        const bool on = !en;
        StripModel* gen = nullptr;
        for (auto& st : s.strips)
            if (st.source.kind == SourceKind::ToneGenerator) gen = &st;
        if (!gen && on) {
            SourceRef r;
            r.kind = SourceKind::ToneGenerator;
            gen = s.strip(mc_->addStrip(StripGroup::Generator, r, "Test Signal"));
        }
        if (gen) {
            for (const auto& b : s.buses) gen->rt->sends[b.slot].enabled = on && b.id == g_toneBus;
            mc_->sendsChanged();
        }
        t.enabled = on;
    }
    pop::EndCard();
    ImGui::EndChild();
}

} // namespace echo::ui
