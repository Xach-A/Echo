#include "audio/AppRouting.h"

#include <windows.h>
#include <mmdeviceapi.h>
#include <roapi.h>
#include <winstring.h>

#include "audio/WinUtil.h"
#include "core/Log.h"

namespace echo::win {

namespace {

// IAudioPolicyConfigFactory: IInspectable (6 slots) + 19 unused methods,
// then SetPersistedDefaultAudioEndpoint (25), GetPersistedDefaultAudioEndpoint
// (26), ClearAllPersistedApplicationDefaultEndpoints (27).
constexpr int kSetSlot = 25;
constexpr int kGetSlot = 26;

const GUID kIid21H2 = {0xab3d4648, 0xe242, 0x459f, {0xb0, 0x2f, 0x54, 0x1c, 0x70, 0x30, 0x63, 0x24}};
const GUID kIidDownlevel = {0x2a59116d, 0x6c4f, 0x45e0, {0xa7, 0x4f, 0x70, 0x7e, 0x3f, 0xef, 0x92, 0x58}};

const wchar_t* kRenderInterface = L"{e6327cad-dcec-4949-ae8a-991e976a79d2}";

using SetFn = HRESULT(STDMETHODCALLTYPE*)(void* self, UINT32 pid, EDataFlow flow, ERole role, HSTRING id);
using GetFn = HRESULT(STDMETHODCALLTYPE*)(void* self, UINT32 pid, EDataFlow flow, ERole role, HSTRING* id);

template <typename Fn>
Fn slot(void* obj, int index) {
    void** vtbl = *static_cast<void***>(obj);
    return reinterpret_cast<Fn>(vtbl[index]);
}

// "{0.0.0.00000000}.{guid}" -> "\\?\SWD#MMDEVAPI#{0.0.0.00000000}.{guid}#{interface}"
std::wstring toInterfacePath(const std::wstring& mmId) {
    return L"\\\\?\\SWD#MMDEVAPI#" + mmId + L"#" + kRenderInterface;
}

std::wstring fromInterfacePath(const std::wstring& path) {
    const std::wstring prefix = L"\\\\?\\SWD#MMDEVAPI#";
    if (path.rfind(prefix, 0) != 0) return path;
    std::wstring rest = path.substr(prefix.size());
    const auto hash = rest.find(L'#');
    return hash == std::wstring::npos ? rest : rest.substr(0, hash);
}

} // namespace

AppOutputRouting::AppOutputRouting() {
    HSTRING cls = nullptr;
    const wchar_t* name = L"Windows.Media.Internal.AudioPolicyConfig";
    if (FAILED(WindowsCreateString(name, static_cast<UINT32>(wcslen(name)), &cls))) return;
    void* f = nullptr;
    HRESULT hr = RoGetActivationFactory(cls, kIid21H2, &f);
    if (FAILED(hr)) hr = RoGetActivationFactory(cls, kIidDownlevel, &f);
    WindowsDeleteString(cls);
    if (SUCCEEDED(hr)) factory_ = f;
    else ECHO_LOG_INFO("Per-app output routing interface unavailable (%s)", hrToString(hr).c_str());
}

AppOutputRouting::~AppOutputRouting() {
    if (factory_) static_cast<IUnknown*>(factory_)->Release();
}

bool AppOutputRouting::setAppOutput(uint32_t pid, const std::string& renderDeviceId, std::string* error) {
    if (!factory_) {
        if (error) *error = "Not supported on this Windows version";
        return false;
    }
    HSTRING id = nullptr;
    if (!renderDeviceId.empty()) {
        const std::wstring path = toInterfacePath(fromUtf8(renderDeviceId));
        if (FAILED(WindowsCreateString(path.c_str(), static_cast<UINT32>(path.size()), &id))) {
            if (error) *error = "Out of memory";
            return false;
        }
    }
    auto set = slot<SetFn>(factory_, kSetSlot);
    HRESULT hr = set(factory_, pid, eRender, eConsole, id);
    if (SUCCEEDED(hr)) hr = set(factory_, pid, eRender, eMultimedia, id);
    if (id) WindowsDeleteString(id);
    if (FAILED(hr)) {
        if (error) *error = hrToString(hr);
        return false;
    }
    ECHO_LOG_INFO("Per-app output for PID %u set to %s", pid, renderDeviceId.empty() ? "default" : renderDeviceId.c_str());
    return true;
}

std::string AppOutputRouting::appOutput(uint32_t pid) const {
    if (!factory_) return {};
    HSTRING id = nullptr;
    auto get = slot<GetFn>(factory_, kGetSlot);
    if (FAILED(get(factory_, pid, eRender, eMultimedia, &id)) || !id) return {};
    UINT32 len = 0;
    const wchar_t* raw = WindowsGetStringRawBuffer(id, &len);
    std::wstring s(raw ? raw : L"", len);
    WindowsDeleteString(id);
    return toUtf8(fromInterfacePath(s));
}

} // namespace echo::win
