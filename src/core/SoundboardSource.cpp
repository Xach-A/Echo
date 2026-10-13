#include "core/SoundboardSource.h"

#include <algorithm>
#include <cstring>

namespace echo {

SoundboardSource::SoundboardSource() {
    channels_ = 2;
    key = "soundboard";
    label = "Soundboard";
    for (auto& p : progress_) p.store(-1.0f, std::memory_order_relaxed);
}

void SoundboardSource::prepare(double engineRate, int maxBlock, double blockSeconds) {
    SourceRuntime::prepare(engineRate, maxBlock, blockSeconds);
    fadeIn_ = static_cast<float>(1.0 / std::max(1.0, engineRate * 0.002));   // 2 ms: no click if a file starts loud
    fadeOut_ = static_cast<float>(1.0 / std::max(1.0, engineRate * 0.010));  // 10 ms when stopped
    for (auto& v : voices_) v = Voice{};
    seenBank_ = bankRt_.load(std::memory_order_acquire);
    state.store(static_cast<int>(SourceState::Running), std::memory_order_relaxed);
}

void SoundboardSource::setBank(std::shared_ptr<const SoundBank> bank) {
    bank_ = std::move(bank);
    bankRt_.store(bank_.get(), std::memory_order_release);
}

void SoundboardSource::push(Cmd c, int pad) {
    if (c != Cmd::StopAll && (pad < 0 || pad >= kMaxPads)) return;
    std::lock_guard<std::mutex> lock(queueMutex_);
    if (queued_ < static_cast<int>(std::size(queue_))) queue_[queued_++] = Request{c, pad};
}

void SoundboardSource::play(int pad) { push(Cmd::Play, pad); }
void SoundboardSource::stop(int pad) { push(Cmd::Stop, pad); }
void SoundboardSource::toggle(int pad) { push(Cmd::Toggle, pad); }
void SoundboardSource::stopAll() { push(Cmd::StopAll, 0); }

float SoundboardSource::progress(int pad) const {
    return pad >= 0 && pad < kMaxPads ? progress_[pad].load(std::memory_order_relaxed) : -1.0f;
}

bool SoundboardSource::padPlaying(int pad) const {
    for (const Voice& v : voices_)
        if (v.active && v.pad == pad && !v.stopping()) return true;
    return false;
}

void SoundboardSource::stopPad(int pad) {
    for (Voice& v : voices_)
        if (v.active && v.pad == pad && !v.stopping()) v.fadeStep = -fadeOut_;
}

void SoundboardSource::startVoice(int pad) {
    const SoundBank* bank = seenBank_;
    if (!bank || pad < 0 || pad >= static_cast<int>(bank->pads.size())) return;
    const SoundBank::Pad& p = bank->pads[static_cast<size_t>(pad)];
    if (!p.clip || p.clip->frames() == 0) return;
    stopPad(pad); // pressing again starts it over; the old copy fades out
    // A free voice, else the oldest one that is already stopping, else the oldest.
    Voice* slot = nullptr;
    for (Voice& v : voices_)
        if (!v.active) {
            slot = &v;
            break;
        }
    if (!slot) {
        for (Voice& v : voices_)
            if (v.stopping() && (!slot || v.started < slot->started)) slot = &v;
    }
    if (!slot) {
        for (Voice& v : voices_)
            if (!slot || v.started < slot->started) slot = &v;
    }
    *slot = Voice{};
    slot->clip = p.clip.get();
    slot->pad = pad;
    slot->gain = p.gain;
    slot->fade = 0.0f;
    slot->fadeStep = fadeIn_;
    slot->started = ++counter_;
    slot->active = true;
}

void SoundboardSource::pull(int frames) {
    zero(frames);
    // A new bank: sounds whose clip is gone fade out right away (within this
    // block, while the old bank is still alive); the others follow their pad.
    const SoundBank* bank = bankRt_.load(std::memory_order_acquire);
    if (bank != seenBank_) {
        for (Voice& v : voices_) {
            if (!v.active) continue;
            int found = -1;
            if (bank)
                for (size_t i = 0; i < bank->pads.size(); ++i)
                    if (bank->pads[i].clip.get() == v.clip) {
                        found = static_cast<int>(i);
                        if (static_cast<int>(i) == v.pad) break;
                    }
            if (found >= 0) {
                v.pad = found;
            } else {
                v.fadeStep = -1.0f / static_cast<float>(std::max(1, std::min(frames, 96)));
                v.pad = -1;
            }
        }
        seenBank_ = bank;
    }

    // Requests, in the order they were made.
    Request reqs[128];
    int n = 0;
    if (queueMutex_.try_lock()) {
        n = queued_;
        std::memcpy(reqs, queue_, sizeof(Request) * static_cast<size_t>(n));
        queued_ = 0;
        queueMutex_.unlock();
    }
    for (int i = 0; i < n; ++i) {
        const Request& r = reqs[i];
        switch (r.cmd) {
        case Cmd::Play: startVoice(r.pad); break;
        case Cmd::Stop: stopPad(r.pad); break;
        case Cmd::Toggle:
            if (padPlaying(r.pad)) stopPad(r.pad);
            else startVoice(r.pad);
            break;
        case Cmd::StopAll:
            for (Voice& v : voices_)
                if (v.active && !v.stopping()) v.fadeStep = -fadeOut_;
            break;
        }
    }

    float* l = channelMut(0);
    float* r = channelMut(1);
    int playing = 0;
    for (Voice& v : voices_) {
        if (!v.active) continue;
        const float* s = v.clip->samples.data();
        const size_t total = v.clip->frames();
        int i = 0;
        for (; i < frames && v.pos < total; ++i, ++v.pos) {
            if (v.fadeStep != 0.0f) {
                v.fade += v.fadeStep;
                if (v.fade >= 1.0f) {
                    v.fade = 1.0f;
                    v.fadeStep = 0.0f;
                } else if (v.fade <= 0.0f) {
                    v.fade = 0.0f;
                    break;
                }
            }
            const float g = v.gain * v.fade;
            l[i] += s[2 * v.pos] * g;
            r[i] += s[2 * v.pos + 1] * g;
        }
        if (v.pos >= total || (v.stopping() && v.fade <= 0.0f)) {
            v.active = false;
            continue;
        }
        ++playing;
    }
    playing_.store(playing, std::memory_order_relaxed);

    // Progress of each pad's newest sound, for the UI.
    float prog[kMaxPads];
    uint64_t newest[kMaxPads] = {};
    for (float& p : prog) p = -1.0f;
    for (const Voice& v : voices_)
        if (v.active && !v.stopping() && v.pad >= 0 && v.pad < kMaxPads && v.started > newest[v.pad]) {
            newest[v.pad] = v.started;
            prog[v.pad] = static_cast<float>(static_cast<double>(v.pos) / static_cast<double>(std::max<size_t>(1, v.clip->frames())));
        }
    for (int p = 0; p < kMaxPads; ++p)
        if (progress_[p].load(std::memory_order_relaxed) != prog[p]) progress_[p].store(prog[p], std::memory_order_relaxed);
    meter.analyze(l, r, frames);
}

} // namespace echo
