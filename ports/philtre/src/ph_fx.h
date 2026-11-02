/* Philtre's effect rack: FX_SLOTS slots run in series on the stereo mix, each holding one effect type (FX_* in
 * ph_params.h) with three controls and a mix, all 0..1. A slot's names and value texts follow its type
 * (fx_label/fx_display). fx_prepare() allocates what a type needs and is called off the audio thread (on a type change);
 * fx_process() never allocates. */
#pragma once

typedef struct {
    int type;               /* the type the state below was prepared for */
    float *buf;             /* delay lines, sized for the type */
    long cap;
    int w, w2;              /* write positions */
    long filled;            /* samples written since the type was set: older parts of a long buffer are not read (no clearing) */
    float ph[4];            /* LFO phases */
    float st[64];           /* small per-type state: filters, followers, gains */
    float c[16];            /* per-type coefficients, recomputed each block */
    float last[4];          /* the controls the coefficients were computed for */
} fxslot_t;

void fx_init(fxslot_t *s);
void fx_free(fxslot_t *s);
int fx_prepare(fxslot_t *s, int type);   /* 0 on success; resets the state */
void fx_process(fxslot_t *s, const float p[4], float bpm, float *L, float *R, int n);
const char *fx_label(int type, int k);   /* k = 0..3: the three controls, then the fourth (usually MIX) */
int fx_display(int type, int k, float v, float bpm, char *buf, int n);
