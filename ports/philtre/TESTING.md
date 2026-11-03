# Philtre: tests

## Offline (2026-11-02)
| Check | How | Result |
|---|---|---|
| Host test, x86 ASan/UBSan | `tools/test_port.sh ports/philtre/vst/vst.json` | PASSED (instances, names, set/get, stepping, note -> audio, STOP toggle, chunk restore, 16 programs, CC 20, NRPN, two threads) |
| Host test, aarch64 (UBSan, arm64 container) | `tools/test_port.sh ports/philtre/vst/vst.json aarch64` | PASSED |
| Builds | the compile step of `tools/build_port.sh`, armv7 (`arm32v7/gcc:11-bullseye`) and aarch64 | no warnings with `-Wall -Wextra`; `VSTPluginMain` only; highest glibc 2.29 (armv7), 2.34 (aarch64) |
| Skin | `skin_check` (in `gen_vst.py`), `tools/studio.py preview` | no EDGE, QLINK or OPTS finding; TOUCH only between open popup lists and the controls under them (a 12 px knob overlap on the FX pages was fixed); every page looked at |
| User wavetables | a stress harness with four WAV files: 16-bit with a `clm ` chunk, 8 frames; 32-bit float, one 600-sample cycle; 24-bit stereo, 4 frames; one that is not audio; a hidden folder | listed sorted, the hidden folder skipped, TABLE clamped to the files there are; the bad file keeps the previous table; a project restores the table by file name |
| Stress, ASan/UBSan | 40000 blocks of random notes, sustain, bend, aftertouch, CCs and parameter values (effect types included), state save/restore into a second instance, three seeds | clean; exact silence after All Sound Off with the rack off |
| Audio | every preset played (4-note chords, mod wheel) and analysed | no DC (a 5 Hz blocker on the output); energy above 18 kHz under 5e-4 of the total except Comb Pluck (white noise into the comb) |
| CPU, relative | `tools/bench.sh ports/philtre/vst/build/philtre-x86.so local` | PASS: p99 4.7-4.9%, worst block 6.5-8.1% (x86 on a shared VM, not a device number). Changing an effect type used to clear a 1 MB delay line inside the parameter call (sweep worst block 20-24%); long lines are now guarded by a count of written samples instead |
| Release zips | `tools/release.py ... --extra ports/philtre/data/wavetables:wavetables --user-data wavetables`, `tools/catalog_check.py --catalog` | OK, 0 warnings; `wavetables/README.txt` in the plugin folder; labelled MPC OS 3.x only |

## Offline, 0.2.0 (2026-11-03): envelope curves, Custom LFO points, shape editors
| Check | How | Result |
|---|---|---|
| Shape editors | `test/run.sh` (25 checks, ASan/UBSan) | PASSED: the editor controls stand in for the chosen envelope / LFO and the host is told both ways; the displays follow (an envelope rises to the top and ends at 0, a square is high then low) and refuse host sets; points: editing makes the LFO Custom from its shape, ADD (after, or before when there is no room), DEL (never the first), POINT stops at the last, times stay between neighbours; a saved state brings the points back (2.3 kB of 8 kB); a +100 attack curve is louder early than -100, and a hold keeps the level up |
| Host test, x86 and aarch64; stress, three seeds | as for 0.1.0 | PASSED; clean |
| Builds | armv7, aarch64 | no warnings; glibc 2.29 / 2.34 |
| Skin | `skin_check`, previews of ENV SHAPE and LFO SHAPE (with display values computed by the engine pasted in) | no overlaps outside open popup lists |
| CPU, relative | `tools/bench.sh ... local`, 0.1.0 and 0.2.0 one after the other, twice | 8 voices the same (p99 2.0-2.2%); the Q-Link sweep mean 2.6-2.7% against 2.3-2.4% (p99 7.9-8.1% against 7.0-7.4%), x86 on a shared VM |

## Offline, 0.3.0 (2026-11-03): Serum 2 preset import
| Check | How | Result |
|---|---|---|
| Converter | `test/test_import.py` (run by `test/run.sh`): Serum 2 presets built by the test (an init, a full patch, a three-oscillator one) and five broken files | PASSED, 54 checks: defaults where a field is absent; units (envelope times, sustain as gain, cutoff, position, detune); oscillator remapping when A is off; LFO points (y flipped, steps kept, 31 points cut to 16); matrix amounts and base offsets; `--source`; effect order; report lines; `--tables`; bad files refused, not crashing (bad magic, cut payload, 100-deep nesting, a list payload, a 2^40-entry map) |
| Engine | `test/test_states.c` (run by `test/run.sh`, ASan/UBSan) | PASSED: each converted state loads and saves back with the same values and points; a 4-note chord is heard and stays finite; a missing table file keeps the named built-in |
| Real files | 7 `.SerumPreset` test files from open-source Serum 2 tools (Serum 2.0.17-2.1.4; not in this repo), then `test_states` on the results | all decoded and converted (2.2-2.4 kB states); sample and multisample oscillators reported; each played (peak 0.37-0.89) |
| Host test x86, builds | as for 0.1.0 | PASSED; armv7 and aarch64 without warnings, glibc 2.29 / 2.34 |
| Calibration (Serum 2.0.16) | presets saved from Init with one change: LFO 1 at 1/8, 1 bar, 1 Hz free; eight matrix slots (Macro 1, Macro 2, Velo, Note#, Aftertouch, Poly Aftertch, Pitch Bend, Mod Wheel); Init's LFO read in Hz mode (6.2 Hz) | the LFO rates and source numbers now in the converter; each of these converts to the same division / Hz / source (not kept in the repo) |

To calibrate a `[g]` unit: in Serum 2, save Init, then the same with one control moved to a known value (e.g. an LFO at
1/8, or Osc A level at 50 %); `serum2_import.py --dump` both and compare the field, then fix the constant in the converter.

## On the device (MPC Live II, to do)
Install: unzip `Philtre-<version>-mpc-armv7.zip` on the MPC and run `./install.sh` as root (it stops MPC, backs up
`MPC.settings`, registers the plugin and starts MPC), or drop the zip into the MPC plugin installer app.

| # | Check | How | Expect |
|---|---|---|---|
| 1 | CPU | `tools/bench.sh ports/philtre/vst/build/philtre.so <ip> -j \| tee ports/philtre/vst/bench.txt` | PASS (p99 <= 15%); WARN is said in the release notes |
| 2 | Heavy presets | play 8-note chords on Hoover Stab, Wide Glass Pad and Pump Chords, `top` on MPC | no dropouts; note the CPU |
| 3 | Load time | insert Philtre | the first instance builds the wavetables: under about a second |
| 4 | Presets | pick each of the 16 from the PRESET menu and play | each sounds; no clicks |
| 5 | Q-Link columns | on MAIN and OSC A, turn the four Q-Links, press the Q-Link button | the outline moves column by column |
| 6 | FX knob names | FX tab: set slot 1 to Delay, then Reverb | the four knob names change (TIME, FEEDBACK, TONE, MIX / SIZE, DAMPING, PRE-DELAY, MIX) |
| 7 | FX sub-pages, MOD sub-pages, LFO sub-page | press the tab again | slots 5-8; slots 5-16; rise and phase |
| 8 | Data wheel | touch TABLE, WT POS, a list field, and turn | steps by one; note what it does on a list field |
| 9 | User wavetables | copy a 2048-sample-frame `.wav` into `wavetables/`, re-insert, step TABLE past Random | its name shows; it plays after a moment |
| 10 | Project | save with a user table and effects, reload | the same sound, the table found by name |
| 11 | Tempo sync | LFO 1 sync 1/4 to Cutoff, Delay 1/8D; change the tempo | both follow |
| 12 | Mono / Legato, glide, sustain, aftertouch (Aftertouch -> Cutoff) | play | as set; says whether MPC passes poly aftertouch |
| 13 | STOP | press STOP with knobs moved | nothing changes |
| 14 | Browser tile | Sounds > INSTRUMENTS | the Philtre tile; a tap loads it |
| 15 | Uninstall | `./uninstall.sh` | entry and folder gone, `wavetables/` kept |
| 16 | ENV SHAPE display | ENV, press again; turn ATTACK, HOLD, the curves; switch EDIT between Env 1-4 | the bars follow within a moment; the knobs show the chosen envelope's values; no stutter in the audio while turning |
| 17 | LFO SHAPE | LFO, press twice; pick a shape, then turn POINT, TIME, LEVEL, CURVE; ADD POINT, DEL POINT, TO POINTS | the bars follow; "2 / 5" style point count; a Custom LFO modulating cutoff sounds as drawn |
| 18 | Display cost | `top` on MPC while sweeping a curve knob | note MPC's main thread; if it is high, raise `PH_DISP_FRAMES` |
| 19 | Display heights | compare the bars with the values | the 25 heights land on the bars' frames (MPC maps a filmstrip's value to a frame: checked offline only) |
| 20 | Imported preset | convert a Serum 2 preset, copy the `.xpl` to the plugin's `Presets/`, restart MPC, open it from the tile's preset page | it loads with its sound (says whether MPC needs the file's plugin path or matches the uid) |
| 21 | Imported table | convert with `--tables`, copy `wavetables/serum2/` | OSC A shows the table's name and plays it after a moment |
| 22 | Compared to Serum | play the same preset in Serum 2 and on the MPC | note what differs most (LFO rates, levels, filter type): the `[g]` units to calibrate first |

Record the results here and in `tested.json` (device, MPC OS version, date).
