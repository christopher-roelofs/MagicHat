#include "rom/identify.h"
#include <stdbool.h>
#include <string.h>

static uint32_t be32(const uint8_t *p)
{
    return (uint32_t)p[0]<<24 | (uint32_t)p[1]<<16 | (uint32_t)p[2]<<8 | p[3];
}

static bool contains(const uint8_t *p, size_t size, const char *text)
{
    size_t len = strlen(text);
    if (!len || len > size) return false;
    /* memchr for the first byte, then compare: the same answer as the
     * byte-by-byte memcmp walk, without a libc call per ROM byte (that walk
     * was 4% of a five-second run). */
    const uint8_t *end = p + size - len + 1;
    for (const uint8_t *q = p; (q = memchr(q, text[0], (size_t)(end - q))); ++q)
        if (!memcmp(q, text, len)) return true;
    return false;
}

mh_rom_device mh_rom_identify(const uint8_t *d, size_t n)
{
    /* MIPS jump to the monitor entry, IDT header, reset CP0 setup and the
     * monitor's Apollo platform identifier. RAM immediates are not matched. */
    if (n >= 0x20000 && be32(d) == 0x08f00007 &&
        !memcmp(d+12, "IDT MONITOR ", 12) &&
        be32(d+32) == 0x40826000 && be32(d+36) == 0x40806800 &&
        contains(d, 0x20000, "Apollo"))
        return MH_ROM_DATAROVER840;

    if (n != 4u*1024*1024) return MH_ROM_UNKNOWN;
    uint32_t sp = be32(d), pc = be32(d+4), base = pc & ~0x3fffffu;
    size_t entry = pc & 0x3fffffu;
    if (!sp || (sp & 1) || sp > 0x1000000 || (pc & 1) ||
        entry < 8 || entry > n-12 ||
        (base != 0x0e000000 && base != 0x02400000)) return MH_ROM_UNKNOWN;
    /* Both reset variants load SP, then clear A0 and initialize USP. */
    if ((d[entry] != 0x2e || (d[entry+1] != 0x7c && d[entry+1] != 0x79)) ||
        be32(d+entry+6) != 0x91c84e60) return MH_ROM_UNKNOWN;

    bool sony1 = contains(d,n,",SONY,1,");
    bool sony2 = contains(d,n,",SONY,2,");
    bool moto = contains(d,n,",MOTO,1,");
    bool hix = contains(d,n,"HIX-300");
    if (base == 0x02400000 && moto && !sony1 && !sony2 &&
        (contains(d,n,"Motorola Envoy") ||
         (contains(d,n,"1,0.31,MOTO,1,") && contains(d,n,"Envoy"))))
        return MH_ROM_ENVOY;
    if (base != 0x0e000000 || moto || (sony1 && sony2)) return MH_ROM_UNKNOWN;
    if (sony2 && !hix && contains(d,n,"PIC-2000")) return MH_ROM_PIC2000;
    if (sony1 && hix && !contains(d,n,"PIC-2000")) return MH_ROM_HIX300;
    if (sony1 && !hix && contains(d,n,"PIC-1000") && !contains(d,n,"PIC-2000"))
        return MH_ROM_PIC1000;
    return MH_ROM_UNKNOWN;
}

const char *mh_rom_device_name(mh_rom_device d)
{
    switch (d) {
    case MH_ROM_DATAROVER840: return "Oki DataRover 840 (MIPS)";
    case MH_ROM_PIC2000: return "Sony PIC-2000 (68k)";
    case MH_ROM_PIC1000: return "Sony PIC-1000 (68k)";
    case MH_ROM_HIX300: return "Sony HIX-300 (68k)";
    case MH_ROM_ENVOY: return "Motorola Envoy (68k)";
    default: return "unknown or ambiguous ROM";
    }
}

const char *mh_rom_device_product(mh_rom_device d)
{
    switch (d) {
    case MH_ROM_DATAROVER840: return "Oki DataRover 840";
    case MH_ROM_PIC2000: return "Sony PIC-2000";
    case MH_ROM_PIC1000: return "Sony PIC-1000";
    case MH_ROM_HIX300: return "Sony HIX-300";
    case MH_ROM_ENVOY: return "Motorola Envoy";
    default: return "Magic Cap device";
    }
}

static const struct { mh_rom_device device; const char *slug; } slugs[] = {
    { MH_ROM_DATAROVER840, "datarover840" },
    { MH_ROM_PIC2000,      "pic2000" },
    { MH_ROM_PIC1000,      "pic1000" },
    { MH_ROM_HIX300,       "hix300" },
    { MH_ROM_ENVOY,        "envoy" },
};

const char *mh_rom_device_slug(mh_rom_device device)
{
    for (size_t i = 0; i < sizeof(slugs)/sizeof(*slugs); i++)
        if (slugs[i].device == device) return slugs[i].slug;
    return "unknown";
}

mh_rom_device mh_rom_device_from_slug(const char *slug)
{
    if (!slug) return MH_ROM_UNKNOWN;
    for (size_t i = 0; i < sizeof(slugs)/sizeof(*slugs); i++)
        if (!strcmp(slug, slugs[i].slug)) return slugs[i].device;
    return MH_ROM_UNKNOWN;
}
