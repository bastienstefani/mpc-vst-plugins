#!/usr/bin/env python3
"""One source for Osmium's parameters: writes vst/params.json (tools/params.py format, read by tools/gen_vst.py)
and src/osm_params.h (the engine's own table: ids, ranges, defaults, display formats, option counts).

    python3 vst/make_params.py        (from ports/osmium; rerun after any change here, then commit both files)

The list order is the VST parameter index: skins, saved projects and automation bind to it. params.lock (written by
gen_vst.py on the first build) fails the build if a key moves. Append new parameters at the end, never insert.
"""
import json
import os
import re

HERE = os.path.dirname(os.path.abspath(__file__))

TABLES = ["Classic", "PWM", "Harmonics", "Vowel", "Sync", "FM", "Fold", "Organ", "Digital", "Reso", "Comb", "Random"]
WAVES = TABLES + ["VA Saw", "VA Square", "VA Tri", "VA Sine"]   # 12 wavetables, then 4 classic waveforms
FTYPES = ["Off", "LP 24", "LP 12", "BP", "HP 12", "HP 24", "Notch"]
LSHAPES = ["Sine", "Triangle", "Saw Up", "Saw Down", "Square", "S&H", "Smooth"]
LSYNC = ["Off", "4/1", "2/1", "1/1", "1/2", "1/2T", "1/4", "1/4T", "1/8", "1/8T", "1/16", "1/16T", "1/32"]
DSYNC = ["Off", "1/32", "1/16T", "1/16", "1/16D", "1/8T", "1/8", "1/8D", "1/4T", "1/4", "1/4D", "1/2", "1/1"]
SOURCES = ["Off", "Amp Env", "Filt Env", "Env 3", "LFO 1", "LFO 2", "LFO 3", "Velocity", "Key", "Mod Whl",
           "Pressure", "Bend", "Random", "Expr"]
DESTS = ["Off", "Pitch", "O1 Pitch", "O2 Pitch", "O3 Pitch", "O1 Shape", "O2 Shape", "O3 Shape", "O1 Level",
         "O2 Level", "O3 Level", "Noise", "Cutoff", "F1 Cutoff", "F2 Cutoff", "F1 Reso", "F2 Reso", "F Balance",
         "Amp", "Pan", "LFO1 Rate", "LFO2 Rate", "LFO3 Rate"]

# display formats, shared with the engine (osm_params.h F_*): how get_param("<key>_display") writes the value
FMTS = ["NONE", "PCT", "BIP", "INT", "SIGNED", "SEMI", "CENT", "FREQ", "ETIME", "LRATE", "FADE", "GLIDE", "CRATE",
        "DTIME", "PAN", "BEND"]

P = []


def num(key, name, lo, hi, d, fmt, integer=False):
    P.append({"key": key, "name": name, "min": lo, "max": hi, "default": d, "fmt": fmt, "int": integer})


def opt(key, name, options, d=0):
    P.append({"key": key, "name": name, "options": options, "default": d, "fmt": "NONE"})


# --- voice / global (VOICE tab) ---
num("vol", "Volume", 0, 100, 70, "PCT")              # parameter 0 (the wrapper absorbs MPC's STOP toggle of it)
num("voices", "Voices", 1, 8, 8, "INT", True)
opt("mode", "Mode", ["Poly", "Mono", "Legato"])
num("glide", "Glide", 0, 100, 0, "GLIDE")
num("bend", "Bend Range", 0, 24, 2, "BEND", True)
num("vel", "Velocity", 0, 100, 70, "PCT")
num("spread", "Spread", 0, 100, 30, "PCT")

# --- three oscillators ---
# Shape: the position in a wavetable; on a VA wave, saw -> two saws (octave), square -> pulse width 50% -> 5%,
# triangle -> saw (skew), sine -> self-FM. One knob and one modulation destination for both kinds.
OSC_DEF = [dict(wave="Classic", shape=40, level=80), dict(wave="VA Saw", shape=0, level=60),
           dict(wave="Vowel", shape=50, level=0)]
for n in (1, 2, 3):
    d = OSC_DEF[n - 1]
    opt("o%d_wave" % n, "Osc %d Wave" % n, WAVES, WAVES.index(d["wave"]))
    num("o%d_shape" % n, "Osc %d Shape" % n, 0, 100, d["shape"], "PCT")
    num("o%d_oct" % n, "Osc %d Octave" % n, -3, 3, 0, "SIGNED", True)
    num("o%d_semi" % n, "Osc %d Semi" % n, -12, 12, 0, "SEMI", True)
    num("o%d_fine" % n, "Osc %d Fine" % n, -100, 100, 7 if n == 2 else 0, "CENT", True)
    num("o%d_level" % n, "Osc %d Level" % n, 0, 100, d["level"], "PCT")

num("noise", "Noise", 0, 100, 0, "PCT")
num("noise_color", "Noise Color", -100, 100, 0, "BIP")

# --- two filters ---
for n in (1, 2):
    opt("f%d_type" % n, "Filter %d Type" % n, FTYPES, 1 if n == 1 else 0)
    num("f%d_cut" % n, "F%d Cutoff" % n, 0, 100, 62 if n == 1 else 100, "FREQ")
    num("f%d_res" % n, "F%d Resonance" % n, 0, 100, 15, "PCT")
    num("f%d_drive" % n, "F%d Drive" % n, 0, 100, 0, "PCT")
    num("f%d_env" % n, "F%d Env Amt" % n, -100, 100, 35 if n == 1 else 0, "BIP")
    num("f%d_key" % n, "F%d Key Track" % n, 0, 100, 50, "PCT")
opt("f_route", "Filter Routing", ["Serial", "Parallel"])
num("f_bal", "F Balance", 0, 100, 50, "PCT")

# --- three envelopes: Amp, Filter, Mod ---
ENV_DEF = [(2, 40, 80, 35), (5, 50, 30, 40), (30, 50, 50, 50)]
for n, nm in ((1, "Amp"), (2, "Filt"), (3, "Env3")):
    a, dd, s, r = ENV_DEF[n - 1]
    num("e%d_a" % n, "%s Attack" % nm, 0, 100, a, "ETIME")
    num("e%d_d" % n, "%s Decay" % nm, 0, 100, dd, "ETIME")
    num("e%d_s" % n, "%s Sustain" % nm, 0, 100, s, "PCT")
    num("e%d_r" % n, "%s Release" % nm, 0, 100, r, "ETIME")

# --- three LFOs ---
for n in (1, 2, 3):
    opt("l%d_shape" % n, "LFO%d Shape" % n, LSHAPES)
    num("l%d_rate" % n, "LFO%d Rate" % n, 0, 100, 74 if n == 1 else 50, "LRATE")   # LFO 1: 5.5 Hz, a vibrato
    opt("l%d_sync" % n, "LFO%d Sync" % n, LSYNC)
    opt("l%d_retrig" % n, "LFO%d Retrig" % n, ["Free", "Key"])
    num("l%d_fade" % n, "LFO%d Fade In" % n, 0, 100, 0, "FADE")

# --- modulation matrix: 16 slots of source x via -> destination ---
MOD_DEF = {1: ("LFO 1", "Mod Whl", "Pitch", 3), 2: ("Velocity", "Off", "Cutoff", 10)}   # mod wheel vibrato, velocity brightens
for n in range(1, 17):
    src, via, dst, amt = MOD_DEF.get(n, ("Off", "Off", "Off", 0))
    opt("m%d_src" % n, "Mod %d Source" % n, SOURCES, SOURCES.index(src))
    opt("m%d_via" % n, "Mod %d Via" % n, SOURCES, SOURCES.index(via))
    opt("m%d_dst" % n, "Mod %d Dest" % n, DESTS, DESTS.index(dst))
    num("m%d_amt" % n, "Mod %d Amount" % n, -100, 100, amt, "BIP")

# --- effects: chorus, delay ---
num("ch_mix", "Chorus Mix", 0, 100, 30, "PCT")
num("ch_rate", "Chorus Rate", 0, 100, 30, "CRATE")
num("ch_depth", "Chorus Depth", 0, 100, 50, "PCT")
num("dl_mix", "Delay Mix", 0, 100, 0, "PCT")
num("dl_time", "Delay Time", 0, 100, 60, "DTIME")
opt("dl_sync", "Delay Sync", DSYNC, DSYNC.index("1/8D"))
num("dl_fb", "Delay Feedback", 0, 100, 35, "PCT")
num("dl_tone", "Delay Tone", 0, 100, 60, "PCT")
opt("dl_mode", "Delay Mode", ["Stereo", "Ping Pong"], 1)


def params_json():
    out = []
    for p in P:
        q = {"key": p["key"], "name": p["name"]}
        if "options" in p:
            q["options"] = p["options"]
            q["default"] = p["default"]
        else:
            q.update({"min": p["min"], "max": p["max"], "default": p["default"]})
            if p["int"]:
                q["display"] = "int"
            q["dynamic_display"] = True   # the engine writes the value with its unit (get_param "<key>_display")
        out.append(q)
    def keys(rx):
        return [p["key"] for p in P if re.match(rx, p["key"])]
    sections = [
        ("Voice", [p["key"] for p in P[:7]]),
        ("Osc 1", keys(r"o1_")), ("Osc 2", keys(r"o2_")), ("Osc 3", keys(r"o3_")), ("Noise", keys(r"noise")),
        ("Filters", keys(r"f\d?_")), ("Envelopes", keys(r"e\d_")), ("LFOs", keys(r"l\d_")),
        ("Mod", keys(r"m\d+_")), ("FX", keys(r"(ch|dl)_")),
    ]
    return {"name": "Osmium", "params": out, "sections": [{"label": l, "keys": k} for l, k in sections]}


def c_header():
    ids = ["P_" + p["key"].upper() for p in P]
    L = ["/* generated by vst/make_params.py: do not edit (rerun it and commit both outputs) */",
         "#pragma once", ""]
    L += ["enum { %s };" % ", ".join("F_" + f for f in FMTS), ""]
    L.append("enum {")
    L += ["    %s," % i for i in ids]
    L += ["    NP", "};", ""]
    L.append("typedef struct { const char *key; float min, max, def; unsigned char fmt, nopts, integer; } osm_pdef_t;")
    L.append("static const osm_pdef_t OSM_P[NP] = {")
    for p in P:
        if "options" in p:
            n = len(p["options"])
            L.append('    {"%s", 0, %d, %d, F_NONE, %d, 1},' % (p["key"], n - 1, p["default"], n))
        else:
            L.append('    {"%s", %g, %g, %g, F_%s, 0, %d},' % (p["key"], p["min"], p["max"], p["default"], p["fmt"],
                                                           1 if p["int"] else 0))
    L += ["};", ""]
    L.append("#define WT_NT %d   /* o<n>_wave below WT_NT is a wavetable, from WT_NT on a VA wave */" % len(TABLES))
    L.append("#define N_SRC %d" % len(SOURCES))
    L.append("#define N_DST %d" % len(DESTS))
    L.append("#define N_SLOTS 16")
    L.append("enum { %s };" % ", ".join("S_" + s.upper().replace(" ", "_").replace("&", "N") for s in SOURCES))
    L.append("enum { %s };" % ", ".join("D_" + s.upper().replace(" ", "_") for s in DESTS))
    L.append("static const float LSYNC_BEATS[%d] = {0, %s};" % (len(LSYNC), ", ".join(
        "%.6ff" % b for b in (16, 8, 4, 2, 4 / 3, 1, 2 / 3, 0.5, 1 / 3, 0.25, 1 / 6, 0.125))))
    L.append("static const float DSYNC_BEATS[%d] = {0, %s};" % (len(DSYNC), ", ".join(
        "%.6ff" % b for b in (0.125, 1 / 6, 0.25, 0.375, 1 / 3, 0.5, 0.75, 2 / 3, 1, 1.5, 2, 4))))
    return "\n".join(L) + "\n"


if __name__ == "__main__":
    assert len({p["key"] for p in P}) == len(P), "duplicate key"
    json.dump(params_json(), open(os.path.join(HERE, "params.json"), "w"), indent=1)
    open(os.path.join(HERE, "..", "src", "osm_params.h"), "w").write(c_header())
    print("%d parameters" % len(P))
