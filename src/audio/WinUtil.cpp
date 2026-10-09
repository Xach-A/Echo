#include "audio/WinUtil.h"

#include <ksmedia.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace echo::win {

std::string toUtf8(const std::wstring& w) { return toUtf8(w.c_str()); }

std::string toUtf8(const wchar_t* w) {
    if (!w || !*w) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    std::string s(n > 0 ? n - 1 : 0, '\0');
    if (n > 1) WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
    return s;
}

std::wstring fromUtf8(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w(n > 0 ? n - 1 : 0, L'\0');
    if (n > 1) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
    return w;
}

std::string hrToString(HRESULT hr) {
    switch (hr) {
    case AUDCLNT_E_DEVICE_INVALIDATED: return "device invalidated (unplugged or disabled)";
    case AUDCLNT_E_DEVICE_IN_USE: return "device in use (exclusive mode by another app)";
    case AUDCLNT_E_UNSUPPORTED_FORMAT: return "unsupported format";
    case AUDCLNT_E_EXCLUSIVE_MODE_NOT_ALLOWED: return "exclusive mode not allowed for this device";
    case AUDCLNT_E_ENDPOINT_CREATE_FAILED: return "endpoint create failed";
    case AUDCLNT_E_SERVICE_NOT_RUNNING: return "Windows Audio service not running";
    case AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED: return "buffer size not aligned";
    case E_ACCESSDENIED: return "access denied (check Windows microphone privacy settings)";
    case E_NOTFOUND: return "device not found";
    default: break;
    }
    char buf[256];
    wchar_t* msg = nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                   nullptr, static_cast<DWORD>(hr), 0, reinterpret_cast<LPWSTR>(&msg), 0, nullptr);
    std::string text = msg ? toUtf8(msg) : std::string();
    if (msg) LocalFree(msg);
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' ')) text.pop_back();
    std::snprintf(buf, sizeof(buf), "0x%08lX%s%s", static_cast<unsigned long>(hr), text.empty() ? "" : " ",
                  text.c_str());
    return buf;
}

ComScope::ComScope(DWORD flags) : hr_(CoInitializeEx(nullptr, flags)) {}
ComScope::~ComScope() {
    if (SUCCEEDED(hr_)) CoUninitialize();
}

DeviceFormat describeFormat(const WAVEFORMATEX* wf) {
    DeviceFormat f;
    if (!wf) return f;
    f.channels = wf->nChannels;
    f.sampleRate = static_cast<int>(wf->nSamplesPerSec);
    f.containerBits = wf->wBitsPerSample;
    f.validBits = wf->wBitsPerSample;
    f.blockAlign = wf->nBlockAlign;
    GUID sub = GUID_NULL;
    WORD tag = wf->wFormatTag;
    if (tag == WAVE_FORMAT_EXTENSIBLE && wf->cbSize >= 22) {
        const auto* ext = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(wf);
        sub = ext->SubFormat;
        f.validBits = ext->Samples.wValidBitsPerSample ? ext->Samples.wValidBitsPerSample : wf->wBitsPerSample;
        if (IsEqualGUID(sub, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT)) tag = WAVE_FORMAT_IEEE_FLOAT;
        else if (IsEqualGUID(sub, KSDATAFORMAT_SUBTYPE_PCM)) tag = WAVE_FORMAT_PCM;
    }
    if (tag == WAVE_FORMAT_IEEE_FLOAT && f.containerBits == 32) f.kind = SampleKind::Float32;
    else if (tag == WAVE_FORMAT_PCM && f.containerBits == 16) f.kind = SampleKind::Int16;
    else if (tag == WAVE_FORMAT_PCM && f.containerBits == 24) f.kind = SampleKind::Int24;
    else if (tag == WAVE_FORMAT_PCM && f.containerBits == 32) f.kind = SampleKind::Int32;
    return f;
}

std::string formatToString(const DeviceFormat& f) {
    const char* k = "?";
    switch (f.kind) {
    case SampleKind::Float32: k = "32-bit float"; break;
    case SampleKind::Int16: k = "16-bit"; break;
    case SampleKind::Int24: k = "24-bit"; break;
    case SampleKind::Int32: k = f.validBits == 24 ? "24-in-32-bit" : "32-bit"; break;
    default: break;
    }
    char buf[96];
    std::snprintf(buf, sizeof(buf), "%d Hz, %d ch, %s", f.sampleRate, f.channels, k);
    return buf;
}

void deviceToFloat(const BYTE* src, float* dst, size_t samples, const DeviceFormat& f) {
    switch (f.kind) {
    case SampleKind::Float32: std::memcpy(dst, src, samples * sizeof(float)); break;
    case SampleKind::Int16: {
        const auto* s = reinterpret_cast<const int16_t*>(src);
        for (size_t i = 0; i < samples; ++i) dst[i] = s[i] * (1.0f / 32768.0f);
        break;
    }
    case SampleKind::Int24: {
        for (size_t i = 0; i < samples; ++i) {
            const BYTE* p = src + i * 3;
            int32_t v = (static_cast<int32_t>(p[0]) << 8) | (static_cast<int32_t>(p[1]) << 16) |
                        (static_cast<int32_t>(p[2]) << 24);
            dst[i] = static_cast<float>(v) * (1.0f / 2147483648.0f);
        }
        break;
    }
    case SampleKind::Int32: {
        const auto* s = reinterpret_cast<const int32_t*>(src);
        for (size_t i = 0; i < samples; ++i) dst[i] = static_cast<float>(s[i]) * (1.0f / 2147483648.0f);
        break;
    }
    default: std::memset(dst, 0, samples * sizeof(float)); break;
    }
}

void floatToDevice(const float* src, BYTE* dst, size_t samples, const DeviceFormat& f) {
    switch (f.kind) {
    case SampleKind::Float32: std::memcpy(dst, src, samples * sizeof(float)); break;
    case SampleKind::Int16: {
        auto* d = reinterpret_cast<int16_t*>(dst);
        for (size_t i = 0; i < samples; ++i) {
            const float v = std::clamp(src[i], -1.0f, 1.0f) * 32767.0f;
            d[i] = static_cast<int16_t>(std::lrintf(v));
        }
        break;
    }
    case SampleKind::Int24: {
        for (size_t i = 0; i < samples; ++i) {
            const int32_t v = static_cast<int32_t>(std::lrint(std::clamp(src[i], -1.0f, 1.0f) * 8388607.0));
            BYTE* p = dst + i * 3;
            p[0] = static_cast<BYTE>(v & 0xFF);
            p[1] = static_cast<BYTE>((v >> 8) & 0xFF);
            p[2] = static_cast<BYTE>((v >> 16) & 0xFF);
        }
        break;
    }
    case SampleKind::Int32: {
        auto* d = reinterpret_cast<int32_t*>(dst);
        for (size_t i = 0; i < samples; ++i) {
            const double v = std::clamp(static_cast<double>(src[i]), -1.0, 1.0) * 2147483647.0;
            d[i] = static_cast<int32_t>(std::llrint(v));
        }
        break;
    }
    default: std::memset(dst, 0, samples * static_cast<size_t>(f.containerBits / 8)); break;
    }
}

WAVEFORMATEXTENSIBLE makeFormat(int sampleRate, int channels, SampleKind kind) {
    WAVEFORMATEXTENSIBLE wf{};
    int bits = 32, valid = 32;
    GUID sub = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
    switch (kind) {
    case SampleKind::Float32: break;
    case SampleKind::Int16: bits = valid = 16; sub = KSDATAFORMAT_SUBTYPE_PCM; break;
    case SampleKind::Int24: bits = valid = 24; sub = KSDATAFORMAT_SUBTYPE_PCM; break;
    case SampleKind::Int32: bits = 32; valid = 32; sub = KSDATAFORMAT_SUBTYPE_PCM; break;
    default: break;
    }
    wf.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    wf.Format.nChannels = static_cast<WORD>(channels);
    wf.Format.nSamplesPerSec = static_cast<DWORD>(sampleRate);
    wf.Format.wBitsPerSample = static_cast<WORD>(bits);
    wf.Format.nBlockAlign = static_cast<WORD>(channels * bits / 8);
    wf.Format.nAvgBytesPerSec = wf.Format.nSamplesPerSec * wf.Format.nBlockAlign;
    wf.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
    wf.Samples.wValidBitsPerSample = static_cast<WORD>(valid);
    wf.dwChannelMask = channels == 1 ? SPEAKER_FRONT_CENTER
                       : channels == 2 ? (SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT)
                                       : 0;
    wf.SubFormat = sub;
    return wf;
}

} // namespace echo::win
