#include "cpu/m68k/core/m68k.h"
#include <string.h>

/* ------------------------------------------------------------- state */

void m68k_init(m68k *c, const m68k_bus *bus)
{
    memset(c, 0, sizeof *c);
    c->bus = *bus;
    c->supervisor = true;
    c->interrupt_mask = 7;
}

void m68k_reset(m68k *c)
{
    /* The processor's clock restarts with it, which is what the reference
     * does; a device measuring elapsed cycles across a reset is measuring
     * across a machine that was off. */
    c->cycles = 0;
    c->supervisor = true;
    c->trace = false;
    c->interrupt_mask = 7;
    c->n = c->z = c->v = c->c = c->x = 0;
    c->exception = 0;
    c->a[7] = c->bus.read(c->bus.ctx, 0, 4);
    c->ssp = c->a[7];
    c->pc = c->bus.read(c->bus.ctx, 4, 4);
}

uint8_t m68k_get_ccr(const m68k *c)
{
    return (uint8_t)((c->x ? 0x10 : 0) | (c->n ? 8 : 0) | (c->z ? 4 : 0) |
                     (c->v ? 2 : 0) | (c->c ? 1 : 0));
}

void m68k_set_ccr(m68k *c, uint8_t ccr)
{
    c->x = (ccr >> 4) & 1; c->n = (ccr >> 3) & 1; c->z = (ccr >> 2) & 1;
    c->v = (ccr >> 1) & 1; c->c = ccr & 1;
}

uint16_t m68k_get_sr(const m68k *c)
{
    return (uint16_t)(m68k_get_ccr(c) | ((uint16_t)c->interrupt_mask << 8) |
                      (c->supervisor ? 0x2000 : 0) | (c->trace ? 0x8000 : 0));
}

void m68k_set_sr(m68k *c, uint16_t sr)
{
    bool supervisor = (sr & 0x2000) != 0;
    if (supervisor != c->supervisor) {
        /* The stack pointer is two registers wearing one name. */
        if (c->supervisor) { c->ssp = c->a[7]; c->a[7] = c->usp; }
        else               { c->usp = c->a[7]; c->a[7] = c->ssp; }
        c->supervisor = supervisor;
    }
    c->trace = (sr & 0x8000) != 0;
    c->interrupt_mask = (uint8_t)((sr >> 8) & 7);
    m68k_set_ccr(c, (uint8_t)(sr & 0x1F));
}

bool m68k_condition(const m68k *c, unsigned cond)
{
    switch (cond) {
    case M68K_COND_T:  return true;
    case M68K_COND_F:  return false;
    case M68K_COND_HI: return !c->c && !c->z;
    case M68K_COND_LS: return c->c || c->z;
    case M68K_COND_CC: return !c->c;
    case M68K_COND_CS: return c->c;
    case M68K_COND_NE: return !c->z;
    case M68K_COND_EQ: return c->z;
    case M68K_COND_VC: return !c->v;
    case M68K_COND_VS: return c->v;
    case M68K_COND_PL: return !c->n;
    case M68K_COND_MI: return c->n;
    case M68K_COND_GE: return c->n == c->v;
    case M68K_COND_LT: return c->n != c->v;
    case M68K_COND_GT: return c->n == c->v && !c->z;
    default:           return c->n != c->v || c->z;   /* LE */
    }
}

/* --------------------------------------------------------- operands */

static uint32_t mask_of(unsigned size)
{
    return size == 1 ? 0xFFu : size == 2 ? 0xFFFFu : 0xFFFFFFFFu;
}

static uint32_t msb_of(unsigned size)
{
    return size == 1 ? 0x80u : size == 2 ? 0x8000u : 0x80000000u;
}

static uint32_t sign_extend(uint32_t v, unsigned size)
{
    return size == 1 ? (uint32_t)(int32_t)(int8_t)v
         : size == 2 ? (uint32_t)(int32_t)(int16_t)v : v;
}

m68k_operand m68k_resolve(m68k *c, const m68k_ea *ea, unsigned size)
{
    m68k_operand op;
    memset(&op, 0, sizeof op);
    if (!size) size = 2;
    switch (ea->mode) {
    case M68K_DN: op.kind = M68K_OPERAND_DREG; op.reg = ea->reg; return op;
    case M68K_AN: op.kind = M68K_OPERAND_AREG; op.reg = ea->reg; return op;
    case M68K_IMM: op.kind = M68K_OPERAND_IMM; op.addr = (uint32_t)ea->disp; return op;
    default: break;
    }
    op.kind = M68K_OPERAND_MEM;
    switch (ea->mode) {
    case M68K_AI:
        op.addr = c->a[ea->reg];
        break;
    case M68K_PI:
        op.addr = c->a[ea->reg];
        /* The stack pointer is never left odd, so a byte moves it by two. */
        c->a[ea->reg] += (ea->reg == 7 && size == 1) ? 2 : size;
        break;
    case M68K_PD:
        c->a[ea->reg] -= (ea->reg == 7 && size == 1) ? 2 : size;
        op.addr = c->a[ea->reg];
        break;
    case M68K_DI:
        op.addr = c->a[ea->reg] + (uint32_t)ea->disp;
        break;
    case M68K_IX:
    case M68K_PCX: {
        /* The displacement already carries the program counter for the
         * program-relative form, and carries it only when the base was not
         * suppressed, so both forms add the same three things here. */
        uint32_t base = ea->mode == M68K_IX
                      ? (ea->base_used ? c->a[ea->reg] : 0) : 0;
        uint32_t index = 0;
        if (ea->index_used) {
            uint32_t r = ea->index < 8 ? c->d[ea->index] : c->a[ea->index - 8];
            index = (ea->index_long ? r : sign_extend(r, 2)) * ea->scale;
        }
        op.addr = base + index + (uint32_t)ea->disp;
        break;
    }
    default:                     /* AW, AL, PCD: already absolute */
        op.addr = (uint32_t)ea->disp;
        break;
    }
    return op;
}

uint32_t m68k_read(m68k *c, const m68k_operand *op, unsigned size)
{
    switch (op->kind) {
    case M68K_OPERAND_DREG: return c->d[op->reg] & mask_of(size);
    case M68K_OPERAND_AREG: return c->a[op->reg] & mask_of(size);
    case M68K_OPERAND_IMM:  return op->addr & mask_of(size);
    default: return c->bus.read(c->bus.ctx, op->addr, size);
    }
}

void m68k_write(m68k *c, const m68k_operand *op, unsigned size, uint32_t value)
{
    switch (op->kind) {
    case M68K_OPERAND_DREG:
        c->d[op->reg] = (c->d[op->reg] & ~mask_of(size)) | (value & mask_of(size));
        return;
    case M68K_OPERAND_AREG:
        /* An address register is written whole, whatever the operand size:
         * a word is sign-extended into it rather than laid on top. */
        c->a[op->reg] = sign_extend(value, size);
        return;
    case M68K_OPERAND_IMM:
        return;
    default:
        c->bus.write(c->bus.ctx, op->addr, size, value);
        return;
    }
}

/* ------------------------------------------------------------- flags */

static void set_nz(m68k *c, uint32_t result, unsigned size)
{
    c->n = (result & msb_of(size)) != 0;
    c->z = (result & mask_of(size)) == 0;
}

static void logic_flags(m68k *c, uint32_t result, unsigned size)
{
    set_nz(c, result, size);
    c->v = 0;
    c->c = 0;
}

static uint32_t do_add(m68k *c, uint32_t a, uint32_t b, unsigned size, bool extend)
{
    uint32_t m = mask_of(size), sign = msb_of(size);
    uint32_t carry_in = extend && c->x ? 1u : 0u;
    uint64_t wide = (uint64_t)(a & m) + (b & m) + carry_in;
    uint32_t r = (uint32_t)wide & m;
    c->c = (wide & ((uint64_t)m + 1)) != 0;
    c->x = c->c;
    c->v = (~(a ^ b) & (a ^ r) & sign) != 0;
    c->n = (r & sign) != 0;
    /* The extended forms leave Z alone unless the result is non-zero, so a
     * multi-word sum reports zero only when every word of it was. */
    if (extend) { if (r) c->z = 0; }
    else c->z = r == 0;
    return r;
}

static uint32_t do_sub(m68k *c, uint32_t a, uint32_t b, unsigned size,
                       bool extend, bool keep_x)
{
    uint32_t m = mask_of(size), sign = msb_of(size);
    uint32_t borrow_in = extend && c->x ? 1u : 0u;
    uint64_t wide = (uint64_t)(a & m) - (b & m) - borrow_in;
    uint32_t r = (uint32_t)wide & m;
    c->c = (wide & ((uint64_t)m + 1)) != 0;
    if (!keep_x) c->x = c->c;
    c->v = ((a ^ b) & (a ^ r) & sign) != 0;
    c->n = (r & sign) != 0;
    if (extend) { if (r) c->z = 0; }
    else c->z = r == 0;
    return r;
}

/*
 * Binary-coded decimal addition and subtraction, digit by digit with the
 * corrections the hardware applies. The zero flag is sticky here as it is
 * in the extended binary forms, so a multi-byte decimal sum reports zero
 * only when every byte of it was.
 */
static uint32_t do_bcd(m68k *c, uint32_t op1, uint32_t op2, bool add)
{
    uint32_t hi1 = op1 & 0xF0, lo1 = op1 & 0x0F;
    uint32_t hi2 = op2 & 0xF0, lo2 = op2 & 0x0F;
    uint32_t result;

    if (add) {
        uint32_t lo = lo1 + lo2 + c->x;
        uint32_t hi = hi1 + hi2;
        result = hi + lo;
        if (lo > 9) result += 0x06;
        if ((result & 0x3F0) > 0x90) { result += 0x60; c->x = 1; }
        else c->x = 0;
    } else {
        uint32_t lo = lo2 - lo1 - c->x;
        uint32_t hi = hi2 - hi1;
        result = hi + lo;
        if (lo & 0xF0) {
            result -= 0x06;
            c->x = ((op2 - op1 - 6 - c->x) & 0x300) > 0xFF;
        } else {
            c->x = ((op2 - op1 - c->x) & 0x300) > 0xFF;
        }
        if (((op2 - op1 - c->x) & 0x100) > 0xFF) result -= 0x60;
    }
    /* This part reports no overflow for a decimal operation. */
    c->v = 0;
    c->c = c->x;
    c->n = (result & 0x80) != 0;
    if (result & 0xFF) c->z = 0;
    return result & 0xFF;
}

/* --------------------------------------------------------- memory ops */

static void push32(m68k *c, uint32_t value)
{
    c->a[7] -= 4;
    c->bus.write(c->bus.ctx, c->a[7], 4, value);
}

static uint32_t pop32(m68k *c)
{
    uint32_t v = c->bus.read(c->bus.ctx, c->a[7], 4);
    c->a[7] += 4;
    return v;
}

static void movem_to_memory(m68k *c, const m68k_insn *insn)
{
    unsigned size = insn->size;
    uint16_t mask = (uint16_t)insn->extra;
    if (insn->dst.mode == M68K_PD) {
        /*
         * Pre-decrement stores from the top down, and its register list is
         * numbered the other way round: the lowest bit names A7 rather than
         * D0. The address register itself is left pointing at the last
         * value written.
         */
        uint32_t addr = c->a[insn->dst.reg];
        for (unsigned bit = 0; bit < 16; bit++) {
            if (!(mask & (1u << bit))) continue;
            unsigned index = 15 - bit;
            uint32_t value = index < 8 ? c->d[index] : c->a[index - 8];
            addr -= size;
            c->bus.write(c->bus.ctx, addr, size, value);
        }
        c->a[insn->dst.reg] = addr;
        return;
    }
    m68k_operand op = m68k_resolve(c, &insn->dst, size);
    uint32_t addr = op.addr;
    for (unsigned index = 0; index < 16; index++) {
        if (!(mask & (1u << index))) continue;
        uint32_t value = index < 8 ? c->d[index] : c->a[index - 8];
        c->bus.write(c->bus.ctx, addr, size, value);
        addr += size;
    }
}

static void movem_to_registers(m68k *c, const m68k_insn *insn)
{
    unsigned size = insn->size;
    uint16_t mask = (uint16_t)insn->extra;
    m68k_operand op = m68k_resolve(c, &insn->src, size);
    uint32_t addr = insn->src.mode == M68K_PI ? c->a[insn->src.reg] - size
                                              : op.addr;
    if (insn->src.mode == M68K_PI) addr = op.addr;
    for (unsigned index = 0; index < 16; index++) {
        if (!(mask & (1u << index))) continue;
        /* A word load is sign-extended into the whole register, data and
         * address registers alike. */
        uint32_t value = sign_extend(c->bus.read(c->bus.ctx, addr, size), size);
        if (index < 8) c->d[index] = value;
        else c->a[index - 8] = value;
        addr += size;
    }
    if (insn->src.mode == M68K_PI) c->a[insn->src.reg] = addr;
}

/* --------------------------------------------------------- shifting */

static uint32_t do_shift(m68k *c, unsigned op, uint32_t value, unsigned count,
                         unsigned size)
{
    uint32_t m = mask_of(size), sign = msb_of(size);
    uint32_t r = value & m;
    unsigned bits = size * 8;

    if (count == 0) {
        /* Nothing moves, and only the rotate through the extend bit has
         * anything to say about the carry. */
        c->c = (op == M68K_OP_ROXL || op == M68K_OP_ROXR) ? c->x : 0;
        c->v = 0;
        set_nz(c, r, size);
        return r;
    }

    bool carry = false;
    bool overflow = false;
    for (unsigned k = 0; k < count; k++) {
        switch (op) {
        case M68K_OP_ASL: {
            carry = (r & sign) != 0;
            uint32_t before = r;
            r = (r << 1) & m;
            if (((before ^ r) & sign) != 0) overflow = true;
            break;
        }
        case M68K_OP_LSL:
            carry = (r & sign) != 0;
            r = (r << 1) & m;
            break;
        case M68K_OP_ASR:
            carry = (r & 1) != 0;
            r = ((r >> 1) | (r & sign)) & m;
            break;
        case M68K_OP_LSR:
            carry = (r & 1) != 0;
            r = (r >> 1) & m;
            break;
        case M68K_OP_ROL:
            carry = (r & sign) != 0;
            r = ((r << 1) | (carry ? 1u : 0u)) & m;
            break;
        case M68K_OP_ROR:
            carry = (r & 1) != 0;
            r = ((r >> 1) | (carry ? sign : 0u)) & m;
            break;
        case M68K_OP_ROXL: {
            bool through = c->x != 0;
            carry = (r & sign) != 0;
            r = ((r << 1) | (through ? 1u : 0u)) & m;
            c->x = carry;
            break;
        }
        default: {                                  /* ROXR */
            bool through = c->x != 0;
            carry = (r & 1) != 0;
            r = ((r >> 1) | (through ? sign : 0u)) & m;
            c->x = carry;
            break;
        }
        }
        (void)bits;
    }

    c->c = carry;
    if (op != M68K_OP_ROL && op != M68K_OP_ROR &&
        op != M68K_OP_ROXL && op != M68K_OP_ROXR)
        c->x = carry;
    c->v = (op == M68K_OP_ASL) ? overflow : 0;
    set_nz(c, r, size);
    return r;
}

/* ------------------------------------------------------------ execute */

/*
 * Take an exception: save the status register, enter supervisor mode with
 * tracing off, build the frame, and vector.
 *
 * Which frame depends on the exception. The short one records where to
 * resume; the six-word one also records which instruction caused it, which
 * the handlers of the arithmetic traps need because the return address
 * already points past them. `instruction_pc` is that instruction, and
 * `c->pc` is already wherever the exception wants to resume, so the two
 * are not the same value and the caller decides each.
 */
static void raise_at(m68k *c, unsigned vector, uint32_t instruction_pc,
                     uint32_t data)
{
    c->exception = vector;
    c->exception_data = data;
    c->exception_count++;
    c->vector_count[vector & 0xFF]++;
    c->last_vector = vector;
    c->last_vector_pc = instruction_pc;

    uint16_t sr = m68k_get_sr(c);
    if (!c->supervisor) {
        c->usp = c->a[7];
        c->a[7] = c->ssp;
        c->supervisor = true;
    }
    c->trace = false;

    bool six_word = vector == M68K_VEC_DIVIDE_BY_ZERO ||
                    vector == M68K_VEC_CHK || vector == M68K_VEC_TRAPV ||
                    vector == M68K_VEC_TRACE;
    if (six_word) {
        c->a[7] -= 4;
        c->bus.write(c->bus.ctx, c->a[7], 4, instruction_pc);
        c->a[7] -= 2;
        c->bus.write(c->bus.ctx, c->a[7], 2, 0x2000u | (vector * 4));
    } else {
        c->a[7] -= 2;
        c->bus.write(c->bus.ctx, c->a[7], 2, vector * 4);
    }
    c->a[7] -= 4;
    c->bus.write(c->bus.ctx, c->a[7], 4, c->pc);
    c->a[7] -= 2;
    c->bus.write(c->bus.ctx, c->a[7], 2, sr);

    c->pc = c->bus.read(c->bus.ctx, c->vbr + vector * 4, 4);
    c->last_vector_sp = c->a[7];
    c->last_vector_to = c->pc;
}

static void raise(m68k *c, unsigned vector, uint32_t data)
{
    /* The short frames resume at the instruction that faulted, so the
     * program counter the caller left behind is also the one to record. */
    raise_at(c, vector, c->pc, data);
}

/*
 * The body of an instruction. m68k_execute wraps it to charge the cycles
 * afterwards, which is what the reference does and is not the same as
 * charging them first: a device read made by this instruction must see the
 * clock as it was before the instruction, or every span the Envoy's
 * battery RAM measures is off by the difference between the two
 * instructions at its ends.
 */
static bool execute_decoded(m68k *c, const m68k_insn *insn)
{
    uint32_t pc = c->pc;
    unsigned size = insn->size ? insn->size : 2;

    c->prev_pc = pc;
    c->exception = 0;
    c->pc = pc + insn->length;
    c->insn_count++;

    if ((insn->flags & M68K_F_PRIVILEGED) && !c->supervisor) {
        c->pc = pc;
        raise(c, M68K_VEC_PRIVILEGE, 0);
        return false;
    }

    switch (insn->op) {
    case M68K_OP_MOVE: {
        m68k_operand s = m68k_resolve(c, &insn->src, size);
        uint32_t value = m68k_read(c, &s, size);
        m68k_operand d = m68k_resolve(c, &insn->dst, size);
        m68k_write(c, &d, size, value);
        logic_flags(c, value, size);
        return true;
    }
    case M68K_OP_MOVEA: {
        m68k_operand s = m68k_resolve(c, &insn->src, size);
        uint32_t value = sign_extend(m68k_read(c, &s, size), size);
        c->a[insn->dst.reg] = value;
        return true;
    }
    case M68K_OP_MOVEQ:
        c->d[insn->dst.reg] = (uint32_t)insn->src.disp;
        logic_flags(c, (uint32_t)insn->src.disp, 4);
        return true;

    case M68K_OP_ADD: case M68K_OP_ADDI: case M68K_OP_ADDQ:
    case M68K_OP_SUB: case M68K_OP_SUBI: case M68K_OP_SUBQ:
    case M68K_OP_AND: case M68K_OP_ANDI:
    case M68K_OP_OR:  case M68K_OP_ORI:
    case M68K_OP_EOR: case M68K_OP_EORI: {
        /* A quick add or subtract on an address register moves the whole
         * register and says nothing about it. */
        if ((insn->op == M68K_OP_ADDQ || insn->op == M68K_OP_SUBQ) &&
            insn->dst.mode == M68K_AN) {
            uint32_t delta = (uint32_t)insn->src.disp;
            c->a[insn->dst.reg] += insn->op == M68K_OP_ADDQ ? delta : -delta;
            return true;
        }
        m68k_operand s = m68k_resolve(c, &insn->src, size);
        uint32_t b = m68k_read(c, &s, size);
        m68k_operand d = m68k_resolve(c, &insn->dst, size);
        uint32_t a = m68k_read(c, &d, size);
        uint32_t r;
        switch (insn->op) {
        case M68K_OP_ADD: case M68K_OP_ADDI: case M68K_OP_ADDQ:
            r = do_add(c, a, b, size, false); break;
        case M68K_OP_SUB: case M68K_OP_SUBI: case M68K_OP_SUBQ:
            r = do_sub(c, a, b, size, false, false); break;
        case M68K_OP_AND: case M68K_OP_ANDI:
            r = a & b; logic_flags(c, r, size); break;
        case M68K_OP_OR: case M68K_OP_ORI:
            r = a | b; logic_flags(c, r, size); break;
        default:
            r = a ^ b; logic_flags(c, r, size); break;
        }
        m68k_write(c, &d, size, r);
        return true;
    }
    case M68K_OP_ADDA: case M68K_OP_SUBA: {
        m68k_operand s = m68k_resolve(c, &insn->src, size);
        uint32_t b = sign_extend(m68k_read(c, &s, size), size);
        uint32_t a = c->a[insn->dst.reg];
        c->a[insn->dst.reg] = insn->op == M68K_OP_ADDA ? a + b : a - b;
        return true;
    }
    case M68K_OP_ADDX: case M68K_OP_SUBX: {
        m68k_operand s = m68k_resolve(c, &insn->src, size);
        uint32_t b = m68k_read(c, &s, size);
        m68k_operand d = m68k_resolve(c, &insn->dst, size);
        uint32_t a = m68k_read(c, &d, size);
        uint32_t r = insn->op == M68K_OP_ADDX ? do_add(c, a, b, size, true)
                                              : do_sub(c, a, b, size, true, false);
        m68k_write(c, &d, size, r);
        return true;
    }
    case M68K_OP_CMP: case M68K_OP_CMPI: case M68K_OP_CMPM: {
        m68k_operand s = m68k_resolve(c, &insn->src, size);
        uint32_t b = m68k_read(c, &s, size);
        m68k_operand d = m68k_resolve(c, &insn->dst, size);
        uint32_t a = m68k_read(c, &d, size);
        do_sub(c, a, b, size, false, true);   /* compares, and keeps X */
        return true;
    }
    case M68K_OP_CMPA: {
        m68k_operand s = m68k_resolve(c, &insn->src, size);
        uint32_t b = sign_extend(m68k_read(c, &s, size), size);
        do_sub(c, c->a[insn->dst.reg], b, 4, false, true);
        return true;
    }
    case M68K_OP_TST: {
        m68k_operand s = m68k_resolve(c, &insn->src, size);
        logic_flags(c, m68k_read(c, &s, size), size);
        return true;
    }
    case M68K_OP_CLR: {
        m68k_operand d = m68k_resolve(c, &insn->dst, size);
        /* The hardware reads before it writes, and a device can tell. */
        (void)m68k_read(c, &d, size);
        m68k_write(c, &d, size, 0);
        c->n = 0; c->z = 1; c->v = 0; c->c = 0;
        return true;
    }
    case M68K_OP_NOT: {
        m68k_operand d = m68k_resolve(c, &insn->dst, size);
        uint32_t r = ~m68k_read(c, &d, size) & mask_of(size);
        m68k_write(c, &d, size, r);
        logic_flags(c, r, size);
        return true;
    }
    case M68K_OP_NEG: case M68K_OP_NEGX: {
        m68k_operand d = m68k_resolve(c, &insn->dst, size);
        uint32_t a = m68k_read(c, &d, size);
        uint32_t r = do_sub(c, 0, a, size, insn->op == M68K_OP_NEGX, false);
        m68k_write(c, &d, size, r);
        return true;
    }
    case M68K_OP_EXT:
        c->d[insn->dst.reg] = size == 2
            ? (c->d[insn->dst.reg] & 0xFFFF0000u) | (sign_extend(c->d[insn->dst.reg], 1) & 0xFFFF)
            : sign_extend(c->d[insn->dst.reg], 2);
        logic_flags(c, c->d[insn->dst.reg], size);
        return true;
    case M68K_OP_EXTB:
        c->d[insn->dst.reg] = sign_extend(c->d[insn->dst.reg], 1);
        logic_flags(c, c->d[insn->dst.reg], 4);
        return true;
    case M68K_OP_SWAP: {
        uint32_t v = c->d[insn->dst.reg];
        v = (v >> 16) | (v << 16);
        c->d[insn->dst.reg] = v;
        logic_flags(c, v, 4);
        return true;
    }
    case M68K_OP_EXG: {
        uint32_t *s = insn->src.mode == M68K_AN ? &c->a[insn->src.reg]
                                                : &c->d[insn->src.reg];
        uint32_t *d = insn->dst.mode == M68K_AN ? &c->a[insn->dst.reg]
                                                : &c->d[insn->dst.reg];
        uint32_t t = *s; *s = *d; *d = t;
        return true;
    }
    case M68K_OP_LEA:
        c->a[insn->dst.reg] = m68k_resolve(c, &insn->src, 4).addr;
        return true;
    case M68K_OP_PEA:
        push32(c, m68k_resolve(c, &insn->src, 4).addr);
        return true;
    case M68K_OP_LINK: {
        /*
         * Written out rather than through the push helper, because linking
         * the stack pointer to itself is a real encoding and the order
         * decides what it stores: the register is decremented first, so
         * LINK A7 saves the lowered stack pointer, not the original.
         */
        uint32_t reg = insn->dst.reg;
        c->a[7] -= 4;
        c->bus.write(c->bus.ctx, c->a[7], 4, c->a[reg]);
        c->a[reg] = c->a[7];
        c->a[7] += insn->extra;
        return true;
    }
    case M68K_OP_UNLK: {
        uint32_t reg = insn->dst.reg;
        c->a[7] = c->a[reg];
        c->a[reg] = pop32(c);
        return true;
    }

    case M68K_OP_MOVEM:
        if (insn->dst.mode != M68K_NOEA) movem_to_memory(c, insn);
        else movem_to_registers(c, insn);
        return true;

    case M68K_OP_BTST: case M68K_OP_BCHG: case M68K_OP_BCLR: case M68K_OP_BSET: {
        m68k_operand s = m68k_resolve(c, &insn->src, 4);
        uint32_t bit = m68k_read(c, &s, 4);
        m68k_operand d = m68k_resolve(c, &insn->dst, size);
        bit &= insn->dst.mode == M68K_DN ? 31u : 7u;
        uint32_t value = m68k_read(c, &d, size);
        c->z = (value & (1u << bit)) == 0;
        if (insn->op == M68K_OP_BTST) return true;
        value = insn->op == M68K_OP_BCHG ? value ^ (1u << bit)
              : insn->op == M68K_OP_BCLR ? value & ~(1u << bit)
              : value | (1u << bit);
        m68k_write(c, &d, size, value);
        return true;
    }
    case M68K_OP_TAS: {
        m68k_operand d = m68k_resolve(c, &insn->dst, 1);
        uint32_t value = m68k_read(c, &d, 1);
        logic_flags(c, value, 1);
        m68k_write(c, &d, 1, value | 0x80u);
        return true;
    }

    case M68K_OP_ASL: case M68K_OP_ASR: case M68K_OP_LSL: case M68K_OP_LSR:
    case M68K_OP_ROL: case M68K_OP_ROR: case M68K_OP_ROXL: case M68K_OP_ROXR: {
        if (insn->dst.mode != M68K_DN) {          /* the memory form, by one */
            m68k_operand d = m68k_resolve(c, &insn->dst, 2);
            uint32_t r = do_shift(c, insn->op, m68k_read(c, &d, 2), 1, 2);
            m68k_write(c, &d, 2, r);
            return true;
        }
        unsigned count = insn->src.mode == M68K_IMM
            ? (unsigned)insn->src.disp : (c->d[insn->src.reg] & 63);
        /* A count held in a register costs one per shift; the table holds
         * the cost of shifting once. An immediate count is flat, however
         * large, which is the reference's model and not obviously the
         * hardware's. */
        if (insn->src.mode != M68K_IMM) c->cycles += count - 1;
        uint32_t r = do_shift(c, insn->op, c->d[insn->dst.reg], count, size);
        c->d[insn->dst.reg] =
            (c->d[insn->dst.reg] & ~mask_of(size)) | (r & mask_of(size));
        return true;
    }

    case M68K_OP_MULU: case M68K_OP_MULS: {
        m68k_operand s = m68k_resolve(c, &insn->src, 2);
        uint32_t b = m68k_read(c, &s, 2);
        uint32_t a = c->d[insn->dst.reg] & 0xFFFF;
        uint32_t r = insn->op == M68K_OP_MULU
            ? a * b
            : (uint32_t)((int32_t)(int16_t)a * (int32_t)(int16_t)b);
        c->d[insn->dst.reg] = r;
        logic_flags(c, r, 4);
        return true;
    }
    case M68K_OP_DIVU: case M68K_OP_DIVS: {
        m68k_operand s = m68k_resolve(c, &insn->src, 2);
        uint32_t divisor = m68k_read(c, &s, 2);
        uint32_t dividend = c->d[insn->dst.reg];
        if (!divisor) {
            raise_at(c, M68K_VEC_DIVIDE_BY_ZERO, pc, dividend);
            return false;
        }
        if (insn->op == M68K_OP_DIVU) {
            uint32_t quotient = dividend / divisor;
            /* A quotient too wide for the destination half leaves the
             * register alone and reports the overflow. The architecture
             * calls the other flags undefined there, and the reference
             * leaves them as they were, so this does too. */
            if (quotient > 0xFFFF) { c->v = 1; return true; }
            uint32_t remainder = dividend % divisor;
            c->d[insn->dst.reg] = (remainder << 16) | (quotient & 0xFFFF);
            logic_flags(c, quotient, 2);
        } else {
            int32_t b = (int32_t)(int16_t)divisor;
            /*
             * The most negative dividend over minus one has no positive
             * counterpart, and dividing it in C is undefined rather than
             * merely wrong: on some hosts it raises. Answer it directly,
             * as the reference does.
             */
            if (dividend == 0x80000000u && b == -1) {
                c->d[insn->dst.reg] = 0;
                c->n = 0; c->z = 0; c->v = 0; c->c = 0;
                return true;
            }
            int32_t a = (int32_t)dividend;
            int32_t quotient = a / b;
            if (quotient > 32767 || quotient < -32768) { c->v = 1; return true; }
            int32_t remainder = a % b;
            c->d[insn->dst.reg] =
                ((uint32_t)remainder << 16) | ((uint32_t)quotient & 0xFFFF);
            logic_flags(c, (uint32_t)quotient, 2);
        }
        return true;
    }

    case M68K_OP_ABCD: case M68K_OP_SBCD: {
        m68k_operand s = m68k_resolve(c, &insn->src, 1);
        uint32_t b = m68k_read(c, &s, 1);
        m68k_operand d = m68k_resolve(c, &insn->dst, 1);
        uint32_t a = m68k_read(c, &d, 1);
        m68k_write(c, &d, 1, do_bcd(c, b, a, insn->op == M68K_OP_ABCD));
        return true;
    }
    case M68K_OP_NBCD: {
        /* Negation is a subtraction from zero, in decimal as in binary. */
        m68k_operand d = m68k_resolve(c, &insn->dst, 1);
        uint32_t a = m68k_read(c, &d, 1);
        m68k_write(c, &d, 1, do_bcd(c, a, 0, false));
        return true;
    }

    case M68K_OP_MULL: {
        /*
         * The extension word says whether the operands are signed and
         * whether the product is kept whole. A 32-bit product reports
         * overflow when the other half was not empty; a 64-bit one has
         * nowhere to overflow to.
         */
        uint16_t ext = (uint16_t)insn->extra;
        unsigned dl = (ext >> 12) & 7, dh = ext & 7;
        bool wide = (ext & 0x400) != 0, is_signed = (ext & 0x800) != 0;
        m68k_operand s = m68k_resolve(c, &insn->src, 4);
        uint32_t b = m68k_read(c, &s, 4);
        uint64_t r = is_signed
            ? (uint64_t)((int64_t)(int32_t)b * (int64_t)(int32_t)c->d[dl])
            : (uint64_t)b * (uint64_t)c->d[dl];
        if (wide) {
            c->n = (r >> 63) & 1; c->z = r == 0; c->v = 0; c->c = 0;
            c->d[dh] = (uint32_t)(r >> 32);
            c->d[dl] = (uint32_t)r;
        } else {
            c->n = (r >> 31) & 1; c->z = (uint32_t)r == 0; c->c = 0;
            c->v = is_signed ? r != (uint64_t)(int64_t)(int32_t)r
                             : (r >> 32) != 0;
            c->d[dl] = (uint32_t)r;
        }
        return true;
    }
    case M68K_OP_DIVL: {
        uint16_t ext = (uint16_t)insn->extra;
        unsigned dl = (ext >> 12) & 7, dh = ext & 7;
        bool wide = (ext & 0x400) != 0, is_signed = (ext & 0x800) != 0;
        m68k_operand s = m68k_resolve(c, &insn->src, 4);
        uint32_t divisor = m68k_read(c, &s, 4);
        if (!divisor) {
            raise_at(c, M68K_VEC_DIVIDE_BY_ZERO, pc, 0);
            return false;
        }
        if (is_signed) {
            int64_t dividend = wide
                ? (int64_t)(((uint64_t)c->d[dh] << 32) | c->d[dl])
                : (int64_t)(int32_t)c->d[dl];
            int64_t divs = (int64_t)(int32_t)divisor;
            /* The one division C leaves undefined rather than merely
             * wrong, answered before it can raise on the host. */
            if (dividend == INT64_MIN && divs == -1) { c->v = 1; return true; }
            int64_t quotient = dividend / divs;
            if (wide && (quotient > INT32_MAX || quotient < INT32_MIN)) {
                c->v = 1;
                return true;
            }
            int64_t remainder = dividend % divs;
            c->n = ((uint64_t)quotient >> 31) & 1;
            c->z = (uint32_t)quotient == 0; c->v = 0; c->c = 0;
            c->d[dh] = (uint32_t)remainder;
            c->d[dl] = (uint32_t)quotient;
        } else {
            uint64_t dividend = wide
                ? ((uint64_t)c->d[dh] << 32) | c->d[dl] : c->d[dl];
            uint64_t quotient = dividend / divisor;
            if (wide && quotient > 0xFFFFFFFFu) { c->v = 1; return true; }
            uint64_t remainder = dividend % divisor;
            c->n = (quotient >> 31) & 1;
            c->z = (uint32_t)quotient == 0; c->v = 0; c->c = 0;
            c->d[dh] = (uint32_t)remainder;
            c->d[dl] = (uint32_t)quotient;
        }
        return true;
    }

    case M68K_OP_MOVEP: {
        /*
         * Alternate bytes, for a peripheral wired to one half of the data
         * bus. The direction lives in the opcode rather than in the
         * operand order, which is why this does not go through the
         * ordinary operand path.
         */
        uint32_t addr = c->a[insn->src.reg] + (uint32_t)insn->src.disp;
        unsigned reg = insn->dst.reg;
        unsigned count = size == 4 ? 4 : 2;
        if (insn->word & 0x80) {
            for (unsigned k = 0; k < count; k++)
                c->bus.write(c->bus.ctx, addr + k * 2, 1,
                             (c->d[reg] >> ((count - 1 - k) * 8)) & 0xFF);
        } else {
            uint32_t v = 0;
            for (unsigned k = 0; k < count; k++)
                v = v << 8 | (c->bus.read(c->bus.ctx, addr + k * 2, 1) & 0xFF);
            c->d[reg] = (c->d[reg] & ~mask_of(size)) | (v & mask_of(size));
        }
        return true;
    }

    case M68K_OP_TRAPCC:
        /* The false condition is the whole point of the common use: it
         * skips its own operand words and costs nothing. */
        if (!m68k_condition(c, insn->cond)) return true;
        raise_at(c, M68K_VEC_TRAPV, pc, 0);
        return false;

    case M68K_OP_MOVEC: {
        uint16_t ext = (uint16_t)insn->extra;
        unsigned reg = (ext >> 12) & 15;
        uint32_t *general = reg < 8 ? &c->d[reg] : &c->a[reg - 8];
        uint32_t *control;
        switch (ext & 0xFFF) {
        case 0x000: control = &c->sfc; break;
        case 0x001: control = &c->dfc; break;
        case 0x800: control = &c->usp; break;
        case 0x801: control = &c->vbr; break;
        default:
            /* A control register this part does not have. */
            c->pc = pc;
            raise(c, M68K_VEC_ILLEGAL, insn->word);
            return false;
        }
        if (insn->word & 1) *control = *general;
        else *general = *control;
        return true;
    }
    case M68K_OP_MOVES: {
        /*
         * The decoder has already put the register and the memory operand
         * in the order the direction bit asked for, so this is an ordinary
         * move but for the space it reaches. Whichever end is memory is
         * addressed through a function code -- DFC when memory is the
         * destination, SFC when it is the source -- and `fc` carries that
         * beside the access for the owner of the bus to act on. On this
         * board the code that matters is 7, CPU space, which is how the
         * reset path reaches the MBAR.
         */
        /*
         * The destination is resolved before the source is read, which
         * matters in exactly one case and matters absolutely there:
         * storing an address register through its own pre-decrement, where
         * the value written is the one the register holds after the
         * decrement rather than before it.
         */
        m68k_operand d = m68k_resolve(c, &insn->dst, size);
        m68k_operand s = m68k_resolve(c, &insn->src, size);
        /*
         * Only the memory end goes through a function code, and only for
         * its own access: resolving an operand can read an extension word,
         * which is an ordinary fetch and must not be made in another
         * space. So it is set immediately before and cleared immediately
         * after, on each side separately.
         */
        c->fc = s.kind == M68K_OPERAND_MEM ? (uint8_t)c->sfc : 0;
        uint32_t value = m68k_read(c, &s, size);
        c->fc = 0;
        /* A register destination takes the whole value, sign-extended into
         * an address register, and is not a flag-setting move. */
        if (d.kind == M68K_OPERAND_AREG) c->a[d.reg] = sign_extend(value, size);
        else {
            c->fc = d.kind == M68K_OPERAND_MEM ? (uint8_t)c->dfc : 0;
            m68k_write(c, &d, size, value);
            c->fc = 0;
        }
        return true;
    }

    case M68K_OP_BRA:
        c->pc = insn->extra;
        return true;
    case M68K_OP_BSR:
        push32(c, c->pc);
        c->pc = insn->extra;
        return true;
    case M68K_OP_BCC:
        if (m68k_condition(c, insn->cond)) {
            c->pc = insn->extra;
            /* Taking a byte-displacement branch costs two more than not
             * taking it; the word and long forms cost the same either
             * way. Measured on the reference, like the table. */
            if (insn->length == 2) c->cycles += 2;
        }
        return true;
    case M68K_OP_DBCC:
        if (m68k_condition(c, insn->cond)) return true;
        {
            /* Only the low word counts down, and it is the value after the
             * decrement that decides, so a register holding zero goes round
             * 65,536 more times rather than none. */
            uint32_t reg = insn->dst.reg;
            uint16_t counter = (uint16_t)(c->d[reg] - 1);
            c->d[reg] = (c->d[reg] & 0xFFFF0000u) | counter;
            if (counter != 0xFFFF) c->pc = insn->extra;
            /* Going round again is the cheap case; the loop ending by the
             * counter running out costs four more. */
            else c->cycles += 4;
        }
        return true;
    case M68K_OP_SCC: {
        m68k_operand d = m68k_resolve(c, &insn->dst, 1);
        m68k_write(c, &d, 1, m68k_condition(c, insn->cond) ? 0xFFu : 0u);
        return true;
    }
    case M68K_OP_JMP:
        c->pc = m68k_resolve(c, &insn->src, 4).addr;
        return true;
    case M68K_OP_JSR: {
        uint32_t target = m68k_resolve(c, &insn->src, 4).addr;
        push32(c, c->pc);
        c->pc = target;
        return true;
    }
    case M68K_OP_RTS:
        c->pc = pop32(c);
        return true;
    case M68K_OP_RTE: {
        /*
         * Everything comes off the supervisor stack before the status
         * register is restored, because restoring it can put that stack
         * away and hand A7 to the user one.
         */
        uint16_t sr = (uint16_t)c->bus.read(c->bus.ctx, c->a[7], 2);
        uint32_t target = c->bus.read(c->bus.ctx, c->a[7] + 2, 4);
        uint16_t format = (uint16_t)c->bus.read(c->bus.ctx, c->a[7] + 6, 2);
        unsigned extra_bytes;
        switch (format >> 12) {
        case 0x0: extra_bytes = 0; break;   /* four words                  */
        case 0x1: extra_bytes = 0; break;   /* throwaway                   */
        case 0x2: extra_bytes = 4; break;   /* six words, with the address */
        case 0xC: extra_bytes = 16; break;  /* this part's bus-fault frame */
        default:
            /* A frame this part does not build and cannot unwind. */
            raise(c, 14, format);
            return false;
        }
        c->a[7] += 8 + extra_bytes;
        m68k_set_sr(c, sr);
        c->pc = target;
        return true;
    }
    case M68K_OP_LPSTOP:
        /*
         * Load a status register and stop until an interrupt it admits.
         * The generic privilege check above has already caught being in
         * user mode; what is left is this instruction's own rule, that a
         * status register clearing the supervisor bit is refused the same
         * way, because stopping in user mode is not a state the part can
         * be left in. The M68000PRM gives the vector as privilege
         * violation for both.
         */
        if (!(insn->extra & 0x2000)) {
            c->pc = pc;
            raise(c, M68K_VEC_PRIVILEGE, 0);
            return false;
        }
        m68k_set_sr(c, (uint16_t)insn->extra);
        c->stopped = true;
        return true;
    case M68K_OP_RTD:
        c->pc = pop32(c);
        c->a[7] += insn->extra;
        return true;
    case M68K_OP_RTR: {
        uint16_t ccr = (uint16_t)c->bus.read(c->bus.ctx, c->a[7], 2);
        c->a[7] += 2;
        m68k_set_ccr(c, (uint8_t)(ccr & 0x1F));
        c->pc = pop32(c);
        return true;
    }

    case M68K_OP_NOP:
        return true;
    case M68K_OP_ORICCR: case M68K_OP_ANDICCR: case M68K_OP_EORICCR: {
        uint8_t ccr = m68k_get_ccr(c), imm = (uint8_t)insn->src.disp;
        m68k_set_ccr(c, (uint8_t)(insn->op == M68K_OP_ORICCR ? (ccr | imm)
                                : insn->op == M68K_OP_ANDICCR ? (ccr & imm)
                                : (ccr ^ imm)));
        return true;
    }
    case M68K_OP_ORISR: case M68K_OP_ANDISR: case M68K_OP_EORISR: {
        uint16_t sr = m68k_get_sr(c), imm = (uint16_t)insn->src.disp;
        m68k_set_sr(c, (uint16_t)(insn->op == M68K_OP_ORISR ? (sr | imm)
                                : insn->op == M68K_OP_ANDISR ? (sr & imm)
                                : (sr ^ imm)));
        return true;
    }
    case M68K_OP_MOVEFSR: {
        m68k_operand d = m68k_resolve(c, &insn->dst, 2);
        m68k_write(c, &d, 2, m68k_get_sr(c));
        return true;
    }
    case M68K_OP_MOVEFCCR: {
        m68k_operand d = m68k_resolve(c, &insn->dst, 2);
        m68k_write(c, &d, 2, m68k_get_ccr(c));
        return true;
    }
    case M68K_OP_MOVETCCR: {
        m68k_operand s = m68k_resolve(c, &insn->src, 2);
        m68k_set_ccr(c, (uint8_t)(m68k_read(c, &s, 2) & 0x1F));
        return true;
    }
    case M68K_OP_MOVETSR: {
        m68k_operand s = m68k_resolve(c, &insn->src, 2);
        m68k_set_sr(c, (uint16_t)m68k_read(c, &s, 2));
        return true;
    }
    case M68K_OP_MOVEUSP:
        if (insn->word & 8) c->a[insn->dst.reg] = c->usp;
        else c->usp = c->a[insn->dst.reg];
        return true;

    case M68K_OP_TRAP:
        /* A trap resumes after itself, and its vector is a short frame. */
        raise(c, M68K_VEC_TRAP0 + insn->extra, 0);
        return false;
    case M68K_OP_TRAPV:
        if (!c->v) return true;
        raise_at(c, M68K_VEC_TRAPV, pc, 0);
        return false;
    case M68K_OP_CHK: {
        m68k_operand s = m68k_resolve(c, &insn->src, size);
        int32_t bound = (int32_t)sign_extend(m68k_read(c, &s, size), size);
        int32_t value = (int32_t)sign_extend(c->d[insn->dst.reg], size);
        if (value >= 0 && value <= bound) { c->n = 0; return true; }
        c->n = value < 0;
        raise_at(c, M68K_VEC_CHK, pc, (uint32_t)value);
        return false;
    }
    case M68K_OP_ILLEGAL:
        c->pc = pc;
        raise(c, M68K_VEC_ILLEGAL, insn->word);
        return false;
    case M68K_OP_LINE_A:
        c->pc = pc;
        raise(c, M68K_VEC_LINE_A, insn->word);
        return false;
    case M68K_OP_LINE_F:
        c->pc = pc;
        raise(c, M68K_VEC_LINE_F, insn->word);
        return false;

    default:
        /* Not implemented yet. Say so rather than run something else: an
         * instruction quietly treated as a no-operation is the one bug
         * this core must never have. */
        c->pc = pc;
        raise(c, M68K_VEC_ILLEGAL, insn->word);
        return false;
    }
}

bool m68k_execute(m68k *c, const m68k_insn *insn)
{
    bool ok = execute_decoded(c, insn);
    /*
     * Everything the encoding decides. What an operand decides -- a branch
     * taken, a loop ending, a shift count in a register -- has already
     * been added above, and since none of those touches a device, adding
     * it early is invisible.
     */
    c->cycles += insn->cycles;
    return ok;
}

bool m68k_interrupt(m68k *c, unsigned level, unsigned vector)
{
    if (level == 0) return false;
    if (level <= c->interrupt_mask && level != 7) return false;
    /* The automatic vectors sit one per level above vector 24. */
    raise_at(c, vector ? vector : 24 + level, c->pc, level);
    /* An interrupt raises the mask to its own level, so the handler runs
     * without being interrupted again by its equal. */
    c->interrupt_mask = (uint8_t)level;
    return true;
}

/*
 * An instruction fetch from an odd address. The MC68349 faults after the
 * branch that reached it, with the twenty-four byte frame this part uses
 * for faults, which is not one of the shapes a 68020 builds. Recorded in
 * docs/HARDWARE.md as a property of the part rather than of any ROM.
 */
void m68k_address_error(m68k *c, uint32_t pc, uint32_t instruction_pc)
{
    uint16_t sr = m68k_get_sr(c);
    c->exception = M68K_VEC_ADDRESS_ERROR;
    c->exception_count++;
    c->vector_count[M68K_VEC_ADDRESS_ERROR]++;
    c->last_vector = M68K_VEC_ADDRESS_ERROR;
    c->last_vector_pc = instruction_pc;
    c->exception_data = pc;
    if (!c->supervisor) {
        c->usp = c->a[7];
        c->a[7] = c->ssp;
        c->supervisor = true;
    }
    c->trace = false;
    uint32_t sp = c->a[7] - 24;
    c->a[7] = sp;
    c->bus.write(c->bus.ctx, sp, 2, sr);
    c->bus.write(c->bus.ctx, sp + 2, 4, pc);
    c->bus.write(c->bus.ctx, sp + 6, 2, 0xC00Cu);
    c->bus.write(c->bus.ctx, sp + 8, 4, pc);
    c->bus.write(c->bus.ctx, sp + 12, 4, 0);
    /* The instruction, not the address it reached: a handler that wants
     * to know what went wrong wants the branch, and the address it landed
     * on is already at +2 and +8. */
    c->bus.write(c->bus.ctx, sp + 16, 4, instruction_pc);
    c->bus.write(c->bus.ctx, sp + 20, 2, 0);
    c->bus.write(c->bus.ctx, sp + 22, 2,
                 (uint16_t)(0x20D0u | ((sr & 0x2000) ? 6u : 2u)));
    c->pc = c->bus.read(c->bus.ctx, c->vbr + M68K_VEC_ADDRESS_ERROR * 4, 4);
    c->last_vector_sp = c->a[7];
    c->last_vector_to = c->pc;
}

bool m68k_implemented(unsigned op)
{
    switch (op) {
    case M68K_OP_MOVE: case M68K_OP_MOVEA: case M68K_OP_MOVEQ:
    case M68K_OP_ADD: case M68K_OP_ADDA: case M68K_OP_ADDI: case M68K_OP_ADDQ:
    case M68K_OP_ADDX:
    case M68K_OP_SUB: case M68K_OP_SUBA: case M68K_OP_SUBI: case M68K_OP_SUBQ:
    case M68K_OP_SUBX:
    case M68K_OP_AND: case M68K_OP_ANDI: case M68K_OP_OR: case M68K_OP_ORI:
    case M68K_OP_EOR: case M68K_OP_EORI:
    case M68K_OP_CMP: case M68K_OP_CMPA: case M68K_OP_CMPI: case M68K_OP_CMPM:
    case M68K_OP_TST: case M68K_OP_CLR: case M68K_OP_NOT: case M68K_OP_NEG:
    case M68K_OP_NEGX: case M68K_OP_EXT: case M68K_OP_EXTB: case M68K_OP_SWAP:
    case M68K_OP_EXG: case M68K_OP_LEA: case M68K_OP_PEA:
    case M68K_OP_LINK: case M68K_OP_UNLK: case M68K_OP_MOVEM:
    case M68K_OP_BTST: case M68K_OP_BCHG: case M68K_OP_BCLR: case M68K_OP_BSET:
    case M68K_OP_TAS:
    case M68K_OP_ASL: case M68K_OP_ASR: case M68K_OP_LSL: case M68K_OP_LSR:
    case M68K_OP_ROL: case M68K_OP_ROR: case M68K_OP_ROXL: case M68K_OP_ROXR:
    case M68K_OP_MULU: case M68K_OP_MULS: case M68K_OP_DIVU: case M68K_OP_DIVS:
    case M68K_OP_MULL: case M68K_OP_DIVL:
    case M68K_OP_ABCD: case M68K_OP_SBCD: case M68K_OP_NBCD:
    case M68K_OP_MOVEP: case M68K_OP_TRAPCC:
    case M68K_OP_MOVEC: case M68K_OP_MOVES:
    case M68K_OP_BRA: case M68K_OP_BSR: case M68K_OP_BCC: case M68K_OP_DBCC:
    case M68K_OP_SCC: case M68K_OP_JMP: case M68K_OP_JSR: case M68K_OP_RTS:
    case M68K_OP_RTD: case M68K_OP_RTR: case M68K_OP_RTE: case M68K_OP_NOP:
    case M68K_OP_ORICCR: case M68K_OP_ANDICCR: case M68K_OP_EORICCR:
    case M68K_OP_ORISR: case M68K_OP_ANDISR: case M68K_OP_EORISR:
    case M68K_OP_MOVEFCCR: case M68K_OP_MOVETCCR:
    case M68K_OP_MOVEFSR: case M68K_OP_MOVETSR: case M68K_OP_MOVEUSP:
    case M68K_OP_LPSTOP:
    /* These complete by raising, which is running them, not failing at
     * them: the caller is told which vector and why. */
    case M68K_OP_TRAP: case M68K_OP_TRAPV: case M68K_OP_CHK:
    case M68K_OP_ILLEGAL: case M68K_OP_LINE_A: case M68K_OP_LINE_F:
        return true;
    default:
        return false;
    }
}

bool m68k_step(m68k *c)
{
    if (c->pc & 1) { m68k_address_error(c, c->pc, c->prev_pc); return false; }
    /*
     * Fetch a word at a time until the instruction is whole. Reading a
     * fixed block instead would be simpler and wrong: the words past the
     * end of a short instruction can be device registers, and reading a
     * device register has consequences whether or not the value is used.
     */
    uint8_t code[MRC_JIT_M68K_MAX_BYTES];
    m68k_insn insn;
    unsigned have = 0;
    do {
        uint32_t w = c->bus.read(c->bus.ctx, c->pc + have, 2);
        code[have] = (uint8_t)(w >> 8);
        code[have + 1] = (uint8_t)w;
        have += 2;
        if (m68k_decode(code, have, c->pc, &insn) && insn.length <= have)
            return m68k_execute(c, &insn);
    } while (have < sizeof code);
    raise(c, M68K_VEC_ILLEGAL, 0);
    return false;
}
