/*
 * bus.h — physical address space for the DataRover 840.
 *
 * Everything the CPU touches goes through here. There are no special cases
 * keyed on PC or on ROM symbol addresses: if an access is not claimed by a
 * region it is a bus error, and we say so.
 */
#ifndef MH_BUS_H
#define MH_BUS_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

typedef struct mh_bus mh_bus;

/* A memory-mapped device. off is relative to the region base. */
typedef uint32_t (*mh_mmio_read_fn)(void *ctx, uint32_t off, unsigned size);
typedef void     (*mh_mmio_write_fn)(void *ctx, uint32_t off, unsigned size,
                                      uint32_t val);

typedef enum {
    MH_REGION_RAM,     /* backed by host memory, read/write */
    MH_REGION_ROM,     /* backed by host memory, read-only  */
    MH_REGION_FLASH,   /* backed by host memory, writable   */
    MH_REGION_MMIO,    /* dispatched to a device            */
    MH_REGION_FLOAT,   /* board decodes it, nothing drives it */
} mh_region_kind;

typedef struct {
    const char       *name;
    mh_region_kind   kind;
    uint32_t          base;     /* physical base address */
    uint32_t          size;     /* bytes */
    uint8_t          *host;     /* RAM/ROM backing store */
    uint32_t          host_len; /* backing store length; accesses wrap modulo
                                   this when smaller than size (mirroring) */
    void             *ctx;      /* MMIO device context */
    mh_mmio_read_fn  read;
    mh_mmio_write_fn write;
} mh_region;

#define MH_MAX_REGIONS 16

struct mh_bus {
    mh_region region[MH_MAX_REGIONS];
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

void mh_bus_init(mh_bus *b);
/* Opt in only when map owners invalidate after editing region geometry or
 * replacing backing stores (CPU fetch spans can hold backing pointers).
 * Entries contain region indices, never host pointers or guest bytes.
 * Writes to existing backing stores need no invalidation. */
void mh_bus_invalidate_lookup(mh_bus *b);
void mh_bus_enable_lookup(mh_bus *b, bool enabled);

/* Returns the new region, or NULL if the table is full. */
/*
 * `window` is the address range the chip select decodes; `len` is how much
 * memory is actually installed. When the window is larger the array mirrors
 * within it, which is what the hardware does. (The ROM does not use that to
 * discover the size; it does not discover the size at all. See
 * docs/HARDWARE.md.)
 */
mh_region *mh_bus_add_ram(mh_bus *b, const char *name, uint32_t base,
                            uint8_t *host, uint32_t len, uint32_t window);
mh_region *mh_bus_add_rom(mh_bus *b, const char *name, uint32_t base,
                            uint32_t size, uint8_t *host, uint32_t host_len);

/*
 * Like a ROM region but writable. The DataRover's program storage is flash,
 * not mask ROM — the DataRover840F flasher writes it, packages install into
 * it, and the OS image itself ships with a patch placeholder in it (see
 * machine.c). We do not model NOR program/erase command sequences: a store
 * simply takes effect.
 */
mh_region *mh_bus_add_flash(mh_bus *b, const char *name, uint32_t base,
                              uint32_t size, uint8_t *host, uint32_t host_len);
/*
 * A region the board decodes but that has nothing installed behind it — an
 * empty card slot, say. Reads float high (all ones), writes go nowhere. This
 * is distinct from an unmapped access, which is a bus error, and from a
 * device, which has behaviour: it records that we know the address is legal
 * and know nothing answers there.
 */
mh_region *mh_bus_add_float(mh_bus *b, const char *name, uint32_t base,
                              uint32_t size);

mh_region *mh_bus_add_mmio(mh_bus *b, const char *name, uint32_t base,
                             uint32_t size, void *ctx,
                             mh_mmio_read_fn rd, mh_mmio_write_fn wr);

/* size is 1, 2 or 4. *ok is set false on an unmapped access. */
uint32_t mh_bus_read(mh_bus *b, uint32_t pa, unsigned size, bool *ok);
void     mh_bus_write(mh_bus *b, uint32_t pa, unsigned size, uint32_t val,
                       bool *ok);

/* Direct backing-store access for loaders and dumpers. Never used by the CPU. */
uint8_t *mh_bus_host_ptr(mh_bus *b, uint32_t pa, uint32_t len);
/* Contiguous readable memory from pa, respecting priority and mirrors.
 * Does not perform/count a read. Invalid after lookup invalidation; intended
 * for short-lived CPU fetch spans, never MMIO or floating regions. */
uint8_t *mh_bus_read_span(mh_bus *b, uint32_t pa, uint32_t *len);

/*
 * Host pointer for the whole 4 KiB page containing pa when every byte of it
 * is served by one host-backed region with no earlier overlay and no mirror
 * seam inside the page: RAM, ROM or flash for reads, RAM only for writes
 * (flash writes are counted, ROM writes are faults). NULL otherwise. Does
 * not count as an access. Invalid after lookup invalidation.
 */
uint8_t *mh_bus_page_host(mh_bus *b, uint32_t pa, bool write);

void mh_bus_print_map(const mh_bus *b, FILE *f);

#endif /* MH_BUS_H */
