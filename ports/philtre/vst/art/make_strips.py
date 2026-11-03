#!/usr/bin/env python3
"""Draws the filmstrips of Philtre's curve displays: one display column, DISP_LEVELS frames of W x H stacked down, a bar
filled from the baseline to the frame's level with a bright cap. env_strip.png fills up from the bottom (0..1), lfo_strip.png
up or down from the middle (-1..1). Each display is DISP_COLS of these side by side (`meter` lines in vst/layout.conf).

    python3 vst/art/make_strips.py      (from ports/philtre; needs Pillow; commit the PNGs)
"""
import os
import sys

from PIL import Image, ImageDraw

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))
import make_params as mp  # noqa: E402

W, H = 11, 440                      # one column; make_layout.py places DISP_COLS of them, W apart
BG, GRID, MID = (17, 15, 22), (30, 27, 38), (52, 46, 66)
LOW, HIGH, CAP = (58, 45, 85), (180, 140, 255), (224, 210, 255)
N = mp.DISP_LEVELS


def mix(a, b, t):
    return tuple(round(x + (y - x) * t) for x, y in zip(a, b))


def column(level, bipolar):
    img = Image.new("RGB", (W, H), BG)
    d = ImageDraw.Draw(img)
    top, bot = 4, H - 5
    for g in (0.25, 0.5, 0.75):   # faint grid: across the columns it reads as lines
        y = round(top + (bot - top) * g)
        d.line([(0, y), (W - 2, y)], fill=MID if bipolar and g == 0.5 else GRID)
    if bipolar:
        base = round((top + bot) / 2)
        y = round(bot - (bot - top) * (level + 1) / 2)
    else:
        base = bot
        y = round(bot - (bot - top) * level)
    y0, y1 = min(y, base), max(y, base)
    for yy in range(y0, y1 + 1):   # the fill, brighter towards the level
        t = 1 - abs(yy - y) / max(1, abs(base - y))
        d.line([(0, yy), (W - 2, yy)], fill=mix(LOW, HIGH, 0.25 + 0.6 * t))
    d.rectangle([0, y - 1, W - 2, y + 1], fill=CAP)   # the cap: the curve itself
    return img


def strip(name, bipolar):
    out = Image.new("RGB", (W, H * N), BG)
    for k in range(N):
        lv = k / (N - 1)
        out.paste(column(lv * 2 - 1 if bipolar else lv, bipolar), (0, H * k))
    out.save(os.path.join(HERE, name))
    print(name, out.size)


if __name__ == "__main__":
    strip("env_strip.png", False)
    strip("lfo_strip.png", True)
