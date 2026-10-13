// Installs ONNX Runtime (Microsoft's AI engine, MIT) on the user's click: the
// DirectML build from nuget.org, which runs AI voices on the graphics card
// when it can and on the CPU otherwise. The package is kept only if it
// matches the pinned size and SHA-256 (checked against NuGet's published
// SHA-512 when pinned); its DLLs and license files go into Echo's ai folder.
#pragma once

#include <cstdint>
#include <string>

#include "audio/Download.h"

namespace echo::win {

inline constexpr const char* kOrtVersion = "1.24.4";
inline constexpr const char* kOrtUrl =
    "https://api.nuget.org/v3-flatcontainer/microsoft.ml.onnxruntime.directml/1.24.4/microsoft.ml.onnxruntime.directml.1.24.4.nupkg";
inline constexpr const char* kOrtSha256 = "57e9f11b73437bef7a309496135d4c1f96b1a8e9ddba60013fa27bfc1d788681";
inline constexpr uint64_t kOrtBytes = 12458649;

// Downloads, checks and unpacks the package and puts onnxruntime.dll,
// onnxruntime_providers_shared.dll and the license files into `aiDir`.
// Blocks; run it on a worker thread.
bool installOnnxRuntime(const std::wstring& aiDir, DownloadProgress& progress, std::string* error);

} // namespace echo::win
