// Modulation effects: chorus, flanger, phaser, ring modulator.
#include "dsp/DelayLine.h"
#include "dsp/effects/EffectUtil.h"

namespace echo {

namespace {

// ---------------------------------------------------------------------------
// Chorus
// ---------------------------------------------------------------------------
const ParamInfo kChorusParams[] = {
    {"rate", "Rate", 0.05f, 5.0f, 0.8f, "Hz", ParamScale::Log},
    {"depth", "Depth", 0.0f, 10.0f, 2.5f, "ms", ParamScale::Linear},
    {"delay", "Delay", 4.0f, 30.0f, 12.0f, "ms", ParamScale::Linear},
    {"voices", "Voices", 1.0f, 4.0f, 2.0f, "", ParamScale::Int},
    {"spread", "Stereo spread", 0.0f, 100.0f, 70.0f, "%", ParamScale::Linear},
    {"mix", "Mix", 0.0f, 100.0f, 50.0f, "%", ParamScale::Linear},
};
ECHO_EFFECT_INFO(kChorusInfo, "chorus", "Chorus", "Modulation",
                 "Multi-voice chorus with stereo spread.", kChorusParams, true);

class ChorusEffect final : public Effect {
public:
    ChorusEffect() : Effect(kChorusInfo) {}

protected:
    void onPrepare() override {
        for (auto& d : line_) d.prepare(static_cast<int>(sr_ * 0.05));
        lfo_.prepare(sr_);
    }
    void onReset() override {
        for (auto& d : line_) d.reset();
        lfo_.reset();
        phase_ = 0.0;
    }
    void processImpl(float* const* ch, int numCh, int n) override {
        const float depth = p(1) * 0.001f * static_cast<float>(sr_);
        const float base = p(2) * 0.001f * static_cast<float>(sr_);
        const int voices = clampv(pi(3), 1, 4);
        const float spread = p(4) / 100.0f;
        const float mix = p(5) / 100.0f;
        const float vg = 1.0f / std::sqrt(static_cast<float>(voices));
        float* L = ch[0];
        float* R = numCh > 1 ? ch[1] : nullptr;
        const double inc = p(0) / sr_;
        const double stereoOff = 0.25 * spread;
        for (int i = 0; i < n; ++i) {
            const float xl = L[i], xr = R ? R[i] : L[i];
            line_[0].push(xl);
            line_[1].push(xr);
            float wl = 0.0f, wr = 0.0f;
            for (int v = 0; v < voices; ++v) {
                const double ph = phase_ + static_cast<double>(v) / voices;
                const float ml = static_cast<float>(std::sin(2.0 * kPiD * ph)) * depth;
                const float mr = static_cast<float>(std::sin(2.0 * kPiD * (ph + stereoOff))) * depth;
                wl += line_[0].readCubic(base + ml) * vg;
                wr += line_[1].readCubic(base + mr) * vg;
            }
            phase_ += inc;
            if (phase_ >= 1.0) phase_ -= 1.0;
            L[i] = xl * (1.0f - mix * 0.5f) + wl * mix * 0.7f;
            if (R) R[i] = xr * (1.0f - mix * 0.5f) + wr * mix * 0.7f;
        }
    }

private:
    DelayLine line_[2];
    SineLfo lfo_;
    double phase_ = 0.0;
};

// ---------------------------------------------------------------------------
// Flanger
// ---------------------------------------------------------------------------
const ParamInfo kFlangerParams[] = {
    {"rate", "Rate", 0.02f, 5.0f, 0.25f, "Hz", ParamScale::Log},
    {"depth", "Depth", 0.0f, 5.0f, 2.0f, "ms", ParamScale::Linear},
    {"delay", "Delay", 0.1f, 10.0f, 1.0f, "ms", ParamScale::Log},
    {"feedback", "Feedback", -95.0f, 95.0f, 50.0f, "%", ParamScale::Linear},
    {"stereo", "Stereo phase", 0.0f, 180.0f, 90.0f, "deg", ParamScale::Linear},
    {"mix", "Mix", 0.0f, 100.0f, 50.0f, "%", ParamScale::Linear},
};
ECHO_EFFECT_INFO(kFlangerInfo, "flanger", "Flanger", "Modulation",
                 "Classic through-zero-style jet flanger with feedback.", kFlangerParams, true);

class FlangerEffect final : public Effect {
public:
    FlangerEffect() : Effect(kFlangerInfo) {}

protected:
    void onPrepare() override {
        for (auto& d : line_) d.prepare(static_cast<int>(sr_ * 0.02));
        lfo_.prepare(sr_);
    }
    void onReset() override {
        for (auto& d : line_) d.reset();
        lfo_.reset();
        fb_[0] = fb_[1] = 0.0f;
    }
    void processImpl(float* const* ch, int numCh, int n) override {
        lfo_.setRate(p(0));
        const float depth = p(1) * 0.001f * static_cast<float>(sr_);
        const float base = p(2) * 0.001f * static_cast<float>(sr_);
        const float fb = p(3) / 100.0f;
        const double off = p(4) / 360.0;
        const float mix = p(5) / 100.0f;
        for (int i = 0; i < n; ++i) {
            const double ph = lfo_.phase();
            const float m0 = 0.5f + 0.5f * static_cast<float>(std::sin(2.0 * kPiD * ph));
            const float m1 = 0.5f + 0.5f * static_cast<float>(std::sin(2.0 * kPiD * (ph + off)));
            lfo_.next();
            const float mods[2] = {m0, m1};
            for (int c = 0; c < numCh; ++c) {
                const float x = ch[c][i];
                line_[c].push(x + fb_[c] * fb);
                const float d = line_[c].readCubic(std::max(1.0f, base + mods[c] * depth));
                fb_[c] = d;
                ch[c][i] = x * (1.0f - mix * 0.5f) + d * mix * 0.7f;
            }
        }
    }

private:
    DelayLine line_[2];
    SineLfo lfo_;
    float fb_[2] = {};
};

// ---------------------------------------------------------------------------
// Phaser
// ---------------------------------------------------------------------------
const char* const kStageChoices[] = {"2", "4", "6", "8", "10", "12", nullptr};
const ParamInfo kPhaserParams[] = {
    {"stages", "Stages", 0.0f, 5.0f, 2.0f, "", ParamScale::Enum, kStageChoices},
    {"rate", "Rate", 0.02f, 5.0f, 0.4f, "Hz", ParamScale::Log},
    {"depth", "Depth", 0.0f, 100.0f, 80.0f, "%", ParamScale::Linear},
    {"center", "Center", 100.0f, 4000.0f, 700.0f, "Hz", ParamScale::Log},
    {"feedback", "Feedback", 0.0f, 90.0f, 50.0f, "%", ParamScale::Linear},
    {"stereo", "Stereo phase", 0.0f, 180.0f, 90.0f, "deg", ParamScale::Linear},
    {"mix", "Mix", 0.0f, 100.0f, 50.0f, "%", ParamScale::Linear},
};
ECHO_EFFECT_INFO(kPhaserInfo, "phaser", "Phaser", "Modulation",
                 "Swept all-pass phaser with feedback.", kPhaserParams, true);

class PhaserEffect final : public Effect {
public:
    PhaserEffect() : Effect(kPhaserInfo) {}

protected:
    void onPrepare() override { lfo_.prepare(sr_); }
    void onReset() override {
        for (auto& c : z_)
            for (auto& v : c) v = 0.0f;
        fb_[0] = fb_[1] = 0.0f;
        lfo_.reset();
    }
    void processImpl(float* const* ch, int numCh, int n) override {
        const int stages = 2 * (clampv(pi(0), 0, 5) + 1);
        lfo_.setRate(p(1));
        const float depth = p(2) / 100.0f;
        const float center = p(3);
        const float fb = p(4) / 100.0f;
        const double off = p(5) / 360.0;
        const float mix = p(6) / 100.0f;
        float coef[2] = {0.0f, 0.0f};
        for (int i = 0; i < n; ++i) {
            if ((i & 15) == 0) {
                // Update all-pass coefficients every 16 samples.
                const double ph = lfo_.phase();
                for (int c = 0; c < 2; ++c) {
                    const double s = std::sin(2.0 * kPiD * (ph + (c ? off : 0.0)));
                    const double f = clampv(center * std::pow(2.0, 2.0 * depth * s), 30.0, sr_ * 0.45);
                    const double t = std::tan(kPiD * f / sr_);
                    coef[c] = static_cast<float>((t - 1.0) / (t + 1.0));
                }
            }
            lfo_.next();
            for (int c = 0; c < numCh; ++c) {
                const float x = ch[c][i];
                float y = x + fb_[c] * fb;
                for (int s = 0; s < stages; ++s) {
                    // First-order all-pass: y = a*x + z; z = x - a*y
                    const float out = coef[c] * y + z_[c][s];
                    z_[c][s] = y - coef[c] * out;
                    y = out;
                }
                fb_[c] = y;
                ch[c][i] = x * (1.0f - mix * 0.5f) + y * mix * 0.5f;
            }
        }
    }

private:
    float z_[2][12] = {};
    float fb_[2] = {};
    SineLfo lfo_;
};

// ---------------------------------------------------------------------------
// Ring modulator
// ---------------------------------------------------------------------------
const char* const kRingWaves[] = {"Sine", "Square", "Triangle", nullptr};
const ParamInfo kRingParams[] = {
    {"freq", "Carrier", 1.0f, 5000.0f, 90.0f, "Hz", ParamScale::Log},
    {"wave", "Waveform", 0.0f, 2.0f, 0.0f, "", ParamScale::Enum, kRingWaves},
    {"lfo_rate", "Wobble rate", 0.0f, 10.0f, 0.0f, "Hz", ParamScale::Linear},
    {"lfo_depth", "Wobble depth", 0.0f, 100.0f, 0.0f, "%", ParamScale::Linear},
    {"mix", "Mix", 0.0f, 100.0f, 100.0f, "%", ParamScale::Linear},
};
ECHO_EFFECT_INFO(kRingInfo, "ringmod", "Ring Modulator", "Modulation",
                 "Multiplies the signal by a carrier: metallic, robotic, alien tones.", kRingParams, false);

class RingModEffect final : public Effect {
public:
    RingModEffect() : Effect(kRingInfo) {}

protected:
    void onPrepare() override { lfo_.prepare(sr_); }
    void onReset() override {
        phase_ = 0.0;
        lfo_.reset();
    }
    void processImpl(float* const* ch, int numCh, int n) override {
        const double f = p(0);
        const int wave = pi(1);
        lfo_.setRate(p(2));
        const double lfoDepth = p(3) / 100.0;
        const float mix = p(4) / 100.0f;
        for (int i = 0; i < n; ++i) {
            const double fm = f * std::pow(2.0, lfoDepth * lfo_.next());
            phase_ += fm / sr_;
            if (phase_ >= 1.0) phase_ -= std::floor(phase_);
            float car;
            switch (wave) {
            case 1: car = phase_ < 0.5 ? 1.0f : -1.0f; break;
            case 2: car = static_cast<float>(phase_ < 0.5 ? 4.0 * phase_ - 1.0 : 3.0 - 4.0 * phase_); break;
            default: car = static_cast<float>(std::sin(2.0 * kPiD * phase_)); break;
            }
            for (int c = 0; c < numCh; ++c) {
                const float x = ch[c][i];
                ch[c][i] = x + (x * car - x) * mix;
            }
        }
    }

private:
    double phase_ = 0.0;
    SineLfo lfo_;
};

} // namespace

void registerModulationEffects(EffectRegistry& r) {
    registerEffect<ChorusEffect>(r, kChorusInfo);
    registerEffect<FlangerEffect>(r, kFlangerInfo);
    registerEffect<PhaserEffect>(r, kPhaserInfo);
    registerEffect<RingModEffect>(r, kRingInfo);
}

} // namespace echo
