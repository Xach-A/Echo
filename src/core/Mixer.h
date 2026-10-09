// Mixer runtime objects: input strips, output buses, routing sends and
// recording taps. Parameters are atomics written by the UI; everything else
// is touched only by the audio thread.
#pragma once

#include <atomic>
#include <memory>
#include <vector>

#include "core/Common.h"
#include "core/Meter.h"
#include "core/Smoothing.h"
#include "core/SpscRing.h"
#include "dsp/Effect.h"
#include "dsp/Limiter.h"

namespace echo {

class SourceRuntime;

struct SendParams {
    std::atomic<bool> enabled{false};
    std::atomic<float> gainDb{0.0f};
    std::atomic<bool> preFader{false};
    std::atomic<bool> blocked{false}; // set by feedback protection
    // audio thread only
    LinearSmoother gain;
};

// Accumulation buffers for all bus slots, owned by the engine.
struct BusAccumulators {
    float* l[kMaxBuses] = {};
    float* r[kMaxBuses] = {};
    bool used[kMaxBuses] = {};
};

// Whole-chain bypass (A/B compare) with a short crossfade. While bypassed
// the chain keeps running in the background for a while, so switching back
// is seamless; after that it stops (saving CPU) and restarts cleanly.
struct ChainBypass {
    LinearSmoother mix;
    int offSamples = 0;
    int keepWarmSamples = 0;
    bool cold = false;
    std::vector<float> scratchL, scratchR, dryL, dryR;
    void prepare(double sampleRate, int maxBlock, bool bypassed);
    void run(ChainRunner& runner, const ChainSnap& chain, bool bypass, float* l, float* r, bool& mono, int n,
             const float* refL = nullptr, const float* refR = nullptr, int refCh = 0);
};

class StripRuntime : NonCopyable {
public:
    StripRuntime();

    // Non-real-time.
    void prepare(double sampleRate, int maxBlock);

    // Real-time.
    void process(const SourceRuntime* src, const ChainSnap& chain, int n, bool anySolo,
                 BusAccumulators& acc, const SourceRuntime* reference = nullptr);

    // --- parameters (UI writes) ---
    std::atomic<float> trimDb{0.0f};
    std::atomic<float> faderDb{0.0f};
    std::atomic<float> pan{0.0f};           // -1 .. +1 (pan for mono, balance for stereo)
    std::atomic<bool> mute{false};
    std::atomic<bool> solo{false};
    std::atomic<bool> invertPolarity{false};
    std::atomic<bool> fxBypass{false};      // A/B compare: whole chain
    std::atomic<int> chanFirst{0};          // first source channel
    std::atomic<int> chanCount{2};          // 1 = mono, 2 = stereo pair
    std::atomic<bool> downmix{false};       // sum all source channels to mono
    SendParams sends[kMaxBuses];

    // --- outputs (audio thread writes) ---
    MeterState inMeter;   // after trim, before effects
    MeterState outMeter;  // after fader/pan
    std::atomic<bool> isMono{true};
    ChainRunner chainRunner;

    const float* outL() const { return postL_.data(); }
    const float* outR() const { return postR_.data(); }
    const float* preL() const { return preL_.data(); }
    const float* preR() const { return preR_.data(); }

private:
    double sr_ = 48000.0;
    LinearSmoother trimSm_, faderSm_, panLSm_, panRSm_, muteSm_;
    ChainBypass bypass_;
    std::vector<float> l_, r_, preL_, preR_, postL_, postR_;
};

class BusRuntime : NonCopyable {
public:
    BusRuntime();
    void prepare(double sampleRate, int maxBlock);
    // Real-time: processes the accumulated mix in place.
    void process(const ChainSnap& chain, float* l, float* r, int n);

    std::atomic<float> faderDb{0.0f};
    std::atomic<bool> mute{false};
    std::atomic<bool> mono{false};
    std::atomic<bool> limiterEnabled{true};
    std::atomic<float> limiterCeilingDb{-0.3f};
    std::atomic<bool> fxBypass{false};

    MeterState meter;
    std::atomic<float> limiterGrDb{0.0f};
    ChainRunner chainRunner;

private:
    LinearSmoother faderSm_, muteSm_;
    ChainBypass bypass_;
    LookaheadLimiter limiter_;
    double sr_ = 48000.0;
};

// Recording tap: the engine copies a signal into `ring` (stereo
// interleaved); a writer thread drains it to disk.
struct RecordTap : NonCopyable {
    enum Kind { StripPost = 0, StripPre = 1, Bus = 2 };
    explicit RecordTap(size_t ringFrames) { ring.allocate(ringFrames * 2); }
    SpscRing<float> ring;
    std::atomic<uint64_t> droppedFrames{0};
    std::atomic<uint64_t> capturedFrames{0};
    float scratch[kMaxBlockSize * 2];
};

} // namespace echo
