// Echo - shared constants and small real-time-safe helpers.
#pragma once

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace echo {

// Hard limits keep every real-time structure fixed-size, so the audio thread
// never allocates. They are generous, not product tiers.
constexpr int kMaxBlockSize = 4096;     // engine frames per processing block
constexpr int kMaxSourceChannels = 8;   // channels kept from a capture device
constexpr int kMaxStrips = 64;          // input strips
constexpr int kMaxBuses = 16;           // output buses
constexpr int kMaxSources = 64;         // distinct capture sources
constexpr int kMaxSinks = 16;           // distinct output devices
constexpr int kMaxChainLength = 24;     // effects per strip/bus chain
constexpr int kMaxEffectParams = 40;    // parameters per effect

constexpr float kPi = 3.14159265358979323846f;
constexpr double kPiD = 3.14159265358979323846;
constexpr float kTwoPi = 2.0f * kPi;

constexpr float kSilenceDb = -120.0f;

inline float dbToGain(float db) {
    return db <= kSilenceDb ? 0.0f : std::pow(10.0f, db * 0.05f);
}

inline float gainToDb(float g) {
    return g <= 1e-6f ? kSilenceDb : 20.0f * std::log10(g);
}

template <typename T>
constexpr T clampv(T v, T lo, T hi) { return v < lo ? lo : (v > hi ? hi : v); }

inline bool isFiniteSample(float x) { return std::isfinite(x); }

// Flush denormals to zero on the calling thread (SSE FTZ + DAZ). Every
// real-time thread calls this once; denormal arithmetic in IIR tails can cost
// 100x and cause dropouts.
void enableFlushDenormals();

// Relaxed atomic float max, used for meter peaks written by the audio thread.
inline void atomicMax(std::atomic<float>& a, float v) {
    float cur = a.load(std::memory_order_relaxed);
    while (v > cur && !a.compare_exchange_weak(cur, v, std::memory_order_relaxed)) {}
}

// Non-copyable mixin.
struct NonCopyable {
    NonCopyable() = default;
    NonCopyable(const NonCopyable&) = delete;
    NonCopyable& operator=(const NonCopyable&) = delete;
};

} // namespace echo
