// Frequency-response helpers for drawing EQ/filter curves in the UI. They
// compute from parameter values, not from audio-thread state.
#pragma once

namespace echo {

class Effect;

double eqResponseDb(const Effect& eq, double freq, double sampleRate);
double filterResponseDb(const Effect& filter, double freq, double sampleRate);

} // namespace echo
