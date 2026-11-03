/* Offline test of imported presets (run.sh builds it with ASan/UBSan): each line of the file named on the command line is
 * a state written by tools/serum2_import.py (test_import.py writes them). The engine loads it, saves it back with the
 * same values and points, and plays a chord that is heard and stays finite. Also: a state naming a wavetable file that
 * is not there keeps the built-in table it names. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "engine.h"
#include "ph_params.h"

static const mpc_engine_t *E;
static int fails;
#define CHECK(c, ...) do { int ok_ = !!(c); printf("%s ", ok_ ? "ok  " : "FAIL"); printf(__VA_ARGS__); printf("\n"); if (!ok_) fails++; } while (0)

static float get(void *s, const char *k) { char b[64] = ""; E->get_param(s, k, b, sizeof b); return strtof(b, NULL); }

/* the value of key in a state string, or NAN */
static float in_state(const char *st, const char *key) {
    char pat[64];
    snprintf(pat, sizeof pat, ";%s=", key);
    const char *p = strstr(st, pat);
    return p ? strtof(p + strlen(pat), NULL) : NAN;
}

static int same_values(const char *a, const char *b, char *bad, int n) {
    for (int i = 0; i < NP; i++) {
        if (PH_P[i].flags & PF_VIRTUAL) continue;
        float x = in_state(a, PH_P[i].key), y = in_state(b, PH_P[i].key);
        if (isnan(x)) continue;
        if (fabsf(x - y) > 1e-3f * (1 + fabsf(x))) { snprintf(bad, n, "%s %g -> %g", PH_P[i].key, x, y); return 0; }
    }
    return 1;
}

static void chord(void *s, float *peak, int *finite) {
    static const int notes[4] = {48, 55, 60, 64};
    int16_t out[256];
    *peak = 0; *finite = 1;
    for (int k = 0; k < 4; k++) { uint8_t on[3] = {0x90, (uint8_t)notes[k], 100}; E->midi(s, on, 3); }
    for (int b = 0; b < 700; b++) {   /* 2 s */
        if (b == 520) for (int k = 0; k < 4; k++) { uint8_t off[3] = {0x80, (uint8_t)notes[k], 0}; E->midi(s, off, 3); }
        E->render(s, out, 128);
        for (int i = 0; i < 256; i++) { float v = fabsf(out[i] / 32768.0f); if (v > *peak) *peak = v; if (!isfinite(v)) *finite = 0; }
    }
}

int main(int argc, char **argv) {
    E = mpc_engine();
    static char line[16384], back[8192], bad[128];

    void *s = E->create(NULL);
    E->set_param(s, "state", "ph1;a_table=3;a_tfile=serum2/missing.wav;b_table=20;b_tfile=gone.wav");
    CHECK(get(s, "a_table") == 3 && get(s, "b_table") == 0,
          "a missing table file keeps the built-in the state names (A %g), a user index falls back to the first (B %g)",
          get(s, "a_table"), get(s, "b_table"));
    E->destroy(s);

    FILE *f = argc > 1 ? fopen(argv[1], "r") : NULL;
    if (!f) { printf("no states file\n"); return 1; }
    int n = 0;
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\n")] = 0;
        if (!line[0]) continue;
        n++;
        s = E->create(NULL);
        E->set_param(s, "state", line);
        E->get_param(s, "state", back, sizeof back);
        CHECK(same_values(line, back, bad, sizeof bad), "preset %d: the engine keeps every value%s%s", n, *bad ? ": " : "", bad);
        bad[0] = 0;
        const char *p1 = strstr(line, ";l1_pts="), *p2 = strstr(back, ";l1_pts=");
        if (p1) {
            size_t l1 = strcspn(p1 + 1, ";"), l2 = p2 ? strcspn(p2 + 1, ";") : 0;
            CHECK(p2 && l1 == l2 && !strncmp(p1, p2, l1), "preset %d: and LFO 1's points (%.*s)", n, (int)l1, p1 + 1);
        }
        float pk; int fin;
        chord(s, &pk, &fin);
        CHECK(fin && pk > 0.01f && pk <= 1.0f, "preset %d: a chord is heard and stays finite (peak %.3f)", n, pk);
        E->destroy(s);
    }
    fclose(f);
    CHECK(n >= 3, "%d imported presets loaded", n);
    printf("%d failure(s)\n", fails);
    return fails != 0;
}
