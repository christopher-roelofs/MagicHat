/* SPECIAL instruction bodies shared by raw and flattened decoded dispatch. */
    case SPECIAL_OP(0x00): set_reg(c, RD(insn), t << SA(insn)); return;              /* SLL  */
    case SPECIAL_OP(0x02): set_reg(c, RD(insn), t >> SA(insn)); return;              /* SRL  */
    case SPECIAL_OP(0x03): set_reg(c, RD(insn), (uint32_t)((int32_t)t >> SA(insn))); return; /* SRA */
    case SPECIAL_OP(0x04): set_reg(c, RD(insn), t << (s & 31)); return;              /* SLLV */
    case SPECIAL_OP(0x06): set_reg(c, RD(insn), t >> (s & 31)); return;              /* SRLV */
    case SPECIAL_OP(0x07): set_reg(c, RD(insn), (uint32_t)((int32_t)t >> (s & 31))); return; /* SRAV */

    case SPECIAL_OP(0x08): branch(c, s); return;                                     /* JR   */
    case SPECIAL_OP(0x09): set_reg(c, RD(insn) ? RD(insn) : 31, c->next_pc);
               branch(c, s); return;                                     /* JALR */

    case SPECIAL_OP(0x0C): raise_exc(c, EXC_Sys, 0, false); return;                  /* SYSCALL */
    case SPECIAL_OP(0x0D): raise_exc(c, EXC_Bp, 0, false);  return;                  /* BREAK   */

    case SPECIAL_OP(0x10): set_reg(c, RD(insn), c->hi); return;                      /* MFHI */
    case SPECIAL_OP(0x11): c->hi = s; return;                                        /* MTHI */
    case SPECIAL_OP(0x12): set_reg(c, RD(insn), c->lo); return;                      /* MFLO */
    case SPECIAL_OP(0x13): c->lo = s; return;                                        /* MTLO */

    case SPECIAL_OP(0x18): {                                                         /* MULT */
        int64_t p = (int64_t)(int32_t)s * (int64_t)(int32_t)t;
        c->lo = (uint32_t)p; c->hi = (uint32_t)(p >> 32); return;
    }
    case SPECIAL_OP(0x19): {                                                         /* MULTU */
        uint64_t p = (uint64_t)s * (uint64_t)t;
        c->lo = (uint32_t)p; c->hi = (uint32_t)(p >> 32); return;
    }
    case SPECIAL_OP(0x1A):                                                           /* DIV  */
        if (t == 0) {
            /* Architecturally undefined. Real R3900 leaves HI/LO with the
             * partial result; we mirror the widely-observed behaviour. */
            c->lo = ((int32_t)s < 0) ? 1u : 0xFFFFFFFFu;
            c->hi = s;
        } else if (s == 0x80000000u && t == 0xFFFFFFFFu) {
            c->lo = 0x80000000u; c->hi = 0;
        } else {
            c->lo = (uint32_t)((int32_t)s / (int32_t)t);
            c->hi = (uint32_t)((int32_t)s % (int32_t)t);
        }
        return;
    case SPECIAL_OP(0x1B):                                                           /* DIVU */
        if (t == 0) { c->lo = 0xFFFFFFFFu; c->hi = s; }
        else        { c->lo = s / t;       c->hi = s % t; }
        return;

    case SPECIAL_OP(0x20): {                                                         /* ADD */
        uint32_t r = s + t;
        if (~(s ^ t) & (s ^ r) & 0x80000000u) { raise_exc(c, EXC_Ov, 0, false); return; }
        set_reg(c, RD(insn), r); return;
    }
    case SPECIAL_OP(0x21): set_reg(c, RD(insn), s + t); return;                      /* ADDU */
    case SPECIAL_OP(0x22): {                                                         /* SUB */
        uint32_t r = s - t;
        if ((s ^ t) & (s ^ r) & 0x80000000u) { raise_exc(c, EXC_Ov, 0, false); return; }
        set_reg(c, RD(insn), r); return;
    }
    case SPECIAL_OP(0x23): set_reg(c, RD(insn), s - t); return;                      /* SUBU */
    case SPECIAL_OP(0x24): set_reg(c, RD(insn), s & t); return;                      /* AND  */
    case SPECIAL_OP(0x25): set_reg(c, RD(insn), s | t); return;                      /* OR   */
    case SPECIAL_OP(0x26): set_reg(c, RD(insn), s ^ t); return;                      /* XOR  */
    case SPECIAL_OP(0x27): set_reg(c, RD(insn), ~(s | t)); return;                   /* NOR  */
    case SPECIAL_OP(0x2A): set_reg(c, RD(insn), (int32_t)s < (int32_t)t); return;    /* SLT  */
    case SPECIAL_OP(0x2B): set_reg(c, RD(insn), s < t); return;                      /* SLTU */

