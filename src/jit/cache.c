#include "jit/jit.h"
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#if defined(__linux__)
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>
#define HAVE_MMAP 1
#else
#define HAVE_MMAP 0
#endif

/*
 * Block cache. Blocks are keyed by virtual start address in a four-way
 * table; native code guards itself against the live instruction bytes on
 * every entry, so nothing here needs write notification. Code lives in one
 * arena that is flushed whole when it fills or when the bus map changes
 * (blocks embed host pointers to guest memory for their guards).
 *
 * The arena is one shared-memory object mapped twice: a writable view the
 * emitter fills and an executable view the CPU runs. No page is ever both
 * writable and executable, and no protection changes happen per block.
 * Where that is unavailable -- an old kernel, or a sandbox that refuses an
 * executable file mapping -- one anonymous mapping is used instead and the
 * window being written is made writable and put back before it is run. That
 * is slower per compiled block and identical in every other respect.
 */
#define SETS MRC_JIT_SETS
#define WAYS MRC_JIT_WAYS
#define ARENA_SIZE (32u << 20)
#define PAGES (1u << 20)             /* 4 GiB of guest virtual space */

struct r3900_jit {
    mrc_jit_tables tables;           /* first: native code loads from here */
    mrc_jit_slot slot[SETS * WAYS];
    uint8_t *arena, *writable;   /* the same bytes; equal when not dual-mapped */
    bool dual;
    size_t used, page_size;
    uint64_t generation;             /* bus map generation the tables match */
    bool tables_built, has_mmu;
    uint64_t compiled, native, fallback, invalidated, flushes, blocks, clock, links;
    bool failed;
};

r3900_jit *mrc_cpu_jit_create(void)
{
#if HAVE_MMAP
    if (!mrc_jit_host_supported()) return NULL;
    long page = sysconf(_SC_PAGESIZE);
    if (page < 4096) return NULL;
    /*
     * The syscall rather than the libc wrapper: this builds against API
     * levels whose headers predate memfd_create, and the wrapper is all the
     * wrapper would be. A kernel without it returns ENOSYS and the
     * single-mapping path below is used.
     */
    void *arena = MAP_FAILED, *writable = MAP_FAILED;
    bool dual = false;
#ifdef __NR_memfd_create
    /* MRC_JIT_SINGLE_MAP=1 exercises the fallback on a host that does not
     * need it; the two paths must behave identically. */
    int fd = getenv("MRC_JIT_SINGLE_MAP") ? -1 :
             (int)syscall(__NR_memfd_create, "mips-jit", 1u /* MFD_CLOEXEC */);
    if (fd >= 0) {
        if (!ftruncate(fd, ARENA_SIZE)) {
            arena = mmap(NULL, ARENA_SIZE, PROT_READ | PROT_EXEC, MAP_SHARED, fd, 0);
            writable = mmap(NULL, ARENA_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        }
        close(fd);
        if (arena != MAP_FAILED && writable != MAP_FAILED) dual = true;
        else {
            /* An executable file mapping can be refused where an anonymous
             * one is allowed; keep neither half of a partial pair. */
            if (arena != MAP_FAILED) munmap(arena, ARENA_SIZE);
            if (writable != MAP_FAILED) munmap(writable, ARENA_SIZE);
            arena = writable = MAP_FAILED;
        }
    }
#endif
    if (!dual) {
        arena = mmap(NULL, ARENA_SIZE, PROT_READ | PROT_EXEC,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        writable = arena;
    }
    r3900_jit *j = calloc(1, sizeof(*j));
    uint8_t **rd = calloc(PAGES, sizeof(*rd)), **wr = calloc(PAGES, sizeof(*wr));
    if (arena == MAP_FAILED || !j || !rd || !wr) {
        free(j); free(rd); free(wr);
        if (arena != MAP_FAILED) munmap(arena, ARENA_SIZE);
        if (dual && writable != MAP_FAILED) munmap(writable, ARENA_SIZE);
        return NULL;
    }
    j->arena = arena;
    j->writable = writable;
    j->dual = dual;
    j->page_size = (size_t)page;
    j->tables.read = rd;
    j->tables.write = wr;
    j->tables.slots = j->slot;
    j->generation = UINT64_MAX;
    return j;
#else
    return NULL;
#endif
}

void mrc_cpu_jit_free(r3900_jit *j)
{
    if (!j) return;
#if HAVE_MMAP
    munmap(j->arena, ARENA_SIZE);
    if (j->dual) munmap(j->writable, ARENA_SIZE);
#endif
    free(j->tables.read);
    free(j->tables.write);
    free(j);
}

void mrc_cpu_jit_report(const r3900_jit *j, FILE *f)
{
    if (j) fprintf(f, "mips-jit: compiled=%" PRIu64 " native=%" PRIu64
                   " fallback=%" PRIu64 " replaced=%" PRIu64 " disabled=%d"
                   " flushes=%" PRIu64 " calls=%" PRIu64 " links=%" PRIu64
                   " mapping=%s\n",
                   j->compiled, j->native, j->fallback, j->invalidated,
                   j->failed, j->flushes, j->blocks, j->links,
                   j->dual ? "dual" : "single");
}
uint64_t mrc_cpu_jit_native_count(const r3900_jit *j) { return j ? j->native : 0; }
void mrc_jit_account(r3900_jit *j, unsigned native, unsigned fallback)
{
    j->native += native; j->fallback += fallback; j->blocks += native != 0;
}

static void flush(r3900_jit *j)
{
    memset(j->slot, 0, sizeof(j->slot));
    j->used = 0;
    j->flushes++;
    j->tables.link = NULL;           /* it pointed into discarded code */
}

/* Direct page mappings follow the CPU's own segment rules: without an MMU
 * every segment maps straight through except kseg0/kseg1; with one, only
 * kseg0/kseg1 are unmapped and the rest stays on the reference path. */
static void build_tables(r3900_jit *j, r3900 *c)
{
    for (uint32_t page = 0; page < PAGES; page++) {
        uint32_t va = page << 12, pa;
        uint8_t *rd = NULL, *wr = NULL;
        bool unmapped = va >= 0x80000000u && va < 0xC0000000u;
        if (unmapped || !c->has_mmu) {
            pa = unmapped ? va & 0x1FFFFFFFu : va;
            rd = mrc_bus_page_host(c->bus, pa, false);
            wr = mrc_bus_page_host(c->bus, pa, true);
        }
        j->tables.read[page] = rd;
        j->tables.write[page] = wr;
    }
    j->tables_built = true;
    j->has_mmu = c->has_mmu;
}

void mrc_jit_prepare(r3900_jit *j, r3900 *c)
{
    if (j->generation == c->bus->lookup_generation && j->tables_built &&
        j->has_mmu == c->has_mmu)
        return;
    build_tables(j, c);
    j->generation = c->bus->lookup_generation;
    flush(j);
    j->flushes--;                    /* the first build is not a flush */
}

static uint32_t fetch_span(r3900 *c, uint32_t va, const uint8_t **bytes)
{
    uint32_t pa = va >= 0x80000000u && va < 0xC0000000u ? va & 0x1FFFFFFFu : va;
    uint32_t length;
    *bytes = mrc_bus_read_span(c->bus, pa, &length);
    if (!*bytes) return 0;
    /* A physical span can cross the virtual segment edge where the
     * translation changes; never compile across it. */
    uint32_t boundary = va < 0x80000000u ? 0x80000000u :
                        va < 0xA0000000u ? 0xA0000000u : 0xC0000000u;
    if (va < 0xC0000000u && length > boundary - va) length = boundary - va;
    return length;
}

static bool compile(r3900_jit *j, r3900 *c, uint32_t va, mrc_jit_slot *out)
{
    mrc_jit_block b = {.va = va};
    uint32_t length = fetch_span(c, va, &b.guest);
    if (length < 4) return false;
    while (b.n < MRC_JIT_MAX_INSNS && b.n < length / 4) {
        const uint8_t *p = b.guest + b.n * 4;
        uint32_t word = (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 |
                        (uint32_t)p[2] << 8 | p[3];
        mrc_jit_ir *i = &b.ir[b.n];
        if (!mrc_jit_decode_mips(word, i)) break;
        b.words[b.n] = word;
        if (b.branch) {
            /* Only a non-branch may fill the slot. A branch in a delay slot
             * is architecturally undefined; leave it to the reference path. */
            if (mrc_jit_is_branch(i)) break;
            b.delay = true; b.n++; break;
        }
        b.n++;
        if (mrc_jit_is_branch(i)) b.branch = b.n;
        else if (i->op == J_EXEC && i->ends_block) break;
    }
    if (!b.n) return false;
#if HAVE_MMAP
    for (unsigned attempt = 0; attempt < 2; attempt++) {
        size_t start = (j->used + 15) & ~(size_t)15;
        size_t cap = start < ARENA_SIZE ? ARENA_SIZE - start : 0;
        size_t first = 0, last = 0;
        if (!j->dual && cap) {
            /* Open only the window this block can reach, and close it again
             * before anything runs from it. */
            if (cap > 65536) cap = 65536;
            first = start & ~(j->page_size - 1);
            last = (start + cap + j->page_size - 1) & ~(j->page_size - 1);
            if (last > ARENA_SIZE) last = ARENA_SIZE;
            if (mprotect(j->arena + first, last - first, PROT_READ | PROT_WRITE))
                return false;
        }
        unsigned chain = 0;
        size_t size = cap ? mrc_jit_emit(j->writable + start, j->arena + start,
                                         cap, &b, &chain) : 0;
        if (!j->dual && cap &&
            mprotect(j->arena + first, last - first, PROT_READ | PROT_EXEC))
            return false;
        if (size) {
            /* Written through one view, executed through the other: clean
             * and invalidate by both names so no cache level is skipped. */
            if (j->dual)
                __builtin___clear_cache((char *)j->writable + start,
                                        (char *)j->writable + start + size);
            __builtin___clear_cache((char *)j->arena + start,
                                    (char *)j->arena + start + size);
            j->used = start + size;
            j->compiled++;
            out->code = (mrc_jit_code)(j->arena + start);
            out->chain = j->arena + start + chain;
            out->n = b.n;
            return true;
        }
        flush(j);
    }
#endif
    return false;
}

const mrc_jit_entry *mrc_jit_find(r3900_jit *j, r3900 *c, uint32_t va,
                                  bool recompile)
{
    if (j->failed) return NULL;
    unsigned set = (((va >> 2) ^ (va >> 16)) & (SETS - 1)) * WAYS;
    unsigned victim = set;
    for (unsigned k = set; k < set + WAYS; k++) {
        if (j->slot[k].va == va && j->slot[k].code) {
            if (!recompile) return &j->slot[k];
            victim = k;
            break;
        }
        if (!j->slot[k].code) victim = k;
    }
    if (recompile) j->invalidated++;
    else if (j->slot[victim].code) {
        /* Set full: replace the way after the last one used, round robin.
         * Blocks are cheap to recompile and hot sets rarely exceed four. */
        victim = set + (unsigned)(j->clock++ % WAYS);
    }
    /* Native lookups read va and chain without a lock; clear the way
     * before it is rewritten so a half-written entry is never a hit. */
    j->slot[victim].code = NULL;
    j->slot[victim].chain = NULL;
    j->slot[victim].va = va;
    if (!compile(j, c, va, &j->slot[victim])) {
        j->slot[victim].code = NULL;
        j->slot[victim].chain = NULL;
        return NULL;
    }
    return &j->slot[victim];
}

void mrc_jit_resolve_link(r3900_jit *j, r3900 *c, bool stale)
{
    uint8_t *site = j->tables.link;
    j->tables.link = NULL;
    if (!site) return;
    /* The successor must satisfy what the run loop checks before entering
     * native code; a block reached through a link skips that check. */
    if ((c->pc & 3) || c->branch_pending || c->power_stopped ||
        (c->has_mmu && !(c->pc >= 0x80000000u && c->pc < 0xc0000000u &&
                         !(c->cp0[CP0_STATUS] & SR_KUc))))
        return;
    uint64_t flushes = j->flushes;
    const mrc_jit_entry *target = mrc_jit_find(j, c, c->pc, stale);
    if (!target || j->flushes != flushes) return;
    size_t offset = (size_t)(site - j->writable);
    /* Without a separate writable view the branch being redirected sits in
     * executable memory; open its page for the store and close it again. */
#if HAVE_MMAP
    size_t first = offset & ~(j->page_size - 1);
    size_t span = j->page_size * 2 > ARENA_SIZE - first ? ARENA_SIZE - first
                                                        : j->page_size * 2;
    if (!j->dual && mprotect(j->arena + first, span, PROT_READ | PROT_WRITE))
        return;
#endif
    mrc_jit_patch_link(site, j->arena + offset, target->chain);
#if HAVE_MMAP
    if (!j->dual && mprotect(j->arena + first, span, PROT_READ | PROT_EXEC))
        return;
#endif
    __builtin___clear_cache((char *)j->arena + offset, (char *)j->arena + offset + 8);
    j->links++;
}

#if !(defined(__x86_64__) && !defined(_WIN32)) && !(defined(__aarch64__) && !defined(__AARCH64EB__))
size_t mrc_jit_emit(uint8_t *out, const uint8_t *exec, size_t cap,
                    const mrc_jit_block *b, unsigned *chain)
{
    (void)out; (void)exec; (void)cap; (void)b; (void)chain;
    return 0;
}
void mrc_jit_patch_link(uint8_t *site, const uint8_t *site_exec,
                        const uint8_t *target)
{
    (void)site; (void)site_exec; (void)target;
}
bool mrc_jit_host_supported(void) { return false; }
#endif
