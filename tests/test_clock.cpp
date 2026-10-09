// Clock-drift control between independent device clocks (simulated time).
#include "doctest/doctest.h"

#include <cmath>
#include <memory>
#include <random>
#include <vector>

#include "core/Sinks.h"
#include "core/Sources.h"

using namespace echo;

namespace {

// Capture device on a simulated clock: delivers fixed-size packets at its
// own (slightly different) rate with jittery arrival, and optionally
// publishes packet timestamps like the WASAPI capture thread does.
class SimCapture final : public CaptureEndpoint {
public:
    SimCapture(bool stamps) : stamps_(stamps) {
        ring_.allocate(48000);
        desc_ = "sim capture";
    }
    int channels() const override { return 1; }
    int sampleRate() const override { return 48000; }
    int periodFrames() const override { return 480; }
    SpscRing<float>& ring() override { return ring_; }
    EndpointStatus status() const override { return EndpointStatus::Running; }
    bool isSparse() const override { return false; }
    double deviceLatencySeconds() const override { return 0.01; }
    const std::string& describe() const override { return desc_; }
    bool flowStamp(FlowStamp& out) const override { return stamps_ && cell_.read(out); }
    double flowClockNow() const override { return now; }

    void deliver(double at) {
        std::vector<float> pkt(480, 0.1f);
        ring_.write(pkt.data(), pkt.size());
        total_ += 480;
        cell_.publish(total_, 480, at);
    }
    double now = 0.0;

private:
    bool stamps_;
    SpscRing<float> ring_;
    std::string desc_;
    FlowStampCell cell_;
    uint64_t total_ = 0;
};

struct DriftRun {
    double meanPpm = 0, minPpm = 1e9, maxPpm = -1e9;
    uint64_t underruns = 0, overruns = 0;
};

DriftRun simulate(bool stamps, double devicePpm, double seconds, uint32_t seed) {
    auto ep = std::make_shared<SimCapture>(stamps);
    DeviceCaptureSource src(ep);
    src.prepare(48000.0, 480, 0.01);
    std::mt19937 rng(seed);
    std::uniform_real_distribution<double> jitter(0.0, 0.0015); // arrival delay 0..1.5 ms
    const double packetPeriod = 480.0 / (48000.0 * (1.0 + devicePpm * 1e-6));
    // Packet k is complete at k * period (steady device clock) and reaches
    // the ring 0..1.5 ms later (scheduling jitter, not accumulated).
    uint64_t k = 0;
    double arrival = 0.0003 + jitter(rng);
    double sum = 0;
    int count = 0;
    DriftRun r;
    for (int block = 0; block * 0.01 < seconds; ++block) {
        const double t = block * 0.01; // engine clock: one 480-frame block per 10 ms
        while (arrival <= t) {
            ep->now = arrival;
            ep->deliver(arrival);
            ++k;
            arrival = 0.0003 + static_cast<double>(k) * packetPeriod + jitter(rng);
        }
        ep->now = t;
        src.pull(480);
        if (t > seconds * 0.3) { // after settling
            const double ppm = src.driftPpm.load();
            sum += ppm;
            ++count;
            r.minPpm = std::min(r.minPpm, ppm);
            r.maxPpm = std::max(r.maxPpm, ppm);
        }
    }
    r.meanPpm = sum / count;
    r.underruns = src.underruns.load();
    r.overruns = src.overruns.load();
    return r;
}

} // namespace

TEST_CASE("clock drift: packet timing is not mistaken for drift") {
    // The device runs 300 ppm fast, so the packet/block phase slips by a whole
    // packet every ~33 s: a fill sampled at block time drifts through a
    // 10 ms sawtooth that old-style control treated as real drift.
    const DriftRun with = simulate(true, 300.0, 150.0, 7);
    const DriftRun without = simulate(false, 300.0, 150.0, 7);
    MESSAGE("with packet timestamps: mean " << with.meanPpm << " ppm, range " << with.minPpm << " .. " << with.maxPpm);
    MESSAGE("block-time fill only:   mean " << without.meanPpm << " ppm, range " << without.minPpm << " .. "
                                            << without.maxPpm);
    CHECK(std::fabs(with.meanPpm - 300.0) < 30.0);
    CHECK(with.maxPpm - with.minPpm < 150.0);
    CHECK(with.maxPpm - with.minPpm < 0.5 * (without.maxPpm - without.minPpm));
    CHECK(with.underruns == 0);
    CHECK(with.overruns == 0);
}

namespace {

// Render device on a simulated clock: its thread takes one period from the
// ring at its own rate (with wake-up jitter), like the WASAPI render thread.
class SimRender final : public RenderEndpoint {
public:
    explicit SimRender(bool stamps) : stamps_(stamps) {
        ring_.allocate(96000);
        desc_ = "sim render";
    }
    int channels() const override { return 2; }
    int sampleRate() const override { return 48000; }
    int periodFrames() const override { return 480; }
    SpscRing<float>& ring() override { return ring_; }
    EndpointStatus status() const override { return EndpointStatus::Running; }
    double deviceLatencySeconds() const override { return 0.01; }
    const std::string& describe() const override { return desc_; }
    void setWakeTarget(WakeTarget*, int) override {}
    bool flowStamp(FlowStamp& out) const override { return stamps_ && cell_.read(out); }
    double flowClockNow() const override { return now; }
    void take(double at) {
        std::vector<float> buf(480 * 2);
        const size_t got = ring_.read(buf.data(), buf.size()) / 2;
        if (got < 480) ++dry;
        total_ += got;
        cell_.publish(total_, static_cast<uint32_t>(got), at);
    }
    double now = 0.0;
    int dry = 0;

private:
    bool stamps_;
    SpscRing<float> ring_;
    std::string desc_;
    FlowStampCell cell_;
    uint64_t total_ = 0;
};

DriftRun simulateRender(bool stamps, double devicePpm, double seconds, uint32_t seed) {
    auto ep = std::make_shared<SimRender>(stamps);
    DeviceRenderSink sink(ep);
    sink.prepare(48000.0, 480, 0.01);
    std::mt19937 rng(seed);
    std::uniform_real_distribution<double> jitter(0.0, 0.0015);
    const double period = 480.0 / (48000.0 * (1.0 + devicePpm * 1e-6));
    uint64_t k = 0;
    double wake = 0.0047 + jitter(rng);
    double sum = 0;
    int count = 0;
    DriftRun r;
    for (int block = 0; block * 0.01 < seconds; ++block) {
        const double t = block * 0.01;
        while (wake <= t) {
            ep->now = wake;
            ep->take(wake);
            ++k;
            wake = 0.0047 + static_cast<double>(k) * period + jitter(rng);
        }
        ep->now = t;
        sink.clear(480);
        sink.push(480);
        if (t > seconds * 0.3) {
            const double ppm = sink.driftPpm.load();
            sum += ppm;
            ++count;
            r.minPpm = std::min(r.minPpm, ppm);
            r.maxPpm = std::max(r.maxPpm, ppm);
        }
    }
    r.meanPpm = sum / count;
    r.underruns = sink.underruns.load() + static_cast<uint64_t>(ep->dry > 2 ? ep->dry - 2 : 0);
    r.overruns = sink.overruns.load();
    return r;
}

} // namespace

TEST_CASE("clock drift: secondary outputs track the device clock without phase wobble") {
    const DriftRun with = simulateRender(true, 300.0, 150.0, 11);
    const DriftRun without = simulateRender(false, 300.0, 150.0, 11);
    MESSAGE("with read timestamps: mean " << with.meanPpm << " ppm, range " << with.minPpm << " .. " << with.maxPpm);
    MESSAGE("block-time fill only: mean " << without.meanPpm << " ppm, range " << without.minPpm << " .. "
                                          << without.maxPpm);
    // A device consuming 300 ppm faster needs 300 ppm more output per input.
    CHECK(std::fabs(std::fabs(with.meanPpm) - 300.0) < 30.0);
    CHECK(with.maxPpm - with.minPpm < 150.0);
    CHECK(with.maxPpm - with.minPpm < 0.5 * (without.maxPpm - without.minPpm));
    CHECK(with.underruns == 0);
    CHECK(with.overruns == 0);
}

TEST_CASE("clock drift: after a dropout the input keeps a wider margin, so repeat glitches do not drop out") {
    // A jittery microphone that twice loses two whole packets (a USB glitch).
    auto ep = std::make_shared<SimCapture>(true);
    DeviceCaptureSource src(ep);
    src.prepare(48000.0, 480, 0.01);
    std::mt19937 rng(5);
    std::uniform_real_distribution<double> jitter(0.0, 0.004); // up to 4 ms late
    const double period = 480.0 / 48000.0;
    uint64_t k = 0;
    double arrival = 0.0021;
    uint64_t underrunsAfterFirst = 0;
    for (int block = 0; block < 9000; ++block) { // 90 s
        const double t = block * 0.01;
        while (arrival <= t) {
            const bool lost = (k >= 2000 && k < 2002) || (k >= 6000 && k < 6002); // at 20 s and 60 s
            ep->now = arrival;
            if (!lost) ep->deliver(arrival);
            ++k;
            arrival = 0.0021 + static_cast<double>(k) * period + jitter(rng);
        }
        ep->now = t;
        src.pull(480);
        if (block == 4000) underrunsAfterFirst = src.underruns.load();
    }
    const uint64_t total = src.underruns.load();
    MESSAGE("dropouts after the first glitch: " << underrunsAfterFirst << ", after the second: " << total
                                                << ", margin now " << src.marginMs.load() << " ms");
    CHECK(underrunsAfterFirst <= 1);
    CHECK(total == underrunsAfterFirst); // the repeat glitch is absorbed
}
