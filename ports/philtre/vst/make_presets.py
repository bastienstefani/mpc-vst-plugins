#!/usr/bin/env python3
"""Philtre's factory presets -> vst/presets.json (MPC's PRESET menu). Original sounds made for this engine.

    python3 vst/make_presets.py        (from ports/philtre, after make_params.py; commit the output)

Every preset sets every parameter (the defaults of make_params.py, then its own changes), so a pick always gives the
same sound. The first preset is the default sound. Values are in each parameter's own units, or an option's label.
"""
import json
import os

import make_params as mp

HERE = os.path.dirname(os.path.abspath(__file__))
DIV = ["1/32", "1/16T", "1/16", "1/16D", "1/8T", "1/8", "1/8D", "1/4T", "1/4", "1/4D", "1/2", "1/1"]
DIST = ["Tube", "Soft", "Hard", "Diode", "Fold", "Sine Fold", "Rectify", "Downsample", "Bitcrush"]


def div(name):   # an FX delay time knob position for a note division
    return round(100 * (DIV.index(name) + 0.5) / len(DIV), 1)


def dmode(name):
    return round(100 * (DIST.index(name) + 0.5) / len(DIST), 1)


def mods(*rows):
    """(source, aux, destination, amount) from slot 1; the rest cleared."""
    out = {}
    for n in range(1, 17):
        src, aux, dst, amt = rows[n - 1] if n <= len(rows) else ("Off", "Off", "Off", 0)
        out.update({"m%d_src" % n: src, "m%d_aux" % n: aux, "m%d_dst" % n: dst, "m%d_amt" % n: amt})
    return out


def rack(*slots):
    """(type, p1, p2, p3, mix) from slot 1; the rest Off."""
    out = {}
    for n in range(1, 9):
        t, a, b, c, m = slots[n - 1] if n <= len(slots) else ("Off", 50, 50, 50, 50)
        out.update({"fx%d_type" % n: t, "fx%d_p1" % n: a, "fx%d_p2" % n: b, "fx%d_p3" % n: c, "fx%d_mix" % n: m})
    return out


VIB = ("LFO 1", "Mod Whl", "Pitch", 3)
VIB_LFO = dict(l1_shape="Sine", l1_sync="Off", l1_rate=74)
D = lambda **k: k   # noqa: E731

PRESETS = [
    ("Init", D()),
    ("Seven Saw Lead", D(
        mode="Legato", glide=20, a_uni=7, a_det=45, a_blend=80, b_on="On", b_pos=40, b_oct=1, b_level=35, b_uni=3,
        b_det=30, f_on="On", f_cut=80, f_res=15, f_env=20, e2_d=60, e2_s=40, vel=40, **VIB_LFO,
        **mods(VIB, ("Velocity", "Off", "Cutoff", 15)),
        **rack(("Ensemble", 40, 50, 80, 35), ("Delay", div("1/8D"), 35, 40, 25), ("Reverb", 50, 40, 10, 20)))),
    ("Wide Glass Pad", D(
        a_table=3, a_pos=30, a_uni=5, a_det=30, b_on="On", b_table=2, b_pos=60, b_uni=3, b_level=50, f_on="On",
        f_type="LP 12", f_cut=65, f_res=10, e1_a=55, e1_d=70, e1_s=85, e1_r=65, l2_shape="Smooth", l2_sync="Off",
        l2_rate=30, l2_mode="Free", l3_sync="1/1", l3_mode="Free",
        **mods(("LFO 2", "Off", "A Pos", 25), ("LFO 3", "Off", "B Pos", 20), ("Mod Whl", "Off", "Cutoff", 20)),
        **rack(("Chorus", 30, 50, 40, 40), ("Reverb", 75, 45, 20, 35), ("EQ", 45, 50, 50, 55)))),
    ("Reese Bass", D(
        mode="Mono", a_uni=2, a_det=25, a_width=30, b_on="On", b_pos=40, b_fine=-12, b_uni=2, b_level=70, b_width=30,
        s_on="On", s_level=60, s_direct="Direct", f_on="On", f_type="MG LP 24", f_cut=45, f_res=25, f_drive=30,
        f_key=50, f_rs="Off",
        **mods(("Mod Whl", "Off", "Cutoff", 30)),
        **rack(("Distortion", 30, dmode("Tube"), 60, 50), ("Multiband", 50, 40, 50, 60), ("EQ", 55, 40, 45, 50)))),
    ("Growl Bass", D(
        mode="Mono", glide=8, a_table=5, a_pos=30, a_warp="FM", a_wamt=35, b_on="On", b_table=6, b_pos=20, b_level=0,
        s_on="On", s_level=50, s_direct="Direct", f_on="On", f_type="MG LP 24", f_cut=60, f_res=35, f_drive=40,
        l1_shape="Triangle", l1_sync="1/8",
        **mods(("LFO 1", "Off", "A Pos", 40), ("LFO 1", "Off", "A Warp", 50), ("LFO 1", "Off", "Cutoff", 30),
               ("Mod Whl", "Off", "B Pos", 60)),
        **rack(("Distortion", 45, dmode("Fold"), 70, 60), ("Multiband", 70, 35, 70, 80), ("EQ", 55, 50, 45, 55)))),
    ("Wobble Quarter", D(
        mode="Mono", a_uni=3, a_det=20, b_on="On", b_pos=60, b_oct=-1, b_level=50, s_on="On", s_level=55,
        s_direct="Direct", f_on="On", f_cut=35, f_res=45, f_drive=20, l1_shape="Sine", l1_sync="1/4",
        **mods(("LFO 1", "Off", "Cutoff", 45), ("Mod Whl", "Off", "LFO1 Rate", 25)),
        **rack(("Distortion", 30, dmode("Soft"), 65, 40), ("Multiband", 40, 40, 40, 50)))),
    ("Deep Sub", D(
        a_pos=0, a_level=30, s_on="On", s_level=85, s_direct="Direct", e1_d=70, e1_r=30, vel=30,
        **rack(("Distortion", 15, dmode("Soft"), 50, 20)))),
    ("Pluck Delay", D(
        a_table=8, a_pos=25, a_uni=3, a_det=15, f_on="On", f_cut=40, f_res=25, f_env=55, e1_d=55, e1_s=0, e1_r=45,
        e2_d=45, e2_r=40, vel=70,
        **mods(("Velocity", "Off", "Cutoff", 20)),
        **rack(("Ping Pong", div("1/8D"), 45, 40, 30), ("Plate", 40, 40, 5, 20)))),
    ("FM Bell Keys", D(
        a_table=5, a_pos=45, b_on="On", b_pos=0, b_oct=2, b_level=25, f_on="On", f_type="LP 12", f_cut=85,
        e1_d=68, e1_s=0, e1_r=62, e3_d=52, e3_s=0, e3_r=50, vel=80, **VIB_LFO,
        **mods(VIB, ("Env 3", "Off", "A Pos", -40), ("Velocity", "Off", "B Level", 30)),
        **rack(("Chorus", 30, 40, 30, 25), ("Plate", 60, 35, 10, 30)))),
    ("Formant Lead", D(
        mode="Legato", glide=18, a_pos=45, a_uni=3, a_det=25, f_on="On", f_type="Formant", f_cut=50, f_res=50,
        l2_shape="Triangle", l2_sync="1/2", l2_mode="Free", **VIB_LFO,
        **mods(VIB, ("LFO 2", "Off", "Cutoff", 25)),
        **rack(("Delay", div("1/4"), 30, 45, 20), ("Reverb", 45, 40, 15, 20)))),
    ("Comb Pluck", D(
        a_table=2, a_pos=80, n_on="On", n_level=60, f_on="On", f_type="Comb +", f_cut=60, f_res=85, f_key=100,
        e1_d=60, e1_s=0, e1_r=50, vel=70,
        **rack(("Reverb", 50, 50, 10, 25)))),
    ("Phase Pad", D(
        a_uni=5, a_det=35, b_on="On", b_table=7, b_pos=50, b_uni=3, b_level=55, f_on="On", f_type="Phaser",
        f_cut=55, f_res=60, l2_shape="Triangle", l2_sync="2/1", l2_mode="Free", e1_a=50, e1_d=60, e1_s=90,
        e1_r=60,
        **mods(("LFO 2", "Off", "Cutoff", 35)),
        **rack(("Chorus", 25, 45, 40, 30), ("Reverb", 70, 40, 20, 35)))),
    ("Pump Chords", D(
        a_uni=5, a_det=30, b_on="On", b_pos=60, b_uni=3, b_oct=-1, b_level=55, f_on="On", f_cut=70,
        l3_shape="Pump", l3_sync="1/4", l3_mode="Free",
        **mods(("LFO 3", "Off", "Amp", 60)),
        **rack(("Ensemble", 30, 40, 70, 30), ("Reverb", 50, 45, 10, 25)))),
    ("Noise Riser", D(
        vol=85, a_on="Off", n_on="On", n_level=100, f_on="On", f_type="BP 12", f_cut=45, f_res=35, l1_shape="Exp Up",
        l1_sync="4/1", l1_mode="Env", e1_a=60, e1_r=60,
        **mods(("LFO 1", "Off", "Cutoff", 70), ("LFO 1", "Off", "Noise Pitch", 40)),
        **rack(("Flanger", 30, 50, 40, 30), ("Reverb", 80, 30, 20, 40)))),
    ("Fold Arp", D(
        a_table=6, a_pos=20, f_on="On", f_cut=60, f_res=30, f_env=30, e1_d=50, e1_s=20, e1_r=35, e2_d=40,
        l1_shape="S&H", l1_sync="1/16",
        **mods(("LFO 1", "Off", "A Pos", 50)),
        **rack(("Ping Pong", div("1/16D"), 40, 50, 25), ("Reverb", 40, 50, 5, 15)))),
    ("Hoover Stab", D(
        glide=25, a_uni=7, a_det=70, a_blend=90, a_warp="PWM", a_wamt=30, b_on="On", b_pos=60, b_oct=-1, b_uni=5,
        b_det=60, b_level=55, e1_d=60, e1_s=70, e1_r=40, l1_sync="1/4",
        **mods(("LFO 1", "Off", "A Pos", 15), ("Mod Whl", "Off", "A Warp", 50)),
        **rack(("Ensemble", 40, 50, 80, 50), ("Distortion", 20, dmode("Soft"), 70, 30), ("Reverb", 45, 40, 10, 20)))),
]


def build():
    params = {p["key"]: p for p in mp.P}
    out = []
    for name, ch in PRESETS:
        for k in ch:
            if k not in params:
                raise SystemExit("preset %r: %r is not a parameter" % (name, k))
        vals = {}
        for p in mp.P:
            if p.get("momentary") or p.get("virtual"):   # stand-ins and displays follow the real parameters
                continue
            vals[p["key"]] = ch.get(p["key"], p["options"][p["default"]] if "options" in p else p["default"])
        out.append({"name": name, "values": vals})
    return {"presets": out}


if __name__ == "__main__":
    json.dump(build(), open(os.path.join(HERE, "presets.json"), "w"), indent=1)
    print("%d presets" % len(PRESETS))
