#include "devices/pccard/pccard.h"

/* A compatible card's CIS, not a dump of a particular physical card.
 * Tuple layout/configuration follows linux-firmware cis/NE2K.cis. NDC /
 * Ethernet is matched by both the Magic Cap Ne2000 package and Linux's
 * pcnet_cs driver. Attribute bytes occupy even card addresses.
 * ROM 0x13C2DB50/0x13C2D8F0 enumerate these tuples through window A. */
static const uint8_t cis[] = {
    0x01,3,0,0,0xff,
    0x15,17,4,1,'N','D','C',0,'E','t','h','e','r','n','e','t',0,0,0xff,
    0x21,2,6,0,                         /* LAN function */
    0x1a,5,1,0x20,0xf8,3,3,            /* COR at attribute 0x3f8 */
    0x1b,9,0xe0,1,0x19,1,0x55,0x65,0x30,0xff,0xff,
    0x14,0,0xff,0                       /* no common-memory link */
};

/* The 68k SDK's PCCard.h defines CISTPL_A0 as a 32-byte MagicCapTuple.
 * PIC-2000 ROM 0E07D38C accepts GMMC/version 00010001 and checks the raw
 * reflected CRC-32 of the first 28 bytes with seed zero (ROM 0E0BA188).
 * IOCD is the SDK's I/O-card type. MIPS keeps the original CIS above. */
static const uint8_t magic_tuple[] = {
    0xa0, 32,
    'G','M','M','C', 0,1,0,1, 'I','O','C','D',
    0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0,
    0x6d,0x1f,0x5f,0x83,
};

bool mrc_ne2000_card_attribute_byte(uint32_t off, uint8_t config, uint8_t *value)
{
    if (!value) return false;
    if (off == 0x3f8) { *value = config; return true; }
    if (!(off & 1) && off / 2 < sizeof(cis)) {
        *value = cis[off / 2];
        return true;
    }
    return false;
}

bool mrc_ne2000_magic_attribute_byte(uint32_t off, uint8_t config, uint8_t *value)
{
    if (!value) return false;
    if (off == 0x3f8) { *value = config; return true; }
    if (off & 1) return false;
    uint32_t index = off / 2;
    const uint32_t prefix = sizeof(cis) - 2; /* before END,0 */
    if (index < prefix) { *value = cis[index]; return true; }
    index -= prefix;
    if (index < sizeof(magic_tuple)) { *value = magic_tuple[index]; return true; }
    index -= sizeof(magic_tuple);
    if (index < 2) { *value = cis[prefix + index]; return true; }
    return false;
}

/* Measured guest mapping: both attribute accesses and I/O at 0x300..0x31f
 * arrive through window A. The package writes COR=0x60 before the ROM's
 * 0x13C34A50/0x13C34B34 byte-I/O helpers and package word-DMA helpers access
 * that range. Glacier +20 bit 3 toggles around odd-byte cycles; it is not
 * a persistent attribute/I/O mode switch. Only this observed I/O range is
 * decoded here: other aliases and the complete controller decoder are unknown. */
static bool io_address(const mrc_pccard *c, mrc_pccard_window w, uint32_t off)
{
    return w == MRC_PCCARD_WINDOW_A && off >= 0x300 && off < 0x320 &&
           (c->config & 0x3f) == 0x20 && !(c->config & 0x80);
}

static bool read_card(mrc_pccard *c, mrc_pccard_window w, uint32_t off,
                      unsigned size, uint32_t *out)
{
    if (io_address(c, w, off)) {
        *out = mrc_ne2000_read(&c->nic, off - 0x300, size);
        /* Board word lanes: the BE guest stores EtherType 0x0806 directly
         * at +310, and packet RAM must contain bytes 08,06. The core's
         * NE2000 data port is little-endian. PROM duplicates hid this swap. */
        if (off == 0x310 && size == 2)
            *out = ((*out & 255) << 8) | ((*out >> 8) & 255);
        return true;
    }
    if (w != MRC_PCCARD_WINDOW_A || size != 1) return false;
    uint8_t byte;
    if (!mrc_ne2000_card_attribute_byte(off, c->config, &byte)) return false;
    *out = byte;
    return true;
}

static bool write_card(mrc_pccard *c, mrc_pccard_window w, uint32_t off,
                       unsigned size, uint32_t value)
{
    if (io_address(c, w, off)) {
        if (off == 0x310 && size == 2)
            value = ((value & 255) << 8) | ((value >> 8) & 255);
        mrc_ne2000_write(&c->nic, off - 0x300, size, value);
        return true;
    }
    if (w != MRC_PCCARD_WINDOW_A || off != 0x3f8 || size != 1) return false;
    c->config = (uint8_t)value;
    if (value & 0x80) (void)mrc_ne2000_read(&c->nic, 0x1f, 1);
    return true;
}

const mrc_pccard_kind mrc_pccard_ne2000 = {
    .name = "ne2000", .read = read_card, .write = write_card
};
