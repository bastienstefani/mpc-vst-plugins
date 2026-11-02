/* Osmium's built-in wavetables (see osm_wt.h): twelve tables of 64 frames, each frame described as a harmonic spectrum
 * (or drawn in the time domain and analysed with an FFT), then resynthesised once per band-limit level with an inverse FFT.
 * Everything is computed here; no sample content is shipped. */
#include <math.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include "osm_wt.h"
#include "osm_params.h"

const int WT_LEN[WT_NL] = {1024, 512, 256, 128, 64, 32, 32, 32, 32};
const int WT_HARM[WT_NL] = {511, 255, 127, 63, 31, 15, 7, 3, 1};
const int WT_OFF[WT_NL] = {0, 1024, 1536, 1792, 1920, 1984, 2016, 2048, 2080};

float *wt_data;
static int wt_refs;
static pthread_mutex_t wt_lock = PTHREAD_MUTEX_INITIALIZER;

#define TD_N 4096          /* time-domain drawing resolution for the drawn tables */
#define NH 512             /* harmonics 1..511 */
#define PI 3.14159265358979323846

static float tw_c[TD_N / 2], tw_s[TD_N / 2];

/* in-place complex radix-2 FFT, n a power of two <= TD_N; sign -1 forward, +1 inverse (unnormalised) */
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

/* ---- spectra: C[k] is the complex amplitude of harmonic k, so the frame is x(p) = sum Re(C[k] e^(2 pi i k p)) ---- */

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

static void build(void) {
    static float re[TD_N], im[TD_N], cr[NH], ci[NH];
    for (int k = 0; k < TD_N / 2; k++) { tw_c[k] = (float)cos(2 * PI * k / TD_N); tw_s[k] = (float)sin(2 * PI * k / TD_N); }
    for (int tb = 0; tb < WT_NT; tb++)
        for (int f = 0; f < WT_NF; f++) {
            double t = (double)f / (WT_NF - 1);
            if (!spectrum(tb, t, cr, ci)) {
                draw(tb, t, re);
                memset(im, 0, sizeof im);
                fft(re, im, TD_N, -1);
                for (int k = 1; k < NH; k++) { cr[k] = 2 * re[k] / TD_N; ci[k] = 2 * im[k] / TD_N; }
            }
            float gain = 1;
            for (int l = 0; l < WT_NL; l++) {
                int n = WT_LEN[l], h = WT_HARM[l];
                memset(re, 0, n * sizeof *re);
                memset(im, 0, n * sizeof *im);
                for (int k = 1; k <= h; k++) {
                    re[k] = cr[k] / 2; im[k] = ci[k] / 2;
                    re[n - k] = cr[k] / 2; im[n - k] = -ci[k] / 2;
                }
                fft(re, im, n, 1);
                if (l == 0) {   /* every level of a frame gets the gain that brings the full-band frame to a peak of 1 */
                    float pk = 0;
                    for (int i = 0; i < n; i++) if (fabsf(re[i]) > pk) pk = fabsf(re[i]);
                    gain = pk > 1e-6f ? 1 / pk : 0;
                }
                float *out = wt_data + ((long)tb * WT_NF + f) * WT_FRAME + WT_OFF[l];
                for (int i = 0; i < n; i++) out[i] = re[i] * gain;
            }
        }
}

int wt_acquire(void) {
    pthread_mutex_lock(&wt_lock);
    if (!wt_refs) {
        wt_data = malloc(sizeof(float) * WT_NT * WT_NF * WT_FRAME);
        if (!wt_data) { pthread_mutex_unlock(&wt_lock); return -1; }
        build();
    }
    wt_refs++;
    pthread_mutex_unlock(&wt_lock);
    return 0;
}

void wt_release(void) {
    pthread_mutex_lock(&wt_lock);
    if (wt_refs > 0 && --wt_refs == 0) { free(wt_data); wt_data = NULL; }
    pthread_mutex_unlock(&wt_lock);
}
