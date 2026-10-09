#include "dsp/PitchDetector.h"

#include <algorithm>
#include <cmath>

namespace echo {

void PitchDetector::prepare(double sampleRate, double minHz, double maxHz) {
    sr_ = sampleRate;
    minHz_ = minHz;
    maxHz_ = maxHz;
    dec_ = std::max(1, static_cast<int>(sampleRate / 12000.0));
    fsd_ = sampleRate / dec_;
    tauMin_ = std::max(2, static_cast<int>(fsd_ / maxHz));
    tauMax_ = std::max(tauMin_ + 4, static_cast<int>(std::ceil(fsd_ / minHz)) + 2);
    win_ = std::max(tauMax_, static_cast<int>(0.020 * fsd_));
    const double cutoff = std::min(0.4 * fsd_, 4000.0);
    for (int s = 0; s < 2; ++s) aa_[s].set(designBiquad(FilterType::LowPass, sampleRate, cutoff, butterworthQ(4, s)));
    decBuf_.assign(static_cast<size_t>(win_ + tauMax_) * 2, 0.0f);
    fullBuf_.assign(static_cast<size_t>((win_ + tauMax_) * dec_) + 64, 0.0f);
    frame_.assign(static_cast<size_t>(win_ + tauMax_), 0.0f);
    diff_.assign(static_cast<size_t>(tauMax_ + 2), 0.0f);
    cmnd_.assign(static_cast<size_t>(tauMax_ + 2), 0.0f);
    reset();
}

void PitchDetector::reset() {
    for (auto& f : aa_) f.reset();
    std::fill(decBuf_.begin(), decBuf_.end(), 0.0f);
    std::fill(fullBuf_.begin(), fullBuf_.end(), 0.0f);
    decPos_ = fullPos_ = decPhase_ = 0;
}

void PitchDetector::push(float x) {
    fullBuf_[fullPos_] = x;
    if (++fullPos_ == static_cast<int>(fullBuf_.size())) fullPos_ = 0;
    const float y = aa_[1].process(aa_[0].process(x));
    if (++decPhase_ >= dec_) {
        decPhase_ = 0;
        decBuf_[decPos_] = y;
        if (++decPos_ == static_cast<int>(decBuf_.size())) decPos_ = 0;
    }
}

void PitchDetector::push(const float* x, int n) {
    for (int i = 0; i < n; ++i) push(x[i]);
}

PitchDetector::Result PitchDetector::analyze() {
    Result r;
    const int total = win_ + tauMax_;
    const int size = static_cast<int>(decBuf_.size());
    for (int i = 0; i < total; ++i) frame_[i] = decBuf_[(decPos_ - total + i + size) % size];

    double energy = 0.0;
    for (int i = tauMax_; i < total; ++i) energy += static_cast<double>(frame_[i]) * frame_[i];
    r.rmsDb = energy > 0 ? static_cast<float>(10.0 * std::log10(energy / win_ + 1e-30)) : -120.0f;
    if (r.rmsDb < silenceDb_) return r;

    // Difference function over the most recent window (lags look back).
    const float* x = frame_.data() + tauMax_;
    for (int tau = 1; tau <= tauMax_; ++tau) {
        double d = 0.0;
        const float* y = x - tau;
        for (int j = 0; j < win_; ++j) {
            const double v = static_cast<double>(x[j]) - y[j];
            d += v * v;
        }
        diff_[tau] = static_cast<float>(d);
    }
    // Cumulative mean normalised difference.
    double run = 0.0;
    cmnd_[0] = 1.0f;
    for (int tau = 1; tau <= tauMax_; ++tau) {
        run += diff_[tau];
        cmnd_[tau] = run > 0 ? static_cast<float>(diff_[tau] * tau / run) : 1.0f;
    }
    int best = -1;
    for (int tau = tauMin_; tau < tauMax_; ++tau) {
        if (cmnd_[tau] < threshold_) {
            while (tau + 1 < tauMax_ && cmnd_[tau + 1] < cmnd_[tau]) ++tau;
            best = tau;
            break;
        }
    }
    if (best < 0) {
        best = tauMin_;
        for (int tau = tauMin_ + 1; tau < tauMax_; ++tau)
            if (cmnd_[tau] < cmnd_[best]) best = tau;
    }
    r.clarity = 1.0f - std::min(1.0f, cmnd_[best]);
    // Parabolic interpolation on the CMNDF.
    double tauEst = best;
    if (best > 1 && best < tauMax_) {
        const double a = cmnd_[best - 1], b = cmnd_[best], c = cmnd_[best + 1];
        const double den = a - 2.0 * b + c;
        if (std::abs(den) > 1e-12) tauEst = best + 0.5 * (a - c) / den;
    }

    // Refine at full rate: search +/- one decimated sample.
    double periodFull = tauEst * dec_;
    if (dec_ > 1) {
        const int fsize = static_cast<int>(fullBuf_.size());
        const int wFull = std::min(win_ * dec_, fsize - static_cast<int>(periodFull) - dec_ - 8);
        const int lo = std::max(2, static_cast<int>(std::floor(periodFull - dec_)));
        const int hi = static_cast<int>(std::ceil(periodFull + dec_));
        double bestD = 1e300;
        int bestT = static_cast<int>(std::lround(periodFull));
        double dv[64];
        const int count = std::min(63, hi - lo + 1);
        for (int k = 0; k < count; ++k) {
            const int tau = lo + k;
            double d = 0.0;
            for (int j = 0; j < wFull; j += 2) {
                const int i0 = (fullPos_ - 1 - j + fsize) % fsize;
                const int i1 = (fullPos_ - 1 - j - tau + 2 * fsize) % fsize;
                const double v = static_cast<double>(fullBuf_[i0]) - fullBuf_[i1];
                d += v * v;
            }
            dv[k] = d;
            if (d < bestD) {
                bestD = d;
                bestT = tau;
            }
        }
        const int bk = bestT - lo;
        double refined = bestT;
        if (bk > 0 && bk < count - 1) {
            const double a = dv[bk - 1], b = dv[bk], c = dv[bk + 1];
            const double den = a - 2.0 * b + c;
            if (std::abs(den) > 1e-20) refined = bestT + 0.5 * (a - c) / den;
        }
        periodFull = refined;
    }

    const bool periodic = cmnd_[best] < std::max(threshold_ * 2.0f, 0.3f);
    if (!periodic || periodFull <= 0.0) return r;
    r.periodSamples = static_cast<float>(periodFull);
    r.hz = static_cast<float>(sr_ / periodFull);
    r.voiced = r.hz >= minHz_ * 0.95 && r.hz <= maxHz_ * 1.05;
    if (!r.voiced) {
        r.hz = 0.0f;
        r.periodSamples = 0.0f;
    }
    return r;
}

} // namespace echo
