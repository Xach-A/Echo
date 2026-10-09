// Minimal streaming WAV writer (PCM 16/24-bit or 32-bit float) and reader
// (for tests and file playback). Not real-time safe: used on worker threads.
#pragma once

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace echo {

class WavWriter {
public:
    ~WavWriter();
    // bitDepth: 16, 24 (PCM) or 32 (IEEE float).
    bool open(const std::wstring& path, int sampleRate, int channels, int bitDepth);
    // Interleaved float frames.
    bool write(const float* interleaved, size_t frames);
    // Rewrites the header sizes (cheap); call periodically for crash safety.
    void updateHeader();
    void close();
    bool isOpen() const { return f_ != nullptr; }
    uint64_t bytesWritten() const { return dataBytes_; }
    uint64_t framesWritten() const { return frames_; }

private:
    FILE* f_ = nullptr;
    int sampleRate_ = 48000, channels_ = 2, bits_ = 24;
    uint64_t dataBytes_ = 0, frames_ = 0;
    std::vector<uint8_t> buf_;
};

struct WavData {
    int sampleRate = 0;
    int channels = 0;
    std::vector<float> samples; // interleaved
};

bool readWav(const std::wstring& path, WavData& out, std::string* error = nullptr);

} // namespace echo
