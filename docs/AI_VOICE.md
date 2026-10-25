# AI voices (RVC)

Echo can use RVC voice models (made with the RVC WebUI or Applio) on this PC,
in real time. AI voices are optional: the regular voice changer never needs
them.

## Add a voice

1. In Echo: **Voices > AI voice > Set up AI voice > Open the voices folder**.
2. Put the voice in it: the folder you downloaded, as it is, or just the
   voice's **.pth** file (and its **.index** file, if it has one).
3. Click **Rescan**, then **Use on mic**.

Echo looks through each folder and its subfolders (four levels), so the usual
download layouts work, e.g. RVC's own `weights\<name>.pth` with
`logs\<name>\added_….index`:

* Every .pth that is a voice model becomes a voice; training checkpoints
  (`G_…` and `D_…` files) and other .pth files are skipped.
* Each voice gets the .index that fits it best: one in its own folder, then one
  named after it, then RVC's `added_…` index. `trained_…` indexes hold no voice
  features and are never used, and an index that belongs to another voice in
  the same folder is never borrowed.
* A folder with several voices (a pack, or one voice saved at several training
  steps) shows each of them, named apart by their file names.
* `echo_hwtest voices` lists what Echo finds and which files it picked.

The first time, Echo also needs two things (the panel shows what's missing):

* **The AI engine**, ONNX Runtime (Microsoft's, free, MIT): click **Install
  the AI engine**. Echo downloads ONNX Runtime 1.24.4 with DirectML (12 MB
  from nuget.org) only then, keeps it only if its size and SHA-256 match, and
  puts its DLLs and license files in `%APPDATA%\Echo\ai`. It converts on the
  graphics card when it can (with the DirectML that comes with Windows) and on
  the CPU otherwise. Alternatives: `tools\fetch_onnxruntime.ps1` (the CPU
  build 1.30 from GitHub, or `-Gpu` for the same DirectML build), or copy
  `onnxruntime.dll` and `onnxruntime_providers_shared.dll` into the `ai`
  folder next to Echo.exe (that one is used first).
* **ContentVec**, the speech model every RVC voice uses (361 MB, MIT): click
  **Download ContentVec**. Echo downloads it only then, from the Applio
  project's copy on Hugging Face, checks its size and SHA-256
  (`d8dd400e…854e`), and keeps it as `%APPDATA%\Echo\ai\contentvec.bin`.

Use only voices you have the right to use, and never to pretend to be
someone. RVC files carry no license information; Echo shows that.

## Controls

| Control | What it does |
|---|---|
| Pitch | Moves your voice up or down (semitones) before the AI voice takes over. +12 suits a deep voice using a high voice's model. |
| Voice match | Voices with an .index: how strongly the index pulls your speech towards the voice's own sound (0–100 %, default 50 %). |
| Mix | Your plain voice vs the AI voice. |
| Quality | RVC voices: five steps from Fastest to Smoothest (default Balanced); the panel shows the delay of each. Smoother steps convert longer pieces and look a little further ahead, so they sound cleaner but reach the listener later. A new step reloads the voice once you let go of the slider (a second or two of your plain voice). |
| Use the graphics card | Shown with the DirectML build of ONNX Runtime; on by default when the fastest card has its own memory (1 GB or more), since built-in graphics can be slower than the CPU. Echo picks the card Windows ranks fastest (on a laptop, the NVIDIA/AMD card rather than the built-in Intel one) and shows its name. Switching reloads the voice. |

## What works

| | Status |
|---|---|
| RVC v2 voices with pitch guidance and the HiFi-GAN vocoder (the usual kind) | Works. Verified on a real 32 kHz v2 voice made with Applio: Echo's voice network matches the official code to within 5e-6 (106 dB signal-to-error); ContentVec matches Hugging Face's implementation to within 3e-5 |
| .index files (FAISS IVF-Flat or flat) | Works: RVC's retrieval (nearest cluster, 8 nearest features, weights 1/distance²), 0.6 ms per frame for a 71,000-vector index |
| RVC v1 voices | Implemented the way the original RVC does it (ContentVec layer 9, projected to 256 values); not tried on a real v1 voice |
| Voices without pitch guidance, the RefineGAN or MRF vocoders, other speech encoders | Not supported; Echo says so when you add one |
| Graphics card (DirectML) | Works with the DirectML build (`fetch_onnxruntime.ps1 -Gpu`, ONNX Runtime 1.24.4). Verified on an NVIDIA GeForce RTX 5070 Ti Laptop GPU with Windows' own DirectML 1.15.5: the voice network and ContentVec match the CPU to within 4e-7 and 3e-6, a real voice sounds the same as on the CPU (0.01 dB log-mel). DirectML doesn't accept 1-D transposed convolutions with output padding, so voices with odd upsampling rates use the equivalent 2-D form (tested equal on the CPU) |

## Speed and delay

An AI voice is always heard a little after you speak: Echo converts your
voice in pieces as you talk. Each run gives the voice up to 2 seconds of what
you said before (the context), so it knows how the sentence goes, and leaves
the newest bit (the lookahead) for the next run, so the part you hear isn't
made at the very edge. Each new piece is lined up with the end of the last one
before they are blended (SOLA, as RVC's own real-time mode does), so the joins
don't cancel or flutter. Echo times the voice when it loads and sets the delay
to leave room for converting a piece; if a voice is too heavy, parts of your
plain voice come through and the panel says so.

Only the newest part of each run goes through the voice's vocoder (the costly
part), which makes the long context cheap on a graphics card. A PC that can't
convert 2 s of context in time gets less (down to 0.4 s; the panel's delay
shows the result). The Quality step sets the rest:

| Quality | Piece, blend, lookahead | Delay, RTX 5070 Ti Laptop GPU | Delay, 12-core CPU (context it gets) | Difference to whole-file conversion, 2 voices (log-mel) |
|---|---|---|---|---|
| Fastest | 80, 20, 0 ms | 0.19 s | 0.25 s (0.4 s) | 5.1 / 5.6 dB |
| Fast | 100, 30, 0 ms | 0.22 s | | 4.8 / 5.4 dB |
| Balanced (default) | 120, 40, 20 ms | 0.28 s | 0.35 s (0.4 s) | 4.6 / 5.4 dB |
| Smooth | 150, 40, 60 ms | 0.35 s | | 4.6 / 5.3 dB |
| Smoothest | 200, 50, 100 ms | 0.47 s | 0.55 s (0.5 s) | 4.5 / 5.4 dB |

Measured with two 40 kHz RVC v2 voices from Hugging Face on 12 s and 10 s of
speech, converted in real time with no fallbacks to the plain voice; on the
graphics card each run took 15–27 ms. Two whole-file conversions with
different random noise differ by 2.7 / 3.4 dB (the best any real-time setting
can do). Before Echo 0.1.2 (0.25–0.4 s of context, plain blends) the same
voices measured 6.4 / 7.2 dB at 0.24 s, 6.0 / 6.5 dB at 0.29 s and 5.3 / 6.3 dB
at 0.43 s. On the CPU, Balanced measured 5.5 dB (Goku) at 0.35 s; the old
Balanced measured 6.0 dB at 0.37 s.

How the 2 s were chosen (Goku, 150 ms pieces): on continuous speech, longer
context kept helping (0.3 s of context 6.0 dB, 0.6 s 5.5, 1 s 5.2, 2 s 4.7;
on the second half of a 22 s recording 2 s 4.7, 4 s 4.5, 6 s 4.4), but not
right after a pause: after 6 s of quiet, 0.5 s of context measured 5.8 dB,
1 s 5.5, 2 s 5.2 and 3 s 5.2, and a context that was mostly silence (4–6 s
long) measured far worse (6–9 dB). Digital silence and a quiet microphone
before speaking measured the same with 2 s.

The Buffer setting (effects editor) only adds delay on top (default 100 ms,
which is below what any voice needs, so it adds none); sessions saved with the
old default of 400 are moved to 100. Sessions saved with Response (Echo 0.1)
move to the step in the same place: Quicker → Fast, Balanced → Balanced,
Smoother → Smooth.

Pitch comes from Echo's own pitch tracker (YIN), not RVC's neural pitch
models (RMVPE, FCPE); octave mistakes on breathy or noisy speech are
possible.

## How it works

Echo reads the .pth file itself (a PyTorch zip; the pickled settings are
interpreted, never run) and builds the voice network as an ONNX graph for
ONNX Runtime: the text encoder (6 transformer layers with relative
positions), the normalizing flow run backwards, and the NSF HiFi-GAN vocoder.
ContentVec is built the same way from its PyTorch weights. Per run:
16 kHz audio → ContentVec features (50 a second) → blended towards the
voice's index → doubled to 100 a second with the pitch of every 10 ms → the
voice network → audio at the voice's rate. As in RVC's real-time mode, the
text encoder reads the whole block, the flow runs on the newest frames plus
the 24 it looks back on, and the vocoder makes only the newest frames (plus
60 ms, its reach back, which is not played); the index is applied where the
flow runs. The pitch of the audio a run shares with the last one is kept, so
only the newest audio is tracked. The index is searched for all frames at once
(the voice's cluster centres are read once per run, not once per frame).

Tools (`echo_hwtest`): `torch-info <file>` lists a checkpoint;
`rvc-convert <voice folder> <in.wav> <out.wav> [pitch] [match] [quality 0-4] [gpu 0/1] [piece context blend lookahead]`
converts a recording through the real-time effect (default Balanced, on the
CPU; the last four, in ms, replace the step's sizes) and prints the delay;
`rvc-offline` converts it in one piece; `rvc-time <voice folder> <in.wav> <context ms> <piece ms> [gpu]`
times each step of a run; `faiss-info <index>` times retrieval.

## Echo model packages (advanced)

Besides RVC voices, a folder can hold an Echo package: an ONNX graph that
takes and returns audio.

```
%APPDATA%\Echo\models\my-voice\
    model.json
    model.onnx
```

`model.json`:

```json
{
  "format": "echo.voice-model",
  "version": 1,
  "name": "My voice (v2)",
  "author": "...",
  "license": "CC-BY-4.0 / custom / ...",
  "source": "where it came from",
  "description": "what it does",
  "consent": "who owns the voice and how consent was given",
  "onnx": "model.onnx",
  "sample_rate": 40000,
  "chunk_ms": 200,
  "context_ms": 100,
  "crossfade_ms": 10,
  "input": "audio",
  "output": "audio_out",
  "pitch_input": "pitch_shift"
}
```

Echo refuses manifests without a `license`. `consent` is shown in the UI.

| Name | Type/shape | Meaning |
|---|---|---|
| `<input>` | float32 `[1, N]` | mono audio at `sample_rate`, range -1..1. N = context + chunk samples (varies by ±1). |
| `<pitch_input>` (optional) | float32 `[1]` | pitch shift in semitones |
| `<output>` | float32 `[1, N]` | converted audio, same rate and length, time-aligned with the input |

Echo plays `chunk` samples of each output, ending the crossfade length plus
up to 10 ms before its end: each is lined up with the end of the last
(within those 10 ms, SOLA) and crossfaded with it. `context_ms` must be at
least the crossfade plus 10 ms for the lining up.
`tools/make_test_model.py` builds a contract-compliant test graph (a
low-pass filter).

## Privacy

Everything runs on this PC; nothing is uploaded. Official ONNX Runtime builds
contain Microsoft trace logging (only collected if a Windows diagnostic
trace session is active); Echo calls `DisableTelemetryEvents()` right after
loading it. Echo loads ONNX Runtime only from these full paths, never from
Windows' own copy: `<Echo>\ai\`, `%APPDATA%\Echo\ai\`, or the
`ECHO_ONNXRUNTIME` environment variable.
