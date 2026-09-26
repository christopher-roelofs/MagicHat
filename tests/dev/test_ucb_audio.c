#include "soc/tx39/ucb1100_audio.h"
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "line %d: %s\n", __LINE__, #x); exit(1); } } while (0)
#define PI 3.14159265358979323846

/* Measure the wanted output or its reconstruction image at 2x Fs, after
 * settling. Frequencies are normalized to the codec input sample rate. */
static double tone(double input_f, double output_f, unsigned control)
{
    ucb1100_audio a = {0};
    double re = 0, im = 0;
    unsigned count = 100000;
    for (unsigned i = 0; i < count + 100000; i++) {
        int16_t input = (int16_t)lround(12000 * sin(2 * PI * input_f * i));
        int16_t out[2];
        mh_ucb_audio_sample(&a, UCB_AUDIO_OUT_ENA | control, (uint16_t)input, out);
        if (i < 100000) continue;
        for (unsigned p = 0; p < 2; p++) {
            double phase = 2 * PI * output_f * (i + p * .5);
            re += out[p] * cos(phase);
            im += out[p] * sin(phase);
        }
    }
    return hypot(re, im) / count / 12000;
}
int main(void)
{
    CHECK(mh_ucb_audio_decode(0x7fff) == 32752);
    CHECK(mh_ucb_audio_decode(0x800f) == -32768);
    CHECK(mh_ucb_audio_decode(0xffff) == -16);
    CHECK(mh_ucb_audio_decode(0x000f) == 0);
    for (unsigned i = 0; i < 24; i++)
        CHECK(fabs(20 * log10(mh_ucb_audio_gain(i)) + 3*i) < 1e-9);
    CHECK(mh_ucb_audio_gain(31) == mh_ucb_audio_gain(23));
    ucb1100_audio a = {0}, b = {0};
    int16_t out[2], other[2];
    for (unsigned i = 0; i < 1000; i++) {
        mh_ucb_audio_sample(&a, UCB_AUDIO_OUT_ENA | UCB_AUDIO_MUTE, 0x432f, out);
        mh_ucb_audio_sample(&b, UCB_AUDIO_OUT_ENA, 0x4320, other);
        CHECK(out[0] == 0 && out[1] == 0);
    }
    /* Mute must not freeze filters; low serial bits never affect them. */
    CHECK(memcmp(&a, &b, sizeof(a)) == 0);
    mh_ucb_audio_sample(&a, UCB_AUDIO_OUT_ENA, 0x4320, out);
    mh_ucb_audio_sample(&b, UCB_AUDIO_OUT_ENA, 0x4320, other);
    CHECK(out[0] == other[0] && out[1] == other[1]);
    for (unsigned i = 0; i < 100000; i++)
        mh_ucb_audio_sample(&a, UCB_AUDIO_OUT_ENA, 0xfd00, out);
    CHECK(abs(out[0]) <= 1 && abs(out[1]) <= 1); /* idle -768 DC removed */
    mh_ucb_audio_sample(&a, 0, 0x7fff, out);
    CHECK(out[0] == 0 && out[1] == 0);
    memset(&b, 0, sizeof(b));
    CHECK(memcmp(&a, &b, sizeof(a)) == 0);
    double f[] = {.00016, .01, .1, .3, .42};
    for (unsigned i = 0; i < sizeof(f)/sizeof(f[0]); i++) {
        double db = 20 * log10(tone(f[i], f[i], 0));
        printf("passband %.5f Fs: %.4f dB\n", f[i], db);
        CHECK(fabs(db) < .5);
    }
    double image = 20 * log10(tone(.4, .6, 0));
    printf("image at 0.6 Fs: %.2f dB\n", image);
    CHECK(image < -70);
    double atten = 20 * log10(tone(.1, .1, 4));
    CHECK(fabs(atten + 12) < .05);
    puts("UCB1100 precision, controls, DC rejection and reconstruction passed");
}
