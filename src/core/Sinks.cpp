#include "core/Sinks.h"

#include <cstring>

namespace echo {

void SinkRuntime::prepare(double engineRate, int maxBlock, double blockSeconds) {
    engineRate_ = engineRate;
    maxBlock_ = maxBlock;
    blockSeconds_ = blockSeconds;
    l_.assign(maxBlock, 0.0f);
    r_.assign(maxBlock, 0.0f);
}

// --------------------------------------------------------------------------

DeviceRenderSink::DeviceRenderSink(std::shared_ptr<RenderEndpoint> endpoint, ResamplerQuality quality)
    : ep_(std::move(endpoint)), quality_(quality) {
    devCh_ = ep_ ? std::max(1, ep_->channels()) : 2;
    if (ep_) label = ep_->describe();
}

DeviceRenderSink::~DeviceRenderSink() {
    if (master_ && ep_) ep_->setWakeTarget(nullptr, 0);
}

void DeviceRenderSink::setMaster(bool master, WakeTarget* wake) {
    master_ = master;
    if (!ep_) return;
    if (master) {
        ep_->setWakeTarget(wake, static_cast<int>(masterTarget_));
    } else {
        ep_->setWakeTarget(nullptr, 0);
    }
}

void DeviceRenderSink::prepare(double engineRate, int maxBlock, double blockSeconds) {
    SinkRuntime::prepare(engineRate, maxBlock, blockSeconds);
    const double devRate = ep_ ? ep_->sampleRate() : engineRate;
    const double ratio = engineRate / devRate; // in (engine) per out (device)
    rs_.prepare(2, ratio, maxBlock, quality_);
    stereo_.assign(static_cast<size_t>(maxBlock) * 2, 0.0f);
    const int maxOut = static_cast<int>(maxBlock / ratio * 1.02) + 256;
    out_.assign(static_cast<size_t>(maxOut) * 2, 0.0f);
    dev_.assign(static_cast<size_t>(maxOut) * devCh_, 0.0f);

    const double period = ep_ ? std::max(1, ep_->periodFrames()) : 480;
    const double blockDev = maxBlock / ratio;
    baseTarget_ = period + blockDev + devRate * 0.002;
    masterTarget_ = period + devRate * 0.001;
    overflowExtra_ = std::max(4.0 * blockDev, devRate * 0.12);
    margin_.prepare(period, 2.0 * period, 1800.0 / std::max(1e-3, blockSeconds)); // see DeviceCaptureSource
    applyTarget();
    dc_.prepare(ratio, targetFill_, devRate, blockSeconds);
    primed_ = false;
    if (master_ && ep_) ep_->setWakeTarget(nullptr, 0); // re-armed by setMaster()
}

void DeviceRenderSink::applyTarget() {
    targetFill_ = baseTarget_ + margin_.extra();
    overflowFill_ = targetFill_ + overflowExtra_;
    const double devRate = ep_ ? ep_->sampleRate() : engineRate_;
    marginMs.store(static_cast<float>(margin_.extra() / devRate * 1000.0), std::memory_order_relaxed);
}

bool DeviceRenderSink::clockUsable() const {
    return master_ && ep_ && ep_->status() == EndpointStatus::Running;
}

bool DeviceRenderSink::clockNeedsData() const {
    if (!ep_) return false;
    const size_t fill = ep_->ring().size() / static_cast<size_t>(devCh_);
    return static_cast<double>(fill) < masterTarget_;
}

double DeviceRenderSink::latencySeconds() const {
    if (!ep_) return 0.0;
    const double devRate = ep_->sampleRate();
    FlowStamp st;
    const double fill = master_ ? masterTarget_
                                : targetFill_ + (ep_->flowStamp(st) ? 0.5 * std::max(1, ep_->periodFrames()) : 0.0);
    return ep_->deviceLatencySeconds() + fill / devRate + rs_.latencyInputFrames() / engineRate_;
}

void DeviceRenderSink::push(int frames) {
    meter.analyze(l_.data(), r_.data(), frames);
    if (!ep_ || ep_->status() != EndpointStatus::Running) {
        online.store(false, std::memory_order_relaxed);
        primed_ = false;
        return;
    }
    online.store(true, std::memory_order_relaxed);
    SpscRing<float>& ring = ep_->ring();
    const double devRate = ep_->sampleRate();
    double fill = static_cast<double>(ring.size() / static_cast<size_t>(devCh_));

    if (!primed_) {
        // Pre-roll silence so the device thread has a cushion from the start.
        rs_.reset();
        dc_.reset();
        // The master device fills its own buffer (about two periods) when it
        // starts; pre-roll that much silence so the start-up does not pull a
        // burst of extra blocks from the input devices.
        double want = master_ ? 2.0 * (ep_ ? ep_->periodFrames() : 480)
                              : targetFill_ - frames * (devRate / engineRate_);
        FlowStamp st;
        if (!master_ && ep_->flowStamp(st) && writtenTotal_ >= st.total) {
            // Start on the drift controller's aim (see push() below) so it
            // does not spend the first seconds correcting.
            const double since = std::max(0.0, ep_->flowClockNow() - st.time) * devRate;
            want += std::max(1, ep_->periodFrames()) - std::max(0.0, static_cast<double>(st.last) - since);
        }
        if (want > fill) {
            ring.writeZeros(static_cast<size_t>(want - fill) * devCh_);
            writtenTotal_ += static_cast<uint64_t>(want - fill);
        }
        primed_ = true;
        fill = static_cast<double>(ring.size() / static_cast<size_t>(devCh_));
    } else if (!master_ && fill < targetFill_ * 0.25) {
        // The device ran (nearly) dry: widen the margin and rebuild the
        // cushion in one step.
        margin_.onDropout();
        applyTarget();
        ring.writeZeros(static_cast<size_t>(targetFill_ * 0.5) * devCh_);
        writtenTotal_ += static_cast<uint64_t>(targetFill_ * 0.5);
        underruns.fetch_add(1, std::memory_order_relaxed);
        dc_.reset();
        fill = static_cast<double>(ring.size() / static_cast<size_t>(devCh_));
    } else if (!master_ && margin_.onCleanBlock()) {
        applyTarget();
    }
    fillMs.store(static_cast<float>(fill / devRate * 1000.0), std::memory_order_relaxed);

    if (!master_ && fill > overflowFill_) {
        // Device is consuming slower than we produce by a large margin
        // (stalled device). Drop this block rather than growing latency.
        overruns.fetch_add(1, std::memory_order_relaxed);
        dc_.reset();
        return;
    }

    double ratio = rs_.nominalRatio();
    if (!master_) {
        // Same idea as for capture: estimate the continuous level from the
        // device thread's newest read (the ring empties in device periods).
        double ctrlFill = fill;
        double ctrlTarget = targetFill_;
        FlowStamp st;
        if (ep_->flowStamp(st) && writtenTotal_ >= st.total) {
            const double since = std::max(0.0, ep_->flowClockNow() - st.time) * devRate;
            ctrlFill = static_cast<double>(writtenTotal_ - st.total) +
                       std::max(0.0, static_cast<double>(st.last) - since);
            ctrlTarget = targetFill_ + std::max(1, ep_->periodFrames()); // see DeviceCaptureSource::pull
        }
        dc_.setTargetFill(ctrlTarget);
        ratio = dc_.update(ctrlFill);
    }
    driftPpm.store(master_ ? 0.0f : static_cast<float>(dc_.correctionPpm()), std::memory_order_relaxed);

    float* st = stereo_.data();
    const float* L = l_.data();
    const float* R = r_.data();
    for (int i = 0; i < frames; ++i) {
        st[2 * i] = L[i];
        st[2 * i + 1] = R[i];
    }
    rs_.push(st, frames);
    const int maxOut = static_cast<int>(out_.size() / 2);
    const int n = rs_.pull(out_.data(), maxOut, ratio);

    const float* o = out_.data();
    float* d = dev_.data();
    if (devCh_ == 2) {
        std::memcpy(d, o, sizeof(float) * n * 2);
    } else if (devCh_ == 1) {
        for (int i = 0; i < n; ++i) d[i] = 0.5f * (o[2 * i] + o[2 * i + 1]);
    } else {
        for (int i = 0; i < n; ++i) {
            float* f = d + static_cast<size_t>(i) * devCh_;
            f[0] = o[2 * i];
            f[1] = o[2 * i + 1];
            for (int c = 2; c < devCh_; ++c) f[c] = 0.0f;
        }
    }
    const size_t want = static_cast<size_t>(n) * devCh_;
    const size_t wrote = ring.write(d, want);
    writtenTotal_ += wrote / static_cast<size_t>(devCh_);
    if (wrote < want) overruns.fetch_add(1, std::memory_order_relaxed);
}

// --------------------------------------------------------------------------

void MemorySink::prepare(double engineRate, int maxBlock, double blockSeconds) {
    SinkRuntime::prepare(engineRate, maxBlock, blockSeconds);
    outL_.assign(capacity_, 0.0f);
    outR_.assign(capacity_, 0.0f);
    written_ = 0;
    online.store(true);
}

void MemorySink::push(int frames) {
    meter.analyze(l_.data(), r_.data(), frames);
    const size_t n = std::min(static_cast<size_t>(frames), capacity_ - written_);
    std::memcpy(outL_.data() + written_, l_.data(), sizeof(float) * n);
    std::memcpy(outR_.data() + written_, r_.data(), sizeof(float) * n);
    written_ += n;
    if (n < static_cast<size_t>(frames)) overruns.fetch_add(1, std::memory_order_relaxed);
}

} // namespace echo
