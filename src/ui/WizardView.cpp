// First-run setup wizard.
#include <windows.h>
#include <shellapi.h>

#include "audio/VirtualCables.h"
#include "imgui.h"
#include "session/Presets.h"
#include "ui/App.h"
#include "ui/Icons.h"
#include "ui/Theme.h"
#include "ui/Widgets.h"

namespace echo::ui {

namespace {
constexpr int kSteps = 8;
const char* kStepTitles[kSteps] = {"Welcome",          "Microphone",        "Headphones",       "Audio settings",
                                   "Virtual microphone", "Test your voice", "Routing",          "Use it in an app"};
int g_scene = 0;
bool g_toneOn = false;
} // namespace

void EchoApp::drawWizard() {
    const Palette& p = pal();
    Session& s = mc_->session();
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    const ImVec2 size(std::min(780.0f * dpi_, vp->WorkSize.x * 0.92f), std::min(580.0f * dpi_, vp->WorkSize.y * 0.9f));
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x * 0.5f, vp->WorkPos.y + vp->WorkSize.y * 0.5f),
                            ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(size, ImGuiCond_Always);
    if (!ImGui::IsPopupOpen("Echo setup")) ImGui::OpenPopup("Echo setup");
    bool open = true;
    if (!ImGui::BeginPopupModal("Echo setup", &open, ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoResize)) {
        if (!open) { // closed with the window's X button
            s.firstRunCompleted = true;
            wizardOpen_ = false;
        }
        return;
    }
    if (wizardCloseRequest_) {
        wizardCloseRequest_ = false;
        open = false;
    }
    // Step indicator.
    for (int i = 0; i < kSteps; ++i) {
        if (i) ImGui::SameLine();
        const ImU32 c = i == wizardStep_ ? p.accent : i < wizardStep_ ? p.good : p.textDim;
        ImGui::TextColored(toVec4(c), "%d %s", i + 1, i == wizardStep_ ? kStepTitles[i] : "");
    }
    ImGui::Separator();
    ImGui::BeginChild("##wizbody", ImVec2(0, -ImGui::GetFrameHeightWithSpacing() * 1.4f));
    ImGui::PushFont(fonts().semibold, fonts().baseSize * 1.35f);
    ImGui::TextUnformatted(kStepTitles[wizardStep_]);
    ImGui::PopFont();
    ImGui::Spacing();

    // Helpers to find/create the main mic strip and headphone/virtual buses.
    auto micStrip = [&]() -> StripModel* {
        for (auto& st : s.strips)
            if (st.group == StripGroup::Hardware) return &st;
        return nullptr;
    };
    auto busOfKind = [&](BusKind k) -> BusModel* {
        for (auto& b : s.buses)
            if (b.kind == k) return &b;
        return nullptr;
    };

    switch (wizardStep_) {
    case 0:
        ImGui::TextWrapped("Echo is a free virtual audio mixer and voice changer. In a few steps you will:");
        ImGui::BulletText("choose your microphone and headphones,");
        ImGui::BulletText("set up a virtual microphone that Discord, OBS, games and browsers can use,");
        ImGui::BulletText("check that your voice reaches it.");
        ImGui::Spacing();
        ImGui::TextWrapped("Echo never changes your Windows default devices, needs no account and works offline. You "
                           "can re-run this wizard from Help > Setup wizard (F1).");
        break;
    case 1: {
        StripModel* mic = micStrip();
        if (!mic) {
            SourceRef r;
            r.kind = SourceKind::CaptureDevice;
            mc_->addStrip(StripGroup::Hardware, r, "Microphone");
            mic = micStrip();
        }
        ImGui::TextWrapped("Pick the microphone you speak into:");
        for (const auto& d : backend_->devices()) {
            if (d.render || !d.active || d.isCable) continue;
            const bool sel = mic && mic->source.deviceId == d.id;
            std::string label = d.name + (d.isDefault ? "  (Windows default)" : "") + (d.isAppBound ? "  [app virtual device]" : "");
            if (ImGui::RadioButton(label.c_str(), sel) && mic) {
                SourceRef r;
                r.kind = SourceKind::CaptureDevice;
                r.deviceId = d.id;
                r.deviceName = d.name;
                mc_->setStripSource(mic->id, r);
            }
        }
        if (mic && ImGui::RadioButton("Always use the Windows default microphone", mic->source.deviceId.empty())) {
            SourceRef r;
            r.kind = SourceKind::CaptureDevice;
            mc_->setStripSource(mic->id, r);
        }
        if (mic) {
            ImGui::Spacing();
            noiseSuppressionCheckbox(*mic, "Remove background noise (recommended)");
            ImGui::Spacing();
            ImGui::Text("Input level (speak now):");
            HLevelMeter("##wizmic", stripMeters(mic->id).in, ImVec2(ImGui::GetContentRegionAvail().x, 20 * dpi_));
            if (!mic->online) ImGui::TextColored(toVec4(p.bad), "%s", mic->status.c_str());
            else if (stripMeters(mic->id).in.holdDb[0] < -70.0f)
                ImGui::TextColored(toVec4(p.warn), "No signal yet. If it stays silent, check Windows Settings > Privacy & security > "
                                                   "Microphone and allow desktop apps to use the microphone.");
        }
        break;
    }
    case 2: {
        BusModel* a = busOfKind(BusKind::Physical);
        if (!a) {
            mc_->addBus(BusKind::Physical, OutputRef{}, "Headphones");
            a = busOfKind(BusKind::Physical);
        }
        ImGui::TextWrapped("Where do you listen? (Used for monitoring and for apps you route to your headphones.)");
        for (const auto& d : backend_->devices()) {
            if (!d.render || !d.active || d.isCable) continue;
            const bool sel = a && a->output.deviceId == d.id;
            std::string label = d.name + (d.isDefault ? "  (Windows default)" : "");
            if (ImGui::RadioButton(label.c_str(), sel) && a) {
                OutputRef o;
                o.deviceId = d.id;
                o.deviceName = d.name;
                mc_->setBusOutput(a->id, o);
            }
        }
        if (a && ImGui::RadioButton("Always use the Windows default output", a->output.deviceId.empty()))
            mc_->setBusOutput(a->id, OutputRef{});
        ImGui::Spacing();
        if (a) {
            // Test tone through a generator strip routed only to this bus.
            if (ToggleButton(g_toneOn ? ICON_STOP " Stop test tone" : ICON_PLAY " Play test tone (-24 dBFS, 440 Hz)", &g_toneOn, p.warn)) {
                StripModel* gen = nullptr;
                for (auto& st : s.strips)
                    if (st.source.kind == SourceKind::ToneGenerator) gen = &st;
                if (!gen && g_toneOn) {
                    SourceRef r;
                    r.kind = SourceKind::ToneGenerator;
                    const uint32_t id = mc_->addStrip(StripGroup::Generator, r, "Test Signal");
                    gen = s.strip(id);
                }
                if (gen) {
                    for (const auto& b : s.buses) gen->rt->sends[b.slot].enabled = b.id == a->id && g_toneOn;
                    mc_->sendsChanged();
                }
                mc_->tone().wave = ToneSource::Sine;
                mc_->tone().frequency = 440.0f;
                mc_->tone().levelDb = -24.0f;
                mc_->tone().enabled = g_toneOn;
            }
            HLevelMeter("##wizout", busMeters(a->id), ImVec2(ImGui::GetContentRegionAvail().x, 20 * dpi_));
            if (!a->online) ImGui::TextColored(toVec4(p.bad), "%s", a->status.c_str());
        }
        break;
    }
    case 3: {
        if (g_toneOn) {
            g_toneOn = false;
            mc_->tone().enabled = false;
        }
        ImGui::TextWrapped("The defaults work for most people. Lower the buffer for less delay if your PC is fast; "
                           "raise it if you hear crackling.");
        const int rates[] = {44100, 48000};
        for (int r : rates) {
            if (ImGui::RadioButton((std::to_string(r) + " Hz").c_str(), s.engine.sampleRate == r) && s.engine.sampleRate != r) {
                s.engine.sampleRate = r;
                pendingEngineInit_ = false;
                mc_->restart();
            }
            ImGui::SameLine();
        }
        ImGui::NewLine();
        const int blocks[] = {128, 256, 480, 1024};
        for (int b : blocks) {
            char lbl[48];
            std::snprintf(lbl, sizeof(lbl), "%d samples (%.1f ms)%s", b, b * 1000.0 / s.engine.sampleRate, b == 480 ? "  recommended" : "");
            if (ImGui::RadioButton(lbl, s.engine.blockSize == b) && s.engine.blockSize != b) {
                s.engine.blockSize = b;
                pendingEngineInit_ = false;
                mc_->restart();
            }
        }
        ImGui::TextDisabled("DSP load now: %.1f%%", mc_->engine().stats().cpuLoad.load() * 100.0f);
        break;
    }
    case 4: {
        const auto& pairs = backend_->cablePairs();
        BusModel* vb = busOfKind(BusKind::Virtual);
        if (pairs.empty()) {
            ImGui::TextColored(toVec4(p.warn), ICON_WARNING " No virtual audio cable driver is installed.");
            ImGui::TextWrapped("A virtual microphone that other apps can select needs a virtual audio cable driver. "
                               "Windows cannot create one from a normal app. Install a free driver such as VB-CABLE "
                               "from its official site (administrator approval and a reboot may be required), then "
                               "come back and press Rescan. You can also skip this and use Echo for monitoring, "
                               "recording and app routing.");
            if (ImGui::Button(ICON_OPEN " Open vb-audio.com/Cable"))
                ShellExecuteW(nullptr, L"open", L"https://vb-audio.com/Cable/", nullptr, nullptr, SW_SHOWNORMAL);
            ImGui::SameLine();
            if (ImGui::Button(ICON_REFRESH " Rescan")) mc_->rescanDevices();
        } else {
            if (!vb) {
                mc_->addBus(BusKind::Virtual, OutputRef{}, "Virtual Mic");
                vb = busOfKind(BusKind::Virtual);
            }
            ImGui::TextWrapped("Choose the cable that becomes your virtual microphone. Echo plays your processed voice "
                               "into its playback side; other apps record from its recording side.");
            for (const auto& c : pairs) {
                if (c.secondary) continue;
                const bool sel = vb && vb->output.deviceId == c.renderId;
                if (ImGui::RadioButton((c.displayName + "   (apps select: " + c.captureName + ")").c_str(), sel) && vb) {
                    OutputRef o;
                    o.deviceId = c.renderId;
                    o.deviceName = c.renderName;
                    mc_->setBusOutput(vb->id, o);
                }
            }
            if (vb && !vb->output.deviceId.empty()) {
                std::string capture;
                for (const auto& c : pairs)
                    if (c.renderId == vb->output.deviceId) capture = c.captureId;
                ImGui::BeginDisabled(cableTestRunning_ || capture.empty());
                if (ImGui::Button(ICON_CHECK " Run loopback test")) startCableTest(vb->output.deviceId, capture);
                ImGui::EndDisabled();
                if (cableTestRunning_) ImGui::TextColored(toVec4(p.warn), "Testing...");
                else if (cableTestDone_ && cableTestRender_ == vb->output.deviceId)
                    ImGui::TextColored(toVec4(cableTestResult_.ok ? p.good : p.bad), "%s (%.1f dBFS, ~%.0f ms)",
                                       cableTestResult_.message.c_str(), cableTestResult_.levelDb, cableTestResult_.latencyMs);
            }
        }
        break;
    }
    case 5: {
        StripModel* mic = micStrip();
        BusModel* vb = busOfKind(BusKind::Virtual);
        BusModel* a = busOfKind(BusKind::Physical);
        if (mic && vb && !mic->rt->sends[vb->slot].enabled.load()) mc_->setSend(mic->id, vb->id, true);
        ImGui::TextWrapped("Speak: your microphone and the virtual microphone meters should both move.");
        if (mic) {
            ImGui::Text("Microphone");
            HLevelMeter("##wm1", stripMeters(mic->id).out, ImVec2(ImGui::GetContentRegionAvail().x, 20 * dpi_));
        }
        if (vb) {
            ImGui::Text("Virtual microphone (%s)", vb->output.deviceName.empty() ? "no device" : vb->output.deviceName.c_str());
            HLevelMeter("##wm2", busMeters(vb->id), ImVec2(ImGui::GetContentRegionAvail().x, 20 * dpi_));
        }
        if (mic && a) {
            bool mon = mic->rt->sends[a->slot].enabled.load();
            if (ImGui::Checkbox("Hear myself in my headphones (monitoring)", &mon)) mc_->setSend(mic->id, a->id, mon);
            ImGui::TextColored(toVec4(p.warn), "Use headphones for monitoring; speakers can cause acoustic feedback.");
        }
        if (mic) {
            ImGui::Spacing();
            ImGui::TextWrapped("Optional: start with a clean voice preset (you can change it any time in the Voice Lab):");
            for (const char* n : {"Clean Microphone", "Noise-Reduced Microphone", "Streaming Voice"}) {
                if (const ChainPreset* pr = presets_.find(n)) {
                    if (ImGui::Button(n)) applyChainPreset(ChainTarget{false, mic->id}, *pr);
                    ImGui::SameLine();
                }
            }
            ImGui::NewLine();
        }
        break;
    }
    case 6:
        ImGui::TextWrapped("How should audio be routed? (You can change this later in the Routing view.)");
        for (int i = 0; i < static_cast<int>(ScenePreset::Count); ++i) {
            ImGui::RadioButton(scenePresetName(static_cast<ScenePreset>(i)), &g_scene, i);
            ImGui::TextDisabled("   %s", scenePresetDescription(static_cast<ScenePreset>(i)));
        }
        if (ImGui::Button("Apply routing")) toast(applyScenePreset(*mc_, static_cast<ScenePreset>(g_scene)), 1);
        break;
    case 7: {
        BusModel* vb = busOfKind(BusKind::Virtual);
        std::string capture = "the cable's recording side";
        for (const auto& c : backend_->cablePairs())
            if (vb && c.renderId == vb->output.deviceId) capture = c.captureName;
        ImGui::TextWrapped("In Discord: User Settings > Voice & Video > Input Device > select:");
        ImGui::PushFont(fonts().semibold, 0.0f);
        ImGui::TextColored(toVec4(p.accent), "   %s", capture.c_str());
        ImGui::PopFont();
        ImGui::TextWrapped("Then use 'Let's Check' in Discord and speak; you should see Discord's meter move and hear "
                           "your processed voice. The same works in OBS (Audio Input Capture), browsers, games and "
                           "recording software. If the app shows no signal, make sure Echo's virtual mic bus meter "
                           "is moving and that the app is not using its own exclusive mode.");
        ImGui::Spacing();
        ImGui::TextWrapped("Tip: Voice Lab has one-click voice characters and pitch correction; Applications lets you "
                           "capture music or game audio per app.");
        break;
    }
    }
    ImGui::EndChild();
    ImGui::Separator();
    ImGui::BeginDisabled(wizardStep_ == 0);
    if (ImGui::Button("Back", ImVec2(110 * dpi_, 0))) --wizardStep_;
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (wizardStep_ < kSteps - 1) {
        if (ImGui::Button("Next", ImVec2(110 * dpi_, 0))) ++wizardStep_;
        ImGui::SameLine();
        if (ImGui::Button("Skip setup", ImVec2(110 * dpi_, 0))) open = false;
    } else if (ImGui::Button("Finish", ImVec2(110 * dpi_, 0))) {
        open = false;
    }
    if (!open) {
        if (g_toneOn) {
            g_toneOn = false;
            mc_->tone().enabled = false;
        }
        s.firstRunCompleted = true;
        wizardOpen_ = false;
        saveNow();
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

} // namespace echo::ui
