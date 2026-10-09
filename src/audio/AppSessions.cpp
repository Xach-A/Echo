#include "audio/AppSessions.h"

#include <audiopolicy.h>
#include <endpointvolume.h>
#include <functiondiscoverykeys_devpkey.h>
#include <psapi.h>

#include <algorithm>
#include <map>

namespace echo::win {

std::string AppSessionMonitor::processExeName(uint32_t pid, std::wstring* fullPath) {
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return {};
    wchar_t buf[MAX_PATH * 2];
    DWORD len = MAX_PATH * 2;
    std::string name;
    if (QueryFullProcessImageNameW(h, 0, buf, &len)) {
        std::wstring path(buf, len);
        if (fullPath) *fullPath = path;
        const auto slash = path.find_last_of(L"\\/");
        name = toUtf8(slash == std::wstring::npos ? path : path.substr(slash + 1));
    }
    CloseHandle(h);
    return name;
}

std::string AppSessionMonitor::fileDescription(const std::wstring& path) {
    DWORD dummy = 0;
    const DWORD size = GetFileVersionInfoSizeW(path.c_str(), &dummy);
    if (!size) return {};
    std::vector<BYTE> data(size);
    if (!GetFileVersionInfoW(path.c_str(), 0, size, data.data())) return {};
    struct LangCp {
        WORD lang, cp;
    }* tr = nullptr;
    UINT trLen = 0;
    if (!VerQueryValueW(data.data(), L"\\VarFileInfo\\Translation", reinterpret_cast<void**>(&tr), &trLen) || !trLen)
        return {};
    wchar_t key[96];
    swprintf(key, 96, L"\\StringFileInfo\\%04x%04x\\FileDescription", tr[0].lang, tr[0].cp);
    wchar_t* desc = nullptr;
    UINT descLen = 0;
    if (!VerQueryValueW(data.data(), key, reinterpret_cast<void**>(&desc), &descLen) || !desc) return {};
    return toUtf8(desc);
}

void AppSessionMonitor::refresh() {
    std::vector<Entry> fresh;
    ComPtr<IMMDeviceEnumerator> en;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&en)))) return;
    ComPtr<IMMDeviceCollection> coll;
    if (FAILED(en->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &coll))) return;
    UINT count = 0;
    coll->GetCount(&count);
    static std::map<uint32_t, std::pair<std::string, std::wstring>> exeCache;
    static std::map<std::wstring, std::string> descCache;
    const DWORD self = GetCurrentProcessId();
    for (UINT d = 0; d < count; ++d) {
        ComPtr<IMMDevice> dev;
        if (FAILED(coll->Item(d, &dev))) continue;
        LPWSTR idw = nullptr;
        dev->GetId(&idw);
        const std::string devId = idw ? toUtf8(idw) : std::string();
        CoTaskMemFree(idw);
        std::string devName;
        ComPtr<IPropertyStore> ps;
        if (SUCCEEDED(dev->OpenPropertyStore(STGM_READ, &ps))) {
            PROPVARIANT pv;
            PropVariantInit(&pv);
            if (SUCCEEDED(ps->GetValue(PKEY_Device_FriendlyName, &pv)) && pv.vt == VT_LPWSTR) devName = toUtf8(pv.pwszVal);
            PropVariantClear(&pv);
        }
        ComPtr<IAudioSessionManager2> mgr;
        if (FAILED(dev->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, nullptr,
                                 reinterpret_cast<void**>(mgr.GetAddressOf()))))
            continue;
        ComPtr<IAudioSessionEnumerator> se;
        if (FAILED(mgr->GetSessionEnumerator(&se))) continue;
        int n = 0;
        se->GetCount(&n);
        for (int i = 0; i < n; ++i) {
            ComPtr<IAudioSessionControl> ctl;
            if (FAILED(se->GetSession(i, &ctl))) continue;
            ComPtr<IAudioSessionControl2> ctl2;
            if (FAILED(ctl.As(&ctl2))) continue;
            AudioSessionState state = AudioSessionStateInactive;
            ctl->GetState(&state);
            if (state == AudioSessionStateExpired) continue;
            DWORD pid = 0;
            ctl2->GetProcessId(&pid);
            Entry e;
            e.info.pid = pid;
            e.info.systemSounds = ctl2->IsSystemSoundsSession() == S_OK;
            if (pid == self) continue; // never list Echo itself
            if (e.info.systemSounds) {
                e.info.displayName = "System sounds";
            } else {
                auto it = exeCache.find(pid);
                if (it == exeCache.end()) {
                    std::wstring path;
                    std::string exe = processExeName(pid, &path);
                    it = exeCache.emplace(pid, std::make_pair(exe, path)).first;
                }
                e.info.exeName = it->second.first;
                e.info.exePath = it->second.second;
                if (e.info.exeName.empty()) continue; // process gone or inaccessible
                auto dc = descCache.find(e.info.exePath);
                if (dc == descCache.end()) dc = descCache.emplace(e.info.exePath, fileDescription(e.info.exePath)).first;
                e.info.displayName = dc->second.empty() ? e.info.exeName : dc->second;
            }
            e.info.deviceId = devId;
            e.info.deviceName = devName;
            e.info.active = state == AudioSessionStateActive;
            ctl.As(&e.volume);
            ctl.As(&e.meter);
            if (e.volume) {
                float v = 1.0f;
                BOOL m = FALSE;
                e.volume->GetMasterVolume(&v);
                e.volume->GetMute(&m);
                e.info.volume = v;
                e.info.muted = m != FALSE;
            }
            fresh.push_back(std::move(e));
        }
    }
    std::sort(fresh.begin(), fresh.end(), [](const Entry& a, const Entry& b) {
        if (a.info.systemSounds != b.info.systemSounds) return !a.info.systemSounds;
        return a.info.displayName < b.info.displayName;
    });
    std::lock_guard<std::mutex> lock(mutex_);
    entries_ = std::move(fresh);
}

void AppSessionMonitor::updateLevels() {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& e : entries_) {
        if (e.meter) {
            float pk = 0.0f;
            if (SUCCEEDED(e.meter->GetPeakValue(&pk))) e.info.peak = pk;
        }
        if (e.volume) {
            float v = 1.0f;
            BOOL m = FALSE;
            if (SUCCEEDED(e.volume->GetMasterVolume(&v))) e.info.volume = v;
            if (SUCCEEDED(e.volume->GetMute(&m))) e.info.muted = m != FALSE;
        }
    }
}

std::vector<AppSession> AppSessionMonitor::sessions() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<AppSession> out;
    out.reserve(entries_.size());
    for (const auto& e : entries_) out.push_back(e.info);
    return out;
}

bool AppSessionMonitor::setVolume(uint32_t pid, const std::string& deviceId, float volume) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& e : entries_)
        if (e.info.pid == pid && e.info.deviceId == deviceId && e.volume)
            return SUCCEEDED(e.volume->SetMasterVolume(std::clamp(volume, 0.0f, 1.0f), nullptr));
    return false;
}

bool AppSessionMonitor::setMute(uint32_t pid, const std::string& deviceId, bool mute) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& e : entries_)
        if (e.info.pid == pid && e.info.deviceId == deviceId && e.volume)
            return SUCCEEDED(e.volume->SetMute(mute ? TRUE : FALSE, nullptr));
    return false;
}

} // namespace echo::win
