// Spectral voice effects: robotisation (zero-phase resynthesis at a fixed
// frame rate gives a monotone, buzzy robot voice) and whisperisation
// (random phases remove the pitch, leaving breath).
#include "dsp/Fft.h"
#include "dsp/effects/EffectUtil.h"

#include <cstring>
#include <vector>

namespace echo {

namespace {

const char* const kRobotModes[] = {"Robot", "Whisper", nullptr};
const ParamInfo kRobotParams[] = {
    {"mode", "Mode", 0.0f, 1.0f, 0.0f, "", ParamScale::Enum, kRobotModes},
    {"pitch", "Robot pitch", 40.0f, 400.0f, 110.0f, "Hz", ParamScale::Log, nullptr, "Monotone pitch of the robot voice"},
    {"output", "Output", -24.0f, 12.0f, 0.0f, "dB", ParamScale::Linear},
    {"mix", "Mix", 0.0f, 100.0f, 100.0f, "%", ParamScale::Linear},
};
ECHO_EFFECT_INFO(kRobotInfo, "robot", "Robot / Whisper", "Voice",
                 "Spectral robot voice (monotone, adjustable pitch) or whisper.", kRobotParams, false);

class RobotEffect final : public Effect {
public:
    RobotEffect() : Effect(kRobotInfo) {}
    int latencySamples() const override { return n_; }

protected:
    void onPrepare() override {
        n_ = sr_ > 50000 ? 1024 : 512;
        fft_.prepare(n_);
        win_.resize(n_);
        makeHann(win_.data(), n_, true);
        for (auto& c : chans_) {
            c.hist.assign(n_, 0.0f);
            c.accum.assign(2 * n_, 0.0f);
        }
        frame_.assign(n_, 0.0f);
        spec_.assign(n_ + 2, 0.0f);
        dry_.assign(static_cast<size_t>(maxBlock_) * 2, 0.0f);
        for (auto& a : align_) a.prepare(n_, maxBlock_);
    }
    void onReset() override {
        for (auto& c : chans_) {
            std::fill(c.hist.begin(), c.hist.end(), 0.0f);
            std::fill(c.accum.begin(), c.accum.end(), 0.0f);
            c.histPos = 0;
        }
        for (auto& a : align_) a.reset();
        t_ = 0;
        next_ = 0;
    }
    void processImpl(float* const* ch, int numCh, int n) override {
        const bool whisper = pi(0) == 1;
        const int hop = whisper ? n_ / 4 : std::max(16, static_cast<int>(sr_ / std::max(20.0f, p(1))));
        const float norm = static_cast<float>(hop) / (0.375f * static_cast<float>(n_));
        const float og = dbToGain(p(2));
        const float mix = p(3) / 100.0f;
        const int size = 2 * n_;
        if (mix < 0.999f)
            for (int c = 0; c < numCh; ++c) align_[c].process(ch[c], dry_.data() + static_cast<size_t>(c) * maxBlock_, n);
        for (int i = 0; i < n; ++i) {
            for (int c = 0; c < numCh; ++c) {
                Chan& st = chans_[c];
                st.hist[st.histPos] = ch[c][i];
                st.histPos = (st.histPos + 1) % n_;
            }
            if (t_ >= next_) {
                for (int c = 0; c < numCh; ++c) synthFrame(chans_[c], whisper);
                next_ = t_ + static_cast<uint64_t>(hop);
            }
            const int pos = static_cast<int>(t_ % static_cast<uint64_t>(size));
            for (int c = 0; c < numCh; ++c) {
                Chan& st = chans_[c];
                ch[c][i] = st.accum[pos] * norm * og;
                st.accum[pos] = 0.0f;
            }
            ++t_;
        }
        if (mix < 0.999f)
            for (int c = 0; c < numCh; ++c) mixLinear(ch[c], dry_.data() + static_cast<size_t>(c) * maxBlock_, n, mix);
    }

private:
    struct Chan {
        std::vector<float> hist, accum;
        int histPos = 0;
    };
    void synthFrame(Chan& st, bool whisper) {
        for (int k = 0; k < n_; ++k) frame_[k] = st.hist[(st.histPos + k) % n_] * win_[k];
        fft_.forward(frame_.data(), spec_.data());
        const int nb = n_ / 2 + 1;
        for (int k = 0; k < nb; ++k) {
            const float mag = std::sqrt(spec_[2 * k] * spec_[2 * k] + spec_[2 * k + 1] * spec_[2 * k + 1]);
            if (whisper) {
                rng_ = rng_ * 1664525u + 1013904223u;
                const float ph = static_cast<float>(rng_ >> 8) / 16777216.0f * kTwoPi;
                spec_[2 * k] = mag * std::cos(ph);
                spec_[2 * k + 1] = mag * std::sin(ph);
            } else {
                spec_[2 * k] = mag;
                spec_[2 * k + 1] = 0.0f;
            }
        }
        fft_.inverse(spec_.data(), frame_.data());
        const int size = 2 * n_;
        const int base = static_cast<int>(t_ % static_cast<uint64_t>(size));
        const int half = n_ / 2;
        for (int k = 0; k < n_; ++k) {
            // Zero-phase result is centred on index 0: rotate by n/2.
            const float v = frame_[(k + half) % n_] * win_[k];
            st.accum[(base + k) % size] += v;
        }
    }

    int n_ = 512;
    RealFft fft_;
    std::vector<float> win_, frame_, spec_, dry_;
    Chan chans_[2];
    AlignDelay align_[2];
    uint64_t t_ = 0, next_ = 0;
    uint32_t rng_ = 13579u;
};

} // namespace

void registerSpectralEffects(EffectRegistry& r) { registerEffect<RobotEffect>(r, kRobotInfo); }

} // namespace echo
