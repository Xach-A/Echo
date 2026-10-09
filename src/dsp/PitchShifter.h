// Pitch shifting cores.
//
// PvPitchShifter: phase vocoder with identity phase locking and optional
//   spectral-envelope (cepstral) processing, giving independent pitch and
//   formant control. Latency = fftSize. Best quality; higher latency.
//
// GrainPitchShifter: two-tap crossfaded delay ("Doppler") shifter. Very low
//   latency, cheap, formants move with pitch; used for doubling, harmonies
//   and a low-latency pitch mode.
#pragma once

#include <vector>

#include "dsp/DelayLine.h"
#include "dsp/Fft.h"

namespace echo {

class PvPitchShifter {
public:
    void prepare(int fftSize, double sampleRate);
    void reset();
    int latency() const { return n_; } // FIFO structure delays by one full frame
    int fftSize() const { return n_; }

    void setPitchRatio(float r) { pitch_ = clampRatio(r); }
    // Ratio applied to the spectral envelope (formants). Ignored unless
    // envelope processing is enabled.
    void setFormantRatio(float r) { formant_ = clampRatio(r); }
    void setEnvelopeProcessing(bool on) { envelope_ = on; }

    void process(float* x, int n);

private:
    static float clampRatio(float r) { return r < 0.25f ? 0.25f : (r > 4.0f ? 4.0f : r); }
    void processFrame();
    void computeEnvelope();

    int n_ = 2048, hop_ = 512, bins_ = 1025, rover_ = 0;
    double sr_ = 48000.0;
    float pitch_ = 1.0f, formant_ = 1.0f;
    bool envelope_ = true;
    RealFft fft_;
    std::vector<float> win_, inFifo_, outFifo_, accum_, frame_, spec_;
    std::vector<float> anaMag_, anaFreq_, anaPhase_, lastPhase_, flatMag_;
    std::vector<float> synMag_, synPhase_;
    std::vector<float> env_, cep_, cepSpec_;
    std::vector<int> peaks_, prevPeakBins_, newPeakBins_;
    std::vector<float> prevPeakPhases_, newPeakPhases_;
};

class GrainPitchShifter {
public:
    void prepare(double sampleRate, double maxGrainSeconds = 0.12);
    void reset();
    // Per-sample processing. grainSamples sets the window length.
    float process(float x, float ratio, float grainSamples);

private:
    // Picks a restart delay for a tap so its waveform lines up with the other
    // tap (WSOLA-style), avoiding the comb-filter dropouts of a naive
    // two-tap shifter on steady tones.
    float alignedDelay(float nominal, float otherDelay, float ratio, float grain);

    DelayLine line_;
    float delay_[2] = {0.0f, 0.0f};
    float phase_ = 0.0f;  // tap 0 window position; tap 1 is offset by 0.5
    float search_ = 480.0f;
    int corrLen_ = 256;
    float maxDelay_ = 4096.0f;
    bool started_ = false;
};

} // namespace echo
