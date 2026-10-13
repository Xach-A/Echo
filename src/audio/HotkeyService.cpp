#include "audio/HotkeyService.h"

#include <windows.h>
#include <xinput.h>

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

#include "core/Common.h"
#include "core/Log.h"
#include "core/Text.h"

namespace echo::win {

namespace {

// Everything the raw input window procedure updates (hotkey thread only).
struct InputState {
    KeyEvents events;
};

// Raw input events -> key codes. Runs on the hotkey thread (the message-only
// window belongs to it).
void handleRawInput(InputState& st, HRAWINPUT h) {
    alignas(8) BYTE buf[sizeof(RAWINPUT) + 16];
    UINT size = sizeof(buf);
    if (GetRawInputData(h, RID_INPUT, buf, &size, sizeof(RAWINPUTHEADER)) == static_cast<UINT>(-1)) return;
    const RAWINPUT* ri = reinterpret_cast<const RAWINPUT*>(buf);
    const double now = monotonicSeconds();
    if (ri->header.dwType == RIM_TYPEKEYBOARD) {
        const RAWKEYBOARD& kb = ri->data.keyboard;
        const bool up = (kb.Flags & RI_KEY_BREAK) != 0;
        const bool e0 = (kb.Flags & RI_KEY_E0) != 0;
        const int make = kb.MakeCode;
        int vkey = kb.VKey;
        // The fake Shift Windows sends around some extended keys (Insert,
        // arrows with Num Lock on...) is not a key press.
        if ((make == 0x2A || make == 0x36) && e0) return;
        switch (vkey) {
        case VK_SHIFT: vkey = make == 0x36 ? VK_RSHIFT : VK_LSHIFT; break;
        case VK_CONTROL: vkey = e0 ? VK_RCONTROL : VK_LCONTROL; break;
        case VK_MENU: vkey = e0 ? VK_RMENU : VK_LMENU; break;
        default: break;
        }
        int key = 0;
        if (vkey > 0 && vkey < 0xFF) key = vkey;
        else if (make > 0 && make < 0x80) key = keycode::scanKey(make, e0); // no virtual-key code
        if (!key) return;
        if (up) st.events.release(key);
        else st.events.press(key, now);
    } else if (ri->header.dwType == RIM_TYPEMOUSE) {
        const USHORT f = ri->data.mouse.usButtonFlags;
        struct Map {
            USHORT down, up;
            int key;
        };
        static const Map buttons[] = {{RI_MOUSE_LEFT_BUTTON_DOWN, RI_MOUSE_LEFT_BUTTON_UP, VK_LBUTTON},
                                      {RI_MOUSE_RIGHT_BUTTON_DOWN, RI_MOUSE_RIGHT_BUTTON_UP, VK_RBUTTON},
                                      {RI_MOUSE_MIDDLE_BUTTON_DOWN, RI_MOUSE_MIDDLE_BUTTON_UP, VK_MBUTTON},
                                      {RI_MOUSE_BUTTON_4_DOWN, RI_MOUSE_BUTTON_4_UP, VK_XBUTTON1},
                                      {RI_MOUSE_BUTTON_5_DOWN, RI_MOUSE_BUTTON_5_UP, VK_XBUTTON2}};
        for (const Map& m : buttons) {
            if (f & m.down) st.events.press(m.key, now);
            if (f & m.up) st.events.release(m.key);
        }
        if (f & RI_MOUSE_HWHEEL) {
            const short delta = static_cast<short>(ri->data.mouse.usButtonData);
            if (delta) st.events.press(delta > 0 ? keycode::kWheelRight : keycode::kWheelLeft, now);
        }
    }
}

LRESULT CALLBACK inputWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_INPUT) {
        if (auto* st = reinterpret_cast<InputState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA)))
            handleRawInput(*st, reinterpret_cast<HRAWINPUT>(lp));
        return DefWindowProcW(hwnd, msg, wp, lp); // lets Windows clean up the input data
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// XInput, loaded at run time (part of Windows; absent on very old systems).
class Controllers {
public:
    Controllers() {
        lib_ = LoadLibraryW(L"xinput1_4.dll");
        if (!lib_) lib_ = LoadLibraryW(L"xinput9_1_0.dll");
        if (lib_) get_ = reinterpret_cast<GetStateFn>(reinterpret_cast<void*>(GetProcAddress(lib_, "XInputGetState")));
    }
    ~Controllers() {
        if (lib_) FreeLibrary(lib_);
    }
    // Reads connected controllers; looks for new ones every 2 s (querying an
    // empty slot is slow).
    int poll(double now) {
        int connected = 0;
        for (int i = 0; i < keycode::kMaxPads; ++i) {
            buttons_[i] = 0;
            if (!get_ || (!present_[i] && now < nextProbe_)) continue;
            XINPUT_STATE s{};
            present_[i] = get_(static_cast<DWORD>(i), &s) == ERROR_SUCCESS;
            if (!present_[i]) continue;
            ++connected;
            uint32_t b = s.Gamepad.wButtons;
            if (s.Gamepad.bLeftTrigger > 100) b |= 1u << keycode::kPadLeftTrigger;
            if (s.Gamepad.bRightTrigger > 100) b |= 1u << keycode::kPadRightTrigger;
            buttons_[i] = b;
        }
        if (now >= nextProbe_) nextProbe_ = now + 2.0;
        return connected;
    }
    bool isDown(int key) const {
        const int pad = (key - keycode::kPadBase) / 32, button = (key - keycode::kPadBase) % 32;
        return pad >= 0 && pad < keycode::kMaxPads && (buttons_[pad] >> button & 1u) != 0;
    }

private:
    using GetStateFn = DWORD(WINAPI*)(DWORD, XINPUT_STATE*);
    HMODULE lib_ = nullptr;
    GetStateFn get_ = nullptr;
    bool present_[keycode::kMaxPads] = {};
    uint32_t buttons_[keycode::kMaxPads] = {};
    double nextProbe_ = 0.0;
};

// Key state as the hotkey logic sees it: Windows' key state, plus raw input
// events (instant taps, keys without a virtual-key code, wheel tilt), plus
// game controllers.
struct PlatformKeys final : KeyStateReader {
    const KeyEvents* events = nullptr;
    const Controllers* pads = nullptr;
    double now = 0.0;
    bool isDown(int key) const override {
        if (key > 0 && key < 0xFF) {
            if (GetAsyncKeyState(key) & 0x8000) return true;
            // Seen by raw input only (a tap shorter than one poll, or a device
            // whose state Windows does not track). A press whose release was
            // missed times out.
            return events->isDown(key, now, key <= VK_XBUTTON2 ? 2.0 : 1.0);
        }
        if (keycode::isScanKey(key)) return events->isDown(key, now, 1.2); // keyboard auto-repeat refreshes it
        if (keycode::isWheelKey(key)) return events->isDown(key, now, 0.25);
        if (keycode::isPadKey(key)) return pads->isDown(key);
        return false;
    }
};

} // namespace

std::string windowsKeyName(int key) {
    UINT sc = 0;
    bool extended = false;
    if (keycode::isScanKey(key)) {
        sc = static_cast<UINT>((key - keycode::kScanBase) & 0x7F);
        extended = key - keycode::kScanBase >= 0x80;
    } else if (key > 0 && key < 0xFF) {
        sc = MapVirtualKeyW(static_cast<UINT>(key), MAPVK_VK_TO_VSC);
        switch (key) {
        case VK_INSERT: case VK_DELETE: case VK_HOME: case VK_END: case VK_PRIOR: case VK_NEXT:
        case VK_LEFT: case VK_RIGHT: case VK_UP: case VK_DOWN: case VK_NUMLOCK: case VK_DIVIDE:
        case VK_SNAPSHOT: case VK_RCONTROL: case VK_RMENU: case VK_APPS:
            extended = true;
            break;
        default:
            break;
        }
    }
    if (!sc) return {};
    LONG lp = static_cast<LONG>(sc) << 16;
    if (extended) lp |= 1 << 24;
    wchar_t buf[64];
    const int n = GetKeyNameTextW(lp, buf, 64);
    if (n <= 0) return {};
    return wideToUtf8(std::wstring(buf, static_cast<size_t>(n)));
}

HotkeyService::HotkeyService() { setKeyNameProvider(&windowsKeyName); }

HotkeyService::~HotkeyService() { stop(); }

void HotkeyService::start(std::function<void(bool)> onGate, std::function<void(int, bool)> onAction) {
    stop();
    onGate_ = std::move(onGate);
    onAction_ = std::move(onAction);
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

void HotkeyService::setActions(std::vector<Action> actions) {
    std::lock_guard<std::mutex> lock(mutex_);
    actions_ = std::move(actions);
    ++settingsVersion_;
}

std::vector<HotkeyService::Event> HotkeyService::takeEvents() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<Event> e;
    e.swap(events_);
    return e;
}

void HotkeyService::beginCapture(int t) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        captured_.reset();
        capturedTarget_ = -1;
    }
    captureRestart_ = true;
    captureTarget_ = t;
}

void HotkeyService::cancelCapture() { captureTarget_ = -1; }

std::optional<KeyBinding> HotkeyService::takeCapture(int* target) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!captured_) return std::nullopt;
    std::optional<KeyBinding> b = captured_;
    if (target) *target = capturedTarget_;
    captured_.reset();
    capturedTarget_ = -1;
    return b;
}

void HotkeyService::threadMain() {
    SetThreadDescription(GetCurrentThread(), L"Echo hotkeys");
    HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    if (!timer) timer = CreateWaitableTimerW(nullptr, FALSE, nullptr);

    // Raw input arrives as messages to a message-only window on this thread.
    InputState input;
    WNDCLASSW wc{};
    wc.lpfnWndProc = inputWndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"EchoHotkeyInput";
    RegisterClassW(&wc); // fails harmlessly if already registered
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, wc.hInstance, nullptr);
    if (hwnd) SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&input));
    bool rawRegistered = false;
    auto registerRaw = [&](bool on) {
        if (!hwnd || on == rawRegistered) return;
        RAWINPUTDEVICE rid[2] = {{0x01, 0x06, on ? static_cast<DWORD>(RIDEV_INPUTSINK) : static_cast<DWORD>(RIDEV_REMOVE), on ? hwnd : nullptr},
                                 {0x01, 0x02, on ? static_cast<DWORD>(RIDEV_INPUTSINK) : static_cast<DWORD>(RIDEV_REMOVE), on ? hwnd : nullptr}};
        if (RegisterRawInputDevices(rid, 2, sizeof(RAWINPUTDEVICE))) {
            rawRegistered = on;
        } else if (on) {
            ECHO_LOG_WARN("Hotkeys: raw input unavailable (error %lu); using key state only", GetLastError());
            rawRegistered = false;
        }
        if (!on) input.events.releaseAll();
        rawActive_ = rawRegistered;
    };

    Controllers pads;
    PlatformKeys keys;
    keys.events = &input.events;
    keys.pads = &pads;

    TalkGate gate;
    PressDetector mutePress;
    KeyCapture capture;
    HotkeySettings s;
    std::vector<Action> actions;
    std::vector<char> actionHeld;
    uint64_t version = ~0ull;
    bool lastOpen = true;
    bool first = true;
    bool rawFailed = false;
    while (!stop_.load(std::memory_order_acquire)) {
        // Raw input events since the last poll.
        MSG msg;
        while (hwnd && PeekMessageW(&msg, hwnd, 0, 0, PM_REMOVE)) DispatchMessageW(&msg);
        keys.now = monotonicSeconds();
        padsConnected_ = pads.poll(keys.now);

        const int target = captureTarget_.load();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (settingsVersion_ != version) {
                version = settingsVersion_;
                s = settings_;
                actions = actions_;
                gate.configure(s.talkMode, s.releaseDelayMs, !s.talkKey.empty());
                mutePress.reset(bindingHeld(s.muteKey, keys));
                // Keys already held when the bindings change are not new presses.
                actionHeld.assign(actions.size(), 0);
                for (size_t i = 0; i < actions.size(); ++i) actionHeld[i] = bindingHeld(actions[i].key, keys);
            }
        }
        const bool anyKey = (s.talkMode != TalkMode::Off && !s.talkKey.empty()) || !s.muteKey.empty() || !actions.empty();
        if (!rawFailed) {
            registerRaw(anyKey || target >= 0);
            rawFailed = (anyKey || target >= 0) && !rawRegistered;
        }

        const bool held = s.talkMode != TalkMode::Off && bindingHeld(s.talkKey, keys);
        const bool open = gate.update(held, keys.now);
        talkHeld_.store(held, std::memory_order_relaxed);
        gateOpen_.store(open, std::memory_order_relaxed);
        if (open != lastOpen || first) {
            if (onGate_) onGate_(open);
            lastOpen = open;
            first = false;
        }

        // Hotkey actions: press and release events (not while choosing a key).
        for (size_t i = 0; i < actions.size(); ++i) {
            const bool h = bindingHeld(actions[i].key, keys);
            if (h == static_cast<bool>(actionHeld[i])) continue;
            actionHeld[i] = h;
            if (target >= 0) continue;
            if (onAction_) onAction_(actions[i].id, h);
            std::lock_guard<std::mutex> lock(mutex_);
            if (events_.size() < 256) events_.push_back(Event{actions[i].id, h});
        }

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
        input.events.endPoll();

        const double wait = !anyKey && target < 0 ? 0.05 : 0.005;
        if (timer) {
            LARGE_INTEGER due;
            due.QuadPart = -static_cast<LONGLONG>(wait * 1e7);
            SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE);
            WaitForSingleObject(timer, 100);
        } else {
            Sleep(static_cast<DWORD>(wait * 1000));
        }
    }
    registerRaw(false);
    if (hwnd) DestroyWindow(hwnd);
    if (timer) CloseHandle(timer);
}

} // namespace echo::win
