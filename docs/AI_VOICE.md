# AI voice conversion

Echo includes an optional, separate AI voice module. **Regular voice changing
(pitch, formants, characters, pitch correction) never depends on it.**

## Status (honest)

| Part | Status |
|---|---|
| Asynchronous inference pipeline (bounded queues, fixed latency, dry fallback, crossfaded chunks, resampling) | Implemented and tested with a real ONNX Runtime 1.30.0 and a deterministic test model (`tests/data/echo-test-lowpass`): audio-thread calls stay under 0.2 ms, zero fallbacks in steady state, output verified sample-accurately. |
| Model packages (manifest, license/consent fields, validation) | Implemented and tested. |
| CPU inference | Tested (test model). |
| GPU (DirectML) | Implemented, **not tested** (needs the DirectML build of ONNX Runtime and a GPU). Falls back to CPU. |
| Voice imitation quality | **Not verified.** Echo ships no voice models and none was available for testing. Quality, latency and CPU use depend entirely on the model you install. |
| RVC / so-vits-svc checkpoints | **Not directly supported.** They need a wrapper ONNX graph that matches the contract below (feature extractor + generator + pitch input). |

## Requirements

1. **ONNX Runtime** (MIT, Microsoft). Not bundled. Install it explicitly:
   `powershell -ExecutionPolicy Bypass -File tools\fetch_onnxruntime.ps1`
   (asks for confirmation, downloads ~83 MB from github.com/microsoft/onnxruntime,
   verifies SHA-256, copies the DLLs to `ai\` next to Echo.exe). Alternatively copy
   `onnxruntime.dll` and `onnxruntime_providers_shared.dll` there yourself, or put
   them in `%APPDATA%\Echo\ai\`, or point the `ECHO_ONNXRUNTIME` environment
   variable at `onnxruntime.dll`. Echo only loads these full paths (never
   Windows' own System32 copy).
   *Privacy:* official ONNX Runtime builds contain Microsoft trace logging
   (ETW, only collected if a Windows diagnostic trace session is active). Echo
   calls `DisableTelemetryEvents()` immediately after loading the runtime.
2. **A voice model** you have the right to use, packaged as described below,
   placed in `%APPDATA%\Echo\models\<folder>\`.

## Using it

Voices > AI voice (optional) > Set up AI voice > (ONNX Runtime found) > choose a model >
**Use on input**.
The effect appears in that input's chain. Controls: Pitch (passed to models
that support it), Mix, Buffer (fixed latency; raise it if "fallbacks to dry
voice" keeps growing). Status shows inference time per chunk and the
real-time factor (must stay well below 1.0).

## Model package format

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

**ONNX I/O contract ("echo.voice-model v1")**

| Name | Type/shape | Meaning |
|---|---|---|
| `<input>` | float32 `[1, N]` | mono audio at `sample_rate`, range -1..1. N = context + chunk samples (varies by ±1). |
| `<pitch_input>` (optional) | float32 `[1]` | pitch shift in semitones |
| `<output>` | float32 `[1, N]` | converted audio, same rate and length, time-aligned with the input |

Echo uses the last `chunk` samples of each output (shifted back by the
crossfade length) and crossfades successive chunks. Models that need
features (HuBERT/ContentVec), F0 or speaker embeddings must compute them
inside the graph.

`tools/make_test_model.py` shows how to build a contract-compliant graph
(it builds the pipeline test model, which is just a low-pass filter).

## Ethics and safety

Echo includes no voices and does not download models. Use only voices you
own or have explicit permission to use, follow the model's license, and do
not use voice conversion to impersonate people, deceive, or commit fraud.
Many platforms require disclosure of synthetic voices.
