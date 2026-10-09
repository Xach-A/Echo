// The user's mixer configuration ("session"). Owned and mutated on the UI
// thread only. Real-time parameters live in the runtime objects' atomics
// (StripRuntime/BusRuntime/Effect), which are the single source of truth
// for those values; everything else lives here.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "core/Mixer.h"
#include "core/Resampler.h"
#include "dsp/Effect.h"
#include "session/DeviceBackend.h"
#include "session/Hotkeys.h"

namespace echo {

struct SourceRef {
    SourceKind kind = SourceKind::None;
    std::string deviceId;
    std::string deviceName;   // fallback match when the ID changes
    uint32_t processId = 0;   // ProcessLoopback (runtime only)
    std::string processName;  // ProcessLoopback: executable name, persisted
    bool exclusive = false;

    std::string key() const;  // de-duplication key for shared sources
    bool operator==(const SourceRef& o) const { return key() == o.key(); }
};

struct OutputRef {
    std::string deviceId;
    std::string deviceName;
    bool exclusive = false;
    std::string key() const { return deviceId + (exclusive ? "|x" : ""); }
};

enum class StripGroup : int { Hardware = 0, Application = 1, Virtual = 2, Generator = 3 };
enum class BusKind : int { Physical = 0, Virtual = 1 };

const char* stripGroupName(StripGroup g);

struct StripModel {
    uint32_t id = 0;
    std::string name;
    uint32_t color = 0xFF6AA9FF; // ABGR (ImGui order)
    StripGroup group = StripGroup::Hardware;
    SourceRef source;
    // Reference signal for echo cancellation (usually the loopback of the
    // playback device). Kind None = no reference.
    SourceRef reference;
    std::shared_ptr<StripRuntime> rt = std::make_shared<StripRuntime>();
    std::vector<std::shared_ptr<Effect>> chain;

    // Runtime status (not persisted).
    std::string status;
    bool online = false;
};

struct BusModel {
    uint32_t id = 0;
    int slot = 0; // stable index used by strip sends
    std::string name;
    uint32_t color = 0xFFFFB86A;
    BusKind kind = BusKind::Physical;
    OutputRef output;
    std::shared_ptr<BusRuntime> rt = std::make_shared<BusRuntime>();
    std::vector<std::shared_ptr<Effect>> chain;

    std::string status;
    bool online = false;
    bool isClock = false; // drives the engine clock (runtime)
};

// A loopback cable assigned a role in Echo.
struct VirtualCableModel {
    std::string name;        // user-facing name, e.g. "Virtual Mic 1"
    std::string renderId, renderName;
    std::string captureId, captureName;
    bool verified = false;   // passed a loopback test
};

struct EngineSettings {
    int sampleRate = 48000;
    int blockSize = 480;     // frames per engine block
    ResamplerQuality resamplerQuality = ResamplerQuality::Normal;
    bool lowLatencyShared = false;
    bool feedbackProtection = true;
    uint32_t clockBusId = 0; // 0 = automatic (first online physical bus)
};

struct RecorderSettings {
    std::string folder;      // empty = default (Music\Echo Recordings)
    int bitDepth = 24;       // 16, 24, or 32 (float)
    int sampleRate = 0;      // 0 = engine rate
};

struct UiSettings {
    int theme = 0;           // 0 dark, 1 darker, 2 light, 3 match Windows (light/dark)
    float uiScale = 1.0f;    // multiplier on top of DPI scale
    int windowX = -1, windowY = -1, windowW = 1480, windowH = 900;
    bool maximized = false;
    bool showTooltips = true;
    bool compactStrips = false;
    std::string accent = "blue"; // blue, coral, green, violet
    // Closing the window keeps Echo running in the notification area.
    bool closeToTray = false;
};

// Version 2: noise suppression became a default part of microphone inputs.
constexpr int kSessionConfigVersion = 2;

struct Session {
    int configVersion = kSessionConfigVersion;
    std::vector<StripModel> strips;
    std::vector<BusModel> buses;
    std::vector<VirtualCableModel> cables;
    EngineSettings engine;
    RecorderSettings recorder;
    UiSettings ui;
    HotkeySettings hotkeys;
    bool firstRunCompleted = false;
    // Add noise suppression to newly added microphone inputs (users can still
    // remove it from any input; this only affects new ones).
    bool autoNoiseSuppression = true;
    uint32_t nextId = 1;

    StripModel* strip(uint32_t id);
    BusModel* bus(uint32_t id);
    BusModel* busBySlot(int slot);
    int freeBusSlot() const;
};

} // namespace echo
