// Real-time pitch correction ("autotune").
//
// Analysis: YIN pitch detection every ~5 ms on a mono sum.
// Decision: nearest note in the chosen key/scale, with correction strength,
//   retune speed (time constant), transition smoothing, and vibrato.
// Synthesis: pitch-synchronous overlap-add (TD-PSOLA). Analysis marks are
//   placed one period apart and aligned to waveform peaks; grains two
//   periods long are re-spaced at period/ratio. Grains are not resampled,
//   so formants are preserved; with "Preserve formants" off, grains are
//   resampled by the ratio (formants follow pitch).
// Unvoiced audio passes through with a fixed grain spacing (identity).
#include "dsp/MusicTheory.h"
#include "dsp/PitchDetector.h"
#include "dsp/PitchDisplay.h"
#include "dsp/effects/EffectUtil.h"

#include <cstring>
#include <vector>

namespace echo {

namespace {

const char* const kStyles[] = {"Natural", "Hard (robotic)", "Custom", nullptr};
const char* const kRanges[] = {"Bass (55-350 Hz)", "Baritone/Tenor (75-600 Hz)", "Alto/Soprano (130-1100 Hz)",
                               "Wide (55-1100 Hz)", nullptr};
constexpr double kRangeLo[4] = {55.0, 75.0, 130.0, 55.0};
constexpr double kRangeHi[4] = {350.0, 600.0, 1100.0, 1100.0};

const ParamInfo kAtParams[] = {
    {"key", "Key", 0.0f, 11.0f, 0.0f, "", ParamScale::Enum, music::kNoteNames},
    {"scale", "Scale", 0.0f, 9.0f, 0.0f, "", ParamScale::Enum, music::kScaleNames},
    {"style", "Style", 0.0f, 2.0f, 0.0f, "", ParamScale::Enum, kStyles,
     "Natural = gentle; Hard = instant robotic snap; Custom = use the controls below"},
    {"strength", "Strength", 0.0f, 100.0f, 100.0f, "%", ParamScale::Linear, nullptr, "How far toward the note to pull"},
    {"speed", "Retune speed", 0.0f, 500.0f, 50.0f, "ms", ParamScale::Linear, nullptr, "0 ms = instant (hard tune)"},
    {"smoothing", "Transition smoothing", 0.0f, 200.0f, 20.0f, "ms", ParamScale::Linear, nullptr,
     "Smooths the detected pitch before choosing notes"},
    {"reference", "A4 reference", 415.0f, 466.0f, 440.0f, "Hz", ParamScale::Linear},
    {"preserve", "Preserve formants", 0.0f, 1.0f, 1.0f, "", ParamScale::Toggle},
    {"vib_rate", "Vibrato rate", 0.0f, 10.0f, 5.5f, "Hz", ParamScale::Linear},
    {"vib_depth", "Vibrato depth", 0.0f, 100.0f, 0.0f, "cents", ParamScale::Linear},
    {"range", "Voice range", 0.0f, 3.0f, 1.0f, "", ParamScale::Enum, kRanges,
     "Narrower ranges track better and add less latency"},
    {"mix", "Mix", 0.0f, 100.0f, 100.0f, "%", ParamScale::Linear},
};
ECHO_EFFECT_INFO(kAtInfo, "autotune", "Pitch Correction (Autotune)", "Voice",
                 "Detects the sung pitch and corrects it toward the nearest note in a key and scale.", kAtParams,
                 false);

class AutotuneEffect final : public Effect, public PitchDisplaySource {
public:
    AutotuneEffect() : Effect(kAtInfo) { history_.allocate(1024); }
    int latencySamples() const override { return latency_; }
    const char* visLabel(int s) const override {
        switch (s) {
        case 0: return "Detected (Hz)";
        case 1: return "Target (Hz)";
        case 2: return "Correction (cents)";
        case 3: return "Clarity";
        default: return nullptr;
        }
    }
    SpscRing<PitchFrame>& pitchHistory() override { return history_; }
    double framesPerSecond() const override { return sr_ / hop_; }

protected:
    void onPrepare() override {
        hop_ = std::max(64, static_cast<int>(std::lround(0.0053 * sr_)));
        for (int r = 0; r < 4; ++r) det_[r].prepare(sr_, kRangeLo[r], kRangeHi[r]);
        const int tmaxWide = static_cast<int>(sr_ / kRangeLo[3]) + 2;
        int size = 1;
        while (size < 12 * tmaxWide + 4 * hop_ + maxBlock_) size <<= 1;
        mask_ = size - 1;
        for (int c = 0; c < 2; ++c) {
            in_[c].assign(size, 0.0f);
            out_[c].assign(size, 0.0f);
        }
        mono_.assign(size, 0.0f);
        marks_.assign(kMarkCap, Mark{});
        track_.assign(kTrackCap, Track{});
        const int maxLatency = hop_ + 3 * tmaxWide;
        for (auto& a : align_) a.prepare(maxLatency, maxBlock_);
        dry_.assign(static_cast<size_t>(maxBlock_) * 2, 0.0f);
        hann_.resize(4097);
        for (int i = 0; i <= 4096; ++i) hann_[i] = 0.5f - 0.5f * std::cos(kTwoPi * i / 4096.0f);
    }

    void onReset() override {
        range_ = clampv(pi(10), 0, 3);
        for (auto& d : det_) d.reset();
        tmax_ = static_cast<int>(sr_ / kRangeLo[range_]) + 2;
        latency_ = hop_ + 3 * tmax_;
        for (int c = 0; c < 2; ++c) {
            std::fill(in_[c].begin(), in_[c].end(), 0.0f);
            std::fill(out_[c].begin(), out_[c].end(), 0.0f);
        }
        std::fill(mono_.begin(), mono_.end(), 0.0f);
        for (auto& a : align_) { // sized for the widest range in onPrepare; no reallocation here
            a.setDelay(latency_, maxBlock_);
            a.reset();
        }
        w_ = 0;
        markHead_ = markCount_ = 0;
        trackHead_ = trackCount_ = 0;
        anaNext_ = tmax_;
        synNext_ = static_cast<double>(tmax_);
        smoothMidi_ = -1.0;
        correction_ = 0.0;
        lastNote_ = -1000;
        vibPhase_ = 0.0;
        voicedTime_ = 0.0;
        sinceHop_ = 0;
    }

    void processImpl(float* const* ch, int numCh, int n) override {
        if (clampv(pi(10), 0, 3) != range_) onReset(); // range change alters latency
        const float mix = p(11) / 100.0f;
        for (int c = 0; c < numCh; ++c)
            align_[c].process(ch[c], dry_.data() + static_cast<size_t>(c) * maxBlock_, n);
        numCh_ = numCh;
        PitchDetector& det = det_[range_];
        for (int i = 0; i < n; ++i) {
            const int64_t wi = w_ & mask_;
            float m = 0.0f;
            for (int c = 0; c < numCh; ++c) {
                in_[c][wi] = ch[c][i];
                m += ch[c][i];
            }
            m /= static_cast<float>(numCh);
            mono_[wi] = m;
            det.push(m);
            ++w_;
            if (++sinceHop_ >= hop_) {
                sinceHop_ = 0;
                analyzeHop(det);
                generateAnalysisMarks();
                generateSynthesis();
            }
            const int64_t o = (w_ - 1 - latency_) & mask_;
            for (int c = 0; c < numCh; ++c) {
                ch[c][i] = out_[c][o];
                out_[c][o] = 0.0f;
            }
        }
        if (mix < 0.999f)
            for (int c = 0; c < numCh; ++c) mixLinear(ch[c], dry_.data() + static_cast<size_t>(c) * maxBlock_, n, mix);
    }

private:
    struct Mark {
        int64_t pos = 0;
        float period = 0.0f;
        bool voiced = false;
    };
    struct Track {
        int64_t center = 0;
        float period = 0.0f; // 0 = unvoiced
        float ratio = 1.0f;
    };
    static constexpr int kMarkCap = 2048;
    static constexpr int kTrackCap = 512;

    void analyzeHop(PitchDetector& det) {
        const PitchDetector::Result r = det.analyze();
        const double hopSec = hop_ / sr_;
        int style = pi(2);
        float strength = p(3) / 100.0f, speedMs = p(4), smoothMs = p(5);
        if (style == 0) {
            strength = 0.85f;
            speedMs = 90.0f;
            smoothMs = 35.0f;
        } else if (style == 1) {
            strength = 1.0f;
            speedMs = 0.0f;
            smoothMs = 0.0f;
        }
        const double ref = p(6);
        const int key = clampv(pi(0), 0, 11), scale = clampv(pi(1), 0, music::NumScales - 1);
        float ratio = 1.0f, targetHz = 0.0f, outHz = 0.0f;
        if (r.voiced) {
            voicedTime_ += hopSec;
            const double midi = music::hzToMidi(r.hz, ref);
            const double aS = smoothMs <= 0.0f ? 0.0 : std::exp(-hopSec / (smoothMs * 0.001));
            smoothMidi_ = smoothMidi_ < 0.0 ? midi : midi + aS * (smoothMidi_ - midi);
            int note = music::nearestNote(smoothMidi_, key, scale);
            // Hysteresis: hold the previous note unless clearly closer to another.
            if (lastNote_ > -1000 && note != lastNote_ && music::noteAllowed(lastNote_, key, scale) &&
                std::abs(smoothMidi_ - lastNote_) < 0.65)
                note = lastNote_;
            lastNote_ = note;
            const double desired = (note - smoothMidi_) * strength;
            const double aR = speedMs <= 0.0f ? 0.0 : std::exp(-hopSec / (speedMs * 0.001));
            correction_ = desired + aR * (correction_ - desired);
            double semis = correction_ + (smoothMidi_ - midi) * strength * (speedMs <= 0.0f ? 1.0 : 0.0);
            // Vibrato fades in after 150 ms of continuous voicing.
            const double vibDepth = p(9) / 100.0 * std::min(1.0, std::max(0.0, (voicedTime_ - 0.15) / 0.3));
            vibPhase_ += p(8) * hopSec;
            vibPhase_ -= std::floor(vibPhase_);
            semis += vibDepth * std::sin(2.0 * kPiD * vibPhase_);
            semis = clampv(semis, -3.0, 3.0);
            ratio = static_cast<float>(std::pow(2.0, semis / 12.0));
            targetHz = static_cast<float>(music::midiToHz(note, ref));
            outHz = r.hz * ratio;
        } else {
            voicedTime_ = 0.0;
            smoothMidi_ = -1.0;
            correction_ *= 0.5;
            lastNote_ = -1000;
        }
        Track& t = track_[(trackHead_ + trackCount_) % kTrackCap];
        if (trackCount_ == kTrackCap) trackHead_ = (trackHead_ + 1) % kTrackCap;
        else ++trackCount_;
        t.center = w_ - det.windowSamples() / 2;
        t.period = r.voiced ? r.periodSamples : 0.0f;
        t.ratio = ratio;

        vis[0].store(r.hz, std::memory_order_relaxed);
        vis[1].store(targetHz, std::memory_order_relaxed);
        vis[2].store(r.voiced ? static_cast<float>(1200.0 * std::log2(ratio)) : 0.0f, std::memory_order_relaxed);
        vis[3].store(r.clarity, std::memory_order_relaxed);
        if (history_.writeAvailable() > 0) {
            PitchFrame f{r.hz, targetHz, outHz, r.clarity};
            history_.write(&f, 1);
        }
    }

    // Latest track entry at or before `pos` (falls back to the oldest).
    const Track* trackAt(int64_t pos) const {
        if (trackCount_ == 0) return nullptr;
        const Track* best = &track_[trackHead_];
        for (int k = trackCount_ - 1; k >= 0; --k) {
            const Track& t = track_[(trackHead_ + k) % kTrackCap];
            if (t.center <= pos) return &t;
        }
        return best;
    }

    void generateAnalysisMarks() {
        const int unvoicedPeriod = static_cast<int>(0.005 * sr_);
        for (int guard = 0; guard < 64; ++guard) {
            const Track* t = trackAt(anaNext_);
            const bool voiced = t && t->period > 0.0f;
            const float T = voiced ? t->period : static_cast<float>(unvoicedPeriod);
            const int radius = voiced ? static_cast<int>(T * 0.2f) : 0;
            if (anaNext_ + static_cast<int64_t>(T) + radius + 2 >= w_) break;
            int64_t best = anaNext_;
            if (voiced) {
                // Align to the waveform peak near the predicted position.
                float bestV = -1e9f;
                for (int64_t q = anaNext_ - radius; q <= anaNext_ + radius; ++q) {
                    const float v = mono_[q & mask_];
                    if (v > bestV) {
                        bestV = v;
                        best = q;
                    }
                }
            }
            Mark& mk = marks_[(markHead_ + markCount_) % kMarkCap];
            if (markCount_ == kMarkCap) markHead_ = (markHead_ + 1) % kMarkCap;
            else ++markCount_;
            mk.pos = best;
            mk.period = T;
            mk.voiced = voiced;
            anaNext_ = best + static_cast<int64_t>(std::lround(T));
        }
    }

    const Mark* nearestMark(double pos) const {
        if (markCount_ == 0) return nullptr;
        const Mark* best = nullptr;
        double bestD = 1e18;
        for (int k = markCount_ - 1; k >= 0; --k) {
            const Mark& m = marks_[(markHead_ + k) % kMarkCap];
            const double d = std::abs(static_cast<double>(m.pos) - pos);
            if (d < bestD) {
                bestD = d;
                best = &m;
            } else if (static_cast<double>(m.pos) < pos) {
                break; // marks are sorted; moving further away
            }
        }
        return best;
    }

    void generateSynthesis() {
        const int64_t outNow = w_ - 1 - latency_;
        const bool preserve = pb(7);
        for (int guard = 0; guard < 64; ++guard) {
            if (synNext_ - tmax_ > static_cast<double>(outNow + hop_)) break;
            const Mark* a = nearestMark(synNext_);
            if (!a || static_cast<double>(a->pos) + a->period * 2.2 >= static_cast<double>(w_)) break;
            const Track* t = trackAt(static_cast<int64_t>(synNext_));
            const float ratio = (a->voiced && t) ? t->ratio : 1.0f;
            const float T = a->period;
            const int halfLen = std::max(8, static_cast<int>(T));
            const float formantStep = preserve ? 1.0f : ratio;
            const float gain = 1.0f / ratio; // overlap normalisation
            const int64_t m = static_cast<int64_t>(std::lround(synNext_));
            const float hScale = 2048.0f / static_cast<float>(halfLen);
            for (int k = -halfLen; k <= halfLen; ++k) {
                const float w = hann_[static_cast<int>((k + halfLen) * hScale)] * gain;
                const double src = static_cast<double>(a->pos) + k * formantStep;
                const int64_t si = static_cast<int64_t>(std::floor(src));
                const float fr = static_cast<float>(src - si);
                const int64_t dst = (m + k) & mask_;
                for (int c = 0; c < numCh_; ++c) {
                    const float s0 = in_[c][si & mask_], s1 = in_[c][(si + 1) & mask_];
                    out_[c][dst] += (s0 + (s1 - s0) * fr) * w;
                }
            }
            synNext_ += T / ratio;
        }
    }

    PitchDetector det_[4];
    SpscRing<PitchFrame> history_;
    std::vector<float> in_[2], out_[2], mono_, dry_, hann_;
    std::vector<Mark> marks_;
    std::vector<Track> track_;
    AlignDelay align_[2];
    int64_t mask_ = 0, w_ = 0, anaNext_ = 0;
    double synNext_ = 0.0;
    int markHead_ = 0, markCount_ = 0, trackHead_ = 0, trackCount_ = 0;
    int hop_ = 256, tmax_ = 640, latency_ = 2176, range_ = 1, sinceHop_ = 0, numCh_ = 1;
    double smoothMidi_ = -1.0, correction_ = 0.0, vibPhase_ = 0.0, voicedTime_ = 0.0;
    int lastNote_ = -1000;
};

} // namespace

void registerPitchCorrectionEffects(EffectRegistry& r) { registerEffect<AutotuneEffect>(r, kAtInfo); }

} // namespace echo
