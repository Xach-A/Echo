// Visual theme ("Friendly Pop"): palette, ImGui style, fonts and DPI scaling.
#pragma once

#include <string>

#include "imgui.h"

namespace echo::ui {

struct Palette {
    // Shared with older views.
    ImU32 bg, panel, panelAlt, frame, frameHover, border, text, textDim, accent, accentDim;
    ImU32 good, warn, bad, meterBg;
    ImU32 groupHardware, groupApps, groupVirtual, groupGen, busPhysical, busVirtual;

    // Friendly Pop.
    ImU32 surface, surfaceAlt;   // cards, quiet panels
    ImU32 text2;                 // secondary text
    ImU32 outline, shadow;       // card outline (1.5 px) and offset shadow
    ImU32 line, lineSoft;        // dividers, quiet outlines
    ImU32 primary, onPrimary;    // main buttons, the active tab
    ImU32 onAccent;              // text on the accent colour
    ImU32 accentTint, accentInk; // selected tiles; text on the tint
    ImU32 track, segBg;          // switch off track; segmented control background
    ImU32 goodTint, goodInk, warnTint, warnInk, badTint, badInk, infoTint;
    ImU32 busATint, busAInk, busBTint, busBInk;
    ImU32 link;
    ImU32 meterGreen, meterAmber;
};

const Palette& pal();

// theme: 0 light, 1 dark, 2 match Windows (the "app mode" setting).
// accent: "coral", "blue", "green" or "violet".
void applyTheme(int theme, float dpiScale, float userScale, const std::string& accent = "coral");
// Windows' app mode (Settings > Personalization > Colors).
bool windowsPrefersLight();
// True when the theme in use is dark (for the window title bar).
bool themeIsDark();
// The accent colour a name stands for in the current light/dark mode.
ImU32 accentColor(const std::string& name);
extern const char* const kAccentNames[4]; // stored names
extern const char* const kAccentLabels[4];

// Combined DPI and interface-size scale; multiply hand-drawn sizes by it.
float uiScale();

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
// 0xRRGGBB -> ImU32
inline ImU32 hex(unsigned v) { return IM_COL32((v >> 16) & 0xFF, (v >> 8) & 0xFF, v & 0xFF, 255); }

} // namespace echo::ui
