// In-memory fake audio devices for testing the controller, sources and sinks
// without hardware.
#pragma once

#include <cmath>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "core/Endpoint.h"
#include "session/DeviceBackend.h"

namespace echo::test {

class FakeCapture final : public CaptureEndpoint {
public:
    FakeCapture(int channels, int rate, int period = 480, bool sparse = false)
        : ch_(channels), rate_(rate), period_(period), sparse_(sparse) {
        ring_.allocate(static_cast<size_t>(rate) * channels);
        desc_ = "fake capture";
    }
    int channels() const override { return ch_; }
    int sampleRate() const override { return rate_; }
    int periodFrames() const override { return period_; }
    SpscRing<float>& ring() override { return ring_; }
    EndpointStatus status() const override { return status_; }
    bool isSparse() const override { return sparse_; }
    double deviceLatencySeconds() const override { return 0.01; }
    const std::string& describe() const override { return desc_; }
    std::string lastError() const override { return error_; }
    EndpointStatus status_ = EndpointStatus::Running;
    std::string error_;

    // Produce `frames` frames of a sine (all channels; channel c is scaled
    // by 1/(c+1) so channel mapping can be verified).
    void produceSine(int frames, double freq, float amp = 0.5f) {
        std::vector<float> buf(static_cast<size_t>(frames) * ch_);
        for (int i = 0; i < frames; ++i) {
            const float v = amp * static_cast<float>(std::sin(phase_));
            phase_ += 2.0 * 3.14159265358979 * freq / rate_;
            for (int c = 0; c < ch_; ++c) buf[static_cast<size_t>(i) * ch_ + c] = v / static_cast<float>(c + 1);
        }
        ring_.write(buf.data(), buf.size());
    }

private:
    int ch_, rate_, period_;
    bool sparse_;
    SpscRing<float> ring_;
    std::string desc_;
    double phase_ = 0.0;
};

class FakeRender final : public RenderEndpoint {
public:
    FakeRender(int channels, int rate, int period = 480) : ch_(channels), rate_(rate), period_(period) {
        ring_.allocate(static_cast<size_t>(rate) * channels);
        desc_ = "fake render";
    }
    int channels() const override { return ch_; }
    int sampleRate() const override { return rate_; }
    int periodFrames() const override { return period_; }
    SpscRing<float>& ring() override { return ring_; }
    EndpointStatus status() const override { return status_; }
    double deviceLatencySeconds() const override { return 0.01; }
    const std::string& describe() const override { return desc_; }
    std::string lastError() const override { return error_; }
    std::string error_;
    void setWakeTarget(WakeTarget* t, int threshold) override {
        wake_ = t;
        threshold_ = threshold;
    }
    // Device consumes `frames` frames (like a hardware period).
    void consume(int frames) {
        std::vector<float> buf(static_cast<size_t>(frames) * ch_);
        const size_t got = ring_.read(buf.data(), buf.size());
        out.insert(out.end(), buf.begin(), buf.begin() + static_cast<long>(got));
        if (got < buf.size()) ++underrunCount;
    }
    EndpointStatus status_ = EndpointStatus::Running;
    std::vector<float> out; // interleaved consumed audio
    int underrunCount = 0;
    WakeTarget* wake_ = nullptr;
    int threshold_ = 0;

private:
    int ch_, rate_, period_;
    SpscRing<float> ring_;
    std::string desc_;
};

class FakeBackend final : public DeviceBackend {
public:
    struct Dev {
        BackendDevice info;
        int channels = 2;
        int rate = 48000;
    };
    std::vector<Dev> devs;
    std::map<std::string, std::string> cablePartners; // render id -> capture id
    std::map<std::string, std::shared_ptr<FakeCapture>> captures;
    std::map<std::string, std::shared_ptr<FakeRender>> renders;
    uint64_t gen = 1;
    bool changed = false;
    bool asyncOpen = false; // endpoints start in status Starting (like real WASAPI opening)
    int openCaptureCalls = 0, openRenderCalls = 0;

    void addDevice(const std::string& id, const std::string& name, bool render, int ch = 2, int rate = 48000,
                   bool isDefault = false, bool cable = false) {
        Dev d;
        d.info.id = id;
        d.info.name = name;
        d.info.render = render;
        d.info.active = true;
        d.info.isDefault = isDefault;
        d.info.isCable = cable;
        d.info.channels = ch;
        d.info.sampleRate = rate;
        d.channels = ch;
        d.rate = rate;
        devs.push_back(d);
    }
    Dev* find(const std::string& id) {
        for (auto& d : devs)
            if (d.info.id == id) return &d;
        return nullptr;
    }

    std::shared_ptr<CaptureEndpoint> openCapture(const CaptureRequest& req, std::string* error) override {
        ++openCaptureCalls;
        std::string id = req.deviceId;
        if (req.kind == SourceKind::ProcessLoopback) id = "proc:" + std::to_string(req.processId);
        if (req.kind == SourceKind::SystemExcludingEcho) id = "system";
        int ch = 2, rate = 48000;
        if (Dev* d = find(req.deviceId)) {
            if (!d->info.active) {
                if (error) *error = "inactive";
                return nullptr;
            }
            ch = d->channels;
            rate = d->rate;
        } else if (req.kind == SourceKind::CaptureDevice || req.kind == SourceKind::RenderLoopback) {
            if (error) *error = "no such device";
            return nullptr;
        }
        auto c = std::make_shared<FakeCapture>(ch, rate, 480, req.kind != SourceKind::CaptureDevice);
        if (asyncOpen) c->status_ = EndpointStatus::Starting; // test flips it later
        captures[id] = c;
        return c;
    }
    std::shared_ptr<RenderEndpoint> openRender(const RenderRequest& req, std::string* error) override {
        ++openRenderCalls;
        Dev* d = find(req.deviceId);
        if (!d || !d->info.active) {
            if (error) *error = "no such device";
            return nullptr;
        }
        auto r = std::make_shared<FakeRender>(d->channels, d->rate);
        if (asyncOpen) r->status_ = EndpointStatus::Starting;
        renders[req.deviceId] = r;
        return r;
    }
    std::vector<BackendDevice> devices() override {
        std::vector<BackendDevice> v;
        for (auto& d : devs) v.push_back(d.info);
        return v;
    }
    uint64_t deviceGeneration() override { return gen; }
    bool pollDeviceChanges() override {
        const bool c = changed;
        changed = false;
        return c;
    }
    std::string resolveDevice(const std::string& id, const std::string& name, bool render) override {
        for (auto& d : devs)
            if (d.info.id == id && d.info.active && d.info.render == render) return id;
        for (auto& d : devs)
            if (!name.empty() && d.info.name == name && d.info.active && d.info.render == render) return d.info.id;
        return {};
    }
    std::string defaultDevice(bool render) override {
        for (auto& d : devs)
            if (d.info.render == render && d.info.isDefault && d.info.active) return d.info.id;
        return {};
    }
    std::string cablePartnerOfRender(const std::string& renderId) override {
        auto it = cablePartners.find(renderId);
        return it == cablePartners.end() ? std::string() : it->second;
    }
    std::vector<BackendProcess> audioProcesses() override { return {{4242, "music.exe", "Music Player"}}; }
    bool processAlive(uint32_t pid) override { return pid == 4242; }
    bool processLoopbackSupported() override { return true; }
};

} // namespace echo::test
