# Osmium

An eight-voice wavetable and virtual-analog synth for MPC OS standalone devices, laid out for the MPC Live II's screen,
its four Q-Link knobs and its data wheel. Original code (GPL-3.0-or-later, `LICENSE`); its architecture is the
familiar one of modern digital polysynths: three oscillators per voice, two filters, three envelopes, three LFOs and a
modulation matrix. No third-party code, samples or wavetables: the twelve wavetables are computed when the plugin loads.

Status: **MVP, offline only** (2026-11-02). It builds for Gen1 (armv7) and Gen2 (aarch64) and passes the host tests;
nothing has run on a device yet. See `TESTING.md`.

## Sound engine
Per voice (8 voices, poly, mono or legato, glide):
- **Three oscillators.** `WAVE` picks one of twelve wavetables (64 frames each, band-limited per octave) or one of four
  classic waveforms. `SHAPE` is the position in the wavetable; on a classic wave it is the wave's own shape:
  | Wave | Shape 0 -> 100 |
  |---|---|
  | Classic, PWM, Harmonics, Vowel, Sync, FM, Fold, Organ, Digital, Reso, Comb, Random | position in the table (Classic: sine, triangle, saw, square, narrow pulses) |
  | VA Saw | a second saw half a cycle on is mixed in (an octave up at 100) |
  | VA Square | pulse width 50% -> 5% |
  | VA Tri | triangle -> saw (skew) |
  | VA Sine | self-FM, brighter |
  Each oscillator has level, octave, semitone and fine tune. Plus a noise source with a colour (dark <-> bright).
- **Two filters** (LP 24, LP 12, BP, HP 12, HP 24, Notch; state-variable, stable under fast modulation), each with cutoff,
  resonance, drive, filter-envelope amount and key tracking, in series or in parallel with a balance.
- **Envelopes:** Amp, Filter and Env 3 (ADSR, 0.5 ms to 10 s). Env 3 only acts through the matrix.
- **LFOs:** three, seven shapes (sine, triangle, saw up/down, square, S&H, smooth random), 0.02-40 Hz or synced to the
  MPC tempo (4 bars to 1/32, triplets), free-running (one LFO for all voices) or restarted per note, with a fade-in.
- **Modulation matrix:** 16 slots of `Source x Via x Amount -> Destination`.
  Sources: Amp Env, Filt Env, Env 3, LFO 1-3, Velocity, Key, Mod Whl (CC 1), Pressure (channel or poly aftertouch),
  Bend, Random (per note), Expr (CC 11). Via `Off` = x1.
  Destinations: Pitch, O1-O3 Pitch (+-24 semitones at 100), O1-O3 Shape, O1-O3 Level, Noise, Cutoff (both filters),
  F1/F2 Cutoff (+-96 semitones), F1/F2 Reso, F Balance, Amp, Pan, LFO1-3 Rate (+-4 octaves; LFOs set to Key only).
- **Effects:** stereo chorus and a stereo or ping-pong delay (10 ms-2 s or synced), feedback with a tone control.
- MIDI: notes, velocity, pitch bend, mod wheel, CC 11, sustain (CC 64), channel and poly aftertouch, CC 120/121/123.
  CC 20-35 move the OSC page's Q-Links and NRPN reaches every parameter (the wrapper's own MIDI control).
- 12 factory presets in MPC's PRESET menu (`vst/presets.json`), the first one being the default sound.

## Screen and controls (MPC Live II)
Seven tabs. Every Q-Link column is one frame of four controls stacked top to bottom, in the order of the four
physical knobs; the **Q-Link button** moves the knobs to the next column, and MPC outlines the column in use
(`qlink_bounds=column`). Frames stay left of x 1010, where MPC's Q-Link sidebar does not cover them. **Touch a control,
then turn the data wheel** to step it: one option or one whole number per click; on a list (`WAVE`, `TYPE`, `SOURCE`...)
a tap opens the full list and a pick closes it.

| Tab | Column 1 | Column 2 | Column 3 | Column 4 |
|---|---|---|---|---|
| OSC | Osc 1: wave, shape, level, octave | Osc 2 | Osc 3 | Noise: level, color |
| TUNE | Semitone osc 1-3 | Fine osc 1-3 | Voice: mode, voices, glide, bend range | Output: volume, velocity, spread |
| FILTER | Filter 1: type, cutoff, resonance, env amount | Filter 2 | F1 drive, F1 key, F2 drive, F2 key | Routing, F1 <> F2 balance |
| ENV | Amp ADSR | Filter ADSR | Env 3 ADSR | F1 env, F2 env, velocity |
| LFO | LFO 1: shape, rate, sync, fade in | LFO 2 | LFO 3 | Retrigger LFO 1-3 |
| MOD | Slot n: source, via, destination, amount (four sub-pages: slots 1-4, 5-8, 9-12, 13-16) | | | |
| FX | Chorus: mix, rate, depth | Delay: mix, time, feedback, tone | Delay sync, mode | Volume |

The plugin also ships an Instruments-browser tile (`vst/art/tile.png`) and a default `.xpl` preset, so it appears as
a tile in Sounds > INSTRUMENTS.

## CPU
Offline (2026-11-02, x86 Xeon 2.1 GHz, relative numbers only): `tools/bench.sh ... local` gives p99 1.5-3.4% and a worst
block of 3.5-12.7% of a 2902 µs block over four runs on a shared VM (PASS); a harness with 8 held voices, three wavetable oscillators, noise, two 24 dB
filters in parallel with drive, chorus and delay took 39 µs per block. The Live II's Cortex-A17 will be several times
slower: the device bench decides (`TESTING.md`). Lower `VOICES` on the TUNE page to save CPU; an oscillator at level 0,
a filter set to Off and an effect at mix 0 cost nothing. The wavetables (6.5 MB, shared by every instance) are computed
when the first instance opens: 0.1 s on the PC, expect several times that on a Gen1 device.

## Build, test, package
From `ports/osmium`:
```
python3 vst/make_params.py      # after changing a parameter: writes vst/params.json and src/osm_params.h
python3 vst/make_presets.py     # after changing a preset or a default: writes vst/presets.json
python3 vst/art/make_tile.py    # the browser tile (Pillow)
./build.sh                      # host test (x86, ASan), then the armv7 + aarch64 builds and the skin (Docker)
../../tools/test_port.sh vst/vst.json aarch64          # the host test in an arm64 container
../../tools/studio.py preview "vst/build/skin/bastienstefani - VST - Osmium/Plugin Skins" -o page_%d.png
```
The skin uses the browser renderer (`"art": "html"`), so `build_port.sh` builds the `mpc-vst-html-art` Docker image the
first time. `skin_check` prints TOUCH warnings for the option lists of the popups (WAVE, TYPE, SHAPE, SYNC, SOURCE, VIA,
DESTINATION): an open list covers the controls under it, and nothing else overlaps.

Parameter indices are locked by `vst/params.lock`: append new parameters at the end of `vst/make_params.py`.

Package (one zip per CPU, see `docs/RELEASING.md`):
```
cd ../..
B=ports/osmium/vst/build
tools/release.py --so $B/osmium.so --skin "$B/skin/bastienstefani - VST - Osmium" --entry $B/pluginlist-entry.xml \
  --version 0.1.0 --repo bastienstefani/mpc-vst-plugins --license GPL-3.0-or-later --id osmium -o dist \
  --about "Eight-voice wavetable and VA synth for 4-knob MPCs."
tools/release.py --so $B/aarch64/osmium.so ... (same arguments)
tools/catalog_check.py dist/Osmium-0.1.0-mpc-armv7.zip --catalog
```

From CI: `.github/workflows/osmium-release.yml` runs on every push that changes `ports/osmium/` (branches `iridum` and
`main`). It calls `vst-release.yml`, which builds both zips and attaches them to a **draft** release `osmium-v<version>`
(the version from `vst/vst.json`), replacing the zips of an earlier draft. A draft is only visible to the repo's
maintainers and makes no tag: publish it after the device checks in `TESTING.md`, and bump the version after that.
A committed `vst/bench.txt` (the `-j` line of `tools/bench.sh`) adds the CPU result to `INSTALL.md`.

## Not in this version
- The sample-based (granular), resonator and FM-operator oscillator modes, a Komplex-style modulator, an arpeggiator,
  user wavetables (WAV files), unison and a reverb.
- MPE. Pressure reads poly aftertouch if MPC passes it to a VST2 instrument, which has not been checked.
- An MPC OS 2.x skin (the skin is in the 3.x shape; `SHADOW_SKIN_MPC_OS=2` would be the way, untested here).
- Live displays: MPC cannot draw a moving wavetable or envelope, so the pages show values, not curves.
