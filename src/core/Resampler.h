// Variable-ratio windowed-sinc resampler.
//
// Used for two jobs at once: fixed sample-rate conversion between a device's
// native rate and the engine rate, and continuous clock-drift compensation
// (the ratio is nudged by a few ppm by a DriftController). The kernel is a
// Kaiser-windowed sinc stored as a polyphase table with linear interpolation
// between phases, so arbitrary (and time-varying) ratios are supported.
//
// ratio = input frames consumed per output frame (inRate / outRate).
//
// Threading: an instance belongs to a single thread (the engine thread).
// prepare() allocates and must not be called on the audio thread.
#pragma once

#include <algorithm>

#include <vector>

namespace echo {

enum class ResamplerQuality { Low = 0, Normal = 1, High = 2 };

// Converts a whole interleaved recording between sample rates (not real
// time: for loading sound files).
std::vector<float> resampleOffline(const std::vector<float>& in, int channels, double inRate, double outRate);

class Resampler {
public:
    // nominalRatio sets the anti-aliasing cutoff. maxInputChunk bounds a
    // single push() so the internal buffer never needs to grow.
    void prepare(int channels, double nominalRatio, int maxInputChunk,
                 ResamplerQuality quality = ResamplerQuality::Normal);
    void reset();

    int channels() const { return channels_; }
    int taps() const { return taps_; }
    double nominalRatio() const { return nominalRatio_; }

    // Appends interleaved input. Returns frames accepted (all, unless full).
    int push(const float* interleaved, int frames);
    int pushSilence(int frames);

    // Frames buffered but not yet consumed (including filter history).
    int bufferedInput() const { return writePos_ - readPos_; }

    // Output frames that can be produced right now at this ratio.
    int outputAvailable(double ratio) const;

    // Additional input frames required before `outFrames` can be produced.
    int inputNeeded(int outFrames, double ratio) const;

    // Produces up to `frames` interleaved output frames. Returns count.
    int pull(float* outInterleaved, int frames, double ratio);

    // Group delay in input frames.
    double latencyInputFrames() const { return taps_ * 0.5; }

    // Space left for push().
    int inputSpace() const { return capacity_ - writePos_ + compactableFrames(); }

private:
    int compactableFrames() const;
    void compact();
    void buildKernel(double cutoff, double beta);

    int channels_ = 0;
    int taps_ = 32;
    int phases_ = 256;
    double nominalRatio_ = 1.0;
    std::vector<float> table_;   // (phases_ + 1) * taps_
    std::vector<float> buffer_;  // capacity_ * channels_, interleaved
    int capacity_ = 0;
    int writePos_ = 0;           // next frame index to write
    int readPos_ = 0;            // integer part of the read position
    double frac_ = 0.0;          // fractional part of the read position
};

// PI controller that keeps a ring buffer near a target fill level by
// adjusting a resampler ratio. Works for both capture (consumer adjusts) and
// render (producer adjusts): in both cases, a ring that is too full asks for
// a larger in/out ratio.
// Extra device buffering that grows after a dropout and shrinks again after a
// long stable stretch: stable systems keep the lowest latency, while a device
// that glitches (e.g. one that occasionally skips a whole packet) stops
// glitching after the first dropout instead of every minute or two.
class AdaptiveMargin {
public:
    void prepare(double stepFrames, double maxFrames, double relaxBlocks) {
        step_ = stepFrames;
        max_ = maxFrames;
        relax_ = relaxBlocks;
        extra_ = 0.0;
        clean_ = 0.0;
    }
    double extra() const { return extra_; }
    void onDropout() {
        extra_ = std::min(max_, extra_ + step_);
        clean_ = 0.0;
    }
    // Call once per block without a dropout; true when the margin shrank.
    bool onCleanBlock() {
        if (extra_ <= 0.0 || ++clean_ < relax_) return false;
        clean_ = 0.0;
        extra_ = std::max(0.0, extra_ - step_);
        return true;
    }

private:
    double step_ = 0.0, max_ = 0.0, relax_ = 1.0, extra_ = 0.0, clean_ = 0.0;
};

class DriftController {
public:
    void prepare(double nominalRatio, double targetFillFrames, double deviceRate,
                 double blockSeconds);
    void reset();
    // Feed the current fill (device frames); returns the ratio to use.
    double update(double fillFrames);
    double ratio() const { return ratio_; }
    double correctionPpm() const { return (ratio_ / nominalRatio_ - 1.0) * 1e6; }
    double smoothedFill() const { return fill_; }
    double targetFill() const { return target_; }
    void setTargetFill(double t) { target_ = t; }

private:
    double nominalRatio_ = 1.0, ratio_ = 1.0;
    double target_ = 0.0, fill_ = 0.0, integ_ = 0.0;
    double deviceRate_ = 48000.0, blockSeconds_ = 0.01;
    bool primed_ = false;
};

} // namespace echo
