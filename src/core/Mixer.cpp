#include "core/Mixer.h"

#include <cstring>

#include "core/Sources.h"

namespace echo {

void ChainBypass::prepare(double sampleRate, int maxBlock, bool bypassed) {
    mix.prepare(sampleRate, 0.015);
    mix.reset(bypassed ? 0.0f : 1.0f);
    keepWarmSamples = static_cast<int>(sampleRate * 10.0);
    offSamples = 0;
    cold = false;
    for (auto* v : {&scratchL, &scratchR, &dryL, &dryR}) v->assign(maxBlock, 0.0f);
}

void ChainBypass::run(ChainRunner& runner, const ChainSnap& chain, bool bypass, float* l, float* r, bool& mono,
                      int n, const float* refL, const float* refR, int refCh) {
    if (chain.count == 0) return;
    mix.setTarget(bypass ? 0.0f : 1.0f);
    const bool fading = mix.isSmoothing();
    if (!fading && mix.current() <= 0.0f) {
        // Fully bypassed: the output is the dry signal. Keep the effects
        // processing a copy for up to 10 s so a quick A/B switch back has no
        // stale tails or start-up gap; then stop and restart them on return.
        if (offSamples < keepWarmSamples) {
            offSamples += n;
            std::memcpy(scratchL.data(), l, sizeof(float) * n);
            std::memcpy(scratchR.data(), mono ? l : r, sizeof(float) * n);
            bool m = mono;
            runner.process(chain, scratchL.data(), scratchR.data(), m, n, refL, refR, refCh);
        } else {
            cold = true;
        }
        return;
    }
    if (cold) {
        // Restart every effect like a fresh switch-on: reset, then pass the
        // dry signal through its latency before fading it in.
        for (int i = 0; i < chain.count; ++i)
            if (Effect* fx = chain.fx[i]) {
                fx->rtActiveMix = 0.0f;
                fx->rtWarmup = -1;
            }
        cold = false;
    }
    offSamples = 0;
    if (!fading) {
        runner.process(chain, l, r, mono, n, refL, refR, refCh);
        return;
    }
    const bool monoBefore = mono;
    std::memcpy(dryL.data(), l, sizeof(float) * n);
    std::memcpy(dryR.data(), monoBefore ? l : r, sizeof(float) * n);
    runner.process(chain, l, r, mono, n, refL, refR, refCh);
    for (int i = 0; i < n; ++i) {
        const float m = mix.next();
        l[i] = dryL[i] + (l[i] - dryL[i]) * m;
        if (!mono) r[i] = dryR[i] + (r[i] - dryR[i]) * m;
    }
}

StripRuntime::StripRuntime() {
    for (auto& s : sends) s.gain.reset(0.0f);
}

void StripRuntime::prepare(double sampleRate, int maxBlock) {
    sr_ = sampleRate;
    for (auto* v : {&l_, &r_, &preL_, &preR_, &postL_, &postR_}) v->assign(maxBlock, 0.0f);
    trimSm_.prepare(sampleRate, 0.02);
    faderSm_.prepare(sampleRate, 0.02);
    panLSm_.prepare(sampleRate, 0.02);
    panRSm_.prepare(sampleRate, 0.02);
    muteSm_.prepare(sampleRate, 0.01);
    bypass_.prepare(sampleRate, maxBlock, fxBypass.load());
    trimSm_.reset(dbToGain(trimDb.load()));
    faderSm_.reset(dbToGain(faderDb.load()));
    panLSm_.reset(1.0f);
    panRSm_.reset(1.0f);
    muteSm_.reset(mute.load() ? 0.0f : 1.0f);
    for (auto& s : sends) {
        s.gain.prepare(sampleRate, 0.02);
        s.gain.reset(s.enabled.load() && !s.blocked.load() ? dbToGain(s.gainDb.load()) : 0.0f);
    }
    chainRunner.prepare(maxBlock);
}

void StripRuntime::process(const SourceRuntime* src, const ChainSnap& chain, int n, bool anySolo,
                           BusAccumulators& acc, const SourceRuntime* reference) {
    float* L = l_.data();
    float* R = r_.data();
    bool mono = true;

    // --- channel mapping ---
    const int sc = src ? src->channels() : 0;
    if (sc <= 0) {
        std::memset(L, 0, sizeof(float) * n);
    } else if (downmix.load(std::memory_order_relaxed) && sc > 1) {
        std::memcpy(L, src->channel(0), sizeof(float) * n);
        for (int c = 1; c < sc; ++c) {
            const float* x = src->channel(c);
            for (int i = 0; i < n; ++i) L[i] += x[i];
        }
        const float k = 1.0f / static_cast<float>(sc);
        for (int i = 0; i < n; ++i) L[i] *= k;
    } else {
        const int first = clampv(chanFirst.load(std::memory_order_relaxed), 0, sc - 1);
        const int count = chanCount.load(std::memory_order_relaxed);
        std::memcpy(L, src->channel(first), sizeof(float) * n);
        if (count >= 2 && sc > 1) {
            std::memcpy(R, src->channel(std::min(first + 1, sc - 1)), sizeof(float) * n);
            mono = false;
        }
    }

    // --- trim / polarity ---
    float trimTarget = dbToGain(trimDb.load(std::memory_order_relaxed));
    if (invertPolarity.load(std::memory_order_relaxed)) trimTarget = -trimTarget;
    trimSm_.setTarget(trimTarget);
    if (mono) {
        trimSm_.applyGain(L, n);
    } else if (!trimSm_.isSmoothing()) {
        trimSm_.applyGain(L, n);
        trimSm_.applyGain(R, n);
    } else {
        for (int i = 0; i < n; ++i) {
            const float g = trimSm_.next();
            L[i] *= g;
            R[i] *= g;
        }
    }
    inMeter.analyze(L, mono ? L : R, n);

    // --- effects ---
    const float* refL = nullptr;
    const float* refR = nullptr;
    int refCh = 0;
    if (reference && reference->channels() > 0) {
        refCh = std::min(2, reference->channels());
        refL = reference->channel(0);
        refR = reference->channel(refCh - 1);
    }
    bypass_.run(chainRunner, chain, fxBypass.load(std::memory_order_relaxed), L, R, mono, n, refL, refR, refCh);
    if (mono) std::memcpy(R, L, sizeof(float) * n);

    // --- fader / pan / mute ---
    const bool muted = mute.load(std::memory_order_relaxed) ||
                       (anySolo && !solo.load(std::memory_order_relaxed));
    muteSm_.setTarget(muted ? 0.0f : 1.0f);
    faderSm_.setTarget(dbToGain(faderDb.load(std::memory_order_relaxed)));
    const float p = clampv(pan.load(std::memory_order_relaxed), -1.0f, 1.0f);
    float gL, gR;
    if (mono) {
        // Constant-power pan, normalised to unity at centre.
        const float th = (p + 1.0f) * kPi * 0.25f;
        gL = std::cos(th) * 1.41421356f;
        gR = std::sin(th) * 1.41421356f;
    } else {
        gL = p > 0.0f ? 1.0f - p : 1.0f;
        gR = p < 0.0f ? 1.0f + p : 1.0f;
    }
    panLSm_.setTarget(gL);
    panRSm_.setTarget(gR);

    float* pL = preL_.data();
    float* pR = preR_.data();
    float* oL = postL_.data();
    float* oR = postR_.data();
    for (int i = 0; i < n; ++i) {
        const float m = muteSm_.next();
        const float f = faderSm_.next() * m;
        pL[i] = L[i] * m;
        pR[i] = R[i] * m;
        oL[i] = L[i] * f * panLSm_.next();
        oR[i] = R[i] * f * panRSm_.next();
    }
    outMeter.analyze(oL, oR, n);
    isMono.store(mono, std::memory_order_relaxed);

    // --- sends ---
    for (int b = 0; b < kMaxBuses; ++b) {
        if (!acc.used[b]) continue;
        SendParams& s = sends[b];
        const bool on = s.enabled.load(std::memory_order_relaxed) &&
                        !s.blocked.load(std::memory_order_relaxed);
        s.gain.setTarget(on ? dbToGain(s.gainDb.load(std::memory_order_relaxed)) : 0.0f);
        if (!s.gain.isSmoothing() && s.gain.current() == 0.0f) continue;
        const bool pre = s.preFader.load(std::memory_order_relaxed);
        const float* sl = pre ? pL : oL;
        const float* sr = pre ? pR : oR;
        float* dl = acc.l[b];
        float* dr = acc.r[b];
        if (s.gain.isSmoothing()) {
            for (int i = 0; i < n; ++i) {
                const float g = s.gain.next();
                dl[i] += sl[i] * g;
                dr[i] += sr[i] * g;
            }
        } else {
            const float g = s.gain.current();
            for (int i = 0; i < n; ++i) {
                dl[i] += sl[i] * g;
                dr[i] += sr[i] * g;
            }
        }
    }
}

// --------------------------------------------------------------------------

BusRuntime::BusRuntime() = default;

void BusRuntime::prepare(double sampleRate, int maxBlock) {
    sr_ = sampleRate;
    faderSm_.prepare(sampleRate, 0.02);
    muteSm_.prepare(sampleRate, 0.01);
    faderSm_.reset(dbToGain(faderDb.load()));
    muteSm_.reset(mute.load() ? 0.0f : 1.0f);
    limiter_.prepare(sampleRate, 0.001, 2);
    limiter_.setReleaseSeconds(0.06f);
    bypass_.prepare(sampleRate, maxBlock, fxBypass.load());
    chainRunner.prepare(maxBlock);
}

void BusRuntime::process(const ChainSnap& chain, float* l, float* r, int n) {
    bool mono = false;
    // Crossfaded like strip chains (an abrupt switch used to click).
    bypass_.run(chainRunner, chain, fxBypass.load(std::memory_order_relaxed), l, r, mono, n);
    if (mono) std::memcpy(r, l, sizeof(float) * n);

    if (this->mono.load(std::memory_order_relaxed)) {
        for (int i = 0; i < n; ++i) l[i] = r[i] = 0.5f * (l[i] + r[i]);
    }
    muteSm_.setTarget(mute.load(std::memory_order_relaxed) ? 0.0f : 1.0f);
    faderSm_.setTarget(dbToGain(faderDb.load(std::memory_order_relaxed)));
    for (int i = 0; i < n; ++i) {
        const float g = faderSm_.next() * muteSm_.next();
        l[i] *= g;
        r[i] *= g;
    }
    if (limiterEnabled.load(std::memory_order_relaxed)) {
        limiter_.setCeilingDb(limiterCeilingDb.load(std::memory_order_relaxed));
        float* ch[2] = {l, r};
        limiter_.process(ch, 2, n);
        limiterGrDb.store(limiter_.currentGainReductionDb(), std::memory_order_relaxed);
    } else {
        limiterGrDb.store(0.0f, std::memory_order_relaxed);
    }
    meter.analyze(l, r, n);
}

} // namespace echo
