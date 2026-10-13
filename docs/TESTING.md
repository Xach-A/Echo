# Testing

Echo has three layers of testing. This page also records the results of the
last full run on the development machine (2026-10-09, Windows 11 Home build
26200, Intel Core Ultra 9 275HX, Realtek HD audio, Blue Snowball USB mic,
LLVM-MinGW clang 23.1.1, Release build).

## 1. Automated tests (`echo_tests.exe`)

160 test cases, **4113 assertions, all passing** (2026-10-11; the 97 cases of
2026-10-09 also ran under AddressSanitizer + UndefinedBehaviorSanitizer with zero
reports, see section 5). They use generated signals,
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
| Persistence & presets | session round trip; tolerant parsing of unknown/future fields; atomic save and recovery from a corrupt file; all 33 factory presets valid; applying "Deep Voice" measured at −5 st through the whole controller path; user preset save/rename/duplicate/delete; older sessions upgraded once (voice gate depth, AI voice Buffer 400 → 100, Response defaulting to Balanced) |
| Recording | bus (post-fader) and strip (pre-fader) recordings in 16/24/32-bit, with 48→44.1 kHz conversion, read back and checked for rate, level and pitch |
| Stress | 300 rapid add/remove/route operations; 40 start/stop cycles of the real engine thread; 120 graph edits while the engine thread runs |
| Noise suppression | hiss, computer fan (hum + broadband), air conditioning, pink room tone (−75…−80 dB); synthetic keyboard clicks (−65 dB energy, peaks −6 → −71 dBFS); digital silence stays silent; clean speech (correlation 0.97, level −0.1 dB, latency 40 ms as reported); speech + fan + typing (noise between words −32 dB, speech −0.5 dB, +8.3 dB spectral SNR); loud speech does not clip; strength 0 / mix 0 transparency; two inputs independent; voice gate (−30 dB extra on noise, speech untouched); 44.1 kHz with resampling (reported latency now within 1 sample of the measured one; it was 61 samples too long before); CPU 0.55 % of one core; default-on rules (new microphones, cables excluded, preset combination, removal remembered, one-time upgrade of old sessions); in the engine: the default microphone strip cleans the virtual mic |
| "Pain" voice | in the factory set with the expected chain and Voices-screen markers, no low-pass anywhere, bit-crusher off by default; squared off: crest 2.7 dB and kurtosis 1.33 on a voiced test sound (dry 5.6 dB / 1.97; Megaphone 10.6 dB, Radio Voice 8.4, Telephone 8.8, Walkie-Talkie 11.4); full-band: 6 kHz and 12 kHz tones come out within 0.6 dB of 1 kHz; 50 Hz hum as strong as the voice comes out 44 dB below it; a quiet voice (about −32 dBFS) comes out at −5.7 dBFS RMS, +20 dB input → +0.5 dB output, peak −0.1 dBFS with +17 dBFS input peaks; −65 dBFS room noise → −95.5 dBFS (−67.5 at full Intensity); Intensity 0–48 dB: level −4.2…+0.7 dB around the default while the crest goes 5.8 → 2.0 dB, never above full scale; speech: band-envelope correlation 0.96, octave-band intelligibility proxy 0.64 (Intensity 0: 0.72, 48: 0.54), −6.5 dBFS RMS out from −20; switching on/off/on again: largest sample step 0.159 vs 0.157 steady (an instant switch: 0.95); session round trip with changed Intensity and Bit-crush |
| Mixed voices | a voice's effects carry its name and the noise cleanup stays in front; mixed voices are ordered pitch → character → echo/reverb whatever the order of picking; taking one out fades only its effects and leaves the others' effects and settings untouched; picking replaces effects added by hand, mixing keeps them; a voice put back while fading out is revived (no new effects); a mix never exceeds 24 effects (refused with a message; a long pick is done in two steps); a voice with its own noise suppression never doubles it; adding a voice and switching voices with audio running: largest sample step 0.023 / 0.042 vs 0.043 steady; saved with the session, and one-voice sessions from before are upgraded |
| Typing & click removal (real recordings) | real keyboard and mouse recordings (BigSoundBank, CC0) at −12 dBFS peaks over a −60 dB room: quick typing −76 dB (peaks −84 dBFS), slow heavy typing −56 dB (−62 dBFS), mouse clicks −72 dB (−90 dBFS), with the voice gate −76…−104 dB; under speech at −18 dBFS peaks: typing in pauses −36 / −43 dB, 64 / 74 dB below the voice, words within 0.06 dB of the same speech without typing; 27 word starts after 300 ms pauses: the first 80 ms within 1 dB for all, the first 30 ms softened for 1 (a quiet "t" burst 50 ms before its vowel; gate: 2, by up to 5 dB) |
| Reset to defaults | every part back to a fresh installation's settings (inputs, outputs, routing and voices; hotkeys; soundboard built-ins without keys; theme, colour, size, tooltips; audio format and recording) while the window keeps its place and the result saves and loads; with only some parts chosen (hotkeys; inputs and outputs; soundboard) everything else stays as it was; nothing chosen changes nothing |
| Hotkey input | a press and release between two polls (instant tap) is seen exactly once; a press whose release is lost times out; controller buttons, wheel tilt, keys without a key code and browser keys can be captured (also with a modifier); names ("Mouse 5", "Controller A", "Shift+Controller 2 LB", "Wheel tilt right"...); voice hotkeys saved, unknown key codes and modes dropped; voice hotkey modes: toggle adds/removes, hold adds and its release removes only what it added, switch replaces / returns to your own voice |
| Soundboard | all 16 built-in sounds (the shipped WAV files, each 48 kHz and listed in `sounds\CREDITS.txt`) at 48 and 44.1 kHz: 0.27–6.2 s, loudest 100 ms at −15.5…−18.6 dBFS RMS (crickets −24), peaks −2.5…−15.4 dBFS at 48 kHz and under −0.95 dBFS after resampling, no DC, silent first and last sample; an unknown sound is refused with a message; built-in pads added once and restorable; sounds added to Echo later (Clown horn, Gunshot) reach older boards once and removed ones stay removed; player: play, start over, toggle, hold (from another thread), stop all, sounds end by themselves, two at once mixed, stopping fades with no click (largest step < 0.05 for a 0.5 sine); a pad removed from the board fades out within the block while others keep playing; resampling 44.1 → 48 kHz keeps pitch and length (mono sounds go to both sides); plays into the virtual mic and the headphones at 0 dB even with push to talk closed; settings saved; WAV read (and cut at the limit), AAC/M4A written with Windows' encoder and decoded back (48 kHz stereo, 1 kHz at 0.5 ± 8 %), a non-audio file refused with a message |
| Echo cancellation | single talk −42 dB after convergence; double talk keeps the near-end voice (correlation 0.92); transparent without reference; end to end through the engine with the playback loopback as reference (−45 dB) |
| Transitions | switching on a latent effect never drops the audio (noise suppression, pitch shifter, echo canceller, limiter: at most 4–5 ms partially dipped while crossfading, vs 11–38 ms of silence before); whole-chain A/B bypass returns seamlessly after 1 s and after 12 s; bus effect bypass is crossfaded (no click) |
| Buffer boundaries & rates | every effect gives the same output (difference < −80 dB after latency alignment) at 480-, 128- and 37-frame blocks; every effect stays finite, bounded and audible at 44.1/48/96 kHz with 64…960-frame blocks |
| Clock drift (simulated time) | 300 ppm device drift with ±1.5 ms packet jitter for 150 s: capture correction 268…332 ppm (−64…+1404 ppm without packet timestamps), render −328…−264 ppm (−1390…+81); a microphone that loses two packets twice drops out once, then the adaptive margin absorbs the repeat |
| Device opening | asynchronous opening: ready, failed (Windows' reason shown, retried) and hung (timed out) devices, the engine keeps running meanwhile; inputs/outputs set to the Windows default follow a default-device change |
| Latency reporting | every latency-reporting effect's reported delay equals its measured delay (noise suppressor 960, echo canceller 768, spectral NR 1024, limiter 96, robot 512, autotune 2180, pitch shifter 1024/2048/4096 samples) |
| RVC voices | a .pth read without running it (settings, 457 tensors of a real voice identical to PyTorch's); the voice network as an ONNX graph vs the official Applio code on a tiny random voice (max difference 2.4e-7) and a real v2 voice (5.3e-6, 106 dB); ContentVec vs Hugging Face's HuBERT on a tiny model (2.9e-6) and the real one (2.6e-5); FAISS IVF-Flat retrieval vs the RVC formula; coarse pitch; voice folders (found, or a message saying what to put there); the real-time pipeline on blocks (also one sample short); odd upsampling rates in 2-D equal the 1-D form; on the graphics card (DirectML build, RTX 5070 Ti Laptop GPU) the voice network and ContentVec match the CPU (3.6e-7, 2.7e-6; skipped with the CPU build); SHA-256 |
| VB-CABLE install | the pinned official download (address, size, SHA-256); unpacking a zip with Windows' tar, and refusing a broken one |
| AI pipeline | manifest validation (licence required); transparent without a model; **real ONNX Runtime** (1.30.0 CPU build and 1.24.4 DirectML build) inference: max audio-thread call 0.15 ms, 0 dry fallbacks after warm-up, 400 ms fixed latency, output level 0.150 (expected 0.15) and stop-band removed; graceful failure on a mismatched model; model selection persisted; the converted voice lands exactly at the reported latency (irregular 440 Hz bursts through the test model: 0 ms off); **overload** (inference ~2.6x slower than real time, test-only delay): audio thread max 0.24 ms, explicit dry fallback (counted, shown in the UI), stale input skipped instead of queued, full AI output back within 1 s after the load drops |

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
| `voice Pain` — a −18 dBFS 300 Hz tone through Pain in the real engine → `CABLE Input` → `CABLE Output`, voice switched off and on again | PASS (2026-10-11, 2 runs of the current, full-scale Pain): −1.8 dBFS RMS received (offline −1.8), distortion 32–33 % (offline 29 %; on again 29–30 %), peak −0.6 dBFS, largest sample step 0.123 around the switches vs 0.122 steady, CPU 0.7–0.8 % of one core, 0 render underruns, 0 recording discontinuities. Earlier versions: 9 runs, one failure of the plain-tone sanity check (voice off), which measures 1–9 % through the cable path depending on the run and allows up to 10 % |
| `soundboard` — Echo's "Ding" and "Air horn" (the shipped recordings) through the soundboard input in the real engine → `CABLE Input` → `CABLE Output` | PASS (2026-10-11, with the real stadium air horn: 2 of 2 runs; with the earlier clown horn: 3 of 3 runs; nothing else playing into the cable): level within 0.01 dB of what Echo played, envelope correlation 0.998–1.000, octave bands within 0.03 dB, 152–189 ms through the cable, 1.4–9.8 ms from play() to Echo's output, 0 render underruns and 0 recording discontinuities during the sounds. Two runs just before FAILED (envelope correlation 0.52 and 0.86, bands within 4.2 and 1.9 dB) while the Echo app was running and sending the microphone into the same cable; the test needs the cable to itself. (With the earlier synthesized sounds: PASS on the last 3 of 5 runs, one early run with 5 render underruns.) |
| `hotkeys` — instant taps: F22 pressed and released in the same `SendInput` call, 20 times | PASS (2026-10-11): 20 of 20 taps reported (press and release each) with raw input; GetAsyncKeyState polling every 5 ms (the method before) saw 1 of 20 |
| `hotkeys` — push to talk and mute key through Windows' real key state (F24/F23 pressed with `SendInput`) | PASS (2 runs): gate opened 4.9–5.3 ms after the key went down (10 presses), closed 156–157 ms after release with a 150 ms release delay, 2 of 2 mute-key presses counted. Synthetic key presses, not a physical keyboard or mouse; not tried inside a running game or with an app running as administrator. |

## 3. Application UI

`Echo.exe --ui-snapshots=<dir>` renders all pages and every setup-wizard step
into PNG files using a throwaway profile, with real devices running. All 17
snapshots (32 now: every screen incl. the Soundboard and mixed voices, the effects editor, Settings sections, all 7 setup
steps, the virtual-mic step and the Devices page as without a cable (Install VB-CABLE), Home in the dark theme, the AI voice setup, and the Midnight, Rose, High contrast and custom themes) (including the Voices screen's noise & echo controls with live noise suppression and
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
* **GPU (DirectML) inference** — no DirectML ONNX Runtime build installed.
* **How RVC voices sound** — the numbers above are measured; nobody has listened
  to Echo's RVC output yet. Real-time conversion differs from whole-file
  conversion by 5.5 dB (log-mel) on a test recording, against 2.6 dB between two
  whole-file conversions with different random noise.
* **Interactive UI flows** (clicking every control) — no UI automation
  framework; covered by reviewing snapshots and manual runs.
* **Installer upgrade over a previous version, and installing for all users
  (administrator mode)** — only the per-user install/uninstall cycle above was run.
