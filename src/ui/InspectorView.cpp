// Effects inspector: edit the selected strip's or bus's effect chain.
#include <algorithm>
#include <cmath>
#include <deque>
#include <unordered_map>

#include "dsp/MusicTheory.h"
#include "dsp/NoiseSuppressorInfo.h"
#include "dsp/PitchDisplay.h"
#include "dsp/Responses.h"
#include "imgui.h"
#include "imgui_stdlib.h"
#include "session/Serialization.h"
#include "session/Paths.h"
#include "core/Text.h"
#include <windows.h>
#include <shellapi.h>
#include "ui/App.h"
#include "ui/EffectVisuals.h"
#include "ui/Icons.h"
#include "ui/Theme.h"
#include "ui/Widgets.h"

namespace echo::ui {

namespace {
std::unordered_map<uint32_t, bool> g_collapsed;
std::unordered_map<uint32_t, std::deque<PitchFrame>> g_pitchHistory;
std::string g_presetName, g_presetDesc;
} // namespace

void drawResponseCurve(const Effect& fx, double sr, ImVec2 size) {
    const Palette& p = pal();
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    ImGui::Dummy(size);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(pos, ImVec2(pos.x + size.x, pos.y + size.y), p.meterBg, 4.0f);
    const double fmin = 20.0, fmax = 20000.0, dbRange = 24.0;
    auto xOf = [&](double f) { return pos.x + static_cast<float>(std::log(f / fmin) / std::log(fmax / fmin)) * size.x; };
    auto yOf = [&](double db) { return pos.y + size.y * 0.5f - static_cast<float>(db / dbRange) * size.y * 0.5f; };
    for (double f : {50.0, 100.0, 200.0, 500.0, 1000.0, 2000.0, 5000.0, 10000.0})
        dl->AddLine(ImVec2(xOf(f), pos.y), ImVec2(xOf(f), pos.y + size.y), withAlpha(p.border, 0.5f));
    for (double d : {-12.0, 0.0, 12.0})
        dl->AddLine(ImVec2(pos.x, yOf(d)), ImVec2(pos.x + size.x, yOf(d)), withAlpha(p.border, d == 0.0 ? 0.9f : 0.5f));
    const bool isEq = std::string(fx.typeId()) == "eq";
    ImVec2 pts[160];
    const int n = 160;
    for (int i = 0; i < n; ++i) {
        const double f = fmin * std::pow(fmax / fmin, i / double(n - 1));
        double db = isEq ? eqResponseDb(fx, f, sr) : filterResponseDb(fx, f, sr);
        db = std::clamp(db, -dbRange, dbRange);
        pts[i] = ImVec2(xOf(f), yOf(db));
    }
    dl->AddPolyline(pts, n, p.accent, 2.0f);
    if (isEq) {
        for (int b = 0; b < 6; ++b) {
            if (fx.param(4 * b) < 0.5f) continue;
            const double f = fx.param(4 * b + 1);
            const double db = std::clamp(static_cast<double>(fx.param(4 * b + 2)), -dbRange, dbRange);
            dl->AddCircleFilled(ImVec2(xOf(f), yOf(db)), 4.0f, p.warn);
        }
    }
}

void drawGrMeter(float grDb, float width) {
    const Palette& p = pal();
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const float h = ImGui::GetFontSize() * 0.55f;
    ImGui::Dummy(ImVec2(width, h));
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(pos, ImVec2(pos.x + width, pos.y + h), p.meterBg, 2.0f);
    const float f = std::clamp(-grDb / 24.0f, 0.0f, 1.0f);
    dl->AddRectFilled(ImVec2(pos.x + width * (1.0f - f), pos.y), ImVec2(pos.x + width, pos.y + h), p.warn, 2.0f);
}

void drawPitchGraph(Effect& fx, ImVec2 size) {
    auto* src = dynamic_cast<PitchDisplaySource*>(&fx);
    if (!src) return;
    auto& hist = g_pitchHistory[fx.instanceId];
    PitchFrame f;
    while (src->pitchHistory().read(&f, 1) == 1) hist.push_back(f);
    const size_t maxFrames = static_cast<size_t>(src->framesPerSecond() * 6.0);
    while (hist.size() > maxFrames) hist.pop_front();
    const Palette& p = pal();
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    ImGui::Dummy(size);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(pos, ImVec2(pos.x + size.x, pos.y + size.y), p.meterBg, 4.0f);
    // Vertical range: centred on recent pitch, 2 octaves.
    double center = 60.0;
    int cnt = 0;
    double sum = 0.0;
    for (const auto& h : hist)
        if (h.detectedHz > 0) {
            sum += music::hzToMidi(h.detectedHz);
            ++cnt;
        }
    if (cnt) center = std::round(sum / cnt);
    const double lo = center - 12.0, hi = center + 12.0;
    auto yOf = [&](double midi) { return pos.y + size.y - static_cast<float>((midi - lo) / (hi - lo)) * size.y; };
    const int key = static_cast<int>(fx.param(0)), scale = static_cast<int>(fx.param(1));
    const float semitonePx = size.y / static_cast<float>(hi - lo);
    const int labelEvery = std::max(1, static_cast<int>(std::ceil(ImGui::GetFontSize() * 1.1f / semitonePx)));
    for (int n = static_cast<int>(lo); n <= static_cast<int>(hi); ++n) {
        const bool allowed = music::noteAllowed(n, key, scale);
        const float y = yOf(n);
        dl->AddLine(ImVec2(pos.x, y), ImVec2(pos.x + size.x, y), withAlpha(allowed ? p.accentDim : p.border, allowed ? 0.6f : 0.25f));
        const bool root = ((n - key) % 12 + 12) % 12 == 0;
        if (allowed && (root || (labelEvery <= 2 && (n % labelEvery) == 0) || (n % labelEvery) == 0)) {
            char lbl[8];
            std::snprintf(lbl, sizeof(lbl), "%s%d", music::kNoteNames[((n % 12) + 12) % 12], n / 12 - 1);
            dl->AddText(ImVec2(pos.x + 3, y - ImGui::GetFontSize() * 0.5f), p.textDim, lbl);
        }
    }
    if (hist.size() > 1) {
        const float dx = size.x / static_cast<float>(maxFrames);
        float x = pos.x + size.x - dx * hist.size();
        ImVec2 prevIn{}, prevOut{};
        bool havePrev = false;
        for (const auto& h : hist) {
            if (h.detectedHz > 0) {
                const ImVec2 in(x, yOf(music::hzToMidi(h.detectedHz)));
                const ImVec2 out(x, yOf(music::hzToMidi(h.outputHz > 0 ? h.outputHz : h.detectedHz)));
                if (havePrev) {
                    dl->AddLine(prevIn, in, withAlpha(p.textDim, 0.8f), 1.5f);
                    dl->AddLine(prevOut, out, p.good, 2.0f);
                }
                prevIn = in;
                prevOut = out;
                havePrev = true;
            } else {
                havePrev = false;
            }
            x += dx;
        }
    }
    dl->AddText(ImVec2(pos.x + size.x - ImGui::CalcTextSize("input  corrected").x - 6, pos.y + 3), p.textDim, "input");
    dl->AddText(ImVec2(pos.x + size.x - ImGui::CalcTextSize("corrected").x - 6, pos.y + 3), p.good, "corrected");
}

void EchoApp::drawEffectCard(const ChainTarget& t, std::vector<std::shared_ptr<Effect>>& chain, int index) {
    Effect& fx = *chain[index];
    const Palette& p = pal();
    ImGui::PushID(static_cast<int>(fx.instanceId));
    const bool bypassed = fx.bypass.load();
    const bool faulted = fx.faulted.load();
    ImGui::PushStyleColor(ImGuiCol_ChildBg, p.panelAlt);
    ImGui::PushStyleColor(ImGuiCol_Border, faulted ? p.bad : bypassed ? p.border : withAlpha(p.accent, 0.6f));
    ImGui::BeginChild("##card", ImVec2(0, 0), ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY);
    // Header.
    ImGui::TextDisabled(ICON_GRIP);
    if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID)) {
        ImGui::SetDragDropPayload("ECHO_FX", &index, sizeof(int));
        ImGui::Text("Move %s", fx.label.c_str());
        ImGui::EndDragDropSource();
    }
    Tip("Drag to reorder");
    ImGui::SameLine();
    bool on = !bypassed;
    if (ToggleButton(ICON_POWER, &on, p.good, ImVec2(0, 0), on ? "Effect on (click to bypass)" : "Bypassed (click to enable)"))
        fx.bypass = !on;
    ImGui::SameLine();
    bool& collapsed = g_collapsed[fx.instanceId];
    ImGui::PushFont(fonts().semibold, 0.0f);
    if (ImGui::Selectable(fx.label.c_str(), false, ImGuiSelectableFlags_AllowOverlap,
                          ImVec2(ImGui::GetContentRegionAvail().x - ImGui::GetFontSize() * 9.5f, 0)))
        collapsed = !collapsed;
    ImGui::PopFont();
    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* pl = ImGui::AcceptDragDropPayload("ECHO_FX")) {
            const int from = *static_cast<const int*>(pl->Data);
            const int to = index;
            const ChainTarget tt = t;
            defer([this, tt, from, to]() { mc_->moveEffect(tt, from, to); });
        }
        ImGui::EndDragDropTarget();
    }
    Tip(fx.info().description);
    const int lat = fx.latencySamples();
    ImGui::SameLine();
    const float cpu = bypassed ? 0.0f : fx.cpuLoad.load() * 100.0f;
    if (lat > 0) ImGui::TextDisabled("%3.0f ms %4.1f%%", lat * 1000.0 / mc_->engine().sampleRate(), cpu);
    else ImGui::TextDisabled("       %4.1f%%", cpu);
    Tip("Latency added by this effect, and the share of one CPU core it uses");
    ImGui::SameLine();
    const ChainTarget tt = t;
    if (ImGui::SmallButton(ICON_UP) && index > 0) defer([this, tt, index]() { mc_->moveEffect(tt, index, index - 1); });
    ImGui::SameLine();
    if (ImGui::SmallButton(ICON_DOWN) && index + 1 < static_cast<int>(chain.size()))
        defer([this, tt, index]() { mc_->moveEffect(tt, index, index + 1); });
    ImGui::SameLine();
    const uint32_t iid = fx.instanceId;
    if (ImGui::SmallButton(ICON_CLOSE)) defer([this, tt, iid]() { mc_->removeEffect(tt, iid); });
    Tip("Remove effect");
    if (faulted) {
        ImGui::TextColored(toVec4(p.bad), ICON_WARNING " This effect produced invalid audio and was bypassed.");
        ImGui::SameLine();
        if (ImGui::SmallButton("Re-enable")) {
            fx.faultCount = 0;
            fx.faulted = false;
            fx.requestReset();
        }
    }

    if (!collapsed) {
        const std::string type = fx.typeId();
        const float w = ImGui::GetContentRegionAvail().x;
        if (type == "eq" || type == "filter") drawResponseCurve(fx, mc_->engine().sampleRate(), ImVec2(w, ImGui::GetFontSize() * 6.0f));
        if (type == "autotune") {
            drawPitchGraph(fx, ImVec2(w, ImGui::GetFontSize() * 9.0f));
            const float det = fx.vis[0].load(), tgt = fx.vis[1].load();
            if (det > 0) {
                const double midi = music::hzToMidi(det, fx.param(6));
                const int n = static_cast<int>(std::lround(midi));
                ImGui::Text("Input %.1f Hz (%s%d %+.0f c)  ->  target %.1f Hz, correction %+.0f cents", det,
                            music::kNoteNames[((n % 12) + 12) % 12], n / 12 - 1, (midi - n) * 100.0, tgt, fx.vis[2].load());
            } else {
                ImGui::TextDisabled("No pitched voice detected");
            }
        }
        const char* gl = fx.visLabel(0);
        if (gl && std::string(gl).find("Gain reduction") != std::string::npos) {
            ImGui::TextDisabled("Gain reduction %.1f dB", fx.vis[0].load());
            drawGrMeter(fx.vis[0].load(), w);
        } else if (type == "gate") {
            Led(fx.vis[0].load() > 0.5f, p.good, ImGui::GetFontSize() * 0.35f);
            ImGui::SameLine();
            ImGui::TextDisabled("%s  (key level %.0f dB)", fx.vis[0].load() > 0.5f ? "Open" : "Closed", fx.vis[1].load());
        } else if (type == noiseSuppressorTypeId()) {
            drawCleanupStatus(fx);
        } else if (type == echoCancellerTypeId()) {
            if (!t.bus) {
                if (StripModel* s = mc_->session().strip(t.id)) {
                    ImGui::TextUnformatted("Speakers to cancel:");
                    ImGui::SameLine();
                    echoReferencePicker(*s, std::min(w * 0.6f, 300.0f * dpi_));
                }
                drawCleanupStatus(fx);
            } else {
                ImGui::TextDisabled("Echo cancellation works on input strips (it needs the microphone signal).");
            }
        } else if (type == "denoise") {
            ImGui::TextDisabled("Noise floor %.0f dB, average attenuation %.1f dB", fx.vis[0].load(), fx.vis[1].load());
        } else if (type == "agc") {
            ImGui::TextDisabled("Applied gain %+.1f dB, level %.1f dBFS", fx.vis[0].load(), fx.vis[1].load());
        }
        // Parameters in a flowing grid.
        const float kd = ImGui::GetFontSize() * 2.4f;
        float x = 0.0f;
        const float spacing = ImGui::GetStyle().ItemSpacing.x;
        for (int i = 0; i < fx.numParams(); ++i) {
            const ParamInfo& pi = fx.paramInfo(i);
            float itemW = pi.scale == ParamScale::Enum ? ImGui::GetFontSize() * 9.0f
                          : pi.scale == ParamScale::Toggle ? ImGui::CalcTextSize(pi.name).x + ImGui::GetFrameHeight() + 8
                                                           : std::max(kd, ImGui::CalcTextSize(pi.name).x);
            if (x > 0.0f && x + itemW > w) x = 0.0f;
            else if (x > 0.0f) ImGui::SameLine();
            ParamControl(fx, i, kd);
            x += itemW + spacing;
        }
    }
    ImGui::EndChild();
    ImGui::PopStyleColor(2);
    ImGui::PopID();
}

void EchoApp::drawInspector() {
    if (!ImGui::Begin("Effects")) {
        ImGui::End();
        return;
    }
    const Palette& p = pal();
    if (!hasSelection_) {
        ImGui::TextWrapped("Select an input strip or output bus in the Mixer to edit its effects chain.");
        ImGui::End();
        return;
    }
    const ChainTarget t = selected_;
    std::vector<std::shared_ptr<Effect>>* chain = mc_->chainFor(t);
    if (!chain) {
        hasSelection_ = false;
        ImGui::End();
        return;
    }
    std::string name;
    std::atomic<bool>* fxBypass = nullptr;
    ImU32 color = p.accent;
    if (t.bus) {
        BusModel* b = mc_->session().bus(t.id);
        name = busLabel(*b) + "  " + b->name;
        fxBypass = &b->rt->fxBypass;
        color = b->color;
    } else {
        StripModel* s = mc_->session().strip(t.id);
        name = s->name;
        fxBypass = &s->rt->fxBypass;
        color = s->color;
        // Gain-staging guidance.
        StripMeters& m = stripMeters(s->id);
        if (m.in.holdDb[0] > -1.0f || m.in.holdDb[1] > -1.0f)
            ImGui::TextColored(toVec4(p.bad), ICON_WARNING " Input is clipping. Lower the strip's Gain knob.");
        else if (m.in.holdDb[0] > -6.0f)
            ImGui::TextColored(toVec4(p.warn), ICON_WARNING " Input is very hot (> -6 dBFS). Consider lowering Gain.");
    }
    ImGui::PushFont(fonts().semibold, fonts().baseSize * 1.25f);
    ImGui::TextColored(toVec4(color), "%s", t.bus ? ICON_SPEAKERS : ICON_MIC);
    ImGui::SameLine();
    ImGui::TextUnformatted(name.c_str());
    ImGui::PopFont();

    bool enabled = !fxBypass->load();
    if (ToggleButton(enabled ? "Effects ON" : "Effects BYPASSED", &enabled, p.good, ImVec2(0, 0),
                     "A/B compare: bypass the whole chain (Ctrl+B)"))
        fxBypass->store(!enabled);
    ImGui::SameLine();
    if (ImGui::Button(ICON_EQUALIZER "  Presets")) ImGui::OpenPopup("##presets");
    ImGui::SameLine();
    if (ImGui::Button(ICON_SAVE "  Save preset")) {
        g_presetName = name + " preset";
        g_presetDesc.clear();
        ImGui::OpenPopup("##savepreset");
    }
    ImGui::SameLine();
    if (ImGui::Button(ICON_DELETE "  Clear") && !chain->empty()) ImGui::OpenPopup("##clearchain");

    if (ImGui::BeginPopup("##presets")) {
        std::vector<std::string> cats;
        for (const auto& pr : presets_.all())
            if (std::find(cats.begin(), cats.end(), pr.category) == cats.end()) cats.push_back(pr.category);
        const ChainPreset* chosen = nullptr;
        for (const auto& cat : cats) {
            if (!ImGui::BeginMenu(cat.c_str())) continue;
            for (const auto& pr : presets_.all()) {
                if (pr.category != cat) continue;
                const std::string label = pr.name + (pr.factory ? "" : "  (user)");
                if (ImGui::MenuItem(label.c_str())) chosen = &pr;
                Tip(pr.description.c_str());
            }
            ImGui::EndMenu();
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Manage user presets...")) managePresets_ = true;
        if (ImGui::MenuItem("Reload presets folder")) {
            presets_.load(paths::presetsDir());
            toast("Reloaded presets", 1);
        }
        if (chosen) applyChainPreset(t, *chosen);
        ImGui::EndPopup();
    }
    if (ImGui::BeginPopup("##savepreset")) {
        ImGui::InputText("Name", &g_presetName);
        ImGui::InputText("Description", &g_presetDesc);
        if (ImGui::Button("Save")) {
            std::string err;
            if (presets_.saveUser(g_presetName, "User", g_presetDesc, chainToJson(*chain), &err))
                toast("Saved preset '" + g_presetName + "'", 1);
            else toast("Saving preset failed: " + err, 3);
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    if (ImGui::BeginPopup("##clearchain")) {
        ImGui::Text("Remove all %d effects?", static_cast<int>(chain->size()));
        if (ImGui::Button("Remove all")) {
            mc_->setChain(t, {});
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    ImGui::Separator();

    ImGui::BeginChild("##chain");
    if (chain->empty()) {
        ImGui::TextDisabled("No effects. Add one below or load a preset.");
    }
    double totalLatency = 0.0;
    for (int i = 0; i < static_cast<int>(chain->size()); ++i) {
        drawEffectCard(t, *chain, i);
        if (!(*chain)[i]->bypass.load()) totalLatency += (*chain)[i]->latencySamples();
    }
    if (ImGui::Button(ICON_ADD "  Add effect", ImVec2(-1, 0))) ImGui::OpenPopup("##addfx");
    const std::string pick = effectPicker("##addfx");
    if (!pick.empty()) {
        if (mc_->addEffect(t, pick)) toast("Added effect", 1);
        else toast("Effect chain is full", 2);
    }
    if (totalLatency > 0)
        ImGui::TextDisabled("Chain latency: %.1f ms", totalLatency * 1000.0 / mc_->engine().sampleRate());
    ImGui::EndChild();
    ImGui::End();
    if (managePresets_) drawPresetManager();
}

void EchoApp::drawPresetManager() {
    ImGui::SetNextWindowSize(ImVec2(620 * dpi_, 420 * dpi_), ImGuiCond_Appearing);
    if (!ImGui::Begin("User presets", &managePresets_, ImGuiWindowFlags_NoDocking)) {
        ImGui::End();
        return;
    }
    ImGui::TextWrapped("User presets are stored as .echopreset JSON files in %s. Factory presets are read-only; "
                       "load one, adjust it and use Save preset to keep your version.",
                       wideToUtf8(paths::presetsDir()).c_str());
    if (ImGui::Button(ICON_FOLDER "  Open presets folder"))
        ShellExecuteW(nullptr, L"open", paths::presetsDir().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    ImGui::SameLine();
    if (ImGui::Button(ICON_REFRESH "  Reload")) presets_.load(paths::presetsDir());
    Tip("To import a preset, copy its .echopreset file into the presets folder and press Reload.");
    ImGui::Separator();
    static std::string renameBuf;
    static std::string renaming;
    std::string action, target;
    if (ImGui::BeginTable("##userpresets", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH)) {
        ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Description", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Actions", ImGuiTableColumnFlags_WidthFixed, 300 * dpi_);
        ImGui::TableHeadersRow();
        bool any = false;
        for (const auto& pr : presets_.all()) {
            if (pr.factory) continue;
            any = true;
            ImGui::PushID(pr.name.c_str());
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            if (renaming == pr.name) {
                ImGui::SetNextItemWidth(-1);
                if (ImGui::InputText("##rn", &renameBuf, ImGuiInputTextFlags_EnterReturnsTrue)) {
                    action = "rename";
                    target = pr.name;
                }
            } else {
                ImGui::TextUnformatted(pr.name.c_str());
            }
            ImGui::TableNextColumn();
            ImGui::TextDisabled("%s", pr.description.c_str());
            ImGui::TableNextColumn();
            if (ImGui::SmallButton("Rename")) {
                renaming = pr.name;
                renameBuf = pr.name;
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("Duplicate")) {
                action = "dup";
                target = pr.name;
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("Export copy")) {
                action = "export";
                target = pr.name;
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("Delete")) {
                action = "delete";
                target = pr.name;
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
        if (!any) ImGui::TextDisabled("No user presets yet. Use 'Save preset' in the Effects panel.");
    }
    std::string err;
    if (action == "rename") {
        if (!presets_.rename(target, renameBuf, &err)) toast(err, 3);
        renaming.clear();
    } else if (action == "dup") {
        if (!presets_.duplicate(target, &err)) toast(err, 3);
    } else if (action == "export") {
        const std::wstring path = paths::appDataDir() + L"\\" + utf8ToWide(target) + L" (export).echopreset";
        if (presets_.exportPreset(target, path, &err)) toast("Exported to the settings folder", 1);
        else toast(err, 3);
    } else if (action == "delete") {
        if (!presets_.remove(target, &err)) toast(err, 3);
    }
    ImGui::End();
}

} // namespace echo::ui
