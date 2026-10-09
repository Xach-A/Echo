#include "core/Log.h"

#include <chrono>
#include <cstdio>
#include <ctime>
#include <deque>
#include <filesystem>
#include <mutex>

namespace echo::log {

namespace {

struct State {
    std::mutex mutex;
    FILE* file = nullptr;
    std::wstring path;
    std::deque<Entry> ring;
    uint64_t count = 0;
    std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
};

State& state() {
    static State s;
    return s;
}

const char* levelName(Level l) {
    switch (l) {
    case Level::Debug: return "DEBUG";
    case Level::Info: return "INFO ";
    case Level::Warn: return "WARN ";
    case Level::Error: return "ERROR";
    }
    return "?";
}

} // namespace

void init(const std::wstring& filePath) {
    State& s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    if (s.file) {
        fclose(s.file);
        s.file = nullptr;
    }
    s.path = filePath;
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(filePath).parent_path(), ec);
    if (std::filesystem::exists(filePath, ec) && std::filesystem::file_size(filePath, ec) > 2 * 1024 * 1024) {
        std::filesystem::path old = filePath + L".1";
        std::filesystem::remove(old, ec);
        std::filesystem::rename(filePath, old, ec);
    }
    s.file = _wfopen(filePath.c_str(), L"ab");
}

void shutdown() {
    State& s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    if (s.file) {
        fclose(s.file);
        s.file = nullptr;
    }
}

void write(Level level, const char* fmt, ...) {
    char msg[2048];
    va_list args;
    va_start(args, fmt);
    vsnprintf(msg, sizeof(msg), fmt, args);
    va_end(args);

    State& s = state();
    const double t =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - s.start).count();

    const auto wall = std::chrono::system_clock::now();
    std::time_t now = std::chrono::system_clock::to_time_t(wall);
    std::tm tmv{};
    localtime_s(&tmv, &now);
    char stamp[40];
    const size_t len = std::strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", &tmv);
    const int ms = static_cast<int>(
        std::chrono::duration_cast<std::chrono::milliseconds>(wall.time_since_epoch()).count() % 1000);
    std::snprintf(stamp + len, sizeof(stamp) - len, ".%03d", ms);

    std::lock_guard<std::mutex> lock(s.mutex);
    if (s.file) {
        std::fprintf(s.file, "%s [%s] %s\n", stamp, levelName(level), msg);
        std::fflush(s.file);
    }
#ifndef NDEBUG
    std::fprintf(stderr, "[%s] %s\n", levelName(level), msg);
#endif
    s.ring.push_back(Entry{t, level, msg});
    while (s.ring.size() > 2000) s.ring.pop_front();
    ++s.count;
}

std::vector<Entry> recent(size_t maxEntries) {
    State& s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    const size_t n = std::min(maxEntries, s.ring.size());
    return std::vector<Entry>(s.ring.end() - static_cast<std::ptrdiff_t>(n), s.ring.end());
}

uint64_t entryCount() {
    State& s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    return s.count;
}

std::wstring filePath() {
    State& s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    return s.path;
}

} // namespace echo::log
