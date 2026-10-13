#include "session/Hotkeys.h"

#include <algorithm>
#include <cstdio>

namespace echo {

namespace {
KeyNameFn g_keyName = nullptr;

bool contains(const std::vector<std::string>& v, const std::string& s) { return std::find(v.begin(), v.end(), s) != v.end(); }
} // namespace

bool isModifierKey(int key) {
    switch (key) {
    case vk::Shift: case vk::Control: case vk::Menu:
    case vk::LShift: case vk::RShift: case vk::LControl: case vk::RControl: case vk::LMenu: case vk::RMenu:
    case vk::LWin: case vk::RWin:
        return true;
    default:
        return false;
    }
}

bool isBindableKey(int key) {
    if (key > 0 && key < 0xFF) return key != vk::LButton && key != vk::RButton && key != vk::Escape;
    return keycode::isScanKey(key) || keycode::isWheelKey(key) || keycode::isPadKey(key);
}

int heldModifiers(const KeyStateReader& keys) {
    int m = 0;
    if (keys.isDown(vk::LControl) || keys.isDown(vk::RControl)) m |= KeyModCtrl;
    if (keys.isDown(vk::LShift) || keys.isDown(vk::RShift)) m |= KeyModShift;
    if (keys.isDown(vk::LMenu) || keys.isDown(vk::RMenu)) m |= KeyModAlt;
    if (keys.isDown(vk::LWin) || keys.isDown(vk::RWin)) m |= KeyModWin;
    return m;
}

bool bindingHeld(const KeyBinding& b, const KeyStateReader& keys) {
    if (b.empty() || !keys.isDown(b.key)) return false;
    return b.mods == 0 || (heldModifiers(keys) & b.mods) == b.mods;
}

// ---------------------------------------------------------------------------

void KeyEvents::press(int key, double now) {
    if (!valid(key)) return;
    down_[key] = true;
    tapped_[key] = true;
    last_[key] = now;
}

void KeyEvents::release(int key) {
    if (valid(key)) down_[key] = false;
}

void KeyEvents::releaseAll() {
    for (bool& d : down_) d = false;
}

bool KeyEvents::isDown(int key, double now, double timeout) const {
    if (!valid(key)) return false;
    return tapped_[key] || (down_[key] && now - last_[key] < timeout);
}

void KeyEvents::endPoll() {
    for (bool& t : tapped_) t = false;
}

// ---------------------------------------------------------------------------

void TalkGate::configure(TalkMode mode, int releaseDelayMs, bool hasKey) {
    const bool changed = mode != mode_ || hasKey != hasKey_;
    mode_ = mode;
    hasKey_ = hasKey;
    releaseDelay_ = (releaseDelayMs < 0 ? 0 : releaseDelayMs) / 1000.0;
    if (changed) {
        held_ = false;
        releasedAt_ = -1.0;
        open_ = mode_ != TalkMode::PushToTalk || !hasKey_;
    }
}

bool TalkGate::update(bool keyHeld, double now) {
    if (mode_ == TalkMode::Off || !hasKey_) {
        open_ = true;
    } else if (mode_ == TalkMode::PushToMute) {
        open_ = !keyHeld;
    } else if (keyHeld) {
        open_ = true;
        releasedAt_ = -1.0;
    } else {
        if (held_) releasedAt_ = now;
        open_ = releasedAt_ >= 0.0 && now - releasedAt_ < releaseDelay_;
    }
    held_ = keyHeld;
    return open_;
}

// ---------------------------------------------------------------------------

const std::vector<int>& KeyCapture::candidateKeys() {
    static const std::vector<int> keys = [] {
        std::vector<int> k;
        for (int v = 1; v < 255; ++v) {
            // The generic Shift/Ctrl/Alt codes duplicate the left/right ones.
            if (v == vk::Shift || v == vk::Control || v == vk::Menu) continue;
            k.push_back(v);
        }
        for (int v = keycode::kScanBase + 1; v < keycode::kScanBase + 0x100; ++v) k.push_back(v);
        k.push_back(keycode::kWheelLeft);
        k.push_back(keycode::kWheelRight);
        for (int pad = 0; pad < keycode::kMaxPads; ++pad)
            for (int b = 0; b < keycode::kPadButtons; ++b) k.push_back(keycode::padKey(pad, b));
        return k;
    }();
    return keys;
}

void KeyCapture::start(const KeyStateReader& keys) {
    active_ = true;
    pendingModifier_ = 0;
    result_ = {};
    for (int k : candidateKeys()) {
        down_[k] = keys.isDown(k);
        ignore_[k] = down_[k];
    }
}

KeyCapture::Result KeyCapture::poll(const KeyStateReader& keys) {
    if (!active_) return Result::Cancelled;
    for (int k : candidateKeys()) {
        const bool d = keys.isDown(k);
        if (!d) {
            if (down_[k] && k == pendingModifier_ && !ignore_[k]) {
                // A modifier pressed and released on its own.
                result_ = {k, 0};
                active_ = false;
                return Result::Done;
            }
            down_[k] = false;
            ignore_[k] = false;
            continue;
        }
        if (down_[k]) continue; // still held
        down_[k] = true;
        if (ignore_[k]) continue;
        if (k == vk::Escape) {
            active_ = false;
            return Result::Cancelled;
        }
        if (!isBindableKey(k)) continue;
        if (isModifierKey(k)) {
            if (!pendingModifier_) pendingModifier_ = k;
            continue;
        }
        result_ = {k, heldModifiers(keys)};
        active_ = false;
        return Result::Done;
    }
    return Result::Waiting;
}

// ---------------------------------------------------------------------------

std::vector<std::string> voiceHotkeyResult(const VoiceHotkey& h, bool down, const std::vector<std::string>& current,
                                           std::vector<std::string>& heldAdded) {
    std::vector<std::string> out = current;
    switch (h.mode) {
    case VoiceKeyMode::Toggle: {
        if (!down || h.voices.empty()) return out;
        const bool allOn = std::all_of(h.voices.begin(), h.voices.end(), [&](const std::string& v) { return contains(current, v); });
        if (allOn) {
            out.erase(std::remove_if(out.begin(), out.end(), [&](const std::string& v) { return contains(h.voices, v); }), out.end());
        } else {
            for (const auto& v : h.voices)
                if (!contains(out, v)) out.push_back(v);
        }
        return out;
    }
    case VoiceKeyMode::Hold:
        if (down) {
            heldAdded.clear();
            for (const auto& v : h.voices)
                if (!contains(out, v)) {
                    out.push_back(v);
                    heldAdded.push_back(v);
                }
        } else {
            out.erase(std::remove_if(out.begin(), out.end(), [&](const std::string& v) { return contains(heldAdded, v); }), out.end());
            heldAdded.clear();
        }
        return out;
    case VoiceKeyMode::Switch: {
        if (!down || h.voices.empty()) return out;
        const bool same = current.size() == h.voices.size() &&
                          std::all_of(h.voices.begin(), h.voices.end(), [&](const std::string& v) { return contains(current, v); });
        return same ? std::vector<std::string>{} : h.voices;
    }
    }
    return out;
}

// ---------------------------------------------------------------------------

void setKeyNameProvider(KeyNameFn fn) { g_keyName = fn; }

namespace {
std::string padButtonName(int button) {
    static const char* const names[keycode::kPadButtons] = {
        "D-pad up", "D-pad down", "D-pad left", "D-pad right", "Menu", "View", "Left stick", "Right stick", "LB", "RB",
        "Button 11", "Button 12", "A", "B", "X", "Y", "LT", "RT"};
    return button >= 0 && button < keycode::kPadButtons ? names[button] : "?";
}

std::string fixedKeyName(int key) {
    switch (key) {
    case vk::MButton: return "Middle mouse button";
    case vk::XButton1: return "Mouse 4";
    case vk::XButton2: return "Mouse 5";
    case vk::LControl: return "Left Ctrl";
    case vk::RControl: return "Right Ctrl";
    case vk::LShift: return "Left Shift";
    case vk::RShift: return "Right Shift";
    case vk::LMenu: return "Left Alt";
    case vk::RMenu: return "Right Alt";
    case vk::LWin: return "Left Windows";
    case vk::RWin: return "Right Windows";
    case keycode::kWheelLeft: return "Wheel tilt left";
    case keycode::kWheelRight: return "Wheel tilt right";
    default: break;
    }
    if (key >= 0x70 && key <= 0x87) return "F" + std::to_string(key - 0x70 + 1);
    if (keycode::isPadKey(key)) {
        const int pad = (key - keycode::kPadBase) / 32, button = (key - keycode::kPadBase) % 32;
        return (pad ? "Controller " + std::to_string(pad + 1) + " " : std::string("Controller ")) + padButtonName(button);
    }
    return {};
}
} // namespace

std::string keyBindingName(const KeyBinding& b) {
    if (b.empty()) return "Not set";
    std::string s;
    if (b.mods & KeyModCtrl) s += "Ctrl+";
    if (b.mods & KeyModShift) s += "Shift+";
    if (b.mods & KeyModAlt) s += "Alt+";
    if (b.mods & KeyModWin) s += "Win+";
    std::string name = fixedKeyName(b.key);
    if (name.empty() && g_keyName) name = g_keyName(b.key);
    if (name.empty()) {
        if ((b.key >= 'A' && b.key <= 'Z') || (b.key >= '0' && b.key <= '9')) {
            name = std::string(1, static_cast<char>(b.key));
        } else if (b.key >= 0x60 && b.key <= 0x69) {
            name = "Num " + std::to_string(b.key - 0x60);
        } else if (keycode::isScanKey(b.key)) {
            char buf[32];
            std::snprintf(buf, sizeof(buf), "Key (scan %s%02X)", b.key - keycode::kScanBase >= 0x80 ? "E0 " : "",
                          (b.key - keycode::kScanBase) & 0x7F);
            name = buf;
        } else {
            switch (b.key) {
            case 0x08: name = "Backspace"; break;
            case 0x09: name = "Tab"; break;
            case 0x0D: name = "Enter"; break;
            case 0x14: name = "Caps Lock"; break;
            case 0x20: name = "Space"; break;
            case 0xA6: name = "Browser Back"; break;
            case 0xA7: name = "Browser Forward"; break;
            case 0xAD: name = "Volume Mute"; break;
            case 0xAE: name = "Volume Down"; break;
            case 0xAF: name = "Volume Up"; break;
            case 0xB0: name = "Next Track"; break;
            case 0xB1: name = "Previous Track"; break;
            case 0xB3: name = "Play/Pause"; break;
            default: {
                char buf[16];
                std::snprintf(buf, sizeof(buf), "Key 0x%02X", b.key);
                name = buf;
            }
            }
        }
    }
    return s + name;
}

const char* talkModeName(TalkMode m) {
    switch (m) {
    case TalkMode::PushToTalk: return "Push to talk";
    case TalkMode::PushToMute: return "Push to mute";
    default: return "Off";
    }
}

const char* voiceKeyModeName(VoiceKeyMode m) {
    switch (m) {
    case VoiceKeyMode::Hold: return "Hold";
    case VoiceKeyMode::Switch: return "Switch";
    default: return "Toggle";
    }
}

} // namespace echo
