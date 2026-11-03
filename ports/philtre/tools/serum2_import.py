#!/usr/bin/env python3
"""Serum 2 presets (.SerumPreset) -> Philtre presets (.xpl) for MPC. Original code (GPL-3.0-or-later); nothing from
Xfer Records. Runs on a computer, from a checkout of this repository (it reads Philtre's parameter list from
../vst/make_params.py and the .xpl writer from ../../../tools/xpl.py).

    python3 serum2_import.py [options] <preset or folder> ...

    -o DIR            output folder (default philtre-import): DIR/Presets/<name>.xpl, DIR/import-report.txt
    --tables DIR      your Serum wavetable folder (Serum 2's "Tables"): each table a preset uses is copied to
                      DIR/wavetables/serum2/, and the preset asks Philtre for it by name (a built-in table stands in
                      while the file is not in Philtre's wavetables/ folder)
    --source ID=NAME  name a Serum mod source by its number (see the report), e.g. --source 21=Random
    --synths PATH     the MPC's Synths folder, for the .xpl's plugin path (default /sdcard/Synths)
    --dump            print a preset's fields ("path = value") instead of converting it
    -q                no report on the terminal (it is still written to the file)

Reading: a .SerumPreset is b"XferJson\\0", a uint64 LE length and a JSON header (name, author, tags), a uint32 LE size of
the payload once decompressed, a uint32 LE format version (2), then a Zstandard frame of CBOR. Zstandard comes from
Python 3.14's `compression.zstd` or the `zstandard` package (`pip install zstandard`); CBOR is decoded here.

Philtre is not Serum: two wavetable oscillators (Serum has three), one filter (two), four LFOs (ten), 16 matrix slots
(64), eight effect slots, and its own effects. What has no counterpart is listed in the report ("x"), and what is
converted with a unit that was not measured is marked "~". The units come from three places, marked in the tables
below: [m] measured on Serum 2.1.5 (a DawDreamer probe published with serum2-preset-loader, MIT), [f] read off preset
files, [s] seen on Serum 2's Init screen, [g] guessed (Serum 1 behaviour or the default lining up). A [g] is a calibration to do: save a Serum preset
with only that control moved and compare (TESTING.md says how).
"""
import json
import math
import os
import re
import shutil
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
PORT = os.path.dirname(HERE)
sys.path.insert(0, os.path.join(PORT, "vst"))
sys.path.insert(0, os.path.join(PORT, "..", "..", "tools"))
import make_params as mp  # noqa: E402  (Philtre's parameters)
import xpl  # noqa: E402            (the .xpl writer)

# ===================================================================================== reading .SerumPreset
MAGIC = b"XferJson\0"
MAX_FILE = 64 << 20
MAX_CBOR = 256 << 20


class PresetError(ValueError):
    pass


def _zstd(data, size):
    try:
        from compression import zstd          # Python 3.14+
        try:
            out = zstd.decompress(data)
        except zstd.ZstdError as e:
            raise PresetError("payload does not decompress: %s" % e) from None
    except ImportError:
        try:
            import zstandard
        except ImportError:
            raise PresetError("Serum 2 presets are Zstandard-compressed: use Python 3.14+, or 'pip install zstandard'") \
                from None
        try:
            out = zstandard.ZstdDecompressor().decompress(data, max_output_size=size)
        except zstandard.ZstdError as e:
            raise PresetError("payload does not decompress: %s" % e) from None
    if len(out) != size:
        raise PresetError("payload is %d bytes, the header says %d" % (len(out), size))
    return out


class _Cbor:
    def __init__(self, b):
        self.b, self.i, self.items = b, 0, 0

    def take(self, n):
        if n > len(self.b) - self.i:
            raise PresetError("CBOR payload cut short")
        self.i += n
        return self.b[self.i - n:self.i]

    def arg(self, info):
        if info < 24:
            return info
        if 24 <= info <= 27:
            return int.from_bytes(self.take(1 << (info - 24)), "big")
        raise PresetError("CBOR: unsupported length code %d" % info)

    def item(self, depth=0):
        if depth > 64:
            raise PresetError("CBOR nested too deep")
        self.items += 1
        if self.items > 4_000_000:
            raise PresetError("CBOR has too many items")
        ib = self.take(1)[0]
        major, info = ib >> 5, ib & 31
        if major == 7:
            simple = {20: False, 21: True, 22: None, 23: None}
            if info in simple:
                return simple[info]
            fmt = {25: ">e", 26: ">f", 27: ">d"}.get(info)
            if not fmt:
                raise PresetError("CBOR: unsupported simple value %d" % info)
            return struct.unpack(fmt, self.take(struct.calcsize(fmt)))[0]
        if info == 31:
            raise PresetError("CBOR: indefinite lengths are not used by presets")
        n = self.arg(info)
        if major == 0:
            return n
        if major == 1:
            return -1 - n
        if major in (2, 3):
            s = self.take(n)
            return bytes(s) if major == 2 else s.decode("utf-8", "replace")
        if n > len(self.b) - self.i:
            raise PresetError("CBOR container longer than the payload")
        if major == 4:
            return [self.item(depth + 1) for _ in range(n)]
        if major == 5:
            d = {}
            for _ in range(n):
                k = self.item(depth + 1)
                d[k if isinstance(k, str) else str(k)] = self.item(depth + 1)
            return d
        return self.item(depth + 1)            # a tag: keep the value


def read_preset(path):
    """-> (JSON header dict, payload tree dict)."""
    with open(path, "rb") as f:
        raw = f.read(MAX_FILE + 1)
    if len(raw) > MAX_FILE:
        raise PresetError("file too large")
    if not raw.startswith(MAGIC):
        raise PresetError("not a Serum 2 preset (Serum 1 .fxp presets are not supported)")
    p = len(MAGIC)
    if len(raw) < p + 8:
        raise PresetError("file cut short")
    (jl,) = struct.unpack_from("<Q", raw, p)
    p += 8
    if jl > len(raw) - p - 8:
        raise PresetError("bad header length")
    try:
        meta = json.loads(raw[p:p + jl].decode("utf-8"))
    except ValueError as e:
        raise PresetError("bad JSON header: %s" % e) from None
    p += jl
    size, ver = struct.unpack_from("<II", raw, p)
    if size > MAX_CBOR:
        raise PresetError("payload too large")
    if ver != 2:
        print("note: %s: payload format %d (2 expected), trying anyway" % (path, ver), file=sys.stderr)
    tree = _Cbor(_zstd(raw[p + 8:], size)).item()
    if not isinstance(tree, dict):
        raise PresetError("payload is not a map")
    return meta if isinstance(meta, dict) else {}, tree


def flatten(tree, prefix=""):
    if isinstance(tree, dict):
        for k in sorted(tree):
            yield from flatten(tree[k], prefix + "." + k if prefix else k)
    elif isinstance(tree, list) and tree and not all(isinstance(x, (int, float)) for x in tree):
        for i, v in enumerate(tree):
            yield from flatten(v, "%s[%d]" % (prefix, i))
    else:
        yield prefix, tree


# ===================================================================================== Serum 2 fields
# A module's "plainParams" holds only what differs from Serum's defaults (or the string "default"): absent = default.
def pp(node):
    """The plainParams of a module node (a dict), {} when "default" or missing."""
    if not isinstance(node, dict):
        return {}
    q = node.get("plainParams")
    return q if isinstance(q, dict) else {}


def num(d, key, default):
    v = d.get(key, default)
    return float(v) if isinstance(v, (int, float)) and not isinstance(v, bool) and math.isfinite(v) else default


def seconds_from_norm(n):          # [m] Serum's time knobs store seconds = 32 n^5
    return 32.0 * n ** 5


ENV_DEF = {"kParamAttack": None, "kParamDecay": 1.0, "kParamSustain": 1.0, "kParamRelease": seconds_from_norm(0.215),
           "kParamHold": 0.0, "kParamCurve1": 50.0, "kParamCurve2": 66.6, "kParamCurve3": 66.6}   # [m] times; [f] curves
ENV1_ATTACK, ENVN_ATTACK = seconds_from_norm(0.1256), seconds_from_norm(0.11)                       # [m] 1 ms, 0.5 ms
OSC_LEVEL_DEF = 0.75               # [g] Serum's oscillator level default (75 %)
NOISE_LEVEL_DEF = 0.25             # [g]
FREQ_LO, FREQ_HI = 8.1758, 22050.0   # [m] filter cutoff: stored 0..1 = FREQ_LO * (FREQ_HI / FREQ_LO)^n Hz, default 0.5
CUTOFF_RANGE_SEMIS = 12 * math.log2(FREQ_HI / FREQ_LO)   # 137 semitones: a 100 % cutoff modulation in Serum
CUTOFF_X_SEMIS = 12 * math.log2(1000)                    # Philtre's cutoff knob spans 120 semitones
PITCH_RANGE_SEMIS = 24.0           # [g] a 100 % pitch modulation of an oscillator
# LFO rate [f] (Serum 2.0.16 presets saved by the user, 2026-11-03): stored in Hz when free (1.0 Hz -> 1.0068); when
# synced the same knob value picks the division: 1/8 -> 10.66, 1 bar -> 1.627, so beats ~ LFO_SYNC_K / stored (a fit on
# two points; the knob snaps, so each division covers a range). [s] the Init LFO is synced at 1/4: the default is put
# where that fit gives 1/4 [g] (Serum's own default number is not known yet).
LFO_SYNC_K = 6.0                   # 1/8 -> 5.3, 1/4 -> 6.2, 1 bar -> 6.5: 6 picks the right one for all three
LFO_RATE_DEF = 6.2                 # [s] Init's LFO in Hz mode shows 6.2 Hz (1/4 synced)


def serum_hz(n):
    return FREQ_LO * (FREQ_HI / FREQ_LO) ** min(max(n, 0.0), 1.0)


# mod sources [f]: the numbers of a ModSlot's "source" (and aux) that are known; --source adds more
SOURCES = {1: "Mod Whl", 2: "Env 1", 3: "Env 2", 4: "Env 3", 5: "Env 4"}
SOURCES.update({6 + i: "LFO %d" % (i + 1) for i in range(10)})      # Serum's LFO 1-10; four are kept
# [f] a preset with one slot per source (Serum 2.0.16, 2026-11-03); Macro 3-8 taken to follow Macro 1-2 [g]
SOURCES.update({16: "Velocity", 17: "Note", 18: "Aftertouch", 19: "Aftertouch", 33: "Bend"})   # 19: poly aftertouch
SOURCES.update({25 + i: "Macro %d" % (i + 1) for i in range(8)})
SOURCE_ALIASES = {"modwheel": "Mod Whl", "mod wheel": "Mod Whl", "velo": "Velocity", "vel": "Velocity",
                  "pitchbend": "Bend", "pitch bend": "Bend", "at": "Aftertouch", "noteon": "Note", "key": "Note"}

# ===================================================================================== Philtre side
P = {p["key"]: p for p in mp.P}
SAVED = [p for p in mp.P if not p.get("virtual") and not p.get("momentary")]


def etime_x(sec):                  # Philtre: map_etime(x) = 0.0005 * 20000^(x/100) s
    return 0.0 if sec <= 0.0005 else 100 * math.log(sec / 0.0005) / math.log(20000)


def cutoff_x(hz):                  # Philtre: map_freq(x) = 20 * 1000^(x/100) Hz
    return 100 * math.log(max(hz, 20.0) / 20) / math.log(1000)


def lrate_x(hz):                   # Philtre: map_lrate(x) = 0.02 * 2000^(x/100) Hz
    return 100 * math.log(max(hz, 0.02) / 0.02) / math.log(2000)


def glide_x(sec):                  # Philtre: map_glide(x) = 0.002 * 1000^(x/100) s
    return 0.0 if sec <= 0 else 100 * math.log(max(sec, 0.002) / 0.002) / math.log(1000)


LSYNC_BEATS = [None, 16, 8, 4, 2, 4 / 3, 1, 2 / 3, 0.5, 1 / 3, 0.25, 1 / 6, 0.125]   # Philtre's sync list (index 0 = Off)


class Patch:
    """Philtre's values (defaults first) and the report."""

    def __init__(self):
        self.v = {p["key"]: p["default"] for p in mp.P}
        self.pts = {}
        self.tfile = {}
        self.notes = []          # ("~" approximated / "x" left out, text)

    def set(self, key, value):
        p = P[key]
        if "options" in p:
            if isinstance(value, str):
                value = p["options"].index(value)
            value = max(0, min(len(p["options"]) - 1, int(value)))
        else:
            value = max(p["min"], min(p["max"], float(value)))
            if p.get("int"):
                value = int(round(value))
        self.v[key] = value

    def get(self, key):
        return self.v[key]

    def approx(self, text):
        self.notes.append(("~", text))

    def drop(self, text):
        self.notes.append(("x", text))

    def state(self):
        out = ["ph1"]
        for p in SAVED:
            x = self.v[p["key"]]
            out.append("%s=%s" % (p["key"], ("%d" % x) if isinstance(x, int) else ("%.5g" % x)))
        for l in range(1, 5):
            if l in self.pts:
                out.append("l%d_pts=%s" % (l, "/".join("%.4g,%.3g,%.3g" % t for t in self.pts[l])))
        for o, rel in sorted(self.tfile.items()):
            out.append("%s_tfile=%s" % (o, rel))
        return ";".join(out)


# ===================================================================================== the conversion
TABLE_WORDS = [  # a built-in table that stands in for a Serum wavetable, by words in its file name
    ("PWM", ("pwm", "pulse")), ("Vowel", ("vowel", "formant", "vox", "voice", "choir", "speak")),
    ("Harmonics", ("harm", "additive", "partial", "spectral", "overtone")), ("Sync", ("sync",)),
    ("FM", ("fm", "bell", "dx")), ("Fold", ("fold", "dist", "drive", "grit")), ("Organ", ("organ", "drawbar")),
    ("Digital", ("digital", "bit", "chip", "8bit", "lofi")), ("Reso", ("reso", "filter", "sweep")),
    ("Comb", ("comb", "flange", "phase")), ("Random", ("noise", "random", "chaos")),
    ("Classic", ("analog", "basic", "default", "shape", "saw", "square", "sine", "tri")),
]


def builtin_table(path):
    words = re.findall(r"[a-z0-9]+", path.lower())
    name = os.path.basename(path).lower()
    for table, keys in TABLE_WORDS:
        for k in keys:
            if k in words or (len(k) > 3 and k in name):
                return table
    return "Classic"


WARP_RULES = [  # Serum warp menu value -> Philtre warp, by words in it ([f]: "kSync", "kPD_OSC")
    ("Sync", ("sync",)), ("PWM", ("pwm",)), ("Bend -", ("bendminus", "bendneg", "bend-", "bendm")),
    ("Bend +", ("bend", "pd", "phasedist")), ("Asym -", ("asymminus", "asymneg", "asym-", "asymm")),
    ("Asym +", ("asym",)), ("Flip", ("flip",)), ("Mirror", ("mirror",)), ("Quantize", ("quant",)),
    ("FM", ("fm",)), ("AM", ("am",)), ("RM", ("rm", "ring")),
]


def warp_of(menu):
    s = re.sub(r"^k", "", str(menu)).lower()
    flat = re.sub(r"[^a-z0-9+-]", "", s)
    words = re.findall(r"[a-z]+", s)
    for w, keys in WARP_RULES:
        for k in keys:
            if (len(k) <= 2 and k in words) or (len(k) > 2 and k in flat):
                return w
    return None


def filter_of(ftype):
    """Serum filter type name ([f]: "LBH12", "MgL6") -> Philtre filter type; None when only a guess."""
    s = re.sub(r"^k", "", str(ftype)).lower()
    pole = int((re.findall(r"\d+", s) or ["12"])[0])
    two = "24" if pole >= 18 else "12"
    if "comb" in s:
        return "Comb -" if any(k in s for k in ("neg", "minus", "-")) else "Comb +"
    if "phas" in s:
        return "Phaser"
    if any(k in s for k in ("form", "vowel")):
        return "Formant"
    if any(k in s for k in ("notch", "reject", "br")):
        return "Notch"
    if s.startswith(("mg", "ladder", "moog", "german", "french")):
        return "MG LP " + two
    if s.startswith("lbh") or s.startswith(("l", "lp", "low")):
        return "LP " + two
    if s.startswith(("h", "hp", "high")):
        return "HP " + two
    if s.startswith(("b", "bp", "band", "peak")):
        return "BP 12"
    return None


NOISE_WORDS = [("Pink", ("pink",)), ("Brown", ("brown", "red", "rumble")), ("Crackle", ("crackle", "vinyl", "crack", "pop")),
               ("Hiss", ("hiss", "air", "tape", "breath", "wind")), ("Digital", ("digital", "bit", "glitch", "data")),
               ("White", ("white",))]


def noise_of(path):
    s = path.lower()
    for t, keys in NOISE_WORDS:
        if any(k in s for k in keys):
            return t, True
    return "White", False


class Converter:
    def __init__(self, tree, meta, opts, name):
        self.t, self.meta, self.o, self.name = tree, meta, opts, name
        self.ph = Patch()
        self.osc_map = {}        # Serum oscillator 0..2 -> "a" / "b"
        self.lfo_map = {}        # Serum LFO 0..9 -> Philtre LFO 1..4
        self.macro_map = {}      # Serum macro 0..7 -> Philtre macro 1..4
        self.offsets = {}        # Philtre key -> base offset from unipolar / bipolar modulation
        self.tables_used = []    # (source path, destination rel)

    def mod(self, name):
        return self.t.get(name) if isinstance(self.t.get(name), dict) else {}

    # ---------------------------------------------------------------- global
    def do_global(self):
        g = pp(self.mod("Global0"))
        ph = self.ph
        if num(g, "kParamMonoToggle", 0) >= 0.5:
            legato = any(num(g, k, 0) >= 0.5 for k in g if "legato" in k.lower())
            ph.set("mode", "Legato" if legato else "Mono")
        poly = num(g, "kParamPolyCount", 8)
        if ph.get("mode") == 0:
            ph.set("voices", min(8, poly))
            if poly > 8:
                ph.approx("%d voices of polyphony -> 8" % poly)
        for k in g:
            kl = k.lower()
            if "porta" in kl and "time" in kl:
                ph.set("glide", glide_x(num(g, k, 0)))
                ph.approx("glide time (unit not measured)")
            elif "bend" in kl and "up" in kl:
                ph.set("bend", num(g, k, 2))
        if "kParamMasterVolume" in g:
            ph.approx("master volume left at Philtre's default (Serum's unit not measured)")

    # ---------------------------------------------------------------- oscillators
    def osc_enabled(self, i):
        return num(pp(self.mod("Oscillator%d" % i)), "kParamEnable", 1.0 if i == 0 else 0.0) >= 0.5

    def do_oscs(self):
        on = [i for i in range(3) if self.osc_enabled(i)]
        for i, o in zip(on, ("a", "b")):
            self.osc_map[i] = o
        for i in on[2:]:
            self.ph.drop("oscillator %s (Philtre has two wavetable oscillators)" % "ABC"[i])
        if on[:2] != [0, 1][:len(on[:2])]:
            self.ph.approx("oscillators %s play on Philtre's %s" % (
                " and ".join("ABC"[i] for i in on[:2]), " and ".join(self.osc_map[i].upper() for i in on[:2])))
        self.ph.set("a_on", "On" if len(on) > 0 else "Off")
        self.ph.set("b_on", "On" if len(on) > 1 else "Off")
        for i, o in self.osc_map.items():
            self.do_osc(i, o)

    def do_osc(self, i, o):
        ph, node = self.ph, self.mod("Oscillator%d" % i)
        q = pp(node)
        L = "ABC"[i]
        kind = str(q.get("kParamType", "kOsc_WT"))
        wt = node.get("WTOsc%d" % i) if isinstance(node.get("WTOsc%d" % i), dict) else {}
        w = pp(wt)
        if kind != "kOsc_WT":
            ph.drop("oscillator %s is a %s oscillator: a built-in wavetable plays instead" % (L, kind[5:] or kind))
        path = str(wt.get("relativePathToWT", "")) if kind == "kOsc_WT" else ""
        if any("embed" in k.lower() for k in wt):
            ph.drop("oscillator %s: the wavetable stored inside the preset is not exported" % L)
        stand_in = builtin_table(path) if path else "Classic"
        ph.set(o + "_table", mp.TABLES.index(stand_in))
        if path:
            rel = self.table_file(path)
            if rel:
                ph.tfile[o] = rel
                ph.approx("oscillator %s: table %s (copied; %s plays until it is in wavetables/)" % (L, path, stand_in))
            else:
                ph.approx("oscillator %s: table %s -> built-in %s" % (L, path, stand_in))
        ph.set(o + "_pos", (num(w, "kParamTablePos", 1.0) - 1) / 255 * 100)                 # [m] 1..256
        if "kParamWarpMenu" in w:
            warp = warp_of(w["kParamWarpMenu"])
            if warp:
                ph.set(o + "_warp", warp)
                ph.set(o + "_wamt", num(w, "kParamWarp", 0) * 100)                           # [f] 0..1
                if warp in ("FM", "AM", "RM") and len(self.osc_map) < 2:
                    ph.approx("oscillator %s: %s needs a second oscillator" % (L, warp))
            else:
                ph.drop("oscillator %s: warp %s" % (L, w["kParamWarpMenu"]))
        if "kParamWarpMenu2" in w or "kParamWarp2" in w:
            ph.drop("oscillator %s: second warp" % L)
        ph.set(o + "_phase", num(w, "kParamInitialPhase", 180.0))                             # [m] degrees
        ph.set(o + "_rand", num(w, "kParamRandomPhase", 100.0))                               # [m] percent
        semis = num(q, "kParamOctave", 0) * 12 + num(q, "kParamPitch", 0) + num(q, "kParamCoarsePit", 0)
        cents = num(q, "kParamFine", 0)
        total = semis + cents / 100
        octv = max(-4, min(4, int(round(total / 12)))) if abs(total) > 12 else 0
        rest = total - octv * 12
        ph.set(o + "_oct", octv)
        ph.set(o + "_semi", int(math.floor(rest + 0.5)))
        ph.set(o + "_fine", (rest - math.floor(rest + 0.5)) * 100)
        if "kParamPitch" in q or "kParamCoarsePit" in q:
            ph.approx("oscillator %s: semitone units [g]" % L)
        ph.set(o + "_level", num(q, "kParamVolume", OSC_LEVEL_DEF) * 100)                   # [g] 0..1
        ph.set(o + "_pan", num(q, "kParamPan", 0))                                            # [f] -100..100
        uni = num(q, "kParamUnison", 1)                                                       # [m] voices
        ph.set(o + "_uni", min(8, uni))
        if uni > 8:
            ph.approx("oscillator %s: %d unison voices -> 8" % (L, uni))
        ph.set(o + "_det", num(q, "kParamDetune", 0.25) * 100)                                # [m] 0..1, default 0.25
        for k in q:
            if "blend" in k.lower():
                ph.set(o + "_blend", num(q, k, 75))
            elif k == "kParamDetuneWid":
                ph.set(o + "_width", num(q, k, 100))
                ph.approx("oscillator %s: unison width unit [g]" % L)

    def table_file(self, path):
        """Copy the Serum table from --tables into OUT/wavetables/serum2/; -> its path under wavetables/, or None."""
        if not self.o.get("tables_index"):
            return None
        rel = path.replace("\\", "/").lstrip("/")
        idx = self.o["tables_index"]
        src = idx["rel"].get(rel.lower()) or idx["base"].get(os.path.basename(rel).lower())
        if not src:
            return None
        base = re.sub(r"[;=]", "_", os.path.basename(src))
        dest_rel = "serum2/" + base
        used = self.o["tables_copied"]
        if used.get(dest_rel, src) != src:              # another table with the same file name
            parent = re.sub(r"[;=]", "_", os.path.basename(os.path.dirname(src)))
            dest_rel = "serum2/%s - %s" % (parent, base)
        if dest_rel not in used:
            used[dest_rel] = src
            dst = os.path.join(self.o["out"], "wavetables", dest_rel)
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            shutil.copyfile(src, dst)
        return dest_rel

    # ---------------------------------------------------------------- sub, noise, routing
    def do_sub_noise(self):
        ph = self.ph
        node = self.mod("Oscillator4")
        q = pp(node)
        if num(q, "kParamEnable", 0) >= 0.5:
            ph.set("s_on", "On")
            ph.set("s_level", num(q, "kParamVolume", OSC_LEVEL_DEF) * 100)
            ph.set("s_pan", num(q, "kParamPan", 0))
            if "kParamOctave" in q:
                ph.set("s_oct", num(q, "kParamOctave", -1))
            sub = pp(node.get("SubOsc4"))
            shape = " ".join(str(v) for v in sub.values() if isinstance(v, str)).lower()
            for name, keys in (("Triangle", ("tri",)), ("Saw", ("saw",)), ("Square", ("square", "sqr")),
                               ("Pulse 25", ("pulse", "pw")), ("Sine", ("sin",))):
                if any(k in shape for k in keys):
                    ph.set("s_shape", name)
                    break
            ph.approx("sub: shape and octave defaults [g]")
        node = self.mod("Oscillator3")
        q = pp(node)
        if num(q, "kParamEnable", 0) >= 0.5:
            ph.set("n_on", "On")
            ph.set("n_level", num(q, "kParamVolume", NOISE_LEVEL_DEF) * 100)
            ph.set("n_pan", num(q, "kParamPan", 0))
            nz = node.get("NoiseOsc3") if isinstance(node.get("NoiseOsc3"), dict) else {}
            ph.set("n_pitch", num(pp(nz), "kParamColor", 0.5) * 100)
            sample = str(nz.get("relativePathToNoiseSample", ""))
            t, sure = noise_of(sample)
            ph.set("n_type", t)
            ph.approx("noise: sample %s -> %s noise" % (sample or "(default)", t) if sample else "noise: White")
            if any("embed" in k.lower() for k in nz):
                ph.drop("noise: the sample stored inside the preset")
        # routing [f]: RoutingSlot0..4 = A, B, C, noise, sub; absent = [s] Init: A through filter 1, the others not
        for slot, (key, osc) in enumerate((("f_ra", 0), ("f_rb", 1), (None, 2), ("f_rn", 3), ("f_rs", 4))):
            dest = str(pp(self.mod("RoutingSlot%d" % slot)).get("kParamRoutingDest", "" if slot else "Filter"))
            through = "filter" in dest.lower()
            if osc < 3:
                if osc not in self.osc_map:
                    continue
                key = "f_r" + self.osc_map[osc]
            ph.set(key, "On" if through else "Off")
            if slot == 4 and dest and not through:
                ph.set("s_direct", "Direct")
            if "2" in dest:
                ph.approx("%s routed to filter 2 -> Philtre's filter" % ("A", "B", "C", "noise", "sub")[slot])

    # ---------------------------------------------------------------- filter
    def do_filter(self):
        ph = self.ph
        f0, f1 = pp(self.mod("VoiceFilter0")), pp(self.mod("VoiceFilter1"))
        on0, on1 = num(f0, "kParamEnable", 0) >= 0.5, num(f1, "kParamEnable", 0) >= 0.5
        q = f0 if on0 or not on1 else f1
        if on0 and on1:
            ph.drop("filter 2 (Philtre has one filter)")
        elif on1:
            ph.approx("filter 2 plays as Philtre's filter")
        if not (on0 or on1):
            return
        ph.set("f_on", "On")
        ftype = q.get("kParamType")
        ft = filter_of(ftype) if ftype is not None else None
        if ft:
            ph.set("f_type", ft)
        else:                                                   # [s] Init: MG Low 12
            ph.set("f_type", "MG LP 12")
            if ftype is not None:
                ph.approx("filter type %s -> MG LP 12" % ftype)
        if ftype is not None and str(ftype).lower().lstrip("k").startswith("lbh"):
            ph.approx("filter %s (low-band-high morph) -> %s" % (ftype, ft))
        ph.set("f_cut", cutoff_x(serum_hz(num(q, "kParamFreq", 0.5))))                       # [m]
        ph.set("f_res", num(q, "kParamReso", 10.0))                                           # [m] percent
        ph.set("f_drive", num(q, "kParamDrive", 0))                                           # [g] percent
        ph.set("f_mix", num(q, "kParamWet", 100))                                             # [g] percent
        ph.set("f_env", 0)
        for k in q:
            if "keytrack" in k.lower() or "kbd" in k.lower():
                ph.set("f_key", num(q, k, 0))
        if num(q, "kParamVar", 0) not in (0.0,):
            ph.drop("filter: the type's variable control")
        if num(q, "kParamStereo", 0) not in (0.0,):
            ph.drop("filter: stereo spread")

    # ---------------------------------------------------------------- envelopes
    def do_envs(self):
        ph = self.ph
        for e in range(4):
            q = pp(self.mod("Env%d" % e))
            k = "e%d_" % (e + 1)
            a = num(q, "kParamAttack", ENV1_ATTACK if e == 0 else ENVN_ATTACK)
            ph.set(k + "a", etime_x(a))
            ph.set(k + "d", etime_x(num(q, "kParamDecay", 1.0)))
            ph.set(k + "s", 100 * num(q, "kParamSustain", 1.0) ** 2)                         # [m] gain = stored^2
            ph.set(k + "r", etime_x(num(q, "kParamRelease", ENV_DEF["kParamRelease"])))
            ph.set(k + "h", etime_x(num(q, "kParamHold", 0)) if num(q, "kParamHold", 0) > 0 else 0)
            for c, pk in ((1, "ac"), (2, "dc"), (3, "rc")):
                ph.set(k + pk, (num(q, "kParamCurve%d" % c, ENV_DEF["kParamCurve%d" % c]) - 50) * 2)   # [g] 50 = straight
            if num(q, "kParamDelay", 0) > 0:
                ph.drop("Env %d: delay" % (e + 1))
            if num(q, "kParamAttack", 0) > 10 or num(q, "kParamDecay", 0) > 10 or num(q, "kParamRelease", 0) > 10:
                ph.approx("Env %d: times over 10 s are cut to 10 s" % (e + 1))

    # ---------------------------------------------------------------- LFOs
    def lfo_points(self, l):
        cd = self.mod("LFO%d" % l).get("curveData")
        if not isinstance(cd, dict):
            return [(0.0, -1.0, 0.0), (0.5, 1.0, 0.0)]          # Serum's default: up and down
        xs, ys, cs = cd.get("xVals") or [], cd.get("yVals") or [], cd.get("curveVals") or []
        n = min(len(xs), len(ys))
        raw = []
        for i in range(n):
            x, y = float(xs[i]), float(ys[i])
            c = float(cs[i]) if i < len(cs) else 0.5
            c = round(max(-1.0, min(1.0, (c - 0.5) * 2)), 3)
            raw.append((min(max(x, 0), 1), 1 - 2 * min(max(y, 0), 1), c if abs(c) >= 0.002 else 0.0))   # [f] y down
        if len(raw) < 2:
            return [(0.0, -1.0, 0.0), (0.5, 1.0, 0.0)]
        if raw[-1][0] >= 1 and abs(raw[-1][1] - raw[0][1]) < 1e-4:
            raw.pop()                                          # Philtre goes back to the first point by itself
        elif raw[-1][0] >= 1:
            raw[-1] = (0.998, raw[-1][1], raw[-1][2])          # a step back to the start
        pts = [(0.0, raw[0][1], raw[0][2])]
        for x, v, c in raw[1:]:
            x = max(x, pts[-1][0] + 0.002)
            if x > 0.999:
                break
            pts.append((x, v, c))
        while len(pts) > mp.MAX_POINTS:                        # keep the points that change the shape most
            def err(i):
                t0, v0, _ = pts[i - 1]
                t1, v1, _ = pts[i + 1] if i + 1 < len(pts) else (1.0, pts[0][1], 0)
                u = (pts[i][0] - t0) / max(t1 - t0, 1e-6)
                return abs(pts[i][1] - (v0 + (v1 - v0) * u))
            del pts[min(range(1, len(pts)), key=err)]
        if len(pts) < 2:
            pts.append((0.5, pts[0][1], 0.0))
        return pts

    def do_lfo(self, l, n):
        ph, node = self.ph, self.mod("LFO%d" % l)
        q = pp(node)
        k = "l%d_" % n
        typ = str(q.get("kParamType", ""))
        if l >= 8 or typ:
            ph.set(k + "shape", "Smooth")
            ph.approx("LFO %d (%s) -> a smooth random LFO" % (l + 1, typ or "chaos"))
        else:
            pts = self.lfo_points(l)
            ph.set(k + "shape", "Custom")
            ph.pts[n] = pts
            cd = node.get("curveData") if isinstance(node.get("curveData"), dict) else {}
            if len(cd.get("xVals") or []) > mp.MAX_POINTS + 1:
                ph.approx("LFO %d: %d points -> %d" % (l + 1, len(cd["xVals"]), len(pts)))
            if any(abs(c) > 0.01 for _, _, c in pts):
                ph.approx("LFO %d: curve bend direction [g]" % (l + 1))
        mode = str(q.get("kParamMode", "")).lower()
        ph.set(k + "mode", "Free" if mode.startswith("free") else "Env" if mode.startswith("env") else "Retrig")   # [s]
        rate = num(q, "kParamRate", LFO_RATE_DEF)
        if num(q, "kParamBeatSync", 1) >= 0.5:
            beats = LFO_SYNC_K / max(rate, 1e-3)                                            # [f] fit
            if num(q, "kParamTriplets", 0) >= 0.5:
                beats *= 2 / 3
            if num(q, "kParamDotted", 0) >= 0.5:
                beats *= 1.5
            best = min(range(1, len(LSYNC_BEATS)), key=lambda i: abs(math.log2(LSYNC_BEATS[i] / beats)))
            ph.set(k + "sync", best)
            if "kParamRate" in q:
                ph.approx("LFO %d: synced rate -> %s (fitted on two Serum divisions)" % (l + 1, mp.LSYNC[best]))
        else:
            ph.set(k + "sync", "Off")
            ph.set(k + "rate", lrate_x(rate))                                                # [f] Hz
            if rate > 40 or rate < 0.02:
                ph.approx("LFO %d: %.3g Hz -> Philtre's 0.02-40 Hz" % (l + 1, rate))
            if "kParamRate" not in q:
                ph.approx("LFO %d: free rate left at Serum's default, which is not known [g]" % (l + 1))
        for key in q:
            if "rise" in key.lower() or ("delay" in key.lower() and num(q, key, 0) > 0):
                ph.drop("LFO %d: rise / delay" % (l + 1))
            elif "phase" in key.lower():
                ph.set(k + "phase", num(q, key, 0) % 360)
                ph.approx("LFO %d: phase unit [g]" % (l + 1))

    # ---------------------------------------------------------------- matrix
    def source_name(self, sid):
        if sid in self.o["sources"]:
            return self.o["sources"][sid]
        return SOURCES.get(sid)

    def philtre_source(self, sid, slot):
        """Serum source number -> Philtre source name, mapping LFOs and macros to free Philtre ones; None if not."""
        name = self.source_name(sid)
        if name is None:
            self.ph.drop("mod slot %d: Serum source #%d not known (name it with --source %d=NAME)" % (slot, sid, sid))
            return None
        m = re.match(r"LFO (\d+)$", name)
        if m:
            l = int(m.group(1)) - 1
            if l not in self.lfo_map:
                if len(self.lfo_map) >= 4:
                    self.ph.drop("mod slot %d: LFO %d (Philtre has four LFOs)" % (slot, l + 1))
                    return None
                self.lfo_map[l] = len(self.lfo_map) + 1
            return "LFO %d" % self.lfo_map[l]
        m = re.match(r"Macro (\d+)$", name)
        if m:
            k = int(m.group(1)) - 1
            if k not in self.macro_map:
                if len(self.macro_map) >= 4:
                    self.ph.drop("mod slot %d: Macro %d (Philtre has four macros)" % (slot, k + 1))
                    return None
                self.macro_map[k] = len(self.macro_map) + 1
            return "Macro %d" % self.macro_map[k]
        if name not in mp.SOURCES:
            self.ph.drop("mod slot %d: source %s" % (slot, name))
            return None
        return name

    def philtre_dest(self, typ, mid, pname):
        """-> (Philtre destination, Serum range in Philtre units per 100 %, Philtre units per 100 %, base key)."""
        osc = self.osc_map.get(mid)
        O = osc.upper() if osc else None
        if typ == "WTOsc" and osc:
            if pname == "kParamTablePos":
                return O + " Pos", 100, 100, osc + "_pos"
            if pname == "kParamWarp":
                return O + " Warp", 100, 100, osc + "_wamt"
        if typ == "Oscillator":
            if osc:
                if pname in ("kParamPitch", "kParamCoarsePit", "kParamOctave", "kParamFine"):
                    rng = {"kParamFine": 2, "kParamOctave": 96}.get(pname, PITCH_RANGE_SEMIS)
                    return O + " Pitch", rng, 24, None
                if pname == "kParamVolume":
                    return O + " Level", 100, 100, osc + "_level"
                if pname == "kParamPan":
                    return O + " Pan", 200, 100, osc + "_pan"
                if pname == "kParamDetune":
                    return O + " Detune", 100, 100, osc + "_det"
            if mid == 3 and pname == "kParamVolume":
                return "Noise Level", 100, 100, "n_level"
            if mid == 4 and pname == "kParamVolume":
                return "Sub Level", 100, 100, "s_level"
            if mid == 4 and pname == "kParamPan":
                return "Sub Pan", 200, 100, "s_pan"
        if typ == "NoiseOsc" and pname == "kParamColor":
            return "Noise Pitch", 100, 100, "n_pitch"
        if typ == "VoiceFilter":
            d = {"kParamFreq": ("Cutoff", CUTOFF_RANGE_SEMIS, 96, "f_cut"), "kParamReso": ("Resonance", 100, 100, "f_res"),
                 "kParamDrive": ("Drive", 100, 100, "f_drive"), "kParamWet": ("Filter Mix", 100, 100, "f_mix")}
            if pname in d:
                return d[pname]
        if typ == "LFO" and pname == "kParamRate" and mid in self.lfo_map:
            return "LFO%d Rate" % self.lfo_map[mid], 100, 100, None
        if typ == "Global" and pname == "kParamMasterVolume":
            return "Amp", 100, 100, None
        return None

    def do_matrix(self):
        ph = self.ph
        slots = []
        for i in range(64):
            node = self.mod("ModSlot%d" % i)
            if not node or not isinstance(node.get("source"), list) or not node.get("destModuleTypeString"):
                continue
            q = pp(node)
            amt = num(q, "kParamAmount", 0)
            if amt == 0:
                continue
            slots.append((i, node, q, amt))
        used = 0
        for i, node, q, amt in slots:
            src = node["source"]
            sid = int(src[0]) if src and isinstance(src[0], (int, float)) else 0
            aux = int(src[1]) if len(src) > 1 and isinstance(src[1], (int, float)) else 0
            typ, mid = str(node.get("destModuleTypeString")), node.get("destModuleID", 0)
            pname = str(node.get("destModuleParamName", ""))
            what = "mod slot %d (%s %s)" % (i + 1, typ, pname.replace("kParam", ""))
            dest = self.philtre_dest(typ, int(mid) if isinstance(mid, (int, float)) else -1, pname)
            if not dest:
                ph.drop(what + ": no such destination in Philtre")
                continue
            sname = self.philtre_source(sid, i + 1)
            if not sname:
                continue
            aname = self.philtre_source(aux, i + 1) if aux else "Off"
            if aname is None:
                ph.approx(what + ": aux source left out")
                aname = "Off"
            if used >= 16:
                ph.drop(what + ": Philtre has 16 matrix slots")
                continue
            dname, rng, scale, base = dest
            bip = num(q, "kParamBipolar", 0) >= 0.5
            lfo = sname.startswith("LFO")
            # Serum: amt * R * (u - bip/2) with u the source 0..1. Philtre: amt' * S * p, an LFO p = 2u - 1, others p = u
            if lfo:
                a2, off = amt * rng / (2 * scale), amt / 100 * rng * (0.5 if not bip else 0.0)
            else:
                a2, off = amt * rng / scale, -amt / 100 * rng * (0.5 if bip else 0.0)
            if abs(a2) > 100:
                ph.approx(what + ": amount %.0f %% -> 100 %% (Philtre's range)" % a2)
                a2 = max(-100, min(100, a2))
            if off and base:   # in the destination's units; f_cut is in percent of 20 Hz..20 kHz (120 semitones)
                self.offsets[base] = self.offsets.get(base, 0) + (off * 100 / CUTOFF_X_SEMIS if base == "f_cut" else off)
            elif off and abs(off) > 1e-6 and lfo:
                ph.approx(what + ": a unipolar LFO plays bipolar (no base to shift)")
            used += 1
            ph.set("m%d_src" % used, sname)
            ph.set("m%d_aux" % used, aname)
            ph.set("m%d_dst" % used, dname)
            ph.set("m%d_amt" % used, a2)
            if node.get("curveData") or any(k not in ("kParamAmount", "kParamBipolar") for k in q):
                ph.approx(what + ": slot curve / output options left out")
        for key, off in self.offsets.items():
            ph.set(key, ph.get(key) + off)

    # ---------------------------------------------------------------- macros
    def do_macros(self):
        for k, n in self.macro_map.items():
            self.ph.set("mac%d" % n, num(pp(self.mod("Macro%d" % k)), "kParamValue", 0))
        for k in range(8):
            if k not in self.macro_map and num(pp(self.mod("Macro%d" % k)), "kParamValue", 0) != 0:
                nm = self.mod("Macro%d" % k).get("name") or "Macro %d" % (k + 1)
                self.ph.drop("macro %d (%s) is set but drives nothing Philtre knows" % (k + 1, nm))

    # ---------------------------------------------------------------- effects
    def do_fx(self):
        ph = self.ph
        rack = self.mod("FXRack0").get("FX")
        for r in (1, 2):
            if self.mod("FXRack%d" % r).get("FX"):
                ph.drop("effect rack %d (Philtre has one rack)" % (r + 1))
        if not isinstance(rack, list):
            return
        slot = 0
        for fx in rack:
            if not isinstance(fx, dict):
                continue
            kind = next((k for k in fx if k.startswith("FX") and isinstance(fx[k], dict)), None)
            if not kind:
                continue
            if slot >= 8:
                ph.drop("effect %s: Philtre has 8 slots" % kind[2:])
                continue
            q = pp(fx[kind])
            got = self.fx_one(kind, q)
            if not got:
                ph.drop("effect %s" % kind[2:])
                continue
            slot += 1
            t, p1, p2, p3, mix = got
            ph.set("fx%d_type" % slot, t)
            for key, val in (("p1", p1), ("p2", p2), ("p3", p3), ("mix", mix)):
                if val is not None:
                    ph.set("fx%d_%s" % (slot, key), val)
            ph.approx("effect %s -> %s (settings [g])" % (kind[2:], t))

    @staticmethod
    def fx_one(kind, q):
        """-> (Philtre effect, P1, P2, P3, mix) in percent (None = Philtre's default), or None."""
        g = lambda k, d=None: num(q, k, d) if d is not None or k in q else None  # noqa: E731
        wet = g("kParamWet", 100)
        k = kind.lower()
        mode = " ".join(str(v) for v in q.values() if isinstance(v, str)).lower()
        if "hyper" in k or "dimension" in k or "ensemble" in k:
            return "Ensemble", g("kParamRate"), g("kParamDetune"), g("kParamDimESize"), wet
        if "dist" in k:
            modes = [("tube", "Tube"), ("soft", "Soft"), ("hard", "Hard"), ("diode", "Diode"), ("sinfold", "Sine Fold"),
                     ("sin", "Sine Fold"), ("fold", "Fold"), ("rect", "Rectify"), ("downs", "Downsample"),
                     ("bit", "Bitcrush"), ("quant", "Bitcrush")]
            m = next((n for key, n in modes if key in mode), "Tube")
            names = ["Tube", "Soft", "Hard", "Diode", "Fold", "Sine Fold", "Rectify", "Downsample", "Bitcrush"]
            return "Distortion", g("kParamDrive", 25), (names.index(m) + 0.5) / len(names) * 100, 100, wet
        if "comp" in k:
            if any(x in q for x in ("kParamGain0", "kParamGain1", "kParamGain2", "kParamRatioBelow")):
                return "Multiband", 60, 50, 50 if num(q, "kParamRatioBelow", 0) > 0 else 0, wet
            thr = num(q, "kParamThreshold", -18)
            ratio = num(q, "kParamRatio", 4)
            return ("Compressor", min(100, max(0, -thr / 40 * 100)),
                    100 * math.log(max(ratio, 1)) / math.log(20), 50, wet)
        if "delay" in k:
            pp_ = "ping" in mode or num(q, "kParamPingPong", 0) >= 0.5
            return "Ping Pong" if pp_ else "Delay", None, g("kParamFeedback"), None, wet
        if "reverb" in k or "verb" in k:
            return ("Plate" if "plate" in mode else "Reverb", g("kParamSize"), g("kParamDamp"),
                    min(100, num(q, "kParamPreDelay", 0) / 150 * 100) if "kParamPreDelay" in q else None, wet)
        if "chorus" in k:
            return "Chorus", None, g("kParamDepth"), None, wet
        if "flang" in k:
            return "Flanger", None, g("kParamDepth"), g("kParamFeedback"), wet
        if "phas" in k:
            return "Phaser", None, g("kParamDepth"), g("kParamFeedback"), wet
        if k in ("fxeq", "fxequalizer") or "eq" == k[2:]:
            return "EQ", None, None, None, None
        if "filter" in k:
            band = 50 if "band" in mode or "bp" in mode else 100 if "high" in mode or "hp" in mode else 0
            return ("Filter", cutoff_x(serum_hz(num(q, "kParamFreq", 0.5))), g("kParamReso"), band, wet)
        return None

    # ---------------------------------------------------------------- all
    def run(self):
        self.do_global()
        self.do_oscs()
        self.do_sub_noise()
        self.do_filter()
        self.do_envs()
        self.do_matrix()
        for l, n in sorted(self.lfo_map.items(), key=lambda x: x[1]):
            self.do_lfo(l, n)
        unused = [l for l in range(10) if l not in self.lfo_map and self.mod("LFO%d" % l).get("curveData")
                  and pp(self.mod("LFO%d" % l))]
        if unused:
            self.ph.notes.append(("~", "LFOs %s are not used by the matrix: left out" %
                                  ", ".join(str(l + 1) for l in unused)))
        self.do_macros()
        self.do_fx()
        for k in ("Arp0", "PitchQuantizer0", "ClipPlayer"):
            if pp(self.mod(k)) and num(pp(self.mod(k)), "kParamEnable", 0) >= 0.5:
                self.ph.drop(k.rstrip("0"))
        return self.ph


# ===================================================================================== files
def vst_info():
    with open(os.path.join(PORT, "vst", "vst.json")) as f:
        return json.load(f)


def safe_name(s):
    s = re.sub(r"[^\w .,()+&'-]", "_", s, flags=re.UNICODE).strip(" .")
    return re.sub(r"\s+", " ", s)[:60] or "Serum preset"


def index_tables(root):
    idx = {"rel": {}, "base": {}}
    for d, dirs, files in os.walk(root):
        dirs[:] = [x for x in dirs if not x.startswith(".")]
        for f in files:
            if f.lower().endswith(".wav"):
                full = os.path.join(d, f)
                rel = os.path.relpath(full, root).replace("\\", "/")
                parts = rel.split("/")
                for i in range(len(parts)):     # "S2 Tables/x.wav" and "x.wav" both find Tables/S2 Tables/x.wav
                    idx["rel"].setdefault("/".join(parts[i:]).lower(), full)
                idx["base"].setdefault(f.lower(), full)
    return idx


def presets_in(paths):
    for p in paths:
        if os.path.isdir(p):
            for d, dirs, files in os.walk(p):
                dirs.sort()
                for f in sorted(files):
                    if f.lower().endswith(".serumpreset"):
                        yield os.path.join(d, f)
        else:
            yield p


def main(argv):
    args, opts = [], {"out": "philtre-import", "sources": {}, "synths": "/sdcard/Synths", "quiet": False,
                      "tables_copied": {}}
    it = iter(argv)
    dump = False
    for a in it:
        if a in ("-o", "--out"):
            opts["out"] = next(it)
        elif a == "--tables":
            opts["tables"] = next(it)
        elif a == "--synths":
            opts["synths"] = next(it).rstrip("/")
        elif a == "--source":
            sid, _, name = next(it).partition("=")
            name = SOURCE_ALIASES.get(name.strip().lower(), name.strip())
            if not sid.strip().isdigit() or not (name in mp.SOURCES or re.match(r"(LFO \d+|Macro [1-8])$", name)):
                sys.exit("--source: expected NUMBER=NAME, NAME one of %s, Macro 1-8, LFO 1-10" % ", ".join(mp.SOURCES[1:]))
            opts["sources"][int(sid)] = name
        elif a == "--dump":
            dump = True
        elif a == "-q":
            opts["quiet"] = True
        elif a in ("-h", "--help"):
            print(__doc__)
            return 0
        else:
            args.append(a)
    if not args:
        sys.exit(__doc__)
    if dump:
        for f in presets_in(args):
            try:
                meta, tree = read_preset(f)
            except (OSError, PresetError) as e:
                print("==", f, "not read:", e)
                continue
            print("==", f, json.dumps(meta)[:300])
            for k, v in flatten(tree):
                if isinstance(v, bytes):
                    v = "<%d bytes>" % len(v)
                elif isinstance(v, list) and len(v) > 12:
                    v = "[%d numbers]" % len(v)
                print(k, "=", json.dumps(v) if isinstance(v, str) else v)
        return 0
    if opts.get("tables"):
        opts["tables_index"] = index_tables(opts["tables"])
    info = vst_info()
    skin = "%s - VST - %s" % (info["vendor"], info["name"])
    so_path = "%s/%s/%s" % (opts["synths"], skin, info["so"])
    uid = int.from_bytes(info["uid"].encode(), "big")
    os.makedirs(os.path.join(opts["out"], "Presets"), exist_ok=True)
    report, n_ok, names = [], 0, set()
    for f in presets_in(args):
        try:
            meta, tree = read_preset(f)
        except (OSError, PresetError) as e:
            report.append("%s\n  x not converted: %s\n" % (f, e))
            continue
        name = str(meta.get("presetName") or tree.get("presetName") or os.path.splitext(os.path.basename(f))[0])
        ph = Converter(tree, meta, opts, name).run()
        state = ph.state()
        if len(state) + 1 > 8192:
            report.append("%s\n  x not converted: state too large\n" % f)
            continue
        base = safe_name(name)
        while base.lower() in names:
            base += "+"
        names.add(base.lower())
        dst = os.path.join(opts["out"], "Presets", base + ".xpl")
        with open(dst, "w", encoding="utf-8") as fh:
            fh.write(xpl.xpl(info["name"], info["vendor"], so_path, uid, info.get("version", 1000),
                             state.encode() + b"\0", preset=name))
        n_ok += 1
        lines = ["%s -> Presets/%s.xpl%s" % (f, base, ("  (by %s)" % meta["presetAuthor"]) if meta.get("presetAuthor") else "")]
        lines += ["  %s %s" % n for n in ph.notes]
        report.append("\n".join(lines) + "\n")
    copied = opts["tables_copied"]
    tail = "%d preset(s) written to %s/Presets" % (n_ok, opts["out"])
    if copied:
        tail += "; %d wavetable(s) copied to %s/wavetables/serum2" % (len(copied), opts["out"])
        if len(copied) > 64:
            tail += " (Philtre lists 64 files: copy fewer)"
    text = "Serum 2 -> Philtre import. ~ approximated or not measured, x left out.\n\n" + "\n".join(report) + "\n" + tail + "\n"
    with open(os.path.join(opts["out"], "import-report.txt"), "w", encoding="utf-8") as fh:
        fh.write(text)
    print(text if not opts["quiet"] else tail)
    return 0 if n_ok else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
