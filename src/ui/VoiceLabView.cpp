// Voice Lab: one-click voice characters, live pitch/formant controls, pitch
// correction and the optional AI voice conversion panel.
#include <windows.h>
#include <shellapi.h>

#include <algorithm>
#include <filesystem>

#include "ai/AiVoiceEffect.h"
#include "ai/OrtRuntime.h"
#include "core/Text.h"
#include "dsp/MusicTheory.h"
#include "imgui.h"
#include "session/Paths.h"
#include "ui/App.h"
#include "ui/EffectVisuals.h"
#include "ui/Icons.h"
#include "ui/Theme.h"
#include "ui/Widgets.h"

namespace echo::ui {

namespace {

uint32_t g_voiceTarget = 0;
std::vector<ai::ModelManifest> g_models;
bool g_modelsScanned = false;
bool g_preferGpu = false;

Effect* findEffect(StripModel& s, const char* type) {
    for (auto& e : s.chain)
        if (std::string(e->typeId()) == type) return e.get();
    return nullptr;
}

const char* presetIcon(const std::string& name) {
    if (name.find("Robot") != std::string::npos) return ICON_ROBOT;
    if (name.find("Telephone") != std::string::npos || name.find("Radio") != std::string::npos) return ICON_SPEAKERS;
    if (name.find("Autotune") != std::string::npos || name.find("Correction") != std::string::npos) return ICON_TONE;
    return ICON_MIC;
}

} // namespace

void EchoApp::drawVoiceLab() {
    if (!ImGui::Begin("Voice Lab")) {
        ImGui::End();
        return;
    }
    const Palette& p = pal();
    Session& s = mc_->session();
    // Target strip (default: first hardware input).
    if (!s.strip(g_voiceTarget)) {
        g_voiceTarget = 0;
        for (auto& st : s.strips)
            if (st.group == StripGroup::Hardware) {
                g_voiceTarget = st.id;
                break;
            }
        if (!g_voiceTarget && !s.strips.empty()) g_voiceTarget = s.strips.front().id;
    }
    StripModel* target = s.strip(g_voiceTarget);
    if (!target) {
        ImGui::TextWrapped("Add a microphone input in the Mixer to use the voice changer.");
        ImGui::End();
        return;
    }
    ImGui::TextDisabled("Voice input:");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(220 * dpi_);
    if (ImGui::BeginCombo("##voicetarget", target->name.c_str())) {
        for (auto& st : s.strips)
            if (ImGui::Selectable(st.name.c_str(), st.id == g_voiceTarget)) g_voiceTarget = st.id;
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    bool on = !target->rt->fxBypass.load();
    if (ToggleButton(on ? ICON_POWER " Voice effects ON" : ICON_POWER " Voice effects OFF", &on, p.good))
        target->rt->fxBypass = !on;
    Tip("Turns the whole effect chain of this input on or off (A/B compare). Shortcut: Ctrl+B with the strip selected.");
    ImGui::SameLine();
    ImGui::Button("Hold to hear original");
    {
        static bool holding = false, saved = false;
        if (ImGui::IsItemActivated()) {
            saved = target->rt->fxBypass.load();
            target->rt->fxBypass = true;
            holding = true;
        }
        if (holding && ImGui::IsItemDeactivated()) {
            target->rt->fxBypass = saved;
            holding = false;
        }
    }
    Tip("Press and hold to compare with your unprocessed voice.");
    ImGui::SameLine();
    if (ImGui::Button(ICON_EQUALIZER " Edit chain")) {
        select(ChainTarget{false, target->id});
        focusWindow("Effects");
    }
    double lat = 0.0;
    for (auto& e : target->chain)
        if (!e->bypass.load()) lat += e->latencySamples() * 1000.0 / mc_->engine().sampleRate();
    if (lat > 0.5) {
        ImGui::SameLine();
        ImGui::TextDisabled("Effect latency %.0f ms", lat);
    }

    if (ImGui::BeginTabBar("##voicetabs")) {
        if (ImGui::BeginTabItem("Characters")) {
            ImGui::TextDisabled("Click a voice to replace this input's effect chain. Edit details in the Effects panel.");
            const float cardW = 150 * dpi_, cardH = 54 * dpi_;
            const int perRow = std::max(1, static_cast<int>(ImGui::GetContentRegionAvail().x / (cardW + 8 * dpi_)));
            auto cards = [&](const char* category) {
                int col = 0;
                for (const auto& pr : presets_.all()) {
                    if (pr.category != category) continue;
                    if (col > 0) ImGui::SameLine();
                    ImGui::PushID(pr.name.c_str());
                    const std::string label = std::string(presetIcon(pr.name)) + "  " + pr.name;
                    if (ImGui::Button(label.c_str(), ImVec2(cardW, cardH))) applyChainPreset(ChainTarget{false, target->id}, pr);
                    Tip(pr.description.c_str());
                    ImGui::PopID();
                    if (++col == perRow) col = 0;
                }
            };
            ImGui::SeparatorText("Voice changer");
            if (ImGui::Button(ICON_CLOSE "  Original voice", ImVec2(cardW * 1.6f, cardH * 0.7f)))
                mc_->applyPresetChain(ChainTarget{false, target->id}, {});
            Tip("Removes the voice effects. Noise suppression and echo cancellation stay on (switch them in the "
                "'Noise & echo' tab).");
            cards("Voice changer");
            ImGui::SeparatorText("Pitch correction");
            cards("Pitch correction");
            ImGui::SeparatorText("Clean voice processing");
            cards("Voice processing");
            ImGui::EndTabItem();
        }
        const ImGuiTabItemFlags nsFlags = voiceTabRequest_ == "Noise & echo" ? ImGuiTabItemFlags_SetSelected : 0;
        voiceTabRequest_.clear();
        if (ImGui::BeginTabItem("Noise & echo", nullptr, nsFlags)) {
            drawNoiseEchoPanel(*target);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Pitch & formant")) {
            Effect* fx = findEffect(*target, "pitch");
            if (!fx) {
                ImGui::TextWrapped("This input has no pitch shifter. Add one to change pitch and formants independently.");
                if (ImGui::Button(ICON_ADD " Add pitch & formant shifter")) mc_->addEffect(ChainTarget{false, target->id}, "pitch", 0);
            } else {
                const float w = std::min(520.0f * dpi_, ImGui::GetContentRegionAvail().x);
                ImGui::SetNextItemWidth(w);
                float semis = fx->param(0);
                if (ImGui::SliderFloat("Pitch (semitones)", &semis, -24.0f, 24.0f, "%+.1f st")) fx->setParam(0, semis);
                ImGui::SetNextItemWidth(w);
                float form = fx->param(2);
                if (ImGui::SliderFloat("Formant (semitones)", &form, -12.0f, 12.0f, "%+.1f st")) fx->setParam(2, form);
                Tip("Formants are the resonances of the vocal tract. Lower = larger/deeper-sounding speaker, higher = smaller.");
                bool preserve = fx->param(3) >= 0.5f;
                if (ImGui::Checkbox("Preserve formants when changing pitch", &preserve)) fx->setParam(3, preserve ? 1.0f : 0.0f);
                int q = static_cast<int>(fx->param(4));
                const char* qs[] = {"Low latency", "Balanced", "High quality"};
                ImGui::SetNextItemWidth(w * 0.5f);
                if (ImGui::Combo("Quality", &q, qs, 3)) fx->setParam(4, static_cast<float>(q));
                ImGui::TextDisabled("Current pitch-shifter latency: %.0f ms (window %s)",
                                    fx->latencySamples() * 1000.0 / mc_->engine().sampleRate(), qs[q]);
                ImGui::Spacing();
                ImGui::TextDisabled("Quick:");
                const struct { const char* n; float st, fm; } quick[] = {
                    {"Deep", -5, -2.5f}, {"Deeper", -9, -5}, {"Higher", 4, 2.5f}, {"Child-like", 6, 4.5f}, {"Reset", 0, 0}};
                for (const auto& qk : quick) {
                    ImGui::SameLine();
                    if (ImGui::Button(qk.n)) {
                        fx->setParam(0, qk.st);
                        fx->setParam(2, qk.fm);
                        fx->setParam(3, 1.0f);
                    }
                }
                ImGui::TextWrapped("Pitch alone rarely changes how old or what gender a voice sounds; formants, "
                                   "resonance, intonation and speaking style matter at least as much.");
            }
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Pitch correction")) {
            Effect* fx = findEffect(*target, "autotune");
            if (!fx) {
                ImGui::TextWrapped("Add pitch correction to snap your voice toward the notes of a key and scale.");
                if (ImGui::Button(ICON_ADD " Add pitch correction")) mc_->addEffect(ChainTarget{false, target->id}, "autotune");
            } else {
                const float w = ImGui::GetContentRegionAvail().x;
                ImGui::SetNextItemWidth(120 * dpi_);
                int key = static_cast<int>(fx->param(0));
                if (ImGui::Combo("Key", &key, music::kNoteNames, 12)) fx->setParam(0, static_cast<float>(key));
                ImGui::SameLine();
                ImGui::SetNextItemWidth(180 * dpi_);
                int scale = static_cast<int>(fx->param(1));
                if (ImGui::Combo("Scale", &scale, music::kScaleNames, music::NumScales)) fx->setParam(1, static_cast<float>(scale));
                ImGui::SameLine();
                ImGui::SetNextItemWidth(170 * dpi_);
                int style = static_cast<int>(fx->param(2));
                const char* styles[] = {"Natural", "Hard (robotic)", "Custom"};
                if (ImGui::Combo("Style", &style, styles, 3)) fx->setParam(2, static_cast<float>(style));
                drawPitchGraph(*fx, ImVec2(w, std::max(140.0f * dpi_, ImGui::GetContentRegionAvail().y - 120 * dpi_)));
                if (style == 2) {
                    ParamControl(*fx, 3);
                    ImGui::SameLine();
                    ParamControl(*fx, 4);
                    ImGui::SameLine();
                    ParamControl(*fx, 5);
                    ImGui::SameLine();
                }
                ParamControl(*fx, 8);
                ImGui::SameLine();
                ParamControl(*fx, 9);
                ImGui::SameLine();
                ParamControl(*fx, 10);
            }
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("AI voice")) {
            drawAiPanel(*target);
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    ImGui::End();
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
