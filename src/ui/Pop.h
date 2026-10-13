// "Friendly Pop" widgets: outlined cards with a soft offset shadow, switches,
// segmented controls, chips, tiles and slim sliders/meters, drawn with Dear
// ImGui primitives in the current theme (see Theme.h). Sizes are in design
// pixels and scaled by uiScale().
#pragma once

#include "imgui.h"

namespace echo::ui::pop {

// ---- text ----
void PageTitle(const char* title, const char* subtitle = nullptr); // 30 px heading + line
void SectionTitle(const char* text);                                 // 19 px
void CardTitle(const char* text);                                    // 17 px
void Label(const char* text);                                        // 14 px, medium
void Caption(const char* text);                                      // 13 px, secondary, wrapped
void Hint(const char* text);                                         // 12 px, dim, wrapped
void TextColoredWrapped(ImU32 color, const char* text, float size = 0.0f);

// ---- cards ----
// A white card with a 1.5 px outline and an offset shadow (quiet: soft
// outline, no shadow). width 0 = all available width; height 0 = fit content.
void BeginCard(const char* id, float width = 0.0f, float height = 0.0f, bool quiet = false, float padX = 20.0f,
               float padY = 18.0f);
void EndCard();
// Thin divider across the current content width.
void Divider(float spaceAbove = 8.0f, float spaceBelow = 8.0f);
// Tinted note with an icon (info/warning/success).
void Banner(const char* icon, const char* text, ImU32 bg, ImU32 iconColor);

// ---- buttons ----
enum class Btn { Primary, Secondary, Accent, Quiet, Danger };
bool Button(const char* label, Btn kind = Btn::Secondary, ImVec2 size = ImVec2(0, 0));
bool SmallButton(const char* label, Btn kind = Btn::Secondary); // 32 px tall
bool LinkButton(const char* label);
// Press-and-hold button: true while held. `heldLabel` is shown while held.
bool HoldButton(const char* label, const char* heldLabel, ImVec2 size = ImVec2(0, 0));

// ---- toggles and choices ----
bool Switch(const char* id, bool* v);
// Title + description on the left, switch on the right, divider below.
bool SwitchRow(const char* title, const char* desc, bool* v, bool divider = true);
// Equal-width segmented control. `subs` (optional) adds a second line.
bool Segmented(const char* id, int* current, const char* const* labels, int count, float width = 0.0f,
               const char* const* subs = nullptr);
bool Chip(const char* label, bool on);
// Rounded pill button in any colours (fill, border, text).
bool Pill(const char* label, ImU32 fill, ImU32 border, ImU32 fg, float width = 0.0f);
// Bus send toggle: A buses blue, B buses violet.
bool SendPill(const char* label, bool on, bool busB, float width = 0.0f, bool blocked = false);
// Height a Tile needs to show all of its text at this width.
float TileHeight(const char* icon, const char* name, const char* sub, float width);
// Voice/preset tile. `corner` is small text in the top-right corner.
bool Tile(const char* id, const char* icon, const char* name, const char* sub, const char* corner, bool selected,
          ImVec2 size);
// Soundboard pad: a square in the pad's colour with its name, key and play
// position (progress 0..1, < 0 = not playing). Reports the left button going
// down and up (a pad can play while held) and requests to edit it (right
// click or the corner button).
struct PadEvents {
    bool pressed = false, released = false, edit = false;
};
PadEvents SoundPadTile(const char* id, const char* icon, const char* name, const char* keyText, const char* modeText,
                       ImU32 tint, float progress, bool warn, ImVec2 size);
// Dashed "add" tile.
bool AddTile(const char* id, const char* label, ImVec2 size);
// Radio-style choice row.
bool RadioRow(const char* id, const char* title, const char* sub, bool selected, float width = 0.0f);
// Checkbox row (bordered).
bool CheckRow(const char* id, const char* title, const char* sub, bool* v, float width = 0.0f);
// A key or mouse button shown as a key cap.
bool KeyCap(const char* label, bool capturing, float minWidth = 160.0f);
bool NavTab(const char* label, bool active);

// ---- status ----
// Returns true when clicked.
bool StatusPill(const char* text, ImU32 bg, ImU32 fg, ImU32 dot);
float StatusPillWidth(const char* text);
void Badge(const char* text, ImU32 bg, ImU32 fg);
void IconBox(const char* icon, ImU32 bg, float box = 36.0f, ImU32 fg = 0);

// ---- values ----
// Slim slider (accent track, round knob). Wheel adjusts; double-click resets.
bool Slider(const char* id, float* v, float minV, float maxV, float defV, float width = 0.0f, ImU32 color = 0);
// Label and value on one line, slider below.
bool SliderRow(const char* label, const char* valueText, const char* id, float* v, float minV, float maxV,
               float defV, float width = 0.0f);
// Horizontal segmented level meter (dBFS).
void SegMeter(float levelDb, float width, float height, int segments = 20);
// Horizontal bar meter (dBFS).
void BarMeter(float levelDb, float width, float height, ImU32 color = 0);

// Shared helpers.
float S(); // uiScale()
float meterFraction(float db); // -60..0 dBFS -> 0..1
// Columns that fit `minWidth` each into `avail` with `gap`.
int columnsFor(float avail, float minWidth, float gap);

} // namespace echo::ui::pop
