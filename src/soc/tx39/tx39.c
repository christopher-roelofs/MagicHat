#include "soc/tx39/tx39.h"

#include <string.h>

void mh_tx39_init(tx39 *s, r3900 *cpu, uint32_t cpu_hz)
{
    memset(s, 0, sizeof(*s));
    s->cpu    = cpu;
    s->cpu_hz = cpu_hz;
    s->log    = stderr;

    s->icu.soc   = s;
    s->sib.soc   = s;
    s->video.soc = s;
    s->timer.soc = s;
    s->mbus.soc  = s;
    s->power.soc = s;

    for (unsigned i = 0; i < 2; i++) {
        s->uart[i].soc   = s;
        s->uart[i].index = i;
    }
    s->uart[0].console = true;   /* UART A is the IDT monitor console */

    mh_ucb_init(&s->sib.codec);
}

static void unknown(tx39 *s, const char *dir, uint32_t off, uint32_t val)
{
    if (s->log_unknown)
        fprintf(s->log, "[tx39] %s to undecoded register +%03X = %08X\n",
                dir, off, val);
}

uint32_t mh_tx39_read(void *ctx, uint32_t off, unsigned size)
{
    tx39 *s = ctx;
    bool decoded = true;
    uint32_t v = 0;

    if (off / 4 < TX39_CFG_SIZE / 4) {
        s->hist_read[off / 4]++;
        if (s->pc_hint)
            s->hist_pc[off / 4] = *s->pc_hint;
    }

    /*
     * The register block is word-oriented. Sub-word reads are not something
     * the ROM should be doing; note them rather than silently masking.
     */
    if (size != 4 && s->log_unknown)
        fprintf(s->log, "[tx39] %u-bit read of +%03X\n", size * 8, off);

    if (off <= TX39_MEMCONFIG8)
        v = s->memconfig[off / 4];
    else if (off >= TX39_VIDEOCTRL1 && off <= TX39_VIDEOCTRL14)
        v = mh_video_read(&s->video, off, &decoded);
    else if (off >= TX39_SIBSIZE && off <= TX39_SIBDMACTRL)
        v = mh_sib_read(&s->sib, off, &decoded);
    else if (off >= TX39_UARTA_BASE && off < TX39_UARTB_BASE)
        v = mh_uart_read(&s->uart[0], off - TX39_UARTA_BASE, &decoded);
    else if (off >= TX39_UARTB_BASE && off <= TX39_UARTB_BASE + 0x14)
        v = mh_uart_read(&s->uart[1], off - TX39_UARTB_BASE, &decoded);
    else if (off >= TX39_MBUS_FIRST && off <= TX39_MBUS_LAST)
        v = mh_mbus_read(&s->mbus, off, &decoded);
    else if (off >= 0x100 && off <= 0x13C)
        v = mh_icu_read(&s->icu, off, &decoded);
    else if (off >= TX39_TIMERRTCHI && off <= TX39_TIMERPERIODIC)
        v = mh_timer_read(&s->timer, off, &decoded);
    else switch (off) {
    case TX39_IRCTRL1:        v = s->irctrl1; break;
    case TX39_IRCTRL2:        v = s->irctrl2; break;
    case TX39_SPICTRL:        v = s->spictrl; break;
    case TX39_SPIHOLD:        v = s->spihold; break;
    case TX39_IOCTRL:         v = s->io_ctrl; break;
    case TX39_IOMFIODATAOUT:  v = s->io_dataout; break;
    case TX39_IOMFIODATADIR:  v = s->io_datadir; break;
    case TX39_IOMFIODATAIN:
        /* Pin 13 is wired to the touch panel's pen-down sense. */
        v = s->io_datain & ~MFIO_PEN_DOWN;
        if (s->sib.codec.pen_down)
            v |= MFIO_PEN_DOWN;
        break;
    case TX39_IOMFIODATASEL:  v = s->io_datasel; break;
    case TX39_IOIOPOWERDWN:   v = s->io_powerdwn; break;
    case TX39_IOMFIOPOWERDWN: v = s->io_mfiopowerdwn; break;
    case TX39_CLOCKCTRL:      v = s->clockctrl; break;
    default:
        if (off >= TX39_EXT_FIRST && off <= TX39_EXT_LAST) {
            v = s->ext[(off - TX39_EXT_FIRST) / 4];
            s->ext_accesses++;
        } else {
            decoded = false;
        }
        break;
    case TX39_POWERCTRL:      v = mh_power_read(&s->power, off, &decoded); break;
    }

    if (!decoded) {
        s->unknown_reads++;
        unknown(s, "read", off, 0);
    }
    return v;
}

void mh_tx39_write(void *ctx, uint32_t off, unsigned size, uint32_t val)
{
    tx39 *s = ctx;
    bool decoded = true;

    if (off / 4 < TX39_CFG_SIZE / 4) {
        s->hist_write[off / 4]++;
        if (s->pc_hint)
            s->hist_pc[off / 4] = *s->pc_hint;
    }

    if (size != 4 && s->log_unknown)
        fprintf(s->log, "[tx39] %u-bit write of +%03X\n", size * 8, off);

    if (off <= TX39_MEMCONFIG8)
        s->memconfig[off / 4] = val;
    else if (off >= TX39_VIDEOCTRL1 && off <= TX39_VIDEOCTRL14)
        decoded = mh_video_write(&s->video, off, val);
    else if (off >= TX39_SIBSIZE && off <= TX39_SIBDMACTRL)
        decoded = mh_sib_write(&s->sib, off, val);
    else if (off >= TX39_UARTA_BASE && off < TX39_UARTB_BASE)
        decoded = mh_uart_write(&s->uart[0], off - TX39_UARTA_BASE, val);
    else if (off >= TX39_UARTB_BASE && off <= TX39_UARTB_BASE + 0x14)
        decoded = mh_uart_write(&s->uart[1], off - TX39_UARTB_BASE, val);
    else if (off >= TX39_MBUS_FIRST && off <= TX39_MBUS_LAST)
        decoded = mh_mbus_write(&s->mbus, off, val);
    else if (off >= 0x100 && off <= 0x13C)
        decoded = mh_icu_write(&s->icu, off, val);
    else if (off >= TX39_TIMERRTCHI && off <= TX39_TIMERPERIODIC)
        decoded = mh_timer_write(&s->timer, off, val);
    else switch (off) {
    case TX39_IRCTRL1:        s->irctrl1 = val; break;
    case TX39_IRCTRL2:        s->irctrl2 = val; break;
    case TX39_IRTXHOLD:       s->irtxhold = val; break;
    case TX39_SPICTRL:        s->spictrl = val; break;
    case TX39_SPIHOLD:        s->spihold = val; break;
    case TX39_IOCTRL:
        /* IODIN is driven by external pins, not the CPU's output latch. */
        s->io_ctrl = (val & ~IOCTRL_IODIN_MASK) | (s->io_ctrl & IOCTRL_IODIN_MASK);
        break;
    case TX39_IOMFIODATAOUT:  s->io_dataout = val; break;
    case TX39_IOMFIODATADIR:  s->io_datadir = val; break;
    case TX39_IOMFIODATASEL:  s->io_datasel = val; break;
    case TX39_IOIOPOWERDWN:   s->io_powerdwn = val; break;
    case TX39_IOMFIOPOWERDWN: s->io_mfiopowerdwn = val; break;
    case TX39_CLOCKCTRL:      s->clockctrl = val; break;
    default:
        if (off >= TX39_EXT_FIRST && off <= TX39_EXT_LAST) {
            s->ext[(off - TX39_EXT_FIRST) / 4] = val;
            s->ext_accesses++;
        } else {
            decoded = false;
        }
        break;
    case TX39_POWERCTRL:      decoded = mh_power_write(&s->power, off, val); break;
    }

    if (!decoded) {
        s->unknown_writes++;
        unknown(s, "write", off, val);
    }
}

void mh_tx39_tick(tx39 *s)
{
    mh_timer_tick(&s->timer);
    /* Advance serial transmission; completed transfers latch interrupts. */
    mh_uart_update(&s->uart[0]);
    mh_uart_update(&s->uart[1]);
    mh_mbus_update(&s->mbus);
    mh_sib_update(&s->sib);
    mh_sib_pump_audio(&s->sib);
    mh_power_tick(&s->power);
}

/* Names for the registers a histogram is likely to show. */
static const char *reg_name(uint32_t off)
{
    switch (off) {
    case TX39_SIBCTRL:      return "SIBCTRL";
    case TX39_SIBSF0CTRL:   return "SIBSF0CTRL";
    case TX39_SIBSF0STAT:   return "SIBSF0STAT";
    case TX39_SIBSF1STAT:   return "SIBSF1STAT";
    case TX39_UARTA_BASE:   return "UARTACTRL1";
    case TX39_UARTA_BASE + TX39_UART_RXHOLD: return "UARTA_RX/TXHOLD";
    case TX39_UARTB_BASE:   return "UARTBCTRL1";
    case TX39_TIMERRTCLO:   return "TIMERRTCLO";
    case TX39_TIMERRTCHI:   return "TIMERRTCHI";
    case TX39_IOMFIODATAIN: return "MFIODATAIN";
    case TX39_IOMFIODATAOUT:return "MFIODATAOUT";
    case TX39_MBUSCTRL:     return "MBUSCTRL";
    case TX39_MBUSCTRL2:    return "MBUSCTRL2";
    case TX39_MBUSDMASTART: return "MBUSDMASTART";
    case TX39_MBUSDMALENGTH:return "MBUSDMALENGTH";
    case TX39_MBUSDMACOUNT: return "MBUSDMACOUNT";
    case TX39_MBUSCOMMAND: return "MBUSCOMMAND";
    case TX39_MBUSPAYLOAD: return "MBUSPAYLOAD";
    case TX39_CLOCKCTRL:    return "CLOCKCTRL";
    case TX39_POWERCTRL:    return "POWERCTRL";
    default: break;
    }
    if (off >= 0x100 && off <= 0x114) {
        static const char *n[] = { "INTRSTATUS1", "INTRSTATUS2", "INTRSTATUS3",
                                   "INTRSTATUS4", "INTRSTATUS5", "INTRSTATUS6" };
        return n[(off - 0x100) / 4];
    }
    if (off >= 0x118 && off <= 0x12C) {
        static const char *n[] = { "INTRENABLE1", "INTRENABLE2", "INTRENABLE3",
                                   "INTRENABLE4", "INTRENABLE5", "INTRENABLE6" };
        return n[(off - 0x118) / 4];
    }
    if (off >= TX39_VIDEOCTRL1 && off <= TX39_VIDEOCTRL14)
        return "VIDEOCTRLn";
    return "";
}

void mh_tx39_print_histogram(const tx39 *s, FILE *f, unsigned top)
{
    typedef struct { uint32_t off; uint64_t r, w; uint32_t pc; } row;
    row rows[TX39_CFG_SIZE / 4];
    unsigned n = 0;

    for (uint32_t i = 0; i < TX39_CFG_SIZE / 4; i++)
        if (s->hist_read[i] || s->hist_write[i])
            rows[n++] = (row){ i * 4, s->hist_read[i], s->hist_write[i],
                               s->hist_pc[i] };

    for (unsigned i = 0; i < n; i++)
        for (unsigned j = i + 1; j < n; j++)
            if (rows[j].r + rows[j].w > rows[i].r + rows[i].w) {
                row t = rows[i]; rows[i] = rows[j]; rows[j] = t;
            }

    fprintf(f, "TX39 register traffic (top %u of %u touched):\n",
            n < top ? n : top, n);
    for (unsigned i = 0; i < n && i < top; i++)
        fprintf(f, "  +%03X %-16s %12llu reads %10llu writes  last@%08X\n",
                rows[i].off, reg_name(rows[i].off),
                (unsigned long long)rows[i].r, (unsigned long long)rows[i].w,
                rows[i].pc);
}
