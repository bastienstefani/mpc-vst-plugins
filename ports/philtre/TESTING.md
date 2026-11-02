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

Record the results here and in `tested.json` (device, MPC OS version, date).
