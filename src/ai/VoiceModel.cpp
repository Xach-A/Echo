#include "ai/VoiceModel.h"

#include <algorithm>
#include <filesystem>

#include "ai/RvcVoice.h"
#include "core/Log.h"
#include "core/Text.h"
#include "session/Serialization.h"

namespace echo::ai {

bool loadManifest(const std::wstring& dir, ModelManifest& m) {
    namespace fs = std::filesystem;
    m.dir = dir;
    m.id = wideToUtf8(fs::path(dir).filename().wstring());
    std::error_code ec0;
    if (!fs::exists(fs::path(dir) / L"model.json", ec0)) return loadRvcManifest(dir, m); // an RVC voice folder
    json j;
    std::string err;
    if (!loadJsonFile((fs::path(dir) / L"model.json").wstring(), j, &err, false)) {
        m.error = "model.json: " + err;
        return false;
    }
    if (j.value("format", "") != "echo.voice-model") {
        m.error = "model.json is not an Echo voice model manifest";
        return false;
    }
    m.name = j.value("name", m.id);
    m.author = j.value("author", "");
    m.license = j.value("license", "");
    m.source = j.value("source", "");
    m.description = j.value("description", "");
    m.consent = j.value("consent", "");
    m.version = j.value("model_version", "");
    m.sampleRate = std::clamp(j.value("sample_rate", 16000), 8000, 96000);
    m.chunkMs = std::clamp(j.value("chunk_ms", 200), 20, 2000);
    m.contextMs = std::clamp(j.value("context_ms", 100), 0, 2000);
    m.crossfadeMs = std::clamp(j.value("crossfade_ms", 10), 0, 100);
    m.inputName = j.value("input", "audio");
    m.outputName = j.value("output", "audio_out");
    m.pitchInputName = j.value("pitch_input", "");
    const std::string onnx = j.value("onnx", "model.onnx");
    m.onnxPath = (fs::path(dir) / utf8ToWide(onnx)).wstring();
    std::error_code ec;
    if (!fs::exists(m.onnxPath, ec)) {
        m.error = "ONNX file '" + onnx + "' not found";
        return false;
    }
    m.sizeBytes = fs::file_size(m.onnxPath, ec);
    if (m.license.empty()) {
        m.error = "Manifest has no license field; Echo only loads models with stated licensing";
        return false;
    }
    m.valid = true;
    return true;
}

std::vector<ModelManifest> scanModels(const std::wstring& modelsDir) {
    std::vector<ModelManifest> out;
    namespace fs = std::filesystem;
    std::error_code ec;
    if (!fs::exists(modelsDir, ec)) return out;
    for (const auto& e : fs::directory_iterator(modelsDir, ec)) {
        if (!e.is_directory()) continue;
        const std::wstring dir = e.path().wstring();
        const std::string folder = wideToUtf8(e.path().filename().wstring());
        std::vector<ModelManifest> voices;
        if (!fs::exists(e.path() / L"model.json", ec)) voices = findRvcVoices(dir, true);
        if (voices.size() <= 1) { // an Echo package, one RVC voice, or what is missing
            ModelManifest m;
            loadManifest(dir, m);
            out.push_back(std::move(m));
            continue;
        }
        // Several voices in one folder (a pack, or one voice saved at several
        // training steps): one entry each, named apart by their file names.
        for (auto& v : voices) {
            const std::string file = wideToUtf8(fs::path(v.pthPath).stem().wstring());
            v.id = folder + "/" + file;
            if (v.name.empty()) v.name = folder;
        }
        std::vector<bool> shared(voices.size());
        for (size_t k = 0; k < voices.size(); ++k)
            shared[k] = std::count_if(voices.begin(), voices.end(), [&](const ModelManifest& o) { return o.name == voices[k].name; }) > 1;
        for (size_t k = 0; k < voices.size(); ++k)
            if (shared[k]) voices[k].name += " (" + wideToUtf8(fs::path(voices[k].pthPath).stem().wstring()) + ")";
        for (auto& v : voices) out.push_back(std::move(v));
    }
    // Voices put straight into the voices folder, without a folder of their own.
    for (auto& v : findRvcVoices(modelsDir, false)) {
        if (!v.valid) continue;
        v.id = wideToUtf8(fs::path(v.pthPath).filename().wstring());
        if (v.name.empty()) v.name = wideToUtf8(fs::path(v.pthPath).stem().wstring());
        out.push_back(std::move(v));
    }
    std::sort(out.begin(), out.end(), [](const ModelManifest& a, const ModelManifest& b) { return a.name < b.name; });
    return out;
}

OnnxVoiceModel::~OnnxVoiceModel() {
    OrtRuntime& rt = OrtRuntime::instance();
    if (!rt.loaded()) return;
    const OrtApi* api = rt.api();
    if (session_) api->ReleaseSession(session_);
    if (opts_) api->ReleaseSessionOptions(opts_);
    if (mem_) api->ReleaseMemoryInfo(mem_);
}

bool OnnxVoiceModel::load(const ModelManifest& m, bool preferGpu, std::string* error) {
    OrtRuntime& rt = OrtRuntime::instance();
    if (!rt.ensureLoaded(error)) return false;
    const OrtApi* api = rt.api();
    manifest_ = m;
    if (!rt.check(api->CreateSessionOptions(&opts_), error)) return false;
    api->SetSessionGraphOptimizationLevel(opts_, ORT_ENABLE_ALL);
    api->SetIntraOpNumThreads(opts_, 2); // leave CPU for the audio engine
    provider_ = "CPU";
    if (preferGpu) {
        const std::string gpu = rt.addGpu(opts_);
        if (!gpu.empty()) provider_ = gpu;
    }
    if (!rt.check(api->CreateSession(rt.env(), m.onnxPath.c_str(), opts_, &session_), error)) return false;
    if (!rt.check(api->CreateCpuMemoryInfo(OrtArenaAllocator, OrtMemTypeDefault, &mem_), error)) return false;

    // Validate the I/O contract.
    OrtAllocator* alloc = nullptr;
    api->GetAllocatorWithDefaultOptions(&alloc);
    size_t nIn = 0, nOut = 0;
    api->SessionGetInputCount(session_, &nIn);
    api->SessionGetOutputCount(session_, &nOut);
    bool hasIn = false, hasOut = false, hasPitch = m.pitchInputName.empty();
    for (size_t i = 0; i < nIn; ++i) {
        char* name = nullptr;
        if (api->SessionGetInputName(session_, i, alloc, &name)) continue;
        if (m.inputName == name) hasIn = true;
        if (!m.pitchInputName.empty() && m.pitchInputName == name) hasPitch = true;
        api->AllocatorFree(alloc, name);
    }
    for (size_t i = 0; i < nOut; ++i) {
        char* name = nullptr;
        if (api->SessionGetOutputName(session_, i, alloc, &name)) continue;
        if (m.outputName == name) hasOut = true;
        api->AllocatorFree(alloc, name);
    }
    if (!hasIn || !hasOut || !hasPitch) {
        if (error)
            *error = "Model does not match its manifest (expected input '" + m.inputName + "', output '" + m.outputName +
                     "'" + (m.pitchInputName.empty() ? "" : ", pitch input '" + m.pitchInputName + "'") + ")";
        return false;
    }
    ECHO_LOG_INFO("Voice model '%s' loaded (%s, %d Hz)", m.name.c_str(), provider_.c_str(), m.sampleRate);
    return true;
}

bool OnnxVoiceModel::run(const float* in, int n, float pitchSemis, std::vector<float>& out, std::string* error) {
    OrtRuntime& rt = OrtRuntime::instance();
    const OrtApi* api = rt.api();
    if (!session_) {
        if (error) *error = "no model";
        return false;
    }
    const int64_t shape[2] = {1, n};
    OrtValue* inputs[2] = {nullptr, nullptr};
    const char* inNames[2] = {manifest_.inputName.c_str(), manifest_.pitchInputName.c_str()};
    size_t nInputs = 1;
    if (!rt.check(api->CreateTensorWithDataAsOrtValue(mem_, const_cast<float*>(in), sizeof(float) * n, shape, 2,
                                                      ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &inputs[0]),
                  error))
        return false;
    float pitch = pitchSemis;
    const int64_t pshape[1] = {1};
    if (!manifest_.pitchInputName.empty()) {
        if (!rt.check(api->CreateTensorWithDataAsOrtValue(mem_, &pitch, sizeof(float), pshape, 1,
                                                          ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &inputs[1]),
                      error)) {
            api->ReleaseValue(inputs[0]);
            return false;
        }
        nInputs = 2;
    }
    const char* outNames[1] = {manifest_.outputName.c_str()};
    OrtValue* output = nullptr;
    const bool ok = rt.check(api->Run(session_, nullptr, inNames, inputs, nInputs, outNames, 1, &output), error);
    for (size_t i = 0; i < nInputs; ++i) api->ReleaseValue(inputs[i]);
    if (!ok) return false;
    OrtTensorTypeAndShapeInfo* info = nullptr;
    size_t count = 0;
    if (!api->GetTensorTypeAndShape(output, &info)) {
        api->GetTensorShapeElementCount(info, &count);
        api->ReleaseTensorTypeAndShapeInfo(info);
    }
    float* data = nullptr;
    if (!rt.check(api->GetTensorMutableData(output, reinterpret_cast<void**>(&data)), error) || !data) {
        api->ReleaseValue(output);
        return false;
    }
    out.assign(data, data + count);
    api->ReleaseValue(output);
    return true;
}

} // namespace echo::ai
