// Devices: physical and virtual endpoints, virtual-cable setup and tests,
// test tone generator.
#include <windows.h>
#include <shellapi.h>

#include <algorithm>

#include "audio/VirtualCables.h"
#include "core/Sources.h"
#include "imgui.h"
#include "ui/App.h"
#include "ui/Icons.h"
#include "ui/Theme.h"
#include "ui/Widgets.h"

namespace echo::ui {

namespace {

void deviceTable(EchoApp& app, bool render) {
    const Palette& p = pal();
    auto devs = app.backend().deviceManager().devices(render ? win::Flow::Render : win::Flow::Capture, false);
    if (ImGui::BeginTable(render ? "##outdevs" : "##indevs", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH)) {
        ImGui::TableSetupColumn("Device", ImGuiTableColumnFlags_WidthStretch, 2.4f);
        ImGui::TableSetupColumn("Format", ImGuiTableColumnFlags_WidthStretch, 1.4f);
        ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthStretch, 1.2f);
        ImGui::TableSetupColumn("State", ImGuiTableColumnFlags_WidthFixed, 90 * app.dpiScale());
        ImGui::TableHeadersRow();
        for (const auto& d : devs) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(d.name.c_str());
            if (d.isDefault) {
                ImGui::SameLine();
                ImGui::TextColored(toVec4(p.accent), "default");
            }
            ImGui::TableNextColumn();
            if (d.active()) ImGui::TextDisabled("%s", win::formatToString(d.mixFormat).c_str());
            ImGui::TableNextColumn();
            if (d.virtualKind == win::VirtualKind::Cable) ImGui::TextColored(toVec4(p.groupVirtual), "Virtual cable");
            else if (d.virtualKind == win::VirtualKind::AppBound) {
                ImGui::TextColored(toVec4(p.warn), "App virtual device");
                Tip(("Owned by " + d.virtualVendor + ". It only carries audio while that application is running and "
                     "cannot be used as a general-purpose cable by Echo.").c_str());
            } else ImGui::TextDisabled("Hardware");
            ImGui::TableNextColumn();
            ImGui::TextColored(toVec4(d.active() ? p.good : p.textDim), "%s",
                               d.active() ? "Active" : d.state == DEVICE_STATE_UNPLUGGED ? "Unplugged" : "Disabled");
        }
        ImGui::EndTable();
    }
}

} // namespace

void EchoApp::drawDevices() {
    if (!ImGui::Begin("Devices")) {
        ImGui::End();
        return;
    }
    const Palette& p = pal();
    if (ImGui::BeginTabBar("##devtabs")) {
        if (ImGui::BeginTabItem("Virtual devices")) {
            const auto& pairs = backend_->cablePairs();
            ImGui::TextWrapped("Echo exposes its outputs to other apps (Discord, OBS, games, browsers) through a "
                               "virtual audio cable driver. Echo plays into the cable's playback side; other apps "
                               "select the cable's recording side as their microphone. Echo does not install drivers "
                               "itself and never changes your Windows default devices.");
            ImGui::Spacing();
            if (pairs.empty()) {
                ImGui::TextColored(toVec4(p.warn), ICON_WARNING " No virtual audio cable is installed.");
                ImGui::TextWrapped("Install a virtual cable driver from its official website, then press Rescan:");
                ImGui::BulletText("VB-CABLE Virtual Audio Device (free/donationware, 1 cable). Extra cables (A+B, C+D) are separate downloads.");
                ImGui::BulletText("Virtual Audio Cable by E. Muzychenko (commercial, many cables).");
                ImGui::BulletText("Any loopback cable whose playback side appears as audio on its recording side.");
                if (ImGui::Button(ICON_OPEN " Open VB-CABLE website (vb-audio.com)"))
                    ShellExecuteW(nullptr, L"open", L"https://vb-audio.com/Cable/", nullptr, nullptr, SW_SHOWNORMAL);
                Tip("Opens your web browser. Installing a driver requires administrator approval and usually a reboot.");
                ImGui::SameLine();
                if (ImGui::Button(ICON_REFRESH " Rescan devices")) mc_->rescanDevices();
            } else {
                const int mainPairs = static_cast<int>(
                    std::count_if(pairs.begin(), pairs.end(), [](const win::CablePair& c) { return !c.secondary; }));
                ImGui::Text("Detected loopback cables: %d", mainPairs);
                ImGui::SameLine();
                if (ImGui::SmallButton(ICON_REFRESH " Rescan")) mc_->rescanDevices();
                if (ImGui::BeginTable("##cables", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH)) {
                    ImGui::TableSetupColumn("Cable", ImGuiTableColumnFlags_WidthStretch, 1.4f);
                    ImGui::TableSetupColumn("Use in Echo", ImGuiTableColumnFlags_WidthStretch, 1.6f);
                    ImGui::TableSetupColumn("Loopback test", ImGuiTableColumnFlags_WidthStretch, 1.4f);
                    ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 230 * dpi_);
                    ImGui::TableHeadersRow();
                    for (const auto& c : pairs) {
                        ImGui::PushID(c.renderId.c_str());
                        ImGui::TableNextRow();
                        ImGui::TableNextColumn();
                        ImGui::TextUnformatted(c.displayName.c_str());
                        if (c.secondary) {
                            ImGui::SameLine();
                            ImGui::TextDisabled("(extra playback endpoint)");
                            Tip("This driver has more than one playback device feeding the same recording side "
                                "(e.g. VB-CABLE's 16-channel endpoint). Either works; the standard one is "
                                "used by default.");
                        }
                        ImGui::TextDisabled("Echo plays into: %s", c.renderName.c_str());
                        ImGui::TextDisabled("Apps record from: %s", c.captureName.c_str());
                        ImGui::TableNextColumn();
                        std::string use;
                        for (const auto& b : mc_->session().buses)
                            if (b.output.deviceId == c.renderId) use += "Virtual mic via bus " + busLabel(b) + " (" + b.name + ")\n";
                        for (const auto& s : mc_->session().strips)
                            if (s.source.deviceId == c.captureId) use += "Virtual speaker -> input '" + s.name + "'\n";
                        if (use.empty()) ImGui::TextDisabled("Not used");
                        else ImGui::TextUnformatted(use.c_str());
                        ImGui::TableNextColumn();
                        if (cableTestRunning_ && cableTestRender_ == c.renderId) {
                            ImGui::TextColored(toVec4(p.warn), "Testing...");
                        } else if (cableTestDone_ && cableTestRender_ == c.renderId) {
                            ImGui::TextColored(toVec4(cableTestResult_.ok ? p.good : p.bad), "%s",
                                               cableTestResult_.ok ? ICON_CHECK " Passed" : ICON_WARNING " Failed");
                            ImGui::TextDisabled("%s, %.1f dBFS, ~%.0f ms", cableTestResult_.message.c_str(),
                                                cableTestResult_.levelDb, cableTestResult_.latencyMs);
                        } else {
                            ImGui::TextDisabled("Not tested");
                        }
                        ImGui::TableNextColumn();
                        ImGui::BeginDisabled(cableTestRunning_);
                        if (ImGui::SmallButton("Run loopback test")) startCableTest(c.renderId, c.captureId);
                        ImGui::EndDisabled();
                        Tip("Plays a short -20 dBFS tone into the cable (inaudible on your speakers) and checks that it "
                            "arrives on the recording side.");
                        if (ImGui::SmallButton("Use as virtual mic")) {
                            OutputRef o;
                            o.deviceId = c.renderId;
                            o.deviceName = c.renderName;
                            BusModel* vb = nullptr;
                            for (auto& b : mc_->session().buses)
                                if (b.kind == BusKind::Virtual && b.output.deviceId.empty()) {
                                    vb = &b;
                                    break;
                                }
                            if (vb) mc_->setBusOutput(vb->id, o);
                            else mc_->addBus(BusKind::Virtual, o, "Virtual Mic " + std::to_string(mc_->session().buses.size()));
                            toast("Apps can now select '" + c.captureName + "' as their microphone.", 1);
                        }
                        ImGui::SameLine();
                        if (ImGui::SmallButton("Use as virtual speaker")) {
                            SourceRef r;
                            r.kind = SourceKind::CaptureDevice;
                            r.deviceId = c.captureId;
                            r.deviceName = c.captureName;
                            mc_->addStrip(StripGroup::Virtual, r, "Virtual Speaker");
                            toast("Set an app's output to '" + c.renderName + "' and it will appear on the new input strip.", 1);
                        }
                        ImGui::PopID();
                    }
                    ImGui::EndTable();
                }
            }
            if (ImGui::CollapsingHeader("Using the virtual mic in Discord, OBS, games...")) {
                ImGui::TextWrapped("1. Route your microphone strip to a B bus whose output is the cable's playback side "
                                   "(the default 'Virtual Mic' bus does this when a cable is installed).\n"
                                   "2. In Discord: User Settings > Voice & Video > Input Device > choose the cable's "
                                   "recording side (e.g. 'CABLE Output (VB-Audio Virtual Cable)').\n"
                                   "3. Turn off Discord's own noise suppression/echo cancellation if you use Echo's.\n"
                                   "4. Speak: the B bus meter in Echo and Discord's input meter should both move.");
            }
            if (ImGui::CollapsingHeader("Removing a virtual cable driver")) {
                ImGui::TextWrapped("Echo installs no drivers, so uninstalling Echo leaves drivers untouched. To remove a "
                                   "cable driver, use its own uninstaller (VB-CABLE: run the setup program again and "
                                   "choose Remove) or Device Manager > Sound, video and game controllers > right-click "
                                   "the cable > Uninstall device (tick 'delete driver'), then restart Windows.");
            }
            if (ImGui::CollapsingHeader("Test any playback/recording pair")) {
                static int r = -1, c = -1;
                auto outs = backend_->deviceManager().devices(win::Flow::Render, true);
                auto ins = backend_->deviceManager().devices(win::Flow::Capture, true);
                ImGui::SetNextItemWidth(300 * dpi_);
                if (ImGui::BeginCombo("Play into", r >= 0 && r < static_cast<int>(outs.size()) ? outs[r].name.c_str() : "Choose...")) {
                    for (int i = 0; i < static_cast<int>(outs.size()); ++i)
                        if (ImGui::Selectable(outs[i].name.c_str(), r == i)) r = i;
                    ImGui::EndCombo();
                }
                ImGui::SetNextItemWidth(300 * dpi_);
                if (ImGui::BeginCombo("Record from", c >= 0 && c < static_cast<int>(ins.size()) ? ins[c].name.c_str() : "Choose...")) {
                    for (int i = 0; i < static_cast<int>(ins.size()); ++i)
                        if (ImGui::Selectable(ins[i].name.c_str(), c == i)) c = i;
                    ImGui::EndCombo();
                }
                ImGui::BeginDisabled(cableTestRunning_ || r < 0 || c < 0 || r >= static_cast<int>(outs.size()) ||
                                     c >= static_cast<int>(ins.size()));
                if (ImGui::Button("Run test")) startCableTest(outs[r].idUtf8, ins[c].idUtf8);
                ImGui::EndDisabled();
                Tip("If the playback device is a speaker, the tone is audible; with a cable it is silent.");
                if (ImGui::Button("Remember this pair as a virtual cable") && r >= 0 && c >= 0 &&
                    r < static_cast<int>(outs.size()) && c < static_cast<int>(ins.size())) {
                    VirtualCableModel vc;
                    vc.name = outs[r].name;
                    vc.renderId = outs[r].idUtf8;
                    vc.renderName = outs[r].name;
                    vc.captureId = ins[c].idUtf8;
                    vc.captureName = ins[c].name;
                    vc.verified = cableTestDone_ && cableTestResult_.ok;
                    mc_->session().cables.push_back(vc);
                    std::vector<win::CablePair> extra;
                    for (const auto& cm : mc_->session().cables)
                        extra.push_back(win::CablePair{cm.renderId, cm.captureId, cm.renderName, cm.captureName, "", cm.name});
                    backend_->setExtraCablePairs(extra);
                    mc_->rebuildGraph();
                    toast("Saved cable pair (used for feedback protection)", 1);
                }
            }
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Inputs")) {
            deviceTable(*this, false);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Outputs")) {
            deviceTable(*this, true);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Test tone")) {
            ToneSource& t = mc_->tone();
            bool en = t.enabled.load();
            if (ToggleButton(en ? ICON_STOP " Stop test signal" : ICON_PLAY " Start test signal", &en, p.warn)) t.enabled = en;
            int wave = t.wave.load();
            ImGui::SetNextItemWidth(200 * dpi_);
            if (ImGui::Combo("Signal", &wave, ToneSource::kWaveNames, ToneSource::NumWaves)) t.wave = wave;
            float f = t.frequency.load();
            ImGui::SetNextItemWidth(300 * dpi_);
            if (ImGui::SliderFloat("Frequency", &f, 20.0f, 20000.0f, "%.0f Hz", ImGuiSliderFlags_Logarithmic)) t.frequency = f;
            float lvl = t.levelDb.load();
            ImGui::SetNextItemWidth(300 * dpi_);
            if (ImGui::SliderFloat("Level", &lvl, -60.0f, 0.0f, "%.1f dBFS")) t.levelDb = lvl;
            ImGui::TextWrapped("The test signal plays through any input strip whose source is 'Test signal generator'. "
                               "Add such a strip in the Mixer (+ > Test signal generator) and route it to the bus "
                               "you want to test. Start low: test tones can be loud in headphones.");
            bool hasGen = false;
            for (const auto& s : mc_->session().strips) hasGen |= s.source.kind == SourceKind::ToneGenerator;
            if (!hasGen && ImGui::Button(ICON_ADD " Add a test-signal strip")) {
                SourceRef r;
                r.kind = SourceKind::ToneGenerator;
                mc_->addStrip(StripGroup::Generator, r, "Test Signal");
            }
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    ImGui::End();
}

} // namespace echo::ui
