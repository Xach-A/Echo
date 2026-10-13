"""Generates Echo's icon (no third-party assets): resources/echo.svg (the
artwork), resources/echo.ico (16-256 px) and resources/echo-256.png.

The design: a dark rounded tile with a soft rim, and a studio microphone with
a green capsule head, a dark body and stem, held in a green U-shaped holder.
Every size is drawn from the shapes below (signed distances, 4x4
supersampling); 24 px and smaller use a simpler drawing that stays readable
(no shadow or sheen, thicker holder, lighter body, tile edge to edge).

Usage:  python tools/make_icon.py        (needs numpy)
"""
import os
import struct
import zlib

import numpy as np

SIZES = [16, 20, 24, 32, 40, 48, 64, 128, 256]

# Colours (sRGB 0-255).
TILE_TOP, TILE_BOTTOM = (44, 49, 57), (19, 21, 26)
RIM_TOP, RIM_BOTTOM = (78, 84, 94), (34, 38, 45)
GREEN_TOP, GREEN_BOTTOM = (118, 255, 108), (44, 214, 58)
GREEN_BAND = (34, 176, 48)
HOLDER_TOP, HOLDER_BOTTOM = (82, 240, 84), (40, 196, 56)
BODY_LEFT, BODY_RIGHT = (72, 79, 90), (24, 27, 32)
COLLAR, COLLAR_EDGE = (12, 13, 16), (58, 63, 71)
STEM = (24, 26, 31)


def geometry(simple):
    """Shapes in tile units (0..1, y down)."""
    g = dict(
        tile_radius=0.22, rim=0.016,
        head=(0.32, 0.68, 0.12, 0.42),          # x0, x1, top, bottom (dome radius = half width)
        band=0.035,                             # darker band at the bottom of the head
        collar=(0.305, 0.695, 0.42, 0.458, 0.012),
        body=(0.33, 0.67, 0.458, 0.745, 0.17),  # x0, x1, top, bottom, bottom corner radius
        stem=(0.465, 0.535, 0.70, 0.905),
        stem_collar=(0.445, 0.555, 0.845, 0.885),
        holder=(0.205, 0.795, 0.445, 0.585, 0.036),  # left x, right x, top, arc centre y, width
        knob=(0.072, 0.392, 0.478),              # width, top, bottom
    )
    if simple:
        g.update(
            tile_radius=0.2, rim=0.0,
            head=(0.30, 0.70, 0.10, 0.43), band=0.0,
            collar=(0.29, 0.71, 0.43, 0.475, 0.02),
            body=(0.32, 0.68, 0.475, 0.76, 0.18),
            stem=(0.44, 0.56, 0.72, 0.93), stem_collar=None,
            holder=(0.19, 0.81, 0.43, 0.56, 0.085),
            knob=None,
        )
    return g


# ---------------------------------------------------------------- distances
def sd_round_box(x, y, x0, x1, y0, y1, r_top, r_bottom):
    cx, cy, hw, hh = (x0 + x1) / 2, (y0 + y1) / 2, (x1 - x0) / 2, (y1 - y0) / 2
    r = np.where(y > cy, r_bottom, r_top)
    qx = np.abs(x - cx) - hw + r
    qy = np.abs(y - cy) - hh + r
    return np.hypot(np.maximum(qx, 0), np.maximum(qy, 0)) + np.minimum(np.maximum(qx, qy), 0) - r


def sd_dome(x, y, x0, x1, top, bottom):
    """A rectangle with a half-circle top."""
    r = (x1 - x0) / 2
    cx, cy = (x0 + x1) / 2, top + r
    d_circle = np.hypot(x - cx, y - cy) - r
    d_rect = sd_round_box(x, y, x0, x1, cy, bottom, 0.0, 0.0)
    return np.where(y < cy, np.maximum(d_circle, y - bottom), d_rect)


def sd_holder(x, y, xl, xr, top, arc_y, width):
    """A U: two vertical strokes joined by a half circle at the bottom."""
    cx, radius = (xl + xr) / 2, (xr - xl) / 2
    d_arc = np.abs(np.hypot(x - cx, y - arc_y) - radius)
    d_left = np.hypot(x - xl, np.maximum(np.maximum(top - y, y - arc_y), 0))
    d_right = np.hypot(x - xr, np.maximum(np.maximum(top - y, y - arc_y), 0))
    d = np.where(y > arc_y, d_arc, np.minimum(d_left, d_right))
    return d - width / 2


# ---------------------------------------------------------------- drawing
def render(size):
    simple = size <= 24
    g = geometry(simple)
    ss = 4
    n = size * ss
    coords = (np.arange(n) + 0.5) / n
    u, v = np.meshgrid(coords, coords)
    pad = 0.0 if simple else (0.03 if size <= 48 else 0.04)
    x = (u - pad) / (1 - 2 * pad)  # tile units
    y = (v - pad) / (1 - 2 * pad)
    # Strokes stay at least 1.6 px wide; mid sizes get a lighter body so it
    # stands out from the tile.
    px = 1.0 / (size * (1 - 2 * pad))
    xl_, xr_, ytop_, arcy_, hw_ = g['holder']
    g['holder'] = (xl_, xr_, ytop_, arcy_, max(hw_, 1.6 * px))
    mid = not simple and size <= 64
    img = np.zeros((n, n, 4))  # premultiplied RGBA

    def over(mask, colour, alpha=1.0):
        a = mask.astype(float) * alpha
        c = np.asarray(colour, dtype=float) / 255.0
        if c.ndim == 1:
            c = np.broadcast_to(c, (n, n, 3))
        img[..., :3] = c * a[..., None] + img[..., :3] * (1 - a[..., None])
        img[..., 3] = a + img[..., 3] * (1 - a)

    def vgrad(c0, c1, t0, t1):
        t = np.clip((y - t0) / (t1 - t0), 0, 1)[..., None]
        return np.asarray(c0) * (1 - t) + np.asarray(c1) * t

    def hgrad(c0, c1, s0, s1):
        t = np.clip((x - s0) / (s1 - s0), 0, 1)[..., None]
        return np.asarray(c0) * (1 - t) + np.asarray(c1) * t

    # Tile and rim.
    d_tile = sd_round_box(x, y, 0, 1, 0, 1, g['tile_radius'], g['tile_radius'])
    over(d_tile < 0, vgrad(TILE_TOP, TILE_BOTTOM, 0, 1))
    if g['rim'] > 0:
        over((d_tile < 0) & (d_tile > -g['rim']), vgrad(RIM_TOP, RIM_BOTTOM, 0, 1))
    inside = d_tile < 0

    hx0, hx1, htop, hbot = g['head']
    bx0, bx1, btop, bbot, brad = g['body']
    sx0, sx1, stop, sbot = g['stem']
    xl, xr, ytop, arc_y, hw = g['holder']
    cx0, cx1, cy0, cy1, crad = g['collar']

    def mic_silhouette(dy):
        yy = y - dy
        d = sd_dome(x, yy, hx0, hx1, htop, hbot)
        d = np.minimum(d, sd_round_box(x, yy, cx0, cx1, cy0, cy1, crad, crad))
        d = np.minimum(d, sd_round_box(x, yy, bx0, bx1, btop, bbot, 0.0, brad))
        d = np.minimum(d, sd_round_box(x, yy, sx0, sx1, stop, sbot, 0.0, 0.02))
        d = np.minimum(d, sd_holder(x, yy, xl, xr, ytop, arc_y, hw))
        return d

    # Soft shadow under the microphone.
    if not simple:
        d = mic_silhouette(0.022)
        shadow = np.clip(1 - (d + 0.01) / 0.05, 0, 1) ** 2
        over(inside, (0, 0, 0), shadow * 0.45)

    # Holder (behind the body and stem).
    holder = sd_holder(x, y, xl, xr, ytop, arc_y, hw) < 0
    over(holder & inside, vgrad(HOLDER_TOP, HOLDER_BOTTOM, ytop, arc_y + (xr - xl) / 2))
    if g['knob']:
        kw, k0, k1 = g['knob']
        for kx in (xl, xr):
            knob = sd_round_box(x, y, kx - kw / 2, kx + kw / 2, k0, k1, kw / 2, kw / 2) < 0
            over(knob, vgrad(GREEN_TOP, HOLDER_TOP, k0, k1))

    # Stem and its collar.
    stem = sd_round_box(x, y, sx0, sx1, stop, sbot, 0.0, 0.02) < 0
    over(stem, hgrad(BODY_LEFT, STEM, sx0, sx1))
    if g['stem_collar']:
        a0, a1, b0, b1 = g['stem_collar']
        over(sd_round_box(x, y, a0, a1, b0, b1, 0.01, 0.01) < 0, COLLAR)
        over(sd_round_box(x, y, a0, a1, b0, b0 + 0.008, 0.004, 0.0) < 0, COLLAR_EDGE)

    # Body: dark, lit from the left, with a soft vertical sheen.
    body_d = sd_round_box(x, y, bx0, bx1, btop, bbot, 0.0, brad)
    body = body_d < 0
    left, right = BODY_LEFT, BODY_RIGHT
    if simple:
        left, right = (96, 103, 114), (52, 57, 65)
    elif mid:
        left, right = (88, 95, 107), (36, 40, 47)
    over(body, hgrad(left, right, bx0, bx1))
    if not simple:
        sheen = np.exp(-((x - (bx0 + 0.07)) / 0.025) ** 2) * np.clip((bbot - 0.05 - y) / 0.1, 0, 1)
        over(body, (255, 255, 255), sheen * 0.10)
        over(body & (body_d > -max(0.008, px)), (104, 111, 123), 0.6)  # edge light

    # Collar.
    over(sd_round_box(x, y, cx0, cx1, cy0, cy1, crad, crad) < 0, COLLAR)
    if not simple:
        over(sd_round_box(x, y, cx0, cx1, cy0, cy0 + 0.008, crad, 0.0) < 0, COLLAR_EDGE)

    # Head: green dome, a darker band at the bottom and a highlight.
    head_d = sd_dome(x, y, hx0, hx1, htop, hbot)
    head = head_d < 0
    over(head, vgrad(GREEN_TOP, GREEN_BOTTOM, htop, hbot))
    if g['band'] > 0:
        over(head & (y > hbot - g['band']), GREEN_BAND)
        over(head & (np.abs(y - (hbot - g['band'])) < 0.004), GREEN_TOP, 0.7)
        hl = np.exp(-(((x - (hx0 + 0.11)) / 0.05) ** 2 + ((y - (htop + 0.09)) / 0.06) ** 2))
        over(head, (255, 255, 255), hl * 0.35)

    # Downsample (premultiplied average) and un-premultiply.
    img = img.reshape(size, ss, size, ss, 4).mean(axis=(1, 3))
    a = img[..., 3:4]
    rgb = np.where(a > 0, img[..., :3] / np.maximum(a, 1e-9), 0)
    return np.concatenate([rgb, a], axis=2)


def png(px):
    height, width = px.shape[:2]
    data = np.clip(np.rint(px * 255), 0, 255).astype(np.uint8)
    raw = b''.join(b'\x00' + data[r].tobytes() for r in range(height))

    def chunk(tag, body):
        c = struct.pack('>I', len(body)) + tag + body
        return c + struct.pack('>I', zlib.crc32(tag + body) & 0xFFFFFFFF)

    ihdr = struct.pack('>IIBBBBB', width, height, 8, 6, 0, 0, 0)
    return b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', ihdr) + chunk(b'IDAT', zlib.compress(raw, 9)) + chunk(b'IEND', b'')


# ---------------------------------------------------------------- SVG
def svg():
    """The large-size drawing as SVG (256 x 256), for documentation and the
    installer artwork."""
    g = geometry(False)
    pad = 0.04
    s = 256 * (1 - 2 * pad)
    o = 256 * pad

    def X(t):
        return round(o + t * s, 2)

    def L(t):
        return round(t * s, 2)

    def rgb(c):
        return '#%02x%02x%02x' % c

    hx0, hx1, htop, hbot = g['head']
    bx0, bx1, btop, bbot, brad = g['body']
    sx0, sx1, stop, sbot = g['stem']
    xl, xr, ytop, arc_y, hw = g['holder']
    cx0, cx1, cy0, cy1, crad = g['collar']
    kw, k0, k1 = g['knob']
    a0, a1, b0, b1 = g['stem_collar']
    hr = (hx1 - hx0) / 2
    R = (xr - xl) / 2
    head = (f'M{X(hx0)} {X(hbot)}V{X(htop + hr)}A{L(hr)} {L(hr)} 0 0 1 {X(hx1)} {X(htop + hr)}'
            f'V{X(hbot)}Z')
    body = (f'M{X(bx0)} {X(btop)}H{X(bx1)}V{X(bbot - brad)}A{L(brad)} {L(brad)} 0 0 1 {X(bx1 - brad)} {X(bbot)}'
            f'H{X(bx0 + brad)}A{L(brad)} {L(brad)} 0 0 1 {X(bx0)} {X(bbot - brad)}Z')
    holder = f'M{X(xl)} {X(ytop)}V{X(arc_y)}A{L(R)} {L(R)} 0 0 0 {X(xr)} {X(arc_y)}V{X(ytop)}'
    mic = (f'<path d="{head}"/><rect x="{X(cx0)}" y="{X(cy0)}" width="{L(cx1 - cx0)}" height="{L(cy1 - cy0)}" rx="{L(crad)}"/>'
           f'<path d="{body}"/><rect x="{X(sx0)}" y="{X(stop)}" width="{L(sx1 - sx0)}" height="{L(sbot - stop)}"/>'
           f'<path d="{holder}" fill="none" stroke="#000" stroke-width="{L(hw)}"/>')
    return f'''<svg xmlns="http://www.w3.org/2000/svg" width="256" height="256" viewBox="0 0 256 256">
  <title>Echo</title>
  <defs>
    <linearGradient id="tile" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="{rgb(TILE_TOP)}"/><stop offset="1" stop-color="{rgb(TILE_BOTTOM)}"/></linearGradient>
    <linearGradient id="rim" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="{rgb(RIM_TOP)}"/><stop offset="1" stop-color="{rgb(RIM_BOTTOM)}"/></linearGradient>
    <linearGradient id="head" x1="0" y1="{X(htop)}" x2="0" y2="{X(hbot)}" gradientUnits="userSpaceOnUse"><stop offset="0" stop-color="{rgb(GREEN_TOP)}"/><stop offset="1" stop-color="{rgb(GREEN_BOTTOM)}"/></linearGradient>
    <linearGradient id="holder" x1="0" y1="{X(ytop)}" x2="0" y2="{X(arc_y + R)}" gradientUnits="userSpaceOnUse"><stop offset="0" stop-color="{rgb(HOLDER_TOP)}"/><stop offset="1" stop-color="{rgb(HOLDER_BOTTOM)}"/></linearGradient>
    <linearGradient id="body" x1="{X(bx0)}" y1="0" x2="{X(bx1)}" y2="0" gradientUnits="userSpaceOnUse"><stop offset="0" stop-color="{rgb(BODY_LEFT)}"/><stop offset="1" stop-color="{rgb(BODY_RIGHT)}"/></linearGradient>
    <radialGradient id="shine" cx="{X(hx0 + 0.11)}" cy="{X(htop + 0.09)}" r="{L(0.07)}" gradientUnits="userSpaceOnUse"><stop offset="0" stop-color="#fff" stop-opacity="0.35"/><stop offset="1" stop-color="#fff" stop-opacity="0"/></radialGradient>
    <filter id="shadow" x="-20%" y="-20%" width="140%" height="140%"><feGaussianBlur stdDeviation="{L(0.02)}"/></filter>
    <clipPath id="headclip"><path d="{head}"/></clipPath>
  </defs>
  <rect x="{X(0)}" y="{X(0)}" width="{L(1)}" height="{L(1)}" rx="{L(g['tile_radius'])}" fill="url(#rim)"/>
  <rect x="{X(g['rim'])}" y="{X(g['rim'])}" width="{L(1 - 2 * g['rim'])}" height="{L(1 - 2 * g['rim'])}" rx="{L(g['tile_radius'] - g['rim'])}" fill="url(#tile)"/>
  <g fill="#000" opacity="0.45" filter="url(#shadow)" transform="translate(0 {L(0.022)})">{mic}</g>
  <path d="{holder}" fill="none" stroke="url(#holder)" stroke-width="{L(hw)}"/>
  <rect x="{X(xl - kw / 2)}" y="{X(k0)}" width="{L(kw)}" height="{L(k1 - k0)}" rx="{L(kw / 2)}" fill="{rgb(HOLDER_TOP)}"/>
  <rect x="{X(xr - kw / 2)}" y="{X(k0)}" width="{L(kw)}" height="{L(k1 - k0)}" rx="{L(kw / 2)}" fill="{rgb(HOLDER_TOP)}"/>
  <rect x="{X(sx0)}" y="{X(stop)}" width="{L(sx1 - sx0)}" height="{L(sbot - stop)}" rx="{L(0.02)}" fill="{rgb(STEM)}"/>
  <rect x="{X(a0)}" y="{X(b0)}" width="{L(a1 - a0)}" height="{L(b1 - b0)}" rx="{L(0.01)}" fill="{rgb(COLLAR)}"/>
  <path d="{body}" fill="url(#body)"/>
  <rect x="{X(cx0)}" y="{X(cy0)}" width="{L(cx1 - cx0)}" height="{L(cy1 - cy0)}" rx="{L(crad)}" fill="{rgb(COLLAR)}"/>
  <path d="{head}" fill="url(#head)"/>
  <g clip-path="url(#headclip)">
    <rect x="{X(hx0)}" y="{X(hbot - g['band'])}" width="{L(hx1 - hx0)}" height="{L(g['band'])}" fill="{rgb(GREEN_BAND)}"/>
    <circle cx="{X(hx0 + 0.11)}" cy="{X(htop + 0.09)}" r="{L(0.07)}" fill="url(#shine)"/>
  </g>
</svg>
'''


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    res = os.path.join(here, '..', 'resources')
    images = [png(render(s)) for s in SIZES]
    header = struct.pack('<HHH', 0, 1, len(images))
    entries = b''
    offset = 6 + 16 * len(images)
    for s, data in zip(SIZES, images):
        dim = 0 if s >= 256 else s
        entries += struct.pack('<BBBBHHII', dim, dim, 0, 0, 1, 32, len(data), offset)
        offset += len(data)
    with open(os.path.join(res, 'echo.ico'), 'wb') as f:
        f.write(header + entries + b''.join(images))
    with open(os.path.join(res, 'echo-256.png'), 'wb') as f:
        f.write(images[-1])
    with open(os.path.join(res, 'echo.svg'), 'w', encoding='utf-8', newline='\n') as f:
        f.write(svg())
    print('wrote', os.path.normpath(os.path.join(res, 'echo.ico')), 'echo-256.png and echo.svg')


if __name__ == '__main__':
    main()
