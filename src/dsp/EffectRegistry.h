// Registry of all effect types, used by the chain editor, presets and
// serialisation.
#pragma once

#include <memory>
#include <string_view>
#include <vector>

#include "dsp/Effect.h"

namespace echo {

using EffectFactory = std::unique_ptr<Effect> (*)();

struct EffectRegistration {
    const EffectInfo* info;
    EffectFactory create;
};

class EffectRegistry {
public:
    static EffectRegistry& instance();

    void add(const EffectInfo& info, EffectFactory create);
    std::unique_ptr<Effect> create(std::string_view typeId) const;
    const EffectInfo* find(std::string_view typeId) const;
    const std::vector<EffectRegistration>& all() const { return entries_; }

private:
    std::vector<EffectRegistration> entries_;
};

// Registration hooks implemented by each effects translation unit.
void registerUtilityEffects(EffectRegistry& r);
void registerEqEffects(EffectRegistry& r);
void registerDynamicsEffects(EffectRegistry& r);
void registerTimeEffects(EffectRegistry& r);
void registerModulationEffects(EffectRegistry& r);
void registerDistortionEffects(EffectRegistry& r);
void registerNoiseEffects(EffectRegistry& r);
void registerVoiceDenoiseEffects(EffectRegistry& r);
void registerEchoCancelEffects(EffectRegistry& r);
void registerSpectralEffects(EffectRegistry& r);
void registerVoiceEffects(EffectRegistry& r);
void registerPitchCorrectionEffects(EffectRegistry& r);
void registerAiEffects(EffectRegistry& r);

} // namespace echo
