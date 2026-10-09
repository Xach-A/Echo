// Delay and reverb.
#include "dsp/Biquad.h"
#include "dsp/DelayLine.h"
#include "dsp/effects/EffectUtil.h"

#include <cstring>

namespace echo {

namespace {

// ---------------------------------------------------------------------------
// Delay / echo
// ---------------------------------------------------------------------------
const ParamInfo kDelayParams[] = {
    {"time", "Time", 1.0f, 2000.0f, 320.0f, "ms", ParamScale::Log},
    {"feedback", "Feedback", 0.0f, 95.0f, 35.0f, "%", ParamScale::Linear},
    {"mix", "Mix", 0.0f, 100.0f, 30.0f, "%", ParamScale::Linear},
    {"pingpong", "Ping-pong", 0.0f, 1.0f, 0.0f, "", ParamScale::Toggle, nullptr, "Echoes alternate left and right"},
    {"low_cut", "Low cut", 20.0f, 2000.0f, 150.0f, "Hz", ParamScale::Log, nullptr, "Filters the repeats"},
    {"high_cut", "High cut", 1000.0f, 20000.0f, 7000.0f, "Hz", ParamScale::Log, nullptr, "Darkens the repeats"},
};
ECHO_EFFECT_INFO(kDelayInfo, "delay", "Delay / Echo", "Time & Space",
                 "Stereo feedback delay with filtered repeats and ping-pong mode.", kDelayParams, true);

class DelayEffect final : public Effect {
public:
    DelayEffect() : Effect(kDelayInfo) {}

protected:
    void onPrepare() override {
        for (auto& d : line_) d.prepare(static_cast<int>(sr_ * 2.1) + 8);
    }
    void onReset() override {
        for (auto& d : line_) d.reset();
        for (auto& f : lc_) f.reset();
        for (auto& f : hc_) f.reset();
        time_ = -1.0f; // snap to the parameter on the first block
        lcF_ = hcF_ = -1.0f;
    }
    void processImpl(float* const* ch, int numCh, int n) override {
        if (p(4) != lcF_ || p(5) != hcF_) {
            lcF_ = p(4);
            hcF_ = p(5);
            for (int c = 0; c < 2; ++c) {
                lc_[c].set(designBiquad(FilterType::HighPass, sr_, lcF_, 0.707));
                hc_[c].set(designBiquad(FilterType::LowPass, sr_, hcF_, 0.707));
            }
        }
        const float targetTime = clampv(static_cast<float>(p(0) * 0.001 * sr_), 1.0f,
                                        static_cast<float>(line_[0].maxDelay() - 4));
        if (time_ < 0.0f) time_ = targetTime;
        const float fb = p(1) / 100.0f;
        const float mix = p(2) / 100.0f;
        const float dryG = std::cos(mix * kPi * 0.5f), wetG = std::sin(mix * kPi * 0.5f);
        const bool ping = pb(3);
        // Slew the delay time (~50 ms glide) so changes sound like tape, not clicks.
        const float slew = 1.0f - timeToCoeff(sr_, 0.05);
        float* L = ch[0];
        float* R = numCh > 1 ? ch[1] : ch[0];
        for (int i = 0; i < n; ++i) {
            time_ += (targetTime - time_) * slew;
            const float dl = line_[0].readCubic(time_);
            const float dr = line_[1].readCubic(time_);
            const float wl = hc_[0].process(lc_[0].process(dl));
            const float wr = hc_[1].process(lc_[1].process(dr));
            const float xl = L[i], xr = numCh > 1 ? R[i] : L[i];
            if (ping) {
                // Mono input feeds the left line; the lines cross-feed.
                line_[0].push(0.5f * (xl + xr) + wr * fb);
                line_[1].push(wl * fb);
            } else {
                line_[0].push(xl + wl * fb);
                line_[1].push(xr + wr * fb);
            }
            L[i] = xl * dryG + dl * wetG;
            if (numCh > 1) R[i] = xr * dryG + dr * wetG;
        }
    }

private:
    DelayLine line_[2];
    Biquad lc_[2], hc_[2];
    float time_ = 0.0f, lcF_ = -1.0f, hcF_ = -1.0f;
};

// ---------------------------------------------------------------------------
// Reverb: 8-line feedback delay network with Householder mixing, input
// diffusion, per-line frequency-dependent decay and gentle modulation.
// ---------------------------------------------------------------------------
const ParamInfo kRevParams[] = {
    {"decay", "Decay time", 0.1f, 12.0f, 1.6f, "s", ParamScale::Log, nullptr, "RT60 at low/mid frequencies"},
    {"size", "Room size", 0.3f, 1.5f, 0.8f, "", ParamScale::Linear},
    {"predelay", "Pre-delay", 0.0f, 200.0f, 15.0f, "ms", ParamScale::Linear},
    {"damping", "HF damping", 0.0f, 1.0f, 0.5f, "", ParamScale::Linear, nullptr, "Higher = darker tail"},
    {"diffusion", "Diffusion", 0.0f, 1.0f, 0.7f, "", ParamScale::Linear},
    {"low_cut", "Low cut", 20.0f, 1000.0f, 120.0f, "Hz", ParamScale::Log},
    {"width", "Width", 0.0f, 100.0f, 100.0f, "%", ParamScale::Linear},
    {"mix", "Mix", 0.0f, 100.0f, 22.0f, "%", ParamScale::Linear},
};
ECHO_EFFECT_INFO(kRevInfo, "reverb", "Reverb", "Time & Space",
                 "Smooth algorithmic room/hall reverb (feedback delay network).", kRevParams, true);

class ReverbEffect final : public Effect {
public:
    ReverbEffect() : Effect(kRevInfo) {}

protected:
    static constexpr int kLines = 8;
    static constexpr int kDiff = 4;
    void onPrepare() override {
        const double maxSize = 1.5;
        for (int i = 0; i < kLines; ++i) lines_[i].prepare(static_cast<int>(kBaseMs[i] * 0.001 * sr_ * maxSize) + 64);
        for (int i = 0; i < kDiff; ++i) diff_[i].prepare(static_cast<int>(kDiffMs[i] * 0.001 * sr_ * maxSize) + 8);
        pre_.prepare(static_cast<int>(0.21 * sr_));
        lfo_.prepare(sr_);
        lfo_.setRate(0.37);
    }
    void onReset() override {
        for (auto& l : lines_) l.reset();
        for (auto& d : diff_) d.reset();
        pre_.reset();
        std::memset(damp_, 0, sizeof(damp_));
        lc_.reset();
        lcF_ = -1.0f;
        lfo_.reset();
    }
    void processImpl(float* const* ch, int numCh, int n) override {
        const float size = p(1);
        const float rt60 = std::max(0.05f, p(0));
        const float dampAmt = p(3);
        const float diffusion = p(4) * 0.7f;
        const float width = p(6) / 100.0f;
        const float mix = p(7) / 100.0f;
        const float dryG = std::cos(mix * kPi * 0.5f), wetG = std::sin(mix * kPi * 0.5f);
        const int preSamples = static_cast<int>(p(2) * 0.001f * sr_);
        if (p(5) != lcF_) {
            lcF_ = p(5);
            lc_.set(designBiquad(FilterType::HighPass, sr_, lcF_, 0.707));
        }
        float len[kLines], gDc[kLines], aDamp[kLines], bDamp[kLines];
        for (int i = 0; i < kLines; ++i) {
            len[i] = static_cast<float>(kBaseMs[i] * 0.001 * sr_ * size);
            // Decay gain for this line length, and a lower gain at Nyquist
            // (HF decays faster as damping increases).
            gDc[i] = std::pow(10.0f, -3.0f * len[i] / (rt60 * static_cast<float>(sr_)));
            const float hfRt = rt60 * (1.0f - 0.85f * dampAmt);
            const float gNy = std::pow(10.0f, -3.0f * len[i] / (std::max(0.05f, hfRt) * static_cast<float>(sr_)));
            aDamp[i] = (gDc[i] - gNy) / (gDc[i] + gNy);
            bDamp[i] = gDc[i] * (1.0f - aDamp[i]);
        }
        float dlen[kDiff];
        for (int i = 0; i < kDiff; ++i) dlen[i] = static_cast<float>(kDiffMs[i] * 0.001 * sr_ * size);

        float* L = ch[0];
        float* R = numCh > 1 ? ch[1] : nullptr;
        for (int i = 0; i < n; ++i) {
            const float in = lc_.process(R ? 0.5f * (L[i] + R[i]) : L[i]);
            pre_.push(in);
            float x = preSamples > 0 ? pre_.read(preSamples) : in;
            // Input diffusion: series Schroeder allpasses.
            for (int d = 0; d < kDiff; ++d) {
                const float delayed = diff_[d].read(static_cast<int>(dlen[d]));
                const float v = x + diffusion * delayed;
                diff_[d].push(v);
                x = delayed - diffusion * v;
            }
            // Read line outputs (two lines gently modulated).
            const float mod = lfo_.next() * static_cast<float>(sr_ * 0.0004);
            float o[kLines];
            for (int k = 0; k < kLines; ++k) {
                const float d = len[k] + (k == 1 ? mod : k == 5 ? -mod : 0.0f);
                float y = lines_[k].readLinear(std::max(2.0f, d));
                // Frequency-dependent decay (one-pole absorption filter).
                damp_[k] = bDamp[k] * y + aDamp[k] * damp_[k];
                o[k] = damp_[k];
            }
            // Householder feedback: f = o - (2/N) * sum(o)
            float sum = 0.0f;
            for (float v : o) sum += v;
            const float hh = sum * (2.0f / kLines);
            for (int k = 0; k < kLines; ++k) lines_[k].push(o[k] - hh + x * (k & 1 ? -0.5f : 0.5f));
            const float wl = (o[0] - o[2] + o[4] - o[6] + o[1] * 0.5f) * 0.6f;
            const float wr = (o[1] - o[3] + o[5] - o[7] + o[2] * 0.5f) * 0.6f;
            const float m = 0.5f * (wl + wr), s = 0.5f * (wl - wr) * width;
            const float outL = m + s, outR = m - s;
            if (R) {
                L[i] = L[i] * dryG + outL * wetG;
                R[i] = R[i] * dryG + outR * wetG;
            } else {
                L[i] = L[i] * dryG + m * wetG;
            }
        }
    }

private:
    static constexpr double kBaseMs[kLines] = {31.7, 37.1, 41.9, 46.3, 53.3, 59.9, 67.1, 73.7};
    static constexpr double kDiffMs[kDiff] = {4.77, 3.59, 12.73, 9.31};
    DelayLine lines_[kLines];
    DelayLine diff_[kDiff];
    DelayLine pre_;
    float damp_[kLines] = {};
    Biquad lc_;
    float lcF_ = -1.0f;
    SineLfo lfo_;
};

} // namespace

void registerTimeEffects(EffectRegistry& r) {
    registerEffect<DelayEffect>(r, kDelayInfo);
    registerEffect<ReverbEffect>(r, kRevInfo);
}

} // namespace echo
