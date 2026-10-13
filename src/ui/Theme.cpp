#include "ui/Theme.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <filesystem>

namespace echo::ui {

namespace {

// Accent colours for light and dark themes; tints and text-on-tint colours
// are mixed from them and the theme's card colour.
struct Accent {
    const char* name;
    unsigned light, dark;
};
const Accent kAccents[kAccentCount] = {
    {"coral", 0xEE5B3B, 0xFF7A59},  {"blue", 0x2F6FED, 0x5B8DEF},   {"green", 0x22A06B, 0x3DC47A},
    {"violet", 0x8B5CF6, 0xA78BFA}, {"pink", 0xDB2777, 0xF472B6},   {"teal", 0x0D9488, 0x2DD4BF},
    {"orange", 0xEA7A0C, 0xFB923C}, {"red", 0xDC2626, 0xF87171},    {"yellow", 0xCA8A04, 0xFACC15},
};

const Accent& accentByName(const std::string& name) {
    for (const auto& a : kAccents)
        if (name == a.name) return a;
    return kAccents[0];
}

ImU32 mixc(ImU32 a, ImU32 b, float t) {
    const ImVec4 x = ImGui::ColorConvertU32ToFloat4(a), y = ImGui::ColorConvertU32ToFloat4(b);
    return ImGui::ColorConvertFloat4ToU32(ImVec4(x.x + (y.x - x.x) * t, x.y + (y.y - x.y) * t, x.z + (y.z - x.z) * t, 1.0f));
}

// Relative luminance (WCAG).
float luminance(ImU32 c) {
    const ImVec4 v = ImGui::ColorConvertU32ToFloat4(c);
    auto lin = [](float u) { return u <= 0.03928f ? u / 12.92f : std::pow((u + 0.055f) / 1.055f, 2.4f); };
    return 0.2126f * lin(v.x) + 0.7152f * lin(v.y) + 0.0722f * lin(v.z);
}

float contrast(ImU32 a, ImU32 b) {
    const float la = luminance(a), lb = luminance(b);
    return (std::max(la, lb) + 0.05f) / (std::min(la, lb) + 0.05f);
}

struct ThemeDef {
    const char* name;
    ThemeColors colors;
};
// Order matches ThemeId. Light and dark use their hand-tuned palettes.
const ThemeDef kThemes[kThemeCount] = {
    {"Light", {0xF4F6FA, 0xFFFFFF, 0x1C2230}},     {"Dark", {0x13161C, 0x1C2028, 0xECEFF4}},
    {"Match Windows", {0xF4F6FA, 0xFFFFFF, 0x1C2230}},
    {"Midnight", {0x0D1222, 0x161D33, 0xE6EAF5}},  {"Ocean", {0x0A1C22, 0x112C35, 0xE2F3F6}},
    {"Forest", {0x0E1813, 0x16261D, 0xE5F1E8}},    {"Sunset", {0x1D111E, 0x2A192C, 0xF7E8EE}},
    {"Slate", {0x22262D, 0x2C313A, 0xE8EBF0}},     {"OLED black", {0x000000, 0x0C0C0E, 0xF2F2F2}},
    {"Rose", {0xFAEFF3, 0xFFFFFF, 0x3A1F2B}},      {"Sand", {0xF5EFE4, 0xFFFCF5, 0x2F2A22}},
    {"Mint", {0xECF6F1, 0xFFFFFF, 0x1B2B24}},      {"High contrast", {0x000000, 0x000000, 0xFFFFFF}},
    {"Custom", {0x101418, 0x1A2028, 0xECEFF4}},
};

Palette lightPalette() {
    Palette p{};
    p.bg = hex(0xF4F6FA);
    p.surface = p.panel = hex(0xFFFFFF);
    p.surfaceAlt = p.panelAlt = hex(0xF7F9FC);
    p.frame = hex(0xFFFFFF);
    p.frameHover = hex(0xF1F4F9);
    p.border = hex(0xC9D1DE);
    p.text = hex(0x1C2230);
    p.text2 = hex(0x4A5366);
    p.textDim = hex(0x6A7387);
    p.outline = hex(0x283041);
    p.shadow = hex(0xDCE3EE);
    p.line = hex(0xE6EBF3);
    p.lineSoft = hex(0xD3DAE6);
    p.primary = hex(0x1C2230);
    p.onPrimary = hex(0xFFFFFF);
    p.onAccent = hex(0x1C2230);
    p.track = hex(0xE4E8F0);
    p.segBg = hex(0xEEF1F6);
    p.good = hex(0x1F9D57);
    p.goodTint = hex(0xDFF5E8);
    p.goodInk = hex(0x17663C);
    p.warn = hex(0xC98A12);
    p.warnTint = hex(0xFFF6E5);
    p.warnInk = hex(0x8A5A00);
    p.bad = hex(0xD93838);
    p.badTint = hex(0xFDE8E8);
    p.badInk = hex(0xA12424);
    p.infoTint = hex(0xEAF0FB);
    p.busPhysical = hex(0x2E62D9);
    p.busATint = hex(0xE4ECFC);
    p.busAInk = hex(0x1F47A8);
    p.busVirtual = hex(0x7A4FE0);
    p.busBTint = hex(0xEEE7FC);
    p.busBInk = hex(0x5A35B5);
    p.link = hex(0x2457C5);
    p.meterBg = hex(0xE6EBF3);
    p.meterGreen = hex(0x2FAE66);
    p.meterAmber = hex(0xE9A93B);
    p.groupHardware = hex(0xE8743B);
    p.groupApps = hex(0x2FAE66);
    p.groupVirtual = hex(0x7A4FE0);
    p.groupGen = hex(0xC99A00);
    return p;
}

Palette darkPalette() {
    Palette p{};
    p.bg = hex(0x13161C);
    p.surface = p.panel = hex(0x1C2028);
    p.surfaceAlt = p.panelAlt = hex(0x222733);
    p.frame = hex(0x242A35);
    p.frameHover = hex(0x2C3340);
    p.border = hex(0x3A4252);
    p.text = hex(0xECEFF4);
    p.text2 = hex(0xB4BCCB);
    p.textDim = hex(0x8A93A6);
    p.outline = hex(0x4A5366);
    p.shadow = hex(0x0A0C10);
    p.line = hex(0x2A303C);
    p.lineSoft = hex(0x343B48);
    p.primary = hex(0xECEFF4);
    p.onPrimary = hex(0x13161C);
    p.onAccent = hex(0x1C2230);
    p.track = hex(0x3A4252);
    p.segBg = hex(0x252B36);
    p.good = hex(0x3DC47A);
    p.goodTint = hex(0x173326);
    p.goodInk = hex(0x7FE0A8);
    p.warn = hex(0xE9A93B);
    p.warnTint = hex(0x3A2E14);
    p.warnInk = hex(0xF2C46B);
    p.bad = hex(0xFF6B6B);
    p.badTint = hex(0x3A1A1A);
    p.badInk = hex(0xFF9C9C);
    p.infoTint = hex(0x1B2536);
    p.busPhysical = hex(0x5B8DEF);
    p.busATint = hex(0x1B2A4A);
    p.busAInk = hex(0x9DB8FF);
    p.busVirtual = hex(0xA78BFA);
    p.busBTint = hex(0x2A2147);
    p.busBInk = hex(0xC4AEFF);
    p.link = hex(0x8DB4FF);
    p.meterBg = hex(0x2A303C);
    p.meterGreen = hex(0x3DC47A);
    p.meterAmber = hex(0xE9A93B);
    p.groupHardware = hex(0xFF9A5C);
    p.groupApps = hex(0x3DC47A);
    p.groupVirtual = hex(0xA78BFA);
    p.groupGen = hex(0xFFD15C);
    return p;
}

// Any theme from its three colours: lines, secondary text, tints and so on
// are mixes of them; status and bus colours come from the light or dark
// palette.
Palette derivedPalette(const ThemeColors& c, bool dark) {
    Palette p = dark ? darkPalette() : lightPalette();
    const ImU32 bg = hex(c.background), sf = hex(c.surface), tx = hex(c.text);
    p.bg = bg;
    p.surface = p.panel = sf;
    p.surfaceAlt = p.panelAlt = mixc(sf, tx, dark ? 0.05f : 0.03f);
    p.frame = dark ? mixc(sf, tx, 0.06f) : sf;
    p.frameHover = mixc(sf, tx, dark ? 0.12f : 0.06f);
    p.border = mixc(sf, tx, 0.23f);
    p.text = tx;
    p.text2 = mixc(tx, sf, 0.27f);
    p.textDim = mixc(tx, sf, 0.42f);
    p.outline = dark ? mixc(sf, tx, 0.30f) : mixc(tx, sf, 0.08f);
    p.shadow = dark ? mixc(bg, hex(0x000000), 0.5f) : mixc(bg, tx, 0.07f);
    p.line = mixc(sf, tx, dark ? 0.10f : 0.08f);
    p.lineSoft = mixc(sf, tx, dark ? 0.17f : 0.15f);
    p.primary = tx;
    p.onPrimary = dark ? bg : sf;
    p.track = mixc(sf, tx, dark ? 0.20f : 0.10f);
    p.segBg = mixc(sf, tx, dark ? 0.07f : 0.05f);
    p.meterBg = p.line;
    const float t = dark ? 0.16f : 0.13f;
    p.goodTint = mixc(sf, p.good, t);
    p.warnTint = mixc(sf, p.warn, t);
    p.badTint = mixc(sf, p.bad, t);
    p.infoTint = mixc(sf, p.busPhysical, t * 0.6f);
    p.busATint = mixc(sf, p.busPhysical, t);
    p.busBTint = mixc(sf, p.busVirtual, t);
    return p;
}

Palette g_active = lightPalette();
bool g_dark = false;
float g_scale = 1.0f;
Fonts g_fonts;

} // namespace

const char* const kAccentNames[kAccentCount] = {"coral", "blue", "green", "violet", "pink", "teal", "orange", "red", "yellow"};
const char* const kAccentLabels[kAccentCount] = {"Coral", "Blue", "Green", "Violet", "Pink", "Teal", "Orange", "Red", "Yellow"};

float contrastRatio(unsigned rgbA, unsigned rgbB) { return contrast(hex(rgbA), hex(rgbB)); }

const char* themeName(int theme) { return kThemes[std::clamp(theme, 0, kThemeCount - 1)].name; }

ThemeColors themeColors(int theme, const ThemeColors& custom) {
    theme = std::clamp(theme, 0, kThemeCount - 1);
    if (theme == kThemeCustom) return custom;
    if (theme == kThemeSystem) return kThemes[windowsPrefersLight() ? kThemeLight : kThemeDark].colors;
    return kThemes[theme].colors;
}

const Palette& pal() { return g_active; }
Fonts& fonts() { return g_fonts; }
float uiScale() { return g_scale; }

bool windowsPrefersLight() {
    DWORD value = 1, size = sizeof(value);
    const LSTATUS r = RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                                   L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &value, &size);
    return r != ERROR_SUCCESS || value != 0;
}

bool themeIsDark() { return g_dark; }

ImU32 accentColor(const std::string& name, unsigned customAccent) {
    if (name == "custom") return hex(customAccent);
    const Accent& a = accentByName(name);
    return hex(g_dark ? a.dark : a.light);
}

void applyTheme(const ThemeChoice& choice, float dpiScale, float userScale) {
    const int theme = std::clamp(choice.theme, 0, kThemeCount - 1);
    const ThemeColors colors = themeColors(theme, choice.custom);
    if (theme == kThemeLight || theme == kThemeDark || theme == kThemeSystem) {
        g_dark = theme == kThemeDark || (theme == kThemeSystem && !windowsPrefersLight());
        g_active = g_dark ? darkPalette() : lightPalette();
    } else {
        g_dark = luminance(hex(colors.background)) < 0.18f;
        g_active = derivedPalette(colors, g_dark);
    }
    if (theme == kThemeContrast) {
        // Strong outlines and full-strength text everywhere.
        Palette& p = g_active;
        p.border = p.outline = p.lineSoft = p.text;
        p.line = hex(0x9A9A9A);
        p.text2 = p.text;
        p.textDim = hex(0xD8D8D8);
        p.surfaceAlt = p.panelAlt = hex(0x161616);
        p.frame = hex(0x101010);
    }
    const ImU32 accent = choice.accent == "custom" ? hex(choice.customAccent)
                                                    : hex(g_dark ? accentByName(choice.accent).dark : accentByName(choice.accent).light);
    Palette& a = g_active;
    a.accent = accent;
    a.accentTint = a.accentDim = mixc(a.surface, accent, g_dark ? 0.16f : 0.12f);
    a.accentInk = g_dark ? mixc(accent, hex(0xFFFFFF), 0.45f) : mixc(accent, hex(0x000000), 0.40f);
    // Text on the accent colour: dark or white, whichever reads better.
    a.onAccent = contrast(accent, hex(0x1C2230)) >= contrast(accent, hex(0xFFFFFF)) ? hex(0x1C2230) : hex(0xFFFFFF);
    // Text on a selected tile: make sure it reads on the tint.
    if (contrast(a.accentInk, a.accentTint) < 4.5f) a.accentInk = a.text;
    const Palette& p = g_active;

    ImGuiStyle& s = ImGui::GetStyle();
    s = ImGuiStyle();
    s.WindowPadding = ImVec2(16, 16);
    s.FramePadding = ImVec2(10, 6);
    s.ItemSpacing = ImVec2(10, 8);
    s.ItemInnerSpacing = ImVec2(8, 6);
    s.CellPadding = ImVec2(8, 6);
    s.ScrollbarSize = 12;
    s.GrabMinSize = 14;
    s.WindowRounding = 14;
    s.ChildRounding = 16;
    s.FrameRounding = 10;
    s.PopupRounding = 12;
    s.ScrollbarRounding = 8;
    s.GrabRounding = 8;
    s.TabRounding = 10;
    s.WindowBorderSize = 1.5f;
    s.ChildBorderSize = 1.5f;
    s.FrameBorderSize = 1.5f;
    s.PopupBorderSize = 1.5f;
    s.TabBorderSize = 0;
    s.WindowTitleAlign = ImVec2(0.0f, 0.5f);
    s.SeparatorTextBorderSize = 1.5f;
    s.SeparatorTextPadding = ImVec2(0, 4);
    s.DockingSeparatorSize = 2;

    ImVec4* c = s.Colors;
    auto v = [](ImU32 col) { return ImGui::ColorConvertU32ToFloat4(col); };
    c[ImGuiCol_Text] = v(p.text);
    c[ImGuiCol_TextDisabled] = v(p.textDim);
    c[ImGuiCol_WindowBg] = v(p.bg);
    c[ImGuiCol_ChildBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_PopupBg] = v(p.surface);
    c[ImGuiCol_Border] = v(p.border);
    c[ImGuiCol_BorderShadow] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_FrameBg] = v(p.frame);
    c[ImGuiCol_FrameBgHovered] = v(p.frameHover);
    c[ImGuiCol_FrameBgActive] = v(p.frameHover);
    c[ImGuiCol_TitleBg] = v(p.surface);
    c[ImGuiCol_TitleBgActive] = v(p.surface);
    c[ImGuiCol_TitleBgCollapsed] = v(p.surface);
    c[ImGuiCol_MenuBarBg] = v(p.surface);
    c[ImGuiCol_ScrollbarBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_ScrollbarGrab] = v(p.lineSoft);
    c[ImGuiCol_ScrollbarGrabHovered] = v(p.border);
    c[ImGuiCol_ScrollbarGrabActive] = v(p.textDim);
    c[ImGuiCol_CheckMark] = v(p.accent); // also the radio-button dot
    c[ImGuiCol_CheckboxSelectedBg] = v(p.accentTint);
    c[ImGuiCol_InputTextCursor] = v(p.text);
    c[ImGuiCol_TextLink] = v(p.link);
    c[ImGuiCol_TreeLines] = v(p.lineSoft);
    c[ImGuiCol_DragDropTarget] = v(p.accent);
    c[ImGuiCol_DragDropTargetBg] = v(withAlpha(p.accent, 0.12f));
    c[ImGuiCol_UnsavedMarker] = v(p.text);
    c[ImGuiCol_NavWindowingHighlight] = v(withAlpha(p.surface, 0.7f));
    c[ImGuiCol_NavWindowingDimBg] = ImVec4(0.11f, 0.13f, 0.19f, 0.25f);
    c[ImGuiCol_PlotLinesHovered] = v(p.accent);
    c[ImGuiCol_PlotHistogramHovered] = v(p.accent);
    c[ImGuiCol_SliderGrab] = v(p.accent);
    c[ImGuiCol_SliderGrabActive] = v(p.accent);
    c[ImGuiCol_Button] = v(p.surface);
    c[ImGuiCol_ButtonHovered] = v(p.frameHover);
    c[ImGuiCol_ButtonActive] = v(p.segBg);
    c[ImGuiCol_Header] = v(p.accentTint);
    c[ImGuiCol_HeaderHovered] = v(p.segBg);
    c[ImGuiCol_HeaderActive] = v(p.accentTint);
    c[ImGuiCol_Separator] = v(p.line);
    c[ImGuiCol_SeparatorHovered] = v(p.lineSoft);
    c[ImGuiCol_SeparatorActive] = v(p.accent);
    c[ImGuiCol_ResizeGrip] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_ResizeGripHovered] = v(withAlpha(p.accent, 0.5f));
    c[ImGuiCol_ResizeGripActive] = v(p.accent);
    c[ImGuiCol_Tab] = v(p.surface);
    c[ImGuiCol_TabHovered] = v(p.segBg);
    c[ImGuiCol_TabSelected] = v(p.segBg);
    c[ImGuiCol_TabSelectedOverline] = v(p.accent);
    c[ImGuiCol_TabDimmed] = v(p.surface);
    c[ImGuiCol_TabDimmedSelected] = v(p.segBg);
    c[ImGuiCol_TabDimmedSelectedOverline] = v(p.accentTint);
    c[ImGuiCol_DockingPreview] = v(withAlpha(p.accent, 0.5f));
    c[ImGuiCol_DockingEmptyBg] = v(p.bg);
    c[ImGuiCol_PlotLines] = v(p.accent);
    c[ImGuiCol_PlotHistogram] = v(p.accent);
    c[ImGuiCol_TableHeaderBg] = v(p.surfaceAlt);
    c[ImGuiCol_TableBorderStrong] = v(p.lineSoft);
    c[ImGuiCol_TableBorderLight] = v(p.line);
    c[ImGuiCol_TableRowBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_TableRowBgAlt] = v(p.surfaceAlt);
    c[ImGuiCol_TextSelectedBg] = v(withAlpha(p.accent, 0.3f));
    c[ImGuiCol_NavCursor] = v(p.accent);
    c[ImGuiCol_ModalWindowDimBg] = g_dark ? ImVec4(0, 0, 0, 0.55f) : ImVec4(0.11f, 0.13f, 0.19f, 0.35f);

    g_scale = dpiScale * userScale;
    s.ScaleAllSizes(g_scale);
    s.FontScaleDpi = g_scale;
    s.FontSizeBase = g_fonts.baseSize;
}

void loadFonts() {
    ImGuiIO& io = ImGui::GetIO();
    io.Fonts->Clear();
    namespace fs = std::filesystem;
    const fs::path dir = "C:\\Windows\\Fonts";
    auto exists = [&](const char* f) {
        std::error_code ec;
        return fs::exists(dir / f, ec);
    };
    auto path = [&](const char* f) { return (dir / f).string(); };
    ImFontConfig cfg;
    cfg.OversampleH = 2;
    cfg.OversampleV = 1;

    auto addIcons = [&]() {
        const char* iconFile = exists("SegoeIcons.ttf") ? "SegoeIcons.ttf" : exists("segmdl2.ttf") ? "segmdl2.ttf" : nullptr;
        if (!iconFile) return false;
        ImFontConfig ic;
        ic.MergeMode = true;
        ic.GlyphOffset = ImVec2(0, 2);
        ic.PixelSnapH = true;
        return io.Fonts->AddFontFromFileTTF(path(iconFile).c_str(), g_fonts.baseSize, &ic) != nullptr;
    };

    if (exists("segoeui.ttf")) {
        g_fonts.regular = io.Fonts->AddFontFromFileTTF(path("segoeui.ttf").c_str(), g_fonts.baseSize, &cfg);
        g_fonts.icons = addIcons();
    }
    if (!g_fonts.regular) g_fonts.regular = io.Fonts->AddFontDefault();
    if (exists("seguisb.ttf")) {
        g_fonts.semibold = io.Fonts->AddFontFromFileTTF(path("seguisb.ttf").c_str(), g_fonts.baseSize, &cfg);
        addIcons();
    }
    if (!g_fonts.semibold) g_fonts.semibold = g_fonts.regular;
    if (exists("consola.ttf")) g_fonts.mono = io.Fonts->AddFontFromFileTTF(path("consola.ttf").c_str(), g_fonts.baseSize, &cfg);
    if (!g_fonts.mono) g_fonts.mono = g_fonts.regular;
    io.FontDefault = g_fonts.regular;
}

} // namespace echo::ui
