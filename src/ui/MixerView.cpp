// Mixer: input strips as cards (fader, meter, mute/solo, sends) and output
// buses as rows. Advanced options live in each card's "..." menu; the full
// routing table can be shown on demand.
#include <cstdio>

#include "dsp/NoiseSuppressorInfo.h"
#include "imgui.h"
#include "imgui_internal.h"
#include "imgui_stdlib.h"
#include "ui/App.h"
#include "ui/Icons.h"
#include "ui/Pop.h"
#include "ui/Theme.h"
#include "ui/Widgets.h"

namespace echo::ui {

namespace {

const char* groupIcon(StripGroup g) {
    switch (g) {
    case StripGroup::Hardware: return ICON_MIC;
    case StripGroup::Application: return ICON_APPS;
    case StripGroup::Virtual: return ICON_PLUG;
    case StripGroup::Generator: return ICON_TONE;
    }
    return ICON_AUDIO;
}

ImU32 groupTint(StripGroup g) {
    const Palette& p = pal();
    switch (g) {
    case StripGroup::Hardware: return p.accentTint;
    case StripGroup::Application: return p.busATint;
    case StripGroup::Virtual: return p.busBTint;
    case StripGroup::Generator: return p.warnTint;
    }
    return p.segBg;
}

std::string sourceLabel(const StripModel& s) {
    switch (s.source.kind) {
    case SourceKind::None: return "No input";
    case SourceKind::CaptureDevice: return s.source.deviceId.empty() ? "Windows default microphone" : s.source.deviceName;
    case SourceKind::RenderLoopback: return s.source.deviceName + " (what it plays)";
    case SourceKind::ProcessLoopback: return "App audio · " + s.source.processName;
    case SourceKind::SystemExcludingEcho: return "All apps except Echo";
    case SourceKind::ToneGenerator: return "Test signal";
    }
    return "?";
}

std::string dbText(float db) {
    char buf[24];
    if (db <= -100.0f) std::snprintf(buf, sizeof(buf), "Off");
    else std::snprintf(buf, sizeof(buf), "%s%.1f dB", db > 0.05f ? "+" : db < -0.05f ? "\xe2\x88\x92" : "", std::fabs(db));
    return buf;
}

// Equal-width on/off button (Mute, Solo, Mono, Limiter).
bool StateButton(const char* label, bool on, ImU32 onFill, ImU32 onText, float width) {
    const Palette& p = pal();
    const float sc = uiScale();
    const ImVec2 size(width, 34.0f * sc);
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const bool clicked = ImGui::InvisibleButton(label, size);
    const bool hov = ImGui::IsItemHovered();
    if (hov) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 b(pos.x + size.x, pos.y + size.y);
    dl->AddRectFilled(pos, b, on ? onFill : hov ? p.frameHover : p.surface, 10.0f * sc);
    dl->AddRect(pos, b, p.outline, 10.0f * sc, 1.5f * sc);
    const char* end = ImGui::FindRenderedTextEnd(label);
    ImGui::PushFont(fonts().semibold, 13.0f);
    const ImVec2 ts = ImGui::CalcTextSize(label, end);
    dl->AddText(ImVec2(pos.x + (size.x - ts.x) * 0.5f, pos.y + (size.y - ts.y) * 0.5f), on ? onText : p.text, label, end);
    ImGui::PopFont();
    return clicked;
}

} // namespace

void EchoApp::drawMixer() {
    const Palette& p = pal();
    const float sc = uiScale();
    Session& s = mc_->session();
    const float avail = ImGui::GetContentRegionAvail().x;

    pop::PageTitle("Mixer", "Every sound coming in, and where it goes.");
    pop::Badge("A · what you hear", p.busATint, p.busAInk);
    ImGui::SameLine(0, 8.0f * sc);
    pop::Badge("B · what others hear", p.busBTint, p.busBInk);
    ImGui::SameLine(0, 16.0f * sc);
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() - 4.0f * sc);
    if (pop::SmallButton(routingOpen_ ? "Hide routing table" : "Routing table")) routingOpen_ = !routingOpen_;
    Tip("Every input against every output in one table, with send levels.");

    for (const auto& w : mc_->feedbackWarnings()) pop::Banner(ICON_WARNING, w.text.c_str(), p.warnTint, p.warn);
    if (routingOpen_) {
        pop::BeginCard("##routing", 0.0f, 0.0f, true);
        drawRouting();
        pop::EndCard();
    }
    ImGui::Dummy(ImVec2(0, 4.0f * sc));

    // ------------------------------------------------------------------ inputs
    const float gap = 16.0f * sc;
    const int cols = pop::columnsFor(avail, 232.0f * sc, gap);
    const float cw = (avail - gap * (cols - 1)) / cols;
    int col = 0;
    for (auto& st : s.strips) {
        if (col++ % cols) ImGui::SameLine(0, gap);
        drawStrip(st, cw);
    }
    if (col % cols) ImGui::SameLine(0, gap);
    {
        // "Add input" tile.
        const ImVec2 pos = ImGui::GetCursorScreenPos();
        const ImVec2 size(cw, 150.0f * sc);
        if (ImGui::InvisibleButton("##addinput", size)) ImGui::OpenPopup("##addsource");
        const bool hov = ImGui::IsItemHovered();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 b(pos.x + size.x, pos.y + size.y);
        if (hov) dl->AddRectFilled(pos, b, p.surfaceAlt, 16.0f * sc);
        // dashed outline
        const float dash = 6.0f * sc;
        for (float x = pos.x + 16 * sc; x < b.x - 16 * sc; x += dash * 2) {
            dl->AddLine(ImVec2(x, pos.y), ImVec2(std::min(x + dash, b.x - 16 * sc), pos.y), p.border, 1.5f * sc);
            dl->AddLine(ImVec2(x, b.y), ImVec2(std::min(x + dash, b.x - 16 * sc), b.y), p.border, 1.5f * sc);
        }
        for (float y = pos.y + 16 * sc; y < b.y - 16 * sc; y += dash * 2) {
            dl->AddLine(ImVec2(pos.x, y), ImVec2(pos.x, std::min(y + dash, b.y - 16 * sc)), p.border, 1.5f * sc);
            dl->AddLine(ImVec2(b.x, y), ImVec2(b.x, std::min(y + dash, b.y - 16 * sc)), p.border, 1.5f * sc);
        }
        const ImVec2 c(pos.x + size.x * 0.5f, pos.y + size.y * 0.5f - 14.0f * sc);
        dl->AddCircleFilled(c, 20.0f * sc, p.surface, 24);
        dl->AddCircle(c, 20.0f * sc, p.outline, 24, 1.5f * sc);
        dl->AddLine(ImVec2(c.x - 8 * sc, c.y), ImVec2(c.x + 8 * sc, c.y), p.text, 2.0f * sc);
        dl->AddLine(ImVec2(c.x, c.y - 8 * sc), ImVec2(c.x, c.y + 8 * sc), p.text, 2.0f * sc);
        const char* t1 = "Add input";
        const ImVec2 ts = ImGui::CalcTextSize(t1);
        dl->AddText(ImVec2(c.x - ts.x * 0.5f, c.y + 28.0f * sc), p.text2, t1);
        SourceRef ref;
        StripGroup grp;
        std::string name;
        if (sourcePicker("##addsource", ref, &grp, &name)) {
            const uint32_t id = mc_->addStrip(grp, ref, name);
            if (id) toast("Added input '" + name + "'", 1);
            else toast("Maximum number of inputs reached", 2);
        }
    }

    // ----------------------------------------------------------------- outputs
    ImGui::Dummy(ImVec2(0, 12.0f * sc));
    pop::SectionTitle("Outputs");
    ImGui::SameLine(0, 16.0f * sc);
    if (pop::SmallButton("Add output")) ImGui::OpenPopup("##addbusmenu");
    if (ImGui::BeginPopup("##addbusmenu")) {
        if (ImGui::MenuItem(ICON_HEADPHONES "  Headphones or speakers (A)")) {
            if (!mc_->addBus(BusKind::Physical, OutputRef{}, "Output")) toast("Maximum number of outputs reached", 2);
        }
        if (ImGui::MenuItem(ICON_PLUG "  Virtual mic (B)")) {
            if (!mc_->addBus(BusKind::Virtual, OutputRef{}, "Virtual Mic")) toast("Maximum number of outputs reached", 2);
        }
        ImGui::EndPopup();
    }
    const int bcols = avail > 900.0f * sc ? 2 : 1;
    const float bw = (avail - gap * (bcols - 1)) / bcols;
    int bi = 0;
    for (auto& b : s.buses) {
        if (bi++ % bcols) ImGui::SameLine(0, gap);
        drawBusRow(b, bw);
    }
}

void EchoApp::drawStrip(StripModel& s, float width) {
    const Palette& p = pal();
    const float sc = uiScale();
    StripRuntime& rt = *s.rt;
    ImGui::PushID(static_cast<int>(s.id));
    pop::BeginCard("##strip", width, 0.0f, false, 16.0f, 16.0f);
    const float inner = ImGui::GetContentRegionAvail().x;

    // Header: icon, name, source (click to change), options.
    pop::IconBox(groupIcon(s.group), groupTint(s.group), 36.0f);
    ImGui::SameLine(0, 10.0f * sc);
    ImGui::BeginGroup();
    ImGui::PushFont(fonts().semibold, 15.0f);
    TextEllipsis(s.name, inner - 90.0f * sc);
    ImGui::PopFont();
    ImGui::PushFont(fonts().regular, 12.0f);
    const std::string src = sourceLabel(s);
    ImGui::PushStyleColor(ImGuiCol_Text, toVec4(s.online ? p.text2 : p.bad));
    TextEllipsis(src, inner - 90.0f * sc);
    ImGui::PopStyleColor();
    ImGui::PopFont();
    ImGui::EndGroup();
    if (ImGui::IsItemClicked()) ImGui::OpenPopup("##pick");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s\n%s\nClick to change the input", src.c_str(), s.status.c_str());
    ImGui::SameLine(inner - 22.0f * sc + ImGui::GetStyle().WindowPadding.x);
    if (ImGui::SmallButton(ICON_MORE)) ImGui::OpenPopup("##stripmenu");
    Tip("Input gain, pan, channels, rename, remove...");
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

    // Effects chip and push-to-talk badge.
    {
        bool voiceFx = false;
        int count = 0;
        for (const auto& e : s.chain) {
            ++count;
            const std::string t = e->typeId();
            voiceFx |= t != noiseSuppressorTypeId() && t != echoCancellerTypeId();
        }
        std::string chip;
        if (count == 0) chip = "Add effect";
        else if (!voiceFx) chip = count == 1 ? "Noise removal" : "Noise & echo removal";
        else chip = s.voice.empty() ? std::to_string(count) + " effects" : s.voice;
        if (rt.fxBypass.load() && count) chip += " (off)";
        const bool fxOn = count > 0 && !rt.fxBypass.load();
        if (pop::Pill((chip + "##fx").c_str(), fxOn ? p.accentTint : p.surface, fxOn ? p.accent : p.lineSoft,
                      fxOn ? p.accentInk : p.text2))
            openEffects(ChainTarget{false, s.id});
        Tip("Open this input's effects");
        const HotkeySettings& hk = mc_->session().hotkeys;
        if (rt.talkGated.load() && hk.talkMode != TalkMode::Off && !hk.talkKey.empty()) {
            ImGui::SameLine(0, 6.0f * sc);
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 4.0f * sc);
            pop::Badge(hk.talkMode == TalkMode::PushToTalk ? "Push to talk" : "Push to mute", p.segBg, p.text2);
        }
    }

    // Meter + fader.
    ImGui::Dummy(ImVec2(0, 2.0f * sc));
    const float meterH = 190.0f * sc;
    const float faderW = 40.0f * sc, meterW = 14.0f * sc, between = 22.0f * sc;
    StripMeters& m = stripMeters(s.id);
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (inner - faderW - meterW - between) * 0.5f);
    LevelMeter("##meter", m.out, ImVec2(meterW, meterH), !rt.isMono.load());
    ImGui::SameLine(0, between);
    float fdb = rt.faderDb.load();
    if (Fader("##fader", &fdb, ImVec2(faderW, meterH), p.accent)) rt.faderDb = fdb;
    {
        const std::string t = dbText(fdb);
        ImGui::PushFont(fonts().semibold, 14.0f);
        const float tw = ImGui::CalcTextSize(t.c_str()).x;
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (inner - tw) * 0.5f);
        ImGui::TextUnformatted(t.c_str());
        ImGui::PopFont();
    }

    // Mute / solo.
    const float half = (inner - 8.0f * sc) * 0.5f;
    if (StateButton("Mute", rt.mute.load(), p.primary, p.onPrimary, half)) rt.mute = !rt.mute.load();
    ImGui::SameLine(0, 8.0f * sc);
    if (StateButton("Solo", rt.solo.load(), p.warnTint, p.warnInk, half)) rt.solo = !rt.solo.load();
    Tip("Solo: only soloed inputs are heard");

    // Sends.
    ImGui::PushFont(fonts().regular, 12.0f);
    ImGui::TextColored(toVec4(p.text2), "Send to");
    ImGui::PopFont();
    for (const auto& b : mc_->session().buses) {
        SendParams& sp = rt.sends[b.slot];
        const bool on = sp.enabled.load();
        const bool blocked = sp.blocked.load();
        const std::string lbl = b.name + " (" + busLabel(b) + ")" + (sp.preFader.load() ? " · pre" : "") + "##send" + std::to_string(b.id);
        if (pop::SendPill(lbl.c_str(), on, b.kind == BusKind::Virtual, inner, blocked)) mc_->setSend(s.id, b.id, !on);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Send to %s '%s'%s\nLevel %+.1f dB, %s\nRight-click for the send level and pre/post fader",
                              busLabel(b).c_str(), b.name.c_str(), blocked ? "\nBlocked: it would create a feedback loop" : "",
                              sp.gainDb.load(), sp.preFader.load() ? "before the fader" : "after the fader");
        if (ImGui::BeginPopupContextItem()) {
            ImGui::Text("Send to %s", b.name.c_str());
            float g = sp.gainDb.load();
            ImGui::SetNextItemWidth(180 * sc);
            if (ImGui::SliderFloat("Level", &g, -60.0f, 12.0f, "%+.1f dB")) sp.gainDb = g;
            bool pre = sp.preFader.load();
            if (ImGui::Checkbox("Before the fader (ignores this input's volume)", &pre)) sp.preFader = pre;
            ImGui::EndPopup();
        }
    }

    // Options menu.
    if (ImGui::BeginPopup("##stripmenu")) {
        ImGui::SetNextItemWidth(200 * sc);
        ImGui::InputText("Name", &s.name);
        float trim = rt.trimDb.load();
        ImGui::SetNextItemWidth(200 * sc);
        if (ImGui::SliderFloat("Input gain", &trim, -24.0f, 36.0f, "%+.1f dB")) rt.trimDb = trim;
        Tip("Gain before effects. Aim for peaks around -12 dBFS.");
        float pan = rt.pan.load();
        ImGui::SetNextItemWidth(200 * sc);
        if (ImGui::SliderFloat(rt.isMono.load() ? "Pan" : "Balance", &pan, -1.0f, 1.0f, "%+.2f")) rt.pan = pan;
        ImVec4 c = toVec4(s.color);
        if (ImGui::ColorEdit3("Colour", &c.x, ImGuiColorEditFlags_NoInputs)) s.color = ImGui::ColorConvertFloat4ToU32(c);
        ImGui::Separator();
        int chMode = rt.downmix.load() ? 2 : (rt.chanCount.load() == 1 ? 0 : 1);
        const char* modes[] = {"Mono", "Stereo (linked)", "Downmix all channels"};
        ImGui::SetNextItemWidth(200 * sc);
        if (ImGui::Combo("Channels", &chMode, modes, 3)) {
            rt.downmix = chMode == 2;
            rt.chanCount = chMode == 0 ? 1 : 2;
        }
        int first = rt.chanFirst.load() + 1;
        ImGui::SetNextItemWidth(200 * sc);
        if (ImGui::SliderInt("First channel", &first, 1, kMaxSourceChannels)) rt.chanFirst = first - 1;
        Tip("For multi-channel interfaces: which input channel (or pair) this input uses.");
        bool inv = rt.invertPolarity.load();
        if (ImGui::Checkbox("Invert polarity", &inv)) rt.invertPolarity = inv;
        bool byp = rt.fxBypass.load();
        if (ImGui::Checkbox("Bypass all effects (A/B compare)", &byp)) rt.fxBypass = byp;
        noiseSuppressionCheckbox(s, "Noise removal");
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
                if (hasSelection_ && !selected_.bus && selected_.id == id) {
                    hasSelection_ = false;
                    effectsOpen_ = false;
                }
            });
        }
        ImGui::EndPopup();
    }
    pop::EndCard();
    ImGui::PopID();
}

void EchoApp::drawBusRow(BusModel& b, float width) {
    const Palette& p = pal();
    const float sc = uiScale();
    BusRuntime& rt = *b.rt;
    const bool isB = b.kind == BusKind::Virtual;
    const ImU32 kc = isB ? p.busVirtual : p.busPhysical;
    ImGui::PushID(static_cast<int>(b.id) + 100000);
    pop::BeginCard("##bus", width, 0.0f, false, 16.0f, 14.0f);
    const float inner = ImGui::GetContentRegionAvail().x;

    // Header: colour bar, name + tag, device (click to change), toggles.
    const ImVec2 hp = ImGui::GetCursorScreenPos();
    ImGui::GetWindowDrawList()->AddRectFilled(hp, ImVec2(hp.x + 8.0f * sc, hp.y + 38.0f * sc), kc, 4.0f * sc);
    ImGui::SetCursorScreenPos(ImVec2(hp.x + 18.0f * sc, hp.y));
    ImGui::BeginGroup();
    ImGui::PushFont(fonts().semibold, 15.0f);
    ImGui::TextUnformatted(b.name.c_str());
    ImGui::SameLine(0, 6.0f * sc);
    ImGui::TextColored(toVec4(kc), "%s", busLabel(b).c_str());
    ImGui::PopFont();
    std::string out = b.output.deviceId.empty() ? (isB ? std::string("No virtual cable chosen") : std::string("Windows default output"))
                                                : b.output.deviceName;
    if (b.isClock && b.online) out += " · timing source";
    ImGui::PushFont(fonts().regular, 12.0f);
    ImGui::PushStyleColor(ImGuiCol_Text, toVec4(b.online ? p.text2 : (isB && b.output.deviceId.empty() ? p.warnInk : p.bad)));
    TextEllipsis(out, inner * 0.5f);
    ImGui::PopStyleColor();
    ImGui::PopFont();
    ImGui::EndGroup();
    if (ImGui::IsItemClicked()) ImGui::OpenPopup("##outpick");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s\n%s%s\nClick to choose the device", out.c_str(), b.status.c_str(),
                          b.isClock ? "\nThis device paces Echo; other devices are kept in sync with it." : "");
    {
        OutputRef o = b.output;
        if (outputPicker("##outpick", o, isB)) {
            const uint32_t id = b.id;
            defer([this, id, o]() { mc_->setBusOutput(id, o); });
        }
    }
    const float tbw = 74.0f * sc;
    ImGui::SameLine(inner - tbw * 3 - 8.0f * sc * 2 - 30.0f * sc + ImGui::GetStyle().WindowPadding.x);
    if (StateButton("Mute", rt.mute.load(), p.primary, p.onPrimary, tbw)) rt.mute = !rt.mute.load();
    ImGui::SameLine(0, 8.0f * sc);
    if (StateButton("Mono", rt.mono.load(), isB ? p.busBTint : p.busATint, isB ? p.busBInk : p.busAInk, tbw)) rt.mono = !rt.mono.load();
    ImGui::SameLine(0, 8.0f * sc);
    if (StateButton("Limiter", rt.limiterEnabled.load(), p.goodTint, p.goodInk, tbw)) rt.limiterEnabled = !rt.limiterEnabled.load();
    Tip("Safety limiter: stops clipping");
    ImGui::SameLine(0, 6.0f * sc);
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 5.0f * sc);
    if (ImGui::SmallButton(ICON_MORE)) ImGui::OpenPopup("##busmenu");

    // Level, volume.
    ImGui::Dummy(ImVec2(0, 2.0f * sc));
    MeterBallistics& m = busMeters(b.id);
    const float meterW = inner * 0.42f;
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 7.0f * sc);
    pop::BarMeter(std::max(m.levelDb[0], m.levelDb[1]), meterW, 10.0f * sc);
    const float gr = rt.limiterGrDb.load();
    if (gr < -0.5f) Tip("The limiter is holding this output down. Lower the volume or the inputs.");
    ImGui::SameLine(0, 14.0f * sc);
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() - 7.0f * sc);
    float pos = faderDbToPos(rt.faderDb.load());
    if (pop::Slider("##vol", &pos, 0.0f, 1.0f, faderDbToPos(0.0f), inner - meterW - 14.0f * sc - 74.0f * sc, kc)) {
        float db = faderPosToDb(pos);
        if (db < -99.0f) db = -120.0f;
        rt.faderDb = db;
    }
    ImGui::SameLine(0, 8.0f * sc);
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 2.0f * sc);
    ImGui::PushFont(fonts().semibold, 13.0f);
    ImGui::TextUnformatted(dbText(rt.faderDb.load()).c_str());
    ImGui::PopFont();
    if (gr < -0.5f) {
        ImGui::PushFont(fonts().regular, 12.0f);
        ImGui::TextColored(toVec4(p.warnInk), "Limiter -%.0f dB", -gr);
        ImGui::PopFont();
    }

    // Effects.
    char fx[48];
    std::snprintf(fx, sizeof(fx), "%s##busfx", b.chain.empty() ? "Add effect" : (std::to_string(b.chain.size()) + " effects").c_str());
    const bool busFx = !b.chain.empty() && !rt.fxBypass.load();
    if (pop::Pill(fx, busFx ? p.accentTint : p.surface, busFx ? p.accent : p.lineSoft, busFx ? p.accentInk : p.text2))
        openEffects(ChainTarget{true, b.id});

    if (ImGui::BeginPopup("##busmenu")) {
        ImGui::SetNextItemWidth(200 * sc);
        ImGui::InputText("Name", &b.name);
        ImVec4 c = toVec4(b.color);
        if (ImGui::ColorEdit3("Colour", &c.x, ImGuiColorEditFlags_NoInputs)) b.color = ImGui::ColorConvertFloat4ToU32(c);
        float ceil = rt.limiterCeilingDb.load();
        ImGui::SetNextItemWidth(200 * sc);
        if (ImGui::SliderFloat("Limiter ceiling", &ceil, -12.0f, 0.0f, "%.1f dBFS")) rt.limiterCeilingDb = ceil;
        bool excl = b.output.exclusive;
        if (ImGui::Checkbox("Exclusive mode (lower latency)", &excl)) {
            OutputRef o = b.output;
            o.exclusive = excl;
            const uint32_t id = b.id;
            defer([this, id, o]() { mc_->setBusOutput(id, o); });
        }
        Tip("Takes the device for Echo alone: lower latency, but other apps can't use it meanwhile.");
        bool clock = mc_->session().engine.clockBusId == b.id;
        if (ImGui::Checkbox("Use as timing source", &clock)) {
            mc_->session().engine.clockBusId = clock ? b.id : 0;
            mc_->rebuildGraph();
        }
        Tip("The device that paces Echo. Automatic: the first working headphones/speakers output.");
        ImGui::Separator();
        if (ImGui::MenuItem(ICON_DELETE "  Remove output")) {
            const uint32_t id = b.id;
            defer([this, id]() {
                mc_->removeBus(id);
                if (hasSelection_ && selected_.bus && selected_.id == id) {
                    hasSelection_ = false;
                    effectsOpen_ = false;
                }
            });
        }
        ImGui::EndPopup();
    }
    pop::EndCard();
    ImGui::PopID();
}

} // namespace echo::ui
