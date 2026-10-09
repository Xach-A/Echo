// Streaming short-time Fourier transform with 75% overlap and sqrt-Hann
// analysis/synthesis windows (perfect reconstruction when the spectrum is
// left unchanged). Latency is N samples (measured: a frame's first hop is
// complete only after the whole frame has been analysed).
#pragma once

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

#include "dsp/Fft.h"

namespace echo {

class Stft {
public:
    void prepare(int n) {
        n_ = n;
        hop_ = n / 4;
        fft_.prepare(n);
        win_.resize(n);
        makeHann(win_.data(), n, true);
        for (float& w : win_) w = std::sqrt(w);
        inFifo_.assign(n, 0.0f);
        outFifo_.assign(n, 0.0f);
        accum_.assign(2 * n, 0.0f);
        frame_.assign(n, 0.0f);
        spec_.assign(n + 2, 0.0f);
        reset();
    }
    void reset() {
        std::fill(inFifo_.begin(), inFifo_.end(), 0.0f);
        std::fill(outFifo_.begin(), outFifo_.end(), 0.0f);
        std::fill(accum_.begin(), accum_.end(), 0.0f);
        rover_ = n_ - hop_;
    }
    int size() const { return n_; }
    int hop() const { return hop_; }
    int latency() const { return n_; }

    // Processes one block in place. `onFrame(float* spectrum)` receives
    // n/2+1 interleaved complex bins and may modify them.
    template <typename F>
    void process(float* x, int count, F&& onFrame) {
        const int lat = n_ - hop_;
        // sqrt-Hann * sqrt-Hann = Hann; Hann at 75% overlap sums to 2.
        const float norm = 0.5f;
        for (int i = 0; i < count; ++i) {
            inFifo_[rover_] = x[i];
            x[i] = outFifo_[rover_ - lat];
            if (++rover_ >= n_) {
                rover_ = lat;
                for (int k = 0; k < n_; ++k) frame_[k] = inFifo_[k] * win_[k];
                fft_.forward(frame_.data(), spec_.data());
                onFrame(spec_.data());
                fft_.inverse(spec_.data(), frame_.data());
                for (int k = 0; k < n_; ++k) accum_[k] += frame_[k] * win_[k] * norm;
                std::memcpy(outFifo_.data(), accum_.data(), sizeof(float) * hop_);
                std::memmove(accum_.data(), accum_.data() + hop_, sizeof(float) * n_);
                std::memset(accum_.data() + n_, 0, sizeof(float) * hop_);
                std::memmove(inFifo_.data(), inFifo_.data() + hop_, sizeof(float) * lat);
            }
        }
    }

private:
    int n_ = 1024, hop_ = 256, rover_ = 0;
    RealFft fft_;
    std::vector<float> win_, inFifo_, outFifo_, accum_, frame_, spec_;
};

} // namespace echo
