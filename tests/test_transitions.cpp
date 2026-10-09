// Switching effects and chains on/off while audio runs: no dropouts, no
// clicks, no stale audio.
#include "doctest/doctest.h"

#include <cmath>
#include <memory>
#include <vector>

#include "TestUtil.h"
#include "core/Mixer.h"
#include "core/Sources.h"
#include "dsp/EffectRegistry.h"

using namespace echo;
using namespace echo::test;

namespace {

constexpr double kSr = 48000.0;
constexpr int kBlock = 480;

std::shared_ptr<Effect> makeFx(const char* type) {
    std::shared_ptr<Effect> e(EffectRegistry::instance().create(type).release());
    REQUIRE(e);
    e->prepare(kSr, kBlock);
    return e;
}

// Milliseconds in [from, to) where the 2 ms RMS is more than 6 dB below `ref`.
double msBelowHalf(const std::vector<float>& x, size_t from, size_t to, float ref) {
    int count = 0;
    for (size_t i = from; i + 96 <= to && i + 96 <= x.size(); i += 48) count += rms(x, i, 96) < ref * 0.5f;
    return count; // one window per millisecond
}

float maxStep(const std::vector<float>& x, size_t from, size_t to) {
    float m = 0.0f;
    for (size_t i = std::max<size_t>(from, 1); i < to && i < x.size(); ++i) m = std::max(m, std::fabs(x[i] - x[i - 1]));
    return m;
}

} // namespace

TEST_CASE("transitions: switching on a latent effect never drops the audio") {
    // Effects that delay the signal (identity settings so the level stays the same).
    struct Case {
        const char* type;
        const char* param;
        float value;
    };
    for (const Case c : {Case{"voice_denoise", "strength", 0.0f}, Case{"pitch", "semitones", 0.0f},
                         Case{"echo_cancel", "suppression", 0.0f}, Case{"limiter", "ceiling", 0.0f}}) {
        CAPTURE(c.type);
        auto fx = makeFx(c.type);
        fx->setParam(c.param, c.value);
        if (std::string(c.type) == "pitch") fx->setParam("preserve", 0.0f);
        fx->bypass = true;
        ChainRunner runner;
        runner.prepare(kBlock);
        ChainSnap chain;
        chain.fx[chain.count++] = fx.get();
        const size_t total = static_cast<size_t>(kSr * 1.5), on = static_cast<size_t>(kSr * 0.5);
        std::vector<float> out(total), dummyR(kBlock);
        double ph = 0.0;
        for (size_t pos = 0; pos < total; pos += kBlock) {
            if (pos == on) fx->bypass = false;
            float* l = out.data() + pos;
            for (int i = 0; i < kBlock; ++i, ph += 2 * kPiD * 220.0 / kSr) l[i] = 0.25f * static_cast<float>(std::sin(ph));
            bool mono = true;
            runner.process(chain, l, dummyR.data(), mono, kBlock);
        }
        const float steady = rms(out, 0, on);
        // Crossfading from the live signal to the (delayed) processed one can
        // partly cancel for a few ms; it must not fall silent for the latency.
        const double low = msBelowHalf(out, on - 960, on + 4800, steady);
        MESSAGE(c.type << " (latency " << fx->latencySamples() / 48.0 << " ms): " << low
                       << " ms more than 6 dB down around switch-on");
        CHECK(low <= 6.0);
        CHECK(rms(out, total - 9600, 9600) > steady * 0.9f); // the effect is active afterwards
    }
}

TEST_CASE("transitions: whole-chain A/B bypass switches back seamlessly") {
    StripRuntime strip;
    strip.prepare(kSr, kBlock);
    auto fx = makeFx("pitch");
    fx->setParam("semitones", -5.0f);
    ChainSnap chain;
    chain.fx[chain.count++] = fx.get();
    BusAccumulators acc{};
    std::vector<float> busL(kBlock), busR(kBlock);
    acc.l[0] = busL.data();
    acc.r[0] = busR.data();
    acc.used[0] = true;
    strip.sends[0].enabled = true;

    class Sine final : public SourceRuntime {
    public:
        void pull(int frames) override {
            for (int i = 0; i < frames; ++i, ph_ += 2 * kPiD * 220.0 / kSr)
                channelMut(0)[i] = 0.25f * static_cast<float>(std::sin(ph_));
        }
        double ph_ = 0.0;
    } src;
    src.prepare(kSr, kBlock, kBlock / kSr);

    // Bypass for 1 s (short A/B: chain kept warm) and for 12 s (chain cold),
    // then switch back: the processed voice returns without a gap.
    for (const double offSeconds : {1.0, 12.0}) {
        CAPTURE(offSeconds);
        std::vector<float> out;
        auto runFor = [&](double seconds) {
            for (int b = 0; b < static_cast<int>(seconds * kSr / kBlock); ++b) {
                std::fill(busL.begin(), busL.end(), 0.0f);
                std::fill(busR.begin(), busR.end(), 0.0f);
                src.pull(kBlock);
                strip.process(&src, chain, kBlock, false, acc);
                out.insert(out.end(), busL.begin(), busL.end());
            }
        };
        strip.fxBypass = false;
        runFor(1.0);
        strip.fxBypass = true;
        runFor(offSeconds);
        const size_t back = out.size();
        strip.fxBypass = false;
        runFor(1.0);
        const float steady = rms(out, back - 9600, 9600);
        const double low = msBelowHalf(out, back, back + 9600, steady);
        MESSAGE("after " << offSeconds << " s bypassed: " << low << " ms more than 6 dB down after switching back");
        CHECK(low <= 6.0);
        CHECK(rms(out, out.size() - 9600, 9600) > steady * 0.7f);
    }
}

TEST_CASE("transitions: bus effect bypass is crossfaded (no click)") {
    BusRuntime bus;
    bus.limiterEnabled = false;
    bus.prepare(kSr, kBlock);
    auto gain = makeFx("gain");
    gain->setParam("gain", 12.0f);
    ChainSnap chain;
    chain.fx[chain.count++] = gain.get();
    std::vector<float> out, l(kBlock), r(kBlock);
    double ph = 0.0;
    for (int b = 0; b < 100; ++b) {
        if (b == 50) bus.fxBypass = true;
        for (int i = 0; i < kBlock; ++i, ph += 2 * kPiD * 143.3 / kSr) l[i] = r[i] = 0.2f * static_cast<float>(std::sin(ph));
        bus.process(chain, l.data(), r.data(), kBlock);
        out.insert(out.end(), l.begin(), l.end());
    }
    // A 143.3 Hz sine at 0.8 peak changes by at most ~0.015 per sample; an
    // abrupt 4x -> 1x gain switch (here at a non-zero phase) jumps by up to 0.6.
    const float step = maxStep(out, 49 * kBlock, 52 * kBlock);
    MESSAGE("largest sample step around the bus bypass switch: " << step);
    CHECK(step < 0.03f);
}
