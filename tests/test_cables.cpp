// Virtual-cable pairing (Windows device layer, no audio streams opened).
#include "doctest/doctest.h"

#include <mmdeviceapi.h>

#include "audio/VirtualCables.h"

using namespace echo;
using namespace echo::win;

namespace {

AudioDeviceInfo cableEndpoint(const char* id, const char* description, const char* iface, Flow flow,
                              const char* container) {
    AudioDeviceInfo d;
    d.idUtf8 = id;
    d.description = description;
    d.interfaceName = iface;
    d.name = std::string(description) + " (" + iface + ")";
    d.containerId = container;
    d.flow = flow;
    d.state = DEVICE_STATE_ACTIVE;
    d.virtualKind = VirtualKind::Cable;
    d.virtualVendor = iface;
    return d;
}

} // namespace

TEST_CASE("virtual cables: VB-CABLE's standard endpoint is the cable; its 16-channel endpoint is a secondary route") {
    // As enumerated on a real machine with VB-CABLE 2024+: the 16-channel
    // playback endpoint comes first and all three share one container.
    const char* vb = "VB-Audio Virtual Cable";
    std::vector<AudioDeviceInfo> devs = {
        cableEndpoint("in16", "CABLE In 16ch", vb, Flow::Render, "{vb}"),
        cableEndpoint("in", "CABLE Input", vb, Flow::Render, "{vb}"),
        cableEndpoint("out", "CABLE Output", vb, Flow::Capture, "{vb}"),
        // A second, independent cable (VB-CABLE A+B pack).
        cableEndpoint("a_in", "CABLE-A Input", "VB-Audio Cable A", Flow::Render, "{a}"),
        cableEndpoint("a_out", "CABLE-A Output", "VB-Audio Cable A", Flow::Capture, "{a}"),
    };
    const auto pairs = findCablePairs(devs);
    REQUIRE(pairs.size() == 3);
    // Main pairs first, in enumeration order.
    CHECK(pairs[0].renderId == "in");
    CHECK(pairs[0].captureId == "out");
    CHECK_FALSE(pairs[0].secondary);
    CHECK(pairs[1].renderId == "a_in");
    CHECK(pairs[1].captureId == "a_out");
    CHECK_FALSE(pairs[1].secondary);
    // The 16-channel endpoint also feeds CABLE Output: kept for feedback detection.
    CHECK(pairs[2].renderId == "in16");
    CHECK(pairs[2].captureId == "out");
    CHECK(pairs[2].secondary);
}

TEST_CASE("virtual cables: endpoints of different drivers are never paired") {
    std::vector<AudioDeviceInfo> devs = {
        cableEndpoint("x_in", "Line 1", "Virtual Audio Cable", Flow::Render, "{x}"),
        cableEndpoint("y_out", "CABLE Output", "VB-Audio Virtual Cable", Flow::Capture, "{y}"),
    };
    CHECK(findCablePairs(devs).empty());
}
