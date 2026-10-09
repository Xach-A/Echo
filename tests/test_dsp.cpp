// DSP tests using generated signals. Each effect is checked for the
// behaviour it claims, plus a fuzz pass for NaN/Inf/instability.
#include "doctest/doctest.h"

#include <memory>
#include <random>

#include "TestUtil.h"
#include "dsp/EffectRegistry.h"
#include "dsp/Fft.h"
#include "dsp/PitchDetector.h"
#include "dsp/PitchShifter.h"
#include "dsp/Responses.h"

using namespace echo;
using namespace echo::test;

namespace {

constexpr double kSr = 48000.0;
constexpr int kBlock = 480;

std::unique_ptr<Effect> make(const char* type) {
    auto e = EffectRegistry::instance().create(type);
    REQUIRE(e != nullptr);
    e->prepare(kSr, kBlock);
    return e;
}

// Runs a mono signal through an effect (as a mono chain would).
std::vector<float> runMono(Effect& e, std::vector<float> x) {
    for (size_t pos = 0; pos < x.size(); pos += kBlock) {
        const int n = static_cast<int>(std::min<size_t>(kBlock, x.size() - pos));
        float* ch[2] = {x.data() + pos, nullptr};
        e.process(ch, 1, n);
    }
    return x;
}

// Runs stereo (L, R) through an effect.
void runStereo(Effect& e, std::vector<float>& l, std::vector<float>& r) {
    for (size_t pos = 0; pos < l.size(); pos += kBlock) {
        const int n = static_cast<int>(std::min<size_t>(kBlock, l.size() - pos));
        float* ch[2] = {l.data() + pos, r.data() + pos};
        e.process(ch, 2, n);
    }
}

double levelDb(const std::vector<float>& y, double freq, double fromSec = 0.5, double lenSec = 0.5) {
    return 20.0 * std::log10(toneAmplitude(y, size_t(fromSec * kSr), size_t(lenSec * kSr), freq, kSr) + 1e-12);
}

std::vector<float> harmonicVoice(double f0, double seconds, float amp = 0.3f) {
    // Band-limited pulse-like signal with a formant-ish spectral tilt.
    std::vector<float> v(size_t(seconds * kSr), 0.0f);
    for (int h = 1; f0 * h < 6000.0; ++h) {
        const float a = amp / static_cast<float>(h);
        for (size_t i = 0; i < v.size(); ++i) v[i] += a * static_cast<float>(std::sin(2 * kPiD * f0 * h * i / kSr));
    }
    return v;
}

} // namespace

// ---------------------------------------------------------------------------

TEST_CASE("fft: forward/inverse round trip and bin placement") {
    RealFft fft;
    fft.prepare(1024);
    std::vector<float> x = sine(48000.0 * 32 / 1024, 48000.0, 1024);
    std::vector<float> spec(1026), back(1024);
    fft.forward(x.data(), spec.data());
    int peakBin = 0;
    float peakMag = 0;
    for (int k = 0; k <= 512; ++k) {
        const float m = std::hypot(spec[2 * k], spec[2 * k + 1]);
        if (m > peakMag) {
            peakMag = m;
            peakBin = k;
        }
    }
    CHECK(peakBin == 32);
    CHECK(peakMag == doctest::Approx(512.0).epsilon(0.001));
    fft.inverse(spec.data(), back.data());
    float err = 0;
    for (int i = 0; i < 1024; ++i) err = std::max(err, std::fabs(back[i] - x[i]));
    CHECK(err < 1e-4f);
}

TEST_CASE("registry: all effect types exist and have sane parameter metadata") {
    const auto& all = EffectRegistry::instance().all();
    CHECK(all.size() >= 25);
    for (const auto& e : all) {
        CAPTURE(e.info->typeId);
        CHECK(e.info->numParams <= kMaxEffectParams);
        for (int i = 0; i < e.info->numParams; ++i) {
            const ParamInfo& p = e.info->params[i];
            CHECK(p.min < p.max);
            CHECK(p.def >= p.min);
            CHECK(p.def <= p.max);
        }
    }
    for (const char* t : {"gain", "eq", "filter", "compressor", "limiter", "gate", "expander", "reverb", "delay",
                          "chorus", "flanger", "phaser", "distortion", "stereo", "deesser", "denoise", "agc",
                          "bitcrusher", "ringmod", "character", "robot", "pitch", "pitch_lite", "harmonizer",
                          "doubler", "octaver", "autotune"}) {
        CAPTURE(t);
        CHECK(EffectRegistry::instance().find(t) != nullptr);
    }
}

TEST_CASE("gain: applies exact dB gain and polarity") {
    auto e = make("gain");
    e->setParam("gain", -6.0f);
    auto y = runMono(*e, sine(1000, kSr, 48000, 0.5f));
    CHECK(levelDb(y, 1000) == doctest::Approx(gainToDb(0.5f) - 6.0).epsilon(0.002));
    e->setParam("gain", 0.0f);
    e->setParam("invert", 1.0f);
    auto x = sine(1000, kSr, 48000, 0.5f);
    y = runMono(*e, x);
    CHECK(y[40000] == doctest::Approx(-x[40000]).epsilon(0.001));
}

TEST_CASE("eq: bell boosts its band and leaves distant bands alone") {
    auto e = make("eq");
    e->setParam("b3_freq", 1000.0f);
    e->setParam("b3_gain", 12.0f);
    e->setParam("b3_q", 1.0f);
    std::vector<float> x(size_t(kSr * 1.0), 0.0f);
    auto a = sine(1000, kSr, x.size(), 0.1f), b = sine(100, kSr, x.size(), 0.1f), c = sine(8000, kSr, x.size(), 0.1f);
    for (size_t i = 0; i < x.size(); ++i) x[i] = a[i] + b[i] + c[i];
    auto y = runMono(*e, x);
    CHECK(levelDb(y, 1000) == doctest::Approx(-20.0 + 12.0).epsilon(0.02));
    CHECK(std::abs(levelDb(y, 100) - (-20.0)) < 0.6);
    CHECK(std::abs(levelDb(y, 8000) - (-20.0)) < 0.6);
    // The UI response curve agrees with the measured audio.
    CHECK(eqResponseDb(*e, 1000.0, kSr) == doctest::Approx(12.0).epsilon(0.01));
}

TEST_CASE("filter: 24 dB/oct high-pass attenuates two octaves below cutoff") {
    auto e = make("filter");
    e->setParam("hp_on", 1.0f);
    e->setParam("hp_freq", 1000.0f);
    e->setParam("hp_slope", 1.0f); // 24 dB/oct
    auto lo = runMono(*e, sine(250, kSr, 48000, 0.5f));
    e->resetState();
    auto hi = runMono(*e, sine(4000, kSr, 48000, 0.5f));
    CHECK(levelDb(lo, 250) - gainToDb(0.5f) < -40.0);
    CHECK(std::abs(levelDb(hi, 4000) - gainToDb(0.5f)) < 0.5);
    CHECK(filterResponseDb(*e, 1000.0, kSr) == doctest::Approx(-3.01).epsilon(0.05));
}

TEST_CASE("compressor: steady-state level follows threshold and ratio") {
    auto e = make("compressor");
    e->setParam("threshold", -18.0f);
    e->setParam("ratio", 4.0f);
    e->setParam("knee", 0.0f);
    e->setParam("attack", 1.0f);
    e->setParam("release", 50.0f);
    e->setParam("detector", 0.0f);
    // Peak detector on a sine at -6 dBFS peak: expect -18 + 12/4 = -15 dBFS.
    auto y = runMono(*e, sine(1000, kSr, size_t(kSr), dbToGain(-6.0f)));
    CHECK(levelDb(y, 1000) == doctest::Approx(-15.0).epsilon(0.03));
    CHECK(e->vis[0].load() < -8.0f);
}

TEST_CASE("compressor: transient is caught within the attack time") {
    auto e = make("compressor");
    e->setParam("threshold", -20.0f);
    e->setParam("ratio", 10.0f);
    e->setParam("attack", 5.0f);
    e->setParam("knee", 0.0f);
    std::vector<float> x(size_t(kSr), 0.0f);
    auto burst = sine(500, kSr, x.size(), 1.0f);
    for (size_t i = 24000; i < x.size(); ++i) x[i] = burst[i];
    auto y = runMono(*e, x);
    // 30 ms after onset the level must be well below the input level.
    CHECK(peak(y.data() + 24000 + 1440, 480) < 0.35f);
}

TEST_CASE("limiter: output never exceeds the ceiling") {
    auto e = make("limiter");
    e->setParam("ceiling", -1.0f);
    e->setParam("drive", 6.0f);
    std::vector<float> x(size_t(kSr), 0.0f);
    std::mt19937 rng(1);
    std::uniform_real_distribution<float> u(-1.0f, 1.0f);
    for (auto& v : x) v = u(rng);
    auto y = runMono(*e, x);
    CHECK(peak(y) <= dbToGain(-1.0f) * 1.0005f);
    CHECK(allFinite(y));
}

TEST_CASE("gate: closes on low-level noise, opens on signal") {
    auto e = make("gate");
    e->setParam("threshold", -40.0f);
    e->setParam("range", -80.0f);
    e->setParam("sc_hpf", 20.0f);
    std::vector<float> x(size_t(kSr * 2), 0.0f);
    std::mt19937 rng(2);
    std::normal_distribution<float> nd(0.0f, dbToGain(-60.0f));
    for (auto& v : x) v = nd(rng);
    auto tone = sine(440, kSr, x.size(), 0.3f);
    for (size_t i = size_t(kSr); i < x.size(); ++i) x[i] += tone[i];
    auto y = runMono(*e, x);
    CHECK(rms(y, size_t(0.5 * kSr), size_t(0.4 * kSr)) < dbToGain(-120.0f + 80.0f)); // attenuated noise
    CHECK(levelDb(y, 440, 1.5, 0.4) == doctest::Approx(gainToDb(0.3f)).epsilon(0.02));
}

TEST_CASE("expander: reduces quiet signals, leaves loud ones") {
    auto e = make("expander");
    e->setParam("threshold", -30.0f);
    e->setParam("ratio", 3.0f);
    e->setParam("range", -40.0f);
    e->setParam("knee", 0.0f);
    auto quiet = runMono(*e, sine(500, kSr, size_t(kSr), dbToGain(-45.0f)));
    e->resetState();
    auto loud = runMono(*e, sine(500, kSr, size_t(kSr), dbToGain(-10.0f)));
    CHECK(levelDb(quiet, 500) < -60.0);
    CHECK(levelDb(loud, 500) == doctest::Approx(-10.0).epsilon(0.02));
}

TEST_CASE("de-esser: reduces sibilant band only") {
    auto e = make("deesser");
    e->setParam("freq", 5000.0f);
    e->setParam("threshold", -40.0f);
    e->setParam("reduction", 12.0f);
    std::vector<float> x(size_t(kSr), 0.0f);
    auto s = sine(8000, kSr, x.size(), 0.3f), v = sine(300, kSr, x.size(), 0.3f);
    for (size_t i = 0; i < x.size(); ++i) x[i] = s[i] + v[i];
    auto y = runMono(*e, x);
    CHECK(levelDb(y, 8000) < gainToDb(0.3f) - 6.0);
    CHECK(std::abs(levelDb(y, 300) - gainToDb(0.3f)) < 1.0);
}

TEST_CASE("noise suppression: removes stationary noise, keeps the tone") {
    auto e = make("denoise");
    e->setParam("reduction", 25.0f);
    std::vector<float> x(size_t(kSr * 4), 0.0f);
    std::mt19937 rng(3);
    std::normal_distribution<float> nd(0.0f, dbToGain(-40.0f));
    for (auto& v : x) v = nd(rng);
    auto tone = sine(600, kSr, x.size(), 0.25f);
    for (size_t i = size_t(kSr * 3); i < x.size(); ++i) x[i] += tone[i];
    auto y = runMono(*e, x);
    const float noiseIn = rms(x, size_t(2.0 * kSr), size_t(0.8 * kSr));
    const float noiseOut = rms(y, size_t(2.0 * kSr), size_t(0.8 * kSr));
    CHECK(gainToDb(noiseOut) - gainToDb(noiseIn) < -12.0);
    CHECK(std::abs(levelDb(y, 600, 3.3, 0.5) - gainToDb(0.25f)) < 3.0);
}

TEST_CASE("agc: brings a quiet signal toward the target") {
    auto e = make("agc");
    e->setParam("target", -20.0f);
    e->setParam("speed", 20.0f);
    e->setParam("max_gain", 24.0f);
    auto y = runMono(*e, sine(440, kSr, size_t(kSr * 3), dbToGain(-32.0f)));
    // RMS of a sine = peak - 3 dB; target -20 dB RMS => peak around -17 dBFS.
    CHECK(std::abs(gainToDb(rms(y, size_t(2.5 * kSr), size_t(0.4 * kSr))) - (-20.0)) < 1.5);
}

TEST_CASE("delay: echo appears at the set time") {
    auto e = make("delay");
    e->setParam("time", 100.0f);
    e->setParam("feedback", 0.0f);
    e->setParam("mix", 50.0f);
    e->setParam("low_cut", 20.0f);
    e->setParam("high_cut", 20000.0f);
    std::vector<float> l(24000, 0.0f), r(24000, 0.0f);
    l[1000] = r[1000] = 1.0f;
    runStereo(*e, l, r);
    size_t best = 0;
    float bv = 0;
    for (size_t i = 2000; i < l.size(); ++i)
        if (std::fabs(l[i]) > bv) {
            bv = std::fabs(l[i]);
            best = i;
        }
    CHECK(std::abs(static_cast<int>(best) - (1000 + 4800)) <= 2);
}

TEST_CASE("reverb: produces a decaying, finite tail") {
    auto e = make("reverb");
    e->setParam("decay", 1.0f);
    e->setParam("mix", 100.0f);
    e->setParam("predelay", 0.0f);
    std::vector<float> l(size_t(kSr * 3), 0.0f), r(l.size(), 0.0f);
    l[100] = r[100] = 1.0f;
    runStereo(*e, l, r);
    CHECK(allFinite(l));
    CHECK(allFinite(r));
    const float early = rms(l, size_t(0.1 * kSr), size_t(0.1 * kSr));
    const float late = rms(l, size_t(1.1 * kSr), size_t(0.1 * kSr));
    CHECK(early > 1e-4f);
    // After 1 s (= RT60) the tail should have decayed by roughly 60 dB.
    CHECK(gainToDb(late) - gainToDb(early) < -40.0);
    CHECK(gainToDb(late) - gainToDb(early) > -80.0);
}

TEST_CASE("pitch detector: accurate on sines and harmonic tones, silent on noise") {
    for (double f : {82.4, 110.0, 196.0, 261.6, 440.0, 659.3, 880.0}) {
        PitchDetector d;
        d.prepare(kSr, 55.0, 1100.0);
        auto x = harmonicVoice(f, 0.3);
        d.push(x.data(), static_cast<int>(x.size()));
        auto r = d.analyze();
        CAPTURE(f);
        CHECK(r.voiced);
        CHECK(std::abs(cents(r.hz, f)) < 5.0);
    }
    PitchDetector d;
    d.prepare(kSr, 55.0, 1100.0);
    std::mt19937 rng(4);
    std::normal_distribution<float> nd(0.0f, 0.1f);
    std::vector<float> noise(size_t(kSr * 0.3));
    for (auto& v : noise) v = nd(rng);
    d.push(noise.data(), static_cast<int>(noise.size()));
    CHECK_FALSE(d.analyze().voiced);
}

TEST_CASE("pitch shifter (phase vocoder): shifts by the requested interval") {
    for (double semis : {-12.0, -5.0, 3.0, 7.0, 12.0}) {
        auto e = make("pitch");
        e->setParam("semitones", static_cast<float>(semis));
        e->setParam("preserve", 0.0f);
        auto y = runMono(*e, sine(440, kSr, size_t(kSr * 1.5), 0.4f));
        const double expect = 440.0 * std::pow(2.0, semis / 12.0);
        const double got = dominantFrequency(y.data() + size_t(0.6 * kSr), size_t(0.6 * kSr), kSr, 150, 1200);
        CAPTURE(semis);
        CHECK(std::abs(cents(got, expect)) < 8.0);
        CHECK(allFinite(y));
        CHECK(rms(y, size_t(0.6 * kSr), size_t(0.6 * kSr)) > 0.15f);
    }
}

TEST_CASE("pitch shifter: formant preservation keeps the spectral envelope") {
    // A harmonic tone with a strong resonance near 1 kHz. Shifting pitch up
    // an octave with formants preserved keeps the energy peak near 1 kHz;
    // without preservation the peak moves to ~2 kHz.
    const double f0 = 125.0;
    std::vector<float> x(size_t(kSr * 1.5), 0.0f);
    for (int h = 1; f0 * h < 5000; ++h) {
        const double f = f0 * h;
        const double env = 1.0 / (1.0 + std::pow((f - 1000.0) / 150.0, 2.0)) + 0.02;
        for (size_t i = 0; i < x.size(); ++i) x[i] += static_cast<float>(0.3 * env * std::sin(2 * kPiD * f * i / kSr));
    }
    auto centroidNear = [&](const std::vector<float>& y) {
        double best = 0, bestA = 0;
        for (double f = 400; f <= 3000; f += 25) {
            const double a = toneAmplitude(y, size_t(0.6 * kSr), size_t(0.6 * kSr), f, kSr);
            if (a > bestA) {
                bestA = a;
                best = f;
            }
        }
        return best;
    };
    auto e1 = make("pitch");
    e1->setParam("semitones", 12.0f);
    e1->setParam("preserve", 1.0f);
    auto e2 = make("pitch");
    e2->setParam("semitones", 12.0f);
    e2->setParam("preserve", 0.0f);
    const double kept = centroidNear(runMono(*e1, x));
    const double moved = centroidNear(runMono(*e2, x));
    CHECK(kept < 1400.0);
    CHECK(moved > 1600.0);
}

TEST_CASE("granular pitch shifter: octave up") {
    auto e = make("pitch_lite");
    e->setParam("semitones", 12.0f);
    auto y = runMono(*e, sine(300, kSr, size_t(kSr), 0.4f));
    const double got = dominantFrequency(y.data() + 24000, 19200, kSr, 200, 1200);
    CHECK(std::abs(cents(got, 600.0)) < 15.0);
}

TEST_CASE("autotune: hard mode snaps an off-key tone to the nearest scale note") {
    auto e = make("autotune");
    e->setParam("style", 1.0f);  // hard
    e->setParam("key", 0.0f);    // C
    e->setParam("scale", 1.0f);  // major: A is allowed, A# is not
    e->setParam("range", 3.0f);
    // 460 Hz is 77 cents above A4 -> nearest C-major note is A4 (440 Hz).
    auto x = harmonicVoice(460.0, 2.0, 0.25f);
    auto y = runMono(*e, x);
    const double got = dominantFrequency(y.data() + size_t(1.0 * kSr), size_t(0.6 * kSr), kSr, 300, 700);
    CHECK(std::abs(cents(got, 440.0)) < 12.0);
    CHECK(e->vis[1].load() == doctest::Approx(440.0f).epsilon(0.001)); // target note
    CHECK(allFinite(y));
}

TEST_CASE("autotune: chromatic mode corrects a slightly flat note, strength 0 leaves it") {
    auto x = harmonicVoice(220.0 * std::pow(2.0, -0.35 / 12.0), 2.0, 0.25f); // A3 - 35 cents
    auto hard = make("autotune");
    hard->setParam("style", 1.0f);
    hard->setParam("scale", 0.0f);
    auto y = runMono(*hard, x);
    CHECK(std::abs(cents(dominantFrequency(y.data() + 48000, 28800, kSr, 150, 400), 220.0)) < 12.0);

    auto off = make("autotune");
    off->setParam("style", 2.0f); // custom
    off->setParam("strength", 0.0f);
    auto z = runMono(*off, x);
    const double orig = 220.0 * std::pow(2.0, -0.35 / 12.0);
    CHECK(std::abs(cents(dominantFrequency(z.data() + 48000, 28800, kSr, 150, 400), orig)) < 8.0);
}

TEST_CASE("ring modulator, bitcrusher and character voices modify the signal") {
    auto x = sine(1000, kSr, 48000, 0.5f);
    {
        auto e = make("ringmod");
        e->setParam("freq", 200.0f);
        auto y = runMono(*e, x);
        CHECK(levelDb(y, 1200) > -13.0); // sidebands (0.5 * 0.5 = -12 dB each)
        CHECK(levelDb(y, 1000) < -40.0); // carrier suppressed
    }
    {
        auto e = make("bitcrusher");
        e->setParam("bits", 4.0f);
        e->setParam("downsample", 1.0f);
        auto y = runMono(*e, x);
        // Quantised to 4 bits: a grid of 1/8 steps.
        bool grid = true;
        for (size_t i = 1000; i < 2000; ++i) grid &= std::fabs(y[i] * 8.0f - std::round(y[i] * 8.0f)) < 1e-4f;
        CHECK(grid);
    }
    {
        auto e = make("character"); // telephone band
        auto lo = runMono(*e, sine(100, kSr, 48000, 0.3f));
        e->resetState();
        auto mid = runMono(*e, sine(1000, kSr, 48000, 0.3f));
        CHECK(levelDb(lo, 100) < levelDb(mid, 1000) - 15.0);
    }
}

TEST_CASE("robot voice: output pitch follows the robot pitch parameter") {
    auto e = make("robot");
    e->setParam("pitch", 150.0f);
    auto y = runMono(*e, harmonicVoice(230.0, 1.5, 0.3f));
    const double f = dominantFrequency(y.data() + 24000, 24000, kSr, 100, 400);
    // Robotisation produces partials at multiples of the frame rate.
    const double ratio = f / 150.0;
    CHECK(std::abs(ratio - std::round(ratio)) < 0.05);
    CHECK(allFinite(y));
}

TEST_CASE("fuzz: every effect stays finite and bounded under random parameters") {
    std::mt19937 rng(1234);
    std::uniform_real_distribution<float> u01(0.0f, 1.0f);
    std::normal_distribution<float> nd(0.0f, 0.2f);
    for (const auto& reg : EffectRegistry::instance().all()) {
        if (std::string(reg.info->typeId) == "ai_voice") continue; // needs a model; tested separately
        CAPTURE(reg.info->typeId);
        for (int trial = 0; trial < 4; ++trial) {
            auto e = reg.create();
            e->prepare(kSr, kBlock);
            for (int i = 0; i < e->numParams(); ++i) {
                const ParamInfo& p = e->paramInfo(i);
                e->setParam(i, p.min + (p.max - p.min) * u01(rng));
            }
            std::vector<float> l(size_t(kSr * 1.0)), r(l.size());
            auto s = sine(220.0 + trial * 97, kSr, l.size(), 0.5f);
            for (size_t i = 0; i < l.size(); ++i) {
                l[i] = s[i] + nd(rng);
                r[i] = s[i] * 0.7f + nd(rng);
            }
            l[1000] = 1.0f; // transient
            runStereo(*e, l, r);
            CHECK(allFinite(l));
            CHECK(allFinite(r));
            CHECK(peak(l) < 64.0f);
            CHECK(peak(r) < 64.0f);
        }
    }
}

TEST_CASE("defaults: effects at default settings pass signal (no unexpected silence)") {
    std::normal_distribution<float> nd(0.0f, 0.1f);
    std::mt19937 rng(9);
    for (const auto& reg : EffectRegistry::instance().all()) {
        if (std::string(reg.info->typeId) == "ai_voice") continue;
        CAPTURE(reg.info->typeId);
        auto e = reg.create();
        e->prepare(kSr, kBlock);
        auto x = harmonicVoice(180.0, 1.0, 0.3f);
        for (auto& v : x) v += nd(rng) * 0.1f;
        std::vector<float> l = x, r = x;
        runStereo(*e, l, r);
        CHECK(rms(l, size_t(0.5 * kSr), size_t(0.4 * kSr)) > dbToGain(-50.0f));
    }
}

TEST_CASE("presets are reproducible: identical settings give identical output") {
    std::vector<float> out[2];
    for (int k = 0; k < 2; ++k) {
        auto a = make("compressor");
        auto b = make("pitch");
        auto c = make("reverb");
        a->setParam("threshold", -24.0f);
        b->setParam("semitones", -4.0f);
        c->setParam("mix", 30.0f);
        std::vector<float> l = harmonicVoice(150.0, 1.0), r = l;
        runStereo(*a, l, r);
        runStereo(*b, l, r);
        runStereo(*c, l, r);
        out[k] = l;
    }
    CHECK(out[0] == out[1]);
}

namespace {
const ParamInfo kNanP[] = {{"x", "x", 0, 1, 0, "", ParamScale::Linear}};
const EffectInfo kNanI{"nan_test", "NaN", "Test", "", kNanP, 1, false};
struct NanEffect final : Effect {
    NanEffect() : Effect(kNanI) {}
    void onPrepare() override {}
    void processImpl(float* const* ch, int numCh, int n) override {
        for (int c = 0; c < numCh; ++c) ch[c][n / 2] = std::nanf("");
    }
};
} // namespace

TEST_CASE("chain runner: bypass crossfades and isolates a faulty effect") {
    NanEffect bad;
    bad.prepare(kSr, kBlock);
    auto gain = make("gain");
    gain->setParam("gain", -6.0f);
    ChainSnap chain;
    chain.fx[0] = &bad;
    chain.fx[1] = gain.get();
    chain.count = 2;
    auto runner = std::make_unique<ChainRunner>();
    runner->prepare(kBlock);
    std::vector<float> l = sine(500, kSr, 48000, 0.5f), r = l;
    for (size_t pos = 0; pos < l.size(); pos += kBlock) {
        bool mono = false;
        runner->process(chain, l.data() + pos, r.data() + pos, mono, kBlock);
    }
    CHECK(allFinite(l));
    CHECK(bad.faulted.load());
    CHECK(levelDb(l, 500, 0.5, 0.4) == doctest::Approx(gainToDb(0.5f) - 6.0).epsilon(0.01));

    // Toggling bypass must not create a discontinuity larger than the sine slope.
    gain->bypass = true;
    std::vector<float> m = sine(500, kSr, 9600, 0.5f), m2 = m;
    for (size_t pos = 0; pos < m.size(); pos += kBlock) {
        bool mono = false;
        runner->process(chain, m.data() + pos, m2.data() + pos, mono, kBlock);
    }
    float maxStep = 0;
    for (size_t i = 1; i < m.size(); ++i) maxStep = std::max(maxStep, std::fabs(m[i] - m[i - 1]));
    CHECK(maxStep < 0.5f * 2 * kPi * 500 / kSr * 1.1f);
}
