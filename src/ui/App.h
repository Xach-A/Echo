// Main UI application: owns the audio backend and controller and draws all
// views. Runs entirely on the UI thread.
#pragma once

#include <array>
#include <atomic>
#include <deque>
#include <map>
#include <set>
#include <functional>
#include <future>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "audio/AppRouting.h"
#include "audio/Download.h"
#include "audio/HotkeyService.h"
#include "audio/VirtualCables.h"
#include "audio/WasapiBackend.h"
#include "core/Meter.h"
#include "session/MixerController.h"
#include "session/Presets.h"
#include "session/Voices.h"
#include "ui/TextureCache.h"

struct HWND__;
typedef HWND__* HWND;
struct ID3D11Device;

namespace echo::ui {

// Top-level screens (tabs in the header).
enum class Page { Home = 0, Voices, Soundboard, Mixer, Apps, Devices, Recorder, Settings, Count };
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

    // Settings > Reset to defaults (see ResetParts), then saves.
    void resetToDefaults(const ResetParts& parts);

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
    // Uses only this voice preset on an input (nullptr = original voice).
    void applyVoice(StripModel& s, const ChainPreset* p);
    // Adds a voice to the input's mix, or takes it out.
    void toggleVoice(StripModel& s, const std::string& name);
    // Sets the voices of an input. exclusive: everything else goes (also
    // effects added by hand); otherwise only voices are added and taken out.
    // Effects taken out fade out (10 ms, in the audio thread) and are removed
    // afterwards, so changing voices never clicks. Returns false (with a
    // message) when the voices don't fit in one chain.
    bool setVoices(StripModel& s, std::vector<std::string> voices, bool exclusive, bool announce = true);
    // Removes effects that have finished fading out (every frame).
    void applyPendingVoices();
    // Voices screen: tiles add/remove voices (true) or pick one (false).
    bool& voiceMixMode() { return voiceMix_; }

    // ---- soundboard ----
    // Plays a pad as its mode says (down = pressed, false = released).
    void triggerPad(int slot, bool down);
    // Asks for sound files and adds them as pads (decoded in the background).
    void importSounds();
    bool importingSounds() const { return !imports_.empty(); }
    void removePad(uint32_t padId);
    // Whether a pad's sound could be loaded ("" = fine).
    std::string padProblem(const SoundPad& p) const;

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
    void drawSoundboard(); // the Soundboard screen
    void drawPadEditor();
    void drawVoiceHotkeys(); // Settings > Hotkeys: voice hotkeys
    void drawAiPanel(StripModel& strip);
    void drawNoiseEchoPanel(StripModel& strip);
    void drawCleanupStatus(Effect& fx);                       // noise suppressor / echo canceller meters
    bool noiseSuppressionCheckbox(StripModel& strip, const char* label); // on/off (adds it when missing)
    bool echoReferencePicker(StripModel& strip, float width); // playback device an echo canceller listens to
    void drawApps();
    void drawDevices();
    // Installing VB-CABLE on request (setup guide and Devices page).
    void drawVbCableInstall();
    void drawRecorder();
    void drawDiagnostics(); // Settings > Diagnostics
    void drawSettings();
    void drawHotkeySettings();
    void pollHotkeys();
    // Key capture popup shared by every hotkey field: returns true when a
    // binding was chosen for `target` (written to *out).
    bool keyCaptureButton(const char* id, int target, KeyBinding* out, float minWidth = 160.0f);
    // What else a key does ("push to talk", "the sound 'Air horn'"...), or "".
    std::string otherUseOfKey(const KeyBinding& k, const KeyBinding* self);
    void rebuildSoundBank(bool force = false);
    void finishImports();
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
    // Voice changes in progress: effects fading out, removed at `at`;
    // `replan` adds the new voices afterwards (when they did not fit at once).
    struct PendingVoices {
        uint32_t strip = 0;
        double at = 0.0;
        std::vector<std::shared_ptr<Effect>> remove;
        bool replan = false;
        std::vector<std::string> target;
    };
    std::vector<PendingVoices> pendingVoices_;
    std::set<const Effect*> fadingEffects(uint32_t strip) const;
    bool voiceMix_ = false;

    // wizard
    bool wizardOpen_ = false;
    bool wizardCloseRequest_ = false;
    int wizardStep_ = 0;

    // cable test
    std::future<win::CableTestResult> cableTestFuture_;
    // The ContentVec download (RVC voices): "" when it succeeded.
    std::future<std::string> contentVecDownload_;
    win::DownloadProgress contentVecProgress_;
    std::string contentVecError_;
    bool snapshotShowAi_ = false;
    // VB-CABLE: download VB-Audio's pack, then open its installer.
    struct VbCableJob {
        std::string error;
        std::wstring setup; // VB-Audio's installer, once downloaded
        bool opened = false;
    };
    std::future<VbCableJob> vbCableJob_;
    win::DownloadProgress vbCableProgress_;
    VbCableJob vbCableLast_;
    bool snapshotNoCable_ = false; // snapshots: show the screens as without a cable
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
    std::vector<win::HotkeyService::Action> appliedActions_;
    std::map<size_t, std::vector<std::string>> holdAdded_; // per voice hotkey: voices a Hold key added
    int captureTarget_ = -1;                               // keyCaptureButton waiting for this target
    int autoCapture_ = -1;                                 // start capturing for this target when its field is drawn
    // soundboard
    std::array<std::atomic<int>, SoundboardSource::kMaxPads> padModes_{}; // read by the hotkey thread
    std::map<std::string, std::shared_ptr<const SoundClip>> clipCache_;
    std::map<std::string, std::string> clipErrors_; // pad file -> why it could not be loaded
    std::string soundBankSignature_;
    struct ImportResult {
        std::string file;  // file name in the sounds folder
        std::string error; // non-empty: failed
        bool truncated = false;
    };
    struct Import {
        std::string name;
        std::future<ImportResult> result;
    };
    std::vector<std::unique_ptr<Import>> imports_;
    uint32_t editPad_ = 0;   // pad being edited (0 = none)
    bool openPadEditor_ = false;
    // engine settings being edited
    EngineSettings pendingEngine_;
    bool pendingEngineInit_ = false;
    friend struct ViewAccess;
};

} // namespace echo::ui
