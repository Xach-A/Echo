"""Makes the soundboard's built-in sounds (sounds/*.wav) from the downloaded
originals listed in sounds/CREDITS.txt: trims silence, cuts long sounds to
length with a fade, removes DC, sets the level and writes 48 kHz 16-bit WAV.

Usage: python tools/prepare_sounds.py <folder of decoded originals> <out folder>

The originals must first be decoded to 48 kHz WAV named as in SOUNDS below
(e.g. `echo_hwtest decode bsb_1831.mp3 bsb_1831.wav`; Ogg Vorbis files need
an Ogg decoder such as stb_vorbis, Windows has none). Needs numpy.
"""
import os
import struct
import sys

import numpy as np

RATE = 48000

# id: source file, then optional settings:
#   start/end  seconds to keep (default: trimmed to where the sound is)
#   max        longest length in seconds (cut with a fade-out)
#   fade       fade-out length in seconds at the end
#   fadein     fade-in length in seconds (default 2 ms)
#   level      loudest 100 ms RMS in dBFS (default -16); the peak stays at -2.5 dBFS or
#              below (sharp hits can gain ~1 dB when resampled to 44.1 kHz)
#   drive      saturation (tanh) amount; turns a hum into a buzz
#   lowpass    low-pass filter cutoff in Hz (after the saturation)
SOUNDS = {
    'airhorn': dict(src='bsb_1048', max=3.6, fade=0.1),
    'clown_horn': dict(src='bsb_1831', max=1.2, fade=0.05),
    'rimshot': dict(src='bsb_0433', max=3.0, fade=0.6),
    'sad_trombone': dict(src='commons_sad_trombone', max=5.0, fade=0.3),
    'tada': dict(src='oga_fanfare_lick', start=0.99, fadein=0.006, fade=0.3),  # its last two notes
    'ding': dict(src='bsb_0479', max=2.5, fade=0.8),
    'buzzer': dict(src='bsb_0593', start=0.5, end=1.4, fadein=0.008, fade=0.03, drive=6.0, lowpass=7000, level=-18),
    'drumroll': dict(src='bsb_2406', max=3.5, fade=0.3),
    'applause': dict(src='bsb_2363', max=6.5, fade=1.0, level=-18),
    'boing': dict(src='bsb_2278', max=2.0, fade=0.3),
    'whoosh': dict(src='bsb_1795', max=1.0, fade=0.1),
    'laser': dict(src='laserRetro_004', fade=0.01),
    'explosion': dict(src='oga_chunky', max=5.0, fade=1.5),
    'gunshot': dict(src='bsb_1023', max=4.0, fade=1.0),
    'boom': dict(src='bsb_2338', max=1.6, fade=0.5),
    'crickets': dict(src='bsb_1020', start=1.0, end=6.0, fade=0.8, fadein=0.3, level=-24),
}


def read_wav(path):
    data = open(path, 'rb').read()
    if data[:4] != b'RIFF' or data[8:12] != b'WAVE':
        raise ValueError(path + ': not a WAV file')
    pos, fmt, raw = 12, None, None
    while pos + 8 <= len(data):
        cid, size = data[pos:pos + 4], struct.unpack('<I', data[pos + 4:pos + 8])[0]
        body = data[pos + 8:pos + 8 + size]
        if cid == b'fmt ':
            fmt = struct.unpack('<HHIIHH', body[:16])
        elif cid == b'data':
            raw = body
        pos += 8 + size + (size & 1)
    tag, ch, rate, _, _, bits = fmt
    if bits == 32 and tag in (3, 0xFFFE):
        x = np.frombuffer(raw, '<f4')
    elif bits == 16:
        x = np.frombuffer(raw, '<i2').astype(np.float32) / 32768.0
    else:
        raise ValueError(path + ': use 32-bit float or 16-bit PCM')
    if rate != RATE:
        raise ValueError(path + ': decode to 48 kHz first')
    return x.reshape(-1, ch).astype(np.float64)


def write_wav16(path, x):
    frames, ch = x.shape
    rng = np.random.default_rng(1)
    dither = (rng.random(x.shape) - rng.random(x.shape)) / 32768.0  # TPDF
    q = np.clip(np.round((x + dither) * 32767.0), -32768, 32767).astype('<i2')
    raw = q.tobytes()
    hdr = b'RIFF' + struct.pack('<I', 36 + len(raw)) + b'WAVE'
    hdr += b'fmt ' + struct.pack('<IHHIIHH', 16, 1, ch, RATE, RATE * ch * 2, ch * 2, 16)
    hdr += b'data' + struct.pack('<I', len(raw))
    open(path, 'wb').write(hdr + raw)


def envelope_db(x, hop):
    mono = x.mean(axis=1)
    n = len(mono) // hop
    e = np.sqrt(np.mean(mono[:n * hop].reshape(n, hop) ** 2, axis=1))
    return 20 * np.log10(e + 1e-12)


def prepare(x, s):
    # Where the sound is: from 45 dB below its loudest 10 ms to 50 dB below.
    hop = RATE // 100
    env = envelope_db(x, hop)
    top = env.max()
    on = np.where(env > top - 45)[0]
    first = max(0, on[0] * hop - RATE // 200)
    last = min(len(x), (np.where(env > top - 50)[0][-1] + 2) * hop)
    start = int(s['start'] * RATE) if 'start' in s else first
    end = int(s['end'] * RATE) if 'end' in s else last
    if 'max' in s:
        end = min(end, start + int(s['max'] * RATE))
    y = x[start:end].copy()

    if 'drive' in s:
        d = s['drive']
        y = np.tanh(d * y / np.abs(y).max()) / np.tanh(d)
    if 'lowpass' in s:
        # RBJ low-pass biquad, Q = 0.707.
        w = 2 * np.pi * s['lowpass'] / RATE
        alpha = np.sin(w) / (2 * 0.7071)
        b0, b1, b2 = (1 - np.cos(w)) / 2, 1 - np.cos(w), (1 - np.cos(w)) / 2
        a0, a1, a2 = 1 + alpha, -2 * np.cos(w), 1 - alpha
        for c in range(y.shape[1]):
            x1 = x2 = y1 = y2 = 0.0
            col = y[:, c]
            for i in range(len(col)):
                v = col[i]
                out = (b0 * v + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2) / a0
                x2, x1, y2, y1 = x1, v, y1, out
                col[i] = out

    # DC blocker (20 Hz), as for the user's own sounds.
    a = 1.0 - 2 * np.pi * 20.0 / RATE
    for c in range(y.shape[1]):
        x1 = y1 = 0.0
        col = y[:, c]
        for i in range(len(col)):
            v = col[i]
            y1 = v - x1 + a * y1
            x1 = v
            col[i] = y1

    # Fades: a short one at the start, the chosen one at the end.
    fi = min(len(y), max(1, int(s.get('fadein', 0.002) * RATE)))
    fo = min(len(y), max(1, int(s.get('fade', 0.01) * RATE)))
    y[:fi] *= np.linspace(0.0, 1.0, fi)[:, None]
    y[-fo:] *= (np.cos(np.linspace(0.0, np.pi, fo)) * 0.5 + 0.5)[:, None]

    # Level: loudest 100 ms RMS at the target, peak at -2.5 dBFS or below.
    win, step = RATE // 10, RATE // 40
    mono2 = (y ** 2).mean(axis=1)
    loudest = max(mono2[i:i + win].mean() for i in range(0, max(1, len(y) - win + 1), step))
    gain = 10 ** (s.get('level', -16) / 20) / np.sqrt(loudest)
    gain = min(gain, 10 ** (-2.5 / 20) / np.abs(y).max())
    return y * gain


def main():
    src_dir, out_dir = sys.argv[1], sys.argv[2]
    os.makedirs(out_dir, exist_ok=True)
    for sid, s in SOUNDS.items():
        x = read_wav(os.path.join(src_dir, s['src'] + '.wav'))
        y = prepare(x, s)
        write_wav16(os.path.join(out_dir, sid + '.wav'), y)
        peak = 20 * np.log10(np.abs(y).max())
        print(f'{sid:13s} {len(y) / RATE:5.2f} s  {y.shape[1]} ch  peak {peak:5.1f} dBFS')


if __name__ == '__main__':
    main()
