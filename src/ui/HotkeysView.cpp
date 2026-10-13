// Settings > Hotkeys: push to talk / push to mute, the mute key, voice
// hotkeys, and the key capture field every hotkey setting uses.
#include <algorithm>
#include <string>

#include "imgui.h"
#include "ui/App.h"
#include "ui/Icons.h"
#include "ui/Pop.h"
#include "ui/Theme.h"
#include "ui/Widgets.h"

namespace echo::ui {

namespace {
constexpr int kVoiceCaptureBase = 100; // + voice hotkey index

bool isVoicePreset(const ChainPreset& p) { return p.category != "Music"; }
} // namespace

bool EchoApp::keyCaptureButton(const char* id, int target, KeyBinding* out, float minWidth) {
    ImGui::PushID(id);
    bool chosen = false;
    const bool capturing = ImGui::IsPopupOpen("Choose a key");
    const std::string name = (out->empty() ? std::string("Set a key") : keyBindingName(*out)) + "##key";
    bool start = pop::KeyCap(name.c_str(), capturing, minWidth);
    Tip("Click, then press a key, mouse button or controller button.");
    if (autoCapture_ == target) {
        autoCapture_ = -1;
        start = true;
    }
    if (start) {
        hotkeys_->beginCapture(target);
        captureTarget_ = target;
        ImGui::OpenPopup("Choose a key");
    }
    if (ImGui::BeginPopupModal("Choose a key", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar)) {
        pop::CardTitle("Press a key or button");
        pop::Caption("Keyboard keys, mouse buttons (side buttons and wheel tilt too) and game controller buttons all work. "
                     "You can hold Ctrl, Shift or Alt with it.");
        pop::Hint("Esc cancels. Left and right click can't be used.");
        int got = -1;
        if (auto b = hotkeys_->takeCapture(&got)) {
            if (got == target) {
                *out = *b;
                chosen = true;
                mc_->markDirty();
            }
            ImGui::CloseCurrentPopup();
        } else if (!hotkeys_->capturing()) {
            ImGui::CloseCurrentPopup();
        }
        if (pop::SmallButton("Cancel", pop::Btn::Quiet)) {
            hotkeys_->cancelCapture();
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    ImGui::PopID();
    return chosen;
}

std::string EchoApp::otherUseOfKey(const KeyBinding& k, const KeyBinding* self) {
    if (k.empty()) return {};
    const HotkeySettings& h = mc_->session().hotkeys;
    auto same = [&](const KeyBinding& o) { return &o != self && o == k; };
    if (h.talkMode != TalkMode::Off && same(h.talkKey))
        return h.talkMode == TalkMode::PushToTalk ? "is your push-to-talk key" : "is your push-to-mute key";
    if (same(h.muteKey)) return "mutes your microphone";
    for (const auto& v : h.voiceKeys)
        if (same(v.key)) return "switches the voice " + (v.voices.empty() ? std::string("(none chosen)") : voicesLabel(v.voices));
    const SoundboardSettings& sb = mc_->session().soundboard;
    if (same(sb.stopAllKey)) return "stops all sounds";
    for (const auto& p : sb.pads)
        if (same(p.key)) return "plays the sound '" + p.name + "'";
    return {};
}

void EchoApp::drawHotkeySettings() {
    const Palette& p = pal();
    const float sc = uiScale();
    HotkeySettings& h = mc_->session().hotkeys;

    auto keyRow = [&](const char* label, const char* id, KeyBinding& key, int target) {
        ImGui::PushID(id);
        ImGui::AlignTextToFramePadding();
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 8.0f * sc);
        ImGui::TextColored(toVec4(p.text2), "%s", label);
        ImGui::SameLine(140.0f * sc);
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() - 8.0f * sc);
        KeyBinding chosen = key;
        if (keyCaptureButton("key", target, &chosen)) {
            const KeyBinding& other = &key == &h.talkKey ? h.muteKey : h.talkKey;
            if (chosen == other) toast(keyBindingName(chosen) + " is already used for " + (&key == &h.talkKey ? "mute" : "push to talk") + ".", 2);
            else key = chosen;
        }
        if (!key.empty()) {
            ImGui::SameLine(0, 8.0f * sc);
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 3.0f * sc);
            if (pop::SmallButton("Clear", pop::Btn::Quiet)) {
                key = {};
                mc_->markDirty();
            }
        }
        const std::string other = otherUseOfKey(key, &key);
        if (!other.empty()) pop::Hint(("This key also " + other + ".").c_str());
        ImGui::PopID();
    };

    pop::BeginCard("##ptt");
    pop::CardTitle("Push to talk");
    pop::Caption("Hotkeys work while a game or another app is in front. Echo only checks whether the keys are held "
                 "down; they still reach the app you are using.");
    ImGui::Dummy(ImVec2(0, 2.0f * sc));
    int mode = static_cast<int>(h.talkMode);
    const char* modes[] = {"Off", "Push to talk", "Push to mute"};
    if (pop::Segmented("##mode", &mode, modes, 3, 420.0f * sc)) {
        h.talkMode = static_cast<TalkMode>(mode);
        mc_->markDirty();
    }
    if (h.talkMode != TalkMode::Off) {
        ImGui::Dummy(ImVec2(0, 4.0f * sc));
        keyRow(h.talkMode == TalkMode::PushToTalk ? "Hold to talk" : "Hold to mute", "talk", h.talkKey, win::HotkeyService::TalkKey);
        if (h.talkMode == TalkMode::PushToTalk) {
            ImGui::Dummy(ImVec2(0, 4.0f * sc));
            float delay = static_cast<float>(h.releaseDelayMs);
            char val[32];
            std::snprintf(val, sizeof(val), "%.2f s", h.releaseDelayMs / 1000.0);
            if (pop::SliderRow("Release delay", val, "##delay", &delay, 0.0f, 1000.0f, 200.0f, 420.0f * sc)) {
                h.releaseDelayMs = static_cast<int>(delay / 10.0f + 0.5f) * 10;
                mc_->markDirty();
            }
            pop::Hint("Keeps the microphone on for a moment after you let go, so the end of a word isn't cut off.");
        }
        ImGui::Dummy(ImVec2(0, 4.0f * sc));
        if (h.talkKey.empty()) {
            pop::Banner(ICON_WARNING, "Choose a key. Until then your microphone stays on.", p.warnTint, p.warn);
        } else {
            const bool open = hotkeys_->gateOpen();
            std::string names;
            for (const StripModel* s : mc_->microphoneStrips()) names += (names.empty() ? "" : ", ") + s->name;
            const std::string msg = std::string(open ? "Microphone on" : "Microphone off") + "   ·   Applies to " +
                                    (names.empty() ? std::string("no microphone inputs yet") : names) +
                                    ". App audio keeps playing.";
            pop::Banner(ICON_MIC, msg.c_str(), open ? p.goodTint : p.surfaceAlt, open ? p.good : p.textDim);
        }
    }
    pop::EndCard();

    ImGui::Dummy(ImVec2(0, 4.0f * sc));
    pop::BeginCard("##mutekey");
    pop::CardTitle("Mute key");
    pop::Caption("Press once to mute every microphone input, again to unmute.");
    keyRow("Mute / unmute", "mute", h.muteKey, win::HotkeyService::MuteKey);
    ImGui::Dummy(ImVec2(0, 4.0f * sc));
    const bool muted = mc_->microphonesMuted();
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(toVec4(p.text2), "Microphones are %s.", muted ? "muted" : "on");
    ImGui::SameLine(0, 12.0f * sc);
    if (pop::SmallButton(muted ? "Unmute now" : "Mute now")) toggleMicrophoneMute();
    pop::EndCard();

    ImGui::Dummy(ImVec2(0, 4.0f * sc));
    drawVoiceHotkeys();

    ImGui::Dummy(ImVec2(0, 4.0f * sc));
    pop::BeginCard("##sbkeys", 0.0f, 0.0f, true);
    pop::CardTitle("Soundboard keys");
    int keys = 0;
    for (const auto& pad : mc_->session().soundboard.pads) keys += !pad.key.empty();
    const std::string summary = keys ? std::to_string(keys) + (keys == 1 ? " sound has a key." : " sounds have keys.")
                                     : std::string("No sound has a key yet.");
    pop::Caption((summary + " Give a sound a key on the Soundboard screen (right-click a sound).").c_str());
    if (pop::LinkButton("Open the soundboard")) navigate(Page::Soundboard);
    pop::EndCard();

    if (!hotkeys_->rawInputActive() && (!h.talkKey.empty() || !h.muteKey.empty() || !h.voiceKeys.empty()))
        pop::Hint("Windows didn't allow Echo to read input events, so very short presses can be missed. Keys still work "
                  "when held for a moment.");
    if (const int pads = hotkeys_->controllersConnected(); pads > 0)
        pop::Hint((std::to_string(pads) + (pads == 1 ? " game controller" : " game controllers") +
                   " connected: its buttons can be used as hotkeys.").c_str());
}

void EchoApp::drawVoiceHotkeys() {
    const float sc = uiScale();
    HotkeySettings& h = mc_->session().hotkeys;
    pop::BeginCard("##voicekeys");
    pop::CardTitle("Voice hotkeys");
    pop::Caption("Switch voices with a key, also while you're in a game. A key can turn on one voice or several at once; "
                 "they act on your main microphone.");
    pop::Hint("Toggle: press to add the voices, press again to take them out.   Hold: on only while you hold the key.   "
              "Switch: press to use only these voices, press again for your own voice.");
    ImGui::Dummy(ImVec2(0, 4.0f * sc));
    int removeAt = -1;
    for (size_t i = 0; i < h.voiceKeys.size(); ++i) {
        VoiceHotkey& v = h.voiceKeys[i];
        ImGui::PushID(static_cast<int>(i));
        keyCaptureButton("key", kVoiceCaptureBase + static_cast<int>(i), &v.key, 170.0f);
        ImGui::SameLine(0, 10.0f * sc);
        int mode = static_cast<int>(v.mode);
        const char* modes[] = {"Toggle", "Hold", "Switch"};
        ImGui::BeginGroup();
        if (pop::Segmented("##mode", &mode, modes, kVoiceKeyModes, 250.0f * sc)) {
            v.mode = static_cast<VoiceKeyMode>(mode);
            holdAdded_.erase(i);
            mc_->markDirty();
        }
        ImGui::EndGroup();
        ImGui::SameLine(0, 10.0f * sc);
        const std::string label = (v.voices.empty() ? std::string("Choose voices") : voicesLabel(v.voices)) + "  " ICON_DOWN "##voices";
        if (pop::Button(label.c_str())) ImGui::OpenPopup("voices");
        if (ImGui::BeginPopup("voices")) {
            pop::Label("Voices this key turns on");
            for (const auto& pr : presets_.all()) {
                if (!isVoicePreset(pr)) continue;
                bool on = std::find(v.voices.begin(), v.voices.end(), pr.name) != v.voices.end();
                if (ImGui::Checkbox(pr.name.c_str(), &on)) {
                    if (on) v.voices.push_back(pr.name);
                    else v.voices.erase(std::remove(v.voices.begin(), v.voices.end(), pr.name), v.voices.end());
                    mc_->markDirty();
                }
            }
            ImGui::EndPopup();
        }
        ImGui::SameLine(0, 10.0f * sc);
        if (pop::SmallButton(ICON_DELETE "##remove", pop::Btn::Quiet)) removeAt = static_cast<int>(i);
        Tip("Remove this hotkey");
        const std::string other = otherUseOfKey(v.key, &v.key);
        if (!other.empty()) pop::Hint(("This key also " + other + ".").c_str());
        ImGui::PopID();
        ImGui::Dummy(ImVec2(0, 2.0f * sc));
    }
    if (removeAt >= 0) {
        h.voiceKeys.erase(h.voiceKeys.begin() + removeAt);
        holdAdded_.clear();
        mc_->markDirty();
    }
    if (h.voiceKeys.empty()) pop::Hint("No voice hotkeys yet.");
    if (pop::Button(ICON_ADD "  Add a voice hotkey")) {
        VoiceHotkey v;
        if (StripModel* mic = primaryMic()) v.voices = mic->voices;
        h.voiceKeys.push_back(v);
        autoCapture_ = kVoiceCaptureBase + static_cast<int>(h.voiceKeys.size() - 1);
        mc_->markDirty();
    }
    pop::EndCard();
}

} // namespace echo::ui
