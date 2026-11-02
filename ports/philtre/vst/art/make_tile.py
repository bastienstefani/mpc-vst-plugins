#!/usr/bin/env python3
"""Draws vst/art/tile.png, the 270x110 tile of MPC's Sounds > INSTRUMENTS browser (vst.json "tile"):
a stack of wavetable frames morphing from a sine to a saw, and the name in Titillium Web (OFL, tools/html_art/fonts).
    python3 vst/art/make_tile.py      (from ports/osmium; needs Pillow; commit the PNG)"""
import math
import os

from PIL import Image, ImageDraw, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))
FONT = os.path.join(HERE, "..", "..", "..", "..", "tools", "html_art", "fonts", "TitilliumWeb-Bold.ttf")
W, H, S = 270, 110, 3   # drawn at 3x, then scaled down

img = Image.new("RGB", (W * S, H * S), (11, 14, 18))
d = ImageDraw.Draw(img)
for y in range(H * S):   # a dark vertical gradient
    t = y / (H * S)
    d.line([(0, y), (W * S, y)], fill=(int(11 + 14 * t), int(14 + 18 * t), int(18 + 24 * t)))
frames = 14
for f in range(frames):   # back to front: each frame a little to the right and higher, like a 3D wavetable view
    t = f / (frames - 1)
    x0, y0, fw, amp = (118 + f * 7) * S, (78 - f * 4) * S, 120 * S, 16 * S
    pts = []
    for i in range(121):
        p = i / 120
        saw = sum(math.sin(2 * math.pi * k * p) / k for k in range(1, 12)) * 0.6
        v = (1 - t) * math.sin(2 * math.pi * p) + t * saw
        pts.append((x0 + p * fw, y0 - v * amp))
    c = (int(70 + 110 * t), int(50 + 90 * t), int(110 + 145 * t))
    d.line(pts, fill=c, width=2 * S)
font = ImageFont.truetype(FONT, 30 * S)
d.text((14 * S, 30 * S), "PHILTRE", font=font, fill=(230, 235, 240))
small = ImageFont.truetype(FONT, 12 * S)
d.text((15 * S, 70 * S), "WAVETABLE SYNTH + FX RACK", font=small, fill=(180, 140, 255))
img.resize((W, H), Image.LANCZOS).save(os.path.join(HERE, "tile.png"))
print("tile.png")
