// Real FFT (power-of-two sizes) built on an iterative radix-2 complex FFT
// of half size. prepare() allocates; forward()/inverse() are real-time safe.
#pragma once

#include <vector>

namespace echo {

class RealFft {
public:
    void prepare(int n);
    int size() const { return n_; }

    // in: n real samples. out: n/2+1 complex bins, interleaved (re, im).
    void forward(const float* in, float* out);
    // in: n/2+1 complex bins (interleaved). out: n real samples, scaled by 1/n
    // so that inverse(forward(x)) == x.
    void inverse(const float* in, float* out);

private:
    void complexFft(float* data, bool inverse);

    int n_ = 0;
    int half_ = 0;
    std::vector<int> bitrev_;
    std::vector<float> twiddle_;  // half_/2 complex
    std::vector<float> post_;     // n/2+1 complex: exp(-2 pi i k / n)
    std::vector<float> work_;     // half_ complex
};

// Hann window of length n (periodic if `periodic`).
void makeHann(float* w, int n, bool periodic = true);

} // namespace echo
