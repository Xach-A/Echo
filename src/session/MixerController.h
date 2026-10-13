// Turns the Session (what the user configured) into a running audio graph.
//
// Responsibilities (UI thread only):
//  * open/close device streams, sharing one stream per device/source,
//  * build and publish engine graph snapshots on every structural change,
//  * reconnect lost devices and follow device-list changes,
//  * choose the engine clock (master output),
//  * detect feedback loops and block offending sends,
//  * effect chain editing, recording, default configuration.
#pragma once

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "core/Engine.h"
#include "core/Recorder.h"
#include "core/SoundboardSource.h"
#include "session/DeviceBackend.h"
#include "session/Session.h"

namespace echo {

struct ChainTarget {
    bool bus = false;
    uint32_t id = 0;
};

// What "Reset to defaults" resets. The user's presets, recordings, AI
// models and sound files are never touched.
struct ResetParts {
    bool mixer = true;      // inputs, outputs, routing, voices and effects
    bool hotkeys = true;    // push to talk, mute key, voice hotkeys
    bool soundboard = true; // back to Echo's built-in sounds, no keys
    bool audio = true;      // sample rate, buffer, quality, recording folder and format
    bool look = true;       // theme, colour, size, tooltips, closing to the notification area
    bool any() const { return mixer || hotkeys || soundboard || audio || look; }
};

struct FeedbackWarning {
    uint32_t stripId = 0;
    uint32_t busId = 0;
    std::string text;
    bool blocked = false;
};

class MixerController {
public:
    explicit MixerController(DeviceBackend& backend);
    ~MixerController();

    Session& session() { return session_; }
    const Session& session() const { return session_; }
    Engine& engine() { return engine_; }
    DeviceBackend& backend() { return backend_; }

    // ---- lifecycle ----
    // threaded=false prepares and publishes the graph without starting the
    // engine thread (tests drive engine().processBlock() manually).
    bool start(bool threaded = true);
    void stop();
    bool running() const { return engine_.running(); }
    // Applies session().engine (sample rate, block size...). Restarts audio.
    void restart();
    // Call once per UI frame. `now` in seconds (monotonic).
    void update(double now);

    // ---- strips ----
    uint32_t addStrip(StripGroup group, const SourceRef& src, const std::string& name);
    void removeStrip(uint32_t id);
    void setStripSource(uint32_t id, const SourceRef& src);
    void moveStrip(uint32_t id, int newIndex);
    void setSend(uint32_t stripId, uint32_t busId, bool enabled);
    void sendsChanged(); // re-run feedback analysis after direct atomic edits

    // ---- buses ----
    uint32_t addBus(BusKind kind, const OutputRef& out, const std::string& name);
    void removeBus(uint32_t id);
    void setBusOutput(uint32_t id, const OutputRef& out);
    void moveBus(uint32_t id, int newIndex);

    // ---- effects ----
    std::vector<std::shared_ptr<Effect>>* chainFor(const ChainTarget& t);
    std::shared_ptr<Effect> createEffect(const std::string& typeId) const; // prepared for the engine
    std::shared_ptr<Effect> addEffect(const ChainTarget& t, const std::string& typeId, int index = -1);
    void removeEffect(const ChainTarget& t, uint32_t instanceId);
    void moveEffect(const ChainTarget& t, int from, int to);
    void setChain(const ChainTarget& t, std::vector<std::shared_ptr<Effect>> chain);
    // Like setChain, for presets: on an input strip, an existing echo
    // canceller / noise suppressor is kept at the front when the new chain
    // has none of its own, so voice effects combine with the cleanup.
    void applyPresetChain(const ChainTarget& t, std::vector<std::shared_ptr<Effect>> chain);
    void chainChanged();

    // ---- voice cleanup defaults ----
    // True for a real microphone (a capture device that is not a virtual
    // cable's output).
    bool isMicrophoneSource(const SourceRef& src) const;
    // Puts noise suppression at the front of a microphone strip's chain when
    // session().autoNoiseSuppression is on and the chain has none. Does not
    // rebuild the graph. Returns true if it added one.
    bool addDefaultNoiseSuppression(StripModel& s);
    // Microphone inputs: the ones push to talk and the mute hotkey act on.
    std::vector<StripModel*> microphoneStrips();
    // True when there is at least one microphone and every one is muted.
    bool microphonesMuted();
    void setMicrophonesMuted(bool muted);
    // One-time upgrades of sessions saved by older versions. Call after
    // loading a session (replaceSession calls it).
    void migrateSession();

    // ---- devices ----
    // How long a device may take to open before it counts as failed (tests).
    void setOpenTimeoutForTesting(double seconds) { openTimeout_ = seconds; }
    // Re-reads the device list (Rescan button), then applies device changes.
    void rescanDevices();
    // Virtual-cable drivers are installed separately by the user. When one
    // is present and a virtual (B) bus has no output yet, the bus gets the
    // first unused cable. Called at startup and on device changes; never
    // touches buses the user already pointed somewhere. Returns true if it
    // assigned one (a notice explains it).
    bool assignCablesToEmptyVirtualBuses();
    // User-facing messages produced by automatic changes (shown as toasts).
    std::vector<std::string> takeNotices() { return std::move(notices_); }

    // ---- generator ----
    ToneSource& tone() { return *tone_; }

    // ---- soundboard ----
    SoundboardSource& soundboard() { return *soundboard_; }
    // Swaps the sounds and publishes the graph (which keeps them alive while
    // the engine may still be playing the old ones).
    void setSoundBank(std::shared_ptr<const SoundBank> bank);
    // The mixer input the soundboard plays through ("Soundboard"): sent to the
    // virtual mic and the headphones. Created if missing; returns its id.
    uint32_t ensureSoundboardStrip();
    StripModel* soundboardStrip();

    // ---- recording ----
    bool startRecording(const std::vector<RecordingTarget>& targets, std::string* error);
    void stopRecording();
    Recorder& recorder() { return recorder_; }

    // ---- status ----
    const std::vector<FeedbackWarning>& feedbackWarnings() const { return warnings_; }
    const SourceRuntime* sourceFor(uint32_t stripId) const;
    const SinkRuntime* sinkFor(uint32_t busId) const;
    const CaptureEndpoint* captureEndpointFor(uint32_t stripId) const;
    const RenderEndpoint* renderEndpointFor(uint32_t busId) const;
    // Estimated input->output latency for a strip feeding a bus (ms).
    double estimatedLatencyMs(uint32_t stripId, uint32_t busId) const;
    uint64_t graphVersion() const { return graphVersion_; }
    std::string clockDescription() const;

    // ---- defaults / persistence support ----
    // Fills the session with a useful starting configuration.
    void createDefaultSession();
    // Settings > Reset to defaults: puts the chosen parts back as a fresh
    // installation has them (the window keeps its place and size). Restarts
    // audio when inputs/outputs or the audio format change.
    void resetSettings(const ResetParts& parts);
    // Replaces the whole session (e.g. after loading a file). Restarts audio.
    void replaceSession(Session s);
    // Prepares runtime objects of every strip/bus/effect for the current format.
    void prepareAll();

    // Set whenever the configuration changes in a way worth saving.
    void markDirty() { dirty_ = true; }
    bool takeDirty() {
        const bool d = dirty_;
        dirty_ = false;
        return d;
    }

    // Graph rebuild (public so the UI can force one after bulk edits).
    void rebuildGraph();

private:
    struct SourceEntry {
        SourceRef ref;
        std::string resolvedId;
        std::shared_ptr<SourceRuntime> runtime;
        std::shared_ptr<CaptureEndpoint> endpoint;
        std::string error;
        double nextRetry = 0.0;
        bool used = false;
        bool pending = false;     // endpoint still opening
        double openedAt = 0.0;    // steady-clock seconds
        uint64_t seen[5] = {};    // glitch counters already logged
    };
    struct SinkEntry {
        OutputRef ref;
        std::string resolvedId;
        std::shared_ptr<DeviceRenderSink> runtime;
        std::shared_ptr<RenderEndpoint> endpoint;
        std::string error;
        double nextRetry = 0.0;
        bool used = false;
        bool pending = false;
        double openedAt = 0.0;
        uint64_t seen[4] = {};
    };

    SourceEntry* ensureSource(SourceRef& ref);
    // Writes every new dropout/gap/discontinuity to the log with the
    // stream's name, so occasional clicks can be traced to their source.
    void logGlitches();
    uint64_t seenEngine_[3] = {};
    void adoptCapture(SourceEntry& e);
    void adoptRender(SinkEntry& e);
    // Adopts streams that finished opening (or records why they failed).
    // Returns true when the graph needs rebuilding.
    bool finishPendingOpens();
    // Inputs/outputs set to "Windows default" move to the new default device
    // when the user changes it in Windows. Returns true if any moved.
    bool followDefaultDevices();
    // Streams dropped while still opening: kept until their thread is done
    // so that destroying them never waits on a slow driver.
    void retireEndpoint(std::shared_ptr<CaptureEndpoint> ep);
    void retireEndpoint(std::shared_ptr<RenderEndpoint> ep);
    double openTimeout_ = 10.0;
    std::vector<std::shared_ptr<CaptureEndpoint>> retiredCaptures_;
    std::vector<std::shared_ptr<RenderEndpoint>> retiredRenders_;
    SinkEntry* ensureSink(const OutputRef& ref);
    void analyzeFeedback();
    void prepareStrip(StripModel& s);
    void prepareBus(BusModel& b);
    void applySnapshot(std::unique_ptr<GraphSnapshot> g);

    DeviceBackend& backend_;
    Session session_;
    Engine engine_;
    std::shared_ptr<ToneSource> tone_;
    std::shared_ptr<SoundboardSource> soundboard_;
    Recorder recorder_;
    std::map<std::string, SourceEntry> sources_;
    std::map<std::string, SinkEntry> sinks_;
    std::map<uint32_t, std::string> stripSourceKey_;
    std::map<uint32_t, std::string> busSinkKey_;
    std::vector<FeedbackWarning> warnings_;
    std::vector<std::string> notices_;
    double now_ = 0.0;
    double lastHealthCheck_ = 0.0;
    uint64_t graphVersion_ = 0;
    bool dirty_ = false;
    bool started_ = false;
};

} // namespace echo
