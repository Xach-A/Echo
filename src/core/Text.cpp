#include "core/Text.h"

#include <cstdint>
#include <cstdio>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace echo {

std::wstring utf8ToWide(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

std::string wideToUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

std::string formatBytes(uint64_t b) {
    char buf[32];
    if (b < 1024) std::snprintf(buf, sizeof(buf), "%llu B", static_cast<unsigned long long>(b));
    else if (b < 1024ull * 1024) std::snprintf(buf, sizeof(buf), "%.1f KB", b / 1024.0);
    else if (b < 1024ull * 1024 * 1024) std::snprintf(buf, sizeof(buf), "%.1f MB", b / (1024.0 * 1024.0));
    else std::snprintf(buf, sizeof(buf), "%.2f GB", b / (1024.0 * 1024.0 * 1024.0));
    return buf;
}

std::string formatDuration(double seconds) {
    const long long t = static_cast<long long>(seconds);
    char buf[32];
    if (t >= 3600) std::snprintf(buf, sizeof(buf), "%lld:%02lld:%02lld", t / 3600, (t / 60) % 60, t % 60);
    else std::snprintf(buf, sizeof(buf), "%lld:%02lld", t / 60, t % 60);
    return buf;
}

} // namespace echo
