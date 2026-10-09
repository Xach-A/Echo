// Level metering shared between the audio thread (writer) and the UI
// (reader). The audio thread publishes per-block peak and mean-square values
// as "max since last read"; the UI consumes them with exchange().
#pragma once

#include <atomic>
#include <cmath>
#include <cstdint>

#include "core/Common.h"

namespace echo {

struct MeterState {
    std::atomic<float> peak[2] = {0.0f, 0.0f};
    std::atomic<float> meanSquare[2] = {0.0f, 0.0f};
    std::atomic<uint32_t> clipCount{0};     // samples >= 0 dBFS (sticky counter)
    std::atomic<uint32_t> nonFiniteCount{0};

    // Audio thread. `r` may equal `l` for mono signals.
    void analyze(const float* l, const float* r, int n) {
        float pk[2] = {0.0f, 0.0f};
        float ss[2] = {0.0f, 0.0f};
        uint32_t clips = 0;
        const float* ch[2] = {l, r};
        for (int c = 0; c < 2; ++c) {
            const float* x = ch[c];
            float p = 0.0f, s = 0.0f;
            for (int i = 0; i < n; ++i) {
                const float a = std::fabs(x[i]);
                p = a > p ? a : p;
                s += x[i] * x[i];
                if (a >= 0.99997f) ++clips;
            }
            pk[c] = p;
            ss[c] = n > 0 ? s / static_cast<float>(n) : 0.0f;
        }
        for (int c = 0; c < 2; ++c) {
            atomicMax(peak[c], pk[c]);
            atomicMax(meanSquare[c], ss[c]);
        }
        if (clips) clipCount.fetch_add(clips, std::memory_order_relaxed);
    }

    // UI thread.
    float takePeak(int c) { return peak[c].exchange(0.0f, std::memory_order_relaxed); }
    float takeMeanSquare(int c) { return meanSquare[c].exchange(0.0f, std::memory_order_relaxed); }
};

// UI-side ballistics: peak with release, peak-hold and clip latch.
struct MeterBallistics {
    float levelDb[2] = {-120.0f, -120.0f};
    float rmsDb[2] = {-120.0f, -120.0f};
    float holdDb[2] = {-120.0f, -120.0f};
    double holdTime[2] = {0.0, 0.0};
    uint32_t lastClipCount = 0;
    bool clipLatched = false;

    void update(MeterState& m, double now, double dt) {
        for (int c = 0; c < 2; ++c) {
            const float pk = gainToDb(m.takePeak(c));
            const float ms = m.takeMeanSquare(c);
            const float rms = ms > 0.0f ? 10.0f * std::log10(ms) : -120.0f;
            const float fall = static_cast<float>(dt * 24.0); // dB per second release
            levelDb[c] = pk > levelDb[c] ? pk : std::max(pk, levelDb[c] - fall);
            rmsDb[c] = rms > rmsDb[c] ? rms : std::max(rms, rmsDb[c] - fall);
            if (pk >= holdDb[c] || now - holdTime[c] > 1.5) {
                holdDb[c] = pk > holdDb[c] ? pk : std::max(pk, holdDb[c] - fall);
                if (pk >= holdDb[c]) holdTime[c] = now;
            }
        }
        const uint32_t cc = m.clipCount.load(std::memory_order_relaxed);
        if (cc != lastClipCount) {
            clipLatched = true;
            lastClipCount = cc;
        }
    }
};

} // namespace echo
