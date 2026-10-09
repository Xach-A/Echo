// AI voice pipeline tests. These use a real ONNX Runtime when one is
// installed next to the test executable (ai\onnxruntime.dll) and the
// deterministic pipeline-test model in tests/data. Without ONNX Runtime the
// inference tests report themselves as skipped.
#include "doctest/doctest.h"

#include <chrono>
#include <filesystem>
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
}
