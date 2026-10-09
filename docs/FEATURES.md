# Feature status

Legend

* **Tested** — implemented and covered by automated tests (`echo_tests`).
* **HW-verified** — exercised on real hardware on the development machine
  (Windows 11 build 26200, Realtek HD audio, Blue Snowball USB mic) with
  `echo_hwtest` or by running the app.
* **Implemented** — implemented and running in the app, not covered by an
  automated or hardware check.
* **Partial** — works with the stated limitation.
* **Blocked** — needs an external component that was not available for testing.
* **Planned** — not implemented.

## Audio engine

| Feature | Status | Notes |
|---|---|---|
| Real-time graph: sources → strips → buses → outputs | Tested, HW-verified | Lock-free snapshots; changes apply without restarting. |
| Physical microphone capture (WASAPI shared) | HW-verified | Blue Snowball: 0 dropouts in a 60 s soak. |
| Physical playback | HW-verified | Realtek headphones; confirmed via loopback. |
| Exclusive mode, IAudioClient3 low-latency shared mode | Implemented | Falls back to shared mode automatically; not hardware-tested. |
| Sample rates 44.1/48/88.2/96 kHz, any device rate | Tested | Windowed-sinc SRC; 44.1↔48 kHz verified within 2 cents, 6-channel output. |
| Block sizes 64–2048 | Tested | Output identical across block sizes (37 vs 480 samples). |
| Mono/stereo strips, channel selection, multi-channel devices, downmix | Tested | 4-channel input pair selection; 6-channel output mapping. |
| Clock-drift compensation | Tested, HW-verified | 200 ppm simulated for 60 s with no underruns; real devices track within ±400 ppm. |
| Device-paced engine clock + timer fallback | HW-verified | Clocked by the master output device. |
| Hot-plug, device loss, reconnection, rename/ID changes | Tested | With fake devices; real unplug not physically tested. |
| Sleep/resume | Implemented | Audio restarted 2 s after resume; not tested. |
| Shared input streams (one stream feeds many strips) | Tested | |
| Mute, solo, fader, trim, polarity, pan (constant power) / balance | Tested | Smoothed, click-free. |
| Pre/post-fader sends with per-send gain | Tested | |
| Bus effects, bus fader/mute/mono, safety limiter per bus | Tested | Limiter verified never to exceed its ceiling. |
| Feedback-loop detection and blocking | Tested | Loopback and cable loops; warnings in UI. |
| NaN/instability guard, auto-bypass of faulty effects | Tested | |
| Meters (peak, RMS, hold, clip latch) | HW-verified | Real signal metering seen in the UI. |
| CPU load, underrun/overrun/drift statistics, latency estimates | HW-verified | Diagnostics view. |
| Measured round-trip latency | HW-verified | Output→Windows loopback 150 ms (Echo's share ≈ 63 ms); output→process loopback 118 ms. |
| ASIO | Planned | ASIO SDK license prevents redistribution; use the device's WASAPI driver. |

## Routing

| Feature | Status |
|---|---|
| Routing matrix (any input → any bus, per-send gain, pre/post) | Tested |
| Mixer-strip bus buttons + signal-flow diagram with live levels | Implemented |
| Mic → virtual mic; mic → headphones; both at once | Tested (fake devices); virtual part Blocked on real cable |
| App audio → headphones only / → virtual output; several apps → one bus | Tested |
| Voice + music → virtual mic; independent mixes per endpoint | Tested |
| Routing scene presets (4) | Tested |

## Virtual devices

| Feature | Status | Notes |
|---|---|---|
| Detection of loopback-cable drivers and pairing of their two sides | HW-verified | VB-CABLE: `CABLE Input` paired with `CABLE Output`, the extra `CABLE In 16ch` endpoint kept as a secondary route; app-bound devices (Voicemeeter, Voicemod, NVIDIA Broadcast) correctly not treated as cables. |
| Cable installed separately is picked up automatically | Tested + HW-verified | An unassigned virtual bus gets the cable on startup, device change or Rescan, with a notification; buses the user set are never changed. |
| Loopback test (tone into playback side, detect on recording side) | HW-verified | Both VB-CABLE playback endpoints: −20.0 dBFS received. |
| Virtual microphone usable by Discord/OBS/games | HW-verified up to the app | Echo's engine → `CABLE Input` → `CABLE Output` recorded by a separate stream: exact level, 0 Echo underruns, ~150 ms. Selecting it inside Discord/OBS was not automated. Rare ~10 ms gaps come from the cable driver (also without Echo). |
| Virtual speaker (cable recording side as an input) | HW-verified | In the `virtualmic` test the recording side is an Echo input strip reading `CABLE Output` (0 underruns after start-up). Playing a third-party app into the cable was not separately tested. |
| Multiple named virtual endpoints | Implemented | One per installed cable; Echo allows 16 buses. |
| Echo's own signed driver | Planned | Requires WDK + EV certificate + Microsoft attestation; see `VIRTUAL_DEVICES.md`. |

## Per-application audio

| Feature | Status | Notes |
|---|---|---|
| App session browser (name, icon, device, level, volume, mute) | HW-verified | Chrome, NVIDIA Broadcast, Realtek console shown with icons. |
| Per-app capture (process loopback) | HW-verified | Own-process tone captured at -39 dBFS. |
| "All system audio except Echo" (feedback-safe) | HW-verified | Echo's own tone absent (-120 dBFS) while other audio is captured. |
| Re-attach to an app by name after restart | Tested | |
| Per-app output device selection | HW-verified (API round trip) | Experimental, undocumented Windows interface; set/read-back/reset verified. Effect on a third-party app's playback not automatically verified. |
| Capture is post app-volume/mute | HW-verified | Documented limitation. |

## Effects (all process audio; all fuzz-tested for NaN/Inf/instability)

| Effect | Status |
|---|---|
| Gain/trim, stereo width/balance/mono/swap | Tested |
| 6-band parametric EQ, HP/LP filters 12–48 dB/oct | Tested (measured responses) |
| Compressor (soft knee, peak/RMS, sidechain HPF, parallel mix), limiter (look-ahead), gate, expander, de-esser (LR4 split band), AGC | Tested |
| Noise suppression (RNNoise neural network) with strength, wet/dry mix and voice-activity gate | Tested + HW-verified (see "Noise suppression and echo cancellation" below) |
| Echo cancellation (adaptive filter, reference = any playback device) | Tested (simulated room echo); not yet verified with real speakers |
| Spectral noise reduction (classic Wiener filter) | Tested (≥12 dB stationary-noise reduction, tone preserved) |
| Reverb (FDN), delay (ping-pong, filtered repeats) | Tested |
| Chorus, flanger, phaser, ring modulator | Fuzz-tested; ring mod sidebands measured |
| Distortion (4× oversampled, 5 curves), bit-crusher/downsampler | Tested |
| Telephone / AM radio / walkie-talkie / megaphone / PA / gramophone | Tested (band-limiting measured) |
| Robot (spectral, adjustable pitch) / whisper | Tested |
| Effect chains: add/remove/reorder (drag & drop)/bypass with crossfade, per-chain A/B | Tested (engine) / Implemented (UI) |

## Noise suppression and echo cancellation

| Feature | Status |
|---|---|
| Neural background-noise removal (RNNoise 0.2, runs locally on the CPU, no network/GPU) | Tested: hiss −55 dB, computer fan −74 dB, air conditioning −67 dB, room tone −59 dB, mechanical keyboard clicks −25 dB (peaks −6 → −31 dBFS); speech + fan + typing: noise between words −32 dB, speech level −0.5 dB, +8.4 dB spectral SNR |
| Natural speech kept, no clipping | Tested: clean speech correlation 0.97, level −0.1 dB; −0.4 dBFS speech peaks stay below full scale |
| Strength (attenuation limit), wet/dry mix, on/off, hold-to-compare | Tested (strength 0 / mix 0 are bit-transparent apart from the fixed delay) / Implemented (UI) |
| Voice activity detection + configurable gate (threshold, hold, depth) | Tested: noise-only passages gated a further 30 dB, speech untouched |
| Latency and CPU | Measured: 20 ms at 48 kHz with 10 ms blocks (reported latency matches the measured delay exactly; 35 ms at 44.1 kHz incl. resampling); 0.5 % of one core offline, 1.0–1.4 % live |
| On by default for microphones | Tested: default session, new microphone inputs and the setup wizard add it (not to virtual-cable outputs, apps or test tones); older saved sessions get it once; removing it is remembered; "Turn on for new microphones" option |
| Combines with voice effects | Tested: applying a voice-changer preset keeps the input's noise suppressor (and echo canceller) at the front with its settings; voice-processing presets carry their own (never duplicated) |
| Independent per microphone | Tested (each input/channel has its own network state) |
| Real microphone | HW-verified (Blue Snowball, quiet room with background voices): room noise −56 → −68 dBFS, 1–1.4 % CPU |
| Echo cancellation (speakers instead of headphones) | Tested with simulated room echo (35 ms delay + 120 ms tail): −42 dB after convergence standalone, −45 dB through the whole engine with the playback loopback as reference; double talk keeps the near-end voice (correlation 0.92, level −0.8 dB); transparent without a reference. Not yet verified with real speakers and room acoustics. |
| Reference device selection, status (reference connected, echo reduction, double talk) | Implemented (Voice Lab > Noise & echo, Effects inspector) |

Default chain order on a microphone: echo cancellation → noise suppression →
gate/EQ/compression → voice transformation. Effects can be reordered freely.

## Voice changing

| Feature | Status |
|---|---|
| Pitch shifting ±24 st (phase vocoder, 3 quality modes) | Tested (±12 st within 8 cents) |
| Independent formant shift, formant preservation | Tested (spectral peak stays/moves as expected) |
| Low-latency granular shifter, harmonizer, doubler, octaver | Tested |
| Character presets (deep, chipmunk, robot, monster, alien, radio, telephone, megaphone, echo/cave, cathedral, whisper, feminine/masculine/child-style, giant, doubled, harmony, underwater, lo-fi) | Tested (all parameters valid; Deep Voice measured −5 st end to end) |
| Real-time bypass / hold-to-compare | Implemented |

## Pitch correction

| Feature | Status |
|---|---|
| YIN pitch detection (55–1100 Hz) | Tested (within 5 cents) |
| Key/scale (10 scales), chromatic, strength, retune speed, smoothing, A4 reference, vibrato, voice range | Tested (hard and chromatic correction to within 12 cents; strength 0 leaves pitch) |
| Natural / Hard / Custom styles | Tested |
| Formant preservation (PSOLA) or formant-following mode | Implemented |
| Live pitch graph (input vs corrected, scale notes) | Implemented |

## AI voice

| Feature | Status |
|---|---|
| Async inference pipeline, fixed latency, dry fallback, chunk crossfade | Tested with real ONNX Runtime 1.30.0 + test model |
| Model packages with license/consent metadata, validation | Tested |
| Graceful failure on bad models / missing runtime | Tested |
| GPU (DirectML) | Implemented, not tested |
| Actual voice imitation | Blocked — no voice model available; quality unverified |

## Recording and monitoring

| Feature | Status |
|---|---|
| Record any strips (pre/post fader) and buses to WAV, 16/24-bit PCM or 32-bit float, resampled rates | Tested (files read back: format, rate, level and pitch verified for 16/24/32-bit and 48→44.1 kHz; UI flow not automated) |
| Background writer thread, crash-safe headers, duration/size/dropped indicators | Implemented |
| Record an input without its effects (e.g. your natural voice next to the changed one) | Tested (tap level measured before a +6 dB effect) |
| Recent recordings list (length from the WAV header, open in the default player, show in folder) | Tested (listing, order, header lengths) / Implemented (UI) |
| Test tone generator (sine, square, noise, pink, sweep, impulses, chirps) | Tested / HW-verified |
| Pre/post-fader monitoring via sends | Tested |

## Application

| Feature | Status |
|---|---|
| Dark/midnight/light themes, per-monitor DPI, UI scale, dockable re-arrangeable panels | Implemented (snapshots reviewed) |
| First-run setup wizard (8 steps) | Implemented (snapshots reviewed) |
| Persistence (autosave, atomic writes, backup recovery), import/export | Tested |
| Presets: 32 factory + user save/rename/duplicate/delete/import/export | Tested |
| Safe mode after crash, crash dumps, logs, diagnostic report | Implemented |
| Start with Windows (per user), single instance | Implemented |
| Push to talk / push to mute (hold a key or mouse button; release delay) and a mute key, working while other apps are in front | Tested (key logic, capture of a new key, engine gate silences microphones only) + HW-verified with synthetic key presses (`echo_hwtest hotkeys`: gate opens ≤ 5.3 ms after the key goes down). Not yet tried with a physical key inside a game, or with apps running as administrator. Keys are read with GetAsyncKeyState polling (no keyboard hook); they still reach the app in front. |
| Keep running in the notification area when the window is closed (Open / Mute microphone / Quit menu) | Implemented |
| Accent colour, "Match Windows" theme | Implemented |
| Keyboard shortcuts (Ctrl+S, Ctrl+R, Ctrl+B, F1), tooltips | Implemented |
| Screen-reader accessibility | Partial — Dear ImGui has no UI Automation support; keyboard navigation works. |
| Installer (per user, no admin) + portable ZIP | See `TESTING.md` |
