/* Philtre's effect rack (see ph_fx.h). Every effect is original code: modulated delay lines (ensemble, flanger, chorus,
 * delays), first-order all-passes (phaser), waveshapers (distortion), feed-forward compressors (single and three-band
 * upward/downward), an 8-line feedback delay network (reverb, plate), RBJ biquads (EQ) and a state-variable filter. */
#include <math.h>
#include <pthread.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "ph_fx.h"
#include "ph_params.h"

#define SR 44100.0f
#define PI_F 3.14159265358979f
#define SINE_N 1024

static float sine_tab[SINE_N + 1];
static pthread_once_t sine_once = PTHREAD_ONCE_INIT;
static void sine_init(void) {
    for (int i = 0; i <= SINE_N; i++) sine_tab[i] = (float)sin(2 * 3.14159265358979323846 * i / SINE_N);
}
static inline float fsin(float ph) {   /* sin(2 pi ph), ph in [0, 1) */
    float x = ph * SINE_N;
    int i = (int)x;
    i &= SINE_N - 1;
    return sine_tab[i] + (sine_tab[i + 1] - sine_tab[i]) * (x - (int)x);
}
static inline float wrap1(float p) { return p >= 1 ? p - 1 : p; }
static inline float clampf(float x, float lo, float hi) { return x < lo ? lo : x > hi ? hi : x; }
static inline float sat(float x) {
    if (x > 3) return 1;
    if (x < -3) return -1;
    float x2 = x * x;
    return x * (27 + x2) / (27 + 9 * x2);
}
/* ring buffer read, d samples behind write position w (mask = size - 1, size a power of two) */
static inline float rd(const float *b, int mask, int w, float d) {
    float rp = (float)w - d;
    float fl = floorf(rp);
    int i0 = (int)fl & mask;
    float fr = rp - fl;
    return b[i0] + (b[(i0 + 1) & mask] - b[i0]) * fr;
}
static inline float one_pole(float fc) { return 1 - expf(-2 * PI_F * fc / SR); }
/* fast log2 / exp2 for the compressors' gain computers (about 0.01 dB) */
static inline float flog2(float x) {
    union { float f; unsigned u; } v = {x};
    float e = (float)((int)((v.u >> 23) & 255) - 128);
    v.u = (v.u & 0x007FFFFF) | 0x3F800000;
    float m = v.f;
    return e + (-0.34484843f * m + 2.02466578f) * m - 0.67487759f + 1;
}
static inline float fexp2(float x) {
    if (x < -126) return 0;
    float fl = floorf(x), f = x - fl;
    union { float f; unsigned u; } v;
    v.u = (unsigned)((int)fl + 127) << 23;
    return v.f * (1 + f * (0.6960656421f + f * (0.2244943400f + f * 0.0790201413f)));
}
#define DB2LOG2 0.16609640f   /* dB -> log2 of the amplitude */

static const float DIV_BEATS[12] = {0.125f, 1.0f / 6, 0.25f, 0.375f, 1.0f / 3, 0.5f, 0.75f, 2.0f / 3, 1, 1.5f, 2, 4};
static const char *const DIV_NAMES[12] = {"1/32", "1/16T", "1/16", "1/16D", "1/8T", "1/8", "1/8D", "1/4T", "1/4", "1/4D",
                                          "1/2", "1/1"};
static const char *const DIST_NAMES[9] = {"Tube", "Soft", "Hard", "Diode", "Fold", "Sine Fold", "Rectify", "Downsample",
                                          "Bitcrush"};

static float m_rate(float v) { return 0.05f * powf(100.0f, v); }                 /* 0.05 .. 5 Hz */
static float m_freq(float v) { return 20.0f * powf(1000.0f, v); }                /* 20 Hz .. 20 kHz */
static int m_div(float v) { int i = (int)(v * 12); return i < 0 ? 0 : i > 11 ? 11 : i; }
static int m_dist(float v) { int i = (int)(v * 9); return i < 0 ? 0 : i > 8 ? 8 : i; }
static float m_rt(int plate, float v) { return plate ? 0.3f + 4.7f * v * v : 0.5f + 9.5f * v * v; }

/* buffer floats per type */
#define DL_N 131072
#define RV_LINE 8192
static long need(int type) {
    switch (type) {
    case FX_ENSEMBLE: return 2 * 4096;
    case FX_FLANGER: return 2 * 1024;
    case FX_CHORUS: return 2 * 2048;
    case FX_DELAY: case FX_PING_PONG: return 2L * DL_N;
    case FX_REVERB: case FX_PLATE: return 8L * RV_LINE + 2 * RV_LINE + 8 * 1024;
    default: return 0;
    }
}

void fx_init(fxslot_t *s) {
    pthread_once(&sine_once, sine_init);
    memset(s, 0, sizeof *s);
}

void fx_free(fxslot_t *s) {
    free(s->buf);
    s->buf = NULL;
    s->cap = 0;
}

int fx_prepare(fxslot_t *s, int type) {
    long n = need(type);
    if (n > s->cap) {
        float *b = calloc(n, sizeof(float));
        if (!b) { s->type = FX_OFF; return -1; }
        free(s->buf);
        s->buf = b;
        s->cap = n;
    } else if (type == FX_REVERB || type == FX_PLATE) {
        memset(s->buf + 10L * RV_LINE, 0, 8 * 1024 * sizeof(float));   /* the diffusers; the long lines are guarded by filled */
    } else if (type != FX_DELAY && type != FX_PING_PONG && n) {
        memset(s->buf, 0, n * sizeof(float));   /* short lines: cheap to clear */
    }
    s->filled = 0;
    s->type = type;
    s->w = s->w2 = 0;
    memset(s->ph, 0, sizeof s->ph);
    memset(s->st, 0, sizeof s->st);
    memset(s->c, 0, sizeof s->c);
    for (int i = 0; i < 4; i++) s->last[i] = -1;
    s->ph[1] = 0.25f;
    return 0;
}

/* ---------------- effects ---------------- */
static void mixout(float *L, float *R, int i, float wl, float wr, float mix) {
    L[i] += (wl - L[i]) * mix;
    R[i] += (wr - R[i]) * mix;
}

static void ensemble(fxslot_t *s, const float *p, float *L, float *R, int n) {
    static const float base_ms[2][3] = {{8, 13, 19}, {9.5f, 14.5f, 17}};
    float *b[2] = {s->buf, s->buf + 4096};
    float inc = m_rate(p[0] * 0.8f) / SR, depth = p[1] * 0.006f * SR, width = p[2], mix = p[3];
    for (int i = 0; i < n; i++) {
        b[0][s->w] = L[i];
        b[1][s->w] = R[i];
        float wet[2];
        for (int c = 0; c < 2; c++) {
            float acc = 0;
            for (int v = 0; v < 3; v++) {
                float ph = s->ph[0] + v / 3.0f + c / 6.0f;
                ph -= floorf(ph);
                acc += rd(b[c], 4095, s->w, base_ms[c][v] * 0.001f * SR + depth * (0.5f + 0.5f * fsin(ph)));
            }
            wet[c] = acc * (1.0f / 3);
        }
        float mid = 0.5f * (wet[0] + wet[1]), side = 0.5f * (wet[0] - wet[1]) * width * 1.5f;
        mixout(L, R, i, mid + side, mid - side, mix);
        s->w = (s->w + 1) & 4095;
        s->ph[0] = wrap1(s->ph[0] + inc);
    }
}

static float shape(int mode, float x) {
    switch (mode) {
    case 0: return sat(x + 0.25f) - 0.2449f;                      /* tube: biased, asymmetric */
    case 1: return sat(x);                                        /* soft */
    case 2: return clampf(x, -1, 1);                              /* hard */
    case 3: return x > 0 ? sat(x) : 0.4f * sat(x * 0.4f);         /* diode */
    case 4: { float t = 0.25f * x + 0.25f; t -= floorf(t); return 4 * fabsf(t - 0.5f) - 1; }   /* triangle fold */
    case 5: { float t = 0.25f * x; t -= floorf(t); return fsin(t); }                          /* sine fold */
    case 6: return 2 * sat(fabsf(x)) - 1;                         /* rectify (an octave up) */
    default: return x;
    }
}

static void distortion(fxslot_t *s, const float *p, float *L, float *R, int n) {
    int mode = m_dist(p[1]);
    float g = powf(10.0f, 36 * p[0] / 20), comp = 1 / (1 + 0.6f * p[0]);
    float tc = one_pole(800.0f * powf(25.0f, p[2])), mix = p[3];
    float *st = s->st;   /* 0,1 tone LP; 2 hold counter; 3,4 held samples; 5..8 DC blocker x/y */
    int hold = 1 + (int)(p[0] * 40);
    float q = powf(2.0f, 16 - 14 * p[0]) * 0.5f;
    for (int i = 0; i < n; i++) {
        float x[2] = {L[i], R[i]}, y[2];
        if (mode == 7) {                                            /* downsample: drive is the amount */
            if (st[2] <= 0) { st[3] = x[0]; st[4] = x[1]; st[2] = (float)hold; }
            st[2] -= 1;
            y[0] = st[3]; y[1] = st[4];
        } else if (mode == 8) {                                     /* bitcrush: drive is the amount */
            y[0] = floorf(x[0] * q + 0.5f) / q; y[1] = floorf(x[1] * q + 0.5f) / q;
        } else {
            y[0] = shape(mode, x[0] * g) * comp; y[1] = shape(mode, x[1] * g) * comp;
        }
        for (int c = 0; c < 2; c++) {
            st[c] += (y[c] - st[c]) * tc;                          /* tone */
            float d = st[c] - st[5 + c] + 0.9993f * st[7 + c];       /* DC blocker */
            st[5 + c] = st[c]; st[7 + c] = d;
            y[c] = d;
        }
        mixout(L, R, i, y[0], y[1], mix);
    }
}

static void flanger(fxslot_t *s, const float *p, float *L, float *R, int n) {
    float *b[2] = {s->buf, s->buf + 1024};
    float inc = m_rate(p[0]) / SR, depth = p[1] * 0.004f * SR, fb = p[2] * 0.9f, mix = p[3], base = 0.0003f * SR;
    for (int i = 0; i < n; i++) {
        float x[2] = {L[i], R[i]}, y[2];
        for (int c = 0; c < 2; c++) {
            float ph = wrap1(s->ph[0] + 0.25f * c);
            y[c] = rd(b[c], 1023, s->w, base + depth * (0.5f + 0.5f * fsin(ph)));
            b[c][s->w] = x[c] + fb * y[c];
        }
        mixout(L, R, i, y[0], y[1], mix);
        s->w = (s->w + 1) & 1023;
        s->ph[0] = wrap1(s->ph[0] + inc);
    }
}

static void phaser(fxslot_t *s, const float *p, float *L, float *R, int n) {
    float inc = m_rate(p[0]) / SR, depth = p[1], fb = p[2] * 0.85f, mix = p[3];
    float *z = s->st;   /* 0..5 left all-pass states, 6..11 right, 12,13 last outputs */
    for (int i0 = 0; i0 < n; i0 += 16) {
        int m = n - i0 < 16 ? n - i0 : 16;
        float a[2];
        for (int c = 0; c < 2; c++) {
            float lfo = 0.5f + 0.5f * fsin(wrap1(s->ph[0] + 0.25f * c));
            float f = 200.0f * powf(2.0f, depth * 5 * lfo), t = tanf(PI_F * clampf(f, 20, 18000) / SR);
            a[c] = (t - 1) / (t + 1);
        }
        for (int i = i0; i < i0 + m; i++) {
            float x[2] = {L[i], R[i]}, y[2];
            for (int c = 0; c < 2; c++) {
                float v = x[c] + fb * z[12 + c];
                for (int k = 0; k < 6; k++) {
                    float o = a[c] * v + z[6 * c + k];
                    z[6 * c + k] = v - a[c] * o;
                    v = o;
                }
                z[12 + c] = v;
                y[c] = 0.5f * (x[c] + v);
            }
            mixout(L, R, i, y[0], y[1], mix);
        }
        s->ph[0] = wrap1(s->ph[0] + inc * m);
    }
}

static void chorus(fxslot_t *s, const float *p, float *L, float *R, int n) {
    float *b[2] = {s->buf, s->buf + 2048};
    float inc = m_rate(p[0]) / SR, depth = p[1] * 0.006f * SR, base = (0.005f + p[2] * 0.02f) * SR, mix = p[3];
    for (int i = 0; i < n; i++) {
        b[0][s->w] = L[i];
        b[1][s->w] = R[i];
        float wet[2];
        for (int c = 0; c < 2; c++) {
            float a = rd(b[c], 2047, s->w, base + depth * (0.5f + 0.5f * fsin(wrap1(s->ph[0] + 0.25f * c))));
            float bb = rd(b[c], 2047, s->w, base * 1.3f + depth * (0.5f + 0.5f * fsin(wrap1(s->ph[0] + 0.5f + 0.25f * c))));
            wet[c] = 0.5f * (a + bb);
        }
        mixout(L, R, i, wet[0], wet[1], mix);
        s->w = (s->w + 1) & 2047;
        s->ph[0] = wrap1(s->ph[0] + inc);
    }
}

static void delay(fxslot_t *s, const float *p, float bpm, int ping, float *L, float *R, int n) {
    float *bl = s->buf, *br = s->buf + DL_N;
    float tgt = clampf(DIV_BEATS[m_div(p[0])] * 60.0f / bpm * SR, 16, DL_N - 4);
    float *st = s->st;   /* 0 current delay, 1,2 low-pass, 3,4 high-pass */
    if (st[0] <= 0) st[0] = tgt;
    float fb = p[1] * 0.95f, mix = p[3];
    float t = p[2], lpc = 1, hpc = 0;
    if (t < 0.5f) lpc = one_pole(20000.0f * powf(0.04f, (0.5f - t) * 2));    /* darker */
    else hpc = one_pole(20.0f * powf(100.0f, (t - 0.5f) * 2));               /* thinner */
    for (int i = 0; i < n; i++) {
        st[0] += (tgt - st[0]) * 0.0007f;
        float yl = 0, yr = 0;
        if (st[0] + 2 < s->filled) { yl = rd(bl, DL_N - 1, s->w, st[0]); yr = rd(br, DL_N - 1, s->w, st[0]); }
        st[1] += (yl - st[1]) * lpc; st[2] += (yr - st[2]) * lpc;
        st[3] += (st[1] - st[3]) * hpc; st[4] += (st[2] - st[4]) * hpc;
        float fl = st[1] - st[3], fr = st[2] - st[4];
        if (ping) { bl[s->w] = 0.5f * (L[i] + R[i]) + fb * fr; br[s->w] = fb * fl; }
        else { bl[s->w] = L[i] + fb * fl; br[s->w] = R[i] + fb * fr; }
        mixout(L, R, i, yl, yr, mix);
        s->w = (s->w + 1) & (DL_N - 1);
        if (s->filled < DL_N) s->filled++;
    }
}

static void compressor(fxslot_t *s, const float *p, float *L, float *R, int n) {
    float thr = -40 * p[0], ratio = powf(20.0f, p[1]), slope = 1 - 1 / ratio, mix = p[3];
    float att = 1 - expf(-1 / (0.0001f * powf(300.0f, p[2]) * SR)), rel = 1 - expf(-1 / (0.02f * powf(25.0f, p[2]) * SR));
    float makeup = -thr * slope * 0.5f;
    float *g = &s->st[0];   /* smoothed gain reduction, dB */
    for (int i = 0; i < n; i++) {
        float lv = fmaxf(fabsf(L[i]), fabsf(R[i])) + 1e-9f;
        float db = flog2(lv) * 6.0206f;
        float red = db > thr ? (db - thr) * slope : 0;
        *g += (red - *g) * (red > *g ? att : rel);
        float gain = fexp2((makeup - *g) * DB2LOG2);
        mixout(L, R, i, L[i] * gain, R[i] * gain, mix);
    }
}

static void multiband(fxslot_t *s, const float *p, float *L, float *R, int n) {
    /* three bands (under 120 Hz, 120 Hz - 2.5 kHz, over 2.5 kHz) split by subtraction, so they sum back exactly; each band is
     * pushed down above -24 dB and, with Upward, pulled up below -42 dB, towards a dense, even level */
    float depth = p[0], up = p[2], mix = p[3];
    float att = 1 - expf(-1 / (0.0005f * powf(20.0f, p[1]) * SR)), rel = 1 - expf(-1 / (0.02f * powf(15.0f, p[1]) * SR));
    float sd = depth * (1 - 1 / 16.0f), su = depth * up * 0.75f;
    float g1 = tanf(PI_F * 120 / SR), g2 = tanf(PI_F * 2500 / SR), k = 1.41421f;
    float a1[2] = {1 / (1 + g1 * (g1 + k)), 1 / (1 + g2 * (g2 + k))}, gg[2] = {g1, g2};
    float *st = s->st;   /* 0..7 filter states [ch][filter][2], 8..10 band followers, 11..13 band gains (dB) */
    for (int i = 0; i < n; i++) {
        float band[2][3];
        float x[2] = {L[i], R[i]};
        for (int c = 0; c < 2; c++) {
            float in = x[c], low = 0, mid = 0;
            for (int f = 0; f < 2; f++) {
                float *ic = &st[c * 4 + f * 2], g = gg[f], A1 = a1[f], A2 = g * A1, A3 = g * A2;
                float v3 = in - ic[1], v1 = A1 * ic[0] + A2 * v3, v2 = ic[1] + A2 * ic[0] + A3 * v3;
                ic[0] = 2 * v1 - ic[0]; ic[1] = 2 * v2 - ic[1];
                if (f == 0) { low = v2; in = x[c] - low; }
                else mid = v2;
            }
            band[c][0] = low; band[c][1] = mid; band[c][2] = in - mid;
        }
        float yl = 0, yr = 0;
        for (int b = 0; b < 3; b++) {
            float lv = fmaxf(fabsf(band[0][b]), fabsf(band[1][b]));
            float *e = &st[8 + b];
            *e += (lv - *e) * (lv > *e ? att : rel);
            float db = flog2(*e + 1e-9f) * 6.0206f, gdb = 0;
            if (db > -24) gdb = -(db + 24) * sd;
            else if (db < -42 && db > -80) gdb = fminf((-42 - db) * su, 24);
            float gain = fexp2(gdb * DB2LOG2);
            yl += band[0][b] * gain;
            yr += band[1][b] * gain;
        }
        mixout(L, R, i, yl, yr, mix);
    }
}

static void reverb(fxslot_t *s, const float *p, int plate, float *L, float *R, int n) {
    static const int HALL[8] = {1433, 1601, 1867, 2053, 2251, 2399, 2617, 2797};
    static const int PLATE_L[8] = {557, 709, 853, 1013, 1151, 1319, 1471, 1609};
    static const int AP_H[4] = {142, 107, 379, 277}, AP_P[4] = {113, 89, 211, 167};
    const int *base = plate ? PLATE_L : HALL, *apl = plate ? AP_P : AP_H;
    float scale = plate ? 0.5f + 1.1f * p[0] : 0.6f + 1.8f * p[0];
    float rt = m_rt(plate, p[0]), damp = one_pole(18000.0f * powf(0.06f, p[1]));
    float pre = p[2] * 0.15f * SR, mix = p[3], apg = plate ? 0.75f : 0.7f;
    float *lines = s->buf, *pd = s->buf + 8 * RV_LINE, *ap = pd + 2 * RV_LINE;
    float len[8], g[8];
    for (int k = 0; k < 8; k++) {
        len[k] = base[k] * scale;
        g[k] = powf(10.0f, -3 * len[k] / (rt * SR));
    }
    float *lp = s->st, inc = 0.7f / SR;   /* lp: 8 damping states; ph[0]: line modulation */
    for (int i = 0; i < n; i++) {
        pd[s->w] = L[i];
        pd[RV_LINE + s->w] = R[i];
        float in[2] = {0, 0};
        if (pre + 2 < s->filled) { in[0] = rd(pd, RV_LINE - 1, s->w, pre + 1); in[1] = rd(pd + RV_LINE, RV_LINE - 1, s->w, pre + 1); }
        for (int c = 0; c < 2; c++)   /* input diffusion: four all-passes per channel */
            for (int a = 0; a < 4; a++) {
                float *bf = ap + (c * 4 + a) * 1024;
                float d = bf[(s->w2 - apl[a] - c * 7) & 1023];
                float v = in[c] + apg * d;
                bf[s->w2] = v;
                in[c] = d - apg * v;
            }
        float md = 6 * fsin(s->ph[0]), o[8], sum = 0;
        for (int k = 0; k < 8; k++) {
            float d = len[k] + (k == 0 ? md : k == 1 ? -md : 0);
            float y = d + 2 < s->filled ? rd(lines + k * RV_LINE, RV_LINE - 1, s->w, d) : 0;
            lp[k] += (y - lp[k]) * damp;
            o[k] = lp[k];
            sum += o[k];
        }
        sum *= 0.25f;   /* Householder: o - 2/N * sum */
        for (int k = 0; k < 8; k++) lines[k * RV_LINE + s->w] = (o[k] - sum) * g[k] + in[k & 1] * 0.3f;
        float wl = (o[0] - o[2] + o[4] - o[6]) * 0.5f, wr = (o[1] - o[3] + o[5] - o[7]) * 0.5f;
        mixout(L, R, i, wl, wr, mix);
        s->w = (s->w + 1) & (RV_LINE - 1);
        s->w2 = (s->w2 + 1) & 1023;
        if (s->filled < RV_LINE) s->filled++;
        s->ph[0] = wrap1(s->ph[0] + inc);
    }
}

static void biquad_shelf(float *c, int high, float f, float db) {   /* RBJ shelf, slope 1 */
    float A = powf(10.0f, db / 40), w = 2 * PI_F * f / SR, cs = cosf(w), sn = sinf(w);
    float al = sn / 2 * sqrtf(2.0f), sa = 2 * sqrtf(A) * al, b0, b1, b2, a0, a1, a2;
    if (!high) {
        b0 = A * ((A + 1) - (A - 1) * cs + sa); b1 = 2 * A * ((A - 1) - (A + 1) * cs); b2 = A * ((A + 1) - (A - 1) * cs - sa);
        a0 = (A + 1) + (A - 1) * cs + sa; a1 = -2 * ((A - 1) + (A + 1) * cs); a2 = (A + 1) + (A - 1) * cs - sa;
    } else {
        b0 = A * ((A + 1) + (A - 1) * cs + sa); b1 = -2 * A * ((A - 1) + (A + 1) * cs); b2 = A * ((A + 1) + (A - 1) * cs - sa);
        a0 = (A + 1) - (A - 1) * cs + sa; a1 = 2 * ((A - 1) - (A + 1) * cs); a2 = (A + 1) - (A - 1) * cs - sa;
    }
    c[0] = b0 / a0; c[1] = b1 / a0; c[2] = b2 / a0; c[3] = a1 / a0; c[4] = a2 / a0;
}
static void biquad_peak(float *c, float f, float db, float q) {
    float A = powf(10.0f, db / 40), w = 2 * PI_F * f / SR, cs = cosf(w), al = sinf(w) / (2 * q);
    float a0 = 1 + al / A;
    c[0] = (1 + al * A) / a0; c[1] = -2 * cs / a0; c[2] = (1 - al * A) / a0; c[3] = -2 * cs / a0; c[4] = (1 - al / A) / a0;
}
static inline float bq(const float *c, float *z, float x) {   /* transposed direct form II */
    float y = c[0] * x + z[0];
    z[0] = c[1] * x - c[3] * y + z[1];
    z[1] = c[2] * x - c[4] * y;
    return y;
}

static void eq(fxslot_t *s, const float *p, float *L, float *R, int n) {
    float *c = s->c;   /* low shelf 0..4 and peak 5..9 here; the high shelf in st[40..44] */
    float *ch = &s->st[40];
    if (p[0] != s->last[0] || p[1] != s->last[1] || p[2] != s->last[2] || p[3] != s->last[3]) {
        biquad_shelf(c, 0, 200, (p[0] - 0.5f) * 36);
        biquad_peak(c + 5, m_freq(0.25f + 0.5f * p[1]), (p[2] - 0.5f) * 36, 0.9f);
        biquad_shelf(ch, 1, 5000, (p[3] - 0.5f) * 36);
        memcpy(s->last, p, sizeof s->last);
    }
    float *z = s->st;   /* 0..11: [ch][biquad][2] */
    for (int i = 0; i < n; i++) {
        L[i] = bq(ch, z + 4, bq(c + 5, z + 2, bq(c, z, L[i])));
        R[i] = bq(ch, z + 10, bq(c + 5, z + 8, bq(c, z + 6, R[i])));
    }
}

static void filter(fxslot_t *s, const float *p, float *L, float *R, int n) {
    float g = tanf(PI_F * clampf(m_freq(p[0]), 20, 0.45f * SR) / SR), k = 2 - 1.95f * p[1], mix = p[3];
    float *z = s->st, *cur = s->c;   /* z: [ch][2]; c[0], c[1]: g and k of the last block, ramped */
    if (cur[0] <= 0) { cur[0] = g; cur[1] = k; }
    float dg = (g - cur[0]) / n, dk = (k - cur[1]) / n, m = p[2];
    float wl = m < 0.5f ? 1 - 2 * m : 0, wb = m < 0.5f ? 2 * m : 2 - 2 * m, wh = m < 0.5f ? 0 : 2 * m - 1;
    for (int i = 0; i < n; i++) {
        cur[0] += dg; cur[1] += dk;
        float G = cur[0], K = cur[1], a1 = 1 / (1 + G * (G + K)), a2 = G * a1, a3 = G * a2;
        float x[2] = {L[i], R[i]}, y[2];
        for (int ch = 0; ch < 2; ch++) {
            float *ic = z + 2 * ch, v3 = x[ch] - ic[1], v1 = a1 * ic[0] + a2 * v3, v2 = ic[1] + a2 * ic[0] + a3 * v3;
            ic[0] = 2 * v1 - ic[0]; ic[1] = 2 * v2 - ic[1];
            y[ch] = sat(wl * v2 + wb * v1 * K + wh * (x[ch] - K * v1 - v2));
        }
        mixout(L, R, i, y[0], y[1], mix);
    }
}

void fx_process(fxslot_t *s, const float p0[4], float bpm, float *L, float *R, int n) {
    float p[4];
    for (int i = 0; i < 4; i++) p[i] = clampf(p0[i], 0, 1);
    if (bpm < 20) bpm = 120;
    switch (s->type) {
    case FX_ENSEMBLE: ensemble(s, p, L, R, n); break;
    case FX_DISTORTION: distortion(s, p, L, R, n); break;
    case FX_FLANGER: flanger(s, p, L, R, n); break;
    case FX_PHASER: phaser(s, p, L, R, n); break;
    case FX_CHORUS: chorus(s, p, L, R, n); break;
    case FX_DELAY: delay(s, p, bpm, 0, L, R, n); break;
    case FX_PING_PONG: delay(s, p, bpm, 1, L, R, n); break;
    case FX_COMPRESSOR: compressor(s, p, L, R, n); break;
    case FX_MULTIBAND: multiband(s, p, L, R, n); break;
    case FX_REVERB: reverb(s, p, 0, L, R, n); break;
    case FX_PLATE: reverb(s, p, 1, L, R, n); break;
    case FX_EQ: eq(s, p, L, R, n); break;
    case FX_FILTER: filter(s, p, L, R, n); break;
    default: break;
    }
}

/* ---------------- names and value texts ---------------- */
static const char *const LABELS[N_FX][4] = {
    {"-", "-", "-", "-"},
    {"RATE", "DEPTH", "WIDTH", "MIX"},            /* ensemble */
    {"DRIVE", "MODE", "TONE", "MIX"},             /* distortion */
    {"RATE", "DEPTH", "FEEDBACK", "MIX"},         /* flanger */
    {"RATE", "DEPTH", "FEEDBACK", "MIX"},         /* phaser */
    {"RATE", "DEPTH", "DELAY", "MIX"},            /* chorus */
    {"TIME", "FEEDBACK", "TONE", "MIX"},          /* delay */
    {"TIME", "FEEDBACK", "TONE", "MIX"},          /* ping pong */
    {"THRESHOLD", "RATIO", "SPEED", "MIX"},       /* compressor */
    {"DEPTH", "TIME", "UPWARD", "MIX"},           /* multiband */
    {"SIZE", "DAMPING", "PRE-DELAY", "MIX"},      /* reverb */
    {"SIZE", "DAMPING", "PRE-DELAY", "MIX"},      /* plate */
    {"LOW", "MID FREQ", "MID GAIN", "HIGH"},      /* EQ */
    {"CUTOFF", "RESONANCE", "LP > HP", "MIX"},    /* filter */
};

const char *fx_label(int type, int k) {
    if (type < 0 || type >= N_FX || k < 0 || k > 3) return "-";
    return LABELS[type][k];
}

static int hz(char *b, int n, float f) {
    return f < 1000 ? snprintf(b, n, "%.0f Hz", f) : snprintf(b, n, "%.2f kHz", f / 1000);
}
static int rate(char *b, int n, float f) { return snprintf(b, n, f < 1 ? "%.2f Hz" : "%.1f Hz", f); }

int fx_display(int type, int k, float v, float bpm, char *b, int n) {
    (void)bpm;
    v = clampf(v, 0, 1);
    if (type == FX_OFF) return snprintf(b, n, "-");
    if (k == 3 && type != FX_EQ) return snprintf(b, n, "%.0f%%", v * 100);
    switch (type) {
    case FX_ENSEMBLE:
        return k == 0 ? rate(b, n, m_rate(v * 0.8f)) : snprintf(b, n, "%.0f%%", v * 100);
    case FX_DISTORTION:
        if (k == 0) return snprintf(b, n, "+%.1f dB", 36 * v);
        if (k == 1) return snprintf(b, n, "%s", DIST_NAMES[m_dist(v)]);
        return hz(b, n, 800.0f * powf(25.0f, v));
    case FX_FLANGER: case FX_PHASER:
        return k == 0 ? rate(b, n, m_rate(v)) : snprintf(b, n, "%.0f%%", v * 100);
    case FX_CHORUS:
        if (k == 0) return rate(b, n, m_rate(v));
        if (k == 2) return snprintf(b, n, "%.1f ms", 5 + v * 20);
        return snprintf(b, n, "%.0f%%", v * 100);
    case FX_DELAY: case FX_PING_PONG:
        if (k == 0) return snprintf(b, n, "%s", DIV_NAMES[m_div(v)]);
        if (k == 2) return v < 0.48f ? snprintf(b, n, "Dark %.0f%%", (0.5f - v) * 200)
                         : v > 0.52f ? snprintf(b, n, "Thin %.0f%%", (v - 0.5f) * 200) : snprintf(b, n, "Flat");
        return snprintf(b, n, "%.0f%%", v * 100);
    case FX_COMPRESSOR:
        if (k == 0) return snprintf(b, n, "%.1f dB", -40 * v);
        if (k == 1) return snprintf(b, n, "%.1f:1", powf(20.0f, v));
        return snprintf(b, n, "%.1f/%.0f ms", 0.1f * powf(300.0f, v), 20.0f * powf(25.0f, v));
    case FX_MULTIBAND:
        return snprintf(b, n, "%.0f%%", v * 100);
    case FX_REVERB: case FX_PLATE:
        if (k == 0) return snprintf(b, n, "%.1f s", m_rt(type == FX_PLATE, v));
        if (k == 2) return snprintf(b, n, "%.0f ms", v * 150);
        return snprintf(b, n, "%.0f%%", v * 100);
    case FX_EQ:
        if (k == 1) return hz(b, n, m_freq(0.25f + 0.5f * v));
        return snprintf(b, n, "%+.1f dB", (v - 0.5f) * 36);
    case FX_FILTER:
        if (k == 0) return hz(b, n, m_freq(v));
        if (k == 2) return snprintf(b, n, v < 0.25f ? "LP" : v < 0.75f ? "BP" : "HP");
        return snprintf(b, n, "%.0f%%", v * 100);
    }
    return snprintf(b, n, "%.0f%%", v * 100);
}
