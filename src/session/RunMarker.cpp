#include "session/RunMarker.h"

#include <windows.h>
#include <ntsecapi.h>

#include <cstdio>
#include <filesystem>

namespace echo::runmarker {

uint64_t signInTime() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return 0;
    TOKEN_STATISTICS stats{};
    DWORD len = 0;
    const bool ok = GetTokenInformation(token, TokenStatistics, &stats, sizeof(stats), &len) != FALSE;
    CloseHandle(token);
    if (!ok) return 0;
    PSECURITY_LOGON_SESSION_DATA data = nullptr;
    if (LsaGetLogonSessionData(&stats.AuthenticationId, &data) != 0 || !data) return 0;
    const uint64_t t = static_cast<uint64_t>(data->LogonTime.QuadPart);
    LsaFreeReturnBuffer(data);
    return t;
}

Previous check(const std::wstring& path, uint64_t signIn) {
    WIN32_FILE_ATTRIBUTE_DATA a{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &a)) return Previous::Clean;
    // The marker is written when Echo starts: older than this sign-in means
    // that run belonged to an earlier one.
    const uint64_t written = (static_cast<uint64_t>(a.ftLastWriteTime.dwHighDateTime) << 32) | a.ftLastWriteTime.dwLowDateTime;
    if (signIn != 0 && written < signIn) return Previous::EndedWithWindows;
    return Previous::Crashed;
}

void create(const std::wstring& path) {
    remove(path); // a fresh file: its time is this run's start
    if (FILE* f = _wfopen(path.c_str(), L"wb")) std::fclose(f);
}

void remove(const std::wstring& path) {
    std::error_code ec;
    std::filesystem::remove(path, ec);
}

} // namespace echo::runmarker
