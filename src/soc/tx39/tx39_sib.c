/*
 * tx39_sib.c — TX39 serial interface bus, host side.
 *
 * The SIB carries a 64-bit frame to and from the UCB1100 codec every sample
 * period. Subframe 0 is the codec's control-register channel; software uses
 * it like a simple indexed register port:
 *
 *   write: SIBSF0CTRL = REGADDR | SIBSF0_WRITE | data
 *   read:  SIBSF0CTRL = REGADDR                       (no WRITE bit)
 *          poll SIBSF0STAT until its REGADDR field echoes the address,
 *          then take the data from its REGDATA field.
 *
 * Both forms complete a frame, which latches SIBSF0INT in INTRSTATUS1. This
 * sequence is taken from NetBSD txsibsf0_reg_read/txsibsf0_reg_write.
 *
 * The previous project called this bus "MBUS" and reverse-engineered the
 * 0x080/0x088 command/response layout by observation; it reached the same
 * bit assignments, which is a good cross-check on both.
 */
#include <stdio.h>
#include <string.h>
#include "soc/tx39/tx39.h"

/*
 * The sound transmit ring.
 *
 * The SIB plays from a buffer in DRAM whose base is in SIBSNDTXSTART and
 * whose size is the SND field of SIBSIZE. Magic Cap sets it up once during
 * boot — base 0x403ED638, which is the TX39's kuseg alias of DRAM bank 0, so
 * physical 0x003ED638, just below the framebuffer — enables ENSND in SIBCTRL
 * and ENSNDBUFF in SIBDMACTRL, and thereafter polls SIBDMACTRL hard: 266,000
 * reads during boot and 156,000 more sitting idle at the Desk. It was waiting
 * on a ring that nothing drained.
 *
 * How fast to drain it is the awkward part. The rate follows the SIB frame
 * sync, which divides down a clock this emulator does not model — see the
 * cycle-timing entry in docs/ACCURACY.md. So the rate below is an assumption,
 * marked as one. The sample count is derived from the CPU cycle counter
 * rather than from how often this function is called, so the pitch does not
 * depend on tick granularity.
 */
/*
 * ROM table at 0x83C210A8: 7200->39, 8000->35, 9600->29,
 * 19200->14, 22050->12, 24000->11, 11025->25 (SNDFSDIV).
 * With the assumed SIBCLK of 9.216 MHz, Fs=2*SIBCLK/(64*(div+1))
 * reduces to 36.864 MHz/(128*(div+1)). This supports the usual ROM
 * setting, but does not model SCLKDIV or the codec's independent divisor.
 * 128 here is NOT a verified SIB frame length. See docs/AUDIO.md.
 */
#define SIB_SND_FRAME_SCLKS 128u

uint32_t mrc_sib_snd_rate_hz(const tx39_sib *sib)
{
    unsigned div = (sib->ctrl >> SIBCTRL_SNDFSDIV_SHIFT) & SIBCTRL_SNDFSDIV_MASK;
    uint32_t hz = (uint32_t)(3686400ull * 10 / ((div + 1) * SIB_SND_FRAME_SCLKS));
    return hz ? hz : 11025u;
}

uint32_t mrc_sib_output_rate_hz(const tx39_sib *sib)
{
    return 2 * mrc_sib_snd_rate_hz(sib);
}

static uint32_t snd_ring_bytes(const tx39_sib *sib)
{
    /*
     * The size field counts 4-byte blocks. Magic Cap programs 1023, giving a
     * 4 KiB ring at 0x003ED638.
     *
     * The unit is measured, not assumed: at an idle Desk the first 4096 bytes
     * of that buffer hold exactly one value, -768, and nothing else — a ring
     * full of silence at the codec's DC offset. At 4097 bytes and beyond the
     * contents are varied heap. Two earlier guesses at this register were
     * wrong, a 2 KiB ring and then a 32 KiB one, and both were rationalised
     * with a plausible-sounding fit before anyone looked at the bytes.
     */
    uint32_t blocks = (sib->size >> SIBSIZE_SND_SHIFT) & SIBSIZE_MASK;
    return (blocks + 1) * 4u;
}

void mrc_sib_set_audio_sink(tx39_sib *sib, mrc_audio_sink fn, void *ctx)
{
    sib->audio_sink = fn;
    sib->audio_ctx = ctx;
}

void mrc_sib_pump_audio(tx39_sib *sib)
{
    tx39 *s = sib->soc;

    bool running = (sib->ctrl & SIBCTRL_ENSND) &&
                   (sib->dmactrl & SIBDMA_ENDMATXSND) &&
                   sib->sndtxstart != 0;

    if (!running) {
        sib->snd_running = false;
        sib->snd_cycle_ref = s->cpu->cycle_count;
        return;
    }
    if (!sib->snd_running) {
        sib->snd_running = true;
        sib->snd_cycle_ref = s->cpu->cycle_count;
        sib->snd_read_off = 0;
    }

    uint32_t bytes = snd_ring_bytes(sib);
    if (bytes < 2)
        return;

    /* The ring base is a kuseg DRAM alias, not a physical address. */
    uint32_t base = sib->sndtxstart;
    if (base >= TX39_KUSEG_DRAM_BANK0)
        base -= TX39_KUSEG_DRAM_BANK0;

    uint64_t elapsed = s->cpu->cycle_count - sib->snd_cycle_ref;
    uint32_t rate = mrc_sib_snd_rate_hz(sib);
    uint64_t owed = elapsed * rate / s->cpu_hz;
    if (owed == 0)
        return;
    /* Bound work per pump. Remaining elapsed time is still owed. */
    if (owed > rate / 4)
        owed = rate / 4;
    sib->snd_cycle_ref += owed * s->cpu_hz / rate;

    uint32_t half = bytes / 2;

    for (uint64_t i = 0; i < owed; i++) {
        bool ok;
        uint32_t v = mrc_bus_read(s->cpu->bus, base + sib->snd_read_off, 2, &ok);
        /* The codec runs even without a speaker attached. Host PCM is 2x
         * the DMA rate for reconstruction; guest DMA timing is unchanged. */
        int16_t output[2];
        mrc_ucb_audio_sample(&s->audio, sib->codec.reg[8], ok ? v : 0, output);
        if (sib->audio_sink) {
            sib->audio_sink(sib->audio_ctx, output[0]);
            sib->audio_sink(sib->audio_ctx, output[1]);
        }
        sib->snd_samples++;

        uint32_t prev = sib->snd_read_off;
        sib->snd_read_off += 2;

        /*
         * Tell software how far the play pointer has got. It refills the half
         * that has just been consumed, and without these it never refills at
         * all — the ring simply repeats, which is exactly what the capture
         * showed before this was added: periodic at 16384 samples, forever.
         */
        if (prev < half && sib->snd_read_off >= half)
            mrc_icu_raise(&s->icu, 1, INT1_SND0_5INT);
        if (sib->snd_read_off >= bytes) {
            sib->snd_read_off = 0;
            mrc_icu_raise(&s->icu, 1, INT1_SND1_0INT | INT1_SNDDMACNTINT);
        }
    }
}

static void sf0_transact(tx39_sib *sib, uint32_t cmd)
{
    unsigned reg   = (cmd >> SIBSF_REGADDR_SHIFT) & SIBSF_REGADDR_MASK;
    uint16_t data  = (uint16_t)((cmd >> SIBSF_REGDATA_SHIFT) & SIBSF_REGDATA_MASK);
    bool     write = (cmd & SIBSF0_WRITE) != 0;

    static const char *const rname[16] = {
        "IO_DATA", "IO_DIR", "IE_RIS", "IE_FAL", "IE_STATUS", "TC_A", "TC_B",
        "AC_A", "AC_B", "TS_CR", "ADC_CR", "ADC_DATA", "ID", "MODE",
        "reg14", "reg15",
    };
    if (sib->log_codec) {
        sib->log_codec--;
        fprintf(sib->soc->log, "[codec] %s %-9s %04X @%08X ra=%08X\n",
                write ? "W" : "R", rname[reg & 0xF],
                write ? data : mrc_ucb_read(&sib->codec, reg),
                sib->soc->pc_hint ? *sib->soc->pc_hint : 0,
                sib->soc->cpu ? sib->soc->cpu->r[31] : 0);
    }

    if (write) {
        sib->codec_writes[reg & 0xF]++;
        mrc_ucb_write(&sib->codec, reg, data);
        /* Power-off may happen after DMA has stopped, so reset the filter
         * here too; waiting for a disabled sample could retain stale audio. */
        if (reg == 8 && !(data & UCB_AUDIO_OUT_ENA))
            memset(&sib->soc->audio, 0, sizeof(sib->soc->audio));
        /* The status register echoes the transaction that just completed. */
        sib->sf0stat = ((uint32_t)reg << SIBSF_REGADDR_SHIFT) | SIBSF0_WRITE |
                       data;
    } else {
        sib->codec_reads[reg & 0xF]++;
        uint16_t v = mrc_ucb_read(&sib->codec, reg);
        sib->sf0stat = ((uint32_t)reg << SIBSF_REGADDR_SHIFT) | v;
    }

    /*
     * The result becomes available at the next frame boundary, not here —
     * see mrc_sib_update() for why that distinction matters.
     */
}

/*
 * The SIB is a continuously clocked frame bus: once enabled it runs a frame
 * every sample period whether or not software has anything to say, and each
 * subframe latches its completion source as it goes by.
 *
 * So SIBSF0INT and SIBSF1INT are periodic, NOT raised by the register write
 * that starts a transfer. The ROM's codec-read sequence at 0x83C046F0 makes
 * the difference visible:
 *
 *     SIBCTRL   |= ENSIB
 *     SIBSF0CTRL = regaddr << 27        (start the read)
 *     INTRCLEAR1 = SIBSF0INT            (clear it AFTER starting)
 *     poll INTRSTATUS1 for SIBSF0INT    (wait for the frame)
 *     INTRCLEAR1 = SIBSF1INT
 *     poll INTRSTATUS1 for SIBSF1INT
 *
 * Raising the source inside the SIBSF0CTRL write puts it up before the clear
 * wipes it, and the wait never ends — the monitor's `dbr` command hangs.
 *
 * We have no frame-rate model, so a frame completes on every machine tick.
 * That is faster than the real 
 * bus and is recorded in docs/OPEN_QUESTIONS.md.
 */
void mrc_sib_update(tx39_sib *sib)
{
    uint32_t bits = 0;

    /*
     * The codec's interrupt is a pin, not a bus transaction, so it reaches
     * the ICU whether or not the SIB is being clocked. The OS depends on
     * that: once sound has finished it power-cycles the bus, and while the
     * bus is off it leaves exactly one interrupt enabled in bank 1 —
     * SIBIRQPOSINT, enable == 0x00000040 — and waits to be woken by a touch.
     *
     * This edge check used to sit below an "if ENSIB is clear, return", so
     * every tap made while the bus was down was discarded. That is most of
     * the time once a sound has played, and it made the machine look dead
     * to input a few seconds after any audio.
     */
    bool codec_irq = sib->codec.irq_out;
    if (codec_irq != sib->irq_seen) {
        bits |= codec_irq ? INT1_SIBIRQPOSINT : INT1_SIBIRQNEGINT;
        sib->irq_seen = codec_irq;
    }

    if (!(sib->ctrl & SIBCTRL_ENSIB)) {
        if (bits)
            mrc_icu_raise(&sib->soc->icu, 1, bits);
        return;
    }
    if (sib->ctrl & SIBCTRL_ENSF0)
        bits |= INT1_SIBSF0INT;
    if (sib->ctrl & SIBCTRL_ENSF1)
        bits |= INT1_SIBSF1INT;

    /*
     * The ROM enables the bus but not the per-subframe enables before doing
     * a codec register access, so gating purely on ENSF0/ENSF1 would stall
     * it. The subframes are what the bus is made of; they go by whenever it
     * is running.
     */
    bits |= INT1_SIBSF0INT | INT1_SIBSF1INT;

    /*
     * The audio and telecom channels carry a sample in every frame while
     * they are enabled, so their input sources are periodic in exactly the
     * same way. Magic Cap's startup depends on this: the routine at
     * 0x83C231B4 waits on SNDININT and does not come back without it.
     *
     * We deliver the interrupt but no audio: SIBSNDHOLD returns whatever was
     * last written to it, and the DMA ring is not driven. That is enough for
     * software waiting on the channel to run, and it is silence rather than
     * invented sound.
     */
    if (sib->ctrl & SIBCTRL_ENSND)
        bits |= INT1_SNDININT;
    if (sib->ctrl & SIBCTRL_ENTEL)
        bits |= INT1_TELININT;

    /*
     * The codec has an IRQ pin of its own, which the SIB presents to the
     * host two ways: as a level in SIBCTRL.SIBIRQ, and as edges in
     * INTRSTATUS1's SIBIRQPOSINT / SIBIRQNEGINT. This is how a touch reaches
     * the OS.
     */
    mrc_icu_raise(&sib->soc->icu, 1, bits);
}

uint32_t mrc_sib_read(tx39_sib *sib, uint32_t off, bool *decoded)
{
    *decoded = true;

    if (off == TX39_SIBCTRL) {
        /* SIBIRQ reports the codec's IRQ pin level. */
        return sib->codec.irq_out ? (sib->ctrl | SIBCTRL_SIBIRQ)
                                  : (sib->ctrl & ~SIBCTRL_SIBIRQ);
    }

    switch (off) {
    case TX39_SIBSIZE:       return sib->size;
    case TX39_SIBSNDRXSTART: return sib->sndrxstart;
    case TX39_SIBSNDTXSTART: return sib->sndtxstart;
    case TX39_SIBTELRXSTART: return sib->telrxstart;
    case TX39_SIBTELTXSTART: return sib->teltxstart;
    case TX39_SIBSNDHOLD:    return sib->sndhold;
    case TX39_SIBTELHOLD:    return sib->telhold;
    case TX39_SIBSF0CTRL:    return sib->sf0ctrl;
    case TX39_SIBSF1CTRL:    return sib->sf1ctrl;
    case TX39_SIBSF0STAT:    return sib->sf0stat;
    case TX39_SIBSF1STAT:    return sib->sf1stat;
    case TX39_SIBDMACTRL: {
        /*
         * Report where the play pointer has got to. The field counts the same
         * 4-byte blocks SIBSIZE does. Without this software sees a pointer
         * that never moves and never refills the ring.
         */
        uint32_t ptr = (sib->snd_read_off / 4u) & SIBDMA_SNDDMAPTR_MASK;
        return (sib->dmactrl & ~(SIBDMA_SNDDMAPTR_MASK << SIBDMA_SNDDMAPTR_SHIFT))
               | (ptr << SIBDMA_SNDDMAPTR_SHIFT);
    }
    default:
        *decoded = false;
        return 0;
    }
}

bool mrc_sib_write(tx39_sib *sib, uint32_t off, uint32_t val)
{
    switch (off) {
    case TX39_SIBSIZE:       sib->size = val; return true;
    case TX39_SIBSNDRXSTART: sib->sndrxstart = val; return true;
    case TX39_SIBSNDTXSTART: sib->sndtxstart = val; return true;
    case TX39_SIBTELRXSTART: sib->telrxstart = val; return true;
    case TX39_SIBTELTXSTART: sib->teltxstart = val; return true;
    case TX39_SIBCTRL:       sib->ctrl = val & ~SIBCTRL_SIBIRQ; return true;
    case TX39_SIBSNDHOLD:    sib->sndhold = val; return true;
    case TX39_SIBTELHOLD:    sib->telhold = val; return true;
    case TX39_SIBSF1CTRL:    sib->sf1ctrl = val; return true;
    case TX39_SIBDMACTRL:
        /* The pointer field is ours to report, not software's to set. */
        sib->dmactrl = val & ~(SIBDMA_SNDDMAPTR_MASK << SIBDMA_SNDDMAPTR_SHIFT);
        return true;

    case TX39_SIBSF0CTRL:
        sib->sf0ctrl = val;
        sf0_transact(sib, val);
        return true;

    case TX39_SIBSF0STAT:
    case TX39_SIBSF1STAT:
        return true;    /* read-only */

    default:
        return false;
    }
}

void mrc_sib_print_codec_traffic(const tx39_sib *sib, FILE *f)
{
    static const char *const name[16] = {
        "IO_DATA", "IO_DIR", "IE_RIS", "IE_FAL", "IE_STATUS", "TC_A", "TC_B",
        "AC_A", "AC_B", "TS_CR", "ADC_CR", "ADC_DATA", "ID", "MODE",
        "reg14", "reg15",
    };
    bool any = false;
    for (unsigned i = 0; i < 16; i++)
        if (sib->codec_reads[i] || sib->codec_writes[i])
            any = true;
    if (!any) {
        fprintf(f, "codec: software made no register accesses\n");
        return;
    }
    fprintf(f, "codec register traffic:\n");
    for (unsigned i = 0; i < 16; i++) {
        if (!sib->codec_reads[i] && !sib->codec_writes[i])
            continue;
        fprintf(f, "  %2u %-10s %10llu reads %10llu writes  now=%04X\n", i,
                name[i], (unsigned long long)sib->codec_reads[i],
                (unsigned long long)sib->codec_writes[i], sib->codec.reg[i]);
    }
    fprintf(f, "  pen: %s at raw (%u,%u); codec IRQ %s\n",
            sib->codec.pen_down ? "down" : "up", sib->codec.pen_x,
            sib->codec.pen_y, sib->codec.irq_out ? "asserted" : "clear");
}
