/*
 * m68k_emit.h — native code for a block of decoded instructions.
 *
 * The emitter translates the instructions it knows and calls the
 * interpreter for the rest, from inside the same block. That is the
 * arrangement that made the MIPS engine tractable: no instruction has to
 * be translated before the thing works, a block never ends early merely
 * because an instruction is rare, and each family that gets translated is
 * a straight win over the call it replaces.
 *
 * The emitted function keeps no guest state in host registers across an
 * instruction boundary. Everything lives in the m68k structure, so an
 * interpreter call in the middle of a block sees exactly the machine it
 * would have seen on its own. Holding state in registers is a later
 * optimisation and a separate argument; correctness first.
 */
#ifndef MRC_M68K_EMIT_H
#define MRC_M68K_EMIT_H

#include "cpu/m68k/core/m68k.h"
#include <stddef.h>

/*
 * Which backend this build has, decided in one place so that the three
 * implementation files cannot all claim the host or all decline it. A
 * big-endian AArch64 is not one of ours: the guest is big-endian and the
 * byte reversal in the generated loads and stores assumes it is not.
 */
#if defined(__x86_64__) && !defined(_WIN32) /* System V ABI only */
#define MRC_M68K_EMIT_X86 1
#elif defined(__aarch64__) && !defined(__AARCH64EB__)
#define MRC_M68K_EMIT_A64 1
#endif

/*
 * A compiled block: uint32_t block(m68k *c).
 *
 * Returns how many instructions it retired. It stops early, having
 * retired fewer, when an instruction raised an exception or sent the
 * program counter somewhere other than the next instruction. The caller
 * must only enter it with the whole block within its budget, and with the
 * program counter at the block's first instruction.
 *
 * Returning zero means the guest bytes the block was compiled from have
 * changed and nothing was run: the caller must build it again. That is
 * unambiguous because a block that gets past its own guard always retires
 * at least its first instruction.
 */
/*
 * `budget` is how many instructions the caller will accept, which the
 * block needs only in order to go round again: a block whose last
 * instruction branches to its own first one jumps back inside the
 * generated code rather than returning, and has to know when to stop. The
 * caller still guarantees the first pass fits.
 *
 * That loop is the difference between dispatching every 2.8 instructions
 * and dispatching once per loop, which on these machines is most of what
 * a block engine is for: a PIC-2000 boot spends its time in a
 * two-instruction DBcc loop.
 */
typedef uint32_t (*m68k_code)(m68k *, uint32_t budget);

/* Whether this build can emit anything for this host. */
bool m68k_emit_supported(void);

/*
 * Emit a block. `out` is the writable view and `exec` the address the same
 * bytes will run at, which differ when the code arena is dual-mapped.
 * `insn` must stay alive and unmoved for as long as the code does: the
 * emitted calls point into it. Returns bytes written, or 0 if `cap` was
 * not enough.
 *
 * `cycles` asks for the processor's cycle count to be kept as well as the
 * instruction count. Only a machine that reads it wants that -- the Envoy,
 * for its battery RAM -- and it costs an add per instruction, so the rest
 * do not pay for it.
 *
 * `*native` receives how many of the instructions were translated rather
 * than left to the interpreter, which is the number worth watching as
 * families are added.
 */
/*
 * Where a block can be entered without its prologue, and where it says
 * which block runs next.
 *
 * `chain` receives the offset of the entry that skips the prologue: it
 * checks the program counter, the budget and the guest's own bytes, so a
 * block reached that way needs no caller to have checked anything. `link`
 * receives the offset of the jump this block takes when it is done, which
 * starts out going to its own exit and can be pointed at another block's
 * chain entry by m68k_patch_link.
 *
 * That is what keeps the engine inside generated code. Without it a block
 * returns to C every 3.1 instructions on a PIC-2000, where the MIPS engine
 * dispatches once per 341.
 */
typedef struct { size_t chain, link; } m68k_emit_points;

/*
 * `owner` is whatever the caller wants back when this block asks to be
 * linked: it is stored in m68k.link_request by the stub the link jumps to
 * until it is resolved, and means nothing to the emitter.
 */

size_t m68k_emit(uint8_t *out, const uint8_t *exec, size_t cap,
                 const m68k_insn *insn, unsigned count, uint32_t va,
                 const uint8_t *guest, const uint8_t *expect,
                 unsigned guard_bytes, bool cycles, unsigned *native,
                 m68k_emit_points *points, void *owner);

/*
 * Point one block's link at another's chain entry. `site` is the link
 * offset in the writable view and `site_exec` the same place as it will
 * run; `target` is the successor's chain entry, as it will run.
 */
void m68k_patch_link(uint8_t *site, const uint8_t *site_exec,
                     const uint8_t *target);

#endif /* MRC_M68K_EMIT_H */
