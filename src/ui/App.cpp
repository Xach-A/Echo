#include "ui/App.h"

#include <windows.h>
#include <commdlg.h>
#include <shellapi.h>
#include <dwmapi.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <filesystem>

#include "Version.h"
#include "audio/AudioDecoder.h"
#include "core/Log.h"
#include "core/WavFile.h"
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

namespace {
// Hotkey action ids (see pollHotkeys).
constexpr int kVoiceActionBase = 1000; // + voice hotkey index
constexpr int kStopAllAction = 1999;   // soundboard: stop every sound
constexpr int kPadActionBase = 2000;   // + soundboard pad slot
constexpr int kMaxPads = SoundboardSource::kMaxPads;
} // namespace

EchoApp::EchoApp() = default;
EchoApp::~EchoApp() {
    contentVecProgress_.cancel = true; // a download in progress stops (the future waits for it)
    ortProgress_.cancel = true;
    updateProgress_.cancel = true;
    vbCableProgress_.cancel = true;
}

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
    SoundboardSource* board = &mc_->soundboard();
    auto* modes = &padModes_;
    hotkeys_->start([engine](bool open) { engine->setMicGate(open); },
                    [board, modes](int id, bool down) {
                        // Soundboard keys play right here on the hotkey thread,
                        // without waiting for the next UI frame.
                        if (id == kStopAllAction) {
                            if (down) board->stopAll();
                            return;
                        }
                        const int slot = id - kPadActionBase;
                        if (slot < 0 || slot >= kMaxPads) return;
                        switch (static_cast<PadMode>((*modes)[static_cast<size_t>(slot)].load(std::memory_order_relaxed))) {
                        case PadMode::Toggle:
                            if (down) board->toggle(slot);
                            break;
                        case PadMode::Hold:
                            if (down) board->play(slot);
                            else board->stop(slot);
                            break;
                        default:
                            if (down) board->play(slot);
                            break;
                        }
                    });
    rebuildSoundBank(true);
    if (!mc_->session().firstRunCompleted) openWizard();
    setTooltipsEnabled(mc_->session().ui.showTooltips);
    return true;
}

void EchoApp::shutdown() {
    if (hotkeys_) hotkeys_->stop();
    // Voice changes still fading out: finish them so the session is saved
    // without the old effects (twice: a change may add voices afterwards).
    for (int pass = 0; pass < 2; ++pass) {
        for (auto& pv : pendingVoices_) pv.at = 0.0;
        applyPendingVoices();
    }
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
    ThemeChoice c;
    if (mc_) {
        const UiSettings& ui = mc_->session().ui;
        c.theme = ui.theme;
        c.accent = ui.accent;
        c.customAccent = ui.customAccent;
        c.custom = {ui.customBackground, ui.customSurface, ui.customText};
    }
    applyTheme(c, dpi_, mc_ ? mc_->session().ui.uiScale : 1.0f);
    if (hwnd_) {
        BOOL dark = themeIsDark() ? TRUE : FALSE;
        DwmSetWindowAttribute(hwnd_, 20 /*DWMWA_USE_IMMERSIVE_DARK_MODE*/, &dark, sizeof(dark));
    }
}

void EchoApp::onSystemThemeChanged() {
    if (mc_ && mc_->session().ui.theme == kThemeSystem) setDpiScale(dpi_);
}

void EchoApp::resetToDefaults(const ResetParts& parts) {
    if (!parts.any()) return;
    if (hotkeys_) hotkeys_->cancelCapture();
    // Voice changes in flight refer to effects that are about to go.
    pendingVoices_.clear();
    holdAdded_.clear();
    mc_->resetSettings(parts);
    if (parts.mixer) {
        voiceTarget_ = 0;
        hasSelection_ = false;
        effectsOpen_ = false;
        voiceMix_ = false;
        stripMeters_.clear();
        busMeters_.clear();
    }
    if (parts.audio) {
        if (mc_->session().recorder.folder.empty())
            mc_->session().recorder.folder = wideToUtf8(paths::defaultRecordingsDir());
        pendingEngineInit_ = false;
    }
    if (parts.look) {
        setDpiScale(dpi_);
        setTooltipsEnabled(mc_->session().ui.showTooltips);
    }
    if (parts.soundboard) soundBankSignature_.clear(); // rebuilt on the next frame
    saveNow();
    ECHO_LOG_INFO("Settings reset to defaults (mixer %d, hotkeys %d, soundboard %d, audio %d, look %d)", parts.mixer,
                  parts.hotkeys, parts.soundboard, parts.audio, parts.look);
    toast("Settings reset to the defaults", 1);
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
    // Voice and soundboard keys.
    std::vector<win::HotkeyService::Action> actions;
    for (size_t i = 0; i < h.voiceKeys.size(); ++i)
        if (!h.voiceKeys[i].key.empty()) actions.push_back({kVoiceActionBase + static_cast<int>(i), h.voiceKeys[i].key});
    const SoundboardSettings& sb = mc_->session().soundboard;
    if (!sb.stopAllKey.empty()) actions.push_back({kStopAllAction, sb.stopAllKey});
    for (size_t i = 0; i < sb.pads.size() && i < static_cast<size_t>(kMaxPads); ++i)
        if (!sb.pads[i].key.empty()) actions.push_back({kPadActionBase + static_cast<int>(i), sb.pads[i].key});
    const bool same = actions.size() == appliedActions_.size() &&
                      std::equal(actions.begin(), actions.end(), appliedActions_.begin(),
                                 [](const auto& a, const auto& b) { return a.id == b.id && a.key == b.key; });
    if (!same) {
        hotkeys_->setActions(actions);
        appliedActions_ = std::move(actions);
        holdAdded_.clear();
    }
    for (int n = hotkeys_->takeMuteToggles(); n > 0; --n) toggleMicrophoneMute();
    for (const auto& ev : hotkeys_->takeEvents()) {
        // Soundboard keys were already played on the hotkey thread.
        const int i = ev.id - kVoiceActionBase;
        if (i < 0 || i >= static_cast<int>(h.voiceKeys.size())) continue;
        StripModel* mic = primaryMic();
        if (!mic) continue;
        const VoiceHotkey vh = h.voiceKeys[static_cast<size_t>(i)];
        std::vector<std::string> target = voiceHotkeyResult(vh, ev.down, mic->voices, holdAdded_[static_cast<size_t>(i)]);
        if (target != mic->voices) setVoices(*mic, std::move(target), vh.mode == VoiceKeyMode::Switch);
    }
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

std::set<const Effect*> EchoApp::fadingEffects(uint32_t strip) const {
    std::set<const Effect*> out;
    for (const auto& pv : pendingVoices_)
        if (pv.strip == strip)
            for (const auto& e : pv.remove) out.insert(e.get());
    return out;
}

bool EchoApp::setVoices(StripModel& s, std::vector<std::string> voices, bool exclusive, bool announce) {
    const ChainTarget t{false, s.id};
    const PresetFinder find = [this](const std::string& n) { return presets_.find(n); };
    // A newer choice replaces one still waiting for old effects to fade out.
    for (auto& pv : pendingVoices_)
        if (pv.strip == s.id) pv.replan = false;
    VoicePlan plan = planVoices(s.chain, voices, exclusive, find, fadingEffects(s.id));
    if (!plan.error.empty()) {
        toast(plan.error, 2);
        return false;
    }
    // Effects brought back while fading out are no longer removed.
    for (auto& pv : pendingVoices_)
        for (const auto& e : plan.revived) pv.remove.erase(std::remove(pv.remove.begin(), pv.remove.end(), e), pv.remove.end());
    auto without = [](const std::vector<std::shared_ptr<Effect>>& chain, const std::vector<std::shared_ptr<Effect>>& gone) {
        std::vector<std::shared_ptr<Effect>> out;
        for (const auto& e : chain)
            if (std::find(gone.begin(), gone.end(), e) == gone.end()) out.push_back(e);
        return out;
    };
    if (!mc_->running()) {
        // Nothing is playing: no fades needed.
        for (const auto& e : plan.revived) e->bypass = false;
        std::vector<std::shared_ptr<Effect>> chain = without(plan.chain, plan.fading);
        if (plan.deferred) {
            VoicePlan again = planVoices(chain, plan.voices, exclusive, find, {});
            if (!again.error.empty() || again.deferred) {
                toast(again.error.empty() ? "That's too many effects for one input." : again.error, 2);
                return false;
            }
            chain = without(again.chain, again.fading);
        }
        mc_->setChain(t, std::move(chain));
    } else {
        applyVoicePlan(plan, s.chain);
        mc_->setChain(t, plan.chain);
        if (!plan.fading.empty() || plan.deferred) {
            const std::set<const Effect*> already = fadingEffects(s.id);
            std::vector<std::shared_ptr<Effect>> fresh;
            for (const auto& e : plan.fading)
                if (!already.count(e.get())) fresh.push_back(e);
            pendingVoices_.push_back(PendingVoices{s.id, now_ + 0.035, std::move(fresh), plan.deferred, plan.voices});
        }
    }
    // A strip pointer may have moved if the session's strips changed; find it again.
    if (StripModel* st = mc_->session().strip(t.id)) st->voices = plan.voices;
    if (announce) toast(plan.voices.empty() ? std::string("Original voice") : "Voice: " + voicesLabel(plan.voices), 1);
    return true;
}

void EchoApp::applyPendingVoices() {
    if (pendingVoices_.empty()) return;
    std::vector<PendingVoices> due;
    for (auto it = pendingVoices_.begin(); it != pendingVoices_.end();) {
        if (now_ >= it->at) {
            due.push_back(std::move(*it));
            it = pendingVoices_.erase(it);
        } else {
            ++it;
        }
    }
    for (auto& pv : due) {
        StripModel* s = mc_->session().strip(pv.strip);
        if (!s) continue;
        std::vector<std::shared_ptr<Effect>> chain;
        for (const auto& e : s->chain)
            if (std::find(pv.remove.begin(), pv.remove.end(), e) == pv.remove.end()) chain.push_back(e);
        if (chain.size() != s->chain.size()) mc_->setChain(ChainTarget{false, s->id}, std::move(chain));
        if (pv.replan)
            if (StripModel* st = mc_->session().strip(pv.strip)) setVoices(*st, pv.target, true, false);
    }
}

void EchoApp::toggleVoice(StripModel& s, const std::string& name) {
    std::vector<std::string> target = s.voices;
    auto it = std::find(target.begin(), target.end(), name);
    if (it != target.end()) target.erase(it);
    else target.push_back(name);
    setVoices(s, std::move(target), false);
}

void EchoApp::applyChainPreset(const ChainTarget& t, const ChainPreset& p) {
    if (!t.bus && presets_.find(p.name)) {
        // On an input a preset is a voice: it replaces the current ones.
        if (StripModel* st = mc_->session().strip(t.id)) setVoices(*st, {p.name}, true);
        return;
    }
    std::vector<std::string> warnings;
    auto chain = chainFromJson(p.chain, &warnings);
    mc_->applyPresetChain(t, std::move(chain)); // keeps an input's noise suppression / echo cancellation
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
    applyPendingVoices();
    finishImports();
    rebuildSoundBank();
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
        {"wizard-5-virtual-mic-install", 4, Page::Home, SettingsTab::Audio, nullptr, false},
        {"wizard-6-test", 5, Page::Home, SettingsTab::Audio, nullptr, false},
        {"wizard-7-use", 6, Page::Home, SettingsTab::Audio, nullptr, false},
        {"home", -1, Page::Home, SettingsTab::Audio, "Deep Voice", false},
        {"voices", -1, Page::Voices, SettingsTab::Audio, nullptr, false},
        {"voices-autotune", -1, Page::Voices, SettingsTab::Audio, "Hard Autotune", false},
        {"voices-pain", -1, Page::Voices, SettingsTab::Audio, "Pain", false},
        {"voices-mix", -1, Page::Voices, SettingsTab::Audio, nullptr, false},
        {"soundboard", -1, Page::Soundboard, SettingsTab::Audio, nullptr, false},
        {"mixer", -1, Page::Mixer, SettingsTab::Audio, "Deep Voice", false},
        {"mixer-effects", -1, Page::Mixer, SettingsTab::Audio, "Streaming Voice", true},
        {"apps", -1, Page::Apps, SettingsTab::Audio, nullptr, false},
        {"devices", -1, Page::Devices, SettingsTab::Audio, nullptr, false},
        {"devices-no-cable", -1, Page::Devices, SettingsTab::Audio, nullptr, false},
        {"recorder", -1, Page::Recorder, SettingsTab::Audio, nullptr, false},
        {"settings-audio", -1, Page::Settings, SettingsTab::Audio, nullptr, false},
        {"settings-hotkeys", -1, Page::Settings, SettingsTab::Hotkeys, nullptr, false},
        {"settings-appearance", -1, Page::Settings, SettingsTab::Appearance, nullptr, false},
        {"settings-startup", -1, Page::Settings, SettingsTab::Startup, nullptr, false},
        {"settings-reset", -1, Page::Settings, SettingsTab::Startup, nullptr, false},
        {"settings-diagnostics", -1, Page::Settings, SettingsTab::Diagnostics, nullptr, false},
        {"settings-about", -1, Page::Settings, SettingsTab::About, nullptr, false},
        {"settings-about-update", -1, Page::Settings, SettingsTab::About, nullptr, false},
        {"home-dark", -1, Page::Home, SettingsTab::Audio, "Deep Voice", false},
        {"voices-ai", -1, Page::Voices, SettingsTab::Audio, nullptr, false},
        {"voices-ai-setup", -1, Page::Voices, SettingsTab::Audio, nullptr, false},
        {"theme-midnight", -1, Page::Voices, SettingsTab::Audio, "Deep Voice", false},
        {"theme-rose", -1, Page::Home, SettingsTab::Audio, "Deep Voice", false},
        {"theme-contrast", -1, Page::Soundboard, SettingsTab::Audio, nullptr, false},
        {"theme-custom", -1, Page::Settings, SettingsTab::Appearance, nullptr, false},
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
        snapshotNoCable_ = std::strcmp(st.file, "wizard-5-virtual-mic-install") == 0 || std::strcmp(st.file, "devices-no-cable") == 0;
        StripModel* mic = primaryMic();
        if (st.preset && mic) {
            if (const ChainPreset* p = presets_.find(st.preset)) applyVoice(*mic, p);
        }
        if (std::strcmp(st.file, "voices-mix") == 0 && mic) {
            voiceMix_ = true;
            setVoices(*mic, {"Deep Voice", "Cave Echo"}, true);
        } else if (std::strcmp(st.file, "voices-mix") != 0) {
            voiceMix_ = false;
        }
        effectsOpen_ = st.effects && mic;
        if (mic) select(ChainTarget{false, mic->id});
        if (st.tab == SettingsTab::Hotkeys || std::strcmp(st.file, "home") == 0) {
            // Example bindings (snapshot mode runs on a throwaway profile).
            HotkeySettings& h = mc_->session().hotkeys;
            h.talkMode = TalkMode::PushToTalk;
            h.talkKey = {vk::XButton2, 0};
            h.muteKey = {'M', KeyModCtrl | KeyModShift};
            if (h.voiceKeys.empty()) {
                h.voiceKeys.push_back(VoiceHotkey{{0x74 /*F5*/, 0}, VoiceKeyMode::Toggle, {"Pain"}});
                h.voiceKeys.push_back(VoiceHotkey{{vk::XButton1, 0}, VoiceKeyMode::Hold, {"Deep Voice", "Cave Echo"}});
            }
        }
        if (std::strcmp(st.file, "soundboard") == 0) {
            SoundboardSettings& sb = mc_->session().soundboard;
            if (sb.pads.size() > 2) {
                sb.pads[0].key = {0x61 /*Num 1*/, 0};
                sb.pads[1].key = {0x62 /*Num 2*/, 0};
                sb.pads[2].mode = PadMode::Hold;
            }
            mc_->soundboard().play(1);
        }
        if (std::strcmp(st.file, "settings-reset") == 0) openResetPopup_ = true;
        // An example of a found update (nothing is downloaded in snapshot mode).
        update_ = UpdateState{};
        if (std::strcmp(st.file, "settings-about-update") == 0) {
            update_.stage = UpdateState::Available;
            update_.release.version = "0.2.0";
            update_.release.installerBytes = 9 << 20;
            update_.release.installerName = "Echo-0.2.0-setup-win64.exe";
            update_.release.notes = "**Highlights**\n- Voices are found in downloaded folders as they are\n- Check for updates in Settings > About";
        }
        snapshotShowAi_ = std::strcmp(st.file, "voices-ai") == 0 || std::strcmp(st.file, "voices-ai-setup") == 0;
        snapshotNoOrt_ = std::strcmp(st.file, "voices-ai-setup") == 0;
        if (std::strcmp(st.file, "home-dark") == 0) {
            mc_->session().ui.theme = 1;
            setDpiScale(dpi_);
        }
        // Ready-made and custom themes, with other accent colours.
        struct ThemeShot {
            const char* file;
            int theme;
            const char* accent;
        };
        for (const ThemeShot& t : {ThemeShot{"theme-midnight", kThemeMidnight, "teal"}, ThemeShot{"theme-rose", kThemeRose, "pink"},
                                   ThemeShot{"theme-contrast", kThemeContrast, "yellow"}, ThemeShot{"theme-custom", kThemeCustom, "custom"}})
            if (std::strcmp(st.file, t.file) == 0) {
                UiSettings& ui = mc_->session().ui;
                ui.theme = t.theme;
                ui.accent = t.accent;
                ui.customAccent = 0x7C3AED;
                ui.customBackground = 0x1B1530;
                ui.customSurface = 0x251D40;
                ui.customText = 0xF1ECFF;
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
    applyPendingVoices();
    finishImports();
    rebuildSoundBank();
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

void EchoApp::applyVoice(StripModel& s, const ChainPreset* p) {
    setVoices(s, p ? std::vector<std::string>{p->name} : std::vector<std::string>{}, true);
}

// ---------------------------------------------------------------------------
// Soundboard
// ---------------------------------------------------------------------------

void EchoApp::triggerPad(int slot, bool down) {
    const auto& pads = mc_->session().soundboard.pads;
    if (slot < 0 || slot >= static_cast<int>(pads.size()) || slot >= kMaxPads) return;
    SoundboardSource& board = mc_->soundboard();
    switch (pads[static_cast<size_t>(slot)].mode) {
    case PadMode::Toggle:
        if (down) board.toggle(slot);
        break;
    case PadMode::Hold:
        if (down) board.play(slot);
        else board.stop(slot);
        break;
    default:
        if (down) board.play(slot);
        break;
    }
}

std::string EchoApp::padProblem(const SoundPad& p) const {
    if (!p.builtin.empty() && !findBuiltinSound(p.builtin)) return "Unknown built-in sound";
    auto it = clipErrors_.find(p.builtin.empty() ? p.file : "builtin:" + p.builtin);
    return it != clipErrors_.end() ? it->second : std::string();
}

void EchoApp::rebuildSoundBank(bool force) {
    const auto& pads = mc_->session().soundboard.pads;
    for (size_t i = 0; i < padModes_.size(); ++i)
        padModes_[i].store(i < pads.size() ? static_cast<int>(pads[i].mode) : 0, std::memory_order_relaxed);
    const int rate = mc_->engine().sampleRate();
    std::string sig = std::to_string(rate);
    for (const auto& p : pads) sig += "|" + p.builtin + ":" + p.file + ":" + std::to_string(p.gainDb);
    if (!force && sig == soundBankSignature_) return;
    soundBankSignature_ = sig;

    auto bank = std::make_shared<SoundBank>();
    std::set<std::string> used;
    for (size_t i = 0; i < pads.size() && i < static_cast<size_t>(kMaxPads); ++i) {
        const SoundPad& p = pads[i];
        const std::string key = (p.builtin.empty() ? "file:" + p.file : "builtin:" + p.builtin) + "@" + std::to_string(rate);
        used.insert(key);
        auto it = clipCache_.find(key);
        if (it == clipCache_.end()) {
            auto clip = std::make_shared<SoundClip>();
            if (!p.builtin.empty()) {
                std::string err;
                clip->samples = loadBuiltinSound(p.builtin, rate, &err);
                if (clip->samples.empty()) clipErrors_["builtin:" + p.builtin] = err;
                else clipErrors_.erase("builtin:" + p.builtin);
            } else {
                const std::wstring path = paths::soundsDir() + L"\\" + utf8ToWide(p.file);
                WavData w;
                std::error_code ec;
                if (!std::filesystem::exists(path, ec)) {
                    clipErrors_[p.file] = "The sound file is missing";
                } else if (!readWav(path, w, nullptr) || w.channels <= 0 || w.samples.empty()) {
                    clipErrors_[p.file] = "The sound file can't be read";
                } else {
                    clip->samples = stereoAtRate(w, rate);
                    clipErrors_.erase(p.file);
                }
            }
            std::shared_ptr<const SoundClip> c;
            if (!clip->samples.empty()) c = clip;
            it = clipCache_.emplace(key, c).first;
        }
        bank->pads.push_back(SoundBank::Pad{it->second, dbToGain(p.gainDb)});
    }
    for (auto it = clipCache_.begin(); it != clipCache_.end();) it = used.count(it->first) ? std::next(it) : clipCache_.erase(it);
    mc_->setSoundBank(bank);
}

void EchoApp::importSounds() {
    const auto& pads = mc_->session().soundboard.pads;
    const size_t room = static_cast<size_t>(kMaxSoundPads) - std::min(static_cast<size_t>(kMaxSoundPads), pads.size() + imports_.size());
    if (room == 0) {
        toast("The soundboard is full (" + std::to_string(kMaxSoundPads) + " sounds). Remove one first.", 2);
        return;
    }
    std::vector<wchar_t> buf(65536, L'\0');
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = hwnd_;
    ofn.lpstrFilter = win::audioFileFilter();
    ofn.lpstrFile = buf.data();
    ofn.nMaxFile = static_cast<DWORD>(buf.size());
    ofn.lpstrTitle = L"Add sounds to the soundboard";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_ALLOWMULTISELECT | OFN_EXPLORER | OFN_NOCHANGEDIR;
    if (!GetOpenFileNameW(&ofn)) return;
    // One file: its full path. Several: the folder, then each file name.
    std::vector<std::wstring> files;
    const std::wstring first(buf.data());
    const wchar_t* p = buf.data() + first.size() + 1;
    if (!*p) files.push_back(first);
    for (; *p; p += std::wcslen(p) + 1) files.push_back(first + L"\\" + p);
    if (files.size() > room) {
        toast("Only " + std::to_string(room) + " more sounds fit on the soundboard.", 2);
        files.resize(room);
    }
    static uint32_t counter = 0;
    for (const auto& f : files) {
        auto imp = std::make_unique<Import>();
        imp->name = wideToUtf8(std::filesystem::path(f).stem().wstring());
        // Echo keeps its own copy (so the pad works if the original moves).
        std::string base;
        for (char c : imp->name)
            if (std::isalnum(static_cast<unsigned char>(c)) || c == ' ' || c == '-' || c == '_') base += c;
        if (base.size() > 40) base.resize(40);
        if (base.empty()) base = "sound";
        char tag[16];
        std::snprintf(tag, sizeof(tag), "%08x", static_cast<unsigned>(GetTickCount64() * 2654435761u + ++counter));
        const std::string fileName = base + "-" + tag + ".wav";
        const std::wstring dest = paths::soundsDir() + L"\\" + utf8ToWide(fileName);
        imp->result = std::async(std::launch::async, [f, dest, fileName]() {
            ImportResult r;
            win::ComScope com(COINIT_MULTITHREADED);
            win::DecodedAudio a;
            std::string err;
            if (!win::decodeAudioFile(f, kMaxSoundSeconds, a, &err)) {
                r.error = err;
                return r;
            }
            r.truncated = a.truncated;
            const size_t frames = a.samples.size() / static_cast<size_t>(a.channels);
            std::vector<float> stereo(frames * 2);
            for (size_t i = 0; i < frames; ++i) {
                const float l = a.samples[i * a.channels];
                stereo[2 * i] = l;
                stereo[2 * i + 1] = a.channels > 1 ? a.samples[i * a.channels + 1] : l;
            }
            constexpr int kRate = 48000;
            if (a.sampleRate != kRate) stereo = resampleOffline(stereo, 2, a.sampleRate, kRate);
            // Never above full scale; short fades so starting or ending it never clicks.
            float peak = 0.0f;
            for (float v : stereo) peak = std::max(peak, std::fabs(v));
            const float g = peak > 0.99f ? 0.99f / peak : 1.0f;
            const size_t n = stereo.size() / 2, in = kRate / 500, out = kRate / 100;
            for (size_t i = 0; i < n; ++i) {
                float e = g;
                if (i < in) e *= static_cast<float>(i) / in;
                if (i + out > n) e *= static_cast<float>(n - i) / out;
                stereo[2 * i] *= e;
                stereo[2 * i + 1] *= e;
            }
            WavWriter w;
            if (!w.open(dest, kRate, 2, 24) || !w.write(stereo.data(), n)) {
                r.error = "Could not save the sound in Echo's sounds folder";
                return r;
            }
            w.close();
            r.file = fileName;
            return r;
        });
        imports_.push_back(std::move(imp));
    }
}

void EchoApp::finishImports() {
    for (auto it = imports_.begin(); it != imports_.end();) {
        Import& im = **it;
        if (im.result.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
            ++it;
            continue;
        }
        const ImportResult r = im.result.get();
        if (!r.error.empty()) {
            toast("Couldn't add '" + im.name + "': " + r.error, 3);
        } else {
            SoundboardSettings& sb = mc_->session().soundboard;
            if (sb.pads.size() < static_cast<size_t>(kMaxSoundPads)) {
                SoundPad pad;
                pad.id = sb.nextPadId++;
                pad.name = im.name;
                pad.file = r.file;
                sb.pads.push_back(pad);
                mc_->markDirty();
                toast(r.truncated ? "Added '" + im.name + "' (only its first 60 seconds)" : "Added '" + im.name + "'",
                      r.truncated ? 2 : 1);
            }
        }
        it = imports_.erase(it);
    }
}

void EchoApp::removePad(uint32_t padId) {
    SoundboardSettings& sb = mc_->session().soundboard;
    auto it = std::find_if(sb.pads.begin(), sb.pads.end(), [&](const SoundPad& p) { return p.id == padId; });
    if (it == sb.pads.end()) return;
    const std::string file = it->file;
    sb.pads.erase(it);
    // Echo's own copy of the sound goes too (the user's original is untouched).
    const bool stillUsed = std::any_of(sb.pads.begin(), sb.pads.end(), [&](const SoundPad& p) { return p.file == file; });
    if (!file.empty() && !stillUsed) {
        const std::wstring path = paths::soundsDir() + L"\\" + utf8ToWide(file);
        DeleteFileW(path.c_str());
        clipErrors_.erase(file);
    }
    mc_->markDirty();
}

void EchoApp::handleFocusRequest() {
    if (!focusRequest_) return;
    const std::string n = focusRequest_;
    focusRequest_ = nullptr;
    if (n == "Effects") {
        effectsOpen_ = hasSelection_;
    } else if (n == "Voice Lab") {
        page_ = Page::Voices;
    } else if (n == "Soundboard") {
        page_ = Page::Soundboard;
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
                 "no telemetry. All audio is processed on this computer.");
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
        std::error_code ec;
        if (!std::filesystem::is_directory(docs, ec))
            toast("The documentation folder is missing (" + wideToUtf8(docs) + ").", 2);
        else if (reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", docs.c_str(), nullptr, nullptr, SW_SHOWNORMAL)) <= 32)
            toast("Couldn't open " + wideToUtf8(docs), 2);
    }
    drawUpdates();
    pop::EndCard();
}

void EchoApp::drawUpdates() {
    const Palette& p = pal();
    const float sc = uiScale();
    using U = UpdateState;
    // Finished background work.
    if (updateCheck_.valid() && updateCheck_.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
        update_ = updateCheck_.get();
    if (updateDownload_.valid() && updateDownload_.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        auto [err, installer] = updateDownload_.get();
        if (err.empty() && win::startUpdate(installer, win::installKind(), &err)) {
            ECHO_LOG_INFO("Updating to Echo %s; the installer restarts Echo", update_.release.version.c_str());
            requestQuit(); // saves; the installer replaces Echo and starts it again
            return;
        }
        update_.stage = U::Failed;
        update_.error = err;
    }
    const std::string page = update_.release.pageUrl.empty() ? "https://github.com/" ECHO_UPDATE_REPO "/releases" : update_.release.pageUrl;
    auto openPage = [&]() { ShellExecuteW(nullptr, L"open", utf8ToWide(page).c_str(), nullptr, nullptr, SW_SHOWNORMAL); };
    auto startCheck = [this]() {
        update_.stage = U::Checking;
        updateCheck_ = std::async(std::launch::async, []() {
            U s;
            const win::UpdateCheck r = win::checkForUpdate(ECHO_VERSION_STRING, s.release, &s.error);
            s.stage = r == win::UpdateCheck::Available ? U::Available : r == win::UpdateCheck::UpToDate ? U::UpToDate : U::Failed;
            return s;
        });
    };

    pop::Divider();
    pop::Label("Updates");
    switch (update_.stage) {
    case U::Idle:
        pop::Caption("Echo looks for a new version only when you click.");
        if (pop::Button(ICON_REFRESH "  Check for updates")) startCheck();
        break;
    case U::Checking:
        pop::Caption("Asking GitHub for the latest version...");
        break;
    case U::UpToDate:
        ImGui::TextColored(toVec4(p.good), ICON_CHECK " You have the latest version (%s).", ECHO_VERSION_STRING);
        if (pop::SmallButton("Check again")) startCheck();
        break;
    case U::Failed:
        pop::TextColoredWrapped(p.bad, ("Couldn't update: " + update_.error).c_str());
        if (pop::SmallButton("Try again")) startCheck();
        ImGui::SameLine();
        if (pop::SmallButton("Open the release page")) openPage();
        break;
    case U::Downloading: {
        const double done = updateProgress_.done.load() / 1048576.0, total = std::max<double>(1.0, update_.release.installerBytes / 1048576.0);
        char label[96];
        std::snprintf(label, sizeof(label), "Downloading Echo %s: %.0f of %.0f MB", update_.release.version.c_str(), done, total);
        ImGui::ProgressBar(static_cast<float>(done / total), ImVec2(std::min(ImGui::GetContentRegionAvail().x, 420.0f * sc), 0), label);
        ImGui::SameLine();
        if (pop::SmallButton("Cancel##update")) updateProgress_.cancel = true;
        break;
    }
    case U::Available: {
        ImGui::PushFont(fonts().semibold, 0.0f);
        ImGui::TextColored(toVec4(p.good), "Echo %s is available%s (you have %s).", update_.release.version.c_str(),
                           update_.release.prerelease ? " as a pre-release" : "", ECHO_VERSION_STRING);
        ImGui::PopFont();
        if (!update_.release.notes.empty()) {
            // The release notes, without Markdown's ** and # marks.
            std::string notes = update_.release.notes;
            for (const char* mark : {"**", "## ", "# "})
                for (size_t at; (at = notes.find(mark)) != std::string::npos;) notes.erase(at, std::strlen(mark));
            ImGui::BeginChild("##notes", ImVec2(std::min(ImGui::GetContentRegionAvail().x, 640.0f * sc), 150.0f * sc),
                              ImGuiChildFlags_Borders);
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextUnformatted(notes.c_str());
            ImGui::PopTextWrapPos();
            ImGui::EndChild();
        }
        if (win::installKind() != win::InstallKind::NotInstalled) {
            if (pop::Button(ICON_DOWNLOAD "  Download and install", pop::Btn::Primary)) {
                update_.stage = U::Downloading;
                updateProgress_.cancel = false;
                updateProgress_.done = 0;
                const updates::Release r = update_.release;
                updateDownload_ = std::async(std::launch::async, [this, r]() {
                    std::string err;
                    std::wstring installer;
                    win::downloadUpdate(r, paths::localDataDir() + L"\\update", updateProgress_, &installer, &err);
                    return std::make_pair(err, installer);
                });
            }
            ImGui::SameLine();
            if (pop::LinkButton("See it on GitHub")) openPage();
            char hint[200];
            std::snprintf(hint, sizeof(hint),
                          "Downloads the installer from GitHub (%.0f MB), checks it, then Echo closes, updates and starts "
                          "again. Your settings stay.",
                          std::max<double>(1.0, update_.release.installerBytes / 1048576.0));
            pop::Hint(hint);
        } else {
            if (pop::Button(ICON_OPEN "  Open the release page", pop::Btn::Primary)) openPage();
            pop::Hint("This copy of Echo wasn't installed with Echo's installer (it's portable or self-built), so it can't "
                      "update itself: get the new version from the release page.");
        }
        break;
    }
    }
}

} // namespace echo::ui
