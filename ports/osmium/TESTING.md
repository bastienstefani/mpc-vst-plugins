# Osmium: tests

## Offline (2026-11-02)
| Check | How | Result |
|---|---|---|
| Host test, x86 ASan/UBSan | `tools/test_port.sh ports/osmium/vst/vst.json` | PASSED (two instances, names, set/get, option and whole-number stepping, note -> audio, STOP toggle of parameter 0, chunk restore, 12 programs, CC 20, NRPN, two threads) |
| Host test, aarch64 (UBSan, arm64 container under QEMU) | `tools/test_port.sh ports/osmium/vst/vst.json aarch64` | PASSED |
| armv7 build (`arm32v7/gcc:11-bullseye`) | the compile step of `tools/build_port.sh` | no warnings with `-Wall -Wextra`; exports `VSTPluginMain` only; highest glibc symbol 2.29 |
| aarch64 build (`gcc:12-bookworm`, arm64) | same | no warnings; highest glibc symbol 2.34 |
| Skin | `tools/skin_check.py` (run by `gen_vst.py`), `tools/studio.py preview` | no EDGE, QLINK or OPTS finding; TOUCH only between open popup lists and the controls under them; every page looked at |
| Release zips | `tools/release.py ... --repo --license --id osmium`, `tools/catalog_check.py --catalog` | OK, 0 warnings, both zips; labelled MPC OS 3.x only |
| CPU, relative | `tools/bench.sh ports/osmium/vst/build/osmium-x86.so local` (the x86 build of the same sources) | PASS in four runs: p99 1.5-3.4%, worst block 3.5-12.7%, the worst block landing on a different stage each run (a shared VM); x86, not a device number |
| Audio | a harness that plays every preset (4-note chords, mod wheel) and an 8-voice worst case | DC under 0.0001 (a 5 Hz DC blocker on the output); energy above 18 kHz under 2e-4 of the total in every preset (2e-3 in the worst case, which drives both filters) |
| Stress, ASan/UBSan | 60000 blocks of random notes, sustain, bend, aftertouch, CCs and parameter values, state save/restore into a second instance, three seeds | clean after one fix (a delay-line read index that could round up to the line length); exact silence after All Sound Off |

The skin in these checks was drawn by Playwright 1.63 with its Chromium, not by the pinned `mpc-vst-html-art` image
(Docker Hub was rate-limited from the build machine); `build_port.sh` uses the image.

## On the device (MPC Live II, to do)
Install: copy `Osmium-0.1.0-mpc-armv7.zip` to the MPC, unzip it and run `./install.sh` as root (it stops MPC, backs up
`MPC.settings`, registers the plugin and starts MPC again), or drop the zip into the MPC plugin installer app.

| # | Check | How | Expect |
|---|---|---|---|
| 1 | CPU | `tools/bench.sh ports/osmium/vst/build/osmium.so <ip> -j \| tee ports/osmium/vst/build/bench.txt` | PASS (p99 <= 15%, worst <= 50%); WARN means "say so in the release notes" |
| 2 | Load time | insert Osmium on a track, time it | the first instance computes the wavetables: under about a second |
| 3 | Sound | play pads and keys on the Init Wave preset, then every preset from the PRESET menu | each preset sounds, no clicks, no dropouts |
| 4 | Q-Link columns | on OSC, turn the four Q-Links, press the Q-Link button three times | each press moves the outline to the next frame; the knobs follow top to bottom |
| 5 | Data wheel | touch SHAPE then turn the wheel; touch WAVE (the field) then turn the wheel | a knob moves; note what the wheel does on a popup field (it may open/close the list rather than step it) |
| 6 | Lists | tap WAVE, pick VA Square; FILTER TYPE; MOD SOURCE / VIA / DESTINATION | a tap opens the list, a pick closes it and the value shows in the field |
| 7 | MOD sub-pages | press the MOD tab again | slots 5-8, 9-12, 13-16 in turn, each with its own Q-Links |
| 8 | Modulation | Init Wave: mod wheel up | vibrato grows (slot 1: LFO 1 x Mod Whl -> Pitch) |
| 9 | Tempo sync | LFO 2 sync 1/4 on a slot to Cutoff; Delay sync 1/8D; change the MPC tempo | both follow the tempo |
| 10 | Voices | play 9+ notes; set VOICES to 4 | the oldest or quietest note is taken over, no stuck notes; CPU drops |
| 11 | Mono / Legato + Glide | TUNE: MODE Legato, GLIDE 40, play overlapping notes | glides between held notes, no retrigger |
| 12 | Aftertouch | slot: Pressure -> Cutoff, press pads harder | the cutoff follows (says whether MPC passes poly aftertouch) |
| 13 | Project | save the project, reload it | every page as it was |
| 14 | STOP | press STOP several times with knobs moved | nothing changes (parameter 0 is Volume) |
| 15 | Browser tile | Sounds > INSTRUMENTS | the Osmium tile; a tap loads it |
| 16 | Uninstall | `./uninstall.sh` from the zip | entry and folder gone; a project that used it opens without it |

Record the results here and in `tested.json` (device, MPC OS version, date) once they are run.
