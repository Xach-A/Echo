// Per-application audio sessions (what the Windows volume mixer shows).
// Used for the Applications page: list apps that are playing audio, their
// volume/mute/level, and to pick a process for per-app capture.
#pragma once

#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <audiopolicy.h>
#include <endpointvolume.h>

#include "audio/WinUtil.h"

// Some SDKs (mingw-w64) only forward-declare IAudioMeterInformation; define
// it from the documented Windows interface when missing.
#if !defined(__IAudioMeterInformation_INTERFACE_DEFINED__) && defined(__MINGW32__)
#define __IAudioMeterInformation_INTERFACE_DEFINED__
struct IAudioMeterInformation : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE GetPeakValue(float* pfPeak) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetMeteringChannelCount(UINT* pnChannelCount) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetChannelsPeakValues(UINT32 u32ChannelCount, float* afPeakValues) = 0;
    virtual HRESULT STDMETHODCALLTYPE QueryHardwareSupport(DWORD* pdwHardwareSupportMask) = 0;
};
__CRT_UUID_DECL(IAudioMeterInformation, 0xC02216F6, 0x8C67, 0x4B5B, 0x9D, 0x00, 0xD0, 0x08, 0xE7, 0x3E, 0x00, 0x64)
#endif

namespace echo::win {

struct AppSession {
    uint32_t pid = 0;
    std::string exeName;       // "spotify.exe"
    std::wstring exePath;
    std::string displayName;   // friendly name (file description) or exe name
    std::string deviceId;      // render endpoint the session plays on
    std::string deviceName;
    bool active = false;       // currently playing
    bool systemSounds = false;
    float volume = 1.0f;
    bool muted = false;
    float peak = 0.0f;         // current peak (0..1), from the session meter
};

class AppSessionMonitor {
public:
    // Re-enumerates sessions on all active render endpoints. Call on the UI
    // thread (COM initialised), roughly once per second.
    void refresh();
    std::vector<AppSession> sessions() const;
    // Updates peaks/volumes of known sessions (cheap; call every frame).
    void updateLevels();

    bool setVolume(uint32_t pid, const std::string& deviceId, float volume);
    bool setMute(uint32_t pid, const std::string& deviceId, bool mute);

    static std::string processExeName(uint32_t pid, std::wstring* fullPath = nullptr);
    static std::string fileDescription(const std::wstring& path);

private:
    struct Entry {
        AppSession info;
        ComPtr<ISimpleAudioVolume> volume;
        ComPtr<IAudioMeterInformation> meter;
    };
    mutable std::mutex mutex_;
    std::vector<Entry> entries_;
};

} // namespace echo::win
