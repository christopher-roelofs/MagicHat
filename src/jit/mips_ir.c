#include "jit/ir.h"

/* Every word decodes: what the backends do not implement becomes J_EXEC.
 * Returns false only for words that must not join a block at all (none at
 * present; the return value is kept so the frontend can veto later). */
bool mrc_jit_decode_mips(uint32_t w, mrc_jit_ir *i)
{
    unsigned op = w >> 26, fn = w & 63;
    unsigned rs = (w >> 21) & 31, rt = (w >> 16) & 31, rd = (w >> 11) & 31;
    *i = (mrc_jit_ir){.op = J_EXEC, .dst = rt, .left = rs, .right = rt,
                       .word = w};
    if (!op) {
        i->dst = rd;
        switch (fn) {
        case 0: case 2: case 3:
            i->op = fn == 0 ? J_SHL : fn == 2 ? J_SHR : J_SAR;
            i->left = rt; i->immediate = true; i->value = (w >> 6) & 31;
            return true;
        case 4: case 6: case 7:
            i->op = fn == 4 ? J_SHL : fn == 6 ? J_SHR : J_SAR;
            i->left = rt; i->right = rs; return true;
        case 8: case 9:
            i->op = J_JUMPR;
            i->dst = fn == 9 ? (rd ? rd : 31) : 0;
            return true;
        case 0x10: i->op = J_MFHI; return true;
        case 0x11: i->op = J_MTHI; i->dst = 0; return true;
        case 0x12: i->op = J_MFLO; return true;
        case 0x13: i->op = J_MTLO; i->dst = 0; return true;
        case 0x18: i->op = J_MULT; i->dst = 0; return true;
        case 0x19: i->op = J_MULTU; i->dst = 0; return true;
        case 0x20: i->op = J_ADD_OV; return true;
        case 0x21: i->op = J_ADD; return true;
        case 0x22: i->op = J_SUB_OV; return true;
        case 0x23: i->op = J_SUB; return true;
        case 0x24: i->op = J_AND; return true;
        case 0x25: i->op = J_OR; return true;
        case 0x26: i->op = J_XOR; return true;
        case 0x27: i->op = J_NOR; return true;
        case 0x2a: i->op = J_SLT; return true;
        case 0x2b: i->op = J_SLTU; return true;
        case 0x0c: case 0x0d:              /* SYSCALL, BREAK: exceptions */
            i->ends_block = true; return true;
        default:                           /* DIV/DIVU continue; reserved */
            i->ends_block = !(fn == 0x1a || fn == 0x1b);
            return true;
        }
    }
    if (op >= 1 && op <= 7) {
        i->dst = 0;
        i->value = (uint32_t)(int32_t)(int16_t)w << 2;
        switch (op) {
        case 1:
            if (rt != 0 && rt != 1 && rt != 16 && rt != 17) {
                i->op = J_EXEC; i->ends_block = true; return true; /* RI */
            }
            i->op = (rt & 1) ? J_BGEZ : J_BLTZ;
            i->dst = (rt & 16) ? 31 : 0;
            i->right = 0; break;
        case 2: case 3:
            i->op = J_JUMP; i->value = (w & 0x03ffffffu) << 2;
            i->dst = op == 3 ? 31 : 0; break;
        case 4: i->op = J_BEQ; break;
        case 5: i->op = J_BNE; break;
        case 6: i->op = J_BLEZ; i->right = 0; break;
        case 7: i->op = J_BGTZ; i->right = 0; break;
        }
        return true;
    }
    i->immediate = true;
    i->value = (uint32_t)(int32_t)(int16_t)w;
    switch (op) {
    case 8: i->op = J_ADD_OV; return true;
    case 9: i->op = J_ADD; return true;
    case 10: i->op = J_SLT; return true;
    case 11: i->op = J_SLTU; return true;
    case 12: i->op = J_AND; i->value = w & 65535; return true;
    case 13: i->op = J_OR; i->value = w & 65535; return true;
    case 14: i->op = J_XOR; i->value = w & 65535; return true;
    case 15: i->op = J_CONST; i->value = w << 16; return true;
    case 0x20: case 0x21: case 0x23: case 0x24: case 0x25:
        i->op = J_LOAD;
        i->size = op == 0x23 ? 4 : (op & 1) ? 2 : 1;
        i->sign = op < 0x24;
        return true;
    case 0x28: case 0x29: case 0x2b:
        i->op = J_STORE; i->dst = 0;
        i->size = op == 0x2b ? 4 : op == 0x29 ? 2 : 1;
        return true;
    case 0x22: case 0x26: case 0x2a: case 0x2e: /* LWL/LWR/SWL/SWR */
    case 0x2f:                             /* CACHE */
        i->immediate = false; return true;
    case 0x10:                             /* COP0 */
        i->immediate = false;
        i->ends_block = rs != 0;           /* MFC0 continues; writes exit */
        return true;
    default:                               /* COP1-3, LWC/SWC, reserved */
        i->immediate = false; i->ends_block = true; return true;
    }
}
