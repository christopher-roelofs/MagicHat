#include "host/wav.h"

#include <stdio.h>
#include <stdlib.h>

struct mh_wav {
    FILE    *f;
    unsigned rate;
    uint32_t samples;
};

static void put32(FILE *f, uint32_t v)
{
    fputc((int)(v & 0xFF), f);        fputc((int)((v >> 8) & 0xFF), f);
    fputc((int)((v >> 16) & 0xFF), f); fputc((int)((v >> 24) & 0xFF), f);
}

static void put16(FILE *f, uint16_t v)
{
    fputc((int)(v & 0xFF), f); fputc((int)((v >> 8) & 0xFF), f);
}

mh_wav *mh_wav_open(const char *path, unsigned rate)
{
    mh_wav *w = calloc(1, sizeof(*w));
    if (!w)
        return NULL;
    w->f = fopen(path, "wb");
    if (!w->f) {
        fprintf(stderr, "cannot write %s\n", path);
        free(w);
        return NULL;
    }
    w->rate = rate;

    /* Header with placeholder lengths; filled in at close. */
    fwrite("RIFF", 1, 4, w->f); put32(w->f, 0);
    fwrite("WAVEfmt ", 1, 8, w->f);
    put32(w->f, 16);            /* PCM fmt chunk size */
    put16(w->f, 1);             /* PCM */
    put16(w->f, 1);             /* mono */
    put32(w->f, rate);
    put32(w->f, rate * 2);      /* byte rate */
    put16(w->f, 2);             /* block align */
    put16(w->f, 16);            /* bits */
    fwrite("data", 1, 4, w->f); put32(w->f, 0);
    return w;
}

void mh_wav_sample(void *ctx, int16_t sample)
{
    mh_wav *w = ctx;
    put16(w->f, (uint16_t)sample);
    w->samples++;
}

uint32_t mh_wav_samples(const mh_wav *w)
{
    return w->samples;
}

void mh_wav_close(mh_wav *w)
{
    if (!w)
        return;
    uint32_t data = w->samples * 2;
    fseek(w->f, 4, SEEK_SET);  put32(w->f, 36 + data);
    fseek(w->f, 40, SEEK_SET); put32(w->f, data);
    fclose(w->f);
    free(w);
}
