// Effect-chain presets (factory + user) and routing scene presets.
//
// Chain preset file (*.echopreset, JSON):
//   { "format": "echo.chain-preset", "version": 1, "name": "...",
//     "category": "...", "description": "...", "chain": [ <effect>... ] }
// See docs/FILE_FORMATS.md.
#pragma once

#include <string>
#include <vector>

#include "session/Serialization.h"

namespace echo {

class MixerController;

struct ChainPreset {
    std::string name;
    std::string category;
    std::string description;
    bool factory = false;
    json chain = json::array();
    std::wstring file; // user presets only
};

std::vector<ChainPreset> factoryChainPresets();

class PresetLibrary {
public:
    // Loads factory presets plus every *.echopreset in `userDir`.
    void load(const std::wstring& userDir);
    const std::vector<ChainPreset>& all() const { return presets_; }
    const ChainPreset* find(const std::string& name) const;

    bool saveUser(const std::string& name, const std::string& category, const std::string& description,
                  const json& chain, std::string* error);
    bool rename(const std::string& oldName, const std::string& newName, std::string* error);
    bool duplicate(const std::string& name, std::string* error);
    bool remove(const std::string& name, std::string* error);
    bool importFile(const std::wstring& path, std::string* error);
    bool exportPreset(const std::string& name, const std::wstring& path, std::string* error) const;

    static json toJson(const ChainPreset& p);
    static bool fromJson(const json& j, ChainPreset& out);

private:
    std::wstring fileFor(const std::string& name) const;
    std::wstring userDir_;
    std::vector<ChainPreset> presets_;
};

// Routing scenes: change sends (and only sends) to common setups.
enum class ScenePreset {
    VoiceToVirtualMic,      // mic -> virtual mic only
    VoicePlusMusicToMic,    // mic + app/desktop audio -> virtual mic; music also in headphones
    HeadphonesOnlyMusic,    // app/desktop audio -> headphones only, never the mic
    MonitorMyVoice,         // mic -> virtual mic and headphones (hear yourself)
    Count
};
const char* scenePresetName(ScenePreset p);
const char* scenePresetDescription(ScenePreset p);
// Returns a human-readable summary of what changed, or an error message.
std::string applyScenePreset(MixerController& mc, ScenePreset p);

} // namespace echo
