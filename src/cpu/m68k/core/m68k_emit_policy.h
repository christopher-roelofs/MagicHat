/*
 * m68k_emit_policy.h — which instructions an emitter translates.
 *
 * Shared between the hosts on purpose. The list is a policy, not an
 * encoding: it says which instructions are worth translating rather than
 * calling the interpreter for, and the answer is the same whatever the
 * processor underneath. Keeping a copy per host would let the two drift,
 * and a drift here is the worst kind of bug this code can have -- one
 * machine running a different set of instructions natively than the other,
 * so that a disagreement only appears on the hardware you are not holding.
 *
 * Adding a form means adding it here and to every emitter's translate().
 * A host that has not caught up would emit nothing for it and the block
 * would be wrong, so each emitter's translate() ends with a default that
 * cannot be reached silently: the two are meant to be edited together.
 */
#ifndef MH_M68K_EMIT_POLICY_H
#define MH_M68K_EMIT_POLICY_H

#include "cpu/m68k/core/m68k_decode.h"
#include <stdlib.h>
#include <string.h>

/* MH_M68K_TRANSLATE=0 leaves every instruction to the interpreter while
 * keeping the block structure, which separates an emitter bug from a core
 * one without changing anything else. */
static inline bool m68k_translate_none(void)
{
    static int off = -1;
    if (off < 0) {
        const char *v = getenv("MH_M68K_TRANSLATE");
        off = v && !strcmp(v, "0");
    }
    return off != 0;
}

/*
 * An effective address an emitter can form without side effects.
 *
 * Pre-decrement and post-increment are deliberately absent: the fast path
 * may decide it cannot proceed and hand the whole instruction to the
 * interpreter, and it can only do that if it has changed nothing yet.
 */
/*
 * An address an emitter can form without changing anything.
 *
 * That last part is the rule: the fast path may discover it cannot finish
 * -- a device register has no direct mapping -- and hand the whole
 * instruction to the interpreter, which can only start over from an
 * untouched machine. So the modes with a side effect, (An)+ and -(An),
 * are deliberately absent. An index is read and never written, which is
 * why the indexed mode is not.
 */
static inline bool m68k_addressable(const m68k_ea *ea)
{
    return ea->mode == M68K_AI || ea->mode == M68K_DI ||
           ea->mode == M68K_AW || ea->mode == M68K_AL ||
           ea->mode == M68K_IX;
}

/*
 * A source an emitter can take without forming an address: a data register
 * or a constant the decoder already worked out. Everything else needs the
 * bus, and the bus is what the interpreter is for.
 */
/*
 * A source that is already in the structure: a register of either kind, or
 * a constant the decoder has already worked out. An address register is
 * here because these machines read one as an operand often -- ADD and SUB
 * from an An are 3% of what a PIC-2000 boot executes -- and it costs the
 * emitter nothing but a different offset.
 */
static inline bool m68k_simple_source(const m68k_ea *ea)
{
    return ea->mode == M68K_DN || ea->mode == M68K_AN ||
           ea->mode == M68K_IMM;
}

/*
 * Instructions that need neither an address nor the bus. The list grows by
 * measurement: each entry was put here because the machine was counted
 * running it, and each is a straight win over the interpreter call it
 * replaces. Everything absent still runs.
 */
static inline bool m68k_translatable(const m68k_insn *insn)
{
    if (m68k_translate_none()) return false;
    /*
     * Never anything that traps outside supervisor mode. The interpreter
     * makes that check before it does anything else; generated code does
     * not, and would run the instruction in user mode where the hardware
     * raises. Nothing below is privileged today, and this is here so that
     * adding something that is cannot go wrong quietly.
     */
    if (insn->flags & M68K_F_PRIVILEGED) return false;
    switch (insn->op) {
    case M68K_OP_NOP:
        return true;
    case M68K_OP_MOVEQ:
        return true;
    case M68K_OP_MOVE:
        /* Register or constant to register, where no address has to be
         * formed and no memory touched. */
        return m68k_simple_source(&insn->src) && insn->dst.mode == M68K_DN;
    case M68K_OP_BRA:
    case M68K_OP_BCC:
    case M68K_OP_DBCC:
        /* A branch is a program counter and a condition, and neither
         * needs the bus. A subroutine call is not here: it pushes. */
        return true;

    /*
     * The arithmetic, comparison and logical forms, where the destination
     * is a data register and the source is one too or is a constant. Every
     * one of them is in the top two dozen of what the PIC-2000 executes.
     *
     * All of them are emitted the same way, by shifting both operands to
     * the top of a host register so that the host's own carry, overflow,
     * sign and zero come out at the guest's width. That is why byte, word
     * and long need no separate cases and why the flags are exact rather
     * than reconstructed.
     */
    case M68K_OP_ADD: case M68K_OP_ADDI: case M68K_OP_ADDQ:
    case M68K_OP_SUB: case M68K_OP_SUBI: case M68K_OP_SUBQ:
        /* A quick add or subtract on an address register moves the whole
         * register and says nothing about it, whatever the size. */
        if (insn->dst.mode == M68K_AN)
            return (insn->op == M68K_OP_ADDQ || insn->op == M68K_OP_SUBQ) &&
                   m68k_simple_source(&insn->src);
        return m68k_simple_source(&insn->src) && insn->dst.mode == M68K_DN;
    case M68K_OP_AND: case M68K_OP_ANDI:
    case M68K_OP_OR:  case M68K_OP_ORI:
    case M68K_OP_EOR: case M68K_OP_EORI:
    case M68K_OP_CMP: case M68K_OP_CMPI:
        return m68k_simple_source(&insn->src) && insn->dst.mode == M68K_DN;
    case M68K_OP_ADDA: case M68K_OP_SUBA: case M68K_OP_CMPA:
        /* The address-register forms sign-extend their source and, for the
         * first two, say nothing about the result. */
        return m68k_simple_source(&insn->src);
    case M68K_OP_TST:
        return insn->src.mode == M68K_DN;
    case M68K_OP_CLR: case M68K_OP_NOT:
        return insn->dst.mode == M68K_DN;
    case M68K_OP_SCC:
        return insn->dst.mode == M68K_DN; /* isolate SCC on AArch64 */
    case M68K_OP_BTST:
        return insn->src.mode == M68K_IMM && insn->dst.mode == M68K_DN;
    case M68K_OP_MULL:
        return m68k_simple_source(&insn->src); /* isolate MULL on AArch64 */

    /*
     * A jump is an address and nothing else. The modes below are the ones
     * an emitter can form without a side effect, which is the same set the
     * inline memory moves use.
     */
    case M68K_OP_JMP:
        return m68k_addressable(&insn->src);

    /*
     * Shifting a data register by a constant. The count is one to eight,
     * known now, so the whole thing folds into a handful of host shifts
     * and a bit test for the carry instead of a loop.
     *
     * The arithmetic left shift is absent: its overflow flag is set if the
     * sign changed at any step, which is a different test again and not
     * worth it until the machine is seen running one. The two rotates
     * through the extend bit are absent for the same reason.
     */
    case M68K_OP_LSL: case M68K_OP_LSR: case M68K_OP_ASR:
    case M68K_OP_ROL: case M68K_OP_ROR:
        return insn->src.mode == M68K_IMM && insn->dst.mode == M68K_DN;
    default:
        return false;
    }
}

/* A move between a data register and ordinary memory, which an emitter
 * does inline with a page-table lookup and a fallback to the interpreter
 * for anything the mapping does not cover. */
static inline bool m68k_memory_move(const m68k_insn *insn)
{
    if (m68k_translate_none()) return false;
    if (insn->op != M68K_OP_MOVE) return false;
    if (insn->src.mode == M68K_DN && m68k_addressable(&insn->dst)) return true;
    if (m68k_addressable(&insn->src) && insn->dst.mode == M68K_DN) return true;
    return false;
}

static inline bool m68k_memory_movea(const m68k_insn *insn)
{
    (void)insn;
    return false; /* diagnostic isolation of recent AArch64 emitter work */
}

/* An ALU operation whose source is ordinary memory and whose destination is
 * a data register.  The address modes here have no register side effects,
 * so a failed direct-page lookup can still restart the instruction in the
 * interpreter without undoing anything. */
static inline bool m68k_memory_alu(const m68k_insn *insn)
{
    if (m68k_translate_none() || insn->dst.mode != M68K_DN ||
        !(m68k_addressable(&insn->src) ||
          insn->src.mode == M68K_PI || insn->src.mode == M68K_PD)) return false;
    switch (insn->op) {
    case M68K_OP_ADD: case M68K_OP_SUB:
    case M68K_OP_AND: case M68K_OP_OR: case M68K_OP_EOR:
    case M68K_OP_CMP:
        return true;
    default:
        return false;
    }
}

/*
 * Everything an emitter does inline with a page-table lookup and a fall
 * back to the interpreter: the moves above, and the two halves of a
 * subroutine call.
 *
 * A call and a return are worth the trouble because they are the third and
 * fourth most frequent things this machine does, and because their address
 * is the stack pointer, which is always ordinary memory in practice and so
 * always takes the fast path. Both change nothing until the lookup has
 * succeeded, which is what lets the slow path start over from an untouched
 * machine.
 */
static inline bool m68k_inline_memory(const m68k_insn *insn)
{
    if (m68k_translate_none()) return false;
    if (m68k_memory_move(insn)) return true;
    if (m68k_memory_movea(insn)) return true;
    if (m68k_memory_alu(insn)) return true;
    if (insn->op == M68K_OP_RTS) return true;
    if (insn->op == M68K_OP_JSR) return m68k_addressable(&insn->src);
    if (insn->op == M68K_OP_BSR) return true; /* isolate BSR on AArch64 */
    return false;
}

/* True for the branches, whose translation sets the program counter
 * itself rather than letting the caller store the next address. */
static inline bool m68k_sets_own_pc(const m68k_insn *insn)
{
    return insn->op == M68K_OP_BRA || insn->op == M68K_OP_BCC ||
           insn->op == M68K_OP_DBCC || insn->op == M68K_OP_JMP ||
           insn->op == M68K_OP_JSR || insn->op == M68K_OP_BSR ||
           insn->op == M68K_OP_RTS;
}

#endif /* MH_M68K_EMIT_POLICY_H */
