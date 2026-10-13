#include "audio/AudioDecoder.h"

#include <windows.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>

#include <algorithm>
#include <cstring>

#include "core/Text.h"
#include "core/WavFile.h"

namespace echo::win {

namespace {

template <typename T>
struct ComPtr {
    T* p = nullptr;
    ~ComPtr() {
        if (p) p->Release();
    }
    T** operator&() { return &p; }
    T* operator->() const { return p; }
    explicit operator bool() const { return p != nullptr; }
};

std::string hrText(const char* what, HRESULT hr) {
    char buf[96];
    std::snprintf(buf, sizeof(buf), "%s (0x%08lX)", what, static_cast<unsigned long>(hr));
    return buf;
}

bool decodeWithMediaFoundation(const std::wstring& path, double maxSeconds, DecodedAudio& out, std::string* error) {
    HRESULT hr = MFStartup(MF_VERSION, MFSTARTUP_LITE);
    if (FAILED(hr)) {
        if (error) *error = hrText("Media Foundation is not available", hr);
        return false;
    }
    struct Shutdown {
        ~Shutdown() { MFShutdown(); }
    } shutdown;

    ComPtr<IMFSourceReader> reader;
    hr = MFCreateSourceReaderFromURL(path.c_str(), nullptr, &reader);
    if (FAILED(hr)) {
        if (error) *error = "This file type can't be read (or the file is not a sound file)";
        return false;
    }
    const DWORD stream = static_cast<DWORD>(MF_SOURCE_READER_FIRST_AUDIO_STREAM);
    reader->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_ALL_STREAMS), FALSE);
    reader->SetStreamSelection(stream, TRUE);

    // Ask for 32-bit float PCM; Media Foundation decodes to it.
    ComPtr<IMFMediaType> want;
    MFCreateMediaType(&want);
    want->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    want->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_Float);
    hr = reader->SetCurrentMediaType(stream, nullptr, want.p);
    if (FAILED(hr)) {
        if (error) *error = hrText("No decoder for this file's sound", hr);
        return false;
    }
    ComPtr<IMFMediaType> got;
    hr = reader->GetCurrentMediaType(stream, &got);
    if (FAILED(hr)) {
        if (error) *error = hrText("Could not read the sound format", hr);
        return false;
    }
    UINT32 rate = 0, channels = 0;
    got->GetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, &rate);
    got->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS, &channels);
    if (!rate || !channels) {
        if (error) *error = "Could not read the sound format";
        return false;
    }
    out.sampleRate = static_cast<int>(rate);
    out.channels = static_cast<int>(channels);
    const size_t maxSamples = maxSeconds > 0.0 ? static_cast<size_t>(maxSeconds * rate) * channels : SIZE_MAX;
    for (;;) {
        DWORD flags = 0;
        ComPtr<IMFSample> sample;
        hr = reader->ReadSample(stream, 0, nullptr, &flags, nullptr, &sample);
        if (FAILED(hr)) {
            if (out.samples.empty()) {
                if (error) *error = hrText("Decoding failed", hr);
                return false;
            }
            break; // keep what was decoded
        }
        if (flags & MF_SOURCE_READERF_ENDOFSTREAM) break;
        if (!sample) continue;
        ComPtr<IMFMediaBuffer> buffer;
        if (FAILED(sample->ConvertToContiguousBuffer(&buffer))) continue;
        BYTE* data = nullptr;
        DWORD bytes = 0;
        if (FAILED(buffer->Lock(&data, nullptr, &bytes))) continue;
        const size_t n = bytes / sizeof(float);
        const size_t take = std::min(n, maxSamples - std::min(maxSamples, out.samples.size()));
        const size_t at = out.samples.size();
        out.samples.resize(at + take);
        std::memcpy(out.samples.data() + at, data, take * sizeof(float));
        buffer->Unlock();
        if (take < n || out.samples.size() >= maxSamples) {
            out.truncated = true;
            break;
        }
    }
    if (out.samples.empty()) {
        if (error) *error = "The file has no sound";
        return false;
    }
    return true;
}

} // namespace

bool decodeAudioFile(const std::wstring& path, double maxSeconds, DecodedAudio& out, std::string* error) {
    out = DecodedAudio{};
    // WAV: Echo's own reader (no Media Foundation needed).
    std::wstring lower = path;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::towlower);
    if (lower.size() > 4 && lower.compare(lower.size() - 4, 4, L".wav") == 0) {
        WavData w;
        if (readWav(path, w, nullptr) && w.sampleRate > 0 && w.channels > 0 && !w.samples.empty()) {
            out.sampleRate = w.sampleRate;
            out.channels = w.channels;
            out.samples = std::move(w.samples);
            const size_t maxSamples = maxSeconds > 0.0 ? static_cast<size_t>(maxSeconds * out.sampleRate) * out.channels : SIZE_MAX;
            if (out.samples.size() > maxSamples) {
                out.samples.resize(maxSamples);
                out.truncated = true;
            }
            return true;
        }
        // Not a format Echo's reader knows (e.g. compressed WAV): try Media Foundation.
    }
    return decodeWithMediaFoundation(path, maxSeconds, out, error);
}

const wchar_t* audioFileFilter() {
    return L"Sound files (*.wav;*.mp3;*.m4a;*.aac;*.wma;*.flac)\0*.wav;*.mp3;*.m4a;*.aac;*.wma;*.flac\0All files (*.*)\0*.*\0";
}

} // namespace echo::win
