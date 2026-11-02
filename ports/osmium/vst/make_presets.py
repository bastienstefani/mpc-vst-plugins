#!/usr/bin/env python3
"""Osmium's factory presets -> vst/presets.json (MPC's PRESET menu; the wrapper's own preset list).

    python3 vst/make_presets.py        (from ports/osmium, after make_params.py; commit the output)

Every preset sets every parameter (the defaults from make_params.py, then its own changes), so picking one always
gives the same sound. The first preset is the plugin's default sound. Values are in each parameter's own units, or
an option's label.
"""
import json
import os

import make_params as mp

HERE = os.path.dirname(os.path.abspath(__file__))


def slots(*rows):
    """(source, via, destination, amount) per mod slot, from slot 1; the rest are cleared."""
    out = {}
    for n in range(1, 17):
        src, via, dst, amt = rows[n - 1] if n <= len(rows) else ("Off", "Off", "Off", 0)
        out.update({"m%d_src" % n: src, "m%d_via" % n: via, "m%d_dst" % n: dst, "m%d_amt" % n: amt})
    return out


VIBRATO = ("LFO 1", "Mod Whl", "Pitch", 3)

PRESETS = [
    ("Init Wave", {}),
    ("Vowel Pad", dict(
        o1_wave="Vowel", o1_shape=20, o2_wave="Vowel", o2_shape=70, o2_fine=9, o2_level=70,
        f1_cut=58, f1_res=20, f1_env=15, e1_a=55, e1_d=60, e1_s=85, e1_r=66, e2_a=50, e2_d=70, e2_s=60, e2_r=60,
        e3_a=72, e3_d=80, e3_s=100, e3_r=60, l2_shape="Triangle", l2_rate=35, ch_mix=55, dl_mix=22, dl_sync="1/4D",
        dl_fb=45, spread=70, **slots(VIBRATO, ("Env 3", "Off", "O1 Shape", 45), ("LFO 2", "Off", "O2 Shape", -25)))),
    ("Sync Lead", dict(
        mode="Mono", glide=28, o1_wave="Sync", o1_shape=15, o2_wave="VA Saw", o2_fine=11, o2_level=55,
        f1_cut=64, f1_res=25, f1_env=30, e1_a=1, e1_d=50, e1_s=90, e1_r=30, e2_a=1, e2_d=48, e2_s=35,
        e3_a=1, e3_d=55, e3_s=20, e3_r=40, l2_rate=40, dl_mix=25, dl_sync="1/8D", dl_fb=40, ch_mix=15,
        **slots(VIBRATO, ("Env 3", "Off", "O1 Shape", 55), ("LFO 2", "Off", "O1 Shape", 15)))),
    ("FM Bells", dict(
        o1_wave="FM", o1_shape=45, o2_wave="FM", o2_oct=1, o2_shape=75, o2_fine=0, o2_level=35,
        f1_type="LP 12", f1_cut=88, f1_res=5, f1_env=0, e1_a=0, e1_d=68, e1_s=0, e1_r=62, e3_a=0, e3_d=52, e3_s=0,
        e3_r=50, ch_mix=25, dl_mix=30, dl_sync="1/4", dl_fb=40, spread=60, vel=85,
        **slots(VIBRATO, ("Env 3", "Off", "O1 Shape", -40), ("Velocity", "Off", "O2 Level", 30)))),
    ("Drawbar Organ", dict(
        o1_wave="Organ", o1_shape=48, o2_level=0, f1_type="LP 12", f1_cut=92, f1_res=0, f1_env=0, f1_key=0,
        e1_a=2, e1_d=40, e1_s=100, e1_r=18, vel=0, ch_mix=65, ch_rate=62, ch_depth=60, l1_rate=78,
        **slots(("LFO 1", "Mod Whl", "Pitch", 2), ("Velocity", "Off", "Off", 0)))),
    ("PWM Strings", dict(
        o1_wave="VA Square", o1_shape=40, o2_wave="VA Square", o2_shape=40, o2_fine=-9, o2_level=75,
        f1_cut=57, f1_res=10, f1_env=15, e1_a=52, e1_d=60, e1_s=85, e1_r=62, e2_a=45, e2_d=60, e2_s=60, e2_r=55,
        l2_shape="Triangle", l2_rate=42, l3_shape="Triangle", l3_rate=38, ch_mix=60, spread=80, dl_mix=15,
        **slots(VIBRATO, ("LFO 2", "Off", "O1 Shape", 35), ("LFO 3", "Off", "O2 Shape", 35)))),
    ("Fold Bass", dict(
        mode="Mono", glide=10, o1_wave="Fold", o1_shape=10, o2_wave="VA Sine", o2_oct=-1, o2_fine=0,
        o2_level=70, f1_cut=38, f1_res=30, f1_env=45, f1_drive=30, f1_key=70, e1_a=0, e1_d=50, e1_s=80, e1_r=15,
        e2_a=0, e2_d=45, e2_s=20, e2_r=30, e3_a=0, e3_d=48, e3_s=10, e3_r=30, ch_mix=0, spread=0,
        **slots(VIBRATO, ("Env 3", "Off", "O1 Shape", 50), ("Velocity", "Off", "Cutoff", 15)))),
    ("Digital Pluck", dict(
        o1_wave="Digital", o1_shape=30, o2_wave="Comb", o2_shape=50, o2_oct=1, o2_fine=0, o2_level=40,
        f1_type="LP 12", f1_cut=50, f1_res=20, f1_env=50, e1_a=0, e1_d=55, e1_s=0, e1_r=45, e2_a=0, e2_d=45, e2_s=0,
        e2_r=40, dl_mix=35, dl_sync="1/8D", dl_fb=45, dl_mode="Ping Pong", ch_mix=20, spread=60,
        **slots(VIBRATO, ("Velocity", "Off", "Cutoff", 20), ("Random", "Off", "O1 Shape", 20)))),
    ("Reso Sweep", dict(
        o1_wave="Reso", o1_shape=0, o2_level=40, o2_fine=12, f1_cut=80, f1_res=10, f1_env=0, e1_a=30, e1_d=60,
        e1_s=90, e1_r=55, l2_shape="Saw Up", l2_sync="1/1", l2_retrig="Key", ch_mix=40, dl_mix=28, dl_sync="1/4D",
        dl_fb=50, spread=70, **slots(VIBRATO, ("LFO 2", "Off", "O1 Shape", 100)))),
    ("Wind Noise", dict(
        o1_level=0, o2_level=0, noise=85, noise_color=0, f1_type="BP", f1_cut=55, f1_res=72, f1_env=0, f1_key=0,
        e1_a=70, e1_d=60, e1_s=100, e1_r=72, l2_shape="Smooth", l2_rate=12, l3_shape="Smooth", l3_rate=20,
        dl_mix=40, dl_sync="1/4D", dl_fb=55, ch_mix=40, vel=30,
        **slots(("LFO 2", "Off", "F1 Cutoff", 25), ("LFO 3", "Off", "Pan", 60), ("Key", "Off", "F1 Cutoff", 30)))),
    ("Random Steps", dict(
        o1_wave="Random", o1_shape=40, o2_wave="Harmonics", o2_shape=60, o2_level=45,
        f1_cut=62, f1_res=30, f1_env=25, e1_a=0, e1_d=55, e1_s=40, e1_r=40, e2_a=0, e2_d=40, e2_s=20,
        l2_shape="S&H", l2_sync="1/16", l3_shape="Smooth", l3_sync="1/2", dl_mix=35, dl_sync="1/8D",
        dl_mode="Ping Pong", dl_fb=45, spread=70,
        **slots(VIBRATO, ("LFO 2", "Off", "O1 Shape", 60), ("LFO 3", "Off", "F1 Cutoff", 15)))),
    ("Dual Filter", dict(
        o1_wave="Harmonics", o1_shape=85, o2_wave="VA Saw", o2_level=70, f_route="Parallel", f_bal=50,
        f1_type="LP 24", f1_cut=45, f1_res=55, f1_env=30, f2_type="HP 12", f2_cut=70, f2_res=40, f2_env=-20,
        e1_a=5, e1_d=60, e1_s=70, e1_r=45, e2_a=10, e2_d=65, e2_s=30, l2_shape="Triangle", l2_rate=30,
        ch_mix=35, spread=60, **slots(VIBRATO, ("LFO 2", "Off", "F Balance", 45)))),
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
            v = ch.get(p["key"], p["options"][p["default"]] if "options" in p else p["default"])
            vals[p["key"]] = v
        out.append({"name": name, "values": vals})
    return {"presets": out}


if __name__ == "__main__":
    json.dump(build(), open(os.path.join(HERE, "presets.json"), "w"), indent=1)
    print("%d presets" % len(PRESETS))
