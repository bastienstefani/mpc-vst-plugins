/* Philtre's user wavetable library: the .wav files under the plugin's wavetables/ folder, listed when an instance opens
 * (sorted, at most 3 folders deep, no symlinks), and loaded one per oscillator on a worker thread, never on the
 * audio thread. A table is handed to the engine with lib_take() and given back with lib_retire() to be freed there. */
#pragma once
#include "ph_wt.h"

#define LIB_MAX 64      /* = USER_SLOTS */
#define LIB_NAME 40

typedef struct ph_lib ph_lib_t;

ph_lib_t *lib_open(const char *dir);           /* lists dir (may be NULL or missing: an empty library) and starts the worker */
void lib_close(ph_lib_t *lib);                  /* stops the worker, frees every table it still holds */
int lib_count(const ph_lib_t *lib);
const char *lib_name(const ph_lib_t *lib, int i);   /* display name (file name without .wav) */
const char *lib_path(const ph_lib_t *lib, int i);   /* path relative to the folder: what a saved project keeps */
int lib_find(const ph_lib_t *lib, const char *rel); /* index of a relative path, or -1 */
void lib_request(ph_lib_t *lib, int osc, int file); /* osc 0/1 wants file (-1: none); the worker loads it */
wtab_t *lib_take(ph_lib_t *lib, int osc, int *file);/* a freshly loaded table for osc (the caller owns it), or NULL; never blocks */
int lib_retire(ph_lib_t *lib, wtab_t *t);           /* hands t back to be freed by the worker; 0 if busy (try again later) */

/* WAV -> frames (exposed for the tests): mono float frames of *frame_len samples. The frame length is the Serum-style
 * "clm " chunk's number when present, else 2048, else the whole file as one frame when it is shorter than 2048. */
float *wav_frames(const char *path, int *nf, int *frame_len);
