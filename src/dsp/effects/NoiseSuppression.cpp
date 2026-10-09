// Spectral noise suppression: minimum-statistics noise tracking with a
// decision-directed Wiener gain (Ephraim & Malah style a-priori SNR), plus
// spectral and temporal gain smoothing to limit "musical noise".
#include "dsp/Stft.h"
#include "dsp/effects/EffectUtil.h"

namespace echo {

namespace {

const ParamInfo kNsParams[] = {
    {"reduction", "Reduction", 0.0f, 40.0f, 20.0f, "dB", ParamScale::Linear, nullptr, "Maximum noise attenuation"},
    {"sensitivity", "Sensitivity", 0.5f, 4.0f, 1.5f, "x", ParamScale::Log, nullptr, "Higher removes more, may thin the voice"},
    {"smoothing", "Smoothing", 0.0f, 100.0f, 50.0f, "%", ParamScale::Linear, nullptr, "Reduces watery artifacts"},
    {"learn_fast", "Fast adaptation", 0.0f, 1.0f, 0.0f, "", ParamScale::Toggle, nullptr, "Track changing noise more quickly"},
};
ECHO_EFFECT_INFO(kNsInfo, "denoise", "Spectral Noise Reduction (classic)", "Restoration",
                 "Classic statistical (Wiener) reduction of steady noise such as hum and hiss. For voice, "
                 "Noise Suppression (RNNoise) usually sounds better.", kNsParams, false);

class NoiseSuppressorEffect final : public Effect {
public:
    NoiseSuppressorEffect() : Effect(kNsInfo) {}
    int latencySamples() const override { return stft_[0].latency(); }
    const char* visLabel(int s) const override {
        return s == 0 ? "Noise floor (dB)" : s == 1 ? "Avg. attenuation (dB)" : nullptr;
    }

protected:
    void onPrepare() override {
        int n = 1024;
        if (sr_ <= 24000) n = 512;
        else if (sr_ > 50000) n = 2048;
        for (auto& s : stft_) s.prepare(n);
        bins_ = n / 2 + 1;
        for (auto& st : state_) {
            st.noise.assign(bins_, 0.0f);
            st.smooth.assign(bins_, 0.0f);
            st.minimum.assign(bins_, 0.0f);
            st.prevGain.assign(bins_, 1.0f);
            st.prevPow.assign(bins_, 0.0f);
            st.gain.assign(bins_, 1.0f);
        }
    }
    void onReset() override {
        for (auto& s : stft_) s.reset();
        for (auto& st : state_) {
            std::fill(st.noise.begin(), st.noise.end(), 0.0f);
            std::fill(st.smooth.begin(), st.smooth.end(), 0.0f);
            std::fill(st.minimum.begin(), st.minimum.end(), 0.0f);
            std::fill(st.prevGain.begin(), st.prevGain.end(), 1.0f);
            std::fill(st.prevPow.begin(), st.prevPow.end(), 0.0f);
            st.frames = 0;
        }
    }
    void processImpl(float* const* ch, int numCh, int n) override {
        const float gMin = dbToGain(-p(0));
        const float over = p(1);
        const float smoothAmt = p(2) / 100.0f;
        const bool fast = pb(3);
        const float hopSec = static_cast<float>(stft_[0].hop() / sr_);
        // Minimum may rise ~6 dB/s (fast: 20 dB/s) so the estimate follows
        // increases in noise level.
        const float rise = std::pow(10.0f, (fast ? 2.0f : 0.6f) * hopSec);
        for (int c = 0; c < numCh; ++c) {
            ChanState& st = state_[c];
            stft_[c].process(ch[c], n, [&](float* X) { frame(st, X, gMin, over, smoothAmt, rise); });
        }
        vis[0].store(noiseDb_, std::memory_order_relaxed);
        vis[1].store(attenDb_, std::memory_order_relaxed);
    }

private:
    struct ChanState {
        std::vector<float> noise, smooth, minimum, prevGain, prevPow, gain;
        int frames = 0;
    };

    void frame(ChanState& st, float* X, float gMin, float over, float smoothAmt, float rise) {
        const int nb = bins_;
        const bool init = st.frames < 12;
        double noiseSum = 0.0, gainSum = 0.0;
        for (int k = 0; k < nb; ++k) {
            const float re = X[2 * k], im = X[2 * k + 1];
            const float pw = re * re + im * im + 1e-20f;
            st.smooth[k] = st.frames == 0 ? pw : 0.7f * st.smooth[k] + 0.3f * pw;
            if (init) {
                // Bootstrap: assume the first ~60 ms is representative noise.
                st.minimum[k] = st.frames == 0 ? st.smooth[k] : std::min(st.minimum[k] * 1.05f, st.smooth[k]);
                st.noise[k] = st.frames == 0 ? pw : 0.8f * st.noise[k] + 0.2f * pw;
            } else {
                st.minimum[k] = std::min(st.minimum[k] * rise, st.smooth[k]);
                // Bias compensation for tracking the minimum of a smoothed
                // periodogram, then a slow recursive average.
                const float target = st.minimum[k] * 1.6f;
                st.noise[k] = 0.9f * st.noise[k] + 0.1f * target;
            }
            const float lambda = std::max(1e-20f, st.noise[k] * over);
            const float gamma = pw / lambda;
            const float xiDd = 0.98f * st.prevGain[k] * st.prevGain[k] * st.prevPow[k] / lambda +
                               0.02f * std::max(gamma - 1.0f, 0.0f);
            const float xi = std::max(xiDd, 1e-3f);
            st.gain[k] = std::max(gMin, xi / (1.0f + xi));
            st.prevPow[k] = pw;
            noiseSum += st.noise[k];
        }
        // Spectral smoothing (3-tap) and temporal release smoothing.
        if (smoothAmt > 0.0f) {
            float prev = st.gain[0];
            for (int k = 1; k < nb - 1; ++k) {
                const float cur = st.gain[k];
                const float sm = 0.25f * prev + 0.5f * cur + 0.25f * st.gain[k + 1];
                st.gain[k] = cur + (sm - cur) * smoothAmt;
                prev = cur;
            }
            const float rel = 0.5f * smoothAmt;
            for (int k = 0; k < nb; ++k)
                if (st.gain[k] < st.prevGain[k]) st.gain[k] = st.gain[k] + (st.prevGain[k] - st.gain[k]) * rel;
        }
        for (int k = 0; k < nb; ++k) {
            X[2 * k] *= st.gain[k];
            X[2 * k + 1] *= st.gain[k];
            st.prevGain[k] = st.gain[k];
            gainSum += st.gain[k];
        }
        ++st.frames;
        const float n = static_cast<float>(stft_[0].size());
        // Sum of the per-bin noise power over the positive spectrum, scaled by
        // the window energy (N/2) and the one-sided FFT gain (N/2): the
        // estimated noise power in dBFS.
        noiseDb_ = 10.0f * std::log10(static_cast<float>(noiseSum) / (n * n * 0.25f) + 1e-20f);
        attenDb_ = gainToDb(static_cast<float>(gainSum / nb));
    }

    Stft stft_[2];
    ChanState state_[2];
    int bins_ = 513;
    float noiseDb_ = -120.0f, attenDb_ = 0.0f;
};

} // namespace

void registerNoiseEffects(EffectRegistry& r) { registerEffect<NoiseSuppressorEffect>(r, kNsInfo); }

} // namespace echo
