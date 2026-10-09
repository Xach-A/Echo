// Visual theme: palette, ImGui style, fonts and DPI scaling.
#pragma once

#include <string>

#include "imgui.h"

namespace echo::ui {

struct Palette {
    ImU32 bg, panel, panelAlt, frame, frameHover, border, text, textDim, accent, accentDim;
    ImU32 good, warn, bad, meterBg;
    ImU32 groupHardware, groupApps, groupVirtual, groupGen, busPhysical, busVirtual;
};

const Palette& pal();

// theme: 0 dark, 1 midnight (darker), 2 light, 3 match Windows (light or
// dark from the "app mode" setting). accent: "blue", "coral", "green" or
// "violet".
void applyTheme(int theme, float dpiScale, float userScale, const std::string& accent = "blue");
// Windows' app mode (Settings > Personalization > Colors).
bool windowsPrefersLight();
// True when the theme in use is a dark one (for the window title bar).
bool themeIsDark();
extern const char* const kAccentNames[4]; // stored names
extern const char* const kAccentLabels[4];

struct Fonts {
    ImFont* regular = nullptr;
    ImFont* semibold = nullptr;
    ImFont* mono = nullptr;
    bool icons = false;
    float baseSize = 15.0f;
};
Fonts& fonts();
void loadFonts();

inline ImU32 withAlpha(ImU32 c, float a) {
    return (c & 0x00FFFFFFu) | (static_cast<ImU32>(a * 255.0f) << 24);
}
inline ImVec4 toVec4(ImU32 c) { return ImGui::ColorConvertU32ToFloat4(c); }
inline ImU32 rgb(int r, int g, int b) { return IM_COL32(r, g, b, 255); }

} // namespace echo::ui
