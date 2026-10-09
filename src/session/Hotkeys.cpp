#include "session/Hotkeys.h"

#include <cstdio>

namespace echo {

namespace {
KeyNameFn g_keyName = nullptr;
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
    return key > 0 && key < 256 && key != vk::LButton && key != vk::RButton && key != vk::Escape;
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

void setKeyNameProvider(KeyNameFn fn) { g_keyName = fn; }

namespace {
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
    default: break;
    }
    if (key >= 0x70 && key <= 0x87) return "F" + std::to_string(key - 0x70 + 1);
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
        } else {
            switch (b.key) {
            case 0x08: name = "Backspace"; break;
            case 0x09: name = "Tab"; break;
            case 0x0D: name = "Enter"; break;
            case 0x14: name = "Caps Lock"; break;
            case 0x20: name = "Space"; break;
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

} // namespace echo
