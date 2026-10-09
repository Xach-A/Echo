// Parametric EQ and high/low-pass filters.
#include "dsp/Biquad.h"
#include "dsp/effects/EffectUtil.h"
#include "dsp/Responses.h"

#include <cstring>

namespace echo {

namespace {

constexpr int kEqBands = 6;

#define EQ_BAND(n, label, f, q)                                                                              \
    {"b" #n "_on", label " on", 0.0f, 1.0f, 1.0f, "", ParamScale::Toggle},                                     \
    {"b" #n "_freq", label " freq", 20.0f, 20000.0f, f, "Hz", ParamScale::Log},                                \
    {"b" #n "_gain", label " gain", -24.0f, 24.0f, 0.0f, "dB", ParamScale::Linear},                             \
    {"b" #n "_q", label " Q", 0.1f, 10.0f, q, "", ParamScale::Log}

const ParamInfo kEqParams[] = {
    EQ_BAND(1, "Low shelf", 100.0f, 0.71f),
    EQ_BAND(2, "Bell 1", 250.0f, 1.0f),
    EQ_BAND(3, "Bell 2", 1000.0f, 1.0f),
    EQ_BAND(4, "Bell 3", 3000.0f, 1.0f),
    EQ_BAND(5, "Bell 4", 6000.0f, 1.0f),
    EQ_BAND(6, "High shelf", 10000.0f, 0.71f),
    {"output", "Output", -24.0f, 24.0f, 0.0f, "dB", ParamScale::Linear},
};
#undef EQ_BAND
ECHO_EFFECT_INFO(kEqInfo, "eq", "Parametric EQ", "EQ & Filters",
                 "Six-band parametric equaliser: low shelf, four bells, high shelf.", kEqParams, false);

FilterType eqBandType(int b) {
    return b == 0 ? FilterType::LowShelf : b == kEqBands - 1 ? FilterType::HighShelf : FilterType::Peak;
}

class EqEffect final : public Effect {
public:
    EqEffect() : Effect(kEqInfo) {}

protected:
    void onPrepare() override { out_.prepare(sr_, 0.02); }
    void onReset() override {
        for (int b = 0; b < kEqBands; ++b) {
            Band& bd = bands_[b];
            bd.freq = p(4 * b + 1);
            bd.gain = pb(4 * b) ? p(4 * b + 2) : 0.0f;
            bd.q = p(4 * b + 3);
            update(b);
            for (auto& f : bd.f) f.reset();
        }
        out_.reset(dbToGain(p(4 * kEqBands)));
        snap_ = true;
    }
    void processImpl(float* const* ch, int numCh, int n) override {
        const float amt = snap_ ? 1.0f : 0.35f;
        snap_ = false;
        for (int b = 0; b < kEqBands; ++b) {
            Band& bd = bands_[b];
            const float tf = p(4 * b + 1), tg = pb(4 * b) ? p(4 * b + 2) : 0.0f, tq = p(4 * b + 3);
            if (tf != bd.freq || tg != bd.gain || tq != bd.q) {
                bd.freq = glideLog(bd.freq, tf, amt);
                bd.gain = glideLinear(bd.gain, tg, amt);
                bd.q = glideLog(bd.q, tq, amt);
                update(b);
            }
            if (std::fabs(bd.gain) < 0.01f) continue; // identity
            for (int c = 0; c < numCh; ++c) bd.f[c].process(ch[c], n);
        }
        out_.setTarget(dbToGain(p(4 * kEqBands)));
        if (numCh == 1) out_.applyGain(ch[0], n);
        else
            for (int i = 0; i < n; ++i) {
                const float g = out_.next();
                ch[0][i] *= g;
                ch[1][i] *= g;
            }
    }

private:
    struct Band {
        Biquad f[2];
        float freq = 1000.0f, gain = 0.0f, q = 1.0f;
    };
    void update(int b) {
        Band& bd = bands_[b];
        const BiquadCoeffs c = designBiquad(eqBandType(b), sr_, bd.freq, bd.q, bd.gain);
        bd.f[0].set(c);
        bd.f[1].set(c);
    }
    Band bands_[kEqBands];
    LinearSmoother out_;
    bool snap_ = true;
};

// --------------------------------------------------------------------------

const char* const kSlopeChoices[] = {"12 dB/oct", "24 dB/oct", "36 dB/oct", "48 dB/oct", nullptr};
const ParamInfo kFilterParams[] = {
    {"hp_on", "High-pass", 0.0f, 1.0f, 1.0f, "", ParamScale::Toggle, nullptr, "Remove rumble below the cutoff"},
    {"hp_freq", "HP cutoff", 10.0f, 2000.0f, 80.0f, "Hz", ParamScale::Log},
    {"hp_slope", "HP slope", 0.0f, 3.0f, 1.0f, "", ParamScale::Enum, kSlopeChoices},
    {"lp_on", "Low-pass", 0.0f, 1.0f, 0.0f, "", ParamScale::Toggle, nullptr, "Remove content above the cutoff"},
    {"lp_freq", "LP cutoff", 200.0f, 22000.0f, 16000.0f, "Hz", ParamScale::Log},
    {"lp_slope", "LP slope", 0.0f, 3.0f, 1.0f, "", ParamScale::Enum, kSlopeChoices},
};
ECHO_EFFECT_INFO(kFilterInfo, "filter", "High/Low-Pass Filter", "EQ & Filters",
                 "Butterworth high-pass and low-pass filters, 12 to 48 dB/octave.", kFilterParams, false);

class FilterEffect final : public Effect {
public:
    FilterEffect() : Effect(kFilterInfo) {}

protected:
    void onPrepare() override {}
    void onReset() override {
        for (int k = 0; k < 2; ++k) {
            Stage& s = stages_[k];
            s.freq = effectiveTarget(k);
            s.order = order(k);
            design(k);
            for (auto& c : s.f)
                for (auto& bq : c) bq.reset();
        }
        snap_ = true;
    }
    void processImpl(float* const* ch, int numCh, int n) override {
        processStages(ch, numCh, n);
        snap_ = false;
    }
    void processStages(float* const* ch, int numCh, int n) {
        for (int k = 0; k < 2; ++k) {
            Stage& s = stages_[k];
            const float target = effectiveTarget(k);
            const int ord = order(k);
            if (ord != s.order) {
                s.order = ord;
                for (auto& c : s.f)
                    for (auto& bq : c) bq.reset();
                design(k);
            }
            if (target != s.freq) {
                s.freq = glideLog(s.freq, target, snap_ ? 1.0f : 0.3f);
                design(k);
            }
            // Fully "off": the cutoff has glided to the extreme of the band.
            if (!pb(k * 3) && s.freq == target) continue;
            for (int c = 0; c < numCh; ++c)
                for (int sec = 0; sec < s.order / 2; ++sec) s.f[c][sec].process(ch[c], n);
        }
    }

private:
    struct Stage {
        Biquad f[2][4];
        float freq = 1000.0f;
        int order = 4;
    };
    int order(int k) const { return 2 * (pi(k * 3 + 2) + 1); }
    float effectiveTarget(int k) const {
        if (pb(k * 3)) return p(k * 3 + 1);
        return k == 0 ? 5.0f : static_cast<float>(sr_ * 0.49);
    }
    void design(int k) {
        Stage& s = stages_[k];
        const FilterType t = k == 0 ? FilterType::HighPass : FilterType::LowPass;
        const float f = std::min(s.freq, static_cast<float>(sr_ * 0.49));
        for (int sec = 0; sec < s.order / 2; ++sec) {
            const BiquadCoeffs c = designBiquad(t, sr_, f, butterworthQ(s.order, sec));
            s.f[0][sec].set(c);
            s.f[1][sec].set(c);
        }
    }
    Stage stages_[2];
    bool snap_ = true;
};

} // namespace

double eqResponseDb(const Effect& e, double f, double fs) {
    double db = 0.0;
    for (int b = 0; b < kEqBands; ++b) {
        if (e.param(4 * b) < 0.5f) continue;
        const BiquadCoeffs c = designBiquad(eqBandType(b), fs, e.param(4 * b + 1), e.param(4 * b + 3), e.param(4 * b + 2));
        db += biquadMagnitudeDb(c, f, fs);
    }
    return db + e.param(4 * kEqBands);
}

double filterResponseDb(const Effect& e, double f, double fs) {
    double db = 0.0;
    for (int k = 0; k < 2; ++k) {
        if (e.param(k * 3) < 0.5f) continue;
        const int ord = 2 * (static_cast<int>(e.param(k * 3 + 2)) + 1);
        for (int sec = 0; sec < ord / 2; ++sec) {
            const BiquadCoeffs c = designBiquad(k == 0 ? FilterType::HighPass : FilterType::LowPass, fs,
                                                e.param(k * 3 + 1), butterworthQ(ord, sec));
            db += biquadMagnitudeDb(c, f, fs);
        }
    }
    return db;
}

void registerEqEffects(EffectRegistry& r) {
    registerEffect<EqEffect>(r, kEqInfo);
    registerEffect<FilterEffect>(r, kFilterInfo);
}

} // namespace echo
