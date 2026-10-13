// Soundboard: pads for Echo's built-in sounds and the user's own, played
// into the virtual mic (and the headphones). Each pad can have a hotkey and
// plays once, plays/stops, or plays while held.
#include <windows.h>
#include <shellapi.h>

#include <algorithm>
#include <cstdio>

#include "imgui.h"
#include "imgui_stdlib.h"
#include "session/Paths.h"
#include "ui/App.h"
#include "ui/Icons.h"
#include "ui/Pop.h"
#include "ui/Theme.h"
#include "ui/Widgets.h"

namespace echo::ui {

namespace {

constexpr int kPadCaptureBase = 10000; // + pad id
constexpr int kStopAllCapture = 50;

const char* padIcon(const SoundPad& p) {
    static const struct {
        const char* id;
        const char* icon;
    } icons[] = {{"airhorn", ICON_VOLUME},   {"rimshot", ICON_AUDIO},   {"sad_trombone", ICON_DOWN}, {"tada", ICON_CHECK},
                 {"ding", ICON_TONE},        {"buzzer", ICON_CLOSE},    {"drumroll", ICON_EQUALIZER}, {"applause", ICON_APPS},
                 {"boing", ICON_UP},         {"whoosh", ICON_SYNC},     {"laser", ICON_POWER},        {"explosion", ICON_SPEAKERS},
                 {"boom", ICON_HEADPHONES},  {"crickets", ICON_MUTE}};
    for (const auto& i : icons)
        if (p.builtin == i.id) return i.icon;
    return ICON_PLAY;
}

ImU32 padColor(const SoundPad& p, size_t index) {
    if (p.color) return p.color;
    static const unsigned palette[] = {0xFF7A59, 0xFFB020, 0x7CCB4A, 0x2EC4B6, 0x4D9DE0, 0x8C6FF7, 0xF06292, 0x8A94A6};
    return hex(palette[index % std::size(palette)]);
}

const char* modeShort(PadMode m) {
    switch (m) {
    case PadMode::Toggle: return "Play / stop";
    case PadMode::Hold: return "Hold";
    default: return "";
    }
}

} // namespace

void EchoApp::drawSoundboard() {
    const Palette& p = pal();
    const float sc = uiScale();
    Session& s = mc_->session();
    SoundboardSettings& sb = s.soundboard;
    pop::PageTitle("Soundboard", "Play sounds into your voice chat. Give any sound a key to play it while you're in a game.");
    const float avail = ImGui::GetContentRegionAvail().x;

    // ---------------------------------------------------------------- controls
    StripModel* strip = mc_->soundboardStrip();
    BusModel* micBus = firstBus(BusKind::Virtual);
    BusModel* phones = firstBus(BusKind::Physical);
    pop::BeginCard("##sbcontrols");
    if (!strip) {
        pop::CardTitle("The soundboard isn't connected");
        pop::Caption("Its input was removed from the mixer, so nobody can hear it.");
        if (pop::Button("Connect the soundboard", pop::Btn::Primary)) mc_->ensureSoundboardStrip();
    } else if (ImGui::BeginTable("##sbc", 2, ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("a", ImGuiTableColumnFlags_WidthStretch, 1.4f);
        ImGui::TableSetupColumn("b", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableNextColumn();
        float fader = strip->rt->faderDb.load();
        char v[24];
        std::snprintf(v, sizeof(v), "%+.0f dB", fader);
        if (pop::SliderRow("Volume", v, "##sbvol", &fader, -40.0f, 6.0f, 0.0f)) {
            strip->rt->faderDb = std::round(fader);
            mc_->markDirty();
        }
        if (micBus) {
            bool on = strip->rt->sends[micBus->slot].enabled.load();
            if (pop::SwitchRow("Others hear it", "Plays into the virtual mic (Discord, games, OBS)", &on)) {
                mc_->setSend(strip->id, micBus->id, on);
            }
        }
        if (phones) {
            bool on = strip->rt->sends[phones->slot].enabled.load();
            if (pop::SwitchRow("You hear it", "Plays in your headphones too", &on, false)) mc_->setSend(strip->id, phones->id, on);
        }
        ImGui::TableNextColumn();
        ImGui::Dummy(ImVec2(0, 2.0f * sc));
        if (pop::Button(importingSounds() ? "Adding sounds..." : ICON_ADD "  Add your own sounds", pop::Btn::Primary)) importSounds();
        Tip("WAV, MP3, M4A, AAC, WMA or FLAC. Up to 60 seconds each.");
        ImGui::SameLine(0, 8.0f * sc);
        const int playing = mc_->soundboard().playingCount();
        ImGui::BeginDisabled(playing == 0);
        if (pop::Button(ICON_STOP "  Stop all")) mc_->soundboard().stopAll();
        ImGui::EndDisabled();
        ImGui::Dummy(ImVec2(0, 4.0f * sc));
        pop::Label("Stop-all key");
        keyCaptureButton("stopall", kStopAllCapture, &sb.stopAllKey, 150.0f);
        if (!sb.stopAllKey.empty()) {
            ImGui::SameLine(0, 8.0f * sc);
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 3.0f * sc);
            if (pop::SmallButton("Clear##stopall", pop::Btn::Quiet)) {
                sb.stopAllKey = {};
                mc_->markDirty();
            }
        }
        const std::string other = otherUseOfKey(sb.stopAllKey, &sb.stopAllKey);
        if (!other.empty()) pop::Hint(("Also " + other + ".").c_str());
        ImGui::EndTable();
    }
    if (strip && micBus && micBus->output.deviceId.empty()) {
        pop::Banner(ICON_WARNING, "Set up the virtual mic so others can hear the soundboard (Devices screen).", p.warnTint, p.warn);
        if (pop::LinkButton("Set up the virtual mic")) navigate(Page::Devices);
    }
    pop::EndCard();
    ImGui::Dummy(ImVec2(0, 6.0f * sc));

    // -------------------------------------------------------------------- pads
    const float gap = 14.0f * sc;
    const int cols = pop::columnsFor(avail, 150.0f * sc, gap);
    const float tw = (avail - gap * (cols - 1)) / cols;
    const float th = std::clamp(tw * 0.85f, 140.0f * sc, 176.0f * sc);
    int shown = 0;
    for (size_t i = 0; i < sb.pads.size(); ++i) {
        SoundPad& pad = sb.pads[i];
        if (shown % cols) ImGui::SameLine(0, gap);
        ++shown;
        const std::string problem = padProblem(pad);
        const float progress = i < static_cast<size_t>(SoundboardSource::kMaxPads) ? mc_->soundboard().progress(static_cast<int>(i)) : -1.0f;
        const std::string key = pad.key.empty() ? std::string() : keyBindingName(pad.key);
        char id[32];
        std::snprintf(id, sizeof(id), "pad%u", pad.id);
        const pop::PadEvents ev = pop::SoundPadTile(id, padIcon(pad), pad.name.c_str(), key.c_str(), modeShort(pad.mode),
                                                    padColor(pad, i), progress, !problem.empty(), ImVec2(tw, th));
        if (!problem.empty()) Tip(problem.c_str());
        else if (ImGui::IsItemHovered()) Tip("Click to play. Right-click (or ... ) to rename it, give it a key or remove it.");
        if (ev.pressed && problem.empty()) triggerPad(static_cast<int>(i), true);
        if (ev.released) triggerPad(static_cast<int>(i), false);
        if (ev.edit) {
            editPad_ = pad.id;
            openPadEditor_ = true;
        }
    }
    if (sb.pads.size() < static_cast<size_t>(kMaxSoundPads)) {
        if (shown % cols) ImGui::SameLine(0, gap);
        if (pop::AddTile("##addpad", importingSounds() ? "Adding..." : "Add your own sounds", ImVec2(tw, th))) importSounds();
    }

    ImGui::Dummy(ImVec2(0, 6.0f * sc));
    bool missing = false;
    for (const auto& b : builtinSounds())
        missing |= std::none_of(sb.pads.begin(), sb.pads.end(), [&](const SoundPad& pd) { return pd.builtin == b.id; });
    if (missing && pop::LinkButton("Bring back Echo's built-in sounds")) {
        addBuiltinPads(sb);
        mc_->markDirty();
    }
    pop::Hint("Echo's built-in sounds are free recordings (credits in the sounds folder next to Echo). Sounds you add are "
              "copied into Echo's sounds folder; the files you picked are not changed. Keys work while a game or another app "
              "is in front.");
    if (pop::LinkButton("Open the sounds folder"))
        ShellExecuteW(nullptr, L"open", paths::soundsDir().c_str(), nullptr, nullptr, SW_SHOWNORMAL);

    drawPadEditor();
}

void EchoApp::drawPadEditor() {
    const float sc = uiScale();
    SoundboardSettings& sb = mc_->session().soundboard;
    if (openPadEditor_) {
        ImGui::OpenPopup("Edit sound");
        openPadEditor_ = false;
    }
    ImGui::SetNextWindowSize(ImVec2(460.0f * sc, 0.0f));
    if (!ImGui::BeginPopupModal("Edit sound", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar)) return;
    auto it = std::find_if(sb.pads.begin(), sb.pads.end(), [&](const SoundPad& pd) { return pd.id == editPad_; });
    if (it == sb.pads.end()) {
        ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
        return;
    }
    SoundPad& pad = *it;
    const int slot = static_cast<int>(it - sb.pads.begin());
    pop::CardTitle("Edit sound");
    pop::Label("Name");
    ImGui::SetNextItemWidth(-1);
    if (ImGui::InputText("##padname", &pad.name)) mc_->markDirty();

    ImGui::Dummy(ImVec2(0, 2.0f * sc));
    pop::Label("Key");
    keyCaptureButton("padkey", kPadCaptureBase + static_cast<int>(pad.id), &pad.key, 200.0f);
    if (!pad.key.empty()) {
        ImGui::SameLine(0, 8.0f * sc);
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 3.0f * sc);
        if (pop::SmallButton("Clear##padkey", pop::Btn::Quiet)) {
            pad.key = {};
            mc_->markDirty();
        }
    }
    const std::string other = otherUseOfKey(pad.key, &pad.key);
    if (!other.empty()) pop::Hint(("This key also " + other + ".").c_str());
    else pop::Hint("Optional. A key, mouse button (side buttons too) or controller button.");

    ImGui::Dummy(ImVec2(0, 2.0f * sc));
    pop::Label("When you press it");
    int mode = static_cast<int>(pad.mode);
    const char* modes[] = {"Play", "Play / stop", "Hold to play"};
    if (pop::Segmented("##padmode", &mode, modes, kPadModes, ImGui::GetContentRegionAvail().x)) {
        pad.mode = static_cast<PadMode>(mode);
        mc_->markDirty();
    }
    pop::Hint(pad.mode == PadMode::Restart ? "Plays the sound; pressing again starts it over."
              : pad.mode == PadMode::Toggle ? "Plays the sound; pressing again stops it."
                                            : "Plays while you hold the key or the pad, and stops when you let go.");

    ImGui::Dummy(ImVec2(0, 2.0f * sc));
    float gain = pad.gainDb;
    char v[24];
    std::snprintf(v, sizeof(v), "%+.0f dB", gain);
    if (pop::SliderRow("Volume", v, "##padgain", &gain, -24.0f, 6.0f, 0.0f)) {
        pad.gainDb = std::round(gain);
        mc_->markDirty();
    }

    pop::Divider();
    const bool playing = mc_->soundboard().progress(slot) >= 0.0f;
    if (pop::Button(playing ? ICON_STOP "  Stop" : ICON_PLAY "  Play")) {
        if (playing) mc_->soundboard().stop(slot);
        else mc_->soundboard().play(slot);
    }
    ImGui::SameLine(0, 8.0f * sc);
    if (pop::Button(ICON_DELETE "  Remove", pop::Btn::Danger)) {
        const uint32_t id = pad.id;
        ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
        removePad(id);
        return;
    }
    ImGui::SameLine(0, 8.0f * sc);
    if (pop::Button("Done", pop::Btn::Primary)) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

} // namespace echo::ui
