// Voices: every voice preset as a tile, fine-tuning of the chosen voice
// (pitch & formant or pitch correction), noise & echo cleanup, and the
// optional AI voice panel.
#include <windows.h>
#include <shellapi.h>

#include <algorithm>
#include <cstdio>
#include <filesystem>

#include "ai/AiVoiceEffect.h"
#include "ai/OrtRuntime.h"
#include "core/Text.h"
#include "dsp/MusicTheory.h"
#include "dsp/NoiseSuppressorInfo.h"
#include "imgui.h"
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
bool g_preferGpu = false;
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

const char* presetIcon(const std::string& name) {
    auto has = [&](const char* s) { return name.find(s) != std::string::npos; };
    if (name.empty()) return ICON_MIC;
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
    pop::PageTitle("Voices", "Pick a voice. The delay it adds is shown on each one.");
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
        for (size_t i = 0; i < list.size(); ++i) {
            if (i % cols) ImGui::SameLine(0, tgap);
            const ChainPreset* pr = list[i];
            const bool sel = pr ? (voiceFx && target->voice == pr->name) : !voiceFx;
            char delay[32];
            const double ms = pr ? presetDelayMs(*pr) : 0.0;
            if (ms < 0.5) std::snprintf(delay, sizeof(delay), pr ? "+0 ms" : "");
            else std::snprintf(delay, sizeof(delay), "+%.0f ms", ms);
            ImGui::PushID(static_cast<int>(i));
            const std::string name = pr ? pr->name : "Original voice";
            const std::string desc = pr ? pr->description : "Your voice, with its noise and echo cleanup";
            if (pop::Tile("##tile", presetIcon(pr ? pr->name : ""), name.c_str(), desc.c_str(), delay, sel, ImVec2(tw, 118.0f * sc)))
                applyVoice(*target, pr);
            Tip(desc.c_str());
            ImGui::PopID();
        }
        if (list.empty()) pop::Hint("No presets here yet. Save one from the effects editor.");

        ImGui::Dummy(ImVec2(0, 8.0f * sc));
        pop::BeginCard("##ai", 0.0f, 0.0f, true);
        pop::CardTitle("AI voice (optional)");
        pop::Caption("Runs on this PC. Needs ONNX Runtime and a voice model you add yourself; Echo doesn't include or "
                     "download models.");
        if (pop::SmallButton(g_aiOpen ? "Hide" : "Set up AI voice")) g_aiOpen = !g_aiOpen;
        if (g_aiOpen) {
            pop::Divider();
            drawAiPanel(*target);
        }
        pop::EndCard();
    }
    ImGui::EndChild();

    // ------------------------------------------------------------ right column
    if (wide) ImGui::SameLine(0, gap);
    ImGui::BeginChild("##voiceright", ImVec2(rightW, 0), ImGuiChildFlags_AutoResizeY, ImGuiWindowFlags_NoScrollbar);
    const ChainTarget t{false, target->id};
    const double sr = mc_->engine().sampleRate();
    pop::BeginCard("##tune");
    Effect* pitch = findEffect(*target, "pitch");
    Effect* tune = findEffect(*target, "autotune");
    const std::string label = voiceLabel(*target);
    if (pitch) {
        pop::CardTitle(("Pitch & formant · " + (target->voice.empty() ? std::string("Custom") : target->voice)).c_str());
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
        char ms[3][24];
        const char* subs[3];
        for (int q = 0; q < 3; ++q) {
            std::snprintf(ms[q], sizeof(ms[q]), "%.0f ms", ((512 << base) << q) * 1000.0 / sr);
            subs[q] = ms[q];
        }
        const char* qs[] = {"Low latency", "Balanced", "High quality"};
        int q = static_cast<int>(pitch->param(4));
        pop::Label("Quality");
        if (pop::Segmented("##quality", &q, qs, 3, ImGui::GetContentRegionAvail().x, subs)) pitch->setParam(4, static_cast<float>(q));
        pop::Hint("Delay of the pitch shifter. Larger windows sound smoother.");
    } else if (tune) {
        pop::CardTitle(("Pitch correction · " + (target->voice.empty() ? std::string("Custom") : target->voice)).c_str());
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
        char note[64];
        std::snprintf(note, sizeof(note), "Pitch correction adds %.0f ms.", tune->latencySamples() * 1000.0 / sr);
        pop::Hint(note);
    } else {
        pop::CardTitle(target->voice.empty() && label == "your own voice" ? "Original voice" : label.c_str());
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

    // Noise & echo.
    ImGui::Dummy(ImVec2(0, 4.0f * sc));
    pop::BeginCard("##noise", 0.0f, 0.0f, false, 20.0f, 8.0f);
    ImGui::Dummy(ImVec2(0, 4.0f * sc));
    pop::CardTitle("Noise & echo");
    Effect* ns = findEffect(*target, noiseSuppressorTypeId());
    Effect* aec = findEffect(*target, echoCancellerTypeId());
    {
        bool nsOn = ns && !ns->bypass.load();
        char desc[96];
        std::snprintf(desc, sizeof(desc), "Fans, keyboards, hum · adds %.0f ms", ns ? ns->latencySamples() * 1000.0 / sr : 20.0);
        if (pop::SwitchRow("Noise removal", desc, &nsOn, !(ns && nsOn))) {
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
            bool gate = ns->param(2) >= 0.5f;
            if (pop::SwitchRow("Voice gate", "Silences breathing and noise between sentences", &gate, !gate))
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
        char desc[96];
        std::snprintf(desc, sizeof(desc), "For speakers instead of headphones · adds %.0f ms",
                      aec ? aec->latencySamples() * 1000.0 / sr : 16.0);
        if (pop::SwitchRow("Speaker echo removal", desc, &aecOn, aecOn)) {
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
    auto& rt = ai::OrtRuntime::instance();
    static bool triedLoad = false;
    if (!triedLoad) {
        triedLoad = true;
        rt.ensureLoaded(); // only loads a DLL the user installed; never downloads
    }
    ImGui::TextWrapped("AI voice conversion runs a neural voice model on this computer. It is optional and "
                       "separate from the regular voice changer. Echo does not include or download any voice "
                       "models: install only models you have the rights to use, and never use a voice to "
                       "impersonate someone without their consent.");
    ImGui::Separator();
    // Runtime status.
    if (rt.loaded()) {
        ImGui::TextColored(toVec4(p.good), ICON_CHECK " ONNX Runtime %s", rt.version().c_str());
        ImGui::SameLine();
        std::string prov;
        for (const auto& s : rt.providers()) prov += s + " ";
        ImGui::TextDisabled("(%s)", prov.c_str());
    } else {
        ImGui::TextColored(toVec4(p.warn), ICON_WARNING " ONNX Runtime is not installed.");
        ImGui::TextWrapped("To enable AI voices, copy onnxruntime.dll (from the official Microsoft release, MIT "
                           "license) into the 'ai' folder next to Echo.exe, or run tools\\fetch_onnxruntime.ps1. "
                           "See docs\\AI_VOICE.md.");
        if (ImGui::Button(ICON_FOLDER " Open Echo's ai folder")) {
            const std::wstring dir = paths::executableDir() + L"\\ai";
            std::error_code ec;
            std::filesystem::create_directories(dir, ec);
            ShellExecuteW(nullptr, L"open", dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        }
        ImGui::SameLine();
        if (ImGui::Button(ICON_REFRESH " Check again")) {
            std::string err;
            if (rt.ensureLoaded(&err)) toast("ONNX Runtime loaded", 1);
            else toast(err, 2);
        }
    }
    ImGui::Separator();
    if (!g_modelsScanned) {
        g_models = ai::scanModels(paths::modelsDir());
        g_modelsScanned = true;
    }
    ImGui::Text("Installed voice models (%d)", static_cast<int>(g_models.size()));
    ImGui::SameLine();
    if (ImGui::SmallButton(ICON_REFRESH " Rescan")) g_modelsScanned = false;
    ImGui::SameLine();
    if (ImGui::SmallButton(ICON_FOLDER " Open models folder"))
        ShellExecuteW(nullptr, L"open", paths::modelsDir().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    Tip("Each model is a folder containing model.json and an ONNX file (see docs/AI_VOICE.md).");
    ImGui::Checkbox("Prefer GPU (DirectML) when available", &g_preferGpu);
    Tip("Requires a DirectML build of ONNX Runtime. Falls back to CPU automatically.");

    Effect* fx = nullptr;
    for (auto& e : strip.chain)
        if (std::string(e->typeId()) == "ai_voice") fx = e.get();
    auto* aiFx = dynamic_cast<ai::AiVoiceEffect*>(fx);

    if (g_models.empty()) {
        ImGui::TextDisabled("No models installed.");
    } else if (ImGui::BeginTable("##models", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH)) {
        ImGui::TableSetupColumn("Model", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("License / source", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Size", ImGuiTableColumnFlags_WidthFixed, 80 * dpi_);
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 110 * dpi_);
        ImGui::TableHeadersRow();
        for (const auto& m : g_models) {
            ImGui::PushID(m.id.c_str());
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(m.name.c_str());
            if (!m.valid) ImGui::TextColored(toVec4(p.bad), "%s", m.error.c_str());
            else ImGui::TextDisabled("%d Hz, %d ms chunks", m.sampleRate, m.chunkMs);
            if (!m.description.empty()) Tip(m.description.c_str());
            ImGui::TableNextColumn();
            ImGui::TextWrapped("%s", m.license.c_str());
            if (!m.source.empty()) ImGui::TextDisabled("%s", m.source.c_str());
            if (!m.consent.empty()) Tip(("Consent: " + m.consent).c_str());
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(formatBytes(m.sizeBytes).c_str());
            ImGui::TableNextColumn();
            ImGui::BeginDisabled(!m.valid || !rt.loaded());
            if (ImGui::Button("Use on input")) {
                if (!aiFx) {
                    auto added = mc_->addEffect(ChainTarget{false, strip.id}, "ai_voice");
                    aiFx = dynamic_cast<ai::AiVoiceEffect*>(added.get());
                }
                if (aiFx) {
                    aiFx->setModel(m, g_preferGpu);
                    toast("Loading AI model '" + m.name + "'", 0);
                }
            }
            ImGui::EndDisabled();
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    if (aiFx) {
        ImGui::SeparatorText("Active AI voice");
        const ai::AiStatus st = aiFx->status();
        const ImU32 col = st == ai::AiStatus::Running ? p.good : st == ai::AiStatus::Error ? p.bad : p.warn;
        ImGui::TextColored(toVec4(col), "%s", aiFx->statusText().c_str());
        if (st == ai::AiStatus::Running) {
            ImGui::Text("Inference %.1f ms per chunk  |  real-time factor %.2f  |  added latency %.0f ms",
                        aiFx->inferenceMs.load(), aiFx->realtimeFactor.load(),
                        aiFx->latencySamples() * 1000.0 / mc_->engine().sampleRate());
            const uint64_t under = aiFx->underrunSamples.load();
            if (under > 0)
                ImGui::TextColored(toVec4(p.warn), "Fallbacks to dry voice: %.2f s total (raise Buffer if this keeps growing)",
                                   under / static_cast<double>(mc_->engine().sampleRate()));
            if (aiFx->realtimeFactor.load() > 0.9f)
                ImGui::TextColored(toVec4(p.bad), ICON_WARNING " The model is too slow for real time on this hardware.");
        }
        for (int i = 0; i < aiFx->numParams(); ++i) {
            if (i) ImGui::SameLine();
            ParamControl(*aiFx, i);
        }
        bool byp = aiFx->bypass.load();
        if (ImGui::Checkbox("Bypass AI voice (hear unprocessed voice)", &byp)) aiFx->bypass = byp;
        if (ImGui::Button("Unload model")) aiFx->clearModel();
    }
}

} // namespace echo::ui
