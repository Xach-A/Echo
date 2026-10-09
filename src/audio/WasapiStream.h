// WASAPI capture and render streams. Each stream owns a dedicated
// MMCSS-registered thread that moves audio between the device buffer and a
// lock-free ring shared with the engine. All COM objects live on that thread.
#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "audio/WinUtil.h"
#include "core/Common.h"
#include "core/Endpoint.h"

namespace echo::win {

enum class CaptureMode {
    Device = 0,          // microphone / line-in / virtual cable output
    Loopback = 1,        // what a render device is playing
    ProcessLoopback = 2, // audio rendered by one process (tree), Win10 2004+
};

struct StreamOptions {
    std::wstring deviceId;          // endpoint ID (unused for ProcessLoopback)
    CaptureMode captureMode = CaptureMode::Device;
    DWORD processId = 0;
    bool includeProcessTree = true; // include child processes (browsers!)
    bool exclusive = false;         // WASAPI exclusive mode
    bool lowLatencyShared = false;  // IAudioClient3 minimum shared-mode period
    int preferredSampleRate = 48000;// exclusive mode / process loopback rate
    double ringSeconds = 1.0;
    std::string label;
};

class WasapiCapture final : public CaptureEndpoint {
public:
    // Opens and waits until the stream runs (or fails).
    static std::shared_ptr<WasapiCapture> open(const StreamOptions& opt, std::string* error);
    // Starts opening on the stream's own thread and returns at once: status
    // is Starting until the device is ready, then Running or Error.
    static std::shared_ptr<WasapiCapture> openAsync(const StreamOptions& opt);
    ~WasapiCapture() override;

    int channels() const override { return format_.channels; }
    int sampleRate() const override { return format_.sampleRate; }
    int periodFrames() const override { return periodFrames_; }
    SpscRing<float>& ring() override { return ring_; }
    EndpointStatus status() const override { return static_cast<EndpointStatus>(status_.load()); }
    bool isSparse() const override { return opt_.captureMode != CaptureMode::Device; }
    double deviceLatencySeconds() const override { return latencySec_; }
    const std::string& describe() const override { return opt_.label; }

    const DeviceFormat& format() const { return format_; }
    const StreamOptions& options() const { return opt_; }
    std::string lastError() const override;
    bool flowStamp(FlowStamp& out) const override { return flow_.read(out); }
    double flowClockNow() const override { return monotonicSeconds(); }
    // Milliseconds since the last packet arrived (diagnostics).
    double msSinceLastPacket() const;
    // True when the device has only ever delivered digital silence (often
    // a sign of Windows microphone privacy blocking the app).
    bool onlySilenceSoFar() const { return !sawNonSilence_.load(); }

private:
    explicit WasapiCapture(const StreamOptions& opt);
    void threadMain();
    HRESULT initialize();

    StreamOptions opt_;
    DeviceFormat format_;
    int periodFrames_ = 480;
    UINT32 bufferFrames_ = 0;
    double latencySec_ = 0.0;
    SpscRing<float> ring_;
    std::vector<float> scratch_;

    ComPtr<IAudioClient> client_;
    ComPtr<IAudioCaptureClient> capture_;
    HANDLE audioEvent_ = nullptr;
    HANDLE stopEvent_ = nullptr;
    HANDLE readyEvent_ = nullptr;
    std::thread thread_;
    FlowStampCell flow_;
    uint64_t flowTotal_ = 0; // device thread only
    std::atomic<int> status_{static_cast<int>(EndpointStatus::Starting)};
    std::atomic<int64_t> lastPacketTick_{0};
    std::atomic<bool> sawNonSilence_{false};
    mutable std::mutex errMutex_;
    std::string error_;
};

class WasapiRender final : public RenderEndpoint {
public:
    static std::shared_ptr<WasapiRender> open(const StreamOptions& opt, std::string* error);
    static std::shared_ptr<WasapiRender> openAsync(const StreamOptions& opt);
    ~WasapiRender() override;

    int channels() const override { return format_.channels; }
    int sampleRate() const override { return format_.sampleRate; }
    int periodFrames() const override { return periodFrames_; }
    SpscRing<float>& ring() override { return ring_; }
    EndpointStatus status() const override { return static_cast<EndpointStatus>(status_.load()); }
    double deviceLatencySeconds() const override { return latencySec_; }
    const std::string& describe() const override { return opt_.label; }
    void setWakeTarget(WakeTarget* target, int thresholdFrames) override;

    const DeviceFormat& format() const { return format_; }
    const StreamOptions& options() const { return opt_; }
    std::string lastError() const override;
    bool flowStamp(FlowStamp& out) const override { return flow_.read(out); }
    double flowClockNow() const override { return monotonicSeconds(); }

private:
    explicit WasapiRender(const StreamOptions& opt);
    void threadMain();
    HRESULT initialize();

    StreamOptions opt_;
    DeviceFormat format_;
    int periodFrames_ = 480;
    UINT32 bufferFrames_ = 0;
    int padTarget_ = 960;
    double latencySec_ = 0.0;
    SpscRing<float> ring_;
    std::vector<float> scratch_;

    ComPtr<IAudioClient> client_;
    ComPtr<IAudioRenderClient> render_;
    HANDLE audioEvent_ = nullptr;
    HANDLE stopEvent_ = nullptr;
    HANDLE readyEvent_ = nullptr;
    std::thread thread_;
    FlowStampCell flow_;
    uint64_t flowTotal_ = 0; // device thread only
    std::atomic<int> status_{static_cast<int>(EndpointStatus::Starting)};
    std::atomic<WakeTarget*> wake_{nullptr};
    std::atomic<int> wakeThreshold_{0};
    mutable std::mutex errMutex_;
    std::string error_;
};

// Process loopback capture needs Windows 10 build 19041 (2004) or later.
bool processLoopbackSupported();

} // namespace echo::win
