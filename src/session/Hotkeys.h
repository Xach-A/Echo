// Hotkeys: push to talk / push to mute / toggle mute, voice hotkeys and the
// platform-independent logic (key bindings, the talk gate, capturing a new
// binding, voice hotkey actions). The Windows side that reads the keyboard,
// mouse and game controllers lives in audio/HotkeyService.
#pragma once

#include <optional>
#include <string>
#include <vector>

namespace echo {

// Modifier keys that must be held together with a binding's key.
enum KeyMod : int { KeyModCtrl = 1, KeyModShift = 2, KeyModAlt = 4, KeyModWin = 8 };

// Key codes. 1-254 are Windows virtual-key codes (keyboard keys, mouse
// buttons, media keys...). Inputs Windows gives no virtual-key code get
// codes of their own above that.
namespace keycode {
constexpr int kScanBase = 0x100;  // keyboard keys without a virtual-key code: 0x100 + scan code (+0x80 when extended)
constexpr int kWheelLeft = 0x200; // mouse wheel tilted left / right (some mice's extra buttons)
constexpr int kWheelRight = 0x201;
constexpr int kPadBase = 0x300;   // game controller buttons: 0x300 + controller * 32 + button
constexpr int kPadButtons = 18;   // the 16 XInput button bits (14 used) + left and right trigger
constexpr int kPadLeftTrigger = 16, kPadRightTrigger = 17;
constexpr int kMaxPads = 4;
constexpr int kMaxKey = kPadBase + kMaxPads * 32; // exclusive upper bound

inline int scanKey(int scanCode, bool extended) { return kScanBase + (scanCode & 0x7F) + (extended ? 0x80 : 0); }
inline int padKey(int pad, int button) { return kPadBase + pad * 32 + button; }
inline bool isScanKey(int k) { return k > kScanBase && k < kScanBase + 0x100; }
inline bool isWheelKey(int k) { return k == kWheelLeft || k == kWheelRight; }
inline bool isPadKey(int k) { return k >= kPadBase && k < kMaxKey && (k - kPadBase) % 32 < kPadButtons; }
} // namespace keycode

// One key, mouse button or controller button (see keycode), plus required
// modifiers. Left/right variants of Ctrl/Shift/Alt/Win (VK_LCONTROL ...) can
// be bindings of their own ("Right Ctrl").
struct KeyBinding {
    int key = 0;  // key code; 0 = not set
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

// What a voice hotkey does with its voices (on the main microphone).
enum class VoiceKeyMode : int {
    Toggle = 0, // press: add the voices to the current mix, press again: take them out
    Hold = 1,   // the voices are on while the key is held (like push to talk)
    Switch = 2, // press: use only these voices, press again: back to your own voice
};
constexpr int kVoiceKeyModes = 3;

struct VoiceHotkey {
    KeyBinding key;
    VoiceKeyMode mode = VoiceKeyMode::Toggle;
    std::vector<std::string> voices; // voice preset names
};

struct HotkeySettings {
    TalkMode talkMode = TalkMode::Off;
    KeyBinding talkKey;
    KeyBinding muteKey;       // toggles microphone mute
    int releaseDelayMs = 200; // push to talk: stays open this long after release
    std::vector<VoiceHotkey> voiceKeys;
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
// Any key code above except left/right click and Esc (which cancels).
bool isBindableKey(int key);

// Reads the current up/down state of keys (Windows input on Windows, a
// table in tests).
class KeyStateReader {
public:
    virtual ~KeyStateReader() = default;
    virtual bool isDown(int key) const = 0;
};

int heldModifiers(const KeyStateReader& keys);
// True while the binding's key and all of its modifiers are held.
bool bindingHeld(const KeyBinding& b, const KeyStateReader& keys);

// Key presses reported as events (Windows raw input). A key that goes down
// and up again between two polls (an instant tap, as some mice, mouse
// software and macro keys send) still counts as down for one poll, so it is
// never missed. Keys that only report presses (auto-repeat, wheel tilt)
// count as released once their events stop for `timeout` seconds.
class KeyEvents {
public:
    void press(int key, double now);
    void release(int key);
    void releaseAll();
    // Down now (and not timed out), or pressed since the last endPoll().
    bool isDown(int key, double now, double timeout) const;
    bool tapped(int key) const { return valid(key) && tapped_[key]; }
    void endPoll();

private:
    static bool valid(int k) { return k > 0 && k < keycode::kMaxKey; }
    bool down_[keycode::kMaxKey] = {};
    bool tapped_[keycode::kMaxKey] = {};
    double last_[keycode::kMaxKey] = {};
};

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

// Records the next key, mouse button or controller button the user presses
// ("Press a key..."). Keys already down when capturing starts are ignored
// until released (the click on the button that started it). A modifier
// pressed and released on its own becomes the binding ("Right Ctrl"); a
// modifier held while another key goes down becomes part of that key's
// binding ("Ctrl+M"). Escape cancels.
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
    bool down_[keycode::kMaxKey] = {};
    bool ignore_[keycode::kMaxKey] = {};
    int pendingModifier_ = 0;
    KeyBinding result_;
};

// A voice hotkey pressed or released: the voices to use afterwards, given the
// voices in use now. `heldAdded` remembers which voices a Hold key added (so
// releasing it does not take out voices that were on anyway); keep one per
// hotkey between calls.
std::vector<std::string> voiceHotkeyResult(const VoiceHotkey& h, bool down, const std::vector<std::string>& current,
                                           std::vector<std::string>& heldAdded);

// "Ctrl+M", "Mouse 5", "Right Ctrl", "F13", "Controller A". keyName() is
// provided by the platform layer when available (key names depend on the
// keyboard layout); the fallback covers mouse buttons, modifiers, letters,
// digits, F-keys and Echo's own codes.
std::string keyBindingName(const KeyBinding& b);
using KeyNameFn = std::string (*)(int key);
void setKeyNameProvider(KeyNameFn fn);

const char* talkModeName(TalkMode m);
const char* voiceKeyModeName(VoiceKeyMode m);

} // namespace echo
