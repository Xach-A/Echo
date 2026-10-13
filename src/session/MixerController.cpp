#include "session/MixerController.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <set>

#include "core/Log.h"
#include "core/Text.h"
#include "dsp/EffectRegistry.h"
#include "dsp/NoiseSuppressorInfo.h"

namespace echo {

namespace {

ChainSnap makeChainSnap(const std::vector<std::shared_ptr<Effect>>& chain) {
    ChainSnap c;
    for (const auto& e : chain) {
        if (c.count >= kMaxChainLength) break;
        c.fx[c.count++] = e.get();
    }
    return c;
}

double steadySeconds() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

bool endpointDead(EndpointStatus s) {
    return s == EndpointStatus::Lost || s == EndpointStatus::Error || s == EndpointStatus::Stopped;
}

} // namespace

MixerController::MixerController(DeviceBackend& backend)
    : backend_(backend), tone_(std::make_shared<ToneSource>()), soundboard_(std::make_shared<SoundboardSource>()) {
    tone_->enabled = false;
    tone_->key = "tone";
}

MixerController::~MixerController() {
    recorder_.stop();
    engine_.stop();
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

void MixerController::prepareStrip(StripModel& s) {
    s.rt->prepare(engine_.sampleRate(), engine_.blockSize());
    for (auto& e : s.chain) e->prepare(engine_.sampleRate(), engine_.blockSize());
}

void MixerController::prepareBus(BusModel& b) {
    b.rt->prepare(engine_.sampleRate(), engine_.blockSize());
    for (auto& e : b.chain) e->prepare(engine_.sampleRate(), engine_.blockSize());
}

void MixerController::prepareAll() {
    engine_.setFormat(session_.engine.sampleRate, session_.engine.blockSize);
    tone_->prepare(engine_.sampleRate(), engine_.blockSize(), engine_.blockSeconds());
    soundboard_->prepare(engine_.sampleRate(), engine_.blockSize(), engine_.blockSeconds());
    for (auto& s : session_.strips) prepareStrip(s);
    for (auto& b : session_.buses) prepareBus(b);
}

bool MixerController::start(bool threaded) {
    if (engine_.running()) return true;
    prepareAll();
    sources_.clear();
    sinks_.clear();
    started_ = true;
    rebuildGraph();
    if (threaded) engine_.start();
    started_ = true;
    ECHO_LOG_INFO("Engine started: %d Hz, %d-frame blocks (%.2f ms)", engine_.sampleRate(), engine_.blockSize(),
                  engine_.blockSeconds() * 1000.0);
    return true;
}

void MixerController::stop() {
    recorder_.stop();
    engine_.stop();
    engine_.publish(nullptr);
    engine_.collectGarbage();
    for (auto& [k, e] : sources_)
        if (e.pending) retireEndpoint(std::move(e.endpoint));
    for (auto& [k, e] : sinks_)
        if (e.pending) retireEndpoint(std::move(e.endpoint));
    sources_.clear();
    sinks_.clear();
    started_ = false;
}

void MixerController::restart() {
    const bool wasRunning = engine_.running() || started_;
    stop();
    if (wasRunning) start();
    else prepareAll();
}

void MixerController::replaceSession(Session s) {
    const bool wasRunning = engine_.running() || started_;
    stop();
    session_ = std::move(s);
    migrateSession();
    assignCablesToEmptyVirtualBuses();
    if (wasRunning) start();
    else prepareAll();
    dirty_ = true;
}

void MixerController::update(double now) {
    now_ = now;
    bool rebuild = false;
    if (backend_.pollDeviceChanges()) {
        rebuild = true;
        followDefaultDevices();
        assignCablesToEmptyVirtualBuses();
    }
    // Every frame (cheap): streams that finished opening join the graph at once.
    if (finishPendingOpens()) rebuild = true;

    // Health check twice a second: dead streams, retries, vanished apps.
    if (now - lastHealthCheck_ >= 0.5) {
        lastHealthCheck_ = now;
        logGlitches();
        for (auto& [key, e] : sources_) {
            if (e.endpoint && endpointDead(e.endpoint->status())) {
                ECHO_LOG_WARN("Input '%s' stopped (%s); will reconnect", e.ref.deviceName.c_str(),
                              endpointStatusName(e.endpoint->status()));
                e.error = std::string("Disconnected - reconnecting");
                e.endpoint.reset();
                e.runtime.reset();
                e.nextRetry = now + 1.0;
                rebuild = true;
            } else if (!e.runtime && !e.pending && e.used && now >= e.nextRetry) {
                rebuild = true;
            } else if (e.ref.kind == SourceKind::ProcessLoopback && e.ref.processId &&
                       !backend_.processAlive(e.ref.processId)) {
                e.endpoint.reset();
                e.runtime.reset();
                e.error = "Application closed - waiting for it to start";
                e.nextRetry = now + 2.0;
                rebuild = true;
            }
        }
        for (auto& [key, e] : sinks_) {
            if (e.endpoint && endpointDead(e.endpoint->status())) {
                ECHO_LOG_WARN("Output '%s' stopped (%s); will reconnect", e.ref.deviceName.c_str(),
                              endpointStatusName(e.endpoint->status()));
                e.error = "Disconnected - reconnecting";
                e.endpoint.reset();
                e.runtime.reset();
                e.nextRetry = now + 1.0;
                rebuild = true;
            } else if (!e.runtime && !e.pending && e.used && now >= e.nextRetry) {
                rebuild = true;
            }
        }
    }
    if (rebuild && started_) rebuildGraph();
    engine_.collectGarbage();
}

// ---------------------------------------------------------------------------
// Source / sink management
// ---------------------------------------------------------------------------

MixerController::SourceEntry* MixerController::ensureSource(SourceRef& ref) {
    if (ref.kind == SourceKind::None) return nullptr;
    SourceEntry& e = sources_[ref.key()];
    e.used = true;
    e.ref = ref;
    if (ref.kind == SourceKind::ToneGenerator) {
        e.runtime = tone_;
        e.error.clear();
        return &e;
    }
    if (ref.kind == SourceKind::Soundboard) {
        e.runtime = soundboard_;
        e.error.clear();
        return &e;
    }
    if (e.pending) return &e;
    if (e.runtime && e.endpoint && !endpointDead(e.endpoint->status())) return &e;
    if (now_ < e.nextRetry) return &e;

    CaptureRequest req;
    req.kind = ref.kind;
    req.exclusive = ref.exclusive;
    req.lowLatency = session_.engine.lowLatencyShared;
    req.preferredRate = engine_.sampleRate();
    req.label = ref.deviceName.empty() ? sourceKindName(ref.kind) : ref.deviceName;
    switch (ref.kind) {
    case SourceKind::CaptureDevice:
    case SourceKind::RenderLoopback: {
        const bool render = ref.kind == SourceKind::RenderLoopback;
        std::string id = ref.deviceId.empty() ? backend_.defaultDevice(render)
                                              : backend_.resolveDevice(ref.deviceId, ref.deviceName, render);
        if (id.empty()) {
            e.error = ref.deviceId.empty() ? "No default device" : "Device not connected";
            e.nextRetry = now_ + 2.0;
            return &e;
        }
        req.deviceId = id;
        e.resolvedId = id;
        for (const auto& d : backend_.devices())
            if (d.id == id) req.label = d.name + (render ? " (loopback)" : "");
        break;
    }
    case SourceKind::ProcessLoopback: {
        if (!backend_.processLoopbackSupported()) {
            e.error = "Per-app capture needs Windows 10 version 2004 or newer";
            e.nextRetry = now_ + 3600.0;
            return &e;
        }
        uint32_t pid = ref.processId && backend_.processAlive(ref.processId) ? ref.processId : 0;
        if (!pid && !ref.processName.empty()) {
            for (const auto& p : backend_.audioProcesses())
                if (_stricmp(p.exeName.c_str(), ref.processName.c_str()) == 0) {
                    pid = p.pid;
                    break;
                }
        }
        if (!pid) {
            e.error = "Waiting for " + ref.processName;
            e.nextRetry = now_ + 2.0;
            return &e;
        }
        req.processId = pid;
        req.label = ref.processName;
        break;
    }
    case SourceKind::SystemExcludingEcho:
        if (!backend_.processLoopbackSupported()) {
            e.error = "Needs Windows 10 version 2004 or newer";
            e.nextRetry = now_ + 3600.0;
            return &e;
        }
        req.label = "System audio (excluding Echo)";
        break;
    default: break;
    }
    std::string err;
    auto ep = backend_.openCapture(req, &err);
    if (!ep) {
        e.error = err.empty() ? "Cannot open device" : err;
        e.nextRetry = now_ + 3.0;
        ECHO_LOG_WARN("Cannot open input '%s': %s", req.label.c_str(), e.error.c_str());
        return &e;
    }
    e.endpoint = ep;
    if (ref.kind == SourceKind::ProcessLoopback) e.ref.processId = req.processId;
    if (ep->status() == EndpointStatus::Starting) {
        e.pending = true;
        e.openedAt = steadySeconds();
        e.error = "Starting...";
        return &e;
    }
    adoptCapture(e);
    return &e;
}

void MixerController::adoptCapture(SourceEntry& e) {
    if (e.endpoint->status() != EndpointStatus::Running) {
        e.error = e.endpoint->lastError();
        if (e.error.empty()) e.error = "Cannot open device";
        ECHO_LOG_WARN("Cannot open input '%s': %s", e.endpoint->describe().c_str(), e.error.c_str());
        e.endpoint.reset();
        e.nextRetry = now_ + 3.0;
        return;
    }
    auto rt = std::make_shared<DeviceCaptureSource>(e.endpoint, session_.engine.resamplerQuality);
    rt->key = e.ref.key();
    rt->prepare(engine_.sampleRate(), engine_.blockSize(), engine_.blockSeconds());
    e.runtime = rt;
    e.error.clear();
}

MixerController::SinkEntry* MixerController::ensureSink(const OutputRef& ref) {
    SinkEntry& e = sinks_[ref.key()];
    e.used = true;
    e.ref = ref;
    if (e.pending) return &e;
    if (e.runtime && e.endpoint && !endpointDead(e.endpoint->status())) return &e;
    if (now_ < e.nextRetry) return &e;
    const std::string id = ref.deviceId.empty() ? backend_.defaultDevice(true)
                                                : backend_.resolveDevice(ref.deviceId, ref.deviceName, true);
    if (id.empty()) {
        e.error = ref.deviceId.empty() ? "No default output device" : "Device not connected";
        e.nextRetry = now_ + 2.0;
        return &e;
    }
    RenderRequest req;
    req.deviceId = id;
    req.exclusive = ref.exclusive;
    req.lowLatency = session_.engine.lowLatencyShared;
    req.preferredRate = engine_.sampleRate();
    req.label = ref.deviceName;
    for (const auto& d : backend_.devices())
        if (d.id == id) req.label = d.name;
    std::string err;
    auto ep = backend_.openRender(req, &err);
    if (!ep) {
        e.error = err.empty() ? "Cannot open device" : err;
        e.nextRetry = now_ + 3.0;
        ECHO_LOG_WARN("Cannot open output '%s': %s", req.label.c_str(), e.error.c_str());
        return &e;
    }
    e.resolvedId = id;
    e.endpoint = ep;
    if (ep->status() == EndpointStatus::Starting) {
        e.pending = true;
        e.openedAt = steadySeconds();
        e.error = "Starting...";
        return &e;
    }
    adoptRender(e);
    return &e;
}

void MixerController::adoptRender(SinkEntry& e) {
    if (e.endpoint->status() != EndpointStatus::Running) {
        e.error = e.endpoint->lastError();
        if (e.error.empty()) e.error = "Cannot open device";
        ECHO_LOG_WARN("Cannot open output '%s': %s", e.endpoint->describe().c_str(), e.error.c_str());
        e.endpoint.reset();
        e.nextRetry = now_ + 3.0;
        return;
    }
    auto rt = std::make_shared<DeviceRenderSink>(e.endpoint, session_.engine.resamplerQuality);
    rt->key = e.ref.key();
    rt->prepare(engine_.sampleRate(), engine_.blockSize(), engine_.blockSeconds());
    e.runtime = rt;
    e.error.clear();
}

bool MixerController::finishPendingOpens() {
    const double t = steadySeconds();
    bool changed = false;
    auto settle = [&](auto& e, auto adopt) {
        if (!e.pending) return;
        if (e.endpoint && e.endpoint->status() == EndpointStatus::Starting) {
            if (t - e.openedAt < openTimeout_) return;
            ECHO_LOG_WARN("Opening '%s' timed out", e.endpoint->describe().c_str());
            retireEndpoint(std::move(e.endpoint));
            e.error = "The device did not respond (timed out)";
            e.nextRetry = now_ + 5.0;
        } else if (e.endpoint) {
            adopt(e);
        }
        e.pending = false;
        changed = true;
    };
    for (auto& [key, e] : sources_) settle(e, [this](SourceEntry& s) { adoptCapture(s); });
    for (auto& [key, e] : sinks_) settle(e, [this](SinkEntry& s) { adoptRender(s); });
    auto reap = [](auto& v) {
        v.erase(std::remove_if(v.begin(), v.end(),
                               [](const auto& ep) { return ep->status() != EndpointStatus::Starting; }),
                v.end());
    };
    reap(retiredCaptures_);
    reap(retiredRenders_);
    return changed;
}

void MixerController::logGlitches() {
    auto report = [](uint64_t* seen, const uint64_t* now, const char* const* names, int count, const std::string& what,
                     float marginMs) {
        std::string msg;
        for (int i = 0; i < count; ++i) {
            if (now[i] > seen[i]) {
                if (!msg.empty()) msg += ", ";
                msg += names[i] + std::string(" +") + std::to_string(now[i] - seen[i]);
            }
            seen[i] = now[i];
        }
        if (!msg.empty())
            ECHO_LOG_WARN("Audio glitch: %s: %s (extra margin now %.0f ms)", what.c_str(), msg.c_str(), marginMs);
    };
    static const char* const kIn[] = {"engine underrun", "backlog drop", "gap while playing", "device overflow",
                                      "Windows discontinuity"};
    for (auto& [key, e] : sources_) {
        if (!e.runtime || !e.endpoint) continue;
        const SourceRuntime& r = *e.runtime;
        const uint64_t now[5] = {r.underruns.load(), r.overruns.load(), r.gaps.load(), e.endpoint->stats.overruns.load(),
                                 e.endpoint->stats.discontinuities.load()};
        report(e.seen, now, kIn, 5, "input '" + e.endpoint->describe() + "'", r.marginMs.load());
    }
    static const char* const kOut[] = {"ran dry", "overflow", "device underrun", "Windows discontinuity"};
    for (auto& [key, e] : sinks_) {
        if (!e.runtime || !e.endpoint) continue;
        const SinkRuntime& k = *e.runtime;
        const uint64_t now[4] = {k.underruns.load(), k.overruns.load(), e.endpoint->stats.underruns.load(),
                                 e.endpoint->stats.discontinuities.load()};
        report(e.seen, now, kOut, 4, "output '" + e.endpoint->describe() + "'", k.marginMs.load());
    }
    static const char* const kEng[] = {"late blocks (timer clock)", "clock device stalled", "exceptions"};
    const EngineStats& es = engine_.stats();
    const uint64_t now[3] = {es.lateBlocks.load(), es.deviceStalls.load(), es.exceptions.load()};
    report(seenEngine_, now, kEng, 3, "engine", 0.0f);
}

bool MixerController::followDefaultDevices() {
    bool moved = false;
    auto release = [&](auto& e, const char* what) {
        ECHO_LOG_INFO("Windows default %s changed; switching '%s'", what, e.ref.deviceName.c_str());
        if (e.pending) retireEndpoint(std::move(e.endpoint));
        e.pending = false;
        e.endpoint.reset();
        e.runtime.reset();
        e.resolvedId.clear();
        e.nextRetry = 0.0;
        e.error = "Switching to the new default device";
        moved = true;
    };
    for (auto& [key, e] : sources_) {
        if (!e.ref.deviceId.empty() || e.resolvedId.empty()) continue;
        if (e.ref.kind != SourceKind::CaptureDevice && e.ref.kind != SourceKind::RenderLoopback) continue;
        const std::string def = backend_.defaultDevice(e.ref.kind == SourceKind::RenderLoopback);
        if (!def.empty() && def != e.resolvedId) release(e, "recording device");
    }
    for (auto& [key, e] : sinks_) {
        if (!e.ref.deviceId.empty() || e.resolvedId.empty()) continue;
        const std::string def = backend_.defaultDevice(true);
        if (!def.empty() && def != e.resolvedId) release(e, "playback device");
    }
    return moved;
}

void MixerController::retireEndpoint(std::shared_ptr<CaptureEndpoint> ep) {
    if (ep && ep->status() == EndpointStatus::Starting) retiredCaptures_.push_back(std::move(ep));
}

void MixerController::retireEndpoint(std::shared_ptr<RenderEndpoint> ep) {
    if (ep && ep->status() == EndpointStatus::Starting) retiredRenders_.push_back(std::move(ep));
}

// ---------------------------------------------------------------------------
// Graph construction
// ---------------------------------------------------------------------------

void MixerController::rebuildGraph() {
    for (auto& [k, e] : sources_) e.used = false;
    for (auto& [k, e] : sinks_) e.used = false;
    stripSourceKey_.clear();
    busSinkKey_.clear();

    auto g = std::make_unique<GraphSnapshot>();
    std::map<SourceRuntime*, int> sourceIndex;
    std::map<SinkRuntime*, int> sinkIndex;

    for (auto& s : session_.strips) {
        if (g->numStrips >= kMaxStrips) break;
        int srcIdx = -1;
        SourceEntry* e = ensureSource(s.source);
        if (e) stripSourceKey_[s.id] = s.source.key();
        if (e && e->runtime) {
            auto it = sourceIndex.find(e->runtime.get());
            if (it == sourceIndex.end() && g->numSources < kMaxSources) {
                srcIdx = g->numSources;
                g->sources[g->numSources++] = e->runtime.get();
                g->keepAlive.push_back(e->runtime);
                sourceIndex[e->runtime.get()] = srcIdx;
            } else if (it != sourceIndex.end()) {
                srcIdx = it->second;
            }
            s.online = true;
            s.status = "OK";
        } else {
            s.online = false;
            s.status = s.source.kind == SourceKind::None ? "No input selected" : (e ? e->error : "Unavailable");
        }
        // ProcessLoopback may have resolved a new PID: remember it.
        if (e && s.source.kind == SourceKind::ProcessLoopback) s.source.processId = e->ref.processId;
        // Push to talk and the mute hotkey act on microphones only.
        s.rt->talkGated = isMicrophoneSource(s.source);
        // Reference (e.g. playback loopback for echo cancellation): only
        // opened when an effect on this strip uses it.
        int refIdx = -1;
        bool wantsRef = false;
        for (const auto& fx : s.chain) wantsRef |= fx->usesReference();
        if (wantsRef && s.reference.kind != SourceKind::None) {
            SourceEntry* re = ensureSource(s.reference);
            if (re && re->runtime) {
                auto it = sourceIndex.find(re->runtime.get());
                if (it == sourceIndex.end() && g->numSources < kMaxSources) {
                    refIdx = g->numSources;
                    g->sources[g->numSources++] = re->runtime.get();
                    g->keepAlive.push_back(re->runtime);
                    sourceIndex[re->runtime.get()] = refIdx;
                } else if (it != sourceIndex.end()) {
                    refIdx = it->second;
                }
            }
        }
        StripSnap snap;
        snap.rt = s.rt.get();
        snap.source = srcIdx;
        snap.reference = refIdx;
        snap.chain = makeChainSnap(s.chain);
        g->strips[g->numStrips++] = snap;
        g->keepAlive.push_back(s.rt);
        for (auto& fx : s.chain) g->keepAlive.push_back(fx);
    }

    for (auto& b : session_.buses) {
        if (g->numBuses >= kMaxBuses) break;
        int sinkIdx = -1;
        b.isClock = false;
        const bool hasOutput = !b.output.deviceId.empty() || b.kind == BusKind::Physical;
        SinkEntry* e = hasOutput ? ensureSink(b.output) : nullptr;
        if (e) busSinkKey_[b.id] = b.output.key();
        if (e && e->runtime) {
            auto it = sinkIndex.find(e->runtime.get());
            if (it == sinkIndex.end() && g->numSinks < kMaxSinks) {
                sinkIdx = g->numSinks;
                g->sinks[g->numSinks++] = e->runtime.get();
                g->keepAlive.push_back(e->runtime);
                sinkIndex[e->runtime.get()] = sinkIdx;
            } else if (it != sinkIndex.end()) {
                sinkIdx = it->second;
            }
            b.online = true;
            b.status = "OK";
        } else {
            b.online = false;
            b.status = !hasOutput ? "No output device assigned" : (e ? e->error : "Unavailable");
        }
        BusSnap snap;
        snap.rt = b.rt.get();
        snap.slot = b.slot;
        snap.sink = sinkIdx;
        snap.chain = makeChainSnap(b.chain);
        g->buses[g->numBuses++] = snap;
        g->keepAlive.push_back(b.rt);
        for (auto& fx : b.chain) g->keepAlive.push_back(fx);
    }

    // Engine clock: the configured clock bus, else the first online physical
    // bus, else any online bus. Without one the engine uses a timer.
    BusModel* clock = nullptr;
    if (session_.engine.clockBusId) {
        BusModel* b = session_.bus(session_.engine.clockBusId);
        if (b && b->online) clock = b;
    }
    if (!clock)
        for (auto& b : session_.buses)
            if (b.online && b.kind == BusKind::Physical) {
                clock = &b;
                break;
            }
    if (!clock)
        for (auto& b : session_.buses)
            if (b.online) {
                clock = &b;
                break;
            }
    DeviceRenderSink* masterSink = nullptr;
    if (clock) {
        clock->isClock = true;
        auto it = sinks_.find(clock->output.key());
        if (it != sinks_.end() && it->second.runtime) masterSink = it->second.runtime.get();
    }
    for (auto& [k, e] : sinks_)
        if (e.runtime) e.runtime->setMaster(e.runtime.get() == masterSink, engine_.wakeTarget());
    if (masterSink) {
        auto it = sinkIndex.find(masterSink);
        g->masterSink = it != sinkIndex.end() ? it->second : -1;
    }

    // The soundboard's sounds: the snapshot keeps them alive while the
    // engine can still be playing them.
    if (soundboard_->bank()) g->keepAlive.push_back(std::const_pointer_cast<SoundBank>(soundboard_->bank()));

    // Recording taps.
    for (const auto& t : recorder_.taps()) {
        if (g->numTaps >= kMaxTaps) break;
        TapSnap ts;
        ts.tap = t.tap.get();
        ts.kind = t.target.kind;
        ts.index = -1;
        if (t.target.kind == RecordTap::Bus) {
            for (int i = 0; i < static_cast<int>(session_.buses.size()); ++i)
                if (session_.buses[i].id == t.target.objectId) ts.index = i;
        } else {
            for (int i = 0; i < static_cast<int>(session_.strips.size()); ++i)
                if (session_.strips[i].id == t.target.objectId) ts.index = i;
        }
        if (ts.index < 0) continue;
        g->taps[g->numTaps++] = ts;
        g->keepAlive.push_back(t.tap);
    }

    applySnapshot(std::move(g));

    // Drop entries nobody references; the retired snapshot keeps their
    // runtimes alive until the engine has moved on.
    for (auto it = sources_.begin(); it != sources_.end();) {
        if (it->second.used) {
            ++it;
            continue;
        }
        if (it->second.pending) retireEndpoint(std::move(it->second.endpoint));
        it = sources_.erase(it);
    }
    for (auto it = sinks_.begin(); it != sinks_.end();) {
        if (it->second.used) {
            ++it;
            continue;
        }
        if (it->second.pending) retireEndpoint(std::move(it->second.endpoint));
        it = sinks_.erase(it);
    }
}

void MixerController::applySnapshot(std::unique_ptr<GraphSnapshot> g) {
    analyzeFeedback();
    engine_.publish(std::move(g));
    ++graphVersion_;
}

// A send strip -> bus is part of a loop if audio leaving that bus's output
// device can come back into the strip, e.g. bus -> "CABLE Input" while the
// strip records "CABLE Output", or loopback-capturing the bus's own device.
void MixerController::analyzeFeedback() {
    warnings_.clear();
    const size_t ns = session_.strips.size();
    const size_t nb = session_.buses.size();
    // Device a strip listens to, as a render-endpoint ID (for loopback) or
    // capture ID (for devices).
    std::vector<std::string> stripCapture(ns), stripLoopRender(ns);
    for (size_t i = 0; i < ns; ++i) {
        const SourceRef& r = session_.strips[i].source;
        auto it = sources_.find(r.key());
        const std::string id = it != sources_.end() ? it->second.resolvedId : r.deviceId;
        if (r.kind == SourceKind::CaptureDevice) stripCapture[i] = id;
        if (r.kind == SourceKind::RenderLoopback) stripLoopRender[i] = id;
    }
    // Strips fed by each bus's output device.
    std::vector<std::vector<size_t>> fed(nb);
    for (size_t b = 0; b < nb; ++b) {
        auto it = sinks_.find(session_.buses[b].output.key());
        if (it == sinks_.end() || it->second.resolvedId.empty()) continue;
        const std::string& renderId = it->second.resolvedId;
        const std::string partner = backend_.cablePartnerOfRender(renderId);
        for (size_t s = 0; s < ns; ++s)
            if (stripLoopRender[s] == renderId || (!partner.empty() && stripCapture[s] == partner))
                fed[b].push_back(s);
    }
    auto sendOn = [&](size_t s, size_t b) {
        return session_.strips[s].rt->sends[session_.buses[b].slot].enabled.load();
    };
    // Can audio entering bus `b0` reach strip `target`?
    auto reaches = [&](size_t b0, size_t target) {
        std::vector<bool> seenBus(nb, false), seenStrip(ns, false);
        std::vector<size_t> stack{b0};
        seenBus[b0] = true;
        while (!stack.empty()) {
            const size_t b = stack.back();
            stack.pop_back();
            for (size_t s : fed[b]) {
                if (s == target) return true;
                if (seenStrip[s]) continue;
                seenStrip[s] = true;
                for (size_t b2 = 0; b2 < nb; ++b2)
                    if (!seenBus[b2] && sendOn(s, b2)) {
                        seenBus[b2] = true;
                        stack.push_back(b2);
                    }
            }
        }
        return false;
    };
    const bool protect = session_.engine.feedbackProtection;
    for (size_t s = 0; s < ns; ++s) {
        for (size_t b = 0; b < nb; ++b) {
            SendParams& sp = session_.strips[s].rt->sends[session_.buses[b].slot];
            const bool loop = sendOn(s, b) && reaches(b, s);
            sp.blocked.store(loop && protect);
            if (loop) {
                FeedbackWarning w;
                w.stripId = session_.strips[s].id;
                w.busId = session_.buses[b].id;
                w.blocked = protect;
                w.text = "'" + session_.strips[s].name + "' -> '" + session_.buses[b].name +
                         "' feeds back into itself through the output device" +
                         (protect ? " (send muted by feedback protection)" : " (protection is off!)");
                warnings_.push_back(std::move(w));
            }
        }
    }
}

void MixerController::sendsChanged() {
    analyzeFeedback();
    dirty_ = true;
}

// ---------------------------------------------------------------------------
// Strips
// ---------------------------------------------------------------------------

uint32_t MixerController::addStrip(StripGroup group, const SourceRef& src, const std::string& name) {
    if (session_.strips.size() >= static_cast<size_t>(kMaxStrips)) return 0;
    StripModel s;
    s.id = session_.nextId++;
    s.group = group;
    s.source = src;
    s.name = name;
    static const uint32_t palette[] = {0xFFFFA94D, 0xFF6AD1FF, 0xFF7CE38B, 0xFFE68AFF, 0xFF6A8CFF, 0xFFFFE066};
    s.color = palette[s.id % 6];
    if (src.kind == SourceKind::CaptureDevice || src.kind == SourceKind::ToneGenerator) s.rt->chanCount = 1;
    if (group == StripGroup::Hardware) addDefaultNoiseSuppression(s);
    prepareStrip(s);
    session_.strips.push_back(std::move(s));
    const uint32_t id = session_.strips.back().id;
    rebuildGraph();
    dirty_ = true;
    return id;
}

void MixerController::removeStrip(uint32_t id) {
    auto& v = session_.strips;
    v.erase(std::remove_if(v.begin(), v.end(), [&](const StripModel& s) { return s.id == id; }), v.end());
    rebuildGraph();
    dirty_ = true;
}

void MixerController::setStripSource(uint32_t id, const SourceRef& src) {
    if (StripModel* s = session_.strip(id)) {
        s->source = src;
        rebuildGraph();
        dirty_ = true;
    }
}

void MixerController::moveStrip(uint32_t id, int newIndex) {
    auto& v = session_.strips;
    auto it = std::find_if(v.begin(), v.end(), [&](const StripModel& s) { return s.id == id; });
    if (it == v.end()) return;
    StripModel m = std::move(*it);
    v.erase(it);
    newIndex = clampv(newIndex, 0, static_cast<int>(v.size()));
    v.insert(v.begin() + newIndex, std::move(m));
    rebuildGraph();
    dirty_ = true;
}

void MixerController::setSend(uint32_t stripId, uint32_t busId, bool enabled) {
    StripModel* s = session_.strip(stripId);
    BusModel* b = session_.bus(busId);
    if (!s || !b) return;
    s->rt->sends[b->slot].enabled.store(enabled);
    sendsChanged();
}

// ---------------------------------------------------------------------------
// Buses
// ---------------------------------------------------------------------------

uint32_t MixerController::addBus(BusKind kind, const OutputRef& out, const std::string& name) {
    const int slot = session_.freeBusSlot();
    if (slot < 0) return 0;
    BusModel b;
    b.id = session_.nextId++;
    b.slot = slot;
    b.kind = kind;
    b.output = out;
    b.name = name;
    b.color = kind == BusKind::Physical ? 0xFF4DD2FF : 0xFFB98CFF;
    prepareBus(b);
    // A new bus starts with every send off and unity gain.
    for (auto& s : session_.strips) {
        s.rt->sends[slot].enabled = false;
        s.rt->sends[slot].gainDb = 0.0f;
        s.rt->sends[slot].preFader = false;
    }
    session_.buses.push_back(std::move(b));
    const uint32_t id = session_.buses.back().id;
    rebuildGraph();
    dirty_ = true;
    return id;
}

void MixerController::removeBus(uint32_t id) {
    BusModel* b = session_.bus(id);
    if (!b) return;
    const int slot = b->slot;
    for (auto& s : session_.strips) s.rt->sends[slot].enabled = false;
    auto& v = session_.buses;
    v.erase(std::remove_if(v.begin(), v.end(), [&](const BusModel& m) { return m.id == id; }), v.end());
    if (session_.engine.clockBusId == id) session_.engine.clockBusId = 0;
    rebuildGraph();
    dirty_ = true;
}

void MixerController::setBusOutput(uint32_t id, const OutputRef& out) {
    if (BusModel* b = session_.bus(id)) {
        b->output = out;
        rebuildGraph();
        dirty_ = true;
    }
}

void MixerController::moveBus(uint32_t id, int newIndex) {
    auto& v = session_.buses;
    auto it = std::find_if(v.begin(), v.end(), [&](const BusModel& b) { return b.id == id; });
    if (it == v.end()) return;
    BusModel m = std::move(*it);
    v.erase(it);
    newIndex = clampv(newIndex, 0, static_cast<int>(v.size()));
    v.insert(v.begin() + newIndex, std::move(m));
    rebuildGraph();
    dirty_ = true;
}

// ---------------------------------------------------------------------------
// Effects
// ---------------------------------------------------------------------------

std::vector<std::shared_ptr<Effect>>* MixerController::chainFor(const ChainTarget& t) {
    if (t.bus) {
        BusModel* b = session_.bus(t.id);
        return b ? &b->chain : nullptr;
    }
    StripModel* s = session_.strip(t.id);
    return s ? &s->chain : nullptr;
}

std::shared_ptr<Effect> MixerController::createEffect(const std::string& typeId) const {
    std::unique_ptr<Effect> e = EffectRegistry::instance().create(typeId);
    if (!e) return nullptr;
    e->prepare(engine_.sampleRate(), engine_.blockSize());
    return std::shared_ptr<Effect>(std::move(e));
}

std::shared_ptr<Effect> MixerController::addEffect(const ChainTarget& t, const std::string& typeId, int index) {
    auto* chain = chainFor(t);
    if (!chain || chain->size() >= static_cast<size_t>(kMaxChainLength)) return nullptr;
    auto fx = createEffect(typeId);
    if (!fx) return nullptr;
    if (index < 0 || index > static_cast<int>(chain->size())) chain->push_back(fx);
    else chain->insert(chain->begin() + index, fx);
    chainChanged();
    return fx;
}

void MixerController::removeEffect(const ChainTarget& t, uint32_t instanceId) {
    auto* chain = chainFor(t);
    if (!chain) return;
    chain->erase(std::remove_if(chain->begin(), chain->end(),
                                [&](const std::shared_ptr<Effect>& e) { return e->instanceId == instanceId; }),
                 chain->end());
    chainChanged();
}

void MixerController::moveEffect(const ChainTarget& t, int from, int to) {
    auto* chain = chainFor(t);
    if (!chain) return;
    const int n = static_cast<int>(chain->size());
    if (from < 0 || from >= n || to < 0 || to >= n || from == to) return;
    auto fx = (*chain)[from];
    chain->erase(chain->begin() + from);
    chain->insert(chain->begin() + to, fx);
    chainChanged();
}

void MixerController::setChain(const ChainTarget& t, std::vector<std::shared_ptr<Effect>> chain) {
    auto* c = chainFor(t);
    if (!c) return;
    if (chain.size() > static_cast<size_t>(kMaxChainLength)) chain.resize(kMaxChainLength);
    for (auto& e : chain)
        if (!e->isPrepared() || e->sampleRate() != engine_.sampleRate()) e->prepare(engine_.sampleRate(), engine_.blockSize());
    *c = std::move(chain);
    chainChanged();
}

void MixerController::applyPresetChain(const ChainTarget& t, std::vector<std::shared_ptr<Effect>> chain) {
    if (!t.bus) {
        if (const auto* cur = chainFor(t)) {
            auto has = [&](const char* type) {
                return std::any_of(chain.begin(), chain.end(),
                                   [&](const std::shared_ptr<Effect>& e) { return std::string(e->typeId()) == type; });
            };
            // Keep the canceller first, then the suppressor (the canonical order).
            std::vector<std::shared_ptr<Effect>> keep;
            for (const char* type : {echoCancellerTypeId(), noiseSuppressorTypeId()}) {
                if (has(type)) continue;
                for (const auto& e : *cur)
                    if (std::string(e->typeId()) == type) {
                        keep.push_back(e);
                        break;
                    }
            }
            chain.insert(chain.begin(), keep.begin(), keep.end());
        }
    }
    setChain(t, std::move(chain));
}

void MixerController::chainChanged() {
    rebuildGraph();
    dirty_ = true;
}

// ---------------------------------------------------------------------------
// Voice cleanup defaults
// ---------------------------------------------------------------------------

bool MixerController::isMicrophoneSource(const SourceRef& src) const {
    if (src.kind != SourceKind::CaptureDevice) return false;
    for (const auto& d : backend_.devices()) {
        if (d.render) continue;
        if (src.deviceId.empty() ? d.isDefault : d.id == src.deviceId) return !d.isCable;
    }
    return true; // unknown/unplugged device: treat as a microphone
}

std::vector<StripModel*> MixerController::microphoneStrips() {
    std::vector<StripModel*> out;
    for (auto& s : session_.strips)
        if (isMicrophoneSource(s.source)) out.push_back(&s);
    return out;
}

bool MixerController::microphonesMuted() {
    const auto mics = microphoneStrips();
    if (mics.empty()) return false;
    for (const StripModel* s : mics)
        if (!s->rt->mute.load()) return false;
    return true;
}

void MixerController::setMicrophonesMuted(bool muted) {
    for (StripModel* s : microphoneStrips()) s->rt->mute = muted;
    markDirty();
}

bool MixerController::addDefaultNoiseSuppression(StripModel& s) {
    if (!session_.autoNoiseSuppression || !isMicrophoneSource(s.source)) return false;
    if (s.chain.size() >= static_cast<size_t>(kMaxChainLength)) return false;
    for (const auto& e : s.chain)
        if (std::string(e->typeId()) == noiseSuppressorTypeId()) return false;
    auto fx = createEffect(noiseSuppressorTypeId());
    if (!fx) return false;
    // After an echo canceller if there is one (AEC works best on the raw mic).
    auto pos = s.chain.begin();
    while (pos != s.chain.end() && std::string((*pos)->typeId()) == echoCancellerTypeId()) ++pos;
    s.chain.insert(pos, std::move(fx));
    return true;
}

void MixerController::rescanDevices() {
    backend_.rescan();
    assignCablesToEmptyVirtualBuses();
    rebuildGraph();
}

bool MixerController::assignCablesToEmptyVirtualBuses() {
    const auto devs = backend_.devices();
    bool assigned = false;
    for (auto& b : session_.buses) {
        if (b.kind != BusKind::Virtual || !b.output.deviceId.empty() || !b.output.deviceName.empty()) continue;
        for (const auto& d : devs) {
            if (!d.render || !d.active || !d.isCable || d.isCableSecondary) continue;
            const bool used = std::any_of(session_.buses.begin(), session_.buses.end(),
                                          [&](const BusModel& o) { return o.output.deviceId == d.id; });
            if (used) continue;
            b.output.deviceId = d.id;
            b.output.deviceName = d.name;
            std::string capture;
            const std::string capId = backend_.cablePartnerOfRender(d.id);
            for (const auto& c : devs)
                if (!c.render && c.id == capId) capture = c.name;
            ECHO_LOG_INFO("Virtual bus '%s' assigned to cable '%s'", b.name.c_str(), d.name.c_str());
            notices_.push_back("Virtual cable found: '" + b.name + "' now plays into " + d.name +
                               (capture.empty() ? std::string(".")
                                                : ". Select \"" + capture + "\" as the microphone in your app."));
            assigned = true;
            break;
        }
    }
    if (assigned) {
        rebuildGraph();
        dirty_ = true;
    }
    return assigned;
}

void MixerController::setSoundBank(std::shared_ptr<const SoundBank> bank) {
    soundboard_->setBank(std::move(bank));
    if (started_) rebuildGraph();
}

StripModel* MixerController::soundboardStrip() {
    for (auto& s : session_.strips)
        if (s.source.kind == SourceKind::Soundboard) return &s;
    return nullptr;
}

uint32_t MixerController::ensureSoundboardStrip() {
    if (StripModel* s = soundboardStrip()) return s->id;
    if (session_.strips.size() >= static_cast<size_t>(kMaxStrips)) return 0;
    StripModel s;
    s.id = session_.nextId++;
    s.name = "Soundboard";
    s.group = StripGroup::Generator;
    s.source.kind = SourceKind::Soundboard;
    s.color = 0xFF6A8CFF;
    s.rt->chanCount = 2;
    // Others hear it (virtual mic) and so do you (headphones).
    for (const auto& b : session_.buses) {
        const bool firstOfKind = &b == &*std::find_if(session_.buses.begin(), session_.buses.end(),
                                                       [&](const BusModel& o) { return o.kind == b.kind; });
        if (firstOfKind) s.rt->sends[b.slot].enabled = true;
    }
    prepareStrip(s);
    session_.strips.push_back(std::move(s));
    const uint32_t id = session_.strips.back().id;
    if (started_) rebuildGraph();
    dirty_ = true;
    return id;
}

void MixerController::migrateSession() {
    if (!session_.soundboard.initialized) {
        addBuiltinPads(session_.soundboard);
        session_.soundboard.initialized = true;
        dirty_ = true;
    } else if (session_.soundboard.builtinVersion < kBuiltinSoundsVersion) {
        addNewBuiltinPads(session_.soundboard); // sounds added to Echo since
        dirty_ = true;
    }
    if (session_.configVersion < 3) {
        ensureSoundboardStrip();
        dirty_ = true;
    }
    if (session_.configVersion < 4) {
        // The voice gate's depth went from -30 dB (hardly heard after the
        // noise removal) to -60 dB: gates still at the old default move along.
        for (auto& s : session_.strips)
            for (auto& e : s.chain)
                if (std::string(e->typeId()) == noiseSuppressorTypeId()) {
                    const int i = e->paramIndex("vad_range");
                    if (i >= 0 && std::fabs(e->param(i) + 30.0f) < 0.01f) e->setParam(i, -60.0f);
                }
        dirty_ = true;
    }
    if (session_.configVersion < 5) {
        // The AI voice's Buffer became the least delay (the voice's Response
        // sets what it needs): ones still at the old default of 400 drop to 100.
        for (auto& s : session_.strips)
            for (auto& e : s.chain)
                if (std::string(e->typeId()) == "ai_voice") {
                    const int i = e->paramIndex("latency");
                    if (i >= 0 && std::fabs(e->param(i) - 400.0f) < 0.01f) e->setParam(i, 100.0f);
                }
        dirty_ = true;
    }
    if (session_.configVersion < 2) {
        // Noise suppression became a default part of microphone inputs. Add
        // it once to microphones of older sessions; afterwards the user's
        // choice (keeping or removing it) is saved like any other effect.
        int added = 0;
        for (auto& s : session_.strips)
            if (s.group == StripGroup::Hardware && addDefaultNoiseSuppression(s)) ++added;
        if (added) ECHO_LOG_INFO("Session upgrade: added noise suppression to %d microphone input(s)", added);
        dirty_ = true;
    }
    session_.configVersion = kSessionConfigVersion;
}

// ---------------------------------------------------------------------------
// Recording
// ---------------------------------------------------------------------------

bool MixerController::startRecording(const std::vector<RecordingTarget>& targets, std::string* error) {
    const std::wstring folder = utf8ToWide(session_.recorder.folder);
    if (folder.empty()) {
        if (error) *error = "No recording folder set";
        return false;
    }
    if (!recorder_.start(targets, folder, engine_.sampleRate(), session_.recorder.sampleRate,
                         session_.recorder.bitDepth, error))
        return false;
    rebuildGraph();
    return true;
}

void MixerController::stopRecording() {
    // The recorder drains and closes the files; the rebuilt graph no longer
    // contains the taps (the retired snapshot keeps them alive meanwhile).
    recorder_.stop();
    rebuildGraph();
}

// ---------------------------------------------------------------------------
// Status
// ---------------------------------------------------------------------------

const SourceRuntime* MixerController::sourceFor(uint32_t stripId) const {
    auto k = stripSourceKey_.find(stripId);
    if (k == stripSourceKey_.end()) return nullptr;
    auto it = sources_.find(k->second);
    return it != sources_.end() ? it->second.runtime.get() : nullptr;
}

const SinkRuntime* MixerController::sinkFor(uint32_t busId) const {
    auto k = busSinkKey_.find(busId);
    if (k == busSinkKey_.end()) return nullptr;
    auto it = sinks_.find(k->second);
    return it != sinks_.end() ? it->second.runtime.get() : nullptr;
}

const CaptureEndpoint* MixerController::captureEndpointFor(uint32_t stripId) const {
    auto k = stripSourceKey_.find(stripId);
    if (k == stripSourceKey_.end()) return nullptr;
    auto it = sources_.find(k->second);
    return it != sources_.end() ? it->second.endpoint.get() : nullptr;
}

const RenderEndpoint* MixerController::renderEndpointFor(uint32_t busId) const {
    auto k = busSinkKey_.find(busId);
    if (k == busSinkKey_.end()) return nullptr;
    auto it = sinks_.find(k->second);
    return it != sinks_.end() ? it->second.endpoint.get() : nullptr;
}

double MixerController::estimatedLatencyMs(uint32_t stripId, uint32_t busId) const {
    double ms = engine_.blockSeconds() * 1000.0;
    if (const SourceRuntime* s = sourceFor(stripId)) ms += s->latencySeconds() * 1000.0;
    if (const SinkRuntime* k = sinkFor(busId)) ms += k->latencySeconds() * 1000.0;
    for (const auto& st : session_.strips)
        if (st.id == stripId)
            for (const auto& fx : st.chain)
                if (!fx->bypass.load()) ms += fx->latencySamples() * 1000.0 / engine_.sampleRate();
    return ms;
}

std::string MixerController::clockDescription() const {
    for (const auto& b : session_.buses)
        if (b.isClock) return "Clocked by " + b.name + " output";
    return "Internal timer clock";
}

// ---------------------------------------------------------------------------
// Defaults
// ---------------------------------------------------------------------------

void MixerController::resetSettings(const ResetParts& parts) {
    const Session defaults;
    if (parts.mixer) {
        // A fresh installation's inputs and outputs. createDefaultSession
        // keeps the other settings; hotkeys are kept here unless reset too.
        const HotkeySettings hotkeys = session_.hotkeys;
        session_.autoNoiseSuppression = defaults.autoNoiseSuppression;
        createDefaultSession();
        session_.hotkeys = hotkeys;
    }
    if (parts.hotkeys) session_.hotkeys = defaults.hotkeys;
    if (parts.soundboard) {
        session_.soundboard = SoundboardSettings{};
        addBuiltinPads(session_.soundboard);
        session_.soundboard.initialized = true;
    }
    if (parts.look) {
        UiSettings ui = defaults.ui;
        ui.windowX = session_.ui.windowX;
        ui.windowY = session_.ui.windowY;
        ui.windowW = session_.ui.windowW;
        ui.windowH = session_.ui.windowH;
        ui.maximized = session_.ui.maximized;
        session_.ui = ui;
    }
    if (parts.audio) {
        session_.recorder = defaults.recorder;
        const EngineSettings& d = defaults.engine;
        EngineSettings& e = session_.engine;
        const bool formatChanged = e.sampleRate != d.sampleRate || e.blockSize != d.blockSize ||
                                   e.resamplerQuality != d.resamplerQuality || e.lowLatencyShared != d.lowLatencyShared;
        e = d;
        if (formatChanged) restart();
        else if (started_) rebuildGraph();
    }
    dirty_ = true;
}

void MixerController::createDefaultSession() {
    Session fresh;
    fresh.engine = session_.engine;
    fresh.ui = session_.ui;
    fresh.recorder = session_.recorder;
    fresh.firstRunCompleted = session_.firstRunCompleted;
    fresh.autoNoiseSuppression = session_.autoNoiseSuppression;
    fresh.cables = session_.cables;
    fresh.soundboard = session_.soundboard; // the user's sounds stay
    const bool wasRunning = engine_.running() || started_;
    stop();
    session_ = std::move(fresh);

    // Buses: headphones (default output) and a virtual-mic bus.
    BusModel a1;
    a1.id = session_.nextId++;
    a1.slot = 0;
    a1.kind = BusKind::Physical;
    a1.name = "Headphones";
    a1.color = 0xFF4DD2FF;
    session_.buses.push_back(std::move(a1));

    BusModel b1;
    b1.id = session_.nextId++;
    b1.slot = 1;
    b1.kind = BusKind::Virtual;
    b1.name = "Virtual Mic";
    b1.color = 0xFFB98CFF;
    for (const auto& d : backend_.devices())
        if (d.render && d.active && d.isCable && !d.isCableSecondary) {
            b1.output.deviceId = d.id;
            b1.output.deviceName = d.name;
            break;
        }
    session_.buses.push_back(std::move(b1));

    StripModel mic;
    mic.id = session_.nextId++;
    mic.name = "Microphone";
    mic.group = StripGroup::Hardware;
    mic.source.kind = SourceKind::CaptureDevice; // empty ID = Windows default microphone
    mic.color = 0xFFFFA94D;
    mic.rt->chanCount = 1;
    mic.rt->sends[1].enabled = true; // mic -> virtual mic
    addDefaultNoiseSuppression(mic);
    session_.strips.push_back(std::move(mic));

    StripModel desk;
    desk.id = session_.nextId++;
    desk.name = "Desktop Audio";
    desk.group = StripGroup::Application;
    desk.source.kind = SourceKind::SystemExcludingEcho;
    desk.color = 0xFF7CE38B;
    session_.strips.push_back(std::move(desk));
    ensureSoundboardStrip();
    if (!session_.soundboard.initialized) {
        addBuiltinPads(session_.soundboard);
        session_.soundboard.initialized = true;
    }

    prepareAll();
    if (wasRunning) start();
    dirty_ = true;
}

} // namespace echo
