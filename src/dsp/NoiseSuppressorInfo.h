// Identifiers of the voice-cleanup effects, used where defaults are applied
// (new microphone strips, presets) without depending on the implementations.
#pragma once

namespace echo {

// "voice_denoise": RNNoise background-noise suppression.
const char* noiseSuppressorTypeId();
// "echo_cancel": acoustic echo cancellation with a playback reference.
const char* echoCancellerTypeId();

} // namespace echo
