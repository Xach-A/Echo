// Optional ONNX Runtime, loaded dynamically at runtime. Echo runs normally
// when it is absent; only the AI voice effect needs it.
//
// Search order (full paths only, never the system search path, so Windows'
// own System32\onnxruntime.dll is never picked up by accident):
//   1. <exe dir>\ai\onnxruntime.dll
//   2. %APPDATA%\Echo\ai\onnxruntime.dll
//   3. the path in the ECHO_ONNXRUNTIME environment variable
#pragma once

#include <mutex>
#include <string>
#include <vector>

#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wmacro-redefined"
#endif
#include "onnxruntime/include/onnxruntime_c_api.h"
#if defined(__clang__)
#pragma clang diagnostic pop
#endif

namespace echo::ai {

class OrtRuntime {
public:
    static OrtRuntime& instance();

    // Loads the DLL and creates the environment once. Thread-safe.
    bool ensureLoaded(std::string* error = nullptr);
    bool loaded() const { return api_ != nullptr; }
    const OrtApi* api() const { return api_; }
    OrtEnv* env() const { return env_; }
    std::string version() const { return version_; }
    std::string dllPath() const { return path_; }
    std::vector<std::string> providers() const { return providers_; }
    std::string lastError() const;
    static std::vector<std::wstring> searchPaths();

    // Converts an OrtStatus to a message and releases it. Returns true if OK.
    bool check(OrtStatus* st, std::string* error) const;

private:
    mutable std::mutex mutex_;
    void* module_ = nullptr;
    const OrtApi* api_ = nullptr;
    OrtEnv* env_ = nullptr;
    std::string version_, path_, error_;
    std::vector<std::string> providers_;
};

} // namespace echo::ai
