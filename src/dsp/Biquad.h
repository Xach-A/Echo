// Second-order IIR sections (RBJ Audio EQ Cookbook designs), transposed
// direct form II with double-precision state for low-frequency stability.
#pragma once

#include <cmath>
#include <complex>

#include "core/Common.h"

namespace echo {

enum class FilterType { LowPass, HighPass, BandPass, Notch, Peak, LowShelf, HighShelf, AllPass };

struct BiquadCoeffs {
    double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
};

inline BiquadCoeffs designBiquad(FilterType type, double fs, double f0, double q, double gainDb = 0.0) {
    f0 = clampv(f0, 1.0, fs * 0.4999);
    q = std::max(q, 0.025);
    const double A = std::pow(10.0, gainDb / 40.0);
    const double w0 = 2.0 * kPiD * f0 / fs;
    const double cw = std::cos(w0), sw = std::sin(w0);
    const double alpha = sw / (2.0 * q);
    double b0 = 1, b1 = 0, b2 = 0, a0 = 1, a1 = 0, a2 = 0;
    switch (type) {
    case FilterType::LowPass:
        b0 = (1 - cw) / 2; b1 = 1 - cw; b2 = (1 - cw) / 2;
        a0 = 1 + alpha; a1 = -2 * cw; a2 = 1 - alpha;
        break;
    case FilterType::HighPass:
        b0 = (1 + cw) / 2; b1 = -(1 + cw); b2 = (1 + cw) / 2;
        a0 = 1 + alpha; a1 = -2 * cw; a2 = 1 - alpha;
        break;
    case FilterType::BandPass: // constant 0 dB peak gain
        b0 = alpha; b1 = 0; b2 = -alpha;
        a0 = 1 + alpha; a1 = -2 * cw; a2 = 1 - alpha;
        break;
    case FilterType::Notch:
        b0 = 1; b1 = -2 * cw; b2 = 1;
        a0 = 1 + alpha; a1 = -2 * cw; a2 = 1 - alpha;
        break;
    case FilterType::Peak:
        b0 = 1 + alpha * A; b1 = -2 * cw; b2 = 1 - alpha * A;
        a0 = 1 + alpha / A; a1 = -2 * cw; a2 = 1 - alpha / A;
        break;
    case FilterType::LowShelf: {
        const double sa = 2 * std::sqrt(A) * alpha;
        b0 = A * ((A + 1) - (A - 1) * cw + sa);
        b1 = 2 * A * ((A - 1) - (A + 1) * cw);
        b2 = A * ((A + 1) - (A - 1) * cw - sa);
        a0 = (A + 1) + (A - 1) * cw + sa;
        a1 = -2 * ((A - 1) + (A + 1) * cw);
        a2 = (A + 1) + (A - 1) * cw - sa;
        break;
    }
    case FilterType::HighShelf: {
        const double sa = 2 * std::sqrt(A) * alpha;
        b0 = A * ((A + 1) + (A - 1) * cw + sa);
        b1 = -2 * A * ((A - 1) + (A + 1) * cw);
        b2 = A * ((A + 1) + (A - 1) * cw - sa);
        a0 = (A + 1) - (A - 1) * cw + sa;
        a1 = 2 * ((A - 1) - (A + 1) * cw);
        a2 = (A + 1) - (A - 1) * cw - sa;
        break;
    }
    case FilterType::AllPass:
        b0 = 1 - alpha; b1 = -2 * cw; b2 = 1 + alpha;
        a0 = 1 + alpha; a1 = -2 * cw; a2 = 1 - alpha;
        break;
    }
    BiquadCoeffs c;
    c.b0 = b0 / a0; c.b1 = b1 / a0; c.b2 = b2 / a0; c.a1 = a1 / a0; c.a2 = a2 / a0;
    return c;
}

// |H(e^jw)| in dB at frequency f.
inline double biquadMagnitudeDb(const BiquadCoeffs& c, double f, double fs) {
    const std::complex<double> z1 = std::polar(1.0, -2.0 * kPiD * f / fs);
    const std::complex<double> z2 = z1 * z1;
    const std::complex<double> num = c.b0 + c.b1 * z1 + c.b2 * z2;
    const std::complex<double> den = 1.0 + c.a1 * z1 + c.a2 * z2;
    return 20.0 * std::log10(std::max(1e-12, std::abs(num / den)));
}

struct Biquad {
    BiquadCoeffs c;
    double z1 = 0.0, z2 = 0.0;

    void reset() { z1 = z2 = 0.0; }
    void set(const BiquadCoeffs& coeffs) { c = coeffs; }
    inline float process(float xin) {
        const double x = xin;
        const double y = c.b0 * x + z1;
        z1 = c.b1 * x - c.a1 * y + z2;
        z2 = c.b2 * x - c.a2 * y;
        return static_cast<float>(y);
    }
    void process(float* x, int n) {
        for (int i = 0; i < n; ++i) x[i] = process(x[i]);
    }
};

// Q values for Butterworth cascades of 2nd-order sections.
inline double butterworthQ(int order, int section) {
    // order is even (2, 4, 6, 8); section in [0, order/2)
    const double theta = kPiD * (2.0 * section + 1.0) / (2.0 * order);
    return 1.0 / (2.0 * std::cos(theta));
}

// One-pole low/high pass (6 dB/oct).
struct OnePole {
    float a = 0.0f, z = 0.0f;
    void setLowpass(double fc, double fs) { a = static_cast<float>(std::exp(-2.0 * kPiD * fc / fs)); }
    void reset() { z = 0.0f; }
    inline float lowpass(float x) { z = x + a * (z - x); return z; }
    inline float highpass(float x) { return x - lowpass(x); }
};

// DC blocker (high-pass at ~10 Hz).
struct DcBlocker {
    float r = 0.9995f, x1 = 0.0f, y1 = 0.0f;
    void prepare(double fs) { r = static_cast<float>(1.0 - 2.0 * kPiD * 10.0 / fs); }
    void reset() { x1 = y1 = 0.0f; }
    inline float process(float x) {
        const float y = x - x1 + r * y1;
        x1 = x;
        y1 = y;
        return y;
    }
};

} // namespace echo
