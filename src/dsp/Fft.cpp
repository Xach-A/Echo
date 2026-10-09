#include "dsp/Fft.h"

#include <cmath>
#include <cstring>

#include "core/Common.h"

namespace echo {

void makeHann(float* w, int n, bool periodic) {
    const double denom = periodic ? n : (n - 1);
    for (int i = 0; i < n; ++i) w[i] = static_cast<float>(0.5 - 0.5 * std::cos(2.0 * kPiD * i / denom));
}

void RealFft::prepare(int n) {
    n_ = n;
    half_ = n / 2;
    bitrev_.resize(half_);
    int bits = 0;
    while ((1 << bits) < half_) ++bits;
    for (int i = 0; i < half_; ++i) {
        int r = 0;
        for (int b = 0; b < bits; ++b)
            if (i & (1 << b)) r |= 1 << (bits - 1 - b);
        bitrev_[i] = r;
    }
    twiddle_.resize(static_cast<size_t>(std::max(1, half_ / 2)) * 2);
    for (int k = 0; k < half_ / 2; ++k) {
        const double a = -2.0 * kPiD * k / half_;
        twiddle_[2 * k] = static_cast<float>(std::cos(a));
        twiddle_[2 * k + 1] = static_cast<float>(std::sin(a));
    }
    post_.resize(static_cast<size_t>(half_ + 1) * 2);
    for (int k = 0; k <= half_; ++k) {
        const double a = -2.0 * kPiD * k / n_;
        post_[2 * k] = static_cast<float>(std::cos(a));
        post_[2 * k + 1] = static_cast<float>(std::sin(a));
    }
    work_.assign(static_cast<size_t>(half_) * 2, 0.0f);
}

void RealFft::complexFft(float* d, bool inverse) {
    const int m = half_;
    for (int i = 0; i < m; ++i) {
        const int j = bitrev_[i];
        if (j > i) {
            std::swap(d[2 * i], d[2 * j]);
            std::swap(d[2 * i + 1], d[2 * j + 1]);
        }
    }
    const float sign = inverse ? -1.0f : 1.0f;
    for (int len = 2; len <= m; len <<= 1) {
        const int halfLen = len >> 1;
        const int step = m / len;
        for (int start = 0; start < m; start += len) {
            for (int j = 0; j < halfLen; ++j) {
                const float wr = twiddle_[2 * j * step];
                const float wi = sign * twiddle_[2 * j * step + 1];
                float* a = d + 2 * (start + j);
                float* b = d + 2 * (start + j + halfLen);
                const float tr = b[0] * wr - b[1] * wi;
                const float ti = b[0] * wi + b[1] * wr;
                b[0] = a[0] - tr;
                b[1] = a[1] - ti;
                a[0] += tr;
                a[1] += ti;
            }
        }
    }
}

void RealFft::forward(const float* in, float* out) {
    const int m = half_;
    float* z = work_.data();
    std::memcpy(z, in, sizeof(float) * n_); // pack x[2k] + i x[2k+1]
    complexFft(z, false);
    // Split into the spectrum of the real sequence.
    out[0] = z[0] + z[1];
    out[1] = 0.0f;
    out[2 * m] = z[0] - z[1];
    out[2 * m + 1] = 0.0f;
    for (int k = 1; k < m; ++k) {
        const float zr = z[2 * k], zi = z[2 * k + 1];
        const float cr = z[2 * (m - k)], ci = -z[2 * (m - k) + 1]; // conj(Z[m-k])
        const float er = 0.5f * (zr + cr), ei = 0.5f * (zi + ci);   // even part
        const float orr = 0.5f * (zi - ci), oi = -0.5f * (zr - cr); // odd part = (Z - conj)/(2i)
        const float wr = post_[2 * k], wi = post_[2 * k + 1];
        out[2 * k] = er + (orr * wr - oi * wi);
        out[2 * k + 1] = ei + (orr * wi + oi * wr);
    }
}

void RealFft::inverse(const float* in, float* out) {
    const int m = half_;
    float* z = work_.data();
    for (int k = 0; k < m; ++k) {
        const float xr = in[2 * k], xi = in[2 * k + 1];
        const float yr = in[2 * (m - k)], yi = -in[2 * (m - k) + 1]; // conj(X[m-k])
        const float er = 0.5f * (xr + yr), ei = 0.5f * (xi + yi);
        // odd = (X - conj(X[m-k])) / 2 * conj(w)
        const float dr = 0.5f * (xr - yr), di = 0.5f * (xi - yi);
        const float wr = post_[2 * k], wi = -post_[2 * k + 1];
        const float orr = dr * wr - di * wi, oi = dr * wi + di * wr;
        // Z = E + i*O
        z[2 * k] = er - oi;
        z[2 * k + 1] = ei + orr;
    }
    complexFft(z, true);
    const float scale = 1.0f / static_cast<float>(m);
    for (int i = 0; i < n_; ++i) out[i] = z[i] * scale;
}

} // namespace echo
