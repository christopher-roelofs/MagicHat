#include "cpu/mips/r3900.h"
#include "jit/jit.h"
#include "util/portable.h"

#include <inttypes.h>
#include <string.h>
#include <stdlib.h>

#define RS(i)   (((i) >> 21) & 0x1F)
#define RT(i)   (((i) >> 16) & 0x1F)
#define RD(i)   (((i) >> 11) & 0x1F)
#define SA(i)   (((i) >>  6) & 0x1F)
#define FN(i)   ((i) & 0x3F)
#define OP(i)   ((i) >> 26)
#define IMM(i)  ((uint32_t)((i) & 0xFFFF))
#define SIMM(i) ((uint32_t)(int32_t)(int16_t)((i) & 0xFFFF))

#if defined(__GNUC__) || defined(__clang__)
/* Keep rare exception machinery out of the flattened instruction loop. */
__attribute__((cold, noinline))
#endif
static void raise_exc(r3900 *c, unsigned code, uint32_t bad_vaddr,
                      bool have_bad);
static void raise_exc_refill(r3900 *c, unsigned code, uint32_t bad_vaddr);

void mh_cpu_init(r3900 *c, mh_bus *bus)
{
    memset(c, 0, sizeof(*c));
    c->bus = bus;
    c->log = stderr;
    c->has_mmu = true;      /* the architectural default; boards override it */
}

void mh_cpu_reset(r3900 *c, uint32_t reset_pc)
{
    memset(c->r, 0, sizeof(c->r));
    c->hi = c->lo = 0;
    c->pc = reset_pc;
    c->next_pc = reset_pc + 4;
    c->cur_pc = reset_pc;
    c->branch_pending = c->in_delay = false;
    c->halted = false;
    c->power_stopped = false;
    c->irq_lines = 0;
    c->insn_count = c->cycle_count = 0;

    memset(c->cp0, 0, sizeof(c->cp0));
    /*
     * Reset state per MIPS-I: BEV=1 (exception vectors in uncached ROM
     * space), interrupts disabled, kernel mode.
     *
     * PRId is a board-visible value. The Magic Cap IDT monitor prints
     * "Toshiba Core - id: 0x2200"; we default to that but it is settable,
     * because the old project fed 0x2200 in and then read its own value
     * back out of the banner, which proves nothing. See docs/ACCURACY.md.
     */
    c->cp0[CP0_STATUS] = SR_BEV;
    c->cp0[CP0_PRID]   = 0x00002200;
    c->cp0[CP0_RANDOM] = R3900_TLB_ENTRIES - 1;
}

void mh_cpu_set_irq(r3900 *c, unsigned line, bool asserted)
{
    if (line > 7)
        return;
    if (asserted) {
        c->irq_lines |= (uint8_t)(1u << line);
        c->irq_ever_asserted |= (uint8_t)(1u << line);
    }
    else
        c->irq_lines &= (uint8_t)~(1u << line);
}

/* ------------------------------------------------------------------ TLB */

static bool tlb_lookup(r3900 *c, uint32_t va, bool write, uint32_t *pa,
                       unsigned *exc, bool *refill)
{
    *refill = false;
    uint32_t vpn  = va & 0xFFFFF000u;
    uint8_t  asid = (uint8_t)(c->cp0[CP0_ENTRYHI] & 0xFF);

    for (unsigned i = 0; i < R3900_TLB_ENTRIES; i++) {
        r3900_tlb_entry *e = &c->tlb[i];
        if ((e->hi & 0xFFFFF000u) != vpn)
            continue;
        bool global = (e->lo & 0x100) != 0;      /* EntryLo.G */
        if (!global && (e->hi & 0xFF) != asid)
            continue;
        if (!(e->lo & 0x200)) {                  /* EntryLo.V */
            *exc = write ? EXC_TLBS : EXC_TLBL;
            return false;                        /* a hit, so not a refill */
        }
        if (write && !(e->lo & 0x400)) {         /* EntryLo.D */
            *exc = EXC_Mod;
            return false;
        }
        *pa = (e->lo & 0xFFFFF000u) | (va & 0xFFFu);
        return true;
    }
    *exc = write ? EXC_TLBS : EXC_TLBL;
    *refill = true;
    return false;
}

bool mh_cpu_translate(r3900 *c, uint32_t va, bool write, uint32_t *pa)
{
    unsigned exc;
    bool refill;
    if (va >= 0x80000000u && va < 0xC0000000u) {
        *pa = va & 0x1FFFFFFFu;             /* kseg0 / kseg1 */
        return true;
    }
    if (!c->has_mmu) {
        *pa = va;
        return true;
    }
    return tlb_lookup(c, va, write, pa, &exc, &refill);
}

static bool translate(r3900 *c, uint32_t va, bool write, uint32_t *pa)
{
    unsigned exc = 0;
    bool refill = false;

    /* DataRover's TX39 runs without a TLB. Keep that common case as one
     * short segment-map branch instead of carrying the MMU checks through
     * every load, store, and instruction fetch. */
    if (MH_LIKELY(!c->has_mmu)) {
        *pa = (va >= 0x80000000u && va < 0xC0000000u)
            ? va & 0x1FFFFFFFu : va;
        return true;
    }

    /* kseg0 (cached) and kseg1 (uncached) are unmapped windows onto the
     * bottom 512 MB of physical space. We do not model the cache, so they
     * behave identically here. */
    if (va >= 0x80000000u) {
        /*
         * On a part with an MMU, user mode may not leave kuseg. On one
         * without, there is nothing to enforce that with — the segmentation
         * check and the translation it guards are the same piece of
         * hardware, and this part has neither.
         *
         * Magic Cap depends on it. It runs with Status.KUc set (user mode)
         * and still reads the RTC at kseg1 0xB0C00144, from 0x13CBF994. On
         * a machine that enforced the check that access could not work, and
         * the OS would not be written that way.
         */
        if (c->has_mmu && (c->cp0[CP0_STATUS] & SR_KUc)) {
            raise_exc(c, write ? EXC_AdES : EXC_AdEL, va, true);
            return false;
        }
        if (va < 0xC0000000u) {
            *pa = va & 0x1FFFFFFFu;
            return true;
        }
        /* kseg2/kseg3 are mapped; fall through to the TLB. */
    }

    /* No MMU: everything that is not kseg0/kseg1 maps straight through. */
    if (tlb_lookup(c, va, write, pa, &exc, &refill))
        return true;

    /* Record the TLB refill context the handler expects. */
    c->cp0[CP0_CONTEXT] = (c->cp0[CP0_CONTEXT] & ~0x007FFFF0u) |
                          ((va >> 10) & 0x001FFFFCu);
    c->cp0[CP0_ENTRYHI] = (c->cp0[CP0_ENTRYHI] & 0xFFu) | (va & 0xFFFFF000u);
    if (refill)
        raise_exc_refill(c, exc, va);
    else
        raise_exc(c, exc, va, true);
    return false;
}

/* ----------------------------------------------------------- exceptions */

static void raise_exc(r3900 *c, unsigned code, uint32_t bad_vaddr,
                      bool have_bad)
{
    uint32_t sr = c->cp0[CP0_STATUS];
    c->exc_count++;

    if (c->log_exceptions) {
        static const char *const name[] = {
            "Int", "Mod", "TLBL", "TLBS", "AdEL", "AdES", "IBE", "DBE",
            "Sys", "Bp",  "RI",   "CpU",  "Ov",   "Tr",   "?14", "?15",
        };
        c->log_exceptions--;
        fprintf(c->log, "[exc] %-4s pc=%08X%s", name[code & 15], c->cur_pc,
                c->in_delay ? " (delay slot)" : "");
        if (have_bad)
            fprintf(c->log, " bad=%08X", bad_vaddr);
        fprintf(c->log, " sr=%08X ra=%08X sp=%08X\n", sr, c->r[31], c->r[29]);
    }

    /* EPC points at the branch when the faulting instruction is in a slot. */
    if (c->in_delay) {
        c->cp0[CP0_EPC] = c->cur_pc - 4;
        c->cp0[CP0_CAUSE] |= CAUSE_BD;
    } else {
        c->cp0[CP0_EPC] = c->cur_pc;
        c->cp0[CP0_CAUSE] &= ~CAUSE_BD;
    }

    c->cp0[CP0_CAUSE] = (c->cp0[CP0_CAUSE] & ~CAUSE_EXCCODE_MASK) |
                        ((code << CAUSE_EXCCODE_SHIFT) & CAUSE_EXCCODE_MASK);

    if (have_bad)
        c->cp0[CP0_BADVADDR] = bad_vaddr;

    /* MIPS-I: shift the KU/IE stack left, entering kernel mode with
     * interrupts disabled. */
    c->cp0[CP0_STATUS] = (sr & ~0x3Fu) | ((sr << 2) & 0x3Cu);

    uint32_t base = (sr & SR_BEV) ? 0xBFC00100u : 0x80000000u;

    c->pc      = base + (c->exc_refill ? 0x000u : 0x080u);
    c->next_pc = c->pc + 4;
    c->branch_pending = false;
}

static void raise_exc_refill(r3900 *c, unsigned code, uint32_t bad_vaddr)
{
    c->exc_refill = true;
    raise_exc(c, code, bad_vaddr, true);
    c->exc_refill = false;
}

/* Returns true when an interrupt was taken, in which case no instruction
 * runs this step: the exception replaces it. */
/*
 * Replay: another machine's device answers and interrupt level, played into
 * this one. Device reads return what that machine's devices said, device
 * writes are checked against what it wrote and go no further, and the
 * hardware interrupt lines follow its recorded level. Then this core runs
 * that machine's program, and the first access that is not the next one in
 * the trace is where the two cores part company -- or, if none is, this
 * core reproduces whatever that machine did, here, where it can be read.
 */
typedef struct { uint32_t insn, addr, value, flags; } replay_rec;
static replay_rec *g_rep;      static size_t g_rep_n, g_rep_i;
static uint32_t  (*g_rlv)[2];  static size_t g_rlv_n, g_rlv_i;
static bool       g_rep_bad;
static uint32_t  *g_take;      static size_t g_take_n, g_take_i;

static bool bus_is_device(uint32_t pa);

void mh_cpu_replay(const char *dev_path, const char *irq_path)
{
    FILE *f = fopen(dev_path, "rb");
    if (!f) { fprintf(stderr, "replay: cannot open %s\n", dev_path); return; }
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 16, SEEK_SET);
    g_rep_n = (size_t)(n - 16) / sizeof(replay_rec);
    g_rep = malloc(g_rep_n * sizeof(replay_rec));
    g_rep_n = fread(g_rep, sizeof(replay_rec), g_rep_n, f);
    fclose(f);
    if (irq_path && (f = fopen(irq_path, "rb"))) {
        fseek(f, 0, SEEK_END); n = ftell(f); fseek(f, 0, SEEK_SET);
        g_rlv_n = (size_t)n / 8;
        g_rlv = malloc(g_rlv_n * 8);
        g_rlv_n = fread(g_rlv, 8, g_rlv_n, f);
        fclose(f);
    }
    fprintf(stderr, "replay: %zu device accesses, %zu interrupt level changes\n",
            g_rep_n, g_rlv_n);
}

/* The other machine's interrupts, each as the count of the instruction it
 * was taken in front of. With these, an interrupt is taken here exactly
 * there and nowhere else; the level trace then only says what Cause reads. */
void mh_cpu_replay_takes(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "replay: cannot open %s\n", path); return; }
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    g_take_n = (size_t)n / 4;
    g_take = malloc(g_take_n * 4);
    g_take_n = fread(g_take, 4, g_take_n, f);
    fclose(f);
    fprintf(stderr, "replay: %zu interrupt takes\n", g_take_n);
}

static bool replay_access(r3900 *c, uint32_t pa, unsigned size, bool wr,
                          uint32_t *val)
{
    if (!g_rep || g_rep_bad || !bus_is_device(pa)) return false;
    if (g_rep_i >= g_rep_n) {
        fprintf(stderr, "replay: past the end of the trace at insn %llu\n",
                (unsigned long long)c->insn_count);
        g_rep_bad = true; c->halted = true; return true;
    }
    const replay_rec *r = &g_rep[g_rep_i];
    uint32_t mask = (size >= 4) ? 0xFFFFFFFFu : ((1u << (size * 8)) - 1u);
    bool rwr = (r->flags & 0x100u) != 0;
    if (r->addr != pa || rwr != wr || (r->flags & 0xFFu) != size ||
        (wr && r->value != (*val & mask))) {
        fprintf(stderr, "replay: diverged at access %zu, insn %llu (trace insn %u), pc %08X:\n"
                "  trace %s%u %08X = %08X\n  here  %s%u %08X = %08X\n",
                g_rep_i, (unsigned long long)c->insn_count, r->insn, c->cur_pc,
                rwr ? "W" : "R", (r->flags & 0xFFu) * 8, r->addr, r->value,
                wr ? "W" : "R", size * 8, pa, wr ? (*val & mask) : 0);
        g_rep_bad = true; c->halted = true; return true;
    }
    /* The two count instructions the same way, or the takes land wrong. */
    static int drift_shown; static int32_t last_drift;
    int32_t drift = (int32_t)(c->insn_count - r->insn);
    if (drift != last_drift && drift_shown < 12) {
        fprintf(stderr, "replay: drift %+d at access %zu (trace insn %u, pc %08X, last take %u)\n",
                drift, g_rep_i, r->insn, c->cur_pc, g_take_i ? g_take[g_take_i - 1] : 0);
        last_drift = drift; drift_shown++;
    }
    if (!wr) *val = r->value;
    g_rep_i++;
    return true;
}

/* When each interrupt was taken, and on which lines. A second
 * implementation cannot reproduce this from the devices alone -- its own
 * clock decides when a level goes up -- so to run the two in step through
 * anything interrupt-driven, it has to be told. */
static FILE *g_irq_trace;

static bool check_interrupts(r3900 *c)
{
    uint32_t cause = c->cp0[CP0_CAUSE];

    /* Hardware lines live in Cause.IP[7:2]; software bits IP[1:0] are set by
     * the OS writing Cause directly and are preserved.
     *
     * This runs on every instruction, so only store when something actually
     * changed — the lines are steady for millions of instructions at a time
     * and an unconditional store to the CP0 array was pure cost. */
    /* The hardware lines' level, every time it changes. When the interrupt
     * was taken is not enough to reproduce it elsewhere: the handler reads
     * Cause afterwards and dispatches on what it finds, so the line has to
     * stay up exactly as long as it did here. */
    if (g_irq_trace) {
        static uint32_t last = 0xFFFFFFFFu;
        uint32_t lv = c->irq_lines & 0xFC;
        if (lv != last) {
            uint32_t rec[2] = { (uint32_t)(c->insn_count & 0xFFFFFFFFu), lv };
            fwrite(rec, sizeof rec, 1, g_irq_trace);
            last = lv;
        }
    }
    if (g_rlv) {
        while (g_rlv_i + 1 < g_rlv_n && g_rlv[g_rlv_i + 1][0] <= c->insn_count)
            g_rlv_i++;
        c->irq_lines = (uint8_t)(g_rlv[g_rlv_i][0] <= c->insn_count
                                 ? (g_rlv[g_rlv_i][1] & 0xFC) : 0);
    }
    uint32_t want = (cause & ~0xFC00u) | ((uint32_t)(c->irq_lines & 0xFC) << 8);
    if (want != cause) {
        cause = want;
        c->cp0[CP0_CAUSE] = cause;
    }

    uint32_t sr = c->cp0[CP0_STATUS];
    uint8_t im = (uint8_t)((sr & SR_IM_MASK) >> SR_IM_SHIFT);
    if (im & (uint8_t)~c->im_ever_enabled)
        c->im_ever_enabled |= im;

    if (!(sr & SR_IEc))
        return false;

    uint32_t pending = (cause & CAUSE_IP_MASK) & (sr & SR_IM_MASK);
    if (g_take) {
        while (g_take_i < g_take_n && g_take[g_take_i] < c->insn_count) {
            fprintf(stderr, "replay: take at insn %u missed (now %llu, sr %08X cause %08X)\n",
                    g_take[g_take_i], (unsigned long long)c->insn_count, sr, cause);
            g_take_i++;
        }
        if (g_take_i >= g_take_n || g_take[g_take_i] != c->insn_count)
            return false;
        g_take_i++;
        if (!pending) pending = 0x1000u;    /* IP4: what this board drives */
    } else if (!pending)
        return false;

    c->irqs_taken++;
    raise_exc(c, EXC_Int, 0, false);
    return true;
}

/* ------------------------------------------------------------ memory ops */

/*
 * Data-bus trace, the companion to the architectural trace above.
 *
 * Validating a second implementation of this core against this one needs
 * more than the register state: the other core has no devices, and cannot
 * grow them before its CPU works. Recording every data access here lets it
 * replay reads and have its writes checked, so a CPU can be brought up
 * against the real ROM with no peripheral models at all.
 *
 * Instruction fetches are not recorded because the architectural trace
 * already carries the instruction word for every retired address.
 */
#define BUS_TRACE_MAGIC 0x53554254u   /* "TBUS" */

static FILE    *g_bus_trace;
static uint64_t g_bus_trace_left;
/* Devices only. A full trace of a boot is gigabytes and most of it is RAM;
 * what a second implementation needs to stay in step is what the devices
 * said, because that is what it cannot work out for itself. */
static bool     g_bus_trace_dev;

void mh_cpu_trace_bus_devices_only(bool on) { g_bus_trace_dev = on; }

void mh_cpu_trace_irq(const char *path)
{
    if (g_irq_trace) { fclose(g_irq_trace); g_irq_trace = NULL; }
    if (path) g_irq_trace = fopen(path, "wb");
}
void mh_cpu_trace_irq_close(void)
{
    if (g_irq_trace) { fclose(g_irq_trace); g_irq_trace = NULL; }
}

/* The device windows of this board: the TX39 block, the two card
 * controllers and their windows, and the kseg3 device. */
static bool bus_is_device(uint32_t pa)
{
    return (pa >= 0x10400000u && pa <  0x10C00400u)
        || (pa >= 0x08000000u && pa <  0x10000000u)
        || (pa >= 0x24000000u && pa <  0x2C000000u)
        || (pa >= 0xFF000000u && pa <  0xFF001000u);
}

void mh_cpu_trace_bus(const char *path, uint64_t count)
{
    if (g_bus_trace) {
        fclose(g_bus_trace);
        g_bus_trace = NULL;
    }
    g_bus_trace_left = 0;
    if (!path || !count)
        return;
    g_bus_trace = fopen(path, "wb");
    if (!g_bus_trace) {
        fprintf(stderr, "trace-bus: cannot write %s\n", path);
        return;
    }
    uint32_t hdr[4] = { BUS_TRACE_MAGIC, 1u, 16u, 0u };
    fwrite(hdr, sizeof hdr, 1, g_bus_trace);
    g_bus_trace_left = count;
}

void mh_cpu_trace_bus_close(void)
{
    if (!g_bus_trace)
        return;
    fclose(g_bus_trace);
    g_bus_trace = NULL;
    g_bus_trace_left = 0;
}

/*
 * One record per data access, in program order, tagged with the instruction
 * count so a divergence can be located in the architectural trace.
 *
 * Accesses that *fail* are recorded too, flagged. A second implementation
 * cannot reproduce a bus error on its own -- it has no bus -- so without
 * this its DBE and IBE paths could never be exercised against this one.
 */
#define BUS_F_WRITE  0x100u
#define BUS_F_ERROR  0x200u
#define BUS_F_IFETCH 0x400u

static void bus_trace_emit(const r3900 *c, uint32_t pa, unsigned size,
                           uint32_t val, uint32_t flags)
{
    /* Narrowed to the access width. A store passes the whole register down
     * and the bus takes the bytes it needs; recording the untruncated value
     * would make the record say something the bus never saw. */
    if (g_bus_trace_dev && !bus_is_device(pa))
        return;
    uint32_t mask = (size >= 4) ? 0xFFFFFFFFu : ((1u << (size * 8)) - 1u);
    uint32_t rec[4];
    rec[0] = (uint32_t)(c->insn_count & 0xFFFFFFFFu);
    rec[1] = pa;
    rec[2] = val & mask;
    rec[3] = (size & 0xFFu) | flags;
    fwrite(rec, sizeof rec, 1, g_bus_trace);
    if (--g_bus_trace_left == 0)
        mh_cpu_trace_bus_close();
}

static bool load(r3900 *c, uint32_t va, unsigned size, uint32_t *out)
{
    uint32_t pa;
    if (va & (size - 1)) {
        raise_exc(c, EXC_AdEL, va, true);
        return false;
    }
    if (!translate(c, va, false, &pa))
        return false;
    bool ok;
    if (MH_UNLIKELY(g_rep != NULL) && replay_access(c, pa, size, false, out))
        return !g_rep_bad;
    *out = mh_bus_read(c->bus, pa, size, &ok);
    if (!ok) {
        if (MH_UNLIKELY(g_bus_trace != NULL))
            bus_trace_emit(c, pa, size, 0, BUS_F_ERROR);
        raise_exc(c, EXC_DBE, va, true);
        return false;
    }
    if (MH_UNLIKELY(g_bus_trace != NULL))
        bus_trace_emit(c, pa, size, *out, 0);
    return true;
}

static uint32_t g_watch_write;
static unsigned g_watch_write_log;

void mh_cpu_watch_write(uint32_t pa)
{
    g_watch_write = pa;
    g_watch_write_log = pa ? 1000000u : 0;
}

/*
 * Architectural state trace, for comparing this core against another
 * implementation of the same part.
 *
 * --trace prints the fetch stream, which answers "what did it run" but not
 * "did it get the same answer". An RTL core is only validated by the second
 * question, so this writes the state after every retired instruction. Text
 * is not an option at these lengths -- a million instructions is already a
 * large file -- so records are fixed-size little-endian binary, described by
 * the header below and by tools/goldtrace.h in the consuming project.
 *
 * Kept out of the CPU struct so saved states stay layout-compatible, for the
 * same reason g_watch_write is a file static.
 */
#define TRACE_STATE_MAGIC   0x54435254u   /* "TRCT" */
#define TRACE_STATE_VERSION 1u
#define TRACE_STATE_WORDS   36u           /* pc, insn, next_pc, hi, lo, r1..r31 */

static FILE    *g_trace_state;
static uint64_t g_trace_state_left;

void mh_cpu_trace_state(const char *path, uint64_t count)
{
    if (g_trace_state) {
        fclose(g_trace_state);
        g_trace_state = NULL;
    }
    g_trace_state_left = 0;
    if (!path || !count)
        return;
    g_trace_state = fopen(path, "wb");
    if (!g_trace_state) {
        fprintf(stderr, "trace-state: cannot write %s\n", path);
        return;
    }
    uint32_t hdr[4] = { TRACE_STATE_MAGIC, TRACE_STATE_VERSION,
                        TRACE_STATE_WORDS * 4u, 0u };
    fwrite(hdr, sizeof hdr, 1, g_trace_state);
    g_trace_state_left = count;
}

void mh_cpu_trace_state_close(void)
{
    if (!g_trace_state)
        return;
    fclose(g_trace_state);
    g_trace_state = NULL;
    g_trace_state_left = 0;
}

/* Called with the instruction retired, after its writeback. */
static void trace_state_emit(const r3900 *c, uint32_t insn)
{
    uint32_t rec[TRACE_STATE_WORDS];
    rec[0] = c->cur_pc;
    rec[1] = insn;
    rec[2] = c->next_pc;
    rec[3] = c->hi;
    rec[4] = c->lo;
    memcpy(&rec[5], &c->r[1], 31 * sizeof(uint32_t));
    fwrite(rec, sizeof rec, 1, g_trace_state);
    if (--g_trace_state_left == 0)
        mh_cpu_trace_state_close();
}

static bool store(r3900 *c, uint32_t va, unsigned size, uint32_t val)
{
    uint32_t pa;
    if (va & (size - 1)) {
        raise_exc(c, EXC_AdES, va, true);
        return false;
    }
    if (!translate(c, va, true, &pa))
        return false;
    if (g_watch_write && g_watch_write_log && (pa & ~3u) == (g_watch_write & ~3u)) {
        g_watch_write_log--;
        fprintf(c->log, "[watch-write] %08X <- %0*X (size %u) at pc=%08X ra=%08X +%llu\n",
                va, (int)size * 2, val, size, c->cur_pc, c->r[31], (unsigned long long)c->insn_count);
    }
    bool ok;
    if (MH_UNLIKELY(g_rep != NULL) && replay_access(c, pa, size, true, &val))
        return !g_rep_bad;
    mh_bus_write(c->bus, pa, size, val, &ok);
    if (!ok) {
        if (MH_UNLIKELY(g_bus_trace != NULL))
            bus_trace_emit(c, pa, size, val, BUS_F_WRITE | BUS_F_ERROR);
        raise_exc(c, EXC_DBE, va, true);
        return false;
    }
    if (MH_UNLIKELY(g_bus_trace != NULL))
        bus_trace_emit(c, pa, size, val, BUS_F_WRITE);
    return true;
}

static inline void set_reg(r3900 *c, unsigned n, uint32_t v)
{
    if (n)
        c->r[n] = v;
}

/* ---------------------------------------------------------------- CP0 */

static uint32_t read_cp0(r3900 *c, unsigned n)
{
    switch (n) {
    case CP0_COUNT:
        /* One Count tick per two instructions is a placeholder, not a
         * measurement. Real cycle accounting is an
         * open item; see docs/ACCURACY.md. */
        return (uint32_t)(c->cycle_count >> 1);
    case CP0_RANDOM: {
        uint32_t wired = 0;
        uint32_t span  = R3900_TLB_ENTRIES - wired;
        return wired + (uint32_t)(c->insn_count % span);
    }
    default:
        return c->cp0[n];
    }
}

static void write_cp0(r3900 *c, unsigned n, uint32_t v)
{
    switch (n) {
    case CP0_RANDOM:
    case CP0_PRID:
    case CP0_BADVADDR:
        return;                      /* read-only */
    case CP0_CAUSE:
        /* Only the two software interrupt bits are writable. */
        c->cp0[CP0_CAUSE] = (c->cp0[CP0_CAUSE] & ~0x300u) | (v & 0x300u);
        return;
    case CP0_INDEX:
        if ((v & 0x3F) >= R3900_TLB_ENTRIES && c->on_trap)
            c->on_trap(c, "TLB Index beyond modelled entry count", v);
        c->cp0[n] = v;
        return;
    case CP0_COUNT:
        c->cycle_count = (uint64_t)v << 1;
        return;
    case CP0_STATUS:
        /*
         * Status.PE is a hardware latch, not a stored bit: the cache sets it
         * on a parity error and software clears it by writing a one. We model
         * no cache and so never set it, which means it must always read back
         * as zero.
         *
         * The ROM's very first instructions (0x83C0001C) write 0x00100000 to
         * Status to clear PE. Storing that bit verbatim makes the monitor
         * print "Parity ERROR detected" after every character it transmits.
         */
        c->cp0[CP0_STATUS] = v & ~SR_PE;
        return;
    default:
        c->cp0[n] = v;
        return;
    }
}

static void cop0_op(r3900 *c, uint32_t insn)
{
    switch (FN(insn)) {
    case 0x01:  /* TLBR */
        c->tlb_used = true;
        {
            unsigned i = (c->cp0[CP0_INDEX] >> 8) & (R3900_TLB_ENTRIES - 1);
            c->cp0[CP0_ENTRYHI] = c->tlb[i].hi;
            c->cp0[CP0_ENTRYLO] = c->tlb[i].lo;
        }
        return;
    case 0x02:  /* TLBWI */
        c->tlb_used = true;
        {
            unsigned i = (c->cp0[CP0_INDEX] >> 8) & (R3900_TLB_ENTRIES - 1);
            c->tlb[i].hi = c->cp0[CP0_ENTRYHI];
            c->tlb[i].lo = c->cp0[CP0_ENTRYLO];
        }
        return;
    case 0x06:  /* TLBWR */
        c->tlb_used = true;
        {
            unsigned i = read_cp0(c, CP0_RANDOM) & (R3900_TLB_ENTRIES - 1);
            c->tlb[i].hi = c->cp0[CP0_ENTRYHI];
            c->tlb[i].lo = c->cp0[CP0_ENTRYLO];
        }
        return;
    case 0x08:  /* TLBP */
        c->tlb_used = true;
        {
            uint32_t vpn  = c->cp0[CP0_ENTRYHI] & 0xFFFFF000u;
            uint8_t  asid = (uint8_t)(c->cp0[CP0_ENTRYHI] & 0xFF);
            c->cp0[CP0_INDEX] = 0x80000000u;
            for (unsigned i = 0; i < R3900_TLB_ENTRIES; i++) {
                bool global = (c->tlb[i].lo & 0x100) != 0;
                if ((c->tlb[i].hi & 0xFFFFF000u) == vpn &&
                    (global || (c->tlb[i].hi & 0xFF) == asid)) {
                    c->cp0[CP0_INDEX] = i << 8;
                    break;
                }
            }
        }
        return;
    case 0x10:  /* RFE — MIPS-I return from exception */
        c->cp0[CP0_STATUS] = (c->cp0[CP0_STATUS] & ~0x0Fu) |
                             ((c->cp0[CP0_STATUS] >> 2) & 0x0Fu);
        return;
    default:
        if (c->on_trap)
            c->on_trap(c, "unimplemented COP0 function", insn);
        raise_exc(c, EXC_RI, 0, false);
        return;
    }
}

/* ---------------------------------------------------------------- exec */

static void branch(r3900 *c, uint32_t target)
{
    c->next_pc = target;
    c->branch_pending = true;
}

/* Instantiate the same semantics with raw and predecoded operands. */
#define SPECIAL_OP(n) (n)
#define DISPATCH(i) OP(i)
#define MH_INSN uint32_t
#define WORD(i) (i)
#define JTARGET(i) (((i) & 0x03FFFFFFu) << 2)
#include "cpu/mips/execute.inc"
#undef MH_INSN
#undef WORD
#undef JTARGET

typedef struct {
    uint32_t word, simm, jump;
    uint16_t imm;
    uint8_t rs, rt, rd, sa, fn, op, dispatch;
} decoded_instruction;

static decoded_instruction decode(uint32_t word)
{
    return (decoded_instruction){word, SIMM(word), (word & 0x03ffffffu) << 2,
        IMM(word), RS(word), RT(word), RD(word), SA(word), FN(word), OP(word),
        OP(word) ? OP(word) : 64 + FN(word)};
}

#undef RS
#undef RT
#undef RD
#undef SA
#undef FN
#undef OP
#undef IMM
#undef SIMM
#define RS(i) ((i)->rs)
#define RT(i) ((i)->rt)
#define RD(i) ((i)->rd)
#define SA(i) ((i)->sa)
#define FN(i) ((i)->fn)
#define OP(i) ((i)->op)
#define IMM(i) ((uint32_t)(i)->imm)
#define SIMM(i) ((i)->simm)
#define WORD(i) ((i)->word)
#define JTARGET(i) ((i)->jump)
#undef SPECIAL_OP
#undef DISPATCH
#define SPECIAL_OP(n) (64 + (n))
#define DISPATCH(i) ((i)->dispatch)
#define MH_FLAT_DISPATCH
#define MH_INSN const decoded_instruction *
#define branch_rel decoded_branch_rel
#define exec_special decoded_exec_special
#define exec_regimm decoded_exec_regimm
#define exec decoded_exec
#include "cpu/mips/execute.inc"
#undef exec
#undef exec_regimm
#undef exec_special
#undef branch_rel
#undef MH_INSN

#undef MH_FLAT_DISPATCH
#undef SPECIAL_OP
#undef DISPATCH
#undef RS
#undef RT
#undef RD
#undef SA
#undef FN
#undef OP
#undef IMM
#undef SIMM
#undef WORD
#undef JTARGET

/* Decode blocks are address-aligned groups of sixteen instructions, not
 * native code or guest I-cache emulation. Each entry is guarded by the live
 * instruction word on use: host/DMA writes and every alias are covered
 * without requiring all writers to notify a code cache. */
#define DECODE_BLOCK_WORDS 16
#define DECODE_BLOCK_COUNT 256
struct r3900_decode_cache {
    struct {
        uint32_t tag;
        uint16_t valid;
        decoded_instruction insn[DECODE_BLOCK_WORDS];
    } block[DECODE_BLOCK_COUNT];
};

r3900_decode_cache *mh_cpu_decode_cache_create(void)
{
    return calloc(1, sizeof(r3900_decode_cache));
}

void mh_cpu_decode_cache_free(r3900_decode_cache *cache)
{
    free(cache);
}

static const decoded_instruction *cached_decode(r3900_decode_cache *cache,
                                                uint32_t pa, uint32_t word)
{
    uint32_t tag = pa >> 6;
    unsigned slot = (tag ^ (tag >> 8)) & (DECODE_BLOCK_COUNT - 1);
    unsigned index = (pa >> 2) & (DECODE_BLOCK_WORDS - 1);
    if (cache->block[slot].tag != tag) {
        cache->block[slot].tag = tag;
        cache->block[slot].valid = 0;
    }
    decoded_instruction *d = &cache->block[slot].insn[index];
    if (!(cache->block[slot].valid & (1u << index)) || d->word != word) {
        *d = decode(word);
        cache->block[slot].valid |= (uint16_t)(1u << index);
    }
    return d;
}

typedef struct {
    uint32_t base, length;
    uint8_t *bytes;
    uint64_t generation;
} fetch_span;

#define STEP_NAME cpu_step
#define STEP_PARAMETERS r3900 *c, fetch_span *span
#define EXECUTE_INSTRUCTION(c, pa, insn) exec(c, insn)
#include "cpu/mips/step.inc"
#undef STEP_NAME
#undef STEP_PARAMETERS
#undef EXECUTE_INSTRUCTION
#define STEP_NAME decoded_cpu_step
#define STEP_PARAMETERS r3900 *c, fetch_span *span, r3900_decode_cache *cache
#define EXECUTE_INSTRUCTION(c, pa, insn) decoded_exec(c, cached_decode(cache, pa, insn))
#include "cpu/mips/step.inc"
#undef STEP_NAME
#undef STEP_PARAMETERS
#undef EXECUTE_INSTRUCTION

void mh_cpu_step(r3900 *c)
{
    cpu_step(c, NULL);
}

#if defined(__GNUC__) || defined(__clang__)
__attribute__((flatten))
#endif
void mh_cpu_run(r3900 *c, uint64_t insns)
{
    fetch_span span = {0};
    for (uint64_t i = 0; i < insns && !c->halted; i++)
        cpu_step(c, c->bus->lookup_cache_enabled ? &span : NULL);
}

#if defined(__GNUC__) || defined(__clang__)
__attribute__((noinline))
#endif
void mh_cpu_run_decoded(r3900 *c, uint64_t insns, r3900_decode_cache *cache)
{
    fetch_span span = {0};
    for (uint64_t i = 0; i < insns && !c->halted; i++)
        decoded_cpu_step(c, c->bus->lookup_cache_enabled ? &span : NULL, cache);
}

/*
 * Native engine helpers. A block calls these for instructions it does not
 * implement itself; they run the reference exec body with the same pipeline
 * bookkeeping the step routine keeps, and report whether the block must stop
 * afterward: an exception or halt moved the machine off the straight-line
 * path, or an access changed the interrupt inputs the block was entered
 * under (the reference loop would notice at its very next step).
 */
static uint32_t jit_finish(r3900 *c, uint32_t expected_pc, uint32_t k,
                           uint8_t irq_before, uint64_t generation_before)
{
    c->r[0] = 0;
    /* Leave when the instruction moved the machine off the straight-line
     * path, changed an interrupt input, or changed the bus map (the rest of
     * the block was fetched under the old one and the page tables are
     * stale, exactly the cases the reference step re-checks each fetch). */
    bool leave = c->pc != expected_pc || c->branch_pending || c->halted ||
                 c->power_stopped || c->irq_lines != irq_before ||
                 c->bus->lookup_generation != generation_before;
    /* The block retires its whole length itself when it completes; on an
     * early exit the k instructions already added stay and this one joins
     * them, with its fetch. */
    if (leave) {
        c->insn_count++;
        c->cycle_count++;
        c->bus->reads += k + 1;
    } else {
        c->insn_count -= k;
        c->cycle_count -= k;
    }
    return leave;
}

/* Which instructions reach the helpers is what decides the next thing
 * worth compiling natively; MH_JIT_STATS=1 prints the histogram at exit. */
static uint64_t jit_helper_histogram[128];
static void jit_print_helper_histogram(void)
{
    static const char *const special[64] = {
        [0x0c] = "syscall", [0x0d] = "break", [0x1a] = "div", [0x1b] = "divu",
        [0x20] = "add", [0x22] = "sub" };
    static const char *const primary[64] = {
        [0x08] = "addi", [0x10] = "cop0", [0x22] = "lwl", [0x26] = "lwr",
        [0x2a] = "swl", [0x2e] = "swr", [0x2f] = "cache",
        [0x20] = "lb", [0x21] = "lh", [0x23] = "lw", [0x24] = "lbu",
        [0x25] = "lhu", [0x28] = "sb", [0x29] = "sh", [0x2b] = "sw" };
    fprintf(stderr, "mips-jit helper calls by instruction:\n");
    for (unsigned k = 0; k < 128; k++) {
        if (!jit_helper_histogram[k]) continue;
        const char *name = k < 64 ? primary[k] : special[k - 64];
        fprintf(stderr, "  %s%s%02x: %" PRIu64 "\n", name ? name : "",
                name ? " " : "", k < 64 ? k : k - 64, jit_helper_histogram[k]);
    }
}
static void jit_count_helper(uint32_t word)
{
    static int enabled = -1;
    if (enabled < 0) {
        enabled = getenv("MH_JIT_STATS") != NULL;
        if (enabled) atexit(jit_print_helper_histogram);
    }
    if (enabled) jit_helper_histogram[(word >> 26) ? word >> 26 : 64 + (word & 63)]++;
}

uint32_t mh_jit_exec(r3900 *c, uint32_t word, uint32_t pc, uint32_t k)
{
    jit_count_helper(word);
    uint8_t irq_before = c->irq_lines;
    uint64_t generation = c->bus->lookup_generation;
    c->cur_pc = pc;
    c->pc = pc + 4;
    c->next_pc = pc + 8;
    c->insn_count += k;
    c->cycle_count += k;
    exec(c, word);
    return jit_finish(c, pc + 4, k, irq_before, generation);
}

uint32_t mh_jit_exec_delay(r3900 *c, uint32_t word, uint32_t pc, uint32_t k,
                            uint32_t target)
{
    jit_count_helper(word);
    uint8_t irq_before = c->irq_lines;
    uint64_t generation = c->bus->lookup_generation;
    c->cur_pc = pc;
    c->in_delay = true;
    c->pc = target;
    c->next_pc = target + 4;
    c->insn_count += k;
    c->cycle_count += k;
    exec(c, word);
    return jit_finish(c, target, k, irq_before, generation);
}

/*
 * Native block loop. Blocks run straight-line code through a branch and its
 * delay slot, with loads and stores to ordinary memory done in place and
 * everything else delegated to the helpers above. Interrupts are sampled at
 * block entry, which is every point the reference loop could take one:
 * device inputs only move between run calls, and instructions that change
 * Status or Cause end their block. Diagnostic modes, delay-slot entry,
 * misalignment and MMU-translated fetches use the reference step.
 */
#if defined(__GNUC__) || defined(__clang__)
__attribute__((flatten))
#endif
void mh_cpu_run_jit(r3900 *c, uint64_t slots, r3900_jit *jit)
{
    fetch_span span = {0};
    bool observed = c->trace || c->trace_after_pc || c->trace_from || g_rep || g_take || c->coverage || c->watch_n || g_trace_state ||
                    g_bus_trace || (g_watch_write && g_watch_write_log);
    for (uint64_t done = 0; done < slots && !c->halted;) {
        /* Page tables and code follow the bus map; a device write can
         * change it inside this call. Cheap when nothing changed. */
        mh_jit_prepare(jit, c);
        bool eligible = !observed && !c->power_stopped && !c->branch_pending &&
            !(c->pc & 3) && !c->r[0] &&
            (!c->has_mmu || (c->pc >= 0x80000000u && c->pc < 0xc0000000u &&
                            !(c->cp0[CP0_STATUS] & SR_KUc)));
        const mh_jit_entry *entry = eligible ? mh_jit_find(jit, c, c->pc, false) : NULL;
        if (!entry || entry->n > slots - done) {
            /* No block here, or the budget cannot take a whole one. */
            cpu_step(c, &span);
            mh_jit_account(jit, 0, 1);
            done++;
            continue;
        }
        c->cur_pc = c->pc;
        c->in_delay = false;
        if (check_interrupts(c)) {
            mh_jit_account(jit, 0, 1);
            done++;
            continue;
        }
        uint32_t budget = slots - done > UINT32_MAX ? UINT32_MAX
                                                    : (uint32_t)(slots - done);
        uint64_t result = entry->code(c, jit, budget);
        uint32_t retired = budget - (uint32_t)result;
        unsigned status = (unsigned)(result >> 32);
        mh_jit_account(jit, retired, 0);
        done += retired;
        if (mh_jit_tables_of(jit)->link)
            mh_jit_resolve_link(jit, c, status == MH_JIT_STALE);
    }
}

static const char *const regname[32] = {
    "zero", "at", "v0", "v1", "a0", "a1", "a2", "a3",
    "t0",   "t1", "t2", "t3", "t4", "t5", "t6", "t7",
    "s0",   "s1", "s2", "s3", "s4", "s5", "s6", "s7",
    "t8",   "t9", "k0", "k1", "gp", "sp", "s8", "ra",
};

void mh_cpu_dump(const r3900 *c, FILE *f)
{
    fprintf(f, "pc=%08X  hi=%08X lo=%08X  insns=%" PRIu64 "\n",
            c->cur_pc, c->hi, c->lo, c->insn_count);
    for (unsigned i = 0; i < 32; i++) {
        fprintf(f, "%4s=%08X%s", regname[i], c->r[i],
                (i % 4 == 3) ? "\n" : "  ");
    }
    fprintf(f, "Status=%08X Cause=%08X EPC=%08X BadVAddr=%08X PRId=%08X\n",
            c->cp0[CP0_STATUS], c->cp0[CP0_CAUSE], c->cp0[CP0_EPC],
            c->cp0[CP0_BADVADDR], c->cp0[CP0_PRID]);
}
