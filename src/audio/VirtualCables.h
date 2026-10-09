// Virtual audio cable integration.
//
// Echo does not ship a kernel driver. It works with any installed loopback
// cable driver (VB-CABLE, VB-Audio Cable A/B, Virtual Audio Cable, ...):
//   * Virtual microphone: Echo renders into the cable's playback side; other
//     apps record from the cable's recording side.
//   * Virtual speaker: other apps play into the cable's playback side; Echo
//     captures the cable's recording side.
// This file detects cable pairs and verifies them with a real loopback test.
#pragma once

#include <string>
#include <vector>

#include "audio/DeviceManager.h"

namespace echo::win {

struct CablePair {
    std::string renderId;    // playback side ("CABLE Input")
    std::string captureId;   // recording side ("CABLE Output")
    std::string renderName;
    std::string captureName;
    std::string vendor;
    std::string displayName; // short name, e.g. "VB-Audio Virtual Cable"
    // Another playback endpoint feeding an already-paired recording side
    // (VB-CABLE's "CABLE In 16ch" next to "CABLE Input"). Not offered as a
    // separate cable, but known to feedback protection.
    bool secondary = false;
};

// Pairs active cable endpoints by driver interface name, container ID and
// endpoint naming conventions. Main pairs come first (best match per
// recording side), then secondary playback endpoints.
std::vector<CablePair> findCablePairs(const std::vector<AudioDeviceInfo>& devices);

struct CableTestResult {
    bool ok = false;
    float levelDb = -120.0f;   // tone level measured on the recording side
    float latencyMs = 0.0f;    // approximate burst arrival delay
    std::string message;
};

// Plays a -20 dBFS 1 kHz tone into the render side for about one second and
// listens on the capture side. Blocks for ~1.2 s. COM must be initialised.
CableTestResult testCable(const CablePair& pair, int sampleRate);

// Generic loopback test between any render and capture endpoint (used by
// the setup wizard for "play into X, record from Y").
CableTestResult testLoopback(const std::string& renderId, const std::string& captureId);

} // namespace echo::win
