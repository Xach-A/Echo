#include "ui/App.h"

#include <windows.h>
#include <shellapi.h>
#include <dwmapi.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>

#include "Version.h"
#include "core/Log.h"
#include "core/Text.h"
#include "dsp/EffectRegistry.h"
#include "dsp/NoiseSuppressorInfo.h"
#include "imgui.h"
#include "imgui_internal.h"
#include "session/Paths.h"
#include "session/Serialization.h"
#include "ui/Icons.h"
#include "ui/Pop.h"
#include "ui/Theme.h"
#include "ui/Widgets.h"

namespace echo::ui {

EchoApp::EchoApp() = default;
EchoApp::~EchoApp() = default;

bool EchoApp::init(HWND hwnd, ID3D11Device* device, bool safeMode, std::string* error) {
    hwnd_ = hwnd;
    safeMode_ = safeMode;
    textures_.setDevice(device);
    backend_ = std::make_unique<win::WasapiBackend>();
    if (!backend_->init(error)) return false;
    mc_ = std::make_unique<MixerController>(*backend_);
    appRouting_ = std::make_unique<win::AppOutputRouting>();
    presets_.load(paths::presetsDir());

    // Load the saved session, or create the default configuration.
    json j;
    std::string err;
    bool loaded = false;
    if (loadJsonFile(paths::sessionFile(), j, &err)) {
        Session s;
        std::vector<std::string> warnings;
        if (sessionFromJson(j, s, &warnings)) {
            mc_->session() = std::move(s);
            mc_->migrateSession();
            mc_->assignCablesToEmptyVirtualBuses(); // a cable installed since the last run
            loaded = true;
            for (const auto& w : warnings) ECHO_LOG_WARN("Session: %s", w.c_str());
            if (!err.empty()) toast("Settings file was damaged; restored the backup copy", 2);
        }
    }
    if (!loaded) {
        mc_->createDefaultSession();
        ECHO_LOG_INFO("No saved session; created defaults");
    }
    if (mc_->session().recorder.folder.empty())
        mc_->session().recorder.folder = wideToUtf8(paths::defaultRecordingsDir());

    // Restore user-verified cable pairs for feedback detection.
    std::vector<win::CablePair> extra;
    for (const auto& c : mc_->session().cables) {
        win::CablePair p;
        p.renderId = c.renderId;
        p.captureId = c.captureId;
        p.renderName = c.renderName;
        p.captureName = c.captureName;
        p.displayName = c.name;
        extra.push_back(p);
    }
    backend_->setExtraCablePairs(extra);

    if (safeMode_) {
        // Safe mode: every effect bypassed, engine not started automatically.
        for (auto& s : mc_->session().strips)
            for (auto& e : s.chain) e->bypass = true;
        for (auto& b : mc_->session().buses)
            for (auto& e : b.chain) e->bypass = true;
        mc_->prepareAll();
        toast("Safe mode: effects bypassed and audio not started. Start it from the Engine menu.", 2);
    } else {
        mc_->start();
    }
    lastSavedJson_ = sessionToJson(mc_->session()).dump();
    // Push to talk: the hotkey thread opens and closes the engine's
    // microphone gate directly, so it responds within a few milliseconds.
    hotkeys_ = std::make_unique<win::HotkeyService>();
    hotkeys_->setSettings(mc_->session().hotkeys);
    appliedHotkeys_ = mc_->session().hotkeys;
    hotkeysApplied_ = true;
    Engine* engine = &mc_->engine();
    hotkeys_->start([engine](bool open) { engine->setMicGate(open); });
    if (!mc_->session().firstRunCompleted) openWizard();
    setTooltipsEnabled(mc_->session().ui.showTooltips);
    return true;
}

void EchoApp::shutdown() {
    if (hotkeys_) hotkeys_->stop();
    saveNow();
    if (mc_) mc_->stop();
    textures_.clear();
}

void EchoApp::saveNow() {
    if (!mc_) return;
    const json j = sessionToJson(mc_->session());
    std::string err;
    if (saveJsonFile(paths::sessionFile(), j, &err)) lastSavedJson_ = j.dump();
    else ECHO_LOG_ERROR("Saving settings failed: %s", err.c_str());
}

void EchoApp::autosave(double now) {
    // Compare the serialised session every 3 s; write only when changed.
    if (now - lastSave_ < 3.0) return;
    lastSave_ = now;
    const json j = sessionToJson(mc_->session());
    std::string s = j.dump();
    if (s == lastSavedJson_) return;
    std::string err;
    if (saveJsonFile(paths::sessionFile(), j, &err)) lastSavedJson_ = std::move(s);
    else toast("Could not save settings: " + err, 3);
}

void EchoApp::setDpiScale(float s) {
    dpi_ = s;
    applyTheme(mc_ ? mc_->session().ui.theme : 0, dpi_, mc_ ? mc_->session().ui.uiScale : 1.0f,
               mc_ ? mc_->session().ui.accent : std::string("blue"));
    if (hwnd_) {
        BOOL dark = themeIsDark() ? TRUE : FALSE;
        DwmSetWindowAttribute(hwnd_, 20 /*DWMWA_USE_IMMERSIVE_DARK_MODE*/, &dark, sizeof(dark));
    }
}

void EchoApp::onSystemThemeChanged() {
    if (mc_ && mc_->session().ui.theme == 3) setDpiScale(dpi_);
}

void EchoApp::toggleMicrophoneMute() {
    if (!mc_) return;
    const bool mute = !mc_->microphonesMuted();
    mc_->setMicrophonesMuted(mute);
    ECHO_LOG_INFO("Microphones %s", mute ? "muted" : "unmuted");
}

void EchoApp::pollHotkeys() {
    if (!hotkeys_) return;
    // Settings edited in the UI, loaded from a file or reset.
    const HotkeySettings& h = mc_->session().hotkeys;
    if (!hotkeysApplied_ || h.talkMode != appliedHotkeys_.talkMode || h.talkKey != appliedHotkeys_.talkKey ||
        h.muteKey != appliedHotkeys_.muteKey || h.releaseDelayMs != appliedHotkeys_.releaseDelayMs) {
        hotkeys_->setSettings(h);
        appliedHotkeys_ = h;
        hotkeysApplied_ = true;
    }
    for (int n = hotkeys_->takeMuteToggles(); n > 0; --n) toggleMicrophoneMute();
}

void EchoApp::toast(const std::string& text, int level) {
    toasts_.push_back(Toast{text, level, now_ + (level >= 2 ? 7.0 : 4.0)});
    if (toasts_.size() > 6) toasts_.pop_front();
    if (level >= 2) ECHO_LOG_WARN("%s", text.c_str());
    else ECHO_LOG_INFO("%s", text.c_str());
}

std::string EchoApp::busLabel(const BusModel& b) const {
    int idx = 0;
    for (const auto& o : mc_->session().buses) {
        if (o.kind == b.kind) ++idx;
        if (o.id == b.id) break;
    }
    return (b.kind == BusKind::Physical ? "A" : "B") + std::to_string(idx);
}

void EchoApp::updateMeters(double now, double dt) {
    for (auto& s : mc_->session().strips) {
        StripMeters& m = stripMeters_[s.id];
        m.in.update(s.rt->inMeter, now, dt);
        m.out.update(s.rt->outMeter, now, dt);
    }
    for (auto& b : mc_->session().buses) busMeters_[b.id].update(b.rt->meter, now, dt);
}

void EchoApp::startCableTest(const std::string& renderId, const std::string& captureId) {
    if (cableTestRunning_) return;
    cableTestRunning_ = true;
    cableTestDone_ = false;
    cableTestRender_ = renderId;
    cableTestFuture_ = std::async(std::launch::async, [renderId, captureId]() {
        win::ComScope com(COINIT_MULTITHREADED);
        return win::testLoopback(renderId, captureId);
    });
}

void EchoApp::applyChainPreset(const ChainTarget& t, const ChainPreset& p) {
    std::vector<std::string> warnings;
    auto chain = chainFromJson(p.chain, &warnings);
    mc_->applyPresetChain(t, std::move(chain)); // keeps the input's noise suppression / echo cancellation
    if (!t.bus)
        if (StripModel* st = mc_->session().strip(t.id)) st->voice = p.name;
    for (const auto& w : warnings) toast(w, 2);
    toast("Loaded preset '" + p.name + "'", 1);
}

std::string EchoApp::effectPicker(const char* popupId) {
    std::string chosen;
    if (ImGui::BeginPopup(popupId)) {
        const char* cats[] = {"Voice", "Dynamics", "Restoration", "EQ & Filters", "Time & Space", "Modulation",
                              "Distortion", "Utility", "AI"};
        for (const char* cat : cats) {
            bool any = false;
            for (const auto& r : EffectRegistry::instance().all()) any |= std::string(r.info->category) == cat;
            if (!any) continue;
            if (ImGui::BeginMenu(cat)) {
                for (const auto& r : EffectRegistry::instance().all()) {
                    if (std::string(r.info->category) != cat) continue;
                    if (ImGui::MenuItem(r.info->name)) chosen = r.info->typeId;
                    Tip(r.info->description);
                }
                ImGui::EndMenu();
            }
        }
        ImGui::EndPopup();
    }
    return chosen;
}

bool EchoApp::sourcePicker(const char* popupId, SourceRef& out, StripGroup* groupOut, std::string* nameOut) {
    bool picked = false;
    if (!ImGui::BeginPopup(popupId)) return false;
    auto devs = backend_->devices();
    auto pick = [&](SourceRef r, StripGroup g, const std::string& name) {
        out = r;
        if (groupOut) *groupOut = g;
        if (nameOut) *nameOut = name;
        picked = true;
        ImGui::CloseCurrentPopup();
    };
    ImGui::TextDisabled("Choose an audio source");
    ImGui::Separator();
    if (ImGui::BeginMenu(ICON_MIC "  Microphone / input device")) {
        SourceRef r;
        r.kind = SourceKind::CaptureDevice;
        if (ImGui::MenuItem("Windows default microphone")) pick(r, StripGroup::Hardware, "Microphone");
        ImGui::Separator();
        for (const auto& d : devs) {
            if (d.render || !d.active || d.isCable) continue;
            if (ImGui::MenuItem(d.name.c_str(), d.isDefault ? "default" : nullptr)) {
                r.deviceId = d.id;
                r.deviceName = d.name;
                pick(r, StripGroup::Hardware, d.name);
            }
            if (d.isAppBound) Tip("Virtual device owned by another application.");
        }
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu(ICON_APPS "  Application audio")) {
        if (!backend_->processLoopbackSupported()) {
            ImGui::TextDisabled("Requires Windows 10 version 2004 or newer");
        } else {
            SourceRef sys;
            sys.kind = SourceKind::SystemExcludingEcho;
            if (ImGui::MenuItem("All system audio (except Echo)")) pick(sys, StripGroup::Application, "Desktop Audio");
            Tip("Captures every application except Echo itself, so it can never feed back.");
            ImGui::Separator();
            auto procs = backend_->audioProcesses();
            if (procs.empty()) ImGui::TextDisabled("No applications are playing audio right now");
            for (const auto& p : procs) {
                std::string label = p.displayName + "  (" + p.exeName + ")";
                if (ImGui::MenuItem(label.c_str())) {
                    SourceRef r;
                    r.kind = SourceKind::ProcessLoopback;
                    r.processId = p.pid;
                    r.processName = p.exeName;
                    pick(r, StripGroup::Application, p.displayName);
                }
            }
        }
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu(ICON_PLUG "  Virtual input (cable)")) {
        bool any = false;
        for (const auto& d : devs) {
            if (d.render || !d.active || !d.isCable) continue;
            any = true;
            if (ImGui::MenuItem(d.name.c_str())) {
                SourceRef r;
                r.kind = SourceKind::CaptureDevice;
                r.deviceId = d.id;
                r.deviceName = d.name;
                pick(r, StripGroup::Virtual, "Virtual Input");
            }
        }
        if (!any) ImGui::TextDisabled("No virtual cable installed (see the Devices screen)");
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu(ICON_SPEAKERS "  Output loopback (what you hear)")) {
        for (const auto& d : devs) {
            if (!d.render || !d.active) continue;
            if (ImGui::MenuItem(d.name.c_str(), d.isDefault ? "default" : nullptr)) {
                SourceRef r;
                r.kind = SourceKind::RenderLoopback;
                r.deviceId = d.id;
                r.deviceName = d.name;
                pick(r, StripGroup::Application, d.name + " loopback");
            }
        }
        ImGui::EndMenu();
    }
    if (ImGui::MenuItem(ICON_TONE "  Test signal generator")) {
        SourceRef r;
        r.kind = SourceKind::ToneGenerator;
        pick(r, StripGroup::Generator, "Test Signal");
    }
    ImGui::EndPopup();
    return picked;
}

bool EchoApp::outputPicker(const char* popupId, OutputRef& out, bool virtualOnly) {
    bool picked = false;
    if (!ImGui::BeginPopup(popupId)) return false;
    ImGui::TextDisabled(virtualOnly ? "Virtual output (cable playback side)" : "Output device");
    ImGui::Separator();
    if (!virtualOnly && ImGui::MenuItem("Windows default output")) {
        out = OutputRef{};
        picked = true;
    }
    bool any = false;
    for (const auto& d : backend_->devices()) {
        if (!d.render || !d.active) continue;
        if (virtualOnly && !d.isCable) continue;
        any = true;
        std::string label = d.name;
        if (d.isCable) label += "  [virtual cable]";
        if (ImGui::MenuItem(label.c_str(), d.isDefault ? "default" : nullptr)) {
            out.deviceId = d.id;
            out.deviceName = d.name;
            picked = true;
        }
        if (d.isAppBound) Tip("Virtual device owned by another app; it only carries audio while that app runs.");
    }
    if (!any) ImGui::TextDisabled(virtualOnly ? "No virtual cable installed. See the Devices screen." : "No devices");
    ImGui::Separator();
    if (ImGui::MenuItem("None (disconnect)")) {
        out = OutputRef{};
        out.deviceName = "";
        out.deviceId = "";
        picked = true;
        // Distinguish "none" for virtual buses: empty ID means no device.
    }
    ImGui::EndPopup();
    return picked;
}

// ---------------------------------------------------------------------------
// Frame
// ---------------------------------------------------------------------------

void EchoApp::frame(double now, double dt) {
    now_ = now;
    mc_->update(now);
    pollHotkeys();
    updateMeters(now, dt);
    if (now - lastAppRefresh_ > 1.0) {
        backend_->sessions().refresh();
        lastAppRefresh_ = now;
    }
    if (appsVisible_) backend_->sessions().updateLevels(); // only while someone can see them
    appsVisible_ = false;
    if (cableTestRunning_ && cableTestFuture_.valid() &&
        cableTestFuture_.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        cableTestResult_ = cableTestFuture_.get();
        cableTestRunning_ = false;
        cableTestDone_ = true;
        toast(cableTestResult_.ok ? "Loopback test passed: " + cableTestResult_.message
                                  : "Loopback test failed: " + cableTestResult_.message,
              cableTestResult_.ok ? 1 : 3);
    }
    {
        static size_t lastWarnings = 0;
        const size_t w = mc_->feedbackWarnings().size();
        if (w > lastWarnings) toast(mc_->feedbackWarnings().back().text, 2);
        lastWarnings = w;
    }

    if (snapshotIndex_ >= 0) snapshotStep();
    handleFocusRequest();
    drawShell();
    if (effectsOpen_) drawInspector();
    if (managePresets_) drawPresetManager();
    if (wizardOpen_) drawWizard();
    if (showDemo_) ImGui::ShowDemoWindow(&showDemo_);
    drawToasts();
    for (auto& fn : deferred_) fn();
    deferred_.clear();
    // Keyboard shortcuts.
    ImGuiIO& io = ImGui::GetIO();
    if (!io.WantTextInput) {
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_S)) {
            saveNow();
            toast("Settings saved", 1);
        }
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_R)) {
            if (mc_->recorder().active()) mc_->stopRecording();
            else navigate(Page::Recorder);
        }
        if (ImGui::IsKeyPressed(ImGuiKey_F1)) openWizard();
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_B) && hasSelection_) {
            if (selected_.bus) {
                if (BusModel* b = mc_->session().bus(selected_.id)) b->rt->fxBypass = !b->rt->fxBypass.load();
            } else if (StripModel* s = mc_->session().strip(selected_.id)) {
                s->rt->fxBypass = !s->rt->fxBypass.load();
            }
        }
    }
    autosave(now);
}

void EchoApp::startSnapshots(const std::wstring& dir) {
    snapshotDir_ = dir;
    snapshotIndex_ = 0;
    snapshotFrame_ = 0;
}

void EchoApp::snapshotStep() {
    struct Step {
        const char* file;
        int wizard;         // wizard step to show, -1 = closed
        Page page;
        SettingsTab tab;
        const char* preset; // voice preset for the microphone
        bool effects;       // open the effects editor
    };
    static const Step steps[] = {
        {"wizard-1-welcome", 0, Page::Home, SettingsTab::Audio, nullptr, false},
        {"wizard-2-microphone", 1, Page::Home, SettingsTab::Audio, nullptr, false},
        {"wizard-3-headphones", 2, Page::Home, SettingsTab::Audio, nullptr, false},
        {"wizard-4-audio", 3, Page::Home, SettingsTab::Audio, nullptr, false},
        {"wizard-5-virtual-mic", 4, Page::Home, SettingsTab::Audio, nullptr, false},
        {"wizard-6-test", 5, Page::Home, SettingsTab::Audio, nullptr, false},
        {"wizard-7-routing", 6, Page::Home, SettingsTab::Audio, nullptr, false},
        {"wizard-8-use", 7, Page::Home, SettingsTab::Audio, nullptr, false},
        {"home", -1, Page::Home, SettingsTab::Audio, "Deep Voice", false},
        {"voices", -1, Page::Voices, SettingsTab::Audio, nullptr, false},
        {"voices-autotune", -1, Page::Voices, SettingsTab::Audio, "Hard Autotune", false},
        {"mixer", -1, Page::Mixer, SettingsTab::Audio, "Deep Voice", false},
        {"mixer-effects", -1, Page::Mixer, SettingsTab::Audio, "Streaming Voice", true},
        {"apps", -1, Page::Apps, SettingsTab::Audio, nullptr, false},
        {"devices", -1, Page::Devices, SettingsTab::Audio, nullptr, false},
        {"recorder", -1, Page::Recorder, SettingsTab::Audio, nullptr, false},
        {"settings-audio", -1, Page::Settings, SettingsTab::Audio, nullptr, false},
        {"settings-hotkeys", -1, Page::Settings, SettingsTab::Hotkeys, nullptr, false},
        {"settings-appearance", -1, Page::Settings, SettingsTab::Appearance, nullptr, false},
        {"settings-startup", -1, Page::Settings, SettingsTab::Startup, nullptr, false},
        {"settings-diagnostics", -1, Page::Settings, SettingsTab::Diagnostics, nullptr, false},
        {"home-dark", -1, Page::Home, SettingsTab::Audio, "Deep Voice", false},
    };
    constexpr int kCount = static_cast<int>(std::size(steps));
    if (snapshotIndex_ >= kCount) {
        quit_ = true;
        return;
    }
    const Step& st = steps[snapshotIndex_];
    if (snapshotFrame_ == 0) {
        if (st.wizard >= 0) {
            wizardOpen_ = true;
            wizardStep_ = st.wizard;
        } else if (wizardOpen_) {
            wizardCloseRequest_ = true;
        }
        page_ = st.page;
        settingsTab_ = st.tab;
        StripModel* mic = primaryMic();
        if (st.preset && mic) {
            if (const ChainPreset* p = presets_.find(st.preset)) applyVoice(*mic, p);
        }
        effectsOpen_ = st.effects && mic;
        if (mic) select(ChainTarget{false, mic->id});
        if (st.tab == SettingsTab::Hotkeys || std::strcmp(st.file, "home") == 0) {
            // Example bindings (snapshot mode runs on a throwaway profile).
            HotkeySettings& h = mc_->session().hotkeys;
            h.talkMode = TalkMode::PushToTalk;
            h.talkKey = {vk::XButton2, 0};
            h.muteKey = {'M', KeyModCtrl | KeyModShift};
        }
        if (std::strcmp(st.file, "home-dark") == 0) {
            mc_->session().ui.theme = 1;
            setDpiScale(dpi_);
        }
    }
    if (++snapshotFrame_ == 45) {
        captureRequest_ = snapshotDir_ + L"\\" + std::wstring(st.file, st.file + std::strlen(st.file)) + L".png";
        snapshotFrame_ = 0;
        ++snapshotIndex_;
    }
}

void EchoApp::idle(double now) {
    now_ = now;
    mc_->update(now);
    pollHotkeys();
    for (const auto& n : mc_->takeNotices()) toast(n, 1);
    autosave(now);
}

void EchoApp::onResume() {
    ECHO_LOG_INFO("System resumed from sleep; restarting audio");
    mc_->rescanDevices();
    if (mc_->running()) mc_->restart();
    toast("Audio restarted after the system resumed", 0);
}

void EchoApp::drawToasts() {
    while (!toasts_.empty() && toasts_.front().expires < now_) toasts_.pop_front();
    if (toasts_.empty()) return;
    const Palette& p = pal();
    const float sc = uiScale();
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    float y = vp->WorkPos.y + vp->WorkSize.y - 24.0f * sc;
    int i = 0;
    for (auto it = toasts_.rbegin(); it != toasts_.rend(); ++it, ++i) {
        const ImU32 col = it->level == 1 ? p.good : it->level == 2 ? p.warn : it->level == 3 ? p.bad : p.accent;
        ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x - 24.0f * sc, y), ImGuiCond_Always, ImVec2(1.0f, 1.0f));
        char id[32];
        std::snprintf(id, sizeof(id), "##toast%d", i);
        ImGui::PushStyleColor(ImGuiCol_WindowBg, toVec4(p.surface));
        ImGui::PushStyleColor(ImGuiCol_Border, toVec4(p.outline));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 14.0f * sc);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.5f * sc);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(16.0f * sc, 12.0f * sc));
        ImGui::Begin(id, nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings |
                         ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoDocking);
        const ImVec2 c = ImGui::GetCursorScreenPos();
        ImGui::GetWindowDrawList()->AddCircleFilled(ImVec2(c.x + 5.0f * sc, c.y + ImGui::GetFontSize() * 0.55f), 5.0f * sc, col, 12);
        ImGui::SetCursorScreenPos(ImVec2(c.x + 18.0f * sc, c.y));
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + ImGui::GetFontSize() * 26.0f);
        ImGui::TextUnformatted(it->text.c_str());
        ImGui::PopTextWrapPos();
        y -= ImGui::GetWindowHeight() + 10.0f * sc;
        ImGui::End();
        ImGui::PopStyleVar(3);
        ImGui::PopStyleColor(2);
    }
}

StripModel* EchoApp::primaryMic() {
    for (auto& st : mc_->session().strips)
        if (st.group == StripGroup::Hardware && st.source.kind == SourceKind::CaptureDevice) return &st;
    for (auto& st : mc_->session().strips)
        if (st.group == StripGroup::Hardware) return &st;
    return nullptr;
}

BusModel* EchoApp::firstBus(BusKind kind) {
    for (auto& b : mc_->session().buses)
        if (b.kind == kind) return &b;
    return nullptr;
}

double EchoApp::presetDelayMs(const ChainPreset& p) {
    const int sr = mc_->engine().sampleRate();
    if (sr != presetDelayRate_) {
        presetDelayCache_.clear();
        presetDelayRate_ = sr;
    }
    if (auto it = presetDelayCache_.find(p.name); it != presetDelayCache_.end()) return it->second;
    // The input's own noise suppression / echo cancellation stays when a voice
    // is applied, so only the voice's other effects add delay.
    double samples = 0.0;
    for (auto& e : chainFromJson(p.chain, nullptr)) {
        const std::string type = e->typeId();
        if (type == noiseSuppressorTypeId() || type == echoCancellerTypeId()) continue;
        e->prepare(sr, mc_->engine().blockSize());
        samples += e->latencySamples();
    }
    const double ms = samples * 1000.0 / sr;
    presetDelayCache_[p.name] = ms;
    return ms;
}

void EchoApp::applyVoice(StripModel& s, const ChainPreset* p) {
    if (p) {
        applyChainPreset(ChainTarget{false, s.id}, *p);
        return;
    }
    mc_->applyPresetChain(ChainTarget{false, s.id}, {});
    s.voice.clear();
    toast("Original voice", 1);
}

void EchoApp::handleFocusRequest() {
    if (!focusRequest_) return;
    const std::string n = focusRequest_;
    focusRequest_ = nullptr;
    if (n == "Effects") {
        effectsOpen_ = hasSelection_;
    } else if (n == "Voice Lab") {
        page_ = Page::Voices;
    } else if (n == "Mixer") {
        page_ = Page::Mixer;
    } else if (n == "Routing") {
        page_ = Page::Mixer;
        routingOpen_ = true;
    } else if (n == "Applications") {
        page_ = Page::Apps;
    } else if (n == "Devices") {
        page_ = Page::Devices;
    } else if (n == "Recorder") {
        page_ = Page::Recorder;
    } else if (n == "Diagnostics") {
        openSettings(SettingsTab::Diagnostics);
    } else if (n == "Settings") {
        page_ = Page::Settings;
    }
}

void EchoApp::drawAbout() {
    pop::BeginCard("##about");
    ImGui::PushFont(fonts().semibold, 22.0f);
    ImGui::TextUnformatted(ECHO_APP_NAME " " ECHO_VERSION_STRING);
    ImGui::PopFont();
    pop::Caption("Free, open-source virtual audio mixer and voice changer for Windows. No accounts, no subscriptions, "
                 "no telemetry, no network access. All audio is processed on this computer.");
    pop::Divider();
    pop::Caption("Licensed under the MIT License. Third-party components: Dear ImGui (MIT), nlohmann/json (MIT), RNNoise "
                 "(BSD-3-Clause), doctest (MIT, tests only). Optional: ONNX Runtime (MIT) when you install it. Windows "
                 "system fonts and icon fonts are used, not redistributed.");
    pop::Caption("The virtual microphone uses a virtual audio cable driver you install separately (for example "
                 "VB-CABLE by VB-Audio), under its own license.");
    pop::Divider();
    pop::Hint(("Settings: " + wideToUtf8(paths::appDataDir())).c_str());
    pop::Hint(("Logs: " + wideToUtf8(paths::logFile())).c_str());
    ImGui::Dummy(ImVec2(0, 4));
    if (pop::Button("Open documentation")) {
        const std::wstring docs = paths::executableDir() + L"\\docs";
        ShellExecuteW(nullptr, L"open", docs.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }
    pop::EndCard();
}

} // namespace echo::ui
