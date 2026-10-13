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
| Install VB-CABLE on click: VB-Audio's official pack downloaded (1.3 MB, size and SHA-256 pinned), unpacked with Windows' tar, VB-Audio's own installer opened as administrator (Windows asks; the user clicks Install Driver) | Download, checksum and unpack HW-verified (`echo_hwtest vbcable-download`: 1.4 s; a second click reuses the checked pack); unpacking tested; the installer itself was not run here (VB-CABLE was already installed) | Never silent; the pack isn't bundled; the vb-audio.com link and VB-Audio's donationware note are shown next to the button. |
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
| Noise suppression (RNNoise neural network) with strength, wet/dry mix, typing & click removal and a voice gate | Tested + HW-verified (see "Noise suppression and echo cancellation" below) |
| Echo cancellation (adaptive filter, reference = any playback device) | Tested (simulated room echo); not yet verified with real speakers |
| Spectral noise reduction (classic Wiener filter) | Tested (≥12 dB stationary-noise reduction, tone preserved) |
| Reverb (FDN), delay (ping-pong, filtered repeats) | Tested |
| Chorus, flanger, phaser, ring modulator | Fuzz-tested; ring mod sidebands measured |
| Distortion (4× oversampled, 5 curves), bit-crusher/downsampler | Tested |
| Telephone / AM radio / walkie-talkie / megaphone / PA / gramophone | Tested (band-limiting measured) |
| Robot (spectral, adjustable pitch) / whisper | Tested |
| "Pain" voice: blown-out microphone modelled on reference recordings, clipping at full scale (gate, fast compressor, pre-emphasis, tube + two hard-clipping stages, de-emphasis, optional bit-crush; no low-pass, peaks just under 0 dBFS), with Intensity (loudness-compensated) and Bit-crush on the Voices screen | Tested (about −5 LUFS from a −20 dBFS voice, crest 2–3 dB vs 8–11 dB for the radio-style voices, half-octave balance within ~3 dB per band of the reference, words kept, room noise kept down at any intensity, click-free switching) + HW-verified (real time through VB-CABLE) |
| Effect chains: add/remove/reorder (drag & drop)/bypass with crossfade, per-chain A/B | Tested (engine) / Implemented (UI) |

## Noise suppression and echo cancellation

| Feature | Status |
|---|---|
| Neural background-noise removal (RNNoise 0.2, runs locally on the CPU, no network/GPU) | Tested: hiss −75 dB, computer fan −80 dB, air conditioning −80 dB, room tone −75 dB; speech + fan + typing: noise between words −32 dB, speech level −0.5 dB, +8.3 dB spectral SNR |
| Typing & click removal (on by default): RNNoise takes some keystrokes for the start of a word; Echo only counts voice that lasts 50 ms or more and doesn't die away like a keystroke, holding the sound back 20 ms to know before a word comes out | Tested on real keyboard and mouse recordings (BigSoundBank, CC0) at −12 dBFS peaks: quick typing −76 dB, slow heavy typing −56 dB, mouse clicks −72 dB (RNNoise alone: −56 / −16 / −33 dB); typing under speech: 36–43 dB less between words, 64–74 dB below the voice, words within 0.1 dB; 27 word starts after a pause all come through (first 80 ms within 1 dB), one quiet "t" burst 50 ms before its vowel is turned down. Noise removal's delay went from 20 to 40 ms |
| Voice gate: near silence (−60 dB) while you're not talking, opened by the same voice check (keystrokes no longer open it) | Tested: typing −76…−104 dB, mouse −85 dB; 27 word starts, first 80 ms within 0.7 dB (two quiet starts softened by up to 5 dB in their first 30 ms); gates saved at the old −30 dB default move to −60 dB once |
| Natural speech kept, no clipping | Tested: clean speech correlation 0.97, level −0.1 dB; −0.4 dBFS speech peaks stay below full scale |
| Strength (attenuation limit), wet/dry mix, on/off, hold-to-compare | Tested (strength 0 / mix 0 are bit-transparent apart from the fixed delay) / Implemented (UI) |
| Voice activity detection + configurable gate (threshold, hold, depth) | Tested: noise-only passages gated a further 30 dB, speech untouched |
| Latency and CPU | Measured: 40 ms at 48 kHz with 10 ms blocks (20 ms of it is the look-ahead of typing & click removal; reported latency matches the measured delay exactly, also at 44.1 kHz with resampling, 54 ms); 0.55 % of one core offline, 1.0–1.4 % live |
| On by default for microphones | Tested: default session, new microphone inputs and the setup wizard add it (not to virtual-cable outputs, apps or test tones); older saved sessions get it once; removing it is remembered; "Turn on for new microphones" option |
| Combines with voice effects | Tested: applying a voice-changer preset keeps the input's noise suppressor (and echo canceller) at the front with its settings; voice-processing presets carry their own (never duplicated) |
| Independent per microphone | Tested (each input/channel has its own network state) |
| Real microphone | HW-verified (Blue Snowball, quiet room with background voices): room noise −56 → −68 dBFS, 1–1.4 % CPU |
| Echo cancellation (speakers instead of headphones) | Tested with simulated room echo (35 ms delay + 120 ms tail): −42 dB after convergence standalone, −45 dB through the whole engine with the playback loopback as reference; double talk keeps the near-end voice (correlation 0.92, level −0.8 dB); transparent without a reference. Not yet verified with real speakers and room acoustics. |
| Reference device selection, status (reference connected, echo reduction, double talk) | Implemented (Voices > Noise & echo, effects editor) |

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
| Mixing voices: pick several voices at once ("Mix voices" on the Voices screen, e.g. Deep Voice + Cave Echo); each can be taken out again without touching the others or their settings; pitch first, echo/reverb last whatever the order of picking; never more than one chain's 24 effects | Tested (order, adding/removing, settings kept, picking vs mixing, no doubled noise suppression, size limit, click-free switching with audio running, saved with the session, old sessions upgraded) |
| Voice hotkeys: any key, mouse button or controller button turns on one voice or several; Toggle (press on / press off), Hold (on while held, like push to talk) or Switch (only these voices / back to your own); none set by default; act on the main microphone | Tested (modes, saving) + the key input is HW-verified (see Application) |

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
| RVC voices: drop a voice's .pth (and .index) into a folder, Use on mic; Pitch, Voice match (index strength), Mix, Response (Quicker / Balanced / Smoother) | Tested: Echo's ONNX graphs of the RVC voice network and of ContentVec match the official implementations (tiny test models in the suite; a real RVC v2 voice: 106 dB signal-to-error); real-time conversion of 12 s of speech with a real voice on the CPU, 0 fallbacks to the plain voice: delay 0.30 s (Quicker), 0.37 s (Balanced), 0.54 s (Smoother); the voice lands within 3 ms of the reported delay |
| .pth reading without running the file's code; .index (FAISS IVF-Flat / flat) retrieval as RVC does it | Tested |
| ContentVec download on click (361 MB, size and SHA-256 checked, kept in `%APPDATA%\Echo\ai`) | Tested (downloaded and verified in 6 s) |
| Async inference pipeline, fixed latency, dry fallback, chunk crossfade; the delay set from the voice's measured speed | Tested with real ONNX Runtime 1.30.0 (the converted voice lands exactly at the reported delay; it was 2 crossfade lengths late before) |
| Echo model packages (model.json + ONNX) with license/consent metadata, validation | Tested |
| RVC v1 voices | Implemented as the original RVC does it, not tried on a real v1 voice |
| GPU (DirectML): `fetch_onnxruntime.ps1 -Gpu`; the fastest card is picked; on by default when available | Tested + HW-verified on an RTX 5070 Ti Laptop GPU: same output as the CPU (graphs within 4e-7, real voice 0.01 dB); delay 0.24 / 0.29 / 0.43 s (Quicker / Balanced / Smoother), 0 fallbacks |
| Pitch for RVC | Echo's YIN tracker (RVC's neural pitch models are not included) |

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
| "Friendly Pop" interface: Home, Voices, Soundboard, Mixer, Apps, Devices, Recorder and Settings screens, floating effects editor; per-monitor DPI, interface size; voice descriptions always shown in full (tiles grow to fit) | Implemented (snapshots of every screen and setup step reviewed) |
| First-run setup wizard (7 steps): microphone, headphones, audio settings, virtual mic (with Install VB-CABLE), test (with hearing yourself), and what to set in apps (Discord and Roblox examples; Windows settings stay as they are) | Implemented (snapshots reviewed, also without a cable) |
| Persistence (autosave, atomic writes, backup recovery), import/export | Tested |
| Reset to defaults (Settings > Startup & data): choose any of inputs/outputs/routing, hotkeys, soundboard, audio quality and recording, look and behaviour; presets, recordings, AI models and sound files are always kept; the window keeps its place | Tested (everything back to a fresh installation; only the chosen parts change; nothing chosen = no change) |
| Presets: 33 factory + user save/rename/duplicate/delete/import/export | Tested |
| Safe mode after crash, crash dumps, logs, diagnostic report; no safe-mode offer after Windows shut down, restarted or signed out (Echo saves its settings then), or when the last run belonged to an earlier Windows sign-in (power loss) | Implemented; shutdown handling tested (simulated shutdown messages + forced end: settings saved, no leftover marker) |
| Start with Windows (per user), single instance | Implemented |
| Push to talk / push to mute (hold a key or mouse button; release delay) and a mute key, working while other apps are in front | Tested (key logic, capture of a new key, engine gate silences microphones only) + HW-verified with synthetic key presses (`echo_hwtest hotkeys`: gate opens ≤ 5 ms after the key goes down). Not yet tried with a physical key inside a game, or with apps running as administrator. |
| Every kind of key input for hotkeys: keyboard keys (also media/browser keys, F13–F24 and keys Windows has no key code for), mouse buttons 3–5 and wheel tilt, and Xbox-style game controller buttons (XInput); presses shorter than one poll (instant taps, as some mice and mouse software send) are never missed | Tested (taps, capture, names, saving) + HW-verified: 20 of 20 instant taps caught (key-state polling alone, the old method, caught 1 of 20). Keys are read with key-state polling plus Windows raw input (no keyboard hook: they still reach the app in front). Not tried with a physical mouse whose button failed before, or with a controller. |
| Soundboard: 16 built-in sounds, real recordings and game sounds, none AI-generated (15 CC0 from BigSoundBank, OpenGameArt and Kenney, one CC BY 4.0 from Wikimedia Commons; shipped in `sounds\` next to Echo.exe, credited in `sounds\CREDITS.txt`; sounds added in later versions reach existing boards once), the user's own sounds (WAV, MP3, M4A/AAC, WMA, FLAC; up to 64 pads, 60 s each, copied into Echo's sounds folder), an optional hotkey per pad (Play, Play/stop or Hold to play) and a stop-all key; plays through its own mixer input into the virtual mic and the headphones, also while push to talk keeps the microphone closed | Tested (every built-in sound shipped, credited, under −1 dBFS also when resampled to 44.1 kHz, and click-free; player modes, bank swaps, resampling, decoding incl. AAC through Windows, routing, saving) + HW-verified through VB-CABLE (`echo_hwtest soundboard`: level ±0.01 dB, octave bands within 0.03 dB, 2–9 ms from play to Echo's output) |
| Keep running in the notification area when the window is closed (Open / Mute microphone / Quit menu) | Implemented |
| Themes: Light, Dark, Match Windows, Midnight, Ocean, Forest, Sunset, Slate, OLED black, Rose, Sand, Mint, High contrast, and a custom theme from three colours (background, cards, text; everything else is worked out from them, with a warning when text would be hard to read); 9 accent colours plus any colour | Implemented (snapshots reviewed: Midnight, Rose, High contrast, custom; settings saved and loaded) |
| Keyboard shortcuts (Ctrl+S, Ctrl+R, Ctrl+B, F1), tooltips | Implemented |
| Screen-reader accessibility | Partial — Dear ImGui has no UI Automation support; keyboard navigation works. |
| Installer (per user, no admin) + portable ZIP | See `TESTING.md` |
