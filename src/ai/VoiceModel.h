// Voice model packages and ONNX inference.
//
// A model is a folder inside %APPDATA%\Echo\models holding either an RVC
// voice (a .pth file and optionally its .index), or an Echo package:
// model.json (manifest, see docs/AI_VOICE.md) and an ONNX file that follows
// the "echo.voice-model v1" I/O contract:
//   input  <input>       float32 [1, N]  mono audio at `sample_rate`
//   input  <pitch_input> float32 [1]     optional pitch shift in semitones
//   output <output>      float32 [1, N]  converted audio, same length/rate
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "ai/OrtRuntime.h"

namespace echo::ai {

struct ModelManifest {
    std::string kind = "onnx"; // "onnx" (Echo package) or "rvc"
    std::string id;           // folder name
    std::string name, author, license, source, description, consent, version;
    int sampleRate = 16000;
    int chunkMs = 200;        // audio per inference call
    int contextMs = 100;      // past audio prepended for continuity
    int crossfadeMs = 10;
    std::string inputName = "audio";
    std::string outputName = "audio_out";
    std::string pitchInputName; // empty = model takes no pitch input
    std::wstring dir, onnxPath;
    std::wstring pthPath, indexPath; // RVC voice and its (optional) index
    uint64_t sizeBytes = 0;
    bool valid = false;
    std::string error;
};

std::vector<ModelManifest> scanModels(const std::wstring& modelsDir);
bool loadManifest(const std::wstring& dir, ModelManifest& out);

// Something that converts a block of voice: input and output are mono audio
// at the model's sample rate, the same length.
class VoiceConverter {
public:
    virtual ~VoiceConverter() = default;
    // Runs one inference. `out` receives the model's output samples.
    virtual bool run(const float* in, int n, float pitchSemis, std::vector<float>& out, std::string* error) = 0;
    virtual const std::string& provider() const = 0;
    // How strongly the voice's index shapes the result (0..1; RVC only).
    virtual void setIndexRate(float) {}
};

class OnnxVoiceModel : public VoiceConverter {
public:
    ~OnnxVoiceModel() override;
    bool load(const ModelManifest& m, bool preferGpu, std::string* error);
    bool run(const float* in, int n, float pitchSemis, std::vector<float>& out, std::string* error) override;
    const std::string& provider() const override { return provider_; }

private:
    OrtSession* session_ = nullptr;
    OrtSessionOptions* opts_ = nullptr;
    OrtMemoryInfo* mem_ = nullptr;
    ModelManifest manifest_;
    std::string provider_ = "CPU";
};

} // namespace echo::ai
