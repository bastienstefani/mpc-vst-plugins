/* Philtre: an 8-voice wavetable synth engine for MPC OS standalone devices (wrapper/engine.h). Per voice: oscillators A
 * and B (built-in or user wavetables, up to 8 unison voices with detune, blend and stereo width, start/random phase, and
 * a warp mode), a sub oscillator, a noise source, and a stereo filter each source can be routed through or around;
 * four ADSR envelopes (Env 1 is the amp), four LFOs (free, retriggered or one-shot), four macros and a 16-slot
 * matrix of source x aux -> destination. Then an 8-slot effect rack (ph_fx.c). 44.1 kHz, 128-frame blocks; modulation
 * every CR samples, levels and filter coefficients ramped per sample. Parameters: vst/make_params.py (ph_params.h). */
#include <math.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "engine.h"
#include "ph_params.h"
#include "ph_wt.h"
#include "ph_wav.h"
#include "ph_fx.h"

#define SR 44100.0f
#define CR 16
#define MAXV 8
#define MAXU 8
#define COMB_N 2048
#define PI_F 3.14159265358979f

/* ---------------- floating point: flush denormals while rendering ---------------- */
#if defined(__arm__)
typedef unsigned fpmode_t;
static inline fpmode_t fp_enter(void) { fpmode_t r; __asm__ volatile("vmrs %0, fpscr" : "=r"(r)); __asm__ volatile("vmsr fpscr, %0" :: "r"(r | (1u << 24))); return r; }
static inline void fp_leave(fpmode_t r) { __asm__ volatile("vmsr fpscr, %0" :: "r"(r)); }
#elif defined(__aarch64__)
typedef unsigned long fpmode_t;
static inline fpmode_t fp_enter(void) { fpmode_t r; __asm__ volatile("mrs %0, fpcr" : "=r"(r)); __asm__ volatile("msr fpcr, %0" :: "r"(r | (1ul << 24))); return r; }
static inline void fp_leave(fpmode_t r) { __asm__ volatile("msr fpcr, %0" :: "r"(r)); }
#elif defined(__SSE__) || defined(__x86_64__)
#include <xmmintrin.h>
typedef unsigned fpmode_t;
static inline fpmode_t fp_enter(void) { fpmode_t r = _mm_getcsr(); _mm_setcsr(r | 0x8040); return r; }
static inline void fp_leave(fpmode_t r) { _mm_setcsr(r); }
#else
typedef int fpmode_t;
static inline fpmode_t fp_enter(void) { return 0; }
static inline void fp_leave(fpmode_t r) { (void)r; }
#endif

static inline float clampf(float x, float lo, float hi) { return x < lo ? lo : x > hi ? hi : x; }
static inline float sat(float x) {
    if (x > 3) return 1;
    if (x < -3) return -1;
    float x2 = x * x;
    return x * (27 + x2) / (27 + 9 * x2);
}
static inline float softclip(float x) {
    float a = fabsf(x);
    if (a <= 0.75f) return x;
    float y = 0.75f + 0.25f * sat((a - 0.75f) * 4);
    return x < 0 ? -y : y;
}
static inline float note_hz(float n) { return 440.0f * exp2f((n - 69.0f) * (1.0f / 12)); }
static inline float exp2_small(float x) {   /* 2^x for |x| < 0.1 */
    float t = x * 0.69314718f;
    return 1 + t * (1 + t * (0.5f + t * (1.0f / 6)));
}

#define SINE_N 1024
static float sine_tab[SINE_N + 1];
static pthread_once_t sine_once = PTHREAD_ONCE_INIT;
static void sine_init(void) { for (int i = 0; i <= SINE_N; i++) sine_tab[i] = (float)sin(2 * 3.14159265358979323846 * i / SINE_N); }
static inline float fsin(float ph) {
    float x = ph * SINE_N;
    int i = (int)x;
    float fr = x - i;
    i &= SINE_N - 1;
    return sine_tab[i] + (sine_tab[i + 1] - sine_tab[i]) * fr;
}

/* ---------------- value mappings shared by the DSP and the display ---------------- */
static float map_freq(float x) { return 20.0f * powf(1000.0f, x / 100); }
static float map_etime(float x) { return 0.0005f * powf(20000.0f, x / 100); }
static float map_lrate(float x) { return 0.02f * powf(2000.0f, x / 100); }
static float map_rise(float x) { return 5.0f * (x / 100) * (x / 100); }
static float map_glide(float x) { return x <= 0 ? 0 : 0.002f * powf(1000.0f, x / 100); }
static float map_detune(float x) { float d = x / 100; return 0.75f * d * sqrtf(d); }   /* +- semitones at the edges */

enum { E_IDLE, E_ATT, E_DEC, E_REL };

typedef struct { float ic1, ic2, a1, a2, a3, k, da1, da2, da3, dk; } svf_t;

typedef struct {
    float ph[MAXU], inc[MAXU], gl[MAXU], gr[MAXU], wm[MAXU];
    const wtab_t *tab;
    int f0, lvl, nu, on, warp, need_mono;
    float ff, wamt, lev, levt, dlev;
    float mono[CR];               /* this chunk's output (before level), for the other oscillator's FM / AM / RM */
} osc_t;

typedef struct {
    int active, note, gate, pedal;
    unsigned order;
    float vel, rnd, pitch, target;
    osc_t o[2];
    float sph, sinc, slev, slevt, sgl, sgr;
    float nlev, nlevt, ngl, ngr, nb[4], nsh[2], ncount, nrate;
    unsigned nseed;
    svf_t sv[2][3];               /* [channel][stage] */
    float lad[2][4], lg, lk, lgt, lkt;
    float comb[2][COMB_N], cdel, cdelt, cfb;
    int cw;
    float ap[2][5], apa, apat, apfb;
    float fdrive, fmix;
    float env[4];
    int est[4];
    float lph[4], lra[4], lrb[4], lrise[4], lrate_mod[4];
    int ldone[4];
    float amp, ampt, pl, pr, plt, prt;
} voice_t;

typedef struct {
    float v[NP];
    int dirty;
    /* derived */
    float ecoef[4][3], esus[4], glide_c, vol_g, ftbase, fkey, fenv, fres, fdrive, fmix, lrise_t[4];
    int nslot, slot_src[N_SLOTS], slot_aux[N_SLOTS], slot_dst[N_SLOTS];
    float slot_amt[N_SLOTS];
    /* play state */
    voice_t vc[MAXV];
    unsigned order;
    int stack[16], nstack, sustain, last_voice;
    float last_pitch, modwheel, expr, chan_at, bend, polyat[128], bpm;
    unsigned seed;
    float glph[4], glra[4], glrb[4], glval[4];
    /* user wavetables */
    ph_lib_t *lib;
    wtab_t *utab[2];
    int ufile[2];
    wtab_t *retire[4];
    int nretire;
    char pend_name[2][256];       /* a table restored by file name before the library knew it */
    /* effects */
    fxslot_t fx[FX_SLOTS];
    float fxmod[FX_SLOTS][2];
    float gain_cur, dc_x[2], dc_y[2];
    float mixL[128], mixR[128];
} ph_t;

/* ---------------- parameter lookup ---------------- */
#define HT 1024
static short htab[HT];
static pthread_once_t ht_once = PTHREAD_ONCE_INIT;
static unsigned hash(const char *s, size_t n) {
    unsigned h = 2166136261u;
    for (size_t i = 0; i < n && s[i]; i++) h = (h ^ (unsigned char)s[i]) * 16777619u;
    return h;
}
static void ht_init(void) {
    for (int i = 0; i < HT; i++) htab[i] = -1;
    for (int p = 0; p < NP; p++) {
        unsigned h = hash(PH_P[p].key, 64) & (HT - 1);
        while (htab[h] >= 0) h = (h + 1) & (HT - 1);
        htab[h] = (short)p;
    }
    pthread_once(&sine_once, sine_init);
}
static int find_param(const char *k, size_t n) {
    unsigned h = hash(k, n) & (HT - 1);
    for (; htab[h] >= 0; h = (h + 1) & (HT - 1)) {
        const char *pk = PH_P[htab[h]].key;
        if (strlen(pk) == n && !strncmp(pk, k, n)) return htab[h];
    }
    return -1;
}

/* ---------------- derived values ---------------- */
static float tick_coef(float tau) { return tau <= 0 ? 1 : 1 - expf(-(float)CR / (tau * SR)); }

static void derive(ph_t *s) {
    const float *v = s->v;
    for (int e = 0; e < 4; e++) {
        int b = P_E1_A + 4 * e;
        s->ecoef[e][0] = tick_coef(map_etime(v[b]) / 1.466f);
        s->ecoef[e][1] = tick_coef(map_etime(v[b + 1]) / 4);
        s->esus[e] = v[b + 2] / 100;
        s->ecoef[e][2] = tick_coef(map_etime(v[b + 3]) / 4);
    }
    float gt = map_glide(v[P_GLIDE]);
    s->glide_c = gt > 0 ? tick_coef(gt / 3) : 1;
    s->ftbase = 69 + 12 * log2f(map_freq(v[P_F_CUT]) / 440);
    s->fkey = v[P_F_KEY] / 100;
    s->fenv = v[P_F_ENV] * 0.96f;
    s->fres = v[P_F_RES] / 100;
    s->fdrive = v[P_F_DRIVE] / 100;
    s->fmix = v[P_F_MIX] / 100;
    for (int l = 0; l < 4; l++) s->lrise_t[l] = map_rise(v[P_L1_RISE + 6 * l]);
    s->nslot = 0;
    for (int m = 0; m < N_SLOTS; m++) {
        int b = P_M1_SRC + 4 * m, src = (int)v[b], aux = (int)v[b + 1], dst = (int)v[b + 2];
        float amt = v[b + 3] / 100;
        if (src == SRC_OFF || dst == DST_OFF || amt == 0) continue;
        s->slot_src[s->nslot] = src; s->slot_aux[s->nslot] = aux; s->slot_dst[s->nslot] = dst; s->slot_amt[s->nslot] = amt;
        s->nslot++;
    }
    float vg = v[P_VOL] / 100;
    s->vol_g = vg * vg * 1.4f;
    s->dirty = 0;
}

/* ---------------- tables ---------------- */
static int user_index(ph_t *s, int o) { return (int)s->v[o ? P_B_TABLE : P_A_TABLE] - WT_NB; }

static const wtab_t *osc_table(ph_t *s, int o) {
    int t = (int)s->v[o ? P_B_TABLE : P_A_TABLE];
    if (t < WT_NB) return wt_builtin(t);
    if (s->utab[o]) return s->utab[o];   /* the requested file, or the one before it while that loads */
    return wt_builtin(0);
}

static void request_table(ph_t *s, int o) {
    int u = user_index(s, o);
    if (u >= 0 && u != s->ufile[o]) lib_request(s->lib, o, u);
}

static void pickup_tables(ph_t *s) {   /* audio thread: never blocks */
    for (int o = 0; o < 2; o++) {
        int file;
        wtab_t *t = lib_take(s->lib, o, &file);
        if (!t) continue;
        if (s->utab[o] && s->nretire < 4) s->retire[s->nretire++] = s->utab[o];
        else if (s->utab[o]) { lib_retire(s->lib, t); continue; }   /* nowhere to park the old one: keep it */
        s->utab[o] = t;
        s->ufile[o] = file;
    }
    while (s->nretire && lib_retire(s->lib, s->retire[s->nretire - 1])) s->nretire--;
}

/* ---------------- voices ---------------- */
static float rand_bi(unsigned *st) {
    *st ^= *st << 13; *st ^= *st >> 17; *st ^= *st << 5;
    return (float)(int32_t)*st * (1.0f / 2147483648.0f);
}

static void voice_start(ph_t *s, voice_t *vc, int note, float vel) {
    int fresh = !vc->active;
    vc->note = note; vc->vel = vel; vc->gate = 1; vc->pedal = 0;
    vc->order = ++s->order;
    vc->rnd = rand_bi(&s->seed);
    vc->target = (float)note;
    if (fresh || s->glide_c >= 1) vc->pitch = (s->glide_c < 1 && s->last_pitch >= 0) ? s->last_pitch : (float)note;
    if (fresh) {
        for (int o = 0; o < 2; o++) {
            float ph0 = s->v[o ? P_B_PHASE : P_A_PHASE] / 360, rnd = s->v[o ? P_B_RAND : P_A_RAND] / 100;
            for (int u = 0; u < MAXU; u++) {
                float p = ph0 + rnd * (0.5f + 0.5f * rand_bi(&s->seed));
                vc->o[o].ph[u] = p - floorf(p);
            }
            vc->o[o].lev = 0;
        }
        memset(vc->sv, 0, sizeof vc->sv);
        memset(vc->lad, 0, sizeof vc->lad);
        memset(vc->comb, 0, sizeof vc->comb);
        memset(vc->ap, 0, sizeof vc->ap);
        memset(vc->env, 0, sizeof vc->env);
        vc->sph = 0; vc->slev = 0; vc->nlev = 0; vc->amp = 0; vc->pl = vc->pr = 1;
        vc->cdel = 0; vc->cw = 0; vc->lg = 0;
        memset(vc->nb, 0, sizeof vc->nb);
        vc->nseed = 0x2545F491u * (unsigned)(vc - s->vc + 1) ^ s->seed;
        if (!vc->nseed) vc->nseed = 1;
    }
    for (int l = 0; l < 4; l++) {
        int mode = (int)s->v[P_L1_MODE + 6 * l];
        if (mode > 0 || fresh) {
            float ph = s->v[P_L1_PHASE + 6 * l] / 360;
            vc->lph[l] = ph - floorf(ph);
            vc->lra[l] = rand_bi(&s->seed); vc->lrb[l] = rand_bi(&s->seed);
            vc->ldone[l] = 0;
        }
        vc->lrise[l] = s->lrise_t[l] > 0 ? 0 : 1;
    }
    for (int e = 0; e < 4; e++) vc->est[e] = E_ATT;
    vc->active = 1;
    s->last_pitch = (float)note;
    s->last_voice = (int)(vc - s->vc);
}

static void voice_release(ph_t *s, voice_t *vc) {
    vc->gate = 0;
    if (s->sustain) { vc->pedal = 1; return; }
    for (int e = 0; e < 4; e++) vc->est[e] = E_REL;
}

static void all_off(ph_t *s, int hard) {
    for (int i = 0; i < MAXV; i++) {
        voice_t *vc = &s->vc[i];
        vc->gate = 0; vc->pedal = 0;
        if (hard) vc->active = 0;
        else for (int e = 0; e < 4; e++) vc->est[e] = E_REL;
    }
    s->nstack = 0;
}

static void note_on(ph_t *s, int note, float vel) {
    int mode = (int)s->v[P_MODE];
    if (mode != 0) {   /* mono / legato: voice 0, last-note priority */
        int had = s->nstack > 0;
        for (int i = 0; i < s->nstack; i++)
            if (s->stack[i] == note) { memmove(s->stack + i, s->stack + i + 1, (s->nstack - i - 1) * sizeof(int)); s->nstack--; break; }
        if (s->nstack == 16) { memmove(s->stack, s->stack + 1, 15 * sizeof(int)); s->nstack--; }
        s->stack[s->nstack++] = note;
        voice_t *vc = &s->vc[0];
        if (mode == 2 && had && vc->active) {
            vc->note = note; vc->target = (float)note; vc->gate = 1;
            s->last_pitch = (float)note;
            return;
        }
        float from = vc->active ? vc->pitch : s->last_pitch;
        voice_start(s, vc, note, vel);
        vc->pitch = (s->glide_c < 1 && from >= 0 && (mode == 1 || had)) ? from : (float)note;
        return;
    }
    int nv = (int)s->v[P_VOICES], pick = -1;
    nv = nv < 1 ? 1 : nv > MAXV ? MAXV : nv;
    for (int i = 0; i < nv && pick < 0; i++)
        if (s->vc[i].active && s->vc[i].note == note) pick = i;
    for (int i = 0; i < nv && pick < 0; i++)
        if (!s->vc[i].active) pick = i;
    if (pick < 0) {
        float best = 2;
        for (int i = 0; i < nv; i++)
            if (!s->vc[i].gate && !s->vc[i].pedal && s->vc[i].env[0] < best) { best = s->vc[i].env[0]; pick = i; }
    }
    if (pick < 0) {
        unsigned old = ~0u;
        for (int i = 0; i < nv; i++)
            if (s->vc[i].order < old) { old = s->vc[i].order; pick = i; }
    }
    voice_start(s, &s->vc[pick], note, vel);
}

static void note_off(ph_t *s, int note) {
    if ((int)s->v[P_MODE] != 0) {
        int top = s->nstack ? s->stack[s->nstack - 1] : -1;
        for (int i = 0; i < s->nstack; i++)
            if (s->stack[i] == note) { memmove(s->stack + i, s->stack + i + 1, (s->nstack - i - 1) * sizeof(int)); s->nstack--; break; }
        voice_t *vc = &s->vc[0];
        if (!vc->active) return;
        if (s->nstack == 0) { if (vc->gate) voice_release(s, vc); }
        else if (note == top) { vc->note = s->stack[s->nstack - 1]; vc->target = (float)vc->note; }
        return;
    }
    for (int i = 0; i < MAXV; i++)
        if (s->vc[i].active && s->vc[i].gate && s->vc[i].note == note) voice_release(s, &s->vc[i]);
}

/* ---------------- LFOs ---------------- */
static float lfo_value(int shape, float ph, float ra, float rb) {
    switch (shape) {
    case LS_SINE: return fsin(ph);
    case LS_TRIANGLE: return ph < 0.25f ? 4 * ph : ph < 0.75f ? 2 - 4 * ph : 4 * ph - 4;
    case LS_SAW_UP: return 2 * ph - 1;
    case LS_SAW_DOWN: return 1 - 2 * ph;
    case LS_SQUARE: return ph < 0.5f ? 1 : -1;
    case LS_SNH: return ra;
    case LS_SMOOTH: return ra + (rb - ra) * (0.5f - 0.5f * fsin(ph * 0.5f + 0.25f));
    case LS_PUMP: { float q = 1 - ph; q *= q; return 1 - 2 * q * q; }      /* ducked at the start, back up quickly */
    case LS_EXP_UP: return 2 * ph * ph * ph - 1;
    default: return floorf(ph * 4) * (2.0f / 3) - 1;                     /* four stairs */
    }
}
static float lfo_hz(ph_t *s, int l) {
    int sync = (int)s->v[P_L1_SYNC + 6 * l];
    if (sync > 0) return s->bpm / 60.0f / LSYNC_BEATS[sync];
    return map_lrate(s->v[P_L1_RATE + 6 * l]);
}
static void lfo_step(float *ph, float *ra, float *rb, float d, unsigned *seed, int oneshot, int *done) {
    if (*done) return;
    *ph += d;
    if (*ph >= 1) {
        if (oneshot) { *ph = 0.9999f; *done = 1; return; }
        *ph -= floorf(*ph);
        *ra = *rb;
        *rb = rand_bi(seed);
    }
}

/* ---------------- modulation sources ---------------- */
static void sources(ph_t *s, voice_t *vc, float *src) {
    src[SRC_OFF] = 1;
    for (int m = 0; m < 4; m++) src[SRC_MACRO_1 + m] = s->v[P_MAC1 + m] / 100;
    src[SRC_MOD_WHL] = s->modwheel;
    src[SRC_BEND] = s->bend;
    src[SRC_EXPR] = s->expr;
    if (!vc) {
        for (int e = 0; e < 4; e++) src[SRC_ENV_1 + e] = 0;
        for (int l = 0; l < 4; l++) src[SRC_LFO_1 + l] = (int)s->v[P_L1_MODE + 6 * l] == 0 ? s->glval[l] : 0;
        src[SRC_VELOCITY] = src[SRC_NOTE] = src[SRC_RANDOM] = 0;
        src[SRC_AFTERTOUCH] = s->chan_at;
        return;
    }
    for (int e = 0; e < 4; e++) src[SRC_ENV_1 + e] = vc->env[e];
    for (int l = 0; l < 4; l++) {
        int mode = (int)s->v[P_L1_MODE + 6 * l];
        float val = mode == 0 ? s->glval[l] : lfo_value((int)s->v[P_L1_SHAPE + 6 * l], vc->lph[l], vc->lra[l], vc->lrb[l]);
        src[SRC_LFO_1 + l] = val * vc->lrise[l];
    }
    src[SRC_VELOCITY] = vc->vel;
    src[SRC_NOTE] = (vc->pitch - 60) / 48;
    src[SRC_AFTERTOUCH] = fmaxf(s->chan_at, s->polyat[vc->note & 127]);
    src[SRC_RANDOM] = vc->rnd;
}

static void matrix(ph_t *s, const float *src, float *dm, int from, int to) {
    for (int m = 0; m < s->nslot; m++) {
        int d = s->slot_dst[m];
        if (d < from || d >= to) continue;
        dm[d] += src[s->slot_src[m]] * (s->slot_aux[m] ? src[s->slot_aux[m]] : 1) * s->slot_amt[m];
    }
}

/* ---------------- filter ---------------- */
static void svf_target(svf_t *f, float g, float k, int n) {
    float a1 = 1 / (1 + g * (g + k)), a2 = g * a1, a3 = g * a2, in = 1.0f / n;
    f->da1 = (a1 - f->a1) * in; f->da2 = (a2 - f->a2) * in; f->da3 = (a3 - f->a3) * in; f->dk = (k - f->k) * in;
}
#define SVF_STEP(F, X, V1, V2) do { \
    float v3_ = (X) - (F)->ic2; \
    V1 = (F)->a1 * (F)->ic1 + (F)->a2 * v3_; \
    V2 = (F)->ic2 + (F)->a2 * (F)->ic1 + (F)->a3 * v3_; \
    (F)->ic1 = 2 * V1 - (F)->ic1; (F)->ic2 = 2 * V2 - (F)->ic2; \
    (F)->a1 += (F)->da1; (F)->a2 += (F)->da2; (F)->a3 += (F)->da3; (F)->k += (F)->dk; } while (0)

static const float VOWEL_F[5][3] = {{730, 1090, 2440}, {530, 1840, 2480}, {270, 2290, 3010}, {570, 840, 2410}, {300, 870, 2240}};

static void filter_tick(ph_t *s, voice_t *vc, float semis, float res, int n) {
    int type = (int)s->v[P_F_TYPE];
    float hz = clampf(note_hz(clampf(semis, -40, 140)), 16, 0.45f * SR);
    float g = tanf(PI_F * hz / SR);
    for (int c = 0; c < 2; c++)
        for (int k = 0; k < 3; k++)
            if (!(fabsf(vc->sv[c][k].ic1) < 1e4f && fabsf(vc->sv[c][k].ic2) < 1e4f)) vc->sv[c][k].ic1 = vc->sv[c][k].ic2 = 0;
    switch (type) {
    case FT_LP_12: case FT_HP_12: case FT_BP_12: case FT_NOTCH:
        for (int c = 0; c < 2; c++) svf_target(&vc->sv[c][0], g, 1.414f * (1 - 0.985f * res), n);
        break;
    case FT_LP_24: case FT_HP_24:
        for (int c = 0; c < 2; c++) {
            svf_target(&vc->sv[c][0], g, 1.848f, n);
            svf_target(&vc->sv[c][1], g, 0.765f * (1 - 0.97f * res), n);
        }
        break;
    case FT_MG_LP_12: case FT_MG_LP_24:
        vc->lgt = g / (1 + g);
        vc->lkt = type == FT_MG_LP_24 ? 3.9f * res : 1.8f * res;
        for (int c = 0; c < 2; c++) for (int k = 0; k < 4; k++)
            if (!(fabsf(vc->lad[c][k]) < 1e4f)) vc->lad[c][k] = 0;
        break;
    case FT_COMB_PLUS: case FT_COMB_MINUS:
        vc->cdelt = clampf(SR / hz, 2, COMB_N - 4);
        vc->cfb = (type == FT_COMB_PLUS ? 1 : -1) * 0.97f * res;
        break;
    case FT_PHASER: {
        float t = tanf(PI_F * clampf(hz, 20, 18000) / SR);
        vc->apat = (t - 1) / (t + 1);
        vc->apfb = 0.9f * res;
        break;
    }
    case FT_FORMANT: {   /* cutoff sweeps the vowels A E I O U; resonance narrows the formants */
        float x = clampf((semis - 30) / 90, 0, 1) * 4;
        int i = (int)x;
        if (i > 3) i = 3;
        float u = x - i, k = 0.6f * (1 - 0.85f * res);
        for (int f = 0; f < 3; f++) {
            float fh = (1 - u) * VOWEL_F[i][f] + u * VOWEL_F[i + 1][f];
            float gf = tanf(PI_F * clampf(fh, 50, 15000) / SR);
            for (int c = 0; c < 2; c++) svf_target(&vc->sv[c][f], gf, k, n);
        }
        break;
    }
    }
}

static void filter_run(ph_t *s, voice_t *vc, float *bl, float *br, int n, float mix, float drive) {
    int type = (int)s->v[P_F_TYPE];
    float dg = 1 + drive * 4, dc = 1 / (0.6f + 0.4f * dg), in = 1.0f / n;
    float lg = vc->lg, lk = vc->lk, dlg = (vc->lgt - vc->lg) * in, dlk = (vc->lkt - vc->lk) * in;
    float cd = vc->cdel <= 0 ? vc->cdelt : vc->cdel, dcd = (vc->cdelt - cd) * in;
    float apa = vc->apa, dapa = (vc->apat - vc->apa) * in;
    float *buf[2] = {bl, br};
    for (int i = 0; i < n; i++) {
        lg += dlg; lk += dlk; cd += dcd; apa += dapa;
        for (int c = 0; c < 2; c++) {
            float dry = buf[c][i], x = drive > 0 ? sat(dry * dg) * dc : dry, v1, v2, y;
            svf_t *a = &vc->sv[c][0], *b = &vc->sv[c][1], *f3 = &vc->sv[c][2];
            switch (type) {
            case FT_LP_12: SVF_STEP(a, x, v1, v2); y = v2; break;
            case FT_HP_12: { float k = a->k; SVF_STEP(a, x, v1, v2); y = x - k * v1 - v2; break; }
            case FT_BP_12: { float k = a->k; SVF_STEP(a, x, v1, v2); y = v1 * sqrtf(k); break; }
            case FT_NOTCH: { float k = a->k; SVF_STEP(a, x, v1, v2); y = x - k * v1; break; }
            case FT_LP_24: SVF_STEP(a, x, v1, v2); SVF_STEP(b, v2, v1, v2); y = v2; break;
            case FT_HP_24: { float k = a->k, h; SVF_STEP(a, x, v1, v2); h = x - k * v1 - v2;
                             k = b->k; SVF_STEP(b, h, v1, v2); y = h - k * v1 - v2; break; }
            case FT_MG_LP_12: case FT_MG_LP_24: {   /* zero-delay-feedback transistor ladder, saturated input */
                float *z = vc->lad[c], G = lg;
                int st = type == FT_MG_LP_24 ? 4 : 2;
                float sum = 0, gp = 1;
                for (int k = st - 1; k >= 0; k--) { sum += gp * (1 - G) * z[k]; gp *= G; }
                float u = sat((x - lk * sum) / (1 + lk * gp));
                for (int k = 0; k < st; k++) {
                    float vv = (u - z[k]) * G, yy = vv + z[k];
                    z[k] = yy + vv;
                    u = yy;
                }
                y = u * (1 + 0.4f * lk);
                break;
            }
            case FT_COMB_PLUS: case FT_COMB_MINUS: {
                float *cb = vc->comb[c], rp = (float)vc->cw - cd, fl = floorf(rp);
                int i0 = (int)fl & (COMB_N - 1);
                float d = cb[i0] + (cb[(i0 + 1) & (COMB_N - 1)] - cb[i0]) * (rp - fl);
                y = x + vc->cfb * d;
                cb[vc->cw] = y;
                y *= 1 - fabsf(vc->cfb) * 0.5f;
                break;
            }
            case FT_PHASER: {
                float *z = vc->ap[c], u = x + vc->apfb * z[4];
                for (int k = 0; k < 4; k++) { float o = apa * u + z[k]; z[k] = u - apa * o; u = o; }
                z[4] = u;
                y = 0.5f * (x + u);
                break;
            }
            default: {   /* formant: three band-passes */
                float x1, x2, o = 0;
                SVF_STEP(a, x, x1, x2); o += x1 * a->k;
                SVF_STEP(b, x, x1, x2); o += 0.6f * x1 * b->k;
                SVF_STEP(f3, x, x1, x2); o += 0.4f * x1 * f3->k;
                y = 2 * o;
                break;
            }
            }
            y = softclip(y);
            buf[c][i] = dry + (y - dry) * mix;
        }
        if (type == FT_COMB_PLUS || type == FT_COMB_MINUS) vc->cw = (vc->cw + 1) & (COMB_N - 1);
    }
    vc->lg = vc->lgt; vc->lk = vc->lkt; vc->cdel = vc->cdelt; vc->apa = vc->apat;
}

/* ---------------- one voice: control tick, then n samples ---------------- */
static void env_tick(ph_t *s, voice_t *vc, int e) {
    float *lv = &vc->env[e];
    switch (vc->est[e]) {
    case E_ATT:
        *lv += (1.3f - *lv) * s->ecoef[e][0];
        if (*lv >= 1) { *lv = 1; vc->est[e] = E_DEC; }
        break;
    case E_DEC: *lv += (s->esus[e] - *lv) * s->ecoef[e][1]; break;
    case E_REL:
        *lv -= *lv * s->ecoef[e][2];
        if (*lv < 1e-4f) { *lv = 0; vc->est[e] = E_IDLE; }
        break;
    default: *lv = 0;
    }
}

static float warp_factor(int warp, float a) {   /* how much a warp raises the harmonics (for the band-limit level) */
    switch (warp) {
    case WARP_SYNC: return 1 + 7 * a;
    case WARP_PWM: return 1 / (1 - 0.95f * a);
    case WARP_BEND_PLUS: case WARP_BEND_MINUS: case WARP_ASYM_PLUS: case WARP_ASYM_MINUS: return 1 + 3 * a;
    case WARP_FM: return 1 + 4 * a;
    default: return 1;
    }
}

static void osc_tick(ph_t *s, voice_t *vc, int o, const float *dm, float base, int n) {
    const float *v = s->v;
    osc_t *os = &vc->o[o];
    int b = o ? P_B_ON : P_A_ON, d = o ? DST_B_PITCH : DST_A_PITCH;   /* d + 0..6: pitch pos warp level pan detune blend */
    float lt = v[b] > 0.5f ? clampf(v[b + (P_A_LEVEL - P_A_ON)] / 100 + dm[d + 3], 0, 1) : 0;
    int other_needs = 0;
    int ow = (int)v[(o ? P_A_WARP : P_B_WARP)];
    if ((ow == WARP_FM || ow == WARP_AM || ow == WARP_RM) && v[o ? P_A_ON : P_B_ON] > 0.5f && v[b] > 0.5f) other_needs = 1;
    os->on = lt > 0 || os->lev > 0 || other_needs;
    os->need_mono = other_needs;
    os->levt = lt;
    os->dlev = (lt - os->lev) / n;
    if (!os->on) return;
    float semis = base + v[b + (P_A_OCT - P_A_ON)] * 12 + v[b + (P_A_SEMI - P_A_ON)] + v[b + (P_A_FINE - P_A_ON)] / 100 + dm[d] * 24;
    float inc = clampf(note_hz(semis) / SR, 0, 0.49f);
    int nu = (int)v[b + (P_A_UNI - P_A_ON)];
    nu = nu < 1 ? 1 : nu > MAXU ? MAXU : nu;
    os->nu = nu;
    float spread = map_detune(clampf(v[b + (P_A_DET - P_A_ON)] + dm[d + 5] * 100, 0, 100));
    float blend = clampf(v[b + (P_A_BLEND - P_A_ON)] / 100 + dm[d + 6], 0, 1);
    float width = v[b + (P_A_WIDTH - P_A_ON)] / 100, pan = clampf(v[b + (P_A_PAN - P_A_ON)] / 100 + dm[d + 4], -1, 1);
    float wsum = 0, maxinc = 0;
    for (int u = 0; u < nu; u++) {
        float off = nu > 1 ? -1 + 2.0f * u / (nu - 1) : 0;
        os->inc[u] = clampf(inc * exp2_small(off * spread / 12), 0, 0.49f);
        if (os->inc[u] > maxinc) maxinc = os->inc[u];
        int center = nu <= 2 || fabsf(off) < 1.0f / (nu - 1) + 1e-4f;
        float w = nu == 1 ? 1 : center ? 1 - 0.5f * blend : blend;
        os->wm[u] = w;
        wsum += w * w;
        float pu = clampf(off * width + pan, -1, 1);
        os->gl[u] = w * sqrtf(1 - pu);
        os->gr[u] = w * sqrtf(1 + pu);
    }
    float norm = wsum > 0 ? 1 / sqrtf(wsum) : 0;
    for (int u = 0; u < nu; u++) { os->gl[u] *= norm; os->gr[u] *= norm; os->wm[u] *= norm; }
    os->warp = (int)v[b + (P_A_WARP - P_A_ON)];
    os->wamt = clampf(v[b + (P_A_WAMT - P_A_ON)] / 100 + dm[d + 2], 0, 1);
    const wtab_t *t = osc_table(s, o);
    os->tab = t;
    os->lvl = wt_level(t, maxinc * warp_factor(os->warp, os->wamt));
    float fr = clampf(v[b + (P_A_POS - P_A_ON)] / 100 + dm[d + 1], 0, 1) * (t->nf - 1);
    int f0 = (int)fr;
    if (f0 > t->nf - 2) f0 = t->nf > 1 ? t->nf - 2 : 0;
    os->f0 = f0;
    os->ff = t->nf > 1 ? fr - f0 : 0;
}

/* one oscillator's unison voices into tl/tr (and mono into os->mono), its warp applied to the phase or the sample */
static void osc_render(osc_t *os, const float *mod, int n, float *tl, float *tr) {
    const wtab_t *t = os->tab;
    int len = t->len[os->lvl], mask = len - 1, f1 = t->nf > 1 ? os->f0 + 1 : os->f0;
    const float *fa = wt_ptr(t, os->f0, os->lvl), *fb = wt_ptr(t, f1, os->lvl);
    float lenf = (float)len, ff = os->ff, a = os->wamt;
    int warp = os->warp;
    float sync = 1 + 7 * a, pwm = 1 - 0.95f * a, ipwm = 1 / pwm, bend = warp == WARP_BEND_PLUS ? 6 * a : -0.85f * a;
    float asym = 6 * a, flip = 1 - a, steps = exp2f(10 - 8 * a), isteps = 1 / steps;
    memset(os->mono, 0, sizeof(float) * n);
    if (warp == WARP_OFF && !os->need_mono) {   /* the common case, without the per-sample warp switch */
        for (int u = 0; u < os->nu; u++) {
            float ph = os->ph[u], inc = os->inc[u], gl = os->gl[u], gr = os->gr[u];
            for (int i = 0; i < n; i++) {
                float idx = ph * lenf;
                int i0 = (int)idx;
                float fr = idx - i0;
                i0 &= mask;
                int i1 = (i0 + 1) & mask;
                float x0 = fa[i0] + (fa[i1] - fa[i0]) * fr, x1 = fb[i0] + (fb[i1] - fb[i0]) * fr;
                float y = x0 + (x1 - x0) * ff;
                tl[i] += y * gl;
                tr[i] += y * gr;
                ph += inc;
                if (ph >= 1) ph -= 1;
            }
            os->ph[u] = ph;
        }
        return;
    }
    for (int u = 0; u < os->nu; u++) {
        float ph = os->ph[u], inc = os->inc[u], gl = os->gl[u], gr = os->gr[u], wm = os->wm[u];
        for (int i = 0; i < n; i++) {
            float p = ph, post = 1;
            switch (warp) {
            case WARP_SYNC: p = ph * sync; p -= (int)p; break;
            case WARP_BEND_PLUS: case WARP_BEND_MINUS: {
                float q = 2 * ph - 1;
                q = q * (1 + bend) / (1 + bend * fabsf(q));
                p = 0.5f + 0.5f * q;
                break;
            }
            case WARP_PWM: p = ph < pwm ? ph * ipwm : 0; break;
            case WARP_ASYM_PLUS: p = ph / (1 + asym * (1 - ph)); break;
            case WARP_ASYM_MINUS: p = 1 - (1 - ph) / (1 + asym * ph); break;
            case WARP_FLIP: if (ph >= flip) post = -1; break;
            case WARP_MIRROR: { float m = ph < 0.5f ? 2 * ph : 2 - 2 * ph; p = ph + a * (m - ph); break; }
            case WARP_QUANTIZE: p = floorf(ph * steps) * isteps; break;
            case WARP_FM: p = ph + a * 2 * mod[i]; p -= floorf(p); break;
            case WARP_AM: post = 1 - a + a * (0.5f + 0.5f * mod[i]); break;
            case WARP_RM: post = 1 - a + a * mod[i]; break;
            }
            float idx = p * lenf;
            int i0 = (int)idx;
            float fr = idx - i0;
            i0 &= mask;
            int i1 = (i0 + 1) & mask;
            float x0 = fa[i0] + (fa[i1] - fa[i0]) * fr, x1 = fb[i0] + (fb[i1] - fb[i0]) * fr;
            float y = (x0 + (x1 - x0) * ff) * post;
            tl[i] += y * gl;
            tr[i] += y * gr;
            os->mono[i] += y * wm;
            ph += inc;
            if (ph >= 1) ph -= 1;
        }
        os->ph[u] = ph;
    }
}

static inline float blep(float t, float dt) {
    if (t < dt) { t /= dt; return t + t - t * t - 1; }
    if (t > 1 - dt) { t = (t - 1) / dt; return t * t + t + t + 1; }
    return 0;
}

static void voice_tick(ph_t *s, voice_t *vc, int n) {
    const float *v = s->v;
    float dt = n / SR;
    for (int e = 0; e < 4; e++) env_tick(s, vc, e);
    for (int l = 0; l < 4; l++) {
        int mode = (int)v[P_L1_MODE + 6 * l];
        if (mode > 0) lfo_step(&vc->lph[l], &vc->lra[l], &vc->lrb[l], lfo_hz(s, l) * exp2f(vc->lrate_mod[l] * 4) * dt,
                               &vc->nseed, mode == 2, &vc->ldone[l]);
        if (vc->lrise[l] < 1) vc->lrise[l] = s->lrise_t[l] > 0 ? fminf(1, vc->lrise[l] + dt / s->lrise_t[l]) : 1;
    }
    float src[N_SRC], dm[N_DST] = {0};
    sources(s, vc, src);
    matrix(s, src, dm, 0, DST_FX1_P1);
    for (int l = 0; l < 4; l++) vc->lrate_mod[l] = dm[DST_LFO1_RATE + l];

    vc->pitch += (vc->target - vc->pitch) * s->glide_c;
    float base = vc->pitch + s->bend * v[P_BEND] + dm[DST_PITCH] * 24;
    osc_tick(s, vc, 0, dm, base, n);
    osc_tick(s, vc, 1, dm, base, n);

    /* sub */
    float slt = v[P_S_ON] > 0.5f ? clampf(v[P_S_LEVEL] / 100 + dm[DST_SUB_LEVEL], 0, 1) : 0;
    vc->slevt = slt;
    if (slt > 0 || vc->slev > 0) {
        vc->sinc = clampf(note_hz(base + v[P_S_OCT] * 12) / SR, 0, 0.49f);
        float pu = clampf(v[P_S_PAN] / 100 + dm[DST_SUB_PAN], -1, 1);
        vc->sgl = sqrtf(1 - pu); vc->sgr = sqrtf(1 + pu);
    }
    /* noise */
    float nlt = v[P_N_ON] > 0.5f ? clampf(v[P_N_LEVEL] / 100 + dm[DST_NOISE_LEVEL], 0, 1) : 0;
    vc->nlevt = nlt;
    if (nlt > 0 || vc->nlev > 0) {
        float np = clampf(v[P_N_PITCH] / 100 + dm[DST_NOISE_PITCH], 0, 1);
        float r = 100.0f * powf(200.0f, np);
        if (v[P_N_KEY] > 0.5f) r *= exp2f((vc->pitch - 60) / 12);
        vc->nrate = clampf(r, 1, SR);
        float pu = clampf(v[P_N_PAN] / 100, -1, 1);
        vc->ngl = sqrtf(1 - pu); vc->ngr = sqrtf(1 + pu);
    }
    /* filter */
    if (v[P_F_ON] > 0.5f) {
        float semis = s->ftbase + s->fkey * (vc->pitch - 60) + s->fenv * vc->env[1] + dm[DST_CUTOFF] * 96;
        filter_tick(s, vc, semis, clampf(s->fres + dm[DST_RESONANCE], 0, 1), n);
        vc->fdrive = clampf(s->fdrive + dm[DST_DRIVE], 0, 1);
        vc->fmix = clampf(s->fmix + dm[DST_FILTER_MIX], 0, 1);
    }
    /* amp and pan */
    float vs = v[P_VEL] / 100;
    vc->ampt = vc->env[0] * (1 - vs + vs * vc->vel) * clampf(1 + dm[DST_AMP], 0, 2) * 0.5f;
    float pan = clampf(dm[DST_PAN], -1, 1);
    vc->plt = sqrtf(1 - pan); vc->prt = sqrtf(1 + pan);
}

static void noise_render(ph_t *s, voice_t *vc, int n, float *nl) {
    int type = (int)s->v[P_N_TYPE];
    float lev = vc->nlev, dl = (vc->nlevt - vc->nlev) / n, *b = vc->nb;
    for (int i = 0; i < n; i++) {
        float w = rand_bi(&vc->nseed), y;
        switch (type) {
        case NZ_PINK:
            b[0] = 0.99765f * b[0] + w * 0.0990460f; b[1] = 0.96300f * b[1] + w * 0.2965164f; b[2] = 0.57000f * b[2] + w * 1.0526913f;
            y = (b[0] + b[1] + b[2] + w * 0.1848f) * 0.25f;
            break;
        case NZ_BROWN: b[3] = (b[3] + 0.02f * w) / 1.02f; y = b[3] * 3.5f; break;
        case NZ_DIGITAL:
            vc->ncount -= vc->nrate / SR;
            if (vc->ncount <= 0) { vc->ncount += 1; vc->nsh[0] = w; }
            y = vc->nsh[0];
            break;
        case NZ_CRACKLE:
            vc->nsh[0] *= 0.96f;
            if ((w * 0.5f + 0.5f) < vc->nrate / (SR * 4)) vc->nsh[0] = rand_bi(&vc->nseed);
            y = vc->nsh[0];
            break;
        case NZ_HISS: {
            float c = 1 - expf(-2 * PI_F * clampf(vc->nrate * 0.5f, 200, 15000) / SR);
            b[0] += (w - b[0]) * c;
            y = w - b[0];
            break;
        }
        default: y = w;
        }
        nl[i] = y * lev;
        lev += dl;
    }
    vc->nlev = vc->nlevt;
}

static void voice_render(ph_t *s, voice_t *vc, int n, float *L, float *R) {
    const float *v = s->v;
    float fl[CR], fr[CR], dl[CR], dr[CR], tl[CR], tr[CR];
    memset(fl, 0, sizeof fl); memset(fr, 0, sizeof fr); memset(dl, 0, sizeof dl); memset(dr, 0, sizeof dr);
    static const float zeros[CR];
    /* the oscillator another one modulates (FM / AM / RM) runs first */
    int aw = vc->o[0].warp, first = (aw == WARP_FM || aw == WARP_AM || aw == WARP_RM) ? 1 : 0;
    for (int k = 0; k < 2; k++) {
        int o = k ? 1 - first : first;
        osc_t *os = &vc->o[o];
        if (!os->on) { memset(os->mono, 0, sizeof os->mono); continue; }
        memset(tl, 0, sizeof tl); memset(tr, 0, sizeof tr);
        const float *mod = k ? vc->o[1 - o].mono : zeros;
        osc_render(os, mod, n, tl, tr);
        int to_f = v[P_F_ON] > 0.5f && v[o ? P_F_RB : P_F_RA] > 0.5f;
        float *ol = to_f ? fl : dl, *or_ = to_f ? fr : dr, lev = os->lev;
        for (int i = 0; i < n; i++) { ol[i] += tl[i] * lev; or_[i] += tr[i] * lev; lev += os->dlev; }
        os->lev = os->levt;
    }
    if (vc->slev > 0 || vc->slevt > 0) {   /* sub */
        int shape = (int)v[P_S_SHAPE];
        float ph = vc->sph, inc = vc->sinc, lev = vc->slev, dlv = (vc->slevt - vc->slev) / n;
        float pw = shape == SUB_PULSE_25 ? 0.25f : shape == SUB_PULSE_12 ? 0.125f : 0.5f;
        int to_f = v[P_F_ON] > 0.5f && v[P_F_RS] > 0.5f && v[P_S_DIRECT] < 0.5f;
        float *ol = to_f ? fl : dl, *or_ = to_f ? fr : dr;
        for (int i = 0; i < n; i++) {
            float y;
            switch (shape) {
            case SUB_SINE: y = fsin(ph); break;
            case SUB_TRIANGLE: y = ph < 0.5f ? 4 * ph - 1 : 3 - 4 * ph; break;
            case SUB_SAW: y = 2 * ph - 1 - blep(ph, inc); break;
            default: {
                float p2 = ph + 1 - pw;
                if (p2 >= 1) p2 -= 1;
                y = (ph < pw ? 1.0f : -1.0f) + blep(ph, inc) - blep(p2, inc) - (2 * pw - 1);
            }
            }
            y *= lev;
            ol[i] += y * vc->sgl; or_[i] += y * vc->sgr;
            lev += dlv;
            ph += inc;
            if (ph >= 1) ph -= 1;
        }
        vc->sph = ph;
        vc->slev = vc->slevt;
    }
    if (vc->nlev > 0 || vc->nlevt > 0) {   /* noise */
        float nb[CR];
        noise_render(s, vc, n, nb);
        int to_f = v[P_F_ON] > 0.5f && v[P_F_RN] > 0.5f;
        float *ol = to_f ? fl : dl, *or_ = to_f ? fr : dr;
        for (int i = 0; i < n; i++) { ol[i] += nb[i] * vc->ngl; or_[i] += nb[i] * vc->ngr; }
    }
    if (v[P_F_ON] > 0.5f) filter_run(s, vc, fl, fr, n, vc->fmix, vc->fdrive);
    float amp = vc->amp, damp = (vc->ampt - vc->amp) / n, pl = vc->pl, pr = vc->pr;
    float dpl = (vc->plt - pl) / n, dpr = (vc->prt - pr) / n;
    for (int i = 0; i < n; i++) {
        L[i] += (fl[i] + dl[i]) * amp * pl;
        R[i] += (fr[i] + dr[i]) * amp * pr;
        amp += damp; pl += dpl; pr += dpr;
    }
    vc->amp = vc->ampt; vc->pl = vc->plt; vc->pr = vc->prt;
    if (vc->est[0] == E_IDLE && !vc->gate) vc->active = 0;
}

/* ---------------- engine interface ---------------- */
static void set_value(ph_t *s, int p, float x);

static void defaults(ph_t *s) {
    for (int p = 0; p < NP; p++) s->v[p] = PH_P[p].def;
    for (int k = 0; k < FX_SLOTS; k++) if (s->fx[k].type != FX_OFF) fx_prepare(&s->fx[k], FX_OFF);
    s->dirty = 1;
}

static void *create(const char *dir) {
    pthread_once(&ht_once, ht_init);
    if (wt_builtin_acquire()) return NULL;
    ph_t *s = calloc(1, sizeof *s);
    if (!s) { wt_builtin_release(); return NULL; }
    for (int k = 0; k < FX_SLOTS; k++) fx_init(&s->fx[k]);
    s->lib = lib_open(dir);
    s->ufile[0] = s->ufile[1] = -1;
    defaults(s);
    s->bpm = 120;
    s->expr = 1;
    s->last_pitch = -1;
    s->seed = 0x1234567u ^ (unsigned)(uintptr_t)s;
    if (!s->seed) s->seed = 1;
    for (int l = 0; l < 4; l++) { s->glra[l] = rand_bi(&s->seed); s->glrb[l] = rand_bi(&s->seed); }
    s->gain_cur = -1;
    return s;
}

static void destroy(void *p) {
    ph_t *s = p;
    if (!s) return;
    lib_close(s->lib);
    for (int o = 0; o < 2; o++) wt_free(s->utab[o]);
    for (int i = 0; i < s->nretire; i++) wt_free(s->retire[i]);
    for (int k = 0; k < FX_SLOTS; k++) fx_free(&s->fx[k]);
    free(s);
    wt_builtin_release();
}

static void midi(void *p, const uint8_t *m, int len) {
    ph_t *s = p;
    if (len < 1) return;
    int st = m[0] & 0xF0, d1 = len > 1 ? m[1] & 127 : 0, d2 = len > 2 ? m[2] & 127 : 0;
    if (s->dirty) derive(s);
    switch (st) {
    case 0x90:
        if (d2) note_on(s, d1, d2 / 127.0f);
        else note_off(s, d1);
        break;
    case 0x80: note_off(s, d1); break;
    case 0xA0: s->polyat[d1] = d2 / 127.0f; break;
    case 0xD0: s->chan_at = d1 / 127.0f; break;
    case 0xE0: s->bend = ((d2 << 7 | d1) - 8192) / 8192.0f; break;
    case 0xB0:
        switch (d1) {
        case 1: s->modwheel = d2 / 127.0f; break;
        case 11: s->expr = d2 / 127.0f; break;
        case 64:
            s->sustain = d2 >= 64;
            if (!s->sustain)
                for (int i = 0; i < MAXV; i++)
                    if (s->vc[i].pedal) { s->vc[i].pedal = 0; for (int e = 0; e < 4; e++) s->vc[i].est[e] = E_REL; }
            break;
        case 120: all_off(s, 1); break;
        case 121: s->modwheel = 0; s->expr = 1; s->bend = 0; s->chan_at = 0; memset(s->polyat, 0, sizeof s->polyat); break;
        case 123: all_off(s, 0); break;
        }
        break;
    }
}

static void set_value(ph_t *s, int p, float x) {
    const ph_pdef_t *d = &PH_P[p];
    if (x != x) return;
    x = clampf(x, d->min, d->max);
    if (d->integer) x = roundf(x);
    if ((p == P_A_TABLE || p == P_B_TABLE) && x >= WT_NB) {   /* only as far as the library goes */
        int last = WT_NB + lib_count(s->lib) - 1;
        if (x > last) x = last < WT_NB ? WT_NB - 1 : last;
    }
    if (p == P_MODE && x != s->v[p]) all_off(s, 0);
    s->v[p] = x;
    if (p == P_A_TABLE || p == P_B_TABLE) request_table(s, p == P_B_TABLE);
    if (p >= P_FX1_TYPE && p <= P_FX8_MIX && (p - P_FX1_TYPE) % 5 == 0) {
        int k = (p - P_FX1_TYPE) / 5;
        if (s->fx[k].type != (int)x) fx_prepare(&s->fx[k], (int)x);   /* allocation here, off the audio thread */
    }
    s->dirty = 1;
}

static void state_set(ph_t *s, const char *str) {
    if (strncmp(str, "ph1", 3)) return;
    defaults(s);
    s->pend_name[0][0] = s->pend_name[1][0] = 0;
    const char *q = str + 3;
    while (*q) {
        while (*q == ';') q++;
        const char *eq = strchr(q, '=');
        if (!eq) break;
        const char *end = strchr(eq, ';');
        size_t vl = end ? (size_t)(end - eq - 1) : strlen(eq + 1);
        if (!strncmp(q, "a_tfile=", 8) || !strncmp(q, "b_tfile=", 8)) {
            int o = q[0] == 'b';
            snprintf(s->pend_name[o], sizeof s->pend_name[o], "%.*s", (int)vl, eq + 1);
        } else {
            int p = find_param(q, (size_t)(eq - q));
            char *e2;
            float x = strtof(eq + 1, &e2);
            if (p >= 0 && e2 != eq + 1) set_value(s, p, x);
        }
        if (!end) break;
        q = end;
    }
    for (int o = 0; o < 2; o++)   /* a user table comes back by its file name; positions shift when files are added */
        if (s->pend_name[o][0]) {
            int f = lib_find(s->lib, s->pend_name[o]);   /* not found (moved, deleted): the first built-in table */
            set_value(s, o ? P_B_TABLE : P_A_TABLE, f >= 0 ? (float)(WT_NB + f) : 0);
        }
}

static void set_param(void *p, const char *key, const char *val) {
    ph_t *s = p;
    if (!strcmp(key, "state")) { state_set(s, val); return; }
    if (!strcmp(key, "lfo_bpm")) { float b = strtof(val, NULL); if (b >= 20 && b <= 999) s->bpm = b; return; }
    int id = find_param(key, strlen(key));
    if (id >= 0) set_value(s, id, strtof(val, NULL));
}

static int fmt_time(char *b, int n, float sec) {
    if (sec < 0.01f) return snprintf(b, n, "%.1f ms", sec * 1000);
    if (sec < 1) return snprintf(b, n, "%.0f ms", sec * 1000);
    return snprintf(b, n, "%.2f s", sec);
}
static int fmt_hz(char *b, int n, float hz) {
    if (hz < 1) return snprintf(b, n, "%.2f Hz", hz);
    if (hz < 100) return snprintf(b, n, "%.1f Hz", hz);
    if (hz < 1000) return snprintf(b, n, "%.0f Hz", hz);
    return snprintf(b, n, "%.2f kHz", hz / 1000);
}

static int display(ph_t *s, int p, char *b, int n) {
    float x = s->v[p];
    switch (PH_P[p].fmt) {
    case F_PCT: return snprintf(b, n, "%.0f%%", x);
    case F_BIP: return snprintf(b, n, "%+.0f", x);
    case F_INT: return snprintf(b, n, "%.0f", x);
    case F_SIGNED: return snprintf(b, n, "%+.0f", x);
    case F_SEMI: return snprintf(b, n, "%+.0f st", x);
    case F_CENT: return snprintf(b, n, "%+.0f ct", x);
    case F_BEND: return snprintf(b, n, "%.0f st", x);
    case F_DEG: return snprintf(b, n, "%.0f deg", x);
    case F_FREQ: return fmt_hz(b, n, map_freq(x));
    case F_ETIME: return fmt_time(b, n, map_etime(x));
    case F_DETUNE: return snprintf(b, n, "%.0f ct", map_detune(x) * 100);
    case F_LRATE: {
        int l = (p - P_L1_RATE) / 6;
        if (s->v[P_L1_SYNC + 6 * l] > 0) return snprintf(b, n, "Sync");
        return fmt_hz(b, n, map_lrate(x));
    }
    case F_RISE: return x <= 0 ? snprintf(b, n, "Off") : fmt_time(b, n, map_rise(x));
    case F_GLIDE: return x <= 0 ? snprintf(b, n, "Off") : fmt_time(b, n, map_glide(x));
    case F_PAN: return x == 0 ? snprintf(b, n, "C") : snprintf(b, n, "%c%.0f", x < 0 ? 'L' : 'R', fabsf(x));
    case F_TABLE: {
        int t = (int)x;
        if (t < WT_NB) return snprintf(b, n, "%s", TABLE_NAMES[t]);
        const char *nm = lib_name(s->lib, t - WT_NB);
        return *nm ? snprintf(b, n, "%s", nm) : snprintf(b, n, "User %d", t - WT_NB + 1);
    }
    case F_FXP: {
        int k = (p - P_FX1_P1) / 5, w = (p - P_FX1_P1) % 5;
        return fx_display((int)s->v[P_FX1_TYPE + 5 * k], w, x / 100, s->bpm, b, n);
    }
    }
    return snprintf(b, n, "%g", x);
}

static int get_param(void *p, const char *key, char *buf, int n) {
    ph_t *s = p;
    if (n <= 0) return 0;
    if (!strcmp(key, "state")) {
        int o = snprintf(buf, n, "ph1");
        for (int i = 0; i < NP && o < n - 1; i++)
            o += snprintf(buf + o, n - o, ";%s=%.6g", PH_P[i].key, s->v[i]);
        for (int k = 0; k < 2 && o < n - 1; k++) {   /* user tables by file name too */
            int u = user_index(s, k);
            const char *rel = lib_path(s->lib, u);
            if (u >= 0 && *rel && !strpbrk(rel, ";=")) o += snprintf(buf + o, n - o, ";%c_tfile=%s", k ? 'b' : 'a', rel);
        }
        return o < n ? o : n - 1;
    }
    size_t len = strlen(key);
    if (len > 8 && !strcmp(key + len - 8, "_display")) {
        int id = find_param(key, len - 8);
        if (id < 0 || PH_P[id].fmt == F_NONE) return 0;
        return display(s, id, buf, n);
    }
    if (len > 5 && !strcmp(key + len - 5, "_name")) {   /* the FX knobs are named after their slot's effect */
        int id = find_param(key, len - 5);
        if (id < P_FX1_P1 || id > P_FX8_MIX || (id - P_FX1_TYPE) % 5 == 0) return 0;
        int k = (id - P_FX1_TYPE) / 5, w = (id - P_FX1_TYPE) % 5 - 1;
        return snprintf(buf, n, "%s", fx_label((int)s->v[P_FX1_TYPE + 5 * k], w));
    }
    int id = find_param(key, len);
    if (id < 0) return 0;
    if (PH_P[id].integer) return snprintf(buf, n, "%d", (int)s->v[id]);
    return snprintf(buf, n, "%.6g", s->v[id]);
}

static void render(void *p, int16_t *out, int frames) {
    ph_t *s = p;
    if (frames > 128) frames = 128;
    fpmode_t fm = fp_enter();
    if (s->dirty) derive(s);
    pickup_tables(s);
    memset(s->mixL, 0, sizeof(float) * frames);
    memset(s->mixR, 0, sizeof(float) * frames);
    for (int pos = 0; pos < frames; pos += CR) {
        int n = frames - pos < CR ? frames - pos : CR;
        float dt = n / SR;
        for (int l = 0; l < 4; l++) {   /* the free-running LFOs, shared by every voice */
            int done = 0;
            lfo_step(&s->glph[l], &s->glra[l], &s->glrb[l], lfo_hz(s, l) * dt, &s->seed, 0, &done);
            float ph = s->glph[l] + s->v[P_L1_PHASE + 6 * l] / 360;
            s->glval[l] = lfo_value((int)s->v[P_L1_SHAPE + 6 * l], ph - floorf(ph), s->glra[l], s->glrb[l]);
        }
        for (int i = 0; i < MAXV; i++) {
            voice_t *vc = &s->vc[i];
            if (!vc->active) continue;
            voice_tick(s, vc, n);
            voice_render(s, vc, n, s->mixL + pos, s->mixR + pos);
        }
    }
    /* effects: their modulation follows the last voice played, and the global sources */
    float src[N_SRC], dm[N_DST] = {0};
    voice_t *lv = s->vc[s->last_voice].active ? &s->vc[s->last_voice] : NULL;
    sources(s, lv, src);
    matrix(s, src, dm, DST_FX1_P1, N_DST);
    for (int k = 0; k < FX_SLOTS; k++) {
        fxslot_t *fx = &s->fx[k];
        if ((int)s->v[P_FX1_TYPE + 5 * k] == FX_OFF || fx->type == FX_OFF) continue;
        int b = P_FX1_P1 + 5 * k;
        float pp[4] = {s->v[b] / 100 + dm[DST_FX1_P1 + k], s->v[b + 1] / 100, s->v[b + 2] / 100,
                       s->v[b + 3] / 100 + dm[DST_FX1_MIX + k]};
        fx_process(fx, pp, s->bpm, s->mixL, s->mixR, frames);
    }
    float g0 = s->gain_cur < 0 ? s->vol_g : s->gain_cur, dg = (s->vol_g - g0) / frames;
    for (int i = 0; i < frames; i++) {
        float xl = s->mixL[i] * g0, xr = s->mixR[i] * g0;
        s->dc_y[0] = xl - s->dc_x[0] + 0.99929f * s->dc_y[0]; s->dc_x[0] = xl;
        s->dc_y[1] = xr - s->dc_x[1] + 0.99929f * s->dc_y[1]; s->dc_x[1] = xr;
        float l = softclip(s->dc_y[0]), r = softclip(s->dc_y[1]);
        if (!(l == l)) l = 0;
        if (!(r == r)) r = 0;
        out[2 * i] = (int16_t)lrintf(l * 32767);
        out[2 * i + 1] = (int16_t)lrintf(r * 32767);
        g0 += dg;
    }
    if (!(s->dc_y[0] == s->dc_y[0]) || !(s->dc_y[1] == s->dc_y[1])) memset(s->dc_y, 0, sizeof s->dc_y);
    s->gain_cur = s->vol_g;
    fp_leave(fm);
}

static const mpc_engine_t ENGINE = {create, destroy, midi, set_param, get_param, render, NULL, NULL};
const mpc_engine_t *mpc_engine(void) { return &ENGINE; }
