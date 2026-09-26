#include "devices/pccard/pccard.h"

#include <string.h>

/* PCMCIA DEVICE type 6 is SRAM, speed code 1 is 250 ns. Attribute bytes
 * occupy even addresses in window A; window B is common memory. ROM
 * 13C34288 probes A, 13C32A78 reads the storage header from B, and
 * 13C32074..13C32094 measures capacity by testing common-memory aliases.
 * The CIS is card identification, not a prebuilt Magic Cap filesystem. */
void mh_pccard_sram_init(mh_pccard *c, unsigned slot, uint8_t *data, uint32_t size)
{
    mh_pccard_init(c, slot, &mh_pccard_sram);
    c->image = data;
    c->image_len = size;
    unsigned unit = 0, units = size / 512;
    while (units > 32) {
        units /= 4;
        unit++;
    }
    uint8_t cis[] = {
        1, 3, 0x61, (uint8_t)(((units - 1) << 3) | unit), 0xff,
        0x15, 13, 4, 1, 'M', 'R', 'C', 0, 'S', 'R', 'A', 'M', 0, 0, 0xff,
        0x21, 2, 1, 0, /* FUNCID: memory */
        0x14, 0,       /* NO_LINK */
        0xff, 0
    };
    memcpy(c->cis, cis, sizeof(cis));
    c->cis_len = sizeof(cis);
}

static bool read_sram(mh_pccard *c, mh_pccard_window w, uint32_t off,
                      unsigned size, uint32_t *out)
{
    uint32_t v = 0;
    for (unsigned j = 0; j < size; j++) {
        uint32_t a = off + j;
        uint8_t b = w == MH_PCCARD_WINDOW_A ?
            (!(a & 1) && a / 2 < c->cis_len ? c->cis[a / 2] : 0xff) :
            c->image[a % c->image_len];
        v = (v << 8) | b;
    }
    *out = v;
    return true;
}

static bool write_sram(mh_pccard *c, mh_pccard_window w, uint32_t off,
                       unsigned size, uint32_t val)
{
    if (w != MH_PCCARD_WINDOW_B) return false;
    for (unsigned j = 0; j < size; j++)
        c->image[(off + j) % c->image_len] = (uint8_t)(val >> ((size - j - 1) * 8));
    return true;
}

const mh_pccard_kind mh_pccard_sram = {
    .name = "sram", .read = read_sram, .write = write_sram
};
