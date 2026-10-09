#include "ai/OrtRuntime.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <filesystem>

#include "core/Log.h"
#include "core/Text.h"
#include "session/Paths.h"

namespace echo::ai {

OrtRuntime& OrtRuntime::instance() {
    static OrtRuntime rt;
    return rt;
}

std::string OrtRuntime::lastError() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return error_;
}

std::vector<std::wstring> OrtRuntime::searchPaths() {
    std::vector<std::wstring> v;
    v.push_back(paths::executableDir() + L"\\ai\\onnxruntime.dll");
    v.push_back(paths::appDataDir() + L"\\ai\\onnxruntime.dll");
    wchar_t env[MAX_PATH * 2];
    const DWORD n = GetEnvironmentVariableW(L"ECHO_ONNXRUNTIME", env, MAX_PATH * 2);
    if (n > 0 && n < MAX_PATH * 2) v.emplace_back(env, n);
    return v;
}

bool OrtRuntime::check(OrtStatus* st, std::string* error) const {
    if (!st) return true;
    if (error) *error = api_ ? api_->GetErrorMessage(st) : "ONNX Runtime error";
    if (api_) api_->ReleaseStatus(st);
    return false;
}

bool OrtRuntime::ensureLoaded(std::string* error) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (api_) return true;
    std::wstring found;
    std::error_code ec;
    for (const auto& p : searchPaths())
        if (!p.empty() && std::filesystem::exists(p, ec)) {
            found = p;
            break;
        }
    if (found.empty()) {
        error_ = "ONNX Runtime is not installed (onnxruntime.dll not found in the 'ai' folder)";
        if (error) *error = error_;
        return false;
    }
    // Load with the DLL's own folder in the search path for its dependencies.
    HMODULE mod = LoadLibraryExW(found.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!mod) {
        error_ = "Could not load " + wideToUtf8(found) + " (error " + std::to_string(GetLastError()) + ")";
        if (error) *error = error_;
        return false;
    }
    using GetApiBaseFn = const OrtApiBase*(ORT_API_CALL*)();
    auto getBase = reinterpret_cast<GetApiBaseFn>(reinterpret_cast<void*>(GetProcAddress(mod, "OrtGetApiBase")));
    const OrtApiBase* base = getBase ? getBase() : nullptr;
    const OrtApi* api = base ? base->GetApi(ORT_API_VERSION) : nullptr;
    if (!api && base) {
        // Older runtime: try the oldest API version this code uses.
        for (uint32_t v = ORT_API_VERSION - 1; v >= 16 && !api; --v) api = base->GetApi(v);
    }
    if (!api) {
        FreeLibrary(mod);
        error_ = "onnxruntime.dll is too old or not a valid ONNX Runtime";
        if (error) *error = error_;
        return false;
    }
    OrtEnv* env = nullptr;
    OrtStatus* st = api->CreateEnv(ORT_LOGGING_LEVEL_WARNING, "echo", &env);
    if (st) {
        error_ = std::string("CreateEnv failed: ") + api->GetErrorMessage(st);
        api->ReleaseStatus(st);
        FreeLibrary(mod);
        if (error) *error = error_;
        return false;
    }
    // Echo does not collect telemetry; switch off ONNX Runtime's optional
    // trace events as early as the API allows.
    if (OrtStatus* t = api->DisableTelemetryEvents(env)) api->ReleaseStatus(t);
    module_ = mod;
    api_ = api;
    env_ = env;
    path_ = wideToUtf8(found);
    version_ = base->GetVersionString();
    char** names = nullptr;
    int count = 0;
    if (!api->GetAvailableProviders(&names, &count)) {
        for (int i = 0; i < count; ++i) providers_.push_back(names[i]);
        api->ReleaseAvailableProviders(names, count);
    }
    std::string list;
    for (const auto& p : providers_) list += p + " ";
    ECHO_LOG_INFO("ONNX Runtime %s loaded from %s (providers: %s)", version_.c_str(), path_.c_str(), list.c_str());
    error_.clear();
    return true;
}

} // namespace echo::ai
