// Engine graph, routing and mixing tests. These run the real engine
// processing code with generated sources and in-memory sinks; no hardware.
#include "doctest/doctest.h"

#include <memory>

#include "TestUtil.h"
#include "core/Engine.h"

using namespace echo;
using namespace echo::test;

namespace {

constexpr int kSr = 48000;
constexpr int kBlock = 480;

struct Rig {
    Engine engine;
    std::vector<std::shared_ptr<ToneSource>> tones;
    std::vector<std::shared_ptr<StripRuntime>> strips;
    std::vector<std::shared_ptr<BusRuntime>> buses;
    std::vector<std::shared_ptr<MemorySink>> sinks;
    std::vector<int> stripSource;
    std::vector<int> busSink;

    Rig() { engine.setFormat(kSr, kBlock); }

    int addTone(float freq, float levelDb) {
        auto t = std::make_shared<ToneSource>();
        t->frequency = freq;
        t->levelDb = levelDb;
        t->prepare(kSr, kBlock, double(kBlock) / kSr);
        tones.push_back(t);
        return int(tones.size()) - 1;
    }
    int addStrip(int source) {
        auto s = std::make_shared<StripRuntime>();
        s->prepare(kSr, kBlock);
        strips.push_back(s);
        stripSource.push_back(source);
        return int(strips.size()) - 1;
    }
    int addBus(int sink) {
        auto b = std::make_shared<BusRuntime>();
        b->limiterEnabled = false;
        b->prepare(kSr, kBlock);
        buses.push_back(b);
        busSink.push_back(sink);
        return int(buses.size()) - 1;
    }
    int addSink(size_t frames = kSr * 2) {
        auto s = std::make_shared<MemorySink>(frames);
        s->prepare(kSr, kBlock, double(kBlock) / kSr);
        sinks.push_back(s);
        return int(sinks.size()) - 1;
    }
    void route(int strip, int bus, bool on = true, float db = 0.0f, bool pre = false) {
        auto& s = strips[strip]->sends[bus];
        s.enabled = on;
        s.gainDb = db;
        s.preFader = pre;
    }
    void publish() {
        auto g = std::make_unique<GraphSnapshot>();
        for (auto& t : tones) {
            g->sources[g->numSources++] = t.get();
            g->keepAlive.push_back(t);
        }
        for (size_t i = 0; i < strips.size(); ++i) {
            g->strips[g->numStrips++] = StripSnap{strips[i].get(), stripSource[i], -1, {}};
            g->keepAlive.push_back(strips[i]);
        }
        for (size_t i = 0; i < buses.size(); ++i) {
            BusSnap bs;
            bs.rt = buses[i].get();
            bs.slot = int(i);
            bs.sink = busSink[i];
            g->buses[g->numBuses++] = bs;
            g->keepAlive.push_back(buses[i]);
        }
        for (auto& s : sinks) {
            g->sinks[g->numSinks++] = s.get();
            g->keepAlive.push_back(s);
        }
        engine.publish(std::move(g));
    }
    void run(int blocks) {
        for (int i = 0; i < blocks; ++i) engine.processBlock();
    }
    // Amplitude of `freq` in the last `seconds` of a sink channel.
    double amp(int sink, double freq, bool right = false, double seconds = 0.2) {
        const auto& d = right ? sinks[sink]->rightData() : sinks[sink]->leftData();
        const size_t n = size_t(seconds * kSr);
        const size_t end = sinks[sink]->written();
        return toneAmplitude(d, end - n, n, freq, kSr);
    }
};

} // namespace

TEST_CASE("graph: tone -> strip -> bus -> sink at unity gain") {
    Rig r;
    int t = r.addTone(1000.0f, -6.0f);
    int s = r.addStrip(t);
    int k = r.addSink();
    int b = r.addBus(k);
    r.route(s, b);
    r.publish();
    r.run(50);
    CHECK(r.amp(k, 1000.0, false) == doctest::Approx(dbToGain(-6.0f)).epsilon(0.01));
    CHECK(r.amp(k, 1000.0, true) == doctest::Approx(dbToGain(-6.0f)).epsilon(0.01));
}

TEST_CASE("routing: one input to multiple destinations simultaneously") {
    Rig r;
    int t = r.addTone(500.0f, -12.0f);
    int s = r.addStrip(t);
    int k1 = r.addSink(), k2 = r.addSink();
    int b1 = r.addBus(k1), b2 = r.addBus(k2);
    r.route(s, b1, true, 0.0f);
    r.route(s, b2, true, -6.0f);
    r.publish();
    r.run(50);
    CHECK(r.amp(k1, 500.0) == doctest::Approx(dbToGain(-12.0f)).epsilon(0.01));
    CHECK(r.amp(k2, 500.0) == doctest::Approx(dbToGain(-18.0f)).epsilon(0.01));
}

TEST_CASE("routing: independent bus mixes stay isolated") {
    // Voice (300 Hz) -> virtual mic bus; music (2 kHz) -> headphones only;
    // voice also monitored in headphones.
    Rig r;
    int voice = r.addTone(300.0f, -10.0f);
    int music = r.addTone(2000.0f, -10.0f);
    int sv = r.addStrip(voice), sm = r.addStrip(music);
    int kHead = r.addSink(), kMic = r.addSink();
    int bHead = r.addBus(kHead), bMic = r.addBus(kMic);
    r.route(sv, bHead);
    r.route(sv, bMic);
    r.route(sm, bHead);
    r.publish();
    r.run(50);
    CHECK(r.amp(kHead, 300.0) == doctest::Approx(dbToGain(-10.0f)).epsilon(0.02));
    CHECK(r.amp(kHead, 2000.0) == doctest::Approx(dbToGain(-10.0f)).epsilon(0.02));
    CHECK(r.amp(kMic, 300.0) == doctest::Approx(dbToGain(-10.0f)).epsilon(0.02));
    CHECK(r.amp(kMic, 2000.0) < 1e-5); // music never enters the mic mix
}

TEST_CASE("routing: changes take effect while running, without clicks") {
    Rig r;
    int t = r.addTone(1000.0f, -6.0f);
    int s = r.addStrip(t);
    int k = r.addSink();
    int b = r.addBus(k);
    r.route(s, b, false);
    r.publish();
    r.run(20);
    CHECK(r.amp(k, 1000.0) < 1e-6);
    r.route(s, b, true);
    r.run(30);
    CHECK(r.amp(k, 1000.0) == doctest::Approx(dbToGain(-6.0f)).epsilon(0.01));
    // Ramp means no sample-to-sample jump larger than the sine itself allows.
    const auto& d = r.sinks[k]->leftData();
    float maxStep = 0.0f;
    for (size_t i = 1; i < r.sinks[k]->written(); ++i) maxStep = std::max(maxStep, std::fabs(d[i] - d[i - 1]));
    const float sineStep = dbToGain(-6.0f) * 2.0f * kPi * 1000.0f / kSr;
    CHECK(maxStep <= sineStep * 1.05f);
}

TEST_CASE("mixer: mute and solo") {
    Rig r;
    int t1 = r.addTone(400.0f, -10.0f), t2 = r.addTone(900.0f, -10.0f);
    int s1 = r.addStrip(t1), s2 = r.addStrip(t2);
    int k = r.addSink();
    int b = r.addBus(k);
    r.route(s1, b);
    r.route(s2, b);
    r.publish();

    r.strips[s1]->mute = true;
    r.run(30);
    CHECK(r.amp(k, 400.0) < 1e-5);
    CHECK(r.amp(k, 900.0) > 0.3);

    r.strips[s1]->mute = false;
    r.strips[s2]->solo = true;
    r.run(30);
    CHECK(r.amp(k, 400.0) < 1e-5); // not soloed -> silent
    CHECK(r.amp(k, 900.0) == doctest::Approx(dbToGain(-10.0f)).epsilon(0.01));

    r.strips[s2]->solo = false;
    r.run(30);
    CHECK(r.amp(k, 400.0) > 0.3);
    CHECK(r.amp(k, 900.0) > 0.3);
}

TEST_CASE("mixer: fader, trim, send gain and bus fader compose in dB") {
    Rig r;
    int t = r.addTone(1000.0f, -6.0f);
    int s = r.addStrip(t);
    int k = r.addSink();
    int b = r.addBus(k);
    r.route(s, b, true, -3.0f);
    r.strips[s]->trimDb = 2.0f;
    r.strips[s]->faderDb = -5.0f;
    r.buses[b]->faderDb = -4.0f;
    r.publish();
    r.run(50);
    CHECK(gainToDb(float(r.amp(k, 1000.0))) == doctest::Approx(-6.0 + 2.0 - 5.0 - 3.0 - 4.0).epsilon(0.005));
}

TEST_CASE("mixer: pre-fader send ignores the channel fader") {
    Rig r;
    int t = r.addTone(1000.0f, -6.0f);
    int s = r.addStrip(t);
    int k1 = r.addSink(), k2 = r.addSink();
    int bPost = r.addBus(k1), bPre = r.addBus(k2);
    r.route(s, bPost, true, 0.0f, false);
    r.route(s, bPre, true, 0.0f, true);
    r.strips[s]->faderDb = -20.0f;
    r.publish();
    r.run(50);
    CHECK(gainToDb(float(r.amp(k1, 1000.0))) == doctest::Approx(-26.0).epsilon(0.01));
    CHECK(gainToDb(float(r.amp(k2, 1000.0))) == doctest::Approx(-6.0).epsilon(0.01));
}

TEST_CASE("mixer: constant-power pan for mono, balance for stereo") {
    Rig r;
    int t = r.addTone(1000.0f, -6.0f);
    int s = r.addStrip(t);
    r.strips[s]->chanCount = 1; // mono strip
    int k = r.addSink();
    int b = r.addBus(k);
    r.route(s, b);
    r.publish();

    r.strips[s]->pan = 0.0f;
    r.run(30);
    const double a = dbToGain(-6.0f);
    CHECK(r.amp(k, 1000.0, false) == doctest::Approx(a).epsilon(0.01));
    CHECK(r.amp(k, 1000.0, true) == doctest::Approx(a).epsilon(0.01));

    r.strips[s]->pan = -1.0f; // hard left: +3 dB left, silence right
    r.run(30);
    CHECK(r.amp(k, 1000.0, false) == doctest::Approx(a * std::sqrt(2.0)).epsilon(0.01));
    CHECK(r.amp(k, 1000.0, true) < 1e-4);

    // Stereo strip: balance attenuates the opposite side only.
    r.strips[s]->chanCount = 2;
    r.strips[s]->pan = 0.5f;
    r.run(30);
    CHECK(r.amp(k, 1000.0, false) == doctest::Approx(a * 0.5).epsilon(0.01));
    CHECK(r.amp(k, 1000.0, true) == doctest::Approx(a).epsilon(0.01));
}

TEST_CASE("mixer: bus limiter keeps output below ceiling") {
    Rig r;
    int t = r.addTone(200.0f, 0.0f);
    int s = r.addStrip(t);
    int k = r.addSink();
    int b = r.addBus(k);
    r.route(s, b, true, 12.0f); // +12 dB overload
    r.buses[b]->limiterEnabled = true;
    r.buses[b]->limiterCeilingDb = -1.0f;
    r.buses[b]->prepare(kSr, kBlock);
    r.publish();
    r.run(60);
    const auto& d = r.sinks[k]->leftData();
    const size_t n = r.sinks[k]->written();
    CHECK(peak(d.data() + n / 2, n / 2) <= dbToGain(-1.0f) * 1.0001f);
    CHECK(r.buses[b]->limiterGrDb.load() < -10.0f);
}

TEST_CASE("engine: snapshots can be swapped repeatedly while processing") {
    Rig r;
    int t = r.addTone(700.0f, -12.0f);
    int s = r.addStrip(t);
    int k = r.addSink(kSr * 10);
    int b = r.addBus(k);
    r.route(s, b);
    for (int i = 0; i < 200; ++i) {
        r.publish();
        r.run(1);
        r.engine.collectGarbage();
    }
    CHECK(r.amp(k, 700.0) == doctest::Approx(dbToGain(-12.0f)).epsilon(0.01));
}

TEST_CASE("engine: odd block sizes produce identical audio (buffer boundaries)") {
    std::vector<float> outs[2];
    const int blocks[2] = {480, 37};
    for (int i = 0; i < 2; ++i) {
        Engine e;
        e.setFormat(kSr, blocks[i]);
        auto tone = std::make_shared<ToneSource>();
        tone->frequency = 1234.0f;
        tone->prepare(kSr, blocks[i], double(blocks[i]) / kSr);
        auto strip = std::make_shared<StripRuntime>();
        strip->sends[0].enabled = true;
        strip->prepare(kSr, blocks[i]);
        auto bus = std::make_shared<BusRuntime>();
        bus->limiterEnabled = false;
        bus->prepare(kSr, blocks[i]);
        auto sink = std::make_shared<MemorySink>(kSr);
        sink->prepare(kSr, blocks[i], 0.01);
        auto g = std::make_unique<GraphSnapshot>();
        g->sources[0] = tone.get();
        g->numSources = 1;
        g->strips[0] = StripSnap{strip.get(), 0, -1, {}};
        g->numStrips = 1;
        BusSnap bs;
        bs.rt = bus.get();
        bs.slot = 0;
        bs.sink = 0;
        g->buses[0] = bs;
        g->numBuses = 1;
        g->sinks[0] = sink.get();
        g->numSinks = 1;
        g->keepAlive = {tone, strip, bus, sink};
        e.publish(std::move(g));
        while (sink->written() < 24000) e.processBlock();
        outs[i].assign(sink->leftData().begin(), sink->leftData().begin() + 24000);
    }
    float maxDiff = 0.0f;
    for (size_t i = 0; i < 24000; ++i) maxDiff = std::max(maxDiff, std::fabs(outs[0][i] - outs[1][i]));
    CHECK(maxDiff < 1e-5f);
}
