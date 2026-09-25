/*
 * m68k_decode.h — CPU32 instruction decoder.
 *
 * The first piece of a 68k core of our own. It answers three questions a
 * translator cannot proceed without: what instruction is this, how long is
 * it, and where are its operands. Nothing here executes anything.
 *
 * The decoded form is shaped for a block translator, not for a switch in an
 * interpreter loop:
 *
 *   - `length` is in bytes, so a caller can walk a block without executing
 *     it, bound the block, and guard the exact byte span it compiled from.
 *     A variable-length ISA makes this the decoder's central obligation:
 *     get a length wrong and every following instruction is garbage.
 *   - Effective addresses are fully unpacked, extension words included, so
 *     an emitter never re-parses them.
 *   - `flags` says what a block must do about the instruction without
 *     re-deriving its semantics: whether it ends the block, whether it can
 *     trap, whether it needs supervisor state.
 *
 * Addressing-mode numbering follows the encoding fields in 68k instruction
 * words, so the decoder can preserve those fields without conversion.
 *
 * This decodes the MC68349's CPU32 instruction set: the 68000 and 68010
 * sets, the 68020 additions CPU32 kept, and the three CPU32-only
 * instructions. Encodings that belong to a 68020 but not to a CPU32 decode
 * as illegal, because that is what this hardware does with them. See
 * docs/M68K_CORE.md for that list.
 */
#ifndef MRC_M68K_DECODE_H
#define MRC_M68K_DECODE_H

#include <stdbool.h>
#include <stdint.h>

/* Addressing modes, numbered in the order encoded by 68k instructions. */
typedef enum {
    M68K_DN = 0,    /* Dn                                    */
    M68K_AN,        /* An                                    */
    M68K_AI,        /* (An)                                  */
    M68K_PI,        /* (An)+                                 */
    M68K_PD,        /* -(An)                                 */
    M68K_DI,        /* (d16,An)                              */
    M68K_IX,        /* (d,An,Xn) brief or full extension     */
    M68K_AW,        /* (xxx).w                               */
    M68K_AL,        /* (xxx).l                               */
    M68K_PCD,       /* (d16,PC)                              */
    M68K_PCX,       /* (d,PC,Xn)                             */
    M68K_IMM,       /* #imm                                  */
    M68K_NOEA,      /* implied: no effective address         */
} m68k_mode;

/*
 * Instruction identity used by this core's decoder and diagnostics. Values
 * are internal and carry no meaning outside this header.
 */
typedef enum {
    M68K_OP_ILLEGAL = 0, M68K_OP_LINE_A, M68K_OP_LINE_F,
    M68K_OP_ABCD, M68K_OP_ADD, M68K_OP_ADDA, M68K_OP_ADDI, M68K_OP_ADDQ,
    M68K_OP_ADDX, M68K_OP_AND, M68K_OP_ANDI, M68K_OP_ANDICCR, M68K_OP_ANDISR,
    M68K_OP_ASL, M68K_OP_ASR, M68K_OP_BCC, M68K_OP_BCHG, M68K_OP_BCLR,
    M68K_OP_BGND, M68K_OP_BKPT, M68K_OP_BRA, M68K_OP_BSET, M68K_OP_BSR,
    M68K_OP_BTST, M68K_OP_CHK, M68K_OP_CHK2, M68K_OP_CLR, M68K_OP_CMP,
    M68K_OP_CMP2, M68K_OP_CMPA, M68K_OP_CMPI, M68K_OP_CMPM, M68K_OP_DBCC,
    M68K_OP_DIVL, M68K_OP_DIVS, M68K_OP_DIVU, M68K_OP_EOR, M68K_OP_EORI,
    M68K_OP_EORICCR, M68K_OP_EORISR, M68K_OP_EXG, M68K_OP_EXT, M68K_OP_EXTB,
    M68K_OP_JMP, M68K_OP_JSR, M68K_OP_LEA, M68K_OP_LINK, M68K_OP_LPSTOP,
    M68K_OP_LSL, M68K_OP_LSR, M68K_OP_MOVE, M68K_OP_MOVEA, M68K_OP_MOVEC,
    M68K_OP_MOVEFCCR, M68K_OP_MOVEFSR, M68K_OP_MOVEM, M68K_OP_MOVEP,
    M68K_OP_MOVEQ, M68K_OP_MOVES, M68K_OP_MOVETCCR, M68K_OP_MOVETSR,
    M68K_OP_MOVEUSP, M68K_OP_MULL, M68K_OP_MULS, M68K_OP_MULU, M68K_OP_NBCD,
    M68K_OP_NEG, M68K_OP_NEGX, M68K_OP_NOP, M68K_OP_NOT, M68K_OP_OR,
    M68K_OP_ORI, M68K_OP_ORICCR, M68K_OP_ORISR, M68K_OP_PEA, M68K_OP_RESET,
    M68K_OP_ROL, M68K_OP_ROR, M68K_OP_ROXL, M68K_OP_ROXR, M68K_OP_RTD,
    M68K_OP_RTE, M68K_OP_RTR, M68K_OP_RTS, M68K_OP_SBCD, M68K_OP_SCC,
    M68K_OP_STOP, M68K_OP_SUB, M68K_OP_SUBA, M68K_OP_SUBI, M68K_OP_SUBQ,
    M68K_OP_SUBX, M68K_OP_SWAP, M68K_OP_TAS, M68K_OP_TBL, M68K_OP_TRAP,
    M68K_OP_TRAPCC, M68K_OP_TRAPV, M68K_OP_TST, M68K_OP_UNLK,
    M68K_OP_COUNT
} m68k_op;

/* Condition field values, as encoded. */
enum { M68K_COND_T = 0, M68K_COND_F, M68K_COND_HI, M68K_COND_LS,
       M68K_COND_CC, M68K_COND_CS, M68K_COND_NE, M68K_COND_EQ,
       M68K_COND_VC, M68K_COND_VS, M68K_COND_PL, M68K_COND_MI,
       M68K_COND_GE, M68K_COND_LT, M68K_COND_GT, M68K_COND_LE };

/* A fully unpacked effective address. */
typedef struct {
    uint8_t mode;          /* m68k_mode                                   */
    uint8_t reg;           /* An or Dn number for register-based modes    */
    uint8_t index;         /* index register, 0-7 Dn and 8-15 An          */
    uint8_t scale;         /* index scale: 1, 2, 4 or 8                   */
    bool    index_long;    /* index is a long, not a sign-extended word   */
    bool    index_used;    /* a full extension word can suppress it       */
    bool    base_used;     /* a full extension word can suppress the base */
    uint8_t ext_cycles;    /* what a full extension word costs to resolve  */
    int32_t disp;          /* displacement, absolute address or immediate */
} m68k_ea;

/* What a block must know without re-deriving the semantics. */
#define M68K_F_ENDS_BLOCK  0x01u  /* control leaves, or untrackable state changes */
#define M68K_F_PRIVILEGED  0x02u  /* traps outside supervisor mode               */
#define M68K_F_MAY_TRAP    0x04u  /* can raise an exception from its own operands */
#define M68K_F_ILLEGAL     0x08u  /* not a CPU32 encoding                        */

typedef struct {
    uint16_t word;      /* the opcode word itself             */
    uint16_t op;        /* m68k_op                            */
    uint8_t  size;      /* operand size in bytes: 0, 1, 2 or 4 */
    uint8_t  length;    /* whole instruction length in bytes   */
    uint8_t  cond;      /* condition field, where one applies   */
    uint8_t  flags;
    /*
     * What the reference core charges for this instruction, which is what
     * the Envoy's battery RAM measures time in. Everything an encoding can
     * decide is already in here, including the extra a MOVEM pays for each
     * register past the first and the penalty a full-format indexed
     * address pays; what depends on an operand -- a branch being taken, a
     * loop ending, a shift count in a register -- is added as the
     * instruction runs. See m68k_cycles.h.
     */
    uint16_t cycles;
    m68k_ea  src;
    m68k_ea  dst;
    uint32_t extra;     /* MOVEM mask, branch target, trap vector, bit number */
    uint32_t next_pc;   /* the instruction after this one, filled in by the
                         * decoder so an emitter has both destinations of a
                         * branch without re-deriving either */
} m68k_insn;

/*
 * Decode one instruction from `code`, which holds `avail` bytes of guest
 * memory in the guest's own big-endian order, as if fetched at `pc`.
 *
 * Returns the instruction length in bytes, or 0 when `avail` does not cover
 * the whole instruction, in which case `out` is not meaningful. An encoding
 * this part does not implement still decodes, with M68K_F_ILLEGAL set and
 * the length of the words that must be skipped to resume: an illegal
 * instruction is two bytes long and traps, which is a decode result like
 * any other, not a decoder failure.
 *
 * `pc` resolves PC-relative modes and branch targets into absolute
 * addresses, so a decoded instruction belongs to the address it was decoded
 * at. That is what a translator wants; a caller that moves code must decode
 * it again.
 */
unsigned m68k_decode(const uint8_t *code, unsigned avail, uint32_t pc,
                     m68k_insn *out);

/* No CPU32 instruction is longer than this. */
#define MRC_JIT_M68K_MAX_BYTES 24

/* The mnemonic, for diagnostics. Never NULL. */
const char *m68k_op_name(unsigned op);
/* The addressing mode, written the way the manual writes it. Never NULL. */
const char *m68k_mode_name(unsigned mode);

/*
 * How a decoded instruction is looked up in the cycle table.
 *
 * Here rather than in either user of it, because both the decoder and the
 * tool that builds the table have to agree exactly, and a second copy
 * would be a second thing to keep in step.
 *
 * (instruction, size, source mode, destination mode) decides the cost
 * everywhere but two encodings, where the direction is a bit of the
 * opcode word and nothing this structure records: MOVEC costs twice as
 * much writing a control register as reading one, and MOVEP one more
 * reading memory than writing it. That bit joins the key for those two.
 */
static inline uint32_t m68k_cycle_key(const m68k_insn *insn)
{
    unsigned size = insn->size == 0 ? 0 : insn->size == 1 ? 1 :
                    insn->size == 2 ? 2 : 3;
    unsigned variant = (insn->op == M68K_OP_MOVEC ? (insn->word & 1) :
                        insn->op == M68K_OP_MOVEP ? ((insn->word >> 7) & 1) : 0);
    return (uint32_t)variant << 19 | (uint32_t)insn->op << 11 |
           (uint32_t)size << 8 |
           (uint32_t)insn->src.mode << 4 | (uint32_t)insn->dst.mode;
}

#endif /* MRC_M68K_DECODE_H */
