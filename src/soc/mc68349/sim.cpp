#include "soc/mc68349/sim.h"
#include <cstdlib>
uint32_t mc68349_sim_read(void *ctx, uint32_t off, unsigned size)
{
    auto *s = (Mc68349Sim *)ctx;
    if (s->duart && off >= 0x700 && off < 0x722 && off + size <= 0x722) {
        const uint32_t value = mc68349_duart_read(s->duart, off - 0x700, size);
        if (std::getenv("MRC_DUART_TRACE"))
            std::fprintf(s->log, "[duart] R%u +%03X = %0*X\n", size * 8,
                         off, (int)(size * 2), value);
        return value;
    }
    uint32_t v = 0;
    for (unsigned i = 0; i < size; i++) {
        unsigned o = off + i;
        /* MC68349UM 4.3.2.2: IDR is a read-only byte, $31. Zero
         * identifies the MC68340 and selects a different ROM clock path. */
        uint8_t b = o == 2 ? 0x31 :
            (uint8_t)(s->reg[o / 2] >> (o & 1 ? 0 : 8));
        v = (v << 8) | b;
    }
    return v;
}

void mc68349_sim_write(void *ctx, uint32_t off, unsigned size, uint32_t val)
{
    auto *s = (Mc68349Sim *)ctx;
    if (s->duart && off >= 0x700 && off < 0x722 && off + size <= 0x722) {
        if (std::getenv("MRC_DUART_TRACE"))
            std::fprintf(s->log, "[duart] W%u +%03X = %0*X\n", size * 8,
                         off, (int)(size * 2), val);
        mc68349_duart_write(s->duart, off - 0x700, size, val);
        return;
    }
    for (unsigned i = 0; i < size; i++) {
        unsigned o = off + size - 1 - i;
        if (o == 2) continue; // read-only MC68349 identification register
        uint8_t b = (uint8_t)(val >> (i * 8));
        uint16_t &w = s->reg[o / 2];
        /* DMA CSR (MC68349UM 7.7.3): W1C status in bits 6..2;
         * IRQ is the OR of these status bits, independent of enables. */
        if (o == 0x78A || o == 0x7AA) {
            uint8_t status = uint8_t(w >> 8) & uint8_t(~b) & 0x7c;
            if (status) status |= 0x80;
            w = uint16_t((w & 255) | unsigned(status) << 8);
            continue;
        }
        if (o & 1) w = (uint16_t)((w & 0xFF00) | b);
        else       w = (uint16_t)((w & 0x00FF) | (b << 8));
    }
    if (s->logged < 64) {
        s->logged++;
        fprintf(s->log, "[sim] +%03X = %0*X%s\n", off, (int)size * 2, val,
                (off >= 0x40 && off <= 0x5F) ? "   (chip select)" : "");
    }

    /*
     * CS0's base register. The ROM writes 0x0E0000F9 here, giving itself the
     * window it is already running from, and from that instant address 0 is
     * DRAM rather than a mirror of ROM. A region of zero size matches
     * nothing, which is how the overlay is retired.
     */
    if (off == 0x44 && !s->overlay_off && s->boot_select_programmed) {
        s->overlay_off = true;
        s->boot_select_programmed(s->board);
        if (s->bus) mrc_bus_invalidate_lookup(s->bus);
        fprintf(s->log, "[sim] CS0 base programmed: boot overlay retired, "
                "low memory is now DRAM\n");
    }
}

/*
 * Read the chip selects back as a memory map.
 *
 * The 68300 SIM programs each chip select as a pair of 32-bit registers from
 * +0x40, and the ROM writes eight of them at reset. The pairing is legible
 * from the values themselves rather than from a datasheet: the word written
 * at +0x44 is 0x0E0000F9, and 0x0E000000 is exactly where this ROM lives, so
 * the second register of each pair carries the base address in its top half
 * and flags in its bottom. That gives 0x04000000, 0x0C000000 and the rest,
 * and those are addresses the boot code goes on to touch -- which is the
 * check that the reading is right.
 */
void mc68349_sim_report_chip_selects(const Mc68349Sim *s, FILE *log)
{
    auto reg32 = [&](unsigned off) {
        return (uint32_t)s->reg[off / 2] << 16 | s->reg[off / 2 + 1];
    };
    bool any = false;
    for (unsigned cs = 0; cs < 4; cs++) {
        uint32_t a = reg32(0x40 + cs * 8);
        uint32_t b = reg32(0x44 + cs * 8);
        if (!a && !b) continue;
        if (!any) {
            fprintf(log, "sim: chip selects as the ROM programmed them\n");
            any = true;
        }
        fprintf(log, "  CS%u  %08X %08X   -> base %08X, flags %02X\n",
                cs, a, b, b & 0xFFFFFF00u, b & 0xFFu);
    }
    if (!any)
        fprintf(log, "sim: no chip selects programmed\n");
}
