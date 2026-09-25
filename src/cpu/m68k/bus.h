#pragma once
/*
 * bus.h -- the machine's bus, its counters and its instruments.
 *
 * The bus owns the instruction clock, coverage set, watchpoint maps and
 * direct page tables shared by the CPU32 interpreter and block engine.
 *
 * The bus accepts byte and word accesses and reports whether anything
 * answered. That `ok` is the whole point: an access nothing decodes
 * is a fact about the board we do not know yet, and it has to be visible
 * rather than quietly reading as zero. Every one is counted, and the first
 * few are printed with the PC that made them, which is how the DataRover's
 * memory map was found and is how this one was.
 */
extern "C" {
#include "core/bus/bus.h"
}
#include "util/portable.h"
#include <cstdio>
#include <cstdint>
#include <map>
#include <set>
#include <vector>
#include <string>
#include <cstring>
#include <cstdlib>

namespace mrc {

struct M68kBus {

    mrc_bus *bus = nullptr;
    FILE *log = stderr;

    uint64_t insns = 0;
    uint64_t unknown_reads = 0, unknown_writes = 0;
    uint64_t ignored_writes = 0;
    uint64_t asserts = 0;
    unsigned exception_logs = 0;
    uint64_t suppressed_exceptions = 0;
    /*
     * Where the machine spends its time, by 64K page.
     *
     * Sampling a program counter twice and seeing the same routine says
     * nothing -- a hot function looks exactly like a stall from two samples,
     * and this session mistook one for the other. A spread over many samples
     * does distinguish them: code that is progressing visits new pages, code
     * that is stuck does not. It is the cheap cousin of the coverage
     * recording that made the MIPS side tractable.
     */
    std::map<uint32_t, uint64_t> pc_pages;
    /*
     * Where the machine writes, by 4K page.
     *
     * A display is the next thing worth finding on this board, and a
     * framebuffer announces itself in write traffic: a contiguous run of
     * pages taking a large share of all stores, rewritten steadily rather
     * than once. Nothing else on a machine this size behaves that way. The
     * same map also shows which parts of RAM are live at all, which is worth
     * having on a board with no memory map beyond what its chip selects say.
     */
    std::map<uint32_t, uint64_t> write_pages;
    bool heat = false;
    /*
     * Coverage, at 256-byte granularity.
     *
     * The MIPS side's most useful technique is diffing which code two runs
     * reach, and this machine has had no equivalent. A plateau is ambiguous
     * without it: ten pages could be a loop walking the same code forever, or
     * work that keeps finding new routines within a narrow area. Recording the
     * blocks actually executed and comparing two runs distinguishes them, and
     * 256 bytes is fine enough to separate routines while staying small
     * enough to dump as text.
     */
    std::set<uint32_t> blocks;
    bool cover = false;
    /*
     * Reads by address, for RAM only.
     *
     * The machine ends up in a closed loop that touches no device, so it is
     * spinning on memory: waiting for a flag that something else was supposed
     * to set. A loop that tight rereads its condition constantly, so the
     * address it waits on is simply the most-read one, by a wide margin.
     * Restricted to RAM because ROM reads are instruction fetches and would
     * swamp it.
     */
    std::map<uint32_t, uint64_t> read_addrs;
    bool readheat = false;
    /*
     * Which instructions this machine actually executes, counted by opcode
     * word. A new CPU core has to decode the whole encoding space to know
     * instruction lengths, but it only has to *execute* what runs, and the
     * order to implement things in is the order this map reports. The
     * equivalent MIPS measurement is what found ADDI to be 29% of a
     * workload after a day of guessing at it.
     */
    std::map<uint16_t, uint64_t> insn_heat_counts;
    /*
     * What runs immediately after a divide. The reference core sets the
     * zero flag after DIVU and DIVS to the opposite of what the
     * architecture says, so whether that matters to these machines is
     * decided by whether anything reads the flag before it is set again.
     * Counting the instruction that follows answers it from the guest
     * rather than from an argument about it.
     */
    std::map<uint16_t, uint64_t> after_divide_counts;
    bool after_divide = false;
    bool insnheat = false;
    /*
     * A data watchpoint: which code reads a given address.
     *
     * --read-heat names the address a spin loop waits on, but not the loop.
     * Going the other way -- from an address to the instructions that touch
     * it -- is what turns "it rereads 0xD010 forever" into a routine that can
     * be disassembled and understood.
     */
    uint32_t watch_read = 0;
    std::map<uint32_t, uint64_t> watch_read_pcs;
    /*
     * The same idea for writes, over a range.
     *
     * The interrupt dispatch tables in low RAM are empty at exit, and no ROM
     * instruction names them with a static address -- the three that do are
     * the handlers reading them. So whatever registers a handler computes the
     * address, and only a runtime watch can catch it. "Nothing ever writes
     * here" and "something writes and it is later cleared" are different
     * findings, and the end state cannot tell them apart.
     */
    uint32_t watch_write_lo = 0, watch_write_hi = 0;
    std::map<uint32_t, uint64_t> watch_write_pcs;
    unsigned watch_write_shown = 0;
    /*
     * PC watchpoints, the same idea as mcap's --watch-pc. Counting how often
     * a named address executes, and printing the registers the first few
     * times, is what turns "the OS asserts" into "these are the ids it did
     * register, and 0x28 is not among them".
     */
    std::map<uint32_t, uint64_t> watch;
    unsigned watch_log = 0;
    /*
     * The last few program counters, dumped when a watchpoint fires.
     *
     * Static disassembly cannot establish how a 68k routine was entered:
     * instructions are variable length, so a decode started at the wrong
     * offset produces plausible code, and it resynchronises within a few
     * instructions -- which means "this alignment reaches the address I care
     * about" is true from almost anywhere and proves nothing. That test was
     * used here to validate a reconstruction and could not have failed.
     *
     * A ring of executed addresses has no such problem: every entry is an
     * instruction the CPU really fetched.
     */
    static const unsigned RING = 96;
    uint32_t ring[RING] = {};
    unsigned ring_at = 0;
    bool     ring_full = false;
    uint64_t sample_every = 0, sample_next = 0;
    unsigned force_irq_level = 0;
    uint64_t force_irq_at = 0;
    bool     force_irq_done = false;
    mutable uint32_t prev_pc = 0;
    /*
     * Where the machine is, for anything this bus reports.
     *
     * A watchpoint that names the instruction making an access needs the
     * active CPU's program counter, so the machine points this at whichever
     * execution engine currently owns the registers.
     */
    const uint32_t *pc_src = nullptr;
    uint32_t at_pc() const { return pc_src ? *pc_src : 0; }
    unsigned ignored_shown = 0;
    std::map<uint32_t, uint64_t> ignored_at;
    uint64_t trace_left = 0;
    uint32_t trace_after_pc = 0;
    uint64_t trace_after_count = 0;
    uint64_t trace_after_hit = 1;
    uint64_t trace_after_seen = 0;
    bool trace_after_armed = false;
    bool     log_unknown = true;
    unsigned unknown_shown = 0;
    bool     stopped = false;
    const char *stop_why = nullptr;

    /* Where unknown accesses happened, so a pattern is visible at exit
     * rather than a wall of individual lines. */
    std::map<uint32_t, uint64_t> unknown_at;

    /* Exceptions by vector, so their cost and frequency is a number rather
     * than an impression. Filled by whichever core takes them. */
    std::map<int, uint64_t> vec_count;
    unsigned frames_shown = 0;

    /*
     * Function codes.
     *
     * A 68010 and later drives three FC pins alongside every access, and
     * `moves` is the instruction that makes them interesting: it substitutes
     * SFC or DFC for the usual supervisor/user encoding, so the same address
     * can name different things. The CPU core records the selected function
     * code with each access.
     *
     * The one place this board is known to care is the reset path, which sets
     * DFC to 7 -- CPU space -- and writes 0x3C0001BB to 0x3FF00. That is the
     * 68340/68349's MBAR, and it is what puts the module space at 0x3C000000.
     * Routed as ordinary data it lands on the boot overlay, which is ROM, and
     * is discarded; the module base then works only because it is hardcoded
     * to the value the ROM would have set. With the function code carried,
     * the address can be derived instead.
     */
    static const unsigned FC_CPU_SPACE = 7;

    /*
     * Direct host pointers for ordinary memory, one per 4 KiB page of the
     * 32-bit address space, rebuilt whenever the bus map generation moves
     * (CS0 overlay retirement, power-on restore, MBAR relocation). A page
     * qualifies only when every byte of it is one uniform host-backed
     * region with no mirror seam inside, exactly the rule the MIPS native
     * engine uses; writes additionally require RAM so ROM writes still
     * reach the bus and are counted as ignored. Accesses that cross a page
     * or land on MMIO, floating or unmapped space take the bus path, so the
     * counters and diagnostics are the same either way. MRC_68K_DIRECT=0
     * disables it for an exact A/B.
     */
    mutable std::vector<uint8_t *> direct_rd, direct_wr;
    mutable uint64_t direct_generation = ~(uint64_t)0;
    mutable uint64_t direct_hits = 0, direct_misses = 0;
    bool direct_enabled = true;

    void direct_prepare() const
    {
        if (MRC_LIKELY(direct_generation == bus->lookup_generation)) return;
        if (direct_rd.empty()) {
            direct_rd.assign(1u << 20, nullptr);
            direct_wr.assign(1u << 20, nullptr);
        }
        for (uint32_t page = 0; page < (1u << 20); page++) {
            direct_rd[page] = mrc_bus_page_host(bus, page << 12, false);
            direct_wr[page] = mrc_bus_page_host(bus, page << 12, true);
        }
        direct_generation = bus->lookup_generation;
        if (getenv("MRC_68K_DIRECT_DEBUG")) {
            unsigned rd = 0, wr = 0;
            for (uint32_t page = 0; page < (1u << 20); page++) {
                rd += direct_rd[page] != nullptr;
                wr += direct_wr[page] != nullptr;
            }
            fprintf(log, "68k: direct tables rebuilt at generation %llu: "
                    "%u readable, %u writable pages\n",
                    (unsigned long long)direct_generation, rd, wr);
            mrc_bus_print_map(bus, log);
        }
    }

    static inline uint32_t load_be(const uint8_t *p, unsigned size)
    {
        if (size == 4) return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 |
                              (uint32_t)p[2] << 8 | p[3];
        if (size == 2) return (uint32_t)p[0] << 8 | p[1];
        return p[0];
    }
    static inline void store_be(uint8_t *p, unsigned size, uint32_t v)
    {
        if (size == 4) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
                         p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; return; }
        if (size == 2) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; return; }
        p[0] = (uint8_t)v;
    }

    uint32_t rd(uint32_t addr, unsigned size) const
    {
        auto *self = const_cast<M68kBus *>(this);
        bool ok = true;
        if (MRC_LIKELY(direct_enabled && !self->watch_read && !self->readheat)) {
            direct_prepare();
            const uint8_t *p = direct_rd[addr >> 12];
            unsigned off = addr & 0xFFFu;
            if (MRC_LIKELY(p && off + size <= 4096)) {
                self->bus->reads++;
                self->direct_hits++;
                return load_be(p + off, size);
            }
            self->direct_misses++;
        }
        if (self->watch_read && addr >= self->watch_read &&
            addr < self->watch_read + 4)
            self->watch_read_pcs[self->at_pc()]++;
        if (self->readheat && addr < 0x01000000u &&
            self->read_addrs.size() < 200000)
            self->read_addrs[addr]++;
        uint32_t v = mrc_bus_read(self->bus, addr, size, &ok);
        if (!ok) {
            self->unknown_reads++;
            self->unknown_at[addr & ~0xFFFu]++;
            if (self->log_unknown && self->unknown_shown < 24) {
                self->unknown_shown++;
                fprintf(self->log, "[68k] read%u  from %08X, nothing decodes it"
                        "  (pc=%08X)\n", size * 8, addr, self->at_pc());
            }
            /* Nothing drives the bus: it floats high. Same choice as the
             * MIPS side, and for the same reason -- a made-up zero is a lie
             * that software may believe. */
            return size == 4 ? 0xFFFFFFFFu : size == 1 ? 0xFFu : 0xFFFFu;
        }
        return v;
    }

    void wr(uint32_t addr, unsigned size, uint32_t val) const
    {
        auto *self = const_cast<M68kBus *>(this);
        bool ok = true;
        if (MRC_LIKELY(direct_enabled && !self->heat && !self->watch_write_hi)) {
            direct_prepare();
            uint8_t *p = direct_wr[addr >> 12];
            unsigned off = addr & 0xFFFu;
            if (MRC_LIKELY(p && off + size <= 4096)) {
                self->bus->writes++;
                store_be(p + off, size, val);
                return;
            }
        }
        /*
         * A write to ROM succeeds as far as the caller is concerned -- the
         * hardware ignores it and so do we -- but the bus counts it. Watching
         * that counter is the only way to tell an ignored write from a real
         * one, and without it a run can report zero undecoded accesses while
         * quietly throwing away half a million writes.
         */
        if (self->heat) self->write_pages[addr & ~0xFFFu] += size;
        if (self->watch_write_hi && addr >= self->watch_write_lo &&
            addr < self->watch_write_hi) {
            self->watch_write_pcs[self->at_pc()]++;
            if (self->watch_write_shown < 32) {
                self->watch_write_shown++;
                fprintf(self->log, "[watch-write] +%llu W%u %08X = %0*X "
                        "(pc=%08X)\n", (unsigned long long)self->insns,
                        size * 8, addr, (int)size * 2, val, self->at_pc());
            }
        }
        uint64_t before = self->bus->faults;
        mrc_bus_write(self->bus, addr, size, val, &ok);
        if (ok && self->bus->faults != before) {
            self->ignored_writes++;
            self->ignored_at[addr & ~0xFFFFFu]++;
            if (self->log_unknown && self->ignored_shown < 12) {
                self->ignored_shown++;
                fprintf(self->log, "[68k] write%u to %08X = %0*X went to ROM "
                        "and was ignored  (pc=%08X)\n", size * 8, addr,
                        (int)size * 2, val, self->at_pc());
            }
        }
        if (!ok) {
            self->unknown_writes++;
            self->unknown_at[addr & ~0xFFFu]++;
            if (self->log_unknown && self->unknown_shown < 24) {
                self->unknown_shown++;
                fprintf(self->log, "[68k] write%u to %08X = %0*X, nothing "
                        "decodes it  (pc=%08X)\n", size * 8, addr,
                        (int)size * 2, val, self->at_pc());
            }
        }
    }

    /*
     * CPU space is a separate address space, not a region of memory, so it
     * does not go through the bus at all. Only one address in it is known to
     * be used here; anything else is reported rather than absorbed, because
     * an unknown CPU-space access is a fact about the board.
     */
    virtual uint32_t cpu_space_read(uint32_t addr, unsigned size) const = 0;
    virtual void cpu_space_write(uint32_t addr, unsigned size, uint32_t val) const = 0;
    virtual ~M68kBus() = default;
};

} /* namespace */
