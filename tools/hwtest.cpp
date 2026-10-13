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
//   echo_hwtest voice [name] [sec]  a voice preset (default Pain) in real time: a tone through its
//                                 effects -> cable -> recording side; level and distortion against
//                                 the offline result, peak, glitches, clicks when switching it
//                                 off and on. Audio stays in memory only.
//   echo_hwtest soundboard        built-in sounds through the soundboard input -> cable ->
//                                 recording side: level, shape, latency, glitches (memory only)
//   echo_hwtest hotkeys           push to talk / mute keys through Windows' real key state:
//                                 presses F24, F23 and F22 (instant taps) with SendInput (keys no app uses)
//   echo_hwtest ort-install <folder>  install ONNX Runtime (DirectML build) there, as the AI setup button does
//   echo_hwtest vbcable-download <folder>  download, check and unpack VB-Audio's VB-CABLE pack as the
//                                 app's Install button does (doesn't open the installer)
//   echo_hwtest all               run everything above (playback is audible, -40 dBFS)
#include <windows.h>
#include <audiopolicy.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <cstdio>
#include <functional>
#include <limits>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "ai/ContentVec.h"
#include "ai/FaissIndex.h"
#include "ai/OnnxGraph.h"
#include "ai/AiVoiceEffect.h"
#include "ai/OrtSession.h"
#include "ai/RvcVoice.h"
#include "ai/RvcModel.h"
#include "ai/TorchFile.h"
#include "audio/AppRouting.h"
#include "audio/Download.h"
#include "audio/HotkeyService.h"
#include "audio/OrtInstall.h"
#include "audio/VbCable.h"
#include "audio/AppSessions.h"
#include "audio/AudioDecoder.h"
#include "audio/DeviceManager.h"
#include "audio/VirtualCables.h"
#include "audio/WasapiStream.h"
#include "core/Engine.h"
#include "core/Log.h"
#include "core/Resampler.h"
#include "core/Text.h"
#include "core/WavFile.h"
#include "dsp/EffectRegistry.h"
#include "dsp/NoiseSuppressorInfo.h"
#include "core/SoundboardSource.h"
#include "session/Presets.h"
#include "session/Soundboard.h"

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

// A voice preset in real time: Echo plays a 300 Hz tone (-18 dBFS) through
// the preset's effect chain into the virtual cable's playback side and
// records the recording side (what Discord/OBS receive), with the voice
// switched off and on again near the end. Compares level and distortion with
// the same chain run offline, and checks the peak, glitch counters and the
// switches for clicks. Not audible; the audio stays in memory.
void cmdVoice(DeviceManager& dm, const std::string& presetName, double seconds) {
    const std::string what = "voice '" + presetName + "' through the virtual microphone";
    const auto presets = factoryChainPresets();
    const ChainPreset* preset = nullptr;
    for (const auto& p : presets)
        if (p.name == presetName) preset = &p;
    if (!preset) {
        result(what.c_str(), -1, "no factory voice with that name");
        return;
    }
    const auto pairs = findCablePairs(dm.devices());
    const CablePair* pair = nullptr;
    for (const auto& p : pairs)
        if (!p.secondary) {
            pair = &p;
            break;
        }
    if (!pair) {
        result(what.c_str(), 0, "no virtual cable installed");
        return;
    }
    std::string err;
    StreamOptions ro;
    ro.deviceId = fromUtf8(pair->renderId);
    ro.label = pair->renderName;
    auto ren = WasapiRender::open(ro, &err);
    StreamOptions co;
    co.deviceId = fromUtf8(pair->captureId);
    co.label = pair->captureName;
    auto cap = ren ? WasapiCapture::open(co, &err) : nullptr;
    if (!ren || !cap) {
        result(what.c_str(), -1, err);
        return;
    }
    constexpr double kSr = 48000.0, kFreq = 300.0;
    constexpr float kLevel = -18.0f;
    auto makeChain = [&] {
        auto c = chainFromJson(preset->chain, nullptr);
        for (auto& e : c) e->prepare(kSr, 480);
        return c;
    };

    // Expected result: the same chain offline on the same tone.
    double expRms = 0.0, expThd = 0.0;
    {
        auto c = makeChain();
        ChainSnap snap;
        for (auto& e : c) snap.fx[snap.count++] = e.get();
        ChainRunner runner;
        runner.prepare(480);
        std::vector<float> x(size_t(kSr * 2)), r(480);
        for (size_t i = 0; i < x.size(); ++i) x[i] = dbToGain(kLevel) * float(std::sin(2 * kPiD * kFreq * i / kSr));
        for (size_t pos = 0; pos < x.size(); pos += 480) {
            bool mono = true;
            runner.process(snap, x.data() + pos, r.data(), mono, 480);
        }
        const size_t a = x.size() - 9600;
        double e = 0;
        for (size_t i = a; i < x.size(); ++i) e += double(x[i]) * x[i];
        expRms = std::sqrt(e / 9600);
        const double fund = goertzel(x, a, 9600, kFreq, kSr), total = std::sqrt(2.0) * expRms;
        expThd = std::sqrt(std::max(0.0, total * total - fund * fund)) / std::max(fund, 1e-9);
    }

    EngineRig rig(int(kSr), 480);
    auto chain = makeChain();
    for (auto& e : chain) rig.keep.push_back(e);
    auto tone = std::make_shared<ToneSource>();
    tone->wave = ToneSource::Sine;
    tone->frequency = float(kFreq);
    tone->levelDb = kLevel;
    const int sTone = rig.addSource(tone);
    const int sCable = rig.addSource(std::make_shared<DeviceCaptureSource>(cap));
    StripRuntime* voice = rig.addStrip(sTone);
    StripRuntime* appStrip = rig.addStrip(sCable);
    voice->chanCount = 1; // like a microphone
    for (auto& e : chain) rig.g->strips[0].chain.fx[rig.g->strips[0].chain.count++] = e.get();
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
    auto mem = std::make_shared<MemorySink>(size_t(kSr * (seconds + 2)));
    const int kMem = rig.addSink(mem);
    rig.addBus(kDev);
    rig.addBus(kMem);
    voice->sends[0].enabled = true;
    appStrip->sends[1].enabled = true;
    rig.go();
    const double tOff = seconds - 4.0, tOn = seconds - 2.0;
    std::printf("Echo plays a %.0f dBFS %.0f Hz tone through '%s' into '%s' and records '%s' for %.0f s; the voice is "
                "switched off at %.0f s and on again at %.0f s (not audible)...\n",
                kLevel, kFreq, presetName.c_str(), pair->renderName.c_str(), pair->captureName.c_str(), seconds, tOff, tOn);
    auto setOn = [&](bool on) {
        for (auto& e : chain)
            if (std::string(e->typeId()) != "bitcrusher") e->bypass = !on; // the optional stage stays as it is
    };
    std::this_thread::sleep_for(std::chrono::milliseconds(int(tOff * 1000)));
    setOn(false);
    std::this_thread::sleep_for(std::chrono::milliseconds(2000));
    setOn(true);
    std::this_thread::sleep_for(std::chrono::milliseconds(2000));
    rig.engine.stop();
    float cpu = 0.0f;
    for (auto& e : chain) cpu += e->cpuLoad.load();

    const auto& d = mem->leftData();
    const size_t n = std::min(mem->written(), d.size());
    auto at = [&](double t) { return std::min(n, size_t(t * kSr)); };
    auto rmsOf = [&](double a, double b) {
        double e = 0;
        for (size_t i = at(a); i < at(b); ++i) e += double(d[i]) * d[i];
        return std::sqrt(e / std::max<size_t>(1, at(b) - at(a)));
    };
    auto thdOf = [&](double a) {
        double e = 0;
        for (size_t i = at(a); i < at(a) + 9600 && i < n; ++i) e += double(d[i]) * d[i];
        const double fund = goertzel(d, at(a), 9600, kFreq, kSr), total = std::sqrt(2.0 * e / 9600);
        return std::sqrt(std::max(0.0, total * total - fund * fund)) / std::max(fund, 1e-9);
    };
    auto stepOf = [&](double a, double b) {
        float m = 0;
        for (size_t i = std::max<size_t>(at(a), 1); i < at(b); ++i) m = std::max(m, std::fabs(d[i] - d[i - 1]));
        return m;
    };
    float pk = 0;
    for (size_t i = at(0.5); i < n; ++i) pk = std::max(pk, std::fabs(d[i]));
    // Windows on the recording side's timeline (the cable adds some tens of ms).
    const double onRms = rmsOf(tOff - 2.5, tOff - 0.5), offRms = rmsOf(tOff + 0.5, tOn - 0.3);
    // Median of five 0.2 s windows per segment (the cable itself has an
    // occasional short step that would spoil a single window).
    struct Spread { double med, lo, hi; };
    auto thdSpread = [&](double a) {
        std::vector<double> v;
        for (int k = 0; k < 5; ++k) v.push_back(thdOf(a + 0.25 * k));
        std::sort(v.begin(), v.end());
        return Spread{v[2], v[0], v[4]};
    };
    const Spread on = thdSpread(tOff - 2.4), offS = thdSpread(tOff + 0.4), again = thdSpread(tOn + 0.4);
    const double onThd = on.med, offThd = offS.med, againThd = again.med;
    std::printf("  distortion per 0.2 s window: voice on %.1f..%.1f %%, off %.1f..%.1f %%, on again %.1f..%.1f %%\n",
                on.lo * 100, on.hi * 100, offS.lo * 100, offS.hi * 100, again.lo * 100, again.hi * 100);
    const float steadyStep = std::max(stepOf(tOff - 2.5, tOff - 0.5), stepOf(tOff + 0.5, tOn - 0.3));
    const float switchStep = std::max(stepOf(tOff - 0.3, tOff + 0.5), stepOf(tOn - 0.3, tOn + 0.5));
    char buf[900];
    std::snprintf(buf, sizeof(buf),
                  "'%s' received %.1f dBFS RMS (offline %.1f), distortion %.0f%% (offline %.0f%%; voice off %.1f%%, on "
                  "again %.0f%%), voice off %.1f dBFS RMS; peak %.2f dBFS; largest sample step %.3f steady, %.3f around "
                  "the switches; CPU %.2f%% of one core; Echo render underruns %llu, recording-side discontinuities %llu",
                  pair->captureName.c_str(), gainToDb(float(onRms)), gainToDb(float(expRms)), onThd * 100, expThd * 100,
                  offThd * 100, againThd * 100, gainToDb(float(offRms)), gainToDb(pk), steadyStep, switchStep, cpu * 100.0f,
                  (unsigned long long)ren->stats.underruns.load(), (unsigned long long)cap->stats.discontinuities.load());
    // Voice off is a sanity check that the distortion comes from the voice:
    // the plain tone through the cable path measures 1.5-6 % here, varying
    // from run to run (drift-correction resampling on both sides).
    const bool ok = n >= at(seconds - 0.5) && std::fabs(gainToDb(float(onRms)) - gainToDb(float(expRms))) < 1.5 &&
                    std::fabs(onThd - expThd) < 0.1 && std::fabs(againThd - expThd) < 0.1 && offThd < 0.1 &&
                    switchStep <= steadyStep * 1.25f && ren->stats.underruns.load() == 0 &&
                    cap->stats.discontinuities.load() == 0;
    result(what.c_str(), ok ? 1 : -1, buf);
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

// Downloads the ContentVec model the way Echo's button does (to `dest`) and
// reports progress, speed and the checksum result.
int contentVecDownload(const std::wstring& dest) {
    win::DownloadProgress prog;
    std::string err;
    bool ok = false;
    const double t0 = monotonicSeconds();
    std::thread t([&]() { ok = win::downloadFile(ai::kContentVecUrl, dest, ai::kContentVecBytes, ai::kContentVecSha256, prog, &err); });
    while (prog.done.load() < ai::kContentVecBytes && t.joinable()) {
        Sleep(1000);
        std::printf("  %.0f / %.0f MB\n", prog.done.load() / 1048576.0, ai::kContentVecBytes / 1048576.0);
        if (monotonicSeconds() - t0 > 900.0) prog.cancel = true;
        if (prog.done.load() >= ai::kContentVecBytes) break;
    }
    t.join();
    std::printf("%s in %.1f s%s%s\n", ok ? "downloaded and verified" : "failed", monotonicSeconds() - t0, ok ? "" : ": ", ok ? "" : err.c_str());
    return ok ? 0 : 1;
}

// Installs ONNX Runtime into a folder as Echo's AI setup button does and
// lists what it put there.
int ortInstall(const std::wstring& dir) {
    win::DownloadProgress prog;
    std::string err;
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    const double t0 = monotonicSeconds();
    if (!win::installOnnxRuntime(dir, prog, &err)) {
        std::printf("failed: %s\n", err.c_str());
        return 1;
    }
    std::printf("installed in %.1f s\n", monotonicSeconds() - t0);
    for (const auto& e : std::filesystem::directory_iterator(dir, ec))
        std::printf("  %s (%llu bytes)\n", wideToUtf8(e.path().filename().wstring()).c_str(),
                    static_cast<unsigned long long>(e.is_regular_file() ? e.file_size() : 0));
    return 0;
}

// Downloads, checks and unpacks VB-Audio's VB-CABLE pack as the app does,
// without opening the installer.
int vbCableDownload(const std::wstring& folder) {
    win::DownloadProgress prog;
    std::string err;
    std::wstring setup;
    const double t0 = monotonicSeconds();
    const bool ok = win::prepareVbCable(folder, prog, &setup, &err);
    const std::string detail = ok ? wideToUtf8(setup) : err;
    std::printf("%s in %.1f s: %s\n", ok ? "downloaded, verified and unpacked" : "failed", monotonicSeconds() - t0, detail.c_str());
    return ok ? 0 : 1;
}

// Converts a whole WAV in one piece (RVC's offline way) with Echo's RVC
// pipeline, for comparison with the real-time result. Output at 48 kHz.
int rvcOffline(const std::wstring& modelDir, const std::wstring& inWav, const std::wstring& outWav, float pitch, float index,
               unsigned seed) {
    WavData w;
    std::string err;
    if (!readWav(inWav, w, &err)) {
        std::printf("can't read the input\n");
        return 1;
    }
    std::vector<float> x(w.samples.size() / w.channels);
    for (size_t i = 0; i < x.size(); ++i) x[i] = w.samples[i * w.channels];
    ai::ModelManifest m;
    if (!ai::loadManifest(modelDir, m)) {
        std::printf("model: %s\n", m.error.c_str());
        return 1;
    }
    std::vector<float> xs = resampleOffline(x, 1, w.sampleRate, m.sampleRate);
    ai::RvcVoice voice;
    voice.seed(seed);
    if (!voice.load(m, false, static_cast<int>(xs.size()), &err)) {
        std::printf("load: %s\n", err.c_str());
        return 1;
    }
    voice.setIndexRate(index / 100.0f);
    std::vector<float> y;
    const double t0 = monotonicSeconds();
    if (!voice.run(xs.data(), static_cast<int>(xs.size()), pitch, y, &err)) {
        std::printf("run: %s\n", err.c_str());
        return 1;
    }
    std::printf("%.1f s in one piece: %.0f ms\n", xs.size() / static_cast<double>(m.sampleRate), (monotonicSeconds() - t0) * 1000.0);
    std::vector<float> out = resampleOffline(y, 1, m.sampleRate, 48000.0);
    WavWriter ww;
    if (!ww.open(outWav, 48000, 1, 16) || !ww.write(out.data(), out.size())) return 1;
    ww.close();
    return 0;
}

// Converts a WAV through the AI voice effect exactly as in real time
// (48 kHz, 10 ms blocks at real-time pace) and writes the result.
// response: 0 Quicker, 1 Balanced, 2 Smoother; gpu: DirectML if available.
int rvcConvert(const std::wstring& modelDir, const std::wstring& inWav, const std::wstring& outWav, float pitch, float index,
               int response, bool gpu) {
    WavData w;
    std::string err;
    if (!readWav(inWav, w, &err) || w.channels < 1) {
        std::printf("can't read %s: %s\n", wideToUtf8(inWav).c_str(), err.c_str());
        return 1;
    }
    std::vector<float> x(w.samples.size() / w.channels);
    for (size_t i = 0; i < x.size(); ++i) x[i] = w.samples[i * w.channels];
    if (w.sampleRate != 48000) x = resampleOffline(x, 1, w.sampleRate, 48000.0);
    ai::ModelManifest m;
    if (!ai::loadManifest(modelDir, m)) {
        std::printf("model: %s\n", m.error.c_str());
        return 1;
    }
    auto fx = EffectRegistry::instance().create("ai_voice");
    auto* ai = dynamic_cast<ai::AiVoiceEffect*>(fx.get());
    if (!ai) return 1;
    fx->prepare(48000.0, 480);
    fx->setParam("pitch", pitch);
    fx->setParam("index", index);
    fx->setParam("response", static_cast<float>(response));
    std::printf("response: %s\n", response == 0 ? "Quicker" : response == 2 ? "Smoother" : "Balanced");
    const double t0 = monotonicSeconds();
    ai->setModel(m, gpu);
    while (ai->status() == ai::AiStatus::Loading && monotonicSeconds() - t0 < 120.0) Sleep(20);
    std::printf("%s (%.1f s to load)\n", ai->statusText().c_str(), monotonicSeconds() - t0);
    if (ai->status() != ai::AiStatus::Running) return 1;
    std::vector<float> y(x.size() + 48000 * 3, 0.0f); // room for the effect's delay
    std::copy(x.begin(), x.end(), y.begin());
    const double t1 = monotonicSeconds();
    for (size_t pos = 0; pos + 480 <= y.size(); pos += 480) {
        float* ch[2] = {y.data() + pos, nullptr};
        fx->process(ch, 1, 480);
        // Real-time pace: one 10 ms block per 10 ms.
        const double due = t1 + (pos / 480 + 1) * 0.010;
        while (monotonicSeconds() < due) Sleep(1);
    }
    const int lat = fx->latencySamples();
    std::vector<float> out(y.begin() + lat, y.begin() + lat + static_cast<long>(x.size()));
    WavWriter ww;
    if (!ww.open(outWav, 48000, 1, 16) || !ww.write(out.data(), out.size())) {
        std::printf("can't write the output\n");
        return 1;
    }
    ww.close();
    std::printf("%.1f s converted; latency %.0f ms; inference %.1f ms per block (real-time factor %.2f); fallbacks to dry %.2f s, "
                "skipped %.2f s\n",
                x.size() / 48000.0, lat / 48.0, ai->inferenceMs.load(), ai->realtimeFactor.load(), ai->underrunSamples.load() / 48000.0,
                ai->skippedSamples.load() / 48000.0);
    return 0;
}

// Loads an RVC .index and times retrieval for a few queries (its own
// vectors must come back as themselves).
// Tries each kind of operator Echo's RVC graphs use, alone, on the graphics
// card (DirectML), and prints which ones it accepts.
int dmlProbe() {
    using ai::OnnxGraph;
    using ai::OnnxType;
    struct Case {
        const char* name;
        std::function<void(OnnxGraph&)> build; // input "x" [1,8,64] float -> output "y"
    };
    const std::vector<Case> cases = {
        {"Slice reversed", [](OnnxGraph& g) { g.output(g.op("Slice", {"x", g.ints({-1}), g.ints({-std::numeric_limits<int64_t>::max()}), g.ints({-1}), g.ints({-1})}), "y", OnnxType::Float, {1, 8, 64}); }},
        {"Slice", [](OnnxGraph& g) { g.output(g.op("Slice", {"x", g.ints({0, 63}), g.ints({7, 64}), g.ints({1, 2})}), "y", OnnxType::Float, {1, 7, 1}); }},
        {"Mod fmod", [](OnnxGraph& g) { g.output(g.op("Mod", {"x", g.scalar(1.0f)}, {ai::attrInt("fmod", 1)}), "y", OnnxType::Float, {1, 8, 64}); }},
        {"CumSum", [](OnnxGraph& g) { g.output(g.op("CumSum", {"x", g.constantInt64({}, {1})}), "y", OnnxType::Float, {1, 8, 64}); }},
        {"Pad", [](OnnxGraph& g) { g.output(g.op("Pad", {"x", g.ints({0, 1, 0, 0, 0, 0})}), "y", OnnxType::Float, {1, 9, 64}); }},
        {"Greater+Cast", [](OnnxGraph& g) { g.output(g.op("Cast", {g.op("Greater", {"x", g.scalar(0.0f)})}, {ai::attrInt("to", 1)}), "y", OnnxType::Float, {1, 8, 64}); }},
        {"Expand", [](OnnxGraph& g) { g.output(g.op("Expand", {g.op("Slice", {"x", g.ints({0}), g.ints({1}), g.ints({2})}), g.ints({1, 8, 64})}), "y", OnnxType::Float, {1, 8, 64}); }},
        {"ConvTranspose", [](OnnxGraph& g) {
             std::vector<float> w(8 * 4 * 20, 0.01f), b(4, 0.0f);
             g.output(g.op("ConvTranspose", {"x", g.constant({8, 4, 20}, w), g.constant({4}, b)},
                           {ai::attrInts("kernel_shape", {20}), ai::attrInts("strides", {10}), ai::attrInts("pads", {5, 5}),
                            ai::attrInts("output_padding", {0})}),
                      "y", OnnxType::Float, {1, 4, 640});
         }},
        {"ConvTranspose odd", [](OnnxGraph& g) {
             std::vector<float> w(8 * 4 * 3, 0.01f), b(4, 0.0f);
             g.output(g.op("ConvTranspose", {"x", g.constant({8, 4, 3}, w), g.constant({4}, b)},
                           {ai::attrInts("kernel_shape", {3}), ai::attrInts("strides", {3}), ai::attrInts("pads", {2, 2}),
                            ai::attrInts("output_padding", {1})}),
                      "y", OnnxType::Float, {1, 4, 189});
         }},
        {"ConvTranspose plain", [](OnnxGraph& g) {
             std::vector<float> w(8 * 4 * 20, 0.01f), b(4, 0.0f);
             g.output(g.op("ConvTranspose", {"x", g.constant({8, 4, 20}, w), g.constant({4}, b)},
                           {ai::attrInts("kernel_shape", {20}), ai::attrInts("strides", {10}), ai::attrInts("pads", {5, 5})}),
                      "y", OnnxType::Float, {1, 4, 640});
         }},
        {"ConvTranspose all attrs", [](OnnxGraph& g) {
             std::vector<float> w(8 * 4 * 20, 0.01f), b(4, 0.0f);
             g.output(g.op("ConvTranspose", {"x", g.constant({8, 4, 20}, w), g.constant({4}, b)},
                           {ai::attrInts("kernel_shape", {20}), ai::attrInts("strides", {10}), ai::attrInts("pads", {5, 5}),
                            ai::attrInts("dilations", {1}), ai::attrInt("group", 1), ai::attrInts("output_padding", {0})}),
                      "y", OnnxType::Float, {1, 4, 640});
         }},
        {"ConvTranspose 2-D", [](OnnxGraph& g) {
             std::vector<float> w(8 * 4 * 20, 0.01f), b(4, 0.0f);
             const std::string x4 = g.op("Unsqueeze", {"x", g.ints({2})});
             const std::string y4 = g.op("ConvTranspose", {x4, g.constant({8, 4, 1, 20}, w), g.constant({4}, b)},
                                         {ai::attrInts("kernel_shape", {1, 20}), ai::attrInts("strides", {1, 10}),
                                          ai::attrInts("pads", {0, 5, 0, 5}), ai::attrInts("output_padding", {0, 0})});
             g.output(g.op("Squeeze", {y4, g.ints({2})}), "y", OnnxType::Float, {1, 4, 640});
         }},
        {"ConvTranspose odd 2-D", [](OnnxGraph& g) {
             std::vector<float> w(8 * 4 * 3, 0.01f), b(4, 0.0f);
             const std::string x4 = g.op("Unsqueeze", {"x", g.ints({2})});
             const std::string y4 = g.op("ConvTranspose", {x4, g.constant({8, 4, 1, 3}, w), g.constant({4}, b)},
                                         {ai::attrInts("kernel_shape", {1, 3}), ai::attrInts("strides", {1, 3}),
                                          ai::attrInts("pads", {0, 2, 0, 2}), ai::attrInts("output_padding", {0, 1})});
             g.output(g.op("Squeeze", {y4, g.ints({2})}), "y", OnnxType::Float, {1, 4, 189});
         }},
        {"ConvTranspose no bias", [](OnnxGraph& g) {
             std::vector<float> w(8 * 4 * 20, 0.01f);
             g.output(g.op("ConvTranspose", {"x", g.constant({8, 4, 20}, w)},
                           {ai::attrInts("kernel_shape", {20}), ai::attrInts("strides", {10}), ai::attrInts("pads", {5, 5})}),
                      "y", OnnxType::Float, {1, 4, 640});
         }},
        {"Conv dilated", [](OnnxGraph& g) {
             std::vector<float> w(8 * 8 * 3, 0.01f), b(8, 0.0f);
             g.output(g.op("Conv", {"x", g.constant({8, 8, 3}, w), g.constant({8}, b)},
                           {ai::attrInts("kernel_shape", {3}), ai::attrInts("pads", {5, 5}), ai::attrInts("dilations", {5}),
                            ai::attrInts("strides", {1})}),
                      "y", OnnxType::Float, {1, 8, 64});
         }},
        {"Conv strided", [](OnnxGraph& g) {
             std::vector<float> w(8 * 8 * 16, 0.01f), b(8, 0.0f);
             g.output(g.op("Conv", {"x", g.constant({8, 8, 16}, w), g.constant({8}, b)},
                           {ai::attrInts("kernel_shape", {16}), ai::attrInts("pads", {4, 4}), ai::attrInts("dilations", {1}),
                            ai::attrInts("strides", {8})}),
                      "y", OnnxType::Float, {1, 8, 8});
         }},
        {"GatherElements", [](OnnxGraph& g) {
             std::vector<int64_t> idx(1 * 8 * 64);
             for (size_t i = 0; i < idx.size(); ++i) idx[i] = static_cast<int64_t>(i % 64);
             g.output(g.op("GatherElements", {"x", g.constantInt64({1, 8, 64}, idx)}, {ai::attrInt("axis", 2)}), "y", OnnxType::Float, {1, 8, 64});
         }},
        {"LayerNormalization", [](OnnxGraph& g) {
             g.output(g.op("LayerNormalization", {"x", g.constant({64}, std::vector<float>(64, 1.0f)), g.constant({64}, std::vector<float>(64, 0.0f))},
                           {ai::attrInt("axis", -1), ai::attrFloat("epsilon", 1e-5f)}),
                      "y", OnnxType::Float, {1, 8, 64});
         }},
        {"Split", [](OnnxGraph& g) { g.output(g.opN("Split", {"x", g.ints({4, 4})}, 2, {ai::attrInt("axis", 1)})[0], "y", OnnxType::Float, {1, 4, 64}); }},
        {"Gather", [](OnnxGraph& g) { g.output(g.op("Gather", {g.constant({4, 64}, std::vector<float>(256, 1.0f)), g.constantInt64({1, 8}, {0, 1, 2, 3, 0, 1, 2, 3})}, {ai::attrInt("axis", 0)}), "y", OnnxType::Float, {1, 8, 64}); }},
        {"Sin", [](OnnxGraph& g) { g.output(g.op("Sin", {"x"}), "y", OnnxType::Float, {1, 8, 64}); }},
        {"Erf", [](OnnxGraph& g) { g.output(g.op("Erf", {"x"}), "y", OnnxType::Float, {1, 8, 64}); }},
        {"InstanceNormalization", [](OnnxGraph& g) {
             g.output(g.op("InstanceNormalization", {"x", g.constant({8}, std::vector<float>(8, 1.0f)), g.constant({8}, std::vector<float>(8, 0.0f))},
                           {ai::attrFloat("epsilon", 1e-5f)}),
                      "y", OnnxType::Float, {1, 8, 64});
         }},
        {"Softmax", [](OnnxGraph& g) { g.output(g.op("Softmax", {"x"}, {ai::attrInt("axis", -1)}), "y", OnnxType::Float, {1, 8, 64}); }},
        {"Exp+Sigmoid+Tanh+Relu", [](OnnxGraph& g) { g.output(g.op("Relu", {g.op("Tanh", {g.op("Sigmoid", {g.op("Exp", {"x"})})})}), "y", OnnxType::Float, {1, 8, 64}); }},
    };
    std::vector<float> x(8 * 64);
    for (size_t i = 0; i < x.size(); ++i) x[i] = 0.01f * static_cast<float>(i % 37) - 0.15f;
    int failed = 0;
    for (const auto& c : cases) {
        OnnxGraph g;
        g.input("x", OnnxType::Float, {1, 8, 64});
        c.build(g);
        const std::vector<uint8_t> bytes = g.serialize();
        ai::OrtGraphSession s;
        std::string err;
        bool ok = s.create(bytes, true, 1, &err);
        std::vector<std::vector<float>> out;
        if (ok) ok = s.run({{"x", OnnxType::Float, {1, 8, 64}, x.data()}}, {"y"}, out, &err);
        std::printf("%-24s %s %s\n", c.name, ok ? "ok  " : "FAIL", ok ? s.provider().c_str() : err.substr(0, 160).c_str());
        failed += ok ? 0 : 1;
    }
    return failed == 0 ? 0 : 1;
}

int faissInfo(const std::wstring& path) {
    ai::FaissIndex idx;
    std::string err;
    const double t0 = monotonicSeconds();
    if (!idx.load(path, &err)) {
        std::printf("load failed: %s\n", err.c_str());
        return 1;
    }
    const double t1 = monotonicSeconds();
    std::printf("index: %d dims, %lld vectors, loaded in %.0f ms\n", idx.dim(), static_cast<long long>(idx.size()), (t1 - t0) * 1000.0);
    std::vector<float> q(static_cast<size_t>(idx.dim())), out(q.size());
    uint32_t seed = 1;
    auto rnd = [&]() {
        seed = seed * 1664525u + 1013904223u;
        return (static_cast<float>(seed >> 8) / 16777216.0f - 0.5f) * 0.4f;
    };
    const double t2 = monotonicSeconds();
    for (int i = 0; i < 50; ++i) {
        for (float& v : q) v = rnd();
        idx.retrieve(q.data(), 8, out.data());
    }
    const double t3 = monotonicSeconds();
    std::printf("retrieval: %.2f ms per frame\n", (t3 - t2) * 1000.0 / 50.0);
    return 0;
}

// Runs ContentVec (pytorch_model.bin) through Echo's ONNX graph on
// audio16k.bin in `dir` and writes echo_feats.bin (for comparison).
int contentVecRun(const std::wstring& bin, const std::wstring& dir, int layer) {
    std::vector<float> audio;
    {
        FILE* f = _wfopen((dir + L"\\audio16k.bin").c_str(), L"rb");
        if (!f) {
            std::printf("missing audio16k.bin\n");
            return 1;
        }
        _fseeki64(f, 0, SEEK_END);
        audio.resize(static_cast<size_t>(_ftelli64(f)) / 4);
        _fseeki64(f, 0, SEEK_SET);
        std::fread(audio.data(), 4, audio.size(), f);
        std::fclose(f);
    }
    ai::ContentVecModel cv;
    std::string err;
    const double t0 = monotonicSeconds();
    if (!cv.load(bin, &err)) {
        std::printf("load failed: %s\n", err.c_str());
        return 1;
    }
    std::vector<uint8_t> onnx;
    const int n = static_cast<int>(audio.size());
    if (!cv.buildGraph(n, layer, false, onnx, &err)) {
        std::printf("build failed: %s\n", err.c_str());
        return 1;
    }
    const double t1 = monotonicSeconds();
    ai::OrtGraphSession session;
    if (!session.create(onnx, false, 0, &err)) {
        std::printf("session failed: %s\n", err.c_str());
        return 1;
    }
    const double t2 = monotonicSeconds();
    std::vector<std::vector<float>> out;
    std::vector<ai::OrtGraphSession::Input> in = {{"audio", ai::OnnxType::Float, {1, n}, audio.data()}};
    if (!session.run(in, {"features"}, out, &err)) {
        std::printf("run failed: %s\n", err.c_str());
        return 1;
    }
    const double t3 = monotonicSeconds();
    for (int r = 0; r < 3; ++r) session.run(in, {"features"}, out, &err);
    const double t4 = monotonicSeconds();
    FILE* f = _wfopen((dir + L"\\echo_feats.bin").c_str(), L"wb");
    if (f) {
        std::fwrite(out[0].data(), 4, out[0].size(), f);
        std::fclose(f);
    }
    std::printf("%d samples -> %d frames: load+graph %.0f ms (%zu bytes), session %.0f ms, first run %.0f ms, then %.0f ms\n", n,
                ai::ContentVecModel::framesFor(n), (t1 - t0) * 1000.0, onnx.size(), (t2 - t1) * 1000.0, (t3 - t2) * 1000.0,
                (t4 - t3) * 1000.0 / 3.0);
    return 0;
}

// Runs an RVC voice (.pth) through Echo's ONNX graph on inputs saved by a
// test script (phone/pitch/pitchf/rnd/noise .bin in `dir`), writes
// echo_out.bin and the graph (echo_rvc.onnx) for comparison with PyTorch.
int rvcRun(const std::wstring& pth, const std::wstring& dir) {
    auto readFile = [](const std::wstring& path, std::vector<uint8_t>& out) {
        FILE* f = _wfopen(path.c_str(), L"rb");
        if (!f) return false;
        _fseeki64(f, 0, SEEK_END);
        out.resize(static_cast<size_t>(_ftelli64(f)));
        _fseeki64(f, 0, SEEK_SET);
        const bool ok = std::fread(out.data(), 1, out.size(), f) == out.size();
        std::fclose(f);
        return ok;
    };
    auto writeFile = [](const std::wstring& path, const void* data, size_t n) {
        FILE* f = _wfopen(path.c_str(), L"wb");
        if (!f) return false;
        const bool ok = std::fwrite(data, 1, n, f) == n;
        std::fclose(f);
        return ok;
    };
    ai::RvcModel model;
    std::string err;
    if (!model.load(pth, &err)) {
        std::printf("load failed: %s\n", err.c_str());
        return 1;
    }
    const ai::RvcInfo& I = model.info();
    std::printf("RVC %s, %d Hz, emb %d, hop %d, vocoder %s\n", I.version.c_str(), I.sampleRate, I.embDim, I.hop, I.vocoder.c_str());
    std::vector<uint8_t> phone, pitch, pitchf, rnd, noise;
    if (!readFile(dir + L"\\phone.bin", phone) || !readFile(dir + L"\\pitch.bin", pitch) || !readFile(dir + L"\\pitchf.bin", pitchf) ||
        !readFile(dir + L"\\rnd.bin", rnd) || !readFile(dir + L"\\noise.bin", noise)) {
        std::printf("missing input files\n");
        return 1;
    }
    const int T = static_cast<int>(pitchf.size() / 4);
    const double t0 = monotonicSeconds();
    std::vector<uint8_t> onnx;
    if (!model.buildGenerator(T, 0, onnx, &err)) {
        std::printf("build failed: %s\n", err.c_str());
        return 1;
    }
    const double t1 = monotonicSeconds();
    writeFile(dir + L"\\echo_rvc.onnx", onnx.data(), onnx.size());
    ai::OrtGraphSession session;
    if (!session.create(onnx, false, 0, &err)) {
        std::printf("session failed: %s\n", err.c_str());
        return 1;
    }
    const double t2 = monotonicSeconds();
    std::vector<ai::OrtGraphSession::Input> in = {
        {"phone", ai::OnnxType::Float, {1, T, I.embDim}, phone.data()},
        {"pitch", ai::OnnxType::Int64, {1, T}, pitch.data()},
        {"pitchf", ai::OnnxType::Float, {1, T}, pitchf.data()},
        {"rnd", ai::OnnxType::Float, {1, I.inter, T}, rnd.data()},
        {"noise", ai::OnnxType::Float, {1, static_cast<int64_t>(T) * I.hop}, noise.data()},
    };
    std::vector<std::vector<float>> out;
    if (!session.run(in, {"audio"}, out, &err)) {
        std::printf("run failed: %s\n", err.c_str());
        return 1;
    }
    double t3 = monotonicSeconds();
    for (int r = 0; r < 3; ++r) session.run(in, {"audio"}, out, &err);
    const double t4 = monotonicSeconds();
    writeFile(dir + L"\\echo_out.bin", out[0].data(), out[0].size() * 4);
    std::printf("%d frames: graph %.0f ms (%zu bytes), session %.0f ms, first run %.0f ms, then %.0f ms per run; %zu samples\n", T,
                (t1 - t0) * 1000.0, onnx.size(), (t2 - t1) * 1000.0, (t3 - t2) * 1000.0, (t4 - t3) * 1000.0 / 3.0, out[0].size());
    return 0;
}

// Lists a PyTorch checkpoint: its top-level keys and every tensor with its
// shape and sum (to compare Echo's reader with PyTorch).
int torchInfo(const std::wstring& path) {
    ai::TorchCheckpoint ck;
    std::string err;
    if (!ck.open(path, &err)) {
        std::printf("open failed: %s\n", err.c_str());
        return 1;
    }
    const ai::PyRef& root = ck.root();
    std::printf("root kind %d, %zu keys\n", static_cast<int>(root->kind), root->dict.size());
    for (const auto& [k, v] : root->dict) {
        std::string val;
        if (v->kind == ai::PyValue::Kind::Str) val = v->s;
        else if (v->isNumber()) val = std::to_string(v->number());
        else if (v->kind == ai::PyValue::Kind::List || v->kind == ai::PyValue::Kind::Tuple) val = std::to_string(v->items.size()) + " items";
        else if (v->kind == ai::PyValue::Kind::Dict) val = std::to_string(v->dict.size()) + " entries";
        std::printf("  %s: %s\n", k->s.c_str(), val.c_str());
    }
    const ai::PyRef weights = root->get("weight") ? root->get("weight") : root;
    const auto tensors = ai::TorchCheckpoint::tensors(weights);
    std::printf("%zu tensors\n", tensors.size());
    for (const auto& [name, t] : tensors) {
        std::vector<float> v;
        if (!ck.read(t, v, &err)) {
            std::printf("%s: %s\n", name.c_str(), err.c_str());
            return 1;
        }
        double sum = 0.0, abs = 0.0;
        for (float x : v) sum += x, abs += std::fabs(x);
        std::string shape;
        for (int64_t d : t.shape) shape += std::to_string(d) + " ";
        std::printf("%s [%s] %s sum %.6f abs %.6f\n", name.c_str(), shape.c_str(), t.dtype.c_str(), sum, abs);
    }
    return 0;
}

// Decodes a sound file the way the soundboard does (WAV, MP3, OGG...) and
// writes it as a 48 kHz 32-bit float WAV, for preparing the built-in sounds.
int decodeSound(const std::wstring& in, const std::wstring& out) {
    win::DecodedAudio a;
    std::string err;
    if (!win::decodeAudioFile(in, 0.0, a, &err)) {
        std::printf("decode failed: %s\n", err.c_str());
        return 1;
    }
    std::vector<float> x = a.sampleRate == 48000 ? std::move(a.samples) : resampleOffline(a.samples, a.channels, a.sampleRate, 48000);
    WavWriter w;
    if (!w.open(out, 48000, a.channels, 32) || !w.write(x.data(), x.size() / a.channels)) {
        std::printf("cannot write %s\n", wideToUtf8(out).c_str());
        return 1;
    }
    w.close();
    std::printf("%d Hz, %d ch, %.3f s\n", a.sampleRate, a.channels, double(x.size() / a.channels) / 48000.0);
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

    // Instant taps: press and release sent together (as some mouse buttons
    // and mouse software do). Key-state polling alone misses most of them;
    // the raw input events must catch every one.
    std::printf("\n== Hotkeys: instant taps (F22 pressed and released in the same instant, 20 times) ==\n");
    std::atomic<int> downs{0}, ups{0};
    HotkeyService taps;
    taps.start([](bool) {}, [&](int id, bool down) {
        if (id != 7) return;
        if (down) ++downs;
        else ++ups;
    });
    taps.setActions({HotkeyService::Action{7, KeyBinding{VK_F22, 0}}});
    Sleep(100);
    // The old way, for comparison: GetAsyncKeyState every 5 ms.
    std::atomic<bool> pollStop{false};
    std::atomic<int> pollSeen{0};
    std::thread poller([&] {
        bool was = false;
        while (!pollStop) {
            const bool d = (GetAsyncKeyState(VK_F22) & 0x8000) != 0;
            if (d && !was) ++pollSeen;
            was = d;
            Sleep(5);
        }
    });
    int sent = 0;
    for (int i = 0; i < 20; ++i) {
        INPUT in[2] = {};
        in[0].type = in[1].type = INPUT_KEYBOARD;
        in[0].ki.wVk = in[1].ki.wVk = VK_F22;
        in[1].ki.dwFlags = KEYEVENTF_KEYUP;
        if (SendInput(2, in, sizeof(INPUT)) == 2) ++sent;
        Sleep(40 + 3 * (i % 5));
    }
    Sleep(100);
    pollStop = true;
    poller.join();
    const bool raw = taps.rawInputActive();
    taps.stop();
    std::snprintf(buf, sizeof(buf), "%d of %d taps reported (press and release each); key-state polling alone saw %d; raw input %s",
                  downs.load(), sent, pollSeen.load(), raw ? "on" : "off");
    result("instant taps are never missed", sent > 0 && downs == sent && ups == sent ? 1 : sent == 0 ? 0 : -1,
           sent == 0 ? "SendInput was blocked, not tested" : buf);
}

// The soundboard in real time: Echo plays built-in sounds through the
// soundboard input into the virtual cable's playback side; the recording
// side (what Discord/OBS receive) is compared with the sound itself. Not
// audible (the cable is not your speakers); everything stays in memory.
void cmdSoundboard(DeviceManager& dm) {
    const char* what = "soundboard through the virtual microphone";
    const auto pairs = findCablePairs(dm.devices());
    const CablePair* pair = nullptr;
    for (const auto& p : pairs)
        if (!p.secondary) {
            pair = &p;
            break;
        }
    if (!pair) {
        result(what, 0, "no virtual cable installed");
        return;
    }
    std::string err;
    StreamOptions ro;
    ro.deviceId = fromUtf8(pair->renderId);
    ro.label = pair->renderName;
    auto ren = WasapiRender::open(ro, &err);
    StreamOptions co;
    co.deviceId = fromUtf8(pair->captureId);
    co.label = pair->captureName;
    auto cap = ren ? WasapiCapture::open(co, &err) : nullptr;
    if (!ren || !cap) {
        result(what, -1, err);
        return;
    }
    EngineRig rig(48000, 480);
    auto board = std::make_shared<SoundboardSource>();
    auto bank = std::make_shared<SoundBank>();
    for (const char* id : {"ding", "airhorn"}) {
        auto clip = std::make_shared<SoundClip>();
        clip->samples = loadBuiltinSound(id, 48000, &err);
        if (clip->samples.empty()) {
            result(what, -1, std::string(id) + ": " + err);
            return;
        }
        bank->pads.push_back({clip, 1.0f});
    }
    board->setBank(bank);
    rig.keep.push_back(bank);
    const int sBoard = rig.addSource(board);
    const int sCable = rig.addSource(std::make_shared<DeviceCaptureSource>(cap));
    StripRuntime* boardStrip = rig.addStrip(sBoard);
    StripRuntime* appStrip = rig.addStrip(sCable);
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
    auto direct = std::make_shared<MemorySink>(48000 * 12);
    auto received = std::make_shared<MemorySink>(48000 * 12);
    const int kDirect = rig.addSink(direct), kRecv = rig.addSink(received);
    rig.addBus(kDev);
    rig.addBus(kRecv);
    rig.addBus(kDirect);
    boardStrip->sends[0].enabled = true; // soundboard -> virtual mic -> CABLE Input
    boardStrip->sends[2].enabled = true; // and straight into memory (reference)
    appStrip->sends[1].enabled = true;   // "Discord" listening to CABLE Output
    rig.go();
    std::printf("Echo plays its 'Ding' and 'Air horn' sounds into '%s' and records '%s' (not audible on your speakers)...\n",
                pair->renderName.c_str(), pair->captureName.c_str());
    Sleep(4000); // the cable settles
    const uint64_t underrunsBefore = ren->stats.underruns.load(), discBefore = cap->stats.discontinuities.load();
    const size_t playAt1 = direct->written();
    const double t1 = monotonicSeconds();
    board->play(0);
    // Engine latency: from play() to the sound in the engine's output.
    double engineMs = -1.0;
    while (monotonicSeconds() - t1 < 1.0) {
        const auto& d = direct->leftData();
        const size_t n = direct->written();
        for (size_t i = playAt1; i < n && engineMs < 0; ++i)
            if (std::fabs(d[i]) > 1e-4f) engineMs = (monotonicSeconds() - t1) * 1000.0;
        if (engineMs >= 0) break;
        std::this_thread::yield();
    }
    Sleep(2600);
    board->play(1);
    Sleep(3500); // the air horn (3.3 s) and the cable's delay
    rig.engine.stop();

    const uint64_t underrunsDuring = ren->stats.underruns.load() - underrunsBefore;
    const uint64_t discDuring = cap->stats.discontinuities.load() - discBefore;

    // The recording side passes through the cable and drift-corrected
    // resampling, so compare what survives that exactly: the sounds' 10 ms
    // level envelope (aligned by cross-correlation), their level, and their
    // octave-band spectrum.
    const auto& ref = direct->leftData();
    const auto& rec = received->leftData();
    const size_t nRef = direct->written(), nRec = received->written();
    auto envelope = [](const std::vector<float>& x, size_t from, size_t to) {
        std::vector<double> e;
        for (size_t i = from; i + 480 <= to; i += 480) {
            double q = 0;
            for (size_t k = i; k < i + 480; ++k) q += double(x[k]) * x[k];
            e.push_back(std::max(-80.0, 10 * std::log10(q / 480 + 1e-12))); // silence is silence, however quiet
        }
        return e;
    };
    // Both sounds: the ding (2.5 s), a 0.1 s pause, the start of the air horn.
    const size_t from = playAt1, to = std::min(nRef, playAt1 + 48000 * 46 / 10);
    const std::vector<double> eRef = envelope(ref, from, to);
    double best = -2.0;
    int lagMs = 0;
    for (int l = 0; l < 500; ++l) { // up to 0.5 s of cable delay, in 1 ms steps
        const size_t f2 = from + static_cast<size_t>(l) * 48;
        if (f2 + (to - from) > nRec) break;
        const std::vector<double> eRec = envelope(rec, f2, f2 + (to - from));
        const size_t n = std::min(eRef.size(), eRec.size());
        double ma = 0, mb = 0;
        for (size_t i = 0; i < n; ++i) ma += eRef[i], mb += eRec[i];
        ma /= n, mb /= n;
        double sab = 0, saa = 0, sbb = 0;
        for (size_t i = 0; i < n; ++i) sab += (eRef[i] - ma) * (eRec[i] - mb), saa += (eRef[i] - ma) * (eRef[i] - ma), sbb += (eRec[i] - mb) * (eRec[i] - mb);
        const double c = sab / std::sqrt(saa * sbb + 1e-12);
        if (c > best) best = c, lagMs = l;
    }
    const size_t off = static_cast<size_t>(lagMs) * 48;
    if (std::getenv("ECHO_HWTEST_DEBUG")) {
        const std::vector<double> eRec = envelope(rec, from + off, from + off + (to - from));
        for (size_t i = 0; i < eRef.size(); i += 10) std::printf("  %4.1fs ref %6.1f rec %6.1f\n", i * 0.01, eRef[i], i < eRec.size() ? eRec[i] : -999.0);
    }
    double eA = 0, eB = 0;
    for (size_t i = from; i < to && i + off < nRec; ++i) eA += double(ref[i]) * ref[i], eB += double(rec[i + off]) * rec[i + off];
    const double levelDiff = 10 * std::log10((eB + 1e-20) / (eA + 1e-20));
    // Octave bands 125 Hz - 8 kHz (Goertzel sums over each band's bins would
    // be slow; a 2nd-order band-pass per band is enough here).
    double worstBand = 0.0;
    for (double fc : {125.0, 250.0, 500.0, 1000.0, 2000.0, 4000.0, 8000.0}) {
        auto bandEnergy = [&](const std::vector<float>& x, size_t a0, size_t a1) {
            const double w0 = 2 * kPiD * fc / 48000.0, alpha = std::sin(w0) / (2 * 1.41);
            const double b0 = alpha, b2 = -alpha, a0c = 1 + alpha, a1c = -2 * std::cos(w0), a2c = 1 - alpha;
            double x1 = 0, x2 = 0, y1 = 0, y2 = 0, e = 0;
            for (size_t i = a0; i < a1; ++i) {
                const double y = (b0 * x[i] + b2 * x2 - a1c * y1 - a2c * y2) / a0c;
                x2 = x1, x1 = x[i], y2 = y1, y1 = y;
                e += y * y;
            }
            return e;
        };
        const double ba = bandEnergy(ref, from, to), bb = bandEnergy(rec, from + off, std::min(nRec, to + off));
        if (ba > eA * 1e-4) worstBand = std::max(worstBand, std::fabs(10 * std::log10((bb + 1e-20) / (ba + 1e-20))));
    }
    char buf[600];
    std::snprintf(buf, sizeof(buf),
                  "'%s' received both sounds: level %+.2f dB vs what Echo played, envelope correlation %.3f, octave bands "
                  "within %.2f dB, cable delay %d ms; play() to sound in Echo's output %.1f ms; during the sounds: render "
                  "underruns %llu, recording-side discontinuities %llu (start-up: %llu underruns)",
                  pair->captureName.c_str(), levelDiff, best, worstBand, lagMs, engineMs,
                  (unsigned long long)underrunsDuring, (unsigned long long)discDuring, (unsigned long long)underrunsBefore);
    const bool ok = best > 0.98 && std::fabs(levelDiff) < 0.5 && worstBand < 1.0 && engineMs >= 0 && engineMs < 40.0 &&
                    underrunsDuring == 0 && discDuring == 0;
    result(what, ok ? 1 : -1, buf);
}

int wmain(int argc, wchar_t** argv) {
    SetConsoleOutputCP(CP_UTF8);
    if (argc > 2 && std::wstring(argv[1]) == L"export-presets") return exportPresets(argv[2]);
    ComScope com(COINIT_MULTITHREADED);
    if (argc > 3 && std::wstring(argv[1]) == L"decode") return decodeSound(argv[2], argv[3]);
    if (argc > 2 && std::wstring(argv[1]) == L"torch-info") return torchInfo(argv[2]);
    if (argc > 3 && std::wstring(argv[1]) == L"rvc-run") return rvcRun(argv[2], argv[3]);
    if (argc > 2 && std::wstring(argv[1]) == L"faiss-info") return faissInfo(argv[2]);
    if (argc > 2 && std::wstring(argv[1]) == L"contentvec-download") return contentVecDownload(argv[2]);
    if (argc > 2 && std::wstring(argv[1]) == L"vbcable-download") return vbCableDownload(argv[2]);
    if (argc > 2 && std::wstring(argv[1]) == L"ort-install") return ortInstall(argv[2]);
    if (argc > 1 && std::wstring(argv[1]) == L"dml-probe") return dmlProbe();
    if (argc > 4 && std::wstring(argv[1]) == L"rvc-offline")
        return rvcOffline(argv[2], argv[3], argv[4], argc > 5 ? static_cast<float>(_wtof(argv[5])) : 0.0f,
                          argc > 6 ? static_cast<float>(_wtof(argv[6])) : 50.0f, argc > 7 ? static_cast<unsigned>(_wtoi(argv[7])) : 1u);
    if (argc > 4 && std::wstring(argv[1]) == L"rvc-convert")
        return rvcConvert(argv[2], argv[3], argv[4], argc > 5 ? static_cast<float>(_wtof(argv[5])) : 0.0f,
                          argc > 6 ? static_cast<float>(_wtof(argv[6])) : 50.0f, argc > 7 ? _wtoi(argv[7]) : 1,
                          argc > 8 && _wtoi(argv[8]) != 0);
    if (argc > 3 && std::wstring(argv[1]) == L"contentvec-run") return contentVecRun(argv[2], argv[3], argc > 4 ? _wtoi(argv[4]) : 12);
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
    if (cmd == L"voice")
        cmdVoice(dm, argc > 2 ? wideToUtf8(argv[2]) : std::string("Pain"), std::max(12.0, argc > 3 ? _wtof(argv[3]) : 12.0));
    if (cmd == L"hotkeys") cmdHotkeys();
    if (cmd == L"soundboard") cmdSoundboard(dm);
    std::printf("\nSummary: %d passed, %d failed, %d skipped\n", g_pass, g_fail, g_skip);
    return g_fail ? 1 : 0;
}
