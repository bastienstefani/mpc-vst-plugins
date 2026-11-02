#!/usr/bin/env python3
"""One source for Philtre's parameters: writes vst/params.json (tools/params.py format, read by tools/gen_vst.py) and
src/ph_params.h (the engine's table: ids, ranges, defaults, display formats, option lists it needs).

    python3 vst/make_params.py        (from ports/philtre; rerun after a change, commit both outputs)

The list order is the VST parameter index (skins, projects and automation bind to it; vst/params.lock guards it):
append new parameters at the end, never insert.
"""
import json
import os
import re

HERE = os.path.dirname(os.path.abspath(__file__))

TABLES = ["Classic", "PWM", "Harmonics", "Vowel", "Sync", "FM", "Fold", "Organ", "Digital", "Reso", "Comb", "Random"]
USER_SLOTS = 64                       # user wavetables (WAV files in the plugin's wavetables/ folder), after the built-ins
WARPS = ["Off", "Sync", "Bend +", "Bend -", "PWM", "Asym +", "Asym -", "Flip", "Mirror", "Quantize", "FM", "AM", "RM"]
SUB_SHAPES = ["Sine", "Triangle", "Saw", "Square", "Pulse 25", "Pulse 12"]
NOISES = ["White", "Pink", "Brown", "Digital", "Crackle", "Hiss"]
FTYPES = ["LP 12", "LP 24", "MG LP 12", "MG LP 24", "HP 12", "HP 24", "BP 12", "Notch", "Comb +", "Comb -", "Phaser",
          "Formant"]
LSHAPES = ["Sine", "Triangle", "Saw Up", "Saw Down", "Square", "S&H", "Smooth", "Pump", "Exp Up", "Stairs"]
LSYNC = ["Off", "4/1", "2/1", "1/1", "1/2", "1/2T", "1/4", "1/4T", "1/8", "1/8T", "1/16", "1/16T", "1/32"]
LMODES = ["Free", "Retrig", "Env"]
SOURCES = ["Off", "Env 1", "Env 2", "Env 3", "Env 4", "LFO 1", "LFO 2", "LFO 3", "LFO 4", "Velocity", "Note", "Mod Whl",
           "Aftertouch", "Bend", "Macro 1", "Macro 2", "Macro 3", "Macro 4", "Random", "Expr"]
DEST_GROUPS = [
    ("VOICE", ["Off", "Pitch", "Amp", "Pan"]),
    ("OSC A", ["A Pitch", "A Pos", "A Warp", "A Level", "A Pan", "A Detune", "A Blend"]),
    ("OSC B", ["B Pitch", "B Pos", "B Warp", "B Level", "B Pan", "B Detune", "B Blend"]),
    ("SUB / NOISE", ["Sub Level", "Sub Pan", "Noise Level", "Noise Pitch"]),
    ("FILTER", ["Cutoff", "Resonance", "Drive", "Filter Mix"]),
    ("LFO", ["LFO1 Rate", "LFO2 Rate", "LFO3 Rate", "LFO4 Rate"]),
    ("FX P1", ["FX1 P1", "FX2 P1", "FX3 P1", "FX4 P1", "FX5 P1", "FX6 P1", "FX7 P1", "FX8 P1"]),
    ("FX MIX", ["FX1 Mix", "FX2 Mix", "FX3 Mix", "FX4 Mix", "FX5 Mix", "FX6 Mix", "FX7 Mix", "FX8 Mix"]),
]
DESTS = [d for _, g in DEST_GROUPS for d in g]
FX_TYPES = ["Off", "Ensemble", "Distortion", "Flanger", "Phaser", "Chorus", "Delay", "Ping Pong", "Compressor",
            "Multiband", "Reverb", "Plate", "EQ", "Filter"]

# display formats shared with the engine (ph_params.h F_*): how get_param("<key>_display") writes a value
FMTS = ["NONE", "PCT", "BIP", "INT", "SIGNED", "SEMI", "CENT", "FREQ", "ETIME", "LRATE", "RISE", "GLIDE", "PAN", "BEND",
        "DEG", "TABLE", "DETUNE", "FXP", "ONOFF"]

P = []


def num(key, name, lo, hi, d, fmt, integer=False, **extra):
    P.append(dict(key=key, name=name, min=lo, max=hi, default=d, fmt=fmt, int=integer, **extra))


def opt(key, name, options, d=0, **extra):
    if isinstance(d, str):
        d = options.index(d)
    P.append(dict(key=key, name=name, options=options, default=d, fmt="NONE", **extra))


ONOFF = ["Off", "On"]

# --- global, macros (MAIN tab) ---
num("vol", "Volume", 0, 100, 70, "PCT")               # parameter 0: the wrapper absorbs MPC's STOP toggle of it
num("voices", "Voices", 1, 8, 8, "INT", True)
opt("mode", "Mode", ["Poly", "Mono", "Legato"])
num("glide", "Glide", 0, 100, 0, "GLIDE")
num("bend", "Bend Range", 0, 24, 2, "BEND", True)
num("vel", "Velocity", 0, 100, 50, "PCT")
for m in range(1, 5):
    num("mac%d" % m, "Macro %d" % m, 0, 100, 0, "PCT")

# --- oscillators A and B ---
for o, on in (("a", 1), ("b", 0)):
    O = o.upper()
    opt("%s_on" % o, "Osc %s" % O, ONOFF, on)
    num("%s_table" % o, "%s Table" % O, 0, len(TABLES) + USER_SLOTS - 1, 0, "TABLE", True)
    num("%s_pos" % o, "%s WT Pos" % O, 0, 100, 40 if o == "a" else 0, "PCT")
    opt("%s_warp" % o, "%s Warp" % O, WARPS)
    num("%s_wamt" % o, "%s Warp Amt" % O, 0, 100, 0, "PCT")
    num("%s_oct" % o, "%s Octave" % O, -4, 4, 0, "SIGNED", True)
    num("%s_semi" % o, "%s Semi" % O, -12, 12, 0, "SEMI", True)
    num("%s_fine" % o, "%s Fine" % O, -100, 100, 0, "CENT", True)
    num("%s_level" % o, "%s Level" % O, 0, 100, 75, "PCT")
    num("%s_uni" % o, "%s Unison" % O, 1, 8, 1, "INT", True)
    num("%s_det" % o, "%s Detune" % O, 0, 100, 25, "DETUNE")
    num("%s_blend" % o, "%s Blend" % O, 0, 100, 75, "PCT")
    num("%s_width" % o, "%s Width" % O, 0, 100, 100, "PCT")
    num("%s_phase" % o, "%s Phase" % O, 0, 360, 0, "DEG", True)
    num("%s_rand" % o, "%s Random" % O, 0, 100, 100, "PCT")
    num("%s_pan" % o, "%s Pan" % O, -100, 100, 0, "PAN")
for o in ("a", "b"):   # stepper arrows of the table readouts
    P.append(dict(key="%s_table_prev" % o, name="%s Table <" % o.upper(), min=0, max=1, default=0, fmt="NONE",
                  momentary=True, step_of="%s_table" % o, step_delta=-1))
    P.append(dict(key="%s_table_next" % o, name="%s Table >" % o.upper(), min=0, max=1, default=0, fmt="NONE",
                  momentary=True, step_of="%s_table" % o, step_delta=1))

# --- sub and noise ---
opt("s_on", "Sub", ONOFF)
opt("s_shape", "Sub Shape", SUB_SHAPES)
num("s_oct", "Sub Octave", -4, 2, -1, "SIGNED", True)
num("s_level", "Sub Level", 0, 100, 75, "PCT")
num("s_pan", "Sub Pan", -100, 100, 0, "PAN")
opt("s_direct", "Sub Direct", ["Filter", "Direct"])
opt("n_on", "Noise", ONOFF)
opt("n_type", "Noise Type", NOISES)
num("n_pitch", "Noise Pitch", 0, 100, 50, "PCT")
opt("n_key", "Noise Key", ONOFF)
num("n_level", "Noise Level", 0, 100, 50, "PCT")
num("n_pan", "Noise Pan", -100, 100, 0, "PAN")

# --- filter ---
opt("f_on", "Filter", ONOFF)
opt("f_type", "Filter Type", FTYPES, "LP 24")
num("f_cut", "Cutoff", 0, 100, 70, "FREQ")
num("f_res", "Resonance", 0, 100, 10, "PCT")
num("f_drive", "Filter Drive", 0, 100, 0, "PCT")
num("f_key", "Key Track", 0, 100, 0, "PCT")
num("f_env", "Env 2 Amount", -100, 100, 0, "BIP")
num("f_mix", "Filter Mix", 0, 100, 100, "PCT")
for s, nm in (("a", "A"), ("b", "B"), ("n", "Noise"), ("s", "Sub")):
    opt("f_r%s" % s, "%s To Filter" % nm, ONOFF, 1)

# --- four envelopes (Env 1 = amp) ---
for e in (1, 2, 3, 4):
    num("e%d_a" % e, "Env%d Attack" % e, 0, 100, 0, "ETIME")
    num("e%d_d" % e, "Env%d Decay" % e, 0, 100, 77, "ETIME")
    num("e%d_s" % e, "Env%d Sustain" % e, 0, 100, 100 if e == 1 else 0, "PCT")
    num("e%d_r" % e, "Env%d Release" % e, 0, 100, 34, "ETIME")

# --- four LFOs ---
for l in (1, 2, 3, 4):
    opt("l%d_shape" % l, "LFO%d Shape" % l, LSHAPES, "Triangle")
    num("l%d_rate" % l, "LFO%d Rate" % l, 0, 100, 50, "LRATE")
    opt("l%d_sync" % l, "LFO%d Sync" % l, LSYNC, "1/4")
    opt("l%d_mode" % l, "LFO%d Mode" % l, LMODES, "Retrig")
    num("l%d_rise" % l, "LFO%d Rise" % l, 0, 100, 0, "RISE")
    num("l%d_phase" % l, "LFO%d Phase" % l, 0, 360, 0, "DEG", True)

# --- modulation matrix: 16 slots of source x aux -> destination ---
for m in range(1, 17):
    opt("m%d_src" % m, "Mod%d Source" % m, SOURCES)
    opt("m%d_aux" % m, "Mod%d Aux" % m, SOURCES)
    opt("m%d_dst" % m, "Mod%d Dest" % m, DESTS)
    num("m%d_amt" % m, "Mod%d Amount" % m, -100, 100, 0, "BIP")

# --- FX rack: 8 slots in series, each one effect with three controls and a mix (names follow the effect) ---
for s in range(1, 9):
    opt("fx%d_type" % s, "FX%d Type" % s, FX_TYPES, refresh_display=True)
    for k, d in (("p1", 50), ("p2", 50), ("p3", 50), ("mix", 50)):
        num("fx%d_%s" % (s, k), "FX%d %s" % (s, k.upper()), 0, 100, d, "FXP", dynamic_name=True)


def params_json():
    out = []
    for p in P:
        q = {"key": p["key"], "name": p["name"]}
        if "options" in p:
            q["options"] = p["options"]
            q["default"] = p["default"]
        else:
            q.update({"min": p["min"], "max": p["max"], "default": p["default"]})
            if p.get("int"):
                q["display"] = "int"
            if not p.get("momentary"):
                q["dynamic_display"] = True   # the engine writes the value with its unit
        for k in ("momentary", "step_of", "step_delta", "dynamic_name", "refresh_display"):
            if k in p:
                q[k] = p[k]
        out.append(q)

    def keys(rx):
        return [p["key"] for p in P if re.match(rx, p["key"])]
    sections = [("Main", [p["key"] for p in P[:10]]), ("Osc A", keys(r"a_")), ("Osc B", keys(r"b_")),
                ("Sub", keys(r"s_")), ("Noise", keys(r"n_")), ("Filter", keys(r"f_")), ("Envelopes", keys(r"e\d_")),
                ("LFOs", keys(r"l\d_")), ("Mod", keys(r"m\d+_")), ("FX", keys(r"fx\d_"))]
    return {"name": "Philtre", "params": out, "sections": [{"label": l, "keys": k} for l, k in sections]}


def ident(s):
    s = s.replace("+", " plus").replace("-", " minus").replace("&", "n")
    return re.sub(r"[^A-Z0-9]+", "_", s.upper()).strip("_")


def c_header():
    L = ["/* generated by vst/make_params.py: do not edit (rerun it and commit both outputs) */", "#pragma once", ""]
    L.append("enum { %s };" % ", ".join("F_" + f for f in FMTS))
    L += ["", "enum {"] + ["    P_%s," % p["key"].upper() for p in P] + ["    NP", "};", ""]
    L.append("typedef struct { const char *key; float min, max, def; unsigned char fmt, nopts, integer; } ph_pdef_t;")
    L.append("static const ph_pdef_t PH_P[NP] = {")
    for p in P:
        if "options" in p:
            n = len(p["options"])
            L.append('    {"%s", 0, %d, %d, F_NONE, %d, 1},' % (p["key"], n - 1, p["default"], n))
        else:
            L.append('    {"%s", %g, %g, %g, F_%s, 0, %d},' % (p["key"], p["min"], p["max"], p["default"], p["fmt"],
                                                           1 if p.get("int") or p.get("momentary") else 0))
    L += ["};", ""]
    L.append("#define WT_NB %d          /* built-in wavetables; a table index from WT_NB on is a user file */" % len(TABLES))
    L.append("#define USER_SLOTS %d" % USER_SLOTS)
    L.append("static const char *const TABLE_NAMES[WT_NB] = {%s};" % ", ".join('"%s"' % t for t in TABLES))
    for name, lst in (("WARP", WARPS), ("SRC", SOURCES), ("DST", DESTS), ("FT", FTYPES), ("LS", LSHAPES),
                      ("FX", FX_TYPES), ("NZ", NOISES), ("SUB", SUB_SHAPES)):
        L.append("enum { %s, N_%s };" % (", ".join("%s_%s" % (name, ident(x)) for x in lst), name))
    L.append("static const float LSYNC_BEATS[%d] = {0, %s};" % (len(LSYNC), ", ".join(
        "%.6ff" % b for b in (16, 8, 4, 2, 4 / 3, 1, 2 / 3, 0.5, 1 / 3, 0.25, 1 / 6, 0.125))))
    L.append("#define N_SLOTS 16")
    L.append("#define FX_SLOTS 8")
    return "\n".join(L) + "\n"


if __name__ == "__main__":
    assert len({p["key"] for p in P}) == len(P), "duplicate key"
    json.dump(params_json(), open(os.path.join(HERE, "params.json"), "w"), indent=1)
    open(os.path.join(HERE, "..", "src", "ph_params.h"), "w").write(c_header())
    print("%d parameters" % len(P))
