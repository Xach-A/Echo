#include "ai/AiVoiceEffect.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <chrono>
#include <cstring>
#include <iterator>
#include <limits>

#include "core/Log.h"
#include "dsp/EffectRegistry.h"
#include "session/Paths.h"

namespace echo {

namespace {
const ParamInfo kAiParams[] = {
    {"pitch", "Pitch", -24.0f, 24.0f, 0.0f, "st", ParamScale::Linear, nullptr,
     "Pitch shift passed to the model (only if the model supports it)"},
    {"mix", "Mix", 0.0f, 100.0f, 100.0f, "%", ParamScale::Linear},
    {"latency", "Buffer", 100.0f, 2000.0f, 400.0f, "ms", ParamScale::Linear, nullptr,
     "Fixed delay that gives inference time to finish. Raise it if you hear dropouts."},
};
ECHO_EFFECT_INFO(kAiInfo, "ai_voice", "AI Voice Conversion", "AI",
                 "Runs a user-installed ONNX voice model asynchronously (off the audio thread).", kAiParams, false);
} // namespace

void registerAiEffects(EffectRegistry& r) { registerEffect<ai::AiVoiceEffect>(r, kAiInfo); }

namespace ai {


AiVoiceEffect::AiVoiceEffect() : Effect(kAiInfo) {
    inRing_.allocate(48000 * 8);
    outRing_.allocate(48000 * 8);
}

AiVoiceEffect::~AiVoiceEffect() {
    stop_ = true;
    if (worker_.joinable()) worker_.join();
}

const char* AiVoiceEffect::visLabel(int s) const {
    switch (s) {
    case 0: return "Status";
    default: return nullptr;
    }
}

void AiVoiceEffect::setModel(const ModelManifest& m, bool preferGpu) {
    {
        std::lock_guard<std::mutex> lock(modelMutex_);
        pending_ = m;
        pendingGpu_ = preferGpu;
        modelName_ = m.name;
        modelId_ = m.id;
    }
    setStatus(AiStatus::Loading, "Loading " + m.name + "...");
    modelDirty_ = true;
    if (!worker_.joinable()) worker_ = std::thread(&AiVoiceEffect::workerMain, this);
}

void AiVoiceEffect::clearModel() {
    {
        std::lock_guard<std::mutex> lock(modelMutex_);
        pending_ = ModelManifest{};
        modelName_.clear();
        modelId_.clear();
    }
    modelDirty_ = true;
    if (!worker_.joinable()) setStatus(AiStatus::NoModel, "No model selected");
}

std::string AiVoiceEffect::saveState() const {
    std::lock_guard<std::mutex> lock(modelMutex_);
    if (modelId_.empty()) return {};
    return modelId_ + (pendingGpu_ ? "|gpu" : "");
}

void AiVoiceEffect::loadState(const std::string& state) {
    const auto bar = state.find('|');
    restoreId_ = state.substr(0, bar);
    restoreGpu_ = bar != std::string::npos && state.substr(bar + 1) == "gpu";
    std::lock_guard<std::mutex> lock(modelMutex_);
    modelId_ = restoreId_;
    pendingGpu_ = restoreGpu_;
}

std::string AiVoiceEffect::modelName() const {
    std::lock_guard<std::mutex> lock(modelMutex_);
    return modelName_;
}

std::string AiVoiceEffect::statusText() const {
    std::lock_guard<std::mutex> lock(modelMutex_);
    return statusText_;
}

std::string AiVoiceEffect::providerName() const {
    std::lock_guard<std::mutex> lock(modelMutex_);
    return provider_;
}

void AiVoiceEffect::setStatus(AiStatus s, const std::string& text) {
    {
        std::lock_guard<std::mutex> lock(modelMutex_);
        statusText_ = text;
    }
    status_ = static_cast<int>(s);
    vis[0].store(static_cast<float>(s), std::memory_order_relaxed);
}

int AiVoiceEffect::latencySamples() const { return active_.load() ? latency_ : 0; }

void AiVoiceEffect::onPrepare() {
    maxDryDelay_ = static_cast<int>(sr_ * 2.5);
    for (int c = 0; c < 2; ++c) {
        dryDelay_[c].prepare(maxDryDelay_, maxBlock_);
        dry_[c].assign(maxBlock_, 0.0f);
    }
    mono_.assign(static_cast<size_t>(maxBlock_) * 2, 0.0f);
    // Re-select the model saved with the session (the user chose it before).
    if (!restoreId_.empty()) {
        const std::string id = restoreId_;
        restoreId_.clear();
        for (const auto& m : scanModels(paths::modelsDir()))
            if (m.id == id && m.valid) {
                setModel(m, restoreGpu_);
                return;
            }
        setStatus(AiStatus::Error, "Saved model '" + id + "' is no longer installed");
    }
}

void AiVoiceEffect::onReset() {
    syncRequest_.fetch_add(1);
    inCount_ = 0;
    readIndex_ = 0;
    wetGain_ = 0.0f;
    outRing_.skip(outRing_.readAvailable());
    for (auto& d : dryDelay_) d.reset();
}

void AiVoiceEffect::processImpl(float* const* ch, int numCh, int n) {
    const bool active = active_.load(std::memory_order_acquire);
    if (!active) {
        lastActive_ = false;
        return; // no model: transparent, no added latency
    }
    const int wantLatency = std::min(std::max(static_cast<int>(p(2) * 0.001 * sr_), minLatency_.load()),
                                     maxDryDelay_ - 1);
    if (!lastActive_ || wantLatency != latency_) {
        // (Re)start the stream alignment (no allocation here).
        latency_ = wantLatency;
        latencyPub_.store(latency_, std::memory_order_relaxed);
        for (auto& d : dryDelay_) d.setDelay(latency_, maxBlock_);
        onReset();
        lastActive_ = true;
    }
    for (int c = 0; c < numCh; ++c) dryDelay_[c].process(ch[c], dry_[c].data(), n);
    const bool synced = syncAck_.load(std::memory_order_acquire) == syncRequest_.load(std::memory_order_relaxed);
    float* mono = mono_.data();
    float* wet = mono_.data() + maxBlock_;
    if (synced) {
        for (int i = 0; i < n; ++i) {
            float m = 0.0f;
            for (int c = 0; c < numCh; ++c) m += ch[c][i];
            mono[i] = m / static_cast<float>(numCh);
        }
        if (inRing_.writeAvailable() >= static_cast<size_t>(n)) inRing_.write(mono, n);
    }
    // Wet samples needed for this block: stream indices [w0, w0 + n).
    const int offset = wetOffset_.load(std::memory_order_relaxed);
    const int64_t w0 = static_cast<int64_t>(inCount_) - latency_ + offset;
    if (synced) inCount_ += static_cast<uint64_t>(n);
    int have = 0, firstWet = 0;
    if (synced && w0 + n > 0) {
        const int64_t start = std::max<int64_t>(w0, 0);
        firstWet = static_cast<int>(start - w0);
        size_t avail = outRing_.readAvailable();
        if (static_cast<int64_t>(readIndex_) < start) {
            const size_t skip = std::min<size_t>(avail, static_cast<size_t>(start - static_cast<int64_t>(readIndex_)));
            outRing_.skip(skip);
            readIndex_ += skip;
            avail -= skip;
        }
        if (static_cast<int64_t>(readIndex_) == start) {
            have = static_cast<int>(std::min<size_t>(avail, static_cast<size_t>(n - firstWet)));
            outRing_.read(wet + firstWet, static_cast<size_t>(have));
            readIndex_ += static_cast<uint64_t>(have);
        }
        const int missing = (n - firstWet) - have;
        if (missing > 0) underrunSamples.fetch_add(static_cast<uint64_t>(missing), std::memory_order_relaxed);
    }
    const float mix = p(1) / 100.0f;
    const float step = 1.0f / static_cast<float>(sr_ * 0.01);
    int skipped = 0;
    for (int i = 0; i < n; ++i) {
        // NaN marks audio the overloaded worker skipped: fall back to dry.
        bool haveWet = i >= firstWet && i < firstWet + have;
        if (haveWet && !(wet[i] == wet[i])) {
            haveWet = false;
            ++skipped;
        }
        const float target = haveWet ? mix : 0.0f;
        wetGain_ = target > wetGain_ ? std::min(target, wetGain_ + step) : std::max(target, wetGain_ - step);
        const float w = haveWet ? wet[i] : 0.0f;
        for (int c = 0; c < numCh; ++c) ch[c][i] = dry_[c][i] * (1.0f - wetGain_) + w * wetGain_;
    }
    if (skipped > 0) underrunSamples.fetch_add(static_cast<uint64_t>(skipped), std::memory_order_relaxed);
}

void AiVoiceEffect::workerMain() {
    std::unique_ptr<OnnxVoiceModel> model;
    ModelManifest m;
    Resampler down, up;
    double ratioDown = 1.0, ratioUp = 1.0;
    int chunkEng = 0, ctxModel = 0, xfModel = 0;
    std::vector<float> chunkIn, history, modelIn, modelOut, emitted, tail, tmp, outEng;
    bool haveTail = false;
    uint64_t ack = syncAck_.load();
    using clock = std::chrono::steady_clock;
    float avgMs = 0.0f;

    auto resetStream = [&]() {
        inRing_.skip(inRing_.readAvailable());
        down.reset();
        up.reset();
        std::fill(history.begin(), history.end(), 0.0f);
        haveTail = false;
    };

    while (!stop_.load()) {
        if (modelDirty_.exchange(false)) {
            active_ = false;
            model.reset();
            ModelManifest next;
            bool gpu = false;
            {
                std::lock_guard<std::mutex> lock(modelMutex_);
                next = pending_;
                gpu = pendingGpu_;
            }
            if (!next.valid) {
                setStatus(AiStatus::NoModel, next.error.empty() ? "No model selected" : next.error);
            } else {
                setStatus(AiStatus::Loading, "Loading " + next.name + "...");
                auto mdl = std::make_unique<OnnxVoiceModel>();
                std::string err;
                if (!mdl->load(next, gpu, &err)) {
                    setStatus(AiStatus::Error, "Could not load model: " + err);
                    ECHO_LOG_WARN("AI model '%s' failed to load: %s", next.name.c_str(), err.c_str());
                } else {
                    m = next;
                    model = std::move(mdl);
                    const double eng = sr_;
                    ratioDown = eng / m.sampleRate;
                    ratioUp = m.sampleRate / eng;
                    chunkEng = static_cast<int>(eng * m.chunkMs / 1000.0);
                    ctxModel = m.sampleRate * m.contextMs / 1000;
                    xfModel = m.sampleRate * m.crossfadeMs / 1000;
                    down.prepare(1, ratioDown, chunkEng + 64, ResamplerQuality::High);
                    up.prepare(1, ratioUp, m.sampleRate * m.chunkMs / 1000 + 64, ResamplerQuality::High);
                    const int chunkModel = static_cast<int>(m.sampleRate * m.chunkMs / 1000.0);
                    history.assign(static_cast<size_t>(ctxModel + chunkModel + 8), 0.0f);
                    tail.assign(static_cast<size_t>(xfModel), 0.0f);
                    // Fixed offset of the wet stream: crossfade lead plus
                    // both resamplers' group delays (engine samples).
                    const double off = xfModel * eng / m.sampleRate + down.latencyInputFrames() +
                                       up.latencyInputFrames() * eng / m.sampleRate;
                    wetOffset_ = -static_cast<int>(std::lround(off));
                    minLatency_ = static_cast<int>(chunkEng * 1.25 + eng * 0.06 + off);
                    {
                        std::lock_guard<std::mutex> lock(modelMutex_);
                        provider_ = model->provider();
                    }
                    resetStream();
                    setStatus(AiStatus::Running, "Running " + m.name + " on " + model->provider());
                    active_ = true;
                }
            }
        }
        if (!model) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            continue;
        }
        const uint64_t req = syncRequest_.load(std::memory_order_acquire);
        if (req != ack) {
            resetStream();
            ack = req;
            syncAck_.store(ack, std::memory_order_release);
        }
        bool worked = false;
        while (!stop_.load() && !modelDirty_.load() && inRing_.readAvailable() >= static_cast<size_t>(chunkEng) &&
               syncRequest_.load() == ack) {
            // Overload: input older than the latency budget can no longer be
            // played in time (the audio thread would discard the result), so
            // skip to the newest chunk instead of working through a stale
            // backlog. The skipped stretch is marked (NaN) so the audio
            // thread falls back to the dry voice there and counts it.
            const size_t backlog = inRing_.readAvailable();
            const size_t budget = static_cast<size_t>(chunkEng) + static_cast<size_t>(std::max(0, latencyPub_.load()));
            if (backlog > budget) {
                const size_t skip = backlog - static_cast<size_t>(chunkEng);
                inRing_.skip(skip);
                size_t left = skip;
                const float marker = std::numeric_limits<float>::quiet_NaN();
                float marks[256];
                std::fill(std::begin(marks), std::end(marks), marker);
                while (left > 0 && outRing_.writeAvailable() > 0) {
                    const size_t k = std::min<size_t>(left, 256);
                    outRing_.write(marks, k);
                    left -= k;
                }
                // The stream restarts after the gap.
                down.reset();
                up.reset();
                std::fill(history.begin(), history.end(), 0.0f);
                haveTail = false;
                skippedSamples.fetch_add(skip);
            }
            chunkIn.resize(chunkEng);
            inRing_.read(chunkIn.data(), chunkEng);
            down.push(chunkIn.data(), chunkEng);
            const int got = down.outputAvailable(ratioDown);
            tmp.resize(got);
            down.pull(tmp.data(), got, ratioDown);
            // Sliding history: [context | new chunk]
            const size_t keep = static_cast<size_t>(ctxModel);
            if (history.size() < keep + tmp.size()) history.resize(keep + tmp.size());
            std::vector<float> ctx(history.end() - static_cast<long>(keep), history.end());
            modelIn.assign(ctx.begin(), ctx.end());
            modelIn.insert(modelIn.end(), tmp.begin(), tmp.end());
            history = modelIn;
            const auto t0 = clock::now();
            std::string err;
            if (!model->run(modelIn.data(), static_cast<int>(modelIn.size()), p(0), modelOut, &err)) {
                setStatus(AiStatus::Error, "Inference failed: " + err);
                ECHO_LOG_WARN("AI inference failed: %s", err.c_str());
                active_ = false;
                model.reset();
                break;
            }
            if (const int extra = testDelayMs_.load(); extra > 0)
                std::this_thread::sleep_for(std::chrono::milliseconds(extra));
            const double ms = std::chrono::duration<double, std::milli>(clock::now() - t0).count();
            avgMs = chunksProcessed.load() == 0 ? static_cast<float>(ms) : avgMs * 0.9f + static_cast<float>(ms) * 0.1f;
            inferenceMs = avgMs;
            realtimeFactor = static_cast<float>(ms / m.chunkMs);
            // Emit the chunk region shifted back by the crossfade length, and
            // blend its first samples with the previous call's tail.
            const int L = static_cast<int>(modelOut.size());
            const int chunk = static_cast<int>(tmp.size());
            emitted.assign(chunk, 0.0f);
            for (int k = 0; k < chunk; ++k) {
                const int idx = L - chunk - xfModel + k;
                float v = (idx >= 0 && idx < L) ? modelOut[idx] : 0.0f;
                if (!std::isfinite(v)) v = 0.0f;
                if (k < xfModel && haveTail) {
                    const float a = (k + 0.5f) / static_cast<float>(xfModel);
                    v = tail[k] * (1.0f - a) + v * a;
                }
                emitted[k] = v;
            }
            for (int k = 0; k < xfModel; ++k) {
                const int idx = L - xfModel + k;
                tail[k] = (idx >= 0 && idx < L) ? modelOut[idx] : 0.0f;
            }
            haveTail = xfModel > 0;
            up.push(emitted.data(), chunk);
            const int outN = up.outputAvailable(ratioUp);
            outEng.resize(outN);
            up.pull(outEng.data(), outN, ratioUp);
            if (outRing_.writeAvailable() >= static_cast<size_t>(outN)) outRing_.write(outEng.data(), outN);
            chunksProcessed.fetch_add(1);
            worked = true;
        }
        if (!worked) std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
}

} // namespace ai
} // namespace echo
