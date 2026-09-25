#include "cpu/m68k/core/m68k_emit.h"
#include "cpu/m68k/core/m68k_block.h"
#include "cpu/m68k/core/m68k_emit_policy.h"

#if defined(MRC_M68K_EMIT_X86)
#include <stddef.h>
#include <string.h>
#include <stdlib.h>

/*
 * x86-64, System V or Windows x64. Inside a block:
 *
 *   rbx   the m68k structure
 *   r12d  instructions retired so far, and the return value
 *   r13   the table of readable pages, r14 the writable ones
 *   rax, rcx, rdx, rdi, rsi  scratch, and dead at every interpreter call
 *
 * Guest state is never held in a host register across an instruction, so
 * an interpreter call in the middle of a block sees the machine exactly as
 * it would have on its own.
 *
 * Windows x64 differs in three ways that matter here: the arguments come
 * in rcx and rdx, rsi and rdi belong to the caller, and every call needs
 * 32 bytes of shadow space below the return address. The prologue saves
 * rsi and rdi with the rest and reserves the shadow space for the block's
 * lifetime; blocks chained together share that frame.
 */
enum { RAX, RCX, RDX, RBX, RSP, RBP, RSI, RDI, R8, R9, R10, R11, R12, R13, R14 };

typedef struct {
    uint8_t *out;
    size_t at, cap;
    bool overflow;
    /* Keep the cycle count as well as the instruction count. Only a
     * machine that reads it asks; see m68k.count_cycles. */
    bool cycles;
} emitter;

static void byte(emitter *e, unsigned b)
{
    if (e->at < e->cap) e->out[e->at++] = (uint8_t)b;
    else e->overflow = true;
}
static void bytes(emitter *e, const void *p, size_t n)
{
    for (size_t k = 0; k < n; k++) byte(e, ((const uint8_t *)p)[k]);
}
static void u32(emitter *e, uint32_t v) { bytes(e, &v, 4); }
static void u64(emitter *e, uint64_t v) { bytes(e, &v, 8); }

static void rex(emitter *e, bool w, unsigned reg, unsigned base)
{
    unsigned r = 0x40 | (w << 3) | ((reg >> 3) << 2) | (base >> 3);
    if (r != 0x40) byte(e, r);
}

/* ModRM for [rbx + disp], which is where every guest field lives. */
static void field(emitter *e, unsigned reg, size_t offset)
{
    int32_t disp = (int32_t)offset;
    if (disp >= -128 && disp < 128) {
        byte(e, 0x40 | ((reg & 7) << 3) | RBX);
        byte(e, (uint8_t)disp);
    } else {
        byte(e, 0x80 | ((reg & 7) << 3) | RBX);
        u32(e, (uint32_t)disp);
    }
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

/*
 * Count the instruction, in the structure rather than only in the register
 * the block returns.
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
    byte(e, 0x48); byte(e, 0xFF); field(e, 0, INSNS);   /* incq insns(rbx) */
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
static void bump_cycles(emitter *e, unsigned n)
{
    if (!e->cycles || !n) return;
    /* addq $n, cycles(rbx) */
    byte(e, 0x48); byte(e, 0x81); field(e, 0, CYCLES); u32(e, n);
}

/* mov dword [rbx+offset], imm32 */
static void store_imm32(emitter *e, size_t offset, uint32_t value)
{
    byte(e, 0xC7); field(e, 0, offset); u32(e, value);
}
/* mov byte [rbx+offset], imm8 */
static void store_imm8(emitter *e, size_t offset, unsigned value)
{
    byte(e, 0xC6); field(e, 0, offset); byte(e, value);
}
/* mov r64, qword [rbx+offset] */
static void load64(emitter *e, unsigned reg, size_t offset)
{
    rex(e, true, reg, RBX); byte(e, 0x8B); field(e, reg, offset);
}
/* mov r32, dword [rbx+offset] */
static void load32(emitter *e, unsigned reg, size_t offset)
{
    rex(e, false, reg, RBX); byte(e, 0x8B); field(e, reg, offset);
}
/* mov dword [rbx+offset], r32, and the byte and word widths of it */
static void store32(emitter *e, size_t offset, unsigned reg)
{
    rex(e, false, reg, RBX); byte(e, 0x89); field(e, reg, offset);
}
static void store16(emitter *e, size_t offset, unsigned reg)
{
    byte(e, 0x66); rex(e, false, reg, RBX); byte(e, 0x89); field(e, reg, offset);
}
static void store8(emitter *e, size_t offset, unsigned reg)
{
    rex(e, false, reg, RBX); byte(e, 0x88); field(e, reg, offset);
}

/*
 * The condition codes a move leaves behind: negative and zero taken from
 * the value at its own width, overflow and carry cleared. The value must
 * already be in eax, and the test is issued at the operand's width so that
 * the sign is the guest's sign and not a host register's.
 */
static void move_flags(emitter *e, unsigned size)
{
    if (size == 1) { byte(e, 0x84); byte(e, 0xC0); }            /* test al,al  */
    else if (size == 2) { byte(e, 0x66); byte(e, 0x85); byte(e, 0xC0); }
    else { byte(e, 0x85); byte(e, 0xC0); }                      /* test eax,eax */
    byte(e, 0x0F); byte(e, 0x98); byte(e, 0xC2);                /* sets dl      */
    store8(e, FLAG(n), RDX);
    byte(e, 0x0F); byte(e, 0x94); byte(e, 0xC2);                /* setz dl      */
    store8(e, FLAG(z), RDX);
    store_imm8(e, FLAG(v), 0);
    store_imm8(e, FLAG(c), 0);
}

/* movzx r32, byte [rbx+offset] */
static void load_flag(emitter *e, unsigned reg, size_t offset)
{
    rex(e, false, reg, RBX);
    byte(e, 0x0F); byte(e, 0xB6); field(e, reg, offset);
}
/* xor r8, imm8 and or r8, r8, on the low bytes of dl and al */
static void xor_dl(emitter *e, unsigned imm) { byte(e, 0x80); byte(e, 0xF2); byte(e, imm); }
static void or_dl_al(emitter *e) { byte(e, 0x08); byte(e, 0xC2); }
static void xor_dl_al(emitter *e) { byte(e, 0x30); byte(e, 0xC2); }
static void mov_imm32(emitter *e, unsigned reg, uint32_t v)
{
    rex(e, false, 0, reg); byte(e, 0xB8 | (reg & 7)); u32(e, v);
}
/* ModRM for [rsi + offset], choosing a displacement wide enough. */
static void from_rsi(emitter *e, unsigned reg, unsigned offset)
{
    if (offset < 128) {
        byte(e, 0x40 | ((reg & 7) << 3) | RSI);
        byte(e, (uint8_t)offset);
    } else {
        byte(e, 0x80 | ((reg & 7) << 3) | RSI);
        u32(e, offset);
    }
}
static void mov_imm64(emitter *e, unsigned reg, uint64_t v)
{
    rex(e, true, 0, reg); byte(e, 0xB8 | (reg & 7)); u64(e, v);
}

/* m68k_execute(c, in), leaving its answer in al. */
static void call_interpreter(emitter *e, const m68k_insn *in)
{
#ifdef _WIN32
    byte(e, 0x48); byte(e, 0x89); byte(e, 0xD9);        /* mov rcx,rbx */
    mov_imm64(e, RDX, (uint64_t)(uintptr_t)in);
#else
    byte(e, 0x48); byte(e, 0x89); byte(e, 0xDF);        /* mov rdi,rbx */
    mov_imm64(e, RSI, (uint64_t)(uintptr_t)in);
#endif
    mov_imm64(e, RAX, (uint64_t)(uintptr_t)m68k_execute);
    byte(e, 0xFF); byte(e, 0xD0);                       /* call rax */
}

/*
 * The condition, as a zero or one in dl.
 *
 * The condition codes are kept as whole bytes holding zero or one, which
 * is what makes this short: the flag is already the answer for half the
 * conditions, and the rest are one or two operations on two of them. A
 * packed status register would need a mask and a shift before any of that.
 */
static void condition(emitter *e, unsigned cond)
{
    switch (cond) {
    case M68K_COND_T: byte(e, 0xB2); byte(e, 1); return;    /* mov dl, 1 */
    case M68K_COND_F: byte(e, 0xB2); byte(e, 0); return;
    case M68K_COND_EQ: load_flag(e, RDX, FLAG(z)); return;
    case M68K_COND_NE: load_flag(e, RDX, FLAG(z)); xor_dl(e, 1); return;
    case M68K_COND_CS: load_flag(e, RDX, FLAG(c)); return;
    case M68K_COND_CC: load_flag(e, RDX, FLAG(c)); xor_dl(e, 1); return;
    case M68K_COND_MI: load_flag(e, RDX, FLAG(n)); return;
    case M68K_COND_PL: load_flag(e, RDX, FLAG(n)); xor_dl(e, 1); return;
    case M68K_COND_VS: load_flag(e, RDX, FLAG(v)); return;
    case M68K_COND_VC: load_flag(e, RDX, FLAG(v)); xor_dl(e, 1); return;
    case M68K_COND_LS:                                      /* carry or zero */
        load_flag(e, RDX, FLAG(c)); load_flag(e, RAX, FLAG(z));
        or_dl_al(e); return;
    case M68K_COND_HI:
        load_flag(e, RDX, FLAG(c)); load_flag(e, RAX, FLAG(z));
        or_dl_al(e); xor_dl(e, 1); return;
    case M68K_COND_LT:                                      /* n differs from v */
        load_flag(e, RDX, FLAG(n)); load_flag(e, RAX, FLAG(v));
        xor_dl_al(e); return;
    case M68K_COND_GE:
        load_flag(e, RDX, FLAG(n)); load_flag(e, RAX, FLAG(v));
        xor_dl_al(e); xor_dl(e, 1); return;
    case M68K_COND_LE:                                      /* that, or zero */
        load_flag(e, RDX, FLAG(n)); load_flag(e, RAX, FLAG(v));
        xor_dl_al(e); load_flag(e, RAX, FLAG(z)); or_dl_al(e); return;
    default:                                                /* GT */
        load_flag(e, RDX, FLAG(n)); load_flag(e, RAX, FLAG(v));
        xor_dl_al(e); load_flag(e, RAX, FLAG(z)); or_dl_al(e);
        xor_dl(e, 1); return;
    }
}


/* Defined below, next to the inline memory moves that share it. */
static void address_into_eax(emitter *e, const m68k_ea *ea);

/* ------------------------------------------------------- arithmetic */
/*
 * Byte, word and long share one shape.
 *
 * Both operands are shifted to the top of a 32-bit register before the
 * operation, so the host's own carry, overflow, sign and zero come out at
 * the guest's width rather than at the host's. That is what makes the
 * flags exact instead of reconstructed, and it is why there are no
 * separate cases per size below.
 */
static unsigned width_shift(unsigned size)
{
    return size == 1 ? 24 : size == 2 ? 16 : 0;
}
static uint32_t width_mask(unsigned size)
{
    return size == 1 ? 0xFFu : size == 2 ? 0xFFFFu : 0xFFFFFFFFu;
}

static void shl_imm(emitter *e, unsigned reg, unsigned by)
{
    if (!by) return;
    byte(e, 0xC1); byte(e, 0xE0 | (reg & 7)); byte(e, by);
}
static void shr_imm(emitter *e, unsigned reg, unsigned by)
{
    if (!by) return;
    byte(e, 0xC1); byte(e, 0xE8 | (reg & 7)); byte(e, by);
}
static void sar_imm(emitter *e, unsigned reg, unsigned by)
{
    if (!by) return;
    byte(e, 0xC1); byte(e, 0xF8 | (reg & 7)); byte(e, by);
}
static void and_imm32(emitter *e, unsigned reg, uint32_t v)
{
    if (v == 0xFFFFFFFFu) return;
    if (reg == RAX) { byte(e, 0x25); u32(e, v); return; }
    byte(e, 0x81); byte(e, 0xE0 | (reg & 7)); u32(e, v);
}
static void mov_rr(emitter *e, unsigned rd, unsigned rs)
{
    byte(e, 0x89); byte(e, 0xC0 | ((rs & 7) << 3) | (rd & 7));
}

static void setcc(emitter *e, unsigned cc, size_t flag)
{
    byte(e, 0x0F); byte(e, cc); byte(e, 0xC2);       /* setcc dl */
    store8(e, flag, RDX);
}
#define SET_O 0x90
#define SET_C 0x92
#define SET_Z 0x94
#define SET_NE 0x95
#define SET_S 0x98

/* Where a register source lives, which is the only thing that differs
 * between a data register and an address one. */
static size_t alu_src(const m68k_insn *in)
{
    return in->src.mode == M68K_AN ? A(in->src.reg) : D(in->src.reg);
}

/* The destination into eax and the source into ecx, both shifted up. */
static void alu_operands(emitter *e, const m68k_insn *in, unsigned shift)
{
    load32(e, RAX, D(in->dst.reg));
    shl_imm(e, RAX, shift);
    if (in->src.mode != M68K_IMM) {
        load32(e, RCX, alu_src(in));
        shl_imm(e, RCX, shift);
    } else {
        mov_imm32(e, RCX, ((uint32_t)in->src.disp & width_mask(in->size)) << shift);
    }
}

/* Put the value in eax back into the destination register at its width. */
static void alu_store(emitter *e, const m68k_insn *in, unsigned shift)
{
    shr_imm(e, RAX, shift);
    if (in->size == 4) store32(e, D(in->dst.reg), RAX);
    else if (in->size == 2) store16(e, D(in->dst.reg), RAX);
    else store8(e, D(in->dst.reg), RAX);
}

/*
 * The source of an address-register form, sign-extended into ecx. These
 * take their operand at its own size and then widen it, which is the one
 * place the guest's size means something other than a mask.
 */
static void alu_source_extended(emitter *e, const m68k_insn *in)
{
    if (in->src.mode != M68K_IMM) {
        if (in->size == 2) {                          /* movsx ecx, word [] */
            byte(e, 0x0F); byte(e, 0xBF); field(e, RCX, alu_src(in));
        } else load32(e, RCX, alu_src(in));
        return;
    }
    uint32_t v = (uint32_t)in->src.disp & width_mask(in->size);
    if (in->size == 2 && (v & 0x8000u)) v |= 0xFFFF0000u;
    mov_imm32(e, RCX, v);
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
        byte(e, 0xF7); byte(e, 0xDA);                    /* neg edx: 0 or -1 */
        load32(e, RAX, D(insn->dst.reg));
        and_imm32(e, RAX, 0xFFFFFF00u);
        byte(e, 0x08); byte(e, 0xD0);                    /* or al,dl */
        store32(e, D(insn->dst.reg), RAX);
        return;
    }
    case M68K_OP_BTST: {
        unsigned bit = (unsigned)insn->src.disp & 31u;
        load32(e, RAX, D(insn->dst.reg));
        byte(e, 0xC1); byte(e, 0xE8); byte(e, bit); /* shr eax, bit */
        byte(e, 0xF6); byte(e, 0xC0); byte(e, 0x01); /* test al, 1 */
        byte(e, 0x0F); byte(e, 0x94); byte(e, 0xC2); /* setz dl */
        store8(e, FLAG(z), RDX);
        return;
    }
    case M68K_OP_MULL: {
        uint16_t ext = (uint16_t)insn->extra;
        unsigned dl = (ext >> 12) & 7, dh = ext & 7;
        bool wide = (ext & 0x400) != 0, is_signed = (ext & 0x800) != 0;
        size_t source = insn->src.mode == M68K_AN
                            ? A(insn->src.reg) : D(insn->src.reg);
        if (insn->src.mode == M68K_IMM)
            mov_imm32(e, RAX, (uint32_t)insn->src.disp);
        else
            load32(e, RAX, source);
        load32(e, RCX, D(dl));
        if (is_signed) {
            rex(e, true, RAX, RAX); byte(e, 0x63); byte(e, 0xC0); /* movsxd rax,eax */
            rex(e, true, RCX, RCX); byte(e, 0x63); byte(e, 0xC9); /* movsxd rcx,ecx */
        }
        rex(e, true, RAX, RCX); byte(e, 0x0F); byte(e, 0xAF); byte(e, 0xC1);
        if (wide) {
            rex(e, true, RAX, RAX); byte(e, 0x85); byte(e, 0xC0); /* test rax,rax */
            setcc(e, SET_S, FLAG(n));
            setcc(e, SET_Z, FLAG(z));
            store_imm8(e, FLAG(v), 0);
            store_imm8(e, FLAG(c), 0);
            /* setcc uses DL, so form the product's high half afterwards. */
            rex(e, true, RAX, RDX); byte(e, 0x89); byte(e, 0xC2); /* mov rdx,rax */
            rex(e, true, 5, RDX); byte(e, 0xC1); byte(e, 0xEA); byte(e, 32);
            store32(e, D(dh), RDX);
            store32(e, D(dl), RAX);
        } else {
            byte(e, 0x85); byte(e, 0xC0);                 /* test eax,eax */
            setcc(e, SET_S, FLAG(n));
            setcc(e, SET_Z, FLAG(z));
            if (is_signed) {
                rex(e, true, RCX, RAX); byte(e, 0x63); byte(e, 0xC8); /* movsxd rcx,eax */
                rex(e, true, RCX, RAX); byte(e, 0x39); byte(e, 0xC8); /* cmp rax,rcx */
                setcc(e, SET_NE, FLAG(v));
            } else {
                rex(e, true, RAX, RDX); byte(e, 0x89); byte(e, 0xC2);
                rex(e, true, 5, RDX); byte(e, 0xC1); byte(e, 0xEA); byte(e, 32);
                byte(e, 0x85); byte(e, 0xD2);             /* test edx,edx */
                setcc(e, SET_NE, FLAG(v));
            }
            store_imm8(e, FLAG(c), 0);
            store32(e, D(dl), RAX);
        }
        return;
    }
    case M68K_OP_BRA:
        store_imm32(e, PC, insn->extra);
        return;
    case M68K_OP_BCC: {
        /* Both destinations are known now; the condition chooses. */
        condition(e, insn->cond);
        /*
         * Taking a byte-displacement branch costs two more than not
         * taking it; the word and long forms cost the same either way.
         * dl is the condition, so the extra is twice it.
         */
        if (e->cycles && insn->length == 2) {
            byte(e, 0x0F); byte(e, 0xB6); byte(e, 0xC2);  /* movzx eax, dl */
            byte(e, 0x01); byte(e, 0xC0);                 /* add eax, eax  */
            byte(e, 0x48); byte(e, 0x01); field(e, RAX, CYCLES); /* add [rbx+cyc], rax */
        }
        mov_imm32(e, RAX, insn->extra);
        mov_imm32(e, RCX, insn->next_pc);
        byte(e, 0x84); byte(e, 0xD2);                   /* test dl, dl   */
        byte(e, 0x0F); byte(e, 0x44); byte(e, 0xC1);    /* cmovz eax,ecx */
        store32(e, PC, RAX);
        return;
    }
    case M68K_OP_DBCC: {
        /*
         * The condition wins outright; otherwise the low half of the
         * register counts down and the branch is taken until it passes
         * zero, which for a sixteen-bit counter means reaching -1.
         */
        condition(e, insn->cond);
        mov_imm32(e, RAX, insn->next_pc);
        byte(e, 0x84); byte(e, 0xD2);                   /* test dl, dl */
        byte(e, 0x75); byte(e, 0);                      /* jnz done    */
        size_t patch_true = e->at - 1;

        byte(e, 0x66); rex(e, false, RCX, RBX);
        byte(e, 0x8B); field(e, RCX, D(insn->dst.reg)); /* mov cx,[Dn] */
        byte(e, 0x66); byte(e, 0xFF); byte(e, 0xC9);    /* dec cx      */
        store16(e, D(insn->dst.reg), RCX);
        byte(e, 0x66); byte(e, 0x83); byte(e, 0xF9); byte(e, 0xFF); /* cmp cx,-1 */
        byte(e, 0x74); byte(e, 0);                      /* je expired  */
        size_t patch_done = e->at - 1;
        mov_imm32(e, RAX, insn->extra);
        size_t patch_skip = 0;
        if (e->cycles) {
            /* Going round again is the cheap case; the loop ending by the
             * counter running out costs four more. */
            byte(e, 0xEB); byte(e, 0);                  /* jmp done    */
            patch_skip = e->at - 1;
            e->out[patch_done] = (uint8_t)(e->at - patch_done - 1);
            byte(e, 0x48); byte(e, 0x83); field(e, 0, CYCLES); byte(e, 4);
            e->out[patch_skip] = (uint8_t)(e->at - patch_skip - 1);
        } else {
            e->out[patch_done] = (uint8_t)(e->at - patch_done - 1);
        }
        e->out[patch_true] = (uint8_t)(e->at - patch_true - 1);
        store32(e, PC, RAX);
        return;
    }
    case M68K_OP_MOVE:
        if (insn->src.mode == M68K_IMM) {
            /* Nothing here depends on the machine, flags included. */
            uint32_t v = (uint32_t)insn->src.disp & width_mask(insn->size);
            if (insn->size == 4) store_imm32(e, D(insn->dst.reg), v);
            else if (insn->size == 2) {
                mov_imm32(e, RAX, v); store16(e, D(insn->dst.reg), RAX);
            } else { mov_imm32(e, RAX, v); store8(e, D(insn->dst.reg), RAX); }
            store_imm8(e, FLAG(n), (v >> (insn->size * 8 - 1)) & 1);
            store_imm8(e, FLAG(z), v == 0);
            store_imm8(e, FLAG(v), 0);
            store_imm8(e, FLAG(c), 0);
            return;
        }
        load32(e, RAX, alu_src(insn));
        if (insn->size == 4) store32(e, D(insn->dst.reg), RAX);
        else if (insn->size == 2) store16(e, D(insn->dst.reg), RAX);
        else store8(e, D(insn->dst.reg), RAX);
        move_flags(e, insn->size);
        return;

    case M68K_OP_ADD: case M68K_OP_ADDI: case M68K_OP_ADDQ:
    case M68K_OP_SUB: case M68K_OP_SUBI: case M68K_OP_SUBQ: {
        bool add = insn->op == M68K_OP_ADD || insn->op == M68K_OP_ADDI ||
                   insn->op == M68K_OP_ADDQ;
        if (insn->dst.mode == M68K_AN) {
            /* The whole register moves and nothing is said about it. */
            uint32_t delta = (uint32_t)insn->src.disp;
            load32(e, RAX, A(insn->dst.reg));
            byte(e, add ? 0x05 : 0x2D); u32(e, delta);
            store32(e, A(insn->dst.reg), RAX);
            return;
        }
        unsigned shift = width_shift(insn->size);
        alu_operands(e, insn, shift);
        byte(e, add ? 0x01 : 0x29); byte(e, 0xC8);
        setcc(e, SET_C, FLAG(c));
        store8(e, FLAG(x), RDX);           /* the extend follows the carry */
        setcc(e, SET_O, FLAG(v));
        setcc(e, SET_S, FLAG(n));
        setcc(e, SET_Z, FLAG(z));
        alu_store(e, insn, shift);
        return;
    }
    case M68K_OP_CMP: case M68K_OP_CMPI: {
        unsigned shift = width_shift(insn->size);
        alu_operands(e, insn, shift);
        byte(e, 0x39); byte(e, 0xC8);                 /* cmp eax, ecx */
        setcc(e, SET_C, FLAG(c));                     /* and X is left alone */
        setcc(e, SET_O, FLAG(v));
        setcc(e, SET_S, FLAG(n));
        setcc(e, SET_Z, FLAG(z));
        return;
    }
    case M68K_OP_AND: case M68K_OP_ANDI:
    case M68K_OP_OR:  case M68K_OP_ORI:
    case M68K_OP_EOR: case M68K_OP_EORI: {
        unsigned shift = width_shift(insn->size);
        alu_operands(e, insn, shift);
        byte(e, insn->op == M68K_OP_AND || insn->op == M68K_OP_ANDI ? 0x21 :
                insn->op == M68K_OP_OR || insn->op == M68K_OP_ORI ? 0x09 : 0x31);
        byte(e, 0xC8);
        setcc(e, SET_S, FLAG(n));
        setcc(e, SET_Z, FLAG(z));
        store_imm8(e, FLAG(v), 0);
        store_imm8(e, FLAG(c), 0);        /* and the extend is untouched */
        alu_store(e, insn, shift);
        return;
    }
    case M68K_OP_ADDA: case M68K_OP_SUBA: {
        alu_source_extended(e, insn);
        load32(e, RAX, A(insn->dst.reg));
        byte(e, insn->op == M68K_OP_ADDA ? 0x01 : 0x29); byte(e, 0xC8);
        store32(e, A(insn->dst.reg), RAX);
        return;
    }
    case M68K_OP_CMPA:
        /* Always a long comparison, whatever the source's size. */
        alu_source_extended(e, insn);
        load32(e, RAX, A(insn->dst.reg));
        byte(e, 0x39); byte(e, 0xC8);
        setcc(e, SET_C, FLAG(c));
        setcc(e, SET_O, FLAG(v));
        setcc(e, SET_S, FLAG(n));
        setcc(e, SET_Z, FLAG(z));
        return;
    case M68K_OP_TST: {
        unsigned shift = width_shift(insn->size);
        load32(e, RAX, D(insn->src.reg));
        shl_imm(e, RAX, shift);
        byte(e, 0x85); byte(e, 0xC0);                 /* test eax, eax */
        setcc(e, SET_S, FLAG(n));
        setcc(e, SET_Z, FLAG(z));
        store_imm8(e, FLAG(v), 0);
        store_imm8(e, FLAG(c), 0);
        return;
    }
    case M68K_OP_CLR:
        if (insn->size == 4) store_imm32(e, D(insn->dst.reg), 0);
        else {
            mov_imm32(e, RAX, 0);
            if (insn->size == 2) store16(e, D(insn->dst.reg), RAX);
            else store8(e, D(insn->dst.reg), RAX);
        }
        store_imm8(e, FLAG(n), 0);
        store_imm8(e, FLAG(z), 1);
        store_imm8(e, FLAG(v), 0);
        store_imm8(e, FLAG(c), 0);
        return;
    case M68K_OP_JMP:
        /* The address is the whole instruction. */
        address_into_eax(e, &insn->src);
        store32(e, PC, RAX);
        return;

    case M68K_OP_LSL: case M68K_OP_LSR: case M68K_OP_ASR:
    case M68K_OP_ROL: case M68K_OP_ROR: {
        unsigned n = (unsigned)insn->src.disp;      /* one to eight */
        unsigned bits = insn->size * 8;
        uint32_t m = width_mask(insn->size);
        bool left = insn->op == M68K_OP_LSL || insn->op == M68K_OP_ROL;
        bool rotate = insn->op == M68K_OP_ROL || insn->op == M68K_OP_ROR;

        load32(e, RAX, D(insn->dst.reg));
        and_imm32(e, RAX, m);
        /*
         * The carry is the last bit to leave, which for a known count is a
         * known bit of the value before anything moved. Taking it first is
         * what lets the result be computed in place afterwards.
         */
        mov_rr(e, RDX, RAX);
        shr_imm(e, RDX, left ? bits - n : n - 1);
        byte(e, 0x83); byte(e, 0xE2); byte(e, 1);   /* and edx, 1 */
        store8(e, FLAG(c), RDX);
        if (!rotate) store8(e, FLAG(x), RDX);       /* rotates leave it */

        switch (insn->op) {
        case M68K_OP_LSL:
            shl_imm(e, RAX, n); and_imm32(e, RAX, m); break;
        case M68K_OP_LSR:
            shr_imm(e, RAX, n); break;
        case M68K_OP_ASR:
            /* Sign-propagating at the guest's width, so the value goes to
             * the top of the register, comes back arithmetically, and is
             * brought down again. */
            shl_imm(e, RAX, 32 - bits);
            sar_imm(e, RAX, n);
            shr_imm(e, RAX, 32 - bits);
            break;
        case M68K_OP_ROL:
            mov_rr(e, RCX, RAX);
            shl_imm(e, RAX, n); shr_imm(e, RCX, bits - n);
            byte(e, 0x09); byte(e, 0xC8); and_imm32(e, RAX, m); break;
        default:                                    /* ROR */
            mov_rr(e, RCX, RAX);
            shr_imm(e, RAX, n); shl_imm(e, RCX, bits - n);
            byte(e, 0x09); byte(e, 0xC8); and_imm32(e, RAX, m); break;
        }
        store_imm8(e, FLAG(v), 0);
        mov_rr(e, RDX, RAX);
        shl_imm(e, RDX, 32 - bits);
        byte(e, 0x85); byte(e, 0xD2);               /* test edx, edx */
        setcc(e, SET_S, FLAG(n));
        setcc(e, SET_Z, FLAG(z));
        if (insn->size == 4) store32(e, D(insn->dst.reg), RAX);
        else if (insn->size == 2) store16(e, D(insn->dst.reg), RAX);
        else store8(e, D(insn->dst.reg), RAX);
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
        load32(e, RAX, D(insn->dst.reg));
        byte(e, 0xF7); byte(e, 0xD0);                 /* not eax: no flags */
        shl_imm(e, RAX, shift);
        byte(e, 0x85); byte(e, 0xC0);                 /* test eax, eax     */
        setcc(e, SET_S, FLAG(n));
        setcc(e, SET_Z, FLAG(z));
        store_imm8(e, FLAG(v), 0);
        store_imm8(e, FLAG(c), 0);
        alu_store(e, insn, shift);
        return;
    }
    default:
        return;
    }
}

/*
 * The address an effective address names, into eax, for the modes that
 * form one by adding a displacement to an address register. Modes with a
 * side effect are deliberately absent: the fast path may decide it cannot
 * proceed and hand the whole instruction to the interpreter, and it can
 * only do that if it has changed nothing yet.
 */
static void address_into_eax(emitter *e, const m68k_ea *ea)
{
    if (ea->mode == M68K_AW || ea->mode == M68K_AL) {
        mov_imm32(e, RAX, (uint32_t)ea->disp);
        return;
    }
    if (ea->mode == M68K_IX) {
        /*
         * Base plus index plus displacement, any of which the extension
         * word may have suppressed. Still no side effect, which is what
         * lets it share the fast path: an index is read and never written.
         * ecx is scratch until the page lookup, which is the next thing
         * that runs.
         */
        if (ea->base_used) load32(e, RAX, A(ea->reg));
        else mov_imm32(e, RAX, 0);
        if (ea->index_used) {
            size_t at = ea->index < 8 ? D(ea->index) : A(ea->index - 8);
            if (ea->index_long) load32(e, RCX, at);
            else { byte(e, 0x0F); byte(e, 0xBF); field(e, RCX, at); }
            unsigned by = ea->scale == 8 ? 3 : ea->scale == 4 ? 2 :
                          ea->scale == 2 ? 1 : 0;
            if (by) { byte(e, 0xC1); byte(e, 0xE1); byte(e, by); }
            byte(e, 0x01); byte(e, 0xC8);                /* add eax, ecx */
        }
        if (ea->disp) { byte(e, 0x05); u32(e, (uint32_t)ea->disp); }
        return;
    }
    load32(e, RAX, A(ea->reg));
    if (ea->disp) {
        byte(e, 0x05); u32(e, (uint32_t)ea->disp);       /* add eax, imm32 */
    }
}

/*
 * Turn the address in eax into a host pointer in rdx with the offset in
 * rcx, or jump to the slow path. Two things send it there: a page with no
 * direct mapping, which is how a device register looks from here, and an
 * access that would run off the end of one, which is rare enough to be
 * worth nothing and awkward enough to be worth avoiding.
 */
static void page_lookup(emitter *e, unsigned size, bool write,
                        size_t *slow, unsigned *nslow)
{
    byte(e, 0x89); byte(e, 0xC1);                        /* mov ecx, eax    */
    byte(e, 0xC1); byte(e, 0xE9); byte(e, M68K_PAGE_BITS); /* shr ecx, 12   */
    /* mov rdx, [r13 or r14 + rcx*8]. Both bases need the displacement
     * form: r13 encodes like rbp, which has no no-displacement mode. */
    byte(e, 0x49); byte(e, 0x8B); byte(e, 0x54);
    byte(e, 0xC0 | (RCX << 3) | ((write ? R14 : R13) & 7));
    byte(e, 0x00);
    rex(e, true, RDX, RDX); byte(e, 0x85); byte(e, 0xD2); /* test rdx, rdx  */
    byte(e, 0x0F); byte(e, 0x84); slow[(*nslow)++] = e->at; u32(e, 0);
    byte(e, 0x89); byte(e, 0xC1);                        /* mov ecx, eax    */
    byte(e, 0x81); byte(e, 0xE1); u32(e, 0xFFF);         /* and ecx, 0xfff  */
    byte(e, 0x81); byte(e, 0xF9); u32(e, 4096u - size);  /* cmp ecx, limit  */
    byte(e, 0x0F); byte(e, 0x87); slow[(*nslow)++] = e->at; u32(e, 0);
}

/* Load size bytes big-endian from [rdx+rcx] into eax. */
static void load_guest(emitter *e, unsigned size)
{
    if (size == 1) {
        byte(e, 0x0F); byte(e, 0xB6); byte(e, 0x04); byte(e, 0x0A); /* movzx eax,[rdx+rcx] */
    } else if (size == 2) {
        byte(e, 0x0F); byte(e, 0xB7); byte(e, 0x04); byte(e, 0x0A); /* movzx eax,word */
        byte(e, 0x66); byte(e, 0xC1); byte(e, 0xC0); byte(e, 8);    /* rol ax, 8 */
    } else {
        byte(e, 0x8B); byte(e, 0x04); byte(e, 0x0A);                /* mov eax,[rdx+rcx] */
        byte(e, 0x0F); byte(e, 0xC8);                               /* bswap eax */
    }
}

/* Store the size low bytes of eax big-endian to [rdx+rcx]. */
static void store_guest(emitter *e, unsigned size)
{
    if (size == 1) {
        byte(e, 0x88); byte(e, 0x04); byte(e, 0x0A);                /* mov [rdx+rcx],al */
    } else if (size == 2) {
        byte(e, 0x66); byte(e, 0xC1); byte(e, 0xC0); byte(e, 8);    /* rol ax, 8 */
        byte(e, 0x66); byte(e, 0x89); byte(e, 0x04); byte(e, 0x0A);
    } else {
        byte(e, 0x0F); byte(e, 0xC8);                               /* bswap eax */
        byte(e, 0x89); byte(e, 0x04); byte(e, 0x0A);
    }
}

bool m68k_emit_supported(void) { return true; }

/*
 * A move between a data register and ordinary memory, inline. Anything the
 * mapping does not cover falls through to the interpreter for that one
 * instruction, which is why the fast path forms the address and looks the
 * page up before it changes anything at all.
 */
static void emit_memory_move(emitter *e, const m68k_insn *insn,
                             size_t *slow, unsigned *nslow)
{
    unsigned size = insn->size;
    if (insn->dst.mode == M68K_DN) {                     /* memory to register */
        address_into_eax(e, &insn->src);
        page_lookup(e, size, false, slow, nslow);
        load_guest(e, size);
        if (size == 4) store32(e, D(insn->dst.reg), RAX);
        else if (size == 2) store16(e, D(insn->dst.reg), RAX);
        else store8(e, D(insn->dst.reg), RAX);
        move_flags(e, size);
    } else {                                             /* register to memory */
        address_into_eax(e, &insn->dst);
        page_lookup(e, size, true, slow, nslow);
        load32(e, RAX, D(insn->src.reg));
        store_guest(e, size);
        /* The stored value is what sets the flags, and storing it big-endian
         * consumed the register, so read it again rather than keep a copy
         * in one of the two the page lookup is using. */
        load32(e, RAX, D(insn->src.reg));
        move_flags(e, size);
    }
}

static void emit_memory_movea(emitter *e, const m68k_insn *insn,
                             size_t *slow, unsigned *nslow)
{
    address_into_eax(e, &insn->src);
    page_lookup(e, insn->size, false, slow, nslow);
    load_guest(e, insn->size);
    if (insn->size == 2) {
        byte(e, 0x0F); byte(e, 0xBF); byte(e, 0xC0);     /* movsx eax,ax */
    }
    store32(e, A(insn->dst.reg), RAX);
}

/* An ALU operation with a side-effect-free memory source.  The page lookup
 * happens before either operand is changed, so a device or unmapped page
 * falls through to m68k_execute with the instruction wholly untouched. */
static void emit_memory_alu(emitter *e, const m68k_insn *insn,
                            size_t *slow, unsigned *nslow)
{
    unsigned shift = width_shift(insn->size);
    bool postinc = insn->src.mode == M68K_PI;
    bool predec = insn->src.mode == M68K_PD;
    if (postinc || predec) {
        unsigned delta = (insn->size == 1 && insn->src.reg == 7) ? 2 :
                         insn->size;
        load32(e, RAX, A(insn->src.reg));
        mov_rr(e, RDI, RAX);                 /* candidate new An */
        if (postinc) {
            byte(e, 0x81); byte(e, 0xC7); u32(e, delta); /* add edi,delta */
        } else {
            byte(e, 0x81); byte(e, 0xEF); u32(e, delta); /* sub edi,delta */
            mov_rr(e, RAX, RDI);             /* predecremented address */
        }
    } else {
        address_into_eax(e, &insn->src);
    }
    page_lookup(e, insn->size, false, slow, nslow);
    if (postinc || predec) store32(e, A(insn->src.reg), RDI);
    load_guest(e, insn->size);                 /* source -> eax */
    mov_rr(e, RCX, RAX);                       /* source -> ecx */
    load32(e, RAX, D(insn->dst.reg));          /* destination -> eax */
    shl_imm(e, RAX, shift);
    shl_imm(e, RCX, shift);

    switch (insn->op) {
    case M68K_OP_ADD:
    case M68K_OP_SUB:
        byte(e, insn->op == M68K_OP_ADD ? 0x01 : 0x29); byte(e, 0xC8);
        setcc(e, SET_C, FLAG(c));
        store8(e, FLAG(x), RDX);
        setcc(e, SET_O, FLAG(v));
        setcc(e, SET_S, FLAG(n));
        setcc(e, SET_Z, FLAG(z));
        alu_store(e, insn, shift);
        return;
    case M68K_OP_CMP:
        byte(e, 0x39); byte(e, 0xC8);
        setcc(e, SET_C, FLAG(c));
        setcc(e, SET_O, FLAG(v));
        setcc(e, SET_S, FLAG(n));
        setcc(e, SET_Z, FLAG(z));
        return;
    case M68K_OP_AND:
    case M68K_OP_OR:
    case M68K_OP_EOR:
        byte(e, insn->op == M68K_OP_AND ? 0x21 :
                insn->op == M68K_OP_OR ? 0x09 : 0x31);
        byte(e, 0xC8);
        setcc(e, SET_S, FLAG(n));
        setcc(e, SET_Z, FLAG(z));
        store_imm8(e, FLAG(v), 0);
        store_imm8(e, FLAG(c), 0);
        alu_store(e, insn, shift);
        return;
    default:
        return;
    }
}

/*
 * A return: the address comes off the stack and the stack pointer moves
 * after it, in that order, so that a lookup that fails has changed nothing.
 */
static void emit_rts(emitter *e, size_t *slow, unsigned *nslow)
{
    load32(e, RAX, A(7));
    page_lookup(e, 4, false, slow, nslow);
    load_guest(e, 4);
    store32(e, PC, RAX);
    load32(e, RAX, A(7));
    byte(e, 0x83); byte(e, 0xC0); byte(e, 4);        /* add eax, 4 */
    store32(e, A(7), RAX);
}

/*
 * A call: the target is worked out, the return address is written below
 * the stack pointer, and only then do the stack pointer and the program
 * counter move. esi holds the target and edi the stack pointer it will
 * have, both dead if the lookup sends this to the interpreter instead.
 */
static void emit_jsr(emitter *e, const m68k_insn *insn,
                     size_t *slow, unsigned *nslow)
{
    address_into_eax(e, &insn->src);
    mov_rr(e, RSI, RAX);
    load32(e, RAX, A(7));
    byte(e, 0x83); byte(e, 0xE8); byte(e, 4);        /* sub eax, 4 */
    mov_rr(e, RDI, RAX);
    page_lookup(e, 4, true, slow, nslow);
    mov_imm32(e, RAX, insn->next_pc);
    store_guest(e, 4);
    store32(e, A(7), RDI);
    store32(e, PC, RSI);
}

/* BSR has the same checked stack write as JSR; decoding already resolved
 * its relative target, so it needs no effective-address calculation. */
static void emit_bsr(emitter *e, const m68k_insn *insn,
                     size_t *slow, unsigned *nslow)
{
    mov_imm32(e, RSI, insn->extra);
    load32(e, RAX, A(7));
    byte(e, 0x83); byte(e, 0xE8); byte(e, 4);        /* sub eax, 4 */
    mov_rr(e, RDI, RAX);
    page_lookup(e, 4, true, slow, nslow);
    mov_imm32(e, RAX, insn->next_pc);
    store_guest(e, 4);
    store32(e, A(7), RDI);
    store32(e, PC, RSI);
}

size_t m68k_emit(uint8_t *out, const uint8_t *exec, size_t cap,
                 const m68k_insn *insn, unsigned count, uint32_t va,
                 const uint8_t *guest, const uint8_t *expect,
                 unsigned guard_bytes, bool cycles, unsigned *native,
                 m68k_emit_points *points, void *owner)
{
    (void)exec;
    emitter e = { out, 0, cap, false, cycles };
    unsigned translated = 0;
    /* Where each early exit has to jump to, patched once the end is known. */
    /*
     * Room for every jump that can need patching: four per instruction,
     * plus one per eight bytes of the guard. Sizing this for the common
     * block rather than the longest one overruns the array on a long one,
     * which is a stack overwrite that shows itself as a wrong answer
     * millions of instructions later.
     */
    size_t exits[M68K_BLOCK_MAX * 5 +
                 (M68K_BLOCK_MAX * MRC_JIT_M68K_MAX_BYTES) / 2 + 8];
    unsigned nexits = 0;

#ifdef _WIN32
    static const uint8_t prologue[] = {
        0x53,                   /* push rbx                          */
        0x41, 0x54,             /* push r12                          */
        0x41, 0x55,             /* push r13                          */
        0x41, 0x56,             /* push r14                          */
        0x55,                   /* push rbp                          */
        0x56,                   /* push rsi                          */
        0x57,                   /* push rdi: seven pushes realign    */
        0x48, 0x83, 0xEC, 0x20, /* sub rsp, 32: the shadow space     */
        0x48, 0x89, 0xCB,       /* mov rbx, rcx                      */
        0x89, 0xD5,             /* mov ebp, edx: the budget          */
        0x45, 0x31, 0xE4,       /* xor r12d, r12d                    */
    };
#else
    static const uint8_t prologue[] = {
        0x53,                   /* push rbx                          */
        0x41, 0x54,             /* push r12                          */
        0x41, 0x55,             /* push r13                          */
        0x41, 0x56,             /* push r14                          */
        0x55,                   /* push rbp: five pushes realign rsp */
        0x48, 0x89, 0xFB,       /* mov rbx, rdi                      */
        0x89, 0xF5,             /* mov ebp, esi: the budget          */
        0x45, 0x31, 0xE4,       /* xor r12d, r12d                    */
    };
#endif
    bytes(&e, prologue, sizeof prologue);
    load64(&e, R13, offsetof(m68k, bus) + offsetof(m68k_bus, read_pages));
    load64(&e, R14, offsetof(m68k, bus) + offsetof(m68k_bus, write_pages));

    /*
     * The block checks its own source bytes before running any of them.
     * Doing it here rather than in the caller is what lets one block jump
     * straight to another later: a block reached that way has no caller to
     * check it. It costs one compare per eight bytes, and a block is
     * usually a few bytes long.
     *
     * Failing sends control to the ordinary exit, which returns the
     * retired count. That count is still zero here, and a block that gets
     * past this point always retires at least one instruction, so zero
     * means stale and nothing else.
     */
    /*
     * A block is only ever correct at the address it was compiled for, so
     * it says so itself. This costs one compare and turns running the
     * wrong block into refusing to run at all, which is the difference
     * between a wrong answer millions of instructions later and a
     * rebuild.
     */
    const size_t chain = e.at;
    byte(&e, 0x81); field(&e, 7, PC); u32(&e, va);
    byte(&e, 0x0F); byte(&e, 0x85);
    exits[nexits++] = e.at; u32(&e, 0);

    /*
     * And that the whole block fits what the caller will accept. The
     * caller used to guarantee this, and still does for the first block;
     * a block reached from another one has no caller to have checked.
     */
    byte(&e, 0x44); byte(&e, 0x89); byte(&e, 0xE0);      /* mov eax, r12d  */
    byte(&e, 0x05); u32(&e, count);                      /* add eax, count */
    byte(&e, 0x39); byte(&e, 0xE8);                      /* cmp eax, ebp   */
    byte(&e, 0x0F); byte(&e, 0x87);                      /* ja  exit       */
    exits[nexits++] = e.at; u32(&e, 0);

    if (guest) {
        mov_imm64(&e, RSI, (uint64_t)(uintptr_t)guest);
        for (unsigned k = 0; k < guard_bytes; ) {
            unsigned left = guard_bytes - k;
            /* The displacement has to be the wide form past 127 bytes,
             * and a block can reach several hundred. */
            if (left >= 8) {
                uint64_t want; memcpy(&want, expect + k, 8);
                byte(&e, 0x48); byte(&e, 0x8B); from_rsi(&e, RAX, k);
                mov_imm64(&e, RCX, want);
                byte(&e, 0x48); byte(&e, 0x39); byte(&e, 0xC8);   /* cmp rax,rcx */
                k += 8;
            } else if (left >= 4) {
                uint32_t want; memcpy(&want, expect + k, 4);
                byte(&e, 0x8B); from_rsi(&e, RAX, k);
                byte(&e, 0x3D); u32(&e, want);                    /* cmp eax,imm */
                k += 4;
            } else {
                uint16_t want; memcpy(&want, expect + k, 2);
                byte(&e, 0x66); byte(&e, 0x8B); from_rsi(&e, RAX, k);
                byte(&e, 0x66); byte(&e, 0x3D); bytes(&e, &want, 2);
                k += 2;
            }
            byte(&e, 0x0F); byte(&e, 0x85);                       /* jne exit */
            exits[nexits++] = e.at; u32(&e, 0);
        }
    }

    uint32_t at = va;
    for (unsigned k = 0; k < count; k++) {
        const m68k_insn *in = &insn[k];
        uint32_t next = at + in->length;
        bool last = k + 1 == count;

        if (m68k_inline_memory(in) && !e.overflow) {
            /*
             * The fast path, then a jump past the interpreter call that
             * follows it. Anything the mapping did not cover lands on that
             * call having changed nothing, which is what makes the two
             * interchangeable.
             */
            size_t slow[4];
            unsigned nslow = 0;
            if (in->op == M68K_OP_RTS) emit_rts(&e, slow, &nslow);
            else if (in->op == M68K_OP_JSR) emit_jsr(&e, in, slow, &nslow);
            else if (in->op == M68K_OP_BSR) emit_bsr(&e, in, slow, &nslow);
            else if (m68k_memory_alu(in)) emit_memory_alu(&e, in, slow, &nslow);
            else if (m68k_memory_movea(in)) emit_memory_movea(&e, in, slow, &nslow);
            else emit_memory_move(&e, in, slow, &nslow);
            if (m68k_sets_own_pc(in)) store_imm32(&e, PREVPC, at);
            else store_imm32(&e, PC, next);
            byte(&e, 0x41); byte(&e, 0xFF); byte(&e, 0xC4);   /* inc r12d */
            bump_insns(&e);
            bump_cycles(&e, in->cycles);
            byte(&e, 0xE9); size_t past = e.at; u32(&e, 0);   /* jmp past */
            for (unsigned x = 0; x < nslow; x++) {
                int32_t rel = (int32_t)(e.at - (slow[x] + 4));
                if (slow[x] + 4 <= cap) memcpy(out + slow[x], &rel, 4);
            }
            store_imm32(&e, PC, at);
            call_interpreter(&e, in);
            byte(&e, 0x41); byte(&e, 0xFF); byte(&e, 0xC4);
            byte(&e, 0x84); byte(&e, 0xC0);                   /* test al,al */
            byte(&e, 0x0F); byte(&e, 0x84);
            exits[nexits++] = e.at; u32(&e, 0);
            /*
             * Only the slow path can have reached a device, and only a
             * device can have asked for the block to stop here.
             */
            if (!last) {
                byte(&e, 0x80); field(&e, 7, YIELD); byte(&e, 0x00);
                byte(&e, 0x0F); byte(&e, 0x85);
                exits[nexits++] = e.at; u32(&e, 0);
            }
            {
                int32_t rel = (int32_t)(e.at - (past + 4));
                if (past + 4 <= cap) memcpy(out + past, &rel, 4);
            }
            if (!last) {
                byte(&e, 0x81); field(&e, 7, PC); u32(&e, next);
                byte(&e, 0x0F); byte(&e, 0x85);
                exits[nexits++] = e.at; u32(&e, 0);
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
            byte(&e, 0x41); byte(&e, 0xFF); byte(&e, 0xC4);   /* inc r12d */
            bump_insns(&e);
            bump_cycles(&e, in->cycles);
            translated++;
        } else {
            /*
             * Hand this one to the interpreter. The program counter is set
             * to the instruction first, because that is where the
             * interpreter expects to find itself, and the two checks after
             * the call are the same two the block runner makes: did it
             * raise, and did it go anywhere other than the next
             * instruction.
             */
            store_imm32(&e, PC, at);
            call_interpreter(&e, in);
            byte(&e, 0x41); byte(&e, 0xFF); byte(&e, 0xC4);   /* inc r12d */
            byte(&e, 0x84); byte(&e, 0xC0);                   /* test al,al */
            if (!last) {
                byte(&e, 0x0F); byte(&e, 0x84);               /* jz rel32 */
                exits[nexits++] = e.at; u32(&e, 0);
                byte(&e, 0x81); field(&e, 7, PC); u32(&e, next); /* cmp [pc],next */
                byte(&e, 0x0F); byte(&e, 0x85);               /* jne rel32 */
                exits[nexits++] = e.at; u32(&e, 0);
                /*
                 * An instruction run by the interpreter is the only one in
                 * a block that can have touched a device, and a machine
                 * that owns one asks here for the block to stop.
                 */
                byte(&e, 0x80); field(&e, 7, YIELD); byte(&e, 0x00);
                byte(&e, 0x0F); byte(&e, 0x85);               /* jne rel32 */
                exits[nexits++] = e.at; u32(&e, 0);
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
     * here: jump to whichever block the machine went to last time from
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
    byte(&e, 0x80); field(&e, 7, YIELD); byte(&e, 0x00);
    byte(&e, 0x0F); byte(&e, 0x85);                      /* jne exit */
    exits[nexits++] = e.at; u32(&e, 0);
    /*
     * And that the part is still fetching. An LPSTOP retires like any
     * other instruction and then nothing more is fetched until an
     * interrupt arrives, which is the owner's business -- so a chain has
     * to stop here, where the caller's loop used to.
     */
    byte(&e, 0x80); field(&e, 7, STOPPED); byte(&e, 0x00);
    byte(&e, 0x0F); byte(&e, 0x85);                      /* jne exit */
    exits[nexits++] = e.at; u32(&e, 0);
    byte(&e, 0xE9);                                      /* jmp link  */
    const size_t link = e.at;
    u32(&e, 0);
    /*
     * Where that jump goes until it is resolved: say who is asking, and
     * leave. Once the caller has pointed the jump at a block this is
     * unreachable, which is why the cost of asking is paid once.
     */
    {
        const size_t stub = e.at;
        int32_t to_stub = (int32_t)(stub - (link + 4));
        /* Only inside the buffer. A block that overrun its reservation is
         * thrown away below, but writing this first would already have
         * been a write past the end of it. */
        if (!e.overflow && link + 4 <= cap) memcpy(out + link, &to_stub, 4);
        mov_imm64(&e, RAX, (uint64_t)(uintptr_t)owner);
        byte(&e, 0x48); byte(&e, 0x89); field(&e, RAX,
                                              offsetof(m68k, link_request));
        byte(&e, 0xE9);                                  /* jmp exit */
        exits[nexits++] = e.at; u32(&e, 0);
    }

    size_t end = e.at;
    static const uint8_t epilogue[] = {
        0x44, 0x89, 0xE0,       /* mov eax, r12d */
#ifdef _WIN32
        0x48, 0x83, 0xC4, 0x20, /* add rsp, 32   */
        0x5F,                   /* pop rdi       */
        0x5E,                   /* pop rsi       */
#endif
        0x5D,                   /* pop rbp       */
        0x41, 0x5E,             /* pop r14       */
        0x41, 0x5D,             /* pop r13       */
        0x41, 0x5C,             /* pop r12       */
        0x5B,                   /* pop rbx       */
        0xC3,                   /* ret           */
    };
    bytes(&e, epilogue, sizeof epilogue);

    if (e.overflow) return 0;
    for (unsigned k = 0; k < nexits; k++) {
        int32_t rel = (int32_t)(end - (exits[k] + 4));
        memcpy(out + exits[k], &rel, 4);
    }
    if (native) *native = translated;
    if (points) { points->chain = chain; points->link = link; }
    return e.at;
}

void m68k_patch_link(uint8_t *site, const uint8_t *site_exec,
                     const uint8_t *target)
{
    /* The four bytes at `site` are the displacement of a jmp rel32, which
     * is relative to the instruction after it. */
    int32_t rel = (int32_t)(target - (site_exec + 4));
    memcpy(site, &rel, 4);
}

#endif
