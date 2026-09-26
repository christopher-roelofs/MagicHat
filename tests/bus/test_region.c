/*
 * The bus's region matcher, and one case that mattered.
 *
 * in_region() compares unsigned quantities, so a region smaller than the
 * access -- a region of size zero above all -- used to make the length term
 * wrap, and the region then matched every address in the machine. Retiring a
 * region by setting its size to zero is a reasonable thing to want to do; it
 * turned a retired boot overlay into a device that answered everywhere, and
 * the symptom was half a million writes silently landing on ROM.
 */
#include "core/bus/bus.h"

#include <stdio.h>

static int fails;

static void check(const char *what, bool got, bool want)
{
    if (got != want) {
        printf("  FAIL    %s: got %s, wanted %s\n", what,
               got ? "hit" : "miss", want ? "hit" : "miss");
        fails++;
    } else {
        printf("  ok      %s\n", what);
    }
}

int main(void)
{
    static uint8_t backing[256];
    mh_bus b;
    bool ok;

    mh_bus_init(&b);
    b.log = stderr;
    mh_region *r = mh_bus_add_ram(&b, "ram", 0x1000, backing,
                                    sizeof(backing), sizeof(backing));

    mh_bus_read(&b, 0x1000, 4, &ok);
    check("an address inside the region is decoded", ok, true);

    mh_bus_read(&b, 0x2000, 4, &ok);
    check("an address outside it is not", ok, false);

    r->size = 0;
    mh_bus_read(&b, 0x1000, 4, &ok);
    check("a retired region stops answering its own base", ok, false);
    mh_bus_read(&b, 0x00000000, 2, &ok);
    check("a retired region does not answer address zero", ok, false);
    mh_bus_read(&b, 0xFFFFFFF0, 2, &ok);
    check("a retired region does not answer the top of memory", ok, false);

    r->size = 2;
    mh_bus_read(&b, 0x1000, 4, &ok);
    check("a 4-byte access does not fit a 2-byte region", ok, false);
    mh_bus_read(&b, 0x1000, 2, &ok);
    check("a 2-byte access does", ok, true);

    static uint8_t rom[16] = {0x12, 0x34};
    mh_bus_init(&b);
    r = mh_bus_add_rom(&b, "overlap", 0x1040, sizeof(rom), rom, sizeof(rom));
    mh_bus_add_ram(&b, "ram", 0x1000, backing, sizeof(backing), sizeof(backing));
    mh_bus_read(&b, 0x1000, 2, &ok);
    check("cached RAM cannot hide higher-priority ROM",
          mh_bus_read(&b, 0x1040, 2, &ok) == 0x1234, true);
    mh_bus_read(&b, 0x1000, 2, &ok);
    mh_bus_write(&b, 0x1040, 2, 0xffff, &ok);
    check("overlapping ROM writes leave RAM untouched", backing[0x40] == 0, true);
    r->size = 0;
    check("retired overlay exposes RAM", mh_bus_read(&b, 0x1040, 2, &ok) == 0, true);
    r->size = sizeof(rom);
    check("restored overlay supersedes cached RAM",
          mh_bus_read(&b, 0x1040, 2, &ok) == 0x1234, true);

    /* Exercise a populated lookup cache, partial-page priority, backing
     * replacement, aliases, and geometry invalidation. */
    static uint8_t pages[8192], replacement[8192];
    mh_bus_init(&b);
    r = mh_bus_add_rom(&b, "partial overlay", 0x1040, sizeof(rom), rom, sizeof(rom));
    mh_region *ram = mh_bus_add_ram(&b, "pages", 0, pages, sizeof(pages), 16384);
    mh_bus_add_ram(&b, "alias", 0x10000, pages, sizeof(pages), sizeof(pages));
    mh_bus_enable_lookup(&b, true);
    uint32_t span_len;
    uint8_t *span = mh_bus_read_span(&b, 0x1000, &span_len);
    check("fetch span stops before higher-priority overlay",
          span == pages + 0x1000 && span_len == 0x40, true);
    span = mh_bus_read_span(&b, 0x1ffc, &span_len);
    check("fetch span stops at mirrored backing boundary",
          span == pages + 0x1ffc && span_len == 4, true);
    mh_bus_read(&b, 0x1000, 4, &ok);
    check("cached page preserves partial overlay priority",
          mh_bus_read(&b, 0x1040, 2, &ok) == 0x1234, true);
    mh_bus_write(&b, 0x20, 4, 0x12345678, &ok);
    check("cached mapping reads current bytes through aliases",
          mh_bus_read(&b, 0x10020, 4, &ok) == 0x12345678, true);
    check("cached mapping retains mirroring",
          mh_bus_read(&b, 0x2020, 4, &ok) == 0x12345678, true);
    ram->host = replacement;
    check("cache never retains old backing pointers",
          mh_bus_read(&b, 0x20, 4, &ok) == 0, true);
    r->size = 0;
    mh_bus_invalidate_lookup(&b);
    check("invalidated overlay exposes underlying RAM",
          mh_bus_read(&b, 0x1040, 2, &ok) == 0, true);
    r->size = sizeof(rom);
    mh_bus_invalidate_lookup(&b);
    check("restored overlay supersedes populated page cache",
          mh_bus_read(&b, 0x1040, 2, &ok) == 0x1234, true);
    mh_bus_read(&b, 0x3ffc, 4, &ok);
    mh_bus_read(&b, 0x3fff, 2, &ok);
    check("cross-page access still checks region boundary", ok, false);
    ram->size = 0;
    mh_bus_invalidate_lookup(&b);
    mh_bus_read(&b, 0x20, 4, &ok);
    check("invalidated retired cached region stops answering", ok, false);

    printf(fails ? "\n%d checks failed\n" : "\nall checks passed\n", fails);
    return fails ? 1 : 0;
}
