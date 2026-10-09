"""Generates resources/echo.ico (no third-party assets).

The icon is drawn procedurally: a rounded square with a blue-violet gradient
and white concentric "echo" arcs around a microphone dot. Each size is
rendered with 4x4 supersampling and stored as a PNG inside the ICO.

Usage:  python tools/make_icon.py
"""
import math
import os
import struct
import zlib

SIZES = [16, 20, 24, 32, 40, 48, 64, 128, 256]


def lerp(a, b, t):
    return a + (b - a) * t


def render(size):
    ss = 4
    n = size * ss
    px = [[(0.0, 0.0, 0.0, 0.0) for _ in range(size)] for _ in range(size)]
    radius = 0.22  # corner radius (fraction of size)
    c1 = (0.18, 0.45, 1.0)   # blue
    c2 = (0.58, 0.32, 0.98)  # violet
    cx, cy = 0.40, 0.5
    for y in range(size):
        for x in range(size):
            acc = [0.0, 0.0, 0.0, 0.0]
            for sy in range(ss):
                for sx in range(ss):
                    u = (x * ss + sx + 0.5) / n
                    v = (y * ss + sy + 0.5) / n
                    # rounded square mask
                    dx = max(abs(u - 0.5) - (0.5 - radius), 0.0)
                    dy = max(abs(v - 0.5) - (0.5 - radius), 0.0)
                    if dx * dx + dy * dy > radius * radius:
                        continue
                    t = (u + v) * 0.5
                    col = [lerp(c1[i], c2[i], t) for i in range(3)]
                    # white elements: dot + three arcs to the right
                    d = math.hypot(u - cx, v - cy)
                    ang = math.degrees(math.atan2(v - cy, u - cx))
                    white = 0.0
                    if d < 0.085:
                        white = 1.0
                    for k, r in enumerate((0.19, 0.29, 0.39)):
                        if abs(d - r) < 0.035 and abs(ang) < 52:
                            white = max(white, 1.0 - 0.18 * k)
                    col = [lerp(col[i], 1.0, white) for i in range(3)]
                    acc[0] += col[0]
                    acc[1] += col[1]
                    acc[2] += col[2]
                    acc[3] += 1.0
            cnt = ss * ss
            a = acc[3] / cnt
            if a > 0:
                px[y][x] = (acc[0] / acc[3], acc[1] / acc[3], acc[2] / acc[3], a)
    return px


def png(size, px):
    raw = bytearray()
    for row in px:
        raw.append(0)
        for r, g, b, a in row:
            raw += bytes((int(r * 255 + 0.5), int(g * 255 + 0.5), int(b * 255 + 0.5), int(a * 255 + 0.5)))

    def chunk(tag, data):
        c = struct.pack(">I", len(data)) + tag + data
        return c + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)

    ihdr = struct.pack(">IIBBBBB", size, size, 8, 6, 0, 0, 0)
    return b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", ihdr) + chunk(b"IDAT", zlib.compress(bytes(raw), 9)) + chunk(b"IEND", b"")


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    out = os.path.join(here, "..", "resources", "echo.ico")
    images = [png(s, render(s)) for s in SIZES]
    header = struct.pack("<HHH", 0, 1, len(images))
    entries = b""
    offset = 6 + 16 * len(images)
    for s, data in zip(SIZES, images):
        dim = 0 if s >= 256 else s
        entries += struct.pack("<BBBBHHII", dim, dim, 0, 0, 1, 32, len(data), offset)
        offset += len(data)
    with open(out, "wb") as f:
        f.write(header + entries + b"".join(images))
    # Also write a 256 px PNG for documentation/installer use.
    with open(os.path.join(here, "..", "resources", "echo-256.png"), "wb") as f:
        f.write(images[-1])
    print("wrote", os.path.normpath(out))


if __name__ == "__main__":
    main()
