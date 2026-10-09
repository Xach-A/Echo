# Echo

A free, open-source Windows 10/11 virtual audio mixer and real-time voice changer:
multichannel mixing and routing, per-application capture, a 30-effect DSP library,
neural background-noise suppression (on by default for microphones) and echo cancellation,
pitch/formant voice changing, pitch correction ("autotune"), recording, and an optional
local AI voice-conversion pipeline. No accounts, subscriptions, ads, telemetry or network access.

## Quick start

1. Install: run `Echo-<version>-setup-win64.exe` (per user, no admin), or unzip the portable build.
2. Start Echo; the setup wizard picks your microphone and headphones.
3. **Virtual microphone for Discord/OBS/games** needs a virtual audio cable driver (e.g. free
   VB-CABLE from vb-audio.com). Install it, press Rescan in Devices; then in Discord choose
   `CABLE Output` as the input device. See `docs/VIRTUAL_DEVICES.md`.
4. Voice Lab: one-click voices (deep, chipmunk, robot, monster, radio, ...), pitch/formant
   sliders and pitch correction. **Noise & echo**: noise suppression strength, voice gate,
   and echo cancellation for people using speakers instead of headphones.

## Build

```powershell
winget install --id MartinStorsjo.LLVM-MinGW.UCRT --scope user
python -m pip install --user cmake ninja
./build.ps1 -Test          # Release build + automated tests
./build.ps1 -Package       # + portable ZIP and installer
```

## Documentation

* `docs/FEATURES.md` — what works and how it was verified (tested / hardware-verified / blocked)
* `docs/ARCHITECTURE.md` — design decisions, real-time model, clocking, AI pipeline
* `docs/BUILDING.md`, `docs/TESTING.md`, `docs/TROUBLESHOOTING.md`
* `docs/VIRTUAL_DEVICES.md`, `docs/AI_VOICE.md`, `docs/FILE_FORMATS.md`

## License

MIT (see `LICENSE`). Third-party components: `THIRD_PARTY_NOTICES.md`.
