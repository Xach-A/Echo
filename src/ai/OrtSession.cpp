#include "ai/OrtSession.h"

#include "core/Log.h"

namespace echo::ai {

OrtGraphSession::~OrtGraphSession() {
    OrtRuntime& rt = OrtRuntime::instance();
    if (!rt.loaded()) return;
    const OrtApi* api = rt.api();
    if (session_) api->ReleaseSession(session_);
    if (opts_) api->ReleaseSessionOptions(opts_);
    if (mem_) api->ReleaseMemoryInfo(mem_);
}

bool OrtGraphSession::create(const std::vector<uint8_t>& model, bool preferGpu, int threads, std::string* error) {
    OrtRuntime& rt = OrtRuntime::instance();
    if (!rt.ensureLoaded(error)) return false;
    const OrtApi* api = rt.api();
    if (!rt.check(api->CreateSessionOptions(&opts_), error)) return false;
    api->SetSessionGraphOptimizationLevel(opts_, ORT_ENABLE_ALL);
    if (threads > 0) api->SetIntraOpNumThreads(opts_, threads);
    // Between blocks the threads sleep instead of spinning: the CPU is
    // shared with the audio engine and the game.
    if (OrtStatus* st = api->AddSessionConfigEntry(opts_, "session.intra_op.allow_spinning", "0")) api->ReleaseStatus(st);
    provider_ = "CPU";
    if (preferGpu) {
        const std::string gpu = rt.addGpu(opts_);
        if (!gpu.empty()) provider_ = gpu;
    }
    if (!rt.check(api->CreateSessionFromArray(rt.env(), model.data(), model.size(), opts_, &session_), error)) return false;
    return rt.check(api->CreateCpuMemoryInfo(OrtArenaAllocator, OrtMemTypeDefault, &mem_), error);
}

bool OrtGraphSession::run(const std::vector<Input>& inputs, const std::vector<const char*>& outputs,
                          std::vector<std::vector<float>>& results, std::string* error) {
    OrtRuntime& rt = OrtRuntime::instance();
    const OrtApi* api = rt.api();
    if (!session_) {
        if (error) *error = "No session";
        return false;
    }
    std::vector<OrtValue*> in(inputs.size(), nullptr), out(outputs.size(), nullptr);
    std::vector<const char*> names;
    bool ok = true;
    for (size_t k = 0; k < inputs.size() && ok; ++k) {
        const Input& i = inputs[k];
        int64_t count = 1;
        for (int64_t d : i.shape) count *= d;
        const size_t elem = i.type == OnnxType::Int64 ? 8 : 4;
        const ONNXTensorElementDataType t =
            i.type == OnnxType::Int64 ? ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64 : ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT;
        ok = rt.check(api->CreateTensorWithDataAsOrtValue(mem_, const_cast<void*>(i.data), static_cast<size_t>(count) * elem,
                                                          i.shape.data(), i.shape.size(), t, &in[k]),
                      error);
        names.push_back(i.name);
    }
    if (ok) ok = rt.check(api->Run(session_, nullptr, names.data(), in.data(), in.size(), outputs.data(), outputs.size(), out.data()), error);
    if (ok) {
        results.resize(outputs.size());
        for (size_t k = 0; k < outputs.size() && ok; ++k) {
            OrtTensorTypeAndShapeInfo* info = nullptr;
            size_t count = 0;
            float* data = nullptr;
            ok = rt.check(api->GetTensorTypeAndShape(out[k], &info), error) &&
                 rt.check(api->GetTensorShapeElementCount(info, &count), error) &&
                 rt.check(api->GetTensorMutableData(out[k], reinterpret_cast<void**>(&data)), error);
            if (info) api->ReleaseTensorTypeAndShapeInfo(info);
            if (ok) results[k].assign(data, data + count);
        }
    }
    for (OrtValue* v : in)
        if (v) api->ReleaseValue(v);
    for (OrtValue* v : out)
        if (v) api->ReleaseValue(v);
    return ok;
}

} // namespace echo::ai
