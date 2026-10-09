// Device-agnostic endpoint interfaces. The engine talks to capture and render
// devices only through these, which keeps the engine testable with fake
// devices and keeps all WASAPI code in src/audio.
//
// An endpoint object has a fixed format for its whole lifetime. If a device
// changes format or disappears, the platform layer creates a new object and
// the session controller swaps it in; the engine never reconfigures streams.
#pragma once

#include <atomic>
#include <cstdint>
#include <string>

#include "core/SpscRing.h"

namespace echo {

enum class EndpointStatus : int {
    Starting = 0,
    Running = 1,
    Lost = 2,     // device removed / invalidated; will be reopened
    Error = 3,    // failed to open or fatal stream error
    Stopped = 4,
};

const char* endpointStatusName(EndpointStatus s);

struct EndpointStats {
    std::atomic<uint64_t> frames{0};         // frames moved by the device thread
    std::atomic<uint64_t> underruns{0};      // render: ring empty when device needed data
    std::atomic<uint64_t> overruns{0};       // capture: ring full, data dropped
    std::atomic<uint64_t> discontinuities{0};// device-reported glitches
    std::atomic<uint64_t> wakeups{0};
};

// Something the engine can be woken through (master clock signalling).
struct WakeTarget {
    virtual ~WakeTarget() = default;
    virtual void wake() = 0;
};

class CaptureEndpoint {
public:
    virtual ~CaptureEndpoint() = default;
    virtual int channels() const = 0;
    virtual int sampleRate() const = 0;
    virtual int periodFrames() const = 0;
    // Interleaved float samples at the device rate, written by the device
    // thread and read by the engine thread.
    virtual SpscRing<float>& ring() = 0;
    virtual EndpointStatus status() const = 0;
    // Loopback-style sources deliver no packets while nothing is playing;
    // running dry is then silence, not an error.
    virtual bool isSparse() const = 0;
    virtual double deviceLatencySeconds() const = 0;
    virtual const std::string& describe() const = 0;
    EndpointStats stats;
};

class RenderEndpoint {
public:
    virtual ~RenderEndpoint() = default;
    virtual int channels() const = 0;
    virtual int sampleRate() const = 0;
    virtual int periodFrames() const = 0;
    // Interleaved float samples at the device rate, written by the engine
    // thread and consumed by the device thread.
    virtual SpscRing<float>& ring() = 0;
    virtual EndpointStatus status() const = 0;
    virtual double deviceLatencySeconds() const = 0;
    virtual const std::string& describe() const = 0;
    // Master clock: after consuming data, if the ring holds fewer than
    // `thresholdFrames` frames, the device thread calls target->wake().
    virtual void setWakeTarget(WakeTarget* target, int thresholdFrames) = 0;
    EndpointStats stats;
};

} // namespace echo
