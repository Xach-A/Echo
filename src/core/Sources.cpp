#include "core/Sources.h"

#include <cmath>
#include <cstring>

namespace echo {

void SourceRuntime::prepare(double engineRate, int maxBlock, double blockSeconds) {
    engineRate_ = engineRate;
    maxBlock_ = maxBlock;
    blockSeconds_ = blockSeconds;
    buf_.assign(static_cast<size_t>(std::max(1, channels_)) * maxBlock, 0.0f);
}

void SourceRuntime::zero(int frames) {
    for (int c = 0; c < channels_; ++c) std::memset(channelMut(c), 0, sizeof(float) * frames);
}

// --------------------------------------------------------------------------

DeviceCaptureSource::DeviceCaptureSource(std::shared_ptr<CaptureEndpoint> endpoint,
                                         ResamplerQuality quality)
    : ep_(std::move(endpoint)), quality_(quality) {
    devCh_ = ep_ ? std::max(1, ep_->channels()) : 1;
    keepCh_ = std::min(devCh_, kMaxSourceChannels);
    channels_ = keepCh_;
    if (ep_) label = ep_->describe();
}

void DeviceCaptureSource::prepare(double engineRate, int maxBlock, double blockSeconds) {
    SourceRuntime::prepare(engineRate, maxBlock, blockSeconds);
    const double devRate = ep_ ? ep_->sampleRate() : engineRate;
    const double ratio = devRate / engineRate;
    const int maxIn = static_cast<int>(maxBlock * ratio * 1.02) + 256;
    rs_.prepare(keepCh_, ratio, maxIn, quality_);
    in_.assign(static_cast<size_t>(maxIn) * devCh_, 0.0f);
    out_.assign(static_cast<size_t>(maxBlock) * keepCh_, 0.0f);

    const double period = ep_ ? std::max(1, ep_->periodFrames()) : 480;
    const double blockDev = maxBlock * ratio;
    baseTarget_ = period + blockDev + devRate * 0.003;
    overflowExtra_ = std::max(4.0 * blockDev, devRate * 0.12);
    // After a dropout: +1 device period (up to 2 periods); relaxes one
    // step per 30 minutes without dropouts.
    margin_.prepare(period, 2.0 * period, 1800.0 / std::max(1e-3, blockSeconds));
    applyTarget();
    dc_.prepare(ratio, targetFill_, devRate, blockSeconds);
    primed_ = false;
}

void DeviceCaptureSource::applyTarget() {
    targetFill_ = baseTarget_ + margin_.extra();
    overflowFill_ = targetFill_ + overflowExtra_;
    const double devRate = ep_ ? ep_->sampleRate() : engineRate_;
    marginMs.store(static_cast<float>(margin_.extra() / devRate * 1000.0), std::memory_order_relaxed);
}

double DeviceCaptureSource::latencySeconds() const {
    if (!ep_) return 0.0;
    const double devRate = ep_->sampleRate();
    // With packet timestamps the ring holds targetFill_ .. +1 packet (see pull()).
    FlowStamp st;
    const double avgFill = targetFill_ + (ep_->flowStamp(st) ? 0.5 * std::max(1, ep_->periodFrames()) : 0.0);
    return ep_->deviceLatencySeconds() + avgFill / devRate + rs_.latencyInputFrames() / devRate;
}

void DeviceCaptureSource::pull(int frames) {
    if (!ep_ || ep_->status() != EndpointStatus::Running) {
        zero(frames);
        primed_ = false;
        state.store(static_cast<int>(SourceState::Offline), std::memory_order_relaxed);
        return;
    }
    SpscRing<float>& ring = ep_->ring();
    const bool sparse = ep_->isSparse();
    const double devRate = ep_->sampleRate();
    size_t fill = ring.readAvailable() / static_cast<size_t>(devCh_);
    fillMs.store(static_cast<float>(fill / devRate * 1000.0), std::memory_order_relaxed);

    // Drift control works on a sawtooth-free estimate of the buffer level:
    // the ring level at the device thread's newest packet plus what the
    // device has captured since (capped at one packet). Without packet
    // timestamps it falls back to the ring level at block time.
    FlowStamp st;
    const bool stamped = ep_->flowStamp(st) && st.total >= readTotal_;
    const double period = std::max(1, ep_->periodFrames());
    const double ahead =
        stamped ? std::min(static_cast<double>(st.last), std::max(0.0, ep_->flowClockNow() - st.time) * devRate) : 0.0;
    // The estimate runs 0..1 packet above the ring level. Aiming one packet
    // above targetFill_ keeps at least targetFill_ in the ring when the
    // engine reads, whatever the packet/block phase (on average half a
    // packet more buffering than controlling on the ring level; measured on
    // real devices this removed most dropouts). `level` = ring level that
    // puts the estimate on its aim (used when (re)starting).
    const double ctrlTarget = stamped ? targetFill_ + period : targetFill_;
    const double level = stamped ? ctrlTarget - ahead : targetFill_;

    if (!primed_) {
        if (fill >= static_cast<size_t>(level)) {
            primed_ = true;
            rs_.reset();
            dc_.reset();
            const size_t excess = fill - static_cast<size_t>(level);
            if (excess > 0) {
                ring.skip(excess * devCh_);
                readTotal_ += excess;
                fill -= excess;
            }
        } else {
            zero(frames);
            state.store(static_cast<int>(sparse ? SourceState::Idle : SourceState::Priming),
                        std::memory_order_relaxed);
            return;
        }
    }

    if (fill > static_cast<size_t>(overflowFill_)) {
        // Backlog (e.g. the engine was descheduled): drop back to the target
        // instead of letting latency grow.
        const size_t drop = fill - static_cast<size_t>(level);
        ring.skip(drop * devCh_);
        readTotal_ += drop;
        fill -= drop;
        overruns.fetch_add(1, std::memory_order_relaxed);
        dc_.reset();
    }

    // (The ring level at block time is a sawtooth whose phase wanders with
    // the clock difference; controlling on it turns phase into false drift.)
    double ctrlFill = static_cast<double>(fill);
    if (stamped && st.total >= readTotal_) ctrlFill = static_cast<double>(st.total - readTotal_) + ahead;
    dc_.setTargetFill(stamped && st.total >= readTotal_ ? ctrlTarget : targetFill_);
    const double ratio = dc_.update(ctrlFill);
    int need = rs_.inputNeeded(frames, ratio);
    const int maxIn = static_cast<int>(in_.size() / devCh_);
    need = std::min(need, maxIn);

    int got = need;
    if (static_cast<size_t>(need) > fill) {
        got = static_cast<int>(fill);
        if (!sparse) {
            underruns.fetch_add(1, std::memory_order_relaxed);
            margin_.onDropout();
            applyTarget();
        } else if (stamped && ep_->flowClockNow() - st.time < 0.05) {
            // A loopback/app capture normally runs dry only when nothing
            // plays. Packets arrived moments ago, so this is a gap in audio
            // that is playing: count it and widen the margin like a dropout.
            gaps.fetch_add(1, std::memory_order_relaxed);
            margin_.onDropout();
            applyTarget();
        }
        primed_ = false; // re-prime to rebuild the safety margin
    } else if (margin_.onCleanBlock()) {
        applyTarget();
    }
    if (got > 0) {
        ring.read(in_.data(), static_cast<size_t>(got) * devCh_);
        readTotal_ += static_cast<uint64_t>(got);
        if (keepCh_ != devCh_) {
            for (int f = 0; f < got; ++f)
                for (int c = 0; c < keepCh_; ++c)
                    in_[static_cast<size_t>(f) * keepCh_ + c] = in_[static_cast<size_t>(f) * devCh_ + c];
        }
        rs_.push(in_.data(), got);
    }
    if (need > got) rs_.pushSilence(need - got);

    const int produced = rs_.pull(out_.data(), frames, ratio);
    for (int c = 0; c < keepCh_; ++c) {
        float* dst = channelMut(c);
        for (int f = 0; f < produced; ++f) dst[f] = out_[static_cast<size_t>(f) * keepCh_ + c];
        if (produced < frames) std::memset(dst + produced, 0, sizeof(float) * (frames - produced));
    }
    meter.analyze(channel(0), channel(keepCh_ > 1 ? 1 : 0), frames);
    driftPpm.store(static_cast<float>(dc_.correctionPpm()), std::memory_order_relaxed);
    state.store(static_cast<int>(SourceState::Running), std::memory_order_relaxed);
}

// --------------------------------------------------------------------------

const char* const ToneSource::kWaveNames[] = {"Sine", "Square", "White noise", "Pink noise",
                                              "Log sweep", "Impulse train", "Chirp train", nullptr};

float ToneSource::chirpSample(int i, double sampleRate) {
    if (i < 0 || i >= kChirpLength) return 0.0f;
    const double t = i / sampleRate;
    const double dur = kChirpLength / sampleRate;
    const double f0 = 500.0, f1 = 8000.0;
    const double ph = 2.0 * kPiD * (f0 * t + 0.5 * (f1 - f0) / dur * t * t);
    const double w = 0.5 - 0.5 * std::cos(2.0 * kPiD * i / (kChirpLength - 1));
    return static_cast<float>(std::sin(ph) * w);
}

ToneSource::ToneSource() {
    channels_ = 2;
    label = "Test signal generator";
}

void ToneSource::prepare(double engineRate, int maxBlock, double blockSeconds) {
    channels_ = 2;
    SourceRuntime::prepare(engineRate, maxBlock, blockSeconds);
    phase_ = 0.0;
    sweepPhase_ = 0.0;
    frameCounter_ = 0;
    nextImpulse_ = 0;
}

void ToneSource::pull(int frames) {
    float* out = channelMut(0);
    if (!enabled.load(std::memory_order_relaxed)) {
        zero(frames);
        frameCounter_ += frames;
        framesGenerated.store(frameCounter_, std::memory_order_relaxed);
        state.store(static_cast<int>(SourceState::Idle), std::memory_order_relaxed);
        return;
    }
    const float gain = dbToGain(levelDb.load(std::memory_order_relaxed));
    const int w = wave.load(std::memory_order_relaxed);
    const double f = clampv(static_cast<double>(frequency.load(std::memory_order_relaxed)), 1.0,
                            engineRate_ * 0.45);
    const double inc = f / engineRate_;

    auto white = [this]() {
        rng_ ^= rng_ << 13;
        rng_ ^= rng_ >> 17;
        rng_ ^= rng_ << 5;
        return static_cast<float>(static_cast<int32_t>(rng_)) * (1.0f / 2147483648.0f);
    };

    for (int i = 0; i < frames; ++i) {
        float s = 0.0f;
        switch (w) {
        case Sine:
            s = static_cast<float>(std::sin(2.0 * kPiD * phase_));
            phase_ += inc;
            if (phase_ >= 1.0) phase_ -= 1.0;
            break;
        case Square:
            s = phase_ < 0.5 ? 1.0f : -1.0f;
            phase_ += inc;
            if (phase_ >= 1.0) phase_ -= 1.0;
            break;
        case WhiteNoise: s = white(); break;
        case PinkNoise: {
            // Paul Kellet's refined pink filter (public-domain coefficients).
            const float wv = white();
            pink_[0] = 0.99886f * pink_[0] + wv * 0.0555179f;
            pink_[1] = 0.99332f * pink_[1] + wv * 0.0750759f;
            pink_[2] = 0.96900f * pink_[2] + wv * 0.1538520f;
            pink_[3] = 0.86650f * pink_[3] + wv * 0.3104856f;
            pink_[4] = 0.55000f * pink_[4] + wv * 0.5329522f;
            pink_[5] = -0.7616f * pink_[5] - wv * 0.0168980f;
            s = (pink_[0] + pink_[1] + pink_[2] + pink_[3] + pink_[4] + pink_[5] + pink_[6] +
                 wv * 0.5362f) * 0.11f;
            pink_[6] = wv * 0.115926f;
            break;
        }
        case Sweep: {
            // Exponential 20 Hz -> 20 kHz sweep, 10 s period.
            const double t = static_cast<double>(frameCounter_ % static_cast<uint64_t>(engineRate_ * 10.0)) /
                             engineRate_;
            const double fr = 20.0 * std::pow(1000.0, t / 10.0);
            sweepPhase_ += fr / engineRate_;
            if (sweepPhase_ >= 1.0) sweepPhase_ -= std::floor(sweepPhase_);
            s = static_cast<float>(std::sin(2.0 * kPiD * sweepPhase_));
            break;
        }
        case ChirpTrain: {
            if (frameCounter_ >= nextImpulse_) {
                lastImpulseFrame.store(frameCounter_, std::memory_order_relaxed);
                const double period = std::max(0.1f, impulsePeriodSec.load(std::memory_order_relaxed));
                nextImpulse_ = frameCounter_ + static_cast<uint64_t>(period * engineRate_);
            }
            const uint64_t start = lastImpulseFrame.load(std::memory_order_relaxed);
            if (frameCounter_ >= start)
                s = chirpSample(static_cast<int>(std::min<uint64_t>(frameCounter_ - start, kChirpLength)), engineRate_);
            break;
        }
        case ImpulseTrain: {
            if (frameCounter_ >= nextImpulse_) {
                s = 1.0f;
                lastImpulseFrame.store(frameCounter_, std::memory_order_relaxed);
                const double period = std::max(0.05f, impulsePeriodSec.load(std::memory_order_relaxed));
                nextImpulse_ = frameCounter_ + static_cast<uint64_t>(period * engineRate_);
            }
            break;
        }
        default: break;
        }
        out[i] = s * gain;
        ++frameCounter_;
    }
    std::memcpy(channelMut(1), out, sizeof(float) * frames);
    framesGenerated.store(frameCounter_, std::memory_order_relaxed);
    meter.analyze(out, out, frames);
    state.store(static_cast<int>(SourceState::Running), std::memory_order_relaxed);
}

} // namespace echo
