/* Philtre's wavetables (see ph_wt.h). The built-in tables are the twelve procedural tables of ports/osmium
 * (src/osm_wt.c, same author and licence), described as harmonic spectra or drawn in the time domain; user tables come
 * from WAV frames (ph_wav.c). Each frame is analysed once and resynthesised per band-limit level with an inverse FFT. */
#include <math.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include "ph_wt.h"
#include "ph_params.h"

#define TD_N 4096          /* the largest FFT: time-domain drawing of the drawn built-in tables */
#define NH 512             /* built-in tables: harmonics 1..511 */
#define BI_NF 64           /* built-in tables: frames */
#define PI 3.14159265358979323846

static float tw_c[TD_N / 2], tw_s[TD_N / 2];
static pthread_once_t tw_once = PTHREAD_ONCE_INIT;
static void tw_init(void) {
    for (int k = 0; k < TD_N / 2; k++) { tw_c[k] = (float)cos(2 * PI * k / TD_N); tw_s[k] = (float)sin(2 * PI * k / TD_N); }
}

/* in-place complex radix-2 FFT, n a power of two <= TD_N; sign -1 forward, +1 inverse (unnormalised). Reentrant. */
static void fft(float *re, float *im, int n, int sign) {
    for (int i = 1, j = 0; i < n; i++) {
        int bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) {
            float t = re[i]; re[i] = re[j]; re[j] = t;
            t = im[i]; im[i] = im[j]; im[j] = t;
        }
    }
    for (int len = 2; len <= n; len <<= 1) {
        int half = len >> 1, step = TD_N / len;
        for (int i = 0; i < n; i += len)
            for (int k = 0; k < half; k++) {
                float wr = tw_c[k * step], wi = sign * tw_s[k * step];
                float *ar = re + i + k, *ai = im + i + k, *br = re + i + k + half, *bi = im + i + k + half;
                float xr = *br * wr - *bi * wi, xi = *br * wi + *bi * wr;
                *br = *ar - xr; *bi = *ai - xi;
                *ar += xr; *ai += xi;
            }
    }
}

/* level layout of a table whose top level has len0 samples */
static void layout(wtab_t *t, int len0) {
    int n = len0, l = 0, off = 0;
    for (; l < WT_MAXL; l++) {
        int len = n < 32 ? 32 : n;
        int h = n < 32 ? (t->harm[l - 1] - 1) / 2 : n / 2 - 1;
        if (h < 1) break;
        t->len[l] = len; t->harm[l] = h; t->off[l] = off;
        off += len;
        n >>= 1;
        if (h == 1) { l++; break; }
    }
    t->nl = l;
    t->fsize = off;
}

/* resynthesise one frame from its half spectrum C[1..h0] (x(p) = sum Re(C[k] e^(2 pi i k p))) into every level;
 * returns the peak of the top level (before gain) */
static float synth_frame(wtab_t *t, int f, const float *cr, const float *ci, float *re, float *im) {
    float pk = 0;
    for (int l = 0; l < t->nl; l++) {
        int n = t->len[l], h = t->harm[l];
        memset(re, 0, n * sizeof *re);
        memset(im, 0, n * sizeof *im);
        for (int k = 1; k <= h; k++) {
            re[k] = cr[k] / 2; im[k] = ci[k] / 2;
            re[n - k] = cr[k] / 2; im[n - k] = -ci[k] / 2;
        }
        fft(re, im, n, 1);
        float *out = t->data + (long)f * t->fsize + t->off[l];
        for (int i = 0; i < n; i++) out[i] = re[i];
        if (l == 0)
            for (int i = 0; i < n; i++) if (fabsf(re[i]) > pk) pk = fabsf(re[i]);
    }
    return pk;
}

static void scale_frame(wtab_t *t, int f, float g) {
    float *p = t->data + (long)f * t->fsize;
    for (int i = 0; i < t->fsize; i++) p[i] *= g;
}

/* ---- the built-in tables' spectra and drawings (from ports/osmium/src/osm_wt.c) ---- */
static void sine_h(float *cr, float *ci, int k, float a) { cr[k] += 0; ci[k] -= a; }   /* a sin(2 pi k p) */

static void shape_classic(float *cr, float *ci, int s) {
    for (int k = 1; k < NH; k++) {
        double a = 0, w;
        switch (s) {
        case 0: a = k == 1; break;                                                           /* sine */
        case 1: if (k & 1) a = 8.0 / (PI * PI) / ((double)k * k) * (((k - 1) / 2) & 1 ? -1 : 1); break;   /* triangle */
        case 2: a = 2.0 / (PI * k); break;                                                   /* saw */
        default:                                                                             /* pulse */
            w = s == 3 ? 0.5 : s == 4 ? 0.25 : 0.1;
            cr[k] += (float)(4.0 / (PI * k) * sin(PI * k * w) * cos(-PI * k * w));
            ci[k] += (float)(4.0 / (PI * k) * sin(PI * k * w) * sin(-PI * k * w));
            continue;
        }
        sine_h(cr, ci, k, (float)a);
    }
}

static void pulse(float *cr, float *ci, double w) {
    for (int k = 1; k < NH; k++) {
        double a = 4.0 / (PI * k) * sin(PI * k * w);
        cr[k] += (float)(a * cos(-PI * k * w));
        ci[k] += (float)(a * sin(-PI * k * w));
    }
}

static const float VOWEL_F[5][5] = {{600, 1040, 2250, 2450, 2750}, {400, 1620, 2400, 2800, 3100},
                                    {250, 1750, 2600, 3050, 3340}, {400, 750, 2400, 2600, 2900},
                                    {350, 600, 2400, 2675, 2950}};
static const float VOWEL_DB[5][5] = {{0, -7, -9, -9, -20}, {0, -12, -9, -12, -18}, {0, -30, -16, -22, -28},
                                     {0, -11, -21, -20, -40}, {0, -20, -32, -28, -36}};
static const float VOWEL_BW[5] = {80, 90, 120, 130, 140};

static const char *const ORGAN[8] = {"800000000", "808000000", "888000000", "888800000",
                                     "888888000", "868868868", "888888888", "800000888"};
static const int ORGAN_H[9] = {1, 3, 2, 4, 6, 8, 10, 12, 16};   /* 16' 5 1/3' 8' 4' 2 2/3' 2' 1 3/5' 1 1/3' 1' */

static unsigned rnd_state;
static float rnd01(void) { rnd_state ^= rnd_state << 13; rnd_state ^= rnd_state >> 17; rnd_state ^= rnd_state << 5; return (rnd_state >> 8) / 16777216.0f; }

static void random_key(float *amp, int key) {
    rnd_state = 0x9e3779b9u * (unsigned)(key + 1);
    for (int i = 0; i < 8; i++) rnd01();
    for (int k = 1; k < NH; k++) {
        float r = rnd01();
        amp[k] = k <= 96 ? r * r / powf((float)k, 0.75f) : 0;
    }
    amp[1] = 0.6f + 0.4f * amp[1];
}

/* spectrum of frame t (0..1) of a table whose frames are defined in the frequency domain; returns 0 if drawn instead */
static int spectrum(int table, double t, float *cr, float *ci) {
    memset(cr, 0, NH * sizeof *cr);
    memset(ci, 0, NH * sizeof *ci);
    switch (table) {
    case 0: {   /* Classic: sine -> triangle -> saw -> square -> pulse 25% -> pulse 10% */
        float ar[NH], ai[NH], br[NH], bi[NH];
        double s = t * 5; int i = (int)s; if (i > 4) i = 4;
        double u = s - i;
        memset(ar, 0, sizeof ar); memset(ai, 0, sizeof ai); memset(br, 0, sizeof br); memset(bi, 0, sizeof bi);
        shape_classic(ar, ai, i); shape_classic(br, bi, i + 1);
        for (int k = 1; k < NH; k++) { cr[k] = (float)((1 - u) * ar[k] + u * br[k]); ci[k] = (float)((1 - u) * ai[k] + u * bi[k]); }
        return 1;
    }
    case 1: pulse(cr, ci, 0.5 - 0.48 * t); return 1;   /* PWM */
    case 2: {   /* Harmonics: a saw built up one harmonic at a time */
        double c = 1 + 63 * t;
        for (int k = 1; k < NH && k <= (int)c + 1; k++) sine_h(cr, ci, k, (float)((k <= (int)c ? 1.0 : c - (int)c) / k));
        return 1;
    }
    case 3: {   /* Vowel: A E I O U formants over a 110 Hz harmonic series */
        double s = t * 4; int i = (int)s; if (i > 3) i = 3;
        double u = s - i;
        for (int k = 1; k < NH; k++) {
            double f = 110.0 * k, a = 0.002 / k;
            for (int j = 0; j < 5; j++) {
                double F = (1 - u) * VOWEL_F[i][j] + u * VOWEL_F[i + 1][j];
                double g = pow(10, ((1 - u) * VOWEL_DB[i][j] + u * VOWEL_DB[i + 1][j]) / 20);
                double d = (f - F) / VOWEL_BW[j];
                a += g * exp(-0.5 * d * d);
            }
            sine_h(cr, ci, k, (float)a);
        }
        return 1;
    }
    case 7: {   /* Organ: drawbar registrations, about 3 dB per drawbar step */
        double s = t * 7; int i = (int)s; if (i > 6) i = 6;
        double u = s - i;
        for (int b = 0; b < 9; b++) {
            int v0 = ORGAN[i][b] - '0', v1 = ORGAN[i + 1][b] - '0';
            double a0 = v0 ? pow(10, (v0 - 8) * 3.0 / 20) : 0, a1 = v1 ? pow(10, (v1 - 8) * 3.0 / 20) : 0;
            sine_h(cr, ci, ORGAN_H[b], (float)((1 - u) * a0 + u * a1));
        }
        return 1;
    }
    case 9: {   /* Reso: a saw with a resonant peak sweeping up the harmonics */
        double kc = 1 + 47 * t;
        for (int k = 1; k < NH; k++) {
            double d = (k - kc) / 1.2;
            sine_h(cr, ci, k, (float)((0.12 + 3.0 / (1 + d * d)) / k));
        }
        return 1;
    }
    case 10: {  /* Comb: a comb filter whose spacing closes in */
        double d = 0.02 + 0.48 * t;
        for (int k = 1; k < NH; k++) sine_h(cr, ci, k, (float)(pow(fabs(cos(PI * k * d)), 1.5) / pow(k, 0.8) + 1e-4 / k));
        return 1;
    }
    case 11: {  /* Random: eight seeded spectra, morphed */
        float a0[NH], a1[NH];
        double s = t * 7; int i = (int)s; if (i > 6) i = 6;
        double u = s - i;
        random_key(a0, i); random_key(a1, i + 1);
        for (int k = 1; k < NH; k++) sine_h(cr, ci, k, (float)((1 - u) * a0[k] + u * a1[k]));
        return 1;
    }
    }
    return 0;
}

/* frame t of a table drawn in the time domain (Sync, FM, Fold, Digital), TD_N points over one cycle */
static void draw(int table, double t, float *x) {
    for (int n = 0; n < TD_N; n++) {
        double p = (double)n / TD_N, v = 0;
        switch (table) {
        case 4: { double r = 1 + 7 * t, f = r * p - floor(r * p); v = 1 - 2 * f; break; }        /* Sync: hard-synced saw */
        case 5: v = sin(2 * PI * p + 7 * t * sin(2 * PI * p)); break;                                   /* FM: 1:1, index 0..7 */
        case 6: v = 2 / PI * asin(sin(PI / 2 * (1 + 9 * t) * sin(2 * PI * p))); break;                 /* Fold */
        case 8: {                                                                                       /* Digital */
            double steps = floor(128 * pow(4.0 / 128, t)), q = floor(32 * pow(2.0 / 32, t)) / 2;
            v = sin(2 * PI * floor(p * steps) / steps);
            v = floor(v * q + 0.5) / q;
            break;
        }
        }
        x[n] = (float)v;
    }
}


/* ---- built-in tables ---- */
static wtab_t builtin[WT_NB];
static int bi_refs;
static pthread_mutex_t bi_lock = PTHREAD_MUTEX_INITIALIZER;

static int build_builtin(void) {
    float *re = malloc(sizeof(float) * TD_N), *im = malloc(sizeof(float) * TD_N);
    float cr[NH], ci[NH];
    if (!re || !im) { free(re); free(im); return -1; }
    for (int tb = 0; tb < WT_NB; tb++) {
        wtab_t *t = &builtin[tb];
        layout(t, 1024);
        t->nf = BI_NF;
        t->data = malloc(sizeof(float) * (long)t->fsize * BI_NF);
        if (!t->data) { free(re); free(im); return -1; }
        for (int f = 0; f < BI_NF; f++) {
            double ft = (double)f / (BI_NF - 1);
            if (!spectrum(tb, ft, cr, ci)) {
                draw(tb, ft, re);
                memset(im, 0, sizeof(float) * TD_N);
                fft(re, im, TD_N, -1);
                for (int k = 1; k < NH; k++) { cr[k] = 2 * re[k] / TD_N; ci[k] = 2 * im[k] / TD_N; }
            }
            float pk = synth_frame(t, f, cr, ci, re, im);
            scale_frame(t, f, pk > 1e-6f ? 1 / pk : 0);   /* every frame at a peak of 1 */
        }
    }
    free(re); free(im);
    return 0;
}

int wt_builtin_acquire(void) {
    pthread_once(&tw_once, tw_init);
    pthread_mutex_lock(&bi_lock);
    int ok = 0;
    if (!bi_refs && build_builtin()) {
        for (int i = 0; i < WT_NB; i++) { free(builtin[i].data); builtin[i].data = NULL; }
        ok = -1;
    }
    if (!ok) bi_refs++;
    pthread_mutex_unlock(&bi_lock);
    return ok;
}

void wt_builtin_release(void) {
    pthread_mutex_lock(&bi_lock);
    if (bi_refs > 0 && --bi_refs == 0)
        for (int i = 0; i < WT_NB; i++) { free(builtin[i].data); builtin[i].data = NULL; }
    pthread_mutex_unlock(&bi_lock);
}

const wtab_t *wt_builtin(int i) { return &builtin[i < 0 ? 0 : i >= WT_NB ? WT_NB - 1 : i]; }

/* ---- user tables ---- */
wtab_t *wt_from_frames(const float *frames, int nf, int frame_len) {
    pthread_once(&tw_once, tw_init);
    if (nf < 1 || frame_len < 2) return NULL;
    if (nf > WT_MAX_FRAMES) nf = WT_MAX_FRAMES;
    wtab_t *t = calloc(1, sizeof *t);
    float *re = malloc(sizeof(float) * WT_USER_LEN), *im = malloc(sizeof(float) * WT_USER_LEN);
    float *cr = malloc(sizeof(float) * WT_USER_LEN / 2), *ci = malloc(sizeof(float) * WT_USER_LEN / 2);
    if (!t || !re || !im || !cr || !ci) goto fail;
    layout(t, WT_USER_LEN);
    t->nf = nf;
    t->data = malloc(sizeof(float) * (long)t->fsize * nf);
    if (!t->data) goto fail;
    float peak = 0;
    for (int f = 0; f < nf; f++) {
        const float *src = frames + (long)f * frame_len;
        for (int i = 0; i < WT_USER_LEN; i++) {   /* resample to WT_USER_LEN (linear; exact for 2048-sample frames) */
            double x = (double)i * frame_len / WT_USER_LEN;
            int i0 = (int)x, i1 = i0 + 1 < frame_len ? i0 + 1 : 0;
            float fr = (float)(x - i0);
            re[i] = src[i0] + (src[i1] - src[i0]) * fr;
        }
        memset(im, 0, sizeof(float) * WT_USER_LEN);
        fft(re, im, WT_USER_LEN, -1);
        for (int k = 1; k < WT_USER_LEN / 2; k++) { cr[k] = 2 * re[k] / WT_USER_LEN; ci[k] = 2 * im[k] / WT_USER_LEN; }
        float pk = synth_frame(t, f, cr, ci, re, im);   /* the DC is dropped */
        if (pk > peak) peak = pk;
    }
    for (int f = 0; f < nf; f++) scale_frame(t, f, peak > 1e-6f ? 1 / peak : 0);   /* the whole table at a peak of 1 */
    free(re); free(im); free(cr); free(ci);
    return t;
fail:
    if (t) free(t->data);
    free(t); free(re); free(im); free(cr); free(ci);
    return NULL;
}

void wt_free(wtab_t *t) {
    if (!t) return;
    free(t->data);
    free(t);
}
