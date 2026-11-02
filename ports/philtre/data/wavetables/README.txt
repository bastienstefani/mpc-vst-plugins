Philtre - your own wavetables
=============================

Put .wav wavetables in this folder (sub-folders up to 3 levels are fine). Philtre lists them, sorted by
path, when it is inserted on a track: re-insert the plugin (or restart MPC) after adding files.

- Format: PCM 16, 24 or 32-bit, or 32-bit float; mono (a stereo file uses its left channel).
- Frames: 2048 samples each, one after the other (the common wavetable format). A file with a "clm "
  chunk ("<!>2048 ...", written by many wavetable editors) uses the frame length it gives. A file shorter
  than 2048 samples is one single-cycle frame. Up to 256 frames per table.
- Up to 64 files. On OSC A or OSC B, step TABLE past the 12 built-in tables to reach them: the file name
  is shown. A project remembers the file by name, so adding files later does not change a saved sound.
- Loading takes a moment (the table is rebuilt in band-limited versions on a background thread); the
  oscillator keeps its previous table until the new one is ready.

Only add files you have the right to use.
