// DeviceBackend implementation on top of WASAPI.
#pragma once

#include <memory>

#include "audio/AppSessions.h"
#include "audio/DeviceManager.h"
#include "audio/VirtualCables.h"
#include "session/DeviceBackend.h"

namespace echo::win {

class WasapiBackend final : public DeviceBackend {
public:
    WasapiBackend();
    ~WasapiBackend() override;
    bool init(std::string* error);

    std::shared_ptr<CaptureEndpoint> openCapture(const CaptureRequest& req, std::string* error) override;
    std::shared_ptr<RenderEndpoint> openRender(const RenderRequest& req, std::string* error) override;
    std::vector<BackendDevice> devices() override;
    uint64_t deviceGeneration() override { return dm_.generation(); }
    bool pollDeviceChanges() override;
    void rescan() override;
    std::string resolveDevice(const std::string& id, const std::string& name, bool render) override;
    std::string defaultDevice(bool render) override;
    std::string cablePartnerOfRender(const std::string& renderId) override;
    std::vector<BackendProcess> audioProcesses() override;
    bool processAlive(uint32_t pid) override;
    bool processLoopbackSupported() override;

    DeviceManager& deviceManager() { return dm_; }
    AppSessionMonitor& sessions() { return sessions_; }
    const std::vector<CablePair>& cablePairs() const { return pairs_; }
    // User-defined pairs (from the setup wizard's loopback test) extend the
    // automatically detected ones.
    void setExtraCablePairs(std::vector<CablePair> extra);

private:
    void refreshPairs();
    DeviceManager dm_;
    AppSessionMonitor sessions_;
    std::vector<CablePair> pairs_, extra_;
    double lastSessionRefresh_ = -10.0;
};

} // namespace echo::win
