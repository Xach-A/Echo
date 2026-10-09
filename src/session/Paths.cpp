#include "session/Paths.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shlobj.h>

#include <filesystem>

#include "Version.h"

namespace echo::paths {

namespace {
std::wstring g_root;

std::wstring known(REFKNOWNFOLDERID id) {
    PWSTR p = nullptr;
    std::wstring out;
    if (SUCCEEDED(SHGetKnownFolderPath(id, KF_FLAG_CREATE, nullptr, &p)) && p) out = p;
    CoTaskMemFree(p);
    return out;
}

std::wstring ensure(const std::wstring& dir) {
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    return dir;
}
} // namespace

void overrideRoot(const std::wstring& root) { g_root = root; }

std::wstring appDataDir() {
    if (!g_root.empty()) return ensure(g_root);
    return ensure(known(FOLDERID_RoamingAppData) + L"\\" + ECHO_APP_ID_W);
}

std::wstring localDataDir() {
    if (!g_root.empty()) return ensure(g_root + L"\\local");
    return ensure(known(FOLDERID_LocalAppData) + L"\\" + ECHO_APP_ID_W);
}

std::wstring sessionFile() { return appDataDir() + L"\\session.json"; }
std::wstring presetsDir() { return ensure(appDataDir() + L"\\presets"); }
std::wstring modelsDir() { return ensure(appDataDir() + L"\\models"); }
std::wstring logFile() { return ensure(localDataDir() + L"\\logs") + L"\\echo.log"; }
std::wstring crashMarkerFile() { return localDataDir() + L"\\running.marker"; }
std::wstring imguiIniFile() { return appDataDir() + L"\\layout.ini"; }

std::wstring defaultRecordingsDir() {
    std::wstring music = known(FOLDERID_Music);
    if (music.empty()) music = known(FOLDERID_Documents);
    return music + L"\\" + ECHO_APP_NAME_W + L" Recordings";
}

std::wstring executableDir() {
    wchar_t buf[MAX_PATH * 2];
    const DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH * 2);
    std::wstring p(buf, n);
    const auto slash = p.find_last_of(L"\\/");
    return slash == std::wstring::npos ? p : p.substr(0, slash);
}

} // namespace echo::paths
