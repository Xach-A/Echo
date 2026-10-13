#include "audio/OrtInstall.h"

#include <filesystem>

#include "audio/VbCable.h"

namespace echo::win {

bool installOnnxRuntime(const std::wstring& aiDir, DownloadProgress& progress, std::string* error) {
    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path work = fs::path(aiDir) / "download";
    fs::remove_all(work, ec);
    fs::create_directories(work, ec);
    const fs::path zip = work / "onnxruntime-directml.zip";
    bool ok = downloadFile(kOrtUrl, zip.wstring(), kOrtBytes, kOrtSha256, progress, error) &&
              unzip(zip.wstring(), (work / "pkg").wstring(), error);
    if (ok) {
        const fs::path lib = work / "pkg" / "runtimes" / "win-x64" / "native";
        const std::pair<fs::path, const char*> files[] = {
            {lib / "onnxruntime.dll", "onnxruntime.dll"},
            {lib / "onnxruntime_providers_shared.dll", "onnxruntime_providers_shared.dll"},
            {work / "pkg" / "LICENSE", "ONNXRUNTIME-LICENSE.txt"},
            {work / "pkg" / "ThirdPartyNotices.txt", "ONNXRUNTIME-ThirdPartyNotices.txt"},
        };
        for (const auto& [from, to] : files) {
            fs::copy_file(from, fs::path(aiDir) / to, fs::copy_options::overwrite_existing, ec);
            if (ec) {
                if (error) *error = "Couldn't put " + std::string(to) + " in place: " + ec.message();
                ok = false;
                break;
            }
        }
    }
    fs::remove_all(work, ec);
    return ok;
}

} // namespace echo::win
