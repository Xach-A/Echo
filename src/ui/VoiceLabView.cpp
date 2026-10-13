// Voices: every voice preset as a tile, fine-tuning of the chosen voice
// (pitch & formant or pitch correction), noise & echo cleanup, and the
// optional AI voice panel.
#include <windows.h>
#include <shellapi.h>

#include <algorithm>
#include <cmath>
#include <chrono>
#include <future>
#include <cstdio>
#include <filesystem>

#include "ai/AiVoiceEffect.h"
#include "ai/RvcVoice.h"
#include "audio/Download.h"
#include "audio/OrtInstall.h"
#include "ai/OrtRuntime.h"
#include "core/Text.h"
#include "dsp/MusicTheory.h"
#include "dsp/NoiseSuppressorInfo.h"
#include "imgui.h"
#include "imgui_internal.h"
#include "session/Paths.h"
#include "ui/App.h"
#include "ui/EffectVisuals.h"
#include "ui/Icons.h"
#include "ui/Pop.h"
#include "ui/Theme.h"
#include "ui/Widgets.h"

namespace echo::ui {

namespace {

std::vector<ai::ModelManifest> g_models;
bool g_modelsScanned = false;
bool g_aiOpen = false;

Effect* findEffect(StripModel& s, const char* type) {
    for (auto& e : s.chain)
        if (std::string(e->typeId()) == type) return e.get();
    return nullptr;
}

bool isCleanup(const Effect& e) {
    const std::string t = e.typeId();
    return t == noiseSuppressorTypeId() || t == echoCancellerTypeId();
}

// Index after the input's echo canceller / noise suppressor.
int afterCleanup(const StripModel& s) {
    int pos = 0;
    while (pos < static_cast<int>(s.chain.size()) && isCleanup(*s.chain[pos])) ++pos;
    return pos;
}

std::string signedText(float v, const char* unit) {
    char buf[48];
    if (std::fabs(v) < 0.05f) std::snprintf(buf, sizeof(buf), "0 %s", unit);
    else std::snprintf(buf, sizeof(buf), "%s%.1f %s", v > 0 ? "+" : "\xe2\x88\x92", std::fabs(v), unit);
    return buf;
}

const char* const kCategoryLabels[] = {"All", "Voice changer", "Pitch correction", "Clean voice", "Your presets"};
const char* const kCategoryNames[] = {"", "Voice changer", "Pitch correction", "Voice processing", ""};

bool inCategory(const ChainPreset& p, int cat) {
    if (p.category == "Music") return false; // bus presets, not voices
    if (cat == 0) return true;
    if (cat == 4) return !p.factory;
    return p.category == kCategoryNames[cat];
}

} // namespace

std::string bufferSizeName(int blockSize, int sampleRate) {
    const double t = blockSize / static_cast<double>(std::max(1, sampleRate));
    std::string name = t < 0.004 ? "Tiny" : t < 0.008 ? "Small" : t <= 0.0111 ? "Normal" : t <= 0.022 ? "Large" : "Extra large";
    if (blockSize == 480 && sampleRate == 48000) name += " (recommended)";
    else if (t < 0.004) name += " (needs a fast PC)";
    else if (t > 0.022) name += " (safest)";
    return name;
}

const char* presetIcon(const std::string& name) {
    auto has = [&](const char* s) { return name.find(s) != std::string::npos; };
    if (name.empty()) return ICON_MIC;
    if (name == "Pain") return ICON_WARNING;
    if (has("Robot") || has("Whisper") || has("Alien") || has("8-bit")) return ICON_ROBOT;
    if (has("Deep") || has("Monster") || has("Giant") || has("Masculine") || has("Octave")) return ICON_DOWN;
    if (has("Chipmunk") || has("Child") || has("Feminine")) return ICON_UP;
    if (has("Radio") || has("Telephone") || has("Walkie") || has("Megaphone") || has("Stadium") || has("DJ")) return ICON_SPEAKERS;
    if (has("Autotune") || has("Correction") || has("Harmony") || has("Doubled")) return ICON_TONE;
    if (has("Cave") || has("Cathedral") || has("Underwater")) return ICON_AUDIO;
    if (has("Gaming")) return ICON_HEADPHONES;
    return ICON_MIC;
}

void EchoApp::drawVoiceLab() {
    const Palette& p = pal();
    const float sc = uiScale();
    Session& s = mc_->session();
    if (!s.strip(voiceTarget_)) {
        voiceTarget_ = 0;
        if (StripModel* mic = primaryMic()) voiceTarget_ = mic->id;
        else if (!s.strips.empty()) voiceTarget_ = s.strips.front().id;
    }
    StripModel* target = s.strip(voiceTarget_);
    pop::PageTitle("Voices", "Pick a voice, or mix several.");
    if (!target) {
        pop::Caption("Add a microphone input to use the voice changer.");
        if (pop::Button("Choose a microphone", pop::Btn::Primary)) navigate(Page::Devices);
        return;
    }
    const float avail = ImGui::GetContentRegionAvail().x;
    const float gap = 20.0f * sc;

    // Category chips, and which input the voice applies to.
    bool anyUser = false;
    for (const auto& pr : presets_.all()) anyUser |= !pr.factory;
    for (int c = 0; c < (anyUser ? 5 : 4); ++c) {
        if (c) ImGui::SameLine(0, 6.0f * sc);
        if (pop::Chip(kCategoryLabels[c], voiceCategory_ == c)) voiceCategory_ = c;
    }
    {
        // One voice at a time, or several mixed.
        const float segW = 230.0f * sc;
        const float comboSpace = s.strips.size() > 1 ? 220.0f * sc + 104.0f * sc : 0.0f;
        ImGui::SameLine(std::max(ImGui::GetCursorPosX() + 12.0f * sc, ImGui::GetStyle().WindowPadding.x + avail - segW - comboSpace));
        int mix = voiceMix_ ? 1 : 0;
        const char* modes[] = {"One voice", "Mix voices"};
        if (pop::Segmented("##voicemix", &mix, modes, 2, segW)) voiceMix_ = mix == 1;
        Tip("Mix voices: click voices to add them to the mix or take them out, e.g. Deep Voice + Cave Echo.");
    }
    if (s.strips.size() > 1) {
        const float comboW = 220.0f * sc;
        ImGui::SameLine(std::max(ImGui::GetCursorPosX() + 12.0f * sc, ImGui::GetStyle().WindowPadding.x + avail - comboW - 92.0f * sc));
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 5.0f * sc);
        ImGui::TextColored(toVec4(p.text2), "Voice for");
        ImGui::SameLine();
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() - 4.0f * sc);
        ImGui::SetNextItemWidth(comboW);
        if (ImGui::BeginCombo("##voicetarget", target->name.c_str())) {
            for (auto& st : s.strips)
                if (ImGui::Selectable(st.name.c_str(), st.id == voiceTarget_)) voiceTarget_ = st.id;
            ImGui::EndCombo();
        }
    }
    ImGui::Dummy(ImVec2(0, 4.0f * sc));

    const bool wide = avail > 980.0f * sc;
    const float rightW = wide ? 400.0f * sc : avail;
    const float leftW = wide ? avail - rightW - gap : avail;

    // ------------------------------------------------------------------ tiles
    ImGui::BeginChild("##voiceleft", ImVec2(leftW, 0), ImGuiChildFlags_AutoResizeY, ImGuiWindowFlags_NoScrollbar);
    {
        bool voiceFx = false;
        for (const auto& e : target->chain) voiceFx |= !isCleanup(*e);
        std::vector<const ChainPreset*> list;
        for (const auto& pr : presets_.all())
            if (inCategory(pr, voiceCategory_)) list.push_back(&pr);
        // Voice changers first, then pitch correction, clean voice, your own.
        auto rank = [](const ChainPreset* pr) {
            if (!pr->factory) return 3;
            return pr->category == "Voice changer" ? 0 : pr->category == "Pitch correction" ? 1 : 2;
        };
        std::stable_sort(list.begin(), list.end(), [&](const ChainPreset* a, const ChainPreset* b) { return rank(a) < rank(b); });
        if (voiceCategory_ == 0) list.insert(list.begin(), nullptr);
        const float tgap = 12.0f * sc;
        const int cols = pop::columnsFor(leftW, 150.0f * sc, tgap);
        const float tw = (leftW - tgap * (cols - 1)) / cols;
        const bool isMainMic = primaryMic() == target;
        // Each row as tall as its tile with the most text, so no description
        // is cut off (at any width or interface size).
        std::vector<float> rowH((list.size() + cols - 1) / cols, 118.0f * sc);
        for (size_t i = 0; i < list.size(); ++i)
            rowH[i / cols] = std::max(rowH[i / cols], pop::TileHeight(ICON_ADD, list[i] ? list[i]->name.c_str() : "Original voice",
                                                                      list[i] ? list[i]->description.c_str()
                                                                              : "Your voice, with its noise and echo cleanup",
                                                                      tw));
        for (size_t i = 0; i < list.size(); ++i) {
            if (i % cols) ImGui::SameLine(0, tgap);
            const ChainPreset* pr = list[i];
            const bool on = pr && std::find(target->voices.begin(), target->voices.end(), pr->name) != target->voices.end();
            const bool sel = pr ? (voiceFx && on) : !voiceFx;
            // The key that switches exactly this voice, if any.
            std::string corner;
            if (pr && isMainMic)
                for (const auto& vk : s.hotkeys.voiceKeys)
                    if (!vk.key.empty() && vk.voices.size() == 1 && vk.voices[0] == pr->name) {
                        corner = keyBindingName(vk.key);
                        break;
                    }
            ImGui::PushID(static_cast<int>(i));
            const std::string name = pr ? pr->name : "Original voice";
            const std::string desc = pr ? pr->description : "Your voice, with its noise and echo cleanup";
            if (pop::Tile("##tile", voiceMix_ && pr ? (on ? ICON_CHECK : ICON_ADD) : presetIcon(pr ? pr->name : ""), name.c_str(), desc.c_str(),
                          corner.c_str(), sel, ImVec2(tw, rowH[i / cols]))) {
                if (voiceMix_ && pr) toggleVoice(*target, pr->name);
                else applyVoice(*target, pr);
            }
            Tip(voiceMix_ && pr ? (on ? "Click to take it out of the mix." : "Click to add it to the mix.") : desc.c_str());
            ImGui::PopID();
        }
        if (list.empty()) pop::Hint("No presets here yet. Save one from the effects editor.");

        ImGui::Dummy(ImVec2(0, 8.0f * sc));
        pop::BeginCard("##ai", 0.0f, 0.0f, true);
        pop::CardTitle("AI voice (optional)");
        pop::Caption("Use RVC voice models (from RVC or Applio) on this PC. Echo doesn't include voices: you add your own.");
        if (pop::SmallButton(g_aiOpen ? "Hide" : "Set up AI voice")) g_aiOpen = !g_aiOpen;
        if (snapshotShowAi_) g_aiOpen = true; // UI snapshot: the AI voice setup in view
        if (g_aiOpen) {
            pop::Divider();
            drawAiPanel(*target);
            if (snapshotShowAi_) {
                ImGui::Dummy(ImVec2(1, 1));
                ImGui::ScrollToItem(ImGuiScrollFlags_KeepVisibleEdgeY);
            }
        }
        pop::EndCard();
    }
    ImGui::EndChild();

    // ------------------------------------------------------------ right column
    if (wide) ImGui::SameLine(0, gap);
    ImGui::BeginChild("##voiceright", ImVec2(rightW, 0), ImGuiChildFlags_AutoResizeY, ImGuiWindowFlags_NoScrollbar);
    const ChainTarget t{false, target->id};
    const double sr = mc_->engine().sampleRate();
    // Quick controls the voice's preset gives its effects ("ui" key, e.g.
    // Pain): Intensity = a distortion stage's drive, Bit-crush = an optional
    // bit-crusher. The n-th effect of a type in the preset is the n-th of that
    // type in the chain (the noise cleanup kept in front has other types).
    Effect* clip = nullptr;
    Effect* crush = nullptr;
    const json* clipFx = nullptr; // its entry in the preset
    float clipDefault = 0.0f;
    const std::string mixLabel = voicesLabel(target->voices);
    for (const std::string& voiceName : target->voices) {
        const ChainPreset* pr = presets_.find(voiceName);
        if (!pr || clip || crush) continue;
        // The n-th effect of a type among this voice's own effects.
        auto nth = [&](const std::string& type, int n) -> Effect* {
            for (auto& e : target->chain)
                if (e->voice == voiceName && type == e->typeId() && n-- == 0) return e.get();
            return nullptr;
        };
        for (size_t i = 0; i < pr->chain.size(); ++i) {
            const json& fx = pr->chain[i];
            const std::string type = fx.value("type", ""), ui = fx.value("ui", "");
            int n = 0;
            for (size_t k = 0; k < i; ++k) n += pr->chain[k].value("type", "") == type;
            if (ui == "intensity" && type == "distortion") {
                clip = nth(type, n);
                clipFx = &fx;
                clipDefault = fx.contains("params") ? fx["params"].value("drive", 18.0f) : 18.0f;
            } else if (ui == "optional" && type == "bitcrusher") {
                crush = nth(type, n);
            }
        }
    }
    Effect* limiter = nullptr; // the limiter after the Intensity stage
    for (size_t i = 0, seen = 0; clip && i < target->chain.size(); ++i) {
        if (target->chain[i].get() == clip) seen = 1;
        else if (seen && std::string(target->chain[i]->typeId()) == "limiter" && !limiter) limiter = target->chain[i].get();
    }
    auto distortionControls = [&]() {
        if (clip) {
            float drive = clip->param(0);
            char v[24];
            std::snprintf(v, sizeof(v), "+%.0f dB", drive);
            // Double-click resets to the voice's own setting.
            if (pop::SliderRow("Intensity", v, "##intensity", &drive, 0.0f, 48.0f, clipDefault))
                applyIntensity(*clip, *clipFx, std::round(drive));
            const bool even = clipFx->value("ui_makeup", 0.0f) > 0.0f;
            const int ceil = limiter ? limiter->paramIndex("ceiling") : -1;
            char hint[200];
            if (limiter && !limiter->bypass.load() && ceil >= 0)
                std::snprintf(hint, sizeof(hint), "How much the voice is distorted%s. Peaks stay under %.1f dBFS at any setting.",
                              even ? "; it stays about as loud" : "", limiter->param(ceil));
            else if (limiter)
                std::snprintf(hint, sizeof(hint), "How much the voice is distorted. Its limiter is off, so higher settings can be louder.");
            else
                std::snprintf(hint, sizeof(hint), "How much the voice is distorted.");
            pop::Hint(hint);
        }
        if (crush) {
            bool on = !crush->bypass.load();
            if (pop::SwitchRow("Bit-crush", "Harsher, digital crunch on top", &on, false)) crush->bypass = !on;
        }
    };
    const bool hasDistortion = clip || crush;

    pop::BeginCard("##tune");
    Effect* pitch = findEffect(*target, "pitch");
    Effect* tune = findEffect(*target, "autotune");
    const std::string label = voiceLabel(*target);
    if (pitch) {
        pop::CardTitle(("Pitch & formant · " + (mixLabel.empty() ? std::string("Custom") : mixLabel)).c_str());
        float st = pitch->param(0);
        if (pop::SliderRow("Pitch", signedText(st, "semitones").c_str(), "##pitch", &st, -24.0f, 24.0f, 0.0f))
            pitch->setParam(0, std::round(st * 2.0f) / 2.0f);
        float fm = pitch->param(2);
        if (pop::SliderRow("Formant (voice size)", signedText(fm, "semitones").c_str(), "##formant", &fm, -12.0f, 12.0f, 0.0f))
            pitch->setParam(2, std::round(fm * 2.0f) / 2.0f);
        bool preserve = pitch->param(3) >= 0.5f;
        if (pop::SwitchRow("Preserve formants", "Off gives the chipmunk / giant sound", &preserve, false))
            pitch->setParam(3, preserve ? 1.0f : 0.0f);
        // Quality: the phase vocoder's window, so its delay, at this rate.
        const int base = sr > 70000 ? 2 : sr > 35000 ? 1 : 0;
        (void)base;
        const char* qs[] = {"Fastest", "Balanced", "Smoothest"};
        int q = static_cast<int>(pitch->param(4));
        pop::Label("Quality");
        if (pop::Segmented("##quality", &q, qs, 3, ImGui::GetContentRegionAvail().x)) pitch->setParam(4, static_cast<float>(q));
        pop::Hint("Smoother sounds better but reaches the listener a little later.");
    } else if (tune) {
        pop::CardTitle(("Pitch correction · " + (mixLabel.empty() ? std::string("Custom") : mixLabel)).c_str());
        const float half = (ImGui::GetContentRegionAvail().x - 10.0f * sc) * 0.5f;
        int key = static_cast<int>(tune->param(0));
        const float rowY = ImGui::GetCursorPosY();
        ImGui::BeginGroup();
        pop::Label("Key");
        ImGui::SetNextItemWidth(half * 0.6f);
        if (ImGui::Combo("##key", &key, music::kNoteNames, 12)) tune->setParam(0, static_cast<float>(key));
        ImGui::EndGroup();
        ImGui::SameLine();
        ImGui::SetCursorPosY(rowY);
        ImGui::BeginGroup();
        pop::Label("Scale");
        int scale = static_cast<int>(tune->param(1));
        ImGui::SetNextItemWidth(half * 1.4f);
        if (ImGui::Combo("##scale", &scale, music::kScaleNames, music::NumScales)) tune->setParam(1, static_cast<float>(scale));
        ImGui::EndGroup();
        int style = static_cast<int>(tune->param(2));
        const char* styles[] = {"Natural", "Hard", "Custom"};
        pop::Label("Style");
        if (pop::Segmented("##style", &style, styles, 3, ImGui::GetContentRegionAvail().x)) tune->setParam(2, static_cast<float>(style));
        if (style == 2) {
            ParamControl(*tune, 3);
            ImGui::SameLine();
            ParamControl(*tune, 4);
            ImGui::SameLine();
            ParamControl(*tune, 5);
        }
        drawPitchGraph(*tune, ImVec2(ImGui::GetContentRegionAvail().x, 120.0f * sc));
        pop::Hint("Pitch correction reaches the listener a little later than your plain voice.");
    } else if (hasDistortion) {
        pop::CardTitle(("Distortion · " + mixLabel).c_str());
        distortionControls();
    } else {
        pop::CardTitle(target->voices.empty() && label == "your own voice" ? "Original voice" : label.c_str());
        pop::Caption(label == "your own voice" ? "Your own voice. Noise removal and speaker echo removal stay on."
                                               : "This voice has no pitch control of its own. You can add one.");
        if (pop::Button("Add pitch & formant shifter")) {
            const int pos = afterCleanup(*target);
            defer([this, t, pos]() { mc_->addEffect(t, "pitch", pos); });
        }
    }
    ImGui::Dummy(ImVec2(0, 4.0f * sc));
    {
        // Hold to compare with the unprocessed voice (whole chain bypassed).
        static bool holding = false, saved = false;
        const float bw = ImGui::GetContentRegionAvail().x;
        if (pop::HoldButton("Hold to hear your original voice", "Hearing your original voice", ImVec2(bw, 42.0f * sc))) {
            if (!holding) {
                saved = target->rt->fxBypass.load();
                target->rt->fxBypass = true;
                holding = true;
            }
        } else if (holding) {
            target->rt->fxBypass = saved;
            holding = false;
        }
    }
    bool on = !target->rt->fxBypass.load();
    if (pop::SwitchRow("Voice effects", "Every effect on this input on or off (Ctrl+B)", &on, false)) target->rt->fxBypass = !on;
    if (pop::Button("Edit all effects", pop::Btn::Secondary, ImVec2(-1, 0))) openEffects(t);
    pop::EndCard();

    if (hasDistortion && (pitch || tune)) {
        ImGui::Dummy(ImVec2(0, 4.0f * sc));
        pop::BeginCard("##distortion");
        pop::CardTitle("Distortion");
        distortionControls();
        pop::EndCard();
    }

    // Hotkeys for these voices (they act on the main microphone).
    if (target == primaryMic()) {
        ImGui::Dummy(ImVec2(0, 4.0f * sc));
        pop::BeginCard("##vkeys");
        pop::CardTitle("Hotkeys");
        HotkeySettings& h = s.hotkeys;
        int shown = 0;
        for (size_t i = 0; i < h.voiceKeys.size(); ++i) {
            VoiceHotkey& vk = h.voiceKeys[i];
            const bool related = std::any_of(vk.voices.begin(), vk.voices.end(), [&](const std::string& n) {
                return std::find(target->voices.begin(), target->voices.end(), n) != target->voices.end();
            });
            if (!related && !vk.key.empty()) continue;
            ImGui::PushID(static_cast<int>(i));
            keyCaptureButton("vk", 100 + static_cast<int>(i), &vk.key, 140.0f);
            ImGui::SameLine(0, 10.0f * sc);
            ImGui::BeginGroup();
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 2.0f * sc);
            pop::Label((std::string(voiceKeyModeName(vk.mode)) + " · " + (vk.voices.empty() ? std::string("no voices") : voicesLabel(vk.voices))).c_str());
            pop::Hint(vk.mode == VoiceKeyMode::Hold ? "On while you hold the key" : vk.mode == VoiceKeyMode::Switch ? "Only these voices; again for your own" : "Press to add, again to take out");
            ImGui::EndGroup();
            ImGui::PopID();
            ++shown;
        }
        if (!shown) pop::Caption(target->voices.empty() ? "Pick a voice, then give it a key to switch it on and off from a game."
                                                        : "Give these voices a key to switch them on and off from a game.");
        if (!target->voices.empty()) {
            const std::string add = "Add a key for " + mixLabel;
            if (pop::Button(add.c_str(), pop::Btn::Secondary, ImVec2(-1, 0))) {
                h.voiceKeys.push_back(VoiceHotkey{{}, VoiceKeyMode::Toggle, target->voices});
                autoCapture_ = 100 + static_cast<int>(h.voiceKeys.size() - 1);
                mc_->markDirty();
            }
        }
        if (pop::LinkButton("All hotkeys")) openSettings(SettingsTab::Hotkeys);
        pop::EndCard();
    }

    // Noise & echo.
    ImGui::Dummy(ImVec2(0, 4.0f * sc));
    pop::BeginCard("##noise", 0.0f, 0.0f, false, 20.0f, 8.0f);
    ImGui::Dummy(ImVec2(0, 4.0f * sc));
    pop::CardTitle("Noise & echo");
    Effect* ns = findEffect(*target, noiseSuppressorTypeId());
    Effect* aec = findEffect(*target, echoCancellerTypeId());
    {
        bool nsOn = ns && !ns->bypass.load();
        if (pop::SwitchRow("Noise removal", "Fans, typing, mouse clicks, hum", &nsOn, !(ns && nsOn))) {
            if (ns) ns->bypass = !nsOn;
            else if (nsOn) {
                const int pos = aec ? 1 : 0;
                defer([this, t, pos]() { mc_->addEffect(t, noiseSuppressorTypeId(), pos); });
            }
        }
        if (ns && nsOn) {
            float strength = ns->param(0);
            char v[16];
            std::snprintf(v, sizeof(v), "%.0f%%", strength);
            if (pop::SliderRow("Strength", v, "##strength", &strength, 0.0f, 100.0f, 100.0f)) ns->setParam(0, strength);
            drawCleanupStatus(*ns);
            const int clicksIdx = ns->paramIndex("clicks");
            bool clicks = clicksIdx >= 0 && ns->param(clicksIdx) >= 0.5f;
            if (clicksIdx >= 0 && pop::SwitchRow("Typing & click removal", "Keyboard and mouse sounds while you're not talking", &clicks))
                ns->setParam(clicksIdx, clicks ? 1.0f : 0.0f);
            bool gate = ns->param(2) >= 0.5f;
            if (pop::SwitchRow("Voice gate", "Complete silence while you're not talking", &gate, !gate))
                ns->setParam(2, gate ? 1.0f : 0.0f);
            if (gate) {
                float thr = ns->param(3);
                std::snprintf(v, sizeof(v), "%.0f%%", thr);
                if (pop::SliderRow("Threshold", v, "##thr", &thr, 5.0f, 95.0f, 60.0f)) ns->setParam(3, thr);
                pop::Divider(4.0f, 0.0f);
            }
        }
    }
    {
        bool aecOn = aec && !aec->bypass.load();
        if (pop::SwitchRow("Speaker echo removal", "For speakers instead of headphones", &aecOn, aecOn)) {
            if (aec) aec->bypass = !aecOn;
            else if (aecOn) {
                const uint32_t id = target->id;
                defer([this, t, id]() {
                    if (StripModel* st = mc_->session().strip(id)) {
                        if (st->reference.kind != SourceKind::RenderLoopback) {
                            st->reference = SourceRef{};
                            st->reference.kind = SourceKind::RenderLoopback;
                        }
                        mc_->addEffect(t, echoCancellerTypeId(), 0);
                    }
                });
            }
        }
        if (aec && aecOn) {
            pop::Label("Speakers to cancel");
            echoReferencePicker(*target, ImGui::GetContentRegionAvail().x);
            drawCleanupStatus(*aec);
        }
    }
    ImGui::Dummy(ImVec2(0, 4.0f * sc));
    if (ImGui::TreeNode("More noise & echo settings")) {
        drawNoiseEchoPanel(*target);
        ImGui::TreePop();
    }
    ImGui::Dummy(ImVec2(0, 4.0f * sc));
    pop::EndCard();
    ImGui::EndChild();
}

void EchoApp::drawAiPanel(StripModel& strip) {
    const Palette& p = pal();
    const float sc = pop::S();
    auto& rt = ai::OrtRuntime::instance();
    static bool triedLoad = false;
    if (!triedLoad) {
        triedLoad = true;
        rt.ensureLoaded(); // only loads a DLL the user installed
    }
    if (!g_modelsScanned) {
        g_models = ai::scanModels(paths::modelsDir());
        g_modelsScanned = true;
    }
    std::error_code ec;
    const bool haveEncoder = std::filesystem::exists(ai::contentVecPath(), ec);

    // How to add a voice.
    pop::Label("Add a voice");
    pop::Caption("1. Open the voices folder.\n"
                 "2. Put the voice in it: the folder you downloaded, as it is, or its .pth file (and .index, if it has one).\n"
                 "3. Click Rescan, then Use on mic.");
    if (pop::SmallButton(ICON_FOLDER " Open the voices folder")) {
        std::filesystem::create_directories(paths::modelsDir(), ec);
        ShellExecuteW(nullptr, L"open", paths::modelsDir().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }
    ImGui::SameLine();
    if (pop::SmallButton(ICON_REFRESH " Rescan")) g_modelsScanned = false;
    pop::Hint("Voices made with RVC or Applio work. Use only voices you have the right to use, and never to pretend to be "
              "someone.");

    // What AI voices need.
    pop::Divider();
    pop::Label("Needed once");
    const bool ortInstalling = ortInstall_.valid() && ortInstall_.wait_for(std::chrono::seconds(0)) != std::future_status::ready;
    if (ortInstall_.valid() && !ortInstalling) {
        ortError_ = ortInstall_.get();
        if (ortError_.empty()) {
            std::string err;
            if (rt.ensureLoaded(&err)) toast("The AI engine is ready", 1);
            else ortError_ = err;
        }
    }
    if (rt.loaded() && !snapshotNoOrt_) {
        ImGui::TextColored(toVec4(p.good), ICON_CHECK " AI engine (ONNX Runtime %s)%s", rt.version().c_str(),
                           rt.hasGpu() ? ", can use the graphics card" : "");
    } else if (ortInstalling) {
        const double done = ortProgress_.done.load() / 1048576.0, total = win::kOrtBytes / 1048576.0;
        char label[64];
        std::snprintf(label, sizeof(label), "Downloading the AI engine: %.0f of %.0f MB", done, total);
        ImGui::ProgressBar(static_cast<float>(done / total), ImVec2(std::min(ImGui::GetContentRegionAvail().x, 420.0f * sc), 0), label);
        ImGui::SameLine();
        if (pop::SmallButton("Cancel##ort")) ortProgress_.cancel = true;
    } else {
        ImGui::TextColored(toVec4(p.warn), ICON_WARNING " AI voices need Echo's AI engine.");
        pop::Caption("ONNX Runtime, Microsoft's free AI engine (12 MB from nuget.org, MIT license). It uses your graphics card "
                     "when it can. Echo checks the file before keeping it.");
        if (!ortError_.empty()) pop::TextColoredWrapped(p.bad, ortError_.c_str());
        if (pop::SmallButton(ICON_DOWNLOAD " Install the AI engine")) {
            ortError_.clear();
            ortProgress_.cancel = false;
            ortProgress_.done = 0;
            ortInstall_ = std::async(std::launch::async, [this]() {
                std::string err;
                std::error_code e2;
                const std::wstring dir = paths::appDataDir() + L"\\ai";
                std::filesystem::create_directories(dir, e2);
                win::installOnnxRuntime(dir, ortProgress_, &err);
                return err;
            });
        }
        pop::Hint("Or run tools\\fetch_onnxruntime.ps1 from Echo's folder.");
    }
    const bool downloading = contentVecDownload_.valid() &&
                             contentVecDownload_.wait_for(std::chrono::seconds(0)) != std::future_status::ready;
    if (contentVecDownload_.valid() && !downloading) {
        contentVecError_ = contentVecDownload_.get();
        if (contentVecError_.empty()) toast("ContentVec is ready", 1);
    }
    if (haveEncoder) {
        ImGui::TextColored(toVec4(p.good), ICON_CHECK " ContentVec speech model");
    } else if (downloading) {
        const double done = contentVecProgress_.done.load() / 1048576.0, total = ai::kContentVecBytes / 1048576.0;
        char label[64];
        std::snprintf(label, sizeof(label), "Downloading ContentVec: %.0f of %.0f MB", done, total);
        ImGui::ProgressBar(static_cast<float>(done / total), ImVec2(std::min(ImGui::GetContentRegionAvail().x, 420.0f * sc), 0), label);
        ImGui::SameLine();
        if (pop::SmallButton("Cancel")) contentVecProgress_.cancel = true;
    } else {
        ImGui::TextColored(toVec4(p.warn), ICON_WARNING " RVC voices need ContentVec.");
        pop::Caption("A speech model every RVC voice uses (361 MB, MIT license, from the Applio project on Hugging Face). Echo "
                     "checks the file before keeping it.");
        if (!contentVecError_.empty()) ImGui::TextColored(toVec4(p.bad), "%s", contentVecError_.c_str());
        if (pop::SmallButton("Download ContentVec")) {
            contentVecError_.clear();
            contentVecProgress_.cancel = false;
            contentVecProgress_.done = 0;
            contentVecDownload_ = std::async(std::launch::async, [this]() {
                std::string err;
                win::downloadFile(ai::kContentVecUrl, ai::contentVecPath(), ai::kContentVecBytes, ai::kContentVecSha256,
                                  contentVecProgress_, &err);
                return err;
            });
        }
    }

    // The voices.
    pop::Divider();
    Effect* fx = nullptr;
    for (auto& e : strip.chain)
        if (std::string(e->typeId()) == "ai_voice") fx = e.get();
    auto* aiFx = dynamic_cast<ai::AiVoiceEffect*>(fx);
    pop::Label(("Your voices (" + std::to_string(g_models.size()) + ")").c_str());
    if (g_models.empty()) pop::Hint("None yet. Follow the steps above, then click Rescan.");
    for (const auto& m : g_models) {
        ImGui::PushID(m.id.c_str());
        const float rowW = ImGui::GetContentRegionAvail().x;
        ImGui::BeginGroup();
        ImGui::PushFont(fonts().semibold, 0.0f);
        ImGui::TextUnformatted(m.name.c_str());
        ImGui::PopFont();
        if (!m.valid) pop::TextColoredWrapped(p.bad, m.error.c_str());
        else pop::Hint((m.description.empty() ? std::to_string(m.sampleRate / 1000) + " kHz" : m.description).c_str());
        ImGui::EndGroup();
        if (!m.license.empty()) Tip(("License: " + m.license + (m.consent.empty() ? "" : "\nConsent: " + m.consent)).c_str());
        ImGui::SameLine(std::max(ImGui::GetCursorPosX(), rowW - 120.0f * sc));
        const bool needsEncoder = m.kind == "rvc" && !haveEncoder;
        ImGui::BeginDisabled(!m.valid || !rt.loaded() || needsEncoder);
        if (pop::SmallButton("Use on mic")) {
            if (!aiFx) {
                auto added = mc_->addEffect(ChainTarget{false, strip.id}, "ai_voice");
                aiFx = dynamic_cast<ai::AiVoiceEffect*>(added.get());
            }
            if (aiFx) {
                // A new AI voice runs on the graphics card when there is a real one.
                aiFx->setModel(m, fx ? aiFx->prefersGpu() : rt.gpuRecommended());
                toast("Loading '" + m.name + "'", 0);
            }
        }
        ImGui::EndDisabled();
        if (needsEncoder && m.valid) Tip("Download ContentVec first (above).");
        ImGui::PopID();
    }

    // The voice in use.
    if (!aiFx) return;
    pop::Divider();
    pop::Label(("AI voice on " + strip.name).c_str());
    const ai::AiStatus st = aiFx->status();
    const ImU32 col = st == ai::AiStatus::Running ? p.good : st == ai::AiStatus::Error ? p.bad : p.warn;
    pop::TextColoredWrapped(col, aiFx->statusText().c_str());
    if (st == ai::AiStatus::Running) {
        const float rtf = aiFx->realtimeFactor.load();
        if (rtf > 0.9f)
            pop::TextColoredWrapped(p.bad, ICON_WARNING " This voice is too heavy for this PC: parts of your plain voice will come "
                                           "through.");
        else if (aiFx->underrunSamples.load() > 0)
            pop::Hint("Now and then your plain voice comes through while the PC is busy.");
    }
    const float w = std::min(ImGui::GetContentRegionAvail().x, 520.0f * sc);
    float pitch = aiFx->param(0);
    char v[32];
    std::snprintf(v, sizeof(v), "%+.0f semitones", pitch);
    if (pop::SliderRow("Pitch", v, "##aipitch", &pitch, -24.0f, 24.0f, 0.0f, w)) aiFx->setParam(0, std::round(pitch));
    pop::Hint("Moves your voice up or down before the AI voice takes over. +12 suits a deep voice using a high voice.");
    const int ix = aiFx->paramIndex("index");
    if (ix >= 0) {
        float match = aiFx->param(ix);
        std::snprintf(v, sizeof(v), "%.0f%%", match);
        if (pop::SliderRow("Voice match", v, "##aiindex", &match, 0.0f, 100.0f, 50.0f, w)) aiFx->setParam(ix, match);
        pop::Hint("For voices with an .index file: higher sounds more like the voice, lower keeps your words clearer.");
    }
    float mix = aiFx->param(1);
    std::snprintf(v, sizeof(v), "%.0f%%", mix);
    if (pop::SliderRow("Mix", v, "##aimix", &mix, 0.0f, 100.0f, 100.0f, w)) aiFx->setParam(1, mix);
    const int rx = aiFx->paramIndex("response");
    if (rx >= 0 && aiFx->modelKind() == "rvc") {
        const char* rs[] = {"Quicker", "Balanced", "Smoother"};
        int r = static_cast<int>(std::lround(aiFx->param(rx)));
        pop::Label("Response");
        if (pop::Segmented("##airesponse", &r, rs, 3, w)) aiFx->setParam(rx, static_cast<float>(r));
        pop::Hint("Quicker: you're heard sooner, but the voice can sound rougher. Smoother: cleaner, but later.");
    }
    if (rt.hasGpu()) {
        bool gpu = aiFx->prefersGpu();
        if (pop::SwitchRow("Use the graphics card", "Faster, so you're heard sooner", &gpu, false)) aiFx->setPreferGpu(gpu);
    }
    if (pop::SmallButton("Remove the AI voice")) {
        const uint32_t id = fx->instanceId;
        defer([this, t = ChainTarget{false, strip.id}, id]() { mc_->removeEffect(t, id); });
    }
}

} // namespace echo::ui
