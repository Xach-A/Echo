// The soundboard's settings: sound pads (Echo's built-in sounds and the
// user's own files), each with an optional hotkey. The sounds play through
// core/SoundboardSource on the "Soundboard" input of the mixer.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "session/Hotkeys.h"

namespace echo {

// What pressing a pad (or its hotkey) does.
enum class PadMode : int {
    Restart = 0, // plays from the start (pressing again starts it over)
    Toggle = 1,  // plays; pressing again stops it
    Hold = 2,    // plays while held
};
constexpr int kPadModes = 3;
const char* padModeName(PadMode m);

struct SoundPad {
    uint32_t id = 0;
    std::string name;
    std::string builtin; // id of a built-in sound, or ""
    std::string file;    // the user's sound: file name in the sounds folder
    KeyBinding key;
    PadMode mode = PadMode::Restart;
    float gainDb = 0.0f; // -24..+6
    uint32_t color = 0;  // ABGR; 0 = from the palette by position
};

struct SoundboardSettings {
    std::vector<SoundPad> pads;
    bool initialized = false; // built-in pads added (once; users may remove them)
    uint32_t nextPadId = 1;
    KeyBinding stopAllKey;
    // The newest set of built-in sounds this board was given
    // (kBuiltinSoundsVersion): sounds added to Echo later are added once,
    // and built-in pads the user removed stay removed.
    int builtinVersion = 0;
};

constexpr int kMaxSoundPads = 64;
// Imported sounds are kept up to this length.
constexpr double kMaxSoundSeconds = 60.0;

struct BuiltinSound {
    const char* id;
    const char* name;
    int since; // the kBuiltinSoundsVersion that added it
};
// 1: the first 14 sounds; 2: Clown horn and Gunshot.
constexpr int kBuiltinSoundsVersion = 2;
const std::vector<BuiltinSound>& builtinSounds();
const BuiltinSound* findBuiltinSound(const std::string& id);

// The built-in sounds are recordings shipped next to Echo.exe
// (sounds\<id>.wav; credits in sounds\CREDITS.txt).
std::wstring builtinSoundFile(const std::string& id);
// Loads a built-in sound: interleaved stereo at `sampleRate`, peaks under
// -1 dBFS. Empty, with the reason in `error`, for an unknown id or a missing
// or damaged file.
std::vector<float> loadBuiltinSound(const std::string& id, int sampleRate, std::string* error = nullptr);

// A sound file's samples as interleaved stereo at `sampleRate` (mono is
// doubled, channels after the first two are dropped).
struct WavData;
std::vector<float> stereoAtRate(const WavData& wav, int sampleRate);

// Adds the built-in sounds that have no pad. Returns how many were added.
int addBuiltinPads(SoundboardSettings& sb);
// Adds the built-in sounds that are newer than the board (once). Returns
// how many were added.
int addNewBuiltinPads(SoundboardSettings& sb);

} // namespace echo
