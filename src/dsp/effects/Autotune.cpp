// Real-time pitch correction ("autotune").
//
// Analysis: YIN pitch detection every ~5 ms on a mono sum, as a tracker: the
//   last period is the detector's prior (no octave jumps on a strong second
//   harmonic), and a gap of up to two frames mid-note keeps the last pitch.
// Decision: the sung pitch is split into its centre (followed quickly at a
//   note's start, then below vibrato rates, so vibrato never moves the note)
//   and the movement around it (vibrato, scoops). The note is the nearest of
//   the key's scale to the centre, held unless the centre is clearly nearer
//   another; a clear legato move away from it is followed quickly. The
//   retune speed pulls the centre onto the note (humanize: more slowly on
//   held notes); the movement around it stays as sung at a note's start and
//   is scaled by Natural vibrato once the note is held (the fastest retune
//   flattens it: the robotic sound). Strength scales the whole correction.
// Synthesis: pitch-synchronous overlap-add (TD-PSOLA). Analysis marks are one
//   period apart, each where a period around it best matches the period
//   around the last mark, to a fraction of a sample (the same point of the
//   waveform; the loudest sample jumps between the formants' similar peaks,
//   whole samples jitter); grains, read with cubic interpolation, two periods
//   long are re-spaced at period/ratio and normalised by the windows' actual
//   overlap (no level ripple at the pitch rate). Grains are not resampled, so
//   formants are preserved; with "Preserve formants" off, grains are
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
    {"speed", "Retune speed", 0.0f, 500.0f, 50.0f, "ms", ParamScale::Linear, nullptr,
     "How quickly a note is pulled onto pitch; 0 ms = instant (hard tune, vibrato flattened too)"},
    {"smoothing", "Transition smoothing", 0.0f, 200.0f, 20.0f, "ms", ParamScale::Linear, nullptr,
     "How quickly a new note is followed: higher glides between notes more smoothly"},
    {"reference", "A4 reference", 415.0f, 466.0f, 440.0f, "Hz", ParamScale::Linear},
    {"preserve", "Preserve formants", 0.0f, 1.0f, 1.0f, "", ParamScale::Toggle},
    {"vib_rate", "Vibrato rate", 0.0f, 10.0f, 5.5f, "Hz", ParamScale::Linear, nullptr, "Added vibrato (with Vibrato depth)"},
    {"vib_depth", "Vibrato depth", 0.0f, 100.0f, 0.0f, "cents", ParamScale::Linear, nullptr, "Adds a vibrato of this depth"},
    {"range", "Voice range", 0.0f, 3.0f, 1.0f, "", ParamScale::Enum, kRanges,
     "Narrower ranges track better and add less latency"},
    {"mix", "Mix", 0.0f, 100.0f, 100.0f, "%", ParamScale::Linear},
    {"humanize", "Humanize", 0.0f, 100.0f, 0.0f, "%", ParamScale::Linear, nullptr,
     "Held notes are pulled onto pitch more slowly, keeping their natural swells; short notes still snap"},
    {"vibrato", "Natural vibrato", 0.0f, 150.0f, 100.0f, "%", ParamScale::Linear, nullptr,
     "Your own vibrato on held notes: 0 = flattened, 100 = as sung, 150 = deeper"},
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
        wsum_.assign(size, 0.0f);
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
        std::fill(wsum_.begin(), wsum_.end(), 0.0f);
        std::fill(mono_.begin(), mono_.end(), 0.0f);
        for (auto& a : align_) { // sized for the widest range in onPrepare; no reallocation here
            a.setDelay(latency_, maxBlock_);
            a.reset();
        }
        w_ = 0;
        markHead_ = markCount_ = 0;
        trackHead_ = trackCount_ = 0;
        anaNext_ = static_cast<double>(tmax_);
        synNext_ = static_cast<double>(tmax_);
        voiced_ = false;
        hang_ = unvoicedHops_ = awayHops_ = 0;
        lastHz_ = lastPeriod_ = 0.0f;
        center1_ = center_ = corr_ = 0.0;
        note_ = -1000;
        noteAge_ = 0.0;
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
            // The grains overlapping here, normalised by their windows' sum.
            const int64_t o = (w_ - 1 - latency_) & mask_;
            const float norm = 1.0f / std::max(wsum_[o], 0.5f);
            for (int c = 0; c < numCh; ++c) {
                ch[c][i] = out_[c][o] * norm;
                out_[c][o] = 0.0f;
            }
            wsum_[o] = 0.0f;
        }
        if (mix < 0.999f)
            for (int c = 0; c < numCh; ++c) mixLinear(ch[c], dry_.data() + static_cast<size_t>(c) * maxBlock_, n, mix);
    }

private:
    struct Mark {
        double pos = 0.0; // to a fraction of a sample
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
    static constexpr int kMaxSearch = 160; // samples either side (15 % of the longest period)

    void analyzeHop(PitchDetector& det) {
        PitchDetector::Result r = det.analyze();
        const double hopSec = hop_ / sr_;
        // A gap of up to two frames inside a note (a breath of noise, a missed
        // period) keeps the last pitch, so the note doesn't restart.
        if (r.voiced) {
            hang_ = 0;
        } else if (voiced_ && hang_ < 2 && r.rmsDb > -55.0f && lastHz_ > 0.0f) {
            ++hang_;
            r.voiced = true;
            r.hz = lastHz_;
            r.periodSamples = lastPeriod_;
        }
        if (r.voiced) {
            lastHz_ = r.hz;
            lastPeriod_ = r.periodSamples;
            unvoicedHops_ = 0;
            det.setPrior(r.periodSamples);
        } else if (++unvoicedHops_ > 10) {
            det.setPrior(0.0f); // after 50 ms without a voice: no prior
        }

        int style = pi(2);
        float strength = p(3) / 100.0f, speedMs = p(4), smoothMs = p(5), vibrato = p(13) / 100.0f;
        if (style == 0) {
            strength = 0.85f;
            speedMs = 90.0f;
            smoothMs = 35.0f;
            vibrato = 1.0f;
        } else if (style == 1) {
            strength = 1.0f;
            speedMs = 0.0f;
            smoothMs = 0.0f;
            vibrato = 0.0f;
        }
        vibrato *= std::min(1.0f, speedMs / 12.0f); // the fastest retunes flatten vibrato too
        const double ref = p(6);
        const int key = clampv(pi(0), 0, 11), scale = clampv(pi(1), 0, music::NumScales - 1);
        float ratio = 1.0f, targetHz = 0.0f, outHz = 0.0f;
        if (r.voiced) {
            voicedTime_ += hopSec;
            const double midi = music::hzToMidi(r.hz, ref);
            if (!voiced_) {
                // A note starts: everything from here.
                center1_ = center_ = midi;
                note_ = music::nearestNote(midi, key, scale);
                corr_ = 0.0;
                noteAge_ = 0.0;
                awayHops_ = 0;
            } else {
                noteAge_ += hopSec;
                // A clear move away from the centre (the next note, sung
                // legato) makes the centre follow quickly again.
                if (std::abs(midi - center_) > 0.7) {
                    if (++awayHops_ >= 3) {
                        noteAge_ = 0.0;
                        awayHops_ = 0;
                    }
                } else {
                    awayHops_ = 0;
                }
                // The centre (two one-pole stages): quick at a note's start, then
                // slow (90 ms each), well below vibrato rates.
                const double fastT = std::max(0.004, smoothMs * 0.001 * 0.5);
                const double slowT = 0.09;
                const double blend = std::clamp((noteAge_ - 0.1) / 0.15, 0.0, 1.0);
                const double a = 1.0 - std::exp(-hopSec / (fastT + (slowT - fastT) * blend));
                center1_ += (midi - center1_) * a;
                center_ += (center1_ - center_) * a;
            }
            // The note: nearest of the scale to the centre, kept unless the
            // centre is clearly nearer another.
            int note = music::nearestNote(center_, key, scale);
            if (note_ > -1000 && note != note_ && music::noteAllowed(note_, key, scale) && std::abs(center_ - note_) < 0.6)
                note = note_;
            if (note != note_) {
                note_ = note;
                noteAge_ = 0.0;
            }
            // Retune: the centre is pulled onto the note; humanize slows this
            // once a note has been held a moment (0.12 s, fully by 0.37 s).
            const double held = std::clamp((noteAge_ - 0.12) / 0.25, 0.0, 1.0) * p(12) / 100.0;
            const double speedEff = speedMs + held * 300.0;
            const double desired = note_ - center_;
            const double aR = speedEff <= 0.0 ? 0.0 : std::exp(-hopSec / (speedEff * 0.001));
            corr_ = desired + aR * (corr_ - desired);
            // The movement around the centre: as sung while a note starts, then
            // scaled by Natural vibrato (fully from 0.35 s on).
            const double dev = midi - center_;
            const double settled = std::clamp((noteAge_ - 0.15) / 0.2, 0.0, 1.0);
            const double outMidi = center_ + corr_ + dev * (settled * vibrato + (1.0 - settled));
            double semis = (outMidi - midi) * strength;
            // Added vibrato fades in after 150 ms of continuous voicing.
            const double vibDepth = p(9) / 100.0 * std::min(1.0, std::max(0.0, (voicedTime_ - 0.15) / 0.3));
            vibPhase_ += p(8) * hopSec;
            vibPhase_ -= std::floor(vibPhase_);
            semis += vibDepth * std::sin(2.0 * kPiD * vibPhase_);
            semis = clampv(semis, -3.0, 3.0);
            ratio = static_cast<float>(std::pow(2.0, semis / 12.0));
            targetHz = static_cast<float>(music::midiToHz(note_, ref));
            outHz = r.hz * ratio;
        } else {
            voicedTime_ = 0.0;
            corr_ *= 0.5;
            note_ = -1000;
        }
        voiced_ = r.voiced;
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
            const Track* t = trackAt(static_cast<int64_t>(anaNext_));
            const bool voiced = t && t->period > 0.0f;
            const float T = voiced ? t->period : static_cast<float>(unvoicedPeriod);
            const int radius = voiced ? std::max(2, static_cast<int>(T * 0.2f)) : 0;
            const int half = static_cast<int>(T * 0.5f);
            if (static_cast<int64_t>(anaNext_) + static_cast<int64_t>(T) + radius + half + 4 >= w_) break;
            double best = std::floor(anaNext_);
            const Mark* last = markCount_ > 0 ? &marks_[(markHead_ + markCount_ - 1) % kMarkCap] : nullptr;
            if (voiced && last && last->voiced && std::abs(last->period - T) < 0.25f * T) {
                // The same point of the waveform as the last mark, one period
                // on: the lag (to a fraction of a sample) at which a period
                // around the candidate best matches the period around the
                // last mark. (The loudest sample would jump between the
                // formants' similar peaks; whole samples would jitter.)
                const int64_t lp = std::llround(last->pos);
                const int lag0 = static_cast<int>(std::lround(T));
                const int r2 = std::min(kMaxSearch, std::max(2, static_cast<int>(T * 0.15f)));
                double score[2 * kMaxSearch + 1];
                int bi = 0;
                for (int i = 0; i <= 2 * r2; ++i) {
                    const int64_t q = lp + lag0 - r2 + i;
                    double xy = 0.0, xx = 0.0, yy = 0.0;
                    for (int k = -half; k <= half; k += 2) {
                        const double xa = mono_[(q + k) & mask_], xb = mono_[(lp + k) & mask_];
                        xy += xa * xb;
                        xx += xa * xa;
                        yy += xb * xb;
                    }
                    score[i] = xy / std::sqrt(xx * yy + 1e-20);
                    if (score[i] > score[bi]) bi = i;
                }
                double frac = 0.0;
                if (bi > 0 && bi < 2 * r2) {
                    const double y0 = score[bi - 1], y1 = score[bi], y2 = score[bi + 1], den = y0 - 2.0 * y1 + y2;
                    if (den < -1e-12) frac = std::clamp(0.5 * (y0 - y2) / den, -0.5, 0.5);
                }
                best = last->pos + (lag0 - r2 + bi) + frac;
            } else if (voiced) {
                // The first mark of a voiced stretch: the waveform's peak near
                // the predicted position.
                float bestV = -1e9f;
                const int64_t c = static_cast<int64_t>(anaNext_);
                for (int64_t q = c - radius; q <= c + radius; ++q) {
                    const float v = mono_[q & mask_];
                    if (v > bestV) {
                        bestV = v;
                        best = static_cast<double>(q);
                    }
                }
            }
            Mark& mk = marks_[(markHead_ + markCount_) % kMarkCap];
            if (markCount_ == kMarkCap) markHead_ = (markHead_ + 1) % kMarkCap;
            else ++markCount_;
            mk.pos = best;
            mk.period = T;
            mk.voiced = voiced;
            anaNext_ = best + T;
        }
    }

    const Mark* nearestMark(double pos) const {
        if (markCount_ == 0) return nullptr;
        const Mark* best = nullptr;
        double bestD = 1e18;
        for (int k = markCount_ - 1; k >= 0; --k) {
            const Mark& m = marks_[(markHead_ + k) % kMarkCap];
            const double d = std::abs(m.pos - pos);
            if (d < bestD) {
                bestD = d;
                best = &m;
            } else if (m.pos < pos) {
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
            if (!a || a->pos + a->period * 2.2 >= static_cast<double>(w_)) break;
            const Track* t = trackAt(static_cast<int64_t>(synNext_));
            const float ratio = (a->voiced && t) ? t->ratio : 1.0f;
            const float T = a->period;
            const int halfLen = std::max(8, static_cast<int>(T));
            const float formantStep = preserve ? 1.0f : ratio;
            const int64_t m = static_cast<int64_t>(std::lround(synNext_));
            const float hScale = 2048.0f / static_cast<float>(halfLen);
            for (int k = -halfLen; k <= halfLen; ++k) {
                const float w = hann_[static_cast<int>((k + halfLen) * hScale)];
                const double src = a->pos + k * formantStep;
                const int64_t si = static_cast<int64_t>(std::floor(src));
                const float fr = static_cast<float>(src - si);
                const int64_t dst = (m + k) & mask_;
                for (int c = 0; c < numCh_; ++c) {
                    // Cubic (Catmull-Rom): fractional marks without dulling the highs.
                    const std::vector<float>& x = in_[c];
                    const float xm = x[(si - 1) & mask_], x0 = x[si & mask_], x1 = x[(si + 1) & mask_], x2 = x[(si + 2) & mask_];
                    const float v =
                        x0 + 0.5f * fr * (x1 - xm + fr * (2.0f * xm - 5.0f * x0 + 4.0f * x1 - x2 + fr * (3.0f * (x0 - x1) + x2 - xm)));
                    out_[c][dst] += v * w;
                }
                wsum_[dst] += w;
            }
            synNext_ += T / ratio;
        }
    }

    PitchDetector det_[4];
    SpscRing<PitchFrame> history_;
    std::vector<float> in_[2], out_[2], wsum_, mono_, dry_, hann_;
    std::vector<Mark> marks_;
    std::vector<Track> track_;
    AlignDelay align_[2];
    int64_t mask_ = 0, w_ = 0;
    double anaNext_ = 0.0;
    double synNext_ = 0.0;
    int markHead_ = 0, markCount_ = 0, trackHead_ = 0, trackCount_ = 0;
    int hop_ = 256, tmax_ = 640, latency_ = 2176, range_ = 1, sinceHop_ = 0, numCh_ = 1;
    // Tracking and correction state (see analyzeHop).
    bool voiced_ = false;
    int hang_ = 0, unvoicedHops_ = 0, awayHops_ = 0, note_ = -1000;
    float lastHz_ = 0.0f, lastPeriod_ = 0.0f;
    double center1_ = 0.0, center_ = 0.0, corr_ = 0.0, noteAge_ = 0.0, vibPhase_ = 0.0, voicedTime_ = 0.0;
};

} // namespace

void registerPitchCorrectionEffects(EffectRegistry& r) { registerEffect<AutotuneEffect>(r, kAtInfo); }

} // namespace echo
