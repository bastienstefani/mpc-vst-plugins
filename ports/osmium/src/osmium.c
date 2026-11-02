/* Osmium: an 8-voice synth engine for MPC OS standalone devices (wrapper/engine.h): per voice three oscillators
 * (built-in wavetables or band-limited classic waveforms), noise, two state-variable filters (serial or parallel),
 * three ADSR envelopes, three LFOs and a 16-slot modulation matrix; then chorus and delay.
 * 44.1 kHz, 128-frame blocks; modulation runs every CR samples and is interpolated per sample where it matters
 * (levels, filter coefficients). Parameters are described once in vst/make_params.py (osm_params.h). */
#include <math.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "engine.h"
#include "osm_params.h"
#include "osm_wt.h"

#define SR 44100.0f
#define CR 16                 /* control period, samples */
#define MAXV 8
#define NOSC 3
#define DL_LEN 131072         /* delay line per channel (2.97 s), a power of two */
#define CH_LEN 2048           /* chorus line per channel (46 ms) */
#define PI_F 3.14159265358979f

/* ---------------- floating point: flush denormals while we render ---------------- */
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
static inline float sat(float x) {   /* rational tanh: ~linear for small x, 1 at 3 */
    if (x > 3) return 1;
    if (x < -3) return -1;
    float x2 = x * x;
    return x * (27 + x2) / (27 + 9 * x2);
}
static inline float softclip(float x) {   /* transparent up to 0.75 of full scale */
    float a = fabsf(x);
    if (a <= 0.75f) return x;
    float y = 0.75f + 0.25f * sat((a - 0.75f) * 4);
    return x < 0 ? -y : y;
}
static inline float note_hz(float n) { return 440.0f * exp2f((n - 69.0f) * (1.0f / 12)); }

/* ---------------- value mappings (knob 0..100 -> units), shared by the DSP and the display text ---------------- */
static float map_freq(float x) { return 20.0f * powf(1000.0f, x / 100); }               /* 20 Hz .. 20 kHz */
static float map_etime(float x) { return 0.0005f * powf(20000.0f, x / 100); }           /* 0.5 ms .. 10 s */
static float map_lrate(float x) { return 0.02f * powf(2000.0f, x / 100); }              /* 0.02 .. 40 Hz */
static float map_fade(float x) { return 5.0f * (x / 100) * (x / 100); }                  /* 0 .. 5 s */
static float map_glide(float x) { return x <= 0 ? 0 : 0.002f * powf(1000.0f, x / 100); } /* off, 2 ms .. 2 s */
static float map_crate(float x) { return 0.05f * powf(100.0f, x / 100); }                /* 0.05 .. 5 Hz */
static float map_dtime(float x) { return 0.01f * powf(200.0f, x / 100); }                /* 10 ms .. 2 s */

enum { E_IDLE, E_ATT, E_DEC, E_SUS, E_REL };
enum { FT_OFF, FT_LP24, FT_LP12, FT_BP, FT_HP12, FT_HP24, FT_NOTCH };

typedef struct { float ic1, ic2, a1, a2, a3, k, da1, da2, da3, dk; } svf_t;

typedef struct {
    int active, note, gate, held_by_pedal;
    unsigned order;
    float vel, rnd, pitch, target, vpan;
    /* oscillators (set per control tick) */
    float phase[NOSC], inc[NOSC], ff[NOSC], shp[NOSC], lev[NOSC], dlev[NOSC], levt[NOSC];
    const float *fa[NOSC], *fb[NOSC];
    int len[NOSC], on[NOSC], wave[NOSC];
    float nlev, dnlev, nlevt, nlp;
    unsigned nseed;
    /* envelopes, LFOs */
    float env[3];
    int est[3];
    float lph[3], lra[3], lrb[3], lfade[3], lrate_mod[3];
    /* filters: two filters of two stages each */
    svf_t f[2][2];
    float bal;
    /* output gains, interpolated per sample */
    float amp, damp, gl, dgl, gr, dgr, ampt, glt, grt;
} voice_t;

typedef struct {
    float v[NP];
    int dirty;
    /* derived from v[] (derive()) */
    float ecoef[3][3], esus[3], glide_c, vol_g;
    float fbase[2], fres[2], fdrive[2], fenv[2], fkey[2];
    float opitch[NOSC];
    float ncoef;
    int nhp;
    float lfade_t[3];
    int nslot, slot_src[N_SLOTS], slot_via[N_SLOTS], slot_dst[N_SLOTS];
    float slot_amt[N_SLOTS];
    /* voices and play state */
    voice_t vc[MAXV];
    unsigned order;
    int stack[16], nstack;
    int sustain;
    float last_pitch;
    float modwheel, expr, chan_at, bend, polyat[128];
    float bpm;
    unsigned seed;
    /* free-running (global) LFOs */
    float glph[3], glra[3], glrb[3], glval[3];
    /* effects */
    float *dl;
    int dl_w, dl_filled, dl_was_on;
    float dl_cur, dl_lp[2];
    float ch[2][CH_LEN];
    int ch_w, ch_was_on;
    float ch_s, ch_c;
    float gain_cur, dc_x[2], dc_y[2];   /* master gain ramp, DC blocker state */
    float mixL[128], mixR[128];
} osm_t;

/* ---------------- parameter lookup ---------------- */
#define HT 512
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
        unsigned h = hash(OSM_P[p].key, 64) & (HT - 1);
        while (htab[h] >= 0) h = (h + 1) & (HT - 1);
        htab[h] = (short)p;
    }
}
static int find_param(const char *k, size_t n) {   /* the first n characters of k */
    unsigned h = hash(k, n) & (HT - 1);
    for (; htab[h] >= 0; h = (h + 1) & (HT - 1)) {
        const char *pk = OSM_P[htab[h]].key;
        if (strlen(pk) == n && !strncmp(pk, k, n)) return htab[h];
    }
    return -1;
}

static void set_value(osm_t *s, int p, float x);

static void defaults(osm_t *s) {
    for (int p = 0; p < NP; p++) s->v[p] = OSM_P[p].def;
    s->dirty = 1;
}

/* ---------------- derived values ---------------- */
static float tick_coef(float tau) { return tau <= 0 ? 1 : 1 - expf(-(float)CR / (tau * SR)); }

static void derive(osm_t *s) {
    const float *v = s->v;
    for (int e = 0; e < 3; e++) {
        int b = P_E1_A + 4 * e;
        s->ecoef[e][0] = tick_coef(map_etime(v[b]) / 1.466f);   /* attack aims at 1.3 and stops at 1 */
        s->ecoef[e][1] = tick_coef(map_etime(v[b + 1]) / 4);
        s->esus[e] = v[b + 2] / 100;
        s->ecoef[e][2] = tick_coef(map_etime(v[b + 3]) / 4);
    }
    float gt = map_glide(v[P_GLIDE]);
    s->glide_c = gt > 0 ? tick_coef(gt / 3) : 1;
    for (int o = 0; o < NOSC; o++) {
        int b = P_O1_WAVE + (P_O2_WAVE - P_O1_WAVE) * o;
        s->opitch[o] = v[b + (P_O1_OCT - P_O1_WAVE)] * 12 + v[b + (P_O1_SEMI - P_O1_WAVE)] + v[b + (P_O1_FINE - P_O1_WAVE)] / 100;
    }
    for (int f = 0; f < 2; f++) {
        int b = P_F1_TYPE + (P_F2_TYPE - P_F1_TYPE) * f;
        s->fbase[f] = 69 + 12 * log2f(map_freq(v[b + 1]) / 440);
        s->fres[f] = v[b + 2] / 100;
        s->fdrive[f] = v[b + 3] / 100;
        s->fenv[f] = v[b + 4] * 0.96f;         /* +-100 -> +-96 semitones */
        s->fkey[f] = v[b + 5] / 100;
    }
    float c = v[P_NOISE_COLOR] / 100;          /* < 0 darker (low-pass), > 0 brighter (high-pass) */
    s->nhp = c > 0;
    float fc = c <= 0 ? 20000.0f * powf(0.01f, -c) : 20.0f * powf(200.0f, c);
    s->ncoef = c == 0 ? 1 : 1 - expf(-2 * PI_F * fc / SR);
    for (int l = 0; l < 3; l++) s->lfade_t[l] = map_fade(v[P_L1_FADE + 5 * l]);
    s->nslot = 0;
    for (int m = 0; m < N_SLOTS; m++) {
        int b = P_M1_SRC + 4 * m;
        int src = (int)v[b], via = (int)v[b + 1], dst = (int)v[b + 2];
        float amt = v[b + 3] / 100;
        if (src == S_OFF || dst == D_OFF || amt == 0) continue;
        s->slot_src[s->nslot] = src; s->slot_via[s->nslot] = via; s->slot_dst[s->nslot] = dst; s->slot_amt[s->nslot] = amt;
        s->nslot++;
    }
    float vg = v[P_VOL] / 100;
    s->vol_g = vg * vg * 1.4f;
    s->dirty = 0;
}

/* ---------------- voices ---------------- */
static const float VPAN[MAXV] = {-1, 1, -0.6f, 0.6f, -0.3f, 0.3f, -0.8f, 0.8f};

static float rand_bi(unsigned *st) {
    *st ^= *st << 13; *st ^= *st >> 17; *st ^= *st << 5;
    return (float)(int32_t)*st * (1.0f / 2147483648.0f);
}

static void voice_start(osm_t *s, voice_t *vc, int note, float vel) {
    int fresh = !vc->active;
    vc->note = note; vc->vel = vel; vc->gate = 1; vc->held_by_pedal = 0;
    vc->order = ++s->order;
    vc->rnd = rand_bi(&s->seed);
    vc->target = (float)note;
    if (fresh || s->glide_c >= 1) vc->pitch = (s->glide_c < 1 && s->last_pitch >= 0) ? s->last_pitch : (float)note;
    if (fresh) {
        memset(vc->phase, 0, sizeof vc->phase);
        memset(vc->f, 0, sizeof vc->f);
        memset(vc->env, 0, sizeof vc->env);
        memset(vc->lev, 0, sizeof vc->lev);
        vc->nlev = 0; vc->nlp = 0; vc->amp = 0; vc->gl = vc->gr = 0;
        vc->nseed = 0x2545F491u * (unsigned)(vc - s->vc + 1) ^ s->seed;
        if (!vc->nseed) vc->nseed = 1;
    }
    for (int l = 0; l < 3; l++) {
        if (s->v[P_L1_RETRIG + 5 * l] > 0.5f || fresh) {
            vc->lph[l] = s->v[P_L1_RETRIG + 5 * l] > 0.5f ? 0 : s->glph[l];
            vc->lra[l] = rand_bi(&s->seed); vc->lrb[l] = rand_bi(&s->seed);
        }
        vc->lfade[l] = s->lfade_t[l] > 0 ? 0 : 1;
    }
    for (int e = 0; e < 3; e++) vc->est[e] = E_ATT;
    vc->active = 1;
    s->last_pitch = (float)note;
}

static void voice_release(osm_t *s, voice_t *vc) {
    vc->gate = 0;
    if (s->sustain) { vc->held_by_pedal = 1; return; }
    for (int e = 0; e < 3; e++) vc->est[e] = E_REL;
}

static void all_off(osm_t *s, int hard) {
    for (int i = 0; i < MAXV; i++) {
        voice_t *vc = &s->vc[i];
        vc->gate = 0; vc->held_by_pedal = 0;
        if (hard) vc->active = 0;
        else for (int e = 0; e < 3; e++) vc->est[e] = E_REL;
    }
    s->nstack = 0;
}

static void note_on(osm_t *s, int note, float vel) {
    int mode = (int)s->v[P_MODE];
    if (mode != 0) {   /* mono / legato: voice 0, last-note priority */
        int had = s->nstack > 0;
        for (int i = 0; i < s->nstack; i++)
            if (s->stack[i] == note) { memmove(s->stack + i, s->stack + i + 1, (s->nstack - i - 1) * sizeof(int)); s->nstack--; break; }
        if (s->nstack == 16) { memmove(s->stack, s->stack + 1, 15 * sizeof(int)); s->nstack--; }
        s->stack[s->nstack++] = note;
        voice_t *vc = &s->vc[0];
        if (mode == 2 && had && vc->active) {   /* legato: no retrigger, glide to the new note */
            vc->note = note; vc->target = (float)note; vc->gate = 1;
            s->last_pitch = (float)note;
            return;
        }
        float from = vc->active ? vc->pitch : s->last_pitch;
        voice_start(s, vc, note, vel);
        vc->vpan = 0;   /* one voice: centred, whatever spread it had in poly */
        if (s->glide_c < 1 && from >= 0 && (mode == 1 || had)) vc->pitch = from;
        else vc->pitch = (float)note;
        return;
    }
    int nv = (int)s->v[P_VOICES], pick = -1;
    if (nv < 1) nv = 1;
    if (nv > MAXV) nv = MAXV;
    for (int i = 0; i < nv && pick < 0; i++)
        if (s->vc[i].active && s->vc[i].note == note) pick = i;          /* the same key again: reuse its voice */
    for (int i = 0; i < nv && pick < 0; i++)
        if (!s->vc[i].active) pick = i;
    if (pick < 0) {                                                      /* steal: the quietest released voice ... */
        float best = 2;
        for (int i = 0; i < nv; i++)
            if (!s->vc[i].gate && !s->vc[i].held_by_pedal && s->vc[i].env[0] < best) { best = s->vc[i].env[0]; pick = i; }
    }
    if (pick < 0) {                                                      /* ... else the oldest */
        unsigned old = ~0u;
        for (int i = 0; i < nv; i++)
            if (s->vc[i].order < old) { old = s->vc[i].order; pick = i; }
    }
    voice_start(s, &s->vc[pick], note, vel);
    s->vc[pick].vpan = VPAN[pick];
}

static void note_off(osm_t *s, int note) {
    int mode = (int)s->v[P_MODE];
    if (mode != 0) {
        int top = s->nstack ? s->stack[s->nstack - 1] : -1;
        for (int i = 0; i < s->nstack; i++)
            if (s->stack[i] == note) { memmove(s->stack + i, s->stack + i + 1, (s->nstack - i - 1) * sizeof(int)); s->nstack--; break; }
        voice_t *vc = &s->vc[0];
        if (!vc->active) return;
        if (s->nstack == 0) { if (vc->gate) voice_release(s, vc); }
        else if (note == top) { vc->note = s->stack[s->nstack - 1]; vc->target = (float)vc->note; }   /* back to the held note */
        return;
    }
    for (int i = 0; i < MAXV; i++)
        if (s->vc[i].active && s->vc[i].gate && s->vc[i].note == note) voice_release(s, &s->vc[i]);
}

/* ---------------- LFOs ---------------- */
static float lfo_value(int shape, float ph, float ra, float rb) {
    switch (shape) {
    case 0: return sinf(2 * PI_F * ph);
    case 1: return ph < 0.25f ? 4 * ph : ph < 0.75f ? 2 - 4 * ph : 4 * ph - 4;
    case 2: return 2 * ph - 1;
    case 3: return 1 - 2 * ph;
    case 4: return ph < 0.5f ? 1 : -1;
    case 5: return ra;
    default: return ra + (rb - ra) * (0.5f - 0.5f * cosf(PI_F * ph));
    }
}
static float lfo_hz(osm_t *s, int l) {
    int sync = (int)s->v[P_L1_SYNC + 5 * l];
    if (sync > 0) return s->bpm / 60.0f / LSYNC_BEATS[sync];
    return map_lrate(s->v[P_L1_RATE + 5 * l]);
}
static void lfo_advance(float *ph, float *ra, float *rb, float d, unsigned *seed) {
    *ph += d;
    if (*ph >= 1) {
        *ph -= floorf(*ph);
        *ra = *rb;
        *rb = rand_bi(seed);
    }
}

/* ---------------- filters ---------------- */
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

static void filter_run(svf_t *st, int type, float drive, float *buf, int n) {
    float dg = 1 + drive * 4, dc = 1 / (0.6f + 0.4f * dg);
    svf_t *a = &st[0], *b = &st[1];
    for (int i = 0; i < n; i++) {
        float x = drive > 0 ? sat(buf[i] * dg) * dc : buf[i], v1, v2, y;
        switch (type) {
        case FT_LP24: SVF_STEP(a, x, v1, v2); SVF_STEP(b, v2, v1, v2); y = v2; break;
        case FT_LP12: SVF_STEP(b, x, v1, v2); y = v2; break;
        case FT_BP: { float k = b->k; SVF_STEP(b, x, v1, v2); y = v1 * sqrtf(k); break; }   /* peak gain 1/sqrt(k): narrow keeps some level */
        case FT_HP12: { float k = b->k; SVF_STEP(b, x, v1, v2); y = x - k * v1 - v2; break; }
        case FT_HP24: { float k = a->k, h, k2; SVF_STEP(a, x, v1, v2); h = x - k * v1 - v2;
                        k2 = b->k; SVF_STEP(b, h, v1, v2); y = h - k2 * v1 - v2; break; }
        case FT_NOTCH: { float k = b->k; SVF_STEP(b, x, v1, v2); y = x - k * v1; break; }
        default: y = x;
        }
        buf[i] = softclip(y);   /* only a resonant peak gets limited; drive above is where the colour is */
    }
}

/* ---------------- one voice: control tick, then n samples ---------------- */
static void env_tick(osm_t *s, voice_t *vc, int e) {
    float *lv = &vc->env[e];
    switch (vc->est[e]) {
    case E_ATT:
        *lv += (1.3f - *lv) * s->ecoef[e][0];
        if (*lv >= 1) { *lv = 1; vc->est[e] = E_DEC; }
        break;
    case E_DEC:
    case E_SUS:
        *lv += (s->esus[e] - *lv) * s->ecoef[e][1];
        break;
    case E_REL:
        *lv -= *lv * s->ecoef[e][2];
        if (*lv < 1e-4f) { *lv = 0; vc->est[e] = E_IDLE; }
        break;
    default: *lv = 0;
    }
}

static void voice_tick(osm_t *s, voice_t *vc, int n) {
    const float *v = s->v;
    float dt = n / SR;
    for (int e = 0; e < 3; e++) env_tick(s, vc, e);
    float src[N_SRC];
    for (int l = 0; l < 3; l++) {
        float val;
        if (v[P_L1_RETRIG + 5 * l] > 0.5f) {
            float hz = lfo_hz(s, l) * exp2f(vc->lrate_mod[l] * 4);
            lfo_advance(&vc->lph[l], &vc->lra[l], &vc->lrb[l], hz * dt, &vc->nseed);
            val = lfo_value((int)v[P_L1_SHAPE + 5 * l], vc->lph[l], vc->lra[l], vc->lrb[l]);
        } else val = s->glval[l];
        if (vc->lfade[l] < 1) vc->lfade[l] = s->lfade_t[l] > 0 ? fminf(1, vc->lfade[l] + dt / s->lfade_t[l]) : 1;
        src[S_LFO_1 + l] = val * vc->lfade[l];
    }
    src[S_OFF] = 1;
    src[S_AMP_ENV] = vc->env[0]; src[S_FILT_ENV] = vc->env[1]; src[S_ENV_3] = vc->env[2];
    src[S_VELOCITY] = vc->vel;
    src[S_KEY] = (vc->pitch - 60) / 48;
    src[S_MOD_WHL] = s->modwheel;
    src[S_PRESSURE] = fmaxf(s->chan_at, s->polyat[vc->note & 127]);
    src[S_BEND] = s->bend;
    src[S_RANDOM] = vc->rnd;
    src[S_EXPR] = s->expr;
    float dm[N_DST] = {0};
    for (int m = 0; m < s->nslot; m++)
        dm[s->slot_dst[m]] += src[s->slot_src[m]] * (s->slot_via[m] ? src[s->slot_via[m]] : 1) * s->slot_amt[m];
    for (int l = 0; l < 3; l++) vc->lrate_mod[l] = dm[D_LFO1_RATE + l];

    vc->pitch += (vc->target - vc->pitch) * s->glide_c;
    float base = vc->pitch + s->bend * v[P_BEND] + dm[D_PITCH] * 24;
    float in = 1.0f / n;
    for (int o = 0; o < NOSC; o++) {
        int b = P_O1_WAVE + (P_O2_WAVE - P_O1_WAVE) * o;
        float lt = clampf(v[b + (P_O1_LEVEL - P_O1_WAVE)] / 100 + dm[D_O1_LEVEL + o], 0, 1);
        vc->on[o] = lt > 0 || vc->lev[o] > 0;
        vc->levt[o] = lt;
        vc->dlev[o] = (lt - vc->lev[o]) * in;
        if (!vc->on[o]) continue;
        float inc = note_hz(base + s->opitch[o] + dm[D_O1_PITCH + o] * 24) / SR;
        vc->inc[o] = clampf(inc, 0, 0.49f);
        float shape = clampf(v[b + (P_O1_SHAPE - P_O1_WAVE)] / 100 + dm[D_O1_SHAPE + o], 0, 1);
        int tb = (int)v[b];
        vc->wave[o] = tb;
        if (tb < WT_NT) {   /* wavetable: Shape is the position */
            int lvl = wt_level_for(vc->inc[o]);
            float fr = shape * (WT_NF - 1);
            int f0 = (int)fr;
            if (f0 > WT_NF - 2) f0 = WT_NF - 2;
            vc->fa[o] = wt_frame(tb, f0, lvl);
            vc->fb[o] = wt_frame(tb, f0 + 1, lvl);
            vc->ff[o] = fr - f0;
            vc->len[o] = WT_LEN[lvl];
        } else {
            vc->shp[o] = shape;
        }
    }
    float nt = clampf(v[P_NOISE] / 100 + dm[D_NOISE], 0, 1);
    vc->nlevt = nt;
    vc->dnlev = (nt - vc->nlev) * in;

    for (int f = 0; f < 2; f++) {
        int type = (int)v[P_F1_TYPE + (P_F2_TYPE - P_F1_TYPE) * f];
        if (type == FT_OFF) continue;
        float st = s->fbase[f] + s->fkey[f] * (vc->pitch - 60) + s->fenv[f] * vc->env[1] + (dm[D_CUTOFF] + dm[D_F1_CUTOFF + f]) * 96;
        float hz = clampf(note_hz(clampf(st, -40, 140)), 16, 0.45f * SR);
        float g = tanf(PI_F * hz / SR);
        float r = clampf(s->fres[f] + dm[D_F1_RESO + f], 0, 1);
        svf_t *a = &vc->f[f][0], *bb = &vc->f[f][1];
        for (int k = 0; k < 2; k++)   /* a state that ran away (it should not) restarts instead of staying loud */
            if (!(fabsf(vc->f[f][k].ic1) < 1e4f && fabsf(vc->f[f][k].ic2) < 1e4f)) vc->f[f][k].ic1 = vc->f[f][k].ic2 = 0;
        if (type == FT_LP24 || type == FT_HP24) {   /* Butterworth pair; the resonance goes on the second stage */
            svf_target(a, g, 1.848f, n);
            svf_target(bb, g, 0.765f * (1 - 0.97f * r), n);
        } else {
            svf_target(bb, g, 1.414f * (1 - 0.985f * r), n);
        }
    }
    vc->bal = clampf(v[P_F_BAL] / 100 + dm[D_F_BALANCE], 0, 1);

    float vs = v[P_VEL] / 100;
    float amp = vc->env[0] * (1 - vs + vs * vc->vel) * clampf(1 + dm[D_AMP], 0, 2) * 0.6f;
    float pan = clampf(vc->vpan * v[P_SPREAD] / 100 + dm[D_PAN], -1, 1);
    float gl = 1.41421f * cosf((pan + 1) * (PI_F / 4)), gr = 1.41421f * sinf((pan + 1) * (PI_F / 4));
    vc->ampt = amp; vc->glt = gl; vc->grt = gr;
    vc->damp = (amp - vc->amp) * in;
    vc->dgl = (gl - vc->gl) * in;
    vc->dgr = (gr - vc->gr) * in;
}

static inline float blep(float t, float dt) {
    if (t < dt) { t /= dt; return t + t - t * t - 1; }
    if (t > 1 - dt) { t = (t - 1) / dt; return t * t + t + t + 1; }
    return 0;
}

static void voice_render(osm_t *s, voice_t *vc, int n, float *L, float *R) {
    const float *v = s->v;
    float buf[CR], b2[CR];
    memset(buf, 0, sizeof buf);
    for (int o = 0; o < NOSC; o++) {
        if (!vc->on[o]) continue;
        float ph = vc->phase[o], inc = vc->inc[o], lev = vc->lev[o], dl = vc->dlev[o];
        if (vc->wave[o] < WT_NT) {
            const float *fa = vc->fa[o], *fb = vc->fb[o];
            float ff = vc->ff[o], len = (float)vc->len[o];
            int mask = vc->len[o] - 1;
            for (int i = 0; i < n; i++) {
                float idx = ph * len;
                int i0 = (int)idx, i1 = (i0 + 1) & mask;
                float fr = idx - i0;
                float x0 = fa[i0] + (fa[i1] - fa[i0]) * fr, x1 = fb[i0] + (fb[i1] - fb[i0]) * fr;
                buf[i] += (x0 + (x1 - x0) * ff) * lev;
                lev += dl;
                ph += inc;
                if (ph >= 1) ph -= 1;
            }
        } else {
            int wave = vc->wave[o] - WT_NT;
            float sh = vc->shp[o];
            float pw = 0.5f - 0.45f * sh;                          /* square: pulse width */
            float r = 0.5f + 0.49f * sh, ir = 2 / r, ir2 = 2 / (1 - r);   /* triangle: rise time, triangle -> saw */
            float sg = 1 / (1 + 0.5f * sh), fm = 2.5f * sh;         /* saw: second saw gain; sine: self-FM index */
            for (int i = 0; i < n; i++) {
                float y;
                switch (wave) {
                case 0: {   /* saw plus a second saw half a cycle on: an octave up at the top */
                    y = 2 * ph - 1 - blep(ph, inc);
                    if (sh > 0) {
                        float p2 = ph + 0.5f;
                        if (p2 >= 1) p2 -= 1;
                        y = (y + sh * (2 * p2 - 1 - blep(p2, inc))) * sg;
                    }
                    break;
                }
                case 1: {
                    float p2 = ph + 1 - pw;
                    if (p2 >= 1) p2 -= 1;
                    y = (ph < pw ? 1.0f : -1.0f) + blep(ph, inc) - blep(p2, inc);
                    y -= 2 * pw - 1;   /* no DC from a narrow pulse */
                    break;
                }
                case 2: y = ph < r ? ph * ir - 1 : 1 - (ph - r) * ir2; break;
                default: y = fm > 0 ? sinf(2 * PI_F * ph + fm * sinf(2 * PI_F * ph)) : sinf(2 * PI_F * ph); break;
                }
                buf[i] += y * lev;
                lev += dl;
                ph += inc;
                if (ph >= 1) ph -= 1;
            }
        }
        vc->phase[o] = ph;
        vc->lev[o] = vc->levt[o];   /* exactly the target: no drift, and a silent oscillator really stops */
        (void)lev;
    }
    if (vc->nlev > 0 || vc->nlevt > 0) {
        float lev = vc->nlev, c = s->ncoef, lp = vc->nlp;
        for (int i = 0; i < n; i++) {
            float w = rand_bi(&vc->nseed);
            lp += (w - lp) * c;
            buf[i] += (s->nhp ? w - lp : lp) * lev;
            lev += vc->dnlev;
        }
        vc->nlp = lp;
        vc->nlev = vc->nlevt;
        (void)lev;
    }
    for (int i = 0; i < n; i++) buf[i] *= 0.5f;   /* headroom ahead of the filters */

    int t1 = (int)v[P_F1_TYPE], t2 = (int)v[P_F2_TYPE];
    if (v[P_F_ROUTE] < 0.5f) {
        if (t1) filter_run(vc->f[0], t1, s->fdrive[0], buf, n);
        if (t2) filter_run(vc->f[1], t2, s->fdrive[1], buf, n);
    } else {
        memcpy(b2, buf, sizeof(float) * n);
        if (t1) filter_run(vc->f[0], t1, s->fdrive[0], buf, n);
        if (t2) filter_run(vc->f[1], t2, s->fdrive[1], b2, n);
        float bl = vc->bal;
        for (int i = 0; i < n; i++) buf[i] += (b2[i] - buf[i]) * bl;
    }
    float amp = vc->amp, gl = vc->gl, gr = vc->gr;
    for (int i = 0; i < n; i++) {
        float x = buf[i] * amp;
        L[i] += x * gl;
        R[i] += x * gr;
        amp += vc->damp; gl += vc->dgl; gr += vc->dgr;
    }
    vc->amp = vc->ampt; vc->gl = vc->glt; vc->gr = vc->grt;
    (void)amp; (void)gl; (void)gr;
    if (vc->est[0] == E_IDLE && !vc->gate) vc->active = 0;
}

/* ---------------- effects ---------------- */
static void chorus(osm_t *s, int n) {
    float mix = s->v[P_CH_MIX] / 100;
    if (mix <= 0) { s->ch_was_on = 0; return; }
    if (!s->ch_was_on) { memset(s->ch, 0, sizeof s->ch); s->ch_was_on = 1; s->ch_s = 0; s->ch_c = 1; }
    float w = 2 * PI_F * map_crate(s->v[P_CH_RATE]) / SR, cw = cosf(w), sw = sinf(w);
    float depth = s->v[P_CH_DEPTH] / 100 * 0.004f * SR, base = 0.008f * SR;
    float dry = 1 - mix * 0.5f, wet = mix * 0.5f;
    for (int i = 0; i < n; i++) {
        float sn = s->ch_s * cw + s->ch_c * sw, cs = s->ch_c * cw - s->ch_s * sw;
        s->ch_s = sn; s->ch_c = cs;
        s->ch[0][s->ch_w] = s->mixL[i];
        s->ch[1][s->ch_w] = s->mixR[i];
        float mod[2] = {sn, cs};
        float *io[2] = {&s->mixL[i], &s->mixR[i]};
        for (int c = 0; c < 2; c++) {
            float d = base + depth * mod[c], rp = (float)s->ch_w - d;
            if (rp < 0) rp += CH_LEN;
            int i0 = (int)rp;
            float fr = rp - i0;
            i0 &= CH_LEN - 1;   /* rp + CH_LEN can round up to CH_LEN itself */
            int i1 = (i0 + 1) & (CH_LEN - 1);
            float y = s->ch[c][i0] + (s->ch[c][i1] - s->ch[c][i0]) * fr;
            *io[c] = *io[c] * dry + y * wet;
        }
        s->ch_w = (s->ch_w + 1) & (CH_LEN - 1);
    }
    float r = 1 / sqrtf(s->ch_s * s->ch_s + s->ch_c * s->ch_c);   /* keep the rotator on the unit circle */
    s->ch_s *= r; s->ch_c *= r;
}

static void delay(osm_t *s, int n) {
    float mix = s->v[P_DL_MIX] / 100;
    if (mix <= 0 || !s->dl) { s->dl_was_on = 0; return; }
    if (!s->dl_was_on) { s->dl_filled = 0; s->dl_was_on = 1; s->dl_lp[0] = s->dl_lp[1] = 0; }
    int sync = (int)s->v[P_DL_SYNC];
    float secs = sync > 0 ? DSYNC_BEATS[sync] * 60.0f / s->bpm : map_dtime(s->v[P_DL_TIME]);
    float tgt = clampf(secs * SR, 16, DL_LEN - 4);
    if (s->dl_cur <= 0) s->dl_cur = tgt;
    float fb = s->v[P_DL_FB] / 100 * 0.95f;
    float tc = 1 - expf(-2 * PI_F * 500.0f * powf(40.0f, s->v[P_DL_TONE] / 100) / SR);
    int ping = s->v[P_DL_MODE] > 0.5f;
    float *bl = s->dl, *br = s->dl + DL_LEN;
    for (int i = 0; i < n; i++) {
        s->dl_cur += (tgt - s->dl_cur) * 0.0007f;
        float rp = (float)s->dl_w - s->dl_cur, yl = 0, yr = 0;
        if (s->dl_cur < s->dl_filled) {
            if (rp < 0) rp += DL_LEN;
            int i0 = (int)rp;
            float fr = rp - i0;
            i0 &= DL_LEN - 1;   /* rp + DL_LEN can round up to DL_LEN itself */
            int i1 = (i0 + 1) & (DL_LEN - 1);
            yl = bl[i0] + (bl[i1] - bl[i0]) * fr;
            yr = br[i0] + (br[i1] - br[i0]) * fr;
        }
        s->dl_lp[0] += (yl - s->dl_lp[0]) * tc;
        s->dl_lp[1] += (yr - s->dl_lp[1]) * tc;
        float inl = s->mixL[i], inr = s->mixR[i];
        if (ping) {
            bl[s->dl_w] = 0.5f * (inl + inr) + fb * s->dl_lp[1];
            br[s->dl_w] = fb * s->dl_lp[0];
        } else {
            bl[s->dl_w] = inl + fb * s->dl_lp[0];
            br[s->dl_w] = inr + fb * s->dl_lp[1];
        }
        s->dl_w = (s->dl_w + 1) & (DL_LEN - 1);
        if (s->dl_filled < DL_LEN) s->dl_filled++;
        s->mixL[i] = inl + yl * mix;
        s->mixR[i] = inr + yr * mix;
    }
}

/* ---------------- engine interface ---------------- */
static void *create(const char *dir) {
    (void)dir;
    pthread_once(&ht_once, ht_init);
    if (wt_acquire()) return NULL;
    osm_t *s = calloc(1, sizeof *s);
    if (!s) { wt_release(); return NULL; }
    s->dl = calloc(2 * DL_LEN, sizeof(float));
    defaults(s);
    s->bpm = 120;
    s->expr = 1;
    s->last_pitch = -1;
    s->seed = 0x1234567u ^ (unsigned)(uintptr_t)s;
    if (!s->seed) s->seed = 1;
    for (int l = 0; l < 3; l++) { s->glra[l] = rand_bi(&s->seed); s->glrb[l] = rand_bi(&s->seed); }
    s->ch_c = 1;
    s->gain_cur = -1;
    return s;
}

static void destroy(void *p) {
    osm_t *s = p;
    if (!s) return;
    free(s->dl);
    free(s);
    wt_release();
}

static void midi(void *p, const uint8_t *m, int len) {
    osm_t *s = p;
    if (len < 1) return;
    int st = m[0] & 0xF0, d1 = len > 1 ? m[1] & 127 : 0, d2 = len > 2 ? m[2] & 127 : 0;
    if (s->dirty) derive(s);
    switch (st) {
    case 0x90:
        if (d2) note_on(s, d1, d2 / 127.0f);
        else note_off(s, d1);   /* a note-on of velocity 0 is a note-off */
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
                    if (s->vc[i].held_by_pedal) { s->vc[i].held_by_pedal = 0; for (int e = 0; e < 3; e++) s->vc[i].est[e] = E_REL; }
            break;
        case 120: all_off(s, 1); break;
        case 121: s->modwheel = 0; s->expr = 1; s->bend = 0; s->chan_at = 0; memset(s->polyat, 0, sizeof s->polyat); break;
        case 123: all_off(s, 0); break;
        }
        break;
    }
}

static void set_value(osm_t *s, int p, float x) {
    const osm_pdef_t *d = &OSM_P[p];
    if (x != x) return;   /* NaN */
    x = clampf(x, d->min, d->max);
    if (d->integer) x = roundf(x);
    if (p == P_MODE && x != s->v[p]) all_off(s, 0);
    if (p == P_DL_MIX && x > 0 && s->v[p] <= 0) s->dl_cur = 0;   /* restart the delay time where it is set */
    s->v[p] = x;
    s->dirty = 1;
}

static void state_set(osm_t *s, const char *str) {
    if (strncmp(str, "osm1", 4)) return;
    defaults(s);
    const char *q = str + 4;
    while (*q) {
        while (*q == ';' || *q == ' ') q++;
        const char *eq = strchr(q, '=');
        if (!eq) break;
        int p = find_param(q, (size_t)(eq - q));
        char *end;
        float x = strtof(eq + 1, &end);
        if (p >= 0 && end != eq + 1) set_value(s, p, x);
        q = strchr(eq, ';');
        if (!q) break;
    }
}

static void set_param(void *p, const char *key, const char *val) {
    osm_t *s = p;
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

static int display(osm_t *s, int p, char *b, int n) {
    float x = s->v[p];
    switch (OSM_P[p].fmt) {
    case F_PCT: return snprintf(b, n, "%.0f%%", x);
    case F_BIP: return snprintf(b, n, "%+.0f", x);
    case F_INT: return snprintf(b, n, "%.0f", x);
    case F_SIGNED: return snprintf(b, n, "%+.0f", x);
    case F_SEMI: return snprintf(b, n, "%+.0f st", x);
    case F_CENT: return snprintf(b, n, "%+.0f ct", x);
    case F_BEND: return snprintf(b, n, "%.0f st", x);
    case F_FREQ: return fmt_hz(b, n, map_freq(x));
    case F_ETIME: return fmt_time(b, n, map_etime(x));
    case F_LRATE: {
        int l = (p - P_L1_RATE) / 5, sync = (int)s->v[P_L1_SYNC + 5 * l];
        if (sync > 0) return snprintf(b, n, "Sync");
        return fmt_hz(b, n, map_lrate(x));
    }
    case F_FADE: return x <= 0 ? snprintf(b, n, "Off") : fmt_time(b, n, map_fade(x));
    case F_GLIDE: return x <= 0 ? snprintf(b, n, "Off") : fmt_time(b, n, map_glide(x));
    case F_CRATE: return fmt_hz(b, n, map_crate(x));
    case F_DTIME:
        if (s->v[P_DL_SYNC] > 0) return snprintf(b, n, "Sync");
        return fmt_time(b, n, map_dtime(x));
    case F_PAN: return x == 0 ? snprintf(b, n, "C") : snprintf(b, n, "%c%.0f", x < 0 ? 'L' : 'R', fabsf(x));
    }
    return snprintf(b, n, "%g", x);
}

static int get_param(void *p, const char *key, char *buf, int n) {
    osm_t *s = p;
    if (n <= 0) return 0;
    if (!strcmp(key, "state")) {
        int o = snprintf(buf, n, "osm1");
        for (int i = 0; i < NP && o < n - 1; i++)
            o += snprintf(buf + o, n - o, ";%s=%.6g", OSM_P[i].key, s->v[i]);
        return o < n ? o : n - 1;
    }
    size_t len = strlen(key);
    if (len > 8 && !strcmp(key + len - 8, "_display")) {
        int id = find_param(key, len - 8);
        if (id < 0 || OSM_P[id].fmt == F_NONE) return 0;
        return display(s, id, buf, n);
    }
    int id = find_param(key, len);
    if (id < 0) return 0;
    if (OSM_P[id].integer) return snprintf(buf, n, "%d", (int)s->v[id]);
    return snprintf(buf, n, "%.6g", s->v[id]);
}

static void render(void *p, int16_t *out, int frames) {
    osm_t *s = p;
    if (frames > 128) frames = 128;
    fpmode_t fm = fp_enter();
    if (s->dirty) derive(s);
    memset(s->mixL, 0, sizeof(float) * frames);
    memset(s->mixR, 0, sizeof(float) * frames);
    for (int pos = 0; pos < frames; pos += CR) {
        int n = frames - pos < CR ? frames - pos : CR;
        float dt = n / SR;
        for (int l = 0; l < 3; l++) {   /* the free-running LFOs, shared by every voice */
            lfo_advance(&s->glph[l], &s->glra[l], &s->glrb[l], lfo_hz(s, l) * dt, &s->seed);
            s->glval[l] = lfo_value((int)s->v[P_L1_SHAPE + 5 * l], s->glph[l], s->glra[l], s->glrb[l]);
        }
        for (int i = 0; i < MAXV; i++) {
            voice_t *vc = &s->vc[i];
            if (!vc->active) continue;
            voice_tick(s, vc, n);
            voice_render(s, vc, n, s->mixL + pos, s->mixR + pos);
        }
    }
    chorus(s, frames);
    delay(s, frames);
    float g0 = s->gain_cur < 0 ? s->vol_g : s->gain_cur, dg = (s->vol_g - g0) / frames;
    for (int i = 0; i < frames; i++) {
        /* a 5 Hz DC blocker first: drive and the limiters make DC from asymmetric waves (narrow pulses) */
        float xl = s->mixL[i] * g0, xr = s->mixR[i] * g0;
        s->dc_y[0] = xl - s->dc_x[0] + 0.99929f * s->dc_y[0]; s->dc_x[0] = xl;
        s->dc_y[1] = xr - s->dc_x[1] + 0.99929f * s->dc_y[1]; s->dc_x[1] = xr;
        float l = softclip(s->dc_y[0]), r = softclip(s->dc_y[1]);
        out[2 * i] = (int16_t)lrintf(l * 32767);
        out[2 * i + 1] = (int16_t)lrintf(r * 32767);
        g0 += dg;
    }
    s->gain_cur = s->vol_g;
    fp_leave(fm);
}

static const mpc_engine_t ENGINE = {create, destroy, midi, set_param, get_param, render, NULL, NULL};
const mpc_engine_t *mpc_engine(void) { return &ENGINE; }
