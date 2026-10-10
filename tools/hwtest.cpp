// echo_hwtest - hardware integration probe for the Echo audio engine.
//
// Runs the real engine and WASAPI layer against the devices on this machine
// and reports measured results. Nothing here is simulated: if a device is
// missing the corresponding check is reported as SKIPPED.
//
//   echo_hwtest list              enumerate endpoints and virtual-cable status
//   echo_hwtest mic [sec]         capture the default microphone, report levels
//   echo_hwtest playback          play a quiet tone through the engine to the default
//                                 output and verify it via loopback capture;
//                                 measures output+loopback round-trip latency
//   echo_hwtest procloop          capture this process's own audio via process loopback
//   echo_hwtest cables            verify every detected virtual cable pair end to end
//   echo_hwtest virtualmic [sec]  Echo's engine -> cable playback side -> cable recording side
//                                 (what Discord/OBS receive): level, gaps, latency (default 15 s)
//   echo_hwtest cableraw [sec] [ms]  same through the cable without Echo's engine (comparison)
//   echo_hwtest sysaudio          verify "all apps except Echo" capture excludes Echo
//   echo_hwtest approute          per-app output device set/read-back/revert (experimental API)
//   echo_hwtest soak [sec]        run mic + system capture + output for N seconds (default 60)
//   echo_hwtest denoise [sec]     capture the default microphone through the engine with noise
//                                 suppression in the strip chain; reports room-noise reduction,
//                                 voice activity, CPU and latency. Audio stays in memory only.
//   echo_hwtest hotkeys           push to talk / mute keys through Windows' real key state:
//                                 presses F24 and F23 with SendInput (keys no app uses)
//   echo_hwtest all               run everything above (playback is audible, -40 dBFS)
#include <windows.h>
#include <audiopolicy.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <cstdio>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "audio/AppRouting.h"
#include "audio/HotkeyService.h"
#include "audio/AppSessions.h"
#include "audio/DeviceManager.h"
#include "audio/VirtualCables.h"
#include "audio/WasapiStream.h"
#include "core/Engine.h"
#include "core/Log.h"
#include "core/Text.h"
#include "dsp/EffectRegistry.h"
#include "dsp/NoiseSuppressorInfo.h"
#include "session/Presets.h"

using namespace echo;
using namespace echo::win;

namespace {

int g_fail = 0, g_pass = 0, g_skip = 0;

void result(const char* name, int status, const std::string& detail) {
    const char* tag = status > 0 ? "PASS" : status == 0 ? "SKIP" : "FAIL";
    if (status > 0) ++g_pass;
    else if (status == 0) ++g_skip;
    else ++g_fail;
    std::printf("[%s] %s: %s\n", tag, name, detail.c_str());
}

double goertzel(const std::vector<float>& x, size_t start, size_t n, double f, double sr) {
    double re = 0, im = 0, ws = 0;
    for (size_t i = 0; i < n && start + i < x.size(); ++i) {
        const double w = 0.5 - 0.5 * std::cos(2 * kPiD * i / (n - 1));
        re += w * x[start + i] * std::cos(2 * kPiD * f * i / sr);
        im += w * x[start + i] * std::sin(2 * kPiD * f * i / sr);
        ws += w;
    }
    return 2 * std::sqrt(re * re + im * im) / ws;
}

// Finds chirp bursts emitted by `tone` in `mem` (same engine clock) and
// reports the delay of the cross-correlation peak.
void measureChirpLatency(const char* what, ToneSource& tone, MemorySink& mem, double sr) {
    std::vector<float> tpl(ToneSource::kChirpLength);
    for (int i = 0; i < ToneSource::kChirpLength; ++i) tpl[i] = ToneSource::chirpSample(i, sr);
    const auto& d = mem.leftData();
    const size_t written = mem.written();
    const uint64_t last = tone.lastImpulseFrame.load();
    const size_t period = size_t(tone.impulsePeriodSec.load() * sr);
    const size_t maxLag = size_t(0.4 * sr);
    std::vector<double> lats;
    for (int k = 1; k < 6; ++k) {
        if (last < k * period) break;
        const size_t emitted = size_t(last - k * period);
        if (emitted + maxLag + tpl.size() >= written) continue;
        double best = 0, energy = 0;
        size_t bestLag = 0;
        for (size_t lag = 0; lag < maxLag; ++lag) {
            double c = 0;
            const float* x = d.data() + emitted + lag;
            for (size_t i = 0; i < tpl.size(); i += 2) c += x[i] * tpl[i];
            if (std::fabs(c) > best) {
                best = std::fabs(c);
                bestLag = lag;
            }
        }
        for (size_t i = 0; i < tpl.size(); ++i) {
            const float v = d[emitted + bestLag + i];
            energy += double(v) * v;
        }
        double tplE = 0;
        for (float v : tpl) tplE += double(v) * v;
        const double ncc = best * 2 / std::sqrt(energy * tplE + 1e-20);
        std::printf("    burst %d: correlation peak %.2f at %.1f ms\n", k, ncc, bestLag * 1000.0 / sr);
        if (ncc > 0.3) lats.push_back(bestLag * 1000.0 / sr);
    }
    char buf[256];
    if (lats.empty()) {
        result(what, 0, "chirp not detected (too quiet or masked)");
        return;
    }
    double mn = lats[0], mx = lats[0], sum = 0;
    for (double v : lats) {
        mn = std::min(mn, v);
        mx = std::max(mx, v);
        sum += v;
    }
    std::snprintf(buf, sizeof(buf), "%.1f ms average (min %.1f, max %.1f, %zu bursts)", sum / lats.size(), mn, mx,
                  lats.size());
    result(what, 1, buf);
}

struct EngineRig {
    Engine engine;
    std::vector<std::shared_ptr<void>> keep;
    std::unique_ptr<GraphSnapshot> g = std::make_unique<GraphSnapshot>();
    int sr, block;
    EngineRig(int sr_, int block_) : sr(sr_), block(block_) { engine.setFormat(sr, block); }

    int addSource(std::shared_ptr<SourceRuntime> s) {
        s->prepare(sr, block, double(block) / sr);
        g->sources[g->numSources] = s.get();
        keep.push_back(s);
        return g->numSources++;
    }
    StripRuntime* addStrip(int source) {
        auto s = std::make_shared<StripRuntime>();
        s->prepare(sr, block);
        g->strips[g->numStrips++] = StripSnap{s.get(), source, -1, {}};
        keep.push_back(s);
        return s.get();
    }
    int addSink(std::shared_ptr<SinkRuntime> s) {
        s->prepare(sr, block, double(block) / sr);
        g->sinks[g->numSinks] = s.get();
        keep.push_back(s);
        return g->numSinks++;
    }
    BusRuntime* addBus(int sink) {
        auto b = std::make_shared<BusRuntime>();
        b->limiterEnabled = false;
        b->prepare(sr, block);
        BusSnap bs;
        bs.rt = b.get();
        bs.slot = g->numBuses;
        bs.sink = sink;
        g->buses[g->numBuses++] = bs;
        keep.push_back(b);
        return b.get();
    }
    void go() {
        g->keepAlive = keep;
        engine.publish(std::move(g));
        engine.start();
    }
};

void cmdList(DeviceManager& dm) {
    std::printf("\n== Audio endpoints ==\n");
    for (const auto& d : dm.devices()) {
        const char* vk = d.virtualKind == VirtualKind::Cable ? "  [virtual cable]"
                         : d.virtualKind == VirtualKind::AppBound ? "  [app-bound virtual]" : "";
        std::printf("%s %-8s %-60s %s%s%s\n", d.flow == Flow::Render ? "OUT" : "IN ",
                    d.active() ? "active" : "inactive", d.name.c_str(),
                    d.active() ? formatToString(d.mixFormat).c_str() : "", d.isDefault ? "  (default)" : "", vk);
    }
    auto pairs = findCablePairs(dm.devices());
    std::printf("\n== Loopback cable pairs detected: %zu ==\n", pairs.size());
    for (const auto& p : pairs) std::printf("  %s  ->  %s\n", p.renderName.c_str(), p.captureName.c_str());
    result("enumerate endpoints", dm.devices().empty() ? -1 : 1,
           std::to_string(dm.devices().size()) + " endpoints");
}

void cmdMic(DeviceManager& dm, double seconds) {
    const std::string id = dm.defaultDeviceId(Flow::Capture);
    if (id.empty()) {
        result("microphone capture", 0, "no default capture device");
        return;
    }
    auto info = dm.find(id);
    StreamOptions opt;
    opt.deviceId = fromUtf8(id);
    opt.label = info ? info->name : id;
    std::string err;
    auto cap = WasapiCapture::open(opt, &err);
    if (!cap) {
        result("microphone capture", -1, opt.label + ": " + err);
        return;
    }
    EngineRig rig(48000, 480);
    int src = rig.addSource(std::make_shared<DeviceCaptureSource>(cap));
    StripRuntime* strip = rig.addStrip(src);
    strip->chanCount = 1;
    auto mem = std::make_shared<MemorySink>(size_t(48000 * (seconds + 1)));
    int k = rig.addSink(mem);
    BusRuntime* bus = rig.addBus(k);
    (void)bus;
    strip->sends[0].enabled = true;
    rig.go();
    std::printf("Capturing '%s' (%s) for %.1f s...\n", opt.label.c_str(), formatToString(cap->format()).c_str(), seconds);
    float maxPeak = 0;
    for (int i = 0; i < int(seconds * 4); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        const float pk = strip->inMeter.takePeak(0);
        maxPeak = std::max(maxPeak, pk);
        std::printf("  t=%.2fs peak %6.1f dBFS  ring %.1f ms  drift %+.0f ppm\n", (i + 1) * 0.25, gainToDb(pk),
                    rig.g ? 0.f : static_cast<DeviceCaptureSource*>(rig.engine.current()->sources[0])->fillMs.load(),
                    static_cast<DeviceCaptureSource*>(rig.engine.current()->sources[0])->driftPpm.load());
    }
    rig.engine.stop();
    const auto* srcRt = static_cast<DeviceCaptureSource*>(rig.engine.current()->sources[0]);
    char buf[256];
    std::snprintf(buf, sizeof(buf), "%s: %llu frames captured, peak %.1f dBFS, underruns %llu, overruns %llu%s",
                  opt.label.c_str(), (unsigned long long)cap->stats.frames.load(), gainToDb(maxPeak),
                  (unsigned long long)srcRt->underruns.load(), (unsigned long long)cap->stats.overruns.load(),
                  cap->onlySilenceSoFar() ? " (only digital silence: mic muted or blocked by privacy settings?)" : "");
    result("microphone capture", cap->stats.frames.load() > 0 ? 1 : -1, buf);
}

// Runs the real microphone through the engine twice in parallel: one strip
// without effects (reference) and one with the noise suppressor, both into
// memory. Reports how much quieter the quiet (noise-only) passages became and
// how the loud (speech) passages changed. Nothing is written to disk.
void cmdDenoise(DeviceManager& dm, double seconds) {
    const std::string id = dm.defaultDeviceId(Flow::Capture);
    if (id.empty()) {
        result("noise suppression on the microphone", 0, "no default capture device");
        return;
    }
    auto info = dm.find(id);
    StreamOptions opt;
    opt.deviceId = fromUtf8(id);
    opt.label = info ? info->name : id;
    std::string err;
    auto cap = WasapiCapture::open(opt, &err);
    if (!cap) {
        result("noise suppression on the microphone", -1, opt.label + ": " + err);
        return;
    }
    EngineRig rig(48000, 480);
    const int src = rig.addSource(std::make_shared<DeviceCaptureSource>(cap));
    std::shared_ptr<Effect> ns(EffectRegistry::instance().create(noiseSuppressorTypeId()).release());
    ns->prepare(48000, 480);
    rig.keep.push_back(ns);
    StripRuntime* raw = rig.addStrip(src);
    StripRuntime* clean = rig.addStrip(src);
    raw->chanCount = clean->chanCount = 1;
    rig.g->strips[1].chain.fx[0] = ns.get();
    rig.g->strips[1].chain.count = 1;
    const size_t cap48 = size_t(48000 * (seconds + 1));
    auto memRaw = std::make_shared<MemorySink>(cap48);
    auto memClean = std::make_shared<MemorySink>(cap48);
    BusRuntime* b0 = rig.addBus(rig.addSink(memRaw));
    BusRuntime* b1 = rig.addBus(rig.addSink(memClean));
    (void)b0;
    (void)b1;
    raw->sends[0].enabled = true;
    clean->sends[1].enabled = true;
    rig.go();
    std::printf("Listening to '%s' for %.0f s (stay quiet for a few seconds, then speak). Nothing is saved.\n",
                opt.label.c_str(), seconds);
    float maxVad = 0.0f;
    for (int i = 0; i < int(seconds * 4); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        maxVad = std::max(maxVad, ns->vis[0].load());
        if (i % 4 == 3)
            std::printf("  t=%2.0fs  voice %3.0f%%  removing %5.1f dB  CPU %.2f%%\n", (i + 1) * 0.25, ns->vis[0].load() * 100.0f,
                        -ns->vis[1].load(), ns->cpuLoad.load() * 100.0f);
    }
    rig.engine.stop();
    const auto& a = memRaw->leftData();
    const auto& b = memClean->leftData();
    const size_t n = std::min(memRaw->written(), memClean->written());
    const int lat = ns->latencySamples();
    if (n < size_t(48000 * 2) || cap->onlySilenceSoFar()) {
        result("noise suppression on the microphone", -1, "no microphone signal (muted or blocked by privacy settings?)");
        return;
    }
    // 10 ms frame energies, time-aligned, first 0.5 s skipped (start-up).
    std::vector<std::pair<double, double>> fr; // (raw, clean)
    for (size_t i = 24000; i + 480 + lat <= n; i += 480) {
        double ea = 0, eb = 0;
        for (int k = 0; k < 480; ++k) {
            ea += double(a[i + k]) * a[i + k];
            eb += double(b[i + k + lat]) * b[i + k + lat];
        }
        fr.push_back({ea / 480 + 1e-14, eb / 480 + 1e-14});
    }
    std::vector<double> sorted;
    for (auto& f : fr) sorted.push_back(f.first);
    std::sort(sorted.begin(), sorted.end());
    const double quietTh = sorted[sorted.size() * 3 / 10]; // quietest 30% = room noise
    const double loudTh = sorted[sorted.size() * 95 / 100]; // loudest 5% = speech/peaks
    double qa = 0, qb = 0, la = 0, lb = 0;
    int nq = 0, nl = 0;
    for (auto& f : fr) {
        if (f.first <= quietTh) {
            qa += f.first;
            qb += f.second;
            ++nq;
        } else if (f.first >= loudTh) {
            la += f.first;
            lb += f.second;
            ++nl;
        }
    }
    const double roomIn = 10 * std::log10(qa / nq), roomOut = 10 * std::log10(qb / nq);
    const double loudIn = 10 * std::log10(la / nl), loudOut = 10 * std::log10(lb / nl);
    const bool spoke = maxVad > 0.8f && loudIn - roomIn > 20.0;
    char buf[512];
    std::snprintf(buf, sizeof(buf),
                  "%s: room noise %.1f -> %.1f dBFS (%.1f dB), loudest passages %.1f -> %.1f dBFS (%+.1f dB), max voice "
                  "probability %.0f%%, CPU %.2f%% of one core, latency %.0f ms%s",
                  opt.label.c_str(), roomIn, roomOut, roomOut - roomIn, loudIn, loudOut, loudOut - loudIn, maxVad * 100.0f,
                  ns->cpuLoad.load() * 100.0f, lat / 48.0, spoke ? "" : " (no speech detected: speech level not judged)");
    const bool ok = roomOut - roomIn <= -6.0 && (!spoke || loudOut - loudIn > -6.0);
    result("noise suppression on the microphone", ok ? 1 : -1, buf);
}

void cmdPlayback(DeviceManager& dm) {
    const std::string id = dm.defaultDeviceId(Flow::Render);
    if (id.empty()) {
        result("physical playback", 0, "no default render device");
        return;
    }
    auto info = dm.find(id);
    StreamOptions ro;
    ro.deviceId = fromUtf8(id);
    ro.label = info ? info->name : id;
    std::string err;
    auto ren = WasapiRender::open(ro, &err);
    if (!ren) {
        result("physical playback", -1, ro.label + ": " + err);
        return;
    }
    StreamOptions lo = ro;
    lo.captureMode = CaptureMode::Loopback;
    lo.label = ro.label + " (loopback)";
    auto loop = WasapiCapture::open(lo, &err);
    if (!loop) {
        result("loopback capture", -1, err);
        return;
    }

    EngineRig rig(48000, 480);
    auto tone = std::make_shared<ToneSource>();
    tone->wave = ToneSource::Sine;
    tone->frequency = 1000.0f;
    tone->levelDb = -40.0f;
    int sTone = rig.addSource(tone);
    int sLoop = rig.addSource(std::make_shared<DeviceCaptureSource>(loop));
    StripRuntime* toneStrip = rig.addStrip(sTone);
    StripRuntime* loopStrip = rig.addStrip(sLoop);
    auto devSink = std::make_shared<DeviceRenderSink>(ren);
    int kDev = rig.addSink(devSink);
    devSink->setMaster(true, rig.engine.wakeTarget());
    rig.g->masterSink = kDev;
    auto mem = std::make_shared<MemorySink>(48000 * 8);
    int kMem = rig.addSink(mem);
    rig.addBus(kDev);
    rig.addBus(kMem);
    toneStrip->sends[0].enabled = true; // tone -> speakers
    loopStrip->sends[1].enabled = true; // loopback -> memory
    rig.go();
    std::printf("Playing a -40 dBFS 1 kHz tone on '%s' for 2 s (verifying via loopback)...\n", ro.label.c_str());
    std::this_thread::sleep_for(std::chrono::milliseconds(2000));

    // Latency: switch to chirp bursts and cross-correlate the loopback
    // capture with the known template (robust against other audio playing).
    tone->wave = ToneSource::ChirpTrain;
    tone->levelDb = -30.0f;
    tone->impulsePeriodSec = 0.5f;
    std::this_thread::sleep_for(std::chrono::milliseconds(2700));
    rig.engine.stop();

    const auto& d = mem->leftData();
    const double a = goertzel(d, 48000, 48000, 1000.0, 48000.0);
    char buf[512];
    std::snprintf(buf, sizeof(buf), "'%s' [%s]: tone level in loopback %.1f dBFS (sent at -40; device effects/APOs "
                                    "can change this), render underruns %llu, engine paced by the device clock",
                  ro.label.c_str(), formatToString(ren->format()).c_str(), gainToDb(float(a)),
                  (unsigned long long)ren->stats.underruns.load());
    result("physical playback + loopback verification", (a > dbToGain(-50.f) && a < dbToGain(-28.f)) ? 1 : -1, buf);
    measureChirpLatency("output -> loopback round trip", *tone, *mem, 48000.0);
    const auto* g = rig.engine.current();
    std::printf("  Echo-side estimates: output path %.1f ms (device period %d frames, device latency %.1f ms), "
                "loopback input path %.1f ms (period %d frames)\n",
                g->sinks[kDev]->latencySeconds() * 1000.0, ren->periodFrames(), ren->deviceLatencySeconds() * 1000.0,
                g->sources[sLoop]->latencySeconds() * 1000.0, loop->periodFrames());
}

// The virtual microphone as another app sees it: Echo's engine plays into
// the cable's playback side (device-paced, exactly like a "B" bus) while a
// separate stream records the cable's recording side, as Discord/OBS would.
int g_vmSeconds = 15;
void cmdVirtualMic(DeviceManager& dm) {
    const auto pairs = findCablePairs(dm.devices());
    const CablePair* pair = nullptr;
    for (const auto& p : pairs)
        if (!p.secondary) {
            pair = &p;
            break;
        }
    if (!pair) {
        result("virtual microphone (engine -> cable -> recording app)", 0, "no virtual cable installed");
        return;
    }
    std::string err;
    StreamOptions ro;
    ro.deviceId = fromUtf8(pair->renderId);
    ro.label = pair->renderName;
    auto ren = WasapiRender::open(ro, &err);
    if (!ren) {
        result("virtual microphone (engine -> cable -> recording app)", -1, pair->renderName + ": " + err);
        return;
    }
    StreamOptions co;
    co.deviceId = fromUtf8(pair->captureId);
    co.label = pair->captureName;
    auto cap = WasapiCapture::open(co, &err);
    if (!cap) {
        result("virtual microphone (engine -> cable -> recording app)", -1, pair->captureName + ": " + err);
        return;
    }
    EngineRig rig(48000, 480);
    auto tone = std::make_shared<ToneSource>();
    tone->wave = ToneSource::Sine;
    tone->frequency = 1000.0f;
    tone->levelDb = -20.0f;
    const int sTone = rig.addSource(tone);
    const int sCable = rig.addSource(std::make_shared<DeviceCaptureSource>(cap));
    StripRuntime* toneStrip = rig.addStrip(sTone);
    StripRuntime* appStrip = rig.addStrip(sCable);
    // As in a normal session: the physical output (silent here) clocks the
    // engine and the virtual-mic bus follows it with drift correction.
    std::shared_ptr<WasapiRender> phones;
    if (const std::string phonesId = dm.defaultDeviceId(Flow::Render); !phonesId.empty()) {
        StreamOptions po;
        po.deviceId = fromUtf8(phonesId);
        po.label = "default output (clock, silent)";
        phones = WasapiRender::open(po, &err);
    }
    auto devSink = std::make_shared<DeviceRenderSink>(ren);
    const int kDev = rig.addSink(devSink);
    if (phones) {
        auto clockSink = std::make_shared<DeviceRenderSink>(phones);
        const int kClock = rig.addSink(clockSink);
        clockSink->setMaster(true, rig.engine.wakeTarget());
        rig.g->masterSink = kClock;
    } else {
        devSink->setMaster(true, rig.engine.wakeTarget());
        rig.g->masterSink = kDev;
    }
    auto mem = std::make_shared<MemorySink>(size_t(48000) * (g_vmSeconds + 4));
    const int kMem = rig.addSink(mem);
    rig.addBus(kDev);
    rig.addBus(kMem);
    toneStrip->sends[0].enabled = true; // Echo -> virtual mic bus -> CABLE Input
    appStrip->sends[1].enabled = true;  // "recording app" listening to CABLE Output
    rig.go();
    std::printf("Echo plays a -20 dBFS 1 kHz tone into '%s'; recording '%s' (not audible on your speakers)...\n",
                pair->renderName.c_str(), pair->captureName.c_str());
    std::this_thread::sleep_for(std::chrono::milliseconds(g_vmSeconds * 1000));
    tone->wave = ToneSource::ChirpTrain;
    tone->levelDb = -20.0f;
    tone->impulsePeriodSec = 0.5f;
    std::this_thread::sleep_for(std::chrono::milliseconds(2700));
    rig.engine.stop();
    // Tone amplitude in every 1 ms period (48 samples = one 1 kHz cycle, so a
    // slight rate adjustment by the recording side does not affect it).
    const auto& d = mem->leftData();
    const size_t periods = std::min(d.size(), size_t(g_vmSeconds) * 48000) / 48;
    std::vector<double> amp;
    for (size_t k = 0; k < periods; ++k) {
        double re = 0, im = 0;
        for (int n = 0; n < 48; ++n) {
            const double w = 2 * kPiD * n / 48.0;
            re += d[k * 48 + n] * std::cos(w);
            im -= d[k * 48 + n] * std::sin(w);
        }
        amp.push_back(2 * std::sqrt(re * re + im * im) / 48);
    }
    // Gaps (amplitude below half): during start-up (both cable sides
    // syncing; VB-CABLE settles within a few seconds) and afterwards.
    constexpr size_t kSettleMs = 4000;
    int startupGaps = 0, steadyGaps = 0, steadyGapMs = 0;
    bool inGap = false, started = false;
    std::string gapList;
    for (size_t k = 0; k < amp.size(); ++k) {
        const bool low = amp[k] < 0.05;
        if (!started) { // until the tone first arrives (path latency)
            started = !low;
            continue;
        }
        if (low && !inGap) {
            (k < kSettleMs ? startupGaps : steadyGaps)++;
            if (gapList.size() < 120) gapList += " " + std::to_string(k) + "ms";
        }
        if (low && k >= kSettleMs) ++steadyGapMs;
        inGap = low;
    }
    std::vector<double> steady(amp.begin() + std::min(amp.size(), kSettleMs), amp.end());
    std::sort(steady.begin(), steady.end());
    const double level = steady.empty() ? 0.0 : gainToDb(float(steady[steady.size() / 2]));
    const auto* srcRt = static_cast<DeviceCaptureSource*>(rig.engine.current()->sources[sCable]);
    char buf[640];
    std::snprintf(buf, sizeof(buf),
                  "'%s' received Echo's tone at %.1f dBFS (sent at -20) over %d s; gaps after start-up: %d (%d ms), "
                  "during the first %.0f s: %d%s; Echo render underruns %llu, recording-side underruns %llu, "
                  "recording-side discontinuities %llu",
                  pair->captureName.c_str(), level, g_vmSeconds, steadyGaps, steadyGapMs, kSettleMs / 1000.0, startupGaps,
                  gapList.empty() ? "" : (" (at" + gapList + ")").c_str(), (unsigned long long)ren->stats.underruns.load(),
                  (unsigned long long)srcRt->underruns.load(), (unsigned long long)cap->stats.discontinuities.load());
    // Echo itself must never run short. Short gaps inside the cable driver
    // also occur with a plain Windows stream (see `cableraw`), so they only
    // fail the test above 0.1 % of the time.
    const bool ok = started && std::fabs(level + 20.0) < 1.0 && ren->stats.underruns.load() == 0 &&
                    cap->stats.discontinuities.load() == 0 && steadyGapMs <= g_vmSeconds;
    result("virtual microphone (engine -> cable -> recording app)", ok ? 1 : -1, buf);
    measureChirpLatency("Echo output -> virtual mic recording side", *tone, *mem, 48000.0);
}

// App capture under load, like capturing Chrome while a game runs: this
// process plays a tone into the virtual cable (inaudible), captures its own
// audio with process loopback (as for any app), with the default output as
// the silent engine clock. Optional busy threads imitate a game's CPU load.
// Reports gaps in the captured tone and every glitch counter.
void cmdAppSoak(DeviceManager& dm, int seconds, int loadThreads) {
    const auto pairs = findCablePairs(dm.devices());
    const CablePair* pair = nullptr;
    for (const auto& p : pairs)
        if (!p.secondary) {
            pair = &p;
            break;
        }
    const std::string outId = dm.defaultDeviceId(Flow::Render);
    if (!pair || outId.empty() || !processLoopbackSupported()) {
        result("app capture under load", 0, "needs a virtual cable, a default output and Windows 10 2004+");
        return;
    }
    std::string err;
    StreamOptions co;
    co.deviceId = fromUtf8(pair->renderId);
    co.label = pair->renderName;
    auto cable = WasapiRender::open(co, &err);
    StreamOptions oo;
    oo.deviceId = fromUtf8(outId);
    oo.label = "default output (clock, silent)";
    auto phones = WasapiRender::open(oo, &err);
    StreamOptions po;
    po.captureMode = CaptureMode::ProcessLoopback;
    po.processId = GetCurrentProcessId();
    po.includeProcessTree = true;
    po.label = "own process (app capture)";
    auto app = WasapiCapture::open(po, &err);
    if (!cable || !phones || !app) {
        result("app capture under load", -1, "could not open streams: " + err);
        return;
    }
    std::atomic<bool> stopLoad{false};
    std::vector<std::thread> burners;
    for (int i = 0; i < loadThreads; ++i)
        burners.emplace_back([&] {
            volatile double x = 1.0;
            while (!stopLoad.load(std::memory_order_relaxed)) x = x * 1.0000001 + 1e-9;
        });
    EngineRig rig(48000, 480);
    auto tone = std::make_shared<ToneSource>();
    tone->wave = ToneSource::Sine;
    tone->frequency = 1000.0f;
    tone->levelDb = -20.0f;
    const int sTone = rig.addSource(tone);
    auto appSrc = std::make_shared<DeviceCaptureSource>(app);
    const int sApp = rig.addSource(appSrc);
    StripRuntime* toneStrip = rig.addStrip(sTone);
    StripRuntime* appStrip = rig.addStrip(sApp);
    auto clockSink = std::make_shared<DeviceRenderSink>(phones);
    const int kClock = rig.addSink(clockSink);
    clockSink->setMaster(true, rig.engine.wakeTarget());
    rig.g->masterSink = kClock;
    auto cableSink = std::make_shared<DeviceRenderSink>(cable);
    const int kCable = rig.addSink(cableSink);
    auto mem = std::make_shared<MemorySink>(size_t(48000) * (seconds + 2));
    const int kMem = rig.addSink(mem);
    rig.addBus(kCable);
    rig.addBus(kMem);
    toneStrip->sends[0].enabled = true; // tone -> cable (this process's audio)
    appStrip->sends[1].enabled = true;  // captured app audio -> memory
    rig.go();
    std::printf("App capture for %d s with %d busy threads (tone into '%s', inaudible)...\n", seconds, loadThreads,
                pair->renderName.c_str());
    std::this_thread::sleep_for(std::chrono::seconds(seconds));
    rig.engine.stop();
    stopLoad = true;
    for (auto& t : burners) t.join();
    // Silent stretches in the captured tone after it first arrives.
    const auto& d = mem->leftData();
    const size_t periods = std::min(d.size(), mem->written()) / 48;
    int gapCount = 0, gapMs = 0;
    bool started = false, inGap = false;
    std::string list;
    for (size_t k = 0; k < periods; ++k) {
        double re = 0, im = 0;
        for (int n = 0; n < 48; ++n) {
            re += d[k * 48 + n] * std::cos(2 * kPiD * n / 48.0);
            im -= d[k * 48 + n] * std::sin(2 * kPiD * n / 48.0);
        }
        const bool low = 2 * std::sqrt(re * re + im * im) / 48 < 0.05;
        if (!started) {
            started = !low;
            continue;
        }
        if (low && !inGap) {
            ++gapCount;
            if (list.size() < 100) list += " " + std::to_string(k) + "ms";
        }
        gapMs += low;
        inGap = low;
    }
    char buf[600];
    std::snprintf(buf, sizeof(buf),
                  "%d s, %d busy threads: %d gaps (%d ms)%s in the captured app audio; app source: underruns %llu, "
                  "gaps %llu, backlog drops %llu, device overflow %llu, Windows discontinuities %llu, margin +%.0f ms; "
                  "app capture buffer %d frames",
                  seconds, loadThreads, gapCount, gapMs, list.c_str(), (unsigned long long)appSrc->underruns.load(),
                  (unsigned long long)appSrc->gaps.load(), (unsigned long long)appSrc->overruns.load(),
                  (unsigned long long)app->stats.overruns.load(), (unsigned long long)app->stats.discontinuities.load(),
                  appSrc->marginMs.load(), app->periodFrames());
    result("app capture under load", started && gapCount == 0 ? 1 : -1, buf);
}

// UI-thread work that touches Windows audio: how long one call takes.
void cmdPerf(DeviceManager& dm) {
    auto time = [](const char* what, int reps, auto fn) {
        std::vector<double> ms;
        for (int i = 0; i < reps; ++i) {
            const auto t0 = std::chrono::steady_clock::now();
            fn();
            ms.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
        }
        std::sort(ms.begin(), ms.end());
        std::printf("  %-48s median %7.2f ms   max %7.2f ms   (%d calls)\n", what, ms[ms.size() / 2], ms.back(), reps);
    };
    time("device list rescan (Rescan / device change)", 20, [&] { dm.rescan(); });
    time("virtual cable pairing", 20, [&] { (void)findCablePairs(dm.devices()); });
    AppSessionMonitor apps;
    time("per-app audio sessions refresh (every 1 s)", 20, [&] { apps.refresh(); });
    time("per-app level meters update (every frame)", 200, [&] { apps.updateLevels(); });
    result("UI-thread device work timing", 1, "see the lines above");
}

// Comparison for `virtualmic`: the same tone through the cable WITHOUT Echo's
// engine (plain WASAPI streams, `queueMs` queued on the playback side), to
// tell gaps caused by the cable driver apart from gaps caused by Echo.
void cmdCableRaw(DeviceManager& dm, int seconds, double queueMs) {
    const auto pairs = findCablePairs(dm.devices());
    const CablePair* pair = nullptr;
    for (const auto& p : pairs)
        if (!p.secondary) {
            pair = &p;
            break;
        }
    if (!pair) return;
    std::string err;
    StreamOptions ro;
    ro.deviceId = fromUtf8(pair->renderId);
    auto ren = WasapiRender::open(ro, &err);
    StreamOptions co;
    co.deviceId = fromUtf8(pair->captureId);
    auto cap = WasapiCapture::open(co, &err);
    if (!ren || !cap) return;
    const int rch = ren->channels(), cch = cap->channels();
    const double rsr = ren->sampleRate();
    std::vector<float> block, tmp(48000 * cch), captured;
    double phase = 0.0;
    const auto start = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - start < std::chrono::seconds(seconds)) {
        const size_t queued = ren->ring().size() / rch;
        const size_t want = static_cast<size_t>(rsr * queueMs / 1000.0);
        if (queued < want) {
            const size_t n = want - queued;
            block.assign(n * rch, 0.0f);
            for (size_t i = 0; i < n; ++i) {
                const float s = 0.1f * static_cast<float>(std::sin(2.0 * kPiD * phase));
                phase += 1000.0 / rsr;
                if (phase >= 1.0) phase -= 1.0;
                for (int c = 0; c < rch; ++c) block[i * rch + c] = s;
            }
            ren->ring().write(block.data(), block.size());
        }
        const size_t frames = std::min(cap->ring().readAvailable(), tmp.size()) / cch;
        if (frames) {
            cap->ring().read(tmp.data(), frames * cch);
            for (size_t i = 0; i < frames; ++i) captured.push_back(tmp[i * cch]);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    int gaps = 0, gapMs = 0;
    bool started = false, inGap = false;
    std::string list;
    for (size_t k = 0; k + 1 < captured.size() / 48; ++k) {
        double re = 0, im = 0;
        for (int n = 0; n < 48; ++n) {
            re += captured[k * 48 + n] * std::cos(2 * kPiD * n / 48.0);
            im -= captured[k * 48 + n] * std::sin(2 * kPiD * n / 48.0);
        }
        const bool low = 2 * std::sqrt(re * re + im * im) / 48 < 0.05;
        if (!started) {
            started = !low;
            continue;
        }
        if (low && !inGap) {
            ++gaps;
            if (list.size() < 100) list += " " + std::to_string(k) + "ms";
        }
        gapMs += low;
        inGap = low;
    }
    std::printf("bare WASAPI, %.0f ms queued, %d s: %d gaps (%d ms)%s; render underruns %llu, capture overruns %llu, "
                "capture discontinuities %llu\n",
                queueMs, seconds, gaps, gapMs, list.c_str(), (unsigned long long)ren->stats.underruns.load(),
                (unsigned long long)cap->stats.overruns.load(), (unsigned long long)cap->stats.discontinuities.load());
}

void cmdProcLoop(DeviceManager& dm) {
    if (!processLoopbackSupported()) {
        result("process loopback", 0, "requires Windows 10 build 19041+");
        return;
    }
    const std::string id = dm.defaultDeviceId(Flow::Render);
    if (id.empty()) {
        result("process loopback", 0, "no default render device");
        return;
    }
    StreamOptions ro;
    ro.deviceId = fromUtf8(id);
    ro.label = "default output";
    std::string err;
    auto ren = WasapiRender::open(ro, &err);
    if (!ren) {
        result("process loopback", -1, err);
        return;
    }
    StreamOptions po;
    po.captureMode = CaptureMode::ProcessLoopback;
    po.processId = GetCurrentProcessId();
    po.includeProcessTree = true;
    po.label = "process loopback (self)";
    auto pl = WasapiCapture::open(po, &err);
    if (!pl) {
        result("process loopback", -1, "activation failed: " + err);
        return;
    }
    EngineRig rig(48000, 480);
    auto tone = std::make_shared<ToneSource>();
    tone->frequency = 700.0f;
    tone->levelDb = -40.0f;
    int sTone = rig.addSource(tone);
    int sPl = rig.addSource(std::make_shared<DeviceCaptureSource>(pl));
    StripRuntime* t = rig.addStrip(sTone);
    StripRuntime* p = rig.addStrip(sPl);
    auto devSink = std::make_shared<DeviceRenderSink>(ren);
    int kDev = rig.addSink(devSink);
    devSink->setMaster(true, rig.engine.wakeTarget());
    rig.g->masterSink = kDev;
    auto mem = std::make_shared<MemorySink>(48000 * 9);
    int kMem = rig.addSink(mem);
    rig.addBus(kDev);
    rig.addBus(kMem);
    t->sends[0].enabled = true;
    p->sends[1].enabled = true;
    rig.go();
    std::printf("Capturing this process's own -40 dBFS 700 Hz tone via process loopback...\n");
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));
    const size_t mark1 = mem->written();

    // Mute this process's audio session and see whether process loopback is
    // affected (documents the Windows behaviour on this machine).
    bool muted = false;
    {
        ComPtr<IMMDeviceEnumerator> en;
        ComPtr<IMMDevice> dev;
        ComPtr<IAudioSessionManager2> mgr;
        ComPtr<ISimpleAudioVolume> vol;
        if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&en))) &&
            SUCCEEDED(en->GetDevice(ro.deviceId.c_str(), &dev)) &&
            SUCCEEDED(dev->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, nullptr, (void**)mgr.GetAddressOf())) &&
            SUCCEEDED(mgr->GetSimpleAudioVolume(nullptr, FALSE, &vol))) {
            muted = SUCCEEDED(vol->SetMute(TRUE, nullptr));
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1500));
        if (vol) vol->SetMute(FALSE, nullptr);
    }
    const size_t mark2 = mem->written();
    tone->wave = ToneSource::ChirpTrain;
    tone->levelDb = -30.0f;
    tone->impulsePeriodSec = 0.5f;
    std::this_thread::sleep_for(std::chrono::milliseconds(2700));
    rig.engine.stop();
    const auto& d = mem->leftData();
    const double a1 = goertzel(d, mark1 - 24000, 24000, 700.0, 48000.0);
    const double a2 = goertzel(d, mark2 - 24000, 24000, 700.0, 48000.0);
    char buf[256];
    std::snprintf(buf, sizeof(buf), "tone level via process loopback %.1f dBFS (expected about -40)", gainToDb(float(a1)));
    result("process loopback capture", (a1 > dbToGain(-46.f)) ? 1 : -1, buf);
    if (muted) {
        std::snprintf(buf, sizeof(buf), "with the app's session muted, process loopback level = %.1f dBFS -> %s",
                      gainToDb(float(a2)),
                      a2 > dbToGain(-46.f) ? "capture is PRE session-mute (app can be muted locally and still captured)"
                                           : "capture is POST session-mute (muting the app also silences capture)");
        result("process loopback vs session mute (behaviour probe)", 1, buf);
    }
    measureChirpLatency("output -> process loopback round trip", *tone, *mem, 48000.0);
}

void cmdCables(DeviceManager& dm) {
    auto pairs = findCablePairs(dm.devices());
    if (pairs.empty()) {
        result("virtual cable loopback", 0,
               "no loopback cable driver installed (e.g. VB-CABLE). Virtual microphone output cannot be verified.");
        return;
    }
    for (const auto& p : pairs) {
        CableTestResult r = testCable(p, 48000);
        char buf[512];
        std::snprintf(buf, sizeof(buf), "%s -> %s: %s (tone %.1f dBFS, latency %.1f ms)", p.renderName.c_str(),
                      p.captureName.c_str(), r.message.c_str(), r.levelDb, r.latencyMs);
        result("virtual cable loopback", r.ok ? 1 : -1, buf);
    }
}

void cmdSysAudio(DeviceManager& dm) {
    if (!processLoopbackSupported()) {
        result("system audio excluding Echo", 0, "requires Windows 10 build 19041+");
        return;
    }
    const std::string id = dm.defaultDeviceId(Flow::Render);
    StreamOptions ro;
    ro.deviceId = fromUtf8(id);
    ro.label = "default output";
    std::string err;
    auto ren = WasapiRender::open(ro, &err);
    if (!ren) {
        result("system audio excluding Echo", -1, err);
        return;
    }
    StreamOptions so;
    so.captureMode = CaptureMode::ProcessLoopback;
    so.processId = GetCurrentProcessId();
    so.includeProcessTree = false; // EXCLUDE this process tree
    so.label = "system audio excluding self";
    auto sys = WasapiCapture::open(so, &err);
    if (!sys) {
        result("system audio excluding Echo", -1, "activation failed: " + err);
        return;
    }
    StreamOptions po = so;
    po.includeProcessTree = true;
    po.label = "self";
    auto self = WasapiCapture::open(po, &err);
    EngineRig rig(48000, 480);
    auto tone = std::make_shared<ToneSource>();
    tone->frequency = 1300.0f;
    tone->levelDb = -40.0f;
    int sTone = rig.addSource(tone);
    int sSys = rig.addSource(std::make_shared<DeviceCaptureSource>(sys));
    int sSelf = self ? rig.addSource(std::make_shared<DeviceCaptureSource>(self)) : -1;
    StripRuntime* t = rig.addStrip(sTone);
    StripRuntime* a = rig.addStrip(sSys);
    StripRuntime* b = sSelf >= 0 ? rig.addStrip(sSelf) : nullptr;
    auto devSink = std::make_shared<DeviceRenderSink>(ren);
    int kDev = rig.addSink(devSink);
    devSink->setMaster(true, rig.engine.wakeTarget());
    rig.g->masterSink = kDev;
    auto memSys = std::make_shared<MemorySink>(48000 * 5);
    auto memSelf = std::make_shared<MemorySink>(48000 * 5);
    int k1 = rig.addSink(memSys), k2 = rig.addSink(memSelf);
    rig.addBus(kDev);
    rig.addBus(k1);
    rig.addBus(k2);
    t->sends[0].enabled = true;
    a->sends[1].enabled = true;
    if (b) b->sends[2].enabled = true;
    rig.go();
    std::printf("Playing a -40 dBFS 1.3 kHz tone from this process for 2.5 s...\n");
    std::this_thread::sleep_for(std::chrono::milliseconds(2500));
    rig.engine.stop();
    const double aSys = goertzel(memSys->leftData(), memSys->written() - 48000, 48000, 1300.0, 48000.0);
    const double aSelf = b ? goertzel(memSelf->leftData(), memSelf->written() - 48000, 48000, 1300.0, 48000.0) : 0.0;
    char buf[256];
    std::snprintf(buf, sizeof(buf), "own tone in 'all apps except Echo' capture: %.1f dBFS; in own-process capture: %.1f dBFS",
                  gainToDb(float(aSys)), gainToDb(float(aSelf)));
    const bool ok = aSys < dbToGain(-70.0f) && (!b || aSelf > dbToGain(-46.0f));
    result("system audio excluding Echo (feedback safety)", ok ? 1 : -1, buf);
}

void cmdAppRoute(DeviceManager& dm) {
    ComScope sta(COINIT_APARTMENTTHREADED); // nested init is harmless if already MTA
    AppOutputRouting r;
    if (!r.available()) {
        result("per-app output routing (experimental)", 0, "interface not available on this Windows build");
        return;
    }
    const DWORD pid = GetCurrentProcessId();
    std::string target;
    for (const auto& d : dm.devices(Flow::Render))
        if (!d.isDefault && d.virtualKind == VirtualKind::None) {
            target = d.idUtf8;
            break;
        }
    if (target.empty()) {
        result("per-app output routing (experimental)", 0, "needs a second active output device");
        return;
    }
    const std::string before = r.appOutput(pid);
    std::string err;
    const bool setOk = r.setAppOutput(pid, target, &err);
    const std::string during = r.appOutput(pid);
    const bool resetOk = r.setAppOutput(pid, "", &err);
    const std::string after = r.appOutput(pid);
    char buf[512];
    std::snprintf(buf, sizeof(buf), "before='%s' set=%s read-back %s, reset=%s after='%s'%s", before.c_str(),
                  setOk ? "ok" : "failed", during == target ? "matches" : ("MISMATCH '" + during + "'").c_str(),
                  resetOk ? "ok" : "failed", after.c_str(), err.empty() ? "" : (" error: " + err).c_str());
    result("per-app output routing (experimental)", setOk && resetOk && during == target && after.empty() ? 1 : -1, buf);
}

void cmdSoak(DeviceManager& dm, double seconds) {
    const std::string micId = dm.defaultDeviceId(Flow::Capture);
    const std::string outId = dm.defaultDeviceId(Flow::Render);
    if (micId.empty() || outId.empty()) {
        result("soak", 0, "needs a default microphone and output");
        return;
    }
    std::string err;
    StreamOptions mo;
    mo.deviceId = fromUtf8(micId);
    mo.label = "mic";
    auto mic = WasapiCapture::open(mo, &err);
    StreamOptions oo;
    oo.deviceId = fromUtf8(outId);
    oo.label = "output";
    auto ren = WasapiRender::open(oo, &err);
    StreamOptions so;
    so.captureMode = CaptureMode::ProcessLoopback;
    so.processId = GetCurrentProcessId();
    so.includeProcessTree = false;
    so.label = "system";
    auto sys = processLoopbackSupported() ? WasapiCapture::open(so, &err) : nullptr;
    if (!mic || !ren) {
        result("soak", -1, "could not open devices: " + err);
        return;
    }
    // A virtual cable, if installed, as a second (drift-corrected) output.
    // Only silence is sent to it.
    std::shared_ptr<WasapiRender> cable;
    for (const auto& d : dm.devices(Flow::Render, true))
        if (d.virtualKind == VirtualKind::Cable && d.name.find("16ch") == std::string::npos) {
            StreamOptions co;
            co.deviceId = d.id;
            co.label = d.name;
            cable = WasapiRender::open(co, &err);
            if (cable) break;
        }
    EngineRig rig(48000, 480);
    auto micSrc = std::make_shared<DeviceCaptureSource>(mic);
    int sMic = rig.addSource(micSrc);
    std::shared_ptr<DeviceCaptureSource> sysSrc;
    int sSys = -1;
    if (sys) {
        sysSrc = std::make_shared<DeviceCaptureSource>(sys);
        sSys = rig.addSource(sysSrc);
    }
    StripRuntime* m = rig.addStrip(sMic);
    if (sSys >= 0) rig.addStrip(sSys);
    auto devSink = std::make_shared<DeviceRenderSink>(ren);
    int kDev = rig.addSink(devSink);
    devSink->setMaster(true, rig.engine.wakeTarget());
    rig.g->masterSink = kDev;
    auto mem = std::make_shared<MemorySink>(48000);
    int kMem = rig.addSink(mem);
    rig.addBus(kDev); // silent: nothing is routed to the speakers
    rig.addBus(kMem);
    std::shared_ptr<DeviceRenderSink> cableSink;
    if (cable) {
        cableSink = std::make_shared<DeviceRenderSink>(cable);
        rig.addBus(rig.addSink(cableSink)); // silent
    }
    m->sends[1].enabled = true;
    rig.go();
    std::printf("Soak: mic + system capture + output%s for %.0f s (nothing audible is played)...\n",
                cable ? " + virtual cable (silence)" : "", seconds);
    float maxLoad = 0;
    std::vector<float> micPpm, micFill, cabPpm, cabFill;
    const int samples = static_cast<int>(seconds * 20);
    for (int i = 0; i < samples; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        if (i % 20 == 19) {
            maxLoad = std::max(maxLoad, rig.engine.stats().cpuPeak.exchange(0.0f));
            mem->rewind();
        }
        if (std::getenv("ECHO_SOAK_TRACE") && i >= 100)
            std::printf("T %.2f mic %+.0f ppm %.1f ms cable %+.0f ppm %.1f ms\n", i / 20.0, micSrc->driftPpm.load(),
                        micSrc->fillMs.load(), cableSink ? cableSink->driftPpm.load() : 0.0f,
                        cableSink ? cableSink->fillMs.load() : 0.0f);
        if (i >= 100) { // after 5 s of settling
            micPpm.push_back(micSrc->driftPpm.load());
            micFill.push_back(micSrc->fillMs.load());
            if (cableSink) {
                cabPpm.push_back(cableSink->driftPpm.load());
                cabFill.push_back(cableSink->fillMs.load());
            }
        }
        if ((i + 1) % 300 == 0)
            std::printf("  %3d s: mic fill %.1f ms drift %+.0f ppm under %llu over %llu | out fill %.1f ms under %llu | DSP %.2f%%\n",
                        (i + 1) / 20, micSrc->fillMs.load(), micSrc->driftPpm.load(),
                        (unsigned long long)micSrc->underruns.load(), (unsigned long long)micSrc->overruns.load(),
                        devSink->fillMs.load(), (unsigned long long)ren->stats.underruns.load(),
                        rig.engine.stats().cpuLoad.load() * 100.0f);
    }
    rig.engine.stop();
    auto pct = [](std::vector<float> v, const char* what, const char* unit) {
        if (v.empty()) return;
        std::sort(v.begin(), v.end());
        auto at = [&](double q) { return v[std::min(v.size() - 1, static_cast<size_t>(q * v.size()))]; };
        std::printf("  %-26s min %+8.1f  p1 %+8.1f  p50 %+8.1f  p99 %+8.1f  max %+8.1f %s\n", what, v.front(), at(0.01),
                    at(0.5), at(0.99), v.back(), unit);
    };
    pct(micPpm, "mic drift correction", "ppm");
    pct(micFill, "mic buffer level", "ms");
    pct(cabPpm, "cable output drift corr.", "ppm");
    pct(cabFill, "cable output buffer level", "ms");
    const uint64_t dropouts = micSrc->underruns.load() + micSrc->overruns.load() + ren->stats.underruns.load() +
                              devSink->underruns.load() + devSink->overruns.load() +
                              (sysSrc ? sysSrc->overruns.load() : 0) +
                              (cableSink ? cableSink->underruns.load() + cableSink->overruns.load() +
                                               cable->stats.underruns.load()
                                         : 0);
    char buf[600];
    std::snprintf(buf, sizeof(buf),
                  "%.0f s, %llu engine blocks, %llu dropouts (mic under %llu over %llu, out device %llu, out ring "
                  "%llu/%llu, system over %llu, cable %llu/%llu device %llu), peak block load %.1f%%",
                  seconds, (unsigned long long)rig.engine.stats().blocks.load(), (unsigned long long)dropouts,
                  (unsigned long long)micSrc->underruns.load(), (unsigned long long)micSrc->overruns.load(),
                  (unsigned long long)ren->stats.underruns.load(), (unsigned long long)devSink->underruns.load(),
                  (unsigned long long)devSink->overruns.load(), (unsigned long long)(sysSrc ? sysSrc->overruns.load() : 0),
                  (unsigned long long)(cableSink ? cableSink->underruns.load() : 0),
                  (unsigned long long)(cableSink ? cableSink->overruns.load() : 0),
                  (unsigned long long)(cable ? cable->stats.underruns.load() : 0), maxLoad * 100.0f);
    std::printf("  adaptive margin at the end: mic %.1f ms, cable %.1f ms\n", micSrc->marginMs.load(),
                cableSink ? cableSink->marginMs.load() : 0.0f);
    result("soak (long-running stability on real devices)", dropouts == 0 ? 1 : -1, buf);
}

} // namespace

// Writes the factory presets as documented .echopreset files.
int exportPresets(const std::wstring& dir) {
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    int n = 0;
    for (const auto& p : factoryChainPresets()) {
        std::string file;
        for (char c : p.name) file.push_back(std::isalnum(static_cast<unsigned char>(c)) || c == ' ' || c == '-' ? c : '_');
        std::string err;
        const std::wstring path = dir + L"\\" + utf8ToWide(file) + L".echopreset";
        if (saveJsonFile(path, PresetLibrary::toJson(p), &err)) {
            ++n;
            std::filesystem::remove(path + L".bak", ec); // repository copies need no backups
        } else {
            std::printf("failed: %s (%s)\n", p.name.c_str(), err.c_str());
        }
    }
    std::printf("exported %d presets\n", n);
    return 0;
}

// Push to talk through GetAsyncKeyState: synthetic F24/F23 presses (no
// application uses them) must open/close the gate and toggle mute.
void cmdHotkeys() {
    std::printf("\n== Hotkeys (push to talk, mute) ==\n");
    HotkeyService hk;
    std::atomic<int> gateChanges{0};
    std::atomic<bool> gate{true};
    hk.start([&](bool open) {
        gate = open;
        ++gateChanges;
    });
    HotkeySettings s;
    s.talkMode = TalkMode::PushToTalk;
    s.talkKey = {VK_F24, 0};
    s.muteKey = {VK_F23, 0};
    s.releaseDelayMs = 150;
    hk.setSettings(s);
    auto key = [](WORD vk, bool down) {
        INPUT in{};
        in.type = INPUT_KEYBOARD;
        in.ki.wVk = vk;
        in.ki.dwFlags = down ? 0 : KEYEVENTF_KEYUP;
        return SendInput(1, &in, sizeof(in)) == 1;
    };
    // Spins (yielding) instead of Sleep(1), whose ~15.6 ms granularity would
    // swamp the measurement.
    auto waitFor = [](const std::function<bool()>& cond, double seconds) {
        const double t0 = monotonicSeconds();
        while (monotonicSeconds() - t0 < seconds) {
            if (cond()) return monotonicSeconds() - t0;
            std::this_thread::yield();
        }
        return -1.0;
    };
    const double closed = waitFor([&] { return !gate.load(); }, 0.5);
    result("push to talk starts closed", closed >= 0 ? 1 : -1, closed >= 0 ? "microphone gate closed" : "gate stayed open");
    if (!key(VK_F24, true)) {
        result("push to talk key", 0, "SendInput was blocked (another program has priority), not tested");
        hk.stop();
        return;
    }
    double opened = waitFor([&] { return gate.load(); }, 0.5);
    key(VK_F24, false);
    char buf[160];
    // A few more presses for a typical figure.
    double worst = opened;
    int opens = opened >= 0 ? 1 : 0;
    for (int i = 0; i < 9 && opened >= 0; ++i) {
        waitFor([&] { return !gate.load(); }, 1.0);
        Sleep(7 * (i + 1) % 13); // land at different points of the 5 ms poll
        key(VK_F24, true);
        const double t = waitFor([&] { return gate.load(); }, 0.5);
        key(VK_F24, false);
        if (t >= 0) ++opens;
        worst = std::max(worst, t);
    }
    std::snprintf(buf, sizeof(buf), "gate opened %.1f ms after the first key press; slowest of %d presses %.1f ms",
                  opened * 1000.0, opens, worst * 1000.0);
    result("push to talk opens", opened >= 0 && opens == 10 && worst < 0.05 ? 1 : -1, opened >= 0 ? buf : "gate did not open");
    const double reclosed = waitFor([&] { return !gate.load(); }, 1.0);
    std::snprintf(buf, sizeof(buf), "closed %.0f ms after release (release delay 150 ms)", reclosed * 1000.0);
    result("push to talk release delay", reclosed >= 0.12 && reclosed < 0.25 ? 1 : -1, reclosed >= 0 ? buf : "gate stayed open");

    key(VK_F23, true);
    Sleep(30);
    key(VK_F23, false);
    Sleep(30);
    key(VK_F23, true);
    Sleep(30);
    key(VK_F23, false);
    Sleep(30);
    const int toggles = hk.takeMuteToggles();
    std::snprintf(buf, sizeof(buf), "%d presses counted (expected 2)", toggles);
    result("mute key presses", toggles == 2 ? 1 : -1, buf);

    s.talkMode = TalkMode::Off;
    hk.setSettings(s);
    const double reopened = waitFor([&] { return gate.load(); }, 0.5);
    result("push to talk off opens the microphone", reopened >= 0 ? 1 : -1, reopened >= 0 ? "gate open" : "gate stayed closed");
    hk.stop();
}

int wmain(int argc, wchar_t** argv) {
    SetConsoleOutputCP(CP_UTF8);
    if (argc > 2 && std::wstring(argv[1]) == L"export-presets") return exportPresets(argv[2]);
    ComScope com(COINIT_MULTITHREADED);
    std::wstring cmd = argc > 1 ? argv[1] : L"list";
    DeviceManager dm;
    std::string err;
    if (!dm.init(&err)) {
        std::printf("Device manager init failed: %s\n", err.c_str());
        return 2;
    }
    if (cmd == L"list" || cmd == L"all") cmdList(dm);
    if (cmd == L"mic" || cmd == L"all") cmdMic(dm, argc > 2 && cmd == L"mic" ? _wtof(argv[2]) : 2.0);
    if (cmd == L"playback" || cmd == L"all") cmdPlayback(dm);
    if (cmd == L"procloop" || cmd == L"all") cmdProcLoop(dm);
    if (cmd == L"cables" || cmd == L"all") cmdCables(dm);
    if (cmd == L"virtualmic" || cmd == L"all") {
        if (cmd == L"virtualmic" && argc > 2) g_vmSeconds = std::max(6, _wtoi(argv[2]));
        cmdVirtualMic(dm);
    }
    if (cmd == L"sysaudio" || cmd == L"all") cmdSysAudio(dm);
    if (cmd == L"approute") cmdAppRoute(dm);
    if (cmd == L"perf") cmdPerf(dm);
    if (cmd == L"appsoak") cmdAppSoak(dm, argc > 2 ? _wtoi(argv[2]) : 60, argc > 3 ? _wtoi(argv[3]) : 0);
    if (cmd == L"cableraw") cmdCableRaw(dm, argc > 2 ? _wtoi(argv[2]) : 15, argc > 3 ? _wtof(argv[3]) : 40.0);
    if (cmd == L"soak") cmdSoak(dm, argc > 2 ? _wtof(argv[2]) : 60.0);
    if (cmd == L"denoise") cmdDenoise(dm, argc > 2 ? _wtof(argv[2]) : 10.0);
    if (cmd == L"hotkeys") cmdHotkeys();
    std::printf("\nSummary: %d passed, %d failed, %d skipped\n", g_pass, g_fail, g_skip);
    return g_fail ? 1 : 0;
}
