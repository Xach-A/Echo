// Signal generation and analysis helpers for tests.
#pragma once

#include <cmath>
#include <complex>
#include <vector>

#include "core/Common.h"

namespace echo::test {

inline std::vector<float> sine(double freq, double sr, size_t n, float amp = 1.0f, double phase = 0.0) {
    std::vector<float> v(n);
    for (size_t i = 0; i < n; ++i)
        v[i] = amp * static_cast<float>(std::sin(2.0 * kPiD * freq * static_cast<double>(i) / sr + phase));
    return v;
}

inline float peak(const float* x, size_t n) {
    float p = 0.0f;
    for (size_t i = 0; i < n; ++i) p = std::max(p, std::fabs(x[i]));
    return p;
}
inline float peak(const std::vector<float>& v, size_t start = 0, size_t len = SIZE_MAX) {
    len = std::min(len, v.size() - start);
    return peak(v.data() + start, len);
}

inline float rms(const float* x, size_t n) {
    double s = 0.0;
    for (size_t i = 0; i < n; ++i) s += static_cast<double>(x[i]) * x[i];
    return n ? static_cast<float>(std::sqrt(s / static_cast<double>(n))) : 0.0f;
}
inline float rms(const std::vector<float>& v, size_t start = 0, size_t len = SIZE_MAX) {
    len = std::min(len, v.size() - start);
    return rms(v.data() + start, len);
}

inline bool allFinite(const std::vector<float>& v) {
    for (float x : v)
        if (!std::isfinite(x)) return false;
    return true;
}

// Amplitude of a sinusoid at `freq` (Goertzel, Hann window), ~1.0 for a
// full-scale sine.
inline double toneAmplitude(const float* x, size_t n, double freq, double sr) {
    double sre = 0.0, sim = 0.0, wsum = 0.0;
    for (size_t i = 0; i < n; ++i) {
        const double w = 0.5 - 0.5 * std::cos(2.0 * kPiD * static_cast<double>(i) / static_cast<double>(n - 1));
        const double ph = 2.0 * kPiD * freq * static_cast<double>(i) / sr;
        sre += w * x[i] * std::cos(ph);
        sim += w * x[i] * std::sin(ph);
        wsum += w;
    }
    return 2.0 * std::sqrt(sre * sre + sim * sim) / wsum;
}
inline double toneAmplitude(const std::vector<float>& v, size_t start, size_t len, double freq, double sr) {
    return toneAmplitude(v.data() + start, std::min(len, v.size() - start), freq, sr);
}

// Dominant frequency via a fine Goertzel scan around the coarse peak.
inline double dominantFrequency(const float* x, size_t n, double sr, double fmin = 40.0, double fmax = 4000.0) {
    double best = fmin, bestA = -1.0;
    for (double f = fmin; f <= fmax; f *= 1.01) {
        const double a = toneAmplitude(x, n, f, sr);
        if (a > bestA) { bestA = a; best = f; }
    }
    double lo = best / 1.012, hi = best * 1.012;
    for (int it = 0; it < 3; ++it) {
        double step = (hi - lo) / 40.0, b = lo, ba = -1.0;
        for (double f = lo; f <= hi; f += step) {
            const double a = toneAmplitude(x, n, f, sr);
            if (a > ba) { ba = a; b = f; }
        }
        lo = b - step;
        hi = b + step;
        best = b;
    }
    return best;
}

inline double cents(double f, double ref) { return 1200.0 * std::log2(f / ref); }

} // namespace echo::test
