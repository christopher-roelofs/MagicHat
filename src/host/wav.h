/*
 * wav.h — capture the emulated sound stream to a file.
 *
 * Audio is hard to check by assertion and easy to be wrong about, so being
 * able to write it out and look at the waveform matters more here than it
 * would elsewhere: "audio works" should be something you can inspect rather
 * than something I claim.
 */
#ifndef MH_WAV_H
#define MH_WAV_H

#include <stdint.h>

/* Fallback only. The caller supplies the actual output rate; DataRover's
 * mh_sib_output_rate_hz() includes 2x codec reconstruction. */
#define MH_AUDIO_RATE_FALLBACK 11025u

typedef struct mh_wav mh_wav;

mh_wav *mh_wav_open(const char *path, unsigned rate);
void     mh_wav_sample(void *ctx, int16_t sample);   /* an mh_audio_sink */
uint32_t mh_wav_samples(const mh_wav *w);
void     mh_wav_close(mh_wav *w);

#endif
