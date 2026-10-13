// An ONNX Runtime session for a model built in memory (OnnxGraph), with
// named float / int64 inputs and float outputs.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "ai/OnnxGraph.h"
#include "ai/OrtRuntime.h"

namespace echo::ai {

class OrtGraphSession {
public:
    ~OrtGraphSession();
    // threads: intra-op threads (0 = ONNX Runtime's default). preferGpu
    // uses DirectML when the installed ONNX Runtime has it.
    bool create(const std::vector<uint8_t>& model, bool preferGpu, int threads, std::string* error);
    bool loaded() const { return session_ != nullptr; }
    const std::string& provider() const { return provider_; }

    struct Input {
        const char* name;
        OnnxType type;
        std::vector<int64_t> shape;
        const void* data; // float or int64 values, row-major
    };
    bool run(const std::vector<Input>& inputs, const std::vector<const char*>& outputs, std::vector<std::vector<float>>& results,
             std::string* error);

private:
    OrtSession* session_ = nullptr;
    OrtSessionOptions* opts_ = nullptr;
    OrtMemoryInfo* mem_ = nullptr;
    std::string provider_ = "CPU";
};

} // namespace echo::ai
