/* Offline test of Philtre's curve editors (run.sh builds it with ASan/UBSan): the ENV and LFO editor controls stand in for
 * the selected envelope's / LFO's own parameters, the engine reports what it changed by itself, the displays follow, LFO
 * points can be edited, added and deleted and come back from a saved state, and the envelope curves and hold are heard. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "engine.h"
#include "ph_params.h"

int mpc_engine_changed(void *inst, char *key, int len);
static const mpc_engine_t *E;
static int fails;
#define CHECK(c, ...) do { int ok_ = !!(c); printf("%s ", ok_ ? "ok  " : "FAIL"); printf(__VA_ARGS__); printf("\n"); if (!ok_) fails++; } while (0)

static void set(void *s, const char *k, float v) { char b[32]; snprintf(b, sizeof b, "%g", v); E->set_param(s, k, b); }
static float get(void *s, const char *k) { char b[64] = ""; E->get_param(s, k, b, sizeof b); return strtof(b, NULL); }
static void text(void *s, const char *k, char *b, int n) { char key[64]; snprintf(key, sizeof key, "%s_display", k); b[0] = 0; E->get_param(s, key, b, n); }
static int reported(void *s, const char *want) {   /* drains the engine's change list; 1 if want was in it */
    char k[64]; int seen = 0;
    while (mpc_engine_changed(s, k, sizeof k)) if (!strcmp(k, want)) seen = 1;
    return seen;
}
static void run(void *s, int blocks) { int16_t out[256]; for (int i = 0; i < blocks; i++) E->render(s, out, 128); }
/* a note's level (RMS, left and right) and brightness (RMS of the step from sample to sample, over the level) */
static void measure(void *s, float *lv, float *br, float *rv) {
    uint8_t on[3] = {0x90, 60, 127}, off[3] = {0x80, 60, 0};
    int16_t out[256]; double sl = 0, sd = 0, sr = 0; float prev = 0; int n = 0;
    E->midi(s, on, 3);
    for (int b = 0; b < 30; b++) {
        E->render(s, out, 128);
        for (int i = 0; i < 256; i += 2) {
            float l = out[i] / 32768.0f, r = out[i + 1] / 32768.0f;
            if (b >= 6) { sl += l * l; sd += (l - prev) * (l - prev); sr += r * r; n++; }
            prev = l;
        }
    }
    E->midi(s, off, 3);
    run(s, 60);
    *lv = sqrtf(sl / n); *br = sqrtf(sd / n) / (*lv + 1e-9f); *rv = sqrtf(sr / n);
}
static float disp(void *s, const char *kind, int i) { char k[32]; snprintf(k, sizeof k, "%s_%d", kind, i); return get(s, k); }

/* peak of the left channel over blocks [b0, b1) after a note-on */
static float level(void *s, int b0, int b1) {
    uint8_t on[3] = {0x90, 60, 127};
    E->midi(s, on, 3);
    int16_t out[256]; float pk = 0;
    for (int b = 0; b < b1; b++) {
        E->render(s, out, 128);
        if (b >= b0) for (int i = 0; i < 256; i += 2) if (fabsf((float)out[i]) > pk) pk = fabsf((float)out[i]);
    }
    uint8_t m[3] = {0xB0, 120, 0};
    E->midi(s, m, 3);
    run(s, 40);
    return pk / 32768;
}

int main(void) {
    E = mpc_engine();
    void *s = E->create(NULL);
    char t[64];

    /* ---- ENV editor ---- */
    CHECK(get(s, "ev_a") == get(s, "e1_a"), "ENV editor starts on Env 1 (attack %g)", get(s, "ev_a"));
    reported(s, "");
    set(s, "ev_sel", 2);
    CHECK(reported(s, "ev_d"), "choosing Env 3 reports the editor's controls");
    set(s, "e3_d", 61);
    CHECK(get(s, "ev_d") == 61 && reported(s, "ev_d"), "a host change of Env 3 decay shows in the editor (%g)", get(s, "ev_d"));
    set(s, "ev_a", 42); set(s, "ev_h", 30); set(s, "ev_rc", -40);
    CHECK(get(s, "e3_a") == 42 && get(s, "e3_h") == 30 && get(s, "e3_rc") == -40 && get(s, "e1_a") != 42,
          "editor knobs set Env 3 only (attack %g, hold %g, R curve %g)", get(s, "e3_a"), get(s, "e3_h"), get(s, "e3_rc"));
    CHECK(reported(s, "e3_a"), "and the host is told Env 3 attack moved");
    text(s, "ev_h", t, sizeof t);
    CHECK(strstr(t, "ms") || strstr(t, " s"), "hold shows a time (\"%s\")", t);
    reported(s, "");
    run(s, 40);
    int dr = 0;
    for (char k[64]; mpc_engine_changed(s, k, sizeof k);) if (!strncmp(k, "envd_", 5)) dr++;
    CHECK(dr > 0, "the display columns that moved are reported (%d)", dr);
    float d1 = disp(s, "envd", 1), dmax = 0, dlast = disp(s, "envd", DISP_COLS);
    for (int i = 1; i <= DISP_COLS; i++) if (disp(s, "envd", i) > dmax) dmax = disp(s, "envd", i);
    CHECK(dmax == DISP_LEVELS - 1 && d1 < dmax && dlast <= 2, "ENV display: rises to the top and ends near 0 (%g .. %g .. %g)", d1, dmax, dlast);
    set(s, "envd_5", 0);
    CHECK(disp(s, "envd", 5) != 0 || dmax == 0, "a host set of a display column is ignored");

    /* ---- LFO editor ---- */
    set(s, "lv_sel", 1);
    set(s, "l2_shape", LS_SQUARE);
    reported(s, "");
    run(s, 40);
    float first = disp(s, "lfod", 5), second = disp(s, "lfod", DISP_COLS - 5);
    CHECK(first >= DISP_LEVELS - 2 && second <= 1, "LFO display of a square: high then low (%g, %g)", first, second);
    CHECK(get(s, "lv_shape") == LS_SQUARE, "the editor's SHAPE shows LFO 2's shape");
    set(s, "lv_pt", 2);
    set(s, "lv_ptl", -50);
    CHECK(get(s, "l2_shape") == LS_CUSTOM && reported(s, "l2_shape"), "editing a point makes LFO 2 Custom, and says so");
    text(s, "lv_pt", t, sizeof t);
    CHECK(!strcmp(t, "2 / 4"), "the square became four points (\"%s\")", t);
    CHECK(get(s, "lv_ptl") == -50, "point 2 level -50 (%g)", get(s, "lv_ptl"));
    set(s, "lv_ptt", 99);
    CHECK(get(s, "lv_ptt") < 50, "a point's time stays before the next point (%g)", get(s, "lv_ptt"));
    set(s, "lv_add", 1);   /* point 2 sits right before point 3: the new point goes before it */
    text(s, "lv_pt", t, sizeof t);
    CHECK(!strcmp(t, "2 / 5") && get(s, "lv_ptt") > 20 && get(s, "lv_ptt") < 30, "ADD with no room after puts it before, selected (\"%s\", %g)", t, get(s, "lv_ptt"));
    set(s, "lv_del", 1);
    text(s, "lv_pt", t, sizeof t);
    CHECK(!strcmp(t, "1 / 4"), "DELETE removes it (\"%s\")", t);
    set(s, "lv_pt", 3);
    set(s, "lv_add", 1);
    text(s, "lv_pt", t, sizeof t);
    CHECK(!strcmp(t, "4 / 5") && get(s, "lv_ptt") > 70, "ADD with room puts it after (\"%s\", %g)", t, get(s, "lv_ptt"));
    set(s, "lv_del", 1);
    set(s, "lv_pt", 1); set(s, "lv_del", 1);
    text(s, "lv_pt", t, sizeof t);
    CHECK(!strcmp(t, "1 / 4"), "the first point can't be deleted (\"%s\")", t);
    set(s, "lv_pt", 9);
    CHECK(get(s, "lv_pt") == 4, "POINT stops at the last point (%g)", get(s, "lv_pt"));
    set(s, "lv_sel", 0);
    set(s, "lv_shape", LS_SAW_UP);
    set(s, "lv_topts", 1);
    CHECK(get(s, "l1_shape") == LS_CUSTOM && get(s, "l2_shape") == LS_CUSTOM, "TO POINTS on LFO 1");

    /* ---- state ---- */
    char st[8192];
    E->get_param(s, "state", st, sizeof st);
    void *b = E->create(NULL);
    E->set_param(b, "state", st);
    set(b, "lv_sel", 1); set(b, "lv_pt", 2);
    CHECK(get(b, "l2_shape") == LS_CUSTOM && get(b, "lv_ptl") == -50 && get(b, "e3_rc") == -40,
          "a saved state brings back the points, the shape and the curves (%g, %g)", get(b, "lv_ptl"), get(b, "e3_rc"));
    CHECK(strlen(st) < 7000, "state size %zu bytes (the wrapper keeps 8192)", strlen(st));
    E->destroy(b);

    /* ---- the curves and hold, heard ---- */
    void *c = E->create(NULL);
    set(c, "f_on", 0); set(c, "a_uni", 1); set(c, "vel", 0);
    set(c, "e1_a", 60); set(c, "e1_ac", 100);
    float fast = level(c, 2, 4);
    set(c, "e1_ac", -100);
    float slow = level(c, 2, 4);
    CHECK(fast > 2 * slow, "attack curve +100 is louder early than -100 (%.3f vs %.3f)", fast, slow);
    set(c, "e1_a", 0); set(c, "e1_d", 30); set(c, "e1_s", 0); set(c, "e1_ac", 0);
    set(c, "e1_h", 0);
    float nohold = level(c, 30, 34);
    set(c, "e1_h", 70);
    float hold = level(c, 30, 34);
    CHECK(hold > 0.1f && nohold < 0.01f, "hold keeps the level up after a short decay would have ended it (%.3f vs %.3f)", hold, nohold);
    E->destroy(c);

    /* ---- an effect slot switched off and on (0.4.0) ---- */
    void *d = E->create(NULL);
    set(d, "f_on", 0); set(d, "a_uni", 1); set(d, "vel", 0); set(d, "a_rand", 0);   /* the same start phase every note */
    float dry = level(d, 4, 8);
    set(d, "fx1_type", FX_DISTORTION); set(d, "fx1_p1", 100); set(d, "fx1_mix", 100);
    float wet = level(d, 4, 8);
    set(d, "fx1_on", 0);
    float off = level(d, 4, 8);
    CHECK(fabsf(off - dry) < 0.03f * dry && fabsf(wet - dry) > 0.2f * dry,   /* (notes differ a little: the DC filter) */
          "a slot switched off sounds as if it were empty (%.4f, %.4f; with it on %.4f)", off, dry, wet);
    CHECK(get(d, "fx1_type") == FX_DISTORTION && get(d, "fx1_p1") == 100, "and keeps its effect and settings");
    char st2[8192] = "";
    E->get_param(d, "state", st2, sizeof st2);
    void *d2 = E->create(NULL);
    E->set_param(d2, "state", st2);
    CHECK(get(d2, "fx1_on") == 0 && strstr(st2, ";fx1_on=0"), "a saved state keeps the slot off");
    E->destroy(d2);
    E->set_param(d, "state", "ph1;fx1_type=6");   /* a 0.3.0 state: no switch saved, so the slot is on */
    CHECK(get(d, "fx1_on") == 1, "a state from before 0.4.0 has its slots on");
    /* a delay switched off while echoing, then on: no echo from before comes back, and no click either way */
    set(d, "a_pos", 0);   /* a sine: its echoes have no edges of their own, so a click would stand out */
    set(d, "fx1_type", FX_DELAY); set(d, "fx1_p1", 0); set(d, "fx1_p2", 90); set(d, "fx1_mix", 100); set(d, "fx1_on", 1);
    uint8_t on[3] = {0x90, 60, 127}, nf[3] = {0x80, 60, 0};
    int16_t out[256];
    E->midi(d, on, 3); run(d, 20); E->midi(d, nf, 3); run(d, 40);   /* 1/32 echoes of a released note */
    float natural = 0, jump = 0, echo = 0, prev = 0;
    E->render(d, out, 128);
    prev = out[0] / 32768.0f;
    for (int i = 2; i < 256; i += 2) { float v = out[i] / 32768.0f; natural = fmaxf(natural, fabsf(v - prev)); echo = fmaxf(echo, fabsf(v)); prev = v; }
    set(d, "fx1_on", 0);
    E->render(d, out, 128);
    for (int i = 0; i < 256; i += 2) { float v = out[i] / 32768.0f; jump = fmaxf(jump, fabsf(v - prev)); prev = v; }
    run(d, 400);
    set(d, "fx1_on", 1);
    float tail = 0;
    for (int b = 0; b < 200; b++) { E->render(d, out, 128); for (int i = 0; i < 256; i++) tail = fmaxf(tail, fabsf(out[i] / 32768.0f)); }
    CHECK(tail < 1e-3f, "switched back on, the delay starts empty (peak %.5f)", tail);
    CHECK(echo > 0.01f && jump <= natural * 1.5f + 1e-3f,
          "switching off mid-echo fades over a block: no step bigger than the echo's own (%.4f vs %.4f, echo %.3f)",
          jump, natural, echo);
    E->destroy(d);

    /* ---- 16 slots and the Splitter (0.5.0) ---- */
    void *e = E->create(NULL);
    set(e, "f_on", 0); set(e, "a_uni", 1); set(e, "vel", 0); set(e, "a_rand", 0);
    float lv0, br0, rv0, lv, br, rv;
    measure(e, &lv0, &br0, &rv0);
    set(e, "fx16_type", FX_DISTORTION); set(e, "fx16_p1", 100); set(e, "fx16_mix", 100);
    measure(e, &lv, &br, &rv);
    CHECK(fabsf(lv - lv0) > 0.2f * lv0, "slot 16 plays (level %.3f against %.3f)", lv, lv0);
    set(e, "fx16_type", FX_OFF);
    set(e, "fx1_type", FX_SPLITTER);
    CHECK(get(e, "fx1_mix") == 100, "a Splitter comes with its mix at 100");
    set(e, "fx1_p1", 100 * logf(200 / 20.0f) / logf(1000)); set(e, "fx1_p2", 100 * logf(2000 / 20.0f) / logf(1000));
    set(e, "fx1_p3", 12.5f);   /* 3 bands */
    measure(e, &lv, &br, &rv);
    CHECK(fabsf(lv - lv0) < 0.01f * lv0 && fabsf(br - br0) < 0.01f * br0,
          "a Splitter with no slot on its bands gives back what went in (level %.4f / %.4f, brightness %.3f / %.3f)", lv, lv0, br, br0);
    set(e, "fx2_type", FX_FILTER); set(e, "fx2_p1", 0); set(e, "fx2_p2", 0); set(e, "fx2_p3", 0); set(e, "fx2_mix", 100);   /* a 20 Hz low-pass */
    set(e, "fx2_band", 3);
    measure(e, &lv, &br, &rv);
    CHECK(br < 0.5f * br0 && lv > 0.6f * lv0, "the low-pass on Band 3 (over 2 kHz) takes the highs only (brightness %.3f -> %.3f, level %.3f -> %.3f)",
          br0, br, lv0, lv);
    set(e, "fx2_band", 1); set(e, "fx1_p1", 100 * logf(60 / 20.0f) / logf(1000));   /* 12 dB/octave: well under the note */
    measure(e, &lv, &br, &rv);
    CHECK(fabsf(lv - lv0) < 0.1f * lv0 && fabsf(br - br0) < 0.1f * br0,
          "on Band 1 (under 60 Hz) it leaves a middle C nearly as it was (level %.3f / %.3f, brightness %.3f / %.3f)", lv, lv0, br, br0);
    set(e, "fx1_p3", 62.5f); set(e, "fx2_band", 2);   /* left / right: Band 2 is the right channel */
    measure(e, &lv, &br, &rv);
    CHECK(rv < 0.05f * lv && lv > 0.9f * lv0, "Left / Right: the low-pass on Band 2 silences the right channel only (L %.3f, R %.4f)", lv, rv);
    set(e, "fx3_type", FX_DISTORTION); set(e, "fx3_band", 0); set(e, "fx3_mix", 0);   /* All: the bands join before it */
    measure(e, &lv, &br, &rv);
    CHECK(rv < 0.05f * lv, "a slot set to All after them plays on the joined bands (R still %.4f)", rv);
    set(e, "fx1_on", 0);
    measure(e, &lv, &br, &rv);
    CHECK(lv < 0.1f * lv0, "the Splitter switched off: the slots set to a band play on the whole sound (level %.4f)", lv);
    char st3[8192] = "";
    E->get_param(e, "state", st3, sizeof st3);
    void *e2 = E->create(NULL);
    E->set_param(e2, "state", st3);
    CHECK(get(e2, "fx1_type") == FX_SPLITTER && get(e2, "fx2_band") == 2 && get(e2, "fx1_p3") == 62.5f && get(e2, "fx1_on") == 0,
          "a saved state keeps the Splitter, its mode and the bands (%zu bytes of 8192)", strlen(st3));
    char nm[32];
    E->get_param(e2, "fx1_p1_name", nm, sizeof nm);
    CHECK(!strcmp(nm, "LOW / MID"), "the Splitter's knobs are named (\"%s\")", nm);
    text(e2, "fx1_p3", nm, sizeof nm);
    CHECK(!strcmp(nm, "Left / Right"), "and its mode shows (\"%s\")", nm);
    E->destroy(e2);
    E->destroy(e);
    E->destroy(s);
    printf("%s\n", fails ? "FAILED" : "PASSED");
    return fails != 0;
}
