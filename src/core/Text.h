// UTF-8 / UTF-16 conversion and small string helpers.
#pragma once

#include <cstdint>
#include <string>

namespace echo {

std::wstring utf8ToWide(const std::string& s);
std::string wideToUtf8(const std::wstring& w);
std::string formatBytes(uint64_t bytes);
std::string formatDuration(double seconds); // h:mm:ss or m:ss

} // namespace echo
