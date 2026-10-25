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
clamped to each parameter's range. On an input, `"voice": "Deep Voice"` marks
an effect as part of that voice (so mixed voices can be taken out one by one).

In a chain preset, an effect may also carry `"ui"`: a quick control on the
Voices screen while that voice is selected. `"intensity"` (on a distortion
effect) shows an Intensity slider for its drive; with `"ui_makeup": k`, the
effect's output rises by k dB per dB of drive below the preset's own value, so
lowering the Intensity does not make the voice much quieter. `"optional"` (on a
bit-crusher) shows an on/off switch. The effects themselves ignore these keys.

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
copy them out to export (or use the effects editor's preset manager).

## Session (`%APPDATA%\Echo\session.json`)

```json
{
  "format": "echo.session", "version": 5, "firstRunCompleted": true,
  "autoNoiseSuppression": true, "nextId": 9,
  "strips": [{
    "id": 3, "name": "Microphone", "color": 4283411967, "group": "hardware",
    "voices": ["Deep Voice", "Cave Echo"],
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
  "ui": { "themeMode": 0, "accent": "coral", "customAccent": "#EE5B3B",
          "customTheme": { "background": "#101418", "cards": "#1A2028", "text": "#ECEFF4" },
          "uiScale": 1.0, "windowX": 100, "windowY": 100, "windowW": 1480,
          "windowH": 900, "maximized": false, "showTooltips": true, "compactStrips": false },
  "hotkeys": { "talkMode": 1, "talkKey": { "key": 6, "mods": 0 }, "muteKey": { "key": 77, "mods": 3 },
               "releaseDelayMs": 200,
               "voiceKeys": [ { "key": { "key": 116, "mods": 0 }, "mode": 0, "voices": ["Pain"] } ] },
  "soundboard": { "initialized": true, "builtinVersion": 2, "nextPadId": 18, "stopAllKey": { "key": 0, "mods": 0 },
                  "pads": [ { "id": 1, "name": "Air horn", "builtin": "airhorn", "file": "",
                              "key": { "key": 97, "mods": 0 }, "mode": 0, "gain": 0.0, "color": 0 },
                            { "id": 15, "name": "My sound", "builtin": "", "file": "My sound-1a2b3c4d.wav",
                              "key": { "key": 0, "mods": 0 }, "mode": 2, "gain": -6.0, "color": 0 } ] }
}
```

* `source.kind`: `capture_device`, `render_loopback`, `process_loopback`,
  `system_excluding_echo`, `tone_generator`, `soundboard`, `none`. An empty `deviceId`
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
  newly added microphones get it. Version 3 added the soundboard's mixer
  input (added once to older sessions). Version 4: the voice gate's default
  depth became −60 dB; noise suppressors saved at the old −30 dB default
  move to −60 dB once. Version 5: the AI voice's `latency` (Buffer) became
  the least delay with a default of 100 ms; AI voices saved at the old
  default of 400 move to 100 once.
* `ui.themeMode`: 0 light, 1 dark, 2 match Windows, 3 Midnight, 4 Ocean,
  5 Forest, 6 Sunset, 7 Slate, 8 OLED black, 9 Rose, 10 Sand, 11 Mint,
  12 High contrast, 13 custom (`customTheme`). `accent`: coral, blue, green,
  violet, pink, teal, orange, red, yellow, or custom (`customAccent`).
  Colours are `"#RRGGBB"`; anything else falls back to the default.
* `soundboard.builtinVersion`: the newest set of Echo's built-in sounds the
  board was given (1: the first 14; 2: Clown horn and Gunshot). Newer
  built-ins are added once; built-in pads the user removed stay removed.
* `voices`: the voice presets in use on an input, in the order they were
  picked (older files have one `"voice"` name instead; its effects are then
  tagged with it on loading).
* Key codes (`key`): 1-254 are Windows virtual-key codes (keyboard keys, mouse
  buttons 4 = middle, 5/6 = side buttons, media keys...); 0x101-0x1FF keys
  without a virtual-key code (0x100 + scan code, +0x80 for extended keys);
  0x200/0x201 mouse wheel tilt left/right; 0x300 + controller × 32 + button
  for game controller buttons (XInput bits 0-15, 16/17 = left/right trigger).
  `mods`: 1 Ctrl, 2 Shift, 4 Alt, 8 Win. Unknown codes are dropped on loading.
* `voiceKeys[].mode`: 0 toggle, 1 hold, 2 switch. Soundboard `pads[].mode`:
  0 play (from the start), 1 play/stop, 2 hold to play; `builtin` names one
  of Echo's own sounds, `file` a sound in `%APPDATA%\Echo\sounds`; `gain`
  is −24..+6 dB.
* Buses: `kind` is `physical` (A) or `virtual` (B). `slot` (0-15) links sends to buses.
* Gains are in dB; `pan` is -1..+1; colours are packed RGBA (ImGui ABGR order).

Saving is atomic (`session.json.tmp` → `session.json`, previous version kept
as `session.json.bak`, which is loaded automatically if the main file is
corrupt). Settings > Startup & data > Export/Import settings uses the same format.

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
| `voice_denoise` | strength, mix, vad_gate, vad_threshold, vad_hold, vad_range (default −60), clicks (typing & click removal, default on) |
| `echo_cancel` | tail, suppression, rate (reference set on the strip) |
| `denoise` | reduction, sensitivity, smoothing, learn_fast |
| `delay` | time, feedback, mix, pingpong, low_cut, high_cut, sync (0 off: use time; 1/2, 1/4, dotted 1/8, 1/8, 1/8 triplet, 1/16), bpm |
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
| `harmonizer` | interval1, level1, pan1, interval2, level2, pan2, dry, mode (0 semitones: interval1/2; 1 in key: steps1/2 along key/scale), key, scale, steps1, steps2 |
| `doubler` | detune, delay, width, level, dry |
| `octaver` | sub, up, dry |
| `autotune` | key, scale, style, strength, speed, smoothing, reference, preserve, vib_rate, vib_depth, range, mix, humanize |
| `ai_voice` | pitch, mix, latency, index (Voice match), response (Quality: 0 Fastest, 1 Fast, 2 Balanced, 3 Smooth, 4 Smoothest; saved as "response", its name in Echo 0.1, where 0-2 meant Quicker, Balanced, Smoother); the model itself is selected in the UI |
