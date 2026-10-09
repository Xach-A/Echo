// Voice changer effects: high-quality pitch/formant shifter, low-latency
// pitch shifter, harmonizer, doubler and octaver.
#include "dsp/PitchShifter.h"
#include "dsp/effects/EffectUtil.h"

#include <cstring>

namespace echo {

namespace {

// ---------------------------------------------------------------------------
// Pitch & formant shifter (phase vocoder)
// ---------------------------------------------------------------------------
const char* const kQualityChoices[] = {"Low latency", "Balanced", "High quality", nullptr};
const ParamInfo kPitchParams[] = {
    {"semitones", "Pitch", -24.0f, 24.0f, 0.0f, "st", ParamScale::Linear, nullptr, "Pitch shift in semitones"},
    {"cents", "Fine", -100.0f, 100.0f, 0.0f, "cents", ParamScale::Linear},
    {"formant", "Formant", -12.0f, 12.0f, 0.0f, "st", ParamScale::Linear, nullptr,
     "Shifts vocal-tract resonances (perceived size/age) independently of pitch"},
    {"preserve", "Preserve formants", 0.0f, 1.0f, 1.0f, "", ParamScale::Toggle, nullptr,
     "Keep the voice's character when changing pitch (off = chipmunk/giant effect)"},
    {"quality", "Quality", 0.0f, 2.0f, 1.0f, "", ParamScale::Enum, kQualityChoices,
     "Larger analysis windows sound smoother but add latency"},
    {"output", "Output", -24.0f, 12.0f, 0.0f, "dB", ParamScale::Linear},
    {"mix", "Mix", 0.0f, 100.0f, 100.0f, "%", ParamScale::Linear},
};
ECHO_EFFECT_INFO(kPitchInfo, "pitch", "Pitch & Formant Shifter", "Voice",
                 "Phase-vocoder pitch shifting with independent formant control.", kPitchParams, false);

class PitchShiftEffect final : public Effect {
public:
    PitchShiftEffect() : Effect(kPitchInfo) {}
    int latencySamples() const override { return shifters_[active_][0].latency(); }

protected:
    void onPrepare() override {
        const int base = sr_ > 70000 ? 2 : sr_ > 35000 ? 1 : 0; // scale window with sample rate
        const int sizes[3] = {512 << base, 1024 << base, 2048 << base};
        for (int q = 0; q < 3; ++q) {
            for (auto& s : shifters_[q]) s.prepare(sizes[q], sr_);
            for (auto& a : align_[q]) a.prepare(shifters_[q][0].latency(), maxBlock_);
        }
        dry_.assign(static_cast<size_t>(maxBlock_) * 2, 0.0f);
        out_.prepare(sr_, 0.02);
    }
    void onReset() override {
        for (auto& q : shifters_)
            for (auto& s : q) s.reset();
        for (auto& q : align_)
            for (auto& a : q) a.reset();
        active_ = clampv(pi(4), 0, 2);
        out_.reset(dbToGain(p(5)));
    }
    void processImpl(float* const* ch, int numCh, int n) override {
        const int q = clampv(pi(4), 0, 2);
        if (q != active_) {
            active_ = q;
            for (auto& s : shifters_[q]) s.reset();
            for (auto& a : align_[q]) a.reset();
        }
        const float semis = p(0) + p(1) / 100.0f;
        const float ratio = std::pow(2.0f, semis / 12.0f);
        const bool preserve = pb(3);
        const float formSemis = p(2);
        const bool envelope = preserve || std::fabs(formSemis) > 0.01f;
        const float formantRatio = std::pow(2.0f, (preserve ? formSemis : semis + formSemis) / 12.0f);
        const float mix = p(6) / 100.0f;
        out_.setTarget(dbToGain(p(5)));
        for (int c = 0; c < numCh; ++c) {
            float* dry = dry_.data() + static_cast<size_t>(c) * maxBlock_;
            align_[q][c].process(ch[c], dry, n);
            PvPitchShifter& s = shifters_[q][c];
            s.setPitchRatio(ratio);
            s.setFormantRatio(formantRatio);
            s.setEnvelopeProcessing(envelope);
            s.process(ch[c], n);
        }
        for (int i = 0; i < n; ++i) {
            const float g = out_.next();
            for (int c = 0; c < numCh; ++c) ch[c][i] *= g;
        }
        if (mix < 0.999f)
            for (int c = 0; c < numCh; ++c) mixLinear(ch[c], dry_.data() + static_cast<size_t>(c) * maxBlock_, n, mix);
    }

private:
    PvPitchShifter shifters_[3][2];
    AlignDelay align_[3][2];
    std::vector<float> dry_;
    LinearSmoother out_;
    int active_ = 1;
};

// ---------------------------------------------------------------------------
// Low-latency pitch shifter (granular)
// ---------------------------------------------------------------------------
const ParamInfo kLiteParams[] = {
    {"semitones", "Pitch", -24.0f, 24.0f, 0.0f, "st", ParamScale::Linear},
    {"cents", "Fine", -100.0f, 100.0f, 0.0f, "cents", ParamScale::Linear},
    {"grain", "Grain", 10.0f, 100.0f, 35.0f, "ms", ParamScale::Linear, nullptr, "Shorter = lower latency, grainier"},
    {"mix", "Mix", 0.0f, 100.0f, 100.0f, "%", ParamScale::Linear},
};
ECHO_EFFECT_INFO(kLiteInfo, "pitch_lite", "Pitch Shifter (Low Latency)", "Voice",
                 "Granular pitch shifter with very low latency; formants follow pitch.", kLiteParams, false);

class PitchLiteEffect final : public Effect {
public:
    PitchLiteEffect() : Effect(kLiteInfo) {}

protected:
    void onPrepare() override {
        for (auto& g : grain_) g.prepare(sr_, 0.11);
    }
    void onReset() override {
        for (auto& g : grain_) g.reset();
    }
    void processImpl(float* const* ch, int numCh, int n) override {
        const float ratio = std::pow(2.0f, (p(0) + p(1) / 100.0f) / 12.0f);
        const float grain = p(2) * 0.001f * static_cast<float>(sr_);
        const float mix = p(3) / 100.0f;
        for (int c = 0; c < numCh; ++c)
            for (int i = 0; i < n; ++i) {
                const float x = ch[c][i];
                const float y = grain_[c].process(x, ratio, grain);
                ch[c][i] = x + (y - x) * mix;
            }
    }

private:
    GrainPitchShifter grain_[2];
};

// ---------------------------------------------------------------------------
// Harmonizer
// ---------------------------------------------------------------------------
const ParamInfo kHarmParams[] = {
    {"interval1", "Voice 1 interval", -24.0f, 24.0f, 4.0f, "st", ParamScale::Int, nullptr, "+4 = major third, +7 = fifth"},
    {"level1", "Voice 1 level", -60.0f, 6.0f, -6.0f, "dB", ParamScale::Linear},
    {"pan1", "Voice 1 pan", -100.0f, 100.0f, -50.0f, "%", ParamScale::Linear},
    {"interval2", "Voice 2 interval", -24.0f, 24.0f, 7.0f, "st", ParamScale::Int},
    {"level2", "Voice 2 level", -60.0f, 6.0f, -8.0f, "dB", ParamScale::Linear},
    {"pan2", "Voice 2 pan", -100.0f, 100.0f, 50.0f, "%", ParamScale::Linear},
    {"dry", "Dry level", -60.0f, 6.0f, 0.0f, "dB", ParamScale::Linear},
};
ECHO_EFFECT_INFO(kHarmInfo, "harmonizer", "Harmonizer", "Voice",
                 "Adds two pitch-shifted harmony voices (fixed intervals).", kHarmParams, true);

class HarmonizerEffect final : public Effect {
public:
    HarmonizerEffect() : Effect(kHarmInfo) {}

protected:
    void onPrepare() override {
        for (auto& g : grain_) g.prepare(sr_, 0.08);
    }
    void onReset() override {
        for (auto& g : grain_) g.reset();
    }
    void processImpl(float* const* ch, int numCh, int n) override {
        if (numCh < 2) return;
        const float r1 = std::pow(2.0f, p(0) / 12.0f), r2 = std::pow(2.0f, p(3) / 12.0f);
        const float l1 = dbToGain(p(1)), l2 = dbToGain(p(4)), dry = dbToGain(p(6));
        const float a1 = (p(2) / 100.0f + 1.0f) * kPi * 0.25f, a2 = (p(5) / 100.0f + 1.0f) * kPi * 0.25f;
        const float g1l = std::cos(a1) * l1, g1r = std::sin(a1) * l1;
        const float g2l = std::cos(a2) * l2, g2r = std::sin(a2) * l2;
        const float grain = 0.035f * static_cast<float>(sr_);
        for (int i = 0; i < n; ++i) {
            const float m = 0.5f * (ch[0][i] + ch[1][i]);
            const float v1 = grain_[0].process(m, r1, grain);
            const float v2 = grain_[1].process(m, r2, grain * 1.13f);
            ch[0][i] = ch[0][i] * dry + v1 * g1l + v2 * g2l;
            ch[1][i] = ch[1][i] * dry + v1 * g1r + v2 * g2r;
        }
    }

private:
    GrainPitchShifter grain_[2];
};

// ---------------------------------------------------------------------------
// Voice doubler
// ---------------------------------------------------------------------------
const ParamInfo kDoubParams[] = {
    {"detune", "Detune", 0.0f, 50.0f, 12.0f, "cents", ParamScale::Linear},
    {"delay", "Delay", 5.0f, 60.0f, 20.0f, "ms", ParamScale::Linear, nullptr, "Timing offset of the doubles"},
    {"width", "Width", 0.0f, 100.0f, 85.0f, "%", ParamScale::Linear},
    {"level", "Double level", -30.0f, 6.0f, -4.0f, "dB", ParamScale::Linear},
    {"dry", "Dry level", -60.0f, 6.0f, 0.0f, "dB", ParamScale::Linear},
};
ECHO_EFFECT_INFO(kDoubInfo, "doubler", "Voice Doubler", "Voice",
                 "Thickens a voice with two slightly detuned, delayed copies.", kDoubParams, true);

class DoublerEffect final : public Effect {
public:
    DoublerEffect() : Effect(kDoubInfo) {}

protected:
    void onPrepare() override {
        for (auto& g : grain_) g.prepare(sr_, 0.08);
        for (auto& d : delay_) d.prepare(static_cast<int>(sr_ * 0.07));
        lfo_.prepare(sr_);
        lfo_.setRate(0.23);
    }
    void onReset() override {
        for (auto& g : grain_) g.reset();
        for (auto& d : delay_) d.reset();
        lfo_.reset();
    }
    void processImpl(float* const* ch, int numCh, int n) override {
        if (numCh < 2) return;
        const float det = p(0);
        const float baseDelay = p(1) * 0.001f * static_cast<float>(sr_);
        const float width = p(2) / 100.0f;
        const float lvl = dbToGain(p(3)), dry = dbToGain(p(4));
        const float grain = 0.03f * static_cast<float>(sr_);
        for (int i = 0; i < n; ++i) {
            const float wob = lfo_.next();
            const float rA = std::pow(2.0f, (det * (0.8f + 0.2f * wob)) / 1200.0f);
            const float rB = std::pow(2.0f, (-det * (0.8f - 0.2f * wob)) / 1200.0f);
            const float m = 0.5f * (ch[0][i] + ch[1][i]);
            delay_[0].push(grain_[0].process(m, rA, grain));
            delay_[1].push(grain_[1].process(m, rB, grain * 1.21f));
            const float a = delay_[0].readLinear(std::max(1.0f, baseDelay * 0.5f));
            const float b = delay_[1].readLinear(std::max(1.0f, baseDelay));
            const float l = a * (0.5f + 0.5f * width) + b * (0.5f - 0.5f * width);
            const float r = b * (0.5f + 0.5f * width) + a * (0.5f - 0.5f * width);
            ch[0][i] = ch[0][i] * dry + l * lvl;
            ch[1][i] = ch[1][i] * dry + r * lvl;
        }
    }

private:
    GrainPitchShifter grain_[2];
    DelayLine delay_[2];
    SineLfo lfo_;
};

// ---------------------------------------------------------------------------
// Octaver
// ---------------------------------------------------------------------------
const ParamInfo kOctParams[] = {
    {"sub", "Octave down", -60.0f, 6.0f, -6.0f, "dB", ParamScale::Linear},
    {"up", "Octave up", -60.0f, 6.0f, -60.0f, "dB", ParamScale::Linear},
    {"dry", "Dry level", -60.0f, 6.0f, 0.0f, "dB", ParamScale::Linear},
};
ECHO_EFFECT_INFO(kOctInfo, "octaver", "Octaver", "Voice",
                 "Blends in voices one octave below and/or above.", kOctParams, false);

class OctaverEffect final : public Effect {
public:
    OctaverEffect() : Effect(kOctInfo) {}

protected:
    void onPrepare() override {
        for (auto& c : grain_)
            for (auto& g : c) g.prepare(sr_, 0.1);
    }
    void onReset() override {
        for (auto& c : grain_)
            for (auto& g : c) g.reset();
    }
    void processImpl(float* const* ch, int numCh, int n) override {
        const float sub = dbToGain(p(0)), up = dbToGain(p(1)), dry = dbToGain(p(2));
        const float grain = 0.05f * static_cast<float>(sr_);
        for (int c = 0; c < numCh; ++c)
            for (int i = 0; i < n; ++i) {
                const float x = ch[c][i];
                float y = x * dry;
                if (sub > 0.0f) y += grain_[c][0].process(x, 0.5f, grain) * sub;
                if (up > 0.0f) y += grain_[c][1].process(x, 2.0f, grain * 0.6f) * up;
                ch[c][i] = y;
            }
    }

private:
    GrainPitchShifter grain_[2][2];
};

} // namespace

void registerVoiceEffects(EffectRegistry& r) {
    registerEffect<PitchShiftEffect>(r, kPitchInfo);
    registerEffect<PitchLiteEffect>(r, kLiteInfo);
    registerEffect<HarmonizerEffect>(r, kHarmInfo);
    registerEffect<DoublerEffect>(r, kDoubInfo);
    registerEffect<OctaverEffect>(r, kOctInfo);
}

} // namespace echo
