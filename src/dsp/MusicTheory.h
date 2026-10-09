// Notes, scales and pitch conversions for pitch correction and displays.
#pragma once

#include <cmath>

namespace echo::music {

inline const char* const kNoteNames[] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B", nullptr};

enum Scale {
    Chromatic = 0, Major, NaturalMinor, HarmonicMinor, MelodicMinor, MajorPentatonic, MinorPentatonic, Blues,
    Dorian, Mixolydian, NumScales
};
inline const char* const kScaleNames[] = {"Chromatic", "Major", "Minor (natural)", "Harmonic minor",
                                          "Melodic minor", "Major pentatonic", "Minor pentatonic", "Blues",
                                          "Dorian", "Mixolydian", nullptr};

// 12-bit masks of allowed pitch classes relative to the key (bit 0 = root).
inline constexpr int kScaleMasks[NumScales] = {
    0xFFF,                                                     // chromatic
    (1 << 0) | (1 << 2) | (1 << 4) | (1 << 5) | (1 << 7) | (1 << 9) | (1 << 11),  // major
    (1 << 0) | (1 << 2) | (1 << 3) | (1 << 5) | (1 << 7) | (1 << 8) | (1 << 10),  // natural minor
    (1 << 0) | (1 << 2) | (1 << 3) | (1 << 5) | (1 << 7) | (1 << 8) | (1 << 11),  // harmonic minor
    (1 << 0) | (1 << 2) | (1 << 3) | (1 << 5) | (1 << 7) | (1 << 9) | (1 << 11),  // melodic minor
    (1 << 0) | (1 << 2) | (1 << 4) | (1 << 7) | (1 << 9),                        // major pentatonic
    (1 << 0) | (1 << 3) | (1 << 5) | (1 << 7) | (1 << 10),                       // minor pentatonic
    (1 << 0) | (1 << 3) | (1 << 5) | (1 << 6) | (1 << 7) | (1 << 10),            // blues
    (1 << 0) | (1 << 2) | (1 << 3) | (1 << 5) | (1 << 7) | (1 << 9) | (1 << 10), // dorian
    (1 << 0) | (1 << 2) | (1 << 4) | (1 << 5) | (1 << 7) | (1 << 9) | (1 << 10), // mixolydian
};

inline double hzToMidi(double hz, double a4 = 440.0) { return 69.0 + 12.0 * std::log2(hz / a4); }
inline double midiToHz(double midi, double a4 = 440.0) { return a4 * std::pow(2.0, (midi - 69.0) / 12.0); }

inline bool noteAllowed(int midiNote, int key, int scale) {
    const int pc = ((midiNote - key) % 12 + 12) % 12;
    return (kScaleMasks[scale] >> pc) & 1;
}

// Nearest allowed MIDI note to a (fractional) MIDI pitch.
inline int nearestNote(double midi, int key, int scale) {
    const int base = static_cast<int>(std::lround(midi));
    int best = base;
    double bestD = 1e9;
    for (int d = -6; d <= 6; ++d) {
        const int n = base + d;
        if (!noteAllowed(n, key, scale)) continue;
        const double dist = std::abs(midi - n);
        if (dist < bestD) {
            bestD = dist;
            best = n;
        }
    }
    return best;
}

} // namespace echo::music
