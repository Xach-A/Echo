// Controller, device integration (with fake devices), sample-rate/channel
// conversion, drift handling, feedback protection, persistence and presets.
#include "doctest/doctest.h"

#include <chrono>
#include <filesystem>
#include <thread>

#include "FakeDevices.h"
#include "TestUtil.h"
#include "dsp/EffectRegistry.h"
#include "session/MixerController.h"
#include "session/Paths.h"
#include "session/Presets.h"
#include "session/Serialization.h"
#include "core/WavFile.h"

using namespace echo;
using namespace echo::test;

namespace {

struct World {
    FakeBackend backend;
    std::unique_ptr<MixerController> mc;
    World() {
        backend.addDevice("mic", "USB Mic", false, 1, 48000, true);
        backend.addDevice("phones", "Headphones", true, 2, 48000, true);
        backend.addDevice("cable_in", "CABLE Input", true, 2, 48000, false, true);
        backend.addDevice("cable_out", "CABLE Output", false, 2, 48000, false, true);
        backend.cablePartners["cable_in"] = "cable_out";
        mc = std::make_unique<MixerController>(backend);
        // Routing tests measure pure test tones, which noise suppression
        // (rightly) treats as noise. The default-on behaviour has its own tests.
        mc->session().autoNoiseSuppression = false;
    }
    // Simulates one engine block of device activity: mic produces, outputs consume.
    void step(int blocks, double micFreq = 440.0) {
        for (int i = 0; i < blocks; ++i) {
            if (auto c = backend.captures["mic"]) c->produceSine(480, micFreq);
            mc->engine().processBlock();
            for (auto& [id, r] : backend.renders) r->consume(480);
        }
    }
};

double renderAmp(FakeRender& r, double freq, int ch = 0, int channels = 2, double seconds = 0.2) {
    const size_t frames = r.out.size() / channels;
    const size_t n = std::min(frames, static_cast<size_t>(seconds * 48000));
    std::vector<float> x(n);
    for (size_t i = 0; i < n; ++i) x[i] = r.out[(frames - n + i) * channels + ch];
    return toneAmplitude(x.data(), n, freq, 48000.0);
}

} // namespace

TEST_CASE("controller: default session routes the microphone to the virtual mic") {
    World w;
    w.mc->createDefaultSession();
    REQUIRE(w.mc->session().buses.size() == 2);
    REQUIRE(w.mc->session().strips.size() == 2);
    CHECK(w.mc->session().buses[1].output.deviceId == "cable_in"); // detected cable
    w.mc->start(false);
    w.step(60);
    // Mic sine reaches the cable input, not the headphones.
    CHECK(renderAmp(*w.backend.renders["cable_in"], 440.0) == doctest::Approx(0.5).epsilon(0.03));
    CHECK(renderAmp(*w.backend.renders["phones"], 440.0) < 1e-4);
    CHECK(w.mc->session().buses[0].isClock); // physical bus drives the clock
    w.mc->stop();
}

TEST_CASE("controller: one capture stream feeds several strips") {
    World w;
    w.mc->createDefaultSession();
    w.mc->start(false);
    SourceRef mic;
    mic.kind = SourceKind::CaptureDevice; // default device again
    const uint32_t second = w.mc->addStrip(StripGroup::Hardware, mic, "Mic copy");
    CHECK(w.backend.openCaptureCalls == 2); // mic + system audio, no second mic stream
    CHECK(w.mc->engine().current()->numSources == 2);
    CHECK(w.mc->sourceFor(second) == w.mc->sourceFor(w.mc->session().strips[0].id));
    w.mc->stop();
}

TEST_CASE("controller: microphone to headphones and virtual mic simultaneously, with independent gains") {
    World w;
    w.mc->createDefaultSession();
    w.mc->start(false);
    auto& mic = w.mc->session().strips[0];
    w.mc->setSend(mic.id, w.mc->session().buses[0].id, true);
    mic.rt->sends[w.mc->session().buses[0].slot].gainDb = -6.0f;
    w.step(80);
    CHECK(renderAmp(*w.backend.renders["cable_in"], 440.0) == doctest::Approx(0.5).epsilon(0.03));
    CHECK(renderAmp(*w.backend.renders["phones"], 440.0) == doctest::Approx(0.5 * dbToGain(-6.0f)).epsilon(0.03));
    w.mc->stop();
}

TEST_CASE("feedback protection: a send that loops through a cable is blocked") {
    World w;
    w.mc->createDefaultSession();
    w.mc->start(false);
    // Strip recording the cable's output side, routed to the bus that plays
    // into the same cable: a loop.
    SourceRef cable;
    cable.kind = SourceKind::CaptureDevice;
    cable.deviceId = "cable_out";
    const uint32_t sid = w.mc->addStrip(StripGroup::Virtual, cable, "Cable return");
    const BusModel& vmic = w.mc->session().buses[1];
    w.mc->setSend(sid, vmic.id, true);
    REQUIRE(w.mc->feedbackWarnings().size() == 1);
    CHECK(w.mc->feedbackWarnings()[0].stripId == sid);
    CHECK(w.mc->session().strip(sid)->rt->sends[vmic.slot].blocked.load());
    // Routing it to headphones instead is fine.
    w.mc->setSend(sid, vmic.id, false);
    w.mc->setSend(sid, w.mc->session().buses[0].id, true);
    CHECK(w.mc->feedbackWarnings().empty());

    // Loopback of the headphone device routed back to headphones: loop.
    SourceRef loop;
    loop.kind = SourceKind::RenderLoopback;
    loop.deviceId = "phones";
    const uint32_t lid = w.mc->addStrip(StripGroup::Application, loop, "What I hear");
    w.mc->setSend(lid, w.mc->session().buses[0].id, true);
    CHECK(w.mc->feedbackWarnings().size() == 1);
    w.mc->stop();
}

TEST_CASE("device loss: output disappears, engine keeps running, device reconnects") {
    World w;
    w.mc->createDefaultSession();
    w.mc->start(false);
    w.step(20);
    auto oldRender = w.backend.renders["cable_in"];
    // Unplug: stream reports Lost and the device vanishes.
    oldRender->status_ = EndpointStatus::Lost;
    w.backend.find("cable_in")->info.active = false;
    w.backend.changed = true;
    w.mc->update(10.0);
    CHECK_FALSE(w.mc->session().buses[1].online);
    w.step(10); // engine still processes without the device
    CHECK(w.mc->engine().stats().exceptions.load() == 0);
    // Replug: same ID becomes active again.
    w.backend.find("cable_in")->info.active = true;
    w.backend.changed = true;
    w.mc->update(20.0);
    CHECK(w.mc->session().buses[1].online);
    CHECK(w.backend.renders["cable_in"] != oldRender); // a fresh stream
    w.backend.renders["cable_in"]->out.clear();
    w.step(60);
    CHECK(renderAmp(*w.backend.renders["cable_in"], 440.0) == doctest::Approx(0.5).epsilon(0.03));
    w.mc->stop();
}

TEST_CASE("device rename/ID change: reconnects by name") {
    World w;
    w.mc->createDefaultSession();
    SourceRef r;
    r.kind = SourceKind::CaptureDevice;
    r.deviceId = "old-id";
    r.deviceName = "USB Mic";
    w.mc->setStripSource(w.mc->session().strips[0].id, r);
    w.mc->start(false);
    CHECK(w.mc->session().strips[0].online);
    w.mc->stop();
}

TEST_CASE("missing devices at startup: safe start, strips report status") {
    World w;
    w.mc->createDefaultSession();
    SourceRef r;
    r.kind = SourceKind::CaptureDevice;
    r.deviceId = "gone";
    r.deviceName = "Unplugged Interface";
    w.mc->setStripSource(w.mc->session().strips[0].id, r);
    w.mc->start(false);
    CHECK_FALSE(w.mc->session().strips[0].online);
    CHECK(w.mc->session().strips[0].status == "Device not connected");
    w.step(5);
    w.mc->stop();
}

TEST_CASE("app capture: process loopback strip resolves the process by name") {
    World w;
    w.mc->createDefaultSession();
    SourceRef app;
    app.kind = SourceKind::ProcessLoopback;
    app.processName = "music.exe";
    const uint32_t id = w.mc->addStrip(StripGroup::Application, app, "Music");
    w.mc->start(false);
    CHECK(w.mc->session().strip(id)->online);
    CHECK(w.mc->session().strip(id)->source.processId == 4242);
    // Learning the process ID must not reopen the capture (it caused a gap).
    const int opens = w.backend.openCaptureCalls;
    for (int i = 0; i < 6; ++i) {
        w.mc->rebuildGraph();
        w.mc->update(1.0 + i);
    }
    CHECK(w.backend.openCaptureCalls == opens);
    CHECK(w.mc->session().strip(id)->online);
    w.mc->stop();
}

TEST_CASE("sample-rate conversion: 44.1 kHz device into a 48 kHz engine and back out") {
    FakeBackend b;
    b.addDevice("mic441", "Mic 44.1", false, 1, 44100, true);
    b.addDevice("out441", "Out 44.1 6ch", true, 6, 44100, true);
    MixerController mc(b);
    mc.session().autoNoiseSuppression = false; // pure-tone measurement
    mc.createDefaultSession();
    mc.start(false);
    auto& mic = mc.session().strips[0];
    mc.setSend(mic.id, mc.session().buses[0].id, true);
    auto cap = b.captures["mic441"];
    auto ren = b.renders["out441"];
    REQUIRE(cap);
    REQUIRE(ren);
    // Device clocks: capture 441 frames and render 441 frames per 480-frame engine block.
    for (int i = 0; i < 300; ++i) {
        cap->produceSine(441, 1000.0, 0.5f);
        mc.engine().processBlock();
        ren->consume(441);
    }
    // Output at 44.1 kHz: frequency preserved, L/R on the first two of six channels.
    const size_t frames = ren->out.size() / 6;
    const size_t n = 44100;
    std::vector<float> l(n), c3(n);
    for (size_t i = 0; i < n; ++i) {
        l[i] = ren->out[(frames - n + i) * 6 + 0];
        c3[i] = ren->out[(frames - n + i) * 6 + 3];
    }
    CHECK(toneAmplitude(l.data(), n, 1000.0, 44100.0) == doctest::Approx(0.5).epsilon(0.03));
    CHECK(std::abs(cents(dominantFrequency(l.data(), n, 44100.0, 800, 1200), 1000.0)) < 2.0);
    CHECK(peak(c3.data(), n) == 0.0f); // surround channels silent
    CHECK(ren->underrunCount < 3);
    mc.stop();
}

TEST_CASE("clock drift: a capture device running 200 ppm fast stays glitch-free") {
    auto ep = std::make_shared<FakeCapture>(2, 48000);
    DeviceCaptureSource src(ep);
    src.prepare(48000, 480, 0.01);
    double acc = 0.0;
    const double perBlock = 480.0 * (1.0 + 200e-6);
    std::vector<float> out;
    for (int block = 0; block < 6000; ++block) { // 60 s
        acc += perBlock;
        const int n = static_cast<int>(acc);
        acc -= n;
        ep->produceSine(n, 997.0, 0.5f);
        src.pull(480);
        if (block >= 5000) out.insert(out.end(), src.channel(0), src.channel(0) + 480);
    }
    CHECK(src.underruns.load() == 0);
    CHECK(src.overruns.load() == 0);
    CHECK(src.driftPpm.load() == doctest::Approx(200.0).epsilon(0.25));
    // Frequency in the engine domain is the device frequency scaled by the
    // clock ratio (997 Hz * 1.0002).
    CHECK(std::abs(cents(dominantFrequency(out.data(), 48000, 48000.0, 900, 1100), 997.0 * 1.0002)) < 1.0);
}

TEST_CASE("channel conversion: 4-channel interface, strip picks channels 3/4") {
    auto ep = std::make_shared<FakeCapture>(4, 48000);
    auto src = std::make_shared<DeviceCaptureSource>(ep);
    src->prepare(48000, 480, 0.01);
    auto strip = std::make_shared<StripRuntime>();
    strip->chanFirst = 2;
    strip->chanCount = 2;
    strip->sends[0].enabled = true;
    strip->prepare(48000, 480);
    auto bus = std::make_shared<BusRuntime>();
    bus->limiterEnabled = false;
    bus->prepare(48000, 480);
    auto sink = std::make_shared<MemorySink>(48000);
    sink->prepare(48000, 480, 0.01);
    Engine e;
    auto g = std::make_unique<GraphSnapshot>();
    g->sources[0] = src.get();
    g->numSources = 1;
    g->strips[0] = StripSnap{strip.get(), 0, -1, {}};
    g->numStrips = 1;
    BusSnap bs;
    bs.rt = bus.get();
    bs.sink = 0;
    g->buses[0] = bs;
    g->numBuses = 1;
    g->sinks[0] = sink.get();
    g->numSinks = 1;
    g->keepAlive = {src, strip, bus, sink};
    e.publish(std::move(g));
    for (int i = 0; i < 40; ++i) {
        ep->produceSine(480, 500.0, 0.6f);
        e.processBlock();
    }
    const size_t n = 9600, start = sink->written() - n;
    // FakeCapture scales channel c by 1/(c+1): channel 3 -> 0.2, channel 4 -> 0.15.
    CHECK(toneAmplitude(sink->leftData(), start, n, 500.0, 48000.0) == doctest::Approx(0.6 / 3).epsilon(0.03));
    CHECK(toneAmplitude(sink->rightData(), start, n, 500.0, 48000.0) == doctest::Approx(0.6 / 4).epsilon(0.03));
}

TEST_CASE("serialization: session round-trips including routing and effect parameters") {
    World w;
    w.mc->createDefaultSession();
    Session& s = w.mc->session();
    s.strips[0].rt->faderDb = -7.5f;
    s.strips[0].rt->pan = 0.25f;
    s.strips[0].rt->sends[s.buses[0].slot].enabled = true;
    s.strips[0].rt->sends[s.buses[0].slot].gainDb = -3.0f;
    s.strips[0].rt->sends[s.buses[0].slot].preFader = true;
    auto comp = w.mc->addEffect({false, s.strips[0].id}, "compressor");
    comp->setParam("threshold", -31.0f);
    comp->bypass = true;
    w.mc->addEffect({false, s.strips[0].id}, "pitch")->setParam("semitones", -4.0f);
    w.mc->addEffect({true, s.buses[1].id}, "limiter")->setParam("ceiling", -2.0f);
    s.buses[1].rt->faderDb = -2.0f;
    s.engine.blockSize = 256;
    s.ui.windowW = 1234;

    const json j = sessionToJson(s);
    Session r;
    std::vector<std::string> warnings;
    REQUIRE(sessionFromJson(json::parse(j.dump()), r, &warnings));
    CHECK(warnings.empty());
    REQUIRE(r.strips.size() == s.strips.size());
    CHECK(r.strips[0].name == s.strips[0].name);
    CHECK(r.strips[0].rt->faderDb.load() == doctest::Approx(-7.5f));
    CHECK(r.strips[0].rt->pan.load() == doctest::Approx(0.25f));
    const int slot = r.buses[0].slot;
    CHECK(r.strips[0].rt->sends[slot].enabled.load());
    CHECK(r.strips[0].rt->sends[slot].gainDb.load() == doctest::Approx(-3.0f));
    CHECK(r.strips[0].rt->sends[slot].preFader.load());
    REQUIRE(r.strips[0].chain.size() == 2);
    CHECK(std::string(r.strips[0].chain[0]->typeId()) == "compressor");
    CHECK(r.strips[0].chain[0]->param(r.strips[0].chain[0]->paramIndex("threshold")) == doctest::Approx(-31.0f));
    CHECK(r.strips[0].chain[0]->bypass.load());
    CHECK(r.strips[0].chain[1]->param(r.strips[0].chain[1]->paramIndex("semitones")) == doctest::Approx(-4.0f));
    REQUIRE(r.buses[1].chain.size() == 1);
    CHECK(r.buses[1].rt->faderDb.load() == doctest::Approx(-2.0f));
    CHECK(r.engine.blockSize == 256);
    CHECK(r.ui.windowW == 1234);
    CHECK(r.buses[1].output.deviceId == "cable_in");
}

TEST_CASE("serialization: unknown effects and fields are tolerated") {
    json j = json::parse(R"({"format":"echo.session","version":99,"futureField":1,
        "buses":[{"id":5,"slot":0,"name":"A","kind":"physical"}],
        "strips":[{"id":7,"name":"S","chain":[{"type":"from_the_future"},{"type":"gain","params":{"gain":-3,"nope":1}}]}]})");
    Session s;
    std::vector<std::string> warnings;
    REQUIRE(sessionFromJson(j, s, &warnings));
    CHECK(warnings.size() == 1);
    REQUIRE(s.strips[0].chain.size() == 1);
    CHECK(s.strips[0].chain[0]->param(0) == doctest::Approx(-3.0f));
    CHECK_FALSE(sessionFromJson(json::parse(R"({"format":"something else"})"), s, nullptr));
}

TEST_CASE("persistence: atomic save, reload and corrupt-file recovery") {
    const auto dir = std::filesystem::temp_directory_path() / "echo_test_persist";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    const std::wstring file = (dir / "session.json").wstring();
    json a = {{"format", "echo.session"}, {"value", 1}};
    json b = {{"format", "echo.session"}, {"value", 2}};
    std::string err;
    REQUIRE(saveJsonFile(file, a, &err));
    REQUIRE(saveJsonFile(file, b, &err)); // a becomes the .bak
    json got;
    REQUIRE(loadJsonFile(file, got, &err));
    CHECK(got["value"] == 2);
    { // corrupt the main file
        FILE* f = _wfopen(file.c_str(), L"wb");
        std::fputs("{ not json", f);
        std::fclose(f);
    }
    REQUIRE(loadJsonFile(file, got, &err));
    CHECK(got["value"] == 1); // recovered from backup
    std::filesystem::remove_all(dir);
}

TEST_CASE("presets: every factory preset uses known effects and parameters") {
    for (const auto& p : factoryChainPresets()) {
        CAPTURE(p.name);
        REQUIRE(p.chain.is_array());
        CHECK(p.chain.size() >= 1);
        for (const auto& fx : p.chain) {
            const std::string type = fx.value("type", "");
            auto e = EffectRegistry::instance().create(type);
            CAPTURE(type);
            REQUIRE(e != nullptr);
            for (auto it = fx["params"].begin(); it != fx["params"].end(); ++it) {
                CAPTURE(it.key());
                CHECK(e->paramIndex(it.key()) >= 0);
            }
        }
    }
    const char* required[] = {"Clean Microphone", "Streaming Voice", "Podcast Voice", "Gaming Voice", "Radio Voice",
                              "Natural Pitch Correction", "Hard Autotune", "Deep Voice", "Chipmunk", "Robot",
                              "Monster", "Telephone", "Noise-Reduced Microphone"};
    PresetLibrary lib;
    lib.load(L"");
    for (const char* n : required) {
        CAPTURE(n);
        CHECK(lib.find(n) != nullptr);
    }
}

TEST_CASE("presets: applying a chain preset changes the DSP; user presets save, rename, duplicate, delete") {
    const auto dir = std::filesystem::temp_directory_path() / "echo_test_presets";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    PresetLibrary lib;
    lib.load(dir.wstring());
    World w;
    w.mc->createDefaultSession();
    w.mc->start(false);
    const ChainPreset* deep = lib.find("Deep Voice");
    REQUIRE(deep);
    std::vector<std::string> warn;
    const uint32_t mic = w.mc->session().strips[0].id;
    w.mc->setChain({false, mic}, chainFromJson(deep->chain, &warn));
    w.step(150, 220.0);
    // Deep Voice lowers pitch by 5 semitones: 220 Hz -> ~164.8 Hz.
    auto& out = *w.backend.renders["cable_in"];
    const size_t frames = out.out.size() / 2, n = 24000;
    std::vector<float> l(n);
    for (size_t i = 0; i < n; ++i) l[i] = out.out[(frames - n + i) * 2];
    CHECK(std::abs(cents(dominantFrequency(l.data(), n, 48000.0, 100, 400), 220.0 * std::pow(2.0, -5.0 / 12.0))) < 10.0);

    std::string err;
    REQUIRE(lib.saveUser("My Voice", "User", "test", chainToJson(w.mc->session().strips[0].chain), &err));
    REQUIRE(lib.duplicate("My Voice", &err));
    REQUIRE(lib.rename("My Voice", "My Voice 2", &err));
    PresetLibrary reload;
    reload.load(dir.wstring());
    CHECK(reload.find("My Voice 2") != nullptr);
    CHECK(reload.find("My Voice (copy)") != nullptr);
    CHECK(reload.find("My Voice") == nullptr);
    REQUIRE(reload.remove("My Voice 2", &err));
    CHECK_FALSE(reload.remove("Deep Voice", &err)); // factory presets are read-only
    w.mc->stop();
    std::filesystem::remove_all(dir);
}

TEST_CASE("scene presets: voice + music to the virtual mic, music never in mic for headphones-only") {
    World w;
    w.mc->createDefaultSession();
    w.mc->start(false);
    Session& s = w.mc->session();
    const int phones = s.buses[0].slot, vmic = s.buses[1].slot;
    applyScenePreset(*w.mc, ScenePreset::VoicePlusMusicToMic);
    CHECK(s.strips[0].rt->sends[vmic].enabled.load());
    CHECK(s.strips[1].rt->sends[vmic].enabled.load());
    CHECK(s.strips[1].rt->sends[phones].enabled.load());
    applyScenePreset(*w.mc, ScenePreset::HeadphonesOnlyMusic);
    CHECK_FALSE(s.strips[1].rt->sends[vmic].enabled.load());
    CHECK(s.strips[1].rt->sends[phones].enabled.load());
    w.mc->stop();
}

TEST_CASE("stress: rapid routing and chain changes while processing") {
    World w;
    w.mc->createDefaultSession();
    w.mc->start(false);
    const uint32_t mic = w.mc->session().strips[0].id;
    const char* types[] = {"eq", "compressor", "reverb", "pitch", "autotune", "denoise", "delay"};
    for (int i = 0; i < 300; ++i) {
        if (i % 3 == 0) w.mc->addEffect({false, mic}, types[i % 7]);
        if (i % 5 == 0 && !w.mc->session().strips[0].chain.empty())
            w.mc->removeEffect({false, mic}, w.mc->session().strips[0].chain.front()->instanceId);
        w.mc->setSend(mic, w.mc->session().buses[i % 2].id, i % 4 != 0);
        w.step(2);
        w.mc->update(i * 0.01);
    }
    CHECK(w.mc->engine().stats().exceptions.load() == 0);
    w.mc->stop();
}

TEST_CASE("stress: repeated start/stop of the real engine thread") {
    World w;
    w.mc->createDefaultSession();
    for (int i = 0; i < 40; ++i) {
        w.mc->start(true);
        std::this_thread::sleep_for(std::chrono::milliseconds(15));
        w.mc->stop();
    }
    CHECK(w.mc->engine().stats().exceptions.load() == 0);
    CHECK_FALSE(w.mc->running());
}

TEST_CASE("stress: graph edits while the real engine thread is running") {
    World w;
    w.mc->createDefaultSession();
    w.mc->start(true);
    const uint32_t mic = w.mc->session().strips[0].id;
    const char* types[] = {"pitch", "reverb", "autotune", "denoise", "eq", "limiter"};
    for (int i = 0; i < 120; ++i) {
        if (auto c = w.backend.captures["mic"]) c->produceSine(480, 330.0);
        w.mc->addEffect({false, mic}, types[i % 6]);
        if (w.mc->session().strips[0].chain.size() > 6)
            w.mc->removeEffect({false, mic}, w.mc->session().strips[0].chain.front()->instanceId);
        w.mc->setSend(mic, w.mc->session().buses[i % 2].id, (i % 3) != 0);
        w.mc->update(i * 0.005);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    const uint64_t blocks = w.mc->engine().stats().blocks.load();
    w.mc->stop();
    CHECK(blocks > 20);
    CHECK(w.mc->engine().stats().exceptions.load() == 0);
}

TEST_CASE("recording: bus and pre-fader strip recordings are written correctly (incl. resampling)") {
    for (int bits : {16, 24, 32}) {
        CAPTURE(bits);
        World w;
        w.mc->createDefaultSession();
        const auto dir = std::filesystem::temp_directory_path() / ("echo_rec_test_" + std::to_string(bits));
        std::filesystem::remove_all(dir);
        w.mc->session().recorder.folder = dir.string();
        w.mc->session().recorder.bitDepth = bits;
        w.mc->session().recorder.sampleRate = bits == 16 ? 44100 : 0; // also test file-rate conversion
        w.mc->start(false);
        const auto& mic = w.mc->session().strips[0];
        mic.rt->faderDb = -6.0f; // post-fader recordings see this, pre-fader ones do not
        std::vector<RecordingTarget> targets = {{RecordTap::Bus, w.mc->session().buses[1].id, "vmic"},
                                                {RecordTap::StripPre, mic.id, "mic pre"}};
        std::string err;
        REQUIRE(w.mc->startRecording(targets, &err));
        w.step(200, 440.0); // 2 s of a 0.5 amplitude 440 Hz "voice"
        std::this_thread::sleep_for(std::chrono::milliseconds(100)); // let the writer drain
        const RecordingStatus st = w.mc->recorder().status();
        w.mc->stopRecording();
        REQUIRE(st.files.size() == 2);
        CHECK(st.droppedFrames == 0);
        for (size_t f = 0; f < 2; ++f) {
            WavData wav;
            REQUIRE(readWav(st.files[f], wav, &err));
            const int rate = bits == 16 ? 44100 : 48000;
            CHECK(wav.sampleRate == rate);
            CHECK(wav.channels == 2);
            const size_t frames = wav.samples.size() / 2;
            CHECK(frames > static_cast<size_t>(rate * 1.8));
            std::vector<float> left(frames);
            for (size_t i = 0; i < frames; ++i) left[i] = wav.samples[2 * i];
            const size_t n = static_cast<size_t>(rate * 0.5);
            const double a = toneAmplitude(left.data() + frames - n, n, 440.0, rate);
            const double expect = f == 0 ? 0.5 * dbToGain(-6.0f) : 0.5; // bus is post-fader, strip tap pre-fader
            CHECK(a == doctest::Approx(expect).epsilon(0.03));
            CHECK(std::abs(cents(dominantFrequency(left.data() + frames - n, n, rate, 300, 600), 440.0)) < 3.0);
        }
        w.mc->stop();
        std::filesystem::remove_all(dir);
    }
}

namespace {
const ChainPreset& factoryPreset(const std::vector<ChainPreset>& all, const char* name) {
    for (const auto& p : all)
        if (p.name == name) return p;
    FAIL("missing factory preset " << name);
    return all.front();
}
int countType(const std::vector<std::shared_ptr<Effect>>& chain, const char* type) {
    int n = 0;
    for (const auto& e : chain) n += std::string(e->typeId()) == type;
    return n;
}
} // namespace

TEST_CASE("noise suppression: on by default for microphones; the user's removal is respected") {
    World w;
    w.mc->session().autoNoiseSuppression = true;
    w.mc->createDefaultSession();
    const uint32_t mic = w.mc->session().strips[0].id;
    auto micChain = [&]() -> std::vector<std::shared_ptr<Effect>>& { return w.mc->session().strip(mic)->chain; };
    REQUIRE(micChain().size() == 1);
    CHECK(std::string(micChain()[0]->typeId()) == "voice_denoise");
    CHECK(w.mc->session().strips[1].chain.empty()); // desktop audio is not a microphone

    // New microphone inputs get it; a virtual cable's output does not.
    SourceRef micRef, cableRef;
    micRef.kind = cableRef.kind = SourceKind::CaptureDevice;
    micRef.deviceId = "mic";
    cableRef.deviceId = "cable_out";
    const uint32_t m2 = w.mc->addStrip(StripGroup::Hardware, micRef, "Mic 2");
    const uint32_t c2 = w.mc->addStrip(StripGroup::Hardware, cableRef, "Cable");
    CHECK(countType(w.mc->session().strip(m2)->chain, "voice_denoise") == 1);
    CHECK(w.mc->session().strip(c2)->chain.empty());

    // Voice-changer presets combine with it: kept first, settings untouched.
    const auto presets = factoryChainPresets();
    micChain()[0]->setParam("strength", 70.0f);
    std::vector<std::string> warn;
    w.mc->applyPresetChain({false, mic}, chainFromJson(factoryPreset(presets, "Deep Voice").chain, &warn));
    REQUIRE(micChain().size() >= 2);
    CHECK(std::string(micChain()[0]->typeId()) == "voice_denoise");
    CHECK(micChain()[0]->param(micChain()[0]->paramIndex("strength")) == doctest::Approx(70.0f));
    CHECK(countType(micChain(), "pitch") == 1);
    // Echo cancellation is kept too, ahead of the suppressor.
    w.mc->addEffect({false, mic}, "echo_cancel", 0);
    w.mc->applyPresetChain({false, mic}, chainFromJson(factoryPreset(presets, "Robot").chain, &warn));
    CHECK(std::string(micChain()[0]->typeId()) == "echo_cancel");
    CHECK(std::string(micChain()[1]->typeId()) == "voice_denoise");
    CHECK(countType(micChain(), "robot") == 1);
    // Voice-processing presets bring their own suppressor: never duplicated.
    w.mc->applyPresetChain({false, mic}, chainFromJson(factoryPreset(presets, "Streaming Voice").chain, &warn));
    CHECK(countType(micChain(), "voice_denoise") == 1);
    CHECK(countType(micChain(), "echo_cancel") == 1);
    // Bus chains are not affected.
    w.mc->applyPresetChain({true, w.mc->session().buses[0].id},
                           chainFromJson(factoryPreset(presets, "Music Enhancer").chain, &warn));
    CHECK(countType(w.mc->session().buses[0].chain, "voice_denoise") == 0);

    // The user removes it: presets and reloading keep it removed.
    for (const char* type : {"voice_denoise", "echo_cancel"})
        for (auto& e : std::vector<std::shared_ptr<Effect>>(micChain()))
            if (std::string(e->typeId()) == type) w.mc->removeEffect({false, mic}, e->instanceId);
    w.mc->applyPresetChain({false, mic}, chainFromJson(factoryPreset(presets, "Deep Voice").chain, &warn));
    CHECK(countType(micChain(), "voice_denoise") == 0);
    Session reloaded;
    REQUIRE(sessionFromJson(sessionToJson(w.mc->session()), reloaded, &warn));
    CHECK(reloaded.autoNoiseSuppression);
    w.mc->replaceSession(std::move(reloaded));
    CHECK(countType(micChain(), "voice_denoise") == 0);

    // Option off: new microphones start without it.
    w.mc->session().autoNoiseSuppression = false;
    const uint32_t m3 = w.mc->addStrip(StripGroup::Hardware, micRef, "Mic 3");
    CHECK(w.mc->session().strip(m3)->chain.empty());
    Session s2;
    REQUIRE(sessionFromJson(sessionToJson(w.mc->session()), s2, &warn));
    CHECK_FALSE(s2.autoNoiseSuppression);
}

TEST_CASE("noise suppression: sessions saved by older versions are upgraded once") {
    World w;
    w.mc->session().autoNoiseSuppression = true;
    w.mc->createDefaultSession();
    Session& cur = w.mc->session();
    cur.strips[0].chain.clear();
    cur.strips[0].chain.push_back(w.mc->createEffect("compressor"));
    cur.configVersion = 1; // as written by Echo 0.1.0
    json j = sessionToJson(cur);
    j.erase("autoNoiseSuppression");
    Session old;
    std::vector<std::string> warn;
    REQUIRE(sessionFromJson(j, old, &warn));
    CHECK(old.configVersion == 1);
    w.mc->replaceSession(std::move(old));
    const auto& chain = w.mc->session().strips[0].chain;
    REQUIRE(chain.size() == 2);
    CHECK(std::string(chain[0]->typeId()) == "voice_denoise"); // before the compressor
    CHECK(std::string(chain[1]->typeId()) == "compressor");
    CHECK(w.mc->session().configVersion == kSessionConfigVersion);
    CHECK(w.mc->session().strips[1].chain.empty());
}

TEST_CASE("virtual cables: a cable installed later is picked up by an unassigned virtual-mic bus") {
    FakeBackend backend;
    backend.addDevice("mic", "USB Mic", false, 1, 48000, true);
    backend.addDevice("phones", "Headphones", true, 2, 48000, true);
    MixerController mc(backend);
    mc.session().autoNoiseSuppression = false;
    mc.createDefaultSession();
    REQUIRE(mc.session().buses.size() == 2);
    REQUIRE(mc.session().buses[1].kind == BusKind::Virtual);
    CHECK(mc.session().buses[1].output.deviceId.empty()); // no cable yet
    mc.start(false);
    mc.update(1.0);
    CHECK(mc.session().buses[1].output.deviceId.empty());
    CHECK(mc.takeNotices().empty());

    // The user installs VB-CABLE separately: two playback endpoints feed one
    // recording side; the 16-channel one is enumerated first.
    backend.addDevice("cable16", "CABLE In 16ch (VB-Audio Virtual Cable)", true, 16, 48000, false, true);
    backend.devs.back().info.isCableSecondary = true;
    backend.addDevice("cable_in", "CABLE Input (VB-Audio Virtual Cable)", true, 2, 48000, false, true);
    backend.addDevice("cable_out", "CABLE Output (VB-Audio Virtual Cable)", false, 2, 48000, false, true);
    backend.cablePartners["cable16"] = "cable_out";
    backend.cablePartners["cable_in"] = "cable_out";
    backend.changed = true;
    mc.update(2.0);
    CHECK(mc.session().buses[1].output.deviceId == "cable_in");
    const auto notes = mc.takeNotices();
    REQUIRE(notes.size() == 1);
    CHECK(notes[0].find("CABLE Output") != std::string::npos);
    // The mic now reaches the cable.
    for (int i = 0; i < 60; ++i) {
        backend.captures["mic"]->produceSine(480, 440.0);
        mc.engine().processBlock();
        for (auto& [id, r] : backend.renders) r->consume(480);
    }
    REQUIRE(backend.renders.count("cable_in"));
    CHECK(renderAmp(*backend.renders["cable_in"], 440.0) > 0.4);

    // Further device changes never re-assign a bus the user has set.
    OutputRef other;
    other.deviceId = "phones";
    other.deviceName = "Headphones";
    mc.setBusOutput(mc.session().buses[1].id, other);
    backend.changed = true;
    mc.update(3.0);
    CHECK(mc.session().buses[1].output.deviceId == "phones");
    CHECK(mc.takeNotices().empty());

    // Feedback protection knows the 16-channel route into CABLE Output too.
    SourceRef cableSrc;
    cableSrc.kind = SourceKind::CaptureDevice;
    cableSrc.deviceId = "cable_out";
    const uint32_t vs = mc.addStrip(StripGroup::Virtual, cableSrc, "Virtual speaker");
    OutputRef to16;
    to16.deviceId = "cable16";
    to16.deviceName = "CABLE In 16ch (VB-Audio Virtual Cable)";
    mc.setBusOutput(mc.session().buses[1].id, to16);
    mc.setSend(vs, mc.session().buses[1].id, true);
    CHECK(mc.session().strip(vs)->rt->sends[mc.session().buses[1].slot].blocked.load());
    mc.stop();
}

TEST_CASE("devices: opening is asynchronous; ready, failed and hung devices are handled without waiting") {
    World w;
    w.backend.asyncOpen = true;
    w.mc->createDefaultSession();
    w.mc->start(false);
    StripModel& mic = w.mc->session().strips[0];
    // Nothing has opened yet: the controller returned immediately.
    REQUIRE(w.backend.captures.count("mic"));
    CHECK_FALSE(mic.online);
    CHECK(mic.status == "Starting...");
    w.step(5); // the engine keeps running meanwhile
    // The devices become ready: they join the graph on the next update.
    w.backend.captures["mic"]->status_ = EndpointStatus::Running;
    for (auto& [id, r] : w.backend.renders) r->status_ = EndpointStatus::Running;
    w.mc->update(1.0);
    CHECK(w.mc->session().strips[0].online);
    w.step(60);
    CHECK(renderAmp(*w.backend.renders["cable_in"], 440.0) == doctest::Approx(0.5).epsilon(0.03));

    // A device that fails while opening reports Windows' reason.
    SourceRef cable;
    cable.kind = SourceKind::CaptureDevice;
    cable.deviceId = "cable_out";
    const uint32_t vs = w.mc->addStrip(StripGroup::Virtual, cable, "Virtual speaker");
    auto failing = w.backend.captures["cable_out"];
    REQUIRE(failing);
    failing->error_ = "Device is in use by another application";
    failing->status_ = EndpointStatus::Error;
    w.mc->update(2.0);
    CHECK_FALSE(w.mc->session().strip(vs)->online);
    CHECK(w.mc->session().strip(vs)->status == "Device is in use by another application");
    // ...and is retried later.
    const int opens = w.backend.openCaptureCalls;
    w.mc->update(6.0);
    CHECK(w.backend.openCaptureCalls > opens);

    // A device that never finishes opening times out instead of hanging.
    w.mc->setOpenTimeoutForTesting(0.05);
    auto hung = w.backend.captures["cable_out"];
    REQUIRE(hung->status_ == EndpointStatus::Starting);
    std::this_thread::sleep_for(std::chrono::milliseconds(80));
    w.mc->update(6.5);
    CHECK(w.mc->session().strip(vs)->status == "The device did not respond (timed out)");
    w.mc->stop();
}

TEST_CASE("devices: inputs and outputs set to the Windows default follow a default change") {
    World w;
    w.backend.addDevice("mic2", "Headset Mic", false, 1, 48000, false);
    w.backend.addDevice("phones2", "USB Headset", true, 2, 48000, false);
    w.mc->createDefaultSession(); // microphone and headphones bus use "Windows default"
    w.mc->start(false);
    w.step(30);
    REQUIRE(w.backend.captures.count("mic"));
    // The user picks the headset as default recording and playback device.
    for (auto& d : w.backend.devs) {
        if (d.info.id == "mic") d.info.isDefault = false;
        if (d.info.id == "mic2") d.info.isDefault = true;
        if (d.info.id == "phones") d.info.isDefault = false;
        if (d.info.id == "phones2") d.info.isDefault = true;
    }
    w.backend.changed = true;
    w.mc->update(1.0);
    REQUIRE(w.backend.captures.count("mic2"));
    REQUIRE(w.backend.renders.count("phones2"));
    CHECK(w.mc->session().strips[0].online);
    // Audio now comes from the new microphone and plays on the new output.
    w.mc->setSend(w.mc->session().strips[0].id, w.mc->session().buses[0].id, true);
    for (int i = 0; i < 80; ++i) {
        w.backend.captures["mic2"]->produceSine(480, 330.0);
        w.mc->engine().processBlock();
        for (auto& [id, r] : w.backend.renders) r->consume(480);
    }
    CHECK(renderAmp(*w.backend.renders["phones2"], 330.0) > 0.3);
    CHECK(renderAmp(*w.backend.renders["cable_in"], 330.0) > 0.3);
    w.mc->stop();
}
