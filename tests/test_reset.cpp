// Settings > Reset to defaults: every part back to a fresh installation's
// settings, or only the parts the user chose.
#include "doctest/doctest.h"

#include <memory>

#include "FakeDevices.h"
#include "TestUtil.h"
#include "session/MixerController.h"
#include "session/Serialization.h"

using namespace echo;
using namespace echo::test;

namespace {

struct World {
    FakeBackend backend;
    std::unique_ptr<MixerController> mc;
    World() {
        backend.addDevice("mic", "USB Mic", false, 1, 48000, true);
        backend.addDevice("phones", "Headphones", true, 2, 48000, true);
        backend.addDevice("cable_in", "CABLE Input", true, 2, 48000, false, true);
        backend.addDevice("cable_out", "CABLE Output", false, 2, 48000, false, true);
        backend.cablePartners["cable_in"] = "cable_out";
        mc = std::make_unique<MixerController>(backend);
    }
};

// Changes something in every part of the settings.
void customize(MixerController& mc) {
    Session& s = mc.session();
    SourceRef app;
    app.kind = SourceKind::ProcessLoopback;
    app.processName = "game.exe";
    mc.addStrip(StripGroup::Application, app, "Game");
    s.strips[0].rt->faderDb = -12.0f;
    s.strips[0].voices = {"Pain"};
    mc.addEffect(ChainTarget{false, s.strips[0].id}, "reverb");
    s.autoNoiseSuppression = false;
    s.hotkeys.talkMode = TalkMode::PushToTalk;
    s.hotkeys.talkKey = {vk::XButton2, 0};
    s.hotkeys.muteKey = {'M', KeyModCtrl};
    s.hotkeys.voiceKeys.push_back(VoiceHotkey{{0x74, 0}, VoiceKeyMode::Toggle, {"Pain"}});
    s.soundboard.pads.erase(s.soundboard.pads.begin());
    s.soundboard.pads[0].key = {0x61, 0};
    s.soundboard.pads[1].gainDb = -9.0f;
    s.soundboard.stopAllKey = {0x62, 0};
    s.ui.theme = 1;
    s.ui.accent = "violet";
    s.ui.uiScale = 1.3f;
    s.ui.closeToTray = true;
    s.ui.showTooltips = false;
    s.ui.windowX = 120;
    s.ui.windowW = 1700;
    s.ui.maximized = true;
    s.engine.blockSize = 256;
    s.engine.resamplerQuality = ResamplerQuality::High;
    s.recorder.bitDepth = 16;
    s.recorder.folder = "D:\\\\Takes";
}

} // namespace

TEST_CASE("settings reset: everything back to a fresh installation's settings") {
    World w;
    w.mc->createDefaultSession();
    customize(*w.mc);
    w.mc->resetSettings(ResetParts{});
    const Session& s = w.mc->session();
    const Session fresh;

    // Mixer: microphone, desktop audio and soundboard; headphones and virtual mic.
    REQUIRE(s.strips.size() == 3);
    CHECK(s.strips[0].name == "Microphone");
    CHECK(s.strips[1].name == "Desktop Audio");
    CHECK(s.strips[2].source.kind == SourceKind::Soundboard);
    CHECK(s.strips[0].rt->faderDb.load() == doctest::Approx(0.0f));
    CHECK(s.strips[0].voices.empty());
    for (const auto& e : s.strips[0].chain) CHECK(std::string(e->typeId()) != "reverb");
    REQUIRE(s.buses.size() == 2);
    CHECK(s.buses[1].output.deviceId == "cable_in");
    CHECK(s.autoNoiseSuppression);
    // Hotkeys cleared.
    CHECK(s.hotkeys.talkMode == TalkMode::Off);
    CHECK(s.hotkeys.talkKey.empty());
    CHECK(s.hotkeys.muteKey.empty());
    CHECK(s.hotkeys.voiceKeys.empty());
    // Soundboard: the built-in sounds, no keys.
    REQUIRE(s.soundboard.pads.size() == builtinSounds().size());
    for (const auto& p : s.soundboard.pads) {
        CHECK(p.key.empty());
        CHECK(p.gainDb == 0.0f);
        CHECK_FALSE(p.builtin.empty());
    }
    CHECK(s.soundboard.stopAllKey.empty());
    // Look: defaults, but the window stays where it is.
    CHECK(s.ui.theme == fresh.ui.theme);
    CHECK(s.ui.accent == fresh.ui.accent);
    CHECK(s.ui.uiScale == doctest::Approx(fresh.ui.uiScale));
    CHECK(s.ui.closeToTray == fresh.ui.closeToTray);
    CHECK(s.ui.showTooltips == fresh.ui.showTooltips);
    CHECK(s.ui.windowX == 120);
    CHECK(s.ui.windowW == 1700);
    CHECK(s.ui.maximized);
    // Audio and recording.
    CHECK(s.engine.blockSize == fresh.engine.blockSize);
    CHECK(s.engine.resamplerQuality == fresh.engine.resamplerQuality);
    CHECK(w.mc->engine().blockSize() == fresh.engine.blockSize);
    CHECK(s.recorder.bitDepth == fresh.recorder.bitDepth);
    CHECK(s.recorder.folder.empty()); // the app fills in the default folder
    CHECK(w.mc->takeDirty());

    // The result saves and loads like any session.
    Session back;
    REQUIRE(sessionFromJson(json::parse(sessionToJson(s).dump()), back, nullptr));
    CHECK(back.strips.size() == 3);
}

TEST_CASE("settings reset: only the parts that were chosen") {
    SUBCASE("hotkeys only: everything else stays") {
        World w;
        w.mc->createDefaultSession();
        w.mc->start(false);
        customize(*w.mc);
        const uint32_t micId = w.mc->session().strips[0].id;
        const size_t strips = w.mc->session().strips.size();
        ResetParts parts{false, false, false, false, false};
        parts.hotkeys = true;
        w.mc->resetSettings(parts);
        const Session& s = w.mc->session();
        CHECK(s.hotkeys.voiceKeys.empty());
        CHECK(s.hotkeys.talkMode == TalkMode::Off);
        CHECK(s.strips.size() == strips);
        CHECK(s.strips[0].id == micId);
        CHECK(s.strips[0].rt->faderDb.load() == doctest::Approx(-12.0f));
        CHECK(s.soundboard.pads[0].key == KeyBinding{0x61, 0});
        CHECK(s.soundboard.stopAllKey == KeyBinding{0x62, 0});
        CHECK(s.ui.theme == 1);
        CHECK(s.engine.blockSize == 256);
        w.mc->stop();
    }
    SUBCASE("inputs and outputs only: hotkeys, soundboard and look stay") {
        World w;
        w.mc->createDefaultSession();
        customize(*w.mc);
        ResetParts parts{false, false, false, false, false};
        parts.mixer = true;
        w.mc->resetSettings(parts);
        const Session& s = w.mc->session();
        CHECK(s.strips.size() == 3);
        CHECK(s.strips[0].voices.empty());
        CHECK(s.hotkeys.voiceKeys.size() == 1);
        CHECK(s.hotkeys.talkKey == KeyBinding{vk::XButton2, 0});
        CHECK(s.soundboard.pads[0].key == KeyBinding{0x61, 0});
        CHECK(s.ui.accent == "violet");
        CHECK(s.engine.blockSize == 256);
        CHECK(s.recorder.bitDepth == 16);
    }
    SUBCASE("soundboard only: the built-in sounds come back, keys go") {
        World w;
        w.mc->createDefaultSession();
        customize(*w.mc);
        ResetParts parts{false, false, false, false, false};
        parts.soundboard = true;
        w.mc->resetSettings(parts);
        const Session& s = w.mc->session();
        CHECK(s.soundboard.pads.size() == builtinSounds().size());
        for (const auto& p : s.soundboard.pads) CHECK(p.key.empty());
        CHECK(s.hotkeys.voiceKeys.size() == 1);
        CHECK(s.strips.size() == 4); // the added "Game" input is still there
    }
    SUBCASE("nothing chosen: nothing changes") {
        World w;
        w.mc->createDefaultSession();
        customize(*w.mc);
        const std::string before = sessionToJson(w.mc->session()).dump();
        w.mc->takeDirty();
        CHECK_FALSE(ResetParts{false, false, false, false, false}.any());
        w.mc->resetSettings(ResetParts{false, false, false, false, false});
        CHECK(sessionToJson(w.mc->session()).dump() == before);
    }
}
