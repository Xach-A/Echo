// The Studio Singer voice's controls on the Voices screen, as settings of
// the effects its preset puts on the input (see factoryChainPresets): each
// control reads its value back from those settings, so a change made in the
// effects editor shows up in the panel too.
#pragma once

#include "dsp/Effect.h"

namespace echo::singer {

// Autotune feel, 0 (natural) .. 100 (robotic): the retune speed
// (200 ms at 0, 18 ms at the default 70, instant at 100).
float tuneFeel(const Effect& autotune);
void setTuneFeel(Effect& autotune, float feel);
const char* tuneFeelName(float feel); // "Natural", "Smooth", "Polished", "Tight", "Robotic"

// Key and scale of the autotune and of the harmony voices.
void setKey(Effect& autotune, Effect* harmonizer, int key, int scale);

// Tone, -100 (darker) .. +100 (brighter): presence and air of the tone EQ
// (and a little less low warmth when brighter). 0 = the preset's.
float tone(const Effect& eq);
void setTone(Effect& eq, float tone);

// Compression, 0 (light) .. 100 (heavy): threshold and ratio (50 = the
// preset's -24 dB, 3.5:1). The compressor's automatic makeup keeps the level.
float compression(const Effect& compressor);
void setCompression(Effect& compressor, float amount);

// Saturation, 0 .. 100: the parallel saturation's drive (0-24 dB); its output
// follows so the voice stays about as loud.
float saturation(const Effect& distortion);
void setSaturation(Effect& distortion, float amount);

// Harmony voices (the harmonizer). In a key they are steps of its scale, so a
// third is major or minor as the key wants; with the chromatic scale (no key)
// they are fixed: a major third, a fifth, an octave.
enum class Harmony { Off = 0, ThirdAbove, ThirdBelow, ThirdAndFifth, OctaveBelow, Count };
const char* harmonyName(Harmony h);
Harmony harmony(const Effect& harmonizer);
void setHarmony(Effect& harmonizer, Harmony h, int key, int scale);

} // namespace echo::singer
