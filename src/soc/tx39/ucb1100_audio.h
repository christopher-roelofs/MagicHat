#ifndef MRC_UCB1100_AUDIO_H
#define MRC_UCB1100_AUDIO_H
#include <stdint.h>

#define UCB_AUDIO_ATT_MASK 0x001fu
#define UCB_AUDIO_MUTE     0x2000u
#define UCB_AUDIO_OUT_ENA  0x8000u
#define UCB_AUDIO_TAPS 127u

/* Per-codec signal history, included in machine snapshots. */
typedef struct {
    double previous_input, highpass;
    double history[UCB_AUDIO_TAPS];
    unsigned cursor;
} ucb1100_audio;

int16_t mrc_ucb_audio_decode(uint16_t serial);
double mrc_ucb_audio_gain(uint16_t control_b);
/* Two reconstructed samples per codec input sample. Mute gates the output,
 * while disabled DAC resets the approximation's filter history. */
void mrc_ucb_audio_sample(ucb1100_audio *a, uint16_t control_b,
                         uint16_t serial, int16_t output[2]);
#endif
