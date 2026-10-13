// "Did the last run close normally?": the marker Echo keeps while it runs,
// and telling a crash apart from a run that ended with Windows.
#include "doctest/doctest.h"

#include <windows.h>

#include <filesystem>

#include "session/RunMarker.h"

using namespace echo;

namespace {
uint64_t fileTimeNow() {
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    return (static_cast<uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
}
constexpr uint64_t kHour = 36000000000ull; // in 100 ns
} // namespace

TEST_CASE("run marker: a crash, a run that ended with Windows, and a clean exit are told apart") {
    const std::wstring path = (std::filesystem::temp_directory_path() / "echo-test-running.marker").wstring();
    runmarker::remove(path);
    CHECK(runmarker::check(path, fileTimeNow()) == runmarker::Previous::Clean);
    runmarker::create(path);
    // Left in this sign-in (it began an hour ago): Echo crashed.
    CHECK(runmarker::check(path, fileTimeNow() - kHour) == runmarker::Previous::Crashed);
    // Left before this sign-in began: the PC shut down, restarted or lost power.
    CHECK(runmarker::check(path, fileTimeNow() + kHour) == runmarker::Previous::EndedWithWindows);
    // Sign-in time unknown: a leftover marker counts as a crash.
    CHECK(runmarker::check(path, 0) == runmarker::Previous::Crashed);
    runmarker::remove(path);
    CHECK(runmarker::check(path, fileTimeNow()) == runmarker::Previous::Clean);
}

TEST_CASE("run marker: the current Windows sign-in time is known and in the past") {
    const uint64_t t = runmarker::signInTime();
    CHECK(t > 0);
    CHECK(t <= fileTimeNow());
}
