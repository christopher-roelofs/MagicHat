#ifndef MH_JIT_IR_H
#define MH_JIT_IR_H
#include <stdbool.h>
#include <stdint.h>
/*
 * Host-neutral block IR. One entry per guest instruction, in program order.
 *
 * Everything the backends do not implement natively is J_EXEC: the raw
 * instruction word runs through the reference interpreter's exec body from
 * inside the block, so a block never ends merely because an instruction is
 * rare. The helper reports whether the instruction left the straight-line
 * path (exception, halt, power stop), and the block exits when it did.
 * Instructions that can change interrupt visibility (CP0 writes, RFE) end
 * the block unconditionally so the dispatcher checks interrupts afterward,
 * exactly where the reference step would.
 */
typedef enum {
    J_ADD, J_SUB, J_AND, J_OR, J_XOR, J_NOR, J_SLT, J_SLTU,
    J_SHL, J_SHR, J_SAR, J_CONST,
    J_ADD_OV, J_SUB_OV,              /* trap on signed overflow (ADDI/ADD/SUB) */
    J_MULT, J_MULTU,                 /* hi:lo = left * right */
    J_MFHI, J_MFLO, J_MTHI, J_MTLO,  /* dst = hi/lo; hi/lo = left */
    J_LOAD,                          /* dst = mem[left + value], size/sign */
    J_STORE,                         /* mem[left + value] = right */
    J_EXEC,                          /* reference interpreter runs word */
    J_BEQ, J_BNE, J_BLEZ, J_BGTZ, J_BLTZ, J_BGEZ,
    J_JUMP, J_JUMPR
} mh_jit_op;
typedef struct {
    mh_jit_op op;
    uint8_t dst, left, right;
    bool immediate;     /* ALU: right operand is `value`                      */
    uint8_t size;       /* J_LOAD/J_STORE: 1, 2 or 4                          */
    bool sign;          /* J_LOAD: sign-extend                                */
    bool ends_block;    /* J_EXEC: exit after this instruction unconditionally */
    uint32_t value;     /* immediate, branch displacement or jump target      */
    uint32_t word;      /* the original instruction, for J_EXEC              */
} mh_jit_ir;
static inline bool mh_jit_is_branch(const mh_jit_ir *i) { return i->op >= J_BEQ; }
bool mh_jit_decode_mips(uint32_t word, mh_jit_ir *ir);
#endif
