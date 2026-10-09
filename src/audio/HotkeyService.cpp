#include "audio/HotkeyService.h"

#include <windows.h>

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

#include "core/Common.h"
#include "core/Log.h"
#include "core/Text.h"

namespace echo::win {

namespace {

struct AsyncKeyReader final : KeyStateReader {
    bool isDown(int key) const override { return (GetAsyncKeyState(key) & 0x8000) != 0; }
};

} // namespace

std::string windowsKeyName(int key) {
    const UINT sc = MapVirtualKeyW(static_cast<UINT>(key), MAPVK_VK_TO_VSC);
    if (!sc) return {};
    LONG lp = static_cast<LONG>(sc) << 16;
    switch (key) {
    case VK_INSERT: case VK_DELETE: case VK_HOME: case VK_END: case VK_PRIOR: case VK_NEXT:
    case VK_LEFT: case VK_RIGHT: case VK_UP: case VK_DOWN: case VK_NUMLOCK: case VK_DIVIDE:
    case VK_SNAPSHOT: case VK_RCONTROL: case VK_RMENU: case VK_APPS:
        lp |= 1 << 24; // extended key
        break;
    default:
        break;
    }
    wchar_t buf[64];
    const int n = GetKeyNameTextW(lp, buf, 64);
    if (n <= 0) return {};
    return wideToUtf8(std::wstring(buf, static_cast<size_t>(n)));
}

HotkeyService::HotkeyService() { setKeyNameProvider(&windowsKeyName); }

HotkeyService::~HotkeyService() { stop(); }

void HotkeyService::start(std::function<void(bool)> onGate) {
    stop();
    onGate_ = std::move(onGate);
    stop_ = false;
    thread_ = std::thread([this] { threadMain(); });
}

void HotkeyService::stop() {
    if (!thread_.joinable()) return;
    stop_ = true;
    thread_.join();
}

void HotkeyService::setSettings(const HotkeySettings& s) {
    std::lock_guard<std::mutex> lock(mutex_);
    settings_ = s;
    ++settingsVersion_;
}

void HotkeyService::beginCapture(Target t) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        captured_.reset();
        capturedTarget_ = -1;
    }
    captureRestart_ = true;
    captureTarget_ = t;
}

void HotkeyService::cancelCapture() { captureTarget_ = -1; }

std::optional<KeyBinding> HotkeyService::takeCapture(Target* target) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!captured_) return std::nullopt;
    std::optional<KeyBinding> b = captured_;
    if (target) *target = static_cast<Target>(capturedTarget_);
    captured_.reset();
    capturedTarget_ = -1;
    return b;
}

void HotkeyService::threadMain() {
    SetThreadDescription(GetCurrentThread(), L"Echo hotkeys");
    AsyncKeyReader keys;
    HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    if (!timer) timer = CreateWaitableTimerW(nullptr, FALSE, nullptr);

    TalkGate gate;
    PressDetector mutePress;
    KeyCapture capture;
    HotkeySettings s;
    uint64_t version = ~0ull;
    bool lastOpen = true;
    bool first = true;
    while (!stop_.load(std::memory_order_acquire)) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (settingsVersion_ != version) {
                version = settingsVersion_;
                s = settings_;
                gate.configure(s.talkMode, s.releaseDelayMs, !s.talkKey.empty());
                mutePress.reset(bindingHeld(s.muteKey, keys));
            }
        }
        const bool held = s.talkMode != TalkMode::Off && bindingHeld(s.talkKey, keys);
        const bool open = gate.update(held, monotonicSeconds());
        talkHeld_.store(held, std::memory_order_relaxed);
        gateOpen_.store(open, std::memory_order_relaxed);
        if (open != lastOpen || first) {
            if (onGate_) onGate_(open);
            lastOpen = open;
            first = false;
        }

        const int target = captureTarget_.load();
        if (target >= 0) {
            if (captureRestart_.exchange(false)) capture.start(keys);
            const KeyCapture::Result r = capture.poll(keys);
            if (r != KeyCapture::Result::Waiting) {
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    if (r == KeyCapture::Result::Done) {
                        captured_ = capture.result();
                        capturedTarget_ = target;
                    }
                }
                int expected = target; // a new beginCapture() wins over this result
                captureTarget_.compare_exchange_strong(expected, -1);
                mutePress.reset(bindingHeld(s.muteKey, keys));
            }
        } else if (!s.muteKey.empty() && mutePress.update(bindingHeld(s.muteKey, keys))) {
            muteToggles_.fetch_add(1);
        }

        const bool idle = (s.talkMode == TalkMode::Off || s.talkKey.empty()) && s.muteKey.empty() && target < 0;
        const double wait = idle ? 0.05 : 0.005;
        if (timer) {
            LARGE_INTEGER due;
            due.QuadPart = -static_cast<LONGLONG>(wait * 1e7);
            SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE);
            WaitForSingleObject(timer, 100);
        } else {
            Sleep(static_cast<DWORD>(wait * 1000));
        }
    }
    if (timer) CloseHandle(timer);
}

} // namespace echo::win
