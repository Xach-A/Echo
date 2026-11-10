// Monophonic pitch detector based on YIN (de Cheveigné & Kawahara, 2002).
//
// Analysis runs on a decimated copy (~12 kHz) for speed, then the period is
// refined against the full-rate signal. Typical accuracy on clean voiced
// input is a few cents.
#pragma once

#include <vector>

#include "dsp/Biquad.h"

namespace echo {

class PitchDetector {
public:
    struct Result {
        float hz = 0.0f;          // 0 when unvoiced
        float periodSamples = 0;  // full-rate period, 0 when unvoiced
        float clarity = 0.0f;     // 1 - CMNDF minimum (1 = perfectly periodic)
        float rmsDb = -120.0f;
        bool voiced = false;
    };

    void prepare(double sampleRate, double minHz, double maxHz);
    void reset();
    void setThresholds(float yinThreshold, float silenceDb) {
        threshold_ = yinThreshold;
        silenceDb_ = silenceDb;
    }

    // Tracking: the period (full-rate samples) found last, 0 = none. With
    // one, analyze() prefers the dip nearest it (an octave away costs as
    // much as 0.15 more CMNDF), so a strong second harmonic or a breath can't
    // make the pitch jump an octave; a real leap up still wins, since a dip
    // at two to four times a strong dip is taken for that dip's multiple.
    void setPrior(float periodSamples) { prior_ = periodSamples; }

    // Feed full-rate audio continuously.
    void push(const float* x, int n);
    void push(float x);
    // Estimate pitch over the most recent window.
    Result analyze();

    // Length of audio (full-rate samples) the estimate describes; its centre
    // lies about half of this before the newest sample.
    int windowSamples() const { return (win_ + tauMax_) * dec_; }
    double minHz() const { return minHz_; }
    double maxHz() const { return maxHz_; }

private:
    double sr_ = 48000.0, fsd_ = 12000.0, minHz_ = 60.0, maxHz_ = 1000.0;
    int dec_ = 4, decPhase_ = 0;
    int tauMin_ = 12, tauMax_ = 200, win_ = 300;
    float threshold_ = 0.15f, silenceDb_ = -55.0f, prior_ = 0.0f;
    Biquad aa_[2];
    std::vector<float> decBuf_;   // circular, decimated
    int decPos_ = 0;
    std::vector<float> fullBuf_;  // circular, full rate
    int fullPos_ = 0;
    std::vector<float> frame_, diff_, cmnd_;
};

} // namespace echo
