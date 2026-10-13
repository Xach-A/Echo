// Noise & echo controls (Voices screen) and the shared status/controls for
// the noise suppressor and echo canceller (also shown in the effects editor).
#include <algorithm>
#include <cstdio>

#include "dsp/NoiseSuppressorInfo.h"
#include "imgui.h"
#include "ui/App.h"
#include "ui/Icons.h"
#include "ui/Theme.h"
#include "ui/Widgets.h"

namespace echo::ui {

namespace {

Effect* findType(StripModel& s, const char* type) {
    for (auto& e : s.chain)
        if (std::string(e->typeId()) == type) return e.get();
    return nullptr;
}

// Press-and-hold bypass of one effect (A/B against the unprocessed input).
void HoldToCompare(const char* label, Effect& fx, const char* tip) {
    static uint32_t holding = 0;
    static bool saved = false;
    ImGui::Button(label);
    if (ImGui::IsItemActivated()) {
        saved = fx.bypass.load();
        fx.bypass = true;
        holding = fx.instanceId;
    }
    if (holding == fx.instanceId && ImGui::IsItemDeactivated()) {
        fx.bypass = saved;
        holding = 0;
    }
    Tip(tip);
}

void LatencyCpu(const Effect& fx, double sr) {
    (void)sr;
    ImGui::TextDisabled("CPU %.1f%% of one core", fx.cpuLoad.load() * 100.0f);
    Tip("Measured while running: the share of one processor core this effect uses on this input.");
}

} // namespace

void EchoApp::drawCleanupStatus(Effect& fx) {
    const Palette& p = pal();
    const std::string type = fx.typeId();
    const float w = std::min(ImGui::GetContentRegionAvail().x, 420.0f * dpi_);
    if (type == noiseSuppressorTypeId()) {
        const float prob = std::clamp(fx.vis[0].load(), 0.0f, 1.0f);
        char overlay[64];
        snprintf(overlay, sizeof overlay, "Voice %.0f%%", prob * 100.0f);
        ImGui::PushStyleColor(ImGuiCol_PlotHistogram, toVec4(prob >= fx.param(3) / 100.0f ? p.good : p.accentDim));
        ImGui::ProgressBar(fx.bypass.load() ? 0.0f : prob, ImVec2(w * 0.5f, 0), overlay);
        ImGui::PopStyleColor();
        Tip("Voice activity: how sure the network is that someone is speaking right now.");
        ImGui::SameLine();
        const float red = fx.vis[1].load();
        if (fx.bypass.load()) ImGui::TextDisabled("Bypassed");
        else ImGui::TextDisabled("Removing %.0f dB", std::max(0.0f, -red));
        Tip("Current level difference between the microphone and the cleaned signal.");
        if (fx.param(2) >= 0.5f) {
            const bool open = fx.vis[2].load() > -1.0f;
            Led(open, p.good, ImGui::GetFontSize() * 0.35f);
            ImGui::SameLine();
            ImGui::TextDisabled("Voice gate %s", open ? "open" : "closed");
        }
    } else if (type == echoCancellerTypeId()) {
        const bool ref = fx.vis[3].load() > 0.5f;
        Led(ref, ref ? p.good : p.warn, ImGui::GetFontSize() * 0.35f);
        ImGui::SameLine();
        if (!ref) {
            ImGui::TextColored(toVec4(p.warn), "No speaker signal connected - echo cancellation is idle");
            Tip("Choose the playback device your speakers are connected to.");
        } else {
            const float refDb = fx.vis[2].load();
            if (refDb < -70.0f) ImGui::TextDisabled("Speakers silent");
            else ImGui::TextDisabled("Speakers %.0f dBFS  |  echo reduced by %.0f dB", refDb, std::max(0.0f, fx.vis[0].load()));
            if (fx.vis[1].load() > 0.5f) {
                ImGui::SameLine();
                ImGui::TextColored(toVec4(p.accent), "  Double talk (adaptation paused)");
                Tip("You and the far end are talking at the same time; the canceller holds its model so your voice "
                    "is not removed.");
            }
        }
    }
}

bool EchoApp::noiseSuppressionCheckbox(StripModel& strip, const char* label) {
    Effect* ns = findType(strip, noiseSuppressorTypeId());
    bool on = ns && !ns->bypass.load();
    const bool clicked = ImGui::Checkbox(label, &on);
    if (clicked) {
        if (ns) {
            ns->bypass = !on;
        } else if (on) {
            int pos = 0; // after an echo canceller, before everything else
            while (pos < static_cast<int>(strip.chain.size()) &&
                   std::string(strip.chain[pos]->typeId()) == echoCancellerTypeId())
                ++pos;
            const ChainTarget t{false, strip.id};
            defer([this, t, pos]() { mc_->addEffect(t, noiseSuppressorTypeId(), pos); });
        }
    }
    Tip("Neural background-noise removal (fans, keyboard and mouse clicks, air conditioning, hiss). Runs locally on "
        "the CPU. More settings under Voices > Noise & echo.");
    return clicked;
}

bool EchoApp::echoReferencePicker(StripModel& strip, float width) {
    std::string current = "Default playback device";
    if (strip.reference.kind == SourceKind::RenderLoopback && !strip.reference.deviceId.empty())
        current = strip.reference.deviceName.empty() ? strip.reference.deviceId : strip.reference.deviceName;
    bool changed = false;
    ImGui::SetNextItemWidth(width);
    if (ImGui::BeginCombo("##echoref", current.c_str())) {
        auto pick = [&](const std::string& id, const std::string& name) {
            strip.reference.kind = SourceKind::RenderLoopback;
            strip.reference.deviceId = id;
            strip.reference.deviceName = name;
            changed = true;
        };
        if (ImGui::Selectable("Default playback device", strip.reference.deviceId.empty())) pick("", "");
        for (const auto& d : backend_->devices()) {
            if (!d.render || !d.active || d.isCable) continue;
            if (ImGui::Selectable(d.name.c_str(), d.id == strip.reference.deviceId)) pick(d.id, d.name);
        }
        ImGui::EndCombo();
    }
    Tip("The speakers your microphone hears. Echo listens to what plays there (locally, nothing is recorded) and "
        "removes it from the microphone.");
    if (changed) {
        const uint32_t id = strip.id;
        defer([this, id]() {
            if (mc_->session().strip(id)) mc_->chainChanged(); // reopens the reference input
        });
    }
    return changed;
}

void EchoApp::drawNoiseEchoPanel(StripModel& strip) {
    const Palette& p = pal();
    const ChainTarget t{false, strip.id};
    const double sr = mc_->engine().sampleRate();
    // Side by side when the panel is wide enough, stacked otherwise.
    const bool twoCol = ImGui::GetContentRegionAvail().x > 900.0f * dpi_;
    if (twoCol && !ImGui::BeginTable("##noiseecho", 2, ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_BordersInnerV |
                                                            ImGuiTableFlags_PadOuterX))
        return;
    if (twoCol) ImGui::TableNextColumn();

    // ---------------------------------------------------------------- noise
    ImGui::SeparatorText("Background noise suppression");
    Effect* ns = findType(strip, noiseSuppressorTypeId());
    if (!ns) {
        ImGui::TextWrapped("Removes fans, keyboard and mouse clicks, air conditioning, hiss and room noise while "
                           "keeping your voice natural. It runs on this computer's CPU; nothing is recorded or "
                           "uploaded.");
        if (!mc_->isMicrophoneSource(strip.source))
            ImGui::TextDisabled("(This input is not a microphone, so it is off by default.)");
        if (ImGui::Button(ICON_ADD " Turn on noise suppression")) {
            int pos = 0;
            while (pos < static_cast<int>(strip.chain.size()) &&
                   std::string(strip.chain[pos]->typeId()) == echoCancellerTypeId())
                ++pos;
            defer([this, t, pos]() { mc_->addEffect(t, noiseSuppressorTypeId(), pos); });
        }
    } else {
        bool on = !ns->bypass.load();
        if (ToggleButton(on ? ICON_POWER " Noise suppression ON" : ICON_POWER " Noise suppression OFF", &on, p.good))
            ns->bypass = !on;
        Tip("Switches noise suppression for this input on or off instantly (no gap in the audio).");
        ImGui::SameLine();
        HoldToCompare("Hold to hear without it", *ns, "Press and hold to hear the microphone without noise suppression.");
        ImGui::SameLine();
        const uint32_t iid = ns->instanceId;
        if (ImGui::SmallButton(ICON_CLOSE " Remove")) defer([this, t, iid]() { mc_->removeEffect(t, iid); });
        Tip("Removes noise suppression from this input. It stays removed (also after restarting) until you turn it "
            "on again.");
        drawCleanupStatus(*ns);
        ImGui::Spacing();
        ParamControl(*ns, 0);
        ImGui::SameLine();
        ParamControl(*ns, 1);
        ImGui::SameLine();
        ImGui::BeginGroup();
        ParamControl(*ns, 2);
        if (ns->param(2) >= 0.5f) {
            ParamControl(*ns, 3);
            ImGui::SameLine();
            ParamControl(*ns, 4);
            ImGui::SameLine();
            ParamControl(*ns, 5);
        }
        ImGui::EndGroup();
        LatencyCpu(*ns, sr);
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        ImGui::TextWrapped("Tip: Strength 100%% removes the most; 60-85%% keeps a little natural room sound. The "
                           "voice gate also silences breathing and noise between sentences.");
        ImGui::PopStyleColor();
    }
    bool autoNs = mc_->session().autoNoiseSuppression;
    if (ImGui::Checkbox("Turn on noise suppression for newly added microphones", &autoNs)) {
        mc_->session().autoNoiseSuppression = autoNs;
        mc_->markDirty();
    }
    Tip("Only affects microphones you add later. Each input keeps its own setting.");

    // ----------------------------------------------------------------- echo
    if (twoCol) ImGui::TableNextColumn();
    else ImGui::Spacing();
    ImGui::SeparatorText("Echo cancellation");
    Effect* aec = findType(strip, echoCancellerTypeId());
    const float ctlW = std::min(ImGui::GetContentRegionAvail().x, 520.0f * dpi_);
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::TextWrapped("For speakers instead of headphones: removes the sound of your speakers (game, music, other "
                       "people's voices) that the microphone picks up, so others don't hear themselves.");
    ImGui::PopStyleColor();
    if (!aec) {
        if (ImGui::Button(ICON_ADD " Turn on echo cancellation")) {
            const uint32_t id = strip.id;
            defer([this, t, id]() {
                if (StripModel* s = mc_->session().strip(id)) {
                    if (s->reference.kind != SourceKind::RenderLoopback) {
                        s->reference = SourceRef{};
                        s->reference.kind = SourceKind::RenderLoopback; // default playback device
                    }
                    mc_->addEffect(t, echoCancellerTypeId(), 0); // first: works on the raw microphone
                }
            });
        }
        Tip("Not needed when you use headphones.");
    } else {
        bool on = !aec->bypass.load();
        if (ToggleButton(on ? ICON_POWER " Echo cancellation ON" : ICON_POWER " Echo cancellation OFF", &on, p.good))
            aec->bypass = !on;
        ImGui::SameLine();
        const uint32_t iid = aec->instanceId;
        if (ImGui::SmallButton(ICON_CLOSE " Remove##aec")) defer([this, t, iid]() { mc_->removeEffect(t, iid); });
        ImGui::TextUnformatted(ICON_SPEAKERS " Speakers to cancel:");
        ImGui::SameLine();
        echoReferencePicker(strip, ctlW * 0.6f);
        drawCleanupStatus(*aec);
        ImGui::Spacing();
        for (int i = 0; i < aec->numParams(); ++i) {
            if (i) ImGui::SameLine();
            ParamControl(*aec, i);
        }
        LatencyCpu(*aec, sr);
    }
    if (twoCol) ImGui::EndTable();
}

} // namespace echo::ui
