#include "ui/Theme.h"

#include <windows.h>

#include <filesystem>

namespace echo::ui {

namespace {

struct AccentSet {
    unsigned color, tint, ink;
};
struct Accent {
    const char* name;
    AccentSet light, dark;
};
const Accent kAccents[] = {
    {"coral", {0xEE5B3B, 0xFDEBE6, 0x8F2F18}, {0xFF7A59, 0x3A2420, 0xFFB4A2}},
    {"blue", {0x2F6FED, 0xE4ECFC, 0x1F47A8}, {0x5B8DEF, 0x1B2A4A, 0x9DB8FF}},
    {"green", {0x22A06B, 0xDFF5E8, 0x17663C}, {0x3DC47A, 0x173326, 0x7FE0A8}},
    {"violet", {0x8B5CF6, 0xEEE7FC, 0x5A35B5}, {0xA78BFA, 0x2A2147, 0xC4AEFF}},
};

const Accent& accentByName(const std::string& name) {
    for (const auto& a : kAccents)
        if (name == a.name) return a;
    return kAccents[0];
}

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

Palette g_active = lightPalette();
bool g_dark = false;
float g_scale = 1.0f;
Fonts g_fonts;

} // namespace

const char* const kAccentNames[4] = {"coral", "blue", "green", "violet"};
const char* const kAccentLabels[4] = {"Coral", "Blue", "Green", "Violet"};

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

ImU32 accentColor(const std::string& name) {
    const Accent& a = accentByName(name);
    return hex(g_dark ? a.dark.color : a.light.color);
}

void applyTheme(int theme, float dpiScale, float userScale, const std::string& accent) {
    g_dark = theme == 1 || (theme == 2 && !windowsPrefersLight());
    g_active = g_dark ? darkPalette() : lightPalette();
    const AccentSet& a = g_dark ? accentByName(accent).dark : accentByName(accent).light;
    g_active.accent = hex(a.color);
    g_active.accentTint = g_active.accentDim = hex(a.tint);
    g_active.accentInk = hex(a.ink);
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
