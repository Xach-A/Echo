// Multi-target WAV recorder. The engine copies tapped signals into lock-free
// rings (RecordTap); a background writer thread drains them to disk, so the
// audio thread never touches the file system.
#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "core/Mixer.h"
#include "core/Resampler.h"
#include "core/WavFile.h"

namespace echo {

struct RecordingTarget {
    int kind = RecordTap::Bus; // RecordTap::Kind
    uint32_t objectId = 0;     // strip or bus id
    std::string label;         // used in the file name
};

struct RecordingStatus {
    bool active = false;
    double seconds = 0.0;
    uint64_t bytes = 0;
    uint64_t droppedFrames = 0;
    std::vector<std::wstring> files;
    std::string error;
};

// A recording found in the recordings folder (newest first).
struct RecordingFile {
    std::wstring path;
    std::string name;      // file name, UTF-8
    int64_t modified = 0;  // seconds since 1970 (local display is the UI's job)
    double seconds = 0.0;  // 0 if the header could not be read
    uint64_t bytes = 0;
};
std::vector<RecordingFile> listRecordings(const std::wstring& folder, size_t maxCount);

class Recorder {
public:
    ~Recorder();

    // Starts recording. Creates one WAV per target. Taps are returned through
    // taps() so the controller can add them to the engine graph.
    bool start(const std::vector<RecordingTarget>& targets, const std::wstring& folder, int engineRate,
               int fileRate, int bitDepth, std::string* error);
    void stop();
    bool active() const { return active_.load(); }

    struct ActiveTap {
        RecordingTarget target;
        std::shared_ptr<RecordTap> tap;
    };
    const std::vector<ActiveTap>& taps() const { return taps_; }
    RecordingStatus status() const;

private:
    void writerMain();

    struct Writer {
        std::shared_ptr<RecordTap> tap;
        WavWriter wav;
        std::unique_ptr<Resampler> rs;
        std::vector<float> in, out;
    };
    std::vector<ActiveTap> taps_;
    std::vector<std::unique_ptr<Writer>> writers_;
    std::vector<std::wstring> files_;
    std::thread thread_;
    std::atomic<bool> active_{false};
    std::atomic<bool> stopRequested_{false};
    std::atomic<uint64_t> frames_{0};
    std::atomic<uint64_t> bytes_{0};
    int fileRate_ = 48000;
    double ratio_ = 1.0;
    mutable std::mutex mutex_;
    std::string error_;
};

} // namespace echo
