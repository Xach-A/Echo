// Noise suppression (RNNoise) and acoustic echo cancellation tests.
//
// Speech material: generated locally with the Windows text-to-speech voices
// by tools/make_speech_samples.ps1 into <build>/test-speech (not committed).
// Tests that need it report "SKIPPED" when it is missing; all noise, click,
// silence and echo tests use synthetic signals and always run.
#include "doctest/doctest.h"

#include <chrono>
#include <filesystem>
#include <memory>
#include <random>

#include "FakeDevices.h"
#include "TestUtil.h"
#include "core/Text.h"
#include "core/WavFile.h"
#include "dsp/EffectRegistry.h"
#include "dsp/Fft.h"
#include "session/MixerController.h"

using namespace echo;
using namespace echo::test;

#ifndef ECHO_TEST_SPEECH_DIR
#define ECHO_TEST_SPEECH_DIR "test-speech"
#endif

namespace {

constexpr double kSr = 48000.0;
constexpr int kBlock = 480;

std::unique_ptr<Effect> makeFx(const char* type, double sr = kSr) {
    auto e = EffectRegistry::instance().create(type);
    REQUIRE(e != nullptr);
    e->prepare(sr, kBlock);
    return e;
}

std::vector<float> run(Effect& e, std::vector<float> x, const std::vector<float>* ref = nullptr) {
    for (size_t pos = 0; pos < x.size(); pos += kBlock) {
        const int n = static_cast<int>(std::min<size_t>(kBlock, x.size() - pos));
        if (ref) e.setReference(ref->data() + pos, ref->data() + pos, 1);
        float* ch[2] = {x.data() + pos, nullptr};
        e.process(ch, 1, n);
    }
    return x;
}

double rmsDb(const std::vector<float>& v, size_t from, size_t len) {
    return gainToDb(std::max(1e-9f, rms(v, from, len)));
}

// Speech at -20 dBFS RMS (active parts), or empty if not generated.
std::vector<float> speech(const char* name) {
    WavData w;
    const std::wstring path = utf8ToWide(std::string(ECHO_TEST_SPEECH_DIR) + "/" + name);
    if (!readWav(path, w) || w.sampleRate != 48000 || w.channels != 1) return {};
    double s = 0.0;
    size_t active = 0;
    for (float v : w.samples)
        if (std::fabs(v) > 1e-3f) {
            s += v * v;
            ++active;
        }
    const float g = active ? dbToGain(-20.0f) / static_cast<float>(std::sqrt(s / active)) : 1.0f;
    for (float& v : w.samples) v *= g;
    return w.samples;
}

std::vector<float> whiteNoise(size_t n, float db, uint32_t seed = 1) {
    std::mt19937 rng(seed);
    std::normal_distribution<float> nd(0.0f, dbToGain(db));
    std::vector<float> v(n);
    for (auto& x : v) x = nd(rng);
    return v;
}

std::vector<float> pinkNoise(size_t n, float db, uint32_t seed = 2) {
    std::mt19937 rng(seed);
    std::normal_distribution<float> nd(0.0f, 1.0f);
    std::vector<float> v(n);
    float b[7] = {};
    for (auto& x : v) {
        const float w = nd(rng);
        b[0] = 0.99886f * b[0] + w * 0.0555179f;
        b[1] = 0.99332f * b[1] + w * 0.0750759f;
        b[2] = 0.96900f * b[2] + w * 0.1538520f;
        b[3] = 0.86650f * b[3] + w * 0.3104856f;
        b[4] = 0.55000f * b[4] + w * 0.5329522f;
        b[5] = -0.7616f * b[5] - w * 0.0168980f;
        x = (b[0] + b[1] + b[2] + b[3] + b[4] + b[5] + b[6] + w * 0.5362f) * 0.11f;
        b[6] = w * 0.115926f;
    }
    const float g = dbToGain(db) / std::max(1e-9f, rms(v, 0, v.size()));
    for (auto& x : v) x *= g;
    return v;
}

// Computer fan: low-passed pink noise + blade/motor hum harmonics.
std::vector<float> fanNoise(size_t n, float db) {
    auto v = pinkNoise(n, 0.0f, 3);
    float lp = 0.0f;
    for (size_t i = 0; i < n; ++i) {
        lp += 0.15f * (v[i] - lp);
        v[i] = lp + 0.3f * static_cast<float>(std::sin(2 * kPiD * 120.0 * i / kSr) +
                                              0.5 * std::sin(2 * kPiD * 240.0 * i / kSr) +
                                              0.3 * std::sin(2 * kPiD * 410.0 * i / kSr));
    }
    const float g = dbToGain(db) / rms(v, 0, n);
    for (auto& x : v) x *= g;
    return v;
}

// Air conditioning: brown-ish rumble.
std::vector<float> acNoise(size_t n, float db) {
    auto w = whiteNoise(n, 0.0f, 4);
    std::vector<float> v(n);
    float acc = 0.0f;
    for (size_t i = 0; i < n; ++i) {
        acc = 0.995f * acc + w[i] * 0.05f;
        v[i] = acc;
    }
    const float g = dbToGain(db) / rms(v, 0, n);
    for (auto& x : v) x *= g;
    return v;
}

// Mechanical keyboard: short resonant noise bursts at random intervals.
std::vector<float> keyboard(size_t n, float peakDb, uint32_t seed = 5) {
    std::mt19937 rng(seed);
    std::uniform_int_distribution<int> gap(static_cast<int>(0.07 * kSr), static_cast<int>(0.25 * kSr));
    std::normal_distribution<float> nd(0.0f, 1.0f);
    std::vector<float> v(n, 0.0f);
    const float amp = dbToGain(peakDb);
    for (size_t t = static_cast<size_t>(0.05 * kSr); t + 2000 < n; t += gap(rng)) {
        float r1 = 0, r2 = 0;
        for (int i = 0; i < 1500; ++i) {
            const float env = std::exp(-i / 180.0f) + 0.4f * std::exp(-i / 600.0f);
            const float x = nd(rng);
            r1 = 0.6f * r1 + x;        // click body
            r2 = 0.95f * r2 + 0.1f * x; // low "thock"
            v[t + i] += amp * 0.35f * env * (0.6f * r1 + 2.0f * r2);
        }
    }
    return v;
}

int measureLag(const std::vector<float>& in, const std::vector<float>& out, int maxLag) {
    const size_t start = 24000, len = std::min<size_t>(96000, in.size() - start - maxLag);
    double best = -1e30;
    int bestLag = 0;
    for (int lag = 0; lag <= maxLag; ++lag) {
        double c = 0.0;
        for (size_t i = start; i < start + len; i += 2) c += static_cast<double>(in[i]) * out[i + lag];
        if (c > best) {
            best = c;
            bestLag = lag;
        }
    }
    return bestLag;
}

double correlation(const std::vector<float>& a, const std::vector<float>& b, size_t from, size_t len, int lagB) {
    double ab = 0, aa = 0, bb = 0;
    for (size_t i = from; i < from + len && i + lagB < b.size(); ++i) {
        ab += static_cast<double>(a[i]) * b[i + lagB];
        aa += static_cast<double>(a[i]) * a[i];
        bb += static_cast<double>(b[i + lagB]) * b[i + lagB];
    }
    return ab / std::sqrt(aa * bb + 1e-30);
}

// Phase-insensitive SNR: clean-speech STFT magnitudes vs the error between
// estimate and clean magnitudes (1024-point Hann frames, 50% overlap).
double spectralSnrDb(const std::vector<float>& clean, const std::vector<float>& est, int lagEst) {
    constexpr int kN = 1024;
    RealFft fft;
    fft.prepare(kN);
    std::vector<float> win(kN), a(kN), b(kN), sa(kN + 2), sb(kN + 2);
    makeHann(win.data(), kN, true);
    double sig = 0, err = 0;
    for (size_t pos = 24000; pos + kN + lagEst < clean.size() && pos + kN + lagEst < est.size(); pos += kN / 2) {
        for (int i = 0; i < kN; ++i) {
            a[i] = clean[pos + i] * win[i];
            b[i] = est[pos + i + lagEst] * win[i];
        }
        fft.forward(a.data(), sa.data());
        fft.forward(b.data(), sb.data());
        for (int k = 0; k <= kN / 2; ++k) {
            const double ma = std::hypot(sa[2 * k], sa[2 * k + 1]), mb = std::hypot(sb[2 * k], sb[2 * k + 1]);
            sig += ma * ma;
            err += (mb - ma) * (mb - ma);
        }
    }
    return 10 * std::log10((sig + 1e-20) / (err + 1e-20));
}

} // namespace

// ---------------------------------------------------------------------------
// Noise suppression
// ---------------------------------------------------------------------------

TEST_CASE("denoise: stationary noises (fan, hiss, air conditioning, room) are strongly reduced") {
    const size_t n = static_cast<size_t>(kSr * 4);
    struct Case {
        const char* name;
        std::vector<float> noise;
        double minReductionDb;
    };
    Case cases[] = {{"microphone hiss", whiteNoise(n, -45.0f), 20.0},
                    {"computer fan", fanNoise(n, -40.0f), 15.0},
                    {"air conditioning", acNoise(n, -40.0f), 15.0},
                    {"room tone (pink)", pinkNoise(n, -45.0f), 15.0}};
    for (auto& c : cases) {
        CAPTURE(c.name);
        auto e = makeFx("voice_denoise");
        auto y = run(*e, c.noise);
        CHECK(allFinite(y));
        const double red = rmsDb(c.noise, n / 2, n / 2) - rmsDb(y, n / 2, n / 2);
        MESSAGE(c.name << ": " << red << " dB reduction");
        CHECK(red >= c.minReductionDb);
    }
}

TEST_CASE("denoise: keyboard and mouse clicks are attenuated") {
    const size_t n = static_cast<size_t>(kSr * 4);
    auto clicks = keyboard(n, -12.0f);
    auto e = makeFx("voice_denoise");
    auto y = run(*e, clicks);
    const double red = rmsDb(clicks, n / 4, n * 3 / 4) - rmsDb(y, n / 4, n * 3 / 4);
    MESSAGE("keyboard clicks: " << red << " dB energy reduction, peak " << gainToDb(peak(clicks)) << " -> "
                                << gainToDb(peak(y.data() + n / 4, n * 3 / 4)) << " dBFS");
    CHECK(red >= 10.0);
}

TEST_CASE("denoise: digital silence stays silent and finite") {
    auto e = makeFx("voice_denoise");
    auto y = run(*e, std::vector<float>(static_cast<size_t>(kSr * 2), 0.0f));
    CHECK(allFinite(y));
    CHECK(peak(y) < 1e-6f);
}

TEST_CASE("denoise: clean speech is preserved, latency is as reported") {
    auto s = speech("speech_a.wav");
    if (s.empty()) {
        MESSAGE("SKIPPED: run tools/make_speech_samples.ps1 to generate speech test material");
        return;
    }
    auto e = makeFx("voice_denoise");
    auto y = run(*e, s);
    const int lag = measureLag(s, y, 3000);
    MESSAGE("measured latency " << lag << " samples (" << lag / 48.0 << " ms), reported " << e->latencySamples());
    CHECK(std::abs(lag - e->latencySamples()) <= 2);
    const double c = correlation(s, y, 24000, s.size() - 24000 - lag - 10, lag);
    const double lvl = rmsDb(y, 24000 + lag, s.size() - 30000 - lag) - rmsDb(s, 24000, s.size() - 30000 - lag);
    MESSAGE("clean speech: correlation " << c << ", level change " << lvl << " dB");
    CHECK(c >= 0.90);
    CHECK(std::abs(lvl) <= 3.0);
}

TEST_CASE("denoise: speech mixed with fan noise and typing is cleaned while the speech is kept") {
    auto s = speech("speech_b.wav");
    if (s.empty()) {
        MESSAGE("SKIPPED: speech test material not generated");
        return;
    }
    const size_t n = s.size();
    auto fan = fanNoise(n, -32.0f);
    auto keys = keyboard(n, -18.0f, 9);
    std::vector<float> mix(n);
    for (size_t i = 0; i < n; ++i) mix[i] = s[i] + fan[i] + keys[i];
    auto e = makeFx("voice_denoise");
    auto y = run(*e, mix);
    const int lat = e->latencySamples();

    // Noise removed = how much closer the output is to the suppressor's own
    // rendering of the clean speech than the input was to the clean speech.
    // (Comparing against the raw clean speech would cap the result: RNNoise's
    // pitch post-filter and band gains reshape even clean speech by ~10 dB of
    // "error" while keeping how it sounds. The level and correlation checks
    // below make sure the clean rendering itself stays faithful.)
    auto ref = makeFx("voice_denoise");
    const auto yClean = run(*ref, s);
    const double snrIn = spectralSnrDb(s, mix, 0), snrOut = spectralSnrDb(yClean, y, 0);
    MESSAGE("spectral SNR in " << snrIn << " dB -> out " << snrOut << " dB (+" << snrOut - snrIn << " dB)");
    CHECK(snrOut - snrIn >= 6.0);
    // Pauses between words: the noise must be strongly reduced. Words: the
    // level must stay close to the clean speech (no "underwater" ducking).
    double nIn = 0, nOut = 0, spClean = 0, spOut = 0;
    for (size_t i = 24000; i + lat + 480 < n; i += 480) {
        double es = 0, em = 0, eo = 0;
        for (int k = 0; k < 480; ++k) {
            es += static_cast<double>(s[i + k]) * s[i + k];
            em += static_cast<double>(mix[i + k]) * mix[i + k];
            eo += static_cast<double>(y[i + k + lat]) * y[i + k + lat];
        }
        if (es / 480 < 1e-6) {
            nIn += em;
            nOut += eo;
        } else if (es / 480 > dbToGain(-30.0f) * dbToGain(-30.0f)) {
            spClean += es;
            spOut += eo;
        }
    }
    const double pauseRed = 10 * std::log10((nOut + 1e-20) / (nIn + 1e-20));
    const double speechLvl = 10 * std::log10((spOut + 1e-20) / (spClean + 1e-20));
    MESSAGE("noise between words: " << pauseRed << " dB; speech level vs clean: " << speechLvl << " dB");
    CHECK(pauseRed <= -20.0);
    CHECK(std::abs(speechLvl) <= 3.0);
    CHECK(correlation(s, y, 24000, n - 30000 - lat, lat) >= 0.85);
    CHECK(allFinite(y));
}

TEST_CASE("denoise: loud speech does not clip") {
    auto s = speech("speech_a.wav");
    if (s.empty()) s = sine(220, kSr, static_cast<size_t>(kSr * 2), 1.0f);
    const float pk = peak(s);
    for (auto& v : s) v *= 0.99f / pk;
    auto e = makeFx("voice_denoise");
    auto y = run(*e, s);
    MESSAGE("input peak 0.99, output peak " << peak(y));
    CHECK(peak(y) <= 1.0f);
}

TEST_CASE("denoise: strength 0 and mix 0 return the original signal (time-aligned)") {
    auto x = pinkNoise(static_cast<size_t>(kSr * 2), -30.0f);
    for (const char* param : {"strength", "mix"}) {
        CAPTURE(param);
        auto e = makeFx("voice_denoise");
        e->setParam(param, 0.0f);
        auto y = run(*e, x);
        const int lat = e->latencySamples();
        float maxErr = 0.0f;
        for (size_t i = 0; i + lat < x.size(); ++i) maxErr = std::max(maxErr, std::fabs(y[i + lat] - x[i]));
        CHECK(maxErr < 1e-5f);
    }
}

TEST_CASE("denoise: each input is processed independently") {
    auto s = speech("speech_a.wav");
    if (s.empty()) s = pinkNoise(static_cast<size_t>(kSr * 3), -25.0f, 11);
    auto noise = fanNoise(s.size(), -30.0f);
    auto alone = makeFx("voice_denoise");
    auto ref = run(*alone, s);
    // Interleave a second instance processing different audio.
    auto a = makeFx("voice_denoise");
    auto b = makeFx("voice_denoise");
    std::vector<float> ya = s, yb = noise;
    for (size_t pos = 0; pos < s.size(); pos += kBlock) {
        const int n = static_cast<int>(std::min<size_t>(kBlock, s.size() - pos));
        float* ca[2] = {ya.data() + pos, nullptr};
        float* cb[2] = {yb.data() + pos, nullptr};
        a->process(ca, 1, n);
        b->process(cb, 1, n);
    }
    CHECK(ya == ref);
}

TEST_CASE("denoise: voice activity gate closes on noise and opens for speech") {
    auto s = speech("speech_a.wav");
    if (s.empty()) {
        MESSAGE("SKIPPED: speech test material not generated");
        return;
    }
    const size_t pre = static_cast<size_t>(kSr * 2);
    std::vector<float> x(pre, 0.0f);
    x.insert(x.end(), s.begin(), s.end());
    auto room = pinkNoise(x.size(), -50.0f, 21);
    for (size_t i = 0; i < x.size(); ++i) x[i] += room[i];
    // Partial strength leaves some room tone so the gate's extra attenuation
    // is measurable.
    auto plain = makeFx("voice_denoise");
    auto gated = makeFx("voice_denoise");
    plain->setParam("strength", 50.0f);
    gated->setParam("strength", 50.0f);
    gated->setParam("vad_gate", 1.0f);
    gated->setParam("vad_range", -30.0f);
    auto y0 = run(*plain, x);
    float maxVad = 0.0f;
    std::vector<float> y1 = x;
    for (size_t pos = 0; pos < y1.size(); pos += kBlock) {
        float* ch[2] = {y1.data() + pos, nullptr};
        gated->process(ch, 1, static_cast<int>(std::min<size_t>(kBlock, y1.size() - pos)));
        if (pos > pre + 24000) maxVad = std::max(maxVad, gated->vis[0].load());
    }
    // Noise-only lead-in: the gate adds attenuation; speech: nearly unchanged.
    const double leadPlain = rmsDb(y0, 24000, pre - 30000), leadGated = rmsDb(y1, 24000, pre - 30000);
    const double spPlain = rmsDb(y0, pre + 24000, s.size() - 48000), spGated = rmsDb(y1, pre + 24000, s.size() - 48000);
    MESSAGE("noise-only: " << leadPlain << " -> " << leadGated << " dBFS; speech: " << spPlain << " -> " << spGated
                           << " dBFS; max voice probability " << maxVad);
    CHECK(leadGated < leadPlain - 15.0);
    CHECK(std::abs(spGated - spPlain) < 2.0);
    CHECK(maxVad > 0.8f);
}

TEST_CASE("denoise: works at 44.1 kHz (internal resampling) with the reported latency") {
    auto e = makeFx("voice_denoise", 44100.0);
    const size_t n = static_cast<size_t>(44100 * 4);
    auto x = sine(300, 44100.0, n, 0.0f);
    std::mt19937 rng(7);
    std::normal_distribution<float> nd(0.0f, dbToGain(-45.0f));
    for (auto& v : x) v = nd(rng);
    std::vector<float> y = x;
    for (size_t pos = 0; pos < y.size(); pos += kBlock) {
        float* ch[2] = {y.data() + pos, nullptr};
        e->process(ch, 1, static_cast<int>(std::min<size_t>(kBlock, y.size() - pos)));
    }
    CHECK(allFinite(y));
    const double red = gainToDb(rms(x, n / 2, n / 2)) - gainToDb(std::max(1e-9f, rms(y, n / 2, n / 2)));
    MESSAGE("44.1 kHz hiss reduction " << red << " dB, latency " << e->latencySamples() << " samples");
    CHECK(red >= 15.0);
    // Latency check with a broadband signal at 44.1 kHz (strength 0 keeps it).
    auto e2 = makeFx("voice_denoise", 44100.0);
    e2->setParam("strength", 0.0f);
    std::vector<float> z = x, z0 = x;
    for (size_t pos = 0; pos < z.size(); pos += kBlock) {
        float* ch[2] = {z.data() + pos, nullptr};
        e2->process(ch, 1, static_cast<int>(std::min<size_t>(kBlock, z.size() - pos)));
    }
    const int lag = measureLag(z0, z, 3000);
    MESSAGE("44.1 kHz measured latency " << lag << ", reported " << e2->latencySamples());
    CHECK(std::abs(lag - e2->latencySamples()) <= 3);
}

TEST_CASE("denoise: CPU cost and latency are reported") {
    auto e = makeFx("voice_denoise");
    auto x = pinkNoise(static_cast<size_t>(kSr * 10), -30.0f);
    const auto t0 = std::chrono::steady_clock::now();
    run(*e, x);
    const double sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    MESSAGE("RNNoise: " << sec * 1000.0 / 10.0 << " ms per second of audio (" << sec * 10.0
                        << "% of one core), latency " << e->latencySamples() / 48.0 << " ms");
    CHECK(sec / 10.0 < 0.25); // well under a quarter of real time
}

// ---------------------------------------------------------------------------
// Echo cancellation
// ---------------------------------------------------------------------------

namespace {

// Far-end "speech-like" signal: pink noise with syllable-rate envelope and
// harmonics, so the test does not depend on TTS.
std::vector<float> farEnd(size_t n, uint32_t seed) {
    auto v = pinkNoise(n, -20.0f, seed);
    for (size_t i = 0; i < n; ++i) {
        const double env = 0.55 + 0.45 * std::sin(2 * kPiD * 3.7 * i / kSr) * std::sin(2 * kPiD * 0.6 * i / kSr);
        v[i] = static_cast<float>(v[i] * env + 0.05 * std::sin(2 * kPiD * 160.0 * i / kSr) * env);
    }
    return v;
}

// Room echo: 35 ms bulk delay, then an exponentially decaying reflection tail.
std::vector<float> echoOf(const std::vector<float>& x, float gainDb) {
    std::mt19937 rng(99);
    std::normal_distribution<float> nd(0.0f, 1.0f);
    const int delay = static_cast<int>(0.035 * kSr), len = static_cast<int>(0.12 * kSr);
    std::vector<float> h(delay + len, 0.0f);
    double e = 0;
    for (int i = 0; i < len; ++i) {
        h[delay + i] = nd(rng) * std::exp(-i / (0.025f * static_cast<float>(kSr)));
        e += h[delay + i] * h[delay + i];
    }
    const float g = dbToGain(gainDb) / static_cast<float>(std::sqrt(e));
    for (auto& v : h) v *= g;
    std::vector<float> y(x.size(), 0.0f);
    for (size_t i = 0; i < x.size(); ++i)
        for (size_t k = delay; k < h.size() && k <= i; ++k) y[i] += h[k] * x[i - k];
    return y;
}

} // namespace

TEST_CASE("echo cancellation: removes a room echo of the playback signal") {
    const size_t n = static_cast<size_t>(kSr * 10);
    auto far = farEnd(n, 31);
    auto mic = echoOf(far, -6.0f);
    auto floor = whiteNoise(n, -75.0f, 32);
    for (size_t i = 0; i < n; ++i) mic[i] += floor[i];
    auto e = makeFx("echo_cancel");
    auto y = run(*e, mic, &far);
    CHECK(allFinite(y));
    const size_t from = static_cast<size_t>(kSr * 6), len = static_cast<size_t>(kSr * 3.5);
    const double erle = rmsDb(mic, from, len) - rmsDb(y, from + e->latencySamples(), len);
    MESSAGE("echo reduction after convergence: " << erle << " dB (effect reports " << e->vis[0].load() << " dB)");
    CHECK(erle >= 20.0);
}

TEST_CASE("echo cancellation: keeps the near-end voice during double talk") {
    const size_t n = static_cast<size_t>(kSr * 12);
    auto far = farEnd(n, 41);
    auto echo = echoOf(far, -6.0f);
    auto nearV = speech("speech_b.wav");
    if (nearV.empty()) nearV = farEnd(n, 77); // different "talker"
    std::vector<float> nearEnd(n, 0.0f);
    const size_t dtStart = static_cast<size_t>(kSr * 6);
    for (size_t i = dtStart; i < n && i - dtStart < nearV.size(); ++i) nearEnd[i] = nearV[i - dtStart];
    std::vector<float> mic(n);
    for (size_t i = 0; i < n; ++i) mic[i] = echo[i] + nearEnd[i];
    auto e = makeFx("echo_cancel");
    auto y = run(*e, mic, &far);
    const int lat = e->latencySamples();
    const size_t from = dtStart + static_cast<size_t>(kSr * 0.5), len = static_cast<size_t>(kSr * 5);
    const double c = correlation(nearEnd, y, from, len, lat);
    const double lvl = rmsDb(y, from + lat, len) - rmsDb(nearEnd, from, len);
    const double echoInDt = rmsDb(echo, from, len) - rmsDb(nearEnd, from, len);
    MESSAGE("double talk: near-end correlation " << c << ", near-end level change " << lvl
                                                  << " dB (echo was " << echoInDt << " dB relative to the voice)");
    CHECK(c >= 0.85);
    CHECK(lvl > -6.0);
    CHECK(lvl < 2.0);
}

TEST_CASE("echo cancellation: transparent when no reference is connected") {
    auto x = pinkNoise(static_cast<size_t>(kSr * 2), -25.0f, 51);
    auto e = makeFx("echo_cancel");
    auto y = run(*e, x, nullptr);
    const int lat = e->latencySamples();
    float maxErr = 0.0f;
    for (size_t i = 0; i + lat < x.size(); ++i) maxErr = std::max(maxErr, std::fabs(y[i + lat] - x[i]));
    CHECK(maxErr < 1e-4f);
}

TEST_CASE("echo cancellation: reference arrives from the playback loopback through the engine") {
    FakeBackend backend;
    backend.addDevice("mic", "USB Mic", false, 1, 48000, true);
    backend.addDevice("phones", "Speakers", true, 2, 48000, true);
    backend.addDevice("cable_in", "CABLE Input", true, 2, 48000, false, true);
    MixerController mc(backend);
    mc.createDefaultSession();
    StripModel& micStrip = mc.session().strips[0];
    micStrip.reference.kind = SourceKind::RenderLoopback; // default playback device
    std::vector<std::shared_ptr<Effect>> chain;
    chain.push_back(mc.createEffect("echo_cancel"));
    mc.setChain({false, micStrip.id}, chain);
    mc.start(false);
    REQUIRE(backend.captures.count("phones")); // loopback reference opened
    const size_t n = static_cast<size_t>(kSr * 10);
    auto far = farEnd(n, 61);
    auto echo = echoOf(far, -6.0f);
    auto loop = backend.captures["phones"];
    auto micCap = backend.captures["mic"];
    auto out = backend.renders["cable_in"];
    std::vector<float> stereo(kBlock * 2);
    for (size_t pos = 0; pos + kBlock <= n; pos += kBlock) {
        for (int i = 0; i < kBlock; ++i) stereo[2 * i] = stereo[2 * i + 1] = far[pos + i];
        loop->ring().write(stereo.data(), stereo.size());
        micCap->ring().write(echo.data() + pos, kBlock);
        mc.engine().processBlock();
        for (auto& [id, r] : backend.renders) r->consume(kBlock);
    }
    // Compare the last 3 s of the virtual-mic output with the echo level.
    const size_t frames = out->out.size() / 2, len = static_cast<size_t>(kSr * 3);
    std::vector<float> l(len);
    for (size_t i = 0; i < len; ++i) l[i] = out->out[(frames - len + i) * 2];
    const double erle = rmsDb(echo, n - len, len) - rmsDb(l, 0, len);
    MESSAGE("end-to-end echo reduction through the engine: " << erle << " dB");
    CHECK(erle >= 15.0);
    mc.stop();
}

TEST_CASE("latency: every effect's reported latency matches its measured delay") {
    // Settings that make each effect (close to) an identity so the delay can
    // be measured by cross-correlation.
    struct Case {
        const char* type;
        std::vector<std::pair<const char*, float>> params;
    };
    const Case cases[] = {
        {"voice_denoise", {{"strength", 0.0f}}},
        {"echo_cancel", {}},
        {"denoise", {{"reduction", 0.0f}}},
        {"limiter", {{"ceiling", 0.0f}}},
        {"robot", {{"mix", 0.0f}}},
        {"autotune", {{"mix", 0.0f}}},
    };
    auto x = pinkNoise(static_cast<size_t>(kSr * 3), -25.0f, 71);
    for (const auto& c : cases) {
        CAPTURE(c.type);
        auto e = makeFx(c.type);
        for (const auto& [id, v] : c.params) e->setParam(id, v);
        auto y = run(*e, x, nullptr);
        const int lag = measureLag(x, y, 6000);
        MESSAGE(c.type << ": measured " << lag << " samples, reported " << e->latencySamples());
        CHECK(std::abs(lag - e->latencySamples()) <= 2);
    }
    // The phase vocoder re-synthesises noise with new phases, so its delay is
    // measured on isolated tone bursts instead (phase restarts at each onset).
    std::vector<float> bursts(static_cast<size_t>(kSr * 3), 0.0f);
    std::mt19937 rng(17);
    std::uniform_int_distribution<int> gap(static_cast<int>(0.1 * kSr), static_cast<int>(0.3 * kSr));
    for (size_t t = 2000; t + 4000 < bursts.size(); t += 2400 + gap(rng))
        for (int i = 0; i < 2400; ++i)
            bursts[t + i] = 0.3f * static_cast<float>(std::sin(kPiD * i / 2400.0) * std::sin(2 * kPiD * 700.0 * i / kSr));
    for (int q = 0; q < 3; ++q) {
        auto e = makeFx("pitch");
        e->setParam("semitones", 0.0f);
        e->setParam("preserve", 0.0f);
        e->setParam("quality", static_cast<float>(q));
        e->resetState();
        auto y = run(*e, bursts, nullptr);
        const int lag = measureLag(bursts, y, 6000);
        MESSAGE("pitch quality " << q << ": measured " << lag << " samples, reported " << e->latencySamples());
        CHECK(std::abs(lag - e->latencySamples()) <= 2);
    }
}

TEST_CASE("noise suppression in the engine: the default microphone strip cleans the virtual mic") {
    FakeBackend backend;
    backend.addDevice("mic", "USB Mic", false, 1, 48000, true);
    backend.addDevice("phones", "Headphones", true, 2, 48000, true);
    backend.addDevice("cable_in", "CABLE Input", true, 2, 48000, false, true);
    MixerController mc(backend);
    mc.createDefaultSession(); // noise suppression is on by default
    REQUIRE(!mc.session().strips[0].chain.empty());
    REQUIRE(std::string(mc.session().strips[0].chain[0]->typeId()) == "voice_denoise");
    mc.start(false);
    const size_t n = static_cast<size_t>(kSr * 6);
    auto in = fanNoise(n, -40.0f);
    auto sp = speech("speech_a.wav");
    const bool withSpeech = !sp.empty();
    if (withSpeech)
        for (size_t i = 0; i < n; ++i) in[i] += sp[i % sp.size()];
    auto micCap = backend.captures["mic"];
    auto out = backend.renders["cable_in"];
    for (size_t pos = 0; pos + kBlock <= n; pos += kBlock) {
        micCap->ring().write(in.data() + pos, kBlock);
        mc.engine().processBlock();
        for (auto& [id, r] : backend.renders) r->consume(kBlock);
    }
    const size_t frames = out->out.size() / 2;
    std::vector<float> l(frames);
    for (size_t i = 0; i < frames; ++i) l[i] = out->out[i * 2];
    const int lag = measureLag(in, l, 20000);
    MESSAGE("engine path delay " << lag << " samples");
    if (withSpeech) {
        // Pauses: noise removed. Words: level kept.
        double nIn = 0, nOut = 0, sIn = 0, sOut = 0;
        for (size_t i = 48000; i + lag + 480 < frames && i + 480 < n; i += 480) {
            double es = 0, ei = 0, eo = 0;
            for (int k = 0; k < 480; ++k) {
                const float c = sp[(i + k) % sp.size()];
                es += static_cast<double>(c) * c;
                ei += static_cast<double>(in[i + k]) * in[i + k];
                eo += static_cast<double>(l[i + k + lag]) * l[i + k + lag];
            }
            if (es / 480 < 1e-6) {
                nIn += ei;
                nOut += eo;
            } else if (es / 480 > 1e-3) {
                sIn += ei;
                sOut += eo;
            }
        }
        const double pause = 10 * std::log10((nOut + 1e-20) / (nIn + 1e-20));
        const double words = 10 * std::log10((sOut + 1e-20) / (sIn + 1e-20));
        MESSAGE("virtual mic: fan noise between words " << pause << " dB, speech level " << words << " dB");
        CHECK(pause <= -20.0);
        CHECK(std::abs(words) <= 3.0);
    } else {
        const double red = rmsDb(l, frames - 96000, 96000) - rmsDb(in, n - 96000, 96000);
        MESSAGE("virtual mic: fan noise " << red << " dB");
        CHECK(red <= -15.0);
    }
    mc.stop();
}
