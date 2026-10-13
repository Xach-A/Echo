// Every kind of key input: presses shorter than one poll (instant taps, as
// some mouse buttons and mouse software send), controller buttons, mouse
// wheel tilt and keyboard keys without a virtual-key code; their names; and
// saving voice hotkeys.
#include "doctest/doctest.h"

#include <vector>

#include "session/Hotkeys.h"
#include "session/Serialization.h"

using namespace echo;

namespace {

struct BigKeyTable final : KeyStateReader {
    std::vector<bool> down = std::vector<bool>(keycode::kMaxKey, false);
    bool isDown(int k) const override { return k > 0 && k < keycode::kMaxKey && down[static_cast<size_t>(k)]; }
};

// Key state the way the hotkey thread builds it from input events.
struct EventKeys final : KeyStateReader {
    const KeyEvents* ev;
    double now = 0.0;
    bool isDown(int k) const override { return ev->isDown(k, now, 1.0); }
};

} // namespace

TEST_CASE("hotkeys: a press shorter than one poll is still seen, exactly once") {
    KeyEvents ev;
    EventKeys keys;
    keys.ev = &ev;
    // Mouse 5 sent as an instant press-and-release between two polls.
    ev.press(vk::XButton2, 1.0);
    ev.release(vk::XButton2);
    keys.now = 1.004;
    CHECK(keys.isDown(vk::XButton2));
    PressDetector press;
    CHECK(press.update(bindingHeld(KeyBinding{vk::XButton2, 0}, keys)));
    ev.endPoll();
    keys.now = 1.009;
    CHECK_FALSE(keys.isDown(vk::XButton2));
    CHECK_FALSE(press.update(bindingHeld(KeyBinding{vk::XButton2, 0}, keys)));

    // Held across polls: down until released.
    ev.press(vk::XButton1, 2.0);
    ev.endPoll();
    keys.now = 2.3;
    CHECK(keys.isDown(vk::XButton1));
    ev.release(vk::XButton1);
    CHECK_FALSE(keys.isDown(vk::XButton1));

    // A press whose release never arrives (e.g. the lock screen ate it) does
    // not stay down forever.
    ev.press(keycode::scanKey(0x5F, true), 3.0);
    ev.endPoll();
    keys.now = 3.5;
    CHECK(keys.isDown(keycode::scanKey(0x5F, true)));
    keys.now = 4.5;
    CHECK_FALSE(keys.isDown(keycode::scanKey(0x5F, true)));
}

TEST_CASE("hotkeys: controller buttons, wheel tilt and keys without a key code can be chosen") {
    for (int key : {keycode::padKey(0, 12), keycode::padKey(1, keycode::kPadRightTrigger), keycode::kWheelLeft,
                    keycode::scanKey(0x6A, true), static_cast<int>(vk::XButton2), 0xA7 /* browser forward */}) {
        BigKeyTable k;
        KeyCapture c;
        c.start(k);
        CHECK(c.poll(k) == KeyCapture::Result::Waiting);
        k.down[static_cast<size_t>(key)] = true;
        REQUIRE(c.poll(k) == KeyCapture::Result::Done);
        CHECK(c.result().key == key);
        CHECK(isBindableKey(key));
    }
    // Controller buttons with a modifier held.
    BigKeyTable k;
    KeyCapture c;
    c.start(k);
    k.down[vk::LControl] = true;
    c.poll(k);
    k.down[static_cast<size_t>(keycode::padKey(0, 13))] = true;
    REQUIRE(c.poll(k) == KeyCapture::Result::Done);
    CHECK(c.result() == KeyBinding{keycode::padKey(0, 13), KeyModCtrl});

    CHECK_FALSE(isBindableKey(vk::LButton));
    CHECK_FALSE(isBindableKey(keycode::padKey(0, 20))); // no such button
    CHECK_FALSE(isBindableKey(keycode::kMaxKey));
    CHECK_FALSE(isBindableKey(keycode::kScanBase)); // scan code 0
}

TEST_CASE("hotkeys: names for every kind of input") {
    setKeyNameProvider(nullptr);
    CHECK(keyBindingName({vk::XButton1, 0}) == "Mouse 4");
    CHECK(keyBindingName({vk::XButton2, 0}) == "Mouse 5");
    CHECK(keyBindingName({keycode::padKey(0, 12), 0}) == "Controller A");
    CHECK(keyBindingName({keycode::padKey(1, 8), KeyModShift}) == "Shift+Controller 2 LB");
    CHECK(keyBindingName({keycode::padKey(0, keycode::kPadLeftTrigger), 0}) == "Controller LT");
    CHECK(keyBindingName({keycode::kWheelRight, 0}) == "Wheel tilt right");
    CHECK(keyBindingName({keycode::scanKey(0x6A, true), 0}) == "Key (scan E0 6A)");
    CHECK(keyBindingName({0xA6, 0}) == "Browser Back");
}

TEST_CASE("hotkeys: voice hotkeys survive saving and loading; unusable keys are dropped") {
    Session s;
    s.hotkeys.voiceKeys.push_back(VoiceHotkey{{0x74, 0}, VoiceKeyMode::Toggle, {"Pain"}});
    s.hotkeys.voiceKeys.push_back(VoiceHotkey{{keycode::padKey(0, 14), KeyModAlt}, VoiceKeyMode::Hold, {"Deep Voice", "Cave Echo"}});
    s.hotkeys.voiceKeys.push_back(VoiceHotkey{{keycode::kWheelLeft, 0}, VoiceKeyMode::Switch, {"Robot"}});
    json j = sessionToJson(s);
    Session r;
    REQUIRE(sessionFromJson(json::parse(j.dump()), r, nullptr));
    REQUIRE(r.hotkeys.voiceKeys.size() == 3);
    for (size_t i = 0; i < 3; ++i) {
        CHECK(r.hotkeys.voiceKeys[i].key == s.hotkeys.voiceKeys[i].key);
        CHECK(r.hotkeys.voiceKeys[i].mode == s.hotkeys.voiceKeys[i].mode);
        CHECK(r.hotkeys.voiceKeys[i].voices == s.hotkeys.voiceKeys[i].voices);
    }
    // A key code from nowhere and an out-of-range mode are not trusted.
    j["hotkeys"]["voiceKeys"][0]["key"]["key"] = 0x7777;
    j["hotkeys"]["voiceKeys"][1]["mode"] = 99;
    REQUIRE(sessionFromJson(j, r, nullptr));
    CHECK(r.hotkeys.voiceKeys[0].key.empty());
    CHECK(r.hotkeys.voiceKeys[1].mode == VoiceKeyMode::Switch);
}
