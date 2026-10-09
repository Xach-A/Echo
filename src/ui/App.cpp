#include "ui/App.h"

#include <windows.h>
#include <shellapi.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>

#include "Version.h"
#include "core/Log.h"
#include "core/Text.h"
#include "dsp/EffectRegistry.h"
#include "imgui.h"
#include "imgui_internal.h"
#include "session/Paths.h"
#include "session/Serialization.h"
#include "ui/Icons.h"
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
    if (!mc_->session().firstRunCompleted) openWizard();
    setTooltipsEnabled(mc_->session().ui.showTooltips);
    return true;
}

void EchoApp::shutdown() {
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
    applyTheme(mc_ ? mc_->session().ui.theme : 0, dpi_, mc_ ? mc_->session().ui.uiScale : 1.0f);
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
        if (!any) ImGui::TextDisabled("No virtual cable installed (see Devices)");
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
    if (!any) ImGui::TextDisabled(virtualOnly ? "No virtual cable installed. See Devices > Virtual devices." : "No devices");
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
    drawDockspace();
    drawMixer();
    drawInspector();
    drawRouting();
    drawVoiceLab();
    drawApps();
    drawDevices();
    drawRecorder();
    drawDiagnostics();
    drawSettings();
    if (wizardOpen_) drawWizard();
    if (showAbout_) drawAbout();
    if (showDemo_) ImGui::ShowDemoWindow(&showDemo_);
    drawToasts();
    for (auto& fn : deferred_) fn();
    deferred_.clear();
    if (focusRequest_) {
        ImGui::SetWindowFocus(focusRequest_);
        focusRequest_ = nullptr;
    }
    // Keyboard shortcuts.
    ImGuiIO& io = ImGui::GetIO();
    if (!io.WantTextInput) {
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_S)) {
            saveNow();
            toast("Settings saved", 1);
        }
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_R)) {
            if (mc_->recorder().active()) mc_->stopRecording();
            else focusWindow("Recorder");
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
        int wizard;          // wizard step to show, -1 = closed
        const char* focus;   // window to bring to front
        const char* preset;  // chain preset to apply to the first input
    };
    static const Step steps[] = {
        {"wizard-1-welcome", 0, nullptr, nullptr},     {"wizard-2-microphone", 1, nullptr, nullptr},
        {"wizard-3-headphones", 2, nullptr, nullptr},  {"wizard-4-audio", 3, nullptr, nullptr},
        {"wizard-5-virtual-mic", 4, nullptr, nullptr}, {"wizard-6-test", 5, nullptr, nullptr},
        {"wizard-7-routing", 6, nullptr, nullptr},     {"wizard-8-use", 7, nullptr, nullptr},
        {"mixer-effects", -1, "Effects", "Streaming Voice"},
        {"voice-lab", -1, "Voice Lab", "Hard Autotune"},
        {"voice-lab-noise-echo", -1, "Voice Lab", "Deep Voice"},
        {"routing", -1, "Routing", nullptr},
        {"applications", -1, "Applications", nullptr},
        {"devices", -1, "Devices", nullptr},
        {"recorder", -1, "Recorder", nullptr},
        {"diagnostics", -1, "Diagnostics", nullptr},
        {"settings", -1, "Settings", nullptr},
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
        if (st.preset && !mc_->session().strips.empty()) {
            if (const ChainPreset* p = presets_.find(st.preset)) applyChainPreset(ChainTarget{false, mc_->session().strips[0].id}, *p);
            select(ChainTarget{false, mc_->session().strips[0].id});
        }
        if (st.focus) focusWindow(st.focus);
        if (std::strcmp(st.file, "voice-lab-noise-echo") == 0 && !mc_->session().strips.empty()) {
            // Show both cleanup effects: echo cancellation listening to the default speakers.
            StripModel& mic = mc_->session().strips[0];
            bool hasAec = false;
            for (const auto& e : mic.chain) hasAec |= std::string(e->typeId()) == "echo_cancel";
            if (!hasAec) {
                mic.reference = SourceRef{};
                mic.reference.kind = SourceKind::RenderLoopback;
                mc_->addEffect(ChainTarget{false, mic.id}, "echo_cancel", 0);
            }
            voiceTabRequest_ = "Noise & echo";
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
    for (const auto& n : mc_->takeNotices()) toast(n, 1);
    autosave(now);
}

void EchoApp::onResume() {
    ECHO_LOG_INFO("System resumed from sleep; restarting audio");
    mc_->rescanDevices();
    if (mc_->running()) mc_->restart();
    toast("Audio restarted after the system resumed", 0);
}

void EchoApp::drawDockspace() {
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    const float statusH = ImGui::GetFrameHeight() + 6.0f;
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(ImVec2(vp->WorkSize.x, vp->WorkSize.y - statusH));
    ImGui::SetNextWindowViewport(vp->ID);
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse |
                             ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBringToFrontOnFocus |
                             ImGuiWindowFlags_NoNavFocus | ImGuiWindowFlags_MenuBar;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(4, 4));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, pal().bg);
    ImGui::Begin("EchoRoot", nullptr, flags);
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(3);
    drawMenuBar();
    const ImGuiID dockId = ImGui::GetID("EchoDock");
    if (!layoutBuilt_ || resetLayout_) {
        layoutBuilt_ = true;
        if (resetLayout_ || !ImGui::DockBuilderGetNode(dockId) || ImGui::DockBuilderGetNode(dockId)->IsLeafNode()) {
            ImGui::DockBuilderRemoveNode(dockId);
            ImGui::DockBuilderAddNode(dockId, ImGuiDockNodeFlags_DockSpace);
            ImGui::DockBuilderSetNodeSize(dockId, ImGui::GetContentRegionAvail());
            ImGuiID main = dockId, right, bottom;
            ImGui::DockBuilderSplitNode(main, ImGuiDir_Right, 0.30f, &right, &main);
            ImGui::DockBuilderSplitNode(main, ImGuiDir_Down, 0.36f, &bottom, &main);
            ImGui::DockBuilderDockWindow("Mixer", main);
            ImGui::DockBuilderDockWindow("Effects", right);
            ImGui::DockBuilderDockWindow("Voice Lab", bottom);
            ImGui::DockBuilderDockWindow("Routing", bottom);
            ImGui::DockBuilderDockWindow("Applications", bottom);
            ImGui::DockBuilderDockWindow("Devices", bottom);
            ImGui::DockBuilderDockWindow("Recorder", bottom);
            ImGui::DockBuilderDockWindow("Diagnostics", bottom);
            ImGui::DockBuilderDockWindow("Settings", bottom);
            ImGui::DockBuilderFinish(dockId);
        }
        resetLayout_ = false;
    }
    ImGui::DockSpace(dockId, ImVec2(0, 0), ImGuiDockNodeFlags_None);
    ImGui::End();
    drawStatusBar();
}

void EchoApp::drawMenuBar() {
    if (!ImGui::BeginMenuBar()) return;
    ImGui::PushFont(fonts().semibold, 0.0f);
    ImGui::TextColored(toVec4(pal().accent), ICON_WAVE " %s", ECHO_APP_NAME);
    ImGui::PopFont();
    ImGui::Spacing();
    if (ImGui::BeginMenu("File")) {
        if (ImGui::MenuItem(ICON_SAVE "  Save settings", "Ctrl+S")) {
            saveNow();
            toast("Settings saved", 1);
        }
        if (ImGui::MenuItem(ICON_OPEN "  Open settings folder")) ShellExecuteW(nullptr, L"open", paths::appDataDir().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        ImGui::Separator();
        if (ImGui::MenuItem("Export session...")) {
            const std::wstring file = paths::appDataDir() + L"\\session-export.json";
            std::string err;
            if (saveJsonFile(file, sessionToJson(mc_->session()), &err)) {
                toast("Exported to " + wideToUtf8(file), 1);
                ShellExecuteW(nullptr, L"open", paths::appDataDir().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            } else toast("Export failed: " + err, 3);
        }
        Tip("Writes session-export.json into the settings folder (portable, documented JSON).");
        if (ImGui::MenuItem("Import session (session-import.json)")) {
            json j;
            std::string err;
            Session s;
            std::vector<std::string> warn;
            if (loadJsonFile(paths::appDataDir() + L"\\session-import.json", j, &err, false) && sessionFromJson(j, s, &warn)) {
                mc_->replaceSession(std::move(s));
                toast("Session imported", 1);
            } else {
                toast("Place a session file named session-import.json in the settings folder first.", 2);
            }
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Reset to defaults...")) openResetPopup_ = true;
        ImGui::Separator();
        if (ImGui::MenuItem("Exit", "Alt+F4")) quit_ = true;
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Engine")) {
        const bool running = mc_->running();
        if (ImGui::MenuItem(running ? ICON_STOP "  Stop audio engine" : ICON_PLAY "  Start audio engine")) {
            if (running) mc_->stop();
            else mc_->start();
        }
        if (ImGui::MenuItem(ICON_REFRESH "  Restart audio engine")) mc_->restart();
        if (ImGui::MenuItem(ICON_SYNC "  Rescan devices")) mc_->rescanDevices();
        ImGui::Separator();
        bool prot = mc_->session().engine.feedbackProtection;
        if (ImGui::MenuItem("Feedback protection", nullptr, &prot)) {
            mc_->session().engine.feedbackProtection = prot;
            mc_->sendsChanged();
        }
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("View")) {
        for (const char* w : {"Mixer", "Effects", "Voice Lab", "Routing", "Applications", "Devices", "Recorder",
                              "Diagnostics", "Settings"})
            if (ImGui::MenuItem(w)) focusWindow(w);
        ImGui::Separator();
        if (ImGui::MenuItem("Reset window layout")) resetLayout_ = true;
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Help")) {
        if (ImGui::MenuItem(ICON_HELP "  Setup wizard", "F1")) openWizard();
        if (ImGui::MenuItem("Documentation (docs folder)")) {
            std::wstring docs = paths::executableDir() + L"\\docs";
            ShellExecuteW(nullptr, L"open", docs.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        }
        if (ImGui::MenuItem("Open log folder"))
            ShellExecuteW(nullptr, L"open", (paths::localDataDir() + L"\\logs").c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        ImGui::Separator();
        if (ImGui::MenuItem(ICON_INFO "  About Echo")) showAbout_ = true;
        ImGui::EndMenu();
    }
    if (openResetPopup_) {
        ImGui::OpenPopup("##reset");
        openResetPopup_ = false;
    }
    if (ImGui::BeginPopupModal("##reset", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Reset all strips, buses, routing and effects to the defaults?");
        ImGui::TextDisabled("User presets, recordings and models are kept.");
        if (ImGui::Button("Reset", ImVec2(120, 0))) {
            mc_->createDefaultSession();
            mc_->session().firstRunCompleted = true;
            toast("Configuration reset to defaults", 1);
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(120, 0))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    // Right side: record indicator.
    if (mc_->recorder().active()) {
        const auto st = mc_->recorder().status();
        ImGui::SameLine(ImGui::GetWindowWidth() - 240);
        const bool blink = std::fmod(now_, 1.0) < 0.6;
        ImGui::TextColored(toVec4(blink ? pal().bad : withAlpha(pal().bad, 0.4f)), ICON_RECORD " REC %s",
                           formatDuration(st.seconds).c_str());
    }
    ImGui::EndMenuBar();
}

void EchoApp::drawStatusBar() {
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    const float h = ImGui::GetFrameHeight() + 6.0f;
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x, vp->WorkPos.y + vp->WorkSize.y - h));
    ImGui::SetNextWindowSize(ImVec2(vp->WorkSize.x, h));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10, 3));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, pal().bg);
    ImGui::Begin("##status", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoDocking |
                     ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav);
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(2);
    Engine& e = mc_->engine();
    const bool running = e.running();
    const ClockMode mode = static_cast<ClockMode>(e.stats().clockMode.load());
    Led(running, running ? pal().good : pal().bad, ImGui::GetFontSize() * 0.3f);
    ImGui::SameLine();
    ImGui::Text("%s", running ? "Audio running" : "Audio stopped");
    ImGui::SameLine(0, 20);
    ImGui::TextDisabled("%d Hz  |  %d samples (%.1f ms)", e.sampleRate(), e.blockSize(), e.blockSeconds() * 1000.0);
    ImGui::SameLine(0, 20);
    const float cpu = e.stats().cpuLoad.load() * 100.0f;
    ImGui::TextColored(toVec4(cpu > 70 ? pal().bad : cpu > 40 ? pal().warn : pal().textDim), "DSP %.1f%%", cpu);
    Tip("Share of the audio block time spent processing (all strips, buses and effects).");
    ImGui::SameLine(0, 20);
    ImGui::TextDisabled("%s", mode == ClockMode::Device ? mc_->clockDescription().c_str()
                              : mode == ClockMode::Timer ? "Internal timer clock" : "");
    uint64_t under = 0, over = 0;
    if (const GraphSnapshot* g = e.current()) {
        for (int i = 0; i < g->numSources; ++i) under += g->sources[i]->underruns.load(), over += g->sources[i]->overruns.load();
        for (int i = 0; i < g->numSinks; ++i) under += g->sinks[i]->underruns.load(), over += g->sinks[i]->overruns.load();
    }
    ImGui::SameLine(0, 20);
    ImGui::TextColored(toVec4(under + over ? pal().warn : pal().textDim), "Dropouts: %llu", static_cast<unsigned long long>(under + over));
    Tip("Buffer underruns + overruns across all devices since the engine started. See Diagnostics for details.");
    if (!mc_->feedbackWarnings().empty()) {
        ImGui::SameLine(0, 20);
        ImGui::TextColored(toVec4(pal().warn), ICON_WARNING " Feedback loop blocked");
        Tip(mc_->feedbackWarnings().front().text.c_str());
    }
    ImGui::SameLine(ImGui::GetWindowWidth() - 150);
    ImGui::TextDisabled("%s v%s", ECHO_APP_NAME, ECHO_VERSION_STRING);
    ImGui::End();
}

void EchoApp::drawToasts() {
    while (!toasts_.empty() && toasts_.front().expires < now_) toasts_.pop_front();
    if (toasts_.empty()) return;
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    float y = vp->WorkPos.y + vp->WorkSize.y - 60.0f * dpi_;
    int i = 0;
    for (auto it = toasts_.rbegin(); it != toasts_.rend(); ++it, ++i) {
        const ImU32 col = it->level == 1 ? pal().good : it->level == 2 ? pal().warn : it->level == 3 ? pal().bad : pal().accent;
        ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x - 20.0f, y), ImGuiCond_Always, ImVec2(1.0f, 1.0f));
        ImGui::SetNextWindowBgAlpha(0.95f);
        char id[32];
        std::snprintf(id, sizeof(id), "##toast%d", i);
        ImGui::PushStyleColor(ImGuiCol_Border, col);
        ImGui::Begin(id, nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings |
                         ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoDocking);
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 28.0f);
        ImGui::TextColored(toVec4(col), "%s", it->level >= 2 ? ICON_WARNING : ICON_INFO);
        ImGui::SameLine();
        ImGui::TextUnformatted(it->text.c_str());
        ImGui::PopTextWrapPos();
        y -= ImGui::GetWindowHeight() + 8.0f;
        ImGui::End();
        ImGui::PopStyleColor();
    }
}

void EchoApp::drawAbout() {
    ImGui::SetNextWindowSize(ImVec2(560 * dpi_, 0), ImGuiCond_Appearing);
    if (ImGui::Begin("About Echo", &showAbout_, ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoCollapse)) {
        ImGui::PushFont(fonts().semibold, fonts().baseSize * 1.6f);
        ImGui::TextColored(toVec4(pal().accent), ICON_WAVE " %s %s", ECHO_APP_NAME, ECHO_VERSION_STRING);
        ImGui::PopFont();
        ImGui::TextWrapped("Free, open-source virtual audio mixer and real-time voice changer for Windows. No accounts, "
                           "no subscriptions, no telemetry, no network access. All audio is processed locally.");
        ImGui::Separator();
        ImGui::TextWrapped("Licensed under the MIT License. Third-party components: Dear ImGui (MIT), nlohmann/json (MIT), "
                           "doctest (MIT, tests only). Optional: ONNX Runtime (MIT) when installed by the user. "
                           "System fonts and icon fonts are used from Windows and are not redistributed.");
        ImGui::TextWrapped("Virtual microphone/speaker endpoints are provided by a separately installed virtual audio "
                           "cable driver (for example VB-CABLE), which has its own license.");
        ImGui::Separator();
        ImGui::TextDisabled("Settings: %s", wideToUtf8(paths::appDataDir()).c_str());
        ImGui::TextDisabled("Logs: %s", wideToUtf8(paths::logFile()).c_str());
    }
    ImGui::End();
}

} // namespace echo::ui
