// Home: your microphone (mute, level, push to talk), the switches you use
// most, what else goes into your mic, and quick voice picks.
#include <algorithm>
#include <cstdio>

#include "dsp/NoiseSuppressorInfo.h"
#include "imgui.h"
#include "ui/App.h"
#include "ui/Icons.h"
#include "ui/Pop.h"
#include "ui/Theme.h"
#include "ui/Widgets.h"

namespace echo::ui {

namespace {

Effect* findType(StripModel& s, const char* type) {
    for (auto& e : s.chain)
        if (std::string(e->typeId()) == type) return e.get();
    return nullptr;
}

bool isCleanup(const Effect& e) {
    const std::string t = e.typeId();
    return t == noiseSuppressorTypeId() || t == echoCancellerTypeId();
}

} // namespace

std::string voiceLabel(const StripModel& s) {
    bool voiceFx = false;
    for (const auto& e : s.chain) voiceFx |= !isCleanup(*e);
    if (!voiceFx) return "your own voice";
    return s.voices.empty() ? "your custom effects" : voicesLabel(s.voices);
}

void EchoApp::drawHome() {
    const Palette& p = pal();
    const float sc = uiScale();
    Session& s = mc_->session();
    StripModel* mic = primaryMic();
    BusModel* a1 = firstBus(BusKind::Physical);
    BusModel* b1 = firstBus(BusKind::Virtual);
    const float avail = ImGui::GetContentRegionAvail().x;
    const float gap = 20.0f * sc;
    const bool wide = avail > 860.0f * sc;
    const float leftW = wide ? (avail - gap) * 0.62f : avail;

    // ---------------------------------------------------------------- mic card
    pop::BeginCard("##mic", leftW, 0.0f, false, 24.0f, 22.0f);
    if (!mic) {
        pop::CardTitle("No microphone yet");
        pop::Caption("Add your microphone to start using your voice in Echo.");
        if (pop::Button("Choose a microphone", pop::Btn::Primary)) navigate(Page::Devices);
    } else {
        const HotkeySettings& hk = s.hotkeys;
        const bool muted = mc_->microphonesMuted();
        const bool talkMode = hk.talkMode != TalkMode::Off && !hk.talkKey.empty();
        const bool gateOpen = !hotkeys_ || hotkeys_->gateOpen();
        std::string dev = mic->source.deviceId.empty() ? "Windows default microphone" : mic->source.deviceName;
        std::string headline;
        if (!mic->online) headline = "Microphone not connected";
        else if (muted) headline = "Microphone muted";
        else if (talkMode && hk.talkMode == TalkMode::PushToTalk) headline = gateOpen ? "You're talking" : "Hold " + keyBindingName(hk.talkKey) + " to talk";
        else if (talkMode) headline = gateOpen ? "You're live" : "Muted while you hold " + keyBindingName(hk.talkKey);
        else headline = "You're live";

        const float btnW = 110.0f * sc;
        const float top = ImGui::GetCursorPosY();
        ImGui::BeginGroup();
        ImGui::PushFont(fonts().regular, 13.0f);
        ImGui::TextColored(toVec4(p.text2), "%s · %s", mic->name.c_str(), dev.c_str());
        ImGui::PopFont();
        ImGui::PushFont(fonts().semibold, 30.0f);
        ImGui::TextUnformatted(headline.c_str());
        ImGui::PopFont();
        ImGui::EndGroup();
        const float contentW = ImGui::GetContentRegionAvail().x;
        ImGui::SameLine(contentW - btnW + ImGui::GetStyle().WindowPadding.x);
        ImGui::SetCursorPosY(top + 6.0f * sc);
        ImGui::BeginGroup();
        if (pop::Button(muted ? "Unmute" : "Mute", muted ? pop::Btn::Primary : pop::Btn::Secondary, ImVec2(btnW, 44.0f * sc)))
            toggleMicrophoneMute();
        if (!hk.muteKey.empty()) {
            const std::string key = keyBindingName(hk.muteKey);
            ImGui::PushFont(fonts().regular, 12.0f);
            const float tw = ImGui::CalcTextSize(key.c_str()).x;
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (btnW - tw) * 0.5f);
            ImGui::TextColored(toVec4(p.textDim), "%s", key.c_str());
            ImGui::PopFont();
        }
        ImGui::EndGroup();

        ImGui::Dummy(ImVec2(0, 2.0f * sc));
        const StripMeters& m = stripMeters(mic->id);
        pop::SegMeter(std::max(m.out.levelDb[0], m.out.levelDb[1]), ImGui::GetContentRegionAvail().x, 14.0f * sc);
        Tip("Your microphone after its effects, volume and mute.");

        if (talkMode) {
            ImGui::Dummy(ImVec2(0, 2.0f * sc));
            const ImVec2 bp = ImGui::GetCursorScreenPos();
            const float bw = ImGui::GetContentRegionAvail().x, bh = 62.0f * sc;
            ImGui::GetWindowDrawList()->AddRectFilled(bp, ImVec2(bp.x + bw, bp.y + bh), p.surfaceAlt, 12.0f * sc);
            ImGui::SetCursorScreenPos(ImVec2(bp.x + 12.0f * sc, bp.y + 11.0f * sc));
            if (pop::KeyCap(keyBindingName(hk.talkKey).c_str(), gateOpen && hk.talkMode == TalkMode::PushToTalk, 110.0f))
                openSettings(SettingsTab::Hotkeys);
            ImGui::SameLine(0, 14.0f * sc);
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 10.0f * sc);
            ImGui::PushFont(fonts().regular, 13.0f);
            ImGui::TextColored(toVec4(p.text2), hk.talkMode == TalkMode::PushToTalk
                                                    ? "Push to talk is on: hold the key to talk. Your mic stays on for a moment after you let go."
                                                    : "Push to mute is on: hold the key to mute.");
            ImGui::PopFont();
            ImGui::SameLine();
            ImGui::SetCursorScreenPos(ImVec2(bp.x + bw - 70.0f * sc, bp.y + 21.0f * sc));
            if (pop::LinkButton("Hotkeys")) openSettings(SettingsTab::Hotkeys);
            ImGui::SetCursorScreenPos(ImVec2(bp.x, bp.y + bh));
            ImGui::Dummy(ImVec2(bw, 0));
        }

        ImGui::Dummy(ImVec2(0, 2.0f * sc));
        std::string capture;
        if (b1)
            for (const auto& c : backend_->cablePairs())
                if (c.renderId == b1->output.deviceId) capture = c.captureName;
        ImGui::PushFont(fonts().regular, 15.0f);
        ImGui::TextColored(toVec4(p.text2), "Sounding like");
        ImGui::SameLine(0, 4.0f * sc);
        ImGui::PushFont(fonts().semibold, 15.0f);
        ImGui::TextUnformatted((voiceLabel(*mic) + ".").c_str());
        ImGui::PopFont();
        ImGui::PopFont();
        if (!capture.empty()) {
            pop::Caption(("Select \"" + capture + "\" as the microphone in your app.").c_str());
        } else {
            pop::Caption("Other apps can't hear Echo yet: set up a virtual mic.");
            if (pop::LinkButton("Set up the virtual mic")) navigate(Page::Devices);
        }
        if (!mic->online && !mic->status.empty()) pop::Banner(ICON_WARNING, mic->status.c_str(), p.badTint, p.bad);
    }
    pop::EndCard();

    // -------------------------------------------------------------- quick card
    if (wide) ImGui::SameLine(0, gap);
    else ImGui::Dummy(ImVec2(0, 4.0f * sc));
    pop::BeginCard("##quick", wide ? avail - leftW - gap : avail, 0.0f, false, 20.0f, 6.0f);
    if (mic) {
        Effect* ns = findType(*mic, noiseSuppressorTypeId());
        bool noise = ns && !ns->bypass.load();
        if (pop::SwitchRow("Noise removal", "Fans, typing, mouse clicks, hum", &noise)) {
            if (ns) {
                ns->bypass = !noise;
            } else if (noise) {
                int pos = 0;
                while (pos < static_cast<int>(mic->chain.size()) && std::string(mic->chain[pos]->typeId()) == echoCancellerTypeId()) ++pos;
                const ChainTarget t{false, mic->id};
                defer([this, t, pos]() { mc_->addEffect(t, noiseSuppressorTypeId(), pos); });
            }
        }
        if (a1) {
            bool hear = mic->rt->sends[a1->slot].enabled.load();
            if (pop::SwitchRow("Hear myself", ("Your voice in " + a1->name).c_str(), &hear)) mc_->setSend(mic->id, a1->id, hear);
        }
    }
    if (b1) {
        ImGui::Dummy(ImVec2(0, 4.0f * sc));
        const float rowW = ImGui::GetContentRegionAvail().x;
        ImGui::PushFont(fonts().semibold, 15.0f);
        ImGui::TextUnformatted("Also in your mic");
        ImGui::PopFont();
        ImGui::SameLine(rowW - 76.0f * sc + ImGui::GetStyle().WindowPadding.x);
        if (pop::LinkButton("Add an app")) navigate(Page::Apps);
        bool any = false;
        for (auto& st : s.strips) {
            if (st.group != StripGroup::Application && st.group != StripGroup::Virtual) continue;
            any = true;
            ImGui::PushID(static_cast<int>(st.id));
            SendParams& sp = st.rt->sends[b1->slot];
            bool on = sp.enabled.load();
            const ImVec2 rp = ImGui::GetCursorScreenPos();
            ImGui::PushFont(fonts().regular, 14.0f);
            ImGui::SetCursorScreenPos(ImVec2(rp.x, rp.y + 6.0f * sc));
            ImGui::TextUnformatted(st.name.c_str());
            ImGui::PopFont();
            ImGui::SetCursorScreenPos(ImVec2(rp.x + rowW - 46.0f * sc, rp.y));
            if (pop::Switch("##inmic", &on)) mc_->setSend(st.id, b1->id, on);
            Tip(on ? "In your mic. Switch off to keep it out." : "Not in your mic. Switch on to mix it in.");
            if (on) {
                float g = sp.gainDb.load();
                char val[24];
                if (g <= -59.9f) std::snprintf(val, sizeof(val), "Off");
                else std::snprintf(val, sizeof(val), "%+.1f dB", g);
                ImGui::SetCursorScreenPos(ImVec2(rp.x, rp.y + 34.0f * sc));
                if (pop::Slider("##gain", &g, -60.0f, 12.0f, 0.0f, rowW - 70.0f * sc)) sp.gainDb = g;
                Tip("How loud this is in your mic (double-click: 0 dB).");
                ImGui::SameLine(0, 8.0f * sc);
                ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 3.0f * sc);
                ImGui::PushFont(fonts().regular, 13.0f);
                ImGui::TextColored(toVec4(p.text2), "%s", val);
                ImGui::PopFont();
            } else {
                ImGui::SetCursorScreenPos(ImVec2(rp.x, rp.y + 30.0f * sc));
                ImGui::Dummy(ImVec2(rowW, 0));
            }
            ImGui::PopID();
        }
        if (!any) pop::Hint("Capture an app in Apps to add music or game sound to your mic.");
        ImGui::Dummy(ImVec2(0, 8.0f * sc));
    }
    pop::EndCard();

    // ------------------------------------------------------------------- voices
    ImGui::Dummy(ImVec2(0, 8.0f * sc));
    pop::SectionTitle("Voice");
    ImGui::SameLine(avail - 190.0f * sc);
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 3.0f * sc);
    if (pop::LinkButton("All voices and fine-tuning")) navigate(Page::Voices);
    if (!mic) return;
    const char* names[] = {"Deep Voice", "Chipmunk", "Robot", "Monster", "Radio Voice", "Hard Autotune", "Megaphone"};
    std::vector<const ChainPreset*> list = {nullptr};
    for (const char* n : names)
        if (const ChainPreset* pr = presets_.find(n)) list.push_back(pr);
    const float tgap = 12.0f * sc;
    const int cols = std::min(static_cast<int>(list.size()), pop::columnsFor(avail, 132.0f * sc, tgap));
    const float tw = (avail - tgap * (cols - 1)) / cols;
    bool voiceFx = false;
    for (const auto& e : mic->chain) voiceFx |= !isCleanup(*e);
    for (size_t i = 0; i < list.size(); ++i) {
        if (i % cols) ImGui::SameLine(0, tgap);
        const ChainPreset* pr = list[i];
        const bool sel = pr ? (voiceFx && mic->voices.size() == 1 && mic->voices[0] == pr->name) : !voiceFx;
        ImGui::PushID(static_cast<int>(i));
        if (pop::Tile("##tile", presetIcon(pr ? pr->name : ""), pr ? pr->name.c_str() : "Original voice", nullptr, nullptr, sel,
                      ImVec2(tw, 84.0f * sc)))
            applyVoice(*mic, pr);
        if (pr) Tip(pr->description.c_str());
        else Tip("Your own voice. Noise removal and echo removal stay as they are.");
        ImGui::PopID();
    }
}

} // namespace echo::ui
