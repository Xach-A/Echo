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

// Themes (UiSettings::theme): light, dark, match Windows (its app mode),
// ready-made themes, and the user's own colours.
enum ThemeId : int {
    kThemeLight = 0,
    kThemeDark,
    kThemeSystem,
    kThemeMidnight,
    kThemeOcean,
    kThemeForest,
    kThemeSunset,
    kThemeSlate,
    kThemeOled,
    kThemeRose,
    kThemeSand,
    kThemeMint,
    kThemeContrast,
    kThemeCustom,
    kThemeCount
};
const char* themeName(int theme);

// A theme's own colours (0xRRGGBB); everything else is worked out from them.
struct ThemeColors {
    unsigned background = 0, surface = 0, text = 0;
};
// The colours of a ready-made theme (or `custom` for kThemeCustom; light or
// dark for match Windows by Windows' setting).
ThemeColors themeColors(int theme, const ThemeColors& custom);

struct ThemeChoice {
    int theme = kThemeLight;
    std::string accent = "coral"; // a kAccentNames entry, or "custom"
    unsigned customAccent = 0xEE5B3B;
    ThemeColors custom;
};
void applyTheme(const ThemeChoice& choice, float dpiScale, float userScale);
// Windows' app mode (Settings > Personalization > Colors).
bool windowsPrefersLight();
// True when the theme in use is dark (for the window title bar).
bool themeIsDark();
// The accent colour a name stands for in the current light/dark mode.
ImU32 accentColor(const std::string& name, unsigned customAccent = 0xEE5B3B);
// WCAG contrast ratio of two colours (0xRRGGBB), 1..21.
float contrastRatio(unsigned rgbA, unsigned rgbB);
constexpr int kAccentCount = 9;
extern const char* const kAccentNames[kAccentCount]; // stored names
extern const char* const kAccentLabels[kAccentCount];

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
