// Voices on an input: one or several voice presets combined. Each voice's
// effects carry its name (Effect::voice), so a voice can be added to or taken
// out of the mix without touching the others (or the effects' settings the
// user changed). The input's noise and echo cleanup always stays in front.
#pragma once

#include <functional>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "dsp/Effect.h"
#include "session/Presets.h"

namespace echo {

// Where a voice goes when voices are combined: 0 pitch and AI voices first,
// 1 character (distortion, robot, radio...), 2 echo and reverb last.
int voiceStage(const json& presetChain);

bool isCleanupEffect(const Effect& e);

struct VoicePlan {
    // The new chain. Effects being taken out are still in it, bypassed (the
    // chain runner fades them out); remove them once faded.
    std::vector<std::shared_ptr<Effect>> chain;
    std::vector<std::shared_ptr<Effect>> fading;
    std::vector<std::shared_ptr<Effect>> revived; // were fading out, wanted again
    std::vector<std::string> voices;              // the input's voices afterwards
    // Picking a voice when the old and new effects together are too many
    // for one chain: the old ones fade out first; plan again afterwards to
    // add the new ones.
    bool deferred = false;
    std::string error; // non-empty: nothing changes
};

using PresetFinder = std::function<const ChainPreset*(const std::string& name)>;

// Plans the chain for `target` voices. exclusive = picking voices (every
// other non-cleanup effect goes, also ones added by hand); otherwise voices
// are added and taken out and everything else stays. `fading` = effects
// still fading out from an earlier change (brought back if their voice is
// wanted again). Unknown voice names are skipped. Does not change anything
// until applyVoicePlan().
VoicePlan planVoices(const std::vector<std::shared_ptr<Effect>>& chain, const std::vector<std::string>& target, bool exclusive,
                     const PresetFinder& find, const std::set<const Effect*>& fading);
// Sets the bypass flags the plan relies on (fading effects off, revived
// ones on). Call right before publishing plan.chain.
void applyVoicePlan(const VoicePlan& plan, const std::vector<std::shared_ptr<Effect>>& oldChain);

// "Pain", "Pain + Cave Echo", "" for none.
std::string voicesLabel(const std::vector<std::string>& voices);

// Sessions from before voices could be combined name one voice and carry
// untagged effects: tags them with that voice.
void tagLegacyVoice(std::vector<std::shared_ptr<Effect>>& chain, const std::string& voice);

} // namespace echo
