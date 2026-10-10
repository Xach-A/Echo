# Troubleshooting

Start with **Diagnostics** (bottom panel): engine state, DSP load, per-device
buffer fill, drift, underruns/overruns and estimated latency, plus the log.
"Save diagnostic report" writes a text file you can share (nothing is sent
anywhere automatically).

## No sound / meters don't move

* **Microphone meter flat**: Windows may block desktop apps from the microphone.
  Settings > Privacy & security > Microphone > allow "Let desktop apps access
  your microphone". Also check the device isn't muted in Windows Sound settings.
* **Strip shows a red status** ("Device not connected"): pick the device again
  from the strip's source button, or plug it back in — Echo reconnects
  automatically within a second or two.
* **Nothing in headphones**: inputs are only heard where they are routed.
  Light the A1 button on the strip (or use Routing). By default the microphone
  goes only to the virtual mic, so you don't hear yourself.
* **Bus "No device"**: a B bus needs a virtual cable (see `VIRTUAL_DEVICES.md`).

## Discord / OBS / game doesn't hear the virtual mic

1. In Echo the B bus meter must move when you speak.
2. The app must use the cable's *recording* side (e.g. `CABLE Output`), not
   `CABLE Input`.
3. Run Devices > Virtual devices > Run loopback test.
4. Some apps open microphones in exclusive mode or cache the device list;
   restart the app after installing the cable.

## Crackles, dropouts, robotic glitches

Echo adds a little buffering to a device after it drops out (shown as the
device's extra margin in Diagnostics) and removes it again after 30 minutes
without problems, so an occasional single click at the start is expected on
some USB/virtual devices; repeated clicks are not.


* Raise the buffer size (Settings > Audio engine > 1024) and press Apply.
* Turn off "Low-latency shared mode" and exclusive mode.
* High DSP load: remove heavy effects (High-quality pitch shifting, AI voice),
  or use the "Low latency" pitch quality.
* Check Diagnostics: underruns on one device point to that device/driver;
  many on the AI effect mean the model is too slow (raise its Buffer).
* Bluetooth headsets switch to low-quality "hands-free" mode when a microphone
  stream is open on them; use a wired mic or a separate device.

## Occasional clicks on the virtual microphone

Measured with VB-CABLE: the cable driver sometimes delivers a ~10 ms gap,
mostly right after streams start (also without Echo). If you hear clicks in
Discord/OBS regularly, raise the buffer/latency setting in VB-CABLE's control
panel, keep Echo's sample rate at 48 kHz like the cable, and check
Diagnostics for Echo dropouts (those would be Echo's, and are counted there).

## Feedback / howling / echo

* Echo blocks routing loops it can detect (yellow/red warnings, red send
  buttons). If you disabled "Feedback protection", turn it back on.
* Acoustic feedback (speakers into the microphone) cannot be detected: use
  headphones when monitoring yourself.
* Capturing "Output loopback" of the device you route to creates a loop; use
  "All system audio (except Echo)" instead.

## Noise suppression / echo cancellation

* **Voice sounds thin or words get cut**: lower *Strength* (60–85 %) in
  Voices > Noise & echo, or turn off the *Voice gate* / lower its
  threshold. Very quiet speech far from the microphone is harder to separate
  from noise — move closer or raise the input gain.
* **Background voices or TV are not removed**: noise suppression keeps
  anything that sounds like speech by design.
* **Turn it off**: Noise & echo tab, the strip's "..." menu, or remove the
  effect. A removed suppressor stays removed; uncheck "Turn on noise
  suppression for newly added microphones" to stop it being added to new ones.
* **Echo cancellation does nothing**: choose the device your speakers are on
  under *Speakers to cancel* (status must show the speakers' level). It needs a
  few seconds of playback to adapt and is not needed with headphones. Audio
  enhancements that change the speaker signal after Windows' loopback point
  (some Realtek/Dolby effects) reduce how much echo can be removed.

## Sound only through Echo, not twice

An app captured with Applications > Capture still plays normally. Use its
**App output** selector to send it to a device you don't listen to (e.g. a
virtual cable), or lower it in the Windows volume mixer — note that muting it
there also mutes Echo's capture.

## Echo won't start / crashes

* After an unclean exit Echo offers safe mode. Start menu also has
  "Echo (safe mode)", or run `Echo.exe --safe-mode`.
* Reset all settings: `Echo.exe --reset-settings` (keeps a backup) or Settings >
  Startup & data > Reset to defaults.
* Logs: `%LOCALAPPDATA%\Echo\logs\echo.log`; crash dumps: `%LOCALAPPDATA%\Echo\crashes`.
* "Could not initialise Direct3D 11": update the graphics driver; Echo falls
  back to software rendering (WARP) when hardware fails.

## AI voice

* "ONNX Runtime is not installed": see `AI_VOICE.md`.
* "Model does not match its manifest": the input/output names in `model.json`
  don't exist in the ONNX file.
* Real-time factor near or above 1.0: the model is too slow; try GPU
  (DirectML build), a smaller model or larger chunks.

## Sample rate / device format changes

Changing a device's format in Windows Sound settings invalidates its stream;
Echo reconnects with the new format automatically and resamples as needed.
