// An RVC voice running in real time: each block of the voice (recent context
// + new audio) goes through the steps RVC's own pipeline uses:
//   16 kHz -> ContentVec speech features (50 a second)
//          -> blended towards the voice's own features (.index, optional)
//          -> doubled to 100 a second, with the pitch (YIN) of each 10 ms
//          -> the voice network -> audio at the voice's sample rate.
// Both networks are built once for the block size (see RvcModel and
// ContentVecModel) and run with ONNX Runtime.
#pragma once

#include <random>
#include <string>
#include <vector>

#include "ai/FaissIndex.h"
#include "ai/OrtSession.h"
#include "ai/RvcModel.h"
#include "ai/VoiceModel.h"
#include "dsp/PitchDetector.h"

namespace echo::ai {

// Where Echo keeps the ContentVec speech model (shared by every RVC voice).
std::wstring contentVecPath();
// The ContentVec download (Applio's copy, MIT) and its checksum.
extern const char* const kContentVecUrl;
extern const char* const kContentVecSha256;
constexpr uint64_t kContentVecBytes = 378342945;

// The RVC voices in a folder, also in its subfolders when `subfolders` is
// set (downloads often nest them, e.g. weights/x.pth and logs/x/added_*.index):
// every .pth that is a playable voice (training checkpoints such as G_/D_
// files are skipped), each paired with the .index that fits it best (same
// folder, then a name containing the voice's, then "added_" over "trained_").
// Nothing playable: one manifest with the error (when a .pth was found) or none.
std::vector<ModelManifest> findRvcVoices(const std::wstring& dir, bool subfolders);

// An RVC voice folder: its first voice; fills the manifest.
bool loadRvcManifest(const std::wstring& dir, ModelManifest& m);

class RvcVoice : public VoiceConverter {
public:
    // Builds the networks for blocks of exactly `samples` samples at the
    // voice's rate (shorter or longer blocks are padded or cut). `encoder`:
    // the ContentVec file (default: contentVecPath()).
    bool load(const ModelManifest& m, bool preferGpu, int samples, std::string* error, const std::wstring& encoder = {});
    bool run(const float* in, int n, float pitchSemis, std::vector<float>& out, std::string* error) override;
    const std::string& provider() const override { return provider_; }
    void setIndexRate(float r) override { indexRate_ = r; }
    bool hasIndex() const { return index_.loaded(); }
    void seed(unsigned s) { rng_.seed(s); }
    const RvcInfo& info() const { return info_; }

    // Pitch (Hz, 0 = unvoiced) of every 10 ms frame of 16 kHz audio,
    // centred on frame i * 160.
    static void trackPitch(const std::vector<float>& x16, int frames, std::vector<float>& f0);

private:
    RvcInfo info_;
    FaissIndex index_;
    OrtGraphSession encoder_, voice_;
    std::string provider_ = "CPU";
    int samples_ = 0;  // block at the voice's rate
    int n16_ = 0;      // the block at 16 kHz
    int pad16_ = 0;    // padding at the end for the speech encoder
    int frames_ = 0;   // 10 ms frames
    int featFrames_ = 0;
    float indexRate_ = 0.5f;
    std::mt19937 rng_{12345};
    std::vector<float> x16_, feats_, phone_, pitchf_, rnd_, noise_, f0_;
    std::vector<int64_t> pitch_;
};

} // namespace echo::ai
