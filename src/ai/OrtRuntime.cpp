#include "ai/OrtRuntime.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <dxgi1_6.h>

#include <cstdio>
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
    // ONNX Runtime 1.N offers API versions up to N: ask an older DLL for its
    // own version first (asking for a newer one prints an error).
    uint32_t apiVersion = ORT_API_VERSION;
    if (base && base->GetVersionString) {
        unsigned major = 0, minor = 0;
        if (std::sscanf(base->GetVersionString(), "%u.%u", &major, &minor) == 2 && major == 1 && minor >= 16 &&
            minor < apiVersion)
            apiVersion = minor;
    }
    const OrtApi* api = base ? base->GetApi(apiVersion) : nullptr;
    while (!api && base && apiVersion > 16) api = base->GetApi(--apiVersion);
    if (!api) {
        FreeLibrary(mod);
        error_ = "onnxruntime.dll is too old or not a valid ONNX Runtime";
        if (error) *error = error_;
        return false;
    }
    OrtEnv* env = nullptr;
    // ECHO_ORT_VERBOSE=1 shows ONNX Runtime's own detailed log (debugging).
    const bool verbose = GetEnvironmentVariableW(L"ECHO_ORT_VERBOSE", nullptr, 0) > 0;
    OrtStatus* st = api->CreateEnv(verbose ? ORT_LOGGING_LEVEL_VERBOSE : ORT_LOGGING_LEVEL_WARNING, "echo", &env);
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
    apiVersion_ = apiVersion;
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

bool OrtRuntime::hasGpu() const {
    for (const auto& p : providers_)
        if (p == "DmlExecutionProvider") return true;
    return false;
}

namespace {
// The graphics card Windows ranks fastest (what DirectML picks with the
// high-performance preference), e.g. "NVIDIA GeForce RTX 4070", and its own
// memory in bytes.
std::string fastestGpuName(uint64_t* dedicatedMemory = nullptr) {
    if (dedicatedMemory) *dedicatedMemory = 0;
    IDXGIFactory6* factory = nullptr;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory6), reinterpret_cast<void**>(&factory)))) return {};
    std::string name;
    IDXGIAdapter1* adapter = nullptr;
    if (SUCCEEDED(factory->EnumAdapterByGpuPreference(0, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, __uuidof(IDXGIAdapter1),
                                                      reinterpret_cast<void**>(&adapter)))) {
        DXGI_ADAPTER_DESC1 d{};
        if (SUCCEEDED(adapter->GetDesc1(&d)) && !(d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) {
            name = wideToUtf8(d.Description);
            if (dedicatedMemory) *dedicatedMemory = d.DedicatedVideoMemory;
        }
        adapter->Release();
    }
    factory->Release();
    return name;
}
} // namespace

bool OrtRuntime::gpuRecommended() const {
    if (!hasGpu()) return false;
    uint64_t memory = 0;
    fastestGpuName(&memory);
    return memory >= (1ull << 30);
}

std::string OrtRuntime::addGpu(OrtSessionOptions* opts) const {
    if (!api_ || !hasGpu()) return {};
    const void* dmlApi = nullptr;
    std::string e;
    if (!check(api_->GetExecutionProviderApi("DML", apiVersion_, &dmlApi), &e) || !dmlApi) {
        ECHO_LOG_WARN("DirectML unavailable, using CPU: %s", e.c_str());
        return {};
    }
    // DirectML needs these (it runs one operator at a time on its queue).
    api_->DisableMemPattern(opts);
    api_->SetSessionExecutionMode(opts, ORT_SEQUENTIAL);
    auto* dml = static_cast<const OrtDmlApiPrefix*>(dmlApi);
    bool ok;
    if (apiVersion_ >= 17 && dml->SessionOptionsAppendExecutionProvider_DML2) {
        OrtDmlDeviceOptionsAbi o; // high performance, graphics cards
        ok = check(dml->SessionOptionsAppendExecutionProvider_DML2(opts, &o), &e);
    } else {
        ok = check(dml->SessionOptionsAppendExecutionProvider_DML(opts, 0), &e);
    }
    if (!ok) {
        ECHO_LOG_WARN("DirectML unavailable, using CPU: %s", e.c_str());
        return {};
    }
    const std::string gpu = fastestGpuName();
    return gpu.empty() ? "DirectML (graphics card)" : "DirectML on " + gpu;
}

} // namespace echo::ai
