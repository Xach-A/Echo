// Helpers shared by effect implementations.
#pragma once

#include <algorithm>
#include <cmath>
#include <iterator>
#include <vector>
#include <memory>

#include "core/Smoothing.h"
#include "dsp/Effect.h"
#include "dsp/EffectRegistry.h"

namespace echo {

#define ECHO_EFFECT_INFO(var, id, name, category, desc, params, widens) \
    static const EffectInfo var{id, name, category, desc, params, static_cast<int>(std::size(params)), widens}

template <typename T>
void registerEffect(EffectRegistry& r, const EffectInfo& info) {
    r.add(info, []() -> std::unique_ptr<Effect> { return std::make_unique<T>(); });
}

// Per-block parameter glides (avoid zipper noise on coefficient updates).
inline float glideLinear(float cur, float target, float amount) {
    const float d = target - cur;
    return std::fabs(d) < 1e-4f ? target : cur + d * amount;
}
inline float glideLog(float cur, float target, float amount) {
    if (cur <= 0.0f || target <= 0.0f) return target;
    const float r = std::log(target / cur);
    return std::fabs(r) < 1e-4f ? target : cur * std::exp(r * amount);
}

// Equal-power dry/wet mix with latency-aligned dry buffer supplied by caller.
inline void mixDryWet(float* wet, const float* dry, int n, float mix) {
    if (mix >= 0.999f) return;
    const float w = std::sin(mix * kPi * 0.5f), d = std::cos(mix * kPi * 0.5f);
    for (int i = 0; i < n; ++i) wet[i] = wet[i] * w + dry[i] * d;
}

// Linear dry/wet mix (for parallel processing where both paths are coherent).
inline void mixLinear(float* wet, const float* dry, int n, float mix) {
    if (mix >= 0.999f) return;
    for (int i = 0; i < n; ++i) wet[i] = dry[i] + (wet[i] - dry[i]) * mix;
}

// Fixed delay used to time-align dry signals with latent wet signals.
class AlignDelay {
public:
    void prepare(int delay, int maxBlock) {
        delay_ = delay;
        buf_.assign(static_cast<size_t>(delay + maxBlock + 1), 0.0f);
        pos_ = 0;
    }
    void reset() { std::fill(buf_.begin(), buf_.end(), 0.0f); pos_ = 0; }
    // Changes the delay without reallocating (clamped to the prepared size).
    void setDelay(int delay, int maxBlock) {
        delay_ = std::clamp(delay, 0, static_cast<int>(buf_.size()) - maxBlock - 1);
    }
    // out[i] = in[i - delay]
    void process(const float* in, float* out, int n) {
        const int size = static_cast<int>(buf_.size());
        for (int i = 0; i < n; ++i) {
            buf_[pos_] = in[i];
            int r = pos_ - delay_;
            if (r < 0) r += size;
            out[i] = buf_[r];
            if (++pos_ == size) pos_ = 0;
        }
    }
    int delay() const { return delay_; }

private:
    std::vector<float> buf_;
    int delay_ = 0, pos_ = 0;
};

} // namespace echo
