// Parameter smoothing primitives used inside DSP code. They are owned and
// touched only by the audio thread; targets typically come from atomics.
#pragma once

#include <algorithm>
#include <cmath>

namespace echo {

// Linear ramp to a target over a fixed time. Cheap and click-free for gains.
class LinearSmoother {
public:
    void prepare(double sampleRate, double rampSeconds) {
        rampSamples_ = std::max(1, static_cast<int>(sampleRate * rampSeconds));
    }
    void reset(float value) { current_ = target_ = value; remaining_ = 0; step_ = 0.0f; }
    void setTarget(float t) {
        if (t == target_) return;
        target_ = t;
        remaining_ = rampSamples_;
        step_ = (target_ - current_) / static_cast<float>(remaining_);
    }
    float next() {
        if (remaining_ > 0) {
            current_ += step_;
            if (--remaining_ == 0) current_ = target_;
        }
        return current_;
    }
    bool isSmoothing() const { return remaining_ > 0; }
    float current() const { return current_; }
    float target() const { return target_; }

    // Applies the ramp as a gain to a buffer.
    void applyGain(float* x, int n) {
        if (!isSmoothing()) {
            const float g = current_;
            if (g == 1.0f) return;
            for (int i = 0; i < n; ++i) x[i] *= g;
            return;
        }
        for (int i = 0; i < n; ++i) x[i] *= next();
    }

private:
    float current_ = 0.0f, target_ = 0.0f, step_ = 0.0f;
    int remaining_ = 0, rampSamples_ = 480;
};

// First-order exponential smoother; time constant in seconds.
class OnePoleSmoother {
public:
    void prepare(double sampleRate, double tauSeconds) {
        coeff_ = tauSeconds <= 0.0 ? 0.0f
                                   : static_cast<float>(std::exp(-1.0 / (sampleRate * tauSeconds)));
    }
    void reset(float v) { y_ = v; }
    float process(float x) { y_ = x + coeff_ * (y_ - x); return y_; }
    float value() const { return y_; }

private:
    float coeff_ = 0.0f, y_ = 0.0f;
};

// Converts attack/release times into one-pole coefficients for envelope
// followers (time to reach ~63%).
inline float timeToCoeff(double sampleRate, double seconds) {
    if (seconds <= 0.0) return 0.0f;
    return static_cast<float>(std::exp(-1.0 / (sampleRate * seconds)));
}

} // namespace echo
