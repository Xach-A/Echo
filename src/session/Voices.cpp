#include "session/Voices.h"

#include <algorithm>
#include <map>

#include "dsp/NoiseSuppressorInfo.h"
#include "session/Serialization.h"
#include "session/Session.h"

namespace echo {

namespace {

bool contains(const std::vector<std::string>& v, const std::string& s) { return std::find(v.begin(), v.end(), s) != v.end(); }

// Echo canceller first, then the noise suppressor (the canonical order).
int cleanupRank(const std::string& type) { return type == echoCancellerTypeId() ? 0 : 1; }

bool isCleanupType(const std::string& t) { return t == noiseSuppressorTypeId() || t == echoCancellerTypeId(); }

// The preset's own effects (its cleanup aside).
std::vector<const json*> presetVoiceEffects(const json& presetChain) {
    std::vector<const json*> out;
    if (presetChain.is_array())
        for (const auto& fx : presetChain)
            if (!isCleanupType(fx.value("type", ""))) out.push_back(&fx);
    return out;
}

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
                     const PresetFinder& find, const std::set<const Effect*>& fading, const VoiceMemory* memory) {
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
        // Its remembered settings, if they still fit its preset (with the
        // preset's cleanup), otherwise the preset's.
        const json* source = &p->chain;
        json merged;
        if (memory)
            if (auto it = memory->find(v); it != memory->end()) {
                const json own = json::parse(it->second, nullptr, false);
                if (fitsPreset(own, p->chain)) {
                    merged = json::array();
                    for (const auto& e : p->chain)
                        if (isCleanupType(e.value("type", ""))) merged.push_back(e);
                    for (const auto& e : own) merged.push_back(e);
                    source = &merged;
                }
            }
        auto fx = chainFromJson(*source, nullptr);
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

    // Voices taken off now: their settings, to remember.
    std::set<std::string> gone;
    for (const auto& e : plan.fading)
        if (!e->voice.empty() && !isCleanupEffect(*e) && !isFading(e.get()) && !contains(want, e->voice)) gone.insert(e->voice);
    for (const auto& v : gone) {
        json own = json::array();
        for (const auto& e : chain)
            if (e->voice == v && !isCleanupEffect(*e) && !isFading(e.get())) own.push_back(effectToJson(*e));
        plan.remember[v] = own.dump();
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

json voiceEffectsJson(const std::vector<std::shared_ptr<Effect>>& chain, const std::string& voice) {
    json out = json::array();
    for (const auto& e : chain)
        if (e->voice == voice && !isCleanupEffect(*e)) out.push_back(effectToJson(*e));
    return out;
}

bool fitsPreset(const json& voiceEffects, const json& presetChain) {
    if (!voiceEffects.is_array()) return false;
    const auto want = presetVoiceEffects(presetChain);
    if (want.size() != voiceEffects.size()) return false;
    for (size_t i = 0; i < want.size(); ++i)
        if (!voiceEffects[i].is_object() || voiceEffects[i].value("type", "") != want[i]->value("type", "")) return false;
    return true;
}

bool resetVoiceEffects(const std::vector<std::shared_ptr<Effect>>& chain, const std::string& voice, const ChainPreset& preset) {
    std::vector<Effect*> live;
    for (const auto& e : chain)
        if (e->voice == voice && !isCleanupEffect(*e)) live.push_back(e.get());
    const auto want = presetVoiceEffects(preset.chain);
    if (live.size() != want.size()) return false;
    for (size_t i = 0; i < live.size(); ++i)
        if (want[i]->value("type", "") != live[i]->typeId()) return false;
    for (size_t i = 0; i < live.size(); ++i) {
        Effect& e = *live[i];
        const json& j = *want[i];
        e.resetParamsToDefault();
        if (auto it = j.find("params"); it != j.end() && it->is_object())
            for (auto p = it->begin(); p != it->end(); ++p) {
                const int idx = e.paramIndex(p.key());
                if (idx < 0) continue;
                if (p->is_number()) e.setParam(idx, p->get<float>());
                else if (p->is_boolean()) e.setParam(idx, p->get<bool>() ? 1.0f : 0.0f);
            }
        e.bypass = j.value("bypass", false);
        e.uiRole = j.value("ui", "");
        e.uiMakeup = j.value("ui_makeup", 0.0f);
        if (auto st = j.find("state"); st != j.end() && st->is_string()) e.loadState(st->get<std::string>());
    }
    return true;
}

void renameVoice(Session& s, const std::string& from, const std::string& to) {
    if (from == to) return;
    for (auto& st : s.strips) {
        for (auto& v : st.voices)
            if (v == from) v = to;
        for (auto& e : st.chain)
            if (e->voice == from) e->voice = to;
    }
    if (auto it = s.voiceSettings.find(from); it != s.voiceSettings.end()) {
        const std::string text = it->second;
        s.voiceSettings.erase(it);
        s.voiceSettings[to] = text;
    }
    for (auto& vk : s.hotkeys.voiceKeys)
        for (auto& v : vk.voices)
            if (v == from) v = to;
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
