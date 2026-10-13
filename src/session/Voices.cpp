#include "session/Voices.h"

#include <algorithm>
#include <map>

#include "dsp/NoiseSuppressorInfo.h"
#include "session/Serialization.h"

namespace echo {

namespace {

bool contains(const std::vector<std::string>& v, const std::string& s) { return std::find(v.begin(), v.end(), s) != v.end(); }

// Echo canceller first, then the noise suppressor (the canonical order).
int cleanupRank(const std::string& type) { return type == echoCancellerTypeId() ? 0 : 1; }

} // namespace

bool isCleanupEffect(const Effect& e) {
    const std::string t = e.typeId();
    return t == noiseSuppressorTypeId() || t == echoCancellerTypeId();
}

int voiceStage(const json& presetChain) {
    static const std::set<std::string> pitchTypes = {"pitch", "pitch_lite", "autotune", "octaver", "harmonizer", "ai_voice"};
    static const std::set<std::string> spaceTypes = {"reverb", "delay"};
    static const std::set<std::string> neutralTypes = {"eq", "filter", "gain", "stereo", "compressor", "limiter", "chorus",
                                                       "flanger", "phaser", "doubler", "gate", "expander", "agc", "deesser"};
    bool space = false, other = false;
    if (!presetChain.is_array()) return 1;
    for (const auto& fx : presetChain) {
        const std::string t = fx.value("type", "");
        if (t == noiseSuppressorTypeId() || t == echoCancellerTypeId()) continue;
        if (pitchTypes.count(t)) return 0;
        if (spaceTypes.count(t)) space = true;
        else if (!neutralTypes.count(t)) other = true;
    }
    return space && !other ? 2 : 1;
}

VoicePlan planVoices(const std::vector<std::shared_ptr<Effect>>& chain, const std::vector<std::string>& target, bool exclusive,
                     const PresetFinder& find, const std::set<const Effect*>& fading) {
    VoicePlan plan;
    for (const auto& v : target)
        if (!v.empty() && find(v) && !contains(plan.voices, v)) plan.voices.push_back(v);
    const std::vector<std::string>& want = plan.voices;
    auto isFading = [&](const Effect* e) { return fading.count(e) > 0; };

    // Voices in the chain now, and ones only fading out that are wanted again.
    std::set<std::string> present, revive;
    for (const auto& e : chain)
        if (!e->voice.empty() && !isCleanupEffect(*e) && !isFading(e.get())) present.insert(e->voice);
    for (const auto& e : chain)
        if (!e->voice.empty() && isFading(e.get()) && contains(want, e->voice) && !present.count(e->voice))
            revive.insert(e->voice);
    std::vector<std::string> toAdd;
    for (const auto& v : want)
        if (!present.count(v) && !revive.count(v)) toAdd.push_back(v);

    // Picking voices: their own noise/echo cleanup replaces the input's.
    std::set<std::string> newCleanup;
    if (exclusive)
        for (const auto& v : toAdd)
            for (const auto& fx : find(v)->chain) {
                const std::string t = fx.value("type", "");
                if (t == noiseSuppressorTypeId() || t == echoCancellerTypeId()) newCleanup.insert(t);
            }

    std::vector<std::shared_ptr<Effect>> out;
    for (const auto& e : chain) {
        bool goes;
        if (isCleanupEffect(*e)) goes = exclusive ? newCleanup.count(e->typeId()) > 0 : isFading(e.get());
        else if (!e->voice.empty()) goes = !contains(want, e->voice) || (isFading(e.get()) && !revive.count(e->voice));
        else goes = exclusive || isFading(e.get()); // added by hand: only picking a voice replaces it
        out.push_back(e);
        if (goes) plan.fading.push_back(e);
        else if (isFading(e.get())) plan.revived.push_back(e);
    }

    // Stages of the voices in the chain, for where a new voice goes.
    std::map<std::string, int> stages;
    auto stageOf = [&](const std::string& v) {
        auto it = stages.find(v);
        if (it != stages.end()) return it->second;
        const ChainPreset* p = find(v);
        const int s = p ? voiceStage(p->chain) : 1;
        stages[v] = s;
        return s;
    };
    std::vector<std::shared_ptr<Effect>> withAdds = out;
    auto hasCleanup = [&](const std::string& type) {
        for (const auto& e : withAdds)
            if (e->typeId() == type && std::find(plan.fading.begin(), plan.fading.end(), e) == plan.fading.end()) return true;
        return false;
    };
    for (const auto& v : toAdd) {
        const ChainPreset* p = find(v);
        auto fx = chainFromJson(p->chain, nullptr);
        std::vector<std::shared_ptr<Effect>> voiceFx;
        for (auto& e : fx) {
            if (isCleanupEffect(*e)) {
                if (!exclusive && hasCleanup(e->typeId())) continue; // the input has its own
                e->voice.clear();
                // In front, canceller before suppressor.
                size_t pos = 0;
                while (pos < withAdds.size() && isCleanupEffect(*withAdds[pos]) &&
                       cleanupRank(withAdds[pos]->typeId()) <= cleanupRank(e->typeId()))
                    ++pos;
                withAdds.insert(withAdds.begin() + static_cast<long>(pos), e);
                continue;
            }
            e->voice = v;
            voiceFx.push_back(e);
        }
        const int stage = stageOf(v);
        size_t pos = 0;
        while (pos < withAdds.size() && isCleanupEffect(*withAdds[pos])) ++pos;
        for (size_t i = 0; i < withAdds.size(); ++i)
            if (!isCleanupEffect(*withAdds[i]) && !withAdds[i]->voice.empty() && stageOf(withAdds[i]->voice) <= stage) pos = i + 1;
        withAdds.insert(withAdds.begin() + static_cast<long>(pos), voiceFx.begin(), voiceFx.end());
    }

    if (withAdds.size() > static_cast<size_t>(kMaxChainLength)) {
        if (!exclusive) {
            plan = VoicePlan{};
            plan.error = "That's too many effects for one input (at most " + std::to_string(kMaxChainLength) +
                         "). Take a voice out first.";
            return plan;
        }
        // Old effects fade out first; the new voices are added afterwards.
        plan.chain = out;
        plan.deferred = true;
        return plan;
    }
    plan.chain = std::move(withAdds);
    return plan;
}

void applyVoicePlan(const VoicePlan& plan, const std::vector<std::shared_ptr<Effect>>& /*oldChain*/) {
    for (const auto& e : plan.fading) e->bypass = true; // the chain runner crossfades it out
    for (const auto& e : plan.revived) e->bypass = false;
}

std::string voicesLabel(const std::vector<std::string>& voices) {
    std::string s;
    for (const auto& v : voices) s += (s.empty() ? "" : " + ") + v;
    return s;
}

void tagLegacyVoice(std::vector<std::shared_ptr<Effect>>& chain, const std::string& voice) {
    if (voice.empty()) return;
    for (const auto& e : chain)
        if (!e->voice.empty()) return; // already tagged
    for (auto& e : chain)
        if (!isCleanupEffect(*e)) e->voice = voice;
}

} // namespace echo
