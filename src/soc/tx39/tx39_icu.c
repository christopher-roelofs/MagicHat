/*
 * tx39_icu.c — TX39 interrupt controller.
 *
 * Six status banks, five of which are also write-to-clear, and six enable
 * banks. Banks 1..5 feed CPU interrupt line IP2; bank 6 (IRQHIGH/IRQLOW)
 * feeds IP4. That split is taken from NetBSD tx39icu.c, which switches on
 * MIPS_INT_MASK_2 and MIPS_INT_MASK_4.
 */
#include "soc/tx39/tx39.h"

void mh_icu_update(tx39_icu *icu)
{
    tx39 *s = icu->soc;
    bool normal = false, high = false;

    for (unsigned b = 0; b < 5; b++)
        if (icu->status[b] & icu->enable[b])
            normal = true;
    if (icu->status[5] & icu->enable[5])
        high = true;

    mh_cpu_set_irq(s->cpu, TX39_IP_NORMAL, normal);
    mh_cpu_set_irq(s->cpu, TX39_IP_HIGH, high);
}

bool mh_icu_pending(const tx39_icu *icu)
{
    for (unsigned b = 0; b < TX39_ICU_NBANK; b++)
        if (icu->status[b] & icu->enable[b])
            return true;
    return false;
}

void mh_icu_raise(tx39_icu *icu, unsigned bank, uint32_t bits)
{
    if (bank < 1 || bank > TX39_ICU_NBANK)
        return;

    uint32_t *st = &icu->status[bank - 1];
    if ((*st & bits) == bits)
        return;     /* already latched; nothing changes */

    *st |= bits;

    mh_icu_update(icu);
}

uint32_t mh_icu_read(tx39_icu *icu, uint32_t off, bool *decoded)
{
    *decoded = true;

    if (off == 0x114) {
        /*
         * INTRSTATUS6 is a summary, not a sixth source: read-only, carrying
         * IRQHIGH/IRQLOW and a 4-bit INTVECT, with the split configured by
         * INTRENABLE6's PRIORITYMASK.
         *
         * The OS idle routine at ROM 0x83C3B28C polls exactly this to decide
         * whether to return or sleep again (0x83C3B410: lw 0x114, and
         * IRQHIGH|IRQLOW, beqz -> sleep). Reporting a constant zero meant it
         * never returned at all: 0x83C3B428, the exit, took 0 hits in 200M
         * instructions while the sleep entry took 4263.
         *
         * IRQHIGH and INTVECT stay clear: both need the per-source priority
         * table behind PRIORITYMASK, which has not been decoded.
         */
        uint32_t v = icu->status[5];
        if (mh_icu_pending(icu))
            v |= INT6_IRQLOW;
        return v;
    }
    if (off >= 0x100 && off <= 0x114 && ((off - 0x100) % 4) == 0)
        return icu->status[(off - 0x100) / 4];          /* INTRSTATUS1..6 */
    if (off >= 0x118 && off <= 0x12C && ((off - 0x118) % 4) == 0)
        return icu->enable[(off - 0x118) / 4];          /* INTRENABLE1..6 */

    /* 0x130..0x13C are TX392X-only banks 7/8; this part does not have them. */
    *decoded = false;
    return 0;
}

bool mh_icu_write(tx39_icu *icu, uint32_t off, uint32_t val)
{
    if (off >= 0x100 && off <= 0x110 && ((off - 0x100) % 4) == 0) {
        icu->status[(off - 0x100) / 4] &= ~val;         /* INTRCLEAR1..5 */
        mh_icu_update(icu);
        return true;
    }
    if (off == 0x114)
        return true;   /* INTRSTATUS6 is read-only; the write is a no-op */

    if (off >= 0x118 && off <= 0x12C && ((off - 0x118) % 4) == 0) {
        icu->enable[(off - 0x118) / 4] = val;
        mh_icu_update(icu);
        return true;
    }
    return false;
}
