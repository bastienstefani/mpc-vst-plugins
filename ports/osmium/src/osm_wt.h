/* Osmium's built-in wavetables: WT_NT tables of WT_NF single-cycle frames, each frame stored band-limited at WT_NL
 * levels (level L holds at most WT_HARM[L] harmonics in WT_LEN[L] samples), so a note picks the level whose top
 * harmonic stays below Nyquist. Generated procedurally at the first wt_acquire() (no sample content ships), shared
 * by every instance, freed when the last one releases it. */
#pragma once

#define WT_NF 64
#define WT_NL 9
#define WT_FRAME 2112   /* floats per frame, all levels: 1024+512+256+128+64+32*4 */

extern const int WT_LEN[WT_NL];
extern const int WT_HARM[WT_NL];
extern const int WT_OFF[WT_NL];

/* the table data: [table][frame][level offset + sample]; valid between wt_acquire() and the matching wt_release() */
extern float *wt_data;

int wt_acquire(void);   /* 0 on success; builds the tables on the first call */
void wt_release(void);

static inline const float *wt_frame(int table, int frame, int level) {
    return wt_data + ((long)table * WT_NF + frame) * WT_FRAME + WT_OFF[level];
}

/* the level for a phase increment (cycles per sample): the richest one whose top harmonic stays under 0.6 of the
 * sample rate after folding (aliases, if any, land above ~17.6 kHz) */
static inline int wt_level_for(float inc) {
    for (int l = 0; l < WT_NL - 1; l++)
        if (WT_HARM[l] * inc <= 0.6f) return l;
    return WT_NL - 1;
}
