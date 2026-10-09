// Visualisations shared by the Effects inspector and the Voice Lab.
#pragma once

#include "dsp/Effect.h"
#include "imgui.h"

namespace echo::ui {

// EQ / filter magnitude response from the effect's parameters.
void drawResponseCurve(const Effect& fx, double sampleRate, ImVec2 size);
// Horizontal gain-reduction bar (0..-24 dB).
void drawGrMeter(float grDb, float width);
// Scrolling pitch-correction graph (input vs corrected pitch, scale notes).
void drawPitchGraph(Effect& fx, ImVec2 size);

} // namespace echo::ui
