// Delay lines with fractional (linear or cubic Hermite) reads and a simple
// sine LFO. Buffers are allocated in prepare(); reads/writes are real-time.
#pragma once

#include <cmath>
#include <vector>

#include "core/Common.h"

namespace echo {

class DelayLine {
public:
    void prepare(int maxDelaySamples) {
        int size = 1;
        while (size < maxDelaySamples + 4) size <<= 1;
        buf_.assign(size, 0.0f);
        mask_ = size - 1;
        pos_ = 0;
    }
    void reset() {
        std::fill(buf_.begin(), buf_.end(), 0.0f);
        pos_ = 0;
    }
    int maxDelay() const { return static_cast<int>(buf_.size()) - 4; }

    inline void push(float x) {
        buf_[pos_] = x;
        pos_ = (pos_ + 1) & mask_;
    }
    // Delay of d samples relative to the most recent push (d >= 1 reads the
    // sample written d pushes ago).
    inline float read(int d) const { return buf_[(pos_ - d) & mask_]; }

    inline float readLinear(float d) const {
        const int i = static_cast<int>(d);
        const float f = d - static_cast<float>(i);
        const float a = read(i), b = read(i + 1);
        return a + (b - a) * f;
    }

    // 4-point cubic Hermite interpolation, needs d >= 1.
    inline float readCubic(float d) const {
        const int i = static_cast<int>(d);
        const float f = d - static_cast<float>(i);
        const float xm1 = read(i - 1), x0 = read(i), x1 = read(i + 1), x2 = read(i + 2);
        const float c1 = 0.5f * (x1 - xm1);
        const float c2 = xm1 - 2.5f * x0 + 2.0f * x1 - 0.5f * x2;
        const float c3 = 0.5f * (x2 - xm1) + 1.5f * (x0 - x1);
        return ((c3 * f + c2) * f + c1) * f + x0;
    }

private:
    std::vector<float> buf_;
    int mask_ = 0;
    int pos_ = 0;
};

class SineLfo {
public:
    void prepare(double fs) { fs_ = fs; }
    void setRate(double hz) { inc_ = hz / fs_; }
    void setPhase(double p) { phase_ = p - std::floor(p); }
    void reset() { phase_ = 0.0; }
    // Returns sin in [-1, 1] at the current phase (with optional offset in
    // cycles), then advances.
    inline float next(double offset = 0.0) {
        double p = phase_ + offset;
        p -= std::floor(p);
        const float v = static_cast<float>(std::sin(2.0 * kPiD * p));
        phase_ += inc_;
        if (phase_ >= 1.0) phase_ -= 1.0;
        return v;
    }
    double phase() const { return phase_; }

private:
    double fs_ = 48000.0, inc_ = 0.0, phase_ = 0.0;
};

// 4x oversampler using polyphase windowed-sinc FIR filters. Used around
// nonlinear stages (distortion) to suppress aliasing.
class Oversampler4x {
public:
    static constexpr int kFactor = 4;
    static constexpr int kTapsPerPhase = 12;
    void prepare();
    void reset();
    // Upsample one input sample into out[4].
    inline void up(float x, float* out) {
        hist_[histPos_] = x;
        hist_[histPos_ + kTapsPerPhase] = x;
        histPos_ = (histPos_ + 1) % kTapsPerPhase;
        const float* h = &hist_[histPos_]; // oldest .. newest
        for (int p = 0; p < kFactor; ++p) {
            const float* c = &up_[p * kTapsPerPhase];
            float acc = 0.0f;
            for (int k = 0; k < kTapsPerPhase; ++k) acc += h[k] * c[k];
            out[p] = acc;
        }
    }
    // Downsample 4 oversampled samples into one output sample.
    inline float down(const float* in) {
        for (int p = 0; p < kFactor; ++p) {
            dhist_[dpos_] = in[p];
            dhist_[dpos_ + kTaps] = in[p];
            dpos_ = (dpos_ + 1) % kTaps;
        }
        const float* h = &dhist_[dpos_];
        float acc = 0.0f;
        for (int k = 0; k < kTaps; ++k) acc += h[k] * down_[k];
        return acc;
    }
    int latencySamples() const { return kTapsPerPhase; } // approx., at base rate

private:
    static constexpr int kTaps = kFactor * kTapsPerPhase;
    float up_[kTaps] = {};
    float down_[kTaps] = {};
    float hist_[2 * kTapsPerPhase] = {};
    int histPos_ = 0;
    float dhist_[2 * kTaps] = {};
    int dpos_ = 0;
};

} // namespace echo
