/*
 * m68k_block.h — basic blocks of decoded instructions.
 *
 * The layer between the interpreter and a translator, and the one that
 * decides whether a translator is possible at all. It answers the three
 * questions a translator has to answer before it can emit anything:
 *
 *   - Where does a block end? A branch, a return, anything that leaves
 *     straight-line execution, or an instruction after which interrupts
 *     have to be looked at again.
 *   - How is the compiled form kept honest when the guest changes the code
 *     underneath it? By comparing the bytes it was built from, on every
 *     entry, against what is there now.
 *   - What does the compiled form do about an instruction it cannot
 *     translate? It calls the interpreter for that one and carries on,
 *     which is what let the MIPS engine run correctly from the first
 *     instruction it supported rather than from the last.
 *
 * On its own this already earns its place: the decode of a hot loop
 * happens once instead of on every iteration. A native emitter can then
 * replace instructions inside a block one family at a time, with the
 * interpreter answering for the rest, and nothing has to be finished
 * before anything works.
 */
#ifndef MRC_M68K_BLOCK_H
#define MRC_M68K_BLOCK_H

#include "cpu/m68k/core/m68k.h"

/* Instructions in one block. Long enough for a real basic block, short
 * enough that a guard over its bytes stays cheap. */
#define M68K_BLOCK_MAX 32

typedef struct m68k_blocks m68k_blocks;

m68k_blocks *m68k_blocks_create(void);
void m68k_blocks_free(m68k_blocks *blocks);

/*
 * Run up to `budget` instructions through the block cache, returning how
 * many were retired. Stops early on an exception, on a halt, or when the
 * next instruction cannot be reached as part of a block.
 *
 * Behaviour is identical to calling m68k_step that many times. Anything
 * else is a bug in this file; block execution is compared with the
 * interpreter by the CPU execution tests.
 */
uint64_t m68k_run_blocks(m68k *c, uint64_t budget, m68k_blocks *blocks);

/*
 * Forget everything cached.
 *
 * Needed whenever something a block was built under has changed and the
 * bytes have not: m68k.stop_pc is the one so far, since a block built
 * while it was clear may run straight through the address it now names.
 * The guest rewriting its own code needs nothing here -- every entry is
 * checked against the bytes it was built from on the way in.
 */
void m68k_blocks_flush(m68k_blocks *blocks);

/* Diagnostics: how the cache is behaving, for deciding what to do next. */
typedef struct {
    uint64_t built;        /* blocks decoded                        */
    uint64_t entered;      /* times a block was entered             */
    uint64_t instructions; /* instructions retired inside blocks     */
    uint64_t stale;        /* entries whose guest bytes had changed */
    uint64_t evicted;      /* entries replaced by a different address */
    uint64_t compiled;     /* blocks for which native code was emitted */
    uint64_t chained;      /* links pointed at a successor              */
    uint64_t relinked;     /* and pointed somewhere else afterwards     */
    uint64_t ran_native;   /* instructions retired by emitted code      */
    /*
     * Of the instructions in emitted blocks, how many the emitter
     * translated rather than left as calls into the interpreter. Counted
     * when a block is compiled and not when it runs, because blocks jump
     * to one another and the caller no longer knows which ran; the
     * execution-weighted version of this is what --insn-heat reports.
     */
    uint64_t emitted_insns, translated;
    uint64_t arena_resets; /* times the code arena was emptied          */
    uint64_t refused;      /* times execution was handed back to the caller */
    uint64_t linked;       /* entries reached from the previous block directly */
    /*
     * Which instructions the refusals are for, by opcode. A handover is
     * cheap once but there are millions of them, and until this existed
     * the only way to order the work of removing them was to guess which
     * one the machines actually meet. Report with m68k_blocks_refusals().
     */
    uint64_t refused_op[M68K_OP_COUNT];
} m68k_block_stats;

/* The commonest refusals, most frequent first, for a report. Returns how
 * many entries were filled, at most `max`. */
typedef struct { const char *name; uint64_t count; } m68k_refusal;
unsigned m68k_blocks_refusals(const m68k_block_stats *st,
                              m68k_refusal *out, unsigned max);

void m68k_blocks_report(const m68k_blocks *blocks, m68k_block_stats *out);

/* What the cache holds for an address, for diagnosing a disagreement. */
typedef struct {
    bool     found;
    uint32_t va, bytes;
    unsigned count, entries;
    bool     compiled;
    uint16_t first_word;
    unsigned first_op;
} m68k_block_view;
void m68k_blocks_describe(const m68k_blocks *blocks, uint32_t pc,
                          m68k_block_view *out);

#endif /* MRC_M68K_BLOCK_H */
