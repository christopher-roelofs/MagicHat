#include "cpu/m68k/core/m68k_emit.h"
#include "cpu/m68k/core/m68k_block.h"
#include "cpu/m68k/core/m68k_emit_policy.h"

#if defined(MRC_M68K_EMIT_A64)
#include <stddef.h>
#include <string.h>

/*
 * AArch64, AAPCS64. The same block shape as the x86-64 backend, register
 * for register, so that the two can be read side by side and a difference
 * between them is a difference in the processor and not in the plan:
 *
 *   x19   the m68k structure                 (rbx there)
 *   w20   instructions retired, the result   (r12d there)
 *   x21   the table of readable pages        (r13 there)
 *   x22   the table of writable ones         (r14 there)
 *   w0-w4, x9, x10, x16  scratch, and dead at every interpreter call
 *
 * Guest state is never held in a host register across an instruction, so
 * an interpreter call in the middle of a block sees the machine exactly as
 * it would have on its own.
 *
 * Encodings are from the Arm Architecture Reference Manual, A64
 * instruction set. Every one of them is spelled out in a comment, because
 * a wrong bit in a hand-assembled instruction is invisible in review and
 * expensive to find by bisection afterwards.
 */
enum { X19 = 19, X20 = 20, X21 = 21, X22 = 22, XZR = 31 };
/* Condition codes, in the order the architecture numbers them. */
enum { A64_EQ = 0, A64_NE, A64_CS, A64_CC, A64_MI, A64_PL, A64_VS, A64_VC,
       A64_HI, A64_LS, A64_GE, A64_LT, A64_GT, A64_LE, A64_AL };

typedef struct {
    uint32_t *out;
    unsigned at, cap;          /* in instructions, not bytes */
    bool overflow;
    /* Keep the cycle count as well as the instruction count. Only a
     * machine that reads it asks; see m68k.count_cycles. */
    bool cycles;
} emitter;

static void insn_word(emitter *e, uint32_t w)
{
    if (e->at < e->cap) e->out[e->at++] = w;
    else e->overflow = true;
}
static unsigned here(const emitter *e) { return e->at; }

/* ------------------------------------------------------- loads, stores */
/*
 * The unsigned scaled-offset forms. Every guest field is at a small
 * positive offset from x19, so the scaled form always fits, and its
 * requirement that the offset be a multiple of the access size is met by
 * the structure's own alignment. The guard below reaches further but is
 * still far inside the twelve-bit field.
 */
static void ldr_w(emitter *e, unsigned rt, unsigned rn, size_t off)
{ insn_word(e, 0xB9400000u | (((uint32_t)off / 4) << 10) | (rn << 5) | rt); }
static void str_w(emitter *e, unsigned rt, unsigned rn, size_t off)
{ insn_word(e, 0xB9000000u | (((uint32_t)off / 4) << 10) | (rn << 5) | rt); }
static void ldr_x(emitter *e, unsigned rt, unsigned rn, size_t off)
{ insn_word(e, 0xF9400000u | (((uint32_t)off / 8) << 10) | (rn << 5) | rt); }
static void str_x(emitter *e, unsigned rt, unsigned rn, size_t off)
{ insn_word(e, 0xF9000000u | (((uint32_t)off / 8) << 10) | (rn << 5) | rt); }
static void ldrb_(emitter *e, unsigned rt, unsigned rn, size_t off)
{ insn_word(e, 0x39400000u | ((uint32_t)off << 10) | (rn << 5) | rt); }
static void strb_(emitter *e, unsigned rt, unsigned rn, size_t off)
{ insn_word(e, 0x39000000u | ((uint32_t)off << 10) | (rn << 5) | rt); }
static void ldrh_(emitter *e, unsigned rt, unsigned rn, size_t off)
{ insn_word(e, 0x79400000u | (((uint32_t)off / 2) << 10) | (rn << 5) | rt); }
static void strh_(emitter *e, unsigned rt, unsigned rn, size_t off)
{ insn_word(e, 0x79000000u | (((uint32_t)off / 2) << 10) | (rn << 5) | rt); }

/* ldr x[rt], [x[rn], w[rm], uxtw #3]: one page-table entry per page. */
static void ldr_x_page(emitter *e, unsigned rt, unsigned rn, unsigned rm)
{
    insn_word(e, 0xF8600800u | (rm << 16) | (2u << 13) | (1u << 12) |
                 (rn << 5) | rt);
}

/* ---------------------------------------------------------- immediates */
static void mov_w(emitter *e, unsigned rd, uint32_t v)
{
    if (!(v >> 16)) insn_word(e, 0x52800000u | (v << 5) | rd);          /* movz */
    else if ((v >> 16) == 0xFFFFu)
        insn_word(e, 0x12800000u | ((~v & 0xFFFFu) << 5) | rd);         /* movn */
    else {
        insn_word(e, 0x52800000u | ((v & 0xFFFFu) << 5) | rd);
        insn_word(e, 0x72A00000u | ((v >> 16) << 5) | rd);              /* movk */
    }
}
static void mov_x(emitter *e, unsigned rd, uint64_t v)
{
    insn_word(e, 0xD2800000u | ((uint32_t)(v & 0xFFFFu) << 5) | rd);
    for (unsigned hw = 1; hw < 4; hw++) {
        uint32_t part = (uint32_t)((v >> (16 * hw)) & 0xFFFFu);
        if (part) insn_word(e, 0xF2800000u | (hw << 21) | (part << 5) | rd);
    }
}
static void mov_rr(emitter *e, unsigned rd, unsigned rm)   /* orr wd,wzr,wm */
{ if (rd != rm) insn_word(e, 0x2A0003E0u | (rm << 16) | rd); }

/* --------------------------------------------------------- arithmetic */
static void add_imm_w(emitter *e, unsigned rd, unsigned rn, uint32_t v)
{ insn_word(e, 0x11000000u | (v << 10) | (rn << 5) | rd); }
static void sub_imm_w(emitter *e, unsigned rd, unsigned rn, uint32_t v)
{ insn_word(e, 0x51000000u | (v << 10) | (rn << 5) | rd); }
static void add_ww(emitter *e, unsigned rd, unsigned rn, unsigned rm)
{ insn_word(e, 0x0B000000u | (rm << 16) | (rn << 5) | rd); }
static void add_xx(emitter *e, unsigned rd, unsigned rn, unsigned rm)
{ insn_word(e, 0x8B000000u | (rm << 16) | (rn << 5) | rd); }
static void cmp_ww(emitter *e, unsigned rn, unsigned rm)   /* subs wzr,.. */
{ insn_word(e, 0x6B00001Fu | (rm << 16) | (rn << 5)); }
static void cmp_xx(emitter *e, unsigned rn, unsigned rm)
{ insn_word(e, 0xEB00001Fu | (rm << 16) | (rn << 5)); }
static void cmp_imm_w(emitter *e, unsigned rn, uint32_t imm12)
{ insn_word(e, 0x7100001Fu | (imm12 << 10) | (rn << 5)); }

/* w[rd] = w[rn] + v, in the shortest form. `scratch` is only touched when
 * the constant is too wide for the twelve-bit immediate. */
static void add_imm32(emitter *e, unsigned rd, unsigned rn, uint32_t v,
                      unsigned scratch)
{
    if (!v) { mov_rr(e, rd, rn); return; }
    if (v < 4096) { add_imm_w(e, rd, rn, v); return; }
    uint32_t neg = (uint32_t)(-(int32_t)v);
    if (neg < 4096) { sub_imm_w(e, rd, rn, neg); return; }
    mov_w(e, scratch, v);
    add_ww(e, rd, rn, scratch);
}

/* ------------------------------------------------------------- logical */
/* and wd, wn, #0xFFF: a logical immediate of twelve set bits, which for a
 * 32-bit element is N=0, immr=0, imms=11. */
static void and_page_offset(emitter *e, unsigned rd, unsigned rn)
{ insn_word(e, 0x12002C00u | (rn << 5) | rd); }
/* eor wd, wn, #1: the same encoding with a single set bit, immr=imms=0. */
static void eor_one(emitter *e, unsigned rd, unsigned rn)
{ insn_word(e, 0x52000000u | (rn << 5) | rd); }
static void orr_ww(emitter *e, unsigned rd, unsigned rn, unsigned rm)
{ insn_word(e, 0x2A000000u | (rm << 16) | (rn << 5) | rd); }
static void eor_ww(emitter *e, unsigned rd, unsigned rn, unsigned rm)
{ insn_word(e, 0x4A000000u | (rm << 16) | (rn << 5) | rd); }

/* ----------------------------------------------------------- bitfields */
static void ubfx(emitter *e, unsigned rd, unsigned rn, unsigned lsb,
                 unsigned width)
{
    insn_word(e, 0x53000000u | (lsb << 16) | ((lsb + width - 1) << 10) |
                 (rn << 5) | rd);
}
static void uxtb_(emitter *e, unsigned rd, unsigned rn) { ubfx(e, rd, rn, 0, 8); }
static void uxth_(emitter *e, unsigned rd, unsigned rn) { ubfx(e, rd, rn, 0, 16); }
/* rev and rev16, which are what makes a big-endian guest cheap here: one
 * instruction, where the interpreter does it a byte at a time. */
static void rev_w(emitter *e, unsigned rd, unsigned rn)
{ insn_word(e, 0x5AC00800u | (rn << 5) | rd); }
static void rev16_w(emitter *e, unsigned rd, unsigned rn)
{ insn_word(e, 0x5AC00400u | (rn << 5) | rd); }

/* --------------------------------------------------- conditional moves */
/* cset wd, cond = csinc wd, wzr, wzr, invert(cond) */
static void cset(emitter *e, unsigned rd, unsigned cond)
{ insn_word(e, 0x1A9F07E0u | ((cond ^ 1u) << 12) | rd); }
static void csel(emitter *e, unsigned rd, unsigned rn, unsigned rm,
                 unsigned cond)
{ insn_word(e, 0x1A800000u | (rm << 16) | (cond << 12) | (rn << 5) | rd); }

/* ------------------------------------------------------------ branches */
/*
 * Every branch out of a block goes to the one epilogue, whose address is
 * not known until the end, so each is recorded and patched. A conditional
 * branch reaches a megabyte, which no block comes close to.
 */
static void b_cond(emitter *e, unsigned cond, unsigned *fix)
{ *fix = here(e); insn_word(e, 0x54000000u | cond); }
static void cbz_w(emitter *e, unsigned rt, unsigned *fix)
{ *fix = here(e); insn_word(e, 0x34000000u | rt); }
static void cbnz_w(emitter *e, unsigned rt, unsigned *fix)
{ *fix = here(e); insn_word(e, 0x35000000u | rt); }
static void cbz_x(emitter *e, unsigned rt, unsigned *fix)
{ *fix = here(e); insn_word(e, 0xB4000000u | rt); }
static void b_always(emitter *e, unsigned *fix)
{ *fix = here(e); insn_word(e, 0x14000000u); }

/* An unconditional branch to somewhere already emitted. */
static void b_back(emitter *e, unsigned target)
{
    int32_t rel = (int32_t)target - (int32_t)here(e);
    insn_word(e, 0x14000000u | ((uint32_t)rel & 0x03FFFFFFu));
}

static void patch(emitter *e, unsigned fix, unsigned target)
{
    if (fix >= e->cap) return;
    int32_t rel = (int32_t)target - (int32_t)fix;
    uint32_t w = e->out[fix];
    if ((w & 0xFC000000u) == 0x14000000u)            /* b: twenty-six bits */
        e->out[fix] = 0x14000000u | ((uint32_t)rel & 0x03FFFFFFu);
    else                                             /* b.cond, cbz, cbnz  */
        e->out[fix] = (w & ~0x00FFFFE0u) | (((uint32_t)rel & 0x7FFFFu) << 5);
}

/* A call through x16, which is the register the architecture reserves for
 * exactly this. The address is built inline rather than branched to
 * directly, because a direct branch reaches 128 MB and the code arena is
 * wherever the kernel put it. */
static void call(emitter *e, const void *fn)
{
    mov_x(e, 16, (uint64_t)(uintptr_t)fn);
    insn_word(e, 0xD63F0200u);                       /* blr x16 */
}

#define D(n)  (offsetof(m68k, d) + (size_t)(n) * 4)
#define A(n)  (offsetof(m68k, a) + (size_t)(n) * 4)
#define PC    offsetof(m68k, pc)
#define FLAG(f) offsetof(m68k, f)
#define YIELD offsetof(m68k, yield)
#define STOPPED offsetof(m68k, stopped)
#define INSNS offsetof(m68k, insn_count)
#define CYCLES offsetof(m68k, cycles)
#define PREVPC offsetof(m68k, prev_pc)

/* w9 is the scratch these use; nothing else is live in it at their sites. */
static void store_imm32(emitter *e, size_t off, uint32_t v)
{
    if (!v) { str_w(e, XZR, X19, off); return; }
    mov_w(e, 9, v);
    str_w(e, 9, X19, off);
}
static void store_imm8(emitter *e, size_t off, unsigned v)
{
    if (!v) { strb_(e, XZR, X19, off); return; }
    mov_w(e, 9, v);
    strb_(e, 9, X19, off);
}

/* One more instruction retired, in the register the block returns. */
static void bump_retired(emitter *e)
{
    insn_word(e, 0x11000400u | (20u << 5) | 20u);    /* add w20, w20, #1 */
}

/*
 * Count the instruction in the structure as well.
 *
 * A machine underneath this can derive a clock from the instruction count,
 * and the PIC-2000 does: its counter register is that count scaled. A
 * block that only told the caller how far it got at the end would freeze
 * that clock for the length of the block, and the guest polls it.
 *
 * Only for instructions translated here. The ones handed to the
 * interpreter are counted by the interpreter, which does it as it starts
 * an instruction, so that an access made by that instruction sees the same
 * count the reference core would have been showing.
 */
static void bump_insns(emitter *e)
{
    ldr_x(e, 9, X19, INSNS);
    insn_word(e, 0x91000400u | (9u << 5) | 9u);      /* add x9, x9, #1 */
    str_x(e, 9, X19, INSNS);
}

/*
 * And the processor's cycles, for a machine that reads them.
 *
 * Everything an encoding decides is already in insn->cycles. The two
 * translated instructions whose cost also depends on an operand -- a
 * conditional branch taken, a loop whose counter expires -- add their
 * extra where they decide it, in translate() below. Shifts are the third
 * such instruction and are only translated with an immediate count, which
 * is a flat cost.
 */
static void add_cycles_imm(emitter *e, unsigned n)
{
    ldr_x(e, 9, X19, CYCLES);
    /* add x9, x9, #n -- the immediate form reaches 4095, and no
     * instruction in the table costs anything like that. */
    insn_word(e, 0x91000000u | ((n & 0xFFFu) << 10) | (9u << 5) | 9u);
    str_x(e, 9, X19, CYCLES);
}

static void bump_cycles(emitter *e, unsigned n)
{
    if (!e->cycles || !n) return;
    add_cycles_imm(e, n);
}

/*
 * The condition codes a move leaves behind: negative and zero taken from
 * the value at its own width, overflow and carry cleared. The value must
 * already be in w0, and the width is the guest's, so the sign is the
 * guest's sign and not a host register's.
 */
static void move_flags(emitter *e, unsigned size)
{
    ubfx(e, 1, 0, size == 1 ? 7 : size == 2 ? 15 : 31, 1);
    strb_(e, 1, X19, FLAG(n));
    if (size == 1) { uxtb_(e, 1, 0); cmp_imm_w(e, 1, 0); }
    else if (size == 2) { uxth_(e, 1, 0); cmp_imm_w(e, 1, 0); }
    else cmp_imm_w(e, 0, 0);
    cset(e, 1, A64_EQ);
    strb_(e, 1, X19, FLAG(z));
    strb_(e, XZR, X19, FLAG(v));
    strb_(e, XZR, X19, FLAG(c));
}

/*
 * The condition, as a zero or one in w3.
 *
 * The condition codes are kept as whole bytes holding zero or one, which
 * is what makes this short: the flag is already the answer for half the
 * conditions, and the rest are one or two operations on two of them. A
 * packed status register would need a mask and a shift before any of that.
 */
static void condition(emitter *e, unsigned cond)
{
    switch (cond) {
    case M68K_COND_T: mov_w(e, 3, 1); return;
    case M68K_COND_F: mov_w(e, 3, 0); return;
    case M68K_COND_EQ: ldrb_(e, 3, X19, FLAG(z)); return;
    case M68K_COND_NE: ldrb_(e, 3, X19, FLAG(z)); eor_one(e, 3, 3); return;
    case M68K_COND_CS: ldrb_(e, 3, X19, FLAG(c)); return;
    case M68K_COND_CC: ldrb_(e, 3, X19, FLAG(c)); eor_one(e, 3, 3); return;
    case M68K_COND_MI: ldrb_(e, 3, X19, FLAG(n)); return;
    case M68K_COND_PL: ldrb_(e, 3, X19, FLAG(n)); eor_one(e, 3, 3); return;
    case M68K_COND_VS: ldrb_(e, 3, X19, FLAG(v)); return;
    case M68K_COND_VC: ldrb_(e, 3, X19, FLAG(v)); eor_one(e, 3, 3); return;
    case M68K_COND_LS:                                  /* carry or zero */
        ldrb_(e, 3, X19, FLAG(c)); ldrb_(e, 4, X19, FLAG(z));
        orr_ww(e, 3, 3, 4); return;
    case M68K_COND_HI:
        ldrb_(e, 3, X19, FLAG(c)); ldrb_(e, 4, X19, FLAG(z));
        orr_ww(e, 3, 3, 4); eor_one(e, 3, 3); return;
    case M68K_COND_LT:                                  /* n differs from v */
        ldrb_(e, 3, X19, FLAG(n)); ldrb_(e, 4, X19, FLAG(v));
        eor_ww(e, 3, 3, 4); return;
    case M68K_COND_GE:
        ldrb_(e, 3, X19, FLAG(n)); ldrb_(e, 4, X19, FLAG(v));
        eor_ww(e, 3, 3, 4); eor_one(e, 3, 3); return;
    case M68K_COND_LE:                                  /* that, or zero */
        ldrb_(e, 3, X19, FLAG(n)); ldrb_(e, 4, X19, FLAG(v));
        eor_ww(e, 3, 3, 4); ldrb_(e, 4, X19, FLAG(z));
        orr_ww(e, 3, 3, 4); return;
    default:                                            /* GT */
        ldrb_(e, 3, X19, FLAG(n)); ldrb_(e, 4, X19, FLAG(v));
        eor_ww(e, 3, 3, 4); ldrb_(e, 4, X19, FLAG(z));
        orr_ww(e, 3, 3, 4); eor_one(e, 3, 3); return;
    }
}


/* Defined below, next to the inline memory moves that share it. */
static void address_into_w0(emitter *e, const m68k_ea *ea);

/* ------------------------------------------------------- arithmetic */
/*
 * Byte, word and long share one shape.
 *
 * Both operands are shifted to the top of a 32-bit register before the
 * operation, so the processor's own carry, overflow, sign and zero come
 * out at the guest's width rather than at the host's. That is what makes
 * the flags exact instead of reconstructed, and it is why there are no
 * separate cases per size below.
 *
 * One asymmetry with x86-64 matters here: after a subtract this processor
 * leaves carry set when there was no borrow, which is the opposite of what
 * the guest means by it, so the guest's carry is taken from CC and not CS.
 */
static unsigned width_shift(unsigned size)
{
    return size == 1 ? 24 : size == 2 ? 16 : 0;
}
static uint32_t width_mask(unsigned size)
{
    return size == 1 ? 0xFFu : size == 2 ? 0xFFFFu : 0xFFFFFFFFu;
}
static void lsl_imm(emitter *e, unsigned rd, unsigned rn, unsigned by)
{
    if (!by) { mov_rr(e, rd, rn); return; }
    insn_word(e, 0x53000000u | (((32u - by) & 31u) << 16) |
                 ((31u - by) << 10) | (rn << 5) | rd);
}
static void lsr_imm(emitter *e, unsigned rd, unsigned rn, unsigned by)
{
    if (!by) { mov_rr(e, rd, rn); return; }
    ubfx(e, rd, rn, by, 32 - by);
}
static void adds_ww(emitter *e, unsigned rd, unsigned rn, unsigned rm)
{ insn_word(e, 0x2B000000u | (rm << 16) | (rn << 5) | rd); }
static void subs_ww(emitter *e, unsigned rd, unsigned rn, unsigned rm)
{ insn_word(e, 0x6B000000u | (rm << 16) | (rn << 5) | rd); }
static void sub_ww(emitter *e, unsigned rd, unsigned rn, unsigned rm)
{ insn_word(e, 0x4B000000u | (rm << 16) | (rn << 5) | rd); }
static void and_ww(emitter *e, unsigned rd, unsigned rn, unsigned rm)
{ insn_word(e, 0x0A000000u | (rm << 16) | (rn << 5) | rd); }
static void mvn_w(emitter *e, unsigned rd, unsigned rm)   /* orn wd,wzr,wm */
{ insn_word(e, 0x2A2003E0u | (rm << 16) | rd); }
/* asr wd, wn, #n, which is a signed bitfield move down to the bottom. */
static void asr_imm(emitter *e, unsigned rd, unsigned rn, unsigned by)
{
    if (!by) { mov_rr(e, rd, rn); return; }
    insn_word(e, 0x13000000u | (by << 16) | (31u << 10) | (rn << 5) | rd);
}
/*
 * and wd, wn, #0xFF or #0xFFFF, and nothing at all for a long. Both are
 * runs of set bits starting at bit zero, which a 32-bit logical immediate
 * encodes as N=0, immr=0 and imms one less than the run's length.
 */
static void and_width(emitter *e, unsigned rd, unsigned rn, unsigned size)
{
    if (size == 4) { mov_rr(e, rd, rn); return; }
    insn_word(e, 0x12000000u | ((size * 8u - 1u) << 10) | (rn << 5) | rd);
}
static void ldrsh_(emitter *e, unsigned rt, unsigned rn, size_t off)
{ insn_word(e, 0x79C00000u | (((uint32_t)off / 2) << 10) | (rn << 5) | rt); }

/* One flag, from a condition, into its byte. */
static void set_flag(emitter *e, unsigned cond, size_t flag)
{
    cset(e, 4, cond);
    strb_(e, 4, X19, flag);
}

/* Where a register source lives, which is the only thing that differs
 * between a data register and an address one. */
static unsigned alu_src(const m68k_insn *in)
{
    return in->src.mode == M68K_AN ? A(in->src.reg) : D(in->src.reg);
}

/* The destination into w0 and the source into w1, both shifted up. */
static void alu_operands(emitter *e, const m68k_insn *in, unsigned shift)
{
    ldr_w(e, 0, X19, D(in->dst.reg));
    lsl_imm(e, 0, 0, shift);
    if (in->src.mode != M68K_IMM) {
        ldr_w(e, 1, X19, alu_src(in));
        lsl_imm(e, 1, 1, shift);
    } else {
        mov_w(e, 1, ((uint32_t)in->src.disp & width_mask(in->size)) << shift);
    }
}

/* Put the value in w3 back into the destination register at its width. */
static void alu_store(emitter *e, const m68k_insn *in, unsigned shift)
{
    lsr_imm(e, 3, 3, shift);
    if (in->size == 4) str_w(e, 3, X19, D(in->dst.reg));
    else if (in->size == 2) strh_(e, 3, X19, D(in->dst.reg));
    else strb_(e, 3, X19, D(in->dst.reg));
}

/*
 * The source of an address-register form, sign-extended into w1. These
 * take their operand at its own size and then widen it, which is the one
 * place the guest's size means something other than a mask.
 */
static void alu_source_extended(emitter *e, const m68k_insn *in)
{
    if (in->src.mode != M68K_IMM) {
        if (in->size == 2) ldrsh_(e, 1, X19, alu_src(in));
        else ldr_w(e, 1, X19, alu_src(in));
        return;
    }
    uint32_t v = (uint32_t)in->src.disp & width_mask(in->size);
    if (in->size == 2 && (v & 0x8000u)) v |= 0xFFFF0000u;
    mov_w(e, 1, v);
}

static void translate(emitter *e, const m68k_insn *insn)
{
    switch (insn->op) {
    case M68K_OP_NOP:
        return;
    case M68K_OP_MOVEQ: {
        /* Everything but the register write is known now, flags included. */
        uint32_t value = (uint32_t)insn->src.disp;
        store_imm32(e, D(insn->dst.reg), value);
        store_imm8(e, FLAG(n), (value >> 31) & 1);
        store_imm8(e, FLAG(z), value == 0);
        store_imm8(e, FLAG(v), 0);
        store_imm8(e, FLAG(c), 0);
        return;
    }
    case M68K_OP_SCC: {
        condition(e, insn->cond);
        sub_ww(e, 3, 31, 3);                             /* w3 = 0 or -1 */
        ubfx(e, 3, 3, 0, 8);                             /* low byte only */
        ldr_w(e, 0, X19, D(insn->dst.reg));
        ubfx(e, 4, 0, 8, 24);
        lsl_imm(e, 4, 4, 8);
        orr_ww(e, 4, 4, 3);
        str_w(e, 4, X19, D(insn->dst.reg));
        return;
    }
    case M68K_OP_BTST: {
        unsigned bit = (unsigned)insn->src.disp & 31u;
        ldr_w(e, 0, X19, D(insn->dst.reg));
        ubfx(e, 3, 0, bit, 1);
        cmp_imm_w(e, 3, 0);
        set_flag(e, A64_EQ, FLAG(z));
        return;
    }
    case M68K_OP_MULL: {
        uint16_t ext = (uint16_t)insn->extra;
        unsigned dl = (ext >> 12) & 7, dh = ext & 7;
        bool wide = (ext & 0x400) != 0, is_signed = (ext & 0x800) != 0;
        if (insn->src.mode == M68K_IMM)
            mov_w(e, 0, (uint32_t)insn->src.disp);
        else
            ldr_w(e, 0, X19, insn->src.mode == M68K_AN
                                  ? A(insn->src.reg) : D(insn->src.reg));
        ldr_w(e, 1, X19, D(dl));
        insn_word(e, (is_signed ? 0x9B207C00u : 0x9BA07C00u) |
                     (1u << 16) | (0u << 5) | 2u); /* smull/umull x2,w0,w1 */
        if (wide) {
            cmp_xx(e, 2, XZR);
            set_flag(e, A64_MI, FLAG(n));
            set_flag(e, A64_EQ, FLAG(z));
            store_imm8(e, FLAG(v), 0);
            store_imm8(e, FLAG(c), 0);
            insn_word(e, 0xD3400000u | (32u << 16) | (63u << 10) |
                         (2u << 5) | 3u); /* lsr x3,x2,#32 */
            str_w(e, 3, X19, D(dh));
            str_w(e, 2, X19, D(dl));
        } else {
            cmp_ww(e, 2, XZR);
            set_flag(e, A64_MI, FLAG(n));
            set_flag(e, A64_EQ, FLAG(z));
            if (is_signed) {
                insn_word(e, 0x93400000u | (31u << 10) | (2u << 5) | 3u);
                cmp_xx(e, 2, 3);
            } else {
                insn_word(e, 0xD3400000u | (32u << 16) | (63u << 10) |
                             (2u << 5) | 3u); /* lsr x3,x2,#32 */
                cmp_ww(e, 3, XZR);
            }
            set_flag(e, A64_NE, FLAG(v));
            store_imm8(e, FLAG(c), 0);
            str_w(e, 2, X19, D(dl));
        }
        return;
    }
    case M68K_OP_BRA:
        store_imm32(e, PC, insn->extra);
        return;
    case M68K_OP_BCC:
        /* Both destinations are known now; the condition chooses. */
        condition(e, insn->cond);
        /*
         * Taking a byte-displacement branch costs two more than not
         * taking it; the word and long forms cost the same either way.
         * w3 is the condition, so the extra is twice it.
         */
        if (e->cycles && insn->length == 2) {
            ldr_x(e, 9, X19, CYCLES);
            /* add x9, x9, x3, lsl #1 */
            insn_word(e, 0x8B000000u | (3u << 16) | (1u << 10) |
                         (9u << 5) | 9u);
            str_x(e, 9, X19, CYCLES);
        }
        mov_w(e, 0, insn->extra);
        mov_w(e, 1, insn->next_pc);
        cmp_imm_w(e, 3, 0);
        csel(e, 0, 1, 0, A64_EQ);      /* condition false: the next address */
        str_w(e, 0, X19, PC);
        return;
    case M68K_OP_DBCC: {
        /*
         * The condition wins outright; otherwise the low half of the
         * register counts down and the branch is taken until it passes
         * zero, which for a sixteen-bit counter means reaching -1.
         */
        unsigned to_done_true, to_done_spent;
        condition(e, insn->cond);
        mov_w(e, 0, insn->next_pc);
        cbnz_w(e, 3, &to_done_true);
        ldrh_(e, 1, X19, D(insn->dst.reg));
        sub_imm_w(e, 1, 1, 1);
        strh_(e, 1, X19, D(insn->dst.reg));
        uxth_(e, 2, 1);
        mov_w(e, 4, 0xFFFFu);
        cmp_ww(e, 2, 4);
        b_cond(e, A64_EQ, &to_done_spent);
        mov_w(e, 0, insn->extra);
        if (e->cycles) {
            /* Going round again is the cheap case; the loop ending by the
             * counter running out costs four more. */
            unsigned to_done;
            b_always(e, &to_done);
            patch(e, to_done_spent, here(e));
            add_cycles_imm(e, 4);
            patch(e, to_done, here(e));
        } else {
            patch(e, to_done_spent, here(e));
        }
        patch(e, to_done_true, here(e));
        str_w(e, 0, X19, PC);
        return;
    }
    case M68K_OP_MOVE:
        if (insn->src.mode == M68K_IMM) {
            /* Nothing here depends on the machine, flags included. */
            uint32_t v = (uint32_t)insn->src.disp & width_mask(insn->size);
            if (insn->size == 4) store_imm32(e, D(insn->dst.reg), v);
            else {
                mov_w(e, 0, v);
                if (insn->size == 2) strh_(e, 0, X19, D(insn->dst.reg));
                else strb_(e, 0, X19, D(insn->dst.reg));
            }
            store_imm8(e, FLAG(n), (v >> (insn->size * 8 - 1)) & 1);
            store_imm8(e, FLAG(z), v == 0);
            store_imm8(e, FLAG(v), 0);
            store_imm8(e, FLAG(c), 0);
            return;
        }
        ldr_w(e, 0, X19, alu_src(insn));
        if (insn->size == 4) str_w(e, 0, X19, D(insn->dst.reg));
        else if (insn->size == 2) strh_(e, 0, X19, D(insn->dst.reg));
        else strb_(e, 0, X19, D(insn->dst.reg));
        move_flags(e, insn->size);
        return;

    case M68K_OP_ADD: case M68K_OP_ADDI: case M68K_OP_ADDQ:
    case M68K_OP_SUB: case M68K_OP_SUBI: case M68K_OP_SUBQ: {
        bool add = insn->op == M68K_OP_ADD || insn->op == M68K_OP_ADDI ||
                   insn->op == M68K_OP_ADDQ;
        if (insn->dst.mode == M68K_AN) {
            /* The whole register moves and nothing is said about it. */
            uint32_t delta = (uint32_t)insn->src.disp;
            if (!add) delta = (uint32_t)(-(int32_t)delta);
            ldr_w(e, 0, X19, A(insn->dst.reg));
            add_imm32(e, 0, 0, delta, 9);
            str_w(e, 0, X19, A(insn->dst.reg));
            return;
        }
        unsigned shift = width_shift(insn->size);
        alu_operands(e, insn, shift);
        if (add) adds_ww(e, 3, 0, 1); else subs_ww(e, 3, 0, 1);
        set_flag(e, add ? A64_CS : A64_CC, FLAG(c));
        strb_(e, 4, X19, FLAG(x));         /* the extend follows the carry */
        set_flag(e, A64_VS, FLAG(v));
        set_flag(e, A64_MI, FLAG(n));
        set_flag(e, A64_EQ, FLAG(z));
        alu_store(e, insn, shift);
        return;
    }
    case M68K_OP_CMP: case M68K_OP_CMPI: {
        unsigned shift = width_shift(insn->size);
        alu_operands(e, insn, shift);
        cmp_ww(e, 0, 1);
        set_flag(e, A64_CC, FLAG(c));                 /* and X is left alone */
        set_flag(e, A64_VS, FLAG(v));
        set_flag(e, A64_MI, FLAG(n));
        set_flag(e, A64_EQ, FLAG(z));
        return;
    }
    case M68K_OP_AND: case M68K_OP_ANDI:
    case M68K_OP_OR:  case M68K_OP_ORI:
    case M68K_OP_EOR: case M68K_OP_EORI: {
        unsigned shift = width_shift(insn->size);
        alu_operands(e, insn, shift);
        if (insn->op == M68K_OP_AND || insn->op == M68K_OP_ANDI)
            and_ww(e, 3, 0, 1);
        else if (insn->op == M68K_OP_OR || insn->op == M68K_OP_ORI)
            orr_ww(e, 3, 0, 1);
        else eor_ww(e, 3, 0, 1);
        cmp_imm_w(e, 3, 0);
        set_flag(e, A64_MI, FLAG(n));
        set_flag(e, A64_EQ, FLAG(z));
        store_imm8(e, FLAG(v), 0);
        store_imm8(e, FLAG(c), 0);        /* and the extend is untouched */
        alu_store(e, insn, shift);
        return;
    }
    case M68K_OP_ADDA: case M68K_OP_SUBA:
        alu_source_extended(e, insn);
        ldr_w(e, 0, X19, A(insn->dst.reg));
        if (insn->op == M68K_OP_ADDA) add_ww(e, 0, 0, 1);
        else sub_ww(e, 0, 0, 1);
        str_w(e, 0, X19, A(insn->dst.reg));
        return;
    case M68K_OP_CMPA:
        /* Always a long comparison, whatever the source's size. */
        alu_source_extended(e, insn);
        ldr_w(e, 0, X19, A(insn->dst.reg));
        cmp_ww(e, 0, 1);
        set_flag(e, A64_CC, FLAG(c));
        set_flag(e, A64_VS, FLAG(v));
        set_flag(e, A64_MI, FLAG(n));
        set_flag(e, A64_EQ, FLAG(z));
        return;
    case M68K_OP_TST: {
        unsigned shift = width_shift(insn->size);
        ldr_w(e, 0, X19, D(insn->src.reg));
        lsl_imm(e, 3, 0, shift);
        cmp_imm_w(e, 3, 0);
        set_flag(e, A64_MI, FLAG(n));
        set_flag(e, A64_EQ, FLAG(z));
        store_imm8(e, FLAG(v), 0);
        store_imm8(e, FLAG(c), 0);
        return;
    }
    case M68K_OP_CLR:
        if (insn->size == 4) str_w(e, XZR, X19, D(insn->dst.reg));
        else if (insn->size == 2) strh_(e, XZR, X19, D(insn->dst.reg));
        else strb_(e, XZR, X19, D(insn->dst.reg));
        store_imm8(e, FLAG(n), 0);
        store_imm8(e, FLAG(z), 1);
        store_imm8(e, FLAG(v), 0);
        store_imm8(e, FLAG(c), 0);
        return;
    case M68K_OP_JMP:
        /* The address is the whole instruction. */
        address_into_w0(e, &insn->src);
        str_w(e, 0, X19, PC);
        return;

    case M68K_OP_LSL: case M68K_OP_LSR: case M68K_OP_ASR:
    case M68K_OP_ROL: case M68K_OP_ROR: {
        unsigned n = (unsigned)insn->src.disp;      /* one to eight */
        unsigned bits = insn->size * 8;
        bool left = insn->op == M68K_OP_LSL || insn->op == M68K_OP_ROL;
        bool rotate = insn->op == M68K_OP_ROL || insn->op == M68K_OP_ROR;

        ldr_w(e, 0, X19, D(insn->dst.reg));
        and_width(e, 0, 0, insn->size);
        /*
         * The carry is the last bit to leave, which for a known count is a
         * known bit of the value before anything moved. Taking it first is
         * what lets the result be computed in place afterwards.
         */
        ubfx(e, 4, 0, left ? bits - n : n - 1, 1);
        strb_(e, 4, X19, FLAG(c));
        if (!rotate) strb_(e, 4, X19, FLAG(x));     /* rotates leave it */

        switch (insn->op) {
        case M68K_OP_LSL:
            lsl_imm(e, 3, 0, n); and_width(e, 3, 3, insn->size); break;
        case M68K_OP_LSR:
            lsr_imm(e, 3, 0, n); break;
        case M68K_OP_ASR:
            /* Sign-propagating at the guest's width, so the value goes to
             * the top of the register, comes back arithmetically, and is
             * brought down again. */
            lsl_imm(e, 3, 0, 32 - bits);
            asr_imm(e, 3, 3, n);
            lsr_imm(e, 3, 3, 32 - bits);
            break;
        case M68K_OP_ROL:
            lsl_imm(e, 3, 0, n); lsr_imm(e, 1, 0, bits - n);
            orr_ww(e, 3, 3, 1); and_width(e, 3, 3, insn->size); break;
        default:                                    /* ROR */
            lsr_imm(e, 3, 0, n); lsl_imm(e, 1, 0, bits - n);
            orr_ww(e, 3, 3, 1); and_width(e, 3, 3, insn->size); break;
        }
        store_imm8(e, FLAG(v), 0);
        lsl_imm(e, 1, 3, 32 - bits);
        cmp_imm_w(e, 1, 0);
        set_flag(e, A64_MI, FLAG(n));
        set_flag(e, A64_EQ, FLAG(z));
        if (insn->size == 4) str_w(e, 3, X19, D(insn->dst.reg));
        else if (insn->size == 2) strh_(e, 3, X19, D(insn->dst.reg));
        else strb_(e, 3, X19, D(insn->dst.reg));
        return;
    }

    case M68K_OP_NOT: {
        /*
         * The shift comes after the complement here, not before it.
         * Inverting a value whose low bits were shifted away sets them,
         * and then nothing is ever zero. Complementing first and shifting
         * afterwards puts the guest's width back at the top with zeroes
         * below it, which is what the flags are read from.
         */
        unsigned shift = width_shift(insn->size);
        ldr_w(e, 0, X19, D(insn->dst.reg));
        mvn_w(e, 3, 0);
        lsl_imm(e, 3, 3, shift);
        cmp_imm_w(e, 3, 0);
        set_flag(e, A64_MI, FLAG(n));
        set_flag(e, A64_EQ, FLAG(z));
        store_imm8(e, FLAG(v), 0);
        store_imm8(e, FLAG(c), 0);
        alu_store(e, insn, shift);
        return;
    }
    default:
        /* Unreachable: the policy header and this switch are one list in
         * two places and are meant to be edited together. */
        return;
    }
}

/*
 * The address an effective address names, into w0, for the modes that form
 * one by adding a displacement to an address register.
 */
static void address_into_w0(emitter *e, const m68k_ea *ea)
{
    if (ea->mode == M68K_AW || ea->mode == M68K_AL) {
        mov_w(e, 0, (uint32_t)ea->disp);
        return;
    }
    if (ea->mode == M68K_IX) {
        /*
         * Base plus index plus displacement, any of which the extension
         * word may have suppressed. Still no side effect, which is what
         * lets it share the fast path: an index is read and never written.
         * w9 is scratch until the page lookup, which is the next thing
         * that runs.
         */
        if (ea->base_used) ldr_w(e, 0, X19, A(ea->reg));
        else mov_w(e, 0, 0);
        if (ea->index_used) {
            size_t at = ea->index < 8 ? D(ea->index) : A(ea->index - 8);
            if (ea->index_long) ldr_w(e, 9, X19, at);
            else ldrsh_(e, 9, X19, at);
            unsigned by = ea->scale == 8 ? 3 : ea->scale == 4 ? 2 :
                          ea->scale == 2 ? 1 : 0;
            if (by) lsl_imm(e, 9, 9, by);
            add_ww(e, 0, 0, 9);
        }
        if (ea->disp) add_imm32(e, 0, 0, (uint32_t)ea->disp, 9);
        return;
    }
    ldr_w(e, 0, X19, A(ea->reg));
    if (ea->disp) add_imm32(e, 0, 0, (uint32_t)ea->disp, 9);
}

/*
 * Turn the address in w0 into a host pointer in x2, or branch to the slow
 * path. Two things send it there: a page with no direct mapping, which is
 * how a device register looks from here, and an access that would run off
 * the end of one, which is rare enough to be worth nothing and awkward
 * enough to be worth avoiding.
 */
static void page_lookup(emitter *e, unsigned size, bool write,
                        unsigned *slow, unsigned *nslow)
{
    ubfx(e, 1, 0, M68K_PAGE_BITS, 32 - M68K_PAGE_BITS);   /* lsr w1,w0,#12 */
    ldr_x_page(e, 2, write ? X22 : X21, 1);
    cbz_x(e, 2, &slow[(*nslow)++]);
    and_page_offset(e, 1, 0);
    cmp_imm_w(e, 1, 4096u - size);
    b_cond(e, A64_HI, &slow[(*nslow)++]);
    add_xx(e, 2, 2, 1);   /* w1 was written by a 32-bit op, so x1 is clean */
}

/* Load size bytes big-endian from [x2] into w0. */
static void load_guest(emitter *e, unsigned size)
{
    if (size == 1) ldrb_(e, 0, 2, 0);
    else if (size == 2) { ldrh_(e, 0, 2, 0); rev16_w(e, 0, 0); }
    else { ldr_w(e, 0, 2, 0); rev_w(e, 0, 0); }
}

/* Store the size low bytes of w0 big-endian to [x2], leaving w0 alone so
 * that the condition codes can still be taken from it. */
static void store_guest(emitter *e, unsigned size)
{
    if (size == 1) strb_(e, 0, 2, 0);
    else if (size == 2) { rev16_w(e, 1, 0); strh_(e, 1, 2, 0); }
    else { rev_w(e, 1, 0); str_w(e, 1, 2, 0); }
}

/*
 * A return: the address comes off the stack and the stack pointer moves
 * after it, in that order, so that a lookup that fails has changed nothing.
 */
static void emit_rts(emitter *e, unsigned *slow, unsigned *nslow)
{
    ldr_w(e, 0, X19, A(7));
    page_lookup(e, 4, false, slow, nslow);
    load_guest(e, 4);
    str_w(e, 0, X19, PC);
    ldr_w(e, 0, X19, A(7));
    add_imm_w(e, 0, 0, 4);
    str_w(e, 0, X19, A(7));
}

/*
 * A call: the target is worked out, the return address is written below
 * the stack pointer, and only then do the stack pointer and the program
 * counter move. w5 holds the target and w6 the stack pointer it will have,
 * both dead if the lookup sends this to the interpreter instead.
 */
static void emit_jsr(emitter *e, const m68k_insn *insn,
                     unsigned *slow, unsigned *nslow)
{
    address_into_w0(e, &insn->src);
    mov_rr(e, 5, 0);
    ldr_w(e, 0, X19, A(7));
    sub_imm_w(e, 0, 0, 4);
    mov_rr(e, 6, 0);
    page_lookup(e, 4, true, slow, nslow);
    mov_w(e, 0, insn->next_pc);
    store_guest(e, 4);
    str_w(e, 6, X19, A(7));
    str_w(e, 5, X19, PC);
}

/* The relative target is known at decode time; otherwise this is the same
 * transactional stack push used by JSR. */
static void emit_bsr(emitter *e, const m68k_insn *insn,
                     unsigned *slow, unsigned *nslow)
{
    mov_w(e, 5, insn->extra);
    ldr_w(e, 0, X19, A(7));
    sub_imm_w(e, 0, 0, 4);
    mov_rr(e, 6, 0);
    page_lookup(e, 4, true, slow, nslow);
    mov_w(e, 0, insn->next_pc);
    store_guest(e, 4);
    str_w(e, 6, X19, A(7));
    str_w(e, 5, X19, PC);
}

bool m68k_emit_supported(void) { return true; }

/*
 * A move between a data register and ordinary memory, inline. Anything the
 * mapping does not cover falls through to the interpreter for that one
 * instruction, which is why the fast path forms the address and looks the
 * page up before it changes anything at all.
 */
static void emit_memory_move(emitter *e, const m68k_insn *insn,
                             unsigned *slow, unsigned *nslow)
{
    unsigned size = insn->size;
    if (insn->dst.mode == M68K_DN) {                     /* memory to register */
        address_into_w0(e, &insn->src);
        page_lookup(e, size, false, slow, nslow);
        load_guest(e, size);
        if (size == 4) str_w(e, 0, X19, D(insn->dst.reg));
        else if (size == 2) strh_(e, 0, X19, D(insn->dst.reg));
        else strb_(e, 0, X19, D(insn->dst.reg));
        move_flags(e, size);
    } else {                                             /* register to memory */
        address_into_w0(e, &insn->dst);
        page_lookup(e, size, true, slow, nslow);
        ldr_w(e, 0, X19, D(insn->src.reg));
        store_guest(e, size);
        move_flags(e, size);
    }
}

static void emit_memory_movea(emitter *e, const m68k_insn *insn,
                             unsigned *slow, unsigned *nslow)
{
    address_into_w0(e, &insn->src);
    page_lookup(e, insn->size, false, slow, nslow);
    load_guest(e, insn->size);
    if (insn->size == 2) asr_imm(e, 0, 0, 16);
    str_w(e, 0, X19, A(insn->dst.reg));
}

/* As on x86, form and validate the address before changing either operand.
 * The fallback can therefore execute the original instruction exactly once
 * when the page is device-backed, unmapped, or crosses a page boundary. */
static void emit_memory_alu(emitter *e, const m68k_insn *insn,
                            unsigned *slow, unsigned *nslow)
{
    unsigned shift = width_shift(insn->size);
    bool postinc = insn->src.mode == M68K_PI;
    bool predec = insn->src.mode == M68K_PD;
    if (postinc || predec) {
        unsigned delta = (insn->size == 1 && insn->src.reg == 7) ? 2 :
                         insn->size;
        ldr_w(e, 0, X19, A(insn->src.reg));
        if (postinc) add_imm_w(e, 5, 0, delta);
        else {
            sub_imm_w(e, 5, 0, delta);
            mov_rr(e, 0, 5);
        }
    } else {
        address_into_w0(e, &insn->src);
    }
    page_lookup(e, insn->size, false, slow, nslow);
    if (postinc || predec) str_w(e, 5, X19, A(insn->src.reg));
    load_guest(e, insn->size);                 /* source -> w0 */
    mov_rr(e, 1, 0);                           /* source -> w1 */
    ldr_w(e, 0, X19, D(insn->dst.reg));        /* destination -> w0 */
    lsl_imm(e, 0, 0, shift);
    lsl_imm(e, 1, 1, shift);

    switch (insn->op) {
    case M68K_OP_ADD:
    case M68K_OP_SUB:
        if (insn->op == M68K_OP_ADD) adds_ww(e, 3, 0, 1);
        else subs_ww(e, 3, 0, 1);
        set_flag(e, insn->op == M68K_OP_ADD ? A64_CS : A64_CC, FLAG(c));
        strb_(e, 4, X19, FLAG(x));
        set_flag(e, A64_VS, FLAG(v));
        set_flag(e, A64_MI, FLAG(n));
        set_flag(e, A64_EQ, FLAG(z));
        alu_store(e, insn, shift);
        return;
    case M68K_OP_CMP:
        cmp_ww(e, 0, 1);
        set_flag(e, A64_CC, FLAG(c));
        set_flag(e, A64_VS, FLAG(v));
        set_flag(e, A64_MI, FLAG(n));
        set_flag(e, A64_EQ, FLAG(z));
        return;
    case M68K_OP_AND:
    case M68K_OP_OR:
    case M68K_OP_EOR:
        if (insn->op == M68K_OP_AND) and_ww(e, 3, 0, 1);
        else if (insn->op == M68K_OP_OR) orr_ww(e, 3, 0, 1);
        else eor_ww(e, 3, 0, 1);
        cmp_imm_w(e, 3, 0);
        set_flag(e, A64_MI, FLAG(n));
        set_flag(e, A64_EQ, FLAG(z));
        store_imm8(e, FLAG(v), 0);
        store_imm8(e, FLAG(c), 0);
        alu_store(e, insn, shift);
        return;
    default:
        return;
    }
}

/*
 * Room for every branch out of the block: three per instruction, one per
 * eight bytes of the guard, and the entry check. Sizing this for the
 * common block rather than the longest one overruns the array, which is a
 * stack overwrite that shows itself as a wrong answer millions of
 * instructions later.
 */
#define MAX_EXITS (M68K_BLOCK_MAX * 3 + \
                   (M68K_BLOCK_MAX * MRC_JIT_M68K_MAX_BYTES) / 8 + 16)

size_t m68k_emit(uint8_t *out, const uint8_t *exec, size_t cap,
                 const m68k_insn *insn, unsigned count, uint32_t va,
                 const uint8_t *guest, const uint8_t *expect,
                 unsigned guard_bytes, bool cycles, unsigned *native,
                 m68k_emit_points *points, void *owner)
{
    (void)exec;
    emitter e = { (uint32_t *)out, 0, (unsigned)(cap / 4), false, cycles };
    unsigned translated = 0;
    unsigned exits[MAX_EXITS], nexits = 0, sink = 0;
    /* A full table is an overflow, not an overwrite. */
    #define EXIT_SLOT() (nexits < MAX_EXITS ? &exits[nexits++] \
                                            : (e.overflow = true, &sink))

    static const uint32_t prologue[] = {
        0xA9BC7BFD,     /* stp x29, x30, [sp, #-64]! */
        0xA90153F3,     /* stp x19, x20, [sp, #16]   */
        0xA9025BF5,     /* stp x21, x22, [sp, #32]   */
        0xF9001BF7,     /* str x23, [sp, #48]        */
        0xAA0003F3,     /* mov x19, x0               */
        0x2A0103F7,     /* mov w23, w1: the budget   */
        0x2A1F03F4,     /* mov w20, wzr              */
    };
    for (unsigned k = 0; k < sizeof prologue / 4; k++)
        insn_word(&e, prologue[k]);
    ldr_x(&e, X21, X19,
          offsetof(m68k, bus) + offsetof(m68k_bus, read_pages));
    ldr_x(&e, X22, X19,
          offsetof(m68k, bus) + offsetof(m68k_bus, write_pages));

    /*
     * A block is only ever correct at the address it was compiled for, so
     * it says so itself. This costs one compare and turns running the
     * wrong block into refusing to run at all, which is the difference
     * between a wrong answer millions of instructions later and a rebuild.
     */
    const unsigned chain = here(&e);
    ldr_w(&e, 0, X19, PC);
    mov_w(&e, 1, va);
    cmp_ww(&e, 0, 1);
    b_cond(&e, A64_NE, EXIT_SLOT());

    /*
     * And that the whole block fits what the caller will accept. The
     * caller used to guarantee this, and still does for the first block;
     * a block reached from another one has no caller to have checked.
     */
    add_imm_w(&e, 0, 20, count);
    cmp_ww(&e, 0, 23);
    b_cond(&e, A64_HI, EXIT_SLOT());

    /*
     * The block checks its own source bytes before running any of them.
     * Doing it here rather than in the caller is what lets one block jump
     * straight to another later: a block reached that way has no caller to
     * check it.
     *
     * Both sides are read from memory, against the x86-64 backend's
     * literal immediates. The bytes to expect live in the block cache
     * entry, whose lifetime is already the code's -- the emitted calls
     * point into the same entry's decoded instructions -- and reading them
     * is four instructions per eight bytes where building each literal
     * would be seven.
     *
     * Failing sends control to the ordinary exit, which returns the
     * retired count. That count is still zero here, and a block that gets
     * past this point always retires at least its first instruction, so
     * zero means stale and nothing else.
     */
    if (guest) {
        mov_x(&e, 9, (uint64_t)(uintptr_t)guest);
        mov_x(&e, 10, (uint64_t)(uintptr_t)expect);
        for (unsigned k = 0; k < guard_bytes; ) {
            unsigned left = guard_bytes - k;
            if (left >= 8) {
                ldr_x(&e, 0, 9, k); ldr_x(&e, 1, 10, k);
                cmp_xx(&e, 0, 1); k += 8;
            } else if (left >= 4) {
                ldr_w(&e, 0, 9, k); ldr_w(&e, 1, 10, k);
                cmp_ww(&e, 0, 1); k += 4;
            } else {
                ldrh_(&e, 0, 9, k); ldrh_(&e, 1, 10, k);
                cmp_ww(&e, 0, 1); k += 2;
            }
            b_cond(&e, A64_NE, EXIT_SLOT());
        }
    }

    uint32_t at = va;
    for (unsigned k = 0; k < count; k++) {
        const m68k_insn *in = &insn[k];
        uint32_t next = at + in->length;
        bool last = k + 1 == count;

        if (m68k_inline_memory(in) && !e.overflow) {
            /*
             * The fast path, then a branch past the interpreter call that
             * follows it. Anything the mapping did not cover lands on that
             * call having changed nothing, which is what makes the two
             * interchangeable.
             */
            unsigned slow[4], nslow = 0, past;
            if (in->op == M68K_OP_RTS) emit_rts(&e, slow, &nslow);
            else if (in->op == M68K_OP_JSR) emit_jsr(&e, in, slow, &nslow);
            else if (in->op == M68K_OP_BSR) emit_bsr(&e, in, slow, &nslow);
            else if (m68k_memory_alu(in)) emit_memory_alu(&e, in, slow, &nslow);
            else if (m68k_memory_movea(in)) emit_memory_movea(&e, in, slow, &nslow);
            else emit_memory_move(&e, in, slow, &nslow);
            if (m68k_sets_own_pc(in)) store_imm32(&e, PREVPC, at);
            else store_imm32(&e, PC, next);
            bump_retired(&e);
            bump_insns(&e);
            bump_cycles(&e, in->cycles);
            b_always(&e, &past);
            for (unsigned x = 0; x < nslow; x++) patch(&e, slow[x], here(&e));
            store_imm32(&e, PC, at);
            insn_word(&e, 0xAA1303E0u);                  /* mov x0, x19 */
            mov_x(&e, 1, (uint64_t)(uintptr_t)in);
            call(&e, (const void *)m68k_execute);
            bump_retired(&e);
            cbz_w(&e, 0, EXIT_SLOT());                   /* it raised */
            /*
             * Only the slow path can have reached a device, and only a
             * device can have asked for the block to stop here.
             */
            if (!last) {
                ldrb_(&e, 1, X19, YIELD);
                cbnz_w(&e, 1, EXIT_SLOT());
            }
            patch(&e, past, here(&e));
            if (!last) {
                ldr_w(&e, 0, X19, PC);
                mov_w(&e, 1, next);
                cmp_ww(&e, 0, 1);
                b_cond(&e, A64_NE, EXIT_SLOT());
            }
            translated++;
        } else if (m68k_translatable(in)) {
            translate(&e, in);
            if (!m68k_sets_own_pc(in))
                store_imm32(&e, PC, next);
            else
                /* Only a branch can leave the program counter somewhere
                 * this block did not choose, and only then does anyone
                 * need to know which instruction put it there. */
                store_imm32(&e, PREVPC, at);
            bump_retired(&e);
            bump_insns(&e);
            bump_cycles(&e, in->cycles);
            translated++;
        } else {
            /*
             * Hand this one to the interpreter. The program counter is set
             * to the instruction first, because that is where the
             * interpreter expects to find itself, and the three checks
             * after the call are the same three the block runner makes:
             * did it raise, did it go anywhere other than the next
             * instruction, and has the machine asked to stop.
             */
            store_imm32(&e, PC, at);
            insn_word(&e, 0xAA1303E0u);                  /* mov x0, x19 */
            mov_x(&e, 1, (uint64_t)(uintptr_t)in);
            call(&e, (const void *)m68k_execute);
            bump_retired(&e);
            if (!last) {
                cbz_w(&e, 0, EXIT_SLOT());
                ldr_w(&e, 0, X19, PC);
                mov_w(&e, 1, next);
                cmp_ww(&e, 0, 1);
                b_cond(&e, A64_NE, EXIT_SLOT());
                ldrb_(&e, 1, X19, YIELD);
                cbnz_w(&e, 1, EXIT_SLOT());
            }
        }
        at = next;
    }

    /*
     * Round again, without leaving.
     *
     * Only when the last instruction branches to this block's own first
     * one, which is what a loop looks like from here. The jump goes back
     * to the entry check rather than to the body, so the program counter
     * and the guest's own bytes are tested every time: the branch may not
     * have been taken, and the guest may have rewritten the loop. Failing
     * either lands on the ordinary exit, which is what should happen.
     *
     * Before jumping, two things the caller would otherwise have checked:
     * that another pass fits in the budget, and that nothing has asked for
     * the processor back.
     */
    /*
     * Whoever runs next.
     *
     * Nothing has asked for the processor back, so control can stay in
     * here: branch to whichever block the machine went to last time from
     * this one. It starts out pointing at this block's own exit and is
     * redirected by m68k_patch_link once the caller has seen where control
     * actually goes -- which it usually can, since 90% of the time a block
     * is followed by the same block as last time.
     *
     * Nothing is assumed about where it lands. The successor's chain entry
     * checks the program counter, the budget and its own bytes, and falls
     * out to the shared exit if any of them says no. The frame and the
     * retired count are the ones this block's prologue set up, so a chain
     * of blocks returns one count for all of them.
     */
    ldrb_(&e, 1, X19, YIELD);
    cbnz_w(&e, 1, EXIT_SLOT());
    /*
     * And that the part is still fetching. An LPSTOP retires like any
     * other instruction and then nothing more is fetched until an
     * interrupt arrives, which is the owner's business -- so a chain has
     * to stop here, where the caller's loop used to.
     */
    ldrb_(&e, 1, X19, STOPPED);
    cbnz_w(&e, 1, EXIT_SLOT());
    const unsigned link = here(&e);
    insn_word(&e, 0x14000000u);          /* b stub, filled in below */
    /*
     * Where that branch goes until it is resolved: say who is asking, and
     * leave. Once the caller has pointed it at a block this is
     * unreachable, which is why the cost of asking is paid once.
     */
    {
        const unsigned stub = here(&e);
        if (link < e.cap)
            e.out[link] = 0x14000000u |
                          ((uint32_t)((int32_t)stub - (int32_t)link) &
                           0x03FFFFFFu);
        mov_x(&e, 9, (uint64_t)(uintptr_t)owner);
        str_x(&e, 9, X19, offsetof(m68k, link_request));
        b_always(&e, EXIT_SLOT());
    }

    unsigned end = here(&e);
    static const uint32_t epilogue[] = {
        0x2A1403E0,     /* mov w0, w20              */
        0xF9401BF7,     /* ldr x23, [sp, #48]       */
        0xA9425BF5,     /* ldp x21, x22, [sp, #32]  */
        0xA94153F3,     /* ldp x19, x20, [sp, #16]  */
        0xA8C47BFD,     /* ldp x29, x30, [sp], #64  */
        0xD65F03C0,     /* ret                      */
    };
    for (unsigned k = 0; k < sizeof epilogue / 4; k++)
        insn_word(&e, epilogue[k]);

    if (e.overflow) return 0;
    for (unsigned k = 0; k < nexits; k++) patch(&e, exits[k], end);
    if (native) *native = translated;
    if (points) { points->chain = (size_t)chain * 4;
                  points->link = (size_t)link * 4; }
    return (size_t)e.at * 4;
    #undef EXIT_SLOT
}

/*
 * The link is an unconditional branch, whose twenty-six-bit displacement
 * counts instructions rather than bytes and reaches 128 MiB either way --
 * comfortably more than one code arena.
 */
void m68k_patch_link(uint8_t *site, const uint8_t *site_exec,
                     const uint8_t *target)
{
    int32_t rel = (int32_t)((target - site_exec) / 4);
    uint32_t w = 0x14000000u | ((uint32_t)rel & 0x03FFFFFFu);
    memcpy(site, &w, 4);
}

#endif
