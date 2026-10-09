# File formats

All Echo files are UTF-8 JSON. Unknown fields are ignored and missing fields
take defaults, so files are forward- and backward-compatible.

## Effect object (used everywhere)

```json
{ "type": "compressor", "bypass": false, "label": "optional custom name",
  "params": { "threshold": -18.0, "ratio": 4.0 } }
```

`type` is a stable effect ID and `params` are keyed by stable parameter IDs
(see the table at the end). Unknown types are skipped with a warning; unknown
parameter IDs are ignored; missing parameters use their defaults; values are
clamped to each parameter's range.

## Chain preset (`*.echopreset`)

```json
{
  "format": "echo.chain-preset",
  "version": 1,
  "name": "Deep Voice",
  "category": "Voice changer",
  "description": "...",
  "chain": [ <effect>, <effect>, ... ]
}
```

Factory presets are listed in `presets/` (exported from the built-in set).
User presets live in `%APPDATA%\Echo\presets\`; copy files there to import,
copy them out to export (or use the Effects panel's preset manager).

## Session (`%APPDATA%\Echo\session.json`)

```json
{
  "format": "echo.session", "version": 2, "firstRunCompleted": true,
  "autoNoiseSuppression": true, "nextId": 9,
  "strips": [{
    "id": 3, "name": "Microphone", "color": 4283411967, "group": "hardware",
    "source": { "kind": "capture_device", "deviceId": "{0.0.1.00000000}.{...}",
                "deviceName": "Microphone (USB Mic)", "processName": "", "exclusive": false },
    "reference": { "kind": "render_loopback", "deviceId": "", "deviceName": "" },
    "trim": 0.0, "fader": 0.0, "pan": 0.0, "mute": false, "solo": false,
    "invert": false, "fxBypass": false, "chanFirst": 0, "chanCount": 1, "downmix": false,
    "sends": [ { "bus": 1, "enabled": true, "gain": 0.0, "preFader": false } ],
    "chain": [ <effect>... ]
  }],
  "buses": [{
    "id": 1, "slot": 0, "name": "Headphones", "color": 4294955597, "kind": "physical",
    "output": { "deviceId": "", "deviceName": "", "exclusive": false },
    "fader": 0.0, "mute": false, "mono": false, "limiter": true, "limiterCeiling": -0.3,
    "fxBypass": false, "chain": []
  }],
  "cables": [ { "name": "...", "renderId": "...", "captureId": "...", "verified": true } ],
  "engine": { "sampleRate": 48000, "blockSize": 480, "resamplerQuality": 1,
              "lowLatencyShared": false, "feedbackProtection": true, "clockBusId": 0 },
  "recorder": { "folder": "C:\\Users\\...\\Music\\Echo Recordings", "bitDepth": 24, "sampleRate": 0 },
  "ui": { "theme": 0, "uiScale": 1.0, "windowX": 100, "windowY": 100, "windowW": 1480,
          "windowH": 900, "maximized": false, "showTooltips": true, "compactStrips": false }
}
```

* `source.kind`: `capture_device`, `render_loopback`, `process_loopback`,
  `system_excluding_echo`, `tone_generator`, `none`. An empty `deviceId`
  means "the Windows default device". Devices are matched by ID first, then
  by `deviceName` (IDs can change after driver reinstalls). Process loopback
  re-attaches to `processName` when the app restarts.
* `group`: `hardware`, `application`, `virtual`, `generator` (display grouping).
* `reference`: signal for effects that need one (the echo canceller): usually
  `render_loopback` of the speakers (empty `deviceId` = default playback
  device); `none` when unused.
* `version`: 2 since noise suppression became a default part of microphone
  inputs. Loading a version-1 file adds it once to microphone strips; after
  that the saved chain is respected. `autoNoiseSuppression` controls whether
  newly added microphones get it.
* Buses: `kind` is `physical` (A) or `virtual` (B). `slot` (0-15) links sends to buses.
* Gains are in dB; `pan` is -1..+1; colours are packed RGBA (ImGui ABGR order).

Saving is atomic (`session.json.tmp` → `session.json`, previous version kept
as `session.json.bak`, which is loaded automatically if the main file is
corrupt). File > Export/Import session uses the same format.

## Voice model manifest

See `AI_VOICE.md`.

## Effect and parameter IDs

| Effect (`type`) | Parameter IDs |
|---|---|
| `gain` | gain, invert |
| `stereo` | width, balance, mono, swap, output |
| `eq` | b1..b6 `_on`, `_freq`, `_gain`, `_q` (b1 low shelf, b6 high shelf), output |
| `filter` | hp_on, hp_freq, hp_slope, lp_on, lp_freq, lp_slope |
| `compressor` | threshold, ratio, attack, release, knee, makeup, auto_makeup, detector, sc_hpf, mix |
| `limiter` | drive, ceiling, release |
| `gate` | threshold, hysteresis, attack, hold, release, range, sc_hpf |
| `expander` | threshold, ratio, attack, release, range, knee |
| `deesser` | freq, threshold, reduction, mode, listen |
| `agc` | target, max_gain, max_cut, speed, gate |
| `voice_denoise` | strength, mix, vad_gate, vad_threshold, vad_hold, vad_range |
| `echo_cancel` | tail, suppression, rate (reference set on the strip) |
| `denoise` | reduction, sensitivity, smoothing, learn_fast |
| `delay` | time, feedback, mix, pingpong, low_cut, high_cut |
| `reverb` | decay, size, predelay, damping, diffusion, low_cut, width, mix |
| `chorus` | rate, depth, delay, voices, spread, mix |
| `flanger` | rate, depth, delay, feedback, stereo, mix |
| `phaser` | stages, rate, depth, center, feedback, stereo, mix |
| `ringmod` | freq, wave, lfo_rate, lfo_depth, mix |
| `distortion` | drive, type, low_cut, tone, output, mix |
| `bitcrusher` | bits, downsample, dither, output, mix |
| `character` | mode, intensity, noise, output, mix |
| `robot` | mode, pitch, output, mix |
| `pitch` | semitones, cents, formant, preserve, quality, output, mix |
| `pitch_lite` | semitones, cents, grain, mix |
| `harmonizer` | interval1, level1, pan1, interval2, level2, pan2, dry |
| `doubler` | detune, delay, width, level, dry |
| `octaver` | sub, up, dry |
| `autotune` | key, scale, style, strength, speed, smoothing, reference, preserve, vib_rate, vib_depth, range, mix |
| `ai_voice` | pitch, mix, latency (the model itself is selected in the UI) |
