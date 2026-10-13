// AI voice pipeline tests. These use a real ONNX Runtime when one is
// installed next to the test executable (ai\onnxruntime.dll) and the
// deterministic pipeline-test model in tests/data. Without ONNX Runtime the
// inference tests report themselves as skipped.
#include "doctest/doctest.h"

#include <chrono>
#include <filesystem>
#include <random>
#include <thread>

#include "TestUtil.h"
#include "ai/AiVoiceEffect.h"
#include "ai/OrtRuntime.h"
#include "core/Text.h"

using namespace echo;
using namespace echo::test;

#ifndef ECHO_TEST_DATA_DIR
#define ECHO_TEST_DATA_DIR "tests/data"
#endif

namespace {
std::wstring dataDir(const char* sub) { return utf8ToWide(std::string(ECHO_TEST_DATA_DIR) + "/" + sub); }
}

TEST_CASE("ai: manifest validation") {
    ai::ModelManifest m;
    REQUIRE(ai::loadManifest(dataDir("echo-test-lowpass"), m));
    CHECK(m.valid);
    CHECK(m.sampleRate == 16000);
    CHECK(m.license == "MIT");
    CHECK(m.pitchInputName == "pitch_shift");

    // A manifest without a license is refused.
    const auto tmp = std::filesystem::temp_directory_path() / "echo_bad_model";
    std::filesystem::remove_all(tmp);
    std::filesystem::create_directories(tmp);
    std::filesystem::copy_file(std::filesystem::path(dataDir("echo-test-lowpass")) / "model.onnx", tmp / "model.onnx");
    {
        FILE* f = _wfopen((tmp / "model.json").wstring().c_str(), L"wb");
        std::fputs(R"({"format":"echo.voice-model","version":1,"name":"x","onnx":"model.onnx"})", f);
        std::fclose(f);
    }
    ai::ModelManifest bad;
    CHECK_FALSE(ai::loadManifest(tmp.wstring(), bad));
    CHECK(bad.error.find("license") != std::string::npos);
    std::filesystem::remove_all(tmp);
}

TEST_CASE("ai: without a model the effect is transparent and adds no latency") {
    ai::AiVoiceEffect e;
    e.prepare(48000, 480);
    auto x = sine(440, 48000, 4800, 0.3f);
    std::vector<float> l = x, r = x;
    for (size_t pos = 0; pos < l.size(); pos += 480) {
        float* ch[2] = {l.data() + pos, r.data() + pos};
        e.process(ch, 2, 480);
    }
    CHECK(l == x);
    CHECK(e.latencySamples() == 0);
}

TEST_CASE("ai: real ONNX inference runs off the audio thread with fixed latency") {
    auto& rt = ai::OrtRuntime::instance();
    std::string err;
    if (!rt.ensureLoaded(&err)) {
        MESSAGE("SKIPPED: ONNX Runtime not installed next to the tests (" << err << ")");
        return;
    }
    ai::ModelManifest m;
    REQUIRE(ai::loadManifest(dataDir("echo-test-lowpass"), m));
    ai::AiVoiceEffect e;
    e.prepare(48000, 480);
    e.setParam("latency", 400.0f);
    e.setModel(m, false);
    for (int i = 0; i < 300 && e.status() != ai::AiStatus::Running; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    REQUIRE(e.status() == ai::AiStatus::Running);

    // 440 Hz passes the model's 2 kHz low-pass (at half gain); 5 kHz does not.
    const int blocks = 450; // 4.5 s, paced like a real device
    auto lo = sine(440, 48000, blocks * 480, 0.3f), hi = sine(5000, 48000, blocks * 480, 0.3f);
    std::vector<float> out;
    out.reserve(blocks * 480);
    double maxCallMs = 0.0;
    uint64_t underrunAtWarm = 0;
    std::vector<float> l(480), r(480);
    for (int b = 0; b < blocks; ++b) {
        for (int i = 0; i < 480; ++i) l[i] = r[i] = lo[b * 480 + i] + hi[b * 480 + i];
        float* ch[2] = {l.data(), r.data()};
        const auto t0 = std::chrono::steady_clock::now();
        e.process(ch, 2, 480);
        maxCallMs = std::max(maxCallMs, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
        out.insert(out.end(), l.begin(), l.end());
        if (b == 200) underrunAtWarm = e.underrunSamples.load();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    MESSAGE("inference " << e.inferenceMs.load() << " ms/chunk, RTF " << e.realtimeFactor.load() << ", max process() call "
                         << maxCallMs << " ms, chunks " << e.chunksProcessed.load());
    CHECK(e.chunksProcessed.load() > 20);
    CHECK(maxCallMs < 3.0);                                 // the audio thread never waits for inference
    CHECK(e.underrunSamples.load() == underrunAtWarm);     // steady state: no fallbacks to dry
    CHECK(e.latencySamples() >= static_cast<int>(0.4 * 48000) - 1);
    const size_t n = 48000, start = out.size() - n;
    const double a440 = toneAmplitude(out, start, n, 440.0, 48000.0);
    const double a5k = toneAmplitude(out, start, n, 5000.0, 48000.0);
    CHECK(a440 == doctest::Approx(0.15).epsilon(0.08)); // 0.5 x 0.3 through the model
    CHECK(a5k < 0.01);                                    // removed by the model's low-pass
    CHECK(allFinite(out));

    // Bypass returns to the dry signal (equally delayed).
    e.clearModel();
    for (int i = 0; i < 100 && e.status() != ai::AiStatus::NoModel; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    CHECK(e.status() == ai::AiStatus::NoModel);
}

TEST_CASE("ai: a model that does not match its manifest fails gracefully") {
    auto& rt = ai::OrtRuntime::instance();
    if (!rt.ensureLoaded()) {
        MESSAGE("SKIPPED: ONNX Runtime not installed");
        return;
    }
    ai::ModelManifest m;
    REQUIRE(ai::loadManifest(dataDir("echo-test-lowpass"), m));
    m.inputName = "does_not_exist";
    ai::AiVoiceEffect e;
    e.prepare(48000, 480);
    e.setModel(m, false);
    for (int i = 0; i < 300 && e.status() == ai::AiStatus::Loading; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    CHECK(e.status() == ai::AiStatus::Error);
    CHECK(e.statusText().find("does not match") != std::string::npos);
    auto x = sine(440, 48000, 4800, 0.3f);
    std::vector<float> l = x, r = x;
    for (size_t pos = 0; pos < l.size(); pos += 480) {
        float* ch[2] = {l.data() + pos, r.data() + pos};
        e.process(ch, 2, 480);
    }
    CHECK(l == x); // audio unaffected
}

TEST_CASE("ai: the selected model is saved with the effect's state") {
    ai::AiVoiceEffect e;
    CHECK(e.saveState().empty());
    e.loadState("my-voice|gpu");
    CHECK(e.saveState() == "my-voice|gpu");
    e.loadState("my-voice|cpu");
    CHECK(e.saveState() == "my-voice|cpu");
    CHECK_FALSE(e.prefersGpu());
    // Saved before there was a choice: decided when the voice loads.
    e.loadState("my-voice");
    CHECK(e.saveState() == "my-voice");
}

TEST_CASE("ai: overload (inference slower than real time) stays bounded and recovers quickly") {
    auto& rt = ai::OrtRuntime::instance();
    std::string err;
    if (!rt.ensureLoaded(&err)) {
        MESSAGE("SKIPPED: ONNX Runtime not installed next to the tests (" << err << ")");
        return;
    }
    ai::ModelManifest m;
    REQUIRE(ai::loadManifest(dataDir("echo-test-lowpass"), m)); // 160 ms chunks
    ai::AiVoiceEffect e;
    e.prepare(48000, 480);
    e.setParam("latency", 400.0f);
    e.setModel(m, false);
    for (int i = 0; i < 300 && e.status() != ai::AiStatus::Running; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    REQUIRE(e.status() == ai::AiStatus::Running);

    std::vector<float> l(480), r(480), out;
    double maxCallMs = 0.0;
    int t = 0;
    auto run = [&](int blocks) {
        for (int b = 0; b < blocks; ++b, ++t) {
            for (int i = 0; i < 480; ++i) {
                const double s = (t * 480 + i) / 48000.0;
                l[i] = r[i] = static_cast<float>(0.3 * std::sin(2 * kPiD * 440.0 * s) + 0.3 * std::sin(2 * kPiD * 5000.0 * s));
            }
            float* ch[2] = {l.data(), r.data()};
            const auto t0 = std::chrono::steady_clock::now();
            e.process(ch, 2, 480);
            maxCallMs = std::max(maxCallMs, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
            out.insert(out.end(), l.begin(), l.end());
            std::this_thread::sleep_for(std::chrono::milliseconds(10)); // paced like a device
        }
    };
    run(200);                               // 2 s normal
    const uint64_t under0 = e.underrunSamples.load();
    e.setArtificialDelayForTesting(400);    // ~2.5x real time: cannot keep up
    run(300);                               // 3 s overloaded
    const uint64_t under1 = e.underrunSamples.load(), skipped1 = e.skippedSamples.load();
    const float rtf1 = e.realtimeFactor.load();
    e.setArtificialDelayForTesting(60);     // ~0.4x real time: can keep up again
    run(100);                               // 1 s to recover
    const uint64_t under2 = e.underrunSamples.load();
    run(300);                               // 3 s: must be clean
    const uint64_t under3 = e.underrunSamples.load();
    MESSAGE("overload: RTF " << rtf1 << ", fallback samples " << under1 - under0 << ", skipped stale input "
                             << skipped1 << "; after recovery: " << under3 - under2 << " fallback samples in 3 s; max process() "
                             << maxCallMs << " ms");
    CHECK(maxCallMs < 3.0);         // the audio thread never waits for inference
    CHECK(rtf1 > 1.0);              // the overload is visible to the UI
    CHECK(under1 > under0);         // and handled by falling back to the dry voice
    CHECK(skipped1 > 0);            // stale input was skipped, not queued
    CHECK(under3 == under2);        // back to full AI output within 1 s
    const size_t n = 48000, start = out.size() - n;
    CHECK(toneAmplitude(out, start, n, 5000.0, 48000.0) < 0.01); // model output again (its low-pass)
    CHECK(allFinite(out));
    e.clearModel();
}

TEST_CASE("ai: the converted voice lands exactly at the reported latency") {
    auto& rt = ai::OrtRuntime::instance();
    std::string err;
    if (!rt.ensureLoaded(&err)) {
        MESSAGE("SKIPPED: ONNX Runtime not installed next to the tests (" << err << ")");
        return;
    }
    ai::ModelManifest m;
    REQUIRE(ai::loadManifest(dataDir("echo-test-lowpass"), m));
    ai::AiVoiceEffect e;
    e.prepare(48000, 480);
    e.setParam("latency", 400.0f);
    e.setModel(m, false);
    for (int i = 0; i < 300 && e.status() != ai::AiStatus::Running; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    REQUIRE(e.status() == ai::AiStatus::Running);
    // 440 Hz bursts of 60 ms at irregular gaps: their envelope shows any offset.
    const int blocks = 400;
    std::vector<float> in(static_cast<size_t>(blocks) * 480), out;
    std::mt19937 rng(9);
    std::uniform_int_distribution<int> gap(4800, 14400);
    for (size_t start = 2400; start + 2880 < in.size(); start += 2880 + static_cast<size_t>(gap(rng)))
        for (size_t i = start; i < start + 2880; ++i)
            in[i] = 0.3f * static_cast<float>(std::sin(2.0 * 3.14159265358979 * 440.0 * (i / 48000.0)));
    std::vector<float> l(480), r(480);
    for (int b = 0; b < blocks; ++b) {
        std::copy_n(in.begin() + b * 480, 480, l.begin());
        std::copy_n(in.begin() + b * 480, 480, r.begin());
        float* ch[2] = {l.data(), r.data()};
        e.process(ch, 2, 480);
        out.insert(out.end(), l.begin(), l.end());
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    const int lat = e.latencySamples();
    // 1 ms envelopes over the last 2 seconds; the lag that lines them up.
    auto env = [](const std::vector<float>& x, size_t from, size_t n) {
        std::vector<double> e2(n / 48);
        for (size_t k = 0; k < e2.size(); ++k)
            for (size_t i = 0; i < 48; ++i) e2[k] += std::fabs(x[from + k * 48 + i]);
        return e2;
    };
    const size_t n = 96000, from = out.size() - n - 4800;
    const auto eo = env(out, from, n);
    int bestLag = 0;
    double best = -1.0;
    for (int lag = -100; lag <= 100; ++lag) {
        // Output at `from` should be the input `lat` earlier; try offsets around it.
        const auto ei = env(in, from - static_cast<size_t>(lat) + static_cast<size_t>(lag * 48 + 4800) - 4800, n);
        double c = 0.0;
        for (size_t k = 0; k < eo.size(); ++k) c += eo[k] * ei[k];
        if (c > best) best = c, bestLag = lag;
    }
    MESSAGE("reported latency " << lat / 48.0 << " ms; the converted voice is off by " << bestLag << " ms");
    CHECK(std::abs(bestLag) <= 1);
}
