#ifndef MRC_JIT_INTERNAL_H
#define MRC_JIT_INTERNAL_H
#include "cpu/mips/r3900.h"
#include "jit/ir.h"
#include <stddef.h>

#define MRC_JIT_MAX_INSNS 64

/*
 * Native block ABI: uint64_t block(r3900 *c, r3900_jit *jit, uint32_t budget).
 *
 * A block has two entries. The call entry saves host registers and falls
 * into the chain entry, which (1) compares the guest words the block was
 * compiled from against live memory, (2) clears in_delay, (3) checks that
 * `budget` covers the whole block and subtracts its length, then runs the
 * body. When the block ends at a statically known successor it jumps to a
 * link site: a patchable branch that first leads to a stub returning to the
 * caller and is later redirected to the successor's chain entry by
 * mrc_jit_resolve_link. Register-indirect successors, blocks that must be
 * followed by an interrupt check, and branches awaiting their delay slot
 * return to the caller.
 *
 * The return value packs the remaining budget in the low word and a status
 * in the high word: MRC_JIT_DONE, MRC_JIT_STALE (the block at c->pc failed
 * its guard: recompile it), or MRC_JIT_BUDGET (the block at c->pc does not
 * fit the remaining budget: step it). Every exit leaves pc, next_pc, cur_pc,
 * branch_pending, in_delay, the GPRs, hi/lo and the data-access bus counters
 * exactly as the reference step sequence would, and insn_count,
 * cycle_count and the fetch count retired, so CP0 Count is exact wherever a
 * helper reads it.
 *
 * Guest GPRs, hi/lo and CP0 stay in the r3900 structure: no host register
 * holds guest state across a helper call, so helpers see the same machine
 * the interpreter would.
 */
typedef uint64_t (*mrc_jit_code)(r3900 *, r3900_jit *, uint32_t);
enum { MRC_JIT_DONE = 0, MRC_JIT_STALE = 1, MRC_JIT_BUDGET = 2 };

/* First member of r3900_jit; native code addresses it through the block's
 * second argument. `read`/`write` map each 4 KiB guest virtual page to its
 * host bytes, NULL where the page is not one uniform host-backed span
 * (MMIO, floating, mirror seams, TLB-mapped, ROM for writes). `link` is
 * set by an exiting block to the writable-view address of the branch it
 * wants redirected to the block at c->pc. */
/* Block table entry; native code searches these for register-indirect
 * jump targets, so the layout is fixed here: 32 bytes, four ways per set,
 * set = ((va >> 2) ^ (va >> 16)) & (MRC_JIT_SETS - 1). */
typedef struct {
    uint32_t va;
    uint32_t n;
    mrc_jit_code code;
    const uint8_t *chain;
    uint64_t pad;
} mrc_jit_slot;
#define MRC_JIT_SETS (1u << 14)
#define MRC_JIT_WAYS 4
_Static_assert(sizeof(mrc_jit_slot) == 32, "native lookups assume 32-byte slots");

typedef struct {
    uint8_t **read, **write;
    uint8_t *link;
    mrc_jit_slot *slots;
} mrc_jit_tables;
static inline mrc_jit_tables *mrc_jit_tables_of(r3900_jit *jit)
{
    return (mrc_jit_tables *)jit;
}

typedef struct {
    uint32_t va;                 /* virtual address of the first instruction */
    const uint8_t *guest;        /* live bytes the guard compares against    */
    uint32_t words[MRC_JIT_MAX_INSNS];
    mrc_jit_ir ir[MRC_JIT_MAX_INSNS];
    unsigned n;                  /* instructions in the block                */
    unsigned branch;             /* index + 1 of the branch, or 0            */
    bool delay;                  /* the delay slot is the block's last insn  */
} mrc_jit_block;

/* Backend entry: returns bytes emitted, or 0 when `cap` is too small.
 * `out` is the writable view; `exec` is where the same bytes execute.
 * `*chain` receives the chain entry's offset. */
size_t mrc_jit_emit(uint8_t *out, const uint8_t *exec, size_t cap,
                    const mrc_jit_block *b, unsigned *chain);
/* Redirect one link site to a chain entry. */
void mrc_jit_patch_link(uint8_t *site, const uint8_t *site_exec,
                        const uint8_t *target);
/* Set when the backend cannot run on this host (wrong ISA, byte order). */
bool mrc_jit_host_supported(void);

/* Helpers the native code calls. `k` is the number of instructions the
 * block completed before this one; the counters CP0 reads see are advanced
 * by it for the duration of the instruction. Nonzero return means the block
 * must exit: the helper has then retired the k + 1 instructions itself. */
uint32_t mrc_jit_exec(r3900 *c, uint32_t word, uint32_t pc, uint32_t k);
uint32_t mrc_jit_exec_delay(r3900 *c, uint32_t word, uint32_t pc, uint32_t k,
                            uint32_t target);

/* Cache side, used by the run loop in r3900.c. */
typedef mrc_jit_slot mrc_jit_entry;
const mrc_jit_entry *mrc_jit_find(r3900_jit *jit, r3900 *c, uint32_t va,
                                  bool recompile);
/* After a block returned with tables.link set: find or compile the block
 * at c->pc (recompiling when `stale`) and redirect the link to it. */
void mrc_jit_resolve_link(r3900_jit *jit, r3900 *c, bool stale);
void mrc_jit_prepare(r3900_jit *jit, r3900 *c);
void mrc_jit_account(r3900_jit *jit, unsigned native, unsigned fallback);
#endif
