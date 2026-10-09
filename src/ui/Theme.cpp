#include "ui/Theme.h"

#include <windows.h>

#include <cstring>
#include <filesystem>

namespace echo::ui {

namespace {

Palette g_dark = {
    IM_COL32(17, 19, 24, 255),    // bg
    IM_COL32(24, 27, 34, 255),    // panel
    IM_COL32(30, 34, 43, 255),    // panelAlt
    IM_COL32(39, 44, 55, 255),    // frame
    IM_COL32(50, 57, 71, 255),    // frameHover
    IM_COL32(52, 58, 72, 255),    // border
    IM_COL32(228, 232, 240, 255), // text
    IM_COL32(138, 147, 166, 255), // textDim
    IM_COL32(76, 141, 255, 255),  // accent
    IM_COL32(46, 86, 160, 255),   // accentDim
    IM_COL32(61, 220, 132, 255),  // good
    IM_COL32(255, 193, 69, 255),  // warn
    IM_COL32(255, 77, 77, 255),   // bad
    IM_COL32(10, 11, 14, 255),    // meterBg
    IM_COL32(255, 169, 77, 255),  // hardware (orange)
    IM_COL32(124, 227, 139, 255), // apps (green)
    IM_COL32(185, 140, 255, 255), // virtual (purple)
    IM_COL32(255, 224, 102, 255), // generators (yellow)
    IM_COL32(77, 210, 255, 255),  // physical bus (cyan)
    IM_COL32(200, 140, 255, 255), // virtual bus (violet)
};

Palette g_midnight = [] {
    Palette p = g_dark;
    p.bg = IM_COL32(8, 9, 12, 255);
    p.panel = IM_COL32(14, 16, 20, 255);
    p.panelAlt = IM_COL32(19, 21, 27, 255);
    p.frame = IM_COL32(28, 31, 39, 255);
    p.frameHover = IM_COL32(40, 45, 57, 255);
    p.border = IM_COL32(38, 42, 52, 255);
    return p;
}();

Palette g_light = [] {
    Palette p = g_dark;
    p.bg = IM_COL32(236, 238, 242, 255);
    p.panel = IM_COL32(248, 249, 251, 255);
    p.panelAlt = IM_COL32(240, 242, 246, 255);
    p.frame = IM_COL32(225, 229, 236, 255);
    p.frameHover = IM_COL32(210, 216, 226, 255);
    p.border = IM_COL32(200, 206, 216, 255);
    p.text = IM_COL32(28, 32, 40, 255);
    p.textDim = IM_COL32(100, 108, 124, 255);
    p.accent = IM_COL32(40, 110, 230, 255);
    p.accentDim = IM_COL32(150, 185, 245, 255);
    p.meterBg = IM_COL32(40, 44, 52, 255);
    p.groupHardware = IM_COL32(222, 128, 30, 255);
    p.groupApps = IM_COL32(40, 160, 70, 255);
    p.groupVirtual = IM_COL32(130, 80, 220, 255);
    p.groupGen = IM_COL32(190, 150, 0, 255);
    p.busPhysical = IM_COL32(0, 150, 200, 255);
    p.busVirtual = IM_COL32(150, 80, 220, 255);
    return p;
}();

Palette g_active = g_dark;
const Palette* g_current = &g_active;
bool g_dark_in_use = true;
Fonts g_fonts;

struct AccentColors {
    ImU32 dark, darkDim, light, lightDim;
};
AccentColors accentColors(const std::string& name) {
    if (name == "coral") return {IM_COL32(255, 122, 89, 255), IM_COL32(150, 70, 52, 255), IM_COL32(214, 72, 40, 255), IM_COL32(247, 180, 160, 255)};
    if (name == "green") return {IM_COL32(61, 200, 130, 255), IM_COL32(36, 110, 75, 255), IM_COL32(22, 135, 78, 255), IM_COL32(150, 215, 180, 255)};
    if (name == "violet") return {IM_COL32(167, 130, 255, 255), IM_COL32(95, 72, 160, 255), IM_COL32(118, 70, 220, 255), IM_COL32(195, 175, 245, 255)};
    return {IM_COL32(76, 141, 255, 255), IM_COL32(46, 86, 160, 255), IM_COL32(40, 110, 230, 255), IM_COL32(150, 185, 245, 255)};
}

} // namespace

const char* const kAccentNames[4] = {"blue", "coral", "green", "violet"};
const char* const kAccentLabels[4] = {"Blue", "Coral", "Green", "Violet"};

const Palette& pal() { return *g_current; }
Fonts& fonts() { return g_fonts; }

bool windowsPrefersLight() {
    DWORD value = 0, size = sizeof(value);
    const LSTATUS r = RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                                   L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &value, &size);
    return r == ERROR_SUCCESS && value != 0;
}

bool themeIsDark() { return g_dark_in_use; }

void applyTheme(int theme, float dpiScale, float userScale, const std::string& accent) {
    if (theme == 3) theme = windowsPrefersLight() ? 2 : 0;
    g_dark_in_use = theme != 2;
    g_active = theme == 1 ? g_midnight : theme == 2 ? g_light : g_dark;
    const AccentColors ac = accentColors(accent);
    g_active.accent = g_dark_in_use ? ac.dark : ac.light;
    g_active.accentDim = g_dark_in_use ? ac.darkDim : ac.lightDim;
    const Palette& p = g_active;
    ImGuiStyle& s = ImGui::GetStyle();
    s = ImGuiStyle();
    s.WindowPadding = ImVec2(10, 10);
    s.FramePadding = ImVec2(8, 5);
    s.ItemSpacing = ImVec2(8, 6);
    s.ItemInnerSpacing = ImVec2(6, 4);
    s.CellPadding = ImVec2(6, 4);
    s.ScrollbarSize = 12;
    s.GrabMinSize = 10;
    s.WindowRounding = 6;
    s.ChildRounding = 6;
    s.FrameRounding = 5;
    s.PopupRounding = 6;
    s.ScrollbarRounding = 6;
    s.GrabRounding = 4;
    s.TabRounding = 5;
    s.WindowBorderSize = 1;
    s.FrameBorderSize = 0;
    s.PopupBorderSize = 1;
    s.TabBorderSize = 0;
    s.WindowTitleAlign = ImVec2(0.0f, 0.5f);
    s.SeparatorTextBorderSize = 1;
    s.DockingSeparatorSize = 3;

    ImVec4* c = s.Colors;
    auto v = [](ImU32 col) { return ImGui::ColorConvertU32ToFloat4(col); };
    c[ImGuiCol_Text] = v(p.text);
    c[ImGuiCol_TextDisabled] = v(p.textDim);
    c[ImGuiCol_WindowBg] = v(p.panel);
    c[ImGuiCol_ChildBg] = v(withAlpha(p.panelAlt, 0.0f));
    c[ImGuiCol_PopupBg] = v(p.panelAlt);
    c[ImGuiCol_Border] = v(p.border);
    c[ImGuiCol_BorderShadow] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_FrameBg] = v(p.frame);
    c[ImGuiCol_FrameBgHovered] = v(p.frameHover);
    c[ImGuiCol_FrameBgActive] = v(p.frameHover);
    c[ImGuiCol_TitleBg] = v(p.bg);
    c[ImGuiCol_TitleBgActive] = v(p.bg);
    c[ImGuiCol_TitleBgCollapsed] = v(p.bg);
    c[ImGuiCol_MenuBarBg] = v(p.bg);
    c[ImGuiCol_ScrollbarBg] = v(withAlpha(p.bg, 0.4f));
    c[ImGuiCol_ScrollbarGrab] = v(p.frameHover);
    c[ImGuiCol_ScrollbarGrabHovered] = v(p.border);
    c[ImGuiCol_ScrollbarGrabActive] = v(p.accentDim);
    c[ImGuiCol_CheckMark] = v(p.accent);
    c[ImGuiCol_SliderGrab] = v(p.accent);
    c[ImGuiCol_SliderGrabActive] = v(p.accent);
    c[ImGuiCol_Button] = v(p.frame);
    c[ImGuiCol_ButtonHovered] = v(p.frameHover);
    c[ImGuiCol_ButtonActive] = v(p.accentDim);
    c[ImGuiCol_Header] = v(withAlpha(p.accent, 0.25f));
    c[ImGuiCol_HeaderHovered] = v(withAlpha(p.accent, 0.35f));
    c[ImGuiCol_HeaderActive] = v(withAlpha(p.accent, 0.5f));
    c[ImGuiCol_Separator] = v(p.border);
    c[ImGuiCol_SeparatorHovered] = v(p.accentDim);
    c[ImGuiCol_SeparatorActive] = v(p.accent);
    c[ImGuiCol_ResizeGrip] = v(withAlpha(p.accent, 0.2f));
    c[ImGuiCol_ResizeGripHovered] = v(withAlpha(p.accent, 0.5f));
    c[ImGuiCol_ResizeGripActive] = v(p.accent);
    c[ImGuiCol_Tab] = v(p.bg);
    c[ImGuiCol_TabHovered] = v(p.frameHover);
    c[ImGuiCol_TabSelected] = v(p.panel);
    c[ImGuiCol_TabSelectedOverline] = v(p.accent);
    c[ImGuiCol_TabDimmed] = v(p.bg);
    c[ImGuiCol_TabDimmedSelected] = v(p.panel);
    c[ImGuiCol_TabDimmedSelectedOverline] = v(p.accentDim);
    c[ImGuiCol_DockingPreview] = v(withAlpha(p.accent, 0.5f));
    c[ImGuiCol_DockingEmptyBg] = v(p.bg);
    c[ImGuiCol_PlotLines] = v(p.accent);
    c[ImGuiCol_PlotHistogram] = v(p.accent);
    c[ImGuiCol_TableHeaderBg] = v(p.panelAlt);
    c[ImGuiCol_TableBorderStrong] = v(p.border);
    c[ImGuiCol_TableBorderLight] = v(withAlpha(p.border, 0.5f));
    c[ImGuiCol_TableRowBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_TableRowBgAlt] = v(withAlpha(p.frame, 0.35f));
    c[ImGuiCol_TextSelectedBg] = v(withAlpha(p.accent, 0.35f));
    c[ImGuiCol_NavCursor] = v(p.accent);
    c[ImGuiCol_ModalWindowDimBg] = ImVec4(0, 0, 0, 0.55f);

    const float scale = dpiScale * userScale;
    s.ScaleAllSizes(scale);
    s.FontScaleDpi = scale;
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
