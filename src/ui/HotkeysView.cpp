// Hotkeys: push to talk / push to mute and the toggle-mute key (a tab in
// Settings), plus the push-to-talk indicator in the status bar.
#include "imgui.h"
#include "ui/App.h"
#include "ui/Icons.h"
#include "ui/Theme.h"
#include "ui/Widgets.h"

namespace echo::ui {

void EchoApp::drawHotkeySettings() {
    const Palette& p = pal();
    HotkeySettings& h = mc_->session().hotkeys;
    ImGui::TextWrapped("Hotkeys work while a game or another app is in front. Echo only checks whether the keys "
                       "are held down; they still reach the app you are using.");

    auto keyButton = [&](const char* id, KeyBinding& key, win::HotkeyService::Target target) {
        ImGui::PushID(id);
        const std::string label = keyBindingName(key);
        if (ImGui::Button((label + "##key").c_str(), ImVec2(200 * dpi_, 0))) {
            hotkeys_->beginCapture(target);
            openKeyCapture_ = true;
        }
        Tip("Click, then press the key or mouse button to use.");
        if (!key.empty()) {
            ImGui::SameLine();
            if (ImGui::Button("Clear")) {
                key = {};
                mc_->markDirty();
            }
        }
        ImGui::PopID();
    };

    ImGui::SeparatorText("Push to talk");
    int mode = static_cast<int>(h.talkMode);
    const char* modes[] = {"Off (microphone always on)", "Push to talk (hold to talk)", "Push to mute (hold to mute)"};
    ImGui::SetNextItemWidth(260 * dpi_);
    if (ImGui::Combo("Mode", &mode, modes, 3)) {
        h.talkMode = static_cast<TalkMode>(mode);
        mc_->markDirty();
    }
    if (h.talkMode != TalkMode::Off) {
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(h.talkMode == TalkMode::PushToTalk ? "Hold to talk:" : "Hold to mute:");
        ImGui::SameLine(130 * dpi_);
        keyButton("talk", h.talkKey, win::HotkeyService::TalkKey);
        if (h.talkMode == TalkMode::PushToTalk) {
            ImGui::SetNextItemWidth(260 * dpi_);
            if (ImGui::SliderInt("Release delay", &h.releaseDelayMs, 0, 1000, "%d ms")) mc_->markDirty();
            Tip("Keeps the microphone on for a moment after you let go, so the end of a word is not cut off.");
        }
        if (h.talkKey.empty()) {
            ImGui::TextColored(toVec4(p.warn), ICON_WARNING " Choose a key. Until then your microphone stays on.");
        } else {
            const bool open = hotkeys_->gateOpen();
            Led(open, open ? p.good : p.textDim, ImGui::GetFontSize() * 0.3f);
            ImGui::SameLine();
            ImGui::TextUnformatted(open ? "Microphone on" : "Microphone off");
        }
        std::string names;
        for (const StripModel* s : mc_->microphoneStrips()) names += (names.empty() ? "" : ", ") + s->name;
        ImGui::TextDisabled("Applies to: %s", names.empty() ? "no microphone inputs yet" : names.c_str());
        Tip("Push to talk silences microphone inputs (including what you hear of yourself). App audio and "
            "other inputs keep playing.");
    }

    ImGui::SeparatorText("Mute key");
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Mute / unmute:");
    ImGui::SameLine(130 * dpi_);
    keyButton("mute", h.muteKey, win::HotkeyService::MuteKey);
    const bool muted = mc_->microphonesMuted();
    ImGui::TextDisabled("Microphones are %s.", muted ? "muted" : "on");
    ImGui::SameLine();
    if (ImGui::SmallButton(muted ? "Unmute now" : "Mute now")) toggleMicrophoneMute();

    // Waiting for the key press.
    if (openKeyCapture_) {
        ImGui::OpenPopup("Choose a key");
        openKeyCapture_ = false;
    }
    if (ImGui::BeginPopupModal("Choose a key", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted("Press the key or mouse button you want to use.");
        ImGui::TextDisabled("You can hold Ctrl, Shift or Alt with it. Esc cancels.");
        ImGui::TextDisabled("Left and right click can't be used.");
        win::HotkeyService::Target target = win::HotkeyService::TalkKey;
        if (auto b = hotkeys_->takeCapture(&target)) {
            KeyBinding& dst = target == win::HotkeyService::TalkKey ? h.talkKey : h.muteKey;
            const KeyBinding& other = target == win::HotkeyService::TalkKey ? h.muteKey : h.talkKey;
            if (*b == other) {
                toast(keyBindingName(*b) + " is already used for " +
                          (target == win::HotkeyService::TalkKey ? "mute" : "push to talk") + ".", 2);
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
}

void EchoApp::drawHotkeyStatus() {
    const Palette& p = pal();
    const HotkeySettings& h = mc_->session().hotkeys;
    if (h.talkMode != TalkMode::Off && !h.talkKey.empty() && hotkeys_) {
        const bool open = hotkeys_->gateOpen();
        ImGui::SameLine(0, 20);
        Led(open, open ? p.good : p.textDim, ImGui::GetFontSize() * 0.3f);
        ImGui::SameLine();
        ImGui::Text("%s: %s", talkModeName(h.talkMode), open ? "mic on" : "mic off");
        Tip(("Hold " + keyBindingName(h.talkKey) + (h.talkMode == TalkMode::PushToTalk ? " to talk." : " to mute.")).c_str());
    }
    if (mc_->microphonesMuted()) {
        ImGui::SameLine(0, 20);
        ImGui::TextColored(toVec4(p.warn), "Microphone muted");
        if (!h.muteKey.empty()) Tip(("Press " + keyBindingName(h.muteKey) + " to unmute.").c_str());
    }
}

} // namespace echo::ui
