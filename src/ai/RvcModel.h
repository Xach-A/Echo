// RVC voice models (Retrieval-based Voice Conversion, the .pth files made by
// the RVC WebUI and Applio): reads the checkpoint and builds the voice
// network as an ONNX graph for ONNX Runtime.
//
// The network (Applio rvc/lib/algorithm, MIT): the text encoder (speech
// features + pitch -> a latent, through 6 transformer layers with relative
// positions), the normalizing flow run backwards, and the NSF HiFi-GAN
// vocoder (a sine at the target pitch, shaped into the voice). Supported:
// v1 and v2 models with pitch guidance and the HiFi-GAN vocoder.
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "ai/TorchFile.h"

namespace echo::ai {

struct RvcInfo {
    std::string name, version, vocoder, embedder;
    int sampleRate = 0;
    bool f0 = true;
    int inter = 192, hidden = 192, filter = 768, heads = 2, layers = 6, kernel = 3;
    std::vector<int> rbKernels, upRates, upKernels;
    std::vector<std::vector<int>> rbDilations;
    int upInitial = 512, speakers = 1, gin = 256;
    int embDim = 768; // speech feature size: 768 (v2), 256 (v1)
    int hop = 0;      // output samples per 10 ms frame
};

class RvcModel {
public:
    bool load(const std::wstring& pth, std::string* error);
    const RvcInfo& info() const { return info_; }

    // The voice for `frames` 10 ms frames as an ONNX model. Inputs:
    //   phone  float [1, frames, embDim]  speech features (ContentVec)
    //   pitch  int64 [1, frames]          coarse pitch 1..255
    //   pitchf float [1, frames]          pitch in Hz (0 = unvoiced)
    //   rnd    float [1, inter, frames]   standard normal noise (latent)
    //   noise  float [1, frames*hop]      standard normal noise (source)
    // Output: audio float [1, frames*hop] at info().sampleRate.
    bool buildGenerator(int frames, int speaker, std::vector<uint8_t>& out, std::string* error) const;

private:
    struct W {
        std::vector<float> v;
        std::vector<int64_t> shape;
    };
    // A parameter: "<prefix>.weight" / ".bias", or a weight-normalized
    // weight (weight_g / weight_v, or the newer parametrizations) fused.
    bool param(const std::string& name, W& out) const;

    TorchCheckpoint ck_;
    std::map<std::string, TensorRef> tensors_;
    RvcInfo info_;
};

// RVC's coarse pitch: 1..255 on a mel scale between 50 and 1100 Hz.
int64_t rvcCoarsePitch(float hz);

} // namespace echo::ai
