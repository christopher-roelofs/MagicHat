/*
 * wav.h — capture the emulated sound stream to a file.
 *
 * Audio is hard to check by assertion and easy to be wrong about, so being
 * able to write it out and look at the waveform matters more here than it
 * would elsewhere: "audio works" should be something you can inspect rather
 * than something I claim.
 */
#ifndef MRC_WAV_H
#define MRC_WAV_H

#include <stdint.h>

/* Fallback only. The caller supplies the actual output rate; DataRover's
 * mrc_sib_output_rate_hz() includes 2x codec reconstruction. */
#define MRC_AUDIO_RATE_FALLBACK 11025u

typedef struct mrc_wav mrc_wav;

mrc_wav *mrc_wav_open(const char *path, unsigned rate);
void     mrc_wav_sample(void *ctx, int16_t sample);   /* an mrc_audio_sink */
uint32_t mrc_wav_samples(const mrc_wav *w);
void     mrc_wav_close(mrc_wav *w);

#endif
