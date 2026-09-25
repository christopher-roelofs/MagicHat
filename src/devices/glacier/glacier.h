/*
 * glacier.h — "Glacier", the GMI-Japan custom PC Card controller.
 *
 * The DataRover has two, one per PCMCIA slot, at physical 0x10400000 and
 * 0x10800000. They are register-identical; the ROM's reset code programs
 * both with the same sequence at 0x83C003F0..0x83C00468, differing only in
 * the value it writes to register 0x20 (0 for slot 0, 1 for slot 1). That
 * alone does not establish a slot-ID register: the shared IRQ handler at
 * 0x13C20174 also toggles its low two bits. Its full routing role is unknown.
 *
 * Registers are 16 bits wide. No Glacier datasheet is available. Card-detect
 * inputs, enabled edge latches and write-one acknowledgements are derived
 * from ROM traces; other registers remain a partial register-file model.
 * See docs/NETWORKING.md for the measured paths and remaining unknowns.
 */
#ifndef MRC_GLACIER_H
#define MRC_GLACIER_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#define GLACIER_WINDOW  0x00400000u
#define GLACIER_NREG    0x40u

/*
 * Register +0x0C is the slot status register, and bits 10 and 11 are the two
 * PCMCIA card-detect lines. The ROM says so: at 0x83C33844 it points the
 * debounce helper at slot 0's +0x0C and calls it three times, masking 0x0400,
 * then 0x0800, then both together, comparing the two singly-read results.
 * Testing two detect pins separately and then jointly is exactly how a host
 * distinguishes a fully seated card from a partly inserted one.
 *
 * Card detect is active low — a card grounds the pins — so an empty slot
 * reads them high. Returning zero, as we did, presents two pins pulled down
 * with no card behind them, and the debounce never settles: it polls the
 * register 195,000 times over 120M instructions instead of 65,000.
 */
#define GLACIER_STATUS      0x0C
#define GLACIER_ST_CD1      (1u << 10)
#define GLACIER_ST_CD2      (1u << 11)
#define GLACIER_ST_CD_MASK  (GLACIER_ST_CD1 | GLACIER_ST_CD2)

/* ROM 0x13C20024..0x13C20170 pairs these enables with the pending
 * registers eight bytes later. 0x13C344C0 acknowledges CD/ready events by
 * writing their masks to +18/+1C. Edge polarity is inferred from the
 * paired event registrations at 0x13C34780 and the active-low CD pins. */
#define GLACIER_RISE_ENABLE  0x10
#define GLACIER_FALL_ENABLE  0x14
#define GLACIER_RISE_PENDING 0x18
#define GLACIER_FALL_PENDING 0x1C

typedef struct {
    const char *name;
    uint16_t    reg[GLACIER_NREG / 2];
    bool        card_present;
    FILE       *log;
    bool        log_unknown;
    uint64_t    unknown_reads, unknown_writes;
} glacier;

void     mrc_glacier_init(glacier *g, const char *name);
void     mrc_glacier_set_present(glacier *g, bool present);
void     mrc_glacier_set_ready_irq(glacier *g, bool high);
void     mrc_glacier_set_card_irq(glacier *g, bool asserted);
void     mrc_glacier_set_memory_inputs(glacier *g, bool ready,
                                       bool write_protected, bool battery_good);
bool     mrc_glacier_irq(const glacier *g);
uint32_t mrc_glacier_read(void *ctx, uint32_t off, unsigned size);
void     mrc_glacier_write(void *ctx, uint32_t off, unsigned size, uint32_t val);

#endif /* MRC_GLACIER_H */
