#include "machines/pic2000/registers.h"
uint32_t pic2000_probe_read(void *ctx, uint32_t off, unsigned size)
{
    auto *p = (Pic2000Registers *)ctx;
    uint32_t v = 0;
    /* Sampled once for the whole access, so a longword read cannot tear --
     * which is the property the ROM's double-read loop is checking for. */
    const uint32_t count = p->insn_src ? p->counter_now() : 0;
    for (unsigned i = 0; i < size; i++) {
        unsigned o = (off + i) & 0xFFF;
        p->reads[o / 2]++;
        uint8_t b;
        if (p->in_counter(o))
            b = (uint8_t)(count >> (8 * (3 - (o - p->counter_off))));
        else if (p->in_adc(o))
            b = (uint8_t)(p->adc_latched >> (8 * (1 - (o - p->adc_off))));
        else
            b = (uint8_t)(p->reg[o / 2] >> ((o & 1) ? 0 : 8));
        if (p->magicbus_empty_input && o == p->magicbus_empty_offset) b |= 0x04;
        if (p->adapter_input && o == 0xD1)
            b = uint8_t((b & ~0x40u) | (p->adapter_attached ? 0x40u : 0));
        if (p->econoram && o == 0xE7) {
            b &= uint8_t(~8u);
            if (p->econoram->line(p->cpu_clock(p->clock_context), p->clock_hz)) b |= 8;
        } else if (p->serial_pullup && o == 0xE7) {
            b &= uint8_t(~8u);
            if (!(p->reg[0xE6 / 2] & 0x0800)) b |= 8;
        }
        v = (v << 8) | b;
    }
    if (p->trace_conv && p->adc_off && off == p->adc_off && p->conv_seen < 96) {
        p->conv_seen++;
        fprintf(p->log, "[%2u] +%llu READ  result %04X (>>6 = %4u)   "
                "ctl(+E0) %04X mux(+E2) %04X\n", p->conv_seen,
                (unsigned long long)*p->insn_src, v, v >> 6,
                p->reg[0xE0 / 2], p->reg[0xE2 / 2]);
    }
    if (p->logged < p->log_max) {
        p->logged++;
        fprintf(p->log, "[%s] R%u +%03X = %0*X\n", p->name, size * 8,
                off, (int)size * 2, v);
    }
    return v;
}

void pic2000_probe_write(void *ctx, uint32_t off, unsigned size, uint32_t val)
{
    auto *p = (Pic2000Registers *)ctx;
    if (p->power_control && off == 0xD0 && size == 2) {
        p->audio_dirty = true;
        p->writes[0xD0 / 2] += 2;
        if (!val) p->power_off = true;
        return; /* Output writes must not overwrite physical input levels. */
    }
    if (off < 0x58 && off + size > 0x4E) p->audio_dirty = true;
    if (off < 0xC6 && off + size > 0xB0) p->irq_dirty = true;
    for (unsigned i = 0; i < size; i++) {
        unsigned o = (off + size - 1 - i) & 0xFFF;
        p->writes[o / 2]++;
        uint8_t b = (uint8_t)(val >> (i * 8));
        uint16_t &w = p->reg[o / 2];
        if (p->is_w1c(o)) {
            /* A one bit clears; a zero bit leaves the bit alone. */
            w &= (uint16_t)~(uint16_t)(o & 1 ? b : (b << 8));
        } else if (o & 1) {
            w = (uint16_t)((w & 0xFF00) | b);
        } else {
            w = (uint16_t)((w & 0x00FF) | (b << 8));
        }
    }
    if (p->econoram && off < 0xE8 && off + size > 0xE6)
        p->econoram->drive((p->reg[0xE6 / 2] & 0x0800) != 0,
                          p->cpu_clock(p->clock_context), p->clock_hz);
    if (p->compare_off && off < p->compare_off + 4 &&
        off + size > p->compare_off)
        p->compare_dirty = true;
    if (p->adc_off && off < p->adc_off + 2 && off + size > p->adc_off) {
        p->adc_dirty = true;
        {
            const int mx = p->reg[0xE2 / 2] & 0x7F;
            const int ch = (int)((val & 0xFFFF) >> 6);
            int v = -1;
            for (unsigned k = 0; k < p->adc_pairs; k++)
                if (p->adc_pair[k].mux == mx && p->adc_pair[k].chan == ch) {
                    v = p->adc_pair[k].value; break;
                }
            /* The PIC pen driver's initial acquisition ends with two
             * consecutive mux 0x69/channel 0 conversions. The first is the
             * X sample left by its pressure sequence; the second, started
             * at 0x0E08B95A, is the Y sample. The physical digitizer changes
             * what it presents between those starts even though the two
             * converter selectors are identical. */
            if (mx == 0x69 && ch == 0 && p->pen_second_690 >= 0 &&
                p->pen_690_starts++ == 1)
                v = p->pen_second_690;
            if (mx != 0x69 || ch != 0)
                p->pen_690_starts = 0;
            if (v < 0) v = p->adc_chan[ch];
            if (v < 0) v = p->adc_value;
            p->adc_latched = v < 0 ? 0u : (uint32_t)v;
        }
        if (p->trace_conv && p->conv_seen < 96) {
            p->conv_seen++;
            fprintf(p->log, "[%2u] +%llu START channel %04X (ch %4u)      "
                    "ctl(+E0) %04X mux(+E2) %04X\n", p->conv_seen,
                    (unsigned long long)*p->insn_src, (unsigned)(val & 0xFFFF),
                    (unsigned)(val & 0xFFFF) >> 6, p->reg[0xE0 / 2],
                    p->reg[0xE2 / 2]);
        }
        if (!p->adc_warned) {
            p->adc_warned = true;
            if (p->adc_value < 0)
                fprintf(p->log, "68k: %s +%03X started a conversion (channel "
                        "%u). The converter's protocol is modelled; the value "
                        "it would measure is NOT; unassigned channels return zero.\n",
                        p->name, p->adc_off, (unsigned)(val & 0xFFFF) >> 6);
            else
                fprintf(p->log, "68k: DEVIATION: %s +%03X returns %d for every "
                        "channel; no modelled sensor produced it\n",
                        p->name, p->adc_off, p->adc_value);
        }
    }
    if (p->adc_off && off < 0x96 && off + size > 0x90 &&
        ((off <= 0x90 && off + size > 0x90) ||
         (off <= 0x94 && off + size > 0x94)))
        p->tx_dirty = true;
    if (p->adc_off && off < 0xE0 && off + size > 0xDE)
        p->pen_timer_dirty = true;
    if (p->logged < p->log_max) {
        p->logged++;
        fprintf(p->log, "[%s] W%u +%03X = %0*X\n", p->name, size * 8,
                off, (int)size * 2, val);
    }
}

void pic2000_probe_report(const Pic2000Registers *p, FILE *log)
{
    bool any = false;
    for (unsigned i = 0; i < 0x800; i++) {
        if (!p->reads[i] && !p->writes[i]) continue;
        if (!any) {
            fprintf(log, "%s at %08X: registers touched\n", p->name, p->base);
            any = true;
        }
        fprintf(log, "  +%03X  %6llu reads  %6llu writes  last %04X\n",
                i * 2, (unsigned long long)p->reads[i],
                (unsigned long long)p->writes[i], p->reg[i]);
    }
    if (!any) fprintf(log, "%s at %08X: never touched%s\n", p->name, p->base,
                      p->preset ? " (despite a preset)" : "");
    else if (p->preset)
        fprintf(log, "  (a register of this device was preset; see the "
                "DEVIATION line above)\n");
}
