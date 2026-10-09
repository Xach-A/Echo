#include "dsp/PitchShifter.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace echo {

void PvPitchShifter::prepare(int fftSize, double sampleRate) {
    n_ = fftSize;
    hop_ = n_ / 4;
    bins_ = n_ / 2 + 1;
    sr_ = sampleRate;
    fft_.prepare(n_);
    win_.resize(n_);
    makeHann(win_.data(), n_, true);
    inFifo_.assign(n_, 0.0f);
    outFifo_.assign(n_, 0.0f);
    accum_.assign(2 * n_, 0.0f);
    frame_.assign(n_, 0.0f);
    spec_.assign(n_ + 2, 0.0f);
    for (auto* v : {&anaMag_, &anaFreq_, &anaPhase_, &lastPhase_, &synMag_, &synPhase_, &flatMag_, &env_})
        v->assign(bins_, 0.0f);
    prevPeakBins_.reserve(bins_);
    prevPeakPhases_.reserve(bins_);
    newPeakBins_.reserve(bins_);
    newPeakPhases_.reserve(bins_);
    cep_.assign(n_, 0.0f);
    cepSpec_.assign(n_ + 2, 0.0f);
    peaks_.reserve(bins_);
    reset();
}

void PvPitchShifter::reset() {
    std::fill(inFifo_.begin(), inFifo_.end(), 0.0f);
    std::fill(outFifo_.begin(), outFifo_.end(), 0.0f);
    std::fill(accum_.begin(), accum_.end(), 0.0f);
    std::fill(lastPhase_.begin(), lastPhase_.end(), 0.0f);
    prevPeakBins_.clear();
    prevPeakPhases_.clear();
    rover_ = n_ - hop_;
}

void PvPitchShifter::process(float* x, int count) {
    const int lat = n_ - hop_;
    for (int i = 0; i < count; ++i) {
        inFifo_[rover_] = x[i];
        x[i] = outFifo_[rover_ - lat];
        if (++rover_ >= n_) {
            rover_ = lat;
            processFrame();
            std::memcpy(outFifo_.data(), accum_.data(), sizeof(float) * hop_);
            std::memmove(accum_.data(), accum_.data() + hop_, sizeof(float) * n_);
            std::memset(accum_.data() + n_, 0, sizeof(float) * hop_);
            std::memmove(inFifo_.data(), inFifo_.data() + hop_, sizeof(float) * lat);
        }
    }
}

void PvPitchShifter::computeEnvelope() {
    // Real cepstrum of the log magnitude, liftered to keep the smooth
    // spectral envelope (formants) and drop the harmonic fine structure.
    for (int k = 0; k < bins_; ++k) {
        cepSpec_[2 * k] = std::log(anaMag_[k] + 1e-9f);
        cepSpec_[2 * k + 1] = 0.0f;
    }
    fft_.inverse(cepSpec_.data(), cep_.data());
    const int lifter = std::clamp(static_cast<int>(std::lround(0.001 * sr_)), 8, n_ / 8);
    for (int i = lifter + 1; i < n_ - lifter; ++i) cep_[i] = 0.0f;
    cep_[lifter] *= 0.5f;
    cep_[n_ - lifter] *= 0.5f;
    fft_.forward(cep_.data(), cepSpec_.data());
    for (int k = 0; k < bins_; ++k) env_[k] = std::exp(std::min(30.0f, cepSpec_[2 * k]));
}

void PvPitchShifter::processFrame() {
    const float twoPi = kTwoPi;
    const float osamp = static_cast<float>(n_ / hop_);
    const float expct = twoPi * static_cast<float>(hop_) / static_cast<float>(n_);

    for (int k = 0; k < n_; ++k) frame_[k] = inFifo_[k] * win_[k];
    fft_.forward(frame_.data(), spec_.data());
    for (int k = 0; k < bins_; ++k) {
        const float re = spec_[2 * k], im = spec_[2 * k + 1];
        const float mag = std::sqrt(re * re + im * im);
        const float ph = std::atan2(im, re);
        float dp = ph - lastPhase_[k] - static_cast<float>(k) * expct;
        lastPhase_[k] = ph;
        dp -= twoPi * std::round(dp / twoPi);
        anaFreq_[k] = static_cast<float>(k) + dp * osamp / twoPi;
        anaMag_[k] = mag;
        anaPhase_[k] = ph;
    }
    if (envelope_) computeEnvelope();

    // Peak-region pitch shifting (Laroche & Dolson): every spectral peak's
    // region of influence is moved rigidly to the shifted frequency, which
    // keeps each partial's window-lobe shape intact. The peak's phase is
    // advanced by its true shifted frequency (tracked across frames) and the
    // other bins in its region keep their analysed phase offsets.
    float maxMag = 0.0f;
    for (int k = 0; k < bins_; ++k) {
        flatMag_[k] = envelope_ ? anaMag_[k] / std::max(env_[k], 1e-9f) : anaMag_[k];
        maxMag = std::max(maxMag, flatMag_[k]);
    }
    const float floorMag = maxMag * 1e-4f;
    peaks_.clear();
    for (int k = 1; k < bins_ - 1; ++k)
        if (flatMag_[k] > flatMag_[k - 1] && flatMag_[k] >= flatMag_[k + 1] && flatMag_[k] > floorMag)
            peaks_.push_back(k);
    std::fill(synMag_.begin(), synMag_.end(), 0.0f);
    std::fill(synPhase_.begin(), synPhase_.end(), 0.0f);
    newPeakBins_.clear();
    newPeakPhases_.clear();
    const int np = static_cast<int>(peaks_.size());
    int lo = 0;
    for (int i = 0; i < np; ++i) {
        const int pk = peaks_[i];
        int hi = bins_;
        if (i + 1 < np) {
            // Region boundary: the magnitude minimum between this peak and the next.
            hi = pk + 1;
            for (int k = pk + 1; k < peaks_[i + 1]; ++k)
                if (flatMag_[k] < flatMag_[hi]) hi = k;
        }
        const float tf = anaFreq_[pk] * pitch_; // true shifted frequency, in bins
        const int jp = static_cast<int>(std::lround(tf));
        if (jp >= 0 && jp < bins_) {
            // Continue the phase of the nearest peak from the previous frame.
            float psi = anaPhase_[pk];
            if (!prevPeakBins_.empty()) {
                auto it = std::lower_bound(prevPeakBins_.begin(), prevPeakBins_.end(), jp);
                int bestIdx = -1, bestDist = 3;
                for (auto c : {it, it == prevPeakBins_.begin() ? it : it - 1}) {
                    if (c == prevPeakBins_.end()) continue;
                    const int d = std::abs(*c - jp);
                    if (d < bestDist) {
                        bestDist = d;
                        bestIdx = static_cast<int>(c - prevPeakBins_.begin());
                    }
                }
                if (bestIdx >= 0) psi = prevPeakPhases_[bestIdx] + expct * tf;
            }
            psi -= twoPi * std::floor(psi / twoPi);
            newPeakBins_.push_back(jp);
            newPeakPhases_.push_back(psi);
            const int shift = jp - pk;
            for (int k = lo; k < hi; ++k) {
                const int j = k + shift;
                if (j < 0 || j >= bins_) continue;
                synMag_[j] += flatMag_[k];
                synPhase_[j] = psi + (anaPhase_[k] - anaPhase_[pk]);
            }
        }
        lo = hi;
    }
    prevPeakBins_.swap(newPeakBins_);
    prevPeakPhases_.swap(newPeakPhases_);

    if (envelope_) {
        for (int j = 0; j < bins_; ++j) {
            const float pos = static_cast<float>(j) / formant_;
            float e;
            if (pos >= bins_ - 1) e = env_[bins_ - 1] * 0.1f;
            else {
                const int i0 = static_cast<int>(pos);
                const float f = pos - i0;
                e = env_[i0] + (env_[i0 + 1] - env_[i0]) * f;
            }
            synMag_[j] *= e;
        }
    }
    for (int j = 0; j < bins_; ++j) {
        spec_[2 * j] = synMag_[j] * std::cos(synPhase_[j]);
        spec_[2 * j + 1] = synMag_[j] * std::sin(synPhase_[j]);
    }
    fft_.inverse(spec_.data(), frame_.data());
    const float norm = 1.0f / 1.5f; // Hann^2 overlap-add at 75% overlap
    for (int k = 0; k < n_; ++k) accum_[k] += frame_[k] * win_[k] * norm;
}

// --------------------------------------------------------------------------

void GrainPitchShifter::prepare(double sampleRate, double maxGrainSeconds) {
    search_ = static_cast<float>(sampleRate * 0.012);
    corrLen_ = static_cast<int>(sampleRate * 0.006);
    const int total = static_cast<int>(sampleRate * maxGrainSeconds * 2.0 + search_ * 2 + corrLen_ * 4) + 64;
    line_.prepare(total);
    maxDelay_ = static_cast<float>(line_.maxDelay() - 4);
    reset();
}

void GrainPitchShifter::reset() {
    line_.reset();
    phase_ = 0.0f;
    started_ = false;
}

float GrainPitchShifter::alignedDelay(float nominal, float otherDelay, float ratio, float grain) {
    // Candidate delays [nominal, nominal + search]. Both taps read at the
    // same rate, so their upcoming outputs match when the input segments
    // starting at their read positions match.
    const int other = static_cast<int>(otherDelay);
    float best = nominal, bestScore = -2.0f;
    const int span = static_cast<int>(search_);
    // Compare the recent past of both read positions (always available).
    const int len = corrLen_;
    if (other + len >= maxDelay_) return nominal;
    double eb = 0.0;
    for (int j = 0; j < len; j += 2) {
        const float b = line_.read(other + j);
        eb += static_cast<double>(b) * b;
    }
    for (int s = 0; s <= span; s += 2) {
        const int d = static_cast<int>(nominal) + s;
        if (d + len > maxDelay_ - grain * std::fabs(1.0f - ratio) - 4) break;
        double xy = 0.0, ea = 0.0;
        for (int j = 0; j < len; j += 2) {
            const float a = line_.read(d + j);
            const float b = line_.read(other + j);
            xy += static_cast<double>(a) * b;
            ea += static_cast<double>(a) * a;
        }
        const float score = static_cast<float>(xy / std::sqrt(ea * eb + 1e-20));
        if (score > bestScore) {
            bestScore = score;
            best = static_cast<float>(d);
        }
    }
    return best;
}

float GrainPitchShifter::process(float x, float ratio, float grainSamples) {
    line_.push(x);
    const float g = std::max(32.0f, grainSamples);
    // Delay drift over a grain: up-shift reads faster (delay shrinks).
    const float drift = (1.0f - ratio) * g; // delay change over one grain
    const float minDelay = 2.0f + (drift < 0.0f ? -drift : 0.0f);
    if (!started_) {
        delay_[0] = minDelay;
        delay_[1] = minDelay + std::fabs(drift) * 0.5f;
        started_ = true;
    }
    const float prev = phase_;
    phase_ += 1.0f / g;
    if (phase_ >= 1.0f) phase_ -= 1.0f;
    // Tap 0 restarts when its window crosses 0; tap 1 when phase crosses 0.5.
    if (phase_ < prev) delay_[0] = alignedDelay(minDelay, delay_[1], ratio, g);
    if (prev < 0.5f && phase_ >= 0.5f) delay_[1] = alignedDelay(minDelay, delay_[0], ratio, g);
    delay_[0] = std::clamp(delay_[0] + (1.0f - ratio), 1.0f, maxDelay_);
    delay_[1] = std::clamp(delay_[1] + (1.0f - ratio), 1.0f, maxDelay_);
    const float p1 = phase_ >= 0.5f ? phase_ - 0.5f : phase_ + 0.5f;
    const float w0 = std::sin(kPi * phase_), w1 = std::sin(kPi * p1);
    return line_.readLinear(delay_[0]) * w0 * w0 + line_.readLinear(delay_[1]) * w1 * w1;
}

} // namespace echo
