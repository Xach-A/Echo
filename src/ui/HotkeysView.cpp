// Settings > Hotkeys: push to talk / push to mute and the mute key.
#include <string>

#include "imgui.h"
#include "ui/App.h"
#include "ui/Icons.h"
#include "ui/Pop.h"
#include "ui/Theme.h"
#include "ui/Widgets.h"

namespace echo::ui {

void EchoApp::drawHotkeySettings() {
    const Palette& p = pal();
    const float sc = uiScale();
    HotkeySettings& h = mc_->session().hotkeys;

    auto keyRow = [&](const char* label, const char* id, KeyBinding& key, win::HotkeyService::Target target) {
        ImGui::PushID(id);
        ImGui::AlignTextToFramePadding();
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 8.0f * sc);
        ImGui::TextColored(toVec4(p.text2), "%s", label);
        ImGui::SameLine(140.0f * sc);
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() - 8.0f * sc);
        const bool capturing = ImGui::IsPopupOpen("Choose a key");
        const std::string name = keyBindingName(key) + "##key";
        if (pop::KeyCap(name.c_str(), capturing)) {
            hotkeys_->beginCapture(target);
            ImGui::OpenPopup("Choose a key");
        }
        Tip("Click, then press the key or mouse button to use.");
        if (!key.empty()) {
            ImGui::SameLine(0, 8.0f * sc);
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 3.0f * sc);
            if (pop::SmallButton("Clear", pop::Btn::Quiet)) {
                key = {};
                mc_->markDirty();
            }
        }
        // Waiting for the key press.
        if (ImGui::BeginPopupModal("Choose a key", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar)) {
            pop::CardTitle("Press a key or mouse button");
            pop::Caption("You can hold Ctrl, Shift or Alt with it. Esc cancels.");
            pop::Hint("Left and right click can't be used.");
            win::HotkeyService::Target got = win::HotkeyService::TalkKey;
            if (auto b = hotkeys_->takeCapture(&got)) {
                KeyBinding& dst = got == win::HotkeyService::TalkKey ? h.talkKey : h.muteKey;
                const KeyBinding& other = got == win::HotkeyService::TalkKey ? h.muteKey : h.talkKey;
                if (*b == other) {
                    toast(keyBindingName(*b) + " is already used for " +
                              (got == win::HotkeyService::TalkKey ? "mute" : "push to talk") + ".", 2);
                } else {
                    dst = *b;
                    mc_->markDirty();
                }
                ImGui::CloseCurrentPopup();
            } else if (!hotkeys_->capturing()) {
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
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
        keyRow(h.talkMode == TalkMode::PushToTalk ? "Hold to talk" : "Hold to mute", "talk", h.talkKey,
               win::HotkeyService::TalkKey);
        if (h.talkMode == TalkMode::PushToTalk) {
            ImGui::Dummy(ImVec2(0, 4.0f * sc));
            float delay = static_cast<float>(h.releaseDelayMs);
            char val[32];
            std::snprintf(val, sizeof(val), "%d ms", h.releaseDelayMs);
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
}

} // namespace echo::ui
