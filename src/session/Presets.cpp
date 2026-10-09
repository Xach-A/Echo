#include "session/Presets.h"

#include <algorithm>
#include <filesystem>

#include "core/Text.h"
#include "session/MixerController.h"

namespace echo {

namespace {

json fx(const char* type, json params = json::object()) {
    return json{{"type", type}, {"bypass", false}, {"params", std::move(params)}};
}

ChainPreset make(const char* name, const char* cat, const char* desc, json chain) {
    ChainPreset p;
    p.name = name;
    p.category = cat;
    p.description = desc;
    p.factory = true;
    p.chain = std::move(chain);
    return p;
}

std::string safeFileName(const std::string& name) {
    std::string s;
    for (char c : name) s.push_back((std::isalnum(static_cast<unsigned char>(c)) || c == ' ' || c == '-' || c == '_') ? c : '_');
    if (s.empty()) s = "preset";
    return s;
}

} // namespace

std::vector<ChainPreset> factoryChainPresets() {
    std::vector<ChainPreset> v;
    const json limiter = fx("limiter", {{"ceiling", -1.0}});
    // Neural noise suppression leads every voice-processing chain (before
    // gate, EQ, compression and any voice transformation).
    auto ns = [](json params = json::object()) { return fx("voice_denoise", std::move(params)); };

    // ---- Clean voice / broadcast ----
    v.push_back(make("Clean Microphone", "Voice processing",
                     "Noise suppression, rumble filter, gentle gate and compression, safety limiter. A transparent "
                     "starting point.",
                     json::array({ns(),
                                  fx("filter", {{"hp_on", 1}, {"hp_freq", 80}, {"hp_slope", 1}}),
                                  fx("gate", {{"threshold", -52}, {"range", -30}, {"release", 200}}),
                                  fx("compressor", {{"threshold", -20}, {"ratio", 2.5}, {"attack", 10}, {"release", 150}, {"knee", 6}, {"makeup", 3}}),
                                  limiter})));
    v.push_back(make("Noise-Reduced Microphone", "Voice processing",
                     "Maximum noise removal for loud rooms: neural noise suppression with the voice-activity gate "
                     "(silence between sentences), plus compression.",
                     json::array({ns({{"vad_gate", 1}, {"vad_range", -40}, {"vad_hold", 350}}),
                                  fx("filter", {{"hp_on", 1}, {"hp_freq", 90}, {"hp_slope", 1}}),
                                  fx("compressor", {{"threshold", -20}, {"ratio", 3}, {"makeup", 4}}),
                                  limiter})));
    v.push_back(make("Streaming Voice", "Voice processing",
                     "Present, compressed broadcast voice for streams: noise suppression, EQ, compression, de-essing.",
                     json::array({ns(),
                                  fx("filter", {{"hp_on", 1}, {"hp_freq", 90}, {"hp_slope", 1}}),
                                  fx("gate", {{"threshold", -50}, {"range", -40}}),
                                  fx("eq", {{"b2_freq", 250}, {"b2_gain", -2.5}, {"b4_freq", 3200}, {"b4_gain", 3}, {"b6_freq", 10000}, {"b6_gain", 2}}),
                                  fx("compressor", {{"threshold", -22}, {"ratio", 3.5}, {"attack", 5}, {"release", 120}, {"makeup", 5}}),
                                  fx("deesser", {{"freq", 6500}, {"threshold", -32}, {"reduction", 8}}),
                                  limiter})));
    v.push_back(make("Podcast Voice", "Voice processing",
                     "Warm, even podcast sound: expander, warmth/clarity EQ, firm compression, de-esser.",
                     json::array({ns({{"strength", 85}}),
                                  fx("filter", {{"hp_on", 1}, {"hp_freq", 70}, {"hp_slope", 1}}),
                                  fx("expander", {{"threshold", -45}, {"ratio", 2}, {"range", -20}}),
                                  fx("eq", {{"b1_freq", 110}, {"b1_gain", 2}, {"b2_freq", 320}, {"b2_gain", -3}, {"b4_freq", 4500}, {"b4_gain", 2}, {"b6_gain", 1}}),
                                  fx("compressor", {{"threshold", -24}, {"ratio", 4}, {"attack", 8}, {"release", 180}, {"knee", 8}, {"makeup", 6}}),
                                  fx("deesser", {{"freq", 6000}, {"threshold", -30}}),
                                  fx("limiter", {{"ceiling", -1.5}})})));
    v.push_back(make("Gaming Voice", "Voice processing",
                     "Aggressive noise rejection and a cutting mid boost so you are heard over game audio.",
                     json::array({ns({{"vad_gate", 1}, {"vad_range", -45}}),
                                  fx("filter", {{"hp_on", 1}, {"hp_freq", 120}, {"hp_slope", 2}}),
                                  fx("eq", {{"b4_freq", 2500}, {"b4_gain", 3}}),
                                  fx("compressor", {{"threshold", -20}, {"ratio", 4}, {"makeup", 6}}),
                                  limiter})));
    v.push_back(make("Broadcast Radio DJ", "Voice processing",
                     "Heavily compressed, bright 'FM radio host' voice.",
                     json::array({ns(),
                                  fx("filter", {{"hp_on", 1}, {"hp_freq", 100}}),
                                  fx("eq", {{"b1_freq", 120}, {"b1_gain", 3}, {"b4_freq", 3500}, {"b4_gain", 3}, {"b6_gain", 3}}),
                                  fx("compressor", {{"threshold", -28}, {"ratio", 8}, {"attack", 2}, {"release", 80}, {"makeup", 10}}),
                                  fx("deesser", {{"freq", 7000}, {"threshold", -28}}),
                                  limiter})));

    // ---- Pitch correction ----
    v.push_back(make("Natural Pitch Correction", "Pitch correction",
                     "Gentle, slow correction toward the nearest semitone (chromatic). Set the key/scale for best results.",
                     json::array({fx("filter", {{"hp_on", 1}, {"hp_freq", 80}}),
                                  fx("autotune", {{"style", 0}, {"scale", 0}, {"range", 1}}),
                                  fx("compressor", {{"threshold", -20}, {"ratio", 2.5}, {"makeup", 3}}),
                                  limiter})));
    v.push_back(make("Hard Autotune", "Pitch correction",
                     "Instant, robotic pitch snapping (the classic 'autotune' effect) with a touch of reverb.",
                     json::array({fx("filter", {{"hp_on", 1}, {"hp_freq", 90}}),
                                  fx("autotune", {{"style", 1}, {"scale", 0}, {"range", 1}}),
                                  fx("compressor", {{"threshold", -22}, {"ratio", 4}, {"makeup", 5}}),
                                  fx("reverb", {{"decay", 1.4}, {"mix", 14}}),
                                  limiter})));

    // ---- Voice changer characters ----
    v.push_back(make("Deep Voice", "Voice changer",
                     "Lower pitch and slightly larger vocal tract (formants down) for a deep voice.",
                     json::array({fx("pitch", {{"semitones", -5}, {"formant", -2.5}, {"preserve", 1}}),
                                  fx("eq", {{"b1_freq", 120}, {"b1_gain", 3}}),
                                  fx("compressor", {{"threshold", -20}, {"ratio", 3}, {"makeup", 3}}),
                                  limiter})));
    v.push_back(make("Chipmunk", "Voice changer",
                     "High, squeaky voice: pitch and formants raised together.",
                     json::array({fx("pitch", {{"semitones", 9}, {"preserve", 0}}), limiter})));
    v.push_back(make("Robot", "Voice changer", "Monotone buzzing robot with a metallic ring.",
                     json::array({fx("robot", {{"pitch", 100}}),
                                  fx("ringmod", {{"freq", 60}, {"mix", 30}}),
                                  fx("reverb", {{"decay", 0.6}, {"mix", 10}}),
                                  limiter})));
    v.push_back(make("Monster", "Voice changer", "Growling creature: much lower pitch and formants, grit and space.",
                     json::array({fx("pitch", {{"semitones", -9}, {"formant", -5}, {"preserve", 1}}),
                                  fx("distortion", {{"drive", 14}, {"type", 2}, {"mix", 40}, {"output", -6}}),
                                  fx("eq", {{"b1_freq", 100}, {"b1_gain", 4}}),
                                  fx("reverb", {{"decay", 1.3}, {"mix", 15}}),
                                  limiter})));
    v.push_back(make("Alien", "Voice changer", "Otherworldly voice with shifted formants, ring modulation and flanging.",
                     json::array({fx("pitch", {{"semitones", 4}, {"formant", 4}, {"preserve", 1}}),
                                  fx("ringmod", {{"freq", 420}, {"mix", 35}, {"lfo_rate", 3}, {"lfo_depth", 20}}),
                                  fx("flanger", {{"rate", 0.4}, {"feedback", 60}, {"mix", 40}}),
                                  fx("reverb", {{"decay", 1.8}, {"mix", 18}}),
                                  limiter})));
    v.push_back(make("Telephone", "Voice changer", "Narrow-band phone line voice.",
                     json::array({fx("character", {{"mode", 0}, {"intensity", 70}, {"noise", -66}}), limiter})));
    v.push_back(make("Radio Voice", "Voice changer", "Old AM radio broadcast with static.",
                     json::array({fx("compressor", {{"threshold", -26}, {"ratio", 6}, {"makeup", 8}}),
                                  fx("character", {{"mode", 1}, {"intensity", 60}, {"noise", -58}}), limiter})));
    v.push_back(make("Walkie-Talkie", "Voice changer", "Clipped, narrow two-way radio voice.",
                     json::array({fx("character", {{"mode", 2}, {"intensity", 80}, {"noise", -52}}), limiter})));
    v.push_back(make("Megaphone", "Voice changer", "Distorted bullhorn voice.",
                     json::array({fx("character", {{"mode", 3}, {"intensity", 75}}), limiter})));
    v.push_back(make("Stadium PA", "Voice changer", "Public-address system with slap-back echoes.",
                     json::array({fx("character", {{"mode", 4}, {"intensity", 55}}),
                                  fx("reverb", {{"decay", 2.5}, {"mix", 20}, {"size", 1.4}}), limiter})));
    v.push_back(make("Cave Echo", "Voice changer", "Repeating echoes in a large stone space.",
                     json::array({fx("delay", {{"time", 280}, {"feedback", 45}, {"mix", 35}}),
                                  fx("reverb", {{"decay", 3.5}, {"mix", 30}, {"size", 1.3}}), limiter})));
    v.push_back(make("Cathedral", "Voice changer", "Long, lush reverb tail.",
                     json::array({fx("reverb", {{"decay", 6.0}, {"mix", 35}, {"size", 1.5}, {"predelay", 40}}), limiter})));
    v.push_back(make("Whisper", "Voice changer", "Turns speech into a breathy whisper.",
                     json::array({fx("robot", {{"mode", 1}}), fx("eq", {{"b5_freq", 5000}, {"b5_gain", 3}}), limiter})));
    v.push_back(make("Feminine-style Voice", "Voice changer",
                     "Raises pitch and formants moderately. Results depend strongly on the speaker; perceived gender "
                     "also depends on intonation, breathiness and speaking style.",
                     json::array({fx("pitch", {{"semitones", 4}, {"formant", 2.5}, {"preserve", 1}}),
                                  fx("eq", {{"b1_freq", 150}, {"b1_gain", -2}, {"b5_freq", 6000}, {"b5_gain", 2}}),
                                  limiter})));
    v.push_back(make("Masculine-style Voice", "Voice changer",
                     "Lowers pitch and formants moderately. Results depend strongly on the speaker; perceived gender "
                     "also depends on resonance, intonation and speaking style.",
                     json::array({fx("pitch", {{"semitones", -4}, {"formant", -2.5}, {"preserve", 1}}),
                                  fx("eq", {{"b1_freq", 130}, {"b1_gain", 2}}), limiter})));
    v.push_back(make("Child-like Voice", "Voice changer", "Higher pitch with a smaller vocal tract.",
                     json::array({fx("pitch", {{"semitones", 6}, {"formant", 4.5}, {"preserve", 1}}), limiter})));
    v.push_back(make("Giant", "Voice changer", "Huge, slow voice: pitch and formants far down together.",
                     json::array({fx("pitch", {{"semitones", -12}, {"preserve", 0}}),
                                  fx("reverb", {{"decay", 2.0}, {"mix", 18}}), limiter})));
    v.push_back(make("Doubled Vocals", "Voice changer", "Thick, doubled voice.",
                     json::array({fx("doubler", {{"detune", 12}, {"delay", 22}}), limiter})));
    v.push_back(make("Harmony Choir", "Voice changer", "Adds a third and a fifth above your voice.",
                     json::array({fx("harmonizer", {{"interval1", 4}, {"interval2", 7}}),
                                  fx("reverb", {{"decay", 1.8}, {"mix", 20}}), limiter})));
    v.push_back(make("Octave Down", "Voice changer", "Blends a voice one octave lower.",
                     json::array({fx("octaver", {{"sub", -3}}), limiter})));
    v.push_back(make("Underwater", "Voice changer", "Muffled, wobbling voice.",
                     json::array({fx("filter", {{"lp_on", 1}, {"lp_freq", 900}, {"lp_slope", 2}, {"hp_on", 0}}),
                                  fx("chorus", {{"rate", 1.5}, {"depth", 6}, {"mix", 60}}),
                                  fx("phaser", {{"rate", 0.3}, {"mix", 40}}), limiter})));
    v.push_back(make("Lo-fi 8-bit", "Voice changer", "Crunchy retro game voice.",
                     json::array({fx("bitcrusher", {{"bits", 6}, {"downsample", 6}}),
                                  fx("filter", {{"hp_on", 1}, {"hp_freq", 200}}), limiter})));

    // ---- Music / bus processing ----
    v.push_back(make("Music Enhancer", "Music",
                     "For music buses: gentle EQ smile, glue compression and limiting.",
                     json::array({fx("eq", {{"b1_freq", 90}, {"b1_gain", 2}, {"b6_freq", 11000}, {"b6_gain", 2}}),
                                  fx("compressor", {{"threshold", -16}, {"ratio", 2}, {"attack", 20}, {"release", 250}, {"makeup", 2}}),
                                  limiter})));
    v.push_back(make("Background Music Ducking Level", "Music",
                     "Quieter, darker music suitable under speech.",
                     json::array({fx("gain", {{"gain", -12}}), fx("filter", {{"hp_on", 0}, {"lp_on", 1}, {"lp_freq", 9000}})})));
    return v;
}

// ---------------------------------------------------------------------------

json PresetLibrary::toJson(const ChainPreset& p) {
    return json{{"format", "echo.chain-preset"}, {"version", 1},         {"name", p.name},
                {"category", p.category},        {"description", p.description}, {"chain", p.chain}};
}

bool PresetLibrary::fromJson(const json& j, ChainPreset& out) {
    if (!j.is_object() || j.value("format", "") != "echo.chain-preset") return false;
    out.name = j.value("name", "Untitled");
    out.category = j.value("category", "User");
    out.description = j.value("description", "");
    out.chain = j.contains("chain") && j["chain"].is_array() ? j["chain"] : json::array();
    return true;
}

void PresetLibrary::load(const std::wstring& userDir) {
    userDir_ = userDir;
    presets_ = factoryChainPresets();
    std::error_code ec;
    if (userDir.empty() || !std::filesystem::exists(userDir, ec)) return;
    for (const auto& entry : std::filesystem::directory_iterator(userDir, ec)) {
        if (!entry.is_regular_file() || entry.path().extension() != L".echopreset") continue;
        json j;
        if (!loadJsonFile(entry.path().wstring(), j, nullptr, false)) continue;
        ChainPreset p;
        if (!fromJson(j, p)) continue;
        p.factory = false;
        p.file = entry.path().wstring();
        if (p.category.empty()) p.category = "User";
        presets_.push_back(std::move(p));
    }
}

const ChainPreset* PresetLibrary::find(const std::string& name) const {
    // Prefer user presets over factory presets with the same name.
    const ChainPreset* hit = nullptr;
    for (const auto& p : presets_)
        if (p.name == name && (!hit || !p.factory)) hit = &p;
    return hit;
}

std::wstring PresetLibrary::fileFor(const std::string& name) const {
    return userDir_ + L"\\" + utf8ToWide(safeFileName(name)) + L".echopreset";
}

bool PresetLibrary::saveUser(const std::string& name, const std::string& category, const std::string& description,
                             const json& chain, std::string* error) {
    if (name.empty()) {
        if (error) *error = "Preset name is empty";
        return false;
    }
    ChainPreset p;
    p.name = name;
    p.category = category.empty() ? "User" : category;
    p.description = description;
    p.chain = chain;
    p.file = fileFor(name);
    if (!saveJsonFile(p.file, toJson(p), error)) return false;
    presets_.erase(std::remove_if(presets_.begin(), presets_.end(),
                                  [&](const ChainPreset& q) { return !q.factory && q.name == name; }),
                   presets_.end());
    presets_.push_back(std::move(p));
    return true;
}

bool PresetLibrary::rename(const std::string& oldName, const std::string& newName, std::string* error) {
    for (auto& p : presets_) {
        if (p.factory || p.name != oldName) continue;
        const ChainPreset copy = p;
        if (!saveUser(newName, copy.category, copy.description, copy.chain, error)) return false;
        std::error_code ec;
        if (copy.file != fileFor(newName)) std::filesystem::remove(copy.file, ec);
        presets_.erase(std::remove_if(presets_.begin(), presets_.end(),
                                      [&](const ChainPreset& q) { return !q.factory && q.name == oldName; }),
                       presets_.end());
        return true;
    }
    if (error) *error = "Only user presets can be renamed";
    return false;
}

bool PresetLibrary::duplicate(const std::string& name, std::string* error) {
    const ChainPreset* p = find(name);
    if (!p) {
        if (error) *error = "Preset not found";
        return false;
    }
    std::string newName = name + " (copy)";
    for (int i = 2; find(newName); ++i) newName = name + " (copy " + std::to_string(i) + ")";
    const ChainPreset copy = *p;
    return saveUser(newName, copy.factory ? "User" : copy.category, copy.description, copy.chain, error);
}

bool PresetLibrary::remove(const std::string& name, std::string* error) {
    for (auto it = presets_.begin(); it != presets_.end(); ++it) {
        if (it->factory || it->name != name) continue;
        std::error_code ec;
        std::filesystem::remove(it->file, ec);
        presets_.erase(it);
        return true;
    }
    if (error) *error = "Factory presets cannot be deleted";
    return false;
}

bool PresetLibrary::importFile(const std::wstring& path, std::string* error) {
    json j;
    if (!loadJsonFile(path, j, error, false)) return false;
    ChainPreset p;
    if (!fromJson(j, p)) {
        if (error) *error = "Not an Echo chain preset";
        return false;
    }
    return saveUser(p.name, p.category, p.description, p.chain, error);
}

bool PresetLibrary::exportPreset(const std::string& name, const std::wstring& path, std::string* error) const {
    const ChainPreset* p = find(name);
    if (!p) {
        if (error) *error = "Preset not found";
        return false;
    }
    return saveJsonFile(path, toJson(*p), error);
}

// ---------------------------------------------------------------------------
// Routing scenes
// ---------------------------------------------------------------------------

const char* scenePresetName(ScenePreset p) {
    switch (p) {
    case ScenePreset::VoiceToVirtualMic: return "Voice to Virtual Mic";
    case ScenePreset::VoicePlusMusicToMic: return "Voice + Music Virtual Mic";
    case ScenePreset::HeadphonesOnlyMusic: return "Headphones-Only Music";
    case ScenePreset::MonitorMyVoice: return "Monitor My Voice";
    default: return "?";
    }
}

const char* scenePresetDescription(ScenePreset p) {
    switch (p) {
    case ScenePreset::VoiceToVirtualMic:
        return "Microphone goes to the virtual microphone only. App/desktop audio is kept out of the mic.";
    case ScenePreset::VoicePlusMusicToMic:
        return "Microphone and app/desktop audio are mixed into the virtual microphone; you also hear the music in "
               "your headphones.";
    case ScenePreset::HeadphonesOnlyMusic:
        return "App/desktop audio plays in your headphones only and never enters the virtual microphone.";
    case ScenePreset::MonitorMyVoice:
        return "Microphone goes to the virtual microphone and to your headphones so you can hear yourself.";
    default: return "";
    }
}

std::string applyScenePreset(MixerController& mc, ScenePreset p) {
    Session& s = mc.session();
    BusModel* phones = nullptr;
    BusModel* vmic = nullptr;
    for (auto& b : s.buses) {
        if (!phones && b.kind == BusKind::Physical) phones = &b;
        if (!vmic && b.kind == BusKind::Virtual) vmic = &b;
    }
    if (!vmic && (p != ScenePreset::HeadphonesOnlyMusic)) return "Add a virtual (B) bus first.";
    if (!phones && (p != ScenePreset::VoiceToVirtualMic)) return "Add a physical (A) output bus first.";
    int voices = 0, apps = 0;
    for (auto& st : s.strips) {
        const bool voice = st.group == StripGroup::Hardware;
        const bool app = st.group == StripGroup::Application || st.group == StripGroup::Virtual;
        auto set = [&](BusModel* b, bool on) {
            if (b) st.rt->sends[b->slot].enabled = on;
        };
        switch (p) {
        case ScenePreset::VoiceToVirtualMic:
            if (voice) { set(vmic, true); ++voices; }
            if (app) { set(vmic, false); ++apps; }
            break;
        case ScenePreset::VoicePlusMusicToMic:
            if (voice) { set(vmic, true); set(phones, false); ++voices; }
            if (app) { set(vmic, true); set(phones, true); ++apps; }
            break;
        case ScenePreset::HeadphonesOnlyMusic:
            if (app) { set(phones, true); set(vmic, false); ++apps; }
            break;
        case ScenePreset::MonitorMyVoice:
            if (voice) { set(vmic, true); set(phones, true); ++voices; }
            break;
        default: break;
        }
    }
    mc.sendsChanged();
    return std::string("Applied '") + scenePresetName(p) + "' to " + std::to_string(voices) + " voice and " +
           std::to_string(apps) + " app input(s).";
}

} // namespace echo
