#include "session/Soundboard.h"

#include <algorithm>

#include "core/Resampler.h"
#include "core/Text.h"
#include "core/WavFile.h"
#include "session/Paths.h"

namespace echo {

const char* padModeName(PadMode m) {
    switch (m) {
    case PadMode::Toggle: return "Play / stop";
    case PadMode::Hold: return "Hold to play";
    default: return "Play";
    }
}

const std::vector<BuiltinSound>& builtinSounds() {
    static const std::vector<BuiltinSound> sounds = {
        {"airhorn", "Air horn", 1},      {"clown_horn", "Clown horn", 2}, {"rimshot", "Ba-dum-tss", 1},
        {"sad_trombone", "Sad trombone", 1}, {"tada", "Ta-da!", 1},       {"ding", "Ding", 1},
        {"buzzer", "Wrong buzzer", 1},   {"drumroll", "Drum roll", 1},     {"applause", "Applause", 1},
        {"boing", "Boing", 1},           {"whoosh", "Whoosh", 1},          {"laser", "Pew pew", 1},
        {"explosion", "Explosion", 1},   {"gunshot", "Gunshot", 2},        {"boom", "Bass boom", 1},
        {"crickets", "Crickets", 1},
    };
    return sounds;
}

const BuiltinSound* findBuiltinSound(const std::string& id) {
    for (const auto& s : builtinSounds())
        if (id == s.id) return &s;
    return nullptr;
}

std::wstring builtinSoundFile(const std::string& id) {
    return paths::builtinSoundsDir() + L"\\" + utf8ToWide(id) + L".wav";
}

std::vector<float> loadBuiltinSound(const std::string& id, int sampleRate, std::string* error) {
    if (!findBuiltinSound(id) || sampleRate < 8000) {
        if (error) *error = "Unknown built-in sound";
        return {};
    }
    WavData w;
    if (!readWav(builtinSoundFile(id), w, nullptr) || w.channels <= 0 || w.sampleRate <= 0 || w.samples.empty()) {
        if (error) *error = "Echo's sound file is missing or damaged; reinstalling Echo brings it back";
        return {};
    }
    return stereoAtRate(w, sampleRate);
}

std::vector<float> stereoAtRate(const WavData& w, int sampleRate) {
    const size_t ch = static_cast<size_t>(w.channels);
    const size_t frames = ch ? w.samples.size() / ch : 0;
    std::vector<float> stereo(frames * 2);
    for (size_t f = 0; f < frames; ++f) {
        const float l = w.samples[f * ch];
        stereo[2 * f] = l;
        stereo[2 * f + 1] = ch > 1 ? w.samples[f * ch + 1] : l;
    }
    return w.sampleRate == sampleRate ? stereo : resampleOffline(stereo, 2, w.sampleRate, sampleRate);
}

namespace {

int addPads(SoundboardSettings& sb, int newerThan) {
    int added = 0;
    for (const auto& s : builtinSounds()) {
        if (s.since <= newerThan) continue;
        if (sb.pads.size() >= static_cast<size_t>(kMaxSoundPads)) break;
        const bool have = std::any_of(sb.pads.begin(), sb.pads.end(), [&](const SoundPad& p) { return p.builtin == s.id; });
        if (have) continue;
        SoundPad p;
        p.id = sb.nextPadId++;
        p.name = s.name;
        p.builtin = s.id;
        sb.pads.push_back(p);
        ++added;
    }
    sb.builtinVersion = kBuiltinSoundsVersion;
    return added;
}

} // namespace

int addBuiltinPads(SoundboardSettings& sb) { return addPads(sb, 0); }

int addNewBuiltinPads(SoundboardSettings& sb) {
    if (sb.builtinVersion >= kBuiltinSoundsVersion) return 0;
    return addPads(sb, sb.builtinVersion);
}

} // namespace echo
