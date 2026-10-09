// Enumerates Windows audio endpoints and tracks hot-plug / default-device
// changes through IMMNotificationClient.
#pragma once

#include <atomic>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "audio/WinUtil.h"

namespace echo::win {

enum class Flow { Render = 0, Capture = 1 };

// How a device relates to virtual audio.
enum class VirtualKind {
    None = 0,      // physical hardware (or unknown)
    Cable = 1,     // loopback cable: audio written to its render side appears on its capture side
    AppBound = 2,  // virtual device owned by another app (Voicemeeter, Voicemod, ...)
};

struct AudioDeviceInfo {
    std::wstring id;
    std::string idUtf8;
    std::string name;           // "Speakers (Realtek(R) Audio)"
    std::string description;    // "Speakers"
    std::string interfaceName;  // "Realtek(R) Audio" (adapter / driver name)
    std::string containerId;
    Flow flow = Flow::Render;
    DWORD state = 0;            // DEVICE_STATE_*
    bool isDefault = false;     // default console device
    bool isDefaultComm = false; // default communications device
    DeviceFormat mixFormat;
    int formFactor = -1;
    VirtualKind virtualKind = VirtualKind::None;
    std::string virtualVendor;  // e.g. "VB-Audio Virtual Cable"

    bool active() const { return state == DEVICE_STATE_ACTIVE; }
};

class DeviceManager {
public:
    DeviceManager();
    ~DeviceManager();

    // Must be called on a thread with COM initialised.
    bool init(std::string* error = nullptr);
    void shutdown();

    // Rescans if a notification arrived since the last scan. Returns true
    // when the device list changed. Call from the UI/controller thread.
    bool refreshIfChanged();
    void rescan();

    std::vector<AudioDeviceInfo> devices() const;
    std::vector<AudioDeviceInfo> devices(Flow flow, bool activeOnly = true) const;
    std::optional<AudioDeviceInfo> find(const std::string& idUtf8) const;
    // Fallback lookup by friendly name (device IDs can change after driver
    // reinstalls).
    std::optional<AudioDeviceInfo> findByName(Flow flow, const std::string& name) const;
    std::string defaultDeviceId(Flow flow, bool communications = false) const;

    uint64_t generation() const { return generation_.load(); }
    uint64_t notificationCount() const { return notifications_.load(); }

    static VirtualKind classify(const AudioDeviceInfo& d, std::string* vendor);

private:
    class Notifier;
    friend class Notifier;
    void onNotification();

    mutable std::mutex mutex_;
    std::vector<AudioDeviceInfo> devices_;
    ComPtr<IMMDeviceEnumerator> enumerator_;
    Notifier* notifier_ = nullptr;
    std::atomic<bool> dirty_{true};
    std::atomic<uint64_t> generation_{0};
    std::atomic<uint64_t> notifications_{0};
};

} // namespace echo::win
