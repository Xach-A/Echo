# Virtual microphones and virtual speakers

## How it works

Windows only lists audio devices that are provided by a kernel-mode audio
driver. A normal application (Echo included) cannot create a device that
Discord or a game can select. Echo therefore works together with a
**virtual audio cable driver**: a driver that creates a playback device and a
recording device and copies everything played into the first to the second.

| You want | Echo does | Other apps do |
|---|---|---|
| **Virtual microphone** (send your processed voice to Discord/OBS/games) | A **B bus** outputs into the cable's *playback* side (e.g. `CABLE Input`) | Select the cable's *recording* side as their microphone (e.g. `CABLE Output`) |
| **Virtual speaker** (bring an app's audio into Echo) | An input strip records the cable's *recording* side | Play to the cable's *playback* side (set it as the app's output) |

Echo detects common cable drivers automatically (VB-CABLE, VB-Audio Cable
A/B/C/D, Hi-Fi Cable, Virtual Audio Cable by E. Muzychenko, and devices named
"Virtual Cable"), pairs their two sides, and can **verify a pair with a real
loopback test** (Devices > Virtual devices > Run loopback test): it plays a
-20 dBFS tone into the playback side for about a second and checks it arrives
on the recording side. Any other pair can be tested and remembered under
"Test any playback/recording pair".

Without any cable driver, Echo still works fully for mixing, monitoring,
recording, effects, per-app capture (no driver needed, see below), and output
to physical devices. Only "other apps select Echo as a microphone" needs one.

## Installing a cable driver

Echo does not bundle or install drivers itself. The cable driver is a
separate product from its publisher: that keeps Echo's installer free of
third-party redistribution terms and of administrator rights, and Echo never
installs a driver silently.

**Install VB-CABLE** (setup guide step 5, or Devices > Virtual mic when no
cable is found) does the downloading for you, only when you click it:

1. Echo downloads VB-Audio's official driver pack
   (`https://download.vb-audio.com/Download_CABLE/VBCABLE_Driver_Pack45.zip`,
   1.3 MB) into `%LOCALAPPDATA%\Echo\VB-CABLE` and keeps it only if its size
   and SHA-256 (`b950e39f…16bfb`) match the pack Echo was tested with.
2. It unpacks the pack with Windows' own `tar.exe` and opens VB-Audio's
   installer (`VBCABLE_Setup_x64.exe`) with "Run as administrator", so
   Windows asks for your permission first.
3. In VB-Audio's window you click **Install Driver**; restart the PC when it
   is done. Echo finds the new cable by itself.

If VB-Audio replaces the pack, the checksum no longer matches and Echo says so
instead of opening anything; the link to vb-audio.com always works. VB-CABLE
stays VB-Audio's product under VB-Audio's license (donationware: copying the
unmodified pack is allowed, and VB-Audio asks users who like it to donate);
Echo shows the vb-audio.com link next to the button.

Installing it by hand works the same:

* **VB-CABLE** (free, donationware, one cable): https://vb-audio.com/Cable/ —
  download, extract, right-click `VBCABLE_Setup_x64.exe` > Run as
  administrator > Install Driver, then reboot. Extra cables (A+B, C+D) are
  separate packages from the same site.
* **Virtual Audio Cable** (commercial, many cables): https://vac.muzychenko.net/
* Any other loopback driver works if its playback side appears as audio on its
  recording side.

Echo notices the new cable by itself (or press Devices > Virtual devices >
**Rescan**). A virtual (B) bus that has no output yet, such as the default
"Virtual Mic" bus, is connected to the first unused cable automatically and a
notification names the device to select in Discord. Buses you already pointed
somewhere are never changed. To use a different cable, press **Use as virtual
mic** next to it.

VB-CABLE adds two playback devices that both feed `CABLE Output`: the
standard `CABLE Input` and a 16-channel `CABLE In 16ch`. Echo uses
`CABLE Input`; the 16-channel one is listed as an extra endpoint of the same
cable, and feedback protection knows about both routes.

**Number of endpoints**: one virtual microphone or virtual speaker per cable.
With one VB-CABLE you get one; with VB-CABLE A+B, three; with VAC, as many as
you configure there. Echo itself imposes no limit beyond 16 buses and 64 inputs.

**Not usable as cables**: devices owned by other apps (Voicemeeter, Voicemod,
NVIDIA Broadcast, Steam Streaming, SteelSeries Sonar, Elgato Wave Link) only
carry audio while those apps run and route it; Echo labels them
"app virtual device".

## Using the virtual microphone

1. Route your microphone strip to a B bus (the B1 button on the strip is lit).
2. Make sure that bus outputs to the cable (`CABLE Input ...`).
3. In the other app choose the cable's recording side as the **microphone
   (input)** and keep your headphones as its **speakers (output)**:
   * **Discord**: User Settings > Voice & Video > Input Device > `CABLE Output (VB-Audio Virtual Cable)`;
     Output Device stays on your headphones.
     Turn off Discord's noise suppression/echo cancellation if Echo already processes the voice.
   * **Roblox**: in a game, Esc > Settings > Input Device > `CABLE Output` (shown when voice chat is on).
   * **OBS**: Sources > + > Audio Input Capture > Device `CABLE Output`.
   * **Browsers/games**: pick `CABLE Output` as the microphone in their settings.
4. Speak: the B bus meter in Echo and the app's input meter both move.

Leave Windows' own sound settings alone: Windows output stays on your
headphones or speakers, and Windows input stays on your real microphone.
Echo listens to that microphone, so don't make `CABLE Output` the Windows
default input. The Windows volume mixer isn't needed either, except for an
app that has no microphone setting of its own: there, Settings > System >
Sound > Volume mixer > the app > Input device > `CABLE Output`.

Mixing music into the virtual mic: capture just that app (Apps) and route it
to the same B bus. Routing > "Voice + Music Virtual Mic" sends every app input,
"Desktop Audio" included, which is everything the PC plays: the game you're in
and the voice chat too, so people hear echoes of themselves. The setup guide
therefore sends only your voice (and the soundboard).

## Per-application capture without a driver

Echo can capture a single application's audio directly (Applications >
Capture) using the Windows process-loopback API (Windows 10 2004+). The app
keeps playing on its own output too; to hear it only through Echo, use the
**App output** selector (experimental; same setting as Windows Settings >
Sound > Volume mixer) to send the app to a device you don't listen to, such
as a virtual cable, then capture it. Verified on the development machine:
process-loopback capture is *after* the app's Windows volume/mute, so muting
the app in the Volume mixer also silences Echo's capture.

## Measured on the development machine (VB-CABLE, Windows 11)

* `echo_hwtest cables`: a −20 dBFS tone into `CABLE Input` and into
  `CABLE In 16ch` both arrive on `CABLE Output` at −20.0 dBFS.
* `echo_hwtest virtualmic`: Echo's engine (clocked by the headphones, like a
  normal session) plays into `CABLE Input` while a separate stream records
  `CABLE Output`, as Discord would: level −20.0 dBFS exactly, 0 Echo
  underruns, about 150 ms from Echo's output to the recording side (most of
  it inside the cable driver).
* VB-CABLE occasionally delivers a ~10 ms stretch of silence, mostly in the
  first seconds after the streams start and otherwise roughly once every one
  or two minutes. The same happens with a plain Windows audio stream and no
  Echo involved (`echo_hwtest cableraw`), so it comes from the cable driver
  or Windows scheduling. If you hear occasional clicks on the virtual mic,
  VB-CABLE's own control panel has a latency/buffer setting; a larger value
  makes these rarer.

## Removing a driver

Uninstalling Echo never touches drivers (the downloaded pack in
`%LOCALAPPDATA%\Echo\VB-CABLE` goes with Echo's data if you choose to delete
that). To remove VB-CABLE run its setup
again as administrator and choose Remove Driver (or Device Manager > Sound,
video and game controllers > right-click the cable > Uninstall device, tick
"Attempt to remove the driver"), then reboot.

## Why Echo has no driver of its own (and how one could be added)

A Windows audio endpoint requires a kernel-mode audio miniport driver (WaveRT
or the Audio Class eXtension). Microsoft's SysVAD sample shows how to build a
virtual one with the Windows Driver Kit. To be loadable on normal PCs with
Secure Boot it must be signed with an EV code-signing certificate and
submitted to Microsoft's Hardware Dev Center for attestation signing; test-
signed drivers require users to enable test-signing mode, which Echo will not
ask users to do. A future `Echo Virtual Audio` driver would:

1. derive from SysVAD (render + capture endpoints, loopback buffer between them),
2. be built with the WDK and signed through attestation,
3. ship as a **separate, optional** installer step with its own uninstaller.

Echo's detection already recognises devices named "Echo Virtual Audio", so no
application change would be needed beyond packaging.
