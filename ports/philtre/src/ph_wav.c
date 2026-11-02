/* Philtre's user wavetable library (see ph_wav.h). */
#include <dirent.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include "ph_wav.h"

#define MAX_DEPTH 3
#define MAX_BYTES (64L << 20)

struct ph_lib {
    char root[1024];
    int n;
    char rel[LIB_MAX][256], name[LIB_MAX][LIB_NAME];
    pthread_t th;
    int running, quit;
    pthread_mutex_t mu;
    pthread_cond_t cv;
    int want[2], have[2];          /* requested file per oscillator; the file the worker has loaded (or is loading) */
    wtab_t *ready[2];              /* loaded, not taken yet */
    int ready_file[2];
    wtab_t *retire[8];             /* given back by the engine, to free */
    int nretire;
};

/* ---- listing ---- */
static int cmp_rel(const void *a, const void *b) { return strcmp((const char *)a, (const char *)b); }

static void scan(ph_lib_t *l, const char *rel, int depth) {
    char path[1400];
    snprintf(path, sizeof path, "%s%s%s", l->root, *rel ? "/" : "", rel);
    DIR *d = opendir(path);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d)) && l->n < LIB_MAX) {
        if (e->d_name[0] == '.') continue;
        char r[512], full[1700];
        snprintf(r, sizeof r, "%s%s%s", rel, *rel ? "/" : "", e->d_name);
        snprintf(full, sizeof full, "%s/%s", l->root, r);
        struct stat st;
        if (lstat(full, &st) || S_ISLNK(st.st_mode)) continue;   /* no symlinks */
        if (S_ISDIR(st.st_mode)) {
            if (depth < MAX_DEPTH) scan(l, r, depth + 1);
            continue;
        }
        size_t len = strlen(r);
        if (!S_ISREG(st.st_mode) || len < 5 || len >= sizeof l->rel[0] || strcasecmp(r + len - 4, ".wav")) continue;
        memcpy(l->rel[l->n++], r, len + 1);   /* len < sizeof l->rel[0], checked above */
    }
    closedir(d);
}

/* ---- WAV ---- */
static uint32_t le32(const unsigned char *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static uint16_t le16(const unsigned char *p) { return (uint16_t)(p[0] | p[1] << 8); }

float *wav_frames(const char *path, int *nf, int *frame_len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size < 44 || size > MAX_BYTES) { fclose(f); return NULL; }
    unsigned char *b = malloc(size);
    if (!b || fread(b, 1, size, f) != (size_t)size) { fclose(f); free(b); return NULL; }
    fclose(f);
    float *out = NULL;
    int fmt = 0, ch = 0, bits = 0, clm = 0;
    const unsigned char *data = NULL;
    uint32_t dlen = 0;
    if (memcmp(b, "RIFF", 4) || memcmp(b + 8, "WAVE", 4)) goto done;
    for (long p = 12; p + 8 <= size;) {
        uint32_t cl = le32(b + p + 4);
        const unsigned char *c = b + p + 8;
        if ((long)cl > size - p - 8) cl = (uint32_t)(size - p - 8);
        if (!memcmp(b + p, "fmt ", 4) && cl >= 16) {
            fmt = le16(c); ch = le16(c + 2); bits = le16(c + 14);
            if (fmt == 0xFFFE && cl >= 26) fmt = le16(c + 24);   /* WAVE_FORMAT_EXTENSIBLE: the sub-format */
        } else if (!memcmp(b + p, "data", 4)) {
            data = c; dlen = cl;
        } else if (!memcmp(b + p, "clm ", 4) && cl > 3) {   /* "<!>2048 ..." : the frame length */
            char txt[32] = {0};
            memcpy(txt, c, cl < sizeof txt - 1 ? cl : sizeof txt - 1);
            char *q = strstr(txt, "<!>");
            if (q) clm = atoi(q + 3);
        }
        p += 8 + cl + (cl & 1);
    }
    if (!data || ch < 1 || !((fmt == 1 && (bits == 16 || bits == 24 || bits == 32)) || (fmt == 3 && bits == 32))) goto done;
    int bps = bits / 8;
    long total = dlen / (bps * ch);
    if (total < 2) goto done;
    int fl = clm >= 16 && clm <= 8192 ? clm : total < WT_USER_LEN ? (int)total : WT_USER_LEN;
    long frames = total / fl;
    if (frames < 1) goto done;
    if (frames > WT_MAX_FRAMES) frames = WT_MAX_FRAMES;
    out = malloc(sizeof(float) * frames * fl);
    if (!out) goto done;
    for (long i = 0; i < frames * fl; i++) {   /* the first channel */
        const unsigned char *s = data + i * bps * ch;
        float v;
        if (fmt == 3) { uint32_t u = le32(s); memcpy(&v, &u, 4); }
        else if (bits == 16) v = (int16_t)le16(s) / 32768.0f;
        else if (bits == 24) v = (int32_t)((uint32_t)(s[0] << 8 | s[1] << 16 | (uint32_t)s[2] << 24)) / 2147483648.0f;
        else v = (int32_t)le32(s) / 2147483648.0f;
        out[i] = v == v ? v : 0;
    }
    *nf = (int)frames;
    *frame_len = fl;
done:
    free(b);
    return out;
}

/* ---- worker ---- */
static wtab_t *load(ph_lib_t *l, int file) {
    char full[1400];
    snprintf(full, sizeof full, "%s/%s", l->root, l->rel[file]);
    int nf = 0, fl = 0;
    float *fr = wav_frames(full, &nf, &fl);
    if (!fr) return NULL;
    wtab_t *t = wt_from_frames(fr, nf, fl);
    free(fr);
    return t;
}

static void *worker(void *arg) {
    ph_lib_t *l = arg;
    pthread_mutex_lock(&l->mu);
    while (!l->quit) {
        if (l->nretire) {
            wtab_t *t = l->retire[--l->nretire];
            pthread_mutex_unlock(&l->mu);
            wt_free(t);
            pthread_mutex_lock(&l->mu);
            continue;
        }
        int o = l->want[0] != l->have[0] ? 0 : l->want[1] != l->have[1] ? 1 : -1;
        if (o >= 0) {
            int file = l->want[o];
            l->have[o] = file;
            pthread_mutex_unlock(&l->mu);
            wtab_t *t = file >= 0 && file < l->n ? load(l, file) : NULL;
            pthread_mutex_lock(&l->mu);
            if (t && l->want[o] == file) {
                if (l->ready[o]) wt_free(l->ready[o]);   /* an earlier load nobody took */
                l->ready[o] = t;
                l->ready_file[o] = file;
            } else {
                wt_free(t);
            }
            continue;
        }
        pthread_cond_wait(&l->cv, &l->mu);
    }
    pthread_mutex_unlock(&l->mu);
    return NULL;
}

ph_lib_t *lib_open(const char *dir) {
    ph_lib_t *l = calloc(1, sizeof *l);
    if (!l) return NULL;
    pthread_mutex_init(&l->mu, NULL);
    pthread_cond_init(&l->cv, NULL);
    l->want[0] = l->want[1] = l->have[0] = l->have[1] = -1;
    if (dir && *dir) {
        snprintf(l->root, sizeof l->root, "%s", dir);
        scan(l, "", 1);
        qsort(l->rel, l->n, sizeof l->rel[0], cmp_rel);
        for (int i = 0; i < l->n; i++) {
            const char *base = strrchr(l->rel[i], '/');
            base = base ? base + 1 : l->rel[i];
            snprintf(l->name[i], LIB_NAME, "%.*s", (int)(strlen(base) - 4 < LIB_NAME - 1 ? strlen(base) - 4 : LIB_NAME - 1), base);
        }
    }
    if (!pthread_create(&l->th, NULL, worker, l)) l->running = 1;
    return l;
}

void lib_close(ph_lib_t *l) {
    if (!l) return;
    pthread_mutex_lock(&l->mu);
    l->quit = 1;
    pthread_cond_signal(&l->cv);
    pthread_mutex_unlock(&l->mu);
    if (l->running) pthread_join(l->th, NULL);
    for (int o = 0; o < 2; o++) wt_free(l->ready[o]);
    while (l->nretire) wt_free(l->retire[--l->nretire]);
    pthread_mutex_destroy(&l->mu);
    pthread_cond_destroy(&l->cv);
    free(l);
}

int lib_count(const ph_lib_t *l) { return l ? l->n : 0; }
const char *lib_name(const ph_lib_t *l, int i) { return l && i >= 0 && i < l->n ? l->name[i] : ""; }
const char *lib_path(const ph_lib_t *l, int i) { return l && i >= 0 && i < l->n ? l->rel[i] : ""; }
int lib_find(const ph_lib_t *l, const char *rel) {
    for (int i = 0; l && i < l->n; i++)
        if (!strcmp(l->rel[i], rel)) return i;
    return -1;
}

void lib_request(ph_lib_t *l, int osc, int file) {
    if (!l || osc < 0 || osc > 1) return;
    pthread_mutex_lock(&l->mu);
    if (l->want[osc] != file) {
        l->want[osc] = file;
        pthread_cond_signal(&l->cv);
    }
    pthread_mutex_unlock(&l->mu);
}

wtab_t *lib_take(ph_lib_t *l, int osc, int *file) {
    if (!l || pthread_mutex_trylock(&l->mu)) return NULL;
    wtab_t *t = l->ready[osc];
    if (t) { *file = l->ready_file[osc]; l->ready[osc] = NULL; }
    pthread_mutex_unlock(&l->mu);
    return t;
}

int lib_retire(ph_lib_t *l, wtab_t *t) {
    if (!t) return 1;
    if (!l) { wt_free(t); return 1; }
    if (pthread_mutex_trylock(&l->mu)) return 0;
    int ok = l->nretire < 8;
    if (ok) { l->retire[l->nretire++] = t; pthread_cond_signal(&l->cv); }
    pthread_mutex_unlock(&l->mu);
    return ok;
}
