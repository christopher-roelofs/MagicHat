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
    mrc_bus b;
    bool ok;

    mrc_bus_init(&b);
    b.log = stderr;
    mrc_region *r = mrc_bus_add_ram(&b, "ram", 0x1000, backing,
                                    sizeof(backing), sizeof(backing));

    mrc_bus_read(&b, 0x1000, 4, &ok);
    check("an address inside the region is decoded", ok, true);

    mrc_bus_read(&b, 0x2000, 4, &ok);
    check("an address outside it is not", ok, false);

    r->size = 0;
    mrc_bus_read(&b, 0x1000, 4, &ok);
    check("a retired region stops answering its own base", ok, false);
    mrc_bus_read(&b, 0x00000000, 2, &ok);
    check("a retired region does not answer address zero", ok, false);
    mrc_bus_read(&b, 0xFFFFFFF0, 2, &ok);
    check("a retired region does not answer the top of memory", ok, false);

    r->size = 2;
    mrc_bus_read(&b, 0x1000, 4, &ok);
    check("a 4-byte access does not fit a 2-byte region", ok, false);
    mrc_bus_read(&b, 0x1000, 2, &ok);
    check("a 2-byte access does", ok, true);

    static uint8_t rom[16] = {0x12, 0x34};
    mrc_bus_init(&b);
    r = mrc_bus_add_rom(&b, "overlap", 0x1040, sizeof(rom), rom, sizeof(rom));
    mrc_bus_add_ram(&b, "ram", 0x1000, backing, sizeof(backing), sizeof(backing));
    mrc_bus_read(&b, 0x1000, 2, &ok);
    check("cached RAM cannot hide higher-priority ROM",
          mrc_bus_read(&b, 0x1040, 2, &ok) == 0x1234, true);
    mrc_bus_read(&b, 0x1000, 2, &ok);
    mrc_bus_write(&b, 0x1040, 2, 0xffff, &ok);
    check("overlapping ROM writes leave RAM untouched", backing[0x40] == 0, true);
    r->size = 0;
    check("retired overlay exposes RAM", mrc_bus_read(&b, 0x1040, 2, &ok) == 0, true);
    r->size = sizeof(rom);
    check("restored overlay supersedes cached RAM",
          mrc_bus_read(&b, 0x1040, 2, &ok) == 0x1234, true);

    /* Exercise a populated lookup cache, partial-page priority, backing
     * replacement, aliases, and geometry invalidation. */
    static uint8_t pages[8192], replacement[8192];
    mrc_bus_init(&b);
    r = mrc_bus_add_rom(&b, "partial overlay", 0x1040, sizeof(rom), rom, sizeof(rom));
    mrc_region *ram = mrc_bus_add_ram(&b, "pages", 0, pages, sizeof(pages), 16384);
    mrc_bus_add_ram(&b, "alias", 0x10000, pages, sizeof(pages), sizeof(pages));
    mrc_bus_enable_lookup(&b, true);
    uint32_t span_len;
    uint8_t *span = mrc_bus_read_span(&b, 0x1000, &span_len);
    check("fetch span stops before higher-priority overlay",
          span == pages + 0x1000 && span_len == 0x40, true);
    span = mrc_bus_read_span(&b, 0x1ffc, &span_len);
    check("fetch span stops at mirrored backing boundary",
          span == pages + 0x1ffc && span_len == 4, true);
    mrc_bus_read(&b, 0x1000, 4, &ok);
    check("cached page preserves partial overlay priority",
          mrc_bus_read(&b, 0x1040, 2, &ok) == 0x1234, true);
    mrc_bus_write(&b, 0x20, 4, 0x12345678, &ok);
    check("cached mapping reads current bytes through aliases",
          mrc_bus_read(&b, 0x10020, 4, &ok) == 0x12345678, true);
    check("cached mapping retains mirroring",
          mrc_bus_read(&b, 0x2020, 4, &ok) == 0x12345678, true);
    ram->host = replacement;
    check("cache never retains old backing pointers",
          mrc_bus_read(&b, 0x20, 4, &ok) == 0, true);
    r->size = 0;
    mrc_bus_invalidate_lookup(&b);
    check("invalidated overlay exposes underlying RAM",
          mrc_bus_read(&b, 0x1040, 2, &ok) == 0, true);
    r->size = sizeof(rom);
    mrc_bus_invalidate_lookup(&b);
    check("restored overlay supersedes populated page cache",
          mrc_bus_read(&b, 0x1040, 2, &ok) == 0x1234, true);
    mrc_bus_read(&b, 0x3ffc, 4, &ok);
    mrc_bus_read(&b, 0x3fff, 2, &ok);
    check("cross-page access still checks region boundary", ok, false);
    ram->size = 0;
    mrc_bus_invalidate_lookup(&b);
    mrc_bus_read(&b, 0x20, 4, &ok);
    check("invalidated retired cached region stops answering", ok, false);

    printf(fails ? "\n%d checks failed\n" : "\nall checks passed\n", fails);
    return fails ? 1 : 0;
}
