// Echo - entry point: window, Direct3D 11, Dear ImGui and the main loop.
#include <windows.h>

#include <algorithm>
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif
#include <dbghelp.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <wincodec.h>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <string>

#include "Version.h"
#include "core/Log.h"
#include "core/Text.h"
#include "imgui.h"
#include "imgui_impl_dx11.h"
#include "imgui_impl_win32.h"
#include "session/Paths.h"
#include "session/Serialization.h"
#include "ui/App.h"
#include "ui/Theme.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace {

const wchar_t* kWindowClass = L"EchoMixerWindow";

ID3D11Device* g_device = nullptr;
ID3D11DeviceContext* g_context = nullptr;
IDXGISwapChain* g_swapChain = nullptr;
ID3D11RenderTargetView* g_rtv = nullptr;
bool g_occluded = false;
UINT g_resizeW = 0, g_resizeH = 0;
echo::ui::EchoApp* g_app = nullptr;
bool g_resumePending = false;
double g_resumeAt = 0.0;

// ---- notification-area (tray) icon: only while "keep running when closed" is on ----
constexpr UINT WM_ECHO_TRAY = WM_APP + 1;
constexpr UINT kTrayOpen = 1, kTrayMute = 2, kTrayQuit = 3;
UINT g_taskbarCreated = 0;
bool g_trayAdded = false;
bool g_trayNoticeShown = false;

void trayAdd(HWND hwnd) {
    NOTIFYICONDATAW nid{sizeof(nid)};
    nid.hWnd = hwnd;
    nid.uID = 1;
    nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP | NIF_SHOWTIP;
    nid.uCallbackMessage = WM_ECHO_TRAY;
    nid.hIcon = static_cast<HICON>(LoadImageW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(1), IMAGE_ICON,
                                              GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0));
    wcsncpy_s(nid.szTip, ECHO_APP_NAME_W, _TRUNCATE);
    g_trayAdded = Shell_NotifyIconW(NIM_ADD, &nid) != FALSE;
    nid.uVersion = NOTIFYICON_VERSION_4;
    if (g_trayAdded) Shell_NotifyIconW(NIM_SETVERSION, &nid);
}

void trayRemove(HWND hwnd) {
    if (!g_trayAdded) return;
    NOTIFYICONDATAW nid{sizeof(nid)};
    nid.hWnd = hwnd;
    nid.uID = 1;
    Shell_NotifyIconW(NIM_DELETE, &nid);
    g_trayAdded = false;
}

void trayNotice(HWND hwnd) {
    if (!g_trayAdded || g_trayNoticeShown) return;
    g_trayNoticeShown = true;
    NOTIFYICONDATAW nid{sizeof(nid)};
    nid.hWnd = hwnd;
    nid.uID = 1;
    nid.uFlags = NIF_INFO;
    nid.dwInfoFlags = NIIF_INFO | NIIF_RESPECT_QUIET_TIME;
    wcsncpy_s(nid.szInfoTitle, ECHO_APP_NAME_W L" is still running", _TRUNCATE);
    wcsncpy_s(nid.szInfo, L"Your microphone, voice and hotkeys keep working. Right-click the icon to quit.", _TRUNCATE);
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

void showMainWindow(HWND hwnd) {
    ShowWindow(hwnd, IsIconic(hwnd) ? SW_RESTORE : SW_SHOW);
    SetForegroundWindow(hwnd);
}

void trayMenu(HWND hwnd, int x, int y) {
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING, kTrayOpen, L"Open " ECHO_APP_NAME_W);
    AppendMenuW(m, MF_STRING | (g_app && g_app->microphonesMuted() ? MF_CHECKED : 0), kTrayMute, L"Mute microphone");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, kTrayQuit, L"Quit " ECHO_APP_NAME_W);
    SetMenuDefaultItem(m, kTrayOpen, FALSE);
    SetForegroundWindow(hwnd); // required for the menu to close when clicking elsewhere
    const UINT cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY, x, y, 0, hwnd, nullptr);
    PostMessageW(hwnd, WM_NULL, 0, 0);
    DestroyMenu(m);
    if (cmd == kTrayOpen) showMainWindow(hwnd);
    else if (cmd == kTrayMute && g_app) g_app->toggleMicrophoneMute();
    else if (cmd == kTrayQuit && g_app) g_app->requestQuit();
}

void createRenderTarget() {
    ID3D11Texture2D* back = nullptr;
    if (SUCCEEDED(g_swapChain->GetBuffer(0, IID_PPV_ARGS(&back)))) {
        g_device->CreateRenderTargetView(back, nullptr, &g_rtv);
        back->Release();
    }
}

void cleanupRenderTarget() {
    if (g_rtv) {
        g_rtv->Release();
        g_rtv = nullptr;
    }
}

bool createDevice(HWND hwnd) {
    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount = 2;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hwnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0};
    D3D_FEATURE_LEVEL got;
    HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels, 2,
                                               D3D11_SDK_VERSION, &sd, &g_swapChain, &g_device, &got, &g_context);
    if (FAILED(hr)) {
        // Older systems / remote sessions: legacy swap effect, then software.
        sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
        sd.BufferCount = 1;
        hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels, 2, D3D11_SDK_VERSION,
                                           &sd, &g_swapChain, &g_device, &got, &g_context);
        if (FAILED(hr))
            hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, levels, 2, D3D11_SDK_VERSION,
                                               &sd, &g_swapChain, &g_device, &got, &g_context);
    }
    if (FAILED(hr)) return false;
    createRenderTarget();
    return true;
}

void cleanupDevice() {
    cleanupRenderTarget();
    if (g_swapChain) g_swapChain->Release();
    if (g_context) g_context->Release();
    if (g_device) g_device->Release();
    g_swapChain = nullptr;
    g_context = nullptr;
    g_device = nullptr;
}

// Saves the current back buffer as PNG (UI snapshot tests and docs).
bool saveBackbufferPng(const std::wstring& path) {
    ID3D11Texture2D* back = nullptr;
    if (FAILED(g_swapChain->GetBuffer(0, IID_PPV_ARGS(&back)))) return false;
    D3D11_TEXTURE2D_DESC d;
    back->GetDesc(&d);
    d.Usage = D3D11_USAGE_STAGING;
    d.BindFlags = 0;
    d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    d.MiscFlags = 0;
    ID3D11Texture2D* staging = nullptr;
    bool ok = false;
    if (SUCCEEDED(g_device->CreateTexture2D(&d, nullptr, &staging))) {
        g_context->CopyResource(staging, back);
        D3D11_MAPPED_SUBRESOURCE m{};
        if (SUCCEEDED(g_context->Map(staging, 0, D3D11_MAP_READ, 0, &m))) {
            std::vector<BYTE> px(static_cast<size_t>(d.Width) * d.Height * 4);
            for (UINT y = 0; y < d.Height; ++y) {
                const BYTE* src = static_cast<const BYTE*>(m.pData) + static_cast<size_t>(y) * m.RowPitch;
                BYTE* dst = px.data() + static_cast<size_t>(y) * d.Width * 4;
                for (UINT x = 0; x < d.Width; ++x) { // RGBA -> BGRA, opaque
                    dst[4 * x] = src[4 * x + 2];
                    dst[4 * x + 1] = src[4 * x + 1];
                    dst[4 * x + 2] = src[4 * x];
                    dst[4 * x + 3] = 255;
                }
            }
            g_context->Unmap(staging, 0);
            IWICImagingFactory* f = nullptr;
            if (SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f)))) {
                IWICStream* s = nullptr;
                IWICBitmapEncoder* enc = nullptr;
                IWICBitmapFrameEncode* fr = nullptr;
                if (SUCCEEDED(f->CreateStream(&s)) && SUCCEEDED(s->InitializeFromFilename(path.c_str(), GENERIC_WRITE)) &&
                    SUCCEEDED(f->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc)) &&
                    SUCCEEDED(enc->Initialize(s, WICBitmapEncoderNoCache)) && SUCCEEDED(enc->CreateNewFrame(&fr, nullptr)) &&
                    SUCCEEDED(fr->Initialize(nullptr)) && SUCCEEDED(fr->SetSize(d.Width, d.Height))) {
                    WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppBGRA;
                    fr->SetPixelFormat(&fmt);
                    ok = SUCCEEDED(fr->WritePixels(d.Height, d.Width * 4, static_cast<UINT>(px.size()), px.data())) &&
                         SUCCEEDED(fr->Commit()) && SUCCEEDED(enc->Commit());
                }
                if (fr) fr->Release();
                if (enc) enc->Release();
                if (s) s->Release();
                f->Release();
            }
        }
        staging->Release();
    }
    back->Release();
    return ok;
}

double nowSeconds() {
    static const auto t0 = std::chrono::steady_clock::now();
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
}

LONG WINAPI crashHandler(EXCEPTION_POINTERS* ep) {
    // Write a minidump next to the logs for diagnosis; never uploaded.
    const std::wstring dir = echo::paths::localDataDir() + L"\\crashes";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    wchar_t name[64];
    SYSTEMTIME st;
    GetLocalTime(&st);
    swprintf(name, 64, L"\\echo-%04d%02d%02d-%02d%02d%02d.dmp", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    const std::wstring path = dir + name;
    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f != INVALID_HANDLE_VALUE) {
        MINIDUMP_EXCEPTION_INFORMATION mei{GetCurrentThreadId(), ep, FALSE};
        MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), f, MiniDumpNormal, &mei, nullptr, nullptr);
        CloseHandle(f);
    }
    ECHO_LOG_ERROR("Crash (exception 0x%08lX); dump written to %s", ep && ep->ExceptionRecord ? ep->ExceptionRecord->ExceptionCode : 0,
                   echo::wideToUtf8(path).c_str());
    echo::log::shutdown();
    return EXCEPTION_EXECUTE_HANDLER;
}

LRESULT WINAPI wndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wParam, lParam)) return true;
    switch (msg) {
    case WM_SIZE:
        if (wParam == SIZE_MINIMIZED) return 0;
        g_resizeW = LOWORD(lParam);
        g_resizeH = HIWORD(lParam);
        return 0;
    case WM_GETMINMAXINFO: {
        auto* mmi = reinterpret_cast<MINMAXINFO*>(lParam);
        const UINT dpi = GetDpiForWindow(hwnd);
        mmi->ptMinTrackSize.x = MulDiv(900, dpi, 96);
        mmi->ptMinTrackSize.y = MulDiv(600, dpi, 96);
        return 0;
    }
    case WM_DPICHANGED: {
        const RECT* r = reinterpret_cast<RECT*>(lParam);
        SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
        if (g_app) g_app->setDpiScale(HIWORD(wParam) / 96.0f);
        return 0;
    }
    case WM_POWERBROADCAST:
        if (wParam == PBT_APMRESUMEAUTOMATIC || wParam == PBT_APMRESUMESUSPEND) {
            // Audio devices are often re-created after sleep; restart cleanly.
            g_resumePending = true;
            g_resumeAt = nowSeconds() + 2.0;
        }
        return TRUE;
    case WM_SYSCOMMAND:
        if ((wParam & 0xfff0) == SC_KEYMENU) return 0; // no ALT application menu
        break;
    case WM_CLOSE:
        if (g_app && g_app->closeToTray()) {
            if (!g_trayAdded) trayAdd(hwnd);
            if (g_trayAdded) {
                ShowWindow(hwnd, SW_HIDE);
                trayNotice(hwnd);
                return 0;
            }
        }
        if (g_app) g_app->requestQuit();
        return 0;
    case WM_ECHO_TRAY:
        switch (LOWORD(lParam)) {
        case NIN_SELECT:
        case NIN_KEYSELECT:
        case WM_LBUTTONDBLCLK:
            showMainWindow(hwnd);
            break;
        case WM_CONTEXTMENU:
            trayMenu(hwnd, static_cast<short>(LOWORD(wParam)), static_cast<short>(HIWORD(wParam)));
            break;
        default:
            break;
        }
        return 0;
    case WM_SETTINGCHANGE:
        if (lParam && g_app && wcscmp(reinterpret_cast<const wchar_t*>(lParam), L"ImmersiveColorSet") == 0)
            g_app->onSystemThemeChanged();
        break;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    if (msg == g_taskbarCreated && g_trayAdded) {
        // Explorer restarted: the icon has to be added again.
        g_trayAdded = false;
        trayAdd(hwnd);
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

} // namespace

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, LPWSTR cmdLine, int showCmd) {
    const std::wstring args = cmdLine ? cmdLine : L"";
    bool safeMode = args.find(L"--safe-mode") != std::wstring::npos;
    const bool resetSettings = args.find(L"--reset-settings") != std::wstring::npos;
    const bool startMinimized = args.find(L"--minimized") != std::wstring::npos;
    std::wstring snapshotDir;
    if (const size_t pos = args.find(L"--ui-snapshots="); pos != std::wstring::npos) {
        snapshotDir = args.substr(pos + 15);
        if (const size_t sp = snapshotDir.find(L' '); sp != std::wstring::npos) snapshotDir.resize(sp);
        std::error_code e2;
        std::filesystem::create_directories(snapshotDir + L"\\profile", e2);
        // Use a throwaway profile so the user's real settings are untouched.
        echo::paths::overrideRoot(snapshotDir + L"\\profile");
    }

    // Single instance: bring the existing window to the front instead.
    HANDLE mutex = CreateMutexW(nullptr, TRUE, L"Local\\EchoMixer.SingleInstance." ECHO_APP_ID_W);
    if (GetLastError() == ERROR_ALREADY_EXISTS && snapshotDir.empty()) {
        if (HWND other = FindWindowW(kWindowClass, nullptr)) showMainWindow(other); // also when in the tray
        return 0;
    }

    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    SetUnhandledExceptionFilter(crashHandler);
    echo::log::init(echo::paths::logFile());
    ECHO_LOG_INFO("---- %s %s starting ----", ECHO_APP_NAME, ECHO_VERSION_STRING);

    if (resetSettings) {
        std::error_code ec;
        std::filesystem::rename(echo::paths::sessionFile(), echo::paths::sessionFile() + L".reset-backup", ec);
        ECHO_LOG_INFO("Settings reset requested from the command line");
    }

    // Crash recovery: a leftover marker means the last run did not exit cleanly.
    const std::wstring marker = echo::paths::crashMarkerFile();
    std::error_code ec;
    if (!safeMode && snapshotDir.empty() && std::filesystem::exists(marker, ec)) {
        const int r = MessageBoxW(nullptr,
                                  ECHO_APP_NAME_W L" did not close normally last time.\n\n"
                                  L"Start in safe mode? (Effects are bypassed and audio is not started automatically, "
                                  L"so you can fix a problematic setting.)",
                                  ECHO_APP_NAME_W, MB_YESNO | MB_ICONWARNING);
        safeMode = r == IDYES;
        ECHO_LOG_WARN("Previous session ended unexpectedly; safe mode %s", safeMode ? "on" : "off");
    }
    if (FILE* f = _wfopen(marker.c_str(), L"wb")) fclose(f);

    ImGui_ImplWin32_EnableDpiAwareness();
    const float dpi = ImGui_ImplWin32_GetDpiScaleForMonitor(MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY));

    // Window placement from the saved session.
    int wx = CW_USEDEFAULT, wy = CW_USEDEFAULT, ww = static_cast<int>(1480 * dpi), wh = static_cast<int>(900 * dpi);
    bool maximized = false;
    bool closeToTray = false;
    {
        echo::json j;
        if (echo::loadJsonFile(echo::paths::sessionFile(), j, nullptr) && j.contains("ui")) {
            const auto& u = j["ui"];
            const int x = u.value("windowX", -1), y = u.value("windowY", -1);
            const int w = u.value("windowW", 0), h = u.value("windowH", 0);
            if (w > 400 && h > 300) {
                ww = w;
                wh = h;
            }
            RECT r{x, y, x + ww, y + wh};
            if (x != -1 && MonitorFromRect(&r, MONITOR_DEFAULTTONULL)) {
                wx = x;
                wy = y;
            }
            maximized = u.value("maximized", false);
            closeToTray = u.value("closeToTray", false);
        }
    }
    g_taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");

    WNDCLASSEXW wc{sizeof(wc)};
    wc.style = CS_CLASSDC;
    wc.lpfnWndProc = wndProc;
    wc.hInstance = hInstance;
    wc.hIcon = LoadIconW(hInstance, MAKEINTRESOURCEW(1));
    wc.hIconSm = wc.hIcon;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kWindowClass;
    RegisterClassExW(&wc);
    HWND hwnd = CreateWindowExW(0, kWindowClass, ECHO_APP_NAME_W, WS_OVERLAPPEDWINDOW, wx, wy, ww, wh, nullptr, nullptr,
                                hInstance, nullptr);
    // Dark title bar to match the theme (Windows 10 2004+).
    BOOL dark = TRUE;
    DwmSetWindowAttribute(hwnd, 20 /*DWMWA_USE_IMMERSIVE_DARK_MODE*/, &dark, sizeof(dark));

    if (!createDevice(hwnd)) {
        cleanupDevice();
        MessageBoxW(hwnd, L"Could not initialise Direct3D 11. Please update your graphics driver.", ECHO_APP_NAME_W,
                    MB_ICONERROR);
        return 1;
    }
    if (startMinimized && closeToTray && snapshotDir.empty()) {
        // Started with Windows and set to live in the tray: no window yet.
        trayAdd(hwnd);
        if (!g_trayAdded) ShowWindow(hwnd, SW_SHOWMINIMIZED);
    } else {
        ShowWindow(hwnd, startMinimized || showCmd == SW_SHOWMINIMIZED ? SW_SHOWMINIMIZED
                         : maximized                                    ? SW_SHOWMAXIMIZED
                                                                        : SW_SHOWNORMAL);
    }
    UpdateWindow(hwnd);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_DockingEnable;
    static std::string iniPath = echo::wideToUtf8(echo::paths::imguiIniFile());
    io.IniFilename = iniPath.c_str();
    io.ConfigWindowsMoveFromTitleBarOnly = true;
    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(g_device, g_context);
    echo::ui::loadFonts();

    echo::ui::EchoApp app;
    g_app = &app;
    std::string err;
    const float windowDpi = GetDpiForWindow(hwnd) / 96.0f;
    echo::ui::applyTheme(0, windowDpi, 1.0f);
    if (!app.init(hwnd, g_device, safeMode, &err)) {
        MessageBoxW(hwnd, (L"Audio system initialisation failed:\n" + echo::utf8ToWide(err)).c_str(), ECHO_APP_NAME_W,
                    MB_ICONERROR);
        ECHO_LOG_ERROR("Init failed: %s", err.c_str());
    }
    app.setDpiScale(windowDpi);
    if (!snapshotDir.empty()) app.startSnapshots(snapshotDir);

    double last = nowSeconds();
    // Frame pacing: Present(1) alone would draw at the monitor's refresh rate
    // (144 Hz+ on many screens). Meters and controls do not need more than
    // 60 fps in front and 30 fps behind other windows.
    HANDLE frameTimer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    if (!frameTimer) frameTimer = CreateWaitableTimerW(nullptr, FALSE, nullptr);
    double nextFrame = last;
    double fpsWindowStart = last, frameMsSum = 0.0;
    int fpsFrames = 0;
    bool fpsLogged = false;
    bool done = false;
    while (!done) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
            if (msg.message == WM_QUIT) done = true;
        }
        if (done || app.quitRequested()) break;
        const double now = nowSeconds();
        const double dt = now - last;
        last = now;
        if (g_resumePending && now >= g_resumeAt) {
            g_resumePending = false;
            app.onResume();
        }
        // The tray icon exists while "keep running when closed" is on.
        if (app.closeToTray() && !g_trayAdded) trayAdd(hwnd);
        else if (!app.closeToTray() && g_trayAdded && IsWindowVisible(hwnd)) trayRemove(hwnd);
        if (!IsWindowVisible(hwnd) || IsIconic(hwnd) ||
            (g_occluded && g_swapChain->Present(0, DXGI_PRESENT_TEST) == DXGI_STATUS_OCCLUDED)) {
            // Not visible: keep device monitoring and autosave alive, skip drawing.
            app.idle(now);
            Sleep(30);
            continue;
        }
        g_occluded = false;
        if (g_resizeW && g_resizeH) {
            cleanupRenderTarget();
            g_swapChain->ResizeBuffers(0, g_resizeW, g_resizeH, DXGI_FORMAT_UNKNOWN, 0);
            g_resizeW = g_resizeH = 0;
            createRenderTarget();
        }
        // Wait for this frame's slot (input is handled on the next pass).
        {
            const bool foreground = GetForegroundWindow() == hwnd;
            const double interval = foreground ? 1.0 / 60.0 : 1.0 / 30.0;
            const double t = nowSeconds();
            if (nextFrame > t + interval) nextFrame = t; // clock jump / long stall
            if (nextFrame > t && frameTimer) {
                LARGE_INTEGER due;
                due.QuadPart = -static_cast<LONGLONG>((nextFrame - t) * 1e7);
                SetWaitableTimer(frameTimer, &due, 0, nullptr, nullptr, FALSE);
                const DWORD w = MsgWaitForMultipleObjects(1, &frameTimer, FALSE, 100, QS_ALLINPUT);
                // Woken by input before this frame's slot: handle the message
                // (back to the pump) and keep waiting, instead of drawing early.
                if (w == WAIT_OBJECT_0 + 1 && nowSeconds() < nextFrame - 0.0005) continue;
            }
            nextFrame = std::max(nextFrame + interval, nowSeconds());
        }
        const double frameStart = nowSeconds();
        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
        app.frame(now, dt);
        ImGui::Render();
        const ImVec4 bg = echo::ui::toVec4(echo::ui::pal().bg);
        const float clear[4] = {bg.x, bg.y, bg.z, 1.0f};
        g_context->OMSetRenderTargets(1, &g_rtv, nullptr);
        g_context->ClearRenderTargetView(g_rtv, clear);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        if (const std::wstring shot = app.takeCaptureRequest(); !shot.empty()) {
            if (!saveBackbufferPng(shot)) ECHO_LOG_WARN("Snapshot failed: %s", echo::wideToUtf8(shot).c_str());
        }
        frameMsSum += (nowSeconds() - frameStart) * 1000.0; // build + draw, before vsync wait
        const HRESULT hr = g_swapChain->Present(1, 0);
        g_occluded = hr == DXGI_STATUS_OCCLUDED;
        ++fpsFrames;
        if (!fpsLogged && nowSeconds() - fpsWindowStart >= 10.0) {
            fpsLogged = true;
            ECHO_LOG_INFO("UI: %.1f frames/s, %.2f ms per frame to build and draw",
                          fpsFrames / (nowSeconds() - fpsWindowStart), frameMsSum / std::max(1, fpsFrames));
        }
        static bool firstFrame = true;
        if (firstFrame) {
            firstFrame = false;
            // Start-up time: process creation -> first frame on screen.
            FILETIME created{}, exited{}, kernel{}, user{}, nowFt{};
            if (GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)) {
                GetSystemTimeAsFileTime(&nowFt);
                const auto ticks = [](const FILETIME& f) {
                    return (static_cast<uint64_t>(f.dwHighDateTime) << 32) | f.dwLowDateTime;
                };
                ECHO_LOG_INFO("First frame shown %.0f ms after process start",
                              (ticks(nowFt) - ticks(created)) / 1e4);
            }
        }
    }

    if (frameTimer) CloseHandle(frameTimer);
    trayRemove(hwnd);

    // Persist window placement.
    WINDOWPLACEMENT wp{sizeof(wp)};
    if (GetWindowPlacement(hwnd, &wp)) {
        auto& ui = app.uiSettings();
        ui.maximized = wp.showCmd == SW_SHOWMAXIMIZED;
        ui.windowX = wp.rcNormalPosition.left;
        ui.windowY = wp.rcNormalPosition.top;
        ui.windowW = wp.rcNormalPosition.right - wp.rcNormalPosition.left;
        ui.windowH = wp.rcNormalPosition.bottom - wp.rcNormalPosition.top;
    }
    app.shutdown();
    g_app = nullptr;
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    cleanupDevice();
    DestroyWindow(hwnd);
    UnregisterClassW(kWindowClass, hInstance);
    std::filesystem::remove(marker, ec);
    ECHO_LOG_INFO("---- %s exited cleanly ----", ECHO_APP_NAME);
    echo::log::shutdown();
    CoUninitialize();
    if (mutex) CloseHandle(mutex);
    return 0;
}
