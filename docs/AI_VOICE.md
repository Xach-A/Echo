# AI voices (RVC)

Echo can use RVC voice models (made with the RVC WebUI or Applio) on this PC,
in real time. AI voices are optional: the regular voice changer never needs
them.

## Add a voice

1. In Echo: **Voices > AI voice > Set up AI voice > Open the voices folder**.
2. Make a folder for the voice (any name) and put the voice's **.pth** file in
   it, and its **.index** file if it has one.
3. Click **Rescan**, then **Use on mic**.

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
| Response | RVC voices: Quicker, Balanced (default) or Smoother. Quicker is heard sooner but can sound rougher; Smoother sounds cleaner but later. Changing it reloads the voice (a second or two of your plain voice). |
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
voice in blocks, each with some of what you said before for context, and the
newest part of each block only blends into the next one. Echo times the voice
when it loads and sets the delay to leave room for converting a block; if a
voice is too heavy, parts of your plain voice come through and the panel says
so. The Response choice sets the block sizes:

| Response | Block, context, blend | Delay, 12-core (24-thread) CPU | Delay, RTX 5070 Ti Laptop GPU | Difference to whole-file conversion (log-mel) |
|---|---|---|---|---|
| Quicker | 120, 250, 30 ms | 0.30 s (block converted in 0.77 of real time) | 0.24 s (0.19) | 6.6 dB |
| Balanced (default) | 150, 300, 40 ms | 0.37 s (0.68 of real time) | 0.29 s (0.19) | 5.9 dB |
| Smoother | 250, 400, 60 ms | 0.54 s (0.54 of real time) | 0.43 s (0.14) | 5.5 dB |

All of these converted 12 s of speech in real time with no fallbacks to the
plain voice (RVC v2 voice, 32 kHz). On the graphics card the delay is mostly
the block itself plus a safety margin for the moments the PC is busy. For comparison, two whole-file conversions with different random
noise differ by 2.6 dB. The Buffer setting (effects editor) only adds delay on
top (default 100 ms, which is below what any voice needs, so it adds none);
sessions saved with the old default of 400 are moved to 100.

Pitch comes from Echo's own pitch tracker (YIN), not RVC's neural pitch
models (RMVPE, FCPE); octave mistakes on breathy or noisy speech are
possible.

## How it works

Echo reads the .pth file itself (a PyTorch zip; the pickled settings are
interpreted, never run) and builds the voice network as an ONNX graph for
ONNX Runtime: the text encoder (6 transformer layers with relative
positions), the normalizing flow run backwards, and the NSF HiFi-GAN vocoder.
ContentVec is built the same way from its PyTorch weights. Per block:
16 kHz audio → ContentVec features (50 a second) → blended towards the
voice's index → doubled to 100 a second with the pitch of every 10 ms → the
voice network → audio at the voice's rate.

Tools (`echo_hwtest`): `torch-info <file>` lists a checkpoint;
`rvc-convert <voice folder> <in.wav> <out.wav> [pitch] [match] [response 0-2]`
converts a recording through the real-time effect (Quicker, Balanced,
Smoother; default Balanced) and prints the delay; `rvc-offline` converts it in one
piece; `faiss-info <index>` times retrieval.

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

Echo uses the last `chunk` samples of each output (shifted back by the
crossfade length) and crossfades successive chunks.
`tools/make_test_model.py` builds a contract-compliant test graph (a
low-pass filter).

## Privacy

Everything runs on this PC; nothing is uploaded. Official ONNX Runtime builds
contain Microsoft trace logging (only collected if a Windows diagnostic
trace session is active); Echo calls `DisableTelemetryEvents()` right after
loading it. Echo loads ONNX Runtime only from these full paths, never from
Windows' own copy: `<Echo>\ai\`, `%APPDATA%\Echo\ai\`, or the
`ECHO_ONNXRUNTIME` environment variable.
