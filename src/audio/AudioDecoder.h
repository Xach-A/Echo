// Reads a sound file for the soundboard: WAV with Echo's own reader, and
// MP3, M4A/AAC, WMA, FLAC... with Windows Media Foundation (part of
// Windows; the formats depend on the codecs installed).
#pragma once

#include <string>
#include <vector>

namespace echo::win {

struct DecodedAudio {
    int sampleRate = 0;
    int channels = 0;
    std::vector<float> samples; // interleaved
    bool truncated = false;     // longer than maxSeconds: the rest was dropped
};

// Call on a thread with COM initialized. maxSeconds <= 0: no limit.
bool decodeAudioFile(const std::wstring& path, double maxSeconds, DecodedAudio& out, std::string* error);

// File-dialog filter for the formats above (double-NUL terminated).
const wchar_t* audioFileFilter();

} // namespace echo::win
