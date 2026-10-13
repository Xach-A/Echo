// The "Pain" voice (blown-out microphone): it must be squared off by
// clipping, as loud as digital audio goes without exceeding full scale,
// full-band (no low-pass), keep words recognisable, keep room noise down,
// switch on and off without clicks, and survive saving.
#include "doctest/doctest.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <random>
#include <string>
#include <vector>

#include "TestUtil.h"
#include "core/Text.h"
#include "core/WavFile.h"
#include "dsp/Biquad.h"
#include "dsp/Effect.h"
#include "session/Presets.h"
#include "session/Serialization.h"

#ifndef ECHO_TEST_SPEECH_DIR
#define ECHO_TEST_SPEECH_DIR "test-speech"
#endif

using namespace echo;
using namespace echo::test;

namespace {

constexpr double kSr = 48000.0;
constexpr int kBlock = 480;
constexpr size_t kIntensity = 5, kCrush = 7; // positions in the Pain chain

const ChainPreset* findPreset(const std::vector<ChainPreset>& all, const char* name) {
    for (const auto& p : all)
        if (p.name == name) return &p;
    return nullptr;
}

const ChainPreset& pain() {
    static const auto all = factoryChainPresets();
    const ChainPreset* p = findPreset(all, "Pain");
    REQUIRE(p);
    return *p;
}

std::vector<std::shared_ptr<Effect>> chainOf(const char* name) {
    static const auto all = factoryChainPresets();
    const ChainPreset* p = findPreset(all, name);
    REQUIRE(p);
    auto chain = chainFromJson(p->chain, nullptr);
    for (auto& e : chain) e->prepare(kSr, kBlock);
    return chain;
}

// Pain with its Intensity set as the Voices screen sets it.
std::vector<std::shared_ptr<Effect>> painAt(float intensity) {
    auto c = chainOf("Pain");
    applyIntensity(*c[kIntensity], pain().chain[kIntensity], intensity);
    return c;
}

// Runs the chain like the engine does (fades, warm-up, bypass) on mono audio.
std::vector<float> run(std::vector<std::shared_ptr<Effect>>& chain, std::vector<float> x) {
    ChainSnap snap;
    for (auto& e : chain) snap.fx[snap.count++] = e.get();
    ChainRunner runner;
    runner.prepare(kBlock);
    std::vector<float> r(kBlock);
    for (size_t pos = 0; pos < x.size(); pos += kBlock) {
        const int n = static_cast<int>(std::min<size_t>(kBlock, x.size() - pos));
        bool mono = true;
        runner.process(snap, x.data() + pos, r.data(), mono, n);
    }
    return x;
}

std::vector<float> tone(double freq, float db, double seconds) {
    return sine(freq, kSr, static_cast<size_t>(kSr * seconds), dbToGain(db));
}

// A voiced-sound stand-in: a 220 Hz harmonic complex, about -21 dBFS RMS.
std::vector<float> voiced(double seconds) {
    std::vector<float> x(static_cast<size_t>(kSr * seconds));
    for (size_t i = 0; i < x.size(); ++i) {
        double v = 0.0;
        for (int h = 1; h <= 8; ++h) v += std::sin(2 * kPiD * 220.0 * h * i / kSr) / h;
        x[i] = 0.1f * static_cast<float>(v);
    }
    return x;
}

// How squared off the second half of a signal is: crest factor (dB) and
// kurtosis (speech ~6-10, a sine 1.5, a square wave 1.0).
struct Shape {
    double crestDb, kurtosis;
};
Shape shape(const std::vector<float>& y) {
    const size_t a = y.size() / 2;
    double m2 = 0, m4 = 0, pk = 0;
    for (size_t i = a; i < y.size(); ++i) {
        const double v = y[i];
        m2 += v * v, m4 += v * v * v * v, pk = std::max(pk, std::fabs(v));
    }
    const double n = double(y.size() - a);
    return {20 * std::log10(pk) - 10 * std::log10(m2 / n), (m4 / n) / ((m2 / n) * (m2 / n))};
}

std::vector<float> speech(const char* name) {
    WavData w;
    if (!readWav(utf8ToWide(std::string(ECHO_TEST_SPEECH_DIR) + "/" + name), w) || w.sampleRate != 48000 || w.channels != 1) return {};
    const float g = dbToGain(-20.0f) / std::max(1e-6f, rms(w.samples));
    for (float& v : w.samples) v *= g;
    return w.samples;
}

// 10 ms level envelope (dB) of a band-passed signal.
std::vector<double> bandEnvelope(const std::vector<float>& x, double f0, double q) {
    Biquad bp;
    bp.set(designBiquad(FilterType::BandPass, kSr, f0, q));
    std::vector<double> e;
    double acc = 0.0;
    for (size_t i = 0; i < x.size(); ++i) {
        const double y = bp.process(x[i]);
        acc += y * y;
        if ((i + 1) % 480 == 0) {
            e.push_back(10.0 * std::log10(acc / 480.0 + 1e-12));
            acc = 0.0;
        }
    }
    return e;
}

double correlation(const std::vector<double>& a, const std::vector<double>& b) {
    const size_t n = std::min(a.size(), b.size());
    double ma = 0, mb = 0;
    for (size_t i = 0; i < n; ++i) ma += a[i], mb += b[i];
    ma /= n, mb /= n;
    double sab = 0, saa = 0, sbb = 0;
    for (size_t i = 0; i < n; ++i) {
        sab += (a[i] - ma) * (b[i] - mb);
        saa += (a[i] - ma) * (a[i] - ma);
        sbb += (b[i] - mb) * (b[i] - mb);
    }
    return sab / std::sqrt(saa * sbb + 1e-12);
}

// Correlation of 10 ms level envelopes in three speech bands: whether the
// syllable and word structure survives.
double envelopeCorrelation(const std::vector<float>& a, const std::vector<float>& b) {
    const double bands[][2] = {{300, 800}, {800, 1600}, {1600, 3200}};
    double sum = 0.0;
    for (const auto& band : bands) {
        const double f0 = std::sqrt(band[0] * band[1]), q = f0 / (band[1] - band[0]);
        sum += correlation(bandEnvelope(a, f0, q), bandEnvelope(b, f0, q));
    }
    return sum / 3.0;
}

// A stricter intelligibility proxy (in the spirit of the speech transmission
// index): the envelope correlation in octave bands 250 Hz - 4 kHz, only where
// the dry voice is active in that band. Clipping everything to full scale
// lowers it.
double octaveIntelligibility(const std::vector<float>& dry, const std::vector<float>& wet) {
    double sum = 0.0;
    for (double fc : {250.0, 500.0, 1000.0, 2000.0, 4000.0}) {
        const auto a = bandEnvelope(dry, fc, 1.41), b = bandEnvelope(wet, fc, 1.41);
        const size_t n = std::min(a.size(), b.size());
        const double amax = *std::max_element(a.begin(), a.begin() + n);
        std::vector<double> A, Bv;
        for (size_t i = 0; i < n; ++i)
            if (a[i] > amax - 40) A.push_back(a[i]), Bv.push_back(b[i]);
        sum += correlation(A, Bv);
    }
    return sum / 5.0;
}

float maxStep(const std::vector<float>& x, size_t from, size_t to) {
    float m = 0.0f;
    for (size_t i = std::max<size_t>(from, 1); i < to && i < x.size(); ++i) m = std::max(m, std::fabs(x[i] - x[i - 1]));
    return m;
}

// Full scale, allowing for float rounding of the limiter's -0.1 dBFS ceiling.
const float kFullScale = dbToGain(-0.05f);

} // namespace

TEST_CASE("pain: a factory voice with the expected chain, no low-pass") {
    const ChainPreset& p = pain();
    CHECK(p.category == "Voice changer");
    CHECK_FALSE(p.description.empty());
    std::vector<std::string> warnings;
    auto chain = chainFromJson(p.chain, &warnings);
    CHECK(warnings.empty());
    std::vector<std::string> types;
    for (const auto& e : chain) types.push_back(e->typeId());
    const std::vector<std::string> expected = {"gate", "filter", "compressor", "eq", "distortion",
                                               "distortion", "eq", "bitcrusher", "distortion", "limiter"};
    CHECK(types == expected);
    // The bit-crusher is an optional, harsher variation (off by default).
    CHECK(chain[kCrush]->bypass.load());
    // Quick controls on the Voices screen: Intensity = the middle clipping
    // stage's drive, Bit-crush = the optional bit-crusher; nothing else.
    for (size_t i = 0; i < p.chain.size(); ++i)
        CHECK(p.chain[i].value("ui", "") == (i == kIntensity ? "intensity" : i == kCrush ? "optional" : ""));
    CHECK(p.chain[kIntensity].value("ui_makeup", 0.0f) == doctest::Approx(0.6f));
    // No frequency cutoff: no low-pass filter, every distortion's tone filter
    // fully open (its highest setting, 16 kHz).
    for (const auto& e : chain) {
        const std::string t = e->typeId();
        if (t == "filter") CHECK(e->param(e->paramIndex("lp_on")) == 0.0f);
        if (t == "distortion") CHECK(e->param(e->paramIndex("tone")) == doctest::Approx(e->paramInfo(e->paramIndex("tone")).max));
    }
    // The limiter only catches filter overshoot at full scale; the clipping
    // stages do the work.
    CHECK(chain.back()->param(chain.back()->paramIndex("ceiling")) >= -0.1f);
}

TEST_CASE("pain: squared off by clipping, far more than the radio-style voices") {
    auto c = chainOf("Pain");
    const Shape s = shape(run(c, voiced(2.0)));
    MESSAGE("voiced sound (dry: crest " << shape(voiced(2.0)).crestDb << " dB, kurtosis " << shape(voiced(2.0)).kurtosis
                                        << "): Pain crest " << s.crestDb << " dB, kurtosis " << s.kurtosis);
    CHECK(s.crestDb < 4.0);
    CHECK(s.kurtosis < 1.6);
    for (const char* other : {"Megaphone", "Radio Voice", "Telephone", "Walkie-Talkie"}) {
        auto o = chainOf(other);
        const Shape so = shape(run(o, voiced(2.0)));
        MESSAGE("  " << std::string(other) << ": crest " << so.crestDb << " dB, kurtosis " << so.kurtosis);
        CHECK(so.crestDb > s.crestDb + 3.0);
    }
}

TEST_CASE("pain: full-band (no low-pass), rumble under the voice cut") {
    // The level of each tone's own frequency at the output (harmonics
    // excluded). On its own, any tone is pulled up and clipped to full scale;
    // a low-pass anywhere after the clipping would show as a lower level.
    auto fundamental = [](double freq) {
        auto c = chainOf("Pain");
        const auto y = run(c, tone(freq, -18.0f, 1.0));
        return gainToDb(float(toneAmplitude(y, y.size() - 9600, 9600, freq, kSr)));
    };
    const float mid = fundamental(1000.0), high = fundamental(6000.0), top = fundamental(12000.0);
    // Rumble as it occurs: 50 Hz hum as strong as the voice's fundamental.
    auto mix = voiced(2.0);
    for (size_t i = 0; i < mix.size(); ++i) mix[i] += 0.1f * static_cast<float>(std::sin(2 * kPiD * 50.0 * i / kSr));
    auto c = chainOf("Pain");
    const auto y = run(c, mix);
    const float hum = gainToDb(float(toneAmplitude(y, y.size() - 9600, 9600, 50.0, kSr)));
    const float voice = gainToDb(float(toneAmplitude(y, y.size() - 9600, 9600, 220.0, kSr)));
    MESSAGE("-18 dBFS tones come out at: 1 kHz " << mid << ", 6 kHz " << high << ", 12 kHz " << top << " dBFS; hum under a voice: "
                                                 << hum << " dBFS vs the voice's fundamental " << voice << " dBFS (equal going in)");
    CHECK(std::fabs(high - mid) < 8.0f);
    CHECK(mid - top < 12.0f);
    CHECK(voice - hum > 20.0f);
}

TEST_CASE("pain: as loud as digital audio goes for quiet and loud voices, never above full scale") {
    // A syllable-like test signal: a 150 Hz harmonic complex switched on and off.
    std::vector<float> x(static_cast<size_t>(kSr * 4));
    for (size_t i = 0; i < x.size(); ++i) {
        const double t = i / kSr;
        double v = 0.0;
        for (int h = 1; h <= 12; ++h) v += std::sin(2 * kPiD * 150.0 * h * t) / h;
        const bool on = std::fmod(t, 0.4) < 0.25;
        x[i] = static_cast<float>(on ? 0.12 * v : 0.0);
    }
    auto at = [&](float gainDb) {
        std::vector<float> s = x;
        for (float& v : s) v *= dbToGain(gainDb);
        auto c = chainOf("Pain");
        return run(c, s);
    };
    // quiet: about -32 dBFS RMS while sounding (a quiet microphone); hot: peaks at +17 dBFS.
    const auto quiet = at(-10.0f), loud = at(10.0f), hot = at(26.0f);
    const float diff = gainToDb(rms(loud)) - gainToDb(rms(quiet));
    // From 0.1 s on: the effects fade in from the dry signal over their first
    // 10-20 ms after being switched on (see the switching test).
    const size_t settled = static_cast<size_t>(kSr * 0.1);
    const float hotPeak = peak(std::vector<float>(hot.begin() + settled, hot.end()));
    MESSAGE("quiet input -> " << gainToDb(rms(quiet)) << " dBFS RMS (with the gaps), +20 dB input -> output +" << diff
                              << " dB; peak with +17 dBFS input peaks " << gainToDb(hotPeak) << " dBFS");
    CHECK(gainToDb(rms(quiet)) > -10.0f);
    CHECK(diff < 3.0f);
    CHECK(hotPeak <= kFullScale);
    CHECK(allFinite(hot));
}

TEST_CASE("pain: room noise between words is not blown up, even at full Intensity") {
    std::mt19937 rng(7);
    std::normal_distribution<float> nd(0.0f, dbToGain(-65.0f));
    std::vector<float> noise(static_cast<size_t>(kSr * 2));
    for (float& v : noise) v = nd(rng);
    for (float intensity : {pain().chain[kIntensity]["params"].value("drive", 0.0f), 48.0f}) {
        auto c = painAt(intensity);
        const auto y = run(c, noise);
        const float out = gainToDb(rms(y, y.size() / 2, y.size() / 2));
        MESSAGE("intensity +" << intensity << " dB: -65 dBFS noise in -> " << out << " dBFS out");
        CHECK(out < -60.0f);
    }
}

TEST_CASE("pain: Intensity changes how hard it clips, not much how loud; never above full scale") {
    const float own = pain().chain[kIntensity]["params"].value("drive", 0.0f);
    auto level = [](const std::vector<float>& y) { return gainToDb(rms(y, y.size() / 2, y.size() / 2)); };
    auto ownChain = painAt(own);
    const float ownLevel = level(run(ownChain, voiced(2.0)));
    double prevKurt = 1e9, firstKurt = 0.0;
    for (float drive : {0.0f, 10.0f, own, 34.0f, 48.0f}) {
        auto c = painAt(drive);
        const auto y = run(c, voiced(2.0));
        auto h = painAt(drive);
        const auto hot = run(h, tone(220.0, -6.0f, 1.0));
        const Shape s = shape(y);
        MESSAGE("intensity +" << drive << " dB: level " << level(y) << " dBFS (own setting " << ownLevel << "), crest " << s.crestDb
                              << " dB, kurtosis " << s.kurtosis << ", peak with a hot input " << gainToDb(peak(hot)) << " dBFS");
        // Without the output compensation, Intensity 0 would be about 15 dB
        // quieter; with it, about 4 dB on this tone (about 3 LU on speech).
        CHECK(std::fabs(level(y) - ownLevel) < 5.0f);
        CHECK(s.kurtosis <= prevKurt + 0.05); // more Intensity, more squared off
        CHECK(peak(hot) <= kFullScale);
        if (drive == 0.0f) firstKurt = s.kurtosis;
        prevKurt = s.kurtosis;
    }
    CHECK(prevKurt < firstKurt - 0.2);
}

TEST_CASE("pain: words stay recognisable") {
    const auto s = speech("speech_a.wav");
    if (s.empty()) {
        MESSAGE("SKIPPED: run tools/make_speech_samples.ps1 to generate speech test material");
        return;
    }
    auto c = chainOf("Pain");
    const auto y = run(c, s);
    auto c0 = painAt(0.0f), c48 = painAt(48.0f);
    const auto y0 = run(c0, s), y48 = run(c48, s);
    const double corr = envelopeCorrelation(s, y), strict = octaveIntelligibility(s, y);
    const double strict0 = octaveIntelligibility(s, y0), strict48 = octaveIntelligibility(s, y48);
    MESSAGE("speech: band-envelope correlation " << corr << ", octave-band intelligibility proxy " << strict << " (Intensity 0: "
                                                 << strict0 << ", 48: " << strict48 << "; dry 1.0), output " << gainToDb(rms(y))
                                                 << " dBFS RMS from -20");
    CHECK(corr > 0.9);
    CHECK(strict > 0.6);
    CHECK(strict0 > strict48); // turning Intensity down makes it clearer
    CHECK(peak(y) <= kFullScale);
    CHECK(gainToDb(rms(y)) > -8.0f); // loud: about 15 dB louder than the voice going in
}

TEST_CASE("pain: switching it on, off and on again does not click or jump in level") {
    auto c = chainOf("Pain");
    ChainSnap snap;
    for (auto& e : c) snap.fx[snap.count++] = e.get();
    ChainRunner runner;
    runner.prepare(kBlock);
    const auto src = voiced(4.0);
    std::vector<float> dry, out, block(kBlock), r(kBlock);
    for (int b = 0; b < 400; ++b) {
        // Off (dry) for the first second; on at 1 s (as when picking the
        // voice: a fresh chain), off at 2 s (another voice), on again at 3 s.
        const bool on = b >= 100 && (b < 200 || b >= 300);
        for (auto& e : c)
            if (std::string(e->typeId()) != "bitcrusher") e->bypass = !on;
        std::copy(src.begin() + size_t(b) * kBlock, src.begin() + size_t(b + 1) * kBlock, block.begin());
        dry.insert(dry.end(), block.begin(), block.end());
        bool mono = true;
        runner.process(snap, block.data(), r.data(), mono, kBlock);
        out.insert(out.end(), block.begin(), block.end());
    }
    const size_t B = kBlock;
    // The steepest step of the dry and of the distorted signal, against the
    // steps around each switch. Switching instantly between the two would
    // jump by up to their combined amplitude.
    const float dryStep = maxStep(out, 50 * B, 95 * B), painStep = maxStep(out, 150 * B, 195 * B);
    const float steady = std::max(dryStep, painStep);
    float instant = 0.0f;
    for (size_t i = 150 * B; i < 195 * B; ++i) instant = std::max(instant, std::fabs(out[i] - dry[i]));
    const float on1 = maxStep(out, 99 * B, 110 * B), off = maxStep(out, 199 * B, 210 * B), on2 = maxStep(out, 299 * B, 310 * B);
    MESSAGE("largest sample step: dry " << dryStep << ", Pain " << painStep << "; switching on " << on1 << ", off " << off
                                        << ", on again " << on2 << " (an instant switch: up to " << instant << ")");
    CHECK(on1 <= steady * 1.25f);
    CHECK(off <= steady * 1.25f);
    CHECK(on2 <= steady * 1.25f);
    CHECK(instant > steady * 2.0f); // the test would see a hard switch

    // No level spike while the effects fade in or out: nothing more than 1 dB
    // above the dry or the steady distorted signal, and never above full scale.
    const float steadyPeak = std::max(peak(std::vector<float>(out.begin() + 50 * B, out.begin() + 95 * B)),
                                      peak(std::vector<float>(out.begin() + 150 * B, out.begin() + 195 * B)));
    float worst = 0.0f;
    for (size_t sw : {100 * B, 200 * B, 300 * B})
        worst = std::max(worst, peak(std::vector<float>(out.begin() + sw - B, out.begin() + sw + 10 * B)));
    MESSAGE("peak around the switches " << gainToDb(worst) << " dBFS, steady " << gainToDb(steadyPeak) << " dBFS");
    CHECK(worst <= steadyPeak * dbToGain(1.0f));
    CHECK(worst <= kFullScale);
}

TEST_CASE("pain: survives saving and loading, with intensity and bit-crush changed") {
    const ChainPreset& p = pain();
    ChainPreset back;
    REQUIRE(PresetLibrary::fromJson(PresetLibrary::toJson(p), back));
    CHECK(back.name == "Pain");
    CHECK(back.chain == p.chain);

    Session s;
    StripModel st;
    st.id = 1;
    st.name = "Microphone";
    st.voices = {"Pain"};
    st.chain = chainFromJson(p.chain, nullptr);
    applyIntensity(*st.chain[kIntensity], p.chain[kIntensity], 12.0f); // intensity (drive and output)
    st.chain[kCrush]->bypass = false;                                   // bit-crush on
    s.strips.push_back(std::move(st));
    s.nextId = 2;
    Session r;
    REQUIRE(sessionFromJson(json::parse(sessionToJson(s).dump()), r, nullptr));
    REQUIRE(r.strips.size() == 1);
    const StripModel& got = r.strips[0];
    CHECK(got.voices == std::vector<std::string>{"Pain"});
    REQUIRE(got.chain.size() == s.strips[0].chain.size());
    for (size_t i = 0; i < got.chain.size(); ++i) {
        CHECK(std::string(got.chain[i]->typeId()) == s.strips[0].chain[i]->typeId());
        CHECK(got.chain[i]->bypass.load() == s.strips[0].chain[i]->bypass.load());
        for (int k = 0; k < got.chain[i]->numParams(); ++k)
            CHECK(got.chain[i]->param(k) == doctest::Approx(s.strips[0].chain[i]->param(k)));
    }
    const Effect& stage = *got.chain[kIntensity];
    CHECK(stage.param(stage.paramIndex("drive")) == doctest::Approx(12.0f));
    CHECK(stage.param(stage.paramIndex("output")) == doctest::Approx(-2.2f)); // -7 + 0.6 x (20 - 12)
}
