// The real-time audio engine.
//
// The UI thread builds an immutable GraphSnapshot (sources, strips, chains,
// buses, sinks, recording taps) and publishes it with publish(). The engine
// thread picks up the newest snapshot at each block boundary. Retired
// snapshots are freed on the UI thread once the engine has completed two
// further blocks, so the audio thread never frees memory or takes a lock.
//
// Clocking: if the snapshot names a master sink whose device is running, the
// engine is paced by that device (it wakes when the device has consumed
// data). Otherwise a high-resolution waitable timer paces it. All other
// devices are kept in sync by adaptive resampling.
#pragma once

#include <atomic>
#include <memory>
#include <thread>
#include <vector>

#include "core/Common.h"
#include "core/Endpoint.h"
#include "core/Mixer.h"
#include "core/Sinks.h"
#include "core/Sources.h"

namespace echo {

constexpr int kMaxTaps = 16;

struct StripSnap {
    StripRuntime* rt = nullptr;
    int source = -1;     // index into GraphSnapshot::sources or -1
    int reference = -1;  // optional reference source (echo cancellation)
    ChainSnap chain;
};

struct BusSnap {
    BusRuntime* rt = nullptr;
    int slot = 0;     // stable slot used by strip sends
    int sink = -1;    // index into GraphSnapshot::sinks or -1
    ChainSnap chain;
};

struct TapSnap {
    RecordTap* tap = nullptr;
    int kind = RecordTap::Bus;
    int index = 0;    // strip index or bus index within the snapshot
};

struct GraphSnapshot {
    int numSources = 0;
    SourceRuntime* sources[kMaxSources] = {};
    int numStrips = 0;
    StripSnap strips[kMaxStrips];
    int numBuses = 0;
    BusSnap buses[kMaxBuses];
    int numSinks = 0;
    SinkRuntime* sinks[kMaxSinks] = {};
    int masterSink = -1;
    int numTaps = 0;
    TapSnap taps[kMaxTaps];
    // Keeps every referenced runtime object alive while the snapshot exists.
    std::vector<std::shared_ptr<void>> keepAlive;
};

enum class ClockMode : int { Stopped = 0, Device = 1, Timer = 2, Manual = 3 };

struct EngineStats {
    std::atomic<float> cpuLoad{0.0f};       // smoothed fraction of block time used
    std::atomic<float> cpuPeak{0.0f};       // max since last read
    std::atomic<float> blockMicros{0.0f};   // last block processing time
    std::atomic<uint64_t> blocks{0};
    std::atomic<uint64_t> lateBlocks{0};    // timer mode: blocks dropped to catch up
    std::atomic<uint64_t> deviceStalls{0};  // master device stopped waking us
    std::atomic<uint64_t> exceptions{0};
    std::atomic<int> clockMode{static_cast<int>(ClockMode::Stopped)};
};

class Engine : NonCopyable {
public:
    Engine();
    ~Engine();

    // Format may only change while stopped.
    void setFormat(int sampleRate, int blockSize);
    int sampleRate() const { return sampleRate_; }
    int blockSize() const { return blockSize_; }
    double blockSeconds() const { return static_cast<double>(blockSize_) / sampleRate_; }

    // UI thread: publish a new graph (takes ownership).
    void publish(std::unique_ptr<GraphSnapshot> snapshot);
    // UI thread: free retired snapshots that the engine can no longer see.
    void collectGarbage();
    // Current snapshot (UI thread; valid until the next publish()).
    const GraphSnapshot* current() const { return current_.load(std::memory_order_acquire); }

    bool start();
    void stop();
    bool running() const { return running_.load(std::memory_order_acquire); }

    // Processes one block synchronously. Used by the engine thread, and by
    // tests while the engine thread is stopped.
    void processBlock();

    // Push-to-talk gate for strips marked talkGated (microphones). Written by
    // the hotkey thread; read once per block.
    void setMicGate(bool open) { micGate_.store(open, std::memory_order_relaxed); }
    bool micGate() const { return micGate_.load(std::memory_order_relaxed); }

    WakeTarget* wakeTarget();
    EngineStats& stats() { return stats_; }
    uint64_t epoch() const { return epoch_.load(std::memory_order_acquire); }
    uint64_t framesProcessed() const { return framesProcessed_.load(std::memory_order_relaxed); }

private:
    void threadMain();
    void processBlockImpl(GraphSnapshot* g, int n);

    int sampleRate_ = 48000;
    int blockSize_ = 480;

    std::atomic<GraphSnapshot*> current_{nullptr};
    struct Retired {
        GraphSnapshot* snap;
        uint64_t epoch;
    };
    std::vector<Retired> retired_;
    std::atomic<uint64_t> epoch_{0};
    std::atomic<uint64_t> framesProcessed_{0};
    std::atomic<bool> micGate_{true};

    std::vector<float> busBuf_; // kMaxBuses * 2 * kMaxBlockSize
    BusAccumulators acc_;

    std::thread thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> stopRequested_{false};
    struct Impl;
    std::unique_ptr<Impl> impl_;
    EngineStats stats_;
};

} // namespace echo
