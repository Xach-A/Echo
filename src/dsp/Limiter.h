// Brick-wall look-ahead peak limiter.
//
// Gain computer: per-sample required gain -> sliding minimum over the
// look-ahead window -> exponential release -> box filter of the same length.
// The box filter turns the instant attack into a linear ramp that reaches
// full reduction exactly when the peak leaves the delay line, so output
// peaks never exceed the ceiling (to float precision).
#pragma once

#include <vector>

namespace echo {

class LookaheadLimiter {
public:
    void prepare(double sampleRate, double lookaheadSeconds, int channels = 2);
    void reset();
    void setCeilingDb(float db);
    void setReleaseSeconds(float s);
    // In place. ch[1] ignored when numCh == 1.
    void process(float* const* ch, int numCh, int n);
    int latencySamples() const { return look_; }
    float currentGainReductionDb() const { return grDb_; }

private:
    double sr_ = 48000.0;
    int look_ = 48;
    int channels_ = 2;
    float ceiling_ = 0.966f;
    float releaseCoeff_ = 0.999f;

    std::vector<float> delay_;   // channels * look_
    int dpos_ = 0;

    // Sliding minimum (monotonic deque over a circular buffer).
    std::vector<float> dqVal_;
    std::vector<int> dqIdx_;
    int dqHead_ = 0, dqTail_ = 0, dqCap_ = 0;
    int sampleIndex_ = 0;

    float env_ = 1.0f;
    std::vector<float> box_;
    int boxPos_ = 0;
    double boxSum_ = 0.0;
    int boxRefresh_ = 0;
    float grDb_ = 0.0f;
};

} // namespace echo
