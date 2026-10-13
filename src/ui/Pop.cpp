#include "ui/Pop.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <vector>

#include "imgui_internal.h"
#include "ui/Icons.h"
#include "ui/Theme.h"

namespace echo::ui::pop {

namespace {

std::vector<ImGuiID> g_cardIds;

// Applies the current style alpha (BeginDisabled) to a raw colour.
ImU32 A(ImU32 c) {
    const float a = ImGui::GetStyle().Alpha;
    if (a >= 1.0f) return c;
    const ImU32 al = static_cast<ImU32>(static_cast<float>((c >> 24) & 0xFF) * a);
    return (c & 0x00FFFFFFu) | (al << 24);
}

ImU32 mix(ImU32 a, ImU32 b, float t) {
    const ImVec4 x = ImGui::ColorConvertU32ToFloat4(a), y = ImGui::ColorConvertU32ToFloat4(b);
    return ImGui::ColorConvertFloat4ToU32(ImVec4(x.x + (y.x - x.x) * t, x.y + (y.y - x.y) * t, x.z + (y.z - x.z) * t,
                                                 x.w + (y.w - x.w) * t));
}

ImVec2 measure(ImFont* f, float size, const char* t, const char* end = nullptr, float wrap = 0.0f) {
    return f->CalcTextSizeA(size * S(), FLT_MAX, wrap, t, end);
}

void text(ImDrawList* dl, ImFont* f, float size, ImVec2 pos, ImU32 col, const char* t, const char* end = nullptr,
          float wrap = 0.0f) {
    dl->AddText(f, size * S(), pos, A(col), t, end, wrap);
}

const char* labelEnd(const char* label) { return ImGui::FindRenderedTextEnd(label); }

void handCursor() {
    if (ImGui::IsItemHovered()) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
}

} // namespace

float S() { return uiScale(); }

float meterFraction(float db) {
    if (!(db > -60.0f)) return 0.0f;
    return std::clamp((db + 60.0f) / 60.0f, 0.0f, 1.0f);
}

int columnsFor(float avail, float minWidth, float gap) {
    return std::max(1, static_cast<int>((avail + gap) / (minWidth + gap)));
}

// ---------------------------------------------------------------------------
// Text
// ---------------------------------------------------------------------------

void PageTitle(const char* title, const char* subtitle) {
    ImGui::PushFont(fonts().semibold, 30.0f);
    ImGui::TextUnformatted(title);
    ImGui::PopFont();
    if (subtitle) {
        ImGui::PushStyleColor(ImGuiCol_Text, toVec4(pal().text2));
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextUnformatted(subtitle);
        ImGui::PopTextWrapPos();
        ImGui::PopStyleColor();
    }
}

void SectionTitle(const char* t) {
    ImGui::PushFont(fonts().semibold, 19.0f);
    ImGui::TextUnformatted(t);
    ImGui::PopFont();
}

void CardTitle(const char* t) {
    ImGui::PushFont(fonts().semibold, 17.0f);
    ImGui::TextUnformatted(t);
    ImGui::PopFont();
}

void Label(const char* t) {
    ImGui::PushFont(fonts().regular, 14.0f);
    ImGui::TextUnformatted(t);
    ImGui::PopFont();
}

void TextColoredWrapped(ImU32 color, const char* t, float size) {
    ImGui::PushFont(fonts().regular, size > 0.0f ? size : 0.0f);
    ImGui::PushStyleColor(ImGuiCol_Text, toVec4(color));
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextUnformatted(t);
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();
    ImGui::PopFont();
}

void Caption(const char* t) { TextColoredWrapped(pal().text2, t, 13.0f); }
void Hint(const char* t) { TextColoredWrapped(pal().textDim, t, 12.0f); }

// ---------------------------------------------------------------------------
// Cards
// ---------------------------------------------------------------------------

void BeginCard(const char* id, float width, float height, bool quiet, float padX, float padY) {
    const Palette& p = pal();
    const float sc = S();
    // The shadow goes on the parent's list *before* the card: ImGui may draw
    // a child's background into its parent's list (render_decorations_in_parent),
    // so anything drawn there afterwards would cover the card. Its height is
    // the card's height from the previous frame.
    const ImGuiID sid = ImGui::GetID(id);
    ImGuiStorage* st = ImGui::GetStateStorage();
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const float avail = ImGui::GetContentRegionAvail().x;
    const float w = width > 0.0f ? width : avail + width;
    const float h = height > 0.0f ? height : st->GetFloat(sid, 0.0f);
    if (!quiet && h > 0.0f && w > 0.0f)
        ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(pos.x + 3 * sc, pos.y + 3 * sc), ImVec2(pos.x + w + 3 * sc, pos.y + h + 3 * sc),
                                                  A(p.shadow), 16.0f * sc);
    ImGui::PushStyleColor(ImGuiCol_ChildBg, toVec4(p.surface));
    ImGui::PushStyleColor(ImGuiCol_Border, toVec4(quiet ? p.lineSoft : p.outline));
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 16.0f * sc);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, 1.5f * sc);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(padX * sc, padY * sc));
    ImGuiChildFlags flags = ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding;
    if (height <= 0.0f) flags |= ImGuiChildFlags_AutoResizeY;
    ImGui::BeginChild(id, ImVec2(width, height > 0.0f ? height : 0.0f), flags, ImGuiWindowFlags_NoScrollbar);
    g_cardIds.push_back(sid);
}

void EndCard() {
    const ImGuiID sid = g_cardIds.empty() ? 0 : g_cardIds.back();
    if (!g_cardIds.empty()) g_cardIds.pop_back();
    ImGui::EndChild();
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor(2);
    if (sid) ImGui::GetStateStorage()->SetFloat(sid, ImGui::GetItemRectSize().y);
}

void Divider(float above, float below) {
    const float sc = S();
    if (above > 0) ImGui::Dummy(ImVec2(0, above * sc - ImGui::GetStyle().ItemSpacing.y));
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const float w = ImGui::GetContentRegionAvail().x;
    ImGui::GetWindowDrawList()->AddLine(p0, ImVec2(p0.x + w, p0.y), A(pal().line), 1.5f * sc);
    ImGui::Dummy(ImVec2(w, std::max(0.0f, below * sc - ImGui::GetStyle().ItemSpacing.y)));
}

void Banner(const char* icon, const char* t, ImU32 bg, ImU32 iconColor) {
    const float sc = S();
    const Palette& p = pal();
    const float w = ImGui::GetContentRegionAvail().x;
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const float textX = 44.0f * sc, padY = 13.0f * sc;
    const float wrap = w - textX - 16.0f * sc;
    const ImVec2 ts = measure(fonts().regular, 14.0f, t, nullptr, wrap);
    const float h = std::max(ts.y, 20.0f * sc) + padY * 2;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(pos, ImVec2(pos.x + w, pos.y + h), A(bg), 14.0f * sc);
    text(dl, fonts().regular, 18.0f, ImVec2(pos.x + 16 * sc, pos.y + padY - 1 * sc), iconColor, icon);
    text(dl, fonts().regular, 14.0f, ImVec2(pos.x + textX, pos.y + padY), p.text, t, nullptr, wrap);
    ImGui::Dummy(ImVec2(w, h));
}

// ---------------------------------------------------------------------------
// Buttons
// ---------------------------------------------------------------------------

namespace {
bool buttonImpl(const char* label, Btn kind, ImVec2 size, float height, bool forceHeld, bool* held) {
    const float sc = S();
    const Palette& p = pal();
    ImFont* f = fonts().semibold;
    const float fs = height < 36.0f * sc ? 13.0f : 14.0f;
    const char* end = labelEnd(label);
    const ImVec2 ts = measure(f, fs, label, end);
    const float h = size.y > 0 ? size.y : height;
    const float w = size.x > 0 ? size.x : size.x < 0 ? ImGui::GetContentRegionAvail().x : ts.x + (kind == Btn::Quiet ? 16 : 32) * sc;
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const bool clicked = ImGui::InvisibleButton(label, ImVec2(w, h));
    const bool hov = ImGui::IsItemHovered(), act = ImGui::IsItemActive();
    if (held) *held = act;
    handCursor();
    if (forceHeld && act) kind = Btn::Primary;
    ImU32 fill = 0, border = 0, fg = p.text;
    switch (kind) {
    case Btn::Primary:
        fill = hov ? mix(p.primary, p.surface, 0.14f) : p.primary;
        border = p.primary;
        fg = p.onPrimary;
        break;
    case Btn::Secondary:
        fill = act ? p.segBg : hov ? p.frameHover : p.surface;
        border = p.outline;
        break;
    case Btn::Accent:
        fill = hov ? mix(p.accent, IM_COL32(255, 255, 255, 255), 0.12f) : p.accent;
        border = p.outline;
        fg = p.onAccent;
        break;
    case Btn::Quiet:
        fill = hov ? p.segBg : 0;
        fg = p.link;
        break;
    case Btn::Danger:
        fill = hov ? p.badTint : p.surface;
        border = p.bad;
        fg = p.badInk;
        break;
    }
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float r = std::min(12.0f * sc, h * 0.5f);
    if (fill) dl->AddRectFilled(pos, ImVec2(pos.x + w, pos.y + h), A(fill), r);
    if (border) dl->AddRect(pos, ImVec2(pos.x + w, pos.y + h), A(border), r, 1.5f * sc);
    text(dl, f, fs, ImVec2(pos.x + (w - ts.x) * 0.5f, pos.y + (h - ts.y) * 0.5f), fg, label, end);
    return clicked;
}
} // namespace

bool Button(const char* label, Btn kind, ImVec2 size) { return buttonImpl(label, kind, size, 40.0f * S(), false, nullptr); }

bool SmallButton(const char* label, Btn kind) { return buttonImpl(label, kind, ImVec2(0, 0), 32.0f * S(), false, nullptr); }

bool LinkButton(const char* label) {
    const Palette& p = pal();
    const char* end = labelEnd(label);
    const ImVec2 ts = measure(fonts().regular, 14.0f, label, end);
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const bool clicked = ImGui::InvisibleButton(label, ts);
    const bool hov = ImGui::IsItemHovered();
    handCursor();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    text(dl, fonts().regular, 14.0f, pos, p.link, label, end);
    if (hov) dl->AddLine(ImVec2(pos.x, pos.y + ts.y), ImVec2(pos.x + ts.x, pos.y + ts.y), A(p.link), 1.0f * S());
    return clicked;
}

bool HoldButton(const char* label, const char* heldLabel, ImVec2 size) {
    const float sc = S();
    ImGui::PushID(label);
    // Measure with the longer text so the button does not change size.
    const float w = size.x != 0 ? size.x
                                : std::max(measure(fonts().semibold, 14.0f, label, labelEnd(label)).x,
                                           measure(fonts().semibold, 14.0f, heldLabel, labelEnd(heldLabel)).x) + 32 * sc;
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const float h = size.y > 0 ? size.y : 40.0f * sc;
    // A fixed ID, so the button stays pressed while its text changes.
    ImGui::InvisibleButton("##hold", ImVec2(w, h));
    const bool held = ImGui::IsItemActive();
    const bool hov = ImGui::IsItemHovered();
    handCursor();
    const Palette& p = pal();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImU32 fill = held ? p.primary : hov ? p.frameHover : p.surface;
    dl->AddRectFilled(pos, ImVec2(pos.x + w, pos.y + h), A(fill), 12.0f * sc);
    dl->AddRect(pos, ImVec2(pos.x + w, pos.y + h), A(held ? p.primary : p.outline), 12.0f * sc, 1.5f * sc);
    const char* t = held ? heldLabel : label;
    const ImVec2 ts = measure(fonts().semibold, 14.0f, t, labelEnd(t));
    text(dl, fonts().semibold, 14.0f, ImVec2(pos.x + (w - ts.x) * 0.5f, pos.y + (h - ts.y) * 0.5f), held ? p.onPrimary : p.text,
         t, labelEnd(t));
    ImGui::PopID();
    return held;
}

// ---------------------------------------------------------------------------
// Toggles and choices
// ---------------------------------------------------------------------------

bool Switch(const char* id, bool* v) {
    const float sc = S();
    const Palette& p = pal();
    const ImVec2 size(46.0f * sc, 28.0f * sc);
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const bool clicked = ImGui::InvisibleButton(id, size);
    if (clicked) *v = !*v;
    handCursor();
    const ImGuiID sid = ImGui::GetItemID();
    ImGuiStorage* st = ImGui::GetStateStorage();
    const float target = *v ? 1.0f : 0.0f;
    float t = st->GetFloat(sid, target);
    t += (target - t) * std::min(1.0f, ImGui::GetIO().DeltaTime * 18.0f);
    if (std::fabs(target - t) < 0.01f) t = target;
    st->SetFloat(sid, t);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 end(pos.x + size.x, pos.y + size.y);
    dl->AddRectFilled(pos, end, A(mix(p.track, p.accent, t)), size.y * 0.5f);
    dl->AddRect(pos, end, A(p.outline), size.y * 0.5f, 1.5f * sc);
    const float r = 9.5f * sc;
    const float x = pos.x + 3.0f * sc + r + t * (size.x - 6.0f * sc - 2.0f * r);
    const ImVec2 c(x, pos.y + size.y * 0.5f);
    dl->AddCircleFilled(c, r, A(IM_COL32(255, 255, 255, 255)), 24);
    dl->AddCircle(c, r, A(p.outline), 24, 1.5f * sc);
    return clicked;
}

bool SwitchRow(const char* title, const char* desc, bool* v, bool divider) {
    const float sc = S();
    const Palette& p = pal();
    const float w = ImGui::GetContentRegionAvail().x;
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    ImGui::PushID(title);
    ImGui::SetCursorScreenPos(ImVec2(p0.x, p0.y + 12.0f * sc));
    ImGui::BeginGroup();
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + w - 64.0f * sc);
    ImGui::PushFont(fonts().semibold, 15.0f);
    ImGui::TextUnformatted(title);
    ImGui::PopFont();
    if (desc && *desc) {
        ImGui::PushFont(fonts().regular, 13.0f);
        ImGui::PushStyleColor(ImGuiCol_Text, toVec4(p.text2));
        ImGui::TextUnformatted(desc);
        ImGui::PopStyleColor();
        ImGui::PopFont();
    }
    ImGui::PopTextWrapPos();
    ImGui::EndGroup();
    const ImVec2 gmin = ImGui::GetItemRectMin(), gmax = ImGui::GetItemRectMax();
    const float cy = (gmin.y + gmax.y) * 0.5f;
    ImGui::SetCursorScreenPos(ImVec2(p0.x + w - 46.0f * sc, cy - 14.0f * sc));
    const bool changed = Switch("##sw", v);
    const float bottom = std::max(gmax.y, cy + 14.0f * sc) + 12.0f * sc;
    if (divider) ImGui::GetWindowDrawList()->AddLine(ImVec2(p0.x, bottom), ImVec2(p0.x + w, bottom), A(p.line), 1.5f * sc);
    ImGui::SetCursorScreenPos(ImVec2(p0.x, bottom + 1.5f * sc));
    ImGui::Dummy(ImVec2(w, 0.0f));
    ImGui::PopID();
    return changed;
}

bool Segmented(const char* id, int* current, const char* const* labels, int count, float width, const char* const* subs) {
    const float sc = S();
    const Palette& p = pal();
    ImGui::PushID(id);
    ImGui::BeginGroup();
    const float h = (subs ? 46.0f : 36.0f) * sc;
    const float inset = 3.0f * sc;
    float segW = 0.0f;
    if (width > 0.0f) {
        segW = (width - inset * 2) / count;
    } else {
        for (int i = 0; i < count; ++i) {
            segW = std::max(segW, measure(fonts().regular, 13.5f, labels[i]).x + 28.0f * sc);
            if (subs && subs[i]) segW = std::max(segW, measure(fonts().regular, 11.0f, subs[i]).x + 24.0f * sc);
        }
    }
    const float w = segW * count + inset * 2;
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(pos, ImVec2(pos.x + w, pos.y + h), A(p.segBg), 12.0f * sc);
    bool changed = false;
    for (int i = 0; i < count; ++i) {
        const ImVec2 a(pos.x + inset + segW * i, pos.y + inset), b(a.x + segW, pos.y + h - inset);
        ImGui::SetCursorScreenPos(a);
        ImGui::PushID(i);
        if (ImGui::InvisibleButton("##seg", ImVec2(b.x - a.x, b.y - a.y)) && *current != i) {
            *current = i;
            changed = true;
        }
        handCursor();
        const bool hov = ImGui::IsItemHovered();
        ImGui::PopID();
        const bool sel = *current == i;
        if (sel) {
            dl->AddRectFilled(a, b, A(p.surface), 9.0f * sc);
            dl->AddRect(a, b, A(p.outline), 9.0f * sc, 1.5f * sc);
        } else if (hov) {
            dl->AddRectFilled(a, b, A(withAlpha(p.surface, 0.55f)), 9.0f * sc);
        }
        const ImVec2 ls = measure(fonts().regular, 13.5f, labels[i]);
        const bool hasSub = subs && subs[i] && *subs[i];
        const ImVec2 ss = hasSub ? measure(fonts().regular, 11.0f, subs[i]) : ImVec2(0, 0);
        const float total = ls.y + (hasSub ? ss.y : 0.0f);
        const float ty = a.y + ((b.y - a.y) - total) * 0.5f;
        text(dl, fonts().regular, 13.5f, ImVec2(a.x + (segW - ls.x) * 0.5f, ty), sel ? p.text : p.text2, labels[i]);
        if (hasSub) text(dl, fonts().regular, 11.0f, ImVec2(a.x + (segW - ss.x) * 0.5f, ty + ls.y), p.textDim, subs[i]);
    }
    ImGui::SetCursorScreenPos(pos);
    ImGui::Dummy(ImVec2(w, h));
    ImGui::EndGroup();
    ImGui::PopID();
    return changed;
}

bool Chip(const char* label, bool on) {
    const float sc = S();
    const Palette& p = pal();
    const char* end = labelEnd(label);
    const ImVec2 ts = measure(fonts().regular, 13.0f, label, end);
    const ImVec2 size(ts.x + 28.0f * sc, 32.0f * sc);
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const bool clicked = ImGui::InvisibleButton(label, size);
    const bool hov = ImGui::IsItemHovered();
    handCursor();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 b(pos.x + size.x, pos.y + size.y);
    dl->AddRectFilled(pos, b, A(on ? p.primary : hov ? p.frameHover : p.surface), size.y * 0.5f);
    if (!on) dl->AddRect(pos, b, A(p.border), size.y * 0.5f, 1.5f * sc);
    text(dl, fonts().regular, 13.0f, ImVec2(pos.x + 14.0f * sc, pos.y + (size.y - ts.y) * 0.5f), on ? p.onPrimary : p.text, label, end);
    return clicked;
}

bool Pill(const char* label, ImU32 fill, ImU32 border, ImU32 fg, float width) {
    const float sc = S();
    const char* end = labelEnd(label);
    const ImVec2 ts = measure(fonts().regular, 13.0f, label, end);
    const ImVec2 size(width > 0 ? width : ts.x + 24.0f * sc, 32.0f * sc);
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const bool clicked = ImGui::InvisibleButton(label, size);
    const bool hov = ImGui::IsItemHovered();
    handCursor();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 b(pos.x + size.x, pos.y + size.y);
    dl->AddRectFilled(pos, b, A(hov ? mix(fill, pal().text, 0.04f) : fill), 10.0f * sc);
    dl->AddRect(pos, b, A(border), 10.0f * sc, 1.5f * sc);
    dl->PushClipRect(pos, b, true);
    text(dl, fonts().regular, 13.0f, ImVec2(pos.x + 10.0f * sc, pos.y + (size.y - ts.y) * 0.5f), fg, label, end);
    dl->PopClipRect();
    return clicked;
}

bool SendPill(const char* label, bool on, bool busB, float width, bool blocked) {
    const float sc = S();
    const Palette& p = pal();
    const char* end = labelEnd(label);
    const ImVec2 ts = measure(fonts().regular, 13.0f, label, end);
    const ImVec2 size(width > 0 ? width : ts.x + 24.0f * sc, 32.0f * sc);
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const bool clicked = ImGui::InvisibleButton(label, size);
    const bool hov = ImGui::IsItemHovered();
    handCursor();
    ImU32 fill, border, fg;
    if (blocked) {
        fill = p.badTint, border = p.bad, fg = p.badInk;
    } else if (on) {
        fill = busB ? p.busBTint : p.busATint;
        border = busB ? p.busVirtual : p.busPhysical;
        fg = busB ? p.busBInk : p.busAInk;
    } else {
        fill = hov ? p.frameHover : p.surface, border = p.lineSoft, fg = p.textDim;
    }
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 b(pos.x + size.x, pos.y + size.y);
    dl->AddRectFilled(pos, b, A(fill), 10.0f * sc);
    dl->AddRect(pos, b, A(border), 10.0f * sc, 1.5f * sc);
    dl->PushClipRect(pos, b, true);
    text(dl, fonts().regular, 13.0f, ImVec2(pos.x + 10.0f * sc, pos.y + (size.y - ts.y) * 0.5f), fg, label, end);
    dl->PopClipRect();
    return clicked;
}

float TileHeight(const char* icon, const char* name, const char* sub, float width) {
    const float sc = S();
    const float pad = 14.0f * sc, wrap = width - pad * 2;
    float h = 12.0f * sc + 12.0f * sc;
    if (icon && *icon) h += 28.0f * sc;
    h += measure(fonts().semibold, 14.0f, name, nullptr, wrap).y + 2.0f * sc;
    if (sub && *sub) h += measure(fonts().regular, 12.0f, sub, nullptr, wrap).y;
    return h;
}

bool Tile(const char* id, const char* icon, const char* name, const char* sub, const char* corner, bool selected, ImVec2 size) {
    const float sc = S();
    const Palette& p = pal();
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const bool clicked = ImGui::InvisibleButton(id, size);
    const bool hov = ImGui::IsItemHovered();
    handCursor();
    const ImVec2 b(pos.x + size.x, pos.y + size.y);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(pos, b, A(selected ? p.accentTint : hov ? p.surfaceAlt : p.surface), 14.0f * sc);
    dl->AddRect(pos, b, A(selected ? p.accent : hov ? p.border : p.lineSoft), 14.0f * sc, 1.5f * sc);
    dl->PushClipRect(pos, b, true);
    const float pad = 14.0f * sc;
    float y = pos.y + 12.0f * sc;
    if (icon && *icon) {
        text(dl, fonts().regular, 20.0f, ImVec2(pos.x + pad, y), p.text, icon);
        y += 28.0f * sc;
    }
    if (corner && *corner) {
        const ImVec2 cs = measure(fonts().regular, 11.0f, corner);
        text(dl, fonts().regular, 11.0f, ImVec2(b.x - pad - cs.x, pos.y + 14.0f * sc), p.textDim, corner);
    }
    text(dl, fonts().semibold, 14.0f, ImVec2(pos.x + pad, y), p.text, name, nullptr, size.x - pad * 2);
    y += measure(fonts().semibold, 14.0f, name, nullptr, size.x - pad * 2).y + 2.0f * sc;
    if (sub && *sub) text(dl, fonts().regular, 12.0f, ImVec2(pos.x + pad, y), p.text2, sub, nullptr, size.x - pad * 2);
    dl->PopClipRect();
    return clicked;
}

PadEvents SoundPadTile(const char* id, const char* icon, const char* name, const char* keyText, const char* modeText,
                       ImU32 tint, float progress, bool warn, ImVec2 size) {
    const float sc = S();
    const Palette& p = pal();
    PadEvents ev;
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    ImGui::PushID(id);
    ImGui::SetNextItemAllowOverlap();
    ImGui::InvisibleButton("##pad", size, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
    const bool hov = ImGui::IsItemHovered();
    const bool held = ImGui::IsItemActive() && ImGui::IsMouseDown(ImGuiMouseButton_Left);
    if (ImGui::IsItemActivated() && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) ev.pressed = true;
    if (ImGui::IsItemDeactivated()) ev.released = true;
    if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) ev.edit = true;
    handCursor();
    const ImVec2 b(pos.x + size.x, pos.y + size.y);
    const bool playing = progress >= 0.0f;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float r = 16.0f * sc;
    const bool dark = themeIsDark();
    const ImU32 fill = mix(p.surface, tint, held ? (dark ? 0.55f : 0.45f) : playing ? (dark ? 0.42f : 0.32f)
                                                                            : hov ? (dark ? 0.32f : 0.24f) : (dark ? 0.22f : 0.16f));
    dl->AddRectFilled(ImVec2(pos.x, pos.y + 3.0f * sc), ImVec2(b.x, b.y + 3.0f * sc), A(p.shadow), r);
    dl->AddRectFilled(ImVec2(pos.x, pos.y + (held ? 2.0f * sc : 0.0f)), ImVec2(b.x, b.y + (held ? 2.0f * sc : 0.0f)), A(fill), r);
    dl->AddRect(ImVec2(pos.x, pos.y + (held ? 2.0f * sc : 0.0f)), ImVec2(b.x, b.y + (held ? 2.0f * sc : 0.0f)),
                A(playing ? p.outline : hov ? p.border : p.outline), r, (playing ? 2.5f : 1.5f) * sc);
    dl->PushClipRect(pos, ImVec2(b.x, b.y + 3.0f * sc), true);
    const float pad = 14.0f * sc;
    const float dy = held ? 2.0f * sc : 0.0f;
    // Icon in a round badge of the pad's colour.
    const ImVec2 ic(pos.x + pad + 15.0f * sc, pos.y + dy + pad + 15.0f * sc);
    dl->AddCircleFilled(ic, 15.0f * sc, A(mix(tint, p.surface, dark ? 0.25f : 0.0f)), 24);
    dl->AddCircle(ic, 15.0f * sc, A(p.outline), 24, 1.5f * sc);
    if (icon && *icon) {
        const char* glyph = warn ? ICON_WARNING : playing ? ICON_STOP : icon;
        const ImVec2 is = measure(fonts().regular, 15.0f, glyph);
        text(dl, fonts().regular, 15.0f, ImVec2(ic.x - is.x * 0.5f, ic.y - is.y * 0.5f), p.onAccent, glyph);
    }
    // Name (up to three lines).
    const float wrap = size.x - pad * 2;
    text(dl, fonts().semibold, 15.0f, ImVec2(pos.x + pad, pos.y + dy + pad + 38.0f * sc), p.text, name, nullptr, wrap);
    // Key cap and mode along the bottom.
    float by = b.y + dy - pad - 22.0f * sc;
    if (playing) by -= 8.0f * sc;
    if (keyText && *keyText) {
        const ImVec2 ks = measure(fonts().semibold, 11.0f, keyText);
        const float kw = std::min(ks.x + 14.0f * sc, size.x - pad * 2);
        const ImVec2 k0(pos.x + pad, by), k1(pos.x + pad + kw, by + 22.0f * sc);
        dl->AddRectFilled(k0, k1, A(p.surface), 7.0f * sc);
        dl->AddRect(k0, k1, A(p.outline), 7.0f * sc, 1.2f * sc);
        dl->PushClipRect(k0, k1, true);
        text(dl, fonts().semibold, 11.0f, ImVec2(k0.x + 7.0f * sc, k0.y + (22.0f * sc - ks.y) * 0.5f), p.text, keyText);
        dl->PopClipRect();
    }
    if (modeText && *modeText) {
        const ImVec2 ms = measure(fonts().regular, 11.0f, modeText);
        text(dl, fonts().regular, 11.0f, ImVec2(b.x - pad - ms.x, by + (22.0f * sc - ms.y) * 0.5f), p.text2, modeText);
    }
    // Play position.
    if (playing) {
        const ImVec2 t0(pos.x + pad, b.y + dy - pad - 4.0f * sc), t1(b.x - pad, b.y + dy - pad + 2.0f * sc);
        dl->AddRectFilled(t0, t1, A(withAlpha(p.text, 0.12f)), 3.0f * sc);
        dl->AddRectFilled(t0, ImVec2(t0.x + (t1.x - t0.x) * std::clamp(progress, 0.0f, 1.0f), t1.y), A(p.text), 3.0f * sc);
    }
    dl->PopClipRect();
    // Edit button in the corner (shown on hover).
    const ImVec2 eb(b.x - 30.0f * sc - 6.0f * sc, pos.y + 8.0f * sc + dy);
    ImGui::SetCursorScreenPos(eb);
    if (ImGui::InvisibleButton("##edit", ImVec2(30.0f * sc, 30.0f * sc))) ev.edit = true;
    const bool editHov = ImGui::IsItemHovered();
    if (editHov) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    if (hov || editHov) {
        const ImVec2 ec(eb.x + 15.0f * sc, eb.y + 15.0f * sc);
        dl->AddCircleFilled(ec, 13.0f * sc, A(editHov ? p.surfaceAlt : p.surface), 20);
        dl->AddCircle(ec, 13.0f * sc, A(p.outline), 20, 1.2f * sc);
        const ImVec2 es = measure(fonts().regular, 13.0f, ICON_MORE);
        text(dl, fonts().regular, 13.0f, ImVec2(ec.x - es.x * 0.5f, ec.y - es.y * 0.5f), p.text, ICON_MORE);
    }
    ImGui::SetCursorScreenPos(pos);
    ImGui::Dummy(size);
    ImGui::PopID();
    return ev;
}

bool AddTile(const char* id, const char* label, ImVec2 size) {
    const float sc = S();
    const Palette& p = pal();
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const bool clicked = ImGui::InvisibleButton(id, size);
    const bool hov = ImGui::IsItemHovered();
    handCursor();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 b(pos.x + size.x, pos.y + size.y);
    if (hov) dl->AddRectFilled(pos, b, A(p.surfaceAlt), 16.0f * sc);
    // Dashed outline.
    const float dash = 7.0f * sc, gapLen = 5.0f * sc, inset = 1.0f * sc;
    const ImU32 col = A(hov ? p.outline : p.border);
    auto dashed = [&](ImVec2 a, ImVec2 c) {
        const float len = std::hypot(c.x - a.x, c.y - a.y);
        const ImVec2 d((c.x - a.x) / len, (c.y - a.y) / len);
        for (float t = 0.0f; t < len; t += dash + gapLen) {
            const float e = std::min(len, t + dash);
            dl->AddLine(ImVec2(a.x + d.x * t, a.y + d.y * t), ImVec2(a.x + d.x * e, a.y + d.y * e), col, 1.5f * sc);
        }
    };
    const float rr = 16.0f * sc;
    dashed(ImVec2(pos.x + rr, pos.y + inset), ImVec2(b.x - rr, pos.y + inset));
    dashed(ImVec2(pos.x + rr, b.y - inset), ImVec2(b.x - rr, b.y - inset));
    dashed(ImVec2(pos.x + inset, pos.y + rr), ImVec2(pos.x + inset, b.y - rr));
    dashed(ImVec2(b.x - inset, pos.y + rr), ImVec2(b.x - inset, b.y - rr));
    dl->PathArcTo(ImVec2(pos.x + rr, pos.y + rr), rr - inset, IM_PI, IM_PI * 1.5f);
    dl->PathStroke(col, 1.5f * sc);
    dl->PathArcTo(ImVec2(b.x - rr, pos.y + rr), rr - inset, IM_PI * 1.5f, IM_PI * 2.0f);
    dl->PathStroke(col, 1.5f * sc);
    dl->PathArcTo(ImVec2(b.x - rr, b.y - rr), rr - inset, 0.0f, IM_PI * 0.5f);
    dl->PathStroke(col, 1.5f * sc);
    dl->PathArcTo(ImVec2(pos.x + rr, b.y - rr), rr - inset, IM_PI * 0.5f, IM_PI);
    dl->PathStroke(col, 1.5f * sc);
    const ImVec2 is = measure(fonts().regular, 22.0f, ICON_ADD);
    const ImVec2 ls = measure(fonts().semibold, 14.0f, label, nullptr, size.x - 24.0f * sc);
    const float total = is.y + 8.0f * sc + ls.y;
    const float y0 = pos.y + (size.y - total) * 0.5f;
    text(dl, fonts().regular, 22.0f, ImVec2(pos.x + (size.x - is.x) * 0.5f, y0), p.text2, ICON_ADD);
    text(dl, fonts().semibold, 14.0f, ImVec2(pos.x + (size.x - ls.x) * 0.5f, y0 + is.y + 8.0f * sc), p.text2, label, nullptr,
         size.x - 24.0f * sc);
    return clicked;
}

bool RadioRow(const char* id, const char* title, const char* sub, bool selected, float width) {
    const float sc = S();
    const Palette& p = pal();
    const bool hasSub = sub && *sub;
    const float w = width > 0 ? width : ImGui::GetContentRegionAvail().x;
    const float h = (hasSub ? 58.0f : 46.0f) * sc;
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const bool clicked = ImGui::InvisibleButton(id, ImVec2(w, h));
    const bool hov = ImGui::IsItemHovered();
    handCursor();
    const ImVec2 b(pos.x + w, pos.y + h);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(pos, b, A(selected ? p.accentTint : hov ? p.surfaceAlt : p.surface), 12.0f * sc);
    dl->AddRect(pos, b, A(selected ? p.outline : p.lineSoft), 12.0f * sc, 1.5f * sc);
    const ImVec2 c(pos.x + 26.0f * sc, pos.y + h * 0.5f);
    dl->AddCircle(c, 10.0f * sc, A(p.outline), 24, 1.5f * sc);
    if (selected) dl->AddCircleFilled(c, 5.0f * sc, A(p.primary), 16);
    const float tx = pos.x + 48.0f * sc;
    dl->PushClipRect(pos, b, true);
    const ImVec2 ts = measure(fonts().semibold, 15.0f, title);
    const ImVec2 ss = hasSub ? measure(fonts().regular, 12.0f, sub) : ImVec2(0, 0);
    const float ty = pos.y + (h - ts.y - ss.y) * 0.5f;
    text(dl, fonts().semibold, 15.0f, ImVec2(tx, ty), p.text, title);
    if (hasSub) text(dl, fonts().regular, 12.0f, ImVec2(tx, ty + ts.y), p.text2, sub);
    dl->PopClipRect();
    return clicked;
}

bool CheckRow(const char* id, const char* title, const char* sub, bool* v, float width) {
    const float sc = S();
    const Palette& p = pal();
    const bool hasSub = sub && *sub;
    const float w = width > 0 ? width : ImGui::GetContentRegionAvail().x;
    const float h = (hasSub ? 58.0f : 44.0f) * sc;
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const bool clicked = ImGui::InvisibleButton(id, ImVec2(w, h));
    if (clicked) *v = !*v;
    const bool hov = ImGui::IsItemHovered();
    handCursor();
    const ImVec2 b(pos.x + w, pos.y + h);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(pos, b, A(hov ? p.surfaceAlt : p.surface), 12.0f * sc);
    dl->AddRect(pos, b, A(*v ? p.outline : p.lineSoft), 12.0f * sc, 1.5f * sc);
    const float bs = 18.0f * sc;
    const ImVec2 b0(pos.x + 16.0f * sc, pos.y + (h - bs) * 0.5f), b1(b0.x + bs, b0.y + bs);
    if (*v) {
        dl->AddRectFilled(b0, b1, A(p.accent), 5.0f * sc);
        const ImVec2 pts[3] = {ImVec2(b0.x + bs * 0.22f, b0.y + bs * 0.52f), ImVec2(b0.x + bs * 0.42f, b0.y + bs * 0.72f),
                               ImVec2(b0.x + bs * 0.78f, b0.y + bs * 0.30f)};
        dl->AddPolyline(pts, 3, A(p.onAccent), 2.0f * sc);
    } else {
        dl->AddRectFilled(b0, b1, A(p.surface), 5.0f * sc);
    }
    dl->AddRect(b0, b1, A(p.outline), 5.0f * sc, 1.5f * sc);
    const float tx = pos.x + 46.0f * sc;
    dl->PushClipRect(pos, b, true);
    const ImVec2 ts = measure(fonts().semibold, 14.5f, title);
    const ImVec2 ss = hasSub ? measure(fonts().regular, 12.0f, sub) : ImVec2(0, 0);
    const float ty = pos.y + (h - ts.y - ss.y) * 0.5f;
    text(dl, fonts().semibold, 14.5f, ImVec2(tx, ty), p.text, title);
    if (hasSub) text(dl, fonts().regular, 12.0f, ImVec2(tx, ty + ts.y), p.text2, sub);
    dl->PopClipRect();
    return clicked;
}

bool KeyCap(const char* label, bool capturing, float minWidth) {
    const float sc = S();
    const Palette& p = pal();
    const char* end = labelEnd(label);
    const ImVec2 ts = measure(fonts().semibold, 14.0f, label, end);
    const ImVec2 size(std::max(minWidth * sc, ts.x + 28.0f * sc), 38.0f * sc);
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const bool clicked = ImGui::InvisibleButton(label, ImVec2(size.x, size.y + 2.0f * sc));
    const bool hov = ImGui::IsItemHovered();
    handCursor();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 b(pos.x + size.x, pos.y + size.y);
    dl->AddRectFilled(ImVec2(pos.x, pos.y + 2.0f * sc), ImVec2(b.x, b.y + 2.0f * sc), A(p.shadow), 10.0f * sc);
    dl->AddRectFilled(pos, b, A(capturing ? p.accentTint : hov ? p.frameHover : p.surface), 10.0f * sc);
    dl->AddRect(pos, b, A(p.outline), 10.0f * sc, 1.5f * sc);
    text(dl, fonts().semibold, 14.0f, ImVec2(pos.x + (size.x - ts.x) * 0.5f, pos.y + (size.y - ts.y) * 0.5f), p.text, label, end);
    return clicked;
}

bool NavTab(const char* label, bool active) {
    const float sc = S();
    const Palette& p = pal();
    const char* end = labelEnd(label);
    const ImVec2 ts = measure(fonts().regular, 14.0f, label, end);
    const ImVec2 size(ts.x + 28.0f * sc, 36.0f * sc);
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const bool clicked = ImGui::InvisibleButton(label, size);
    const bool hov = ImGui::IsItemHovered();
    handCursor();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 b(pos.x + size.x, pos.y + size.y);
    if (active) dl->AddRectFilled(pos, b, A(p.primary), 10.0f * sc);
    else if (hov) dl->AddRectFilled(pos, b, A(p.segBg), 10.0f * sc);
    text(dl, active ? fonts().semibold : fonts().regular, 14.0f, ImVec2(pos.x + 14.0f * sc, pos.y + (size.y - ts.y) * 0.5f),
         active ? p.onPrimary : p.text2, label, end);
    return clicked;
}

// ---------------------------------------------------------------------------
// Status
// ---------------------------------------------------------------------------

float StatusPillWidth(const char* t) { return measure(fonts().regular, 13.0f, t).x + 36.0f * S(); }

bool StatusPill(const char* t, ImU32 bg, ImU32 fg, ImU32 dot) {
    const float sc = S();
    const ImVec2 ts = measure(fonts().regular, 13.0f, t);
    const ImVec2 size(ts.x + 36.0f * sc, 30.0f * sc);
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    ImGui::PushID(t);
    const bool clicked = ImGui::InvisibleButton("##pill", size);
    ImGui::PopID();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(pos, ImVec2(pos.x + size.x, pos.y + size.y), A(bg), size.y * 0.5f);
    dl->AddCircleFilled(ImVec2(pos.x + 16.0f * sc, pos.y + size.y * 0.5f), 4.0f * sc, A(dot), 12);
    text(dl, fonts().regular, 13.0f, ImVec2(pos.x + 26.0f * sc, pos.y + (size.y - ts.y) * 0.5f), fg, t);
    return clicked;
}

void Badge(const char* t, ImU32 bg, ImU32 fg) {
    const float sc = S();
    const ImVec2 ts = measure(fonts().regular, 12.0f, t);
    const ImVec2 size(ts.x + 20.0f * sc, 24.0f * sc);
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    ImGui::Dummy(size);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(pos, ImVec2(pos.x + size.x, pos.y + size.y), A(bg), size.y * 0.5f);
    text(dl, fonts().regular, 12.0f, ImVec2(pos.x + 10.0f * sc, pos.y + (size.y - ts.y) * 0.5f), fg, t);
}

void IconBox(const char* icon, ImU32 bg, float box, ImU32 fg) {
    const float sc = S();
    const ImVec2 size(box * sc, box * sc);
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    ImGui::Dummy(size);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(pos, ImVec2(pos.x + size.x, pos.y + size.y), A(bg), 10.0f * sc);
    const float fs = box * 0.5f;
    const ImVec2 ts = measure(fonts().regular, fs, icon);
    text(dl, fonts().regular, fs, ImVec2(pos.x + (size.x - ts.x) * 0.5f, pos.y + (size.y - ts.y) * 0.5f),
         fg ? fg : pal().text, icon);
}

// ---------------------------------------------------------------------------
// Values
// ---------------------------------------------------------------------------

bool Slider(const char* id, float* v, float minV, float maxV, float defV, float width, ImU32 color) {
    const float sc = S();
    const Palette& p = pal();
    const float w = width > 0 ? width : ImGui::GetContentRegionAvail().x;
    const float h = 24.0f * sc;
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton(id, ImVec2(w, h));
    const bool hov = ImGui::IsItemHovered(), act = ImGui::IsItemActive();
    handCursor();
    const float r = 9.0f * sc;
    const float x0 = pos.x + r, x1 = pos.x + w - r;
    bool changed = false;
    float val = *v;
    ImGuiIO& io = ImGui::GetIO();
    if (hov && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
        val = defV;
    } else if (act) {
        const float t = std::clamp((io.MousePos.x - x0) / std::max(1.0f, x1 - x0), 0.0f, 1.0f);
        val = minV + t * (maxV - minV);
    }
    if (hov && io.MouseWheel != 0.0f) val += io.MouseWheel * (maxV - minV) * (io.KeyShift ? 0.002f : 0.01f);
    val = std::clamp(val, minV, maxV);
    if (val != *v) {
        *v = val;
        changed = true;
    }
    const float t = (maxV > minV) ? (*v - minV) / (maxV - minV) : 0.0f;
    const float cy = pos.y + h * 0.5f, xk = x0 + t * (x1 - x0);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(ImVec2(x0, cy - 3.0f * sc), ImVec2(x1, cy + 3.0f * sc), A(p.track), 3.0f * sc);
    dl->AddRectFilled(ImVec2(x0, cy - 3.0f * sc), ImVec2(xk, cy + 3.0f * sc), A(color ? color : p.accent), 3.0f * sc);
    dl->AddCircleFilled(ImVec2(xk, cy), r, A(IM_COL32(255, 255, 255, 255)), 24);
    dl->AddCircle(ImVec2(xk, cy), r, A(p.outline), 24, 1.5f * sc);
    return changed;
}

bool SliderRow(const char* label, const char* valueText, const char* id, float* v, float minV, float maxV, float defV,
               float width) {
    const float w = width > 0 ? width : ImGui::GetContentRegionAvail().x;
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    ImGui::PushFont(fonts().regular, 14.0f);
    ImGui::TextUnformatted(label);
    ImGui::PopFont();
    if (valueText && *valueText) {
        const ImVec2 vs = measure(fonts().regular, 13.0f, valueText);
        ImGui::GetWindowDrawList()->AddText(fonts().regular, 13.0f * S(), ImVec2(p0.x + w - vs.x, p0.y + 1.0f * S()),
                                            A(pal().text2), valueText);
    }
    return Slider(id, v, minV, maxV, defV, w);
}

void SegMeter(float levelDb, float width, float height, int segments) {
    const float sc = S();
    const Palette& p = pal();
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    ImGui::Dummy(ImVec2(width, height));
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float gap = 4.0f * sc;
    const float segW = (width - gap * (segments - 1)) / segments;
    const float f = meterFraction(levelDb);
    for (int i = 0; i < segments; ++i) {
        const float x = pos.x + i * (segW + gap);
        const bool lit = (i + 0.5f) / segments < f;
        const float pos01 = (i + 0.5f) / segments;
        const ImU32 col = !lit ? p.meterBg : pos01 < 0.8f ? p.meterGreen : pos01 < 0.95f ? p.meterAmber : p.bad;
        dl->AddRectFilled(ImVec2(x, pos.y), ImVec2(x + segW, pos.y + height), A(col), 4.0f * sc);
    }
}

void BarMeter(float levelDb, float width, float height, ImU32 color) {
    const Palette& p = pal();
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    ImGui::Dummy(ImVec2(width, height));
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float r = height * 0.45f;
    dl->AddRectFilled(pos, ImVec2(pos.x + width, pos.y + height), A(p.meterBg), r);
    const float f = meterFraction(levelDb);
    if (f > 0.0f) dl->AddRectFilled(pos, ImVec2(pos.x + width * f, pos.y + height), A(color ? color : p.meterGreen), r);
}

} // namespace echo::ui::pop
