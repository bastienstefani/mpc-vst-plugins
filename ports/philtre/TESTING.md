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

Checked against Serum (2026-11-10, offline): a kick preset (Serum 2.1.5) and Serum's own render of it at 130 BPM. The
first conversion was far off: effects switched off in Serum were converted as on (four distortions in series), the EQ
was flat, the pitch envelope swept 4.5 semitones instead of 24 and bent the wrong way. After the fixes, Philtre's render
(`test_states`-style harness, the same tempo and note) follows Serum's pitch drop (115 / 85 / 58 / 45 / 40 / 37 Hz at
20 / 41 / 73 / 115 / 165 / 300 ms against 123 / 84 / 60 / 46 / 41 / 37 Hz), its level is 1.8 dB lower, and it has more
energy between 120 Hz and 2 kHz (12 % against 4 %): the built-in table stood in for Serum's, and the effects differ.

To calibrate a `[g]` unit: in Serum 2, save Init, then the same with one control moved to a known value (e.g. an LFO at
1/8, or Osc A level at 50 %); `serum2_import.py --dump` both and compare the field, then fix the constant in the converter.

## Offline, 0.4.0 (2026-11-11): effect slots switched on and off
| Check | How | Result |
|---|---|---|
| Engine | `test/test_editors.c` (run by `test/run.sh`, ASan/UBSan) | PASSED: a slot switched off sounds as if it were empty and keeps its effect and settings; a saved state keeps it off; a state from before 0.4.0 has its slots on; a delay switched back on starts empty; switching off mid-echo (a sine's echoes, 0.22 peak) makes no step bigger than the echo's own (eight runs) |
| Serum import | `test/test_import.py` | PASSED: an effect switched off in Serum comes as a slot switched off with its settings; ten effects with three off: the seven on keep their slots, the first off one fills the eighth |
| The user's kick | converted again and rendered | EQ, Distortion, three slots switched off, Distortion, Multiband; the render is bit-identical to the same state with those three slots empty |
| Skin | `skin_check`, `tools/studio.py preview` of FX 1-4 and 5-8 | the switch sits right of a narrower `EFFECT` list (152 px); no overlap but the open lists' (as before) |
| Host test x86 and aarch64, builds | as for 0.1.0 | PASSED; armv7 and aarch64 without warnings, glibc 2.29 / 2.34 |

## Offline, 0.5.0 (2026-11-11): 16 effect slots and the Splitter
| Check | How | Result |
|---|---|---|
| Engine | `test/test_editors.c` (run by `test/run.sh`, ASan/UBSan) | PASSED: slot 16 plays; a Splitter comes with its mix at 100 and, with nothing on its bands, gives back what went in (level and brightness within 1 %); a 20 Hz low-pass on Band 3 (over 2 kHz) halves the brightness and keeps the level; on Band 1 (under 60 Hz) it leaves a middle C as it was; Left / Right with the low-pass on Band 2 silences the right channel only; a slot set to All after the bands plays on them joined; the Splitter switched off leaves the band slots on the whole sound; a saved state keeps the Splitter, its mode and the bands (3.1 kB); its knobs and mode are named |
| Serum import | `test/test_import.py` | PASSED: Serum's Splitter (145 Hz) with the HyperD counted in its band 2 comes as Philtre's Splitter with the Ensemble on Band 3, the effects after it on All; 18 effects with six off: the twelve on keep their slots, the first four off ones fill the rest |
| The user's kick | converted again and rendered | 11 slots (EQ, Distortion, four switched off, Distortion, Splitter, Ensemble on Band 3, Multiband); level and spectrum as before; the same state without the Splitter (the Ensemble on the whole sound) is 4 dB lower |
| CPU, relative | a worst-case harness (8 voices x 2 oscillators x 8 unison, sub, noise, ladder filter), x86 | 4.7% of real time with no effect, 6.2% with 8 slots on, 7.1% with 16, 7.2% with a Splitter and three slots on its bands |
| Skin | `skin_check`, previews of FX 1-4 ... 13-16 and MOD | four FX sub-pages; the band selector (ALL, BAND 1-3) sits at the foot of each slot, with no name so it clears the MIX knob; the destination list has an FX 9-16 column |
| Host test x86 and aarch64, builds | as for 0.1.0 | PASSED (400 parameters); armv7 and aarch64 without warnings, glibc 2.29 / 2.34 |

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
| 23 | FX switch | FX tab: put Distortion in slot 1, play, tap its switch off and on | the sound loses and gets back the distortion, without a click; the switch shows the state after a project reload |
| 24 | Splitter | FX tab: slot 1 Splitter (3 bands), slot 2 Distortion on Band 3, slot 3 Reverb on All; press FX for slots 5-16 | only the highs distort, the reverb takes the whole sound; FX pages 1-4 ... 13-16 cycle |

Record the results here and in `tested.json` (device, MPC OS version, date).
