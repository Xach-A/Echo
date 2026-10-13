// The soundboard as an audio source: plays sound clips (polyphonic, stereo)
// into the mix like any input, so the mixer's strip controls its volume and
// where it goes (virtual mic, headphones).
//
// Clips live in an immutable SoundBank built on the UI thread. setBank()
// swaps it; the controller then publishes a graph whose snapshot keeps the
// new bank alive, while the retired snapshot keeps the old one alive until
// the engine has moved on (sounds of pads that left the bank fade out within
// the block that notices the swap). Play/stop requests come from any thread
// (UI clicks, the hotkey thread) through a small queue the audio thread only
// try-locks, so it never waits.
#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <vector>

#include "core/Sources.h"

namespace echo {

struct SoundClip {
    std::vector<float> samples; // interleaved stereo at the engine rate
    size_t frames() const { return samples.size() / 2; }
};

struct SoundBank {
    struct Pad {
        std::shared_ptr<const SoundClip> clip;
        float gain = 1.0f;
    };
    std::vector<Pad> pads; // index = pad slot (position on the soundboard)
};

class SoundboardSource final : public SourceRuntime {
public:
    static constexpr int kMaxPads = 64;
    static constexpr int kMaxVoices = 24;

    SoundboardSource();
    void prepare(double engineRate, int maxBlock, double blockSeconds) override;
    void pull(int frames) override;

    // UI thread. Publish a graph afterwards (MixerController::setSoundBank).
    void setBank(std::shared_ptr<const SoundBank> bank);
    const std::shared_ptr<const SoundBank>& bank() const { return bank_; }

    // Any thread.
    void play(int pad);   // from the start; a copy already playing fades out
    void stop(int pad);   // fades out
    void toggle(int pad); // stops it if playing, else plays it
    void stopAll();

    // UI: position (0..1) of the pad's newest sound, -1 when it is not playing.
    float progress(int pad) const;
    // Sounds playing right now (UI).
    int playingCount() const { return playing_.load(std::memory_order_relaxed); }

private:
    enum class Cmd : uint8_t { Play, Stop, Toggle, StopAll };
    struct Request {
        Cmd cmd;
        int pad;
    };
    struct Voice {
        const SoundClip* clip = nullptr;
        int pad = -1;
        size_t pos = 0;
        float gain = 1.0f;
        float fade = 0.0f;     // 0..1
        float fadeStep = 0.0f; // per frame; negative while stopping
        uint64_t started = 0;
        bool active = false;
        bool stopping() const { return fadeStep < 0.0f; }
    };

    void push(Cmd c, int pad);
    void startVoice(int pad);
    void stopPad(int pad);
    bool padPlaying(int pad) const;

    std::shared_ptr<const SoundBank> bank_; // UI thread
    std::atomic<const SoundBank*> bankRt_{nullptr};
    const SoundBank* seenBank_ = nullptr; // audio thread

    std::mutex queueMutex_;
    Request queue_[128];
    int queued_ = 0;

    Voice voices_[kMaxVoices];
    uint64_t counter_ = 0;
    float fadeIn_ = 0.0f, fadeOut_ = 0.0f; // per-frame steps
    std::atomic<float> progress_[kMaxPads];
    std::atomic<int> playing_{0};
};

} // namespace echo
