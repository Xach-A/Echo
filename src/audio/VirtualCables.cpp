#include "audio/VirtualCables.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <thread>

#include "audio/WasapiStream.h"
#include "core/Common.h"

namespace echo::win {

namespace {

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

// "CABLE Input (VB-Audio Virtual Cable)" -> "cable"; "Line 1" -> "line 1".
std::string stem(const AudioDeviceInfo& d) {
    std::string s = lower(d.description.empty() ? d.name : d.description);
    const auto paren = s.find('(');
    if (paren != std::string::npos) s = s.substr(0, paren);
    for (const char* w : {"input", "output", "playback", "recording", "speakers", "microphone", "mic"}) {
        size_t p;
        while ((p = s.find(w)) != std::string::npos) s.erase(p, std::strlen(w));
    }
    s.erase(std::remove_if(s.begin(), s.end(), [](char c) { return c == ' ' || c == '-' || c == '_'; }), s.end());
    return s;
}

} // namespace

std::vector<CablePair> findCablePairs(const std::vector<AudioDeviceInfo>& devices) {
    std::vector<const AudioDeviceInfo*> renders, captures;
    for (const auto& d : devices) {
        if (!d.active() || d.virtualKind != VirtualKind::Cable) continue;
        (d.flow == Flow::Render ? renders : captures).push_back(&d);
    }
    // Score every playback/recording combination; same driver is required.
    struct Candidate {
        size_t r, c;
        int score;
    };
    std::vector<Candidate> cands;
    for (size_t ri = 0; ri < renders.size(); ++ri)
        for (size_t ci = 0; ci < captures.size(); ++ci) {
            const auto* r = renders[ri];
            const auto* c = captures[ci];
            int score = 0;
            if (!r->interfaceName.empty() && r->interfaceName == c->interfaceName) score += 4;
            if (!r->containerId.empty() && r->containerId == c->containerId) score += 2;
            if (stem(*r) == stem(*c)) score += 3;
            if (score >= 4) cands.push_back({ri, ci, score});
        }
    // Best matches first, so "CABLE Input" (same name stem as "CABLE Output")
    // wins over "CABLE In 16ch" regardless of enumeration order.
    std::stable_sort(cands.begin(), cands.end(), [](const Candidate& a, const Candidate& b) { return a.score > b.score; });
    std::vector<int> partner(renders.size(), -1);
    std::vector<bool> secondary(renders.size(), false), captureUsed(captures.size(), false);
    for (const auto& k : cands)
        if (partner[k.r] < 0 && !captureUsed[k.c]) {
            partner[k.r] = static_cast<int>(k.c);
            captureUsed[k.c] = true;
        }
    // Remaining playback endpoints of the same driver feed an already-paired
    // recording side: keep them (feedback protection must know every route).
    for (const auto& k : cands)
        if (partner[k.r] < 0) {
            partner[k.r] = static_cast<int>(k.c);
            secondary[k.r] = true;
        }
    std::vector<CablePair> out;
    for (const bool pass : {false, true})
        for (size_t ri = 0; ri < renders.size(); ++ri) {
            if (partner[ri] < 0 || secondary[ri] != pass) continue;
            const auto* r = renders[ri];
            const auto* c = captures[partner[ri]];
            CablePair p;
            p.renderId = r->idUtf8;
            p.captureId = c->idUtf8;
            p.renderName = r->name;
            p.captureName = c->name;
            p.vendor = r->virtualVendor;
            p.displayName = r->interfaceName.empty() ? r->name : r->interfaceName;
            p.secondary = pass;
            out.push_back(std::move(p));
        }
    return out;
}

CableTestResult testLoopback(const std::string& renderId, const std::string& captureId) {
    CableTestResult res;
    StreamOptions ro;
    ro.deviceId = fromUtf8(renderId);
    ro.label = "loopback test (render)";
    std::string err;
    auto ren = WasapiRender::open(ro, &err);
    if (!ren) {
        res.message = "cannot open playback side: " + err;
        return res;
    }
    StreamOptions co;
    co.deviceId = fromUtf8(captureId);
    co.label = "loopback test (capture)";
    auto cap = WasapiCapture::open(co, &err);
    if (!cap) {
        res.message = "cannot open recording side: " + err;
        return res;
    }

    const int rch = ren->channels();
    const double rsr = ren->sampleRate();
    const int cch = cap->channels();
    const double csr = cap->sampleRate();
    const double freq = 1000.0;
    const float amp = dbToGain(-20.0f);
    double phase = 0.0;
    std::vector<float> block;
    std::vector<float> captured;
    captured.reserve(static_cast<size_t>(csr * 2));
    std::vector<float> tmp(static_cast<size_t>(csr) * cch);

    // Drain whatever the capture side already holds.
    cap->ring().skip(cap->ring().readAvailable());

    using clock = std::chrono::steady_clock;
    const auto start = clock::now();
    const double silenceLead = 0.2; // seconds of silence before the burst
    bool burstStarted = false;
    clock::time_point burstTime;
    size_t burstCaptureIndex = 0;
    int detectedAt = -1;
    while (clock::now() - start < std::chrono::milliseconds(1200)) {
        // Keep ~40 ms queued on the render side.
        const double elapsed = std::chrono::duration<double>(clock::now() - start).count();
        const size_t queued = ren->ring().size() / rch;
        const size_t wantFrames = static_cast<size_t>(rsr * 0.04);
        if (queued < wantFrames) {
            const size_t n = wantFrames - queued;
            block.assign(n * rch, 0.0f);
            const bool tone = elapsed >= silenceLead;
            if (tone && !burstStarted) {
                burstStarted = true;
                burstTime = clock::now() + std::chrono::microseconds(static_cast<int64_t>(queued / rsr * 1e6));
                burstCaptureIndex = captured.size();
            }
            for (size_t i = 0; i < n; ++i) {
                const float s = tone ? amp * static_cast<float>(std::sin(2.0 * kPiD * phase)) : 0.0f;
                phase += freq / rsr;
                if (phase >= 1.0) phase -= 1.0;
                for (int c = 0; c < rch; ++c) block[i * rch + c] = s;
            }
            ren->ring().write(block.data(), block.size());
        }
        // Collect capture (first channel).
        const size_t avail = std::min(cap->ring().readAvailable(), tmp.size());
        const size_t frames = avail / cch;
        if (frames) {
            cap->ring().read(tmp.data(), frames * cch);
            for (size_t i = 0; i < frames; ++i) captured.push_back(tmp[i * cch]);
            if (burstStarted && detectedAt < 0) {
                // Onset: first 5 ms window after the burst with energy.
                for (size_t i = burstCaptureIndex; i + 240 < captured.size(); i += 48) {
                    float pk = 0.0f;
                    for (size_t k = i; k < i + 240; ++k) pk = std::max(pk, std::fabs(captured[k]));
                    if (pk > amp * 0.3f) {
                        detectedAt = static_cast<int>(i);
                        res.latencyMs = static_cast<float>(
                            std::chrono::duration<double, std::milli>(clock::now() - burstTime).count());
                        break;
                    }
                }
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(3));
    }
    // Level of the last 300 ms of capture at the test frequency.
    const size_t n = std::min(captured.size(), static_cast<size_t>(csr * 0.3));
    if (n < 1000) {
        res.message = "recording side delivered no audio";
        return res;
    }
    double re = 0, im = 0, ws = 0;
    const size_t s0 = captured.size() - n;
    for (size_t i = 0; i < n; ++i) {
        const double w = 0.5 - 0.5 * std::cos(2 * kPiD * i / (n - 1));
        re += w * captured[s0 + i] * std::cos(2 * kPiD * freq * i / csr);
        im += w * captured[s0 + i] * std::sin(2 * kPiD * freq * i / csr);
        ws += w;
    }
    const double a = 2 * std::sqrt(re * re + im * im) / ws;
    res.levelDb = gainToDb(static_cast<float>(a));
    res.ok = a > dbToGain(-32.0f);
    res.message = res.ok ? "test tone received" : "test tone NOT received on the recording side";
    return res;
}

CableTestResult testCable(const CablePair& pair, int /*sampleRate*/) {
    return testLoopback(pair.renderId, pair.captureId);
}

} // namespace echo::win
