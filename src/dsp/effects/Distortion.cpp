// Distortion/saturation, bit-crusher and character voices (telephone,
// radio, walkie-talkie, megaphone...).
#include "dsp/Biquad.h"
#include "dsp/DelayLine.h"
#include "dsp/effects/EffectUtil.h"

namespace echo {

namespace {

inline float shape(int type, float x) {
    switch (type) {
    case 0: return std::tanh(x);                                   // soft clip
    case 1: return clampv(x, -1.0f, 1.0f);                         // hard clip
    case 2: {                                                      // tube: asymmetric
        const float y = x >= 0.0f ? 1.0f - std::exp(-x) : -0.8f * (1.0f - std::exp(1.25f * x));
        return y;
    }
    case 3: {                                                      // fuzz: squared-off
        const float y = std::tanh(3.0f * x);
        return y * y * y * 0.6f + y * 0.4f;
    }
    case 4: {                                                      // foldback
        float y = x;
        for (int k = 0; k < 4 && std::fabs(y) > 1.0f; ++k) y = y > 1.0f ? 2.0f - y : (y < -1.0f ? -2.0f - y : y);
        return clampv(y, -1.0f, 1.0f);
    }
    default: return x;
    }
}

// ---------------------------------------------------------------------------
// Distortion
// ---------------------------------------------------------------------------
const char* const kDistTypes[] = {"Soft clip", "Hard clip", "Tube", "Fuzz", "Foldback", nullptr};
const ParamInfo kDistParams[] = {
    {"drive", "Drive", 0.0f, 48.0f, 18.0f, "dB", ParamScale::Linear},
    {"type", "Type", 0.0f, 4.0f, 0.0f, "", ParamScale::Enum, kDistTypes},
    {"low_cut", "Low cut", 20.0f, 1000.0f, 80.0f, "Hz", ParamScale::Log, nullptr, "Filter before the clipper"},
    {"tone", "Tone", 500.0f, 16000.0f, 6000.0f, "Hz", ParamScale::Log, nullptr, "Low-pass after the clipper"},
    {"output", "Output", -36.0f, 12.0f, -8.0f, "dB", ParamScale::Linear},
    {"mix", "Mix", 0.0f, 100.0f, 100.0f, "%", ParamScale::Linear},
};
ECHO_EFFECT_INFO(kDistInfo, "distortion", "Distortion / Saturation", "Distortion",
                 "4x oversampled waveshaping distortion with tone control.", kDistParams, false);

class DistortionEffect final : public Effect {
public:
    DistortionEffect() : Effect(kDistInfo) {}

protected:
    void onPrepare() override {
        for (auto& o : os_) o.prepare();
        drive_.prepare(sr_, 0.03);
        out_.prepare(sr_, 0.03);
    }
    void onReset() override {
        for (auto& o : os_) o.reset();
        for (auto& f : pre_) f.reset();
        for (auto& f : post_) f.reset();
        for (auto& d : dc_) d.reset();
        lcF_ = toneF_ = -1.0f;
        drive_.reset(dbToGain(p(0)));
        out_.reset(dbToGain(p(4)));
        for (auto& d : dc_) d.prepare(sr_);
    }
    void processImpl(float* const* ch, int numCh, int n) override {
        if (p(2) != lcF_ || p(3) != toneF_) {
            lcF_ = p(2);
            toneF_ = p(3);
            for (int c = 0; c < 2; ++c) {
                pre_[c].set(designBiquad(FilterType::HighPass, sr_, lcF_, 0.707));
                post_[c].set(designBiquad(FilterType::LowPass, sr_, toneF_, 0.707));
            }
        }
        const int type = pi(1);
        const float mix = p(5) / 100.0f;
        drive_.setTarget(dbToGain(p(0)));
        out_.setTarget(dbToGain(p(4)));
        float up[Oversampler4x::kFactor];
        for (int i = 0; i < n; ++i) {
            const float g = drive_.next(), og = out_.next();
            for (int c = 0; c < numCh; ++c) {
                const float x = ch[c][i];
                os_[c].up(pre_[c].process(x) * g, up);
                for (float& u : up) u = shape(type, u);
                const float y = dc_[c].process(post_[c].process(os_[c].down(up))) * og;
                ch[c][i] = x + (y - x) * mix;
            }
        }
    }

private:
    Oversampler4x os_[2];
    Biquad pre_[2], post_[2];
    DcBlocker dc_[2];
    LinearSmoother drive_, out_;
    float lcF_ = -1.0f, toneF_ = -1.0f;
};

// ---------------------------------------------------------------------------
// Bit-crusher / downsampler
// ---------------------------------------------------------------------------
const ParamInfo kCrushParams[] = {
    {"bits", "Bit depth", 1.0f, 24.0f, 8.0f, "bits", ParamScale::Linear},
    {"downsample", "Downsample", 1.0f, 64.0f, 4.0f, "x", ParamScale::Log, nullptr, "Sample-and-hold factor"},
    {"dither", "Dither", 0.0f, 1.0f, 0.0f, "", ParamScale::Toggle},
    {"output", "Output", -24.0f, 12.0f, 0.0f, "dB", ParamScale::Linear},
    {"mix", "Mix", 0.0f, 100.0f, 100.0f, "%", ParamScale::Linear},
};
ECHO_EFFECT_INFO(kCrushInfo, "bitcrusher", "Bit-crusher / Downsampler", "Distortion",
                 "Reduces bit depth and sample rate for lo-fi and retro sounds.", kCrushParams, false);

class BitcrusherEffect final : public Effect {
public:
    BitcrusherEffect() : Effect(kCrushInfo) {}

protected:
    void onPrepare() override {}
    void onReset() override {
        phase_ = 0.0f;
        held_[0] = held_[1] = 0.0f;
    }
    void processImpl(float* const* ch, int numCh, int n) override {
        const float bits = p(0);
        const float levels = std::pow(2.0f, bits - 1.0f);
        const float step = 1.0f / std::max(1.0f, p(1));
        const bool dither = pb(2);
        const float og = dbToGain(p(3));
        const float mix = p(4) / 100.0f;
        for (int i = 0; i < n; ++i) {
            phase_ += step;
            const bool sample = phase_ >= 1.0f;
            if (sample) phase_ -= 1.0f;
            for (int c = 0; c < numCh; ++c) {
                const float x = ch[c][i];
                if (sample) {
                    float v = x;
                    if (dither) {
                        rng_ = rng_ * 1664525u + 1013904223u;
                        const float r1 = static_cast<float>(rng_ >> 8) / 16777216.0f;
                        rng_ = rng_ * 1664525u + 1013904223u;
                        const float r2 = static_cast<float>(rng_ >> 8) / 16777216.0f;
                        v += (r1 - r2) / levels; // TPDF
                    }
                    held_[c] = std::round(v * levels) / levels;
                }
                const float y = held_[c] * og;
                ch[c][i] = x + (y - x) * mix;
            }
        }
    }

private:
    float phase_ = 0.0f;
    float held_[2] = {};
    uint32_t rng_ = 22222u;
};

// ---------------------------------------------------------------------------
// Voice character: band-limited "device" voices.
// ---------------------------------------------------------------------------
const char* const kCharModes[] = {"Telephone", "AM radio", "Walkie-talkie", "Megaphone", "Intercom / PA",
                                  "Old gramophone", nullptr};
const ParamInfo kCharParams[] = {
    {"mode", "Device", 0.0f, 5.0f, 0.0f, "", ParamScale::Enum, kCharModes},
    {"intensity", "Intensity", 0.0f, 100.0f, 70.0f, "%", ParamScale::Linear, nullptr, "Amount of distortion and coloration"},
    {"noise", "Noise / static", -90.0f, -20.0f, -60.0f, "dB", ParamScale::Linear},
    {"output", "Output", -24.0f, 12.0f, 0.0f, "dB", ParamScale::Linear},
    {"mix", "Mix", 0.0f, 100.0f, 100.0f, "%", ParamScale::Linear},
};
ECHO_EFFECT_INFO(kCharInfo, "character", "Voice Character", "Voice",
                 "Telephone, radio, walkie-talkie, megaphone, PA and gramophone voices.", kCharParams, false);

class CharacterEffect final : public Effect {
public:
    CharacterEffect() : Effect(kCharInfo) {}

protected:
    struct Mode {
        float hp, lp, peakF, peakG, peakQ, drive, crushRate;
        int clipType;
        float crackle;
    };
    static constexpr Mode kModes[6] = {
        {300.0f, 3400.0f, 1700.0f, 4.0f, 1.0f, 6.0f, 8000.0f, 0, 0.0f},     // telephone
        {200.0f, 4500.0f, 1200.0f, 3.0f, 0.8f, 10.0f, 0.0f, 2, 0.0f},       // AM radio
        {500.0f, 2800.0f, 1500.0f, 6.0f, 1.2f, 18.0f, 11025.0f, 1, 0.0f},   // walkie-talkie
        {450.0f, 4000.0f, 1600.0f, 9.0f, 2.0f, 20.0f, 0.0f, 1, 0.0f},       // megaphone
        {250.0f, 5000.0f, 2500.0f, 5.0f, 1.0f, 4.0f, 0.0f, 0, 0.0f},        // intercom / PA
        {350.0f, 3000.0f, 1000.0f, 5.0f, 0.7f, 8.0f, 0.0f, 2, 1.0f},        // gramophone
    };
    void onPrepare() override {
        for (auto& d : pa_) d.prepare(static_cast<int>(sr_ * 0.2));
    }
    void onReset() override {
        mode_ = -1;
        for (auto& c : f_)
            for (auto& b : c) b.reset();
        for (auto& d : pa_) d.reset();
        held_[0] = held_[1] = 0.0f;
        phase_ = 0.0f;
    }
    void processImpl(float* const* ch, int numCh, int n) override {
        const int m = clampv(pi(0), 0, 5);
        if (m != mode_) {
            mode_ = m;
            const Mode& md = kModes[m];
            for (int c = 0; c < 2; ++c) {
                f_[c][0].set(designBiquad(FilterType::HighPass, sr_, md.hp, 0.707));
                f_[c][1].set(designBiquad(FilterType::HighPass, sr_, md.hp, 0.707));
                f_[c][2].set(designBiquad(FilterType::LowPass, sr_, md.lp, 0.707));
                f_[c][3].set(designBiquad(FilterType::LowPass, sr_, md.lp, 0.707));
                f_[c][4].set(designBiquad(FilterType::Peak, sr_, md.peakF, md.peakQ, md.peakG));
            }
        }
        const Mode& md = kModes[m];
        const float inten = p(1) / 100.0f;
        const float drive = dbToGain(md.drive * inten);
        const float noise = dbToGain(p(2));
        const float og = dbToGain(p(3));
        const float mix = p(4) / 100.0f;
        const float crushStep = md.crushRate > 0.0f ? md.crushRate / static_cast<float>(sr_) : 1.0f;
        const bool pa = m == 4;
        for (int i = 0; i < n; ++i) {
            phase_ += crushStep;
            const bool sample = phase_ >= 1.0f;
            if (sample) phase_ -= 1.0f;
            rng_ = rng_ * 1664525u + 1013904223u;
            float nz = (static_cast<float>(rng_ >> 9) / 4194304.0f - 1.0f) * noise;
            if (md.crackle > 0.0f && (rng_ & 0xFFF) == 0) nz += 0.3f * noise * 30.0f; // vinyl ticks
            for (int c = 0; c < numCh; ++c) {
                const float x = ch[c][i];
                float y = x + nz;
                y = f_[c][0].process(y);
                y = f_[c][1].process(y);
                y = f_[c][4].process(y);
                y = shape(md.clipType, y * drive) / std::max(1.0f, std::sqrt(drive));
                if (sample) held_[c] = y;
                if (md.crushRate > 0.0f) y = held_[c];
                y = f_[c][2].process(y);
                y = f_[c][3].process(y);
                if (pa) {
                    // Short slap-back reflections of a public address horn.
                    pa_[c].push(y);
                    y += 0.35f * pa_[c].read(static_cast<int>(sr_ * 0.045)) +
                         0.2f * pa_[c].read(static_cast<int>(sr_ * 0.11));
                }
                y *= og;
                ch[c][i] = x + (y - x) * mix;
            }
        }
    }

private:
    Biquad f_[2][5];
    DelayLine pa_[2];
    int mode_ = -1;
    float held_[2] = {};
    float phase_ = 0.0f;
    uint32_t rng_ = 987654321u;
};

} // namespace

void registerDistortionEffects(EffectRegistry& r) {
    registerEffect<DistortionEffect>(r, kDistInfo);
    registerEffect<BitcrusherEffect>(r, kCrushInfo);
    registerEffect<CharacterEffect>(r, kCharInfo);
}

} // namespace echo
