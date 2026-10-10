// Push to talk / mute hotkeys (bindings, the talk gate, key capture, the
// engine's microphone gate) and the recorder's no-effects tap and file list.
#include "doctest/doctest.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <vector>

#include "TestUtil.h"
#include "core/Engine.h"
#include "core/Recorder.h"
#include "core/WavFile.h"
#include "dsp/EffectRegistry.h"
#include "session/Hotkeys.h"
#include "session/Serialization.h"

using namespace echo;
using namespace echo::test;

namespace {

struct KeyTable final : KeyStateReader {
    bool down[256] = {};
    bool isDown(int k) const override { return k > 0 && k < 256 && down[k]; }
};

constexpr int kSr = 48000;
constexpr int kBlock = 480;

} // namespace

TEST_CASE("hotkeys: a binding is held only with its modifiers") {
    KeyTable k;
    const KeyBinding ctrlM{'M', KeyModCtrl};
    k.down['M'] = true;
    CHECK_FALSE(bindingHeld(ctrlM, k));
    k.down[vk::RControl] = true;
    CHECK(bindingHeld(ctrlM, k));
    k.down[vk::RControl] = false;
    k.down[vk::LControl] = true;
    CHECK(bindingHeld(ctrlM, k));
    // Extra modifiers do not block it (like Discord's push to talk).
    k.down[vk::LShift] = true;
    CHECK(bindingHeld(ctrlM, k));

    const KeyBinding rightCtrl{vk::RControl, 0};
    CHECK_FALSE(bindingHeld(rightCtrl, k)); // only the left one is down
    k.down[vk::RControl] = true;
    CHECK(bindingHeld(rightCtrl, k));
    CHECK_FALSE(bindingHeld(KeyBinding{}, k));
}

TEST_CASE("hotkeys: push to talk opens on press and closes after the release delay") {
    TalkGate g;
    g.configure(TalkMode::PushToTalk, 200, true);
    CHECK_FALSE(g.update(false, 0.0)); // closed until the key is pressed
    CHECK(g.update(true, 0.10));
    CHECK(g.update(true, 0.15));
    CHECK(g.update(false, 0.20));      // released: still open...
    CHECK(g.update(false, 0.39));      // ...for 200 ms
    CHECK_FALSE(g.update(false, 0.41));
    CHECK(g.update(true, 0.50));       // and again on the next press

    TalkGate quick;
    quick.configure(TalkMode::PushToTalk, 0, true);
    CHECK(quick.update(true, 1.0));
    CHECK_FALSE(quick.update(false, 1.005));
}

TEST_CASE("hotkeys: push to mute, off, and push to talk without a key") {
    TalkGate g;
    g.configure(TalkMode::PushToMute, 200, true);
    CHECK(g.update(false, 0.0));
    CHECK_FALSE(g.update(true, 0.1));
    CHECK(g.update(false, 0.2)); // no delay when unmuting

    TalkGate off;
    off.configure(TalkMode::Off, 200, true);
    CHECK(off.update(false, 0.0));
    CHECK(off.update(true, 0.1));

    // Without a key, push to talk must not leave the microphone closed forever.
    TalkGate noKey;
    noKey.configure(TalkMode::PushToTalk, 200, false);
    CHECK(noKey.update(false, 0.0));

    // Switching modes starts from the new mode's resting state.
    g.configure(TalkMode::PushToTalk, 200, true);
    CHECK_FALSE(g.open());
}

TEST_CASE("hotkeys: the mute key toggles once per press") {
    PressDetector d;
    d.reset(false);
    CHECK(d.update(true));
    CHECK_FALSE(d.update(true)); // held: no repeat
    CHECK_FALSE(d.update(false));
    CHECK(d.update(true));
    // A key that was already held when the detector was set up does not count.
    PressDetector held;
    held.reset(true);
    CHECK_FALSE(held.update(true));
}

TEST_CASE("hotkeys: capturing a new binding") {
    KeyTable k;
    KeyCapture c;

    SUBCASE("the click that started it is ignored; Ctrl+M becomes the binding") {
        k.down[vk::LButton] = true;
        c.start(k);
        CHECK(c.poll(k) == KeyCapture::Result::Waiting);
        k.down[vk::LButton] = false;
        CHECK(c.poll(k) == KeyCapture::Result::Waiting);
        k.down[vk::LControl] = true;
        CHECK(c.poll(k) == KeyCapture::Result::Waiting);
        k.down['M'] = true;
        REQUIRE(c.poll(k) == KeyCapture::Result::Done);
        CHECK(c.result() == KeyBinding{'M', KeyModCtrl});
        CHECK_FALSE(c.active());
    }
    SUBCASE("a modifier pressed and released on its own") {
        c.start(k);
        k.down[vk::RControl] = true;
        CHECK(c.poll(k) == KeyCapture::Result::Waiting);
        k.down[vk::RControl] = false;
        REQUIRE(c.poll(k) == KeyCapture::Result::Done);
        CHECK(c.result() == KeyBinding{vk::RControl, 0});
    }
    SUBCASE("mouse side button; left and right click are never bound") {
        c.start(k);
        k.down[vk::RButton] = true;
        CHECK(c.poll(k) == KeyCapture::Result::Waiting);
        k.down[vk::XButton2] = true;
        REQUIRE(c.poll(k) == KeyCapture::Result::Done);
        CHECK(c.result() == KeyBinding{vk::XButton2, 0});
    }
    SUBCASE("a key held at the start counts only after it is pressed again") {
        k.down['V'] = true;
        c.start(k);
        CHECK(c.poll(k) == KeyCapture::Result::Waiting);
        k.down['V'] = false;
        CHECK(c.poll(k) == KeyCapture::Result::Waiting);
        k.down['V'] = true;
        REQUIRE(c.poll(k) == KeyCapture::Result::Done);
        CHECK(c.result() == KeyBinding{'V', 0});
    }
    SUBCASE("Escape cancels") {
        c.start(k);
        k.down[vk::Escape] = true;
        CHECK(c.poll(k) == KeyCapture::Result::Cancelled);
        CHECK_FALSE(c.active());
    }
}

TEST_CASE("hotkeys: binding names") {
    CHECK(keyBindingName(KeyBinding{'M', KeyModCtrl | KeyModShift}) == "Ctrl+Shift+M");
    CHECK(keyBindingName(KeyBinding{vk::XButton2, 0}) == "Mouse 5");
    CHECK(keyBindingName(KeyBinding{vk::XButton1, 0}) == "Mouse 4");
    CHECK(keyBindingName(KeyBinding{vk::RControl, 0}) == "Right Ctrl");
    CHECK(keyBindingName(KeyBinding{0x7C, 0}) == "F13");
    CHECK(keyBindingName(KeyBinding{}) == "Not set");
}

TEST_CASE("hotkeys: settings survive saving and loading; bad values are dropped") {
    Session s;
    s.hotkeys.talkMode = TalkMode::PushToTalk;
    s.hotkeys.talkKey = {vk::XButton2, 0};
    s.hotkeys.muteKey = {'M', KeyModCtrl | KeyModShift};
    s.hotkeys.releaseDelayMs = 350;
    s.ui.closeToTray = true;
    s.ui.accent = "coral";
    Session r;
    REQUIRE(sessionFromJson(json::parse(sessionToJson(s).dump()), r, nullptr));
    CHECK(r.hotkeys.talkMode == TalkMode::PushToTalk);
    CHECK(r.hotkeys.talkKey == s.hotkeys.talkKey);
    CHECK(r.hotkeys.muteKey == s.hotkeys.muteKey);
    CHECK(r.hotkeys.releaseDelayMs == 350);
    CHECK(r.ui.closeToTray);
    CHECK(r.ui.accent == "coral");

    json j = sessionToJson(s);
    j["hotkeys"]["talkMode"] = 9;
    j["hotkeys"]["talkKey"]["key"] = vk::LButton; // left click is never a hotkey
    j["hotkeys"]["releaseDelayMs"] = 99999;
    REQUIRE(sessionFromJson(j, r, nullptr));
    CHECK(r.hotkeys.talkMode == TalkMode::PushToMute); // clamped into range
    CHECK(r.hotkeys.talkKey.empty());
    CHECK(r.hotkeys.releaseDelayMs == 2000);

    // Sessions saved before hotkeys existed load with them off.
    j.erase("hotkeys");
    REQUIRE(sessionFromJson(j, r, nullptr));
    CHECK(r.hotkeys.talkMode == TalkMode::Off);
    CHECK(r.hotkeys.talkKey.empty());
}

TEST_CASE("engine: the push-to-talk gate silences microphone strips only") {
    auto toneA = std::make_shared<ToneSource>();
    auto toneB = std::make_shared<ToneSource>();
    toneA->frequency = 500.0f;
    toneB->frequency = 700.0f;
    for (auto& t : {toneA, toneB}) {
        t->levelDb = -12.0f;
        t->prepare(kSr, kBlock, double(kBlock) / kSr);
    }
    auto mic = std::make_shared<StripRuntime>();
    auto app = std::make_shared<StripRuntime>();
    mic->talkGated = true;
    for (auto& s : {mic, app}) {
        s->sends[0].enabled = true;
        s->prepare(kSr, kBlock);
    }
    auto bus = std::make_shared<BusRuntime>();
    bus->limiterEnabled = false;
    bus->prepare(kSr, kBlock);
    auto sink = std::make_shared<MemorySink>(kSr);
    sink->prepare(kSr, kBlock, double(kBlock) / kSr);
    Engine e;
    e.setFormat(kSr, kBlock);
    auto g = std::make_unique<GraphSnapshot>();
    g->sources[0] = toneA.get();
    g->sources[1] = toneB.get();
    g->numSources = 2;
    g->strips[0] = StripSnap{mic.get(), 0, -1, {}};
    g->strips[1] = StripSnap{app.get(), 1, -1, {}};
    g->numStrips = 2;
    BusSnap bs;
    bs.rt = bus.get();
    bs.sink = 0;
    g->buses[0] = bs;
    g->numBuses = 1;
    g->sinks[0] = sink.get();
    g->numSinks = 1;
    g->keepAlive = {toneA, toneB, mic, app, bus, sink};
    e.publish(std::move(g));

    auto runAndMeasure = [&](double freq) {
        sink->rewind();
        for (int i = 0; i < 30; ++i) e.processBlock(); // 300 ms; the gate ramps in 10 ms
        const size_t n = 9600;
        return toneAmplitude(sink->leftData(), sink->written() - n, n, freq, kSr);
    };
    const double level = dbToGain(-12.0f);
    CHECK(runAndMeasure(500.0) == doctest::Approx(level).epsilon(0.02)); // gate open by default

    e.setMicGate(false);
    CHECK(runAndMeasure(500.0) < 1e-4);                                  // microphone silent
    CHECK(runAndMeasure(700.0) == doctest::Approx(level).epsilon(0.02)); // app audio unaffected

    e.setMicGate(true);
    CHECK(runAndMeasure(500.0) == doctest::Approx(level).epsilon(0.02));

    // The gate is independent of the strip's own mute.
    mic->mute = true;
    CHECK(runAndMeasure(500.0) < 1e-4);
}

TEST_CASE("recorder: the no-effects tap records the input before its effects") {
    auto tone = std::make_shared<ToneSource>();
    tone->frequency = 1000.0f;
    tone->levelDb = -12.0f;
    tone->prepare(kSr, kBlock, double(kBlock) / kSr);
    auto strip = std::make_shared<StripRuntime>();
    strip->prepare(kSr, kBlock);
    std::shared_ptr<Effect> gain(EffectRegistry::instance().create("gain").release());
    REQUIRE(gain);
    gain->prepare(kSr, kBlock);
    gain->setParam("gain", 6.0f);
    auto raw = std::make_shared<RecordTap>(kSr);
    auto post = std::make_shared<RecordTap>(kSr);
    Engine e;
    e.setFormat(kSr, kBlock);
    auto g = std::make_unique<GraphSnapshot>();
    g->sources[0] = tone.get();
    g->numSources = 1;
    g->strips[0] = StripSnap{strip.get(), 0, -1, {}};
    g->strips[0].chain.fx[g->strips[0].chain.count++] = gain.get();
    g->numStrips = 1;
    g->taps[0] = TapSnap{raw.get(), RecordTap::StripRaw, 0};
    g->taps[1] = TapSnap{post.get(), RecordTap::StripPost, 0};
    g->numTaps = 2;
    g->keepAlive = {tone, strip, gain, raw, post};
    e.publish(std::move(g));
    for (int i = 0; i < 40; ++i) e.processBlock();

    auto leftOf = [](RecordTap& t) {
        std::vector<float> inter(t.ring.readAvailable());
        inter.resize(t.ring.read(inter.data(), inter.size()));
        std::vector<float> l(inter.size() / 2);
        for (size_t i = 0; i < l.size(); ++i) l[i] = inter[2 * i];
        return l;
    };
    const auto rawL = leftOf(*raw);
    const auto postL = leftOf(*post);
    REQUIRE(rawL.size() >= 9600);
    REQUIRE(postL.size() >= 9600);
    CHECK(toneAmplitude(rawL, rawL.size() - 9600, 9600, 1000.0, kSr) == doctest::Approx(dbToGain(-12.0f)).epsilon(0.02));
    CHECK(toneAmplitude(postL, postL.size() - 9600, 9600, 1000.0, kSr) == doctest::Approx(dbToGain(-6.0f)).epsilon(0.02));
}

TEST_CASE("recorder: recent recordings are listed newest first with their length") {
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / "echo-test-recordings";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir);
    auto write = [&](const char* name, double seconds, int bits) {
        WavWriter w;
        REQUIRE(w.open((dir / name).wstring(), 48000, 2, bits));
        std::vector<float> frames(static_cast<size_t>(48000 * seconds) * 2, 0.1f);
        REQUIRE(w.write(frames.data(), frames.size() / 2));
        w.close();
    };
    write("older.wav", 1.5, 24);
    write("newer.wav", 0.5, 16);
    fs::last_write_time(dir / "older.wav", fs::file_time_type::clock::now() - std::chrono::hours(1));
    { std::ofstream(dir / "notes.txt") << "not audio"; }

    WavInfo info;
    REQUIRE(readWavInfo((dir / "older.wav").wstring(), info));
    CHECK(info.sampleRate == 48000);
    CHECK(info.channels == 2);
    CHECK(info.bitDepth == 24);
    CHECK(info.seconds == doctest::Approx(1.5).epsilon(0.001));

    const auto list = listRecordings(dir.wstring(), 10);
    REQUIRE(list.size() == 2);
    CHECK(list[0].name == "newer.wav");
    CHECK(list[0].seconds == doctest::Approx(0.5).epsilon(0.001));
    CHECK(list[1].name == "older.wav");
    CHECK(list[1].bytes > list[0].bytes);
    CHECK(listRecordings(dir.wstring(), 1).size() == 1);
    CHECK(listRecordings((dir / "missing").wstring(), 10).empty());
    fs::remove_all(dir, ec);
}
