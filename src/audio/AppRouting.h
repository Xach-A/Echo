// Per-application output device ("App volume and device preferences").
//
// EXPERIMENTAL: Windows exposes this setting in Settings > System > Sound >
// Volume mixer, but not through a documented API. This uses the same
// internal interface as that page (also used by open-source tools such as
// EarTrumpet). It is only invoked on an explicit user action, can be undone
// by choosing "System default", and Echo falls back to opening the Windows
// settings page when the interface is unavailable.
#pragma once

#include <cstdint>
#include <string>

namespace echo::win {

class AppOutputRouting {
public:
    AppOutputRouting();
    ~AppOutputRouting();
    bool available() const { return factory_ != nullptr; }
    // renderDeviceId: MMDevice endpoint ID; empty resets the app to default.
    bool setAppOutput(uint32_t pid, const std::string& renderDeviceId, std::string* error);
    // Returns the MMDevice ID the app is pinned to, or "" for default.
    std::string appOutput(uint32_t pid) const;

private:
    void* factory_ = nullptr; // IUnknown-derived, vtable-addressed
};

} // namespace echo::win
