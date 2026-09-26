#include "jit/jit.h"
#if defined(__aarch64__) && !defined(__AARCH64EB__)
#include <stddef.h>
#include <string.h>
/*
 * AArch64 backend. Same block shape as x86-64, plus a block-local register
 * cache: the first eleven guest registers a block touches live in host
 * registers for the block's duration and are written back at every exit and
 * before every helper call (after which nothing is trusted to be cached, so
 * helpers see and change exactly the memory image the interpreter would).
 *
 * Register use inside a block:
 *   x19 r3900 *c          x21 write page table   x23 branch destination
 *   x20 read page table   x22 mh_bus *          w24 remaining budget
 *   x25 r3900_jit *       w4-w8, w10-w15 guest register cache
 *   w0-w3, w9, w16, w17 scratch
 *
 * Encodings follow the Arm Architecture Reference Manual (A64 instruction
 * set). Guard words are compared eight bytes at a time against literals
 * placed after the code.
 */
enum { X19 = 19, X20, X21, X22, X23, X24, X25, XZR = 31 };
enum { EQ = 0, NE, CS, CC, MI, PL, VS, VC, HI, LS, GE, LT, GT, LE };
#define CACHE_SLOTS 11
static const uint8_t cache_regs[CACHE_SLOTS] = {4, 5, 6, 7, 8, 10, 11, 12, 13, 14, 15};

typedef struct {
    int8_t host[32];             /* host register per guest register, or -1 */
    bool dirty[32];
    uint8_t guest[CACHE_SLOTS];  /* guest register per slot, 32 = free */
} regcache;

typedef struct {
    uint32_t *out, *p, *end;
    bool overflow;
    unsigned n;
    unsigned fail_fixups[MH_JIT_MAX_INSNS + 1], nfail;
    unsigned done_fixups[MH_JIT_MAX_INSNS * 4], ndone;
    struct { unsigned site, stub; } links[3];
    unsigned nlinks;
    struct { unsigned insn; uint64_t value; } literals[MH_JIT_MAX_INSNS / 2 + 1];
    unsigned nliterals;
    regcache rc;
} emitter;

static void insn(emitter *e, uint32_t w)
{
    if (e->p < e->end) *e->p++ = w;
    else e->overflow = true;
}
static unsigned here(const emitter *e) { return (unsigned)(e->p - e->out); }

#define GPR(n) ((uint32_t)offsetof(r3900, r) + (uint32_t)(n) * 4)

static void ldr_w(emitter *e, unsigned rt, unsigned rn, uint32_t off)
{
    insn(e, 0xB9400000u | ((off / 4) << 10) | (rn << 5) | rt);
}
static void str_w(emitter *e, unsigned rt, unsigned rn, uint32_t off)
{
    insn(e, 0xB9000000u | ((off / 4) << 10) | (rn << 5) | rt);
}
static void ldr_x(emitter *e, unsigned rt, unsigned rn, uint32_t off)
{
    insn(e, 0xF9400000u | ((off / 8) << 10) | (rn << 5) | rt);
}
static void str_x(emitter *e, unsigned rt, unsigned rn, uint32_t off)
{
    insn(e, 0xF9000000u | ((off / 8) << 10) | (rn << 5) | rt);
}
static void strb_imm(emitter *e, unsigned rt, unsigned rn, uint32_t off)
{
    insn(e, 0x39000000u | (off << 10) | (rn << 5) | rt);
}
static void mov_w(emitter *e, unsigned rd, uint32_t v)
{
    if (!(v >> 16)) insn(e, 0x52800000u | (v << 5) | rd);
    else if ((v >> 16) == 0xFFFF) insn(e, 0x12800000u | ((~v & 0xFFFF) << 5) | rd);
    else {
        insn(e, 0x52800000u | ((v & 0xFFFF) << 5) | rd);
        insn(e, 0x72A00000u | ((v >> 16) << 5) | rd);
    }
}
static void mov_x(emitter *e, unsigned rd, uint64_t v)
{
    insn(e, 0xD2800000u | ((uint32_t)(v & 0xFFFF) << 5) | rd);
    for (unsigned hw = 1; hw < 4; hw++) {
        uint32_t part = (uint32_t)((v >> (16 * hw)) & 0xFFFF);
        if (part) insn(e, 0xF2800000u | (hw << 21) | (part << 5) | rd);
    }
}
static void mov_rr(emitter *e, unsigned rd, unsigned rm)      /* 32-bit */
{
    if (rd != rm) insn(e, 0x2A0003E0u | (rm << 16) | rd);
}
static void cmp_rr(emitter *e, unsigned rn, unsigned rm)
{
    insn(e, 0x6B00001Fu | (rm << 16) | (rn << 5));
}
static void cmp_imm(emitter *e, unsigned rn, unsigned imm12)
{
    insn(e, 0x7100001Fu | (imm12 << 10) | (rn << 5));
}
/* w[rd] = w[rn] + v, choosing the shortest form. Clobbers w17 if needed.
 * Register 31 is the stack pointer in the immediate forms, so a $zero
 * source (wzr from the cache) becomes a plain constant. */
static void add_imm32(emitter *e, unsigned rd, unsigned rn, uint32_t v)
{
    if (rn == XZR) mov_w(e, rd, v);
    else if (v < 4096) insn(e, 0x11000000u | (v << 10) | (rn << 5) | rd);
    else if (-v < 4096) insn(e, 0x51000000u | ((-v) << 10) | (rn << 5) | rd);
    else {
        mov_w(e, 17, v);
        insn(e, 0x0B000000u | (17u << 16) | (rn << 5) | rd);
    }
}
static void b_cond(emitter *e, unsigned cond, unsigned *fixup)
{
    *fixup = here(e); insn(e, 0x54000000u | cond);
}
static void jump(emitter *e, unsigned *fixup)
{
    *fixup = here(e); insn(e, 0x14000000u);
}
static void patch(emitter *e, unsigned fixup, unsigned target)
{
    if (fixup >= (unsigned)(e->end - e->out)) return;
    int32_t rel = (int32_t)target - (int32_t)fixup;
    uint32_t w = e->out[fixup];
    if ((w & 0xFC000000u) == 0x14000000u)
        e->out[fixup] = 0x14000000u | ((uint32_t)rel & 0x03FFFFFFu);
    else                                                /* b.cond / cbz / ldr lit */
        e->out[fixup] = (w & ~0x00FFFFE0u) | (((uint32_t)rel & 0x7FFFFu) << 5);
}
static void call(emitter *e, const void *fn)
{
    mov_x(e, 16, (uint64_t)(uintptr_t)fn);
    insn(e, 0xD63F0200u);                               /* blr x16 */
}
static void add_counter(emitter *e, unsigned rn, uint32_t off, unsigned v)
{
    ldr_x(e, 9, rn, off);
    insn(e, 0x91000000u | (v << 10) | (9u << 5) | 9);   /* add x9,x9,#v */
    str_x(e, 9, rn, off);
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
    for (unsigned s = 0; s < CACHE_SLOTS; s++)
        if (e->rc.guest[s] == 32) {
            e->rc.guest[s] = (uint8_t)guest;
            e->rc.host[guest] = (int8_t)cache_regs[s];
            return cache_regs[s];
        }
    return -1;
}
/* Host register holding guest `guest` for reading. $zero reads as wzr.
 * When the cache is full the value is loaded into `scratch` instead. */
static unsigned rc_read(emitter *e, unsigned guest, unsigned scratch)
{
    if (!guest) return XZR;
    if (e->rc.host[guest] >= 0) return (unsigned)e->rc.host[guest];
    int h = rc_alloc(e, guest);
    if (h < 0) h = (int)scratch;
    ldr_w(e, (unsigned)h, X19, GPR(guest));
    return (unsigned)h;
}
/* Host register to write guest `guest` into. Writes to $zero or to an
 * uncached register go to `scratch`; rc_written then stores the latter. */
static unsigned rc_write(emitter *e, unsigned guest, unsigned scratch)
{
    if (!guest) return scratch;
    if (e->rc.host[guest] < 0 && rc_alloc(e, guest) < 0) return scratch;
    e->rc.dirty[guest] = true;
    return (unsigned)e->rc.host[guest];
}
static void rc_written(emitter *e, unsigned guest, unsigned host)
{
    if (guest && (int)host != e->rc.host[guest]) str_w(e, host, X19, GPR(guest));
}
static void rc_writeback(emitter *e)
{
    for (unsigned g = 1; g < 32; g++)
        if (e->rc.dirty[g]) {
            str_w(e, (unsigned)e->rc.host[g], X19, GPR(g));
            e->rc.dirty[g] = false;
        }
}
static void rc_invalidate(emitter *e) { rc_reset(&e->rc); }
/* Reload everything `state` has cached so a slow path rejoins the fast
 * path with the same registers live (dirty flags come with the state). */
static void rc_restore(emitter *e, const regcache *state)
{
    for (unsigned g = 1; g < 32; g++)
        if (state->host[g] >= 0) ldr_w(e, (unsigned)state->host[g], X19, GPR(g));
    e->rc = *state;
}

/* ------------------------------------------------------------ pieces */

static void exit_block(emitter *e, unsigned retired)
{
    if (e->n - retired) add_imm32(e, X24, X24, e->n - retired);
    jump(e, &e->done_fixups[e->ndone++]);
}
static void link_site(emitter *e, unsigned site)
{
    e->links[e->nlinks].site = site;
    e->links[e->nlinks].stub = 0;
    e->nlinks++;
}
/* Helper call for instruction k: guest state goes to memory first, and
 * nothing is cached afterwards. Exits with k + 1 retired when told to. */
static void helper(emitter *e, const mh_jit_block *b, unsigned k)
{
    rc_writeback(e);
    rc_invalidate(e);
    insn(e, 0xAA1303E0u);                               /* mov x0, x19 */
    mov_w(e, 1, b->words[k]);
    mov_w(e, 2, b->va + k * 4);
    mov_w(e, 3, k);
    if (b->delay && k == b->n - 1) {
        mov_rr(e, 4, X23);
        call(e, (const void *)mh_jit_exec_delay);
    } else call(e, (const void *)mh_jit_exec);
    unsigned over = here(e);
    insn(e, 0x34000000u);                               /* cbz w0, over */
    exit_block(e, k + 1);
    patch(e, over, here(e));
}
/* Out-of-line slow path that rejoins the fast path's cache state. */
static void slow_path(emitter *e, const mh_jit_block *b, unsigned k,
                      const regcache *at_branch)
{
    regcache join = e->rc;
    e->rc = *at_branch;
    helper(e, b, k);
    rc_restore(e, &join);
}

/* w1 = address, x3 = host page, or branch to the slow path. */
static void address(emitter *e, const mh_jit_ir *i, bool write, unsigned *slow, unsigned *slow2)
{
    unsigned base = rc_read(e, i->left, 1);
    if (i->value) add_imm32(e, 1, base, i->value);
    else mov_rr(e, 1, base);
    if (i->size > 1) {
        insn(e, i->size == 4 ? 0x7200043Fu : 0x7200003Fu); /* tst w1,#3 / #1 */
        b_cond(e, NE, slow);
    } else *slow = 0;
    insn(e, 0x530C7C22u);                               /* lsr w2, w1, #12 */
    insn(e, 0xF8627800u | ((write ? X21 : X20) << 5) | 3); /* ldr x3,[xT,x2,lsl#3] */
    *slow2 = here(e);
    insn(e, 0xB4000003u);                               /* cbz x3, slow2 */
    insn(e, 0x12002C21u);                               /* and w1, w1, #0xfff */
}

static void memory_op(emitter *e, const mh_jit_block *b, unsigned k)
{
    const mh_jit_ir *i = &b->ir[k];
    unsigned slow = 0, slow2 = 0, done = 0;
    bool write = i->op == J_STORE;
    address(e, i, write, &slow, &slow2);
    regcache at_branch = e->rc;
    if (!write) {
        unsigned d = rc_write(e, i->dst, 9);
        if (i->size == 4) {
            insn(e, 0xB8616860u | d);                   /* ldr wd,[x3,x1] */
            insn(e, 0x5AC00800u | (d << 5) | d);        /* rev wd,wd */
        } else if (i->size == 2) {
            insn(e, 0x78616860u | d);                   /* ldrh wd,[x3,x1] */
            insn(e, 0x5AC00400u | (d << 5) | d);        /* rev16 wd,wd */
            if (i->sign) insn(e, 0x13003C00u | (d << 5) | d); /* sxth */
        } else insn(e, (i->sign ? 0x38E16860u : 0x38616860u) | d);
        rc_written(e, i->dst, d);
        add_counter(e, X22, (uint32_t)offsetof(mh_bus, reads), 1);
    } else {
        unsigned v = rc_read(e, i->right, 9);
        if (i->size == 4) { insn(e, 0x5AC00800u | (v << 5) | 9); insn(e, 0xB8216860u | 9); }
        else if (i->size == 2) { insn(e, 0x5AC00400u | (v << 5) | 9); insn(e, 0x78216860u | 9); }
        else insn(e, 0x38216860u | v);                  /* strb wv,[x3,x1] */
        add_counter(e, X22, (uint32_t)offsetof(mh_bus, writes), 1);
    }
    jump(e, &done);
    if (slow) patch(e, slow, here(e));
    patch(e, slow2, here(e));
    slow_path(e, b, k, &at_branch);
    patch(e, done, here(e));
}

/* ADD/SUB/ADDI: on signed overflow the reference path raises the
 * exception; the result register is untouched either way until then. */
static void trapping_alu(emitter *e, const mh_jit_block *b, unsigned k)
{
    const mh_jit_ir *i = &b->ir[k];
    unsigned overflow, done;
    unsigned l = rc_read(e, i->left, 1), r;
    if (i->immediate) { mov_w(e, 2, i->value); r = 2; }
    else r = rc_read(e, i->right, 2);
    insn(e, (i->op == J_ADD_OV ? 0x2B000000u : 0x6B000000u) | (r << 16) | (l << 5) | 3); /* adds/subs w3 */
    b_cond(e, VS, &overflow);
    regcache at_branch = e->rc;
    unsigned d = rc_write(e, i->dst, 9);
    if (i->dst) mov_rr(e, d, 3);
    rc_written(e, i->dst, d);
    jump(e, &done);
    patch(e, overflow, here(e));
    slow_path(e, b, k, &at_branch);
    patch(e, done, here(e));
}

static void alu(emitter *e, const mh_jit_ir *i)
{
    if (!i->dst) return;
    unsigned l, r, d;
    switch (i->op) {
    case J_CONST:
        d = rc_write(e, i->dst, 3); mov_w(e, d, i->value); rc_written(e, i->dst, d); return;
    case J_ADD:
        if (i->immediate) {
            l = rc_read(e, i->left, 1);
            d = rc_write(e, i->dst, 3);
            add_imm32(e, d, l, i->value);
            rc_written(e, i->dst, d);
            return;
        }
        /* fall through */
    case J_SUB: case J_AND: case J_OR: case J_XOR: case J_NOR: {
        static const uint32_t op[] = {0x0B000000u, 0x4B000000u, 0x0A000000u,
                                      0x2A000000u, 0x4A000000u, 0x2A000000u};
        l = rc_read(e, i->left, 1);
        if (i->immediate) { mov_w(e, 2, i->value); r = 2; } else r = rc_read(e, i->right, 2);
        d = rc_write(e, i->dst, 3);
        insn(e, op[i->op - J_ADD] | (r << 16) | (l << 5) | d);
        if (i->op == J_NOR) insn(e, 0x2A2003E0u | (d << 16) | d); /* mvn wd, wd */
        rc_written(e, i->dst, d);
        return;
    }
    case J_SLT: case J_SLTU:
        l = rc_read(e, i->left, 1);
        if (i->immediate) { mov_w(e, 2, i->value); r = 2; } else r = rc_read(e, i->right, 2);
        cmp_rr(e, l, r);
        d = rc_write(e, i->dst, 3);
        insn(e, 0x1A9F07E0u | (((i->op == J_SLT ? LT : CC) ^ 1) << 12) | d); /* cset wd */
        rc_written(e, i->dst, d);
        return;
    case J_SHL: case J_SHR: case J_SAR:
        l = rc_read(e, i->left, 1);
        if (i->immediate) {
            unsigned sh = i->value & 31;
            d = rc_write(e, i->dst, 3);
            if (!sh) mov_rr(e, d, l);
            else if (i->op == J_SHL)
                insn(e, 0x53000000u | (((32 - sh) & 31) << 16) | ((31 - sh) << 10) | (l << 5) | d);
            else
                insn(e, (i->op == J_SHR ? 0x53000000u : 0x13000000u) |
                        (sh << 16) | (31u << 10) | (l << 5) | d);
        } else {
            r = rc_read(e, i->right, 2);
            d = rc_write(e, i->dst, 3);
            insn(e, (i->op == J_SHL ? 0x1AC02000u : i->op == J_SHR ? 0x1AC02400u
                                                                     : 0x1AC02800u) |
                    (r << 16) | (l << 5) | d);
        }
        rc_written(e, i->dst, d);
        return;
    case J_MFHI: case J_MFLO:
        d = rc_write(e, i->dst, 3);
        ldr_w(e, d, X19, i->op == J_MFHI ? (uint32_t)offsetof(r3900, hi)
                                         : (uint32_t)offsetof(r3900, lo));
        rc_written(e, i->dst, d);
        return;
    default: return;
    }
}

static void hilo(emitter *e, const mh_jit_ir *i)
{
    unsigned l, r;
    switch (i->op) {
    case J_MTHI: l = rc_read(e, i->left, 1); str_w(e, l, X19, (uint32_t)offsetof(r3900, hi)); return;
    case J_MTLO: l = rc_read(e, i->left, 1); str_w(e, l, X19, (uint32_t)offsetof(r3900, lo)); return;
    case J_MULT: case J_MULTU:
        l = rc_read(e, i->left, 1);
        r = rc_read(e, i->right, 2);
        insn(e, (i->op == J_MULT ? 0x9B207C00u : 0x9BA07C00u) | (r << 16) | (l << 5) | 3);
        str_w(e, 3, X19, (uint32_t)offsetof(r3900, lo));
        insn(e, 0xD360FC63u);                           /* lsr x3, x3, #32 */
        str_w(e, 3, X19, (uint32_t)offsetof(r3900, hi));
        return;
    default: return;
    }
}

static void branch(emitter *e, const mh_jit_ir *i, uint32_t pc)
{
    uint32_t fallthrough = pc + 8;
    if (i->op == J_JUMP) mov_w(e, X23, ((pc + 4) & 0xF0000000u) | i->value);
    else if (i->op == J_JUMPR) mov_rr(e, X23, rc_read(e, i->left, 1));
    else {
        static const uint8_t cond[] = {EQ, NE, LE, GT, LT, GE};
        unsigned l = rc_read(e, i->left, 1), r = rc_read(e, i->right, 2);
        cmp_rr(e, l, r);
        mov_w(e, X23, fallthrough);
        mov_w(e, 3, pc + 4 + i->value);
        insn(e, 0x1A800000u | (X23 << 16) | (cond[i->op - J_BEQ] << 12) | (3u << 5) | X23);
    }
    if (i->dst) {
        unsigned d = rc_write(e, i->dst, 3);
        mov_w(e, d, fallthrough);
        rc_written(e, i->dst, d);
    }
}

static void status_exit(emitter *e, unsigned status, unsigned *to_ret)
{
    insn(e, 0xD2C00000u | (status << 5) | 9);           /* movz x9,#status,lsl#32 */
    insn(e, 0xAA180120u);                               /* orr x0, x9, x24 */
    jump(e, to_ret);
}
static void record_site(emitter *e, uint8_t *out_bytes, unsigned site)
{
    mov_x(e, 9, (uint64_t)(uintptr_t)(out_bytes + site * 4));
    str_x(e, 9, X25, (uint32_t)offsetof(mh_jit_tables, link));
}

size_t mh_jit_emit(uint8_t *out, const uint8_t *exec, size_t cap,
                    const mh_jit_block *b, unsigned *chain)
{
    (void)exec;
    emitter e = {.out = (uint32_t *)out, .p = (uint32_t *)out,
                 .end = (uint32_t *)(out + (cap & ~(size_t)3)), .n = b->n};
    rc_reset(&e.rc);
    static const uint32_t prologue[] = {
        0xA9BB7BFD,     /* stp x29, x30, [sp, #-80]! */
        0xA90153F3,     /* stp x19, x20, [sp, #16] */
        0xA9025BF5,     /* stp x21, x22, [sp, #32] */
        0xA90363F7,     /* stp x23, x24, [sp, #48] */
        0xF90023F9,     /* str x25, [sp, #64] */
        0xAA0003F3,     /* mov x19, x0 */
        0xAA0103F9,     /* mov x25, x1 */
        0x2A0203F8,     /* mov w24, w2 */
    };
    for (unsigned k = 0; k < sizeof(prologue) / 4; k++) insn(&e, prologue[k]);
    ldr_x(&e, X20, 1, (uint32_t)offsetof(mh_jit_tables, read));
    ldr_x(&e, X21, 1, (uint32_t)offsetof(mh_jit_tables, write));
    ldr_x(&e, X22, X19, (uint32_t)offsetof(r3900, bus));
    *chain = here(&e) * 4;

    /* Guard against the live words, two per compare where possible. */
    mov_x(&e, 9, (uint64_t)(uintptr_t)b->guest);
    for (unsigned k = 0; k < b->n; k += 2) {
        if (k + 1 < b->n) {
            uint64_t expect = (uint64_t)__builtin_bswap32(b->words[k]) |
                              (uint64_t)__builtin_bswap32(b->words[k + 1]) << 32;
            ldr_x(&e, 10, 9, k * 4);
            e.literals[e.nliterals].insn = here(&e);
            e.literals[e.nliterals++].value = expect;
            insn(&e, 0x58000000u | 11);                 /* ldr x11, literal */
            insn(&e, 0xEB0B015Fu);                      /* cmp x10, x11 */
        } else {
            ldr_w(&e, 10, 9, k * 4);
            mov_w(&e, 11, __builtin_bswap32(b->words[k]));
            cmp_rr(&e, 10, 11);
        }
        b_cond(&e, NE, &e.fail_fixups[e.nfail++]);
    }
    cmp_imm(&e, X24, b->n);
    unsigned budget_fixup;
    b_cond(&e, CC, &budget_fixup);                      /* w24 < n */
    insn(&e, 0x51000000u | (b->n << 10) | (X24 << 5) | X24); /* sub w24,w24,#n */
    strb_imm(&e, XZR, X19, (uint32_t)offsetof(r3900, in_delay));

    for (unsigned k = 0; k < b->n; k++) {
        const mh_jit_ir *i = &b->ir[k];
        uint32_t pc = b->va + k * 4;
        if (mh_jit_is_branch(i)) branch(&e, i, pc);
        else if (i->op == J_LOAD || i->op == J_STORE) memory_op(&e, b, k);
        else if (i->op == J_ADD_OV || i->op == J_SUB_OV) trapping_alu(&e, b, k);
        else if (i->op == J_EXEC) helper(&e, b, k);
        else if (i->op >= J_MULT && i->op <= J_MTLO && i->op != J_MFHI && i->op != J_MFLO) hilo(&e, i);
        else alu(&e, i);
    }
    rc_writeback(&e);

    const mh_jit_ir *last = &b->ir[b->n - 1];
    bool needs_check = last->op == J_EXEC && last->ends_block;
    mov_w(&e, 9, b->va + (b->n - 1) * 4);
    str_w(&e, 9, X19, (uint32_t)offsetof(r3900, cur_pc));
    add_counter(&e, X19, (uint32_t)offsetof(r3900, insn_count), b->n);
    add_counter(&e, X19, (uint32_t)offsetof(r3900, cycle_count), b->n);
    add_counter(&e, X22, (uint32_t)offsetof(mh_bus, reads), b->n);
    if (!b->branch) {
        mov_w(&e, 9, b->va + b->n * 4);
        str_w(&e, 9, X19, (uint32_t)offsetof(r3900, pc));
        add_imm32(&e, 9, 9, 4);
        str_w(&e, 9, X19, (uint32_t)offsetof(r3900, next_pc));
        if (!needs_check) { unsigned site; jump(&e, &site); link_site(&e, site); }
    } else if (b->delay) {
        const mh_jit_ir *br = &b->ir[b->branch - 1];
        uint32_t bpc = b->va + (b->branch - 1) * 4;
        mov_w(&e, 9, 1);
        strb_imm(&e, 9, X19, (uint32_t)offsetof(r3900, in_delay));
        str_w(&e, X23, X19, (uint32_t)offsetof(r3900, pc));
        add_imm32(&e, 9, X23, 4);
        str_w(&e, 9, X19, (uint32_t)offsetof(r3900, next_pc));
        if (!needs_check && br->op == J_JUMP) {
            unsigned site; jump(&e, &site); link_site(&e, site);
        } else if (!needs_check && br->op == J_JUMPR) {
            /* Block-table lookup of the destination in w23; a hit continues
             * in that block's chain entry, a miss returns to the caller. */
            insn(&e, 0x53027EE1u);                      /* lsr w1, w23, #2 */
            insn(&e, 0x4A574021u);                      /* eor w1, w1, w23, lsr #16 */
            insn(&e, 0x12003421u);                      /* and w1, w1, #0x3fff */
            insn(&e, 0xD379E021u);                      /* lsl x1, x1, #7 */
            ldr_x(&e, 9, X25, (uint32_t)offsetof(mh_jit_tables, slots));
            insn(&e, 0x8B010129u);                      /* add x9, x9, x1 */
            for (unsigned way = 0; way < MH_JIT_WAYS; way++) {
                uint32_t at = (uint32_t)(way * sizeof(mh_jit_slot));
                unsigned miss1, miss2;
                ldr_w(&e, 2, 9, at + (uint32_t)offsetof(mh_jit_slot, va));
                cmp_rr(&e, 2, X23);
                b_cond(&e, NE, &miss1);
                ldr_x(&e, 2, 9, at + (uint32_t)offsetof(mh_jit_slot, chain));
                miss2 = here(&e);
                insn(&e, 0xB4000002u);                  /* cbz x2, miss2 */
                insn(&e, 0xD61F0040u);                  /* br x2 */
                patch(&e, miss1, here(&e));
                patch(&e, miss2, here(&e));
            }
        } else if (!needs_check) {
            unsigned not_taken, taken, fall;
            mov_w(&e, 9, bpc + 4 + br->value);
            cmp_rr(&e, X23, 9);
            b_cond(&e, NE, &not_taken);
            jump(&e, &taken); link_site(&e, taken);
            patch(&e, not_taken, here(&e));
            jump(&e, &fall); link_site(&e, fall);
        }
    } else {
        mov_w(&e, 9, 1);
        strb_imm(&e, 9, X19, (uint32_t)offsetof(r3900, branch_pending));
        mov_w(&e, 9, b->va + b->n * 4);
        str_w(&e, 9, X19, (uint32_t)offsetof(r3900, pc));
        str_w(&e, X23, X19, (uint32_t)offsetof(r3900, next_pc));
    }
    unsigned done_label = here(&e);
    mov_rr(&e, 0, X24);                                 /* DONE, remaining */
    unsigned ret_label = here(&e);
    static const uint32_t epilogue[] = {
        0xF94023F9,     /* ldr x25, [sp, #64] */
        0xA94363F7,     /* ldp x23, x24, [sp, #48] */
        0xA9425BF5,     /* ldp x21, x22, [sp, #32] */
        0xA94153F3,     /* ldp x19, x20, [sp, #16] */
        0xA8C57BFD,     /* ldp x29, x30, [sp], #80 */
        0xD65F03C0,     /* ret */
    };
    for (unsigned k = 0; k < 6; k++) insn(&e, epilogue[k]);

    for (unsigned k = 0; k < e.nlinks; k++) {
        e.links[k].stub = here(&e);
        record_site(&e, out, e.links[k].site);
        unsigned j; jump(&e, &j); patch(&e, j, done_label);
    }
    unsigned stale_label = here(&e), stale_site, to_ret, to_ret2;
    jump(&e, &stale_site);
    record_site(&e, out, stale_site);
    status_exit(&e, MH_JIT_STALE, &to_ret);
    unsigned budget_label = here(&e);
    status_exit(&e, MH_JIT_BUDGET, &to_ret2);

    /* Literal pool, 8-byte aligned. */
    if (here(&e) & 1) insn(&e, 0xD503201Fu);            /* nop */
    for (unsigned k = 0; k < e.nliterals; k++) {
        unsigned at = here(&e);
        insn(&e, (uint32_t)e.literals[k].value);
        insn(&e, (uint32_t)(e.literals[k].value >> 32));
        patch(&e, e.literals[k].insn, at);
    }
    if (e.overflow) return 0;
    patch(&e, stale_site, stale_site + 1);
    patch(&e, to_ret, ret_label);
    patch(&e, to_ret2, ret_label);
    patch(&e, budget_fixup, budget_label);
    for (unsigned k = 0; k < e.nfail; k++) patch(&e, e.fail_fixups[k], stale_label);
    for (unsigned k = 0; k < e.ndone; k++) patch(&e, e.done_fixups[k], done_label);
    for (unsigned k = 0; k < e.nlinks; k++) patch(&e, e.links[k].site, e.links[k].stub);
    return (size_t)here(&e) * 4;
}

void mh_jit_patch_link(uint8_t *site, const uint8_t *site_exec,
                        const uint8_t *target)
{
    int64_t rel = (target - site_exec) / 4;
    uint32_t w = 0x14000000u | ((uint32_t)rel & 0x03FFFFFFu);
    memcpy(site, &w, 4);
}

bool mh_jit_host_supported(void) { return true; }
#endif
