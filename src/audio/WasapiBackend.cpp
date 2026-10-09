#include "audio/WasapiBackend.h"

#include "audio/WasapiStream.h"

namespace echo::win {

WasapiBackend::WasapiBackend() = default;
WasapiBackend::~WasapiBackend() { dm_.shutdown(); }

bool WasapiBackend::init(std::string* error) {
    if (!dm_.init(error)) return false;
    refreshPairs();
    sessions_.refresh();
    return true;
}

void WasapiBackend::refreshPairs() {
    pairs_ = findCablePairs(dm_.devices());
    for (const auto& e : extra_) {
        bool dup = false;
        for (const auto& p : pairs_) dup |= p.renderId == e.renderId;
        if (!dup) pairs_.push_back(e);
    }
}

void WasapiBackend::setExtraCablePairs(std::vector<CablePair> extra) {
    extra_ = std::move(extra);
    refreshPairs();
}

void WasapiBackend::rescan() {
    dm_.rescan();
    refreshPairs();
}

bool WasapiBackend::pollDeviceChanges() {
    if (!dm_.refreshIfChanged()) return false;
    refreshPairs();
    return true;
}

std::shared_ptr<CaptureEndpoint> WasapiBackend::openCapture(const CaptureRequest& req, std::string* error) {
    StreamOptions o;
    o.label = req.label;
    o.exclusive = req.exclusive;
    o.lowLatencyShared = req.lowLatency;
    o.preferredSampleRate = req.preferredRate;
    switch (req.kind) {
    case SourceKind::CaptureDevice:
        o.deviceId = fromUtf8(req.deviceId);
        o.captureMode = CaptureMode::Device;
        break;
    case SourceKind::RenderLoopback:
        o.deviceId = fromUtf8(req.deviceId);
        o.captureMode = CaptureMode::Loopback;
        break;
    case SourceKind::ProcessLoopback:
        o.captureMode = CaptureMode::ProcessLoopback;
        o.processId = req.processId;
        o.includeProcessTree = true;
        break;
    case SourceKind::SystemExcludingEcho:
        // Exclude-mode process loopback of our own process: every other
        // app's audio, and never Echo's own output (no feedback).
        o.captureMode = CaptureMode::ProcessLoopback;
        o.processId = GetCurrentProcessId();
        o.includeProcessTree = false;
        break;
    default:
        if (error) *error = "Unsupported source type";
        return nullptr;
    }
    // Opens on the stream thread; the controller adopts it once running
    // (a slow driver never blocks the UI).
    return WasapiCapture::openAsync(o);
}

std::shared_ptr<RenderEndpoint> WasapiBackend::openRender(const RenderRequest& req, std::string* error) {
    StreamOptions o;
    o.deviceId = fromUtf8(req.deviceId);
    o.label = req.label;
    o.exclusive = req.exclusive;
    o.lowLatencyShared = req.lowLatency;
    o.preferredSampleRate = req.preferredRate;
    return WasapiRender::openAsync(o);
}

std::vector<BackendDevice> WasapiBackend::devices() {
    std::vector<BackendDevice> out;
    for (const auto& d : dm_.devices()) {
        BackendDevice b;
        b.id = d.idUtf8;
        b.name = d.name;
        b.render = d.flow == Flow::Render;
        b.active = d.active();
        b.isDefault = d.isDefault;
        b.isCable = d.virtualKind == VirtualKind::Cable;
        for (const auto& p : pairs_)
            if (p.secondary && p.renderId == d.idUtf8) b.isCableSecondary = true;
        b.isAppBound = d.virtualKind == VirtualKind::AppBound;
        b.channels = d.mixFormat.channels;
        b.sampleRate = d.mixFormat.sampleRate;
        out.push_back(std::move(b));
    }
    return out;
}

std::string WasapiBackend::resolveDevice(const std::string& id, const std::string& name, bool render) {
    const Flow f = render ? Flow::Render : Flow::Capture;
    if (auto d = dm_.find(id); d && d->active() && d->flow == f) return d->idUtf8;
    if (!name.empty())
        if (auto d = dm_.findByName(f, name)) return d->idUtf8;
    return {};
}

std::string WasapiBackend::defaultDevice(bool render) {
    return dm_.defaultDeviceId(render ? Flow::Render : Flow::Capture);
}

std::string WasapiBackend::cablePartnerOfRender(const std::string& renderId) {
    for (const auto& p : pairs_)
        if (p.renderId == renderId) return p.captureId;
    return {};
}

std::vector<BackendProcess> WasapiBackend::audioProcesses() {
    // Session enumeration is cheap but not free; refresh at most every 1 s.
    const double now = static_cast<double>(GetTickCount64()) / 1000.0;
    if (now - lastSessionRefresh_ > 1.0) {
        sessions_.refresh();
        lastSessionRefresh_ = now;
    }
    std::vector<BackendProcess> out;
    for (const auto& s : sessions_.sessions()) {
        if (s.systemSounds || !s.pid) continue;
        bool dup = false;
        for (const auto& o : out) dup |= o.pid == s.pid;
        if (dup) continue;
        out.push_back(BackendProcess{s.pid, s.exeName, s.displayName});
    }
    return out;
}

bool WasapiBackend::processAlive(uint32_t pid) {
    HANDLE h = OpenProcess(SYNCHRONIZE, FALSE, pid);
    if (!h) return false;
    const bool alive = WaitForSingleObject(h, 0) == WAIT_TIMEOUT;
    CloseHandle(h);
    return alive;
}

bool WasapiBackend::processLoopbackSupported() { return win::processLoopbackSupported(); }

} // namespace echo::win
