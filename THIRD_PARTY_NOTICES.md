# Third-party notices

Echo is licensed under the MIT License (see `LICENSE`). It includes or uses
the following third-party components. Each is redistributed under its own
license, reproduced below.

| Component | Version | License | How it is used |
|---|---|---|---|
| Dear ImGui (docking branch) | 1.92.9b | MIT | User interface (compiled into Echo.exe) |
| nlohmann/json | 3.12.0 | MIT | Settings and preset files (compiled in) |
| doctest | 2.5.3 | MIT | Automated tests only (not shipped in Echo.exe) |
| ONNX Runtime C API headers | 1.30.0 | MIT | Compile-time headers for the optional AI module |
| RNNoise (incl. trained model weights) | 0.2 | BSD-3-Clause | Noise suppression effect (compiled into Echo.exe) |
| Soundboard sounds (15 files) | — | CC0 1.0 (public domain) | Built-in soundboard sounds (`sounds\*.wav`, shipped next to Echo.exe) |
| "Sad Trombone" by Sad Tromboner | — | CC BY 4.0 | Built-in soundboard sound (`sounds\sad_trombone.wav`) |

**Not bundled (installed separately by the user, if wanted):**

* **ONNX Runtime binaries** (`onnxruntime.dll`, MIT, Microsoft). Optional, for AI voice
  conversion. Official builds include Microsoft trace-logging; Echo calls
  `DisableTelemetryEvents()` after loading it. See `docs/AI_VOICE.md`.
* **Virtual audio cable drivers** (for example VB-CABLE by VB-Audio, Virtual Audio Cable by
  E. Muzychenko). These are separate products under their own licenses; Echo only detects and
  uses them. Echo does not redistribute any driver.
* **Voice models**. Echo ships no voice models. Users install models they are licensed to use.
* **ContentVec** (the speech model RVC voices need; MIT; by Kaizhi Qian et al., the copy
  published by the Applio project at huggingface.co/IAHispano/Applio). Downloaded only when
  the user clicks Download ContentVec, checked by SHA-256, kept in `%APPDATA%\Echo\ai`.
  Echo doesn't redistribute it.

**System resources used at runtime (not redistributed):** the Segoe UI, Segoe Fluent Icons /
Segoe MDL2 Assets and Consolas fonts installed with Windows.

**RVC voices** are run by Echo's own code (ONNX graphs built from the voice's weights),
written from the published architecture of RVC (Retrieval-based Voice Conversion WebUI, MIT)
and Applio (MIT); their code is not included. FAISS index files are read from the format
FAISS documents; FAISS itself is not included.

**Algorithms and published methods** implemented from the literature (no third-party code):
RBJ Audio EQ Cookbook biquads (R. Bristow-Johnson); YIN pitch estimation (de Cheveigné &
Kawahara, 2002); phase-vocoder pitch shifting with identity phase locking (Laroche & Dolson,
1999); cepstral spectral-envelope estimation; TD-PSOLA; feedback delay network reverb (Jot);
feed-forward compressor gain computer (Giannoulis, Massberg & Reiss, 2012); decision-directed
Wiener noise suppression (Ephraim & Malah, 1984) with minimum-statistics noise tracking
(Martin, 2001); multidelay block frequency-domain adaptive filter for echo cancellation
(Soo & Pang, 1990) with a residual-echo suppressor; Paul Kellet's pink-noise filter
coefficients (public domain).

---

## Soundboard sounds

The soundboard's 16 built-in sounds are recordings and game sounds by other people (none
AI-generated),
shipped as WAV files in the `sounds` folder next to Echo.exe. Echo only trimmed them,
removed DC offset, set their level and converted them to 48 kHz 16-bit WAV
(`tools/prepare_sounds.py`; the buzzer was also saturated). `sounds\CREDITS.txt` lists each
file with its original.

* 15 sounds are dedicated to the public domain under **CC0 1.0**
  (https://creativecommons.org/publicdomain/zero/1.0/): by Joseph Sardin, Dorian Clair
  and Simon Lacelle from BigSoundBank (https://bigsoundbank.com), "Classic fanfare lick"
  by fvcalderan (its last two notes) and "Chunky Explosion" by Joth from OpenGameArt
  (https://opengameart.org/content/classic-fanfare-lick,
  https://opengameart.org/content/chunky-explosion) and `laserRetro_004` from Kenney's
  Sci-fi Sounds (https://kenney.nl/assets/sci-fi-sounds).
  No attribution is required; it is given anyway.
* `sad_trombone.wav`: **"Sad Trombone" by Sad Tromboner**
  (https://commons.wikimedia.org/wiki/File:Sad_Trombone.wav), from Wikimedia Commons,
  licensed under **CC BY 4.0** (https://creativecommons.org/licenses/by/4.0/). Trimmed and
  level-adjusted for Echo.

The noise-removal tests use short excerpts of BigSoundBank keyboard and mouse recordings
(CC0, by Joseph Sardin; `tests\data\noise\CREDITS.txt`). They are not shipped.

## ONNX Runtime and DirectML (not included)

ONNX Runtime (MIT, Microsoft) is downloaded only on request: by Echo's **Install the AI engine**
button (the DirectML build from nuget.org, with its LICENSE and ThirdPartyNotices.txt, into
`%APPDATA%\Echo\ai`), or by `tools\fetch_onnxruntime.ps1` (the CPU build from GitHub, or with
`-Gpu` the DirectML build). The DirectML build uses the DirectML that is part of Windows. Only if Windows' copy is too old does the script
offer Microsoft's DirectML package, under Microsoft's DirectML license (which allows Microsoft to
collect usage data through Windows); it asks first.

## VB-CABLE (not included)

Echo doesn't include VB-CABLE. Only when you click **Install VB-CABLE**, Echo downloads
VB-Audio's unmodified official driver pack from vb-audio.com and opens VB-Audio's own
installer. VB-CABLE is © Vincent Burel (VB-Audio), donationware, under VB-Audio's license
(`readme.txt` in the pack; https://vb-audio.com/Services/licensing.htm). If it helps you,
VB-Audio welcomes donations: https://vb-audio.com/Cable/

---

## RNNoise

Source: https://gitlab.xiph.org/xiph/rnnoise (release 0.2), vendored unmodified in
`third_party/rnnoise` (see `VERSION.txt` there for the archive checksum).

```
Copyright (c) 2007-2017, 2024 Jean-Marc Valin
Copyright (c) 2023 Amazon
Copyright (c) 2017, Mozilla
Copyright (c) 2005-2017, Xiph.Org Foundation
Copyright (c) 2003-2004, Mark Borgerding

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions
are met:

- Redistributions of source code must retain the above copyright
notice, this list of conditions and the following disclaimer.

- Redistributions in binary form must reproduce the above copyright
notice, this list of conditions and the following disclaimer in the
documentation and/or other materials provided with the distribution.

- Neither the name of the Xiph.Org Foundation nor the names of its
contributors may be used to endorse or promote products derived from
this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
A PARTICULAR PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE FOUNDATION
OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
```

---

## Dear ImGui

```
The MIT License (MIT)

Copyright (c) 2014-2026 Omar Cornut

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

---

## nlohmann/json

```
MIT License 

Copyright (c) 2013-2025 Niels Lohmann

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

---

## doctest

```
The MIT License (MIT)

Copyright (c) 2016-2023 Viktor Kirilov

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

---

## ONNX Runtime

```
MIT License

Copyright (c) Microsoft Corporation

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```
