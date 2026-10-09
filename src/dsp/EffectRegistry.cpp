#include "dsp/EffectRegistry.h"

namespace echo {

EffectRegistry& EffectRegistry::instance() {
    static EffectRegistry* reg = [] {
        auto* r = new EffectRegistry();
        registerUtilityEffects(*r);
        registerEqEffects(*r);
        registerVoiceDenoiseEffects(*r);
        registerEchoCancelEffects(*r);
        registerDynamicsEffects(*r);
        registerNoiseEffects(*r);
        registerTimeEffects(*r);
        registerModulationEffects(*r);
        registerDistortionEffects(*r);
        registerSpectralEffects(*r);
        registerVoiceEffects(*r);
        registerPitchCorrectionEffects(*r);
        registerAiEffects(*r);
        return r;
    }();
    return *reg;
}

void EffectRegistry::add(const EffectInfo& info, EffectFactory create) {
    entries_.push_back(EffectRegistration{&info, create});
}

std::unique_ptr<Effect> EffectRegistry::create(std::string_view typeId) const {
    for (const auto& e : entries_)
        if (typeId == e.info->typeId) return e.create();
    return nullptr;
}

const EffectInfo* EffectRegistry::find(std::string_view typeId) const {
    for (const auto& e : entries_)
        if (typeId == e.info->typeId) return e.info;
    return nullptr;
}

} // namespace echo
