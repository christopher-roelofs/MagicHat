#include "machines/pic2000/audio.h"
#include <cmath>

unsigned Pic2000Audio::rate(uint16_t reg)
{
    /* ROM 0E0A9FBE..0E0AA024 selects these encodings for the SDK's
     * 16.16 rates. No invented oscillator/divisor formula for other values. */
    switch (reg) {
    case 0x0d20: return 7350;
    case 0x0c30: return 8820;
    case 0x0b40: return 11025;
    case 0x0a50: return 14700;
    case 0x0950: return 17640;
    case 0x0960: return 22050;
    default: return 0;
    }
}

unsigned Pic2000Audio::divider_rate(uint16_t reg)
{
    // Hypothesis, not a recovered register specification: two 4-bit
    // counter endpoints, inclusive descending interval modulo 16.
    // All six ROM rate selections fit an 88,200-Hz input. See HIX300.md.
    if (reg & 0xf00f) return 0;
    unsigned ticks = (((reg >> 8) & 15) - ((reg >> 4) & 15) + 1) & 15;
    if (ticks == 1) return 0; // above the current 44.1-kHz sampling engine
    return 88200 / (ticks ? ticks : 16);
}

void Pic2000Audio::tick(uint64_t now, unsigned clock, Pic2000Registers &r, mrc_bus &bus)
{
    if (now < cycles) { cycles = now; phase = 0; }
    phase += (now - cycles) * output_rate;
    cycles = now;
    unsigned hz = rate(r.reg[0x52 / 2]);
    if (!hz && experimental_divider) hz = divider_rate(r.reg[0x52 / 2]);
    /* 0E0AABA4 uses bit 4 for the separate recording configuration. */
    bool on = !r.power_off && (r.reg[0x4e / 2] & 1) &&
              !(r.reg[0x4e / 2] & 0x10) && hz;
    if (on != running) {
        running = on; offset = 0; held = 0;
        source_phase = on ? output_rate : 0;
        filter1 = filter2 = 0;
    }
    if (approximate_output && hz != filter_rate) {
        filter_rate = hz;
        // Unverified presentation approximation, NOT a recovered PIC circuit.
        // Two cascaded RC poles at 0.4 * source rate, sampled at 44.1 kHz.
        filter_alpha = 1 - std::exp(-2 * 3.141592653589793 * 0.4 * hz / output_rate);
    }
    while (phase >= clock) {
        phase -= clock;
        if (running) {
            if (source_phase >= output_rate) {
                source_phase -= output_rate;
                if (sample_ready_irq) r.set_bit32(0xb0, 20);
                /* Envoy's direct serial/sample access uses 1001; PIC's
                 * ring playback uses 1007. Direct mode must not read RAM
                 * or manufacture DMA refill events. */
                if ((r.reg[0x4e / 2] & 6) == 6) {
                    bool ok;
                    uint32_t address = ((uint32_t)r.reg[0x50 / 2] << 12) + offset;
                    held = (int16_t)mrc_bus_read(&bus, address, 2, &ok);
                    if (!ok) held = 0;
                    samples++;
                    offset = (offset + 2) & 0xfff;
                    /* The ROM mixer refills 1024 words per call at 0E0ADCAE.
                     * Half/full sources are cleared together at 0E0AA13E. */
                    if (offset == 0x800 || offset == 0) {
                        r.set_bit32(0xb0, offset ? 18 : 19);
                        half_irqs++;
                    }
                }
            }
            source_phase += hz;
        }
        /* ROM 0E0AA0BC sets bit 10 before disabling the speaker. Keep the
         * observed zero-volume endpoint even in raw PCM comparison mode. */
        bool mute = !running || (r.reg[0x54 / 2] & 0x400) ||
                    (r.reg[0x56 / 2] & 0xff0) == 0xff0;
        double output = held;
        if (approximate_output && running) {
            filter1 += filter_alpha * (output - filter1);
            filter2 += filter_alpha * (filter1 - filter2);
            // ROM proves reversed 4-bit volume codes, not the gain curve or
            // channel routing. Assume linear amplitude and average the fields
            // for mono. See docs/PIC2000_AUDIO.md; the hardware curve is unverified.
            unsigned volume = r.reg[0x56 / 2];
            double gain = (30 - ((volume >> 8) & 15) - ((volume >> 4) & 15)) / 30.0;
            output = filter2 * gain;
        }
        if (sink) sink(ctx, mute ? 0 : static_cast<int16_t>(std::lround(output)));
    }
}
