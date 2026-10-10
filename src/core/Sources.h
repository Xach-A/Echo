// Audio sources pulled once per engine block. A source is shared by every
// strip that uses it, so one capture stream can feed many strips without
// opening the device twice.
#pragma once

#include <atomic>
#include <memory>
#include <string>
#include <vector>

#include "core/Common.h"
#include "core/Endpoint.h"
#include "core/Meter.h"
#include "core/Resampler.h"

namespace echo {

enum class SourceState : int { Offline = 0, Priming = 1, Running = 2, Idle = 3 };

class SourceRuntime : NonCopyable {
public:
    virtual ~SourceRuntime() = default;

    // Non-real-time. Allocates buffers for the engine format.
    virtual void prepare(double engineRate, int maxBlock, double blockSeconds);
    // Real-time: produce `frames` frames into the planar channel buffers.
    virtual void pull(int frames) = 0;
    // Estimated input latency contributed by buffering, in seconds.
    virtual double latencySeconds() const { return 0.0; }

    int channels() const { return channels_; }
    const float* channel(int c) const { return buf_.data() + static_cast<size_t>(c) * maxBlock_; }
    float* channelMut(int c) { return buf_.data() + static_cast<size_t>(c) * maxBlock_; }

    std::string key;   // identity used for de-duplication by the controller
    std::string label; // human-readable description

    std::atomic<int> state{static_cast<int>(SourceState::Offline)};
    std::atomic<uint64_t> underruns{0};
    std::atomic<uint64_t> overruns{0};
    // App/loopback captures (which are silent when nothing plays): ran dry
    // while audio was actually playing (an audible gap).
    std::atomic<uint64_t> gaps{0};
    std::atomic<float> fillMs{0.0f};
    std::atomic<float> driftPpm{0.0f};
    std::atomic<float> marginMs{0.0f}; // adaptive extra buffering after dropouts
    MeterState meter; // raw level of the first two channels

protected:
    void zero(int frames);
    int channels_ = 2;
    int maxBlock_ = 0;
    double engineRate_ = 48000.0;
    double blockSeconds_ = 0.01;
    std::vector<float> buf_;
};

// Pulls audio from a capture endpoint ring through an adaptive resampler.
class DeviceCaptureSource final : public SourceRuntime {
public:
    explicit DeviceCaptureSource(std::shared_ptr<CaptureEndpoint> endpoint,
                                 ResamplerQuality quality = ResamplerQuality::Normal);
    void prepare(double engineRate, int maxBlock, double blockSeconds) override;
    void pull(int frames) override;
    double latencySeconds() const override;
    CaptureEndpoint* endpoint() const { return ep_.get(); }
    const std::shared_ptr<CaptureEndpoint>& endpointPtr() const { return ep_; }

private:
    std::shared_ptr<CaptureEndpoint> ep_;
    ResamplerQuality quality_;
    Resampler rs_;
    DriftController dc_;
    std::vector<float> in_;   // interleaved device frames
    std::vector<float> out_;  // interleaved engine frames
    int devCh_ = 0;
    int keepCh_ = 0;          // channels kept (<= kMaxSourceChannels)
    void applyTarget();
    double baseTarget_ = 0.0; // device frames
    double targetFill_ = 0.0; // device frames (base + adaptive margin)
    double overflowFill_ = 0.0;
    double overflowExtra_ = 0.0;
    AdaptiveMargin margin_;
    bool primed_ = false;
    uint64_t readTotal_ = 0;  // frames removed from the ring (read or skipped)
};

// Test-signal generator (sine, noise, sweep, impulse train).
class ToneSource final : public SourceRuntime {
public:
    enum Wave { Sine = 0, Square, WhiteNoise, PinkNoise, Sweep, ImpulseTrain, ChirpTrain, NumWaves };
    static const char* const kWaveNames[];

    // The chirp burst used by ChirpTrain (for cross-correlation latency
    // measurement): Hann-windowed linear sweep 500 Hz -> 8 kHz.
    static constexpr int kChirpLength = 2048;
    static float chirpSample(int i, double sampleRate);

    ToneSource();
    void prepare(double engineRate, int maxBlock, double blockSeconds) override;
    void pull(int frames) override;

    std::atomic<int> wave{Sine};
    std::atomic<float> frequency{440.0f};
    std::atomic<float> levelDb{-18.0f};
    std::atomic<bool> enabled{true};
    std::atomic<float> impulsePeriodSec{1.0f};
    // Sample index (engine clock) of the last impulse emitted.
    std::atomic<uint64_t> lastImpulseFrame{0};
    std::atomic<uint64_t> framesGenerated{0};

private:
    double phase_ = 0.0;
    double sweepPhase_ = 0.0;
    uint32_t rng_ = 0x12345678u;
    float pink_[7] = {};
    uint64_t frameCounter_ = 0;
    uint64_t nextImpulse_ = 0;
};

} // namespace echo
