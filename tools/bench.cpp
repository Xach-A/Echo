// echo_bench - offline performance benchmark of the Echo engine and effects.
//
// Runs the real engine (sources -> strips -> effect chains -> buses -> sinks)
// without audio hardware: capture and render devices are in-memory endpoints
// that behave like WASAPI streams (including sample-rate conversion when the
// "device" rate differs from the engine rate). Only Engine::processBlock() is
// timed. Results are synthetic CPU measurements, not end-to-end latency.
//
//   echo_bench                 all scenarios
//   echo_bench effects         every effect alone (48 kHz, 480 and 128 frames)
//   echo_bench mixer           routing/mixing overhead, no effects
//   echo_bench voice           one microphone with a full voice chain
//   echo_bench heavy           8 microphones with voice chains + 4 apps + 4 buses
//   echo_bench ... --seconds N audio seconds per measurement (default 20)
#include <windows.h>
#include <avrt.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <random>
#include <string>
#include <vector>

#include "core/Engine.h"
#include "core/Text.h"
#include "dsp/EffectRegistry.h"
#include "session/Presets.h"
#include "session/Serialization.h"

using namespace echo;

namespace {

double g_seconds = 20.0;

// Capture "device" that always has data (pink noise + a voice-like tone).
class BenchCapture final : public CaptureEndpoint {
public:
    BenchCapture(int ch, int rate) : ch_(ch), rate_(rate) {
        ring_.allocate(static_cast<size_t>(rate) * ch);
        desc_ = "bench capture";
        std::mt19937 rng(static_cast<uint32_t>(rate * 7 + ch));
        std::normal_distribution<float> nd(0.0f, 0.02f);
        sig_.resize(static_cast<size_t>(rate) * ch); // 1 s loop
        for (size_t i = 0; i < sig_.size() / ch; ++i) {
            const float v = 0.2f * static_cast<float>(std::sin(2 * kPiD * 180.0 * i / rate) *
                                                      (0.6 + 0.4 * std::sin(2 * kPiD * 3.0 * i / rate))) +
                            nd(rng);
            for (int c = 0; c < ch; ++c) sig_[i * ch + c] = v;
        }
    }
    int channels() const override { return ch_; }
    int sampleRate() const override { return rate_; }
    int periodFrames() const override { return rate_ / 100; }
    SpscRing<float>& ring() override { return ring_; }
    EndpointStatus status() const override { return EndpointStatus::Running; }
    bool isSparse() const override { return false; }
    double deviceLatencySeconds() const override { return 0.01; }
    const std::string& describe() const override { return desc_; }
    // Device side: deliver what a real device would for `engineFrames` engine frames.
    void produce(double frames) {
        acc_ += frames;
        const size_t n = static_cast<size_t>(acc_);
        acc_ -= static_cast<double>(n);
        for (size_t i = 0; i < n; ++i) {
            ring_.write(&sig_[pos_ * ch_], static_cast<size_t>(ch_));
            if (++pos_ >= sig_.size() / ch_) pos_ = 0;
        }
    }

private:
    int ch_, rate_;
    SpscRing<float> ring_;
    std::string desc_;
    std::vector<float> sig_;
    size_t pos_ = 0;
    double acc_ = 0.0;
};

class BenchRender final : public RenderEndpoint {
public:
    BenchRender(int ch, int rate) : ch_(ch), rate_(rate) {
        ring_.allocate(static_cast<size_t>(rate) * ch);
        desc_ = "bench render";
    }
    int channels() const override { return ch_; }
    int sampleRate() const override { return rate_; }
    int periodFrames() const override { return rate_ / 100; }
    SpscRing<float>& ring() override { return ring_; }
    EndpointStatus status() const override { return EndpointStatus::Running; }
    double deviceLatencySeconds() const override { return 0.01; }
    const std::string& describe() const override { return desc_; }
    void setWakeTarget(WakeTarget*, int) override {}
    void consume(double frames) {
        acc_ += frames;
        const size_t n = static_cast<size_t>(acc_);
        acc_ -= static_cast<double>(n);
        ring_.skip(std::min(ring_.readAvailable(), n * ch_));
    }

private:
    int ch_, rate_;
    SpscRing<float> ring_;
    std::string desc_;
    double acc_ = 0.0;
};

struct Rig {
    Engine engine;
    std::unique_ptr<GraphSnapshot> g = std::make_unique<GraphSnapshot>();
    std::vector<std::shared_ptr<void>> keep;
    std::vector<std::shared_ptr<BenchCapture>> caps;
    std::vector<std::shared_ptr<BenchRender>> rens;
    int sr, block;

    Rig(int sr_, int block_) : sr(sr_), block(block_) { engine.setFormat(sr, block); }
    int addCapture(int ch, int devRate) {
        auto ep = std::make_shared<BenchCapture>(ch, devRate);
        caps.push_back(ep);
        auto s = std::make_shared<DeviceCaptureSource>(ep);
        s->prepare(sr, block, double(block) / sr);
        keep.push_back(s);
        g->sources[g->numSources] = s.get();
        return g->numSources++;
    }
    int addTone(float freq) {
        auto t = std::make_shared<ToneSource>();
        t->wave = ToneSource::PinkNoise;
        t->frequency = freq;
        t->levelDb = -24.0f;
        t->prepare(sr, block, double(block) / sr);
        keep.push_back(t);
        g->sources[g->numSources] = t.get();
        return g->numSources++;
    }
    StripRuntime* addStrip(int source, const std::vector<std::shared_ptr<Effect>>& chain, int reference = -1) {
        auto s = std::make_shared<StripRuntime>();
        s->prepare(sr, block);
        StripSnap snap;
        snap.rt = s.get();
        snap.source = source;
        snap.reference = reference;
        for (const auto& e : chain) {
            e->prepare(sr, block);
            keep.push_back(e);
            snap.chain.fx[snap.chain.count++] = e.get();
        }
        g->strips[g->numStrips++] = snap;
        keep.push_back(s);
        return s.get();
    }
    int addRender(int devRate) {
        auto ep = std::make_shared<BenchRender>(2, devRate);
        rens.push_back(ep);
        auto s = std::make_shared<DeviceRenderSink>(ep);
        s->prepare(sr, block, double(block) / sr);
        keep.push_back(s);
        g->sinks[g->numSinks] = s.get();
        return g->numSinks++;
    }
    BusRuntime* addBus(int sink, const std::vector<std::shared_ptr<Effect>>& chain = {}) {
        auto b = std::make_shared<BusRuntime>();
        b->prepare(sr, block);
        BusSnap bs;
        bs.rt = b.get();
        bs.slot = g->numBuses;
        bs.sink = sink;
        for (const auto& e : chain) {
            e->prepare(sr, block);
            keep.push_back(e);
            bs.chain.fx[bs.chain.count++] = e.get();
        }
        g->buses[g->numBuses++] = bs;
        keep.push_back(b);
        return b.get();
    }
    void publish() {
        g->keepAlive = keep;
        engine.publish(std::move(g));
    }

    struct Result {
        double avgUs = 0, p50Us = 0, p99Us = 0, maxUs = 0, blockUs = 0;
    };
    Result run(double seconds) {
        const int warm = std::max(50, static_cast<int>(0.5 * sr / block));
        const int count = std::max(200, static_cast<int>(seconds * sr / block));
        std::vector<double> t;
        t.reserve(count);
        LARGE_INTEGER f, a, b;
        QueryPerformanceFrequency(&f);
        for (int i = 0; i < warm + count; ++i) {
            for (auto& c : caps) c->produce(double(block) * c->sampleRate() / sr);
            QueryPerformanceCounter(&a);
            engine.processBlock();
            QueryPerformanceCounter(&b);
            for (auto& r : rens) r->consume(double(block) * r->sampleRate() / sr);
            if (i >= warm) t.push_back(double(b.QuadPart - a.QuadPart) * 1e6 / double(f.QuadPart));
        }
        Result res;
        res.blockUs = 1e6 * block / sr;
        double sum = 0;
        for (double v : t) sum += v;
        res.avgUs = sum / t.size();
        std::sort(t.begin(), t.end());
        res.p50Us = t[t.size() / 2];
        res.p99Us = t[std::min(t.size() - 1, t.size() * 99 / 100)];
        res.maxUs = t.back();
        return res;
    }
};

std::vector<std::shared_ptr<Effect>> presetChain(const char* name) {
    for (const auto& p : factoryChainPresets())
        if (p.name == name) return chainFromJson(p.chain, nullptr);
    std::printf("missing preset %s\n", name);
    return {};
}

void printHeader(const char* title) {
    std::printf("\n== %s ==\n", title);
    std::printf("%-44s %8s %8s %8s %8s %7s %7s\n", "case", "avg us", "p50 us", "p99 us", "max us", "avg %", "max %");
}

void printRow(const std::string& name, const Rig::Result& r) {
    std::printf("%-44s %8.1f %8.1f %8.1f %8.1f %6.2f%% %6.1f%%\n", name.c_str(), r.avgUs, r.p50Us, r.p99Us, r.maxUs,
                100.0 * r.avgUs / r.blockUs, 100.0 * r.maxUs / r.blockUs);
    std::fflush(stdout);
}

void benchEffects() {
    for (const auto& [sr, block] : {std::pair{48000, 480}, std::pair{48000, 128}}) {
        char title[96];
        std::snprintf(title, sizeof title, "each effect alone, mono voice strip, %d Hz / %d frames", sr, block);
        printHeader(title);
        {
            Rig rig(sr, block);
            const int src = rig.addCapture(1, sr);
            rig.addStrip(src, {});
            rig.addBus(rig.addRender(sr));
            rig.g->strips[0].rt->sends[0].enabled = true;
            rig.publish();
            printRow("(no effect: strip + bus + device I/O)", rig.run(g_seconds * 0.5));
        }
        for (const auto& reg : EffectRegistry::instance().all()) {
            Rig rig(sr, block);
            const int src = rig.addCapture(1, sr);
            int ref = -1;
            if (std::string(reg.info->typeId) == "echo_cancel") ref = rig.addTone(300.0f);
            std::shared_ptr<Effect> fx(reg.create().release());
            rig.addStrip(src, {fx}, ref);
            rig.addBus(rig.addRender(sr));
            rig.g->strips[0].rt->sends[0].enabled = true;
            rig.publish();
            printRow(std::string(reg.info->typeId) + (std::string(reg.info->typeId) == "ai_voice" ? " (no model)" : ""),
                     rig.run(g_seconds * 0.5));
        }
    }
}

void benchMixer() {
    printHeader("mixing only: 16 inputs x 8 buses, every send on, no effects");
    for (const auto& [sr, block] : {std::pair{48000, 480}, std::pair{48000, 128}, std::pair{48000, 64},
                                    std::pair{44100, 441}, std::pair{48000, 1024}}) {
        Rig rig(sr, block);
        for (int i = 0; i < 16; ++i) {
            const int src = rig.addTone(200.0f + 50.0f * i);
            StripRuntime* s = rig.addStrip(src, {});
            s->chanCount = 2;
            for (int b = 0; b < 8; ++b) s->sends[b].enabled = true;
        }
        for (int b = 0; b < 8; ++b) rig.addBus(b < 4 ? rig.addRender(48000) : -1);
        rig.publish();
        char name[64];
        std::snprintf(name, sizeof name, "%d Hz / %d frames", sr, block);
        printRow(name, rig.run(g_seconds * 0.5));
    }
}

void benchVoice() {
    printHeader("one microphone: noise suppression + Streaming Voice chain + Deep Voice, 2 outputs");
    for (const auto& [sr, block] : {std::pair{48000, 480}, std::pair{48000, 256}, std::pair{48000, 128},
                                    std::pair{44100, 441}}) {
        Rig rig(sr, block);
        const int mic = rig.addCapture(1, 48000);
        auto chain = presetChain("Streaming Voice");
        auto deep = presetChain("Deep Voice");
        chain.insert(chain.end(), deep.begin(), deep.end());
        StripRuntime* s = rig.addStrip(mic, chain);
        s->sends[0].enabled = true;
        s->sends[1].enabled = true;
        rig.addBus(rig.addRender(48000));
        rig.addBus(rig.addRender(44100)); // resampled output device
        rig.publish();
        char name[96];
        std::snprintf(name, sizeof name, "%d Hz / %d frames (%zu effects)", sr, block, chain.size());
        printRow(name, rig.run(g_seconds));
    }
}

void benchHeavy() {
    printHeader("heavy: 8 mics with voice chains, 4 apps with music chains, 4 buses with effects");
    for (const auto& [sr, block] : {std::pair{48000, 480}, std::pair{48000, 128}}) {
        Rig rig(sr, block);
        for (int i = 0; i < 8; ++i) {
            const int mic = rig.addCapture(1, i % 2 ? 44100 : 48000);
            StripRuntime* s = rig.addStrip(mic, presetChain(i % 2 ? "Podcast Voice" : "Streaming Voice"));
            s->sends[0].enabled = true;
            s->sends[1].enabled = true;
        }
        for (int i = 0; i < 4; ++i) {
            const int app = rig.addCapture(2, 48000);
            StripRuntime* s = rig.addStrip(app, presetChain("Music Enhancer"));
            s->chanCount = 2;
            s->sends[2].enabled = true;
            s->sends[3].enabled = true;
        }
        rig.addBus(rig.addRender(48000), presetChain("Music Enhancer"));
        rig.addBus(rig.addRender(48000));
        rig.addBus(rig.addRender(44100), presetChain("Music Enhancer"));
        rig.addBus(rig.addRender(48000));
        rig.publish();
        char name[64];
        std::snprintf(name, sizeof name, "%d Hz / %d frames", sr, block);
        printRow(name, rig.run(g_seconds));
    }
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    SetConsoleOutputCP(CP_UTF8);
    enableFlushDenormals();
    DWORD task = 0;
    HANDLE mm = AvSetMmThreadCharacteristicsW(L"Pro Audio", &task);
    std::vector<std::string> which;
    for (int i = 1; i < argc; ++i) {
        const std::string a = wideToUtf8(argv[i]);
        if (a == "--seconds" && i + 1 < argc) g_seconds = std::max(1.0, _wtof(argv[++i]));
        else which.push_back(a);
    }
    auto want = [&](const char* s) { return which.empty() || std::find(which.begin(), which.end(), s) != which.end(); };
    std::printf("echo_bench: synthetic CPU benchmark (no audio hardware). %.0f s of audio per row.\n", g_seconds);
    std::printf("Times are Engine::processBlock() wall time per block; %% = share of the block's real-time duration.\n");
    if (want("mixer")) benchMixer();
    if (want("voice")) benchVoice();
    if (want("heavy")) benchHeavy();
    if (want("effects")) benchEffects();
    if (mm) AvRevertMmThreadCharacteristics(mm);
    return 0;
}
