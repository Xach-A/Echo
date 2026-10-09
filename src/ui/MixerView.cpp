// Mixer: input strips (grouped by type) and output bus strips.
#include <cstdio>

#include "imgui.h"
#include "imgui_stdlib.h"
#include "ui/App.h"
#include "ui/Icons.h"
#include "ui/Theme.h"
#include "ui/Widgets.h"

namespace echo::ui {

namespace {

ImU32 groupColor(StripGroup g) {
    switch (g) {
    case StripGroup::Hardware: return pal().groupHardware;
    case StripGroup::Application: return pal().groupApps;
    case StripGroup::Virtual: return pal().groupVirtual;
    case StripGroup::Generator: return pal().groupGen;
    }
    return pal().accent;
}

const char* groupIcon(StripGroup g) {
    switch (g) {
    case StripGroup::Hardware: return ICON_MIC;
    case StripGroup::Application: return ICON_APPS;
    case StripGroup::Virtual: return ICON_PLUG;
    case StripGroup::Generator: return ICON_TONE;
    }
    return ICON_AUDIO;
}

std::string sourceLabel(const StripModel& s) {
    switch (s.source.kind) {
    case SourceKind::None: return "No input";
    case SourceKind::CaptureDevice: return s.source.deviceId.empty() ? "Default microphone" : s.source.deviceName;
    case SourceKind::RenderLoopback: return s.source.deviceName + " (loopback)";
    case SourceKind::ProcessLoopback: return s.source.processName;
    case SourceKind::SystemExcludingEcho: return "All apps except Echo";
    case SourceKind::ToneGenerator: return "Test signal";
    }
    return "?";
}

void colorBar(ImU32 c, float width) {
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(p.x - 6, p.y - 6), ImVec2(p.x + width - 6, p.y - 2), c, 3.0f,
                                              ImDrawFlags_RoundCornersTop);
}

} // namespace

void EchoApp::drawMixer() {
    if (!ImGui::Begin("Mixer")) {
        ImGui::End();
        return;
    }
    Session& s = mc_->session();
    const float scale = dpi_ * s.ui.uiScale;
    const float stripW = (s.ui.compactStrips ? 92.0f : 112.0f) * scale;
    const float stripH = std::max(ImGui::GetContentRegionAvail().y - ImGui::GetFrameHeightWithSpacing() * 1.4f, 380.0f * scale);

    ImGui::BeginChild("##mixerScroll", ImVec2(0, 0), ImGuiChildFlags_None, ImGuiWindowFlags_HorizontalScrollbar);
    const StripGroup order[] = {StripGroup::Hardware, StripGroup::Application, StripGroup::Virtual, StripGroup::Generator};
    bool first = true;
    for (StripGroup g : order) {
        bool any = false;
        for (auto& st : s.strips) any |= st.group == g;
        if (!any) continue;
        if (!first) ImGui::SameLine(0, 14 * scale);
        first = false;
        ImGui::BeginGroup();
        GroupHeader(stripGroupName(g), groupColor(g));
        bool firstInGroup = true;
        for (auto& st : s.strips) {
            if (st.group != g) continue;
            if (!firstInGroup) ImGui::SameLine();
            firstInGroup = false;
            drawStrip(st, stripW, stripH);
        }
        ImGui::EndGroup();
    }
    // Add input.
    if (!first) ImGui::SameLine(0, 10 * scale);
    ImGui::BeginGroup();
    ImGui::TextDisabled(" ");
    if (ImGui::Button(ICON_ADD "##addstrip", ImVec2(34 * scale, stripH))) ImGui::OpenPopup("##addsource");
    Tip("Add an input: microphone, application, virtual cable, loopback or test signal");
    {
        SourceRef ref;
        StripGroup grp;
        std::string name;
        if (sourcePicker("##addsource", ref, &grp, &name)) {
            const uint32_t id = mc_->addStrip(grp, ref, name);
            if (id) {
                select(ChainTarget{false, id});
                toast("Added input '" + name + "'", 1);
            } else toast("Maximum number of inputs reached", 2);
        }
    }
    ImGui::EndGroup();

    // Buses.
    ImGui::SameLine(0, 22 * scale);
    {
        const ImVec2 p = ImGui::GetCursorScreenPos();
        ImGui::GetWindowDrawList()->AddLine(ImVec2(p.x - 11 * scale, p.y), ImVec2(p.x - 11 * scale, p.y + stripH + 20 * scale),
                                            pal().border, 2.0f);
    }
    for (int k = 0; k < 2; ++k) {
        const BusKind kind = k == 0 ? BusKind::Physical : BusKind::Virtual;
        bool any = false;
        for (auto& b : s.buses) any |= b.kind == kind;
        if (!any) continue;
        ImGui::BeginGroup();
        GroupHeader(k == 0 ? "Output buses (A)" : "Virtual outputs (B)", k == 0 ? pal().busPhysical : pal().busVirtual);
        bool firstBus = true;
        for (auto& b : s.buses) {
            if (b.kind != kind) continue;
            if (!firstBus) ImGui::SameLine();
            firstBus = false;
            drawBusStrip(b, stripW, stripH);
        }
        ImGui::EndGroup();
        ImGui::SameLine(0, 14 * scale);
    }
    ImGui::BeginGroup();
    ImGui::TextDisabled(" ");
    if (ImGui::Button(ICON_ADD "##addbus", ImVec2(34 * scale, stripH))) ImGui::OpenPopup("##addbusmenu");
    Tip("Add an output bus (physical device or virtual cable)");
    if (ImGui::BeginPopup("##addbusmenu")) {
        if (ImGui::MenuItem(ICON_HEADPHONES "  Physical output bus (A)")) {
            const uint32_t id = mc_->addBus(BusKind::Physical, OutputRef{}, "Output");
            if (!id) toast("Maximum number of buses reached", 2);
        }
        if (ImGui::MenuItem(ICON_PLUG "  Virtual output bus (B)")) {
            OutputRef none;
            const uint32_t id = mc_->addBus(BusKind::Virtual, none, "Virtual Out");
            if (!id) toast("Maximum number of buses reached", 2);
        }
        ImGui::EndPopup();
    }
    ImGui::EndGroup();
    ImGui::EndChild();
    ImGui::End();
}

void EchoApp::drawStrip(StripModel& s, float width, float height) {
    const Palette& p = pal();
    const float scale = dpi_ * mc_->session().ui.uiScale;
    StripRuntime& rt = *s.rt;
    const bool selected = hasSelection_ && !selected_.bus && selected_.id == s.id;
    ImGui::PushID(static_cast<int>(s.id));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, selected ? p.panelAlt : withAlpha(p.panelAlt, 0.55f));
    ImGui::PushStyleColor(ImGuiCol_Border, selected ? p.accent : p.border);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(6 * scale, 6 * scale));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(4 * scale, 4 * scale));
    ImGui::BeginChild("##strip", ImVec2(width, height), ImGuiChildFlags_Borders, ImGuiWindowFlags_NoScrollbar);
    if (ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows) && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
        select(ChainTarget{false, s.id});
    const float inner = ImGui::GetContentRegionAvail().x;

    // Header: colour bar, icon, name.
    colorBar(s.color, width);
    ImGui::TextColored(toVec4(s.color), "%s", groupIcon(s.group));
    ImGui::SameLine();
    ImGui::PushFont(fonts().semibold, 0.0f);
    TextEllipsis(s.name, inner - ImGui::GetFontSize() * 2.6f);
    ImGui::PopFont();
    ImGui::SameLine(inner - ImGui::GetFontSize() * 0.9f);
    if (ImGui::SmallButton(ICON_MORE)) ImGui::OpenPopup("##stripmenu");
    Tip("Strip options");

    // Source selector.
    ImGui::PushStyleColor(ImGuiCol_Button, p.frame);
    const std::string src = sourceLabel(s);
    if (ImGui::Button(("##src" + std::to_string(s.id)).c_str(), ImVec2(inner, 0))) ImGui::OpenPopup("##pick");
    {
        const ImVec2 mn = ImGui::GetItemRectMin(), mx = ImGui::GetItemRectMax();
        ImGui::PushClipRect(mn, mx, true);
        ImGui::GetWindowDrawList()->AddText(ImVec2(mn.x + 6, mn.y + (mx.y - mn.y - ImGui::GetFontSize()) * 0.5f),
                                            s.online ? p.text : p.bad, src.c_str());
        ImGui::PopClipRect();
    }
    ImGui::PopStyleColor();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s\n%s\nClick to change the input", src.c_str(), s.status.c_str());
    {
        SourceRef ref;
        StripGroup grp;
        std::string name;
        if (sourcePicker("##pick", ref, &grp, &name)) {
            const uint32_t id = s.id;
            defer([this, id, ref, grp]() {
                mc_->setStripSource(id, ref);
                if (StripModel* st = mc_->session().strip(id)) st->group = grp;
            });
        }
    }
    if (!s.online) {
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + inner);
        ImGui::TextColored(toVec4(p.bad), "%s", s.status.c_str());
        ImGui::PopTextWrapPos();
    }

    // Trim + pan knobs.
    const float kd = ImGui::GetFontSize() * 2.0f;
    float trim = rt.trimDb.load();
    if (Knob("Gain", &trim, -24.0f, 36.0f, 0.0f, "%+.1f", false, kd, s.color)) rt.trimDb = trim;
    Tip("Input gain (trim) before effects. Aim for peaks around -12 dBFS on the input meter.");
    ImGui::SameLine(inner * 0.5f + 4 * scale);
    float pan = rt.pan.load();
    const bool mono = rt.isMono.load();
    if (Knob(mono ? "Pan" : "Balance", &pan, -1.0f, 1.0f, 0.0f, "%+.2f", false, kd, s.color)) rt.pan = pan;

    // Fader + meters: fill the space left after the rows below them
    // (dB readout, M/S/FX, and the bus buttons, two per row).
    const int busRows = (static_cast<int>(mc_->session().buses.size()) + 1) / 2;
    const float below = ImGui::GetTextLineHeightWithSpacing() + ImGui::GetFrameHeightWithSpacing() * (1 + busRows) +
                        ImGui::GetStyle().WindowPadding.y;
    const float faderH = std::max(100.0f * scale, ImGui::GetContentRegionAvail().y - below);
    const float faderW = std::min(34.0f * scale, inner * 0.38f);
    StripMeters& m = stripMeters(s.id);
    float fdb = rt.faderDb.load();
    if (Fader("##fader", &fdb, ImVec2(faderW, faderH), s.color)) rt.faderDb = fdb;
    ImGui::SameLine();
    LevelMeter("##inmeter", m.in, ImVec2(6 * scale, faderH), false);
    Tip("Input level (after gain, before effects)");
    ImGui::SameLine();
    LevelMeter("##outmeter", m.out, ImVec2(std::max(14.0f * scale, inner - faderW - 18 * scale), faderH), true, true);
    char dbtxt[24];
    if (fdb <= -100.0f) std::snprintf(dbtxt, sizeof(dbtxt), "-inf");
    else std::snprintf(dbtxt, sizeof(dbtxt), "%+.1f dB", fdb);
    ImGui::TextDisabled("%s", dbtxt);

    // Mute / solo / FX.
    const float bw = (inner - 8 * scale) / 3.0f;
    bool mute = rt.mute.load(), solo = rt.solo.load();
    if (ToggleButton("M", &mute, p.bad, ImVec2(bw, 0), "Mute (M)")) rt.mute = mute;
    ImGui::SameLine();
    if (ToggleButton("S", &solo, p.warn, ImVec2(bw, 0), "Solo: only soloed inputs are heard")) rt.solo = solo;
    ImGui::SameLine();
    char fxl[16];
    std::snprintf(fxl, sizeof(fxl), "FX %d", static_cast<int>(s.chain.size()));
    bool fxOn = !s.chain.empty() && !rt.fxBypass.load();
    if (ToggleButton(fxl, &fxOn, p.accent, ImVec2(bw, 0), "Open this input's effects chain")) {
        select(ChainTarget{false, s.id});
        focusWindow("Effects");
    }

    // Bus assignment.
    const auto& buses = mc_->session().buses;
    const int perRow = 2;
    const float tw = (inner - 4 * scale * (perRow - 1)) / perRow;
    int col = 0;
    for (const auto& b : buses) {
        if (col > 0) ImGui::SameLine();
        SendParams& sp = rt.sends[b.slot];
        bool on = sp.enabled.load();
        const bool blocked = sp.blocked.load();
        const std::string lbl = busLabel(b) + (sp.preFader.load() ? "*" : "") + "##send" + std::to_string(b.id);
        const ImU32 onCol = blocked ? p.bad : (b.kind == BusKind::Physical ? p.busPhysical : p.busVirtual);
        if (ToggleButton(lbl.c_str(), &on, onCol, ImVec2(tw, 0))) mc_->setSend(s.id, b.id, on);
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Send to %s '%s'%s\nGain %+.1f dB, %s\nRight-click for send gain and pre/post fader",
                              busLabel(b).c_str(), b.name.c_str(), blocked ? "\nBLOCKED: would create a feedback loop" : "",
                              sp.gainDb.load(), sp.preFader.load() ? "pre-fader" : "post-fader");
        }
        if (ImGui::BeginPopupContextItem()) {
            ImGui::Text("Send to %s", b.name.c_str());
            float g = sp.gainDb.load();
            ImGui::SetNextItemWidth(160 * scale);
            if (ImGui::SliderFloat("Gain", &g, -60.0f, 12.0f, "%+.1f dB")) sp.gainDb = g;
            bool pre = sp.preFader.load();
            if (ImGui::Checkbox("Pre-fader (ignores this strip's fader)", &pre)) sp.preFader = pre;
            ImGui::EndPopup();
        }
        if (++col == perRow) col = 0;
    }

    // Options menu.
    if (ImGui::BeginPopup("##stripmenu")) {
        ImGui::SetNextItemWidth(180 * scale);
        ImGui::InputText("Name", &s.name);
        ImVec4 c = toVec4(s.color);
        if (ImGui::ColorEdit3("Colour", &c.x, ImGuiColorEditFlags_NoInputs)) s.color = ImGui::ColorConvertFloat4ToU32(c);
        ImGui::Separator();
        int chMode = rt.downmix.load() ? 2 : (rt.chanCount.load() == 1 ? 0 : 1);
        const char* modes[] = {"Mono", "Stereo (linked)", "Downmix all channels"};
        ImGui::SetNextItemWidth(180 * scale);
        if (ImGui::Combo("Channels", &chMode, modes, 3)) {
            rt.downmix = chMode == 2;
            rt.chanCount = chMode == 0 ? 1 : 2;
        }
        int first = rt.chanFirst.load() + 1;
        ImGui::SetNextItemWidth(180 * scale);
        if (ImGui::SliderInt("First channel", &first, 1, kMaxSourceChannels)) rt.chanFirst = first - 1;
        Tip("For multi-channel interfaces: which input channel (or pair) this strip uses.");
        bool inv = rt.invertPolarity.load();
        if (ImGui::Checkbox("Invert polarity", &inv)) rt.invertPolarity = inv;
        bool byp = rt.fxBypass.load();
        if (ImGui::Checkbox("Bypass all effects (A/B compare)", &byp)) rt.fxBypass = byp;
        noiseSuppressionCheckbox(s, "Noise suppression");
        ImGui::Separator();
        int idx = 0;
        for (size_t i = 0; i < mc_->session().strips.size(); ++i)
            if (mc_->session().strips[i].id == s.id) idx = static_cast<int>(i);
        const uint32_t id = s.id;
        if (ImGui::MenuItem(ICON_UP "  Move left")) defer([this, id, idx]() { mc_->moveStrip(id, idx - 1); });
        if (ImGui::MenuItem(ICON_DOWN "  Move right")) defer([this, id, idx]() { mc_->moveStrip(id, idx + 1); });
        ImGui::Separator();
        if (ImGui::MenuItem(ICON_DELETE "  Remove input")) {
            defer([this, id]() {
                mc_->removeStrip(id);
                if (hasSelection_ && !selected_.bus && selected_.id == id) hasSelection_ = false;
            });
        }
        ImGui::EndPopup();
    }
    ImGui::EndChild();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(2);
    ImGui::PopID();
}

void EchoApp::drawBusStrip(BusModel& b, float width, float height) {
    const Palette& p = pal();
    const float scale = dpi_ * mc_->session().ui.uiScale;
    BusRuntime& rt = *b.rt;
    const bool selected = hasSelection_ && selected_.bus && selected_.id == b.id;
    const ImU32 kc = b.kind == BusKind::Physical ? p.busPhysical : p.busVirtual;
    ImGui::PushID(static_cast<int>(b.id) + 100000);
    ImGui::PushStyleColor(ImGuiCol_ChildBg, selected ? p.panelAlt : withAlpha(p.panelAlt, 0.55f));
    ImGui::PushStyleColor(ImGuiCol_Border, selected ? p.accent : p.border);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(6 * scale, 6 * scale));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(4 * scale, 4 * scale));
    ImGui::BeginChild("##bus", ImVec2(width, height), ImGuiChildFlags_Borders, ImGuiWindowFlags_NoScrollbar);
    if (ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows) && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
        select(ChainTarget{true, b.id});
    const float inner = ImGui::GetContentRegionAvail().x;
    colorBar(b.color, width);
    ImGui::TextColored(toVec4(kc), "%s", busLabel(b).c_str());
    ImGui::SameLine();
    ImGui::PushFont(fonts().semibold, 0.0f);
    TextEllipsis(b.name, inner - ImGui::GetFontSize() * 3.4f);
    ImGui::PopFont();
    ImGui::SameLine(inner - ImGui::GetFontSize() * 0.9f);
    if (ImGui::SmallButton(ICON_MORE)) ImGui::OpenPopup("##busmenu");

    const std::string out = b.output.deviceId.empty()
                                ? (b.kind == BusKind::Physical ? std::string("Default output") : std::string("No device"))
                                : b.output.deviceName;
    if (ImGui::Button("##outdev", ImVec2(inner, 0))) ImGui::OpenPopup("##outpick");
    {
        const ImVec2 mn = ImGui::GetItemRectMin(), mx = ImGui::GetItemRectMax();
        ImGui::PushClipRect(mn, mx, true);
        ImGui::GetWindowDrawList()->AddText(ImVec2(mn.x + 6, mn.y + (mx.y - mn.y - ImGui::GetFontSize()) * 0.5f),
                                            b.online ? p.text : p.bad,
                                            ((b.kind == BusKind::Physical ? ICON_HEADPHONES " " : ICON_PLUG " ") + out).c_str());
        ImGui::PopClipRect();
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s\n%s\nClick to choose the output device", out.c_str(), b.status.c_str());
    {
        OutputRef o = b.output;
        if (outputPicker("##outpick", o, b.kind == BusKind::Virtual)) {
            const uint32_t id = b.id;
            defer([this, id, o]() { mc_->setBusOutput(id, o); });
        }
    }
    if (!b.online) {
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + inner);
        ImGui::TextColored(toVec4(b.kind == BusKind::Virtual && b.output.deviceId.empty() ? p.warn : p.bad), "%s",
                           b.status.c_str());
        ImGui::PopTextWrapPos();
    } else if (b.isClock) {
        ImGui::TextColored(toVec4(p.textDim), ICON_SYNC " engine clock");
        Tip("This output's device paces the audio engine; all other devices are resampled to stay in sync.");
    } else {
        ImGui::TextDisabled(" ");
    }

    const float below = ImGui::GetTextLineHeightWithSpacing() + ImGui::GetFrameHeightWithSpacing() * 2 +
                        ImGui::GetStyle().WindowPadding.y;
    const float faderH = std::max(100.0f * scale, ImGui::GetContentRegionAvail().y - below);
    const float faderW = std::min(34.0f * scale, inner * 0.4f);
    float fdb = rt.faderDb.load();
    if (Fader("##fader", &fdb, ImVec2(faderW, faderH), kc)) rt.faderDb = fdb;
    ImGui::SameLine();
    LevelMeter("##meter", busMeters(b.id), ImVec2(inner - faderW - 8 * scale, faderH), true, true);
    char dbtxt[40];
    const float gr = rt.limiterGrDb.load();
    if (fdb <= -100.0f) std::snprintf(dbtxt, sizeof(dbtxt), "-inf");
    else std::snprintf(dbtxt, sizeof(dbtxt), "%+.1f dB", fdb);
    ImGui::TextDisabled("%s", dbtxt);
    if (gr < -0.5f) {
        ImGui::SameLine();
        ImGui::TextColored(toVec4(p.warn), "LIM %.0f", gr);
        Tip("The output limiter is reducing gain to prevent clipping. Lower the fader or input levels.");
    }
    const float avail3 = inner - 8 * scale;
    bool mute = rt.mute.load(), monoB = rt.mono.load(), lim = rt.limiterEnabled.load();
    if (ToggleButton("M", &mute, p.bad, ImVec2(avail3 * 0.24f, 0), "Mute this output")) rt.mute = mute;
    ImGui::SameLine();
    if (ToggleButton("Mono", &monoB, p.accent, ImVec2(avail3 * 0.42f, 0), "Sum to mono")) rt.mono = monoB;
    ImGui::SameLine();
    if (ToggleButton("Lim", &lim, p.good, ImVec2(avail3 * 0.34f, 0), "Safety limiter (prevents clipping)")) rt.limiterEnabled = lim;
    char fxl[16];
    std::snprintf(fxl, sizeof(fxl), "FX %d", static_cast<int>(b.chain.size()));
    bool fxOn = !b.chain.empty() && !rt.fxBypass.load();
    if (ToggleButton(fxl, &fxOn, p.accent, ImVec2(inner, 0), "Open this bus's effects chain")) {
        select(ChainTarget{true, b.id});
        focusWindow("Effects");
    }

    if (ImGui::BeginPopup("##busmenu")) {
        ImGui::SetNextItemWidth(180 * scale);
        ImGui::InputText("Name", &b.name);
        ImVec4 c = toVec4(b.color);
        if (ImGui::ColorEdit3("Colour", &c.x, ImGuiColorEditFlags_NoInputs)) b.color = ImGui::ColorConvertFloat4ToU32(c);
        float ceil = rt.limiterCeilingDb.load();
        ImGui::SetNextItemWidth(180 * scale);
        if (ImGui::SliderFloat("Limiter ceiling", &ceil, -12.0f, 0.0f, "%.1f dBFS")) rt.limiterCeilingDb = ceil;
        bool excl = b.output.exclusive;
        if (ImGui::Checkbox("Exclusive mode (lower latency)", &excl)) {
            OutputRef o = b.output;
            o.exclusive = excl;
            const uint32_t id = b.id;
            defer([this, id, o]() { mc_->setBusOutput(id, o); });
        }
        Tip("Takes the device exclusively: lower latency, but other apps cannot use it meanwhile.");
        bool clock = mc_->session().engine.clockBusId == b.id;
        if (ImGui::Checkbox("Use as engine clock", &clock)) {
            mc_->session().engine.clockBusId = clock ? b.id : 0;
            mc_->rebuildGraph();
        }
        ImGui::Separator();
        if (ImGui::MenuItem(ICON_DELETE "  Remove bus")) {
            const uint32_t id = b.id;
            defer([this, id]() {
                mc_->removeBus(id);
                if (hasSelection_ && selected_.bus && selected_.id == id) hasSelection_ = false;
            });
        }
        ImGui::EndPopup();
    }
    ImGui::EndChild();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(2);
    ImGui::PopID();
}

} // namespace echo::ui
