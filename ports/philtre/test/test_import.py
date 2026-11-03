#!/usr/bin/env python3
"""Offline test of tools/serum2_import.py on synthetic Serum 2 presets built here (no third-party preset files): the
container and CBOR reader, the field mapping, the report, the .xpl, bad files. Writes the converted states, one per
line, to the file named by its argument, for test/test_states.c to load into the engine.

    python3 test/test_import.py [states.txt]       (needs Python 3.14+ or the zstandard package; exits 77 to skip)
"""
import json
import os
import re
import struct
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "tools"))
sys.path.insert(0, os.path.join(HERE, "..", "..", "..", "tools"))
try:
    from compression import zstd as _z

    def compress(b):
        return _z.compress(b)
except ImportError:
    try:
        import zstandard
    except ImportError:
        print("skip: no Zstandard (Python 3.14+ or pip install zstandard)")
        sys.exit(77)

    def compress(b):
        return zstandard.ZstdCompressor().compress(b)

import serum2_import as si  # noqa: E402
import xpl  # noqa: E402

fails = 0


def check(ok, what):
    global fails
    print("%s %s" % ("ok  " if ok else "FAIL", what))
    if not ok:
        fails += 1


def cbor(v):
    def head(major, n):
        if n < 24:
            return bytes([major << 5 | n])
        for info, size in ((24, 1), (25, 2), (26, 4), (27, 8)):
            if n < 1 << (8 * size):
                return bytes([major << 5 | info]) + n.to_bytes(size, "big")
    if v is None:
        return b"\xf6"
    if v is True or v is False:
        return b"\xf5" if v else b"\xf4"
    if isinstance(v, int):
        return head(0, v) if v >= 0 else head(1, -1 - v)
    if isinstance(v, float):
        f32 = struct.pack(">f", v)
        return b"\xfa" + f32 if struct.unpack(">f", f32)[0] == v else b"\xfb" + struct.pack(">d", v)
    if isinstance(v, str):
        b = v.encode()
        return head(3, len(b)) + b
    if isinstance(v, bytes):
        return head(2, len(v)) + v
    if isinstance(v, list):
        return head(4, len(v)) + b"".join(cbor(x) for x in v)
    if isinstance(v, dict):
        return head(5, len(v)) + b"".join(cbor(k) + cbor(x) for k, x in v.items())
    raise TypeError(type(v))


def preset_bytes(tree, name="Test", raw_cbor=None):
    meta = json.dumps({"fileType": "SerumPreset", "presetName": name, "presetAuthor": "test"}).encode()
    body = raw_cbor if raw_cbor is not None else cbor(tree)
    return si.MAGIC + struct.pack("<Q", len(meta)) + meta + struct.pack("<II", len(body), 2) + compress(body)


def triangle_lfo():
    return {"curveData": {"xVals": [0.0, 0.5, 1.0], "yVals": [1.0, 0.0, 1.0], "curveVals": [0.5, 0.5, 0.5],
                          "numPoints": 2}, "plainParams": "default"}


def base_tree():
    t = {"presetName": "Test", "Global0": {"plainParams": "default"}}
    for i in range(5):
        t["Oscillator%d" % i] = {"plainParams": "default"}
    for i in range(3):
        t["Oscillator%d" % i]["WTOsc%d" % i] = {"plainParams": "default", "relativePathToWT": "S2 Tables/Default Shapes.wav"}
    for e in range(4):
        t["Env%d" % e] = {"plainParams": {"kParamCurve1": 50.0, "kParamCurve2": 66.6, "kParamCurve3": 66.6}}
    for l in range(10):
        t["LFO%d" % l] = triangle_lfo()
    for r in range(3):
        t["FXRack%d" % r] = {"FX": [], "displayName": ""}
    return t


def state_of(path):
    text = open(path, encoding="utf-8").read()
    b = xpl.b64dec(re.search(r"<state>(.*?)</state>", text, re.S).group(1).strip())
    check(b[:4] == b"CcnK" and b[8:12] == b"FBCh", "the .xpl holds an fxb chunk set")
    check(struct.unpack(">i", b[16:20])[0] == int.from_bytes(b"PhlT", "big"), "with Philtre's uid")
    (n,) = struct.unpack(">i", b[156:160])
    chunk = b[160:160 + n]
    check(chunk.endswith(b"\0") and chunk.startswith(b"ph1;"), "and a NUL-ended Philtre state")
    return chunk[:-1].decode()


def values(state):
    d = {}
    for part in state.split(";")[1:]:
        k, _, v = part.partition("=")
        d[k] = v
    return d


def near(a, b, tol=0.6):
    return abs(float(a) - b) <= tol


def main():
    out_states = sys.argv[1] if len(sys.argv) > 1 else None
    tmp = tempfile.mkdtemp(prefix="philtre-import-")
    src = os.path.join(tmp, "in")
    os.makedirs(os.path.join(src, "sub"))
    tables = os.path.join(tmp, "Tables", "S2 Tables")
    os.makedirs(tables)
    with open(os.path.join(tables, "Default Shapes.wav"), "wb") as f:
        f.write(b"RIFF\x24\0\0\0WAVEfmt ")   # only copied, never read by the converter

    # 1: init-like; 2: a full patch; 3: three oscillators, a long LFO, an unknown source; bad files
    t1 = base_tree()
    t2 = base_tree()
    t2["Global0"] = {"plainParams": {"kParamMonoToggle": 1.0, "kParamPolyCount": 16.0}}
    t2["Oscillator0"]["plainParams"] = {"kParamUnison": 12.0, "kParamDetune": 0.5, "kParamOctave": -1.0,
                                        "kParamFine": 30.0, "kParamPan": -40.0, "kParamVolume": 0.5}
    t2["Oscillator0"]["WTOsc0"] = {"plainParams": {"kParamTablePos": 128.5, "kParamWarp": 0.4, "kParamWarpMenu": "kSync",
                                                   "kParamInitialPhase": 90.0, "kParamRandomPhase": 0.0},
                                   "relativePathToWT": "S2 Tables/Digital/Vowel Morph.wav"}
    t2["Oscillator1"]["plainParams"] = {"kParamEnable": 1.0, "kParamOctave": 2.0}
    t2["Oscillator1"]["WTOsc1"]["plainParams"] = {"kParamWarpMenu": "kFM_OSC", "kParamWarp": 0.25}
    t2["Oscillator3"] = {"plainParams": {"kParamEnable": 1.0, "kParamVolume": 0.3},
                         "NoiseOsc3": {"plainParams": {"kParamColor": 0.8}, "relativePathToNoiseSample": "Analog/Pink Noise.wav"}}
    t2["Oscillator4"] = {"plainParams": {"kParamEnable": 1.0, "kParamOctave": -2.0}, "SubOsc4": {"plainParams": "default"}}
    t2["RoutingSlot1"] = {"plainParams": {"kParamRoutingDest": "kRoutingDestFilter"}}
    t2["RoutingSlot4"] = {"plainParams": {"kParamRoutingDest": "kRoutingDestDirect"}}
    t2["VoiceFilter0"] = {"plainParams": {"kParamEnable": 1.0, "kParamType": "MgL24", "kParamFreq": 0.5,
                                         "kParamReso": 40.0, "kParamDrive": 20.0}}
    t2["Env0"]["plainParams"].update({"kParamAttack": 0.01, "kParamDecay": 0.5, "kParamSustain": 0.5,
                                      "kParamRelease": 0.3, "kParamCurve1": 100.0})
    t2["Env1"]["plainParams"].update({"kParamAttack": 0.0, "kParamDecay": 0.2, "kParamSustain": 0.0})
    t2["LFO2"] = {"curveData": {"xVals": [0.0, 0.25, 0.25, 1.0], "yVals": [0.0, 0.0, 1.0, 1.0],
                                "curveVals": [0.5, 0.9, 0.5, 0.5], "numPoints": 3},
                  "plainParams": {"kParamMode": "Free", "kParamRate": 10.66}}
    t2["Macro0"] = {"name": "CUT", "plainParams": {"kParamValue": 40.0}}
    t2["ModSlot0"] = {"source": [8, 0], "destModuleTypeString": "VoiceFilter", "destModuleID": 0,
                      "destModuleParamName": "kParamFreq", "plainParams": {"kParamAmount": 50.0}}
    t2["ModSlot1"] = {"source": [3, 0], "destModuleTypeString": "WTOsc", "destModuleID": 0,
                      "destModuleParamName": "kParamTablePos", "plainParams": {"kParamAmount": 30.0, "kParamBipolar": 1.0}}
    t2["ModSlot2"] = {"source": [8, 0], "destModuleTypeString": "Oscillator", "destModuleID": 0,
                      "destModuleParamName": "kParamPan", "plainParams": {"kParamAmount": 20.0, "kParamBipolar": 1.0}}
    t2["ModSlot3"] = {"source": [22, 0], "destModuleTypeString": "VoiceFilter", "destModuleID": 0,
                      "destModuleParamName": "kParamReso", "plainParams": {"kParamAmount": 50.0}}
    t2["ModSlot4"] = {"source": [23, 0], "destModuleTypeString": "VoiceFilter", "destModuleID": 0,
                      "destModuleParamName": "kParamDrive", "plainParams": {"kParamAmount": 10.0}}
    t2["FXRack0"]["FX"] = [
        {"type": 0, "FXDistortion": {"plainParams": {"kParamDrive": 60.0, "kParamMode": "kDiode2", "kParamWet": 80.0}}},
        {"type": 2, "FXDelay": {"plainParams": {"kParamFeedback": 45.0, "kParamWet": 25.0}}},
        {"type": 7, "FXBode": {"plainParams": {"kParamWet": 50.0}}},
        {"type": 4, "FXReverb": {"plainParams": {"kParamSize": 70.0, "kParamWet": 30.0}}}]
    t2["FXRack1"]["FX"] = [{"type": 4, "FXReverb": {"plainParams": "default"}}]
    t3 = base_tree()
    for i in range(3):
        t3["Oscillator%d" % i]["plainParams"] = {"kParamEnable": 1.0}
    t3["Oscillator0"]["plainParams"]["kParamEnable"] = 0.0
    t3["Oscillator2"]["plainParams"]["kParamType"] = "kOsc_Granular"
    xs = [i / 30 for i in range(31)]
    t3["LFO0"] = {"curveData": {"xVals": xs, "yVals": [0.5 + 0.5 * ((-1) ** i) * (i % 7) / 7 for i in range(30)] + [0.5],
                                "curveVals": [0.5] * 31}, "plainParams": "default"}
    for k in range(5):
        t3["ModSlot%d" % k] = {"source": [6 + k, 0], "destModuleTypeString": "WTOsc", "destModuleID": 1,
                               "destModuleParamName": "kParamTablePos", "plainParams": {"kParamAmount": 10.0}}
    t3["ModSlot5"] = {"source": [25, 6], "destModuleTypeString": "Oscillator", "destModuleID": 2,
                      "destModuleParamName": "kParamVolume", "plainParams": {"kParamAmount": 10.0}}
    t3["ModSlot6"] = {"source": [1, 0], "destModuleTypeString": "Env", "destModuleID": 0,
                      "destModuleParamName": "kParamAttack", "plainParams": {"kParamAmount": 10.0}}
    t3["Macro2"] = {"name": "WIDE", "plainParams": {"kParamValue": 70.0}}
    t3["Macro0"] = {"name": "M1", "plainParams": {"kParamValue": 55.0}}
    t3["LFO1"]["plainParams"] = {"kParamBeatSync": 0.0, "kParamRate": 1.0068}
    t3["LFO2"]["plainParams"] = {"kParamRate": 1.627}

    files = {"init.SerumPreset": preset_bytes(t1, "Init Test"), "full.SerumPreset": preset_bytes(t2, "Full / Test"),
             "sub/three.SerumPreset": preset_bytes(t3, "Three Osc"),
             "bad-magic.SerumPreset": b"not a preset at all",
             "cut.SerumPreset": preset_bytes(t1)[:-20],
             "deep.SerumPreset": preset_bytes(None, raw_cbor=b"\x81" * 100 + b"\xf6"),
             "notmap.SerumPreset": preset_bytes(None, raw_cbor=cbor([1, 2, 3])),
             "huge-len.SerumPreset": preset_bytes(None, raw_cbor=b"\xbb" + (1 << 40).to_bytes(8, "big"))}
    for n, b in files.items():
        with open(os.path.join(src, n), "wb") as f:
            f.write(b)

    out = os.path.join(tmp, "out")
    r = subprocess.run([sys.executable, os.path.join(HERE, "..", "tools", "serum2_import.py"), "-o", out, "-q",
                        "--tables", os.path.join(tmp, "Tables"), "--source", "23=Velocity", src],
                       capture_output=True, text=True)
    check(r.returncode == 0, "the converter runs (%s)" % (r.stderr.strip()[-300:] or "exit 0"))
    report = open(os.path.join(out, "import-report.txt"), encoding="utf-8").read()
    names = sorted(os.listdir(os.path.join(out, "Presets")))
    check(names == ["Full _ Test.xpl", "Init Test.xpl", "Three Osc.xpl"], "three presets, named after them: %s" % names)
    for bad in ("bad-magic", "cut", "deep", "notmap", "huge-len"):
        check(re.search(r"%s\.SerumPreset\n  x not converted" % bad, report) is not None, "%s is refused in the report" % bad)
    check(os.path.exists(os.path.join(out, "wavetables", "serum2", "Default Shapes.wav")), "--tables copies a used table")

    s1 = state_of(os.path.join(out, "Presets", "Init Test.xpl"))
    v1 = values(s1)
    check(v1["a_on"] == "1" and v1["b_on"] == "0" and v1["a_tfile"] == "serum2/Default Shapes.wav" and v1["a_table"] == "0",
          "init: oscillator A only, its table copied and named, Classic standing in")
    check(near(v1["a_det"], 25) and v1["a_uni"] == "1" and near(v1["a_pos"], 0) and near(v1["a_phase"], 180),
          "init: Serum's defaults (detune 0.25, 1 voice, first frame, phase 180)")
    check(near(v1["e1_a"], si.etime_x(si.ENV1_ATTACK), 0.01) and near(v1["e1_d"], 76.7) and v1["e1_s"] == "100" and near(v1["e1_r"], 34.1),
          "init: Env 1 is 1 ms / 1 s / 0 dB / 15 ms (e1 %s %s %s %s)" % (v1["e1_a"], v1["e1_d"], v1["e1_s"], v1["e1_r"]))
    check(v1["f_on"] == "0" and "l1_pts" not in v1 and v1["m1_src"] == "0", "init: no filter, no matrix, no points")

    s2 = state_of(os.path.join(out, "Presets", "Full _ Test.xpl"))
    v2 = values(s2)
    opts = {p["key"]: p.get("options") for p in si.mp.P}
    o = lambda k: opts[k][int(v2[k])]  # noqa: E731
    check(o("mode") == "Mono", "mono")
    check(v2["a_uni"] == "8" and near(v2["a_det"], 50) and o("a_warp") == "Sync"
          and near(v2["a_wamt"], 40) and near(v2["a_phase"], 90) and near(v2["a_rand"], 0),
          "osc A: 12 voices -> 8, detune, warp, phase")
    check(v2["a_oct"] == "0" and v2["a_semi"] == "-12" and near(v2["a_fine"], 30) and near(v2["a_level"], 50),
          "osc A: one octave down plus 30 cents, level (%s %s %s)" % (v2["a_oct"], v2["a_semi"], v2["a_fine"]))
    check(int(v2["a_table"]) == si.mp.TABLES.index("Vowel") and "a_tfile" not in v2, "osc A: a table not in --tables gets a stand-in by name")
    check(o("b_warp") == "FM" and v2["b_oct"] == "2", "osc B: FM warp, two octaves up")
    check(o("n_type") == "Pink" and near(v2["n_pitch"], 80) and near(v2["n_level"], 30), "noise: pink, pitch, level")
    check(v2["s_on"] == "1" and v2["s_oct"] == "-2" and o("s_direct") == "Direct" and v2["f_rs"] == "0",
          "sub: on, two octaves down, direct")
    check(v2["f_ra"] == "1" and v2["f_rb"] == "1" and v2["f_rn"] == "0", "routing: A (default) and B through the filter")
    check(o("f_type") == "MG LP 24" and near(v2["f_res"], 40) and near(v2["f_drive"], 20),
          "filter: MG LP 24, resonance, drive")
    # Env 1: attack 10 ms, sustain 0.5 -> 25 % gain, attack curve 100 -> +100
    check(near(v2["e1_a"], si.etime_x(0.01)) and near(v2["e1_s"], 25) and near(v2["e1_ac"], 100) and near(v2["e1_dc"], 33.2),
          "Env 1: attack, sustain as gain, curves")
    check(v2["e2_s"] == "0" and near(v2["e2_a"], 0), "Env 2: zero attack and sustain")
    # LFO 3 (source 8) is the first used: Philtre LFO 1, Free, its points (step at 0.25), unipolar -> offset
    check(o("l1_shape") == "Custom" and o("l1_mode") == "Free" and o("l1_sync") == "1/8",
          "Serum LFO 3 -> Philtre LFO 1: points, Free, rate 10.66 -> 1/8 (%s)" % o("l1_sync"))
    pts = [tuple(float(x) for x in p.split(",")) for p in v2["l1_pts"].split("/")]
    check(len(pts) == 4 and pts[0][:2] == (0.0, 1.0) and abs(pts[1][0] - 0.25) < 1e-6 and pts[2][1] == -1.0
          and 0.25 < pts[2][0] < 0.26 and abs(pts[1][2] - 0.8) < 1e-6 and pts[3][:2] == (0.998, -1.0),
          "LFO points: y flipped, the step kept just after 0.25, the end step kept, curve 0.9 -> 0.8 (%s)" % v2["l1_pts"])
    m = {i: (o("m%d_src" % i), o("m%d_dst" % i), float(v2["m%d_amt" % i])) for i in range(1, 17)}
    rng = si.CUTOFF_RANGE_SEMIS
    check(m[1][:2] == ("LFO 1", "Cutoff") and abs(m[1][2] - 50 * rng / 192) < 0.01, "slot 1: LFO -> cutoff, scaled to 96 semitones")
    base = si.cutoff_x(si.serum_hz(0.5)) + 0.25 * rng * 100 / si.CUTOFF_X_SEMIS
    check(near(v2["f_cut"], base, 0.05), "a unipolar LFO raises the cutoff by half its amount (%s, %.2f)" % (v2["f_cut"], base))
    check(m[2][:2] == ("Env 2", "A Pos") and abs(m[2][2] - 30) < 0.01, "slot 2: bipolar Env 2 -> A Pos, full amount")
    check(near(v2["a_pos"], 50 - 15, 0.1), "and the bipolar envelope lowers the position by half (%s)" % v2["a_pos"])
    check(m[3][:2] == ("LFO 1", "A Pan") and abs(m[3][2] - 20) < 0.01, "slot 3: bipolar LFO -> pan (range 200)")
    check(m[4][:2] == ("Velocity", "Drive") and abs(m[4][2] - 10) < 0.01, "slot 4: --source 23=Velocity")
    check(m[5][0] == "Off", "no more slots")
    check("Serum source #22 not known" in report, "an unknown source is named in the report")
    fx = [(o("fx%d_type" % i), v2["fx%d_mix" % i]) for i in range(1, 9)]
    check([f[0] for f in fx[:4]] == ["Distortion", "Delay", "Reverb", "Off"], "effects in order, Bode left out: %s" % fx[:4])
    check(int(float(v2["fx1_p2"]) / 100 * 9) == 3 and near(v2["fx1_p1"], 60) and near(v2["fx2_p2"], 45),
          "distortion mode Diode, drive; delay feedback")
    check("effect Bode" in report and "effect rack 2" in report, "the report names the Bode and the second rack")
    check("macro 1 (CUT) is set but drives nothing" in report, "an unlinked macro is reported")

    s3 = state_of(os.path.join(out, "Presets", "Three Osc.xpl"))
    v3 = values(s3)
    o3 = lambda k: opts[k][int(v3[k])]  # noqa: E731
    check(v3["a_on"] == "1" and v3["b_on"] == "1" and "oscillators B and C play on Philtre's A and B" in report,
          "osc A off: B and C play on Philtre's A and B")
    check("Granular oscillator" in report, "a granular oscillator is reported")
    check([o3("m%d_src" % i) for i in range(1, 7)] == ["LFO 1", "LFO 2", "LFO 3", "LFO 4", "Macro 1", "Off"] and
          "LFO 5 (Philtre has four LFOs)" in report, "four LFOs kept, the fifth reported")
    check(o3("m1_dst") == "A Pos", "Serum B's position follows it to Philtre's A")
    check(len(v3["l1_pts"].split("/")) == 16, "31 LFO points -> 16")
    check(o3("l2_sync") == "Off" and near(v3["l2_rate"], si.lrate_x(1.0068), 0.01) and o3("l3_sync") == "1/1"
          and o3("l4_sync") == "1/4", "LFO rates: free 1 Hz stays 1 Hz, 1.627 synced is 1 bar, the default is 1/4 (%s %s %s)"
          % (v3["l2_rate"], o3("l3_sync"), o3("l4_sync")))
    check(o3("m5_aux") == "LFO 1" and o3("m5_dst") == "B Level" and near(v3["mac1"], 55),
          "source 25 is Macro 1 (its value carried), aux LFO 1, Serum C's level on Philtre's B")
    check("no such destination" in report, "an Env destination is reported")
    check("macro 3 (WIDE)" in report, "macro 3 reported")

    # unknown --source name refused
    r = subprocess.run([sys.executable, os.path.join(HERE, "..", "tools", "serum2_import.py"), "-o", out, "--source",
                        "25=Banana", src], capture_output=True, text=True)
    check(r.returncode != 0 and "--source" in (r.stderr + r.stdout), "--source with an unknown name is refused")

    if out_states:
        with open(out_states, "w") as f:
            for st in (s1, s2, s3):
                f.write(st + "\n")
    print("%d failure(s)" % fails)
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
