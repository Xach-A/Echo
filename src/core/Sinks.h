// Output sinks. Each distinct output device is one sink; every bus assigned
// to that device is summed into it once per block.
#pragma once

#include <atomic>
#include <memory>
#include <string>
#include <vector>

#include "core/Common.h"
#include "core/Endpoint.h"
#include "core/Meter.h"
#include "core/Resampler.h"

namespace echo {

class SinkRuntime : NonCopyable {
public:
    virtual ~SinkRuntime() = default;

    virtual void prepare(double engineRate, int maxBlock, double blockSeconds);
    float* left() { return l_.data(); }
    float* right() { return r_.data(); }
    void clear(int frames) {
        std::memset(l_.data(), 0, sizeof(float) * frames);
        std::memset(r_.data(), 0, sizeof(float) * frames);
    }
    // Real-time: consume `frames` frames from left()/right().
    virtual void push(int frames) = 0;

    // Master-clock support (only device sinks can drive the engine).
    virtual bool clockUsable() const { return false; }
    virtual bool clockNeedsData() const { return false; }
    virtual double latencySeconds() const { return 0.0; }

    std::string key;
    std::string label;
    MeterState meter;
    std::atomic<uint64_t> underruns{0};
    std::atomic<uint64_t> overruns{0};
    std::atomic<float> fillMs{0.0f};
    std::atomic<float> driftPpm{0.0f};
    std::atomic<bool> online{false};

protected:
    double engineRate_ = 48000.0;
    int maxBlock_ = 0;
    double blockSeconds_ = 0.01;
    std::vector<float> l_, r_;
};

class DeviceRenderSink final : public SinkRuntime {
public:
    explicit DeviceRenderSink(std::shared_ptr<RenderEndpoint> endpoint,
                              ResamplerQuality quality = ResamplerQuality::Normal);
    ~DeviceRenderSink() override;

    // Non-real-time: make this sink the engine clock (or not).
    void setMaster(bool master, WakeTarget* wake);
    bool isMaster() const { return master_; }

    void prepare(double engineRate, int maxBlock, double blockSeconds) override;
    void push(int frames) override;
    bool clockUsable() const override;
    bool clockNeedsData() const override;
    double latencySeconds() const override;

    RenderEndpoint* endpoint() const { return ep_.get(); }
    const std::shared_ptr<RenderEndpoint>& endpointPtr() const { return ep_; }

private:
    std::shared_ptr<RenderEndpoint> ep_;
    ResamplerQuality quality_;
    Resampler rs_;
    DriftController dc_;
    std::vector<float> stereo_; // interleaved engine-rate input
    std::vector<float> out_;    // interleaved stereo device-rate output
    std::vector<float> dev_;    // interleaved device-channel output
    int devCh_ = 2;
    bool master_ = false;
    bool primed_ = false;
    double targetFill_ = 0.0;   // device frames
    double masterTarget_ = 0.0; // device frames
    double overflowFill_ = 0.0;
};

// Collects output in memory (tests, latency probes). Preallocated; frames
// beyond capacity are counted as overruns.
class MemorySink final : public SinkRuntime {
public:
    explicit MemorySink(size_t capacityFrames) : capacity_(capacityFrames) {}
    void prepare(double engineRate, int maxBlock, double blockSeconds) override;
    void push(int frames) override;
    const std::vector<float>& leftData() const { return outL_; }
    const std::vector<float>& rightData() const { return outR_; }
    size_t written() const { return written_; }
    void rewind() { written_ = 0; }

private:
    size_t capacity_;
    size_t written_ = 0;
    std::vector<float> outL_, outR_;
};

} // namespace echo
