#include "devices/glacier/glacier.h"

#include <string.h>

void mh_glacier_init(glacier *g, const char *name)
{
    memset(g, 0, sizeof(*g));
    g->name = name;
    g->log  = stderr;
    g->card_present = false;
}

uint32_t mh_glacier_read(void *ctx, uint32_t off, unsigned size)
{
    glacier *g = ctx;

    if (off >= GLACIER_NREG) {
        /* No decoded device is known above this register file. Inserting a
         * card does not justify inventing zero-valued registers here. */
        g->unknown_reads++;
        if (g->log_unknown)
            fprintf(g->log, "[%s] read outside register file +%06X\n",
                    g->name, off);
        return 0xFFFFFFFFu;
    }

    uint16_t v = g->reg[off / 2];

    if (off == GLACIER_STATUS) {
        /* Pin inputs cannot be overridden by a register-file write. */
        v &= ~GLACIER_ST_CD_MASK;
        if (!g->card_present) v |= GLACIER_ST_CD_MASK;
    }
    return v;
}

void mh_glacier_write(void *ctx, uint32_t off, unsigned size, uint32_t val)
{
    glacier *g = ctx;

    if (off >= GLACIER_NREG) {
        g->unknown_writes++;
        if (g->log_unknown)
            fprintf(g->log, "[%s] write outside register file +%06X = %08X\n",
                    g->name, off, val);
        return;
    }

    if (g->log_unknown && (off != GLACIER_STATUS) && g->reg[off / 2] != (uint16_t)val)
        fprintf(g->log, "[%s] control +%02X = %04X\n", g->name, off, (uint16_t)val);

    if (off >= GLACIER_RISE_PENDING && off <= 0x1E) {
        g->reg[off / 2] &= ~(uint16_t)val; /* pending bits: write one to clear */
    } else if (off == GLACIER_STATUS) {
        /* CD and RDY/IREQ are device inputs, not software output bits. */
        const uint16_t inputs = GLACIER_ST_CD_MASK | 14;
        g->reg[off / 2] = (g->reg[off / 2] & inputs) | ((uint16_t)val & ~inputs);
    } else {
        g->reg[off / 2] = (uint16_t)val;
    }
}

void mh_glacier_set_present(glacier *g, bool present)
{
    if (g->card_present == present) return;
    g->card_present = present;
    unsigned pending = present ? GLACIER_FALL_PENDING : GLACIER_RISE_PENDING;
    g->reg[pending / 2] |= GLACIER_ST_CD_MASK;
}

void mh_glacier_set_memory_inputs(glacier *g, bool ready,
                                   bool write_protected, bool battery_good)
{
    /* ROM 13C338D4: ready (bit 2); 13C33924 / selector 0511:
     * write-protect (bit 3); 13C346CC: battery input (bit 1).
     * The other battery input is wired to the board's TX39 IO pin. */
    uint16_t value = (ready ? 4 : 0) | (write_protected ? 8 : 0) |
                     (battery_good ? 2 : 0);
    uint16_t old = g->reg[GLACIER_STATUS / 2] & 14;
    g->reg[GLACIER_STATUS / 2] = (g->reg[GLACIER_STATUS / 2] & ~14) | value;
    g->reg[GLACIER_RISE_PENDING / 2] |= value & ~old;
    g->reg[GLACIER_FALL_PENDING / 2] |= old & ~value;
}

bool mh_glacier_irq(const glacier *g)
{
    for (unsigned off = 0x10; off <= 0x16; off += 2)
        if (g->reg[off / 2] & g->reg[(off + 8) / 2]) return true;
    return false;
}

void mh_glacier_set_ready_irq(glacier *g, bool high)
{
    /* Legacy I/O-card status helper. ROM 0x13C33974 debounces bit 3;
     * Ne2000 package offset 0x580 disables that interrupt. For SRAM this
     * same status bit is write-protect, not the ready input. */
    const uint16_t bit = 8;
    bool old = (g->reg[GLACIER_STATUS / 2] & bit) != 0;
    if (old == high) return;
    g->reg[GLACIER_STATUS / 2] ^= bit;
    g->reg[(high ? GLACIER_RISE_PENDING : GLACIER_FALL_PENDING) / 2] |= bit;
}

void mh_glacier_set_card_irq(glacier *g, bool asserted)
{
    /* With WCPack/Ne2000 installed the guest writes +1C=4, then enables
     * +14 bit 2. ROM 0x13C20050/0x13C200F8 dispatches falling events from
     * these words. The NE2000 driver masks bit 3 after readiness. Thus the
     * controller's I/O-card IRQ indication is distinct from ready bit 3;
     * the physical pin mux inside Glacier remains undocumented. */
    const uint16_t bit = 4;
    bool high = !asserted;
    bool old = (g->reg[GLACIER_STATUS / 2] & bit) != 0;
    if (old == high) return;
    g->reg[GLACIER_STATUS / 2] ^= bit;
    g->reg[(high ? GLACIER_RISE_PENDING : GLACIER_FALL_PENDING) / 2] |= bit;
}
