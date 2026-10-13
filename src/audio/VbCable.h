// VB-CABLE, VB-Audio's free (donationware) virtual audio cable, installed on
// the user's request: Echo downloads VB-Audio's official driver pack, keeps it
// only if it matches the pinned size and SHA-256, unpacks it and opens
// VB-Audio's own installer, which asks Windows for administrator approval.
// The user installs the driver in VB-Audio's window; Echo never installs a
// driver itself, runs nothing silently and doesn't bundle the pack.
#pragma once

#include <cstdint>
#include <string>

#include "audio/Download.h"

namespace echo::win {

inline constexpr const char* kVbCableUrl = "https://download.vb-audio.com/Download_CABLE/VBCABLE_Driver_Pack45.zip";
inline constexpr const char* kVbCableSha256 = "b950e39f01af1d04ea623c8f6d8eb9b6ea5c477c637295fabf20631c85116bfb";
inline constexpr uint64_t kVbCableBytes = 1318877;
inline constexpr const wchar_t* kVbCablePage = L"https://vb-audio.com/Cable/";

// Downloads the pack into `folder` (reusing an already checked copy), unpacks
// it there and returns VB-Audio's installer for this PC. Blocks; run it on a
// worker thread.
bool prepareVbCable(const std::wstring& folder, DownloadProgress& progress, std::wstring* setupExe, std::string* error);

// Opens VB-Audio's installer as administrator (Windows asks the user first).
// False when it couldn't start or the user said no to Windows.
bool openVbCableSetup(const std::wstring& setupExe, std::string* error);

// Unpacks a .zip into `dir` with Windows' own tar.exe.
bool unzip(const std::wstring& zip, const std::wstring& dir, std::string* error);

} // namespace echo::win
