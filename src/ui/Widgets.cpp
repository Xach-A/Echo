#include "ui/Widgets.h"

#include <cmath>
#include <cstdio>

#include "imgui_internal.h"
#include "ui/Theme.h"

namespace echo::ui {

namespace {
bool g_tips = true;

struct TaperPoint {
    float pos, db;
};
const TaperPoint kTaper[] = {{0.0f, -120.0f}, {0.04f, -70.0f}, {0.12f, -50.0f}, {0.22f, -40.0f}, {0.32f, -30.0f},
                             {0.44f, -20.0f}, {0.58f, -10.0f}, {0.66f, -5.0f},  {0.75f, 0.0f},   {0.87f, 6.0f},
                             {1.0f, 12.0f}};
} // namespace

void setTooltipsEnabled(bool on) { g_tips = on; }

void Tip(const char* text) {
    if (g_tips && text && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal | ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("%s", text);
}

void HelpMarker(const char* text) {
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
        ImGui::BeginTooltip();
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 30.0f);
        ImGui::TextUnformatted(text);
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}

float faderPosToDb(float pos) {
    pos = clampv(pos, 0.0f, 1.0f);
    for (size_t i = 1; i < std::size(kTaper); ++i) {
        if (pos <= kTaper[i].pos) {
            const float t = (pos - kTaper[i - 1].pos) / (kTaper[i].pos - kTaper[i - 1].pos);
            return kTaper[i - 1].db + t * (kTaper[i].db - kTaper[i - 1].db);
        }
    }
    return kTaper[std::size(kTaper) - 1].db;
}

float faderDbToPos(float db) {
    if (db <= kTaper[0].db) return 0.0f;
    for (size_t i = 1; i < std::size(kTaper); ++i) {
        if (db <= kTaper[i].db) {
            const float t = (db - kTaper[i - 1].db) / (kTaper[i].db - kTaper[i - 1].db);
            return kTaper[i - 1].pos + t * (kTaper[i].pos - kTaper[i - 1].pos);
        }
    }
    return 1.0f;
}

bool Fader(const char* id, float* db, ImVec2 size, ImU32 color, float minDb, float maxDb) {
    ImGuiWindow* window = ImGui::GetCurrentWindow();
    if (window->SkipItems) return false;
    const Palette& p = pal();
    ImGui::PushID(id);
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##fader", size);
    const bool hovered = ImGui::IsItemHovered();
    const bool active = ImGui::IsItemActive();
    bool changed = false;
    ImGuiIO& io = ImGui::GetIO();
    const float knobH = std::max(14.0f, size.x * 0.55f);
    const float trackTop = pos.y + knobH * 0.5f, trackBot = pos.y + size.y - knobH * 0.5f;
    float v = *db;

    if (ImGui::IsItemClicked() && io.KeyCtrl) ImGui::OpenPopup("##entry");
    if (hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
        v = 0.0f;
        changed = true;
    } else if (active && !io.KeyCtrl && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 0.0f)) {
        const float fine = io.KeyShift ? 0.2f : 1.0f;
        float posN = faderDbToPos(v) - io.MouseDelta.y / (trackBot - trackTop) * fine;
        v = faderPosToDb(posN);
        changed = true;
    }
    if (hovered && io.MouseWheel != 0.0f) {
        v = (v <= -100.0f ? -60.0f : v) + io.MouseWheel * (io.KeyShift ? 0.1f : 0.5f);
        changed = true;
    }
    v = clampv(v, minDb, maxDb);
    if (v < -99.0f) v = -120.0f;

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float cx = pos.x + size.x * 0.5f;
    // Scale ticks.
    const float ticks[] = {12, 6, 0, -5, -10, -20, -30, -40, -60};
    for (float t : ticks) {
        const float y = trackBot - faderDbToPos(t) * (trackBot - trackTop);
        dl->AddLine(ImVec2(pos.x + 2, y), ImVec2(pos.x + size.x * 0.25f, y), withAlpha(p.textDim, t == 0 ? 0.9f : 0.4f));
        dl->AddLine(ImVec2(pos.x + size.x * 0.75f, y), ImVec2(pos.x + size.x - 2, y), withAlpha(p.textDim, t == 0 ? 0.9f : 0.4f));
    }
    // Track and fill.
    dl->AddRectFilled(ImVec2(cx - 2.5f, trackTop), ImVec2(cx + 2.5f, trackBot), p.meterBg, 2.5f);
    const float posN = faderDbToPos(v);
    const float ky = trackBot - posN * (trackBot - trackTop);
    dl->AddRectFilled(ImVec2(cx - 2.5f, ky), ImVec2(cx + 2.5f, trackBot), withAlpha(color, 0.75f), 2.5f);
    // Knob.
    const ImVec2 k0(pos.x + 3, ky - knobH * 0.5f), k1(pos.x + size.x - 3, ky + knobH * 0.5f);
    dl->AddRectFilled(ImVec2(k0.x + 1, k0.y + 2), ImVec2(k1.x + 1, k1.y + 2), IM_COL32(0, 0, 0, 90), 4.0f);
    dl->AddRectFilled(k0, k1, active ? p.frameHover : (hovered ? IM_COL32(70, 78, 96, 255) : IM_COL32(58, 65, 80, 255)), 4.0f);
    dl->AddRect(k0, k1, withAlpha(color, 0.9f), 4.0f, 1.5f);
    dl->AddLine(ImVec2(k0.x + 4, ky), ImVec2(k1.x - 4, ky), p.text, 2.0f);

    if (hovered || active) {
        char buf[32];
        if (v <= -100.0f) std::snprintf(buf, sizeof(buf), "-inf dB");
        else std::snprintf(buf, sizeof(buf), "%+.1f dB", v);
        ImGui::SetTooltip("%s\nDrag to adjust (Shift = fine)\nDouble-click: 0 dB, Ctrl+click: type", buf);
    }
    if (ImGui::BeginPopup("##entry")) {
        float tmp = v <= -100.0f ? -120.0f : v;
        ImGui::SetNextItemWidth(110);
        ImGui::SetKeyboardFocusHere();
        if (ImGui::InputFloat("dB", &tmp, 0.5f, 3.0f, "%.1f", ImGuiInputTextFlags_EnterReturnsTrue)) {
            v = clampv(tmp, minDb, maxDb);
            changed = true;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    ImGui::PopID();
    if (changed) *db = v;
    return changed;
}

static float meterPos(float db) {
    // -60..0 dB maps to 0..0.9, 0..+6 dB to 0.9..1.0
    if (db <= -60.0f) return 0.0f;
    if (db <= 0.0f) return (db + 60.0f) / 60.0f * 0.9f;
    return 0.9f + std::min(db, 6.0f) / 6.0f * 0.1f;
}

void LevelMeter(const char* id, MeterBallistics& m, ImVec2 size, bool stereo, bool showScale) {
    const Palette& p = pal();
    ImGui::PushID(id);
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##meter", size);
    if (ImGui::IsItemClicked()) m.clipLatched = false;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float ledH = 6.0f;
    const ImVec2 led0 = pos, led1(pos.x + size.x, pos.y + ledH);
    dl->AddRectFilled(led0, led1, m.clipLatched ? p.bad : withAlpha(p.bad, 0.15f), 2.0f);
    const float top = pos.y + ledH + 2.0f, bot = pos.y + size.y;
    const float h = bot - top;
    const int nch = stereo ? 2 : 1;
    const float gap = 2.0f;
    const float bw = (size.x - gap * (nch - 1)) / nch;
    for (int c = 0; c < nch; ++c) {
        const float x0 = pos.x + c * (bw + gap), x1 = x0 + bw;
        dl->AddRectFilled(ImVec2(x0, top), ImVec2(x1, bot), p.meterBg, 2.0f);
        const float level = meterPos(m.levelDb[c]);
        // Segmented gradient: green -> yellow (-12) -> red (-3)
        const float yG = bot - h * std::min(level, meterPos(-12.0f));
        dl->AddRectFilled(ImVec2(x0, yG), ImVec2(x1, bot), p.good);
        if (level > meterPos(-12.0f)) {
            const float yY = bot - h * std::min(level, meterPos(-3.0f));
            dl->AddRectFilled(ImVec2(x0, yY), ImVec2(x1, yG), p.warn);
            if (level > meterPos(-3.0f)) dl->AddRectFilled(ImVec2(x0, bot - h * level), ImVec2(x1, yY), p.bad);
        }
        // RMS line and peak hold.
        const float rmsY = bot - h * meterPos(m.rmsDb[c]);
        if (m.rmsDb[c] > -60.0f) dl->AddLine(ImVec2(x0, rmsY), ImVec2(x1, rmsY), IM_COL32(255, 255, 255, 70), 1.0f);
        if (m.holdDb[c] > -60.0f) {
            const float hy = bot - h * meterPos(m.holdDb[c]);
            dl->AddLine(ImVec2(x0, hy), ImVec2(x1, hy), m.holdDb[c] > -3.0f ? p.bad : p.text, 2.0f);
        }
    }
    if (showScale) {
        for (float d : {0.0f, -6.0f, -12.0f, -24.0f, -36.0f, -48.0f}) {
            const float y = bot - h * meterPos(d);
            dl->AddLine(ImVec2(pos.x - 3, y), ImVec2(pos.x, y), withAlpha(p.textDim, 0.6f));
        }
    }
    if (ImGui::IsItemHovered()) {
        if (stereo)
            ImGui::SetTooltip("Peak L %.1f / R %.1f dBFS\nHold L %.1f / R %.1f dBFS%s", m.levelDb[0], m.levelDb[1],
                              m.holdDb[0], m.holdDb[1], m.clipLatched ? "\nCLIPPED - click to clear" : "");
        else
            ImGui::SetTooltip("Peak %.1f dBFS\nHold %.1f dBFS%s", m.levelDb[0], m.holdDb[0],
                              m.clipLatched ? "\nCLIPPED - click to clear" : "");
    }
    ImGui::PopID();
}

void HLevelMeter(const char* id, MeterBallistics& m, ImVec2 size) {
    const Palette& p = pal();
    ImGui::PushID(id);
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##hm", size);
    if (ImGui::IsItemClicked()) m.clipLatched = false;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float gap = 2.0f, bh = (size.y - gap) * 0.5f;
    for (int c = 0; c < 2; ++c) {
        const float y0 = pos.y + c * (bh + gap), y1 = y0 + bh;
        dl->AddRectFilled(ImVec2(pos.x, y0), ImVec2(pos.x + size.x, y1), p.meterBg, 2.0f);
        const float f = meterPos(m.levelDb[c]);
        const float xg = pos.x + size.x * std::min(f, meterPos(-12.0f));
        dl->AddRectFilled(ImVec2(pos.x, y0), ImVec2(xg, y1), p.good);
        if (f > meterPos(-12.0f)) {
            const float xy = pos.x + size.x * std::min(f, meterPos(-3.0f));
            dl->AddRectFilled(ImVec2(xg, y0), ImVec2(xy, y1), p.warn);
            if (f > meterPos(-3.0f)) dl->AddRectFilled(ImVec2(xy, y0), ImVec2(pos.x + size.x * f, y1), p.bad);
        }
        if (m.holdDb[c] > -60.0f) {
            const float hx = pos.x + size.x * meterPos(m.holdDb[c]);
            dl->AddLine(ImVec2(hx, y0), ImVec2(hx, y1), p.text, 2.0f);
        }
    }
    for (float d : {-48.0f, -24.0f, -12.0f, -6.0f, 0.0f}) {
        const float x = pos.x + size.x * meterPos(d);
        char lbl[8];
        std::snprintf(lbl, sizeof(lbl), "%.0f", d);
        dl->AddText(ImVec2(x + 2, pos.y + size.y + 1), p.textDim, lbl);
    }
    ImGui::Dummy(ImVec2(size.x, ImGui::GetFontSize()));
    if (ImGui::IsItemHovered() || ImGui::IsItemActive())
        ImGui::SetTooltip("Peak %.1f / %.1f dBFS", m.levelDb[0], m.levelDb[1]);
    ImGui::PopID();
}

void MiniMeter(float levelDb, ImVec2 size) {
    const Palette& p = pal();
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    ImGui::Dummy(size);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(pos, ImVec2(pos.x + size.x, pos.y + size.y), p.meterBg, 2.0f);
    const float f = meterPos(levelDb);
    const ImU32 col = levelDb > -3.0f ? p.bad : levelDb > -12.0f ? p.warn : p.good;
    dl->AddRectFilled(pos, ImVec2(pos.x + size.x * f, pos.y + size.y), col, 2.0f);
}

bool Knob(const char* label, float* value, float minV, float maxV, float defV, const char* fmt, bool logScale,
          float diameter, ImU32 color) {
    const Palette& p = pal();
    if (diameter <= 0.0f) diameter = ImGui::GetFontSize() * 2.6f;
    if (!color) color = p.accent;
    ImGui::PushID(label);
    ImGui::BeginGroup();
    const float labelW = std::max(diameter, ImGui::CalcTextSize(label).x);
    const ImVec2 start = ImGui::GetCursorScreenPos();
    const ImVec2 pos(start.x + (labelW - diameter) * 0.5f, start.y);
    ImGui::SetCursorScreenPos(pos);
    ImGui::InvisibleButton("##knob", ImVec2(diameter, diameter));
    const bool hovered = ImGui::IsItemHovered(), active = ImGui::IsItemActive();
    ImGuiIO& io = ImGui::GetIO();
    auto toNorm = [&](float v) {
        if (logScale && minV > 0.0f) return std::log(v / minV) / std::log(maxV / minV);
        return (v - minV) / (maxV - minV);
    };
    auto fromNorm = [&](float n) {
        n = clampv(n, 0.0f, 1.0f);
        if (logScale && minV > 0.0f) return minV * std::pow(maxV / minV, n);
        return minV + n * (maxV - minV);
    };
    bool changed = false;
    float v = *value;
    if (hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
        v = defV;
        changed = true;
    } else if (active && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 0.0f)) {
        const float speed = (io.KeyShift ? 0.0008f : 0.005f);
        v = fromNorm(toNorm(v) - io.MouseDelta.y * speed + io.MouseDelta.x * speed * 0.5f);
        changed = true;
    }
    if (hovered && io.MouseWheel != 0.0f) {
        v = fromNorm(toNorm(v) + io.MouseWheel * (io.KeyShift ? 0.005f : 0.02f));
        changed = true;
    }
    v = clampv(v, minV, maxV);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 c(pos.x + diameter * 0.5f, pos.y + diameter * 0.5f);
    const float r = diameter * 0.5f - 2.0f;
    const float a0 = kPi * 0.75f, a1 = kPi * 2.25f;
    const float n = toNorm(v);
    dl->AddCircleFilled(c, r, hovered || active ? p.frameHover : p.frame, 32);
    dl->PathArcTo(c, r - 1.5f, a0, a1, 32);
    dl->PathStroke(p.meterBg, 3.0f);
    // Bipolar ranges draw from the centre.
    const bool bipolar = minV < 0.0f && maxV > 0.0f && !logScale;
    const float aStart = bipolar ? a0 + (a1 - a0) * toNorm(0.0f) : a0;
    const float aVal = a0 + (a1 - a0) * n;
    if (std::fabs(aVal - aStart) > 0.01f) {
        dl->PathArcTo(c, r - 1.5f, std::min(aStart, aVal), std::max(aStart, aVal), 32);
        dl->PathStroke(color, 3.0f);
    }
    const ImVec2 tip(c.x + std::cos(aVal) * (r - 5), c.y + std::sin(aVal) * (r - 5));
    dl->AddLine(c, tip, p.text, 2.0f);
    char buf[64];
    std::snprintf(buf, sizeof(buf), fmt, v);
    const ImVec2 ts = ImGui::CalcTextSize(buf);
    ImGui::SetCursorScreenPos(ImVec2(start.x + (labelW - ts.x) * 0.5f, pos.y + diameter + 1));
    ImGui::TextUnformatted(buf);
    const ImVec2 ls = ImGui::CalcTextSize(label);
    ImGui::SetCursorScreenPos(ImVec2(start.x + (labelW - ls.x) * 0.5f, ImGui::GetCursorScreenPos().y - 2));
    ImGui::TextColored(toVec4(p.textDim), "%s", label);
    ImGui::EndGroup();
    if (hovered && !active) ImGui::SetTooltip("%s: %s\nDrag or scroll (Shift = fine), double-click to reset", label, buf);
    ImGui::PopID();
    if (changed) *value = v;
    return changed;
}

std::string formatParam(const ParamInfo& p, float v) {
    char buf[64];
    if (p.scale == ParamScale::Enum && p.choices) {
        int i = static_cast<int>(std::lround(v)), k = 0;
        for (; p.choices[k]; ++k)
            if (k == i) return p.choices[k];
        return "?";
    }
    if (p.scale == ParamScale::Toggle) return v >= 0.5f ? "On" : "Off";
    const std::string unit = p.unit ? p.unit : "";
    if (unit == "Hz" && v >= 1000.0f) std::snprintf(buf, sizeof(buf), "%.2f kHz", v / 1000.0f);
    else if (p.scale == ParamScale::Int) std::snprintf(buf, sizeof(buf), "%d %s", static_cast<int>(std::lround(v)), unit.c_str());
    else if (std::fabs(v) >= 100.0f) std::snprintf(buf, sizeof(buf), "%.0f %s", v, unit.c_str());
    else if (std::fabs(v) >= 10.0f) std::snprintf(buf, sizeof(buf), "%.1f %s", v, unit.c_str());
    else std::snprintf(buf, sizeof(buf), "%.2f %s", v, unit.c_str());
    return buf;
}

bool ParamControl(Effect& fx, int index, float knobDiameter) {
    const ParamInfo& p = fx.paramInfo(index);
    float v = fx.param(index);
    bool changed = false;
    ImGui::PushID(index);
    if (p.scale == ParamScale::Toggle) {
        bool b = v >= 0.5f;
        if (ImGui::Checkbox(p.name, &b)) {
            fx.setParam(index, b ? 1.0f : 0.0f);
            changed = true;
        }
        if (p.help) Tip(p.help);
    } else if (p.scale == ParamScale::Enum && p.choices) {
        int cur = static_cast<int>(std::lround(v));
        ImGui::BeginGroup();
        ImGui::TextColored(toVec4(pal().textDim), "%s", p.name);
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 9.0f);
        if (ImGui::BeginCombo("##enum", formatParam(p, v).c_str())) {
            for (int k = 0; p.choices[k]; ++k) {
                if (ImGui::Selectable(p.choices[k], k == cur)) {
                    fx.setParam(index, static_cast<float>(k));
                    changed = true;
                }
            }
            ImGui::EndCombo();
        }
        ImGui::EndGroup();
        if (p.help) Tip(p.help);
    } else {
        const std::string text = formatParam(p, v);
        std::string fmt;
        for (char ch : text) fmt += ch == '%' ? std::string("%%") : std::string(1, ch);
        // The knob prints `fmt` via snprintf with the value; pass a constant
        // string so the formatted text is shown verbatim.
        float tmp = v;
        if (Knob(p.name, &tmp, p.min, p.max, p.def, fmt.c_str(), p.scale == ParamScale::Log, knobDiameter)) {
            fx.setParam(index, tmp);
            changed = true;
        }
        if (p.help) Tip(p.help);
    }
    ImGui::PopID();
    return changed;
}

bool ToggleButton(const char* label, bool* v, ImU32 onColor, ImVec2 size, const char* tooltip) {
    const Palette& p = pal();
    const bool on = *v;
    ImGui::PushStyleColor(ImGuiCol_Button, on ? onColor : p.frame);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, on ? withAlpha(onColor, 0.85f) : p.frameHover);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, on ? withAlpha(onColor, 0.7f) : p.frameHover);
    ImGui::PushStyleColor(ImGuiCol_Text, on ? IM_COL32(15, 17, 22, 255) : p.text);
    const bool clicked = ImGui::Button(label, size);
    ImGui::PopStyleColor(4);
    if (tooltip) Tip(tooltip);
    if (clicked) *v = !*v;
    return clicked;
}

void Led(bool on, ImU32 color, float radius) {
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    ImGui::Dummy(ImVec2(radius * 2, radius * 2));
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 c(pos.x + radius, pos.y + radius);
    dl->AddCircleFilled(c, radius, on ? color : withAlpha(color, 0.18f), 12);
}

void GroupHeader(const char* text, ImU32 color) {
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float h = ImGui::GetFontSize();
    dl->AddRectFilled(pos, ImVec2(pos.x + 3, pos.y + h), color, 1.5f);
    ImGui::SetCursorScreenPos(ImVec2(pos.x + 8, pos.y));
    ImGui::PushFont(fonts().semibold, 0.0f);
    ImGui::TextColored(toVec4(color), "%s", text);
    ImGui::PopFont();
}

void TextEllipsis(const std::string& s, float width, ImU32 color) {
    std::string t = s;
    if (ImGui::CalcTextSize(t.c_str()).x > width) {
        while (!t.empty() && ImGui::CalcTextSize((t + "...").c_str()).x > width) t.pop_back();
        t += "...";
    }
    if (color) ImGui::TextColored(toVec4(color), "%s", t.c_str());
    else ImGui::TextUnformatted(t.c_str());
    if (t != s && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", s.c_str());
}

} // namespace echo::ui
