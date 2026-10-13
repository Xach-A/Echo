// Combined voices on an input (session/Voices): tagging, order, taking a
// voice out without touching the others, picking vs mixing, fades, limits,
// saving; and the voice hotkey actions.
#include "doctest/doctest.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "TestUtil.h"
#include "dsp/Effect.h"
#include "dsp/EffectRegistry.h"
#include "dsp/NoiseSuppressorInfo.h"
#include "session/Hotkeys.h"
#include "session/Presets.h"
#include "session/Serialization.h"
#include "session/Voices.h"

using namespace echo;
using namespace echo::test;

namespace {

const std::vector<ChainPreset>& factory() {
    static const auto p = factoryChainPresets();
    return p;
}
const ChainPreset* findPreset(const std::string& n) {
    for (const auto& p : factory())
        if (p.name == n) return &p;
    return nullptr;
}
const PresetFinder kFind = [](const std::string& n) { return findPreset(n); };

using Chain = std::vector<std::shared_ptr<Effect>>;

std::shared_ptr<Effect> makeFx(const char* type) {
    auto e = std::shared_ptr<Effect>(EffectRegistry::instance().create(type).release());
    e->prepare(48000.0, 480);
    return e;
}

// What the app does once the fades are over: the faded effects leave.
Chain settle(const VoicePlan& plan) {
    Chain out;
    for (const auto& e : plan.chain)
        if (std::find(plan.fading.begin(), plan.fading.end(), e) == plan.fading.end()) out.push_back(e);
    return out;
}

Chain apply(const Chain& chain, const std::vector<std::string>& target, bool exclusive) {
    const VoicePlan plan = planVoices(chain, target, exclusive, kFind, {});
    REQUIRE(plan.error.empty());
    applyVoicePlan(plan, chain);
    return settle(plan);
}

int indexOf(const Chain& c, const char* type) {
    for (size_t i = 0; i < c.size(); ++i)
        if (std::string(c[i]->typeId()) == type) return static_cast<int>(i);
    return -1;
}

int count(const Chain& c, const std::string& type) {
    return static_cast<int>(std::count_if(c.begin(), c.end(), [&](const auto& e) { return type == e->typeId(); }));
}

} // namespace

TEST_CASE("voices: a voice's effects carry its name; the noise cleanup stays in front") {
    const Chain start = {makeFx(noiseSuppressorTypeId())};
    const VoicePlan plan = planVoices(start, {"Deep Voice"}, true, kFind, {});
    REQUIRE(plan.error.empty());
    CHECK(plan.voices == std::vector<std::string>{"Deep Voice"});
    CHECK(plan.fading.empty());
    REQUIRE(plan.chain.size() == 1 + findPreset("Deep Voice")->chain.size());
    CHECK(plan.chain[0] == start[0]);
    CHECK(plan.chain[0]->voice.empty());
    for (size_t i = 1; i < plan.chain.size(); ++i) {
        CHECK(plan.chain[i]->voice == "Deep Voice");
        CHECK(std::string(plan.chain[i]->typeId()) == findPreset("Deep Voice")->chain[i - 1].value("type", ""));
    }
    // Unknown names are skipped.
    CHECK(planVoices(start, {"No Such Voice"}, true, kFind, {}).voices.empty());
}

TEST_CASE("voices: mixed voices go in a sensible order, whatever order they are picked in") {
    Chain c = {makeFx(noiseSuppressorTypeId())};
    c = apply(c, {"Cave Echo"}, false);
    c = apply(c, {"Cave Echo", "Deep Voice"}, false);
    // Pitch first, echo and reverb last.
    CHECK(indexOf(c, "pitch") > 0);
    CHECK(indexOf(c, "pitch") < indexOf(c, "delay"));
    CHECK(indexOf(c, "delay") < indexOf(c, "reverb"));
    CHECK(std::string(c[0]->typeId()) == noiseSuppressorTypeId());
    CHECK(voiceStage(findPreset("Deep Voice")->chain) == 0);
    CHECK(voiceStage(findPreset("Pain")->chain) == 1);
    CHECK(voiceStage(findPreset("Cave Echo")->chain) == 2);
}

TEST_CASE("voices: taking one out of the mix leaves the others and their settings alone") {
    Chain c = apply({makeFx(noiseSuppressorTypeId())}, {"Deep Voice", "Pain"}, false);
    // The user turns up Pain's Intensity.
    Effect* painClip = nullptr;
    int distortions = 0;
    for (const auto& e : c)
        if (e->voice == "Pain" && std::string(e->typeId()) == "distortion" && ++distortions == 2) painClip = e.get();
    REQUIRE(painClip);
    painClip->setParam(painClip->paramIndex("drive"), 33.0f);
    const Chain before = c;
    std::vector<bool> wasBypassed;
    for (const auto& e : before) wasBypassed.push_back(e->bypass.load());

    const VoicePlan plan = planVoices(c, {"Pain"}, false, kFind, {});
    REQUIRE(plan.error.empty());
    applyVoicePlan(plan, c);
    // Deep Voice's effects fade out (bypassed, still in the chain until removed).
    for (const auto& e : plan.fading) {
        CHECK(e->voice == "Deep Voice");
        CHECK(e->bypass.load());
    }
    CHECK(plan.fading.size() == findPreset("Deep Voice")->chain.size());
    const Chain after = settle(plan);
    for (const auto& e : after) CHECK(e->voice != "Deep Voice");
    // Pain is exactly as it was: same effects, same settings, same on/off
    // states (its optional bit-crusher stays off).
    for (size_t i = 0; i < before.size(); ++i)
        if (before[i]->voice == "Pain") {
            CHECK(std::find(after.begin(), after.end(), before[i]) != after.end());
            CHECK(before[i]->bypass.load() == wasBypassed[i]);
        }
    CHECK(painClip->param(painClip->paramIndex("drive")) == doctest::Approx(33.0f));
}

TEST_CASE("voices: picking a voice replaces everything; mixing keeps effects added by hand") {
    Chain c = apply({makeFx(noiseSuppressorTypeId())}, {"Deep Voice"}, true);
    auto handEq = makeFx("eq");
    c.push_back(handEq); // added in the effects editor (no voice)

    const VoicePlan mix = planVoices(c, {"Deep Voice", "Cave Echo"}, false, kFind, {});
    CHECK(std::find(mix.fading.begin(), mix.fading.end(), handEq) == mix.fading.end());
    CHECK(std::find(mix.chain.begin(), mix.chain.end(), handEq) != mix.chain.end());

    const VoicePlan pick = planVoices(c, {"Robot"}, true, kFind, {});
    CHECK(std::find(pick.fading.begin(), pick.fading.end(), handEq) != pick.fading.end());
    const Chain after = settle(pick);
    CHECK(std::find(after.begin(), after.end(), handEq) == after.end());
    CHECK(pick.voices == std::vector<std::string>{"Robot"});
    CHECK(std::string(after[0]->typeId()) == noiseSuppressorTypeId()); // the cleanup survives a pick
    // Picking nothing = your own voice: only the cleanup is left.
    const Chain own = settle(planVoices(after, {}, true, kFind, {}));
    REQUIRE(own.size() == 1);
    CHECK(std::string(own[0]->typeId()) == noiseSuppressorTypeId());
}

TEST_CASE("voices: a voice put back while it is still fading out comes back as it was") {
    const Chain c = apply({makeFx(noiseSuppressorTypeId())}, {"Pain"}, false);
    const VoicePlan out = planVoices(c, {}, false, kFind, {});
    applyVoicePlan(out, c);
    std::set<const Effect*> fading;
    for (const auto& e : out.fading) fading.insert(e.get());
    REQUIRE(!fading.empty());

    const VoicePlan back = planVoices(out.chain, {"Pain"}, false, kFind, fading);
    REQUIRE(back.error.empty());
    CHECK(back.chain.size() == out.chain.size()); // nothing new created
    CHECK(back.fading.empty());
    CHECK(back.revived.size() == fading.size());
    applyVoicePlan(back, out.chain);
    for (const auto& e : back.chain) CHECK_FALSE(e->bypass.load());
}

TEST_CASE("voices: a mix never grows past one chain's limit") {
    Chain c = {makeFx(noiseSuppressorTypeId())};
    std::vector<std::string> voices;
    bool refused = false;
    for (const auto& p : factory()) {
        if (p.category != "Voice changer") continue;
        voices.push_back(p.name);
        const VoicePlan plan = planVoices(c, voices, false, kFind, {});
        if (!plan.error.empty()) {
            refused = true;
            CHECK(plan.chain.empty());
            break;
        }
        CHECK(plan.chain.size() <= static_cast<size_t>(kMaxChainLength));
        c = settle(plan);
    }
    CHECK(refused);
    // Picking a long voice over a long mix: the old effects fade out first,
    // the new ones are added afterwards.
    const VoicePlan pick = planVoices(c, {"Pain"}, true, kFind, {});
    if (pick.deferred) {
        CHECK(pick.chain.size() <= static_cast<size_t>(kMaxChainLength));
        const VoicePlan later = planVoices(settle(pick), {"Pain"}, true, kFind, {});
        CHECK(later.error.empty());
        CHECK_FALSE(later.deferred);
    }
}

TEST_CASE("voices: a voice with its own noise suppression does not double it") {
    const Chain c = {makeFx(noiseSuppressorTypeId())};
    const Chain mixed = apply(c, {"Streaming Voice"}, false);
    CHECK(count(mixed, noiseSuppressorTypeId()) == 1);
    CHECK(mixed[0] == c[0]); // the input's own one stays
    const Chain picked = apply(c, {"Streaming Voice"}, true);
    CHECK(count(picked, noiseSuppressorTypeId()) == 1);
    CHECK(std::string(picked[0]->typeId()) == noiseSuppressorTypeId());
    CHECK(picked[0]->voice.empty()); // cleanup is never a voice effect: it stays when the voice goes
    const Chain own = apply(picked, {}, true);
    CHECK(count(own, noiseSuppressorTypeId()) == 1);
}

TEST_CASE("voices: changing voices while audio plays does not click") {
    Chain c = apply({}, {"Deep Voice"}, true);
    for (auto& e : c) e->prepare(48000.0, 480);
    ChainRunner runner;
    runner.prepare(480);
    auto snapOf = [](const Chain& ch) {
        ChainSnap s;
        for (const auto& e : ch) s.fx[s.count++] = e.get();
        return s;
    };
    std::vector<float> out, block(480), r(480);
    size_t t = 0;
    auto run = [&](const Chain& ch, int blocks) {
        const ChainSnap snap = snapOf(ch);
        for (int b = 0; b < blocks; ++b) {
            for (int i = 0; i < 480; ++i, ++t) {
                double v = 0.0;
                for (int h = 1; h <= 8; ++h) v += std::sin(2 * kPiD * 220.0 * h * t / 48000.0) / h;
                block[i] = 0.1f * static_cast<float>(v);
            }
            bool mono = true;
            runner.process(snap, block.data(), r.data(), mono, 480);
            out.insert(out.end(), block.begin(), block.end());
        }
    };
    run(c, 100);
    // Add Cave Echo to the mix (fades in), then switch to only Robot (Deep
    // Voice and Cave Echo fade out, then leave the chain).
    VoicePlan add = planVoices(c, {"Deep Voice", "Cave Echo"}, false, kFind, {});
    applyVoicePlan(add, c);
    for (auto& e : add.chain) e->prepare(48000.0, 480);
    run(add.chain, 100);
    c = settle(add);
    VoicePlan pick = planVoices(c, {"Robot"}, true, kFind, {});
    applyVoicePlan(pick, c);
    for (auto& e : pick.chain)
        if (!e->isPrepared()) e->prepare(48000.0, 480);
    run(pick.chain, 4); // 40 ms of fading
    run(settle(pick), 100);
    CHECK(allFinite(out));
    auto maxStep = [&](size_t a, size_t b) {
        float m = 0.0f;
        for (size_t i = std::max<size_t>(a, 1); i < b; ++i) m = std::max(m, std::fabs(out[i] - out[i - 1]));
        return m;
    };
    const float steady = std::max({maxStep(50 * 480, 95 * 480), maxStep(150 * 480, 195 * 480), maxStep(260 * 480, 300 * 480)});
    const float sw1 = maxStep(99 * 480, 110 * 480), sw2 = maxStep(199 * 480, 215 * 480);
    MESSAGE("largest sample step: steady " << steady << ", adding a voice " << sw1 << ", switching voices " << sw2);
    CHECK(sw1 <= steady * 1.25f);
    CHECK(sw2 <= steady * 1.25f);
}

TEST_CASE("voices: saved with the session; sessions from before mixing are upgraded") {
    Session s;
    StripModel st;
    st.id = 1;
    st.name = "Microphone";
    st.chain = apply({makeFx(noiseSuppressorTypeId())}, {"Deep Voice", "Cave Echo"}, false);
    st.voices = {"Deep Voice", "Cave Echo"};
    s.strips.push_back(std::move(st));
    s.nextId = 2;
    Session r;
    REQUIRE(sessionFromJson(json::parse(sessionToJson(s).dump()), r, nullptr));
    REQUIRE(r.strips.size() == 1);
    CHECK(r.strips[0].voices == s.strips[0].voices);
    REQUIRE(r.strips[0].chain.size() == s.strips[0].chain.size());
    for (size_t i = 0; i < r.strips[0].chain.size(); ++i) CHECK(r.strips[0].chain[i]->voice == s.strips[0].chain[i]->voice);

    // A session written before voices could be mixed: one "voice" name and
    // untagged effects.
    json old = sessionToJson(s);
    json& sj = old["strips"][0];
    sj.erase("voices");
    sj["voice"] = "Deep Voice";
    for (auto& fx : sj["chain"]) fx.erase("voice");
    Session up;
    REQUIRE(sessionFromJson(old, up, nullptr));
    CHECK(up.strips[0].voices == std::vector<std::string>{"Deep Voice"});
    for (const auto& e : up.strips[0].chain) CHECK(e->voice == (isCleanupEffect(*e) ? "" : "Deep Voice"));
}

TEST_CASE("voice hotkeys: toggle, hold and switch") {
    std::vector<std::string> held;
    const VoiceHotkey toggle{{0x74, 0}, VoiceKeyMode::Toggle, {"Pain"}};
    auto v = voiceHotkeyResult(toggle, true, {"Deep Voice"}, held);
    CHECK(v == std::vector<std::string>{"Deep Voice", "Pain"});              // added to the mix
    CHECK(voiceHotkeyResult(toggle, false, v, held) == v);                   // release does nothing
    CHECK(voiceHotkeyResult(toggle, true, v, held) == std::vector<std::string>{"Deep Voice"}); // and out again

    const VoiceHotkey hold{{0x75, 0}, VoiceKeyMode::Hold, {"Pain", "Cave Echo"}};
    v = voiceHotkeyResult(hold, true, {"Cave Echo"}, held);
    CHECK(v == std::vector<std::string>{"Cave Echo", "Pain"});
    CHECK(held == std::vector<std::string>{"Pain"}); // Cave Echo was on already
    v = voiceHotkeyResult(hold, false, v, held);
    CHECK(v == std::vector<std::string>{"Cave Echo"}); // letting go keeps what was on before
    CHECK(held.empty());

    const VoiceHotkey sw{{0x76, 0}, VoiceKeyMode::Switch, {"Robot"}};
    v = voiceHotkeyResult(sw, true, {"Deep Voice", "Pain"}, held);
    CHECK(v == std::vector<std::string>{"Robot"});
    CHECK(voiceHotkeyResult(sw, true, v, held).empty()); // again: your own voice
}
