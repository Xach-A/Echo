// Push-to-talk / push-to-mute / toggle-mute hotkeys: settings and the
// platform-independent logic (key bindings, the talk gate, capturing a new
// binding). The Windows side that reads the keyboard and mouse lives in
// audio/HotkeyService.
#pragma once

#include <optional>
#include <string>
#include <vector>

namespace echo {

// Modifier keys that must be held together with a binding's key.
enum KeyMod : int { KeyModCtrl = 1, KeyModShift = 2, KeyModAlt = 4, KeyModWin = 8 };

// One key or mouse button, identified by its Windows virtual-key code, plus
// required modifiers. Left/right variants of Ctrl/Shift/Alt/Win (VK_LCONTROL
// ...) can be bindings of their own ("Right Ctrl").
struct KeyBinding {
    int key = 0;  // virtual-key code; 0 = not set
    int mods = 0; // KeyMod bits
    bool empty() const { return key == 0; }
    bool operator==(const KeyBinding& o) const { return key == o.key && mods == o.mods; }
    bool operator!=(const KeyBinding& o) const { return !(*this == o); }
};

enum class TalkMode : int {
    Off = 0,        // microphones are always live (unless muted)
    PushToTalk = 1, // microphones are silent unless the talk key is held
    PushToMute = 2, // microphones are live unless the talk key is held
};

struct HotkeySettings {
    TalkMode talkMode = TalkMode::Off;
    KeyBinding talkKey;
    KeyBinding muteKey;       // toggles microphone mute
    int releaseDelayMs = 200; // push to talk: stays open this long after release
};

// Virtual-key codes the logic needs to know about (values from WinUser.h,
// repeated here so this file builds and tests without Windows headers).
namespace vk {
constexpr int LButton = 0x01, RButton = 0x02, MButton = 0x04, XButton1 = 0x05, XButton2 = 0x06;
constexpr int Shift = 0x10, Control = 0x11, Menu = 0x12, Escape = 0x1B;
constexpr int LWin = 0x5B, RWin = 0x5C;
constexpr int LShift = 0xA0, RShift = 0xA1, LControl = 0xA2, RControl = 0xA3, LMenu = 0xA4, RMenu = 0xA5;
} // namespace vk

bool isModifierKey(int key);
// Bindings that would get in the way of using a mouse: left/right click.
bool isBindableKey(int key);

// Reads the current up/down state of keys (GetAsyncKeyState on Windows, a
// table in tests).
class KeyStateReader {
public:
    virtual ~KeyStateReader() = default;
    virtual bool isDown(int key) const = 0;
};

int heldModifiers(const KeyStateReader& keys);
// True while the binding's key and all of its modifiers are held.
bool bindingHeld(const KeyBinding& b, const KeyStateReader& keys);

// Decides whether microphones may be heard, from the talk key's state.
// Push to talk opens immediately on press and closes `releaseDelayMs` after
// release, so the ends of words are not cut off. Without a key set, the
// mode behaves like Off (never a permanently closed microphone).
class TalkGate {
public:
    void configure(TalkMode mode, int releaseDelayMs, bool hasKey);
    // Returns whether the microphone gate is open. `now` in seconds.
    bool update(bool keyHeld, double now);
    bool open() const { return open_; }

private:
    TalkMode mode_ = TalkMode::Off;
    double releaseDelay_ = 0.2;
    bool hasKey_ = false;
    bool open_ = true;
    bool held_ = false;
    double releasedAt_ = -1.0;
};

// Rising-edge detector for the toggle-mute key.
class PressDetector {
public:
    // True once per press.
    bool update(bool held) {
        const bool pressed = held && !was_;
        was_ = held;
        return pressed;
    }
    void reset(bool held) { was_ = held; }

private:
    bool was_ = false;
};

// Records the next key or mouse button the user presses ("Press a key...").
// Keys already down when capturing starts are ignored until released (the
// click on the button that started it). A modifier pressed and released on
// its own becomes the binding ("Right Ctrl"); a modifier held while another
// key goes down becomes part of that key's binding ("Ctrl+M"). Escape
// cancels.
class KeyCapture {
public:
    enum class Result { Waiting, Done, Cancelled };
    void start(const KeyStateReader& keys);
    Result poll(const KeyStateReader& keys);
    bool active() const { return active_; }
    void cancel() { active_ = false; }
    KeyBinding result() const { return result_; }

    // Keys checked while capturing.
    static const std::vector<int>& candidateKeys();

private:
    bool active_ = false;
    bool down_[256] = {};
    bool ignore_[256] = {};
    int pendingModifier_ = 0;
    KeyBinding result_;
};

// "Ctrl+M", "Mouse 5", "Right Ctrl", "F13". keyName() is provided by the
// platform layer when available (key names depend on the keyboard layout);
// the fallback covers mouse buttons, modifiers, letters, digits and F-keys.
std::string keyBindingName(const KeyBinding& b);
using KeyNameFn = std::string (*)(int key);
void setKeyNameProvider(KeyNameFn fn);

const char* talkModeName(TalkMode m);

} // namespace echo
