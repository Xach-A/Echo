// Interface for effects that publish a pitch-tracking history for display
// (pitch correction). The audio thread produces frames; the UI consumes.
#pragma once

#include "core/SpscRing.h"

namespace echo {

struct PitchFrame {
    float detectedHz; // 0 when unvoiced
    float targetHz;   // note the corrector is steering toward (0 if none)
    float outputHz;   // resulting pitch after correction
    float clarity;    // 0..1 periodicity
};

class PitchDisplaySource {
public:
    virtual ~PitchDisplaySource() = default;
    virtual SpscRing<PitchFrame>& pitchHistory() = 0;
    virtual double framesPerSecond() const = 0;
};

} // namespace echo
