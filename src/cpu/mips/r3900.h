/*
 * r3900.h — Toshiba TX39 (R3900) CPU core.
 *
 * MIPS-I ISA, 32-bit, big-endian. The R3900 is R3000A-compatible at the
 * user-instruction level but differs in the details that matter to us:
 *
 *   - register scoreboarding, so there is NO architectural load delay slot
 *     (unlike the R3000A). We do not model one; see docs/ACCURACY.md.
 *   - a fast multiply-accumulate unit (MADD family).
 *   - its own CP0 Config layout and PRId.
 *
 * Anything the ROM executes that we do not implement raises a Reserved
 * Instruction exception AND is logged. We never silently treat an unknown
 * encoding as a NOP.
 */
#ifndef MH_R3900_H
#define MH_R3900_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "core/bus/bus.h"

/* CP0 register numbers. */
enum {
    CP0_INDEX = 0, CP0_RANDOM = 1, CP0_ENTRYLO = 2, CP0_CONTEXT = 4,
    CP0_BADVADDR = 8, CP0_COUNT = 9, CP0_ENTRYHI = 10, CP0_COMPARE = 11,
    CP0_STATUS = 12, CP0_CAUSE = 13, CP0_EPC = 14, CP0_PRID = 15,
    /* On the R3900 the Config register is CP0 #3, not #16 as on the R4000.
     * The ROM confirms it: 0x83C00330 sets bit 11 of $3 and 0x83C00358
     * clears bits 11:10, which are cache-enable controls. */
    CP0_CONFIG = 3,
};

/* Status register. */
#define SR_IEc      0x00000001u   /* current interrupt enable */
#define SR_KUc      0x00000002u   /* current kernel/user      */
#define SR_IEp      0x00000004u
#define SR_KUp      0x00000008u
#define SR_IEo      0x00000010u
#define SR_KUo      0x00000020u
#define SR_IM_SHIFT 8
#define SR_IM_MASK  0x0000FF00u
#define SR_ISC      0x00010000u
#define SR_SWC_     0x00020000u
#define SR_PZ       0x00040000u
#define SR_CM       0x00080000u
#define SR_PE       0x00100000u   /* cache parity error latch */
#define SR_TS       0x00200000u
#define SR_SWC      0x00020000u
#define SR_BEV      0x00400000u

/* Cause register. */
#define CAUSE_EXCCODE_SHIFT 2
#define CAUSE_EXCCODE_MASK  0x0000007Cu
#define CAUSE_IP_SHIFT      8
#define CAUSE_IP_MASK       0x0000FF00u
#define CAUSE_CE_SHIFT      28
#define CAUSE_BD            0x80000000u

/* Exception codes. */
enum {
    EXC_Int = 0,  EXC_Mod = 1,  EXC_TLBL = 2, EXC_TLBS = 3,
    EXC_AdEL = 4, EXC_AdES = 5, EXC_IBE = 6,  EXC_DBE = 7,
    EXC_Sys = 8,  EXC_Bp = 9,   EXC_RI = 10,  EXC_CpU = 11,
    EXC_Ov = 12,  EXC_Tr = 13,
};

/*
 * TLB. The exact entry count for the TMPR3902U is unverified; the TX39 family
 * documents 32 dual entries. We implement 32 and log any Index write above
 * that so the ROM can tell us if we are wrong.
 */
#define R3900_TLB_ENTRIES 32

typedef struct {
    uint32_t hi;    /* EntryHi as written */
    uint32_t lo;    /* EntryLo as written */
} r3900_tlb_entry;

typedef struct r3900 r3900;

/* Called when the core hits something it cannot faithfully execute. */
typedef void (*r3900_trap_fn)(r3900 *c, const char *what, uint32_t insn);

struct r3900 {
    uint32_t r[32];
    uint32_t hi, lo;
    uint32_t pc;        /* address of the instruction to fetch next */
    uint32_t next_pc;   /* address after that (branch target if pending)   */
    uint32_t cur_pc;    /* address of the instruction being executed       */
    bool     branch_pending;  /* the instruction just executed was a branch */
    bool     in_delay;        /* the current instruction is in a delay slot */

    uint32_t cp0[32];
    r3900_tlb_entry tlb[R3900_TLB_ENTRIES];

    mh_bus *bus;

    /*
     * External interrupt lines IP2..IP7 (hardware). Devices set/clear bits
     * here; the core ORs them into Cause.IP each step. Bit n of `irq_lines`
     * corresponds to Cause.IP bit n.
     */
    uint8_t irq_lines;

    uint64_t insn_count;
    uint64_t cycle_count;

    bool     halted;
    bool     tlb_used;      /* set the first time a TLB instruction executes */
    bool     exc_refill;    /* the exception being raised is a TLB refill */
    bool     power_stopped; /* external power controller suspended execution */
    uint64_t exc_count;
    uint64_t cache_ops;     /* CACHE instructions retired (we model no cache) */

    /*
     * Interrupt wiring evidence. `irq_ever_asserted` is the set of Cause.IP
     * lines our devices have ever driven; `im_ever_enabled` is the set the
     * software has ever unmasked in Status.IM. If a device drives a line the
     * OS never enables, or the OS waits on a line nothing drives, the
     * ICU-to-CPU mapping is wrong — and that is invisible from anywhere else,
     * because the symptom is simply that nothing ever happens.
     */
    uint8_t  irq_ever_asserted;
    uint8_t  im_ever_enabled;
    uint64_t irqs_taken;

    /*
     * PC watchpoints. Counting how often a given ROM address executes is the
     * cheapest way to ask "does control ever reach here?", which is the
     * question that matters when the machine runs fine but does nothing.
     */
#define R3900_MAX_WATCH 16
    uint32_t watch_pc[R3900_MAX_WATCH];
    uint64_t watch_hits[R3900_MAX_WATCH];
    unsigned watch_n;
    unsigned watch_log;     /* log the first n hits, with registers */
    uint64_t watch_after;   /* start logging only past this instruction count */

    /*
     * Execution coverage, one bit per instruction word of the 512 MB of
     * physical space kseg0 and kseg1 can reach. Running the same window with
     * and without an input and diffing the two shows exactly which code that
     * input reached — which answers "how far does it get?" without having to
     * guess addresses.
     */
    uint8_t *coverage;      /* NULL when not recording */
    uint32_t coverage_words;

    /* Diagnostics. Observation only. */
    bool     trace;
    /* --trace-after: start the trace at the hit-th execution of a pc, for
     * n instructions. Observation only. */
    uint32_t trace_after_pc;
    uint64_t trace_after_hit, trace_after_hits, trace_after_n;
    uint64_t trace_from;
    unsigned log_exceptions;   /* report this many exceptions, then stop */
    FILE    *log;
    r3900_trap_fn on_trap;

    /*
     * Whether this part has a translation lookaside buffer.
     *
     * The TMPR3902U does not. Two independent facts from the ROM establish
     * that, and both are checked at run time:
     *
     *   1. It never executes a TLB instruction (--verify reports tlb_used).
     *   2. Within the first few hundred instructions of reset it stores to
     *      kuseg 0x0000C1BC (at ROM 0x83C004EC) and to kseg3 0xFF000010
     *      (at ROM 0x83C00568). Neither could work through a TLB that
     *      nothing has programmed.
     *
     * With no MMU, kseg0/kseg1 mask off the top three bits as always, and
     * kuseg and kseg2/kseg3 map straight through to the same physical
     * address. --force-mmu turns the TLB back on to re-test this.
     */
    bool     has_mmu;
};

void mh_cpu_init(r3900 *c, mh_bus *bus);
void mh_cpu_reset(r3900 *c, uint32_t reset_pc);
void mh_cpu_step(r3900 *c);
void mh_cpu_run(r3900 *c, uint64_t insns);
/* Host-only decode cache; never part of a CPU snapshot. Word guards cover
 * writable code and direct DMA/host writes without serializing cache state. */
typedef struct r3900_decode_cache r3900_decode_cache;
/* Optional host-native engine; CPU/snapshot layouts remain unchanged. */
typedef struct r3900_jit r3900_jit;
r3900_jit *mh_cpu_jit_create(void);
void mh_cpu_jit_free(r3900_jit *jit);
void mh_cpu_run_jit(r3900 *c, uint64_t slots, r3900_jit *jit);
void mh_cpu_jit_report(const r3900_jit *jit, FILE *f);
uint64_t mh_cpu_jit_native_count(const r3900_jit *jit);

r3900_decode_cache *mh_cpu_decode_cache_create(void);
void mh_cpu_decode_cache_free(r3900_decode_cache *cache);
void mh_cpu_run_decoded(r3900 *c, uint64_t insns, r3900_decode_cache *cache);

/* Raise/clear a hardware interrupt line. line is 2..6 (IP2..IP6). */
void mh_cpu_set_irq(r3900 *c, unsigned line, bool asserted);

void mh_cpu_dump(const r3900 *c, FILE *f);

/*
 * Write a fixed-size architectural-state record for each of the next `count`
 * retired instructions. Used to validate a second implementation of this
 * part against this one; see the comment in r3900.c for the layout.
 */
void mh_cpu_trace_state(const char *path, uint64_t count);
void mh_cpu_trace_state_close(void);

/*
 * Write one record per data-bus access, so that a second implementation can
 * replay reads and have its writes checked before it has any devices.
 */
void mh_cpu_trace_bus(const char *path, uint64_t count);
void mh_cpu_trace_bus_devices_only(bool on);
void mh_cpu_trace_irq(const char *path);
void mh_cpu_replay(const char *dev_path, const char *irq_path);
void mh_cpu_replay_takes(const char *path);
void mh_cpu_trace_irq_close(void);
void mh_cpu_trace_bus_close(void);

/* Translate a virtual address for debug/inspection. Returns false on failure. */
bool mh_cpu_translate(r3900 *c, uint32_t va, bool write, uint32_t *pa);

#endif /* MH_R3900_H */

/* Diagnostic: log the first 64 stores that touch this word (0 = off).  Kept
 * outside the CPU struct so saved states stay layout-compatible. */
void mh_cpu_watch_write(uint32_t pa);
