// AI voice conversion as an effect.
//
// Real-time safety: the audio thread only copies samples into a bounded
// input queue and reads finished samples from a bounded output queue.
// Inference runs on a dedicated worker thread. The wet signal is delayed by
// a fixed latency; if inference falls behind, the effect plays the dry
// (equally delayed) signal and counts an underrun instead of glitching.
#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "ai/VoiceModel.h"
#include "core/Resampler.h"
#include "core/SpscRing.h"
#include "dsp/Effect.h"
#include "dsp/effects/EffectUtil.h"

namespace echo::ai {

enum class AiStatus : int { NoModel = 0, Loading = 1, Running = 2, Error = 3 };

// How a voice is converted in pieces (milliseconds). Each run converts the
// newest `chunk` with up to `context` of audio before it, and `lookahead` of
// it is left after the part that is played, so that part isn't made at the
// very edge. Pieces are lined up (SOLA) and blended over `crossfade`.
// Delay: chunk + lookahead + crossfade + a margin for converting it.
struct StreamPlan {
    int chunkMs = 150;
    int contextMs = 2000;
    int crossfadeMs = 40;
    int lookaheadMs = 0;
};
// The plan for each Quality step of an RVC voice (0 fastest .. 4 smoothest).
StreamPlan rvcStreamPlan(int quality);

// Where a new piece lines up best with the end of the last one (SOLA): the
// shift (0..search) at which `wav` correlates best with `tail` (wav holds
// tail.size() + search samples or more). Shifts about as good as the middle
// one lose to it, so a steady tone keeps its timing; silence: the middle.
int solaOffset(const std::vector<float>& wav, const std::vector<float>& tail, int search);

class AiVoiceEffect final : public Effect {
public:
    AiVoiceEffect();
    ~AiVoiceEffect() override;

    // UI thread: select a model (empty manifest = unload). Loading happens
    // on the worker thread.
    void setModel(const ModelManifest& m, bool preferGpu);
    void clearModel();
    // Runs the voice on the graphics card (DirectML) or the CPU; a loaded
    // voice reloads.
    void setPreferGpu(bool gpu);
    bool prefersGpu() const;
    std::string modelName() const;
    std::string modelKind() const; // "rvc" or "onnx" ("" with no model)
    std::string statusText() const;
    std::string providerName() const;
    AiStatus status() const { return static_cast<AiStatus>(status_.load()); }

    int latencySamples() const override;
    const char* visLabel(int s) const override;
    // Persists the selected model ("<model folder>|gpu" or "|cpu"; a plain
    // "<model folder>", saved before there was a choice, means the graphics
    // card when Echo can use one).
    std::string saveState() const override;
    void loadState(const std::string& state) override;

    // Statistics (UI)
    std::atomic<float> inferenceMs{0.0f};
    std::atomic<float> realtimeFactor{0.0f};
    std::atomic<uint64_t> underrunSamples{0};
    std::atomic<uint64_t> chunksProcessed{0};
    // Input the worker dropped because it had fallen too far behind to be
    // useful (inference slower than real time): it skips to the newest audio.
    std::atomic<uint64_t> skippedSamples{0};
    // The context the loaded RVC voice really hears (ms; shorter than its
    // plan's when the PC can't convert that much in time).
    std::atomic<int> contextMs{0};
    std::atomic<int> loadedQuality{-1}; // the Quality step the RVC voice was loaded with
    // Tests only: extra time added to every inference call.
    void setArtificialDelayForTesting(int ms) { testDelayMs_.store(ms); }
    // Tests and measurements only: RVC voices load with this plan instead of
    // their Quality step's (context used as given).
    void setStreamPlanForTesting(const StreamPlan& p) {
        std::lock_guard<std::mutex> lock(modelMutex_);
        testPlan_ = p;
        haveTestPlan_ = true;
    }

protected:
    void onPrepare() override;
    void onReset() override;
    void processImpl(float* const* ch, int numCh, int n) override;

private:
    void workerMain();
    void select(const ModelManifest& m, bool preferGpu, bool gpuAuto);
    void setStatus(AiStatus s, const std::string& text);

    // Audio-thread state
    SpscRing<float> inRing_, outRing_;
    AlignDelay dryDelay_[2];
    std::vector<float> dry_[2];
    std::vector<float> mono_;
    uint64_t inCount_ = 0;     // samples pushed to the worker
    uint64_t readIndex_ = 0;   // index of the next sample in outRing_
    float wetGain_ = 0.0f;     // smoothed dry/wet crossfade during underruns
    int latency_ = 0;          // samples (engine rate)

    // Worker
    std::thread worker_;
    std::atomic<bool> stop_{false};
    std::atomic<bool> modelDirty_{false};
    mutable std::mutex modelMutex_;
    ModelManifest pending_;
    bool pendingGpu_ = false;
    bool gpuAuto_ = false;         // no choice saved: the graphics card if Echo can use one
    std::string modelId_;          // folder name of the selected model
    std::string restoreId_;        // model to load after prepare()
    bool restoreGpu_ = false, restoreGpuAuto_ = false;
    std::string modelName_, modelKind_, statusText_, provider_;
    std::atomic<int> status_{static_cast<int>(AiStatus::NoModel)};
    std::atomic<bool> active_{false}; // a model is loaded and producing audio
    // Lock-free reset handshake between the audio thread and the worker:
    // the audio thread bumps syncRequest_; the worker flushes its stream
    // state and echoes the value in syncAck_.
    std::atomic<uint64_t> syncRequest_{0};
    std::atomic<uint64_t> syncAck_{0};
    std::atomic<int> wetOffset_{0};   // wet stream offset (engine samples)
    std::atomic<int> minLatency_{0};  // chunk + margin (engine samples)
    std::atomic<int> latencyPub_{0};  // latency_ published for the worker
    std::atomic<int> testDelayMs_{0};
    StreamPlan testPlan_;
    bool haveTestPlan_ = false;
    bool lastActive_ = false;         // audio thread only
    int maxDryDelay_ = 0;
};

} // namespace echo::ai
