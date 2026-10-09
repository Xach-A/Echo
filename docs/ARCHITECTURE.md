# Echo architecture

This document records the technical decisions behind Echo and explains how
the pieces fit together. It is written for developers extending Echo.

## 1. Decisions (ADR summary)

| Decision | Choice | Why | Rejected alternatives |
|---|---|---|---|
| Language | C++20 | Real-time audio needs predictable latency, no GC, direct OS APIs. | C# (GC pauses on the audio path), Rust (no toolchain on the target machine, smaller Windows audio ecosystem). |
| Audio API | WASAPI (shared, exclusive, IAudioClient3 low-latency shared, loopback, process loopback) | Native to Windows 10/11, no third-party SDK, supports every capture mode Echo needs. | ASIO (Steinberg's SDK license forbids redistributing the SDK and most consumer devices lack ASIO drivers; ASIO-only hardware can still be used through its WDM/WASAPI driver). PortAudio/RtAudio (an extra abstraction layer that hides the loopback/process-loopback features Echo depends on). |
| UI | Dear ImGui (docking) on Direct3D 11 | MIT licensed, a few files, renders at monitor rate on its own thread with zero coupling to audio, fully custom widgets (faders, meters, knobs), dockable/re-arrangeable panels. | JUCE (AGPLv3 or commercial since v8; would force AGPL on the whole app), Qt (large LGPL deployment, plugin/DLL management), WebView2 UI (a second runtime and an IPC boundary). |
| Virtual endpoints | Integrate with any installed loopback-cable driver; no Echo kernel driver is shipped | A user-mode app cannot create audio endpoints. A new kernel driver must be EV-signed and attestation-signed by Microsoft to load on Secure Boot systems; shipping an unsigned driver would require users to disable signature enforcement, which Echo must never ask for. | Writing/bundling a SysVAD-derived driver (unsignable here), bundling VB-CABLE (license forbids redistribution). See `VIRTUAL_DEVICES.md`. |
| Per-app capture | WASAPI process loopback (`AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK`) | Official API (Windows 10 2004+), no driver, captures a process tree. Exclude-mode on Echo's own PID gives "all system audio except Echo", which cannot feed back. | Hooking/injection (fragile, anti-cheat hostile). |
| Per-app output selection | Undocumented `IAudioPolicyConfigFactory` (same as Windows Volume mixer), clearly marked experimental, user-initiated only | Lets users send an app to a virtual cable from inside Echo. | — (Windows offers no documented API; fallback opens Windows Settings). |
| DSP | Own implementations of published algorithms | Exact control over real-time safety (no allocation/locks), MIT-compatible, testable. | LGPL/GPL DSP libraries (license), Rubber Band (GPL/commercial). |
| AI inference | ONNX Runtime C API loaded dynamically (optional) | MIT, CPU everywhere, DirectML GPU builds available, stable C ABI usable from any compiler. Echo works fully without it. | Python runtimes (deployment), PyTorch/LibTorch (size, CUDA coupling). |
| Settings | JSON (nlohmann/json), atomic writes with backup | Human-readable, documented, diff-friendly, recoverable. | Registry (opaque, not portable). |
| Build | CMake + Ninja; LLVM-MinGW (clang, static libc++) or MSVC | Reproducible without Visual Studio (no admin install needed); MSVC also supported. | MSBuild-only projects. |

## 2. Module map

```
src/core      Real-time engine: ring buffers, resampler + drift control, sources,
              sinks, strips, buses, graph snapshots, engine thread, meters,
              recorder (WAV writer thread), logging.            [no UI, no WASAPI]
src/dsp       Effect base class, chain runner (bypass crossfades, fault isolation),
              FFT, biquads, STFT, pitch detector, pitch shifters, all effects.
src/session   Session model, MixerController (turns the model into engine graphs),
              DeviceBackend interface, JSON serialisation, presets, paths.
src/audio     Windows platform layer: device enumeration + hot-plug, WASAPI streams,
              process loopback, app sessions, per-app routing, virtual cables,
              WasapiBackend (implements DeviceBackend).
src/ai        Optional AI voice module: dynamic ONNX Runtime loader, model packages,
              asynchronous AI voice effect.
src/ui        Dear ImGui application and views (mixer, effects, routing, voice lab,
              apps, devices, recorder, diagnostics, settings, setup wizard).
src/app       Win32 window, Direct3D 11, main loop, crash handling, single instance.
tests         doctest unit/integration tests with generated signals and fake devices.
tools         echo_hwtest (real-hardware probe), icon and test-model generators,
              ONNX Runtime fetch script.
```

`echo_core` (core + dsp + session + ai) has no dependency on WASAPI, so all
mixing, routing, DSP and controller logic is tested with fake devices.

## 3. Signal path

```
capture device / app process / loopback / test tone
   -> device thread (WASAPI, MMCSS "Pro Audio") -> lock-free ring (device rate)
   -> engine thread: adaptive resampler (rate conversion + clock-drift control)
   -> SOURCE (shared: one stream per device, many strips may read it)
   -> STRIP: channel mapping -> trim/polarity -> effect chain -> fader/pan/mute/solo
              -> sends (per bus: on/off, gain, pre/post fader), feedback-blocked if looping
   -> BUS: sum of sends -> bus effect chain -> fader/mute/mono -> safety limiter
   -> SINK (one per output device, sums all buses assigned to it)
   -> adaptive resampler -> lock-free ring -> device thread -> WASAPI render
```

Buses are the outputs: "A" buses usually target physical devices, "B" buses
target virtual cables (virtual microphones). Several buses may target the
same device; one bus targets at most one device.

## 4. Real-time rules

The engine thread and the device threads never allocate, lock, log, or touch
files. Concretely:

* **Graph changes** are published as immutable `GraphSnapshot`s through one
  atomic pointer. The UI builds the snapshot (allocating freely), swaps it
  in, and frees the old one only after the engine has finished two further
  blocks (epoch counter). Every runtime object referenced by a snapshot is
  kept alive by the snapshot's `keepAlive` list, so deletion always happens
  on the UI thread.
* **Parameters** (faders, sends, effect parameters) are atomics written by the
  UI and read once per block; DSP smooths them (linear ramps, per-block
  coefficient glides) so changes never click.
* **Device I/O** goes through single-producer/single-consumer rings sized at
  stream creation.
* **Statistics** (meters, underruns, drift, CPU) flow back through atomics; the
  UI and the diagnostics view turn them into text.
* **Fault isolation**: every effect's output is checked per block; an effect
  that produces NaN/Inf or a blown-up level is reset, its block replaced by
  the dry signal, and after three faults it is auto-bypassed and flagged in
  the UI. Exceptions never cross the audio callback (`try/catch` around each
  effect and around the block).
* **Denormals** are flushed to zero on every real-time thread (SSE FTZ/DAZ).
* **Effect resets** may run on the audio thread (re-enable, fault recovery),
  so every `onReset()` is allocation-free (RNNoise is re-initialised in place
  with `rnnoise_init`; delay lines are sized for their maximum in `prepare`).
* **Switching effects on/off** never drops audio: a freshly enabled (or newly
  added) effect is reset, then the dry signal is passed through for the
  effect's latency before a 10 ms crossfade (otherwise a latent effect such as
  noise suppression, 20 ms, would output silence for that long). A whole
  chain's A/B bypass (strips and buses) is a 15 ms crossfade; while bypassed,
  the chain keeps running in the background for up to 10 s so a quick A/B
  switch back is seamless, then stops and restarts cleanly on return.

## 5. Clocking and drift

Every physical device has its own crystal; two devices nominally at 48 kHz
differ by tens to hundreds of ppm. Echo picks one output device as the
**engine clock** (the first online physical bus, or the one the user marks).
Its render thread wakes the engine whenever its ring drops below one device
period plus 1 ms, and the engine processes blocks until the ring is full
again, so the master output never needs resampling for drift. When no output
is available the engine falls back to a high-resolution waitable timer.

All other devices pass through a variable-ratio windowed-sinc resampler
(Kaiser window, 16/32/64 taps selectable, 512-phase table with linear phase
interpolation). A PI controller holds each device's buffer at a target level
by nudging the ratio (clamped to ±0.5 %). The same resampler performs fixed
rate conversion (e.g. 44.1 kHz devices in a 48 kHz engine).

* **Sawtooth-free level estimate.** A device fills (or empties) its ring in
  whole packets, so the ring level sampled at engine-block time is a
  sawtooth whose phase slowly wanders with the clock difference; controlling
  on it turns that phase into false "drift" (measured on the development
  machine: corrections swinging by more than 1000 ppm). Each WASAPI device
  thread therefore publishes a `FlowStamp` (frames moved, size and QPC time
  of its newest transfer) through a lock-free sequence lock, and the engine
  controls on the continuous estimate "ring level at the newest packet +
  frames the device has produced since (capped at one packet)". The aim is
  one packet above the old ring target, so the old safety margin is in the
  ring at every packet/block phase (it used to be there only on average);
  this costs on average half a packet (5 ms at 10 ms periods) of extra
  buffering per drift-corrected device, which the latency estimates
  include. A stream (re)starts exactly on its aim.
  Simulated (300 ppm, ±1.5 ms jitter, 150 s): correction range 268–332 ppm
  vs −64…+1404 ppm without timestamps (`tests/test_clock.cpp`).
* **Adaptive margin.** Real devices occasionally slip a whole packet (seen
  on this machine for the Realtek output, a USB microphone and VB-CABLE),
  which shifts a buffer by 10 ms at once. After a dropout a device gets one
  device period of extra buffering (up to two periods); one step is removed again
  after 30 minutes without dropouts. Stable systems keep the lowest latency;
  glitchy ones stop glitching after the first dropout.

## 6. Device lifecycle

`DeviceManager` registers an `IMMNotificationClient`; notifications only set a
dirty flag. The UI thread's `MixerController::update()` rescans, and every
0.5 s checks stream health: streams that report `AUDCLNT_E_DEVICE_INVALIDATED`
(unplug, disable, format change, sleep) are dropped and reopened when the
device is back (matched by ID, then by friendly name). The engine keeps
running throughout; a missing input produces silence and a missing output is
skipped. After system resume the app restarts audio cleanly.

* **Asynchronous opening.** Streams are opened on their own device thread
  (`WasapiCapture/WasapiRender::openAsync`); the controller adopts a stream
  into the graph once it reports Running (checked every UI frame) and shows
  "Starting..." meanwhile. A slow driver (the Realtek output takes ~0.5 s
  here) never blocks the UI; failures report Windows' message, a device that
  does not respond within 10 s times out, and streams abandoned while still
  opening are parked until their thread finishes (destroying them never waits).
* **Windows default devices.** Inputs and outputs set to "Windows default"
  follow the user's default-device changes (they are reopened on the new
  default when Windows notifies a change).
* **Virtual cables** installed while Echo runs are picked up automatically by
  an unassigned virtual (B) bus; buses the user set are never changed.

## 7. Feedback protection

The controller builds a graph: strip → bus (enabled sends), bus → output
device, output device → strips that listen to it (loopback of that device, or
the recording side of the same virtual cable). Any send whose bus can reach
its own strip is a loop; with protection on (default) the send is muted by an
atomic flag and the UI shows a warning. The "all system audio except Echo"
source is loop-free by construction (Windows excludes Echo's process).

## 8. Latency

Echo reports *estimates* computed from real buffer states (device latency
reported by WASAPI, ring targets, resampler group delay, block size, effect
latency). With 48 kHz, 480-sample blocks and 10 ms shared-mode device
periods, mic→output estimates are about 30 ms + 30 ms (input + output paths).
Measured on the development machine: output→Windows loopback round trip
≈150 ms, of which Echo's own buffering is ≈63 ms; the rest is the Windows
loopback tap. Smaller blocks, low-latency shared mode and exclusive mode
reduce Echo's share. No end-to-end latency is promised without measuring the
user's hardware (Diagnostics view, `echo_hwtest playback`, cable loopback test).

## 9. Effects

All effects derive from `Effect` (parameters with metadata, `prepare()` off
the audio thread, `process()` in place on 1 or 2 channels). The chain runner
keeps signals mono while possible (an effect flagged `widensStereo` switches
the chain to stereo) and crossfades bypass changes over 10 ms. Effects with
look-ahead/latency report it (verified against the measured delay by a test
for every such effect); the inspector shows per-effect latency and CPU (the
chain runner times each effect with QPC and keeps a smoothed share of real
time). See `FEATURES.md` for the full list and their test status.

Effects may take a **reference input** (`usesReference()`): a strip names a
reference source (`StripModel::reference`, e.g. the loopback of a playback
device); the graph opens it like any other shared source and the chain runner
hands the reference block to the effect before `process()`. The echo
canceller uses this.

Voice cleanup:

* **Noise Suppression (RNNoise)**: RNNoise 0.2 (Valin 2018: 22-band gains
  from a GRU network plus a pitch comb filter, 10 ms frames at 48 kHz). One
  network state per channel per strip. Frame adapter without extra buffering
  when the engine block is a multiple of 480 samples (fixed 20 ms latency =
  RNNoise's own delay); other rates are resampled to 48 kHz and back. The
  attenuation limit (strength) mixes back the time-aligned input; the VAD
  probability drives an optional gate. x86 builds select SSE4.1/AVX2 kernels
  at runtime.
* **Echo Cancellation (AEC)**: multidelay block frequency-domain adaptive
  filter (256-sample blocks, partitions up to the echo tail), per-bin
  normalised step, double-talk detection (adaptation slowed when the error
  exceeds the predicted echo), divergence reset, and a spectral residual-echo
  suppressor. 16 ms latency.
* **Defaults**: noise suppression is inserted first on microphone strips
  (`MixerController::addDefaultNoiseSuppression`; session option
  `autoNoiseSuppression`); `applyPresetChain` keeps an existing echo
  canceller/noise suppressor in front of a preset that has none; sessions
  older than config version 2 get it once on load (`migrateSession`).

Voice-specific processing:

* **Pitch & Formant Shifter**: phase vocoder (75 % overlap) with peak-region
  shifting and identity phase locking; spectral envelope by liftered cepstrum
  for independent formant shift. 3 quality modes (window 512/1024/2048 at 48 kHz).
* **Low-latency shifter / harmonizer / doubler / octaver**: two-tap granular
  shifter with WSOLA-style correlation alignment at grain restarts.
* **Pitch correction**: YIN detection on a decimated signal with full-rate
  refinement, key/scale snapping with hysteresis, retune speed, transition
  smoothing, vibrato, natural/hard/custom styles, and TD-PSOLA resynthesis
  (formants preserved, or resampled grains when preservation is off).

## 10. AI voice pipeline

```
audio thread:  mono mix -> SPSC input ring ----------------------------+
               output <- crossfade(dry delayed by L, wet from output ring)
worker thread: input ring -> resample to model rate -> [context | chunk]
               -> ONNX Runtime -> crossfaded chunk -> resample -> output ring
```

The audio thread never waits: the wet signal is read at a fixed delay `L`
(user "Buffer", minimum chunk + margin); if the worker is late the effect plays
the equally delayed dry signal and counts the samples. Loading, inference
errors and unloading are handled on the worker; the audio thread only sees an
atomic "active" flag and a lock-free reset handshake. See `AI_VOICE.md`.

## 11. Persistence and recovery

* `%APPDATA%\Echo\session.json` — the whole mixer (strips, buses, routing,
  effects with parameters, engine and UI settings). Autosaved every 3 s when
  changed; atomic replace with a `.bak` fallback used if the main file is
  corrupt.
* `%APPDATA%\Echo\presets\*.echopreset`, `%APPDATA%\Echo\models\*\`, `layout.ini`.
* `%LOCALAPPDATA%\Echo\logs\echo.log` (rotated), `crashes\*.dmp` (minidumps,
  never uploaded), and a `running.marker`; if Echo did not exit cleanly, the
  next start offers **safe mode** (effects bypassed, audio not auto-started).
