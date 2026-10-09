# Testing

Echo has three layers of testing. This page also records the results of the
last full run on the development machine (2026-10-09, Windows 11 Home build
26200, Intel Core Ultra 9 275HX, Realtek HD audio, Blue Snowball USB mic,
LLVM-MinGW clang 23.1.1, Release build).

## 1. Automated tests (`echo_tests.exe`)

86 test cases, **1996 assertions, all passing**. They use generated signals,
the real engine and controller code, and in-memory fake devices (no audio
hardware needed). The AI cases additionally load a real ONNX Runtime.

Speech material for the noise-suppression tests is generated locally with the
Windows text-to-speech voices (`tools/make_speech_samples.ps1`, two voices,
48 kHz; not committed). Without it, the speech cases report SKIPPED and the
synthetic noise/click/silence/echo cases still run.

| Area | Test cases |
|---|---|
| DSP correctness | FFT; gain/polarity; EQ bell response (and UI curve agreement); 24 dB/oct HP; compressor steady state and attack; limiter ceiling on noise; gate open/close; expander; de-esser band selectivity; noise suppression (≥12 dB, tone kept); AGC; delay timing; reverb RT60 range; ring mod sidebands; bit-crusher quantisation; telephone band-limit; robot pitch |
| Voice | YIN accuracy 82–880 Hz (< 5 cents) and unvoiced noise; phase-vocoder shifts −12…+12 st (< 8 cents, level kept); formant preservation (spectral peak stays vs moves); granular octave; autotune hard snap to scale (< 12 cents) and chromatic correction; strength 0 leaves pitch |
| Robustness | every effect under random parameters, 4 trials each (no NaN/Inf, bounded); defaults never silent; reproducible output; NaN-producing effect isolated; bypass crossfade has no discontinuity |
| Engine & routing | unity path; multiple destinations; isolated bus mixes; live routing changes without clicks; mute/solo; dB composition of trim/fader/send/bus; pre-fader sends; pan law/balance; bus limiter; repeated snapshot swaps; identical output for 37- vs 480-sample blocks |
| Controller & devices | virtual-cable pairing with VB-CABLE's real endpoint names (standard endpoint preferred, 16-channel endpoint secondary, different drivers never paired); a cable installed while Echo runs is assigned to the unassigned virtual-mic bus (and never to a bus the user set) and feedback protection covers the secondary endpoint; default session; shared capture streams; mic → headphones + virtual mic with independent gains; feedback blocking (cable loop and loopback loop); device loss and reconnection; reconnect by name; missing devices at startup; app capture by process name; 44.1↔48 kHz with 6-channel output; 200 ppm drift for 60 s with zero under/overruns; 4-channel input channel selection |
| Persistence & presets | session round trip; tolerant parsing of unknown/future fields; atomic save and recovery from a corrupt file; all 32 factory presets valid; applying "Deep Voice" measured at −5 st through the whole controller path; user preset save/rename/duplicate/delete |
| Recording | bus (post-fader) and strip (pre-fader) recordings in 16/24/32-bit, with 48→44.1 kHz conversion, read back and checked for rate, level and pitch |
| Stress | 300 rapid add/remove/route operations; 40 start/stop cycles of the real engine thread; 120 graph edits while the engine thread runs |
| Noise suppression | hiss, computer fan (hum + broadband), air conditioning, pink room tone (−55…−74 dB); mechanical keyboard clicks (−25 dB energy, peaks −6 → −31 dBFS); digital silence stays silent; clean speech (correlation 0.97, level −0.1 dB); speech + fan + typing (noise between words −32 dB, speech −0.5 dB, +8.4 dB spectral SNR); loud speech does not clip; strength 0 / mix 0 transparency; two inputs independent; voice-activity gate (−30 dB extra on noise, speech untouched); 44.1 kHz with resampling; CPU 0.54 % of one core; default-on rules (new microphones, cables excluded, preset combination, removal remembered, one-time upgrade of old sessions); in the engine: the default microphone strip cleans the virtual mic |
| Echo cancellation | single talk −42 dB after convergence; double talk keeps the near-end voice (correlation 0.92); transparent without reference; end to end through the engine with the playback loopback as reference (−45 dB) |
| Latency reporting | every latency-reporting effect's reported delay equals its measured delay (noise suppressor 960, echo canceller 768, spectral NR 1024, limiter 96, robot 512, autotune 2180, pitch shifter 1024/2048/4096 samples) |
| AI pipeline | manifest validation (licence required); transparent without a model; **real ONNX Runtime 1.30.0** inference: max audio-thread call 0.15 ms, 0 dry fallbacks after warm-up, 400 ms fixed latency, output level 0.150 (expected 0.15) and stop-band removed; graceful failure on a mismatched model; model selection persisted |

Run: `build/release/bin/echo_tests.exe` (see `BUILDING.md`).

## 2. Hardware probe (`echo_hwtest.exe`)

Real devices, real WASAPI streams, real engine. Results on the development
machine:

| Check | Result |
|---|---|
| `list` — enumerate endpoints | PASS: 27 endpoints; Voicemeeter/Voicemod/NVIDIA devices classified as app-bound; **no loopback cable installed** |
| `mic` — capture default microphone 2 s | PASS: 99 840 frames, 0 underruns/overruns, levels −47…−88 dBFS (room) |
| `playback` — engine → headphones, verified via loopback | PASS: tone received at −34…−39 dBFS (sent at −40; Realtek APOs change level), 0 render underruns, device-paced clock |
| `playback` — latency (chirp cross-correlation, 5 bursts) | 149.8 ms output→Windows-loopback round trip (stable ±0.1 ms); Echo's own estimate for its buffers: 31 ms output + 32 ms loopback input |
| `procloop` — per-process capture of own audio | PASS: −39 dBFS; muting the app's session also mutes the capture (post-mute); round trip 118 ms |
| `sysaudio` — "all apps except Echo" | PASS: Echo's own tone −120 dBFS in that capture (excluded) vs −38.5 dBFS in own-process capture |
| `approute` — per-app output device (experimental API) | PASS: set → read back → reset to default |
| `soak 60` — mic + system capture + output | PASS: 6005 blocks, 0 dropouts, DSP ≈0.6 %, peak block 6.5 % |
| `cables` — virtual cable loopback | PASS (VB-CABLE installed 2026-10-09): `CABLE Input` → `CABLE Output` and `CABLE In 16ch` → `CABLE Output`, −20.0 dBFS received, ~106–126 ms |
| `virtualmic` — Echo's engine (headphones as clock) → `CABLE Input` → `CABLE Output` recorded by a separate stream | PASS: −20.0 dBFS (sent −20), 0 Echo render underruns, 0 recording discontinuities, ~150 ms; over 78 s after start-up 1 gap of ~10 ms. `cableraw` (no Echo engine) showed the same kind of gaps at a similar rate, so they come from the cable driver/Windows |
| `denoise 12` — default mic through the engine with and without noise suppression (in memory only) | PASS: room noise −56.1 → −68.0 dBFS (−11.9 dB), CPU 1.0–1.4 % of one core, 20 ms latency. The room had background voices (voice probability up to 100 %), so speech preservation was judged by the automated speech tests, not here. |

## 3. Application UI

`Echo.exe --ui-snapshots=<dir>` renders all pages and every setup-wizard step
into PNG files using a throwaway profile, with real devices running. All 17
snapshots (including Voice Lab > Noise & echo with live noise suppression and
echo cancellation on the real microphone) were reviewed; the app ran with live metering from the real
microphone, 0 dropouts, ~1 % DSP load, and exited cleanly. Normal launch,
first-run wizard, autosave, and clean shutdown were also checked by hand.

## 4. Installer

`Echo-0.1.0-setup-win64.exe` was run silently as a normal user
(`/VERYSILENT /SUPPRESSMSGBOXES /CURRENTUSER /DIR=<scratch folder>`): exit
code 0, no elevation prompt, files installed (no ONNX Runtime or drivers
bundled), Start-menu shortcuts and the per-user uninstall entry created. The
installed `Echo.exe` started and rendered all 17 snapshot pages with live
audio, then exited cleanly. The silent uninstall (exit code 0) removed the
program folder, shortcuts and uninstall entry, and kept the user's settings
(the "also delete settings?" question defaults to No).

## Not tested (and why)

* **Selecting the virtual microphone inside Discord/OBS** — not automated;
  the cable's recording side that those apps read was verified directly
  (`echo_hwtest virtualmic`).
* **Exclusive mode, low-latency shared mode** — implemented with automatic
  fallback; not exercised on this hardware.
* **Physical unplug/replug, sleep/resume** — logic tested with fake devices only.
* **Echo cancellation with real speakers** — verified with simulated room echo
  and through the engine with fake devices; not yet with a physical speaker/
  microphone pair (needs someone speaking while audio plays aloud).
* **GPU (DirectML) inference and real voice-conversion models** — no DirectML
  ONNX Runtime build or voice model available; only the pipeline was tested.
* **Interactive UI flows** (clicking every control) — no UI automation
  framework; covered by reviewing snapshots and manual runs.
* **Installer upgrade over a previous version, and installing for all users
  (administrator mode)** — only the per-user install/uninstall cycle above was run.
