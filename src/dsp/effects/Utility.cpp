// Gain/trim and stereo utility effects.
#include "dsp/effects/EffectUtil.h"

namespace echo {

namespace {

const ParamInfo kGainParams[] = {
    {"gain", "Gain", -60.0f, 24.0f, 0.0f, "dB", ParamScale::Linear, nullptr, "Gain applied to the signal"},
    {"invert", "Invert polarity", 0.0f, 1.0f, 0.0f, "", ParamScale::Toggle, nullptr, "Flip the waveform polarity"},
};
ECHO_EFFECT_INFO(kGainInfo, "gain", "Gain / Trim", "Utility", "Clean gain change with polarity invert.",
                 kGainParams, false);

class GainEffect final : public Effect {
public:
    GainEffect() : Effect(kGainInfo) {}

protected:
    void onPrepare() override { sm_.prepare(sr_, 0.02); }
    void onReset() override { sm_.reset(target()); }
    void processImpl(float* const* ch, int numCh, int n) override {
        sm_.setTarget(target());
        if (!sm_.isSmoothing()) {
            for (int c = 0; c < numCh; ++c) sm_.applyGain(ch[c], n);
            return;
        }
        for (int i = 0; i < n; ++i) {
            const float g = sm_.next();
            for (int c = 0; c < numCh; ++c) ch[c][i] *= g;
        }
    }

private:
    float target() const {
        const float g = dbToGain(p(0));
        return pb(1) ? -g : g;
    }
    LinearSmoother sm_;
};

// --------------------------------------------------------------------------

const ParamInfo kStereoParams[] = {
    {"width", "Width", 0.0f, 200.0f, 100.0f, "%", ParamScale::Linear, nullptr, "0% = mono, 100% = unchanged, 200% = extra wide"},
    {"balance", "Balance", -100.0f, 100.0f, 0.0f, "%", ParamScale::Linear, nullptr, "Left/right balance"},
    {"mono", "Mono", 0.0f, 1.0f, 0.0f, "", ParamScale::Toggle, nullptr, "Sum to mono"},
    {"swap", "Swap L/R", 0.0f, 1.0f, 0.0f, "", ParamScale::Toggle, nullptr, "Swap left and right"},
    {"output", "Output", -24.0f, 12.0f, 0.0f, "dB", ParamScale::Linear},
};
ECHO_EFFECT_INFO(kStereoInfo, "stereo", "Stereo Width & Balance", "Utility",
                 "Mid/side width, balance, mono and channel swap.", kStereoParams, true);

class StereoEffect final : public Effect {
public:
    StereoEffect() : Effect(kStereoInfo) {}

protected:
    void onPrepare() override {
        width_.prepare(sr_, 0.03);
        balL_.prepare(sr_, 0.03);
        balR_.prepare(sr_, 0.03);
        out_.prepare(sr_, 0.02);
    }
    void onReset() override {
        width_.reset(p(0) / 100.0f);
        balL_.reset(1.0f);
        balR_.reset(1.0f);
        out_.reset(dbToGain(p(4)));
    }
    void processImpl(float* const* ch, int numCh, int n) override {
        if (numCh < 2) return;
        float* L = ch[0];
        float* R = ch[1];
        if (pb(3))
            for (int i = 0; i < n; ++i) std::swap(L[i], R[i]);
        width_.setTarget(pb(2) ? 0.0f : p(0) / 100.0f);
        const float bal = p(1) / 100.0f;
        balL_.setTarget(bal > 0.0f ? 1.0f - bal : 1.0f);
        balR_.setTarget(bal < 0.0f ? 1.0f + bal : 1.0f);
        out_.setTarget(dbToGain(p(4)));
        for (int i = 0; i < n; ++i) {
            const float m = 0.5f * (L[i] + R[i]);
            const float s = 0.5f * (L[i] - R[i]) * width_.next();
            const float g = out_.next();
            L[i] = (m + s) * balL_.next() * g;
            R[i] = (m - s) * balR_.next() * g;
        }
    }

private:
    LinearSmoother width_, balL_, balR_, out_;
};

} // namespace

void registerUtilityEffects(EffectRegistry& r) {
    registerEffect<GainEffect>(r, kGainInfo);
    registerEffect<StereoEffect>(r, kStereoInfo);
}

} // namespace echo
