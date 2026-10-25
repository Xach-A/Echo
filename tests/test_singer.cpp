// Studio Singer: the preset's chain on synthetic singing (harmonic tones with
// a pitch contour): pitch correction in a key (sustained, melody, vibrato,
// consonants and breaths, quiet and loud, a noisy microphone, silence first),
// the harmonizer in key, the echo in time with a tempo, and the Voices
// screen's controls. Pitch is measured with Echo's YIN tracker on the output.
#include "doctest/doctest.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <memory>
#include <random>
#include <vector>

#include "TestUtil.h"
#include "dsp/Effect.h"
#include "dsp/EffectRegistry.h"
#include "dsp/MusicTheory.h"
#include "dsp/PitchDetector.h"
#include "dsp/PitchDisplay.h"
#include "session/Presets.h"
#include "session/Serialization.h"
#include "session/StudioSinger.h"

using namespace echo;
using namespace echo::test;

namespace {

constexpr double kSr = 48000.0;
constexpr int kBlock = 480;

// A sung tone: harmonics (1/h, up to 6 kHz) of a pitch following f0(t), at
// `amp` times an envelope.
std::vector<float> sing(double seconds, const std::function<double(double)>& f0, float amp = 0.25f,
                        const std::function<double(double)>& env = nullptr) {
    std::vector<float> v(static_cast<size_t>(seconds * kSr), 0.0f);
    double ph = 0.0;
    for (size_t i = 0; i < v.size(); ++i) {
        const double t = i / kSr, f = f0(t);
        ph += f / kSr;
        ph -= std::floor(ph);
        double s = 0.0;
        for (int h = 1; h <= 40 && f * h < 6000.0; ++h) s += std::sin(2.0 * kPiD * h * ph) / h;
        v[i] = static_cast<float>(amp * 0.6 * s * (env ? env(t) : 1.0));
    }
    return v;
}

double centsOff(double hz, double ref) { return 1200.0 * std::log2(hz / ref); }

// Pitch every 5 ms (0 = unvoiced); frame k covers up to sample (k + 1) * 240.
std::vector<double> track(const std::vector<float>& y) {
    PitchDetector d;
    d.prepare(kSr, 70.0, 900.0);
    std::vector<double> f;
    for (size_t i = 0; i < y.size(); ++i) {
        d.push(y[i]);
        if ((i + 1) % 240 == 0) {
            const PitchDetector::Result r = d.analyze();
            f.push_back(r.voiced ? r.hz : 0.0);
        }
    }
    return f;
}

// Cents of the voiced frames between two times (s) from `ref`.
std::vector<double> centsBetween(const std::vector<double>& f, double from, double to, double ref) {
    std::vector<double> c;
    for (size_t k = static_cast<size_t>(from * 200.0); k < f.size() && k < static_cast<size_t>(to * 200.0); ++k)
        if (f[k] > 0.0) c.push_back(centsOff(f[k], ref));
    return c;
}

double median(std::vector<double> v) {
    if (v.empty()) return 1e9;
    std::sort(v.begin(), v.end());
    return v[v.size() / 2];
}

double stddev(const std::vector<double>& v) {
    if (v.size() < 2) return 0.0;
    double m = 0.0, s = 0.0;
    for (double x : v) m += x;
    m /= v.size();
    for (double x : v) s += (x - m) * (x - m);
    return std::sqrt(s / (v.size() - 1));
}

double rmsDb(const std::vector<float>& y, double from, double to) {
    const size_t a = static_cast<size_t>(from * kSr), b = std::min(y.size(), static_cast<size_t>(to * kSr));
    double s = 0.0;
    for (size_t i = a; i < b; ++i) s += static_cast<double>(y[i]) * y[i];
    return 10.0 * std::log10(s / std::max<size_t>(1, b - a) + 1e-20);
}

const ChainPreset& singerPreset() {
    static const std::vector<ChainPreset> all = factoryChainPresets();
    for (const auto& p : all)
        if (p.name == "Studio Singer") return p;
    FAIL("no Studio Singer preset");
    return all.front();
}

// One of the preset's effects on its own, with the preset's settings.
std::shared_ptr<Effect> presetEffect(const char* type) {
    for (const auto& j : singerPreset().chain)
        if (j.value("type", "") == type) {
            auto e = effectFromJson(j, nullptr);
            e->prepare(kSr, kBlock);
            return e;
        }
    return nullptr;
}

std::vector<float> runMono(Effect& e, std::vector<float> x) {
    for (size_t pos = 0; pos < x.size(); pos += kBlock) {
        const int n = static_cast<int>(std::min<size_t>(kBlock, x.size() - pos));
        float* ch[2] = {x.data() + pos, nullptr};
        e.process(ch, 1, n);
    }
    return x;
}

// The whole preset as an input runs it: a mono microphone, widened to stereo
// by the first effect that needs it.
struct Chain {
    std::vector<std::shared_ptr<Effect>> fx;
    ChainSnap snap;
    ChainRunner runner;
    Chain() {
        fx = chainFromJson(singerPreset().chain, nullptr);
        for (auto& e : fx) e->prepare(kSr, kBlock);
        snap.count = static_cast<int>(fx.size());
        for (size_t i = 0; i < fx.size(); ++i) snap.fx[i] = fx[i].get();
        runner.prepare(kBlock);
    }
    Effect* find(const char* type, int nth = 0) {
        for (auto& e : fx)
            if (std::string(e->typeId()) == type && nth-- == 0) return e.get();
        return nullptr;
    }
    int latency() const {
        int sum = 0;
        for (const auto& e : fx)
            if (!e->bypass.load()) sum += e->latencySamples();
        return sum;
    }
    void run(const std::vector<float>& x, std::vector<float>& l, std::vector<float>& r) {
        l = x;
        r = x;
        for (size_t pos = 0; pos < x.size(); pos += kBlock) {
            const int n = static_cast<int>(std::min<size_t>(kBlock, x.size() - pos));
            bool mono = true;
            runner.process(snap, l.data() + pos, r.data() + pos, mono, n);
            if (mono) std::copy_n(l.data() + pos, n, r.data() + pos);
        }
    }
};

} // namespace

TEST_CASE("studio singer: the preset builds its whole chain, in range, with little delay") {
    const ChainPreset& p = singerPreset();
    CHECK(p.category == "Pitch correction");
    std::vector<std::string> types;
    for (const auto& j : p.chain) {
        const std::string type = j.value("type", "");
        types.push_back(type);
        auto e = EffectRegistry::instance().create(type);
        REQUIRE(e);
        for (auto it = j["params"].begin(); it != j["params"].end(); ++it) {
            CAPTURE(type);
            CAPTURE(it.key());
            const int i = e->paramIndex(it.key());
            REQUIRE(i >= 0);
            const float v = it->get<float>();
            CHECK(v >= e->paramInfo(i).min);
            CHECK(v <= e->paramInfo(i).max);
        }
    }
    const std::vector<std::string> expected = {"filter", "eq", "autotune", "compressor", "eq", "distortion", "deesser",
                                               "doubler", "harmonizer", "delay", "reverb", "gain", "limiter"};
    CHECK(types == expected);
    Chain c;
    CHECK(c.find("harmonizer")->bypass.load());  // harmony: off until chosen
    CHECK_FALSE(c.find("doubler")->bypass.load()); // doubling: on, quietly
    const double ms = c.latency() * 1000.0 / kSr;
    MESSAGE("Studio Singer's delay (latency of its effects): " << ms << " ms");
    CHECK(ms < 60.0);
}

TEST_CASE("studio singer: a sustained, slightly sharp note lands on the note") {
    // A4 + 30 cents, drifting by 5 cents.
    auto x = sing(2.0, [](double t) { return 440.0 * std::pow(2.0, (30.0 + 5.0 * std::sin(2.0 * kPiD * 0.7 * t)) / 1200.0); });
    auto at = presetEffect("autotune");
    const double lat = at->latencySamples() / kSr;
    const auto f = track(runMono(*at, x));
    const double m = median(centsBetween(f, 0.6 + lat, 1.9, 440.0));
    MESSAGE("sustained note: input +30 cents, output " << m << " cents from A4");
    CHECK(std::abs(m) < 8.0);
    // Through the whole chain (doubling, echo and reverb mixed in).
    Chain c;
    std::vector<float> l, r;
    c.run(x, l, r);
    const double got = dominantFrequency(l.data() + static_cast<size_t>(1.0 * kSr), static_cast<size_t>(0.8 * kSr), kSr, 300, 600);
    CHECK(std::abs(centsOff(got, 440.0)) < 10.0);
    CHECK(allFinite(l));
    CHECK(allFinite(r));
}

TEST_CASE("studio singer: a melody's notes land on the key's notes, quickly") {
    // C major: C4 D4 E4 G4 A4, each sung a little off, gliding 30 ms between.
    const double notes[] = {60, 62, 64, 67, 69}, off[] = {25, -30, 20, -15, 28};
    const double len = 0.45;
    auto pitchAt = [&](double t) {
        const int k = std::min(4, static_cast<int>(t / len));
        const double into = t - k * len;
        double midi = notes[k] + off[k] / 100.0;
        if (k > 0 && into < 0.03) {
            const double prev = notes[k - 1] + off[k - 1] / 100.0;
            midi = prev + (midi - prev) * into / 0.03;
        }
        return music::midiToHz(midi);
    };
    auto x = sing(5 * len, pitchAt);
    auto at = presetEffect("autotune");
    singer::setKey(*at, nullptr, 0, music::Major);
    const double lat = at->latencySamples() / kSr;
    const auto f = track(runMono(*at, x));
    for (int k = 0; k < 5; ++k) {
        const double onset = k * len + lat, target = music::midiToHz(notes[k]);
        const double m = median(centsBetween(f, onset + 0.12, onset + len - 0.03, target));
        // When it gets within 15 cents of the note after the onset.
        double reach = -1.0;
        for (size_t i = static_cast<size_t>(onset * 200.0); i < f.size() && i < static_cast<size_t>((onset + len) * 200.0); ++i)
            if (f[i] > 0.0 && std::abs(centsOff(f[i], target)) < 15.0) {
                reach = i / 200.0 - onset;
                break;
            }
        MESSAGE("note " << k << ": sung " << off[k] << " cents off, output " << m << " cents; on the note after " << reach * 1000.0
                        << " ms (tracker included)");
        CHECK(std::abs(m) < 10.0);
        CHECK(reach >= 0.0);
        CHECK(reach < 0.12);
    }
}

TEST_CASE("studio singer: held notes keep their vibrato with Humanize; Robotic flattens it") {
    // A4 with a 5.5 Hz vibrato of +-50 cents, centred 10 cents sharp.
    auto x = sing(2.6, [](double t) { return 440.0 * std::pow(2.0, (10.0 + 50.0 * std::sin(2.0 * kPiD * 5.5 * t)) / 1200.0); });
    auto natural = presetEffect("autotune"); // the preset: quick retune, Humanize 35 %
    const double lat = natural->latencySamples() / kSr;
    const auto keep = centsBetween(track(runMono(*natural, x)), 1.2 + lat, 2.5, 440.0);
    auto robotic = presetEffect("autotune");
    singer::setTuneFeel(*robotic, 100.0f);
    robotic->setParam("humanize", 0.0f);
    const auto flat = centsBetween(track(runMono(*robotic, x)), 1.2 + lat, 2.5, 440.0);
    const auto in = centsBetween(track(x), 1.2, 2.5, 440.0);
    MESSAGE("vibrato (spread of the pitch): sung " << stddev(in) << " cents, Studio Singer " << stddev(keep) << ", Robotic "
                                                  << stddev(flat) << "; centre " << median(keep) << " / " << median(flat) << " cents");
    CHECK(stddev(keep) > 0.5 * stddev(in));
    CHECK(stddev(flat) < 0.35 * stddev(in));
    CHECK(std::abs(median(keep)) < 12.0);
    CHECK(std::abs(median(flat)) < 8.0);
}

TEST_CASE("studio singer: consonants and breaths pass through untuned") {
    // Note, "t" (white noise, 60 ms), note, "s" (noise above 4 kHz, 150 ms),
    // note, breath (quiet noise, 300 ms).
    std::mt19937 rng(4);
    std::normal_distribution<float> nd(0.0f, 1.0f);
    auto note = sing(0.3, [](double) { return 330.0; });
    std::vector<float> x;
    std::vector<std::pair<size_t, size_t>> noise;
    auto addNoise = [&](double secs, float level, bool high) {
        const size_t a = x.size();
        float prev = 0.0f;
        for (size_t i = 0; i < static_cast<size_t>(secs * kSr); ++i) {
            float v = nd(rng) * level;
            if (high) {
                const float d = v - prev; // a crude high-pass: differences
                prev = v;
                v = d * 0.7f;
            }
            x.push_back(v);
        }
        noise.emplace_back(a, x.size());
    };
    x.insert(x.end(), note.begin(), note.end());
    addNoise(0.06, 0.05f, false);
    x.insert(x.end(), note.begin(), note.end());
    addNoise(0.15, 0.04f, true);
    x.insert(x.end(), note.begin(), note.end());
    addNoise(0.3, 0.006f, false);
    x.insert(x.end(), 9600, 0.0f);
    auto at = presetEffect("autotune");
    auto* display = dynamic_cast<PitchDisplaySource*>(at.get());
    REQUIRE(display);
    const size_t lat = static_cast<size_t>(at->latencySamples());
    std::vector<float> y = x;
    std::vector<int> heardNote(y.size() / kBlock + 1, 0); // per block: frames that steered to a note
    for (size_t pos = 0; pos < y.size(); pos += kBlock) {
        const int n = static_cast<int>(std::min<size_t>(kBlock, y.size() - pos));
        float* ch[2] = {y.data() + pos, nullptr};
        at->process(ch, 1, n);
        PitchFrame fr;
        while (display->pitchHistory().read(&fr, 1) == 1) heardNote[pos / kBlock] += fr.targetHz > 0.0f;
    }
    // Inside each noise (away from the notes): no note steered to, and the
    // output keeps its level and brightness (unvoiced audio is re-cut into
    // overlapping slices, not retuned, so it isn't sample for sample the same).
    auto brightness = [](const float* v, size_t n) {
        double d = 0.0, e = 0.0;
        for (size_t i = 1; i < n; ++i) {
            d += (static_cast<double>(v[i]) - v[i - 1]) * (static_cast<double>(v[i]) - v[i - 1]);
            e += static_cast<double>(v[i]) * v[i];
        }
        return d / (e + 1e-30);
    };
    for (const auto& [a, b] : noise) {
        const size_t from = a + 960, to = b - 960;
        if (to <= from) continue;
        int notes = 0;
        // (Pitch frames come as the input is analysed: at input time.)
        for (size_t blk = from / kBlock + 1; blk < to / kBlock; ++blk) notes += heardNote[blk];
        const double level = gainToDb(rms(y.data() + from + lat, to - from)) - gainToDb(rms(x.data() + from, to - from));
        const double bright = brightness(y.data() + from + lat, to - from) / brightness(x.data() + from, to - from);
        MESSAGE("noise at " << a / kSr << " s: " << notes << " frames heard a note; level " << level << " dB, brightness x" << bright);
        CHECK(notes == 0);
        CHECK(std::abs(level) < 2.0);
        CHECK(bright > 0.8);
        CHECK(bright < 1.25);
    }
    CHECK(allFinite(y));
}

TEST_CASE("studio singer: quiet singing then a loud phrase comes out more even, under the ceiling") {
    // 1.5 s at a quiet level, then 1.5 s 24 dB louder (near full scale).
    auto x = sing(3.0, [](double t) { return t < 1.5 ? 392.0 : 440.0; }, 0.45f,
                  [](double t) { return t < 1.5 ? std::pow(10.0, -24.0 / 20.0) : 1.0; });
    Chain c;
    std::vector<float> l, r;
    c.run(x, l, r);
    const double inQuiet = rmsDb(x, 0.6, 1.4), inLoud = rmsDb(x, 2.1, 2.9);
    const double outQuiet = rmsDb(l, 0.7, 1.45), outLoud = rmsDb(l, 2.2, 2.95);
    MESSAGE("quiet to loud: input " << inLoud - inQuiet << " dB apart, output " << outLoud - outQuiet << " dB (levels "
                                    << outQuiet << " / " << outLoud << " dBFS); peak " << gainToDb(peak(l)) << " dBFS");
    CHECK(outLoud - outQuiet < inLoud - inQuiet - 6.0); // evened out
    CHECK(outLoud - outQuiet > 8.0);                    // not flattened
    CHECK(peak(l) < dbToGain(-0.9f));
    CHECK(peak(r) < dbToGain(-0.9f));
}

TEST_CASE("studio singer: on a noisy microphone the voice is tuned and the noise isn't turned into notes") {
    // Room noise at -40 dBFS throughout; singing (+25 cents) from 1 s on.
    std::mt19937 rng(11);
    std::normal_distribution<float> nd(0.0f, 0.01f);
    auto x = sing(2.8, [](double) { return 392.0 * std::pow(2.0, 25.0 / 1200.0); }, 0.25f, [](double t) { return t < 1.0 ? 0.0 : 1.0; });
    for (float& v : x) v += nd(rng);
    auto at = presetEffect("autotune");
    auto* display = dynamic_cast<PitchDisplaySource*>(at.get());
    REQUIRE(display);
    int noiseFrames = 0, noteFrames = 0;
    std::vector<float> y = x;
    for (size_t pos = 0; pos < y.size(); pos += kBlock) {
        float* ch[2] = {y.data() + pos, nullptr};
        at->process(ch, 1, kBlock);
        PitchFrame fr;
        while (display->pitchHistory().read(&fr, 1) == 1)
            if (pos < static_cast<size_t>(0.95 * kSr)) {
                ++noiseFrames;
                noteFrames += fr.targetHz > 0.0f;
            }
    }
    MESSAGE("noise alone: " << noteFrames << " of " << noiseFrames << " pitch frames heard a note");
    CHECK(noiseFrames > 100);
    CHECK(noteFrames <= noiseFrames / 20);
    const double lat = at->latencySamples() / kSr;
    const double m = median(centsBetween(track(y), 1.5 + lat, 2.7, 392.0));
    MESSAGE("singing over the noise: " << m << " cents from G4");
    CHECK(std::abs(m) < 10.0);
}

TEST_CASE("studio singer: silence stays silent, then singing starts on the note") {
    auto x = sing(2.5, [](double) { return 440.0 * std::pow(2.0, 20.0 / 1200.0); }, 0.25f, [](double t) { return t < 1.0 ? 0.0 : 1.0; });
    Chain c;
    std::vector<float> l, r;
    c.run(x, l, r);
    CHECK(peak(l, 0, static_cast<size_t>(0.95 * kSr)) == 0.0f); // nothing made of silence
    CHECK(peak(r, 0, static_cast<size_t>(0.95 * kSr)) == 0.0f);
    const double lat = c.latency() / kSr;
    const auto f = track(l);
    const double m = median(centsBetween(f, 1.25 + lat, 2.4, 440.0));
    MESSAGE("first note after silence: " << m << " cents from A4");
    CHECK(std::abs(m) < 10.0);
    CHECK(allFinite(l));
}

TEST_CASE("studio singer: the key and scale decide the note") {
    // F#4 + 20 cents: D major has F#; D minor doesn't (its nearest note is G).
    auto x = sing(1.8, [](double) { return music::midiToHz(66.2); });
    auto major = presetEffect("autotune");
    singer::setKey(*major, nullptr, 2, music::Major);
    auto minor = presetEffect("autotune");
    singer::setKey(*minor, nullptr, 2, music::NaturalMinor);
    const double lat = major->latencySamples() / kSr;
    const double inMajor = median(centsBetween(track(runMono(*major, x)), 0.6 + lat, 1.7, music::midiToHz(66)));
    const double inMinor = median(centsBetween(track(runMono(*minor, x)), 0.6 + lat, 1.7, music::midiToHz(67)));
    MESSAGE("F#4 + 20 cents: D major -> F#4 " << inMajor << " cents; D minor -> G4 " << inMinor << " cents");
    CHECK(std::abs(inMajor) < 8.0);
    CHECK(std::abs(inMinor) < 8.0);
    CHECK(music::scaleStep(69, 2, 0, music::Major) == 72); // A -> C: a minor third in C major
    CHECK(music::scaleStep(67, 2, 0, music::Major) == 71); // G -> B: a major third
    CHECK(music::scaleStep(60, -7, 0, music::Major) == 48);
    CHECK(music::scaleStep(60, 3, 0, music::Chromatic) == 63);
}

TEST_CASE("harmonizer: in key, a third above is major or minor as the key wants") {
    auto harmonyOf = [](double sungHz, bool inKey) {
        auto e = EffectRegistry::instance().create("harmonizer");
        e->prepare(kSr, kBlock);
        e->setParam("mode", inKey ? 1.0f : 0.0f);
        e->setParam("key", 0.0f);   // C
        e->setParam("scale", 1.0f); // major
        e->setParam("steps1", 2.0f);
        e->setParam("interval1", 4.0f);
        e->setParam("level1", 0.0f);
        e->setParam("level2", -60.0f);
        e->setParam("dry", -60.0f); // hear the harmony only
        auto l = sing(1.5, [=](double) { return sungHz; });
        auto r = l;
        for (size_t pos = 0; pos < l.size(); pos += kBlock) {
            float* ch[2] = {l.data() + pos, r.data() + pos};
            e->process(ch, 2, kBlock);
        }
        std::vector<float> m(l.size());
        for (size_t i = 0; i < m.size(); ++i) m[i] = l[i] + r[i];
        return dominantFrequency(m.data() + static_cast<size_t>(0.7 * kSr), static_cast<size_t>(0.7 * kSr), kSr, 300, 700);
    };
    const double a = harmonyOf(440.0, true), g = harmonyOf(392.0, true), fixed = harmonyOf(440.0, false);
    MESSAGE("in C major: A4 -> " << a << " Hz, G4 -> " << g << " Hz; fixed +4: A4 -> " << fixed << " Hz");
    CHECK(std::abs(centsOff(a, music::midiToHz(72))) < 15.0);     // C5: a minor third
    CHECK(std::abs(centsOff(g, music::midiToHz(71))) < 15.0);     // B4: a major third
    CHECK(std::abs(centsOff(fixed, music::midiToHz(73))) < 15.0); // C#5: always a major third
}

TEST_CASE("delay: synced to the tempo, a quarter note at 120 BPM is 500 ms") {
    auto echoAt = [](float sync, float bpm) {
        auto e = EffectRegistry::instance().create("delay");
        e->prepare(kSr, kBlock);
        e->setParam("sync", sync);
        e->setParam("bpm", bpm);
        e->setParam("feedback", 0.0f);
        e->setParam("mix", 50.0f);
        e->setParam("low_cut", 20.0f);
        e->setParam("high_cut", 20000.0f);
        std::vector<float> l(static_cast<size_t>(kSr), 0.0f), r;
        l[1000] = 1.0f;
        r = l;
        for (size_t pos = 0; pos < l.size(); pos += kBlock) {
            float* ch[2] = {l.data() + pos, r.data() + pos};
            e->process(ch, 2, kBlock);
        }
        size_t best = 1100;
        for (size_t i = 1100; i < l.size(); ++i)
            if (std::fabs(l[i]) > std::fabs(l[best])) best = i;
        return (best - 1000) * 1000.0 / kSr;
    };
    CHECK(echoAt(2.0f, 120.0f) == doctest::Approx(500.0).epsilon(0.002));
    CHECK(echoAt(3.0f, 100.0f) == doctest::Approx(450.0).epsilon(0.002)); // dotted 1/8 at 100 BPM
    CHECK(echoAt(0.0f, 120.0f) == doctest::Approx(320.0).epsilon(0.002)); // off: the Time setting
}

TEST_CASE("studio singer: the panel's controls set and read back their effects; saturation keeps the level") {
    Chain c;
    Effect* at = c.find("autotune");
    Effect* tone = c.find("eq", 1);
    Effect* comp = c.find("compressor");
    Effect* sat = c.find("distortion");
    Effect* harm = c.find("harmonizer");
    REQUIRE(at);
    REQUIRE(tone);
    REQUIRE(comp);
    REQUIRE(sat);
    REQUIRE(harm);
    // The preset's own settings read as the controls' middle values.
    CHECK(singer::tuneFeel(*at) == doctest::Approx(70.0f).epsilon(0.01));
    CHECK(std::string(singer::tuneFeelName(singer::tuneFeel(*at))) == "Polished");
    CHECK(singer::tone(*tone) == doctest::Approx(0.0f));
    CHECK(singer::compression(*comp) == doctest::Approx(50.0f));
    CHECK(singer::saturation(*sat) == doctest::Approx(33.3f).epsilon(0.01));
    CHECK(singer::harmony(*harm) == singer::Harmony::Off);
    // Set and read back.
    singer::setTuneFeel(*at, 100.0f);
    CHECK(at->param(at->paramIndex("speed")) == 0.0f);
    CHECK(std::string(singer::tuneFeelName(singer::tuneFeel(*at))) == "Robotic");
    singer::setTone(*tone, 60.0f);
    CHECK(singer::tone(*tone) == doctest::Approx(60.0f));
    singer::setCompression(*comp, 80.0f);
    CHECK(singer::compression(*comp) == doctest::Approx(80.0f));
    singer::setHarmony(*harm, singer::Harmony::ThirdAndFifth, 2, music::NaturalMinor);
    CHECK_FALSE(harm->bypass.load());
    CHECK(singer::harmony(*harm) == singer::Harmony::ThirdAndFifth);
    CHECK(harm->param(harm->paramIndex("mode")) == 1.0f); // in the key
    singer::setKey(*at, harm, 7, music::Chromatic);        // no key: fixed intervals
    CHECK(harm->param(harm->paramIndex("mode")) == 0.0f);
    CHECK(singer::harmony(*harm) == singer::Harmony::ThirdAndFifth);
    singer::setHarmony(*harm, singer::Harmony::OctaveBelow, 7, music::Chromatic);
    CHECK(singer::harmony(*harm) == singer::Harmony::OctaveBelow);
    singer::setHarmony(*harm, singer::Harmony::Off, 7, music::Chromatic);
    CHECK(harm->bypass.load());

    // Saturation from none to full: about as loud (on a sung phrase).
    auto x = sing(2.0, [](double t) { return music::midiToHz(t < 1.0 ? 64 : 67); });
    auto level = [&](float amount) {
        Chain k;
        singer::setSaturation(*k.find("distortion"), amount);
        std::vector<float> l, r;
        k.run(x, l, r);
        return rmsDb(l, 0.4, 1.9);
    };
    const double none = level(0.0f), preset = level(33.3f), full = level(100.0f);
    MESSAGE("level with saturation 0 / 33 (preset) / 100: " << none << " / " << preset << " / " << full << " dBFS");
    CHECK(std::abs(preset - none) < 1.5);
    CHECK(std::abs(full - none) < 1.5);
}

TEST_CASE("studio singer: a long, loud take stays finite and bounded, and its tail dies away") {
    // A melody with consonants, pushed to near full scale, then 4 s of silence.
    std::mt19937 rng(21);
    std::normal_distribution<float> nd(0.0f, 0.08f);
    auto x = sing(4.0, [](double t) { return music::midiToHz(60 + 2 * (static_cast<int>(t * 3) % 5)); }, 0.9f);
    for (size_t i = 0; i < x.size(); ++i)
        if (i % 24000 < 2400) x[i] = nd(rng); // a burst of noise every half second
    x.insert(x.end(), static_cast<size_t>(4.0 * kSr), 0.0f);
    Chain c;
    std::vector<float> l, r;
    c.run(x, l, r);
    CHECK(allFinite(l));
    CHECK(allFinite(r));
    CHECK(peak(l) < dbToGain(-0.9f));
    CHECK(peak(r) < dbToGain(-0.9f));
    const double tail = rmsDb(l, 7.0, 8.0);
    MESSAGE("3 s after the singing stops: " << tail << " dBFS");
    CHECK(tail < -70.0);
}
