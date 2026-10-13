// Downloads one file over HTTPS (WinHTTP) and checks it before keeping it:
// exact size and SHA-256. Used only when the user asks for it (the ContentVec
// speech model for RVC voices).
#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>

namespace echo::win {

struct DownloadProgress {
    std::atomic<uint64_t> done{0}, total{0};
    std::atomic<bool> cancel{false};
};

// Saves `url` to `dest` (through dest + ".part", moved into place only when
// the size and SHA-256 match). Blocks; run it on a worker thread.
bool downloadFile(const std::string& url, const std::wstring& dest, uint64_t expectedSize, const std::string& sha256Hex,
                  DownloadProgress& progress, std::string* error);

// SHA-256 of a file, lowercase hex ("" if it can't be read).
std::string sha256File(const std::wstring& path);

} // namespace echo::win
