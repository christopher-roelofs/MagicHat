#include "jit/jit.h"
/* System V calling convention; the Windows x64 ABI is not implemented. */
#if defined(__x86_64__) && !defined(_WIN32)
#include <stddef.h>
#include <string.h>
/*
 * x86-64 System V backend.
 *
 * Register use inside a block:
 *   rbx  r3900 *c            r13  read page table    r15  mrc_bus *
 *   r12d branch destination  r14  write page table   ebp  remaining budget
 *   [rsp] r3900_jit *        esi, edi, r8d-r11d: guest register cache
 *   eax, ecx, edx: scratch
 * The register cache works as on AArch64: the first six guest registers a
 * block touches stay in host registers, are written back at every exit and
 * before every helper call, and nothing is assumed cached after a helper.
 */
enum { RAX, RCX, RDX, RBX, RSP, RBP, RSI, RDI, R8, R9, R10, R11, R12, R13, R14, R15 };
enum { CC_B = 2, CC_E = 4, CC_NE = 5, CC_L = 0xC, CC_GE = 0xD, CC_LE = 0xE, CC_G = 0xF };

#ifndef CACHE_SLOTS
#define CACHE_SLOTS 6
#endif
static const uint8_t cache_regs[CACHE_SLOTS] = {RSI, RDI, R8, R9, R10, R11};
typedef struct {
    int8_t host[32];
    bool dirty[32];
    uint8_t guest[CACHE_SLOTS];
} regcache;

typedef struct {
    uint8_t *out, *p, *end;
    bool overflow;
    unsigned n;                                  /* block length */
    regcache rc;
    unsigned fail_fixups[MRC_JIT_MAX_INSNS + 1], nfail;
    unsigned done_fixups[MRC_JIT_MAX_INSNS * 4], ndone;
    struct { unsigned site, stub; } links[3];    /* rel32 field, stub jump */
    unsigned nlinks;
} emitter;

static void byte(emitter *e, unsigned b)
{
    if (e->p < e->end) *e->p++ = (uint8_t)b;
    else e->overflow = true;
}
static void bytes(emitter *e, const void *v, size_t n)
{
    for (size_t k = 0; k < n; k++) byte(e, ((const uint8_t *)v)[k]);
}
static void imm32(emitter *e, uint32_t v) { bytes(e, &v, 4); }
static void imm64(emitter *e, uint64_t v) { bytes(e, &v, 8); }
static unsigned here(const emitter *e) { return (unsigned)(e->p - e->out); }

static void rex(emitter *e, bool w, unsigned reg, unsigned index, unsigned base)
{
    unsigned r = 0x40 | (w << 3) | ((reg >> 3) << 2) | ((index >> 3) << 1) | (base >> 3);
    if (r != 0x40) byte(e, r);
}
/* ModRM (+SIB) for [base + disp]. */
static void mem(emitter *e, unsigned reg, unsigned base, int32_t disp)
{
    unsigned mod = disp == 0 && (base & 7) != RBP ? 0 : disp >= -128 && disp < 128 ? 1 : 2;
    byte(e, (mod << 6) | ((reg & 7) << 3) | (base & 7));
    if ((base & 7) == RSP) byte(e, 0x24);
    if (mod == 1) byte(e, (uint8_t)disp);
    else if (mod == 2) imm32(e, (uint32_t)disp);
}
/* ModRM + SIB for [base + index * scale]. */
static void mem_index(emitter *e, unsigned reg, unsigned base, unsigned index, unsigned scale_log)
{
    /* rbp/r13 as a base need mod=01 with a zero displacement. */
    bool disp = (base & 7) == RBP;
    byte(e, (disp ? 0x40 : 0) | ((reg & 7) << 3) | 4);
    byte(e, (scale_log << 6) | ((index & 7) << 3) | (base & 7));
    if (disp) byte(e, 0);
}
static void modrm_reg(emitter *e, unsigned reg, unsigned rm)
{
    byte(e, 0xC0 | ((reg & 7) << 3) | (rm & 7));
}

/* Guest register slot on the r3900 structure. */
#define GPR(n) ((int32_t)offsetof(r3900, r) + (int32_t)(n) * 4)

static void load_mem32(emitter *e, unsigned r, unsigned base, int32_t disp)
{
    rex(e, false, r, 0, base); byte(e, 0x8B); mem(e, r, base, disp);
}
static void store_mem32(emitter *e, unsigned base, int32_t disp, unsigned r)
{
    rex(e, false, r, 0, base); byte(e, 0x89); mem(e, r, base, disp);
}
static void store_imm32(emitter *e, unsigned base, int32_t disp, uint32_t v)
{
    rex(e, false, 0, 0, base); byte(e, 0xC7); mem(e, 0, base, disp); imm32(e, v);
}
static void store_imm8(emitter *e, unsigned base, int32_t disp, unsigned v)
{
    rex(e, false, 0, 0, base); byte(e, 0xC6); mem(e, 0, base, disp); byte(e, v);
}
static void load_mem64(emitter *e, unsigned r, unsigned base, int32_t disp)
{
    rex(e, true, r, 0, base); byte(e, 0x8B); mem(e, r, base, disp);
}
static void mov_rr(emitter *e, unsigned dst, unsigned src)
{
    rex(e, false, src, 0, dst); byte(e, 0x89); modrm_reg(e, src, dst);
}
static void mov_rr64(emitter *e, unsigned dst, unsigned src)
{
    rex(e, true, src, 0, dst); byte(e, 0x89); modrm_reg(e, src, dst);
}
static void mov_imm(emitter *e, unsigned r, uint32_t v)
{
    if (!v) { rex(e, false, r, 0, r); byte(e, 0x31); modrm_reg(e, r, r); return; }
    rex(e, false, 0, 0, r); byte(e, 0xB8 | (r & 7)); imm32(e, v);
}
static void mov_imm64(emitter *e, unsigned r, uint64_t v)
{
    rex(e, true, 0, 0, r); byte(e, 0xB8 | (r & 7)); imm64(e, v);
}
/* op r32, imm32 using the 81 /n family (imm8 form when it fits). */
static void alu_ri(emitter *e, unsigned n, unsigned r, uint32_t v)
{
    rex(e, false, 0, 0, r);
    if ((int32_t)v >= -128 && (int32_t)v < 128) {
        byte(e, 0x83); modrm_reg(e, n, r); byte(e, v & 255);
    } else {
        byte(e, 0x81); modrm_reg(e, n, r); imm32(e, v);
    }
}
static void alu_mi(emitter *e, unsigned n, unsigned base, int32_t disp, uint32_t v)
{
    rex(e, false, 0, 0, base);
    if ((int32_t)v >= -128 && (int32_t)v < 128) {
        byte(e, 0x83); mem(e, n, base, disp); byte(e, v & 255);
    } else {
        byte(e, 0x81); mem(e, n, base, disp); imm32(e, v);
    }
}
enum { ALU_ADD = 0, ALU_OR = 1, ALU_AND = 4, ALU_SUB = 5, ALU_XOR = 6, ALU_CMP = 7 };
static void shift_imm(emitter *e, unsigned n, unsigned r, unsigned count)
{
    rex(e, false, 0, 0, r); byte(e, 0xC1); modrm_reg(e, n, r); byte(e, count);
}
static void shift_cl(emitter *e, unsigned n, unsigned r)
{
    rex(e, false, 0, 0, r); byte(e, 0xD3); modrm_reg(e, n, r);
}
enum { SH_SHL = 4, SH_SHR = 5, SH_SAR = 7 };
static void setcc_movzx(emitter *e, unsigned cc, unsigned r)
{
    rex(e, false, 0, 0, r); byte(e, 0x0F); byte(e, 0x90 | cc); modrm_reg(e, 0, r);
    rex(e, false, r, 0, r); byte(e, 0x0F); byte(e, 0xB6); modrm_reg(e, r, r);
}
static void cmov(emitter *e, unsigned cc, unsigned dst, unsigned src)
{
    rex(e, false, dst, 0, src); byte(e, 0x0F); byte(e, 0x40 | cc); modrm_reg(e, dst, src);
}
static void jcc32(emitter *e, unsigned cc, unsigned *fixup)
{
    byte(e, 0x0F); byte(e, 0x80 | cc); *fixup = here(e); imm32(e, 0);
}
static void jmp32(emitter *e, unsigned *fixup)
{
    byte(e, 0xE9); *fixup = here(e); imm32(e, 0);
}
static void patch(emitter *e, unsigned fixup, unsigned target)
{
    if (fixup + 4 > (unsigned)(e->end - e->out)) return;
    int32_t rel = (int32_t)target - (int32_t)(fixup + 4);
    memcpy(e->out + fixup, &rel, 4);
}
static void call_abs(emitter *e, const void *fn)
{
    mov_imm64(e, RAX, (uint64_t)(uintptr_t)fn);
    byte(e, 0xFF); byte(e, 0xD0);
}
static void bswap(emitter *e, unsigned r)
{
    rex(e, false, 0, 0, r); byte(e, 0x0F); byte(e, 0xC8 | (r & 7));
}
static void ror16_8(emitter *e, unsigned r)
{
    byte(e, 0x66); rex(e, false, 0, 0, r); byte(e, 0xC1); modrm_reg(e, 1, r); byte(e, 8);
}
static void add_counter64(emitter *e, unsigned base, int32_t disp, unsigned v)
{
    rex(e, true, 0, 0, base); byte(e, 0x83); mem(e, 0, base, disp); byte(e, v);
}
static void inc_counter64(emitter *e, unsigned base, int32_t disp)
{
    add_counter64(e, base, disp, 1);
}

/* ---------------------------------------------------- register cache */

static void rc_reset(regcache *rc)
{
    memset(rc->host, -1, sizeof(rc->host));
    memset(rc->dirty, 0, sizeof(rc->dirty));
    memset(rc->guest, 32, sizeof(rc->guest));
}
static int rc_alloc(emitter *e, unsigned guest)
{
    for (unsigned k = 0; k < CACHE_SLOTS; k++)
        if (e->rc.guest[k] == 32) {
            e->rc.guest[k] = (uint8_t)guest;
            e->rc.host[guest] = (int8_t)cache_regs[k];
            return cache_regs[k];
        }
    return -1;
}
/* Host register holding guest `guest` for reading; $zero and uncached
 * registers when the cache is full are materialized in `scratch`. */
static unsigned rc_read(emitter *e, unsigned guest, unsigned scratch)
{
    if (!guest) { mov_imm(e, scratch, 0); return scratch; }
    if (e->rc.host[guest] >= 0) return (unsigned)e->rc.host[guest];
    int h = rc_alloc(e, guest);
    if (h < 0) h = (int)scratch;
    load_mem32(e, (unsigned)h, RBX, GPR(guest));
    return (unsigned)h;
}
static unsigned rc_write(emitter *e, unsigned guest, unsigned scratch)
{
    if (!guest) return scratch;
    if (e->rc.host[guest] < 0 && rc_alloc(e, guest) < 0) return scratch;
    e->rc.dirty[guest] = true;
    return (unsigned)e->rc.host[guest];
}
static void rc_written(emitter *e, unsigned guest, unsigned host)
{
    if (guest && (int)host != e->rc.host[guest]) store_mem32(e, RBX, GPR(guest), host);
}
static void rc_writeback(emitter *e)
{
    for (unsigned g = 1; g < 32; g++)
        if (e->rc.dirty[g]) {
            store_mem32(e, RBX, GPR(g), (unsigned)e->rc.host[g]);
            e->rc.dirty[g] = false;
        }
}
static void rc_invalidate(emitter *e) { rc_reset(&e->rc); }
static void rc_restore(emitter *e, const regcache *state)
{
    for (unsigned g = 1; g < 32; g++)
        if (state->host[g] >= 0) load_mem32(e, (unsigned)state->host[g], RBX, GPR(g));
    e->rc = *state;
}

/* ------------------------------------------------------------ pieces */

/* op r32, r32 in the 01/29/21/09/31/39 family (dst op= src). */
static void alu_rr(emitter *e, unsigned n, unsigned dst, unsigned src)
{
    static const uint8_t table[8] = {0x01, 0x09, 0, 0, 0x21, 0x29, 0x31, 0x39};
    rex(e, false, src, 0, dst); byte(e, table[n]); modrm_reg(e, src, dst);
}
/* d = l op r for a two-operand machine, without clobbering r. */
static void alu3(emitter *e, unsigned n, unsigned d, unsigned l, unsigned r)
{
    if (d == l) alu_rr(e, n, d, r);
    else if (d != r) { mov_rr(e, d, l); alu_rr(e, n, d, r); }
    else if (n == ALU_ADD || n == ALU_AND || n == ALU_OR || n == ALU_XOR) alu_rr(e, n, d, l);
    else { mov_rr(e, RAX, l); alu_rr(e, n, RAX, r); mov_rr(e, d, RAX); }
}

/* Leave the block after `retired` instructions: hand back the budget the
 * rest of the block had claimed and take the normal exit. */
static void exit_block(emitter *e, unsigned retired)
{
    if (e->n - retired) alu_ri(e, ALU_ADD, RBP, e->n - retired);
    jmp32(e, &e->done_fixups[e->ndone++]);
}

/* A patchable rel32 branch (already emitted, field at `site`) that must
 * initially reach a stub returning to the caller with the site recorded. */
static void link_site(emitter *e, unsigned site)
{
    e->links[e->nlinks].site = site;
    e->links[e->nlinks].stub = 0;
    e->nlinks++;
}

/* Helper call for instruction k: guest state goes to memory first and
 * nothing is cached afterwards. Exits with k + 1 retired when told to. */
static void helper(emitter *e, const mrc_jit_block *b, unsigned k)
{
    rc_writeback(e);
    rc_invalidate(e);
    mov_rr64(e, RDI, RBX);
    mov_imm(e, RSI, b->words[k]);
    mov_imm(e, RDX, b->va + k * 4);
    mov_imm(e, RCX, k);
    if (b->delay && k == b->n - 1) {
        mov_rr(e, R8, R12);
        call_abs(e, (const void *)mrc_jit_exec_delay);
    } else call_abs(e, (const void *)mrc_jit_exec);
    byte(e, 0x85); modrm_reg(e, RAX, RAX);              /* test eax,eax */
    byte(e, 0x74); unsigned at = here(e); byte(e, 0);   /* je over */
    exit_block(e, k + 1);
    if (!e->overflow) e->out[at] = (uint8_t)(here(e) - at - 1);
}
static void slow_path(emitter *e, const mrc_jit_block *b, unsigned k,
                      const regcache *at_branch)
{
    regcache join = e->rc;
    e->rc = *at_branch;
    helper(e, b, k);
    rc_restore(e, &join);
}

/* Effective address in ecx, host page in rax, or jump to the slow path. */
static void address(emitter *e, const mrc_jit_ir *i, bool write, unsigned *slow, unsigned *slow2)
{
    unsigned base = rc_read(e, i->left, RCX);
    mov_rr(e, RCX, base);
    if (i->value) alu_ri(e, ALU_ADD, RCX, i->value);
    if (i->size > 1) {
        byte(e, 0xF6); modrm_reg(e, 0, RCX); byte(e, i->size - 1); /* test cl, mask */
        jcc32(e, CC_NE, slow);
    } else *slow = 0;
    mov_rr(e, RDX, RCX);
    shift_imm(e, SH_SHR, RDX, 12);
    rex(e, true, RAX, RDX, write ? R14 : R13); byte(e, 0x8B);
    mem_index(e, RAX, write ? R14 : R13, RDX, 3);
    rex(e, true, RAX, 0, RAX); byte(e, 0x85); modrm_reg(e, RAX, RAX); /* test rax,rax */
    jcc32(e, CC_E, slow2);
    alu_ri(e, ALU_AND, RCX, 0xFFF);
}

static void memory_op(emitter *e, const mrc_jit_block *b, unsigned k)
{
    const mrc_jit_ir *i = &b->ir[k];
    unsigned slow = 0, slow2 = 0, done = 0;
    bool write = i->op == J_STORE;
    address(e, i, write, &slow, &slow2);
    regcache at_branch = e->rc;
    if (!write) {
        unsigned d = rc_write(e, i->dst, RDX);
        rex(e, false, d, RCX, RAX);
        if (i->size == 4) { byte(e, 0x8B); mem_index(e, d, RAX, RCX, 0); bswap(e, d); }
        else if (i->size == 2) {
            byte(e, 0x0F); byte(e, 0xB7); mem_index(e, d, RAX, RCX, 0);
            ror16_8(e, d);
            if (i->sign) { rex(e, false, d, 0, d); byte(e, 0x0F); byte(e, 0xBF); modrm_reg(e, d, d); }
        } else {
            byte(e, 0x0F); byte(e, i->sign ? 0xBE : 0xB6); mem_index(e, d, RAX, RCX, 0);
        }
        rc_written(e, i->dst, d);
        inc_counter64(e, R15, (int32_t)offsetof(mrc_bus, reads));
    } else {
        unsigned v = rc_read(e, i->right, RDX);
        if (i->size == 4) { mov_rr(e, RDX, v); bswap(e, RDX); v = RDX; }
        else if (i->size == 2) { mov_rr(e, RDX, v); ror16_8(e, RDX); v = RDX; }
        if (i->size == 2) byte(e, 0x66);
        /* A byte store from esi/edi needs REX to name sil/dil. */
        if (i->size == 1 && (v == RSI || v == RDI)) byte(e, 0x40);
        rex(e, false, v, RCX, RAX);
        byte(e, i->size == 1 ? 0x88 : 0x89);
        mem_index(e, v, RAX, RCX, 0);
        inc_counter64(e, R15, (int32_t)offsetof(mrc_bus, writes));
    }
    jmp32(e, &done);
    if (slow) patch(e, slow, here(e));
    patch(e, slow2, here(e));
    slow_path(e, b, k, &at_branch);
    patch(e, done, here(e));
}

/* ADD/SUB/ADDI: the sum is discarded and the reference path raises the
 * overflow exception whenever the host flags say so. */
static void trapping_alu(emitter *e, const mrc_jit_block *b, unsigned k)
{
    const mrc_jit_ir *i = &b->ir[k];
    unsigned overflow, done;
    unsigned l = rc_read(e, i->left, RAX);
    mov_rr(e, RAX, l);
    unsigned n = i->op == J_ADD_OV ? ALU_ADD : ALU_SUB;
    if (i->immediate) alu_ri(e, n, RAX, i->value);
    else alu_rr(e, n, RAX, rc_read(e, i->right, RDX));
    jcc32(e, 0 /* jo */, &overflow);
    regcache at_branch = e->rc;
    unsigned d = rc_write(e, i->dst, RAX);
    mov_rr(e, d, RAX);
    rc_written(e, i->dst, d);
    jmp32(e, &done);
    patch(e, overflow, here(e));
    slow_path(e, b, k, &at_branch);
    patch(e, done, here(e));
}

static void alu(emitter *e, const mrc_jit_ir *i)
{
    if (!i->dst) return;                  /* no side effects: skip */
    unsigned l, r, d;
    switch (i->op) {
    case J_CONST:
        d = rc_write(e, i->dst, RAX); mov_imm(e, d, i->value); rc_written(e, i->dst, d); return;
    case J_ADD: case J_SUB: case J_AND: case J_OR: case J_XOR: case J_NOR: {
        static const uint8_t n[] = {ALU_ADD, ALU_SUB, ALU_AND, ALU_OR, ALU_XOR, ALU_OR};
        unsigned op = n[i->op - J_ADD];
        if (i->op == J_AND && !i->immediate && (!i->left || !i->right)) {
            d = rc_write(e, i->dst, RAX); mov_imm(e, d, 0); rc_written(e, i->dst, d);
            return;
        }
        l = rc_read(e, i->left, RAX);
        if (i->immediate) {
            d = rc_write(e, i->dst, RAX);
            mov_rr(e, d, l);
            if (i->value) alu_ri(e, op, d, i->value);
        } else {
            r = rc_read(e, i->right, RDX);
            d = rc_write(e, i->dst, RAX);
            alu3(e, op, d, l, r);
        }
        if (i->op == J_NOR) { rex(e, false, 0, 0, d); byte(e, 0xF7); modrm_reg(e, 2, d); }
        rc_written(e, i->dst, d);
        return;
    }
    case J_SLT: case J_SLTU:
        l = rc_read(e, i->left, RAX);
        if (i->immediate) alu_ri(e, ALU_CMP, l, i->value);
        else alu_rr(e, ALU_CMP, l, rc_read(e, i->right, RDX));
        setcc_movzx(e, i->op == J_SLT ? CC_L : CC_B, RAX);
        d = rc_write(e, i->dst, RAX);
        mov_rr(e, d, RAX);
        rc_written(e, i->dst, d);
        return;
    case J_SHL: case J_SHR: case J_SAR: {
        unsigned n = i->op == J_SHL ? SH_SHL : i->op == J_SHR ? SH_SHR : SH_SAR;
        l = rc_read(e, i->left, RAX);
        if (!i->immediate) mov_rr(e, RCX, rc_read(e, i->right, RCX));
        d = rc_write(e, i->dst, RAX);
        mov_rr(e, d, l);
        if (i->immediate) { if (i->value) shift_imm(e, n, d, i->value); }
        else shift_cl(e, n, d);
        rc_written(e, i->dst, d);
        return;
    }
    case J_MFHI: case J_MFLO:
        d = rc_write(e, i->dst, RAX);
        load_mem32(e, d, RBX, i->op == J_MFHI ? (int32_t)offsetof(r3900, hi)
                                              : (int32_t)offsetof(r3900, lo));
        rc_written(e, i->dst, d);
        return;
    default: return;
    }
}

static void hilo(emitter *e, const mrc_jit_ir *i)
{
    unsigned l;
    switch (i->op) {
    case J_MTHI: l = rc_read(e, i->left, RAX); store_mem32(e, RBX, (int32_t)offsetof(r3900, hi), l); return;
    case J_MTLO: l = rc_read(e, i->left, RAX); store_mem32(e, RBX, (int32_t)offsetof(r3900, lo), l); return;
    case J_MULT: case J_MULTU:
        if (!i->left || !i->right) {
            store_imm32(e, RBX, (int32_t)offsetof(r3900, hi), 0);
            store_imm32(e, RBX, (int32_t)offsetof(r3900, lo), 0);
            return;
        }
        l = rc_read(e, i->left, RAX);
        mov_rr(e, RAX, l);
        { unsigned r = rc_read(e, i->right, RDX);
          rex(e, false, 0, 0, r); byte(e, 0xF7); modrm_reg(e, i->op == J_MULT ? 5 : 4, r); }
        store_mem32(e, RBX, (int32_t)offsetof(r3900, lo), RAX);
        store_mem32(e, RBX, (int32_t)offsetof(r3900, hi), RDX);
        return;
    default: return;
    }
}

/* Leaves the destination in r12d. Link writes happen here, before the delay
 * slot, as in the reference sequence. */
static void branch(emitter *e, const mrc_jit_ir *i, uint32_t pc)
{
    uint32_t fallthrough = pc + 8;
    if (i->op == J_JUMP) mov_imm(e, R12, ((pc + 4) & 0xF0000000u) | i->value);
    else if (i->op == J_JUMPR) mov_rr(e, R12, rc_read(e, i->left, RAX));
    else {
        static const uint8_t cc[] = {CC_E, CC_NE, CC_LE, CC_G, CC_L, CC_GE};
        unsigned l = rc_read(e, i->left, RAX);
        if (i->right) alu_rr(e, ALU_CMP, l, rc_read(e, i->right, RDX));
        else alu_ri(e, ALU_CMP, l, 0);
        mov_imm(e, R12, fallthrough);
        mov_imm(e, RCX, pc + 4 + i->value);
        cmov(e, cc[i->op - J_BEQ], R12, RCX);
    }
    if (i->dst) {
        unsigned d = rc_write(e, i->dst, RAX);
        mov_imm(e, d, fallthrough);
        rc_written(e, i->dst, d);
    }
}

static void status_exit(emitter *e, unsigned status, unsigned *to_ret)
{
    mov_imm64(e, RAX, (uint64_t)status << 32);
    rex(e, true, RBP, 0, RAX); byte(e, 0x09); modrm_reg(e, RBP, RAX); /* or rax,rbp */
    jmp32(e, to_ret);
}

size_t mrc_jit_emit(uint8_t *out, const uint8_t *exec, size_t cap,
                    const mrc_jit_block *b, unsigned *chain)
{
    (void)exec;
    emitter e = {.out = out, .p = out, .end = out + cap, .n = b->n};
    rc_reset(&e.rc);
    static const uint8_t prologue[] = {
        0x53, 0x55, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57, 0x56,
        /* push rbx, rbp, r12-r15, rsi: seven slots keep rsp 16-aligned */
        0x48, 0x89, 0xFB,                                     /* mov rbx,rdi */
        0x89, 0xD5,                                           /* mov ebp,edx */
    };
    bytes(&e, prologue, sizeof(prologue));
    load_mem64(&e, R13, RSI, (int32_t)offsetof(mrc_jit_tables, read));
    load_mem64(&e, R14, RSI, (int32_t)offsetof(mrc_jit_tables, write));
    load_mem64(&e, R15, RBX, (int32_t)offsetof(r3900, bus));
    *chain = here(&e);

    /* Guard: the live big-endian words must still be what was compiled. */
    mov_imm64(&e, RAX, (uint64_t)(uintptr_t)b->guest);
    for (unsigned k = 0; k < b->n; k++) {
        alu_mi(&e, ALU_CMP, RAX, (int32_t)(k * 4), __builtin_bswap32(b->words[k]));
        jcc32(&e, CC_NE, &e.fail_fixups[e.nfail++]);
    }
    /* Budget: the whole block or none of it. Only then does the block
     * start touching state (a predecessor's delay-slot in_delay must
     * survive an exit taken here). */
    alu_ri(&e, ALU_CMP, RBP, b->n);
    unsigned budget_fixup;
    jcc32(&e, CC_B, &budget_fixup);
    alu_ri(&e, ALU_SUB, RBP, b->n);
    store_imm8(&e, RBX, (int32_t)offsetof(r3900, in_delay), 0);

    for (unsigned k = 0; k < b->n; k++) {
        const mrc_jit_ir *i = &b->ir[k];
        uint32_t pc = b->va + k * 4;
        if (mrc_jit_is_branch(i)) branch(&e, i, pc);
        else if (i->op == J_LOAD || i->op == J_STORE) memory_op(&e, b, k);
        else if (i->op == J_ADD_OV || i->op == J_SUB_OV) trapping_alu(&e, b, k);
        else if (i->op == J_EXEC) helper(&e, b, k);
        else if (i->op >= J_MULT && i->op <= J_MTLO && i->op != J_MFHI && i->op != J_MFLO) hilo(&e, i);
        else alu(&e, i);
    }
    rc_writeback(&e);

    /* Straight-line end, branch with its slot, or branch awaiting its slot.
     * Successors known here get link sites; the rest return to the caller.
     * A block ending in an instruction that must be followed by an
     * interrupt check (J_EXEC with ends_block) never links. */
    const mrc_jit_ir *last = &b->ir[b->n - 1];
    bool needs_check = last->op == J_EXEC && last->ends_block;
    store_imm32(&e, RBX, (int32_t)offsetof(r3900, cur_pc), b->va + (b->n - 1) * 4);
    add_counter64(&e, RBX, (int32_t)offsetof(r3900, insn_count), b->n);
    add_counter64(&e, RBX, (int32_t)offsetof(r3900, cycle_count), b->n);
    add_counter64(&e, R15, (int32_t)offsetof(mrc_bus, reads), b->n);
    if (!b->branch) {
        store_imm32(&e, RBX, (int32_t)offsetof(r3900, pc), b->va + b->n * 4);
        store_imm32(&e, RBX, (int32_t)offsetof(r3900, next_pc), b->va + b->n * 4 + 4);
        if (!needs_check) {
            unsigned site;
            jmp32(&e, &site); link_site(&e, site);
        }
    } else if (b->delay) {
        const mrc_jit_ir *br = &b->ir[b->branch - 1];
        uint32_t bpc = b->va + (b->branch - 1) * 4;
        store_imm8(&e, RBX, (int32_t)offsetof(r3900, in_delay), 1);
        store_mem32(&e, RBX, (int32_t)offsetof(r3900, pc), R12);
        alu_ri(&e, ALU_ADD, R12, 4);
        store_mem32(&e, RBX, (int32_t)offsetof(r3900, next_pc), R12);
        if (!needs_check && br->op == J_JUMP) {
            unsigned site;
            jmp32(&e, &site); link_site(&e, site);
        } else if (!needs_check && br->op == J_JUMPR) {
            /* Look the destination up in the block table and continue in
             * a cached block's chain entry; a miss returns to the caller.
             * r12d still holds the destination (the +4 was undone). */
            alu_ri(&e, ALU_SUB, R12, 4);
            mov_rr(&e, RAX, R12); shift_imm(&e, SH_SHR, RAX, 2);
            mov_rr(&e, RCX, R12); shift_imm(&e, SH_SHR, RCX, 16);
            rex(&e, false, RCX, 0, RAX); byte(&e, 0x31); modrm_reg(&e, RCX, RAX); /* xor eax,ecx */
            alu_ri(&e, ALU_AND, RAX, MRC_JIT_SETS - 1);
            shift_imm(&e, SH_SHL, RAX, 7);           /* * 4 ways * 32 bytes */
            load_mem64(&e, RCX, RSP, 0);
            load_mem64(&e, RCX, RCX, (int32_t)offsetof(mrc_jit_tables, slots));
            rex(&e, true, RAX, 0, RCX); byte(&e, 0x01); modrm_reg(&e, RAX, RCX); /* add rcx,rax */
            for (unsigned way = 0; way < MRC_JIT_WAYS; way++) {
                int32_t at = (int32_t)(way * sizeof(mrc_jit_slot));
                unsigned miss1, miss2;
                rex(&e, false, R12, 0, RCX); byte(&e, 0x39);
                mem(&e, R12, RCX, at + (int32_t)offsetof(mrc_jit_slot, va)); /* cmp [slot.va],r12d */
                jcc32(&e, CC_NE, &miss1);
                load_mem64(&e, RAX, RCX, at + (int32_t)offsetof(mrc_jit_slot, chain));
                rex(&e, true, RAX, 0, RAX); byte(&e, 0x85); modrm_reg(&e, RAX, RAX);
                jcc32(&e, CC_E, &miss2);
                byte(&e, 0xFF); byte(&e, 0xE0);      /* jmp rax */
                patch(&e, miss1, here(&e));
                patch(&e, miss2, here(&e));
            }
        } else if (!needs_check) {
            unsigned taken, fall;
            alu_ri(&e, ALU_CMP, R12, bpc + 8 + br->value);  /* r12 = target+4 */
            jcc32(&e, CC_E, &taken); link_site(&e, taken);
            jmp32(&e, &fall); link_site(&e, fall);
        }
    } else {
        store_imm8(&e, RBX, (int32_t)offsetof(r3900, branch_pending), 1);
        store_imm32(&e, RBX, (int32_t)offsetof(r3900, pc), b->va + b->n * 4);
        store_mem32(&e, RBX, (int32_t)offsetof(r3900, next_pc), R12);
    }
    unsigned done_label = here(&e);
    mov_rr(&e, RAX, RBP);                       /* status DONE, remaining */
    unsigned ret_label = here(&e);
    static const uint8_t epilogue[] = {
        0x59, 0x41, 0x5F, 0x41, 0x5E, 0x41, 0x5D, 0x41, 0x5C, 0x5D, 0x5B, 0xC3 };
    bytes(&e, epilogue, sizeof(epilogue));

    /* Link stubs: record the site, then take the normal exit. */
    for (unsigned k = 0; k < e.nlinks; k++) {
        e.links[k].stub = here(&e);
        mov_imm64(&e, RAX, (uint64_t)(uintptr_t)(out + e.links[k].site));
        load_mem64(&e, RCX, RSP, 0);
        rex(&e, true, RAX, 0, RCX); byte(&e, 0x89);
        mem(&e, RAX, RCX, (int32_t)offsetof(mrc_jit_tables, link));
        unsigned j; jmp32(&e, &j); patch(&e, j, done_label);
    }
    /* Stale guard: a link site too, so a recompiled block replaces this one
     * for every predecessor chained here. */
    unsigned stale_label = here(&e), stale_site, to_ret;
    jmp32(&e, &stale_site);
    mov_imm64(&e, RAX, (uint64_t)(uintptr_t)(out + stale_site));
    load_mem64(&e, RCX, RSP, 0);
    rex(&e, true, RAX, 0, RCX); byte(&e, 0x89);
    mem(&e, RAX, RCX, (int32_t)offsetof(mrc_jit_tables, link));
    status_exit(&e, MRC_JIT_STALE, &to_ret);
    unsigned budget_label = here(&e), to_ret2;
    status_exit(&e, MRC_JIT_BUDGET, &to_ret2);

    if (e.overflow) return 0;
    patch(&e, stale_site, stale_site + 4);      /* falls into its stub */
    patch(&e, to_ret, ret_label);
    patch(&e, to_ret2, ret_label);
    patch(&e, budget_fixup, budget_label);
    for (unsigned k = 0; k < e.nfail; k++) patch(&e, e.fail_fixups[k], stale_label);
    for (unsigned k = 0; k < e.ndone; k++) patch(&e, e.done_fixups[k], done_label);
    for (unsigned k = 0; k < e.nlinks; k++) patch(&e, e.links[k].site, e.links[k].stub);
    return (size_t)(e.p - out);
}

void mrc_jit_patch_link(uint8_t *site, const uint8_t *site_exec,
                        const uint8_t *target)
{
    int32_t rel = (int32_t)(target - (site_exec + 4));
    memcpy(site, &rel, 4);
}

bool mrc_jit_host_supported(void) { return true; }
#endif
