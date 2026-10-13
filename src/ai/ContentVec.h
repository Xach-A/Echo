// ContentVec (a HuBERT speech encoder, MIT): turns 16 kHz speech into 50
// feature frames a second that describe what is said, not who says it. RVC
// voices are trained on these features. Echo reads the model's PyTorch
// weights (pytorch_model.bin, Hugging Face format) and builds it as an ONNX
// graph.
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "ai/TorchFile.h"

namespace echo::ai {

class ContentVecModel {
public:
    bool load(const std::wstring& bin, std::string* error);

    // Feature frames for `samples` samples of 16 kHz audio.
    static int framesFor(int samples);

    // The encoder for exactly `samples` samples. Input: audio float
    // [1, samples]. Output: features float [1, frames, dim()] from
    // transformer layer `layer` (counting from 1; 0 = the last, as RVC v2
    // uses; v1 uses layer 9 projected to 256 values, finalProj = true).
    bool buildGraph(int samples, int layer, bool finalProj, std::vector<uint8_t>& out, std::string* error) const;
    int dim() const { return dim_; }
    int layers() const { return layers_; }

private:
    struct W {
        std::vector<float> v;
        std::vector<int64_t> shape;
    };
    bool param(const std::string& name, W& out) const;

    TorchCheckpoint ck_;
    std::map<std::string, TensorRef> tensors_;
    int dim_ = 768, layers_ = 12;
};

} // namespace echo::ai
