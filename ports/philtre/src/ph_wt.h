/* Philtre's wavetables. A table is nf single-cycle frames, each stored band-limited at nl levels (level l holds at most
 * harm[l] harmonics in len[l] samples), so a note reads the level whose top harmonic stays under Nyquist.
 * Built-in tables (WT_NB, 64 frames, 1024-sample top level) are computed at the first acquire and shared by every
 * instance; a user table (any number of frames up to 256, resampled to 2048 samples) is built from a WAV file. */
#pragma once

#define WT_MAXL 10
#define WT_USER_LEN 2048
#define WT_MAX_FRAMES 256

typedef struct {
    int nf, nl, fsize;                               /* frames, levels, floats per frame (all levels) */
    int len[WT_MAXL], harm[WT_MAXL], off[WT_MAXL];
    float *data;                                     /* [frame][level offset + sample] */
} wtab_t;

int wt_builtin_acquire(void);                        /* 0 on success; builds the built-in tables on the first call */
void wt_builtin_release(void);
const wtab_t *wt_builtin(int i);                     /* 0 <= i < WT_NB, between acquire and release */

/* a user table from nf frames of frame_len samples each (any length; resampled to WT_USER_LEN), normalised to a peak
 * of 1. NULL on failure. Slow (FFTs): call it off the audio thread. */
wtab_t *wt_from_frames(const float *frames, int nf, int frame_len);
void wt_free(wtab_t *t);

static inline const float *wt_ptr(const wtab_t *t, int frame, int level) {
    return t->data + (long)frame * t->fsize + t->off[level];
}
/* the richest level whose top harmonic stays under 0.6 of the sample rate after folding (aliases above ~17.6 kHz) */
static inline int wt_level(const wtab_t *t, float inc) {
    for (int l = 0; l < t->nl - 1; l++)
        if (t->harm[l] * inc <= 0.6f) return l;
    return t->nl - 1;
}
