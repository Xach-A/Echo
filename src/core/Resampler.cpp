#include "core/Resampler.h"
#include "core/Common.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace echo {

namespace {

double besselI0(double x) {
    // Power series; converges quickly for the beta range used here.
    double sum = 1.0, term = 1.0;
    const double q = x * x * 0.25;
    for (int k = 1; k < 64; ++k) {
        term *= q / (static_cast<double>(k) * static_cast<double>(k));
        sum += term;
        if (term < sum * 1e-12) break;
    }
    return sum;
}

} // namespace

void Resampler::prepare(int channels, double nominalRatio, int maxInputChunk,
                        ResamplerQuality quality) {
    channels_ = std::max(1, channels);
    nominalRatio_ = nominalRatio > 0.0 ? nominalRatio : 1.0;

    double factor = 0.90, beta = 8.0;
    switch (quality) {
    case ResamplerQuality::Low:    taps_ = 16; phases_ = 256; factor = 0.80; beta = 6.0; break;
    case ResamplerQuality::Normal: taps_ = 32; phases_ = 512; factor = 0.90; beta = 8.0; break;
    case ResamplerQuality::High:   taps_ = 64; phases_ = 512; factor = 0.94; beta = 9.5; break;
    }
    const double base = std::min(1.0, 1.0 / nominalRatio_);
    buildKernel(base * factor, beta);

    capacity_ = std::max(8192, 4 * maxInputChunk + 4 * taps_);
    buffer_.assign(static_cast<size_t>(capacity_) * channels_, 0.0f);
    reset();
}

void Resampler::buildKernel(double cutoff, double beta) {
    table_.assign(static_cast<size_t>(phases_ + 1) * taps_, 0.0f);
    const double half = taps_ * 0.5;
    const double i0b = besselI0(beta);
    for (int ph = 0; ph <= phases_; ++ph) {
        const double frac = static_cast<double>(ph) / phases_;
        double sum = 0.0;
        float* row = &table_[static_cast<size_t>(ph) * taps_];
        for (int k = 0; k < taps_; ++k) {
            const double x = frac + half - 1.0 - k; // distance from output time
            double w = 0.0;
            if (std::abs(x) < half) {
                const double r = x / half;
                w = besselI0(beta * std::sqrt(std::max(0.0, 1.0 - r * r))) / i0b;
            }
            const double y = cutoff * x;
            const double s = std::abs(y) < 1e-12 ? 1.0 : std::sin(kPiD * y) / (kPiD * y);
            const double h = cutoff * s * w;
            row[k] = static_cast<float>(h);
            sum += h;
        }
        // Normalise each phase to unity DC gain so the ripple across phases
        // does not appear as modulation noise.
        if (std::abs(sum) > 1e-9)
            for (int k = 0; k < taps_; ++k) row[k] = static_cast<float>(row[k] / sum);
    }
}

void Resampler::reset() {
    std::fill(buffer_.begin(), buffer_.end(), 0.0f);
    // Prefill half a kernel of silence so the first real sample can be
    // reached by the filter window; this is the resampler's fixed latency.
    writePos_ = taps_ / 2;
    readPos_ = taps_ / 2 - 1;
    frac_ = 0.0;
}

int Resampler::compactableFrames() const {
    return std::max(0, readPos_ - taps_ / 2 + 1);
}

void Resampler::compact() {
    const int start = compactableFrames();
    if (start <= 0) return;
    const int remain = writePos_ - start;
    if (remain > 0)
        std::memmove(buffer_.data(), buffer_.data() + static_cast<size_t>(start) * channels_,
                     static_cast<size_t>(remain) * channels_ * sizeof(float));
    writePos_ -= start;
    readPos_ -= start;
}

int Resampler::push(const float* in, int frames) {
    if (frames <= 0) return 0;
    if (writePos_ + frames > capacity_) compact();
    const int n = std::min(frames, capacity_ - writePos_);
    if (n > 0) {
        std::memcpy(buffer_.data() + static_cast<size_t>(writePos_) * channels_, in,
                    static_cast<size_t>(n) * channels_ * sizeof(float));
        writePos_ += n;
    }
    return n;
}

int Resampler::pushSilence(int frames) {
    if (frames <= 0) return 0;
    if (writePos_ + frames > capacity_) compact();
    const int n = std::min(frames, capacity_ - writePos_);
    if (n > 0) {
        std::memset(buffer_.data() + static_cast<size_t>(writePos_) * channels_, 0,
                    static_cast<size_t>(n) * channels_ * sizeof(float));
        writePos_ += n;
    }
    return n;
}

int Resampler::outputAvailable(double ratio) const {
    const int maxOffset = writePos_ - 1 - taps_ / 2 - readPos_;
    if (maxOffset < 0) return 0;
    // Largest n with floor(frac + (n-1)*ratio) <= maxOffset.
    const double v = (maxOffset + 1 - frac_) / ratio;
    int n = static_cast<int>(std::ceil(v - 1e-9));
    return std::max(0, n);
}

int Resampler::inputNeeded(int outFrames, double ratio) const {
    if (outFrames <= 0) return 0;
    const double last = frac_ + (outFrames - 1) * ratio;
    const int needWrite = readPos_ + static_cast<int>(std::floor(last + 1e-9)) + taps_ / 2 + 1;
    return std::max(0, needWrite - writePos_);
}

int Resampler::pull(float* out, int frames, double ratio) {
    const int avail = std::min(frames, outputAvailable(ratio));
    const int ch = channels_;
    const int half = taps_ / 2;
    float coeff[64];
    for (int n = 0; n < avail; ++n) {
        const double phasePos = frac_ * phases_;
        int ip = static_cast<int>(phasePos);
        if (ip >= phases_) ip = phases_ - 1;
        const float fp = static_cast<float>(phasePos - ip);
        const float* a = &table_[static_cast<size_t>(ip) * taps_];
        const float* b = a + taps_;
        for (int k = 0; k < taps_; ++k) coeff[k] = a[k] + (b[k] - a[k]) * fp;

        const float* src = buffer_.data() + static_cast<size_t>(readPos_ - half + 1) * ch;
        float* dst = out + static_cast<size_t>(n) * ch;
        if (ch == 2) {
            float l = 0.0f, r = 0.0f;
            for (int k = 0; k < taps_; ++k) {
                l += src[2 * k] * coeff[k];
                r += src[2 * k + 1] * coeff[k];
            }
            dst[0] = l;
            dst[1] = r;
        } else {
            for (int c = 0; c < ch; ++c) {
                float acc = 0.0f;
                for (int k = 0; k < taps_; ++k) acc += src[k * ch + c] * coeff[k];
                dst[c] = acc;
            }
        }
        frac_ += ratio;
        const int adv = static_cast<int>(frac_);
        readPos_ += adv;
        frac_ -= adv;
    }
    return avail;
}

// --------------------------------------------------------------------------

void DriftController::prepare(double nominalRatio, double targetFillFrames, double deviceRate,
                              double blockSeconds) {
    nominalRatio_ = nominalRatio;
    target_ = targetFillFrames;
    deviceRate_ = deviceRate > 0 ? deviceRate : 48000.0;
    blockSeconds_ = blockSeconds > 0 ? blockSeconds : 0.01;
    reset();
}

void DriftController::reset() {
    ratio_ = nominalRatio_;
    integ_ = 0.0;
    fill_ = target_;
    primed_ = false;
}

double DriftController::update(double fillFrames) {
    if (!primed_) {
        fill_ = fillFrames;
        primed_ = true;
    }
    // ~0.5 s moving average hides device packetisation sawtooth.
    const double alpha = 1.0 - std::exp(-blockSeconds_ / 0.5);
    fill_ += alpha * (fillFrames - fill_);

    const double err = (fill_ - target_) / deviceRate_; // seconds
    // Critically damped PI for an integrating plant: Kp^2 = 4 Ki.
    constexpr double kp = 0.2, ki = 0.01;
    integ_ = std::clamp(integ_ + err * ki * blockSeconds_, -0.003, 0.003);
    const double corr = std::clamp(kp * err + integ_, -0.005, 0.005);
    ratio_ = nominalRatio_ * (1.0 + corr);
    return ratio_;
}

} // namespace echo
