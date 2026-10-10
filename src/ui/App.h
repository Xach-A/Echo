// Main UI application: owns the audio backend and controller and draws all
// views. Runs entirely on the UI thread.
#pragma once

#include <atomic>
#include <deque>
#include <functional>
#include <future>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "audio/AppRouting.h"
#include "audio/HotkeyService.h"
#include "audio/VirtualCables.h"
#include "audio/WasapiBackend.h"
#include "core/Meter.h"
#include "session/MixerController.h"
#include "session/Presets.h"
#include "ui/TextureCache.h"

struct HWND__;
typedef HWND__* HWND;
struct ID3D11Device;

namespace echo::ui {

// Top-level screens (tabs in the header).
enum class Page { Home = 0, Voices, Mixer, Apps, Devices, Recorder, Settings, Count };
// Sections of the Settings screen.
enum class SettingsTab { Audio = 0, Hotkeys, Appearance, Startup, Privacy, Diagnostics, About, Count };

struct Toast {
    std::string text;
    int level = 0; // 0 info, 1 success, 2 warning, 3 error
    double expires = 0.0;
};

// "Deep Voice", "your own voice" or "your custom effects" for an input.
std::string voiceLabel(const StripModel& s);

class EchoApp {
public:
    EchoApp();
    ~EchoApp();

    bool init(HWND hwnd, ID3D11Device* device, bool safeMode, std::string* error);
    void frame(double now, double dt);
    // Called instead of frame() while the window is minimized/occluded.
    void idle(double now);
    // After the system resumes from sleep: restart audio cleanly.
    void onResume();

    // UI smoke test: renders each page and asks the host to save PNGs.
    void startSnapshots(const std::wstring& dir);
    // Returns a path when the host should capture the frame just rendered.
    std::wstring takeCaptureRequest() {
        std::wstring p;
        p.swap(captureRequest_);
        return p;
    }
    void shutdown();
    bool quitRequested() const { return quit_; }
    void requestQuit() { quit_ = true; }
    void setDpiScale(float s);
    float dpiScale() const { return dpi_; }
    UiSettings& uiSettings() { return mc_->session().ui; }
    void saveNow();

    // ---- shared state used by the views ----
    win::WasapiBackend& backend() { return *backend_; }
    MixerController& mc() { return *mc_; }
    PresetLibrary& presets() { return presets_; }
    TextureCache& textures() { return textures_; }
    HWND hwnd() const { return hwnd_; }
    double now() const { return now_; }

    struct StripMeters {
        MeterBallistics in, out;
    };
    StripMeters& stripMeters(uint32_t id) { return stripMeters_[id]; }
    MeterBallistics& busMeters(uint32_t id) { return busMeters_[id]; }

    void toast(const std::string& text, int level = 0);
    void select(ChainTarget t) {
        selected_ = t;
        hasSelection_ = true;
    }
    bool hasSelection() const { return hasSelection_; }
    ChainTarget selection() const { return selected_; }
    // Shows a screen or panel by its old window name ("Effects", "Recorder",
    // "Voice Lab"...): kept so existing callers keep working.
    void focusWindow(const char* name) { focusRequest_ = name; }
    void navigate(Page p) { page_ = p; }
    void openSettings(SettingsTab t) {
        page_ = Page::Settings;
        settingsTab_ = t;
    }
    // Opens the effects editor for a strip or bus.
    void openEffects(ChainTarget t) {
        select(t);
        effectsOpen_ = true;
    }

    // Structural edits requested while iterating strips/buses are deferred
    // to the end of the frame.
    void defer(std::function<void()> fn) { deferred_.push_back(std::move(fn)); }

    // Microphone mute (all microphone inputs), used by the mute hotkey and
    // the tray menu.
    bool microphonesMuted() { return mc_ && mc_->microphonesMuted(); }
    void toggleMicrophoneMute();
    // Closing the window keeps Echo running in the notification area.
    bool closeToTray() const { return mc_ && mc_->session().ui.closeToTray; }
    // Windows switched between light and dark app mode.
    void onSystemThemeChanged();

    // Runs the setup wizard (first run or from the Help menu).
    void openWizard() { wizardOpen_ = true; wizardStep_ = 0; }

    // Async virtual-cable loopback test.
    void startCableTest(const std::string& renderId, const std::string& captureId);
    bool cableTestRunning() const { return cableTestRunning_; }
    const win::CableTestResult* cableTestResult() const { return cableTestDone_ ? &cableTestResult_ : nullptr; }
    std::string cableTestRenderId() const { return cableTestRender_; }

    // Applies a factory/user chain preset to a strip or bus.
    void applyChainPreset(const ChainTarget& t, const ChainPreset& p);
    // Effect type picker popup; returns chosen type id or "".
    std::string effectPicker(const char* popupId);

    // Source/output pickers (popups). Return true when a choice was made.
    bool sourcePicker(const char* popupId, SourceRef& out, StripGroup* groupOut, std::string* nameOut);
    bool outputPicker(const char* popupId, OutputRef& out, bool virtualOnly);

    std::string busLabel(const BusModel& b) const; // "A1", "B1" ...

    // The main microphone input (first hardware input) and the first bus of a kind.
    StripModel* primaryMic();
    BusModel* firstBus(BusKind kind);
    // Delay a voice preset adds on top of the input's noise/echo cleanup (ms,
    // at the engine rate), measured from the effects' own latency.
    double presetDelayMs(const ChainPreset& p);
    // Applies a voice preset (nullptr = original voice) to an input.
    void applyVoice(StripModel& s, const ChainPreset* p);

private:
    // shell
    void drawShell();
    void drawHeader();
    void handleFocusRequest();
    // screens (each in its own source file)
    void drawHome();
    void drawMixer();
    void drawStrip(StripModel& s, float width);
    void drawBusRow(BusModel& b, float width);
    void drawInspector(); // floating effects editor
    void drawEffectCard(const ChainTarget& t, std::vector<std::shared_ptr<Effect>>& chain, int index);
    void drawPresetManager();
    void drawRouting(); // routing table (inside the Mixer)
    void drawFlowDiagram(ImVec2 size);
    void drawVoiceLab(); // the Voices screen
    void drawAiPanel(StripModel& strip);
    void drawNoiseEchoPanel(StripModel& strip);
    void drawCleanupStatus(Effect& fx);                       // noise suppressor / echo canceller meters
    bool noiseSuppressionCheckbox(StripModel& strip, const char* label); // on/off (adds it when missing)
    bool echoReferencePicker(StripModel& strip, float width); // playback device an echo canceller listens to
    void drawApps();
    void drawDevices();
    void drawRecorder();
    void drawDiagnostics(); // Settings > Diagnostics
    void drawSettings();
    void drawHotkeySettings();
    void pollHotkeys();
    void drawWizard();
    void drawAbout(); // Settings > About
    void drawToasts();
    void updateMeters(double now, double dt);
    void autosave(double now);
    std::string diagnosticReport();

    HWND hwnd_ = nullptr;
    std::unique_ptr<win::WasapiBackend> backend_;
    std::unique_ptr<MixerController> mc_;
    std::unique_ptr<win::AppOutputRouting> appRouting_;
    std::unordered_map<uint32_t, std::string> appOutputCache_;
    double lastAppOutputRefresh_ = -10.0;
    PresetLibrary presets_;
    TextureCache textures_;
    std::unordered_map<uint32_t, StripMeters> stripMeters_;
    std::unordered_map<uint32_t, MeterBallistics> busMeters_;
    std::deque<Toast> toasts_;
    std::vector<std::function<void()>> deferred_;
    bool openResetPopup_ = false;
    // snapshot mode
    void snapshotStep();
    std::wstring snapshotDir_, captureRequest_;
    int snapshotIndex_ = -1, snapshotFrame_ = 0;
    bool managePresets_ = false;
    ChainTarget selected_;
    bool hasSelection_ = false;
    const char* focusRequest_ = nullptr;

    double now_ = 0.0, lastSave_ = 0.0, lastAppRefresh_ = 0.0;
    bool appsVisible_ = false; // Applications panel drawn last frame (per-app meters needed)
    std::string lastSavedJson_;
    float dpi_ = 1.0f;
    bool quit_ = false;
    bool safeMode_ = false;
    bool showDemo_ = false;
    // navigation
    Page page_ = Page::Home;
    SettingsTab settingsTab_ = SettingsTab::Audio;
    bool effectsOpen_ = false;
    bool routingOpen_ = false;
    int voiceCategory_ = 0;
    uint32_t voiceTarget_ = 0;
    std::unordered_map<std::string, double> presetDelayCache_;
    int presetDelayRate_ = 0;

    // wizard
    bool wizardOpen_ = false;
    bool wizardCloseRequest_ = false;
    int wizardStep_ = 0;

    // cable test
    std::future<win::CableTestResult> cableTestFuture_;
    bool cableTestRunning_ = false;
    bool cableTestDone_ = false;
    win::CableTestResult cableTestResult_;
    std::string cableTestRender_;

    // recorder view state
    std::unordered_map<uint64_t, bool> recordSelection_;
    std::vector<RecordingFile> recent_;
    double recentRefreshed_ = -10.0;
    bool recentWasActive_ = false;
    // diagnostics
    std::vector<float> cpuHistory_;
    int diagStrip_ = 0, diagBus_ = 0;
    // hotkeys
    std::unique_ptr<win::HotkeyService> hotkeys_;
    HotkeySettings appliedHotkeys_;
    bool hotkeysApplied_ = false;
    bool openKeyCapture_ = false;
    // engine settings being edited
    EngineSettings pendingEngine_;
    bool pendingEngineInit_ = false;
    friend struct ViewAccess;
};

} // namespace echo::ui
