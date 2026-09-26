#include "soc/tx39/tx39.h"
#include <stdio.h>
#include <stdlib.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "line %d: %s\n", __LINE__, #x); exit(1); } } while (0)

static unsigned received;
static int16_t last;
static void sample(void *ctx, int16_t value)
{
    (void)ctx;
    received++;
    last = value;
}

int main(void)
{
    mh_bus bus;
    r3900 cpu = {0};
    tx39 soc;
    uint8_t ram[16] = {0x12, 0x34, 0xff, 0xfe};
    mh_bus_init(&bus);
    CHECK(mh_bus_add_ram(&bus, "sound", 0, ram, sizeof(ram), sizeof(ram)));
    cpu.bus = &bus;
    mh_tx39_init(&soc, &cpu, 36864000);
    tx39_sib *sib = &soc.sib;
    sib->ctrl = SIBCTRL_ENSIB | SIBCTRL_ENSND | (35u << SIBCTRL_SNDFSDIV_SHIFT);
    sib->dmactrl = SIBDMA_ENDMATXSND;
    sib->size = 3u << SIBSIZE_SND_SHIFT;
    sib->sndtxstart = TX39_KUSEG_DRAM_BANK0;
    CHECK(mh_sib_snd_rate_hz(sib) == 8000);
    mh_sib_pump_audio(sib);
    cpu.cycle_count = 4 * 4608;
    mh_sib_pump_audio(sib);
    CHECK(sib->snd_samples == 4 && sib->snd_read_off == 8);
    CHECK(soc.icu.status[0] & INT1_SND0_5INT);
    cpu.cycle_count = 8 * 4608;
    mh_sib_pump_audio(sib);
    CHECK(sib->snd_samples == 8 && sib->snd_read_off == 0);
    CHECK(soc.icu.status[0] & INT1_SND1_0INT);
    CHECK(soc.icu.status[0] & INT1_SNDDMACNTINT);

    /* Attaching a speaker must not restart the ring or its clock. */
    mh_sib_set_audio_sink(sib, sample, NULL);
    cpu.cycle_count = 9 * 4608;
    mh_sib_pump_audio(sib);
    CHECK(received == 2 && last == 0); /* DAC disabled, DMA still moves. */
    cpu.cycle_count = 10 * 4608;
    mh_sib_pump_audio(sib);
    CHECK(received == 4 && last == 0);
    mh_sib_set_audio_sink(sib, NULL, NULL);
    cpu.cycle_count = 12 * 4608;
    mh_sib_pump_audio(sib);
    CHECK(received == 4 && sib->snd_read_off == 8);
    sib->dmactrl = 0;
    cpu.cycle_count = 13 * 4608;
    mh_sib_pump_audio(sib);
    CHECK(sib->snd_samples == 12);
    sib->dmactrl = SIBDMA_ENDMATXSND;
    sib->codec.reg[8] = UCB_AUDIO_OUT_ENA | UCB_AUDIO_MUTE;
    mh_sib_pump_audio(sib);
    mh_sib_set_audio_sink(sib, sample, NULL);
    unsigned before = received;
    cpu.cycle_count += 80 * 4608;
    mh_sib_pump_audio(sib);
    CHECK(received == before + 160 && last == 0);
    CHECK(sib->snd_samples == 92); /* mute did not stall DMA */
    CHECK(soc.audio.highpass != 0); /* processing continued behind mute */
    CHECK(mh_sib_output_rate_hz(sib) == 16000);
    sib->dmactrl = 0;
    CHECK(mh_sib_write(sib, TX39_SIBSF0CTRL,
                       (8u << SIBSF_REGADDR_SHIFT) | SIBSF0_WRITE));
    CHECK(soc.audio.highpass == 0 && soc.audio.previous_input == 0);
    puts("SIB DMA, codec control, and host sink independence passed");
    return 0;
}
