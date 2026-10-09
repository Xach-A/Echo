#include "dsp/Effect.h"

#include <cstring>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace echo {

namespace {
std::atomic<uint32_t> g_nextInstanceId{1};
}

Effect::Effect(const EffectInfo& info)
    : label(info.name), instanceId(g_nextInstanceId.fetch_add(1)), info_(info) {
    for (int i = 0; i < kMaxEffectParams; ++i) params_[i].store(0.0f, std::memory_order_relaxed);
    resetParamsToDefault();
}

int Effect::paramIndex(std::string_view id) const {
    for (int i = 0; i < info_.numParams; ++i)
        if (id == info_.params[i].id) return i;
    return -1;
}

void Effect::setParam(int i, float v) {
    if (i < 0 || i >= info_.numParams) return;
    const ParamInfo& pi = info_.params[i];
    if (!std::isfinite(v)) v = pi.def;
    v = clampv(v, pi.min, pi.max);
    if (pi.scale == ParamScale::Enum || pi.scale == ParamScale::Int || pi.scale == ParamScale::Toggle)
        v = std::round(v);
    params_[i].store(v, std::memory_order_relaxed);
}

void Effect::setParam(std::string_view id, float v) { setParam(paramIndex(id), v); }

void Effect::resetParamsToDefault() {
    for (int i = 0; i < info_.numParams; ++i)
        params_[i].store(info_.params[i].def, std::memory_order_relaxed);
}

void Effect::prepare(double sampleRate, int maxBlock) {
    sr_ = sampleRate;
    maxBlock_ = maxBlock;
    onPrepare();
    onReset();
    rtActiveMix = -1.0f;
    prepared_ = true;
}

void Effect::process(float* const* ch, int numCh, int n) {
    if (resetRequested_.exchange(false, std::memory_order_acquire)) onReset();
    try {
        processImpl(ch, numCh, n);
    } catch (...) {
        // Effects are written not to throw; if one ever does, isolate it.
        faulted.store(true, std::memory_order_relaxed);
        for (int c = 0; c < numCh; ++c) std::memset(ch[c], 0, sizeof(float) * n);
    }
}

// --------------------------------------------------------------------------

void ChainRunner::prepare(int /*maxBlock*/) {
    std::memset(dryL_, 0, sizeof(dryL_));
    std::memset(dryR_, 0, sizeof(dryR_));
}

static bool bufferHealthy(const float* x, int n) {
    for (int i = 0; i < n; ++i) {
        const float v = x[i];
        // NaN fails both comparisons; 1e4 (+80 dBFS) means a blown-up filter.
        if (!(v > -1.0e4f && v < 1.0e4f)) return false;
    }
    return true;
}

namespace {
double qpcSeconds() {
    static const double inv = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return 1.0 / static_cast<double>(f.QuadPart);
    }();
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return static_cast<double>(t.QuadPart) * inv;
}
} // namespace

void ChainRunner::process(const ChainSnap& chain, float* l, float* r, bool& mono, int n, const float* refL,
                          const float* refR, int refCh) {
    for (int i = 0; i < chain.count; ++i) {
        Effect* fx = chain.fx[i];
        if (!fx) continue;
        const bool wantActive = !fx->bypass.load(std::memory_order_relaxed) &&
                                !fx->faulted.load(std::memory_order_relaxed);
        const float target = wantActive ? 1.0f : 0.0f;
        if (fx->rtActiveMix < 0.0f) fx->rtActiveMix = target;
        if (fx->rtActiveMix <= 0.0f && target <= 0.0f) continue; // fully bypassed

        if (fx->rtActiveMix <= 0.0f && target > 0.0f) fx->resetState(); // fresh start

        if (mono && fx->info().widensStereo) {
            std::memcpy(r, l, sizeof(float) * n);
            mono = false;
        }
        const int numCh = mono ? 1 : 2;
        std::memcpy(dryL_, l, sizeof(float) * n);
        if (numCh == 2) std::memcpy(dryR_, r, sizeof(float) * n);

        float* chans[2] = {l, r};
        if (fx->usesReference()) fx->setReference(refL, refR, refCh);
        const double t0 = qpcSeconds();
        fx->process(chans, numCh, n);
        const float load = static_cast<float>((qpcSeconds() - t0) * fx->sampleRate() / std::max(1, n));
        const float prev = fx->cpuLoad.load(std::memory_order_relaxed);
        fx->cpuLoad.store(prev + 0.05f * (load - prev), std::memory_order_relaxed);

        if (!bufferHealthy(l, n) || (numCh == 2 && !bufferHealthy(r, n))) {
            std::memcpy(l, dryL_, sizeof(float) * n);
            if (numCh == 2) std::memcpy(r, dryR_, sizeof(float) * n);
            fx->resetState();
            if (fx->faultCount.fetch_add(1, std::memory_order_relaxed) + 1 >= 3)
                fx->faulted.store(true, std::memory_order_relaxed);
            faults.fetch_add(1, std::memory_order_relaxed);
            continue;
        }

        if (fx->rtActiveMix != target) {
            // 10 ms equal-gain crossfade between dry and processed signal.
            const float step = 1.0f / static_cast<float>(std::max(1.0, fx->sampleRate() * 0.010));
            float m = fx->rtActiveMix;
            for (int s = 0; s < n; ++s) {
                m = target > m ? std::min(target, m + step) : std::max(target, m - step);
                l[s] = dryL_[s] + (l[s] - dryL_[s]) * m;
                if (numCh == 2) r[s] = dryR_[s] + (r[s] - dryR_[s]) * m;
            }
            fx->rtActiveMix = m;
        }
    }
}

} // namespace echo
