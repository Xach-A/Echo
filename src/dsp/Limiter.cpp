#include "dsp/Limiter.h"

#include <algorithm>
#include <cmath>

#include "core/Common.h"
#include "core/Smoothing.h"

namespace echo {

void LookaheadLimiter::prepare(double sampleRate, double lookaheadSeconds, int channels) {
    sr_ = sampleRate;
    look_ = std::max(1, static_cast<int>(std::lround(sampleRate * lookaheadSeconds)));
    channels_ = std::clamp(channels, 1, 2);
    delay_.assign(static_cast<size_t>(look_) * 2, 0.0f);
    dqCap_ = look_ + 2;
    dqVal_.assign(dqCap_, 1.0f);
    dqIdx_.assign(dqCap_, 0);
    box_.assign(look_, 1.0f);
    setReleaseSeconds(0.08f);
    reset();
}

void LookaheadLimiter::reset() {
    std::fill(delay_.begin(), delay_.end(), 0.0f);
    dpos_ = 0;
    dqHead_ = dqTail_ = 0;
    sampleIndex_ = 0;
    env_ = 1.0f;
    std::fill(box_.begin(), box_.end(), 1.0f);
    boxPos_ = 0;
    boxSum_ = static_cast<double>(look_);
    boxRefresh_ = 0;
    grDb_ = 0.0f;
}

void LookaheadLimiter::setCeilingDb(float db) { ceiling_ = dbToGain(std::min(db, 0.0f)); }

void LookaheadLimiter::setReleaseSeconds(float s) {
    releaseCoeff_ = timeToCoeff(sr_, std::max(0.001f, s));
}

void LookaheadLimiter::process(float* const* ch, int numCh, int n) {
    float* x0 = ch[0];
    float* x1 = numCh > 1 ? ch[1] : nullptr;
    float* d0 = delay_.data();
    float* d1 = delay_.data() + look_;
    const float ceil = ceiling_;
    float minGain = 1.0f;

    for (int i = 0; i < n; ++i) {
        float pk = std::fabs(x0[i]);
        if (x1) pk = std::max(pk, std::fabs(x1[i]));
        const float req = pk > ceil ? ceil / pk : 1.0f;

        // Sliding minimum over the last look_+1 required gains.
        while (dqHead_ != dqTail_) {
            const int back = (dqTail_ - 1 + dqCap_) % dqCap_;
            if (dqVal_[back] >= req) dqTail_ = back;
            else break;
        }
        dqVal_[dqTail_] = req;
        dqIdx_[dqTail_] = sampleIndex_;
        dqTail_ = (dqTail_ + 1) % dqCap_;
        while (dqIdx_[dqHead_] < sampleIndex_ - look_) dqHead_ = (dqHead_ + 1) % dqCap_;
        const float hold = dqVal_[dqHead_];

        // Instant attack, exponential release.
        env_ = hold < env_ ? hold : hold + releaseCoeff_ * (env_ - hold);

        // Box filter: turns the step into a ramp that completes on time.
        boxSum_ += static_cast<double>(env_) - box_[boxPos_];
        box_[boxPos_] = env_;
        if (++boxPos_ == look_) boxPos_ = 0;
        float g = static_cast<float>(boxSum_ / look_);
        g = std::min(g, 1.0f);
        minGain = std::min(minGain, g);

        const float y0 = d0[dpos_];
        d0[dpos_] = x0[i];
        x0[i] = y0 * g;
        if (x1) {
            const float y1 = d1[dpos_];
            d1[dpos_] = x1[i];
            x1[i] = y1 * g;
        }
        if (++dpos_ == look_) dpos_ = 0;

        if (++sampleIndex_ >= (1 << 30)) {
            // Re-base indices long before overflow.
            for (int k = 0; k < dqCap_; ++k) dqIdx_[k] -= (1 << 29);
            sampleIndex_ -= (1 << 29);
        }
    }
    // Refresh the running sum periodically to cancel accumulated rounding.
    boxRefresh_ += n;
    if (boxRefresh_ > look_ * 256) {
        double s = 0.0;
        for (float v : box_) s += v;
        boxSum_ = s;
        boxRefresh_ = 0;
    }
    grDb_ = gainToDb(minGain);
}

} // namespace echo
