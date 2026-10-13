// Reads hotkeys (push to talk, mute, voice and soundboard keys) on a small
// background thread.
//
// Keys and mouse buttons are polled with GetAsyncKeyState every 5 ms (every
// 50 ms while no hotkey is set), and Windows raw input reports every key and
// mouse button event as it happens: presses shorter than a poll (some mice
// and mouse software send a button as an instant press-and-release), keys
// Windows has no virtual-key code for, and mouse wheel tilt. Xbox-style game
// controllers are read with XInput. None of this needs a keyboard hook: it
// does not delay or swallow input for other programs (the game still sees
// the key), and it fails safe: if Windows stops reporting key state (secure
// desktop, lock screen), a push-to-talk microphone stays closed.
//
// The microphone gate is handed to the engine directly from this thread
// (onGate), so push to talk responds within one poll; so are hotkey actions
// (onAction, used for soundboard sounds). Presses of the toggle-mute key and
// action events are also queued for the UI thread.
#pragma once

#include <atomic>
#include <functional>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

#include "session/Hotkeys.h"

namespace echo::win {

class HotkeyService {
public:
    // Capture targets used by the push-to-talk settings; callers may use any
    // other number for their own bindings.
    enum Target { TalkKey = 0, MuteKey = 1 };

    // A binding the thread watches; its presses and releases are reported
    // with `id`.
    struct Action {
        int id = 0;
        KeyBinding key;
    };
    struct Event {
        int id = 0;
        bool down = false;
    };

    HotkeyService();
    ~HotkeyService();

    // onGate is called on the polling thread with the initial state and
    // whenever the microphone gate opens or closes; onAction (optional) on
    // every press and release of an action's binding.
    void start(std::function<void(bool open)> onGate, std::function<void(int id, bool down)> onAction = nullptr);
    void stop();

    void setSettings(const HotkeySettings& s);
    void setActions(std::vector<Action> actions);

    // ---- UI thread ----
    int takeMuteToggles() { return muteToggles_.exchange(0); }
    std::vector<Event> takeEvents();
    bool talkKeyHeld() const { return talkHeld_.load(std::memory_order_relaxed); }
    bool gateOpen() const { return gateOpen_.load(std::memory_order_relaxed); }
    // Diagnostics: raw input registered, game controllers connected.
    bool rawInputActive() const { return rawActive_.load(std::memory_order_relaxed); }
    int controllersConnected() const { return padsConnected_.load(std::memory_order_relaxed); }

    // Waits for the next key, mouse button or controller button (Esc
    // cancels); finished captures are collected with takeCapture().
    void beginCapture(int target);
    void cancelCapture();
    bool capturing() const { return captureTarget_.load() >= 0; }
    // A finished capture: returns the binding and which target it is for. A
    // cancelled capture returns nullopt and clears capturing().
    std::optional<KeyBinding> takeCapture(int* target);

private:
    void threadMain();

    std::function<void(bool)> onGate_;
    std::function<void(int, bool)> onAction_;
    std::thread thread_;
    std::atomic<bool> stop_{false};

    std::mutex mutex_;
    HotkeySettings settings_;
    std::vector<Action> actions_;
    uint64_t settingsVersion_ = 0;
    std::vector<Event> events_; // guarded by mutex_

    std::atomic<int> muteToggles_{0};
    std::atomic<bool> talkHeld_{false};
    std::atomic<bool> gateOpen_{true};
    std::atomic<bool> rawActive_{false};
    std::atomic<int> padsConnected_{0};

    // capture
    std::atomic<int> captureTarget_{-1}; // -1 = not capturing
    std::atomic<bool> captureRestart_{false};
    std::optional<KeyBinding> captured_;  // guarded by mutex_
    int capturedTarget_ = -1;             // guarded by mutex_
};

// Layout-aware key name (GetKeyNameText); registered with
// setKeyNameProvider() by HotkeyService.
std::string windowsKeyName(int key);

} // namespace echo::win
