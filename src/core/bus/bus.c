#include "core/bus/bus.h"

#include "util/portable.h"

#include <string.h>

void mh_bus_init(mh_bus *b)
{
    memset(b, 0, sizeof(*b));
    b->log = stderr;
}

void mh_bus_invalidate_lookup(mh_bus *b)
{
    b->lookup_generation++;
    memset(b->lookup_cache, 0, sizeof(b->lookup_cache));
}

void mh_bus_enable_lookup(mh_bus *b, bool enabled)
{
    mh_bus_invalidate_lookup(b);
    b->lookup_cache_enabled = enabled;
}

static mh_region *alloc_region(mh_bus *b)
{
    if (b->nregion >= MH_MAX_REGIONS)
        return NULL;
    mh_bus_invalidate_lookup(b);
    return &b->region[b->nregion++];
}

mh_region *mh_bus_add_ram(mh_bus *b, const char *name, uint32_t base,
                            uint8_t *host, uint32_t len, uint32_t window)
{
    mh_region *r = alloc_region(b);
    if (!r)
        return NULL;
    if (window < len)
        window = len;
    *r = (mh_region){ .name = name, .kind = MH_REGION_RAM, .base = base,
                       .size = window, .host = host, .host_len = len };
    return r;
}

mh_region *mh_bus_add_rom(mh_bus *b, const char *name, uint32_t base,
                            uint32_t size, uint8_t *host, uint32_t host_len)
{
    mh_region *r = alloc_region(b);
    if (!r)
        return NULL;
    *r = (mh_region){ .name = name, .kind = MH_REGION_ROM, .base = base,
                       .size = size, .host = host, .host_len = host_len };
    return r;
}

mh_region *mh_bus_add_flash(mh_bus *b, const char *name, uint32_t base,
                              uint32_t size, uint8_t *host, uint32_t host_len)
{
    mh_region *r = mh_bus_add_rom(b, name, base, size, host, host_len);
    if (r)
        r->kind = MH_REGION_FLASH;
    return r;
}

mh_region *mh_bus_add_float(mh_bus *b, const char *name, uint32_t base,
                              uint32_t size)
{
    mh_region *r = alloc_region(b);
    if (!r)
        return NULL;
    *r = (mh_region){ .name = name, .kind = MH_REGION_FLOAT, .base = base,
                       .size = size };
    return r;
}

mh_region *mh_bus_add_mmio(mh_bus *b, const char *name, uint32_t base,
                             uint32_t size, void *ctx,
                             mh_mmio_read_fn rd, mh_mmio_write_fn wr)
{
    mh_region *r = alloc_region(b);
    if (!r)
        return NULL;
    *r = (mh_region){ .name = name, .kind = MH_REGION_MMIO, .base = base,
                       .size = size, .ctx = ctx, .read = rd, .write = wr };
    return r;
}

static uint32_t site(const mh_bus *b)
{
    return b->pc_hint ? *b->pc_hint : 0;
}

static inline bool in_region(const mh_region *r, uint32_t pa, unsigned size)
{
    /*
     * The size check comes first, and it is not redundant. Both terms are
     * unsigned, so a region smaller than the access -- a region of size zero
     * most of all -- makes `r->size - size` wrap to something enormous and
     * the region then matches every address in the machine. A zero-size
     * region is a reasonable way to retire one, and it turned a retired boot
     * overlay into a device that answered the whole address space.
     */
    return r->size >= size && pa >= r->base && pa - r->base <= r->size - size;
}

static mh_region *find(mh_bus *b, uint32_t pa, unsigned size)
{
    uint32_t page = pa >> 12;
    unsigned slot = (page ^ (page >> 8)) & 255;
    bool page_access = size && size <= 4096 && (pa & 4095) <= 4096 - size;
    if (b->lookup_cache_enabled && page_access &&
        b->lookup_cache[slot].region_plus_one &&
        b->lookup_cache[slot].page == page) {
        unsigned i = b->lookup_cache[slot].region_plus_one - 1;
        b->last_hit = i;
        return &b->region[i];
    }
    /* First registered decode wins, including after an overlay changes.
     * A last-hit shortcut is unsafe: Envoy's ROM at 00400000 overlaps
     * the low RAM mirror. A RAM data read must not redirect the next ROM
     * instruction fetch into RAM. */
    mh_region *r;
    for (unsigned i = 0; i < b->nregion; i++) {
        r = &b->region[i];
        if (in_region(r, pa, size)) {
            b->last_hit = i;
            if (b->lookup_cache_enabled && page_access &&
                in_region(r, pa & ~4095u, 4096)) {
                /* Cache only uniform pages. Even a one-byte earlier
                 * overlay makes this page unsuitable for the fast path. */
                bool uniform = true;
                uint64_t start = (uint64_t)page << 12;
                for (unsigned j = 0; j < i; j++) {
                    const mh_region *p = &b->region[j];
                    if (p->size && p->base < start + 4096 &&
                        (uint64_t)p->base + p->size > start) {
                        uniform = false;
                        break;
                    }
                }
                if (uniform) {
                    b->lookup_cache[slot].page = page;
                    b->lookup_cache[slot].region_plus_one = i + 1;
                }
            }
            return r;
        }
    }
    return NULL;
}

/*
 * The TX39 is big-endian in this design (the Magic Cap ROM is BE).
 *
 * These are written as an explicit byte assembly rather than a loop because
 * the loop cost four shift-or iterations on every instruction fetch. In this
 * form the compiler recognises the idiom and emits one unaligned load plus a
 * byte swap. A memcpy into a uint32_t followed by __builtin_bswap32 would do
 * the same on x86 but would be wrong on a big-endian host; this is correct
 * everywhere and just as fast where it matters.
 *
 * The pointer may be unaligned: the ROM mirrors within a chip select whose
 * installed size (4,528,151 bytes) is not a multiple of four, so a mirrored
 * access can land anywhere. Byte accesses cannot fault on alignment.
 */
static inline uint32_t load_be(const uint8_t *p, unsigned size)
{
    switch (size) {
    case 4:
        return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
               ((uint32_t)p[2] << 8)  |  (uint32_t)p[3];
    case 2:
        return ((uint32_t)p[0] << 8) | (uint32_t)p[1];
    default:
        return p[0];
    }
}

static inline void store_be(uint8_t *p, unsigned size, uint32_t v)
{
    switch (size) {
    case 4:
        p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
        p[2] = (uint8_t)(v >> 8);  p[3] = (uint8_t)v;
        return;
    case 2:
        p[0] = (uint8_t)(v >> 8);  p[1] = (uint8_t)v;
        return;
    default:
        p[0] = (uint8_t)v;
        return;
    }
}

/*
 * Where in the backing store an offset lands.
 *
 * A chip select can decode more address space than the chips behind it hold,
 * so the contents mirror through the rest — the ROM's sizing loop depends on
 * that. Modelling it with a plain modulo put a 32-bit integer division on
 * every instruction fetch and every load and store, which is the most
 * expensive integer operation there is and was costing far more than the
 * mirroring is worth. Almost every access is inside the installed size.
 */
static inline uint32_t backing_offset(const mh_region *r, uint32_t off)
{
    if (MH_LIKELY(off < r->host_len))
        return off;
    /* Most Magic Cap ROM and DRAM chips are power-of-two sized. Their
     * mirrored windows have the same result with a mask and avoid a costly
     * integer division on the uncommon out-of-bounds access. */
    if ((r->host_len & (r->host_len - 1u)) == 0)
        return off & (r->host_len - 1u);
    return off % r->host_len;
}

uint32_t mh_bus_read(mh_bus *b, uint32_t pa, unsigned size, bool *ok)
{
    mh_region *r = find(b, pa, size);
    *ok = true;
    b->reads++;

    if (MH_UNLIKELY(!r)) {
        b->faults++;
        *ok = false;
        if (b->log_unmapped)
            fprintf(b->log, "[bus] unmapped read%u  pa=%08X @%08X\n", size * 8, pa, site(b));
        return 0;
    }

    uint32_t off = pa - r->base;

    if (MH_UNLIKELY(r->kind == MH_REGION_FLOAT)) {
        b->float_reads++;
        return size == 4 ? 0xFFFFFFFFu : (1u << (size * 8)) - 1u;
    }

    if (MH_UNLIKELY(r->kind == MH_REGION_MMIO)) {
        b->mmio_reads++;
        uint32_t v = r->read ? r->read(r->ctx, off, size) : 0;
        if (b->log_mmio)
            fprintf(b->log, "[mmio] R%u %s+%03X = %08X  @%08X\n", size * 8,
                    r->name, off, v, site(b));
        return v;
    }

    return load_be(r->host + backing_offset(r, off), size);
}

void mh_bus_write(mh_bus *b, uint32_t pa, unsigned size, uint32_t val,
                   bool *ok)
{
    mh_region *r = find(b, pa, size);
    *ok = true;
    b->writes++;

    if (MH_UNLIKELY(!r)) {
        b->faults++;
        *ok = false;
        if (b->log_unmapped)
            fprintf(b->log, "[bus] unmapped write%u pa=%08X val=%08X @%08X\n",
                    size * 8, pa, val, site(b));
        return;
    }

    uint32_t off = pa - r->base;

    if (MH_UNLIKELY(r->kind == MH_REGION_MMIO)) {
        b->mmio_writes++;
        if (b->log_mmio)
            fprintf(b->log, "[mmio] W%u %s+%03X = %08X  @%08X\n", size * 8,
                    r->name, off, val, site(b));
        if (r->write)
            r->write(r->ctx, off, size, val);
        return;
    }

    if (MH_UNLIKELY(r->kind == MH_REGION_FLOAT))
        return;

    if (MH_UNLIKELY(r->kind == MH_REGION_FLASH)) {
        b->flash_writes++;
        if (b->log_mmio)
            fprintf(b->log, "[flash] W%u %s+%08X = %08X  @%08X\n", size * 8,
                    r->name, off, val, site(b));
        store_be(r->host + backing_offset(r, off), size, val);
        return;
    }

    if (MH_UNLIKELY(r->kind == MH_REGION_ROM)) {
        /* A write to ROM is a real event worth knowing about, but the
         * hardware simply ignores it. Do the same, and count it. */
        b->faults++;
        if (b->log_unmapped)
            fprintf(b->log, "[bus] write%u to ROM %s+%08X val=%08X @%08X "
                    "(ignored)\n", size * 8, r->name, off, val, site(b));
        return;
    }

    store_be(r->host + backing_offset(r, off), size, val);
}

uint8_t *mh_bus_host_ptr(mh_bus *b, uint32_t pa, uint32_t len)
{
    mh_region *r = find(b, pa, 1);
    if (!r || !r->host)
        return NULL;
    uint32_t off = pa - r->base;
    if (off + len > r->host_len)
        return NULL;
    return r->host + off;
}

uint8_t *mh_bus_page_host(mh_bus *b, uint32_t pa, bool write)
{
    uint32_t start = pa & ~4095u;
    for (unsigned i = 0; i < b->nregion; i++) {
        mh_region *r = &b->region[i];
        if (!r->size)
            continue;
        if (!in_region(r, start, 1)) {
            /* An earlier region touching any byte of the page makes the
             * page non-uniform, exactly as find()'s cache rule has it. */
            if (r->base < start + 4096 && (uint64_t)r->base + r->size > start)
                return NULL;
            continue;
        }
        if (!in_region(r, start, 4096) || !r->host || !r->host_len)
            return NULL;
        if (write ? r->kind != MH_REGION_RAM
                  : (r->kind != MH_REGION_RAM && r->kind != MH_REGION_ROM &&
                     r->kind != MH_REGION_FLASH))
            return NULL;
        uint32_t backing = backing_offset(r, start - r->base);
        if ((uint64_t)backing + 4096 > r->host_len)
            return NULL;                 /* the mirror wraps inside the page */
        return r->host + backing;
    }
    return NULL;
}

uint8_t *mh_bus_read_span(mh_bus *b, uint32_t pa, uint32_t *len)
{
    *len = 0;
    mh_region *r = find(b, pa, 1);
    if (!r || !r->host || !r->host_len ||
        (r->kind != MH_REGION_RAM && r->kind != MH_REGION_ROM &&
         r->kind != MH_REGION_FLASH))
        return NULL;
    uint32_t off = pa - r->base;
    uint32_t backing = backing_offset(r, off);
    uint64_t end = (uint64_t)pa + r->size - off;
    uint64_t backing_end = (uint64_t)pa + r->host_len - backing;
    if (end > backing_end) end = backing_end;
    if (end > UINT64_C(0x100000000)) end = UINT64_C(0x100000000);
    for (mh_region *p = b->region; p < r; p++)
        if (p->size && p->base > pa && p->base < end)
            end = p->base;
    *len = (uint32_t)(end - pa);
    return r->host + backing;
}

void mh_bus_print_map(const mh_bus *b, FILE *f)
{
    static const char *kind[] = { "RAM", "ROM", "FLSH", "MMIO", "----" };
    fprintf(f, "Physical memory map:\n");
    for (unsigned i = 0; i < b->nregion; i++) {
        const mh_region *r = &b->region[i];
        fprintf(f, "  %08X-%08X  %-4s  %s", r->base, r->base + r->size - 1,
                kind[r->kind], r->name);
        if (r->kind == MH_REGION_FLOAT)
            fprintf(f, " (decoded, nothing installed)");
        if (r->host && r->host_len < r->size)
            fprintf(f, " (mirrors %u bytes)", r->host_len);
        fprintf(f, "\n");
    }
}
