# Testing

Echo has three layers of testing. This page also records the results of the
last full run on the development machine (2026-10-09, Windows 11 Home build
26200, Intel Core Ultra 9 275HX, Realtek HD audio, Blue Snowball USB mic,
LLVM-MinGW clang 23.1.1, Release build).

## 1. Automated tests (`echo_tests.exe`)

97 test cases, **2684 assertions, all passing** (also under AddressSanitizer +
UndefinedBehaviorSanitizer with zero reports, see section 5). They use generated signals,
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
| Transitions | switching on a latent effect never drops the audio (noise suppression, pitch shifter, echo canceller, limiter: at most 4–5 ms partially dipped while crossfading, vs 11–38 ms of silence before); whole-chain A/B bypass returns seamlessly after 1 s and after 12 s; bus effect bypass is crossfaded (no click) |
| Buffer boundaries & rates | every effect gives the same output (difference < −80 dB after latency alignment) at 480-, 128- and 37-frame blocks; every effect stays finite, bounded and audible at 44.1/48/96 kHz with 64…960-frame blocks |
| Clock drift (simulated time) | 300 ppm device drift with ±1.5 ms packet jitter for 150 s: capture correction 268…332 ppm (−64…+1404 ppm without packet timestamps), render −328…−264 ppm (−1390…+81); a microphone that loses two packets twice drops out once, then the adaptive margin absorbs the repeat |
| Device opening | asynchronous opening: ready, failed (Windows' reason shown, retried) and hung (timed out) devices, the engine keeps running meanwhile; inputs/outputs set to the Windows default follow a default-device change |
| Latency reporting | every latency-reporting effect's reported delay equals its measured delay (noise suppressor 960, echo canceller 768, spectral NR 1024, limiter 96, robot 512, autotune 2180, pitch shifter 1024/2048/4096 samples) |
| AI pipeline | manifest validation (licence required); transparent without a model; **real ONNX Runtime 1.30.0** inference: max audio-thread call 0.15 ms, 0 dry fallbacks after warm-up, 400 ms fixed latency, output level 0.150 (expected 0.15) and stop-band removed; graceful failure on a mismatched model; model selection persisted; **overload** (inference ~2.6x slower than real time, test-only delay): audio thread max 0.24 ms, explicit dry fallback (counted, shown in the UI), stale input skipped instead of queued, full AI output back within 1 s after the load drops |

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
| `soak 180` — mic + system capture + output + VB-CABLE (silence), drift percentiles | 2 final runs: PASS, 0 dropouts each; mic buffer median 25–26 ms, cable output 25–27 ms (see section 6 for before/after) |
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

## 4. Benchmark (`echo_bench.exe`)

Synthetic CPU measurement of `Engine::processBlock()` with in-memory devices
(48 kHz, 480-frame blocks unless noted, 20 s of audio per row, MMCSS thread):

| Scenario | Average | Share of real time |
|---|---|---|
| 16 inputs × 8 buses, all sends on, no effects | 187 µs | 1.9 % |
| one microphone: noise suppression + Streaming Voice + Deep Voice (11 effects), 2 outputs (one resampled) | 184 µs | 1.8 % |
| heavy: 8 microphones with voice chains, 4 apps + 4 buses with music chains | 900 µs (p99 1.2–1.6 ms) | 9 % |
| same at 128-frame blocks | 244 µs (max 1.8 ms = 54–67 % of one block) | 9 % |
| single effects (each alone, incl. a strip, a bus and device I/O ≈ 24 µs) | echo cancellation 130, pitch 85–92, autotune 81–86, noise suppression 72 µs; all others ≤ 66 µs | ≤ 1.3 % |

Worst blocks at small block sizes come from frame-based effects (10 ms RNNoise
frames, FFT windows) that happen to complete in the same block.

## 5. Sanitizers

The whole test suite (97 cases, including the real ONNX Runtime and speech
tests) was built with `-DECHO_SANITIZE=ON` (clang AddressSanitizer +
UndefinedBehaviorSanitizer) and passed with **zero sanitizer reports**.
LeakSanitizer and ThreadSanitizer are not available on Windows; concurrency is
covered by the stress tests with the real engine thread and by review.

## 6. Optimization pass (2026-10-09): before / after

"Before" = commit 4b3243d built in a separate worktree and run with the same
tools on the same machine; "after" = this version.

| Measurement | Before | After |
|---|---|---|
| Start-up: process start → first frame (3 runs, snapshot mode) | 826–1089 ms | 230–255 ms (devices open in the background; the slow Realtek output still takes ~0.5 s to start) |
| UI CPU, window visible and idle (15 s) | 39.7 % of one core (drawing at the monitor's refresh rate) | 9.8 % in front (60 fps), 5.9 % average over 3 min (30 fps behind other windows) |
| Memory, 3 min idle | — | 69.3 → 69.4 MB private (no growth) |
| Silence when switching on noise suppression / pitch / echo cancellation | 15 / 38 / 11 ms | 4 / 4 / 5 ms partial dip (crossfade), no silence |
| A/B "bypass all effects" switching back | 9–11 ms dip | 0–1 ms |
| Bus effect bypass: largest sample step (143 Hz test tone) | 0.47 (click) | 0.015 (the tone's own slope) |
| Real-device soaks, dropouts (mic + system + output + cable) | 6 in 600 s (runs of 120/120/180/180 s: 3, 1, 0, 2) | 1 in 1080 s (six 180 s runs: 0, 0, 1, 0, 0, 0) |
| Buffer level medians in those soaks | mic 23 ms, cable output 21–22 ms | mic 25–26 ms, cable output 25–27 ms (the cost of the larger worst-case margin) |
| DSP CPU (`echo_bench`, all scenarios) | see section 4 | unchanged within ±3 % (no DSP algorithm was changed for speed) |
| `-O2` vs `-O3` release build | — | no measurable difference; `-O2` kept |

Real devices also produce occasional 10 ms steps on their own (Realtek output,
USB microphone, VB-CABLE); the drift controller then corrects for several
seconds (correction excursions up to ~2000 ppm ≈ 3.5 cents, inaudible). These
steps occur in both versions; the larger worst-case margin and the adaptive
margin keep them from becoming dropouts.

## 7. Installer

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
* **Physical unplug/replug, sleep/resume, changing the Windows default device
  by hand** — logic tested with fake devices only.
* **Thread sanitizer** — not available on Windows.
* **Echo cancellation with real speakers** — verified with simulated room echo
  and through the engine with fake devices; not yet with a physical speaker/
  microphone pair (needs someone speaking while audio plays aloud).
* **GPU (DirectML) inference and real voice-conversion models** — no DirectML
  ONNX Runtime build or voice model available; only the pipeline was tested.
* **Interactive UI flows** (clicking every control) — no UI automation
  framework; covered by reviewing snapshots and manual runs.
* **Installer upgrade over a previous version, and installing for all users
  (administrator mode)** — only the per-user install/uninstall cycle above was run.
