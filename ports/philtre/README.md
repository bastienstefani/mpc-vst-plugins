# Philtre

An eight-voice wavetable synth for MPC OS standalone devices, with a Serum-style architecture laid out for the MPC Live
II's screen, its four Q-Link knobs and its data wheel. Original code (GPL-3.0-or-later, `LICENSE`). Not affiliated with
Xfer Records: no Serum code, presets, wavetables or artwork. The built-in wavetables are computed when the plugin loads,
the presets are original, and you can load your own `.wav` wavetables.

Status: **MVP, offline only** (2026-11-02). It builds for Gen1 (armv7) and Gen2 (aarch64) and passes the host tests and a
sanitizer stress run; nothing has run on a device yet. See `TESTING.md`.

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
- **Envelopes:** four ADSR (Env 1 is the amp). **LFOs:** four, ten shapes (sine, triangle, saw up/down, square, S&H,
  smooth, pump, exponential rise, stairs), free-running, retriggered per note or one-shot (Env mode), 0.02-40 Hz or synced
  to the MPC tempo, with rise (fade-in) and start phase. **Macros:** four knobs, as modulation sources.
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
| LFO | LFO n: shape, rate, sync, mode (sub-page 2: rise, phase) | | | |
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
./build.sh                      # host test (x86, ASan), then the armv7 + aarch64 builds and the skin (Docker)
../../tools/test_port.sh vst/vst.json aarch64
```
Parameter indices are locked by `vst/params.lock`: append new parameters at the end of `make_params.py`. `skin_check`
prints TOUCH warnings for the option lists of the popups (an open list covers the controls under it); nothing else
overlaps. Packaging adds the wavetables folder: `tools/release.py ... --extra ports/philtre/data/wavetables:wavetables
--user-data wavetables --repo bastienstefani/mpc-vst-plugins --license GPL-3.0-or-later --id philtre`.

From CI: `.github/workflows/philtre-release.yml` runs on each push that changes `ports/philtre/` (branches `Serum` and
`main`) and attaches both zips to a **draft** release `philtre-v<version>`; publish it after the device checks.

## Not in this version
Drawing wavetables or LFO shapes (MPC pages cannot draw), dragging effects to reorder them, a second filter, envelope hold
and curves, per-slot curves in the matrix, sample-based noise, MPE, and a 2.x skin (the skin is MPC OS 3.x only).
