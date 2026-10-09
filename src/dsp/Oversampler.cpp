#include <cmath>
#include <cstring>

#include "dsp/DelayLine.h"

namespace echo {

namespace {
double i0(double x) {
    double s = 1.0, t = 1.0;
    for (int k = 1; k < 40; ++k) {
        t *= (x * x * 0.25) / (k * k);
        s += t;
    }
    return s;
}
} // namespace

void Oversampler4x::prepare() {
    // Low-pass at 0.45 of the base-rate Nyquist band, designed at 4x rate.
    double h[kTaps];
    const double fc = 0.9 / kFactor; // normalised to oversampled Nyquist
    const double beta = 7.0;
    const double mid = (kTaps - 1) * 0.5;
    double sum = 0.0;
    for (int i = 0; i < kTaps; ++i) {
        const double x = i - mid;
        const double s = std::abs(x) < 1e-9 ? 1.0 : std::sin(kPiD * fc * x) / (kPiD * fc * x);
        const double r = x / (mid + 1.0);
        const double w = i0(beta * std::sqrt(std::max(0.0, 1.0 - r * r))) / i0(beta);
        h[i] = fc * s * w;
        sum += h[i];
    }
    for (double& v : h) v /= sum; // unity DC gain at the oversampled rate
    for (int p = 0; p < kFactor; ++p)
        for (int j = 0; j < kTapsPerPhase; ++j)
            up_[p * kTapsPerPhase + j] = static_cast<float>(kFactor * h[kFactor * (kTapsPerPhase - 1 - j) + p]);
    for (int i = 0; i < kTaps; ++i) down_[i] = static_cast<float>(h[kTaps - 1 - i]);
    reset();
}

void Oversampler4x::reset() {
    std::memset(hist_, 0, sizeof(hist_));
    std::memset(dhist_, 0, sizeof(dhist_));
    histPos_ = 0;
    dpos_ = 0;
}

} // namespace echo
