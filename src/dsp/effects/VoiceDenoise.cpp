// Background noise suppression with RNNoise (Valin, "A Hybrid DSP/Deep
// Learning Approach to Real-Time Full-Band Speech Enhancement", 2018).
//
// RNNoise is a recurrent neural network trained on clean speech and many
// noise types (fans, keyboards, traffic, hiss...). It estimates per-band
// gains every 10 ms and also outputs a voice-activity probability.
//
// RNNoise alone lets some keystrokes and mouse clicks through: it takes them
// for the start of a word for a frame or a few (measured on real keyboard
// recordings: 1-4 frames in a row, while words are 11+). So the voice
// probability only counts as speech when it stays up for kConfirm frames.
// The output is held back kLook frames so that this is known before the
// start of a word comes out (nothing of the word is cut).
//
// Controls:
//   Strength  attenuation limit: 100% removes everything RNNoise classifies
//             as noise; lower values mix part of the original noise back in,
//             which sounds more natural in mild noise.
//   Mix       time-aligned wet/dry comparison.
//   Typing & click removal (on by default): turns everything down by
//             kClickDepthDb while nobody is speaking.
//   Voice gate  optional: turns everything down by its depth (default
//             -60 dB, near silence) while nobody is speaking.
//
// Each channel of each strip runs its own RNNoise state (independent
// microphones never interact). RNNoise works at 48 kHz in 480-sample frames;
// other engine rates are resampled transparently.
#include "core/Resampler.h"
#include "dsp/effects/EffectUtil.h"
#include "dsp/NoiseSuppressorInfo.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
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
// Frames the output is held back for the speech check. With RNNoise's own
// delay (its voice probability describes the newest input frame, its audio
// is 2 frames older), the check sees kLook + 2 frames ahead.
constexpr int kLook = 2;
constexpr int kAhead = kLook + 2;
constexpr int kConfirm = kAhead + 1;   // frames of voice in a row that count as speech
constexpr float kClickDepthDb = -40.0f;
// A keystroke peaks in its first frames and has died down 15 dB a few frames
// later; a word keeps its level (also one that starts with a "p" or "t").
constexpr float kClickDecayDb = 15.0f;
constexpr float kJumpDb = 10.0f;
// Steady sounds this far above the background pass even before the network
// hears a voice: the "s", "f" or "h" a word starts with has no pitch yet.
// The voice gate asks for more, so that breathing stays out.
constexpr float kSoundDb = 12.0f;
constexpr float kGateSoundDb = 15.0f;
constexpr float kSteadyDropDb = 6.0f;
constexpr float kFloorRiseDb = 0.05f; // background estimate: follows drops at once, rises 5 dB/s
constexpr int kClickHoldFrames = 20;   // keeps the ends of words
constexpr int kHist = 32;              // voice-probability history (frames)

const ParamInfo kVdParams[] = {
    {"strength", "Strength", 0.0f, 100.0f, 100.0f, "%", ParamScale::Linear, nullptr,
     "How much of the detected noise to remove. 100% = full suppression; lower keeps some room tone."},
    {"mix", "Mix", 0.0f, 100.0f, 100.0f, "%", ParamScale::Linear, nullptr,
     "Wet/dry: 0% = original microphone, 100% = processed (time-aligned)."},
    {"vad_gate", "Voice gate", 0.0f, 1.0f, 0.0f, "", ParamScale::Toggle, nullptr,
     "Silences everything while you're not speaking (breathing, room noise)."},
    {"vad_threshold", "Voice threshold", 5.0f, 95.0f, 60.0f, "%", ParamScale::Linear, nullptr,
     "Voice probability above which the gate opens."},
    {"vad_hold", "Gate hold", 0.0f, 2000.0f, 300.0f, "ms", ParamScale::Linear, nullptr,
     "How long the gate stays open after speech stops."},
    {"vad_range", "Gate depth", -80.0f, 0.0f, -60.0f, "dB", ParamScale::Linear, nullptr,
     "Attenuation while the gate is closed."},
    {"clicks", "Typing & click removal", 0.0f, 1.0f, 1.0f, "", ParamScale::Toggle, nullptr,
     "Removes keyboard typing and mouse clicks while you're not speaking."},
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
            dry48_[c].prepare(static_cast<size_t>(kNetDelay + kFrame * 2));
            look_[c].prepare(static_cast<size_t>(kFrame * (kLook + 2)));
            dry_[c].assign(maxBlock_, 0.0f);
        }
        tmp_.assign(static_cast<size_t>(maxBlock_ * 4 + kFrame * 4), 0.0f);
        // Gain curves of the frames on their way out (channel 0 works them
        // out, channel 1 uses the same ones): enough for one call's frames.
        curveFrames_ = static_cast<int>(std::ceil(maxBlock_ * 48000.0 / sr_ / kFrame)) + 4;
        curves_.assign(static_cast<size_t>(curveFrames_) * kFrame, 1.0f);
        // Fixed latency = RNNoise's own delay + the look-ahead + frame
        // buffering. When the engine block is a whole number of 480-sample
        // frames at 48 kHz, every call completes its frames, so no extra
        // buffering frame is needed.
        const int frameEng = static_cast<int>(std::ceil(kFrame * sr_ / 48000.0));
        if (resample_) {
            primeEng_ = frameEng + 2 * static_cast<int>(down_[0].latencyInputFrames()) + 96;
            // The resamplers start with half a kernel of silence already in
            // their history, so each one delays the signal by only half a
            // sample (measured); the rest of the kernel is covered by the prime.
            const double r = sr_ / 48000.0;
            latency_ = primeEng_ + static_cast<int>(std::lround((kNetDelay + kLook * kFrame + 0.5) * r + 0.5));
        } else {
            primeEng_ = (maxBlock_ % kFrame == 0) ? 0 : kFrame;
            latency_ = primeEng_ + kNetDelay + kLook * kFrame;
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
            dry48_[c].r = dry48_[c].w = 0;
            dry48_[c].pushZeros(kNetDelay); // lines the input up with RNNoise's output
            look_[c].r = look_[c].w = 0;
            look_[c].pushZeros(static_cast<size_t>(kLook * kFrame));
            if (resample_) {
                down_[c].reset();
                up_[c].reset();
            } else if (primeEng_ > 0) {
                out48_[c].pushZeros(static_cast<size_t>(primeEng_)); // frame-adapter prime
            }
            if (resample_) outEng_[c].pushZeros(static_cast<size_t>(primeEng_));
            align_[c].reset();
            frames_[c] = 0;
        }
        for (float& v : vadHist_) v = 0.0f;
        for (float& v : levelHist_) v = -120.0f;
        floorDb_ = 20.0f;
        gateGain_ = 1.0f;
        hold_ = 0;
        vad_ = 0.0f;
    }

    void processImpl(float* const* ch, int numCh, int n) override {
        const float strength = p(0) / 100.0f;
        const float mix = p(1) / 100.0f;
        // Residual noise kept by the attenuation limit (strength 100% -> -60 dB floor -> effectively full).
        const float residual = strength >= 0.999f ? 0.0f : dbToGain(-60.0f * strength);
        double inPow = 0.0, outPow = 0.0;
        float frameIn[kFrame], frameOut[kFrame], dry48[kFrame];
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
            // 2. RNNoise frames, the strength (attenuation limit), then the
            //    look-ahead and the speech gain.
            while (in48_[c].size() >= static_cast<size_t>(kFrame)) {
                in48_[c].pop(frameIn, kFrame);
                dry48_[c].push(frameIn, kFrame);
                dry48_[c].pop(dry48, kFrame);
                for (int i = 0; i < kFrame; ++i) frameIn[i] *= kScale;
                const float prob = rnnoise_process_frame(st_[c], frameOut, frameIn);
                const int64_t k = frames_[c]++;
                if (c == 0) {
                    vad_ = prob;
                    vadHist_[k % kHist] = prob;
                    double e = 0.0;
                    for (int i = 0; i < kFrame; ++i) e += static_cast<double>(frameIn[i]) * frameIn[i];
                    const float level = static_cast<float>(10.0 * std::log10(e / kFrame / (kScale * kScale) + 1e-12));
                    levelHist_[k % kHist] = level;
                    floorDb_ = level < floorDb_ ? level : floorDb_ + kFloorRiseDb;
                }
                for (int i = 0; i < kFrame; ++i) {
                    const float x = frameIn[i] / kScale;
                    float y = frameOut[i] / kScale;
                    if (residual > 0.0f) y += residual * (dry48[i] - y);
                    inPow += static_cast<double>(x) * x;
                    outPow += static_cast<double>(y) * y;
                    frameOut[i] = y;
                }
                look_[c].push(frameOut, kFrame);
                look_[c].pop(frameOut, kFrame); // the frame kLook frames back
                float* curve = &curves_[static_cast<size_t>(k % curveFrames_) * kFrame];
                if (c == 0) speechGain(k, curve);
                for (int i = 0; i < kFrame; ++i) frameOut[i] *= curve[i];
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
            // 4. wet/dry
            const float* dry = dry_[c].data();
            if (mix < 0.999f)
                for (int i = 0; i < n; ++i) ch[c][i] = dry[i] + (ch[c][i] - dry[i]) * mix;
        }
        if (inPow > 0.0) vis[1].store(static_cast<float>(10.0 * std::log10((outPow + 1e-12) / (inPow + 1e-12))),
                                      std::memory_order_relaxed);
        vis[0].store(vad_, std::memory_order_relaxed);
        vis[2].store(gainToDb(gateGain_), std::memory_order_relaxed);
    }

private:
    // The gain for the frame now leaving the look-ahead, sample by sample.
    // Frame k just went through RNNoise; its voice probability describes
    // input frame k, and the frame leaving holds input frame k - kAhead.
    void speechGain(int64_t k, float* curve) {
        const bool gate = pb(2), clicks = pb(6);
        const float thr = p(3) / 100.0f;
        float depthDb = 0.0f;
        if (clicks) depthDb = kClickDepthDb * p(0) / 100.0f; // follows Strength
        if (gate) depthDb = std::min(depthDb, p(5));
        const int64_t a = k - kAhead;
        auto voiced = [&](int64_t i) { return i >= 0 && i <= k && i > k - kHist && vadHist_[i % kHist] >= thr; };
        bool open = depthDb >= -0.01f, fast = false;
        if (!open && a >= 0) {
            if (voiced(a)) {
                // Speech: part of kConfirm or more frames of voice in a row.
                int64_t s = a, e = a;
                while (s > a - kConfirm && voiced(s - 1)) --s;
                while (e < k && voiced(e + 1)) ++e;
                open = e - s + 1 >= kConfirm && !keystroke(s, k);
                if (open) hold_ = gate ? static_cast<int>(p(4) / 10.0f) : kClickHoldFrames;
            } else {
                // Just before speech starts: open a frame early (the start of
                // a word is often quieter than the network notices).
                bool next = true;
                for (int64_t i = a + 1; i <= k; ++i) next = next && voiced(i);
                open = next && !keystroke(a + 1, k);
            }
            if (!open) open = steadySound(a, k, gate ? kGateSoundDb : kSoundDb);
            if (!open && hold_ > 0) {
                // The hold keeps the quiet end of a word. A sudden jump in
                // level right after it is something new (a keystroke; a new
                // word was let through above), so it ends the hold at once.
                if (jumpAt(a) || jumpAt(a + 1)) {
                    hold_ = 0;
                    fast = true;
                } else {
                    --hold_;
                    open = true;
                }
            }
        }
        const float target = open ? 1.0f : dbToGain(depthDb);
        const float coeff = target > gateGain_ ? attack48_ : fast ? fastRelease48_ : release48_;
        float g = gateGain_;
        for (int i = 0; i < kFrame; ++i) {
            g = target + coeff * (g - target);
            curve[i] = g;
        }
        gateGain_ = g;
    }

    // A run of voice frames from s that is really a keystroke: loudest in
    // its first frames, then quickly gone.
    bool keystroke(int64_t s, int64_t k) const {
        const int64_t last = std::min(s + kAhead, k); // the frames known so far
        if (s < 1 || last < s + 2 || k - (s - 1) >= kHist) return false;
        auto level = [&](int64_t i) { return levelHist_[i % kHist]; };
        const float peak = std::max({level(s - 1), level(s), level(s + 1)});
        float after = -120.0f;
        for (int64_t i = s + 2; i <= last; ++i) after = std::max(after, level(i));
        return after < peak - kClickDecayDb;
    }

    // Frame a holds a sound clearly above the background that holds or
    // grows over the next frames: the start of a word. A keystroke, its
    // ring and its release click all die away.
    bool steadySound(int64_t a, int64_t k, float marginDb) const {
        auto level = [&](int64_t i) { return levelHist_[i % kHist]; };
        if (level(a) < floorDb_ + marginDb || level(a) < -70.0f) return false;
        for (int64_t i = a + 1; i <= std::min(a + kAhead, k); ++i)
            if (level(i) < level(a) - kSteadyDropDb) return false;
        return true;
    }

    // Frame i is 10 dB or more louder than the two before it.
    bool jumpAt(int64_t i) const {
        const int64_t k = frames_[0] - 1;
        if (i < 2 || i > k || k - (i - 2) >= kHist) return false;
        auto level = [&](int64_t j) { return levelHist_[j % kHist]; };
        return level(i) > -70.0f && level(i) >= std::max(level(i - 1), level(i - 2)) + kJumpDb;
    }

    DenoiseState* st_[2] = {nullptr, nullptr};
    bool resample_ = false;
    Resampler down_[2], up_[2];
    Fifo in48_[2], out48_[2], outEng_[2], dry48_[2], look_[2];
    AlignDelay align_[2];
    std::vector<float> dry_[2], tmp_, curves_;
    int curveFrames_ = 8;
    int64_t frames_[2] = {0, 0};
    float vadHist_[kHist] = {};
    float levelHist_[kHist] = {}; // input level per frame (dBFS)
    float floorDb_ = 20.0f;       // background level (the first frame sets it)
    int latency_ = kFrame, primeEng_ = kFrame;
    float vad_ = 0.0f, gateGain_ = 1.0f;
    const float attack48_ = timeToCoeff(48000.0, 0.001);  // opening: 1 ms
    const float release48_ = timeToCoeff(48000.0, 0.040); // closing: 40 ms
    const float fastRelease48_ = timeToCoeff(48000.0, 0.003); // before a keystroke
    int hold_ = 0;
};

} // namespace

const char* noiseSuppressorTypeId() { return kVdInfo.typeId; }

void registerVoiceDenoiseEffects(EffectRegistry& r) { registerEffect<VoiceDenoiseEffect>(r, kVdInfo); }

} // namespace echo
