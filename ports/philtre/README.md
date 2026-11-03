# Philtre

An eight-voice wavetable synth for MPC OS standalone devices, with a Serum-style architecture laid out for the MPC Live
II's screen, its four Q-Link knobs and its data wheel. Original code (GPL-3.0-or-later, `LICENSE`). Not affiliated with
Xfer Records: no Serum code, presets, wavetables or artwork. The built-in wavetables are computed when the plugin loads,
the presets are original, and you can load your own `.wav` wavetables.

Status: **0.3.0, offline only** (2026-11-03; the Serum 2 preset import is new in 0.3.0, the envelope curves and shape
editors in 0.2.0). It builds for Gen1 (armv7) and Gen2 (aarch64) and passes the host tests and a sanitizer stress run;
nothing has run on a device yet. See `TESTING.md`.

## Sound engine
Per voice (8 voices, poly, mono or legato, glide, bend range, velocity):
- **Oscillators A and B.** `TABLE` steps through 12 built-in wavetables (64 frames, band-limited per octave) and then your
  own files. `WT POS` moves through the frames. **Unison** up to 8 voices with `DETUNE` (up to +-75 cents), `BLEND` (centre
  against outer voices), `WIDTH` (stereo spread); `PHASE` and `RANDOM` (start phase, spread per note); `PAN`, `LEVEL`,
  octave, semitone, fine. **Warp** modes and an amount: Sync, Bend +/-, PWM, Asym +/-, Flip, Mirror, Quantize, and FM, AM,
  RM from the other oscillator (A from B, or B from A; the modulator may sit at level 0 to be heard only through the other).
- **Sub** (sine, triangle, saw, square, two pulses; -4 to +2 octaves), straight to the output (Direct) or through the filter.
- **Noise** (white, pink, brown, digital sample-and-hold, crackle, hiss), with a pitch (rate or colour) and key tracking.
- **Filter**, stereo: LP 12/24, MG LP 12/24 (saturated transistor ladder), HP 12/24, BP 12, Notch, Comb +/-, Phaser,
  Formant (the cutoff sweeps the vowels); cutoff, resonance, drive, key tracking, an Env 2 amount and a dry/wet mix. Each
  source (A, B, noise, sub) is routed through it or around it.
- **Envelopes:** four, attack-hold-decay-sustain-release (Env 1 is the amp), with a curve on the attack, the decay and
  the release (+: fast then slow, -: slow then fast, 0: straight) and a hold (0 = none).
- **LFOs:** four, ten shapes (sine, triangle, saw up/down, square, S&H, smooth, pump, exponential rise, stairs) or
  **Custom**: 2 to 16 points round the cycle, each with a level and a curve to the next point. Free-running, retriggered
  per note or one-shot (Env mode), 0.02-40 Hz or synced to the MPC tempo, with rise (fade-in) and start phase.
- **Macros:** four knobs, as modulation sources.
- **Modulation matrix:** 16 slots of `Source x Aux x Amount -> Destination`. Sources: Env 1-4, LFO 1-4 (-1..1), Velocity,
  Note, Mod Whl, Aftertouch (channel or poly), Bend, Macro 1-4, Random (per note), Expr (CC 11). Destinations: pitch, amp,
  pan; per oscillator pitch, WT position, warp, level, pan, detune, blend; sub level and pan; noise level and pitch; cutoff,
  resonance, drive, filter mix; LFO rates; and P1 and Mix of each of the 8 FX slots (effects follow the last voice played).
- MIDI: notes, velocity, bend, CC 1, CC 11, sustain, aftertouch, CC 120/121/123. CC 20-35 move the MAIN page's Q-Links (the
  macros first), and NRPN reaches every parameter.

## Effect rack
Eight slots in series. Each slot picks an effect (`EFFECT` list) and has three controls and a mix; on the MPC the four
knobs are **named after the effect** (MPC reads the names from the plugin):

| Effect | P1 | P2 | P3 | P4 |
|---|---|---|---|---|
| Ensemble (detuned multi-voice chorus) | Rate | Depth | Width | Mix |
| Distortion | Drive | Mode (Tube, Soft, Hard, Diode, Fold, Sine Fold, Rectify, Downsample, Bitcrush) | Tone | Mix |
| Flanger, Phaser | Rate | Depth | Feedback | Mix |
| Chorus | Rate | Depth | Delay | Mix |
| Delay, Ping Pong (synced to the tempo) | Time (1/32 to 1/1, triplets, dotted) | Feedback | Tone (dark / thin) | Mix |
| Compressor | Threshold | Ratio | Speed | Mix |
| Multiband (3 bands, downward and upward) | Depth | Time | Upward | Mix |
| Reverb (hall), Plate | Size | Damping | Pre-delay | Mix |
| EQ | Low shelf | Mid frequency | Mid gain | High shelf |
| Filter | Cutoff | Resonance | LP > BP > HP | Mix |

The order is the slot order: to move an effect, pick it in another slot. A slot set to Off costs nothing.

## Your own wavetables
Put `.wav` files in the plugin's `wavetables/` folder (`<Synths folder>/bastienstefani - VST - Philtre/wavetables/`, up to
3 sub-folder levels, 64 files). Frames are 2048 samples (or what a `clm ` chunk says, as many wavetable editors write it),
up to 256 per table; a file shorter than 2048 samples is one single-cycle frame. 16/24/32-bit PCM or 32-bit float, the
left channel of a stereo file. The list is read when the plugin is inserted; a file loads on a background thread when a
TABLE step reaches it, and projects remember it by file name. The installer keeps this folder on an upgrade.

## Shape editors
MPC pages cannot draw a live curve, so the shape of the edited envelope or LFO is shown as **40 bars of 25 heights**: each
bar is a filmstrip (a `meter`) following a parameter the engine sets. The engine recomputes the bars at most every 50 ms
(`PH_DISP_FRAMES`, a compile-time define), only after a change, and tells MPC which ones moved; nothing moves while you
just play, so the screen thread has nothing to redraw.
- **ENV > ENV SHAPE** (press ENV again): `EDIT` picks Env 1-4; ATTACK, HOLD, DECAY, RELEASE and SUSTAIN, A CURVE,
  D CURVE, R CURVE set that envelope (they stand in for its own parameters, which the ENV page and the matrix use).
- **LFO > LFO SHAPE** (press LFO twice more): `EDIT` picks LFO 1-4; `SHAPE` sets its shape. POINT picks a point (shown as
  "2 / 5"), TIME, LEVEL and CURVE set it; ADD POINT adds one halfway to the next (or the previous) point, DEL POINT deletes
  the chosen one (never the first, never below two), TO POINTS turns the current shape into points. Editing a point of a
  built-in shape first turns that shape into points, so editing starts from what you heard.
- A project saves the points with the plugin's state; presets (the PRESET menu) use the built-in shapes.

## Serum 2 presets
`tools/serum2_import.py` turns Serum 2 presets (`.SerumPreset` files you own) into Philtre presets (`.xpl`), on a
computer, from a copy of this repository (it reads Philtre's parameter list from `vst/make_params.py`). Philtre is not
Serum, so a converted preset is a starting point in the same direction, not the same sound.
```
pip install zstandard                    # Serum 2 presets are Zstandard-compressed; not needed with Python 3.14+
python3 ports/philtre/tools/serum2_import.py -o out <preset files or folders>
python3 ports/philtre/tools/serum2_import.py -o out --tables "<Serum 2 Presets>/Tables" <presets>   # with the wavetables
```
Then copy `out/Presets/*.xpl` into `<Synths folder>/bastienstefani - VST - Philtre/Presets/` on the MPC and restart MPC
(it reads that folder at startup); the presets are on Philtre's preset page (Sounds > INSTRUMENTS, tap the Philtre tile).
With `--tables`, also copy `out/wavetables/serum2/` into the plugin's `wavetables/` folder: each oscillator then plays
Serum's own table (Philtre reads Serum's 2048-sample frames), and until the file is there a built-in table chosen from the
file's name stands in. An upgrade of Philtre replaces the plugin folder except `wavetables/`: copy the presets again.
`--synths` sets the Synths folder written in the files (default `/sdcard/Synths`).

- **Converted:** the first two oscillators that are on (table, position, warp, unison up to 8, detune, phase, pitch, level,
  pan), noise (a noise type chosen from the sample's name), sub, the filter routing, filter 1 (or filter 2 when only it is
  on), the four envelopes with their curves, the LFOs the matrix uses (four: points, mode, rate), the first 16 matrix slots
  whose source and destination Philtre has (amounts rescaled to each destination's range; a unipolar LFO or a bipolar
  envelope moves the destination's base value by half its amount, as Serum does), the macros the matrix uses, mono /
  legato, and the main effect rack in order (Hyper/Dimension as Ensemble, Distortion, Compressor or Multiband, Delay,
  Reverb or Plate, Chorus, Flanger, Phaser, EQ, Filter).
- **Left out, listed in `out/import-report.txt` with "x":** a third oscillator, sample, granular and spectral oscillators
  (a built-in table plays), a wavetable stored inside the preset, the second warp, filter 2 when both are on, LFOs past
  four and the chaos LFOs (a smooth random LFO plays), matrix sources the converter does not know (the report gives the
  number: `--source 25="Macro 1"` names it), slot curves, the arpeggiator and clips, effect racks 2 and 3, and effects
  Philtre has no counterpart for.
- **Accuracy:** unison, detune, wavetable position, cutoff, resonance, envelope times and sustain convert with units
  measured on Serum 2.1.5 (published by the serum2-preset-loader project, MIT). A free LFO rate is in Hz and a synced
  one is fitted on two divisions, both from presets saved for the purpose. Levels, pan, pitch ranges, curve shapes and
  effect settings are read off preset files or guessed (the defaults of the filter type, routing, LFO rate and mode
  were checked on Serum 2's Init screen); the report marks them "~" and
  the converter marks them `[g]` next to their constant. `TESTING.md` says how to calibrate one.
- The format comes from community reverse engineering (no Xfer Records code or documentation); the converter reads your
  files on your computer. Presets and wavetables belong to their makers: do not share converted factory content.

## Screen and controls (MPC Live II)
Nine tabs; every Q-Link column is one frame of four stacked controls in the order of the four physical knobs, the
Q-Link button moves to the next column (outlined by MPC), and frames stay clear of the Q-Link sidebar. Touch a control,
then turn the data wheel: one step per click on lists and whole numbers. Lists open on a tap.

| Tab | Column 1 | Column 2 | Column 3 | Column 4 |
|---|---|---|---|---|
| MAIN | Macro 1-4 | Mode, voices, glide, bend | Volume, velocity | Cutoff, resonance, drive, Env 2 amount |
| OSC A, OSC B | Table, WT pos, warp, warp amount | Octave, semi, fine, level | Unison voices, detune, blend, width | Phase, random, pan, on |
| SUB NOISE | Sub on, shape, octave, level | Sub pan, route | Noise on, type, pitch, level | Key track, pan |
| FILTER | On, type, cutoff, resonance | Drive, key track, Env 2 amount, mix | Routing A, B, noise, sub | Env 2 ADSR |
| ENV | Env 1 ADSR | Env 2 | Env 3 | Env 4 |
| ENV SHAPE (ENV, 2nd press) | Attack, hold, decay, release | Sustain, A, D and R curves | Edit (Env 1-4) | |
| LFO | LFO n: shape, rate, sync, mode (2nd press: rise, phase) | | | |
| LFO SHAPE (LFO, 3rd press) | Point, time, level, curve | Shape | Edit (LFO 1-4) | |
| MOD | Slot n: source, aux, destination, amount (four sub-pages, slots 1-16) | | | |
| FX | Slot n: effect (tap), then P1, P2, P3, mix on the Q-Links (two sub-pages, slots 1-8) | | | |

## Presets
Sixteen original presets in MPC's PRESET menu (`vst/presets.json`, written by `vst/make_presets.py`): Init, Seven Saw Lead,
Wide Glass Pad, Reese Bass, Growl Bass, Wobble Quarter, Deep Sub, Pluck Delay, FM Bell Keys, Formant Lead, Comb Pluck,
Phase Pad, Pump Chords, Noise Riser, Fold Arp, Hoover Stab.

## CPU
Offline (2026-11-02, x86 Xeon 2.1 GHz on a shared VM, relative numbers only): `tools/bench.sh ... local` PASS, p99
4.7-4.9%, worst block 6.5-8.1%. Eight voices of the heaviest presets took 3-4.6% of a block, and a worst case (8 voices x
2 oscillators x 8 unison, sub, noise, ladder filter and all 8 effect slots) 8.7%. The Live II's Cortex-A17 is several
times slower: the device bench decides (`TESTING.md`). Unison voices, the number of voices (`VOICES` on MAIN) and the
reverbs cost the most; an oscillator at level 0, a filter set Off and an empty slot cost nothing.

## Build, test, package
From `ports/philtre`:
```
python3 vst/make_params.py      # parameters -> vst/params.json, src/ph_params.h
python3 vst/make_presets.py     # presets -> vst/presets.json
python3 vst/make_layout.py      # the skin layout -> vst/layout.conf
python3 vst/art/make_tile.py    # the Instruments-browser tile (Pillow)
python3 vst/art/make_strips.py  # the shape displays' filmstrips (Pillow)
./build.sh                      # host test (x86, ASan), then the armv7 + aarch64 builds and the skin (Docker)
../../tools/test_port.sh vst/vst.json aarch64
test/run.sh                     # the shape editors' and the Serum import's own tests (x86, ASan/UBSan; the import
                                # test needs Zstandard for Python and builds its Serum presets itself)
```
Parameter indices are locked by `vst/params.lock`: append new parameters at the end of `make_params.py`. `skin_check`
prints TOUCH warnings for the option lists of the popups (an open list covers the controls under it); nothing else
overlaps. Packaging adds the wavetables folder: `tools/release.py ... --extra ports/philtre/data/wavetables:wavetables
--user-data wavetables --repo bastienstefani/mpc-vst-plugins --license GPL-3.0-or-later --id philtre`.

From CI: `.github/workflows/philtre-release.yml` runs on each push that changes `ports/philtre/` (branches `Serum` and
`main`) and attaches both zips to a **draft** release `philtre-v<version>`; publish it after the device checks.

## Not in this version
Drawing wavetables or curves by hand (MPC pages cannot draw: curves are edited point by point), a moving playhead on the
displays (redrawing costs MPC's screen thread a lot), dragging effects to reorder them, a second filter, per-slot curves
in the matrix, sample-based noise, MPE, and a 2.x skin (the skin is MPC OS 3.x only).

## Changes
- 0.3.0 (2026-11-03): Serum 2 preset import (`tools/serum2_import.py`). A project or preset naming a user wavetable file
  that is missing now keeps the built-in table it names (a converted preset's stand-in) instead of the first one.
- 0.2.0 (2026-11-03): envelope hold and curves; Custom LFO shapes made of points; the ENV SHAPE and LFO SHAPE editors with
  bar displays. Parameters only appended, but `Custom` was appended to the LFO shape list, so a project saved with a 0.1.0
  draft reads its LFO shapes one option off; the default envelope curves (A +25, D and R +50) make 0.1.0 sounds slightly
  different. Release 0.1.0 was never published.
- 0.1.0 (2026-11-02): first version.
