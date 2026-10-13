// Base class for every audio effect.
//
// Contract:
//  * prepare() runs on a non-real-time thread and may allocate.
//  * process() runs on the audio thread: no allocation, no locks, no I/O.
//  * Parameters are atomics. The UI writes them at any time; the effect reads
//    them once per block and smooths internally where needed.
//  * process() handles 1 (mono) or 2 (stereo) channels, in place.
#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <string_view>

#include "core/Common.h"

namespace echo {

enum class ParamScale : uint8_t {
    Linear,   // continuous, linear mapping
    Log,      // continuous, logarithmic mapping (frequencies, times)
    Enum,     // integer index into `choices`
    Toggle,   // 0 / 1
    Int,      // integer steps
};

struct ParamInfo {
    const char* id;
    const char* name;
    float min;
    float max;
    float def;
    const char* unit;
    ParamScale scale;
    const char* const* choices = nullptr; // Enum only; nullptr-terminated
    const char* help = nullptr;
};

struct EffectInfo {
    const char* typeId;      // stable identifier used in presets
    const char* name;        // display name
    const char* category;    // "Dynamics", "EQ & Filters", "Voice", ...
    const char* description;
    const ParamInfo* params;
    int numParams;
    bool widensStereo;       // produces stereo output from mono input
};

class Effect : NonCopyable {
public:
    explicit Effect(const EffectInfo& info);
    virtual ~Effect() = default;

    const EffectInfo& info() const { return info_; }
    const char* typeId() const { return info_.typeId; }
    int numParams() const { return info_.numParams; }
    const ParamInfo& paramInfo(int i) const { return info_.params[i]; }
    int paramIndex(std::string_view id) const;

    float param(int i) const { return params_[i].load(std::memory_order_relaxed); }
    void setParam(int i, float v);
    void setParam(std::string_view id, float v);
    void resetParamsToDefault();

    // User-visible label for this instance (defaults to the type name).
    std::string label;
    // The voice preset this effect belongs to on an input ("" = added by
    // hand, or input cleanup). Lets combined voices be added and taken out
    // one at a time.
    std::string voice;
    uint32_t instanceId;

    std::atomic<bool> bypass{false};
    std::atomic<bool> faulted{false};      // set when auto-bypassed after failures
    std::atomic<uint32_t> faultCount{0};

    // Non-real-time.
    void prepare(double sampleRate, int maxBlock);
    bool isPrepared() const { return prepared_; }
    double sampleRate() const { return sr_; }
    void requestReset() { resetRequested_.store(true, std::memory_order_release); }

    // Real-time. Called by the chain; never throws.
    void process(float* const* ch, int numCh, int n);
    void resetState() { onReset(); }

    virtual int latencySamples() const { return 0; }

    // Reference ("sidechain") input, e.g. the playback signal for echo
    // cancellation. Effects that need it return true; the chain runner then
    // passes the strip's reference source before every process() call. The
    // pointers are only valid during that call and may be null.
    virtual bool usesReference() const { return false; }
    void setReference(const float* l, const float* r, int numCh) {
        refL_ = l;
        refR_ = r;
        refCh_ = numCh;
    }

    // Share of the real-time budget spent in this effect (smoothed, 1 = 100%
    // of one block's duration). Written by the chain runner.
    std::atomic<float> cpuLoad{0.0f};

    // Extra non-parameter state saved with sessions/presets (UI thread),
    // e.g. the AI effect's selected model. Empty = nothing to save.
    virtual std::string saveState() const { return {}; }
    virtual void loadState(const std::string& /*state*/) {}

    // Audio-thread-only bypass crossfade position (1 = active, 0 = bypassed,
    // negative = not yet initialised). Managed by ChainRunner.
    float rtActiveMix = -1.0f;
    // Audio-thread-only: samples left until a (re)started effect's output is
    // valid (its latency); the dry signal is passed until then. -1 = idle.
    int rtWarmup = -1;

    // Live values published by the audio thread for UI display (gain
    // reduction, detected pitch, gate state, ...). Meaning is per effect;
    // see visLabel().
    static constexpr int kVisSlots = 4;
    std::atomic<float> vis[kVisSlots] = {0.0f, 0.0f, 0.0f, 0.0f};
    virtual const char* visLabel(int slot) const { return nullptr; }

protected:
    virtual void onPrepare() = 0;
    virtual void onReset() {}
    virtual void processImpl(float* const* ch, int numCh, int n) = 0;

    float p(int i) const { return params_[i].load(std::memory_order_relaxed); }
    int pi(int i) const { return static_cast<int>(std::lround(p(i))); }
    bool pb(int i) const { return p(i) >= 0.5f; }

    double sr_ = 48000.0;
    int maxBlock_ = kMaxBlockSize;
    const float* refL_ = nullptr;
    const float* refR_ = nullptr;
    int refCh_ = 0;

private:
    const EffectInfo& info_;
    std::atomic<float> params_[kMaxEffectParams];
    std::atomic<bool> resetRequested_{false};
    bool prepared_ = false;
};

// Effect chain snapshot as seen by the audio thread.
struct ChainSnap {
    int count = 0;
    Effect* fx[kMaxChainLength] = {};
};

// Per-chain audio-thread state (bypass crossfades, scratch buffers).
class ChainRunner {
public:
    void prepare(int maxBlock);
    // Processes `chain` in place. `mono` tracks whether the signal is still
    // dual-mono (only L is meaningful); it is updated when an effect widens.
    // refL/refR: optional reference signal for effects that use one.
    void process(const ChainSnap& chain, float* l, float* r, bool& mono, int n, const float* refL = nullptr,
                 const float* refR = nullptr, int refCh = 0);

    // Effects that produced non-finite output since the last call (UI reads).
    std::atomic<uint32_t> faults{0};

private:
    float dryL_[kMaxBlockSize];
    float dryR_[kMaxBlockSize];
};

} // namespace echo
