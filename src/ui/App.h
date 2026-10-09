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

struct Toast {
    std::string text;
    int level = 0; // 0 info, 1 success, 2 warning, 3 error
    double expires = 0.0;
};

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
    void focusWindow(const char* name) { focusRequest_ = name; }

    // Structural edits requested while iterating strips/buses are deferred
    // to the end of the frame.
    void defer(std::function<void()> fn) { deferred_.push_back(std::move(fn)); }

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

private:
    // views (each in its own source file)
    void drawMenuBar();
    void drawStatusBar();
    void drawDockspace();
    void drawMixer();
    void drawStrip(StripModel& s, float width, float height);
    void drawBusStrip(BusModel& b, float width, float height);
    void drawInspector();
    void drawEffectCard(const ChainTarget& t, std::vector<std::shared_ptr<Effect>>& chain, int index);
    void drawPresetManager();
    void drawRouting();
    void drawFlowDiagram(ImVec2 size);
    void drawVoiceLab();
    void drawAiPanel(StripModel& strip);
    void drawNoiseEchoPanel(StripModel& strip);
    void drawCleanupStatus(Effect& fx);                       // noise suppressor / echo canceller meters
    bool noiseSuppressionCheckbox(StripModel& strip, const char* label); // on/off (adds it when missing)
    bool echoReferencePicker(StripModel& strip, float width); // playback device an echo canceller listens to
    void drawApps();
    void drawDevices();
    void drawRecorder();
    void drawDiagnostics();
    void drawSettings();
    void drawWizard();
    void drawAbout();
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
    std::string voiceTabRequest_; // Voice Lab tab to bring to front once
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
    bool layoutBuilt_ = false;
    bool resetLayout_ = false;
    bool showAbout_ = false;
    bool showDemo_ = false;

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
    // diagnostics
    std::vector<float> cpuHistory_;
    int diagStrip_ = 0, diagBus_ = 0;
    // engine settings being edited
    EngineSettings pendingEngine_;
    bool pendingEngineInit_ = false;
    friend struct ViewAccess;
};

} // namespace echo::ui
