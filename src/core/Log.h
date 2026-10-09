// Thread-safe application logger. Never call from a real-time audio thread:
// audio threads report problems through atomic counters, which the UI and
// diagnostics code turn into log lines.
#pragma once

#include <cstdarg>
#include <cstdint>
#include <string>
#include <vector>

namespace echo::log {

enum class Level { Debug, Info, Warn, Error };

// Opens (appends to) a log file. Rotates when the file exceeds ~2 MB.
void init(const std::wstring& filePath);
void shutdown();

void write(Level level, const char* fmt, ...)
#if defined(__GNUC__) || defined(__clang__)
    __attribute__((format(printf, 2, 3)))
#endif
    ;

#define ECHO_LOG_DEBUG(...) ::echo::log::write(::echo::log::Level::Debug, __VA_ARGS__)
#define ECHO_LOG_INFO(...) ::echo::log::write(::echo::log::Level::Info, __VA_ARGS__)
#define ECHO_LOG_WARN(...) ::echo::log::write(::echo::log::Level::Warn, __VA_ARGS__)
#define ECHO_LOG_ERROR(...) ::echo::log::write(::echo::log::Level::Error, __VA_ARGS__)

struct Entry {
    double time;      // seconds since logger start
    Level level;
    std::string text;
};

// Most recent entries (bounded), for the diagnostics view.
std::vector<Entry> recent(size_t maxEntries = 500);
uint64_t entryCount();
std::wstring filePath();

} // namespace echo::log
