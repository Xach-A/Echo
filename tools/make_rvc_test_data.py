"""Builds the RVC test data (tests/data/rvc): a tiny RVC voice and a tiny
ContentVec with random weights, a small FAISS IVF-Flat index, inputs, and the
outputs of the official implementations, so Echo's tests can check its own
RVC graphs and pipeline exactly.

Needs PyTorch, transformers and Applio's source (rvc/lib/algorithm, MIT):
    python tools/make_rvc_test_data.py <applio source folder>
"""
import json
import os
import struct
import sys

import numpy as np
import torch
from transformers import HubertConfig, HubertModel

sys.path.insert(0, sys.argv[1])
from rvc.lib.algorithm.synthesizers import Synthesizer  # noqa: E402

out = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "tests", "data", "rvc")
voice_dir = os.path.join(out, "tiny-voice")
os.makedirs(voice_dir, exist_ok=True)
torch.manual_seed(1)
rng = np.random.default_rng(1)

# ------------------------------------------------------------ tiny ContentVec
cv_cfg = HubertConfig(hidden_size=128, num_hidden_layers=2, num_attention_heads=2, intermediate_size=64,
                      conv_dim=(32,) * 7, feat_extract_norm="group", do_stable_layer_norm=False,
                      num_conv_pos_embeddings=128, num_conv_pos_embedding_groups=16, feat_proj_layer_norm=True)
cv = HubertModel(cv_cfg).eval()
with torch.no_grad():
    for p in cv.parameters():
        p.add_(torch.randn_like(p) * 0.02)  # away from the default init (LayerNorm 1/0 etc.)
sd = {k: v.half() for k, v in cv.state_dict().items()}
torch.save(sd, os.path.join(out, "contentvec-tiny.bin"))
cv_half = HubertModel(cv_cfg).eval()
cv_half.load_state_dict({k: v.float() for k, v in sd.items()})
n16 = 8000
t = np.arange(n16) / 16000
audio = (0.3 * np.sin(2 * np.pi * 180 * t) + 0.05 * rng.standard_normal(n16)).astype(np.float32)
audio.tofile(os.path.join(out, "cv_audio.bin"))
with torch.no_grad():
    feats = cv_half(torch.from_numpy(audio)[None])["last_hidden_state"][0].numpy()
feats.astype(np.float32).tofile(os.path.join(out, "cv_feats.bin"))

# ------------------------------------------------------------- tiny RVC voice
cfg = [513, 32, 16, 16, 32, 2, 2, 3, 0, "1", [3, 5], [[1, 3], [1, 3]], [10, 8, 2, 2], 32, [20, 16, 4, 4], 2, 8, 32000]
net = Synthesizer(*cfg, use_f0=True, text_enc_hidden_dim=128, vocoder="HiFi-GAN").eval()
with torch.no_grad():
    for p in net.parameters():
        p.add_(torch.randn_like(p) * 0.05)
weights = {k: v.half() for k, v in net.state_dict().items() if not k.startswith("enc_q")}
torch.save({"weight": weights, "config": cfg, "sr": 32000, "f0": 1, "version": "v2", "vocoder": "HiFi-GAN",
            "embedder_model": "contentvec", "model_name": "Tiny test voice"},
           os.path.join(voice_dir, "tiny.pth"))
ref = Synthesizer(*cfg, use_f0=True, text_enc_hidden_dim=128, vocoder="HiFi-GAN").eval()
ref.load_state_dict({k: v.float() for k, v in weights.items()}, strict=False)
ref.remove_weight_norm()
T, hop = 40, 320
phone = rng.standard_normal((1, T, 128)).astype(np.float32)
f0 = np.linspace(110, 240, T).astype(np.float32)
f0[10:14] = 0
mel = 1127 * np.log(1 + f0 / 700)
mmin, mmax = 1127 * np.log(1 + 50 / 700), 1127 * np.log(1 + 1100 / 700)
mel[mel > 0] = (mel[mel > 0] - mmin) * 254 / (mmax - mmin) + 1
mel = np.clip(mel, 1, 255)
pitch = np.rint(mel).astype(np.int64)[None]
rnd = rng.standard_normal((1, 16, T)).astype(np.float32)
noise = rng.standard_normal((1, T * hop)).astype(np.float32)
for name, arr in [("gen_phone", phone), ("gen_pitch", pitch), ("gen_pitchf", f0[None]), ("gen_rnd", rnd), ("gen_noise", noise)]:
    arr.tofile(os.path.join(out, name + ".bin"))
real_randn_like = torch.randn_like


def fake_randn_like(x, *a, **k):
    if tuple(x.shape) == (1, 16, T):
        return torch.from_numpy(rnd)
    if tuple(x.shape) == (1, T * hop, 1):
        return torch.from_numpy(noise).reshape(1, T * hop, 1)
    raise RuntimeError(f"unexpected randn_like {tuple(x.shape)}")


torch.randn_like = fake_randn_like
with torch.no_grad():
    audio_out = ref.infer(torch.from_numpy(phone), torch.tensor([T]), torch.from_numpy(pitch), torch.from_numpy(f0[None]),
                          torch.tensor([0]))[0][0, 0].numpy()
torch.randn_like = real_randn_like
audio_out.astype(np.float32).tofile(os.path.join(out, "gen_audio.bin"))

# ---------------------------------------------------- tiny FAISS IVF-Flat index
# Written in FAISS's own file layout (IndexIVFFlat, write_index), 128 dims.
d, nlist, per = 128, 4, 25
cent = rng.standard_normal((nlist, d)).astype(np.float32)
lists = [(cent[i] + 0.3 * rng.standard_normal((per, d))).astype(np.float32) for i in range(nlist)]
with open(os.path.join(voice_dir, "tiny.index"), "wb") as f:
    def header(n):
        f.write(struct.pack("<iqqqBi", d, n, 1 << 20, 1 << 20, 1, 1))
    f.write(b"IwFl")
    header(nlist * per)
    f.write(struct.pack("<QQ", nlist, 1))
    f.write(b"IxF2")
    header(nlist)
    f.write(struct.pack("<Q", nlist * d))
    f.write(cent.tobytes())
    f.write(struct.pack("<BQ", 0, 0))  # no direct map
    f.write(b"ilar")
    f.write(struct.pack("<QQ", nlist, d * 4))
    f.write(b"full")
    f.write(struct.pack("<Q", nlist))
    f.write(struct.pack(f"<{nlist}Q", *([per] * nlist)))
    ids = 0
    for i in range(nlist):
        f.write(lists[i].tobytes())
        f.write(struct.pack(f"<{per}q", *range(ids, ids + per)))
        ids += per
# Expected retrieval (RVC: nearest cluster, 8 nearest, weights 1/d^2).
queries = (cent[[0, 2]] + 0.2 * rng.standard_normal((2, d))).astype(np.float32)
queries.tofile(os.path.join(out, "index_queries.bin"))
expected = []
for q in queries:
    c = int(np.argmin(((cent - q) ** 2).sum(1)))
    dist = ((lists[c] - q) ** 2).sum(1)
    near = np.argsort(dist)[:8]
    w = 1 / dist[near] ** 2
    expected.append((lists[c][near] * (w / w.sum())[:, None]).sum(0))
np.array(expected, np.float32).tofile(os.path.join(out, "index_expected.bin"))

json.dump({"note": "Generated by tools/make_rvc_test_data.py; random weights, not a voice."},
          open(os.path.join(out, "README.json"), "w"), indent=2)
print("written to", os.path.abspath(out))
