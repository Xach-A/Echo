// "Did the last run close normally?" for the safe-mode offer. A marker file
// exists while Echo runs and is removed when it exits, also when Windows shuts
// down, restarts or signs out. A marker left from an earlier Windows sign-in
// means that run ended with Windows (for example a battery that ran out), not
// that Echo crashed.
#pragma once

#include <cstdint>
#include <string>

namespace echo::runmarker {

enum class Previous { Clean, Crashed, EndedWithWindows };

// When the current Windows sign-in began (FILETIME, UTC); 0 if unknown.
uint64_t signInTime();

// What the marker left by the previous run says. With an unknown sign-in
// time a leftover marker counts as a crash.
Previous check(const std::wstring& path, uint64_t signIn);

void create(const std::wstring& path);
void remove(const std::wstring& path);

} // namespace echo::runmarker
