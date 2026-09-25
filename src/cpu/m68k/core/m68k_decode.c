#include "cpu/m68k/core/m68k_decode.h"
#include "cpu/m68k/core/m68k_cycles.h"
#include <string.h>

/*
 * The decoder is written as one pass over the opcode word's top nibble,
 * because that is how the ISA is organised and any other arrangement has to
 * re-derive it. Each line function fills in the instruction and leaves the
 * stream positioned after whatever extension words it consumed; the length
 * falls out of where the stream ended, so there is no separate length table
 * to keep in step with the decoding.
 */

typedef struct {
    const uint8_t *code;
    unsigned avail, at;
    uint32_t pc;
    bool ok;
} fetch;

static uint16_t next16(fetch *f)
{
    if (f->at + 2 > f->avail) { f->ok = false; return 0; }
    uint16_t v = (uint16_t)((uint16_t)f->code[f->at] << 8 | f->code[f->at + 1]);
    f->at += 2;
    return v;
}

static uint32_t next32(fetch *f)
{
    uint32_t high = next16(f);
    uint32_t low = next16(f);
    return high << 16 | low;
}

static void ea_none(m68k_ea *ea)
{
    memset(ea, 0, sizeof *ea);
    ea->mode = M68K_NOEA;
    ea->scale = 1;
}

static void ea_simple(m68k_ea *ea, unsigned mode, unsigned reg)
{
    memset(ea, 0, sizeof *ea);
    ea->mode = (uint8_t)mode;
    ea->reg = (uint8_t)reg;
    ea->scale = 1;
}

/*
 * An indexed extension word. The brief form is the 68000's. The full form
 * is a CPU32 addition that adds a wider base displacement and lets the base
 * or index be suppressed; its memory-indirect variants belong to the 68020
 * and this part does not have them, so they decode as illegal.
 */
static bool decode_index(fetch *f, m68k_ea *ea, uint32_t base_pc, bool pc_relative)
{
    uint16_t ext = next16(f);
    if (!f->ok) return false;
    ea->index = (uint8_t)((ext >> 12) & 15);
    ea->index_long = (ext >> 11) & 1;
    ea->scale = (uint8_t)(1u << ((ext >> 9) & 3));
    ea->index_used = true;
    ea->base_used = true;
    if (!(ext & 0x100)) {
        ea->disp = (int8_t)(ext & 0xFF);
        if (pc_relative) ea->disp += (int32_t)base_pc;
        return true;
    }
    unsigned bdsize = (ext >> 4) & 3;
    if ((ext & 7) != 0 || (ext & 8) != 0 || bdsize == 0)
        return false;                       /* memory indirect, or reserved */
    /*
     * Resolving a full-format address costs more than a brief one, and how
     * much more depends on the base displacement. Read out of the
     * reference core, which is where this model has to agree: nothing, two
     * for a word displacement, six for a long one.
     */
    ea->ext_cycles = bdsize == 3 ? 6 : bdsize == 2 ? 2 : 0;
    ea->base_used = !((ext >> 7) & 1);
    ea->index_used = !((ext >> 6) & 1);
    ea->disp = bdsize == 2 ? (int32_t)(int16_t)next16(f)
             : bdsize == 3 ? (int32_t)next32(f) : 0;
    /* Suppressing the base suppresses the program counter with it, so the
     * displacement stands alone rather than being relative to anything. */
    if (pc_relative && ea->base_used) ea->disp += (int32_t)base_pc;
    return true;
}

/* Decode the effective address named by a 6-bit mode/register field. */
static bool decode_ea(fetch *f, unsigned mode, unsigned reg, unsigned size,
                      m68k_ea *ea)
{
    ea_simple(ea, mode, reg);
    uint32_t extpc = f->pc + f->at;
    switch (mode) {
    case 0: case 1: case 2: case 3: case 4:
        return true;
    case 5:
        ea->disp = (int32_t)(int16_t)next16(f);
        return f->ok;
    case 6:
        return decode_index(f, ea, 0, false) && f->ok;
    case 7:
        switch (reg) {
        case 0:
            ea->mode = M68K_AW;
            ea->disp = (int32_t)(int16_t)next16(f);
            return f->ok;
        case 1:
            ea->mode = M68K_AL;
            ea->disp = (int32_t)next32(f);
            return f->ok;
        case 2:
            ea->mode = M68K_PCD;
            ea->disp = (int32_t)(int16_t)next16(f) + (int32_t)extpc;
            return f->ok;
        case 3:
            ea->mode = M68K_PCX;
            return decode_index(f, ea, extpc, true) && f->ok;
        case 4:
            ea->mode = M68K_IMM;
            if (size == 4) ea->disp = (int32_t)next32(f);
            else if (size == 1) ea->disp = (int32_t)(next16(f) & 0xFF);
            else ea->disp = (int32_t)(int16_t)next16(f);
            return f->ok;
        default:
            return false;
        }
    default:
        return false;
    }
}

/* Operand size from the usual two-bit field. 0 means "not one of these". */
static unsigned size_of(unsigned field)
{
    return field == 0 ? 1 : field == 1 ? 2 : field == 2 ? 4 : 0;
}

/* A destination that is a register, for instructions that name one. */
static void set_reg(m68k_ea *ea, bool address, unsigned reg)
{
    ea_simple(ea, address ? M68K_AN : M68K_DN, reg);
}

static void illegal(m68k_insn *insn)
{
    insn->op = M68K_OP_ILLEGAL;
    insn->flags |= M68K_F_ILLEGAL | M68K_F_ENDS_BLOCK;
    insn->size = 0;
    ea_none(&insn->src);
    ea_none(&insn->dst);
}

/* ------------------------------------------------------------ line 0 */

static void line0(fetch *f, m68k_insn *insn)
{
    uint16_t w = insn->word;
    unsigned mode = (w >> 3) & 7, reg = w & 7;
    unsigned sizefield = (w >> 6) & 3;

    if (w & 0x100) {
        /* MOVEP shares this space with the dynamic bit operations; the
         * address-register mode is what tells them apart. */
        if (mode == 1) {
            insn->op = M68K_OP_MOVEP;
            insn->size = (w & 0x40) ? 4 : 2;
            ea_simple(&insn->src, M68K_DI, reg);
            insn->src.disp = (int32_t)(int16_t)next16(f);
            set_reg(&insn->dst, false, (w >> 9) & 7);
            return;
        }
        static const uint8_t bitop[4] = {
            M68K_OP_BTST, M68K_OP_BCHG, M68K_OP_BCLR, M68K_OP_BSET };
        insn->op = bitop[sizefield];
        set_reg(&insn->src, false, (w >> 9) & 7);
        insn->size = mode == 0 ? 4 : 1;
        if (!decode_ea(f, mode, reg, insn->size, &insn->dst)) { illegal(insn); return; }
        /* Only BTST may read from immediate or program space. */
        if (insn->dst.mode == M68K_AN ||
            (insn->op != M68K_OP_BTST &&
             (insn->dst.mode == M68K_IMM || insn->dst.mode == M68K_PCD ||
              insn->dst.mode == M68K_PCX)))
            illegal(insn);
        return;
    }

    switch ((w >> 9) & 7) {
    case 0: case 1: case 2: case 3: case 5: case 6: {
        static const uint8_t ops[8] = {
            M68K_OP_ORI, M68K_OP_ANDI, M68K_OP_SUBI, M68K_OP_ADDI,
            0, M68K_OP_EORI, M68K_OP_CMPI, 0 };
        unsigned family = (w >> 9) & 7;
        if (sizefield == 3) {
            /* CMP2/CHK2 on CPU32; the 68020's CAS is not on this part. */
            if (family == 0 || family == 1 || family == 2) {
                uint16_t ext = next16(f);
                if (!f->ok) return;
                insn->op = (ext & 0x800) ? M68K_OP_CHK2 : M68K_OP_CMP2;
                insn->size = family == 0 ? 1 : family == 1 ? 2 : 4;
                insn->flags |= M68K_F_MAY_TRAP;
                set_reg(&insn->dst, (ext & 0x8000) != 0, (ext >> 12) & 7);
                if (!decode_ea(f, mode, reg, insn->size, &insn->src) ||
                    insn->src.mode == M68K_DN || insn->src.mode == M68K_AN ||
                    insn->src.mode == M68K_PI || insn->src.mode == M68K_PD ||
                    insn->src.mode == M68K_IMM)
                    illegal(insn);
                return;
            }
            illegal(insn);
            return;
        }
        insn->op = ops[family];
        insn->size = size_of(sizefield);
        /* The immediate forms that name CCR or SR instead of an EA. */
        if ((w & 0xFF) == 0x3C && family != 2 && family != 3 && family != 6) {
            insn->op = family == 0 ? M68K_OP_ORICCR
                     : family == 1 ? M68K_OP_ANDICCR : M68K_OP_EORICCR;
            insn->size = 1;
            ea_simple(&insn->src, M68K_IMM, 0);
            insn->src.disp = (int32_t)(next16(f) & 0xFF);
            ea_none(&insn->dst);
            return;
        }
        if ((w & 0xFF) == 0x7C && family != 2 && family != 3 && family != 6) {
            insn->op = family == 0 ? M68K_OP_ORISR
                     : family == 1 ? M68K_OP_ANDISR : M68K_OP_EORISR;
            insn->size = 2;
            insn->flags |= M68K_F_PRIVILEGED | M68K_F_ENDS_BLOCK;
            ea_simple(&insn->src, M68K_IMM, 0);
            insn->src.disp = (int32_t)(int16_t)next16(f);
            ea_none(&insn->dst);
            return;
        }
        ea_simple(&insn->src, M68K_IMM, 0);
        if (insn->size == 4) insn->src.disp = (int32_t)next32(f);
        else if (insn->size == 1) insn->src.disp = (int32_t)(next16(f) & 0xFF);
        else insn->src.disp = (int32_t)(int16_t)next16(f);
        if (!f->ok) return;
        if (!decode_ea(f, mode, reg, insn->size, &insn->dst)) { illegal(insn); return; }
        /* CMPI is the one immediate form that may read program space,
         * and even it cannot name an immediate as its destination. */
        if (insn->dst.mode == M68K_AN || insn->dst.mode == M68K_IMM ||
            ((insn->dst.mode == M68K_PCD || insn->dst.mode == M68K_PCX) &&
             insn->op != M68K_OP_CMPI))
            illegal(insn);
        return;
    }
    case 4: {
        /* Static bit operations, whose bit number is an extension word. */
        static const uint8_t bitop[4] = {
            M68K_OP_BTST, M68K_OP_BCHG, M68K_OP_BCLR, M68K_OP_BSET };
        insn->op = bitop[sizefield];
        ea_simple(&insn->src, M68K_IMM, 0);
        insn->src.disp = (int32_t)(next16(f) & 0xFF);
        insn->size = mode == 0 ? 4 : 1;
        if (!f->ok) return;
        if (!decode_ea(f, mode, reg, insn->size, &insn->dst)) { illegal(insn); return; }
        if (insn->dst.mode == M68K_AN || insn->dst.mode == M68K_IMM ||
            (insn->op != M68K_OP_BTST &&
             (insn->dst.mode == M68K_PCD || insn->dst.mode == M68K_PCX)))
            illegal(insn);
        return;
    }
    case 7:
        /* MOVES is privileged and reaches the alternate function codes. */
        if (sizefield == 3) { illegal(insn); return; }
        insn->op = M68K_OP_MOVES;
        insn->size = size_of(sizefield);
        insn->flags |= M68K_F_PRIVILEGED;
        {
            uint16_t ext = next16(f);
            if (!f->ok) return;
            bool to_memory = (ext & 0x800) != 0;
            m68k_ea ea;
            if (!decode_ea(f, mode, reg, insn->size, &ea) ||
                ea.mode == M68K_DN || ea.mode == M68K_AN ||
                ea.mode == M68K_IMM || ea.mode == M68K_PCD ||
                ea.mode == M68K_PCX) { illegal(insn); return; }
            if (to_memory) {
                set_reg(&insn->src, (ext & 0x8000) != 0, (ext >> 12) & 7);
                insn->dst = ea;
            } else {
                insn->src = ea;
                set_reg(&insn->dst, (ext & 0x8000) != 0, (ext >> 12) & 7);
            }
        }
        return;
    default:
        illegal(insn);
        return;
    }
}

/* ------------------------------------------------ lines 1, 2 and 3 */

static void line_move(fetch *f, m68k_insn *insn)
{
    uint16_t w = insn->word;
    unsigned top = w >> 12;
    insn->size = top == 1 ? 1 : top == 2 ? 4 : 2;
    if (!decode_ea(f, (w >> 3) & 7, w & 7, insn->size, &insn->src)) {
        illegal(insn); return;
    }
    if (insn->size == 1 && insn->src.mode == M68K_AN) { illegal(insn); return; }
    unsigned dmode = (w >> 6) & 7, dreg = (w >> 9) & 7;
    if (dmode == 1) {
        /* A move into an address register is a different instruction: it
         * sign-extends and leaves the flags alone. */
        if (insn->size == 1) { illegal(insn); return; }
        insn->op = M68K_OP_MOVEA;
        ea_simple(&insn->dst, M68K_AN, dreg);
        return;
    }
    insn->op = M68K_OP_MOVE;
    if (!decode_ea(f, dmode, dreg, insn->size, &insn->dst) ||
        insn->dst.mode == M68K_IMM || insn->dst.mode == M68K_PCD ||
        insn->dst.mode == M68K_PCX)
        illegal(insn);
}

/* ------------------------------------------------------------ line 4 */

static void line4(fetch *f, m68k_insn *insn)
{
    uint16_t w = insn->word;
    unsigned mode = (w >> 3) & 7, reg = w & 7;
    unsigned sizefield = (w >> 6) & 3;

    switch (w & 0xFFF8) {
    case 0x4808:
        insn->op = M68K_OP_LINK;
        insn->size = 4;
        ea_simple(&insn->dst, M68K_AN, reg);
        insn->extra = next32(f);
        return;
    case 0x4840:
        insn->op = M68K_OP_SWAP;
        insn->size = 4;
        set_reg(&insn->dst, false, reg);
        return;
    case 0x4848:
        insn->op = M68K_OP_BKPT;
        insn->flags |= M68K_F_ENDS_BLOCK | M68K_F_MAY_TRAP;
        insn->extra = reg;
        return;
    case 0x4880:
        insn->op = M68K_OP_EXT; insn->size = 2;
        set_reg(&insn->dst, false, reg);
        return;
    case 0x48C0:
        insn->op = M68K_OP_EXT; insn->size = 4;
        set_reg(&insn->dst, false, reg);
        return;
    case 0x49C0:
        insn->op = M68K_OP_EXTB; insn->size = 4;
        set_reg(&insn->dst, false, reg);
        return;
    case 0x4E50:
        insn->op = M68K_OP_LINK; insn->size = 2;
        ea_simple(&insn->dst, M68K_AN, reg);
        insn->extra = (uint32_t)(int32_t)(int16_t)next16(f);
        return;
    case 0x4E58:
        insn->op = M68K_OP_UNLK; insn->size = 4;
        ea_simple(&insn->dst, M68K_AN, reg);
        return;
    case 0x4E60: case 0x4E68:
        insn->op = M68K_OP_MOVEUSP; insn->size = 4;
        insn->flags |= M68K_F_PRIVILEGED;
        ea_simple(&insn->dst, M68K_AN, reg);
        return;
    default:
        break;
    }
    if ((w & 0xFFF0) == 0x4E40) {
        insn->op = M68K_OP_TRAP;
        insn->flags |= M68K_F_ENDS_BLOCK | M68K_F_MAY_TRAP;
        insn->extra = w & 15;
        return;
    }
    switch (w) {
    case 0x4AFA:
        insn->op = M68K_OP_BGND;
        insn->flags |= M68K_F_ENDS_BLOCK;
        return;
    case 0x4AFC:
        insn->op = M68K_OP_ILLEGAL;
        insn->flags |= M68K_F_ENDS_BLOCK | M68K_F_MAY_TRAP;
        return;
    case 0x4E70: insn->op = M68K_OP_RESET;
        insn->flags |= M68K_F_PRIVILEGED | M68K_F_ENDS_BLOCK; return;
    case 0x4E71: insn->op = M68K_OP_NOP; return;
    case 0x4E72:
        insn->op = M68K_OP_STOP;
        insn->flags |= M68K_F_PRIVILEGED | M68K_F_ENDS_BLOCK;
        insn->extra = next16(f);
        return;
    case 0x4E73: insn->op = M68K_OP_RTE;
        insn->flags |= M68K_F_PRIVILEGED | M68K_F_ENDS_BLOCK; return;
    case 0x4E74:
        insn->op = M68K_OP_RTD;
        insn->flags |= M68K_F_ENDS_BLOCK;
        insn->extra = (uint32_t)(int32_t)(int16_t)next16(f);
        return;
    case 0x4E75: insn->op = M68K_OP_RTS; insn->flags |= M68K_F_ENDS_BLOCK; return;
    case 0x4E76: insn->op = M68K_OP_TRAPV;
        insn->flags |= M68K_F_MAY_TRAP | M68K_F_ENDS_BLOCK; return;
    case 0x4E77: insn->op = M68K_OP_RTR; insn->flags |= M68K_F_ENDS_BLOCK; return;
    case 0x4E7A: case 0x4E7B:
        insn->op = M68K_OP_MOVEC;
        insn->size = 4;
        insn->flags |= M68K_F_PRIVILEGED | M68K_F_ENDS_BLOCK;
        insn->extra = next16(f);
        return;
    default:
        break;
    }

    switch ((w >> 8) & 15) {
    case 0x0:
        if (sizefield == 3) {
            insn->op = M68K_OP_MOVEFSR; insn->size = 2;
            insn->flags |= M68K_F_PRIVILEGED;
            if (!decode_ea(f, mode, reg, 2, &insn->dst) ||
                insn->dst.mode == M68K_AN || insn->dst.mode == M68K_IMM ||
                insn->dst.mode == M68K_PCD || insn->dst.mode == M68K_PCX)
                illegal(insn);
            return;
        }
        insn->op = M68K_OP_NEGX;
        break;
    case 0x2:
        if (sizefield == 3) {
            insn->op = M68K_OP_MOVEFCCR; insn->size = 2;
            if (!decode_ea(f, mode, reg, 2, &insn->dst) ||
                insn->dst.mode == M68K_AN || insn->dst.mode == M68K_IMM ||
                insn->dst.mode == M68K_PCD || insn->dst.mode == M68K_PCX)
                illegal(insn);
            return;
        }
        insn->op = M68K_OP_CLR;
        break;
    case 0x4:
        if (sizefield == 3) {
            insn->op = M68K_OP_MOVETCCR; insn->size = 2;
            if (!decode_ea(f, mode, reg, 2, &insn->src) ||
                insn->src.mode == M68K_AN) illegal(insn);
            return;
        }
        insn->op = M68K_OP_NEG;
        break;
    case 0x6:
        if (sizefield == 3) {
            insn->op = M68K_OP_MOVETSR; insn->size = 2;
            insn->flags |= M68K_F_PRIVILEGED | M68K_F_ENDS_BLOCK;
            if (!decode_ea(f, mode, reg, 2, &insn->src) ||
                insn->src.mode == M68K_AN) illegal(insn);
            return;
        }
        insn->op = M68K_OP_NOT;
        break;
    case 0x8:
        if (sizefield == 0) {
            insn->op = M68K_OP_NBCD; insn->size = 1;
            if (!decode_ea(f, mode, reg, 1, &insn->dst) ||
                insn->dst.mode == M68K_AN || insn->dst.mode == M68K_IMM ||
                insn->dst.mode == M68K_PCD || insn->dst.mode == M68K_PCX)
                illegal(insn);
            return;
        }
        if (sizefield == 1) {
            insn->op = M68K_OP_PEA; insn->size = 4;
            if (!decode_ea(f, mode, reg, 4, &insn->src) ||
                insn->src.mode == M68K_DN || insn->src.mode == M68K_AN ||
                insn->src.mode == M68K_PI || insn->src.mode == M68K_PD ||
                insn->src.mode == M68K_IMM)
                illegal(insn);
            return;
        }
        /* MOVEM, registers to memory. */
        insn->op = M68K_OP_MOVEM;
        insn->size = sizefield == 2 ? 2 : 4;
        insn->extra = next16(f);
        if (!f->ok) return;
        if (!decode_ea(f, mode, reg, insn->size, &insn->dst) ||
            insn->dst.mode == M68K_DN || insn->dst.mode == M68K_AN ||
            insn->dst.mode == M68K_PI || insn->dst.mode == M68K_IMM ||
            insn->dst.mode == M68K_PCD || insn->dst.mode == M68K_PCX)
            illegal(insn);
        return;
    case 0xA:
        if (sizefield == 3) {
            insn->op = M68K_OP_TAS; insn->size = 1;
            if (!decode_ea(f, mode, reg, 1, &insn->dst) ||
                insn->dst.mode == M68K_AN || insn->dst.mode == M68K_IMM ||
                insn->dst.mode == M68K_PCD || insn->dst.mode == M68K_PCX)
                illegal(insn);
            return;
        }
        insn->op = M68K_OP_TST;
        insn->size = size_of(sizefield);
        if (!decode_ea(f, mode, reg, insn->size, &insn->src) ||
            (insn->src.mode == M68K_AN && insn->size == 1))
            illegal(insn);
        return;
    case 0xC:
        if (sizefield == 0 || sizefield == 1) {
            /* The 32-bit multiply and divide CPU32 inherited. */
            insn->op = sizefield == 0 ? M68K_OP_MULL : M68K_OP_DIVL;
            insn->size = 4;
            if (sizefield == 1) insn->flags |= M68K_F_MAY_TRAP;
            insn->extra = next16(f);
            if (!f->ok) return;
            if (!decode_ea(f, mode, reg, 4, &insn->src) ||
                insn->src.mode == M68K_AN) illegal(insn);
            return;
        }
        /* MOVEM, memory to registers. */
        insn->op = M68K_OP_MOVEM;
        insn->size = sizefield == 2 ? 2 : 4;
        insn->extra = next16(f);
        if (!f->ok) return;
        if (!decode_ea(f, mode, reg, insn->size, &insn->src) ||
            insn->src.mode == M68K_DN || insn->src.mode == M68K_AN ||
            insn->src.mode == M68K_PD || insn->src.mode == M68K_IMM)
            illegal(insn);
        return;
    case 0xE:
        if (sizefield == 2) {
            insn->op = M68K_OP_JSR;
            insn->flags |= M68K_F_ENDS_BLOCK;
        } else if (sizefield == 3) {
            insn->op = M68K_OP_JMP;
            insn->flags |= M68K_F_ENDS_BLOCK;
        } else { illegal(insn); return; }
        insn->size = 4;
        if (!decode_ea(f, mode, reg, 4, &insn->src) ||
            insn->src.mode == M68K_DN || insn->src.mode == M68K_AN ||
            insn->src.mode == M68K_PI || insn->src.mode == M68K_PD ||
            insn->src.mode == M68K_IMM)
            illegal(insn);
        return;
    default:
        break;
    }

    if (insn->op == M68K_OP_NEGX || insn->op == M68K_OP_CLR ||
        insn->op == M68K_OP_NEG || insn->op == M68K_OP_NOT) {
        insn->size = size_of(sizefield);
        if (!decode_ea(f, mode, reg, insn->size, &insn->dst) ||
            insn->dst.mode == M68K_AN || insn->dst.mode == M68K_IMM ||
            insn->dst.mode == M68K_PCD || insn->dst.mode == M68K_PCX)
            illegal(insn);
        return;
    }

    /*
     * What is left in line 4 is register-and-EA shaped: CHK and LEA. Their
     * operand field is three bits wide, not the two of an ordinary size
     * field, and reading only two of them turns every LEA into a CHK.
     */
    switch ((w >> 6) & 7) {
    case 4:                                  /* CHK.L, a 68020 addition */
    case 6:                                  /* CHK.W                    */
        insn->op = M68K_OP_CHK;
        insn->size = ((w >> 6) & 7) == 6 ? 2 : 4;
        insn->flags |= M68K_F_MAY_TRAP;
        set_reg(&insn->dst, false, (w >> 9) & 7);
        if (!decode_ea(f, mode, reg, insn->size, &insn->src) ||
            insn->src.mode == M68K_AN)
            illegal(insn);
        return;
    case 7:
        insn->op = M68K_OP_LEA;
        insn->size = 4;
        ea_simple(&insn->dst, M68K_AN, (w >> 9) & 7);
        if (!decode_ea(f, mode, reg, 4, &insn->src) ||
            insn->src.mode == M68K_DN || insn->src.mode == M68K_AN ||
            insn->src.mode == M68K_PI || insn->src.mode == M68K_PD ||
            insn->src.mode == M68K_IMM)
            illegal(insn);
        return;
    default:
        illegal(insn);
        return;
    }
}

/* ------------------------------------------------------------ line 5 */

static void line5(fetch *f, m68k_insn *insn)
{
    uint16_t w = insn->word;
    unsigned mode = (w >> 3) & 7, reg = w & 7;
    unsigned sizefield = (w >> 6) & 3;
    insn->cond = (uint8_t)((w >> 8) & 15);

    if (sizefield == 3) {
        if (mode == 1) {
            insn->op = M68K_OP_DBCC;
            insn->size = 2;
            insn->flags |= M68K_F_ENDS_BLOCK;
            set_reg(&insn->dst, false, reg);
            uint32_t base = f->pc + f->at;
            insn->extra = (uint32_t)((int32_t)base + (int32_t)(int16_t)next16(f));
            return;
        }
        if (mode == 7 && reg >= 2 && reg <= 4) {
            /* TRAPcc, a CPU32 addition, optionally with operand words. */
            insn->op = M68K_OP_TRAPCC;
            insn->flags |= M68K_F_MAY_TRAP | M68K_F_ENDS_BLOCK;
            if (reg == 2) { insn->size = 2; insn->extra = next16(f); }
            else if (reg == 3) { insn->size = 4; insn->extra = next32(f); }
            return;
        }
        insn->op = M68K_OP_SCC;
        insn->size = 1;
        if (!decode_ea(f, mode, reg, 1, &insn->dst) ||
            insn->dst.mode == M68K_AN || insn->dst.mode == M68K_IMM ||
            insn->dst.mode == M68K_PCD || insn->dst.mode == M68K_PCX)
            illegal(insn);
        return;
    }

    insn->op = (w & 0x100) ? M68K_OP_SUBQ : M68K_OP_ADDQ;
    insn->cond = 0;
    insn->size = size_of(sizefield);
    ea_simple(&insn->src, M68K_IMM, 0);
    insn->src.disp = ((w >> 9) & 7) ? (int32_t)((w >> 9) & 7) : 8;
    if (!decode_ea(f, mode, reg, insn->size, &insn->dst) ||
        insn->dst.mode == M68K_IMM || insn->dst.mode == M68K_PCD ||
        insn->dst.mode == M68K_PCX ||
        (insn->dst.mode == M68K_AN && insn->size == 1))
        illegal(insn);
}

/* ------------------------------------------------------------ line 6 */

static void line6(fetch *f, m68k_insn *insn)
{
    uint16_t w = insn->word;
    unsigned cond = (w >> 8) & 15;
    int8_t byte = (int8_t)(w & 0xFF);
    uint32_t base = f->pc + 2;
    int32_t disp;

    if (byte == 0) disp = (int32_t)(int16_t)next16(f);
    else if ((uint8_t)byte == 0xFF) disp = (int32_t)next32(f);
    else disp = byte;
    if (!f->ok) return;

    insn->op = cond == 0 ? M68K_OP_BRA : cond == 1 ? M68K_OP_BSR : M68K_OP_BCC;
    insn->cond = (uint8_t)cond;
    insn->size = byte == 0 ? 2 : (uint8_t)byte == 0xFF ? 4 : 1;
    insn->flags |= M68K_F_ENDS_BLOCK;
    insn->extra = (uint32_t)((int32_t)base + disp);
}

/* ------------------------------------------------------------ line 7 */

static void line7(fetch *f, m68k_insn *insn)
{
    (void)f;
    if (insn->word & 0x100) { illegal(insn); return; }
    insn->op = M68K_OP_MOVEQ;
    insn->size = 4;
    ea_simple(&insn->src, M68K_IMM, 0);
    insn->src.disp = (int8_t)(insn->word & 0xFF);
    set_reg(&insn->dst, false, (insn->word >> 9) & 7);
}

/* --------------------------------------------- lines 8, 9, 11, 12, 13 */

/*
 * The arithmetic and logic lines share one encoding shape: a register, a
 * direction, a size and an effective address, with a few special cases
 * carved out of the register-to-register corner of the space.
 */
static void line_alu(fetch *f, m68k_insn *insn)
{
    uint16_t w = insn->word;
    unsigned top = w >> 12;
    unsigned dreg = (w >> 9) & 7, opmode = (w >> 6) & 7;
    unsigned mode = (w >> 3) & 7, reg = w & 7;

    /* An opmode of 3 or 7 makes the register operand an address register,
     * except in lines 8 and 12, which spend that corner on divide and
     * multiply instead. */
    if (opmode == 3 || opmode == 7) {
        if (top == 8 || top == 12) {
            insn->op = top == 8 ? (opmode == 3 ? M68K_OP_DIVU : M68K_OP_DIVS)
                                : (opmode == 3 ? M68K_OP_MULU : M68K_OP_MULS);
            insn->size = 2;
            if (top == 8) insn->flags |= M68K_F_MAY_TRAP;
            set_reg(&insn->dst, false, dreg);
            if (!decode_ea(f, mode, reg, 2, &insn->src) ||
                insn->src.mode == M68K_AN) illegal(insn);
            return;
        }
        unsigned size = opmode == 3 ? 2 : 4;
        insn->op = top == 9 ? M68K_OP_SUBA : top == 11 ? M68K_OP_CMPA
                 : M68K_OP_ADDA;
        insn->size = (uint8_t)size;
        ea_simple(&insn->dst, M68K_AN, dreg);
        if (!decode_ea(f, mode, reg, size, &insn->src)) illegal(insn);
        return;
    }

    unsigned size = size_of(opmode & 3);
    bool to_ea = (opmode & 4) != 0;

    /*
     * Line 11 spends its whole to-memory direction on EOR, with the
     * address-register mode carved out for CMPM. It is not the
     * register-to-register special case the other lines have there, and
     * treating it as one turns every EOR into a CMP.
     */
    if (top == 11 && to_ea) {
        if (mode == 1) {
            insn->op = M68K_OP_CMPM;
            insn->size = (uint8_t)size;
            ea_simple(&insn->src, M68K_PI, reg);
            ea_simple(&insn->dst, M68K_PI, dreg);
            return;
        }
        insn->op = M68K_OP_EOR;
        insn->size = (uint8_t)size;
        set_reg(&insn->src, false, dreg);
        if (!decode_ea(f, mode, reg, size, &insn->dst) ||
            insn->dst.mode == M68K_AN || insn->dst.mode == M68K_IMM ||
            insn->dst.mode == M68K_PCD || insn->dst.mode == M68K_PCX)
            illegal(insn);
        return;
    }

    /* The register-to-register special cases: the binary-coded decimal and
     * extended-precision forms, and the register exchange. */
    if (to_ea && (mode == 0 || mode == 1)) {
        if ((top == 8 || top == 12) && size == 1) {
            insn->op = top == 8 ? M68K_OP_SBCD : M68K_OP_ABCD;
            insn->size = 1;
        } else if (top == 9 || top == 13) {
            insn->op = top == 9 ? M68K_OP_SUBX : M68K_OP_ADDX;
            insn->size = (uint8_t)size;
        } else if (top == 12) {
            unsigned kind = (w >> 3) & 0x1F;
            if (kind != 0x08 && kind != 0x09 && kind != 0x11) { illegal(insn); return; }
            insn->op = M68K_OP_EXG;
            insn->size = 4;
            /* Three forms: two data registers, two address registers, or a
             * data register with an address register in that order. */
            set_reg(&insn->src, kind == 0x09, dreg);
            set_reg(&insn->dst, kind != 0x08, reg);
            return;
        } else {
            /* OR and AND cannot write a register through this direction. */
            illegal(insn);
            return;
        }
        ea_simple(&insn->src, mode == 1 ? M68K_PD : M68K_DN, reg);
        ea_simple(&insn->dst, mode == 1 ? M68K_PD : M68K_DN, dreg);
        return;
    }

    static const uint8_t family[16] = {
        0,0,0,0,0,0,0,0, M68K_OP_OR,M68K_OP_SUB,0,M68K_OP_CMP,
        M68K_OP_AND,M68K_OP_ADD,0,0 };
    insn->op = family[top];
    insn->size = (uint8_t)size;
    if (!size) { illegal(insn); return; }
    if (to_ea) {
        set_reg(&insn->src, false, dreg);
        if (!decode_ea(f, mode, reg, size, &insn->dst) ||
            insn->dst.mode == M68K_DN || insn->dst.mode == M68K_AN ||
            insn->dst.mode == M68K_IMM || insn->dst.mode == M68K_PCD ||
            insn->dst.mode == M68K_PCX)
            illegal(insn);
        return;
    }
    set_reg(&insn->dst, false, dreg);
    if (!decode_ea(f, mode, reg, size, &insn->src)) { illegal(insn); return; }
    /* An address register is a whole-word quantity: the logical operations
     * never accept one, and the arithmetic ones never accept a byte of it. */
    if (insn->src.mode == M68K_AN &&
        (size == 1 || insn->op == M68K_OP_OR || insn->op == M68K_OP_AND))
        illegal(insn);
}

/* ----------------------------------------------------------- line 14 */

static void line14(fetch *f, m68k_insn *insn)
{
    uint16_t w = insn->word;
    unsigned sizefield = (w >> 6) & 3;
    static const uint8_t shifts[2][4] = {
        { M68K_OP_ASR, M68K_OP_LSR, M68K_OP_ROXR, M68K_OP_ROR },
        { M68K_OP_ASL, M68K_OP_LSL, M68K_OP_ROXL, M68K_OP_ROL },
    };
    bool left = (w & 0x100) != 0;

    if (sizefield == 3) {
        /* A memory shift, always by one and always a word. The 68020 puts
         * its bit-field instructions here; CPU32 has none of them. */
        unsigned kind = (w >> 9) & 7;
        if (kind > 3) { illegal(insn); return; }
        insn->op = shifts[left][kind];
        insn->size = 2;
        ea_simple(&insn->src, M68K_IMM, 0);
        insn->src.disp = 1;
        if (!decode_ea(f, (w >> 3) & 7, w & 7, 2, &insn->dst) ||
            insn->dst.mode == M68K_DN || insn->dst.mode == M68K_AN ||
            insn->dst.mode == M68K_IMM || insn->dst.mode == M68K_PCD ||
            insn->dst.mode == M68K_PCX)
            illegal(insn);
        return;
    }

    insn->op = shifts[left][(w >> 3) & 3];
    insn->size = size_of(sizefield);
    if (w & 0x20) set_reg(&insn->src, false, (w >> 9) & 7);
    else {
        ea_simple(&insn->src, M68K_IMM, 0);
        insn->src.disp = ((w >> 9) & 7) ? (int32_t)((w >> 9) & 7) : 8;
    }
    set_reg(&insn->dst, false, w & 7);
}

/* ------------------------------------------------------------ entry */

/*
 * What this instruction costs, in the reference core's cycles.
 *
 * The table is keyed on what an encoding decides; the two things an
 * encoding decides that the key does not capture are added here, because
 * both are known now rather than when the instruction runs:
 *
 *   - MOVEM charges four for every register past the first, and the table
 *     holds the one-register cost;
 *   - a full-format indexed address charges a penalty for resolving it,
 *     which decode_index has already worked out.
 *
 * Zero for an encoding the table has no entry for, which should not
 * happen: a missing entry shows up as a cost of nothing and can be
 * investigated using the core's instruction diagnostics.
 */
static uint16_t decode_cycles(const m68k_insn *insn)
{
    /*
     * An encoding that is not an instruction costs what raising costs,
     * whatever its operand fields happened to parse as. Twenty, measured
     * on the reference like everything else here.
     */
    if (insn->flags & M68K_F_ILLEGAL) return 20;
    uint32_t key = m68k_cycle_key(insn);
    unsigned lo = 0, hi = M68K_CYCLE_TABLE_COUNT;
    uint16_t cycles = 0;
    while (lo < hi) {
        unsigned mid = (lo + hi) / 2;
        if (m68k_cycle_table[mid].key == key) { cycles = m68k_cycle_table[mid].cycles; break; }
        if (m68k_cycle_table[mid].key < key) lo = mid + 1;
        else hi = mid;
    }
    if (insn->op == M68K_OP_MOVEM) {
        unsigned registers = 0;
        for (unsigned bit = 0; bit < 16; bit++)
            if (insn->extra & (1u << bit)) registers++;
        cycles = (uint16_t)(cycles + 4 * (registers ? registers - 1 : 0));
    }
    cycles = (uint16_t)(cycles + insn->src.ext_cycles + insn->dst.ext_cycles);
    return cycles;
}

unsigned m68k_decode(const uint8_t *code, unsigned avail, uint32_t pc,
                     m68k_insn *out)
{
    fetch f = { code, avail, 0, pc, true };
    memset(out, 0, sizeof *out);
    ea_none(&out->src);
    ea_none(&out->dst);
    out->word = next16(&f);
    if (!f.ok) return 0;

    switch (out->word >> 12) {
    case 0x0: line0(&f, out); break;
    case 0x1: case 0x2: case 0x3: line_move(&f, out); break;
    case 0x4: line4(&f, out); break;
    case 0x5: line5(&f, out); break;
    case 0x6: line6(&f, out); break;
    case 0x7: line7(&f, out); break;
    case 0x8: case 0x9: case 0xB: case 0xC: case 0xD: line_alu(&f, out); break;
    case 0xA:
        out->op = M68K_OP_LINE_A;
        out->flags |= M68K_F_ENDS_BLOCK | M68K_F_MAY_TRAP;
        break;
    case 0xE: line14(&f, out); break;
    default:
        /* Line F. On this part the only encodings that are instructions
         * rather than a trap are LPSTOP and the table lookups. */
        if (out->word == 0xF800) {
            out->op = M68K_OP_LPSTOP;
            out->flags |= M68K_F_PRIVILEGED | M68K_F_ENDS_BLOCK;
            out->extra = next16(&f);
            if (out->extra != 0x01C0) illegal(out);
            else out->extra = next16(&f);
        } else if ((out->word & 0xFF00) == 0xF800) {
            out->op = M68K_OP_TBL;
            out->size = 4;
            out->extra = next16(&f);
        } else {
            out->op = M68K_OP_LINE_F;
            out->flags |= M68K_F_ENDS_BLOCK | M68K_F_MAY_TRAP;
        }
        break;
    }
    if (!f.ok) return 0;
    if (f.at > 255) return 0;
    out->length = (uint8_t)f.at;
    out->next_pc = pc + f.at;
    out->cycles = decode_cycles(out);
    /* An illegal encoding is two bytes whatever its operands looked like:
     * nothing past the opcode word was fetched by the hardware either. */
    if (out->flags & M68K_F_ILLEGAL) { out->length = 2; out->next_pc = pc + 2; }
    return out->length;
}

static const char *const names[M68K_OP_COUNT] = {
    "ILLEGAL", "LINE_A", "LINE_F", "ABCD", "ADD", "ADDA", "ADDI", "ADDQ",
    "ADDX", "AND", "ANDI", "ANDICCR", "ANDISR", "ASL", "ASR", "BCC", "BCHG",
    "BCLR", "BGND", "BKPT", "BRA", "BSET", "BSR", "BTST", "CHK", "CHK2",
    "CLR", "CMP", "CMP2", "CMPA", "CMPI", "CMPM", "DBCC", "DIVL", "DIVS",
    "DIVU", "EOR", "EORI", "EORICCR", "EORISR", "EXG", "EXT", "EXTB", "JMP",
    "JSR", "LEA", "LINK", "LPSTOP", "LSL", "LSR", "MOVE", "MOVEA", "MOVEC",
    "MOVEFCCR", "MOVEFSR", "MOVEM", "MOVEP", "MOVEQ", "MOVES", "MOVETCCR",
    "MOVETSR", "MOVEUSP", "MULL", "MULS", "MULU", "NBCD", "NEG", "NEGX",
    "NOP", "NOT", "OR", "ORI", "ORICCR", "ORISR", "PEA", "RESET", "ROL",
    "ROR", "ROXL", "ROXR", "RTD", "RTE", "RTR", "RTS", "SBCD", "SCC",
    "STOP", "SUB", "SUBA", "SUBI", "SUBQ", "SUBX", "SWAP", "TAS", "TBL",
    "TRAP", "TRAPCC", "TRAPV", "TST", "UNLK",
};

static const char *const mode_names[] = {
    "Dn", "An", "(An)", "(An)+", "-(An)", "(d16,An)", "(d,An,Xn)",
    "(xxx).w", "(xxx).l", "(d16,PC)", "(d,PC,Xn)", "#imm", "-",
};

const char *m68k_mode_name(unsigned mode)
{
    return mode < sizeof mode_names / sizeof *mode_names ? mode_names[mode]
                                                         : "?";
}

const char *m68k_op_name(unsigned op)
{
    return op < M68K_OP_COUNT && names[op] ? names[op] : "?";
}
