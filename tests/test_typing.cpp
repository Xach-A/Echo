// Noise removal against real keyboard typing and mouse clicks (recordings in
// tests/data/noise, CC0 from BigSoundBank): typing alone must disappear, and
// typing under speech must go between the words while the words stay whole.
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
#include "dsp/Effect.h"
#include "dsp/EffectRegistry.h"
#include "dsp/NoiseSuppressorInfo.h"

using namespace echo;
using namespace echo::test;

#ifndef ECHO_TEST_SPEECH_DIR
#define ECHO_TEST_SPEECH_DIR "test-speech"
#endif

namespace {

constexpr double kSr = 48000.0;
constexpr int kBlock = 480;
constexpr size_t kF = 480; // 10 ms

std::vector<float> loadMono(const std::string& path) {
    WavData w;
    if (!readWav(utf8ToWide(path), w) || w.sampleRate != 48000 || w.channels != 1) return {};
    return w.samples;
}

std::vector<float> noiseClip(const char* name, float peakDb) {
    auto v = loadMono(std::string(ECHO_TEST_DATA_DIR) + "/noise/" + name + ".wav");
    REQUIRE_MESSAGE(!v.empty(), std::string(name));
    const float g = dbToGain(peakDb) / peak(v);
    for (float& x : v) x *= g;
    return v;
}

// Speech at -20 dBFS RMS (active parts), or empty if not generated.
std::vector<float> speech(const char* name) {
    auto v = loadMono(std::string(ECHO_TEST_SPEECH_DIR) + "/" + name);
    double s = 0.0;
    size_t active = 0;
    for (float x : v)
        if (std::fabs(x) > 1e-3f) {
            s += x * x;
            ++active;
        }
    const float g = active ? dbToGain(-20.0f) / static_cast<float>(std::sqrt(s / active)) : 1.0f;
    for (float& x : v) x *= g;
    return v;
}

std::vector<float> roomTone(size_t n, float db) {
    std::mt19937 rng(7);
    std::normal_distribution<float> nd(0.0f, 1.0f);
    std::vector<float> v(n);
    float lp = 0.0f;
    for (auto& x : v) {
        lp = 0.9f * lp + 0.1f * nd(rng);
        x = lp;
    }
    const float g = dbToGain(db) / std::max(1e-9f, rms(v, 0, n));
    for (auto& x : v) x *= g;
    return v;
}

std::unique_ptr<Effect> denoiser(bool gate) {
    auto e = EffectRegistry::instance().create(noiseSuppressorTypeId());
    REQUIRE(e != nullptr);
    e->prepare(kSr, kBlock);
    if (gate) e->setParam("vad_gate", 1.0f);
    return e;
}

std::vector<float> run(Effect& e, std::vector<float> x) {
    for (size_t pos = 0; pos < x.size(); pos += kBlock) {
        const int n = static_cast<int>(std::min<size_t>(kBlock, x.size() - pos));
        float* ch[2] = {x.data() + pos, nullptr};
        e.process(ch, 1, n);
    }
    return x;
}

double db(double meanSquare) { return 10.0 * std::log10(meanSquare + 1e-20); }

double meanSquare(const std::vector<float>& v, size_t from, size_t to) {
    double e = 0.0;
    size_t n = 0;
    for (size_t i = from; i < to && i < v.size(); ++i, ++n) e += static_cast<double>(v[i]) * v[i];
    return n ? e / n : 0.0;
}

// 10 ms frames of `s` with speech in them (above -60 dBFS).
std::vector<bool> speechFrames(const std::vector<float>& s) {
    std::vector<bool> on(s.size() / kF);
    for (size_t f = 0; f < on.size(); ++f) on[f] = meanSquare(s, f * kF, f * kF + kF) >= 1e-6;
    return on;
}

} // namespace

TEST_CASE("typing: real keyboard typing and mouse clicks on their own disappear") {
    for (const char* name : {"keyboard_quick", "keyboard_slow", "mouse_clicks"}) {
        // Loud: peaks at -12 dBFS (a mechanical keyboard next to the mic),
        // over a quiet room.
        auto clip = noiseClip(name, -12.0f);
        auto room = roomTone(clip.size(), -60.0f);
        for (size_t i = 0; i < clip.size(); ++i) clip[i] += room[i];
        for (bool gate : {false, true}) {
            auto e = denoiser(gate);
            const auto y = run(*e, clip);
            const size_t lat = static_cast<size_t>(e->latencySamples());
            const size_t from = 4800, to = clip.size() - lat;
            const double red = db(meanSquare(y, from + lat, to + lat)) - db(meanSquare(clip, from, to));
            const float pk = peak(y, from + lat, to - from);
            const std::string what = std::string(name) + (gate ? " (voice gate on)" : "");
            MESSAGE(what << ": " << red << " dB, peaks -12 -> " << gainToDb(pk) << " dBFS");
            CHECK_MESSAGE(red <= -45.0, what);
            CHECK_MESSAGE(gainToDb(pk) <= -50.0f, what);
        }
    }
}

TEST_CASE("typing: under speech, typing goes between the words and the words stay whole") {
    auto s = speech("speech_b.wav");
    if (s.empty()) {
        MESSAGE("SKIPPED: speech test material not generated");
        return;
    }
    for (const char* name : {"keyboard_quick", "keyboard_slow"}) {
        auto keys = noiseClip(name, -18.0f);
        std::vector<float> mix(s.size());
        for (size_t i = 0; i < s.size(); ++i) mix[i] = s[i] + keys[i % keys.size()];
        auto e = denoiser(false);
        const auto y = run(*e, mix);
        auto ref = denoiser(false);
        const auto yClean = run(*ref, s); // the same speech without typing
        const size_t lat = static_cast<size_t>(e->latencySamples());
        const auto on = speechFrames(s);

        // Pauses: more than 250 ms from any word (past the hold that keeps
        // the ends of words). Words: frames with speech.
        double pauseIn = 0, pauseOut = 0, words = 0, wordsOut = 0;
        int pauses = 0, wordFrames = 0;
        for (size_t f = 30; f + 30 < on.size() && (f + 1) * kF + lat <= y.size(); ++f) {
            bool near = false;
            for (size_t g = f - 25; g <= f + 25; ++g) near = near || on[g];
            const double in = meanSquare(mix, f * kF, f * kF + kF), out = meanSquare(y, f * kF + lat, f * kF + kF + lat);
            if (!near) {
                pauseIn += in;
                pauseOut += out;
                ++pauses;
            } else if (on[f]) {
                words += meanSquare(yClean, f * kF + lat, f * kF + kF + lat);
                wordsOut += out;
                ++wordFrames;
            }
        }
        REQUIRE(pauses > 20);
        const double speechDb = db(words / wordFrames);
        const double typingDb = db(pauseOut / pauses);
        const double removed = typingDb - db(pauseIn / pauses);
        const double kept = db(wordsOut / wordFrames) - speechDb;
        MESSAGE(std::string(name) << ": typing in pauses " << removed << " dB (now " << typingDb - speechDb
                                  << " dB below the voice); words " << kept << " dB vs without typing");
        CHECK(removed <= -30.0);
        CHECK(typingDb - speechDb <= -55.0);
        CHECK(std::abs(kept) <= 1.0);
    }
}

// Speech cut into pieces at its quietest points, with 300 ms of room tone
// between them: many word starts after a pause.
std::vector<float> speechWithPauses(std::vector<size_t>* starts) {
    std::vector<float> out;
    auto room = roomTone(48000 * 60, -60.0f);
    size_t r = 0;
    for (const char* name : {"speech_a.wav", "speech_b.wav"}) {
        const auto s = speech(name);
        if (s.empty()) return {};
        size_t pos = 0;
        while (pos + 48000 < s.size()) {
            // The quietest 10 ms between 0.5 and 1.0 s from here.
            size_t cut = pos + 24000;
            double best = 1e30;
            for (size_t f = pos + 24000; f + kF < pos + 48000; f += kF)
                if (const double e = meanSquare(s, f, f + kF); e < best) best = e, cut = f;
            for (int i = 0; i < 14400; ++i) out.push_back(room[r++ % room.size()]);
            // The piece starts at its first sound, where it grows (not on the
            // fading end of the word before).
            size_t from = pos;
            while (from + kF < cut && meanSquare(s, from, from + kF) < 1e-6) from += kF;
            while (from + 2 * kF < cut && meanSquare(s, from + kF, from + 2 * kF) < meanSquare(s, from, from + kF)) from += kF;
            if (starts && cut - from > 4 * kF) starts->push_back(out.size());
            for (size_t i = from; i < cut; ++i) out.push_back(s[i] + room[r++ % room.size()]);
            pos = cut;
        }
    }
    return out;
}

TEST_CASE("typing: word starts after a pause are not cut (look-ahead, soft starts)") {
    std::vector<size_t> starts;
    const auto x = speechWithPauses(&starts);
    if (x.empty()) {
        MESSAGE("SKIPPED: speech test material not generated");
        return;
    }
    // Reference: noise removal alone (no typing removal, no gate).
    auto ref = denoiser(false);
    ref->setParam("clicks", 0.0f);
    const auto yRef = run(*ref, x);
    for (bool gate : {false, true}) {
        auto e = denoiser(gate);
        const auto y = run(*e, x);
        const size_t lat = static_cast<size_t>(e->latencySamples());
        // First 30 ms: a quiet "t" or "k" burst 50 ms or more before its
        // vowel looks just like a keystroke and may be turned down (allowed
        // once). First 80 ms: the word itself always comes through.
        int softened = 0, cut = 0;
        double worst30 = 0.0, worst80 = 0.0;
        for (size_t st : starts) {
            const double l30 = db(meanSquare(y, st + lat, st + lat + 3 * kF)) - db(meanSquare(yRef, st + lat, st + lat + 3 * kF));
            const double l80 = db(meanSquare(y, st + lat, st + lat + 8 * kF)) - db(meanSquare(yRef, st + lat, st + lat + 8 * kF));
            worst30 = std::min(worst30, l30);
            worst80 = std::min(worst80, l80);
            if (l30 < -3.0) {
                ++softened;
                MESSAGE("word start at " << st / 48 << " ms: first 30 ms " << l30 << " dB, first 80 ms " << l80 << " dB");
            }
            if (l80 < -3.0) ++cut;
        }
        MESSAGE(std::string(gate ? "voice gate" : "typing removal") << ": " << starts.size() << " word starts; first 30 ms worst "
                                                                      << worst30 << " dB (" << softened << " softened), first 80 ms worst "
                                                                      << worst80 << " dB");
        CHECK(starts.size() >= 15);
        CHECK(softened <= (gate ? 2 : 1));
        CHECK(cut == 0);
    }
}
