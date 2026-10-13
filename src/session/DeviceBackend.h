// Abstraction over the platform audio layer used by the session controller.
// The Windows implementation lives in src/audio (WasapiBackend); tests use a
// fake implementation, so all controller logic is testable without devices.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "core/Endpoint.h"

namespace echo {

enum class SourceKind : int {
    None = 0,
    CaptureDevice = 1,       // microphone, line-in, or a virtual cable's recording side
    RenderLoopback = 2,      // everything playing on an output device
    ProcessLoopback = 3,     // one application's audio (process tree)
    SystemExcludingEcho = 4, // all system audio except Echo itself (feedback-safe)
    ToneGenerator = 5,       // built-in test signal
    Soundboard = 6,          // Echo's soundboard (sound pads)
};

const char* sourceKindName(SourceKind k);

struct CaptureRequest {
    SourceKind kind = SourceKind::CaptureDevice;
    std::string deviceId;
    uint32_t processId = 0;
    bool exclusive = false;
    bool lowLatency = false;
    int preferredRate = 48000;
    std::string label;
};

struct RenderRequest {
    std::string deviceId;
    bool exclusive = false;
    bool lowLatency = false;
    int preferredRate = 48000;
    std::string label;
};

struct BackendDevice {
    std::string id;
    std::string name;
    bool render = false;
    bool active = false;
    bool isDefault = false;
    bool isCable = false;    // loopback cable endpoint
    bool isCableSecondary = false; // extra playback endpoint of a cable that has a main one
    bool isAppBound = false; // virtual device owned by another app
    int channels = 0;
    int sampleRate = 0;
};

struct BackendProcess {
    uint32_t pid = 0;
    std::string exeName;   // "spotify.exe"
    std::string displayName;
};

class DeviceBackend {
public:
    virtual ~DeviceBackend() = default;

    // Opening may complete asynchronously: the endpoint can be returned with
    // status Starting and become Running (or Error) later. nullptr = failed
    // immediately (`error` says why).
    virtual std::shared_ptr<CaptureEndpoint> openCapture(const CaptureRequest& req, std::string* error) = 0;
    virtual std::shared_ptr<RenderEndpoint> openRender(const RenderRequest& req, std::string* error) = 0;

    // Device list (refreshed by the backend when Windows reports changes).
    virtual std::vector<BackendDevice> devices() = 0;
    // Increments whenever the device list changes.
    virtual uint64_t deviceGeneration() = 0;
    // Polls for device changes; returns true when the list changed.
    virtual bool pollDeviceChanges() = 0;
    // Re-reads the device list now (user pressed Rescan).
    virtual void rescan() {}

    // Returns `id` when that endpoint is present and active; otherwise the ID
    // of an active endpoint with the same name; otherwise "".
    virtual std::string resolveDevice(const std::string& id, const std::string& name, bool render) = 0;
    virtual std::string defaultDevice(bool render) = 0;

    // For a loopback cable: the capture endpoint that receives audio rendered
    // to `renderId` ("" if none known). Used for feedback detection.
    virtual std::string cablePartnerOfRender(const std::string& renderId) = 0;

    // Running processes that currently have audio sessions.
    virtual std::vector<BackendProcess> audioProcesses() = 0;
    virtual bool processAlive(uint32_t pid) = 0;
    virtual bool processLoopbackSupported() = 0;
};

} // namespace echo
