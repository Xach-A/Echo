#include "ai/AiVoiceEffect.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <iterator>
#include <limits>

#include "ai/RvcVoice.h"
#include "core/Log.h"
#include "dsp/EffectRegistry.h"
#include "session/Paths.h"

namespace echo {

namespace {
const char* const kQualityNames[] = {"Fastest", "Fast", "Balanced", "Smooth", "Smoothest", nullptr};
const ParamInfo kAiParams[] = {
    {"pitch", "Pitch", -24.0f, 24.0f, 0.0f, "st", ParamScale::Linear, nullptr,
     "Pitch shift passed to the model (only if the model supports it)"},
    {"mix", "Mix", 0.0f, 100.0f, 100.0f, "%", ParamScale::Linear},
    {"latency", "Buffer", 100.0f, 2000.0f, 100.0f, "ms", ParamScale::Linear, nullptr,
     "Least delay. The delay is never shorter than the voice needs (see Quality). Raise it if you hear dropouts."},
    {"index", "Voice match", 0.0f, 100.0f, 50.0f, "%", ParamScale::Linear, nullptr,
     "RVC voices with an index: how strongly the index pulls your speech towards the voice's own sound"},
    // Saved as "response" (its name before Echo 0.1.2).
    {"response", "Quality", 0.0f, 4.0f, 2.0f, "", ParamScale::Enum, kQualityNames,
     "RVC voices: faster is heard sooner, smoother sounds cleaner but a little later"},
};
constexpr int kQualitySteps = 5;
constexpr int kSearchMs = 10; // how far a new piece may shift to line up with the last
constexpr int kDecodeMarginMs = 60; // made before the piece too, so it isn't at the vocoder's edge
ECHO_EFFECT_INFO(kAiInfo, "ai_voice", "AI Voice Conversion", "AI",
                 "Runs a voice model you added (RVC or ONNX) off the audio thread.", kAiParams, false);
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

void AiVoiceEffect::setModel(const ModelManifest& m, bool preferGpu) { select(m, preferGpu, false); }

void AiVoiceEffect::select(const ModelManifest& m, bool preferGpu, bool gpuAuto) {
    {
        std::lock_guard<std::mutex> lock(modelMutex_);
        pending_ = m;
        pendingGpu_ = preferGpu;
        gpuAuto_ = gpuAuto;
        modelName_ = m.name;
        modelKind_ = m.kind;
        modelId_ = m.id;
    }
    setStatus(AiStatus::Loading, "Loading " + m.name + "...");
    modelDirty_ = true;
    if (!worker_.joinable()) worker_ = std::thread(&AiVoiceEffect::workerMain, this);
}

void AiVoiceEffect::setPreferGpu(bool gpu) {
    bool reload = false;
    {
        std::lock_guard<std::mutex> lock(modelMutex_);
        if (pendingGpu_ == gpu && !gpuAuto_) return;
        pendingGpu_ = gpu;
        gpuAuto_ = false;
        reload = pending_.valid;
    }
    if (reload) {
        setStatus(AiStatus::Loading, "Loading " + modelName() + "...");
        modelDirty_ = true;
    }
}

bool AiVoiceEffect::prefersGpu() const {
    std::lock_guard<std::mutex> lock(modelMutex_);
    return pendingGpu_;
}

void AiVoiceEffect::clearModel() {
    {
        std::lock_guard<std::mutex> lock(modelMutex_);
        pending_ = ModelManifest{};
        modelName_.clear();
        modelKind_.clear();
        modelId_.clear();
    }
    modelDirty_ = true;
    if (!worker_.joinable()) setStatus(AiStatus::NoModel, "No model selected");
}

std::string AiVoiceEffect::saveState() const {
    std::lock_guard<std::mutex> lock(modelMutex_);
    if (modelId_.empty()) return {};
    if (gpuAuto_) return modelId_;
    return modelId_ + (pendingGpu_ ? "|gpu" : "|cpu");
}

void AiVoiceEffect::loadState(const std::string& state) {
    const auto bar = state.find('|');
    restoreId_ = state.substr(0, bar);
    restoreGpu_ = bar != std::string::npos && state.substr(bar + 1) == "gpu";
    restoreGpuAuto_ = bar == std::string::npos;
    std::lock_guard<std::mutex> lock(modelMutex_);
    modelId_ = restoreId_;
    pendingGpu_ = restoreGpu_;
    gpuAuto_ = restoreGpuAuto_;
}

std::string AiVoiceEffect::modelName() const {
    std::lock_guard<std::mutex> lock(modelMutex_);
    return modelName_;
}

std::string AiVoiceEffect::modelKind() const {
    std::lock_guard<std::mutex> lock(modelMutex_);
    return modelKind_;
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
                select(m, restoreGpu_, restoreGpuAuto_);
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

int solaOffset(const std::vector<float>& wav, const std::vector<float>& tail, int search) {
    const int n = static_cast<int>(tail.size()), mid = search / 2;
    double eTail = 0.0;
    for (float v : tail) eTail += static_cast<double>(v) * v;
    if (eTail < 1e-9) return mid;
    int bestOff = mid;
    double best = -1e30;
    for (int o = 0; o <= search; ++o) {
        double num = 0.0, e = 0.0;
        for (int k = 0; k < n; ++k) {
            const double w = wav[static_cast<size_t>(o + k)];
            num += w * tail[static_cast<size_t>(k)];
            e += w * w;
        }
        const double score = num / std::sqrt(e * eTail + 1e-12) - 0.05 * std::abs(o - mid) / std::max(1, mid);
        if (score > best) best = score, bestOff = o;
    }
    return bestOff;
}

namespace {

// Time one run of a loaded voice on a voice-like buzz (silence would skip the
// pitch tracking), after a warm-up run: graphics cards are slow the first time.
double timeRuns(VoiceConverter& v, int samples, int sampleRate) {
    std::vector<float> in(static_cast<size_t>(samples), 0.0f), out;
    for (size_t i = 0; i < in.size(); ++i)
        for (int h = 1; h <= 8; ++h)
            in[i] += 0.1f / h * static_cast<float>(std::sin(2.0 * 3.14159265358979 * 140.0 * h * static_cast<double>(i) / sampleRate));
    std::string err;
    v.run(in.data(), samples, 0.0f, out, &err);
    double total = 0.0;
    for (int i = 0; i < 2; ++i) {
        const auto t0 = std::chrono::steady_clock::now();
        v.run(in.data(), samples, 0.0f, out, &err);
        total += std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    }
    return total / 2.0;
}

} // namespace

StreamPlan rvcStreamPlan(int quality) {
    // chunk, context, crossfade, lookahead. Measured (docs/AI_VOICE.md): 2 s
    // of context sounded best, also right after a pause (longer is worse
    // after one); shorter pieces cost little with it; lookahead a little.
    static const StreamPlan kPlans[kQualitySteps] = {
        {80, 2000, 20, 0}, {100, 2000, 30, 0}, {120, 2000, 40, 20}, {150, 2000, 40, 60}, {200, 2000, 50, 100}};
    return kPlans[std::clamp(quality, 0, kQualitySteps - 1)];
}

void AiVoiceEffect::workerMain() {
    std::unique_ptr<VoiceConverter> model;
    ModelManifest m;
    Resampler down, up;
    double ratioDown = 1.0, ratioUp = 1.0;
    int chunkEng = 0, ctxModel = 0, xfModel = 0, searchModel = 0, laModel = 0;
    std::vector<float> chunkIn, history, modelIn, modelOut, wav, emitted, tail, fadeIn, tmp, outEng;
    bool haveTail = false;
    bool continuous = false; // the next block follows on from the last
    uint64_t ack = syncAck_.load();
    using clock = std::chrono::steady_clock;
    float avgMs = 0.0f;

    auto resetStream = [&]() {
        inRing_.skip(inRing_.readAvailable());
        down.reset();
        up.reset();
        std::fill(history.begin(), history.end(), 0.0f);
        haveTail = false;
        continuous = false;
    };
    auto qualityParam = [&]() { return std::clamp(static_cast<int>(std::lround(p(4))), 0, kQualitySteps - 1); };

    int quality = -1;     // the Quality an RVC voice was loaded with
    int seenQuality = -1; // the newest value of the control, and since when
    clock::time_point seenAt{};
    while (!stop_.load()) {
        if (model && m.kind == "rvc") {
            // A new Quality rebuilds the voice: wait until the slider rests.
            const int q = qualityParam();
            if (q != seenQuality) {
                seenQuality = q;
                seenAt = clock::now();
            } else if (q != quality && clock::now() - seenAt >= std::chrono::milliseconds(400)) {
                modelDirty_ = true;
            }
        }
        if (modelDirty_.exchange(false)) {
            active_ = false;
            model.reset();
            ModelManifest next;
            bool gpu = false, gpuAuto = false, fixedPlan = false;
            StreamPlan plan;
            {
                std::lock_guard<std::mutex> lock(modelMutex_);
                next = pending_;
                gpu = pendingGpu_;
                gpuAuto = gpuAuto_;
                fixedPlan = haveTestPlan_;
                plan = testPlan_;
            }
            if (gpuAuto && next.valid) {
                OrtRuntime& rt = OrtRuntime::instance();
                gpu = rt.ensureLoaded() && rt.gpuRecommended();
                std::lock_guard<std::mutex> lock(modelMutex_);
                if (gpuAuto_) {
                    pendingGpu_ = gpu;
                    gpuAuto_ = false;
                }
            }
            if (!next.valid) {
                setStatus(AiStatus::NoModel, next.error.empty() ? "No model selected" : next.error);
            } else {
                setStatus(AiStatus::Loading, "Loading " + next.name + "...");
                std::unique_ptr<VoiceConverter> mdl;
                std::string err;
                bool loaded = false;
                double runSec = 0.0;
                if (next.kind == "rvc") {
                    quality = seenQuality = qualityParam();
                    seenAt = clock::now();
                    if (!fixedPlan) plan = rvcStreamPlan(quality);
                    // The context shrinks until a run fits in the time the
                    // plan's delay leaves for it (the voice is rebuilt).
                    const int sr = next.sampleRate;
                    const int floorCtx = std::max(400, plan.crossfadeMs + kSearchMs + plan.lookaheadMs + kDecodeMarginMs + 100);
                    for (int attempt = 0;; ++attempt) {
                        auto rvc = std::make_unique<RvcVoice>();
                        // + 10 ms: the resampler hands over a sample more now and then.
                        const int block = sr * (plan.contextMs + plan.chunkMs + 10) / 1000;
                        const int decode =
                            sr * (plan.chunkMs + plan.crossfadeMs + kSearchMs + plan.lookaheadMs + kDecodeMarginMs) / 1000;
                        loaded = rvc->load(next, gpu, block, &err, {}, decode);
                        if (!loaded) break;
                        runSec = timeRuns(*rvc, block, sr);
                        // Fits when it adds no delay (see minLatency_) or, on a
                        // slower PC, when it still takes at most 55 % of the time.
                        const double budget = std::max((plan.chunkMs * 0.00025 + 0.03) / 1.5, plan.chunkMs * 0.00055);
                        if (fixedPlan || runSec <= budget || plan.contextMs <= floorCtx || attempt >= 3) {
                            mdl = std::move(rvc);
                            break;
                        }
                        // The speech features and the pitch grow with the block;
                        // the voice network mostly makes the newest audio. Aim a
                        // little under the budget with what is left for them.
                        const RvcVoice::StepTimes& t = rvc->lastTimes();
                        const double grows = std::max(1e-4, t.prep + t.encoder + t.index + t.pitch);
                        const double room = budget * 0.9 - std::max(0.0, runSec - grows);
                        const double fit = room / grows * (plan.contextMs + plan.chunkMs) - plan.chunkMs;
                        const int ctx = std::max(floorCtx, std::min(plan.contextMs - 100, static_cast<int>(fit) / 100 * 100));
                        ECHO_LOG_INFO("AI voice: %.0f ms per run with %d ms of context (%.0f ms fit); trying %d ms", runSec * 1000.0,
                                      plan.contextMs, budget * 1000.0, ctx);
                        plan.contextMs = ctx;
                    }
                } else {
                    plan = StreamPlan{next.chunkMs, next.contextMs, next.crossfadeMs, 0};
                    auto onnx = std::make_unique<OnnxVoiceModel>();
                    loaded = onnx->load(next, gpu, &err);
                    if (loaded) runSec = timeRuns(*onnx, next.sampleRate * (plan.contextMs + plan.chunkMs) / 1000, next.sampleRate);
                    mdl = std::move(onnx);
                }
                if (!loaded) {
                    setStatus(AiStatus::Error, "Could not load model: " + err);
                    ECHO_LOG_WARN("AI model '%s' failed to load: %s", next.name.c_str(), err.c_str());
                } else {
                    m = next;
                    m.chunkMs = plan.chunkMs;
                    m.contextMs = plan.contextMs;
                    m.crossfadeMs = plan.crossfadeMs;
                    model = std::move(mdl);
                    contextMs = plan.contextMs;
                    loadedQuality = m.kind == "rvc" && !fixedPlan ? quality : -1;
                    const double eng = sr_;
                    const int sr = m.sampleRate;
                    ratioDown = eng / sr;
                    ratioUp = sr / eng;
                    chunkEng = static_cast<int>(eng * plan.chunkMs / 1000.0);
                    const int chunkModel = static_cast<int>(sr * plan.chunkMs / 1000.0);
                    ctxModel = sr * plan.contextMs / 1000;
                    xfModel = std::min(sr * plan.crossfadeMs / 1000, chunkModel / 2);
                    laModel = sr * plan.lookaheadMs / 1000;
                    // The piece played, its blend and the search must lie within the context.
                    searchModel = std::clamp(ctxModel - xfModel - laModel, 0, sr * kSearchMs / 1000);
                    down.prepare(1, ratioDown, chunkEng + 64, ResamplerQuality::High);
                    up.prepare(1, ratioUp, chunkModel + 64, ResamplerQuality::High);
                    history.assign(static_cast<size_t>(ctxModel + chunkModel + 8), 0.0f);
                    tail.assign(static_cast<size_t>(xfModel), 0.0f);
                    fadeIn.resize(static_cast<size_t>(xfModel));
                    for (int k = 0; k < xfModel; ++k) {
                        const double s = std::sin(0.5 * 3.14159265358979 * (k + 0.5) / xfModel);
                        fadeIn[static_cast<size_t>(k)] = static_cast<float>(s * s);
                    }
                    // Each piece played starts lookahead + crossfade + search
                    // before the newest audio of its run, less the shift that
                    // lines it up (half the search, as planned): converted
                    // sample i holds the voice of input sample i - that lag.
                    // (The resamplers add only about a sample.)
                    const double off = (laModel + xfModel + searchModel - searchModel / 2) * eng / sr;
                    wetOffset_ = static_cast<int>(std::lround(off));
                    // The delay must leave room for a run: allow 1.5x its time.
                    minLatency_ = static_cast<int>(chunkEng + std::max(chunkEng * 0.25 + eng * 0.06, 1.5 * runSec * eng + eng * 0.03) + off);
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
                continuous = false;
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
            model->setIndexRate(p(3) / 100.0f);
            model->setAdvance(continuous ? static_cast<int>(tmp.size()) : 0);
            continuous = true;
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
            // This run's piece: chunk + crossfade + search, ending lookahead
            // before the newest audio. Line it up with the last piece's end,
            // blend over the crossfade, keep its own end for the next.
            const int L = static_cast<int>(modelOut.size());
            const int chunk = static_cast<int>(tmp.size());
            const int need = chunk + xfModel + searchModel;
            const int begin = L - laModel - need;
            wav.assign(static_cast<size_t>(need), 0.0f);
            for (int k = 0; k < need; ++k) {
                const int idx = begin + k;
                const float v = (idx >= 0 && idx < L) ? modelOut[static_cast<size_t>(idx)] : 0.0f;
                wav[static_cast<size_t>(k)] = std::isfinite(v) ? v : 0.0f;
            }
            const int off = haveTail && searchModel > 0 ? solaOffset(wav, tail, searchModel) : searchModel / 2;
            emitted.resize(static_cast<size_t>(chunk));
            for (int k = 0; k < chunk; ++k) {
                float v = wav[static_cast<size_t>(off + k)];
                if (haveTail && k < xfModel) {
                    const float a = fadeIn[static_cast<size_t>(k)];
                    v = v * a + tail[static_cast<size_t>(k)] * (1.0f - a);
                }
                emitted[static_cast<size_t>(k)] = v;
            }
            for (int k = 0; k < xfModel; ++k) tail[static_cast<size_t>(k)] = wav[static_cast<size_t>(off + chunk + k)];
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
