// Dynamics processors: compressor, limiter, noise gate, expander, de-esser,
// automatic gain control.
#include "dsp/Biquad.h"
#include "dsp/Limiter.h"
#include "dsp/effects/EffectUtil.h"

#include <cstring>

namespace echo {

namespace {

// Static gain computer with soft knee (Giannoulis, Massberg & Reiss 2012).
inline float compressGainDb(float xDb, float thr, float ratio, float knee) {
    const float over = xDb - thr;
    float y;
    if (2.0f * over < -knee) y = xDb;
    else if (knee > 0.0f && 2.0f * std::fabs(over) <= knee) {
        const float t = over + knee * 0.5f;
        y = xDb + (1.0f / ratio - 1.0f) * t * t / (2.0f * knee);
    } else y = thr + over / ratio;
    return y - xDb; // <= 0
}

// ---------------------------------------------------------------------------
// Compressor
// ---------------------------------------------------------------------------
const char* const kDetectorChoices[] = {"Peak", "RMS", nullptr};
const ParamInfo kCompParams[] = {
    {"threshold", "Threshold", -60.0f, 0.0f, -18.0f, "dB", ParamScale::Linear, nullptr, "Level where compression starts"},
    {"ratio", "Ratio", 1.0f, 20.0f, 4.0f, ":1", ParamScale::Log},
    {"attack", "Attack", 0.1f, 200.0f, 8.0f, "ms", ParamScale::Log},
    {"release", "Release", 5.0f, 2000.0f, 150.0f, "ms", ParamScale::Log},
    {"knee", "Knee", 0.0f, 24.0f, 6.0f, "dB", ParamScale::Linear},
    {"makeup", "Makeup", -12.0f, 24.0f, 0.0f, "dB", ParamScale::Linear},
    {"auto_makeup", "Auto makeup", 0.0f, 1.0f, 0.0f, "", ParamScale::Toggle},
    {"detector", "Detector", 0.0f, 1.0f, 0.0f, "", ParamScale::Enum, kDetectorChoices},
    {"sc_hpf", "Sidechain HPF", 20.0f, 500.0f, 20.0f, "Hz", ParamScale::Log, nullptr, "Ignore low frequencies in detection"},
    {"mix", "Mix", 0.0f, 100.0f, 100.0f, "%", ParamScale::Linear, nullptr, "Parallel (New York) compression"},
};
ECHO_EFFECT_INFO(kCompInfo, "compressor", "Compressor", "Dynamics",
                 "Feed-forward stereo-linked compressor with soft knee.", kCompParams, false);

class CompressorEffect final : public Effect {
public:
    CompressorEffect() : Effect(kCompInfo) {}
    const char* visLabel(int s) const override { return s == 0 ? "Gain reduction (dB)" : nullptr; }

protected:
    void onPrepare() override { makeup_.prepare(sr_, 0.05); }
    void onReset() override {
        g_ = 0.0f;
        ms_ = 0.0f;
        for (auto& f : sc_) f.reset();
        makeup_.reset(dbToGain(makeupDb()));
        scFreq_ = -1.0f;
    }
    void processImpl(float* const* ch, int numCh, int n) override {
        const float thr = p(0), ratio = p(1), knee = p(4);
        const float aA = timeToCoeff(sr_, p(2) * 0.001), aR = timeToCoeff(sr_, p(3) * 0.001);
        const bool rms = pi(7) == 1;
        const float rmsCoeff = timeToCoeff(sr_, 0.010);
        const float mix = p(9) / 100.0f;
        if (p(8) != scFreq_) {
            scFreq_ = p(8);
            const BiquadCoeffs c = designBiquad(FilterType::HighPass, sr_, scFreq_, 0.707);
            sc_[0].set(c);
            sc_[1].set(c);
        }
        const bool useSc = scFreq_ > 21.0f;
        makeup_.setTarget(dbToGain(makeupDb()));
        float minG = 0.0f;
        for (int i = 0; i < n; ++i) {
            float d = 0.0f;
            for (int c = 0; c < numCh; ++c) {
                const float s = useSc ? sc_[c].process(ch[c][i]) : ch[c][i];
                d = std::max(d, std::fabs(s));
            }
            float xDb;
            if (rms) {
                ms_ = d * d + rmsCoeff * (ms_ - d * d);
                xDb = ms_ > 1e-12f ? 10.0f * std::log10(ms_) : -120.0f;
            } else {
                xDb = gainToDb(d);
            }
            const float gc = compressGainDb(xDb, thr, ratio, knee);
            g_ = gc < g_ ? aA * g_ + (1.0f - aA) * gc : aR * g_ + (1.0f - aR) * gc;
            minG = std::min(minG, g_);
            const float gain = dbToGain(g_) * makeup_.next();
            for (int c = 0; c < numCh; ++c) {
                const float x = ch[c][i];
                const float y = x * gain;
                ch[c][i] = mix >= 0.999f ? y : x + (y - x) * mix;
            }
        }
        vis[0].store(minG, std::memory_order_relaxed);
    }

private:
    float makeupDb() const {
        float m = p(5);
        if (pb(6)) m += -compressGainDb(0.0f, p(0), p(1), p(4)) * 0.5f;
        return m;
    }
    float g_ = 0.0f, ms_ = 0.0f, scFreq_ = -1.0f;
    Biquad sc_[2];
    LinearSmoother makeup_;
};

// ---------------------------------------------------------------------------
// Limiter
// ---------------------------------------------------------------------------
const ParamInfo kLimParams[] = {
    {"drive", "Input gain", 0.0f, 24.0f, 0.0f, "dB", ParamScale::Linear, nullptr, "Push the signal into the limiter"},
    {"ceiling", "Ceiling", -24.0f, 0.0f, -1.0f, "dBFS", ParamScale::Linear, nullptr, "Peaks never exceed this level"},
    {"release", "Release", 1.0f, 1000.0f, 80.0f, "ms", ParamScale::Log},
};
ECHO_EFFECT_INFO(kLimInfo, "limiter", "Limiter", "Dynamics",
                 "Brick-wall look-ahead peak limiter (2 ms look-ahead).", kLimParams, false);

class LimiterEffect final : public Effect {
public:
    LimiterEffect() : Effect(kLimInfo) {}
    int latencySamples() const override { return lim_.latencySamples(); }
    const char* visLabel(int s) const override { return s == 0 ? "Gain reduction (dB)" : nullptr; }

protected:
    void onPrepare() override {
        lim_.prepare(sr_, 0.002, 2);
        drive_.prepare(sr_, 0.02);
    }
    void onReset() override {
        lim_.reset();
        drive_.reset(dbToGain(p(0)));
    }
    void processImpl(float* const* ch, int numCh, int n) override {
        drive_.setTarget(dbToGain(p(0)));
        for (int i = 0; i < n; ++i) {
            const float g = drive_.next();
            for (int c = 0; c < numCh; ++c) ch[c][i] *= g;
        }
        lim_.setCeilingDb(p(1));
        lim_.setReleaseSeconds(p(2) * 0.001f);
        lim_.process(ch, numCh, n);
        vis[0].store(lim_.currentGainReductionDb(), std::memory_order_relaxed);
    }

private:
    LookaheadLimiter lim_;
    LinearSmoother drive_;
};

// ---------------------------------------------------------------------------
// Noise gate
// ---------------------------------------------------------------------------
const ParamInfo kGateParams[] = {
    {"threshold", "Threshold", -90.0f, 0.0f, -45.0f, "dB", ParamScale::Linear, nullptr, "Gate opens above this level"},
    {"hysteresis", "Hysteresis", 0.0f, 20.0f, 4.0f, "dB", ParamScale::Linear, nullptr, "Closes this far below the threshold"},
    {"attack", "Attack", 0.05f, 50.0f, 0.5f, "ms", ParamScale::Log},
    {"hold", "Hold", 0.0f, 1000.0f, 60.0f, "ms", ParamScale::Linear},
    {"release", "Release", 5.0f, 2000.0f, 150.0f, "ms", ParamScale::Log},
    {"range", "Range", -90.0f, 0.0f, -80.0f, "dB", ParamScale::Linear, nullptr, "Attenuation while closed"},
    {"sc_hpf", "Key HPF", 20.0f, 1000.0f, 80.0f, "Hz", ParamScale::Log, nullptr, "Ignore rumble when deciding to open"},
};
ECHO_EFFECT_INFO(kGateInfo, "gate", "Noise Gate", "Dynamics",
                 "Silences the signal below a threshold, with hysteresis and hold.", kGateParams, false);

class GateEffect final : public Effect {
public:
    GateEffect() : Effect(kGateInfo) {}
    const char* visLabel(int s) const override {
        return s == 0 ? "Gate open" : s == 1 ? "Key level (dB)" : nullptr;
    }

protected:
    void onPrepare() override {}
    void onReset() override {
        env_ = 0.0f;
        gain_ = dbToGain(p(5));
        open_ = false;
        holdLeft_ = 0;
        for (auto& f : key_) f.reset();
        keyFreq_ = -1.0f;
    }
    void processImpl(float* const* ch, int numCh, int n) override {
        if (p(6) != keyFreq_) {
            keyFreq_ = p(6);
            const BiquadCoeffs c = designBiquad(FilterType::HighPass, sr_, keyFreq_, 0.707);
            key_[0].set(c);
            key_[1].set(c);
        }
        const float openThr = dbToGain(p(0));
        const float closeThr = dbToGain(p(0) - p(1));
        const float floorG = dbToGain(p(5));
        const float envRel = timeToCoeff(sr_, 0.010);
        const float aA = timeToCoeff(sr_, p(2) * 0.001);
        const float aR = timeToCoeff(sr_, p(4) * 0.001);
        const int holdSamples = static_cast<int>(p(3) * 0.001f * sr_);
        float maxEnv = 0.0f;
        for (int i = 0; i < n; ++i) {
            float k = 0.0f;
            for (int c = 0; c < numCh; ++c) k = std::max(k, std::fabs(key_[c].process(ch[c][i])));
            env_ = k > env_ ? k : k + envRel * (env_ - k); // instant attack, 10 ms release
            maxEnv = std::max(maxEnv, env_);
            if (env_ >= openThr) {
                open_ = true;
                holdLeft_ = holdSamples;
            } else if (open_ && env_ < closeThr) {
                if (holdLeft_ > 0) --holdLeft_;
                else open_ = false;
            } else if (open_ && holdLeft_ > 0) {
                --holdLeft_;
            }
            const float target = open_ ? 1.0f : floorG;
            const float a = target > gain_ ? aA : aR;
            gain_ = target + a * (gain_ - target);
            for (int c = 0; c < numCh; ++c) ch[c][i] *= gain_;
        }
        vis[0].store(open_ ? 1.0f : 0.0f, std::memory_order_relaxed);
        vis[1].store(gainToDb(maxEnv), std::memory_order_relaxed);
    }

private:
    float env_ = 0.0f, gain_ = 0.0f, keyFreq_ = -1.0f;
    bool open_ = false;
    int holdLeft_ = 0;
    Biquad key_[2];
};

// ---------------------------------------------------------------------------
// Downward expander
// ---------------------------------------------------------------------------
const ParamInfo kExpParams[] = {
    {"threshold", "Threshold", -80.0f, 0.0f, -40.0f, "dB", ParamScale::Linear},
    {"ratio", "Ratio", 1.0f, 10.0f, 2.0f, ":1", ParamScale::Log, nullptr, "Downward expansion ratio below threshold"},
    {"attack", "Attack", 0.1f, 100.0f, 2.0f, "ms", ParamScale::Log},
    {"release", "Release", 5.0f, 2000.0f, 120.0f, "ms", ParamScale::Log},
    {"range", "Range", -80.0f, 0.0f, -30.0f, "dB", ParamScale::Linear, nullptr, "Maximum attenuation"},
    {"knee", "Knee", 0.0f, 24.0f, 6.0f, "dB", ParamScale::Linear},
};
ECHO_EFFECT_INFO(kExpInfo, "expander", "Expander", "Dynamics",
                 "Gentle downward expander: lowers quiet noise without hard gating.", kExpParams, false);

class ExpanderEffect final : public Effect {
public:
    ExpanderEffect() : Effect(kExpInfo) {}
    const char* visLabel(int s) const override { return s == 0 ? "Gain reduction (dB)" : nullptr; }

protected:
    void onPrepare() override {}
    void onReset() override {
        g_ = 0.0f;
        env_ = 0.0f;
    }
    void processImpl(float* const* ch, int numCh, int n) override {
        const float thr = p(0), ratio = p(1), range = p(4), knee = p(5);
        const float aA = timeToCoeff(sr_, p(2) * 0.001), aR = timeToCoeff(sr_, p(3) * 0.001);
        const float envRel = timeToCoeff(sr_, 0.020);
        float minG = 0.0f;
        for (int i = 0; i < n; ++i) {
            float d = 0.0f;
            for (int c = 0; c < numCh; ++c) d = std::max(d, std::fabs(ch[c][i]));
            env_ = d > env_ ? d : d + envRel * (env_ - d);
            const float xDb = gainToDb(env_);
            const float under = thr - xDb; // > 0 below threshold
            float gc = 0.0f;
            if (under > knee * 0.5f) gc = -under * (ratio - 1.0f);
            else if (knee > 0.0f && under > -knee * 0.5f) {
                const float t = under + knee * 0.5f;
                gc = -(ratio - 1.0f) * t * t / (2.0f * knee);
            }
            gc = std::max(gc, range);
            // Attack = gain recovering (opening), release = closing.
            g_ = gc > g_ ? aA * g_ + (1.0f - aA) * gc : aR * g_ + (1.0f - aR) * gc;
            minG = std::min(minG, g_);
            const float gain = dbToGain(g_);
            for (int c = 0; c < numCh; ++c) ch[c][i] *= gain;
        }
        vis[0].store(minG, std::memory_order_relaxed);
    }

private:
    float g_ = 0.0f, env_ = 0.0f;
};

// ---------------------------------------------------------------------------
// De-esser (dynamic high band, complementary split)
// ---------------------------------------------------------------------------
const char* const kDeessModes[] = {"Split band", "Wideband", nullptr};
const ParamInfo kDeessParams[] = {
    {"freq", "Frequency", 2000.0f, 12000.0f, 6000.0f, "Hz", ParamScale::Log, nullptr, "Sibilance band starts here"},
    {"threshold", "Threshold", -60.0f, 0.0f, -30.0f, "dB", ParamScale::Linear},
    {"reduction", "Max reduction", 0.0f, 24.0f, 10.0f, "dB", ParamScale::Linear},
    {"mode", "Mode", 0.0f, 1.0f, 0.0f, "", ParamScale::Enum, kDeessModes},
    {"listen", "Listen", 0.0f, 1.0f, 0.0f, "", ParamScale::Toggle, nullptr, "Hear only the detected sibilance"},
};
ECHO_EFFECT_INFO(kDeessInfo, "deesser", "De-esser", "Dynamics",
                 "Reduces harsh 's' and 'sh' sounds by compressing the high band.", kDeessParams, false);

class DeesserEffect final : public Effect {
public:
    DeesserEffect() : Effect(kDeessInfo) {}
    const char* visLabel(int s) const override { return s == 0 ? "Gain reduction (dB)" : nullptr; }

protected:
    void onPrepare() override {}
    void onReset() override {
        g_ = 0.0f;
        env_ = 0.0f;
        freq_ = -1.0f;
        for (auto& c : hp_)
            for (auto& f : c) f.reset();
        for (auto& c : lp_)
            for (auto& f : c) f.reset();
        for (auto& f : det_) f.reset();
    }
    void processImpl(float* const* ch, int numCh, int n) override {
        if (p(0) != freq_) {
            freq_ = glideLog(freq_ < 0 ? p(0) : freq_, p(0), 0.5f);
            // Linkwitz-Riley 4th-order crossover: LP4 + HP4 sums to an
            // all-pass, so the bands separate cleanly and recombine flat.
            const BiquadCoeffs hp = designBiquad(FilterType::HighPass, sr_, freq_, 0.7071);
            const BiquadCoeffs lp = designBiquad(FilterType::LowPass, sr_, freq_, 0.7071);
            const BiquadCoeffs bp = designBiquad(FilterType::HighPass, sr_, freq_, 1.2);
            for (int c = 0; c < 2; ++c) {
                hp_[c][0].set(hp);
                hp_[c][1].set(hp);
                lp_[c][0].set(lp);
                lp_[c][1].set(lp);
                det_[c].set(bp);
            }
        }
        const float thr = p(1), maxRed = p(2);
        const bool wide = pi(3) == 1, listen = pb(4);
        const float aA = timeToCoeff(sr_, 0.0005), aR = timeToCoeff(sr_, 0.060);
        const float envRel = timeToCoeff(sr_, 0.005);
        float minG = 0.0f;
        for (int i = 0; i < n; ++i) {
            float d = 0.0f;
            for (int c = 0; c < numCh; ++c) d = std::max(d, std::fabs(det_[c].process(ch[c][i])));
            env_ = d > env_ ? d : d + envRel * (env_ - d);
            const float over = gainToDb(env_) - thr;
            const float gc = over > 0.0f ? std::max(-maxRed, -over * 0.75f) : 0.0f; // ~4:1
            g_ = gc < g_ ? aA * g_ + (1.0f - aA) * gc : aR * g_ + (1.0f - aR) * gc;
            minG = std::min(minG, g_);
            const float gain = dbToGain(g_);
            for (int c = 0; c < numCh; ++c) {
                const float x = ch[c][i];
                const float hi = hp_[c][1].process(hp_[c][0].process(x));
                const float lo = lp_[c][1].process(lp_[c][0].process(x));
                if (listen) ch[c][i] = hi;
                else if (wide) ch[c][i] = x * gain;
                else ch[c][i] = lo + hi * gain; // low band + attenuated high band
            }
        }
        vis[0].store(minG, std::memory_order_relaxed);
    }

private:
    float g_ = 0.0f, env_ = 0.0f, freq_ = -1.0f;
    Biquad hp_[2][2], lp_[2][2], det_[2];
};

// ---------------------------------------------------------------------------
// Automatic gain control
// ---------------------------------------------------------------------------
const ParamInfo kAgcParams[] = {
    {"target", "Target level", -40.0f, -6.0f, -20.0f, "dBFS", ParamScale::Linear, nullptr, "Average (RMS) level to aim for"},
    {"max_gain", "Max boost", 0.0f, 36.0f, 18.0f, "dB", ParamScale::Linear},
    {"max_cut", "Max cut", 0.0f, 36.0f, 12.0f, "dB", ParamScale::Linear},
    {"speed", "Speed", 1.0f, 30.0f, 6.0f, "dB/s", ParamScale::Log, nullptr, "How quickly the gain may change"},
    {"gate", "Silence threshold", -90.0f, -20.0f, -55.0f, "dBFS", ParamScale::Linear, nullptr, "Gain is frozen below this level"},
};
ECHO_EFFECT_INFO(kAgcInfo, "agc", "Auto Gain (AGC)", "Dynamics",
                 "Slowly rides the gain toward a target loudness; frozen during silence.", kAgcParams, false);

class AgcEffect final : public Effect {
public:
    AgcEffect() : Effect(kAgcInfo) {}
    const char* visLabel(int s) const override {
        return s == 0 ? "Applied gain (dB)" : s == 1 ? "Measured level (dBFS)" : nullptr;
    }

protected:
    void onPrepare() override {}
    void onReset() override {
        ms_ = 0.0f;
        gainDb_ = 0.0f;
    }
    void processImpl(float* const* ch, int numCh, int n) override {
        const float a = timeToCoeff(sr_, 0.4); // ~400 ms loudness window
        const float target = p(0), maxG = p(1), maxC = p(2), rate = p(3) / static_cast<float>(sr_), gate = p(4);
        float level = -120.0f;
        for (int i = 0; i < n; ++i) {
            float s = 0.0f;
            for (int c = 0; c < numCh; ++c) s += ch[c][i] * ch[c][i];
            s /= static_cast<float>(numCh);
            ms_ = s + a * (ms_ - s);
            level = ms_ > 1e-12f ? 10.0f * std::log10(ms_) : -120.0f;
            if (level > gate) {
                const float want = clampv(target - level, -maxC, maxG);
                const float d = want - gainDb_;
                gainDb_ += clampv(d, -rate, rate);
            }
            const float g = dbToGain(gainDb_);
            for (int c = 0; c < numCh; ++c) ch[c][i] *= g;
        }
        vis[0].store(gainDb_, std::memory_order_relaxed);
        vis[1].store(level, std::memory_order_relaxed);
    }

private:
    float ms_ = 0.0f, gainDb_ = 0.0f;
};

} // namespace

void registerDynamicsEffects(EffectRegistry& r) {
    registerEffect<CompressorEffect>(r, kCompInfo);
    registerEffect<LimiterEffect>(r, kLimInfo);
    registerEffect<GateEffect>(r, kGateInfo);
    registerEffect<ExpanderEffect>(r, kExpInfo);
    registerEffect<DeesserEffect>(r, kDeessInfo);
    registerEffect<AgcEffect>(r, kAgcInfo);
}

} // namespace echo
