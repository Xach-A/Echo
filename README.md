# Echo

A free, open-source Windows 10/11 virtual audio mixer and real-time voice changer:
multichannel mixing and routing, per-application capture, a 30-effect DSP library,
neural background-noise suppression (on by default for microphones) and echo cancellation,
pitch/formant voice changing, pitch correction ("autotune"), a soundboard, recording, and
optional AI voices (RVC models, run on your PC). No accounts, subscriptions, ads or telemetry;
Echo goes online only when you click Download ContentVec (the speech model RVC voices need) or
Install VB-CABLE.

## Quick start

1. Install: run `Echo-<version>-setup-win64.exe` (per user, no admin), or unzip the portable build.
2. Start Echo; the setup guide picks your microphone and headphones.
3. **Virtual microphone for Discord, Roblox, OBS and games** needs a virtual audio cable driver.
   The setup guide's **Install VB-CABLE** button downloads VB-Audio's free installer and opens it
   (or get it from vb-audio.com). Then in the app choose `CABLE Output` as the microphone (input)
   and keep your headphones as its speakers (output); leave Windows' own sound settings as they
   are. See `docs/VIRTUAL_DEVICES.md`.
4. **Home**: mute, push to talk status, noise removal, hearing yourself and quick voices.
   **Voices**: one-click voices (deep, chipmunk, robot, monster, radio, ...) with the delay each
   adds, pitch/formant and pitch correction, noise removal, voice gate and echo cancellation.
   **Mixer**, **Apps**, **Devices**, **Recorder** and **Settings** (including push-to-talk and
   mute hotkeys) are the other screens.

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
