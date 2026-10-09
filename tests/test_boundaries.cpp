// Buffer-boundary and sample-rate robustness of every effect.
#include "doctest/doctest.h"

#include <cmath>
#include <memory>
#include <random>
#include <string>
#include <vector>

#include "TestUtil.h"
#include "dsp/EffectRegistry.h"

using namespace echo;
using namespace echo::test;

namespace {

// Speech-like test input: harmonic tone with vibrato and a level envelope,
// plus a little noise (exercises pitch trackers, dynamics and filters).
std::vector<float> testSignal(double sr, double seconds, uint32_t seed = 3) {
    const size_t n = static_cast<size_t>(sr * seconds);
    std::vector<float> x(n);
    std::mt19937 rng(seed);
    std::normal_distribution<float> nd(0.0f, 0.01f);
    double ph = 0.0;
    for (size_t i = 0; i < n; ++i) {
        const double t = i / sr;
        const double f0 = 165.0 * (1.0 + 0.02 * std::sin(2 * kPiD * 5.0 * t));
        ph += 2 * kPiD * f0 / sr;
        double v = 0.0;
        for (int h = 1; h <= 8; ++h) v += std::sin(h * ph) / h;
        const double env = 0.6 + 0.4 * std::sin(2 * kPiD * 1.3 * t);
        x[i] = static_cast<float>(0.12 * env * v) + nd(rng);
    }
    return x;
}

std::vector<float> runEffect(const char* type, double sr, int block, const std::vector<float>& in, int* latency) {
    auto fx = EffectRegistry::instance().create(type);
    REQUIRE(fx);
    fx->prepare(sr, block);
    std::vector<float> l = in, r = in;
    for (size_t pos = 0; pos < l.size(); pos += block) {
        const int n = static_cast<int>(std::min<size_t>(block, l.size() - pos));
        float* ch[2] = {l.data() + pos, r.data() + pos};
        fx->process(ch, fx->info().widensStereo ? 2 : 1, n);
    }
    if (latency) *latency = fx->latencySamples();
    return l;
}

} // namespace

TEST_CASE("effects: output does not depend on the processing block size") {
    // The engine may run any block size; an effect must give the same audio
    // (apart from a block-size-dependent latency) whether it is called with
    // 480, 128 or 37 frames at a time.
    const double sr = 48000.0;
    const auto in = testSignal(sr, 2.0);
    int checked = 0;
    for (const auto& reg : EffectRegistry::instance().all()) {
        const std::string type = reg.info->typeId;
        CAPTURE(type);
        int lat480 = 0;
        const auto ref = runEffect(type.c_str(), sr, 480, in, &lat480);
        for (const int block : {128, 37}) {
            CAPTURE(block);
            int lat = 0;
            const auto out = runEffect(type.c_str(), sr, block, in, &lat);
            const int shift = lat - lat480; // extra delay at this block size
            // Compare after both have settled (skip 0.5 s), aligned by latency.
            double err = 0.0, sig = 0.0;
            for (size_t i = 24000; i + 4096 < ref.size(); ++i) {
                const long j = static_cast<long>(i) + shift;
                if (j < 0 || static_cast<size_t>(j) >= out.size()) continue;
                const double d = static_cast<double>(out[j]) - ref[i];
                err += d * d;
                sig += static_cast<double>(ref[i]) * ref[i];
            }
            const double relDb = 10.0 * std::log10((err + 1e-30) / (sig + 1e-30));
            if (relDb > -80.0) MESSAGE(type << " @ " << block << " frames: difference " << relDb << " dB (latency "
                                             << lat480 << " -> " << lat << ")");
            CHECK(relDb < -80.0);
        }
        ++checked;
    }
    CHECK(checked >= 30);
}

TEST_CASE("effects: stable and audible at 44.1, 48 and 96 kHz with odd block sizes") {
    for (const auto& [sr, block] : {std::pair{44100.0, 441}, std::pair{48000.0, 256}, std::pair{96000.0, 960},
                                    std::pair{44100.0, 64}}) {
        const auto in = testSignal(sr, 1.5);
        const float inRms = rms(in, in.size() / 3, in.size() / 2);
        for (const auto& reg : EffectRegistry::instance().all()) {
            const std::string type = reg.info->typeId;
            CAPTURE(type);
            CAPTURE(sr);
            CAPTURE(block);
            const auto out = runEffect(type.c_str(), sr, block, in, nullptr);
            CHECK(allFinite(out));
            float peak = 0.0f;
            for (float v : out) peak = std::max(peak, std::fabs(v));
            CHECK(peak < 4.0f); // no blow-up (input peaks ~0.35)
            // Every effect passes audible signal at its default settings.
            const float outRms = rms(out, out.size() / 3, out.size() / 2);
            CHECK(outRms > inRms * 0.05f);
        }
    }
}
