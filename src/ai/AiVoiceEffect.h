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

class AiVoiceEffect final : public Effect {
public:
    AiVoiceEffect();
    ~AiVoiceEffect() override;

    // UI thread: select a model (empty manifest = unload). Loading happens
    // on the worker thread.
    void setModel(const ModelManifest& m, bool preferGpu);
    void clearModel();
    std::string modelName() const;
    std::string statusText() const;
    std::string providerName() const;
    AiStatus status() const { return static_cast<AiStatus>(status_.load()); }

    int latencySamples() const override;
    const char* visLabel(int s) const override;
    // Persists the selected model ("<model folder>|gpu" or "<model folder>").
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
    // Tests only: extra time added to every inference call.
    void setArtificialDelayForTesting(int ms) { testDelayMs_.store(ms); }

protected:
    void onPrepare() override;
    void onReset() override;
    void processImpl(float* const* ch, int numCh, int n) override;

private:
    void workerMain();
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
    std::string modelId_;          // folder name of the selected model
    std::string restoreId_;        // model to load after prepare()
    bool restoreGpu_ = false;
    std::string modelName_, statusText_, provider_;
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
    bool lastActive_ = false;         // audio thread only
    int maxDryDelay_ = 0;
};

} // namespace echo::ai
