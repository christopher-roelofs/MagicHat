/*
 * bus.h — physical address space for the DataRover 840.
 *
 * Everything the CPU touches goes through here. There are no special cases
 * keyed on PC or on ROM symbol addresses: if an access is not claimed by a
 * region it is a bus error, and we say so.
 */
#ifndef MRC_BUS_H
#define MRC_BUS_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

typedef struct mrc_bus mrc_bus;

/* A memory-mapped device. off is relative to the region base. */
typedef uint32_t (*mrc_mmio_read_fn)(void *ctx, uint32_t off, unsigned size);
typedef void     (*mrc_mmio_write_fn)(void *ctx, uint32_t off, unsigned size,
                                      uint32_t val);

typedef enum {
    MRC_REGION_RAM,     /* backed by host memory, read/write */
    MRC_REGION_ROM,     /* backed by host memory, read-only  */
    MRC_REGION_FLASH,   /* backed by host memory, writable   */
    MRC_REGION_MMIO,    /* dispatched to a device            */
    MRC_REGION_FLOAT,   /* board decodes it, nothing drives it */
} mrc_region_kind;

typedef struct {
    const char       *name;
    mrc_region_kind   kind;
    uint32_t          base;     /* physical base address */
    uint32_t          size;     /* bytes */
    uint8_t          *host;     /* RAM/ROM backing store */
    uint32_t          host_len; /* backing store length; accesses wrap modulo
                                   this when smaller than size (mirroring) */
    void             *ctx;      /* MMIO device context */
    mrc_mmio_read_fn  read;
    mrc_mmio_write_fn write;
} mrc_region;

#define MRC_MAX_REGIONS 16

struct mrc_bus {
    mrc_region region[MRC_MAX_REGIONS];
    unsigned   nregion;
    /* Diagnostic last match; not safe as an overlapping-map shortcut. */
    unsigned   last_hit;
    bool lookup_cache_enabled;
    uint64_t lookup_generation;
    struct {
        uint32_t page;
        unsigned region_plus_one;
    } lookup_cache[256];

    /* Diagnostics. Observation only — these never change bus behaviour. */
    bool     log_mmio;
    bool     log_unmapped;
    FILE    *log;
    /* Optional: points at the CPU's current PC so logs can name the access
     * site. Diagnostics only; the bus never reads it for anything else. */
    const uint32_t *pc_hint;

    /* Counters. */
    uint64_t reads, writes, mmio_reads, mmio_writes, float_reads, faults;
    uint64_t flash_writes;
};

void mrc_bus_init(mrc_bus *b);
/* Opt in only when map owners invalidate after editing region geometry or
 * replacing backing stores (CPU fetch spans can hold backing pointers).
 * Entries contain region indices, never host pointers or guest bytes.
 * Writes to existing backing stores need no invalidation. */
void mrc_bus_invalidate_lookup(mrc_bus *b);
void mrc_bus_enable_lookup(mrc_bus *b, bool enabled);

/* Returns the new region, or NULL if the table is full. */
/*
 * `window` is the address range the chip select decodes; `len` is how much
 * memory is actually installed. When the window is larger the array mirrors
 * within it, which is what the hardware does. (The ROM does not use that to
 * discover the size; it does not discover the size at all. See
 * docs/HARDWARE.md.)
 */
mrc_region *mrc_bus_add_ram(mrc_bus *b, const char *name, uint32_t base,
                            uint8_t *host, uint32_t len, uint32_t window);
mrc_region *mrc_bus_add_rom(mrc_bus *b, const char *name, uint32_t base,
                            uint32_t size, uint8_t *host, uint32_t host_len);

/*
 * Like a ROM region but writable. The DataRover's program storage is flash,
 * not mask ROM — the DataRover840F flasher writes it, packages install into
 * it, and the OS image itself ships with a patch placeholder in it (see
 * machine.c). We do not model NOR program/erase command sequences: a store
 * simply takes effect.
 */
mrc_region *mrc_bus_add_flash(mrc_bus *b, const char *name, uint32_t base,
                              uint32_t size, uint8_t *host, uint32_t host_len);
/*
 * A region the board decodes but that has nothing installed behind it — an
 * empty card slot, say. Reads float high (all ones), writes go nowhere. This
 * is distinct from an unmapped access, which is a bus error, and from a
 * device, which has behaviour: it records that we know the address is legal
 * and know nothing answers there.
 */
mrc_region *mrc_bus_add_float(mrc_bus *b, const char *name, uint32_t base,
                              uint32_t size);

mrc_region *mrc_bus_add_mmio(mrc_bus *b, const char *name, uint32_t base,
                             uint32_t size, void *ctx,
                             mrc_mmio_read_fn rd, mrc_mmio_write_fn wr);

/* size is 1, 2 or 4. *ok is set false on an unmapped access. */
uint32_t mrc_bus_read(mrc_bus *b, uint32_t pa, unsigned size, bool *ok);
void     mrc_bus_write(mrc_bus *b, uint32_t pa, unsigned size, uint32_t val,
                       bool *ok);

/* Direct backing-store access for loaders and dumpers. Never used by the CPU. */
uint8_t *mrc_bus_host_ptr(mrc_bus *b, uint32_t pa, uint32_t len);
/* Contiguous readable memory from pa, respecting priority and mirrors.
 * Does not perform/count a read. Invalid after lookup invalidation; intended
 * for short-lived CPU fetch spans, never MMIO or floating regions. */
uint8_t *mrc_bus_read_span(mrc_bus *b, uint32_t pa, uint32_t *len);

/*
 * Host pointer for the whole 4 KiB page containing pa when every byte of it
 * is served by one host-backed region with no earlier overlay and no mirror
 * seam inside the page: RAM, ROM or flash for reads, RAM only for writes
 * (flash writes are counted, ROM writes are faults). NULL otherwise. Does
 * not count as an access. Invalid after lookup invalidation.
 */
uint8_t *mrc_bus_page_host(mrc_bus *b, uint32_t pa, bool write);

void mrc_bus_print_map(const mrc_bus *b, FILE *f);

#endif /* MRC_BUS_H */
