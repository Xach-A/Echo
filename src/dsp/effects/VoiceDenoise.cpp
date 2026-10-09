// Background noise suppression with RNNoise (Valin, "A Hybrid DSP/Deep
// Learning Approach to Real-Time Full-Band Speech Enhancement", 2018).
//
// RNNoise is a recurrent neural network trained on clean speech and many
// noise types (fans, keyboards, traffic, hiss...). It estimates per-band
// gains every 10 ms and also outputs a voice-activity probability, which
// drives the optional voice-activity gate here.
//
// Controls:
//   Strength  attenuation limit: 100% removes everything RNNoise classifies
//             as noise; lower values mix part of the original noise back in,
//             which sounds more natural in mild noise.
//   Mix       time-aligned wet/dry comparison.
//   VAD gate  optional extra attenuation when no voice is detected.
//
// Each channel of each strip runs its own RNNoise state (independent
// microphones never interact). RNNoise works at 48 kHz in 480-sample frames;
// other engine rates are resampled transparently.
#include "core/Resampler.h"
#include "dsp/effects/EffectUtil.h"
#include "dsp/NoiseSuppressorInfo.h"

#include <cstring>
#include <vector>

extern "C" {
#include "rnnoise.h"
}

namespace echo {

namespace {

constexpr int kFrame = 480;        // RNNoise frame at 48 kHz
constexpr int kNetDelay = 960;     // RNNoise's own algorithmic delay (measured: 2 frames)
constexpr float kScale = 32768.0f; // RNNoise expects 16-bit sample scale

const ParamInfo kVdParams[] = {
    {"strength", "Strength", 0.0f, 100.0f, 100.0f, "%", ParamScale::Linear, nullptr,
     "How much of the detected noise to remove. 100% = full suppression; lower keeps some room tone."},
    {"mix", "Mix", 0.0f, 100.0f, 100.0f, "%", ParamScale::Linear, nullptr,
     "Wet/dry: 0% = original microphone, 100% = processed (time-aligned)."},
    {"vad_gate", "Voice gate", 0.0f, 1.0f, 0.0f, "", ParamScale::Toggle, nullptr,
     "Also attenuate everything while no voice is detected (voice activity detection)."},
    {"vad_threshold", "Voice threshold", 5.0f, 95.0f, 60.0f, "%", ParamScale::Linear, nullptr,
     "Voice probability above which the gate opens."},
    {"vad_hold", "Gate hold", 0.0f, 2000.0f, 300.0f, "ms", ParamScale::Linear, nullptr,
     "How long the gate stays open after speech stops."},
    {"vad_range", "Gate depth", -80.0f, 0.0f, -30.0f, "dB", ParamScale::Linear, nullptr,
     "Attenuation while the gate is closed."},
};
ECHO_EFFECT_INFO(kVdInfo, "voice_denoise", "Noise Suppression (RNNoise)", "Restoration",
                 "Neural background-noise removal for voice: fans, keyboard and mouse clicks, air "
                 "conditioning, hiss and room noise. Runs locally on the CPU.",
                 kVdParams, false);

// Simple single-threaded FIFO for the frame adapter.
struct Fifo {
    std::vector<float> buf;
    size_t r = 0, w = 0; // monotonically increasing indices
    void prepare(size_t cap) {
        buf.assign(cap, 0.0f);
        r = w = 0;
    }
    size_t size() const { return w - r; }
    void push(const float* x, size_t n) {
        for (size_t i = 0; i < n; ++i) buf[(w + i) % buf.size()] = x[i];
        w += n;
    }
    void pushZeros(size_t n) {
        for (size_t i = 0; i < n; ++i) buf[(w + i) % buf.size()] = 0.0f;
        w += n;
    }
    void pop(float* x, size_t n) {
        for (size_t i = 0; i < n; ++i) x[i] = buf[(r + i) % buf.size()];
        r += n;
    }
};

class VoiceDenoiseEffect final : public Effect {
public:
    VoiceDenoiseEffect() : Effect(kVdInfo) {}
    ~VoiceDenoiseEffect() override {
        for (auto*& s : st_)
            if (s) rnnoise_destroy(s);
    }
    int latencySamples() const override { return latency_; }
    const char* visLabel(int s) const override {
        switch (s) {
        case 0: return "Voice probability";
        case 1: return "Noise reduction (dB)";
        case 2: return "Gate gain (dB)";
        default: return nullptr;
        }
    }

protected:
    void onPrepare() override {
        for (auto*& s : st_) {
            if (s) rnnoise_destroy(s);
            s = rnnoise_create(nullptr);
        }
        resample_ = std::abs(sr_ - 48000.0) > 0.5;
        const double ratioIn = sr_ / 48000.0; // engine -> 48 kHz (in/out)
        for (int c = 0; c < 2; ++c) {
            if (resample_) {
                down_[c].prepare(1, ratioIn, maxBlock_ + 64, ResamplerQuality::High);
                up_[c].prepare(1, 1.0 / ratioIn, static_cast<int>(maxBlock_ / ratioIn) + kFrame * 2 + 64,
                               ResamplerQuality::High);
            }
            in48_[c].prepare(static_cast<size_t>(kFrame * 8 + maxBlock_ * 4));
            out48_[c].prepare(static_cast<size_t>(kFrame * 8 + maxBlock_ * 4));
            outEng_[c].prepare(static_cast<size_t>(kFrame * 8 + maxBlock_ * 4));
            dry_[c].assign(maxBlock_, 0.0f);
        }
        tmp_.assign(static_cast<size_t>(maxBlock_ * 4 + kFrame * 4), 0.0f);
        // Fixed latency = RNNoise's own delay + frame buffering. When the
        // engine block is a whole number of 480-sample frames at 48 kHz, every
        // call completes its frames, so no extra buffering frame is needed.
        const int frameEng = static_cast<int>(std::ceil(kFrame * sr_ / 48000.0));
        if (resample_) {
            primeEng_ = frameEng + 2 * static_cast<int>(down_[0].latencyInputFrames()) + 96;
            const double r = sr_ / 48000.0;
            latency_ = primeEng_ + static_cast<int>(std::lround((kNetDelay + up_[0].latencyInputFrames()) * r +
                                                                down_[0].latencyInputFrames()));
        } else {
            primeEng_ = (maxBlock_ % kFrame == 0) ? 0 : kFrame;
            latency_ = primeEng_ + kNetDelay;
        }
        for (auto& a : align_) a.prepare(latency_, maxBlock_);
    }

    void onReset() override {
        // Re-initialise in place: may run on the audio thread (re-enable,
        // fault recovery), so no allocation here.
        for (auto* s : st_)
            if (s) rnnoise_init(s, nullptr);
        for (int c = 0; c < 2; ++c) {
            in48_[c].r = in48_[c].w = 0;
            out48_[c].r = out48_[c].w = 0;
            outEng_[c].r = outEng_[c].w = 0;
            if (resample_) {
                down_[c].reset();
                up_[c].reset();
            } else if (primeEng_ > 0) {
                out48_[c].pushZeros(static_cast<size_t>(primeEng_)); // frame-adapter prime
            }
            if (resample_) outEng_[c].pushZeros(static_cast<size_t>(primeEng_));
            align_[c].reset();
        }
        gateGain_ = 1.0f;
        holdFrames_ = 0;
        vad_ = 0.0f;
    }

    void processImpl(float* const* ch, int numCh, int n) override {
        const float strength = p(0) / 100.0f;
        const float mix = p(1) / 100.0f;
        const bool gate = pb(2);
        const float thr = p(3) / 100.0f;
        const int holdFrames = static_cast<int>(p(4) / 10.0f);
        // Residual noise kept by the attenuation limit (strength 100% -> -60 dB floor -> effectively full).
        const float residual = strength >= 0.999f ? 0.0f : dbToGain(-60.0f * strength);
        double inPow = 0.0, outPow = 0.0;
        float frameIn[kFrame], frameOut[kFrame];
        for (int c = 0; c < numCh; ++c) {
            align_[c].process(ch[c], dry_[c].data(), n);
            // 1. engine rate -> 48 kHz frame FIFO
            if (resample_) {
                down_[c].push(ch[c], n);
                const double ratio = down_[c].nominalRatio();
                const int got = down_[c].pull(tmp_.data(), down_[c].outputAvailable(ratio), ratio);
                in48_[c].push(tmp_.data(), static_cast<size_t>(got));
            } else {
                in48_[c].push(ch[c], static_cast<size_t>(n));
            }
            // 2. RNNoise frames
            while (in48_[c].size() >= static_cast<size_t>(kFrame)) {
                in48_[c].pop(frameIn, kFrame);
                for (int i = 0; i < kFrame; ++i) frameIn[i] *= kScale;
                const float prob = rnnoise_process_frame(st_[c], frameOut, frameIn);
                if (c == 0) updateGate(prob, gate, thr, holdFrames);
                for (int i = 0; i < kFrame; ++i) {
                    const float x = frameIn[i] / kScale, y = frameOut[i] / kScale;
                    inPow += static_cast<double>(x) * x;
                    outPow += static_cast<double>(y) * y;
                    frameOut[i] = y;
                }
                out48_[c].push(frameOut, kFrame);
            }
            // 3. 48 kHz -> engine rate
            if (resample_) {
                const size_t avail = out48_[c].size();
                out48_[c].pop(tmp_.data(), avail);
                up_[c].push(tmp_.data(), static_cast<int>(avail));
                const double ratio = up_[c].nominalRatio();
                const int got = up_[c].pull(tmp_.data(), up_[c].outputAvailable(ratio), ratio);
                outEng_[c].push(tmp_.data(), static_cast<size_t>(got));
                Fifo& src = outEng_[c];
                const size_t take = std::min(src.size(), static_cast<size_t>(n));
                src.pop(ch[c], take);
                if (take < static_cast<size_t>(n)) std::memset(ch[c] + take, 0, sizeof(float) * (n - take));
            } else {
                const size_t take = std::min(out48_[c].size(), static_cast<size_t>(n));
                out48_[c].pop(ch[c], take);
                if (take < static_cast<size_t>(n)) std::memset(ch[c] + take, 0, sizeof(float) * (n - take));
            }
            // 4. strength (attenuation limit), voice gate, wet/dry
            const float* dry = dry_[c].data();
            float g = gateGain_;
            const float target = gateTarget_;
            const float coeff = target > g ? gateAttack_ : gateRelease_;
            for (int i = 0; i < n; ++i) {
                float y = ch[c][i];
                if (residual > 0.0f) y += residual * (dry[i] - y);
                g = target + coeff * (g - target);
                y *= g;
                ch[c][i] = dry[i] + (y - dry[i]) * mix;
            }
            if (c == numCh - 1) gateGain_ = g;
        }
        if (inPow > 0.0) vis[1].store(static_cast<float>(10.0 * std::log10((outPow + 1e-12) / (inPow + 1e-12))),
                                      std::memory_order_relaxed);
        vis[0].store(vad_, std::memory_order_relaxed);
        vis[2].store(gainToDb(gateGain_), std::memory_order_relaxed);
    }

private:
    void updateGate(float prob, bool gate, float thr, int holdFrames) {
        vad_ = prob;
        if (!gate) {
            gateTarget_ = 1.0f;
            return;
        }
        if (prob >= thr) holdFrames_ = holdFrames;
        else if (holdFrames_ > 0) --holdFrames_;
        gateTarget_ = (prob >= thr || holdFrames_ > 0) ? 1.0f : dbToGain(p(5));
        gateAttack_ = timeToCoeff(sr_, 0.005);
        gateRelease_ = timeToCoeff(sr_, 0.080);
    }

    DenoiseState* st_[2] = {nullptr, nullptr};
    bool resample_ = false;
    Resampler down_[2], up_[2];
    Fifo in48_[2], out48_[2], outEng_[2];
    AlignDelay align_[2];
    std::vector<float> dry_[2], tmp_;
    int latency_ = kFrame, primeEng_ = kFrame;
    float vad_ = 0.0f, gateGain_ = 1.0f, gateTarget_ = 1.0f;
    float gateAttack_ = 0.99f, gateRelease_ = 0.999f;
    int holdFrames_ = 0;
};

} // namespace

const char* noiseSuppressorTypeId() { return kVdInfo.typeId; }

void registerVoiceDenoiseEffects(EffectRegistry& r) { registerEffect<VoiceDenoiseEffect>(r, kVdInfo); }

} // namespace echo
