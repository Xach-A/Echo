// Custom audio widgets drawn with Dear ImGui primitives.
#pragma once

#include <string>

#include "core/Meter.h"
#include "dsp/Effect.h"
#include "imgui.h"

namespace echo::ui {

// Fader position (0..1) <-> dB, console-style taper (+12 dB at the top,
// 0 dB at 75%, -inf at the bottom).
float faderPosToDb(float pos);
float faderDbToPos(float db);

// Vertical fader. Double-click resets to 0 dB; Ctrl+click types a value;
// mouse wheel adjusts by 0.5 dB (Shift: 0.1 dB). Returns true if changed.
bool Fader(const char* id, float* db, ImVec2 size, ImU32 color, float minDb = -120.0f, float maxDb = 12.0f);

// Stereo (or mono) level meter with peak hold and clip latch. Clicking the
// clip LED clears it. `rms` draws an RMS overlay.
void LevelMeter(const char* id, MeterBallistics& m, ImVec2 size, bool stereo = true, bool showScale = false);

// Horizontal stereo meter with peak hold (used in the setup wizard).
void HLevelMeter(const char* id, MeterBallistics& m, ImVec2 size);

// Thin horizontal meter (for lists).
void MiniMeter(float levelDb, ImVec2 size);

// Rotary knob. Drag vertically; Shift = fine; double-click resets.
bool Knob(const char* label, float* value, float minV, float maxV, float defV, const char* fmt, bool logScale = false,
          float diameter = 0.0f, ImU32 color = 0);

// Draws a control for parameter `index` of an effect (knob, combo, toggle).
bool ParamControl(Effect& fx, int index, float knobDiameter = 0.0f);
std::string formatParam(const ParamInfo& p, float v);

// Toggle button with on/off colours.
bool ToggleButton(const char* label, bool* v, ImU32 onColor, ImVec2 size = ImVec2(0, 0), const char* tooltip = nullptr);
// Small LED dot.
void Led(bool on, ImU32 color, float radius);

// Section header with a coloured bar.
void GroupHeader(const char* text, ImU32 color);

// Tooltip helper honoring the "show tooltips" setting.
void Tip(const char* text);
void setTooltipsEnabled(bool on);

// Help marker "(?)" with tooltip.
void HelpMarker(const char* text);

// Text that is truncated with an ellipsis to fit a width.
void TextEllipsis(const std::string& s, float width, ImU32 color = 0);

} // namespace echo::ui
