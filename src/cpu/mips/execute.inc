/* Shared instruction semantics instantiated for raw and decoded operands.
 * Keep decoding/cache policy in r3900.c. This file intentionally has no guard. */
static void branch_rel(r3900 *c, MH_INSN insn, bool taken)
{
    if (taken)
        branch(c, c->pc + (SIMM(insn) << 2));
    else
        c->branch_pending = true;   /* the delay slot still executes */
}

#ifndef MH_FLAT_DISPATCH
static void exec_special(r3900 *c, MH_INSN insn)
{
    uint32_t s = c->r[RS(insn)], t = c->r[RT(insn)];

    switch (FN(insn)) {
#include "cpu/mips/special.inc"

    default:
        if (c->on_trap)
            c->on_trap(c, "unimplemented SPECIAL function", WORD(insn));
        raise_exc(c, EXC_RI, 0, false);
        return;
    }
}

#endif

static void exec_regimm(r3900 *c, MH_INSN insn)
{
    int32_t s = (int32_t)c->r[RS(insn)];

    switch (RT(insn)) {
    case 0x00: branch_rel(c, insn, s <  0); return;                      /* BLTZ   */
    case 0x01: branch_rel(c, insn, s >= 0); return;                      /* BGEZ   */
    case 0x10: set_reg(c, 31, c->next_pc); branch_rel(c, insn, s <  0); return; /* BLTZAL */
    case 0x11: set_reg(c, 31, c->next_pc); branch_rel(c, insn, s >= 0); return; /* BGEZAL */
    default:
        if (c->on_trap)
            c->on_trap(c, "unimplemented REGIMM function", WORD(insn));
        raise_exc(c, EXC_RI, 0, false);
        return;
    }
}

static void exec(r3900 *c, MH_INSN insn)
{
    uint32_t s = c->r[RS(insn)], t = c->r[RT(insn)];
    uint32_t v;

    switch (DISPATCH(insn)) {
#ifdef MH_FLAT_DISPATCH
#include "cpu/mips/special.inc"
#else
    case 0x00: exec_special(c, insn); return;
#endif
    case 0x01: exec_regimm(c, insn);  return;

    case 0x02: branch(c, (c->pc & 0xF0000000u) | JTARGET(insn));
               return;                                                   /* J   */
    case 0x03: set_reg(c, 31, c->next_pc);
               branch(c, (c->pc & 0xF0000000u) | JTARGET(insn));
               return;                                                   /* JAL */

    case 0x04: branch_rel(c, insn, s == t); return;                      /* BEQ  */
    case 0x05: branch_rel(c, insn, s != t); return;                      /* BNE  */
    case 0x06: branch_rel(c, insn, (int32_t)s <= 0); return;             /* BLEZ */
    case 0x07: branch_rel(c, insn, (int32_t)s >  0); return;             /* BGTZ */

    case 0x08: {                                                         /* ADDI */
        uint32_t r = s + SIMM(insn);
        if (~(s ^ SIMM(insn)) & (s ^ r) & 0x80000000u) {
            raise_exc(c, EXC_Ov, 0, false); return;
        }
        set_reg(c, RT(insn), r); return;
    }
    case 0x09: set_reg(c, RT(insn), s + SIMM(insn)); return;             /* ADDIU */
    case 0x0A: set_reg(c, RT(insn), (int32_t)s < (int32_t)SIMM(insn)); return; /* SLTI  */
    case 0x0B: set_reg(c, RT(insn), s < SIMM(insn)); return;             /* SLTIU */
    case 0x0C: set_reg(c, RT(insn), s & IMM(insn)); return;              /* ANDI  */
    case 0x0D: set_reg(c, RT(insn), s | IMM(insn)); return;              /* ORI   */
    case 0x0E: set_reg(c, RT(insn), s ^ IMM(insn)); return;              /* XORI  */
    case 0x0F: set_reg(c, RT(insn), IMM(insn) << 16); return;            /* LUI   */

    case 0x10:                                                           /* COP0 */
        switch (RS(insn)) {
        case 0x00: set_reg(c, RT(insn), read_cp0(c, RD(insn))); return;  /* MFC0 */
        case 0x04: write_cp0(c, RD(insn), t); return;                    /* MTC0 */
        default:
            if (WORD(insn) & (1u << 25)) { cop0_op(c, WORD(insn)); return; }         /* CO   */
            if (c->on_trap)
                c->on_trap(c, "unimplemented COP0 rs field", WORD(insn));
            raise_exc(c, EXC_RI, 0, false);
            return;
        }

    case 0x11: case 0x12: case 0x13:                                     /* COP1-3 */
        /* No FPU and no coprocessors 2/3 on this part. The architectural
         * response is a Coprocessor Unusable exception, and the OS may well
         * depend on that; do not fake a result. */
        c->cp0[CP0_CAUSE] = (c->cp0[CP0_CAUSE] & ~(3u << CAUSE_CE_SHIFT)) |
                            ((OP(insn) - 0x10) << CAUSE_CE_SHIFT);
        raise_exc(c, EXC_CpU, 0, false);
        return;

    case 0x20: if (load(c, s + SIMM(insn), 1, &v))
                   set_reg(c, RT(insn), (uint32_t)(int32_t)(int8_t)v);
               return;                                                   /* LB  */
    case 0x21: if (load(c, s + SIMM(insn), 2, &v))
                   set_reg(c, RT(insn), (uint32_t)(int32_t)(int16_t)v);
               return;                                                   /* LH  */
    case 0x23: if (load(c, s + SIMM(insn), 4, &v))
                   set_reg(c, RT(insn), v);
               return;                                                   /* LW  */
    case 0x24: if (load(c, s + SIMM(insn), 1, &v)) set_reg(c, RT(insn), v);
               return;                                                   /* LBU */
    case 0x25: if (load(c, s + SIMM(insn), 2, &v)) set_reg(c, RT(insn), v);
               return;                                                   /* LHU */

    case 0x22: case 0x26: {                                              /* LWL/LWR */
        uint32_t addr = s + SIMM(insn);
        uint32_t word;
        if (!load(c, addr & ~3u, 4, &word))
            return;
        unsigned b = addr & 3;                 /* big-endian byte position */
        if (OP(insn) == 0x22) {                /* LWL */
            unsigned sh = b * 8;
            uint32_t mask = sh ? (0xFFFFFFFFu >> (32 - sh)) : 0;
            set_reg(c, RT(insn), (word << sh) | (t & mask));
        } else {                               /* LWR */
            unsigned sh = (3 - b) * 8;
            uint32_t mask = sh ? (0xFFFFFFFFu << (32 - sh)) : 0;
            set_reg(c, RT(insn), (word >> sh) | (t & mask));
        }
        return;
    }

    case 0x28: store(c, s + SIMM(insn), 1, t); return;                   /* SB */
    case 0x29: store(c, s + SIMM(insn), 2, t); return;                   /* SH */
    case 0x2B: store(c, s + SIMM(insn), 4, t); return;                   /* SW */

    case 0x2A: case 0x2E: {                                              /* SWL/SWR */
        uint32_t addr = s + SIMM(insn);
        uint32_t word;
        if (!load(c, addr & ~3u, 4, &word))
            return;
        unsigned b = addr & 3;
        if (OP(insn) == 0x2A) {                /* SWL */
            unsigned sh = b * 8;
            uint32_t mask = sh ? (0xFFFFFFFFu << (32 - sh)) : 0;
            store(c, addr & ~3u, 4, (word & mask) | (t >> sh));
        } else {                               /* SWR */
            unsigned sh = (3 - b) * 8;
            uint32_t mask = sh ? (0xFFFFFFFFu >> (32 - sh)) : 0;
            store(c, addr & ~3u, 4, (word & mask) | (t << sh));
        }
        return;
    }

    case 0x2F:
        /*
         * CACHE. The R3900 adds this over MIPS-I; the ROM uses it at
         * 0x83C008AC to invalidate 4096 bytes in 16-byte lines (the 4 KB
         * I-cache) and at 0x83C008D4 for 1024 bytes (the 1 KB D-cache),
         * which matches the TMPR3902U's documented cache sizes.
         *
         * We model no caches, and cache maintenance has no program-visible
         * effect on a machine that has none — so this is one of the few
         * places where doing nothing is the accurate answer rather than a
         * shortcut. It stays a decoded instruction, not a fall-through to
         * the unknown-opcode trap, so the trap keeps its meaning.
         */
        c->cache_ops++;
        return;

    case 0x30: case 0x31: case 0x32: case 0x33:                          /* LWCz */
    case 0x38: case 0x39: case 0x3A: case 0x3B:                          /* SWCz */
        c->cp0[CP0_CAUSE] = (c->cp0[CP0_CAUSE] & ~(3u << CAUSE_CE_SHIFT)) |
                            ((OP(insn) & 3u) << CAUSE_CE_SHIFT);
        raise_exc(c, EXC_CpU, 0, false);
        return;

    default:
        if (c->on_trap)
            c->on_trap(c, OP(insn) == 0 ? "unimplemented SPECIAL function" :
                       "unimplemented opcode", WORD(insn));
        raise_exc(c, EXC_RI, 0, false);
        return;
    }
}
