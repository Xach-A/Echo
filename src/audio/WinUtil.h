// Small Windows/COM helpers shared by the audio platform layer.
#pragma once

#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <mmreg.h>
#include <wrl/client.h>

#include <string>

namespace echo::win {

using Microsoft::WRL::ComPtr;

std::string toUtf8(const std::wstring& w);
std::string toUtf8(const wchar_t* w);
std::wstring fromUtf8(const std::string& s);
std::string hrToString(HRESULT hr);

// RAII COM apartment initialisation for the current thread.
class ComScope {
public:
    explicit ComScope(DWORD flags = COINIT_MULTITHREADED);
    ~ComScope();
    bool ok() const { return SUCCEEDED(hr_); }

private:
    HRESULT hr_;
};

enum class SampleKind { Float32, Int16, Int24, Int32, Unknown };

struct DeviceFormat {
    int channels = 0;
    int sampleRate = 0;
    int containerBits = 0;
    int validBits = 0;
    int blockAlign = 0;
    SampleKind kind = SampleKind::Unknown;
};

DeviceFormat describeFormat(const WAVEFORMATEX* wf);
std::string formatToString(const DeviceFormat& f);

// Convert interleaved device samples to/from float. `samples` counts
// individual samples (frames * channels). Output to integer formats is
// clamped (TPDF dither is not applied; float devices are the common case).
void deviceToFloat(const BYTE* src, float* dst, size_t samples, const DeviceFormat& f);
void floatToDevice(const float* src, BYTE* dst, size_t samples, const DeviceFormat& f);

// Builds a WAVEFORMATEXTENSIBLE for the given parameters.
WAVEFORMATEXTENSIBLE makeFormat(int sampleRate, int channels, SampleKind kind);

} // namespace echo::win
