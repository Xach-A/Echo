#include "audio/DeviceManager.h"

#include <functiondiscoverykeys_devpkey.h>
#include <propvarutil.h>

#include <algorithm>

#include "core/Log.h"

namespace echo::win {

namespace {

// Defined locally: the import libraries of some SDKs do not export these.
const PROPERTYKEY kFormFactorKey = {{0x1da5d803, 0xd492, 0x4edd, {0x8c, 0x23, 0xe0, 0xc0, 0xff, 0xee, 0x7f, 0x0e}}, 0};
const PROPERTYKEY kDeviceFormatKey = {{0xf19f064d, 0x082c, 0x4e27, {0xbc, 0x73, 0x68, 0x82, 0xa1, 0xbb, 0x8e, 0x4c}}, 0};

std::string propString(IPropertyStore* ps, const PROPERTYKEY& key) {
    PROPVARIANT pv;
    PropVariantInit(&pv);
    std::string out;
    if (SUCCEEDED(ps->GetValue(key, &pv))) {
        if (pv.vt == VT_LPWSTR && pv.pwszVal) out = toUtf8(pv.pwszVal);
        else if (pv.vt == VT_CLSID && pv.puuid) {
            wchar_t buf[64];
            StringFromGUID2(*pv.puuid, buf, 64);
            out = toUtf8(buf);
        }
    }
    PropVariantClear(&pv);
    return out;
}

int propUInt(IPropertyStore* ps, const PROPERTYKEY& key, int def) {
    PROPVARIANT pv;
    PropVariantInit(&pv);
    int out = def;
    if (SUCCEEDED(ps->GetValue(key, &pv)) && pv.vt == VT_UI4) out = static_cast<int>(pv.ulVal);
    PropVariantClear(&pv);
    return out;
}

bool containsI(const std::string& hay, const char* needle) {
    std::string h = hay, n = needle;
    std::transform(h.begin(), h.end(), h.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    std::transform(n.begin(), n.end(), n.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return h.find(n) != std::string::npos;
}

} // namespace

class DeviceManager::Notifier final : public IMMNotificationClient {
public:
    explicit Notifier(DeviceManager* owner) : owner_(owner) {}
    ULONG STDMETHODCALLTYPE AddRef() override { return ++ref_; }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG r = --ref_;
        if (r == 0) delete this;
        return r;
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IMMNotificationClient)) {
            *ppv = static_cast<IMMNotificationClient*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    HRESULT STDMETHODCALLTYPE OnDeviceStateChanged(LPCWSTR, DWORD) override { ping(); return S_OK; }
    HRESULT STDMETHODCALLTYPE OnDeviceAdded(LPCWSTR) override { ping(); return S_OK; }
    HRESULT STDMETHODCALLTYPE OnDeviceRemoved(LPCWSTR) override { ping(); return S_OK; }
    HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged(EDataFlow, ERole, LPCWSTR) override { ping(); return S_OK; }
    HRESULT STDMETHODCALLTYPE OnPropertyValueChanged(LPCWSTR, const PROPERTYKEY key) override {
        // Format changes (user changed the default format in Sound settings)
        // matter; most other property churn does not.
        if (key == kDeviceFormatKey || key == PKEY_Device_FriendlyName) ping();
        return S_OK;
    }
    void detach() { owner_.store(nullptr); }

private:
    void ping() {
        if (DeviceManager* o = owner_.load()) o->onNotification();
    }
    std::atomic<ULONG> ref_{1};
    std::atomic<DeviceManager*> owner_;
};

DeviceManager::DeviceManager() = default;

DeviceManager::~DeviceManager() { shutdown(); }

bool DeviceManager::init(std::string* error) {
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                  IID_PPV_ARGS(&enumerator_));
    if (FAILED(hr)) {
        if (error) *error = "Cannot create MMDeviceEnumerator: " + hrToString(hr);
        ECHO_LOG_ERROR("Cannot create MMDeviceEnumerator: %s", hrToString(hr).c_str());
        return false;
    }
    notifier_ = new Notifier(this);
    hr = enumerator_->RegisterEndpointNotificationCallback(notifier_);
    if (FAILED(hr)) ECHO_LOG_WARN("Endpoint notifications unavailable: %s", hrToString(hr).c_str());
    rescan();
    return true;
}

void DeviceManager::shutdown() {
    if (enumerator_ && notifier_) enumerator_->UnregisterEndpointNotificationCallback(notifier_);
    if (notifier_) {
        notifier_->detach();
        notifier_->Release();
        notifier_ = nullptr;
    }
    enumerator_.Reset();
}

void DeviceManager::onNotification() {
    notifications_.fetch_add(1);
    dirty_.store(true);
}

bool DeviceManager::refreshIfChanged() {
    if (!dirty_.exchange(false)) return false;
    rescan();
    return true;
}

VirtualKind DeviceManager::classify(const AudioDeviceInfo& d, std::string* vendor) {
    const std::string& iface = d.interfaceName;
    const std::string& name = d.name;
    struct Sig {
        const char* match;
        VirtualKind kind;
        const char* vendor;
    };
    // Loopback cables: what is played into the render side can be recorded
    // from the matching capture side, with no other app involved.
    static const Sig sigs[] = {
        {"VB-Audio Virtual Cable", VirtualKind::Cable, "VB-Audio Virtual Cable"},
        {"VB-Audio Cable", VirtualKind::Cable, "VB-Audio Cable A/B/C/D"},
        {"VB-Audio Hi-Fi Cable", VirtualKind::Cable, "VB-Audio Hi-Fi Cable"},
        {"Virtual Audio Cable", VirtualKind::Cable, "Virtual Audio Cable (VAC)"},
        {"Virtual Cable", VirtualKind::Cable, "Virtual Cable"},
        {"Echo Virtual Audio", VirtualKind::Cable, "Echo Virtual Audio"},
        // Virtual devices bound to another application's engine.
        {"Voicemeeter", VirtualKind::AppBound, "Voicemeeter (requires Voicemeeter running)"},
        {"Voicemod", VirtualKind::AppBound, "Voicemod"},
        {"NVIDIA Broadcast", VirtualKind::AppBound, "NVIDIA Broadcast"},
        {"Steam Streaming", VirtualKind::AppBound, "Steam"},
        {"Elgato", VirtualKind::AppBound, "Elgato Wave Link"},
        {"SteelSeries Sonar", VirtualKind::AppBound, "SteelSeries Sonar"},
        {"NVIDIA Virtual Audio", VirtualKind::AppBound, "NVIDIA"},
    };
    for (const auto& s : sigs) {
        if (containsI(iface, s.match) || containsI(name, s.match)) {
            if (vendor) *vendor = s.vendor;
            return s.kind;
        }
    }
    return VirtualKind::None;
}

void DeviceManager::rescan() {
    if (!enumerator_) return;
    std::vector<AudioDeviceInfo> list;
    std::wstring defIds[2][2];
    for (int f = 0; f < 2; ++f) {
        for (int r = 0; r < 2; ++r) {
            ComPtr<IMMDevice> dev;
            if (SUCCEEDED(enumerator_->GetDefaultAudioEndpoint(f == 0 ? eRender : eCapture,
                                                              r == 0 ? eConsole : eCommunications, &dev))) {
                LPWSTR id = nullptr;
                if (SUCCEEDED(dev->GetId(&id)) && id) {
                    defIds[f][r] = id;
                    CoTaskMemFree(id);
                }
            }
        }
    }
    for (int f = 0; f < 2; ++f) {
        ComPtr<IMMDeviceCollection> coll;
        const DWORD mask = DEVICE_STATE_ACTIVE | DEVICE_STATE_UNPLUGGED | DEVICE_STATE_DISABLED;
        if (FAILED(enumerator_->EnumAudioEndpoints(f == 0 ? eRender : eCapture, mask, &coll))) continue;
        UINT count = 0;
        coll->GetCount(&count);
        for (UINT i = 0; i < count; ++i) {
            ComPtr<IMMDevice> dev;
            if (FAILED(coll->Item(i, &dev))) continue;
            AudioDeviceInfo info;
            info.flow = f == 0 ? Flow::Render : Flow::Capture;
            LPWSTR id = nullptr;
            if (FAILED(dev->GetId(&id)) || !id) continue;
            info.id = id;
            CoTaskMemFree(id);
            info.idUtf8 = toUtf8(info.id);
            dev->GetState(&info.state);
            ComPtr<IPropertyStore> ps;
            if (SUCCEEDED(dev->OpenPropertyStore(STGM_READ, &ps))) {
                info.name = propString(ps.Get(), PKEY_Device_FriendlyName);
                info.description = propString(ps.Get(), PKEY_Device_DeviceDesc);
                info.interfaceName = propString(ps.Get(), PKEY_DeviceInterface_FriendlyName);
                info.containerId = propString(ps.Get(), PKEY_Device_ContainerId);
                info.formFactor = propUInt(ps.Get(), kFormFactorKey, -1);
                PROPVARIANT pv;
                PropVariantInit(&pv);
                if (SUCCEEDED(ps->GetValue(kDeviceFormatKey, &pv)) && pv.vt == VT_BLOB &&
                    pv.blob.cbSize >= sizeof(WAVEFORMATEX))
                    info.mixFormat = describeFormat(reinterpret_cast<WAVEFORMATEX*>(pv.blob.pBlobData));
                PropVariantClear(&pv);
            }
            if (info.name.empty()) info.name = "(unnamed device)";
            info.isDefault = info.id == defIds[f][0];
            info.isDefaultComm = info.id == defIds[f][1];
            info.virtualKind = classify(info, &info.virtualVendor);
            list.push_back(std::move(info));
        }
    }
    std::sort(list.begin(), list.end(), [](const AudioDeviceInfo& a, const AudioDeviceInfo& b) {
        if (a.flow != b.flow) return a.flow < b.flow;
        if (a.active() != b.active()) return a.active();
        return a.name < b.name;
    });
    {
        std::lock_guard<std::mutex> lock(mutex_);
        devices_ = std::move(list);
    }
    generation_.fetch_add(1);
}

std::vector<AudioDeviceInfo> DeviceManager::devices() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return devices_;
}

std::vector<AudioDeviceInfo> DeviceManager::devices(Flow flow, bool activeOnly) const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<AudioDeviceInfo> out;
    for (const auto& d : devices_)
        if (d.flow == flow && (!activeOnly || d.active())) out.push_back(d);
    return out;
}

std::optional<AudioDeviceInfo> DeviceManager::find(const std::string& idUtf8) const {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& d : devices_)
        if (d.idUtf8 == idUtf8) return d;
    return std::nullopt;
}

std::optional<AudioDeviceInfo> DeviceManager::findByName(Flow flow, const std::string& name) const {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& d : devices_)
        if (d.flow == flow && d.active() && d.name == name) return d;
    return std::nullopt;
}

std::string DeviceManager::defaultDeviceId(Flow flow, bool communications) const {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& d : devices_)
        if (d.flow == flow && (communications ? d.isDefaultComm : d.isDefault)) return d.idUtf8;
    return {};
}

} // namespace echo::win
