// Reads the push-to-talk / mute hotkeys on a small background thread.
//
// Keys and mouse buttons are polled with GetAsyncKeyState every 5 ms (every
// 50 ms while no hotkey is set). Polling needs no keyboard hook, does not
// delay or swallow input for other programs (the game still sees the key),
// and fails safe: if Windows stops reporting key state (secure desktop,
// lock screen), a push-to-talk microphone stays closed.
//
// The microphone gate is handed to the engine directly from this thread
// (onGate), so push to talk responds within one poll. Presses of the
// toggle-mute key are counted and applied by the UI thread.
#pragma once

#include <atomic>
#include <functional>
#include <mutex>
#include <optional>
#include <thread>

#include "session/Hotkeys.h"

namespace echo::win {

class HotkeyService {
public:
    enum Target { TalkKey = 0, MuteKey = 1 };

    HotkeyService();
    ~HotkeyService();

    // onGate is called on the polling thread with the initial state and
    // whenever the microphone gate opens or closes.
    void start(std::function<void(bool open)> onGate);
    void stop();

    void setSettings(const HotkeySettings& s);

    // ---- UI thread ----
    int takeMuteToggles() { return muteToggles_.exchange(0); }
    bool talkKeyHeld() const { return talkHeld_.load(std::memory_order_relaxed); }
    bool gateOpen() const { return gateOpen_.load(std::memory_order_relaxed); }

    // Waits for the next key or mouse button (Esc cancels); finished
    // captures are collected with takeCapture().
    void beginCapture(Target t);
    void cancelCapture();
    bool capturing() const { return captureTarget_.load() >= 0; }
    // A finished capture: returns the binding and which key it is for. A
    // cancelled capture returns nullopt and clears capturing().
    std::optional<KeyBinding> takeCapture(Target* target);

private:
    void threadMain();

    std::function<void(bool)> onGate_;
    std::thread thread_;
    std::atomic<bool> stop_{false};

    std::mutex mutex_;
    HotkeySettings settings_;
    uint64_t settingsVersion_ = 0;

    std::atomic<int> muteToggles_{0};
    std::atomic<bool> talkHeld_{false};
    std::atomic<bool> gateOpen_{true};

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
