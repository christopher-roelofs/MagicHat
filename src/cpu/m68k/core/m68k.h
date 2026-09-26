/*
 * m68k.h — CPU32 execution state and semantics.
 *
 * The second piece of a 68k core of our own, on top of the decoder. It is
 * arranged the way the MIPS engine's semantics are: written once, and
 * reachable both from an ordinary interpreter loop and, later, from inside
 * a compiled block. That is the arrangement that let the MIPS translator
 * hand anything it could not compile back to the reference body rather than
 * end the block, and it is why almost none of the instruction set had to be
 * compiled natively before the thing was fast.
 *
 * Two consequences show up in this interface:
 *
 *   - Execution takes an already decoded instruction. Nothing here decodes,
 *     so a translator that decoded once at compile time does not pay for it
 *     again at run time.
 *   - An effective address is resolved to an operand first, and read or
 *     written after. An instruction that reads and writes one address must
 *     apply a pre-decrement or post-increment exactly once, and splitting
 *     the two steps is what makes that true for an emitter as well as for
 *     the interpreter.
 *
 * Condition codes are five separate bytes rather than bits in the status
 * register. That is how the reference core holds them, it keeps the common
 * case to a byte store, and it is the representation a translator wants
 * before it starts keeping them in host flags.
 */
#ifndef MH_M68K_H
#define MH_M68K_H

#include "cpu/m68k/core/m68k_decode.h"

/* Exception vectors this core can raise. */
enum {
    M68K_VEC_BUS_ERROR = 2, M68K_VEC_ADDRESS_ERROR = 3,
    M68K_VEC_ILLEGAL = 4, M68K_VEC_DIVIDE_BY_ZERO = 5,
    M68K_VEC_CHK = 6, M68K_VEC_TRAPV = 7, M68K_VEC_PRIVILEGE = 8,
    M68K_VEC_TRACE = 9, M68K_VEC_LINE_A = 10, M68K_VEC_LINE_F = 11,
    M68K_VEC_TRAP0 = 32,
};

typedef struct m68k m68k;

/* Guest memory, big-endian, sizes of 1, 2 and 4 bytes. */
typedef struct {
    uint32_t (*read)(void *ctx, uint32_t addr, unsigned size);
    void     (*write)(void *ctx, uint32_t addr, unsigned size, uint32_t value);
    void     *ctx;
    /*
     * Optional direct mappings of ordinary memory: one host pointer per
     * 4 KiB of guest address space, or NULL where an access has to go
     * through the functions above because something happens when it does.
     * Generated code uses these to read and write without a call; anything
     * they do not cover, including an access that would cross the end of a
     * page, takes the slow path and is indistinguishable from it.
     *
     * A page belongs here only if reading and writing it has no effect
     * beyond the memory itself. A device register does not qualify however
     * ordinary it looks.
     */
    uint8_t *const *read_pages;
    uint8_t *const *write_pages;
} m68k_bus;

#define M68K_PAGE_BITS 12
#define M68K_PAGE_COUNT (1u << (32 - M68K_PAGE_BITS))

struct m68k {
    uint32_t d[8];
    uint32_t a[8];          /* a[7] is whichever stack pointer is active */
    uint32_t usp, ssp;      /* the inactive one waits here               */
    uint32_t pc;            /* the instruction about to be executed      */

    uint32_t vbr;           /* vector base, where exceptions look        */
    uint32_t sfc, dfc;      /* the function codes MOVES reaches through  */
    /*
     * The function code of the access in progress, or 0 for an ordinary
     * one. Only MOVES sets it, and only across its own memory access: a
     * 68010 and later substitutes SFC or DFC for the usual supervisor/user
     * encoding, so the same address can name a different thing. Carried
     * beside the access rather than as a parameter because every other
     * access in this core, and all of the generated code, would otherwise
     * have to pass a constant.
     */
    uint8_t fc;

    uint8_t n, z, v, c, x;  /* condition codes, one byte each            */
    uint8_t interrupt_mask;
    bool    supervisor;
    bool    trace;

    m68k_bus bus;

    /*
     * Set when an instruction could not complete. The caller decides what
     * to do about it; this core does not build exception frames yet, and
     * says so rather than pretending the instruction ran.
     */
    unsigned exception;     /* vector number, or 0 */
    uint32_t exception_data;/* the operand that caused it, where there is one */

    /*
     * Give execution back rather than carry on.
     *
     * A machine with devices in it has to act on a device write at the
     * instruction that makes it: a write that arms a timer decides when
     * the timer fires, and running thirty more instructions first moves
     * that by more than one tick of a 128 kHz counter. Whatever owns the
     * machine sets this from inside its own bus, and the block runner
     * stops as soon as the instruction that set it has retired.
     *
     * The owner clears it. This core never does.
     */
    bool yield;

    /*
     * Refuse instructions this core does not implement, instead of
     * raising for them.
     *
     * On its own this core is the whole processor and an unimplemented
     * instruction is an illegal one. Underneath a machine that already
     * has a working processor it is neither: it is something to be handed
     * back, run there, and returned from. A block stops before one of
     * these, and the runner returns without retiring it, so the caller
     * finds its program counter sitting on the instruction to run.
     */
    bool hand_back;

    /*
     * Set by LPSTOP: the processor has stopped fetching and is waiting for
     * an interrupt the new mask admits. Not a halt -- device clocks go on
     * running, and the machine's idle loop is what decides how long to let
     * them run before looking again. The owner clears it, the way it
     * clears `yield`; this core only ever sets it.
     */
    bool stopped;

    /*
     * An address to stop in front of, or zero for none.
     *
     * A machine watching for a program counter -- to substitute a result,
     * to start a trace, to count a routine -- otherwise has to look at
     * every instruction, which means running every instruction somewhere
     * that looks. On the HIX-300 that was nine million instructions on the
     * reference core to catch one address. Told the address instead, the
     * block runner returns with the program counter sitting on it, having
     * retired nothing, and the machine does whatever it was watching for.
     *
     * A block is never built across this address, so it is also never run
     * across one. Change it through whatever the owner uses to flush the
     * block cache, because blocks built earlier were built under the old
     * answer.
     */
    uint32_t stop_pc;

    /*
     * A block asking to be told where control went.
     *
     * Emitted blocks jump straight to one another, and the jump starts out
     * going to a stub that sets this and returns. Whatever owns the block
     * cache sees it, points that jump at the block now at the program
     * counter, and clears it -- after which the two run as one and the
     * stub never executes again.
     *
     * It has to come from the block rather than be worked out by the
     * caller, because once blocks chain, the block the caller dispatched
     * and the block that actually finished are different blocks. Guessing
     * instead cost 2.5 million relinks and a sixty-fold slowdown, which is
     * how this field came to exist.
     */
    void *link_request;

    uint64_t insn_count;
    /*
     * Processor cycles, in the reference core's model of them.
     *
     * Instructions are the clock everywhere else in these machines, and
     * for the two Sony parts they are enough. The Envoy is not: its
     * battery RAM is a one-wire part whose data line is a function of
     * elapsed *cycles*, and the difference between writing a one and
     * writing a zero is how long the host held the line low. A count of
     * instructions cannot express that, because the same loop body costs
     * a different number of cycles depending on what is in it.
     *
     * The values preserve the original CPU32 timing table. They are not
     * verified MC68349 bus timings; execution correctness does not depend
     * on cycle accuracy.
     */
    uint64_t cycles;
    /*
     * Whether anything reads that count.
     *
     * Emitted code counts instructions but not cycles: what a conditional
     * branch or a shift with its count in a register charges depends on
     * the operand, and the emitted forms are branchless on purpose. So a
     * machine that reads the cycle count says so here and gets no native
     * code, and the count stays right rather than standing still through
     * exactly the code that runs fastest.
     *
     * The Envoy is the one machine that reads it.
     */
    bool count_cycles;
    /*
     * How many exceptions this core has raised.
     *
     * Under a machine these do not pass through the reference core, so
     * they do not reach whatever diagnostics it prints. Counting them is
     * the least that can be offered in exchange, and a count that has
     * moved is the sign to re-run on the reference to see what they were.
     */
    uint64_t exception_count;
    /*
     * And which vectors they were.
     *
     * The reference core reports this from its own exception hooks, and a
     * machine running on this one saw nothing: an exit report that says
     * "62,599 privilege violations" is the difference between a machine
     * working and a machine wrong, and it names which. Counted here rather
     * than through a callback because raising happens in three places and
     * a machine only ever wants the totals.
     */
    uint32_t vector_count[256];
    /*
     * The last one, for a machine that wants to print it as it happens.
     * `exception` is cleared at the start of every instruction; these are
     * not, so a caller that looks after a whole stretch still finds them.
     */
    unsigned last_vector;
    uint32_t last_vector_pc;    /* the instruction that raised it       */
    uint32_t last_vector_sp;    /* where the frame it built starts      */
    uint32_t last_vector_to;    /* and the handler it vectored to       */

    /*
     * Where the instruction that ran last began.
     *
     * A processor that fetches from an odd address faults for the
     * instruction that sent it there, not for the address it landed on,
     * and the frame records both. A caller that takes that fault over
     * therefore has to be told which instruction it was, and by then this
     * core has already moved past it.
     */
    uint32_t prev_pc;
};

/* Where an effective address ended up, once its side effects have run. */
typedef struct {
    enum { M68K_OPERAND_DREG, M68K_OPERAND_AREG, M68K_OPERAND_MEM,
           M68K_OPERAND_IMM } kind;
    uint32_t addr;          /* the address, or the immediate value */
    uint8_t  reg;
} m68k_operand;

void m68k_init(m68k *c, const m68k_bus *bus);
void m68k_reset(m68k *c);

/* The status register, assembled from and taken apart into the fields. */
uint16_t m68k_get_sr(const m68k *c);
void     m68k_set_sr(m68k *c, uint16_t sr);
uint8_t  m68k_get_ccr(const m68k *c);
void     m68k_set_ccr(m68k *c, uint8_t ccr);
/* True when the condition field selects. */
bool     m68k_condition(const m68k *c, unsigned cond);

/*
 * Resolve an effective address, applying its pre-decrement or
 * post-increment once. Read and write afterwards through the operand, so
 * that a read-modify-write instruction moves the register a single time.
 */
m68k_operand m68k_resolve(m68k *c, const m68k_ea *ea, unsigned size);
uint32_t m68k_read(m68k *c, const m68k_operand *op, unsigned size);
void     m68k_write(m68k *c, const m68k_operand *op, unsigned size, uint32_t value);

/*
 * Execute one already decoded instruction. `pc` is where it was decoded
 * from; the core advances its own program counter past it first, so that a
 * branch target computed at decode time and a return address pushed here
 * agree with the hardware. Returns false when the instruction raised an
 * exception instead of completing.
 */
bool m68k_execute(m68k *c, const m68k_insn *insn);

/* Decode and execute one instruction at the current program counter. */
bool m68k_step(m68k *c);

/*
 * An instruction fetch that landed on an odd address, faulted.
 *
 * The MC68349 records both addresses: `landed` is where the fetch went,
 * and `instruction_pc` is the instruction that sent it there, which by
 * the time anyone notices is no longer the program counter. A caller
 * running this core in blocks notices after the block has ended and so
 * has to supply both; m68k_step supplies prev_pc for the second itself.
 */
void m68k_address_error(m68k *c, uint32_t landed, uint32_t instruction_pc);

/*
 * Offer an interrupt at `level`. Taken when the level is above the mask, or
 * at level seven, which no mask refuses. `vector` is what the device
 * supplies; pass 0 for the automatic vector, which is what a board that
 * does not supply one gets. Returns whether it was taken.
 */
bool m68k_interrupt(m68k *c, unsigned level, unsigned vector);

/*
 * Whether this core runs the given instruction, as opposed to raising for
 * it. The core is the only thing that knows, so it is the only thing that
 * says: a second list kept elsewhere would drift, and a test using a stale
 * one would report coverage it does not have.
 */
bool m68k_implemented(unsigned op);

#endif /* MH_M68K_H */
