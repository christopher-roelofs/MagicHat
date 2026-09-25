/*
 * test_r3900.c — semantics tests for the CPU core.
 *
 * These cover the details that are easy to get subtly wrong and hard to
 * notice later: delay-slot bookkeeping, link-register values, big-endian
 * byte order, the unaligned load/store pair, exception EPC when the faulting
 * instruction sits in a delay slot, and interrupt gating.
 *
 * A wrong answer here is a bug we would otherwise spend days chasing through
 * ROM traces.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/bus/bus.h"
#include "cpu/mips/r3900.h"

static int failures;
static bool cached_execution;
static bool decoded_execution;
static bool jit_execution;
static const char *current_test = "";

#define CHECK(cond, fmt, ...)                                                \
    do {                                                                     \
        if (!(cond)) {                                                       \
            printf("FAIL %s: " fmt "\n", current_test, ##__VA_ARGS__);       \
            failures++;                                                      \
        }                                                                    \
    } while (0)

#define CHECK_EQ(got, want, what)                                            \
    do {                                                                     \
        uint32_t g_ = (uint32_t)(got), w_ = (uint32_t)(want);                \
        CHECK(g_ == w_, "%s: got %08X want %08X", what, g_, w_);             \
    } while (0)

/* ------------------------------------------------------------ fixture */

#define TEST_RAM_SIZE 0x10000u
#define CODE_VA       0x80001000u
#define CODE_PA       0x00001000u
#define DATA_VA       0x80002000u
#define DATA_PA       0x00002000u

typedef struct {
    mrc_bus  bus;
    r3900    cpu;
    r3900_decode_cache *cache;
    r3900_jit *jit;
    uint8_t *ram;
    uint32_t next;      /* next code offset to emit at */
} fixture;

static void fx_init(fixture *f)
{
    f->jit = jit_execution ? mrc_cpu_jit_create() : NULL;
    if (jit_execution && !f->jit) abort();
    f->cache = decoded_execution ? mrc_cpu_decode_cache_create() : NULL;
    if (decoded_execution && !f->cache) abort();
    f->ram = calloc(1, TEST_RAM_SIZE);
    mrc_bus_init(&f->bus);
    mrc_bus_add_ram(&f->bus, "ram", 0, f->ram, TEST_RAM_SIZE, TEST_RAM_SIZE);
    mrc_bus_enable_lookup(&f->bus, cached_execution);
    mrc_cpu_init(&f->cpu, &f->bus);
    mrc_cpu_reset(&f->cpu, CODE_VA);
    /* Run out of kseg0 with BEV clear so exception vectors land in RAM. */
    f->cpu.cp0[CP0_STATUS] = 0;
    f->next = CODE_PA;
}

static void fx_free(fixture *f)
{
    mrc_cpu_jit_free(f->jit);
    mrc_cpu_decode_cache_free(f->cache);
    free(f->ram);
}

static void fx_run(fixture *f, uint64_t count)
{
    if (f->jit) mrc_cpu_run_jit(&f->cpu, count, f->jit);
    else if (f->cache) mrc_cpu_run_decoded(&f->cpu, count, f->cache);
    else mrc_cpu_run(&f->cpu, count);
}

static void emit_at(fixture *f, uint32_t pa, uint32_t insn)
{
    f->ram[pa + 0] = (uint8_t)(insn >> 24);
    f->ram[pa + 1] = (uint8_t)(insn >> 16);
    f->ram[pa + 2] = (uint8_t)(insn >> 8);
    f->ram[pa + 3] = (uint8_t)insn;
}

static void emit(fixture *f, uint32_t insn)
{
    emit_at(f, f->next, insn);
    f->next += 4;
}

static uint32_t load_word(const fixture *f, uint32_t pa)
{
    return (uint32_t)f->ram[pa] << 24 | (uint32_t)f->ram[pa + 1] << 16 |
           (uint32_t)f->ram[pa + 2] << 8 | f->ram[pa + 3];
}

/* Instruction builders. */
static uint32_t I(unsigned op, unsigned rs, unsigned rt, uint32_t imm)
{ return (op << 26) | (rs << 21) | (rt << 16) | (imm & 0xFFFF); }
static uint32_t R(unsigned rs, unsigned rt, unsigned rd, unsigned sa, unsigned fn)
{ return (rs << 21) | (rt << 16) | (rd << 11) | (sa << 6) | fn; }
static uint32_t J(unsigned op, uint32_t target)
{ return (op << 26) | ((target >> 2) & 0x03FFFFFFu); }

#define ORI(rt, rs, imm)   I(0x0D, rs, rt, imm)
#define LUI(rt, imm)       I(0x0F, 0,  rt, imm)
#define ADDIU(rt, rs, imm) I(0x09, rs, rt, imm)
#define ADDU(rd, rs, rt)   R(rs, rt, rd, 0, 0x21)
#define NOP                0u

/* ------------------------------------------------------------- tests */

static void test_alu(void)
{
    current_test = "alu";
    fixture f; fx_init(&f);

    emit(&f, LUI(2, 0x1234));           /* lui  v0, 0x1234      */
    emit(&f, ORI(2, 2, 0x5678));        /* ori  v0, v0, 0x5678  */
    emit(&f, ADDIU(3, 0, 0xFFFF));      /* addiu v1, zero, -1   */
    emit(&f, R(2, 3, 4, 0, 0x2A));      /* slt  a0, v0, v1      */
    emit(&f, R(2, 3, 5, 0, 0x2B));      /* sltu a1, v0, v1      */
    emit(&f, R(0, 3, 6, 4, 0x03));      /* sra  a2, v1, 4       */
    emit(&f, R(0, 3, 7, 4, 0x02));      /* srl  a3, v1, 4       */

    fx_run(&f, 7);

    CHECK_EQ(f.cpu.r[2], 0x12345678, "lui/ori");
    CHECK_EQ(f.cpu.r[3], 0xFFFFFFFF, "addiu sign-extends");
    CHECK_EQ(f.cpu.r[4], 0, "slt is signed: 0x12345678 < -1 is false");
    CHECK_EQ(f.cpu.r[5], 1, "sltu is unsigned: 0x12345678 < 0xFFFFFFFF");
    CHECK_EQ(f.cpu.r[6], 0xFFFFFFFF, "sra keeps the sign");
    CHECK_EQ(f.cpu.r[7], 0x0FFFFFFF, "srl does not");
    fx_free(&f);
}

static void test_r0_is_hardwired(void)
{
    current_test = "r0";
    fixture f; fx_init(&f);
    emit(&f, ORI(0, 0, 0xFFFF));        /* ori zero, zero, -1 */
    fx_run(&f, 1);
    CHECK_EQ(f.cpu.r[0], 0, "writes to r0 are discarded");
    fx_free(&f);
}

static void test_branch_delay_slot(void)
{
    current_test = "branch delay slot";
    fixture f; fx_init(&f);

    /*        beq  zero, zero, +2      ; taken, skips the ori at +8
     *        ori  v0, zero, 0x11      ; delay slot — MUST execute
     *        ori  v1, zero, 0x22      ; skipped
     * target: ori a0, zero, 0x33
     */
    emit(&f, I(0x04, 0, 0, 2));
    emit(&f, ORI(2, 0, 0x11));
    emit(&f, ORI(3, 0, 0x22));
    emit(&f, ORI(4, 0, 0x33));

    fx_run(&f, 3);

    CHECK_EQ(f.cpu.r[2], 0x11, "the delay slot executes even when taken");
    CHECK_EQ(f.cpu.r[3], 0, "the skipped instruction does not");
    CHECK_EQ(f.cpu.r[4], 0x33, "control reached the branch target");
    fx_free(&f);
}

static void test_branch_not_taken_runs_delay_slot(void)
{
    current_test = "untaken branch delay slot";
    fixture f; fx_init(&f);

    emit(&f, ORI(1, 0, 1));
    emit(&f, I(0x04, 1, 0, 2));         /* beq at, zero, +2 — not taken */
    emit(&f, ORI(2, 0, 0x11));          /* delay slot — still executes */
    emit(&f, ORI(3, 0, 0x22));          /* and so does this */

    fx_run(&f, 4);

    CHECK_EQ(f.cpu.r[2], 0x11, "delay slot runs on an untaken branch");
    CHECK_EQ(f.cpu.r[3], 0x22, "execution continues in order");
    fx_free(&f);
}

static void test_jal_link_register(void)
{
    current_test = "jal link register";
    fixture f; fx_init(&f);

    /* The link value is the address AFTER the delay slot: jal_pc + 8. */
    emit(&f, J(0x03, CODE_VA + 0x20));  /* jal target */
    emit(&f, NOP);                      /* delay slot */
    fx_run(&f, 2);

    CHECK_EQ(f.cpu.r[31], CODE_VA + 8, "ra = jal_pc + 8");
    CHECK_EQ(f.cpu.pc, CODE_VA + 0x20, "pc = jump target");
    fx_free(&f);
}

static void test_jalr_link_register(void)
{
    current_test = "jalr link register";
    fixture f; fx_init(&f);

    emit(&f, LUI(8, (CODE_VA + 0x40) >> 16));
    emit(&f, ORI(8, 8, (CODE_VA + 0x40) & 0xFFFF));
    emit(&f, R(8, 0, 31, 0, 0x09));     /* jalr ra, t0 */
    emit(&f, NOP);
    fx_run(&f, 4);

    CHECK_EQ(f.cpu.r[31], CODE_VA + 0x10, "ra = jalr_pc + 8");
    CHECK_EQ(f.cpu.pc, CODE_VA + 0x40, "pc = register target");
    fx_free(&f);
}

static void test_j_keeps_pc_region(void)
{
    current_test = "j region";
    fixture f; fx_init(&f);
    emit(&f, J(0x02, 0x00000100));      /* j 0x100 — region comes from PC */
    emit(&f, NOP);
    fx_run(&f, 2);
    CHECK_EQ(f.cpu.pc, 0x80000100, "J takes its top nibble from the delay slot PC");
    fx_free(&f);
}

static void test_big_endian_memory(void)
{
    current_test = "big-endian memory";
    fixture f; fx_init(&f);

    f.ram[DATA_PA + 0] = 0xAA;
    f.ram[DATA_PA + 1] = 0xBB;
    f.ram[DATA_PA + 2] = 0xCC;
    f.ram[DATA_PA + 3] = 0xDD;

    emit(&f, LUI(8, DATA_VA >> 16));
    emit(&f, I(0x23, 8, 2, 0x2000));    /* lw  v0, 0x2000(t0) */
    emit(&f, I(0x24, 8, 3, 0x2000));    /* lbu v1, 0x2000(t0) */
    emit(&f, I(0x25, 8, 4, 0x2002));    /* lhu a0, 0x2002(t0) */
    emit(&f, I(0x20, 8, 5, 0x2000));    /* lb  a1, 0x2000(t0) */

    fx_run(&f, 5);

    CHECK_EQ(f.cpu.r[2], 0xAABBCCDD, "lw is big-endian");
    CHECK_EQ(f.cpu.r[3], 0xAA, "lbu takes the first byte");
    CHECK_EQ(f.cpu.r[4], 0xCCDD, "lhu takes the third and fourth");
    CHECK_EQ(f.cpu.r[5], 0xFFFFFFAA, "lb sign-extends");
    fx_free(&f);
}

static void test_store_sizes(void)
{
    current_test = "store sizes";
    fixture f; fx_init(&f);

    emit(&f, LUI(8, DATA_VA >> 16));
    emit(&f, LUI(9, 0x1122));
    emit(&f, ORI(9, 9, 0x3344));
    emit(&f, I(0x2B, 8, 9, 0x2000));    /* sw t1, 0x2000(t0) */
    emit(&f, I(0x29, 8, 9, 0x2004));    /* sh t1, 0x2004(t0) */
    emit(&f, I(0x28, 8, 9, 0x2008));    /* sb t1, 0x2008(t0) */

    fx_run(&f, 6);

    CHECK_EQ(f.ram[DATA_PA + 0], 0x11, "sw byte 0");
    CHECK_EQ(f.ram[DATA_PA + 3], 0x44, "sw byte 3");
    CHECK_EQ(f.ram[DATA_PA + 4], 0x33, "sh stores the low halfword, high byte first");
    CHECK_EQ(f.ram[DATA_PA + 5], 0x44, "sh low byte");
    CHECK_EQ(f.ram[DATA_PA + 8], 0x44, "sb stores the low byte");
    fx_free(&f);
}

static void test_lwl_lwr(void)
{
    current_test = "lwl/lwr";
    fixture f; fx_init(&f);

    /* Memory: 00 11 22 33 44 55 66 77 — read the unaligned word at +1. */
    for (unsigned i = 0; i < 8; i++)
        f.ram[DATA_PA + i] = (uint8_t)(i * 0x11);

    emit(&f, LUI(8, DATA_VA >> 16));
    emit(&f, ORI(2, 0, 0));             /* v0 = 0 */
    emit(&f, I(0x22, 8, 2, 0x2001));    /* lwl v0, 1(t0) */
    emit(&f, I(0x26, 8, 2, 0x2004));    /* lwr v0, 4(t0) */

    fx_run(&f, 4);

    CHECK_EQ(f.cpu.r[2], 0x11223344, "lwl+lwr assemble the unaligned word");
    fx_free(&f);
}

static void test_swl_swr(void)
{
    current_test = "swl/swr";
    fixture f; fx_init(&f);

    emit(&f, LUI(8, DATA_VA >> 16));
    emit(&f, LUI(9, 0xAABB));
    emit(&f, ORI(9, 9, 0xCCDD));
    emit(&f, I(0x2A, 8, 9, 0x2001));    /* swl t1, 1(t0) */
    emit(&f, I(0x2E, 8, 9, 0x2004));    /* swr t1, 4(t0) */

    fx_run(&f, 5);

    CHECK_EQ(f.ram[DATA_PA + 1], 0xAA, "swl byte 1");
    CHECK_EQ(f.ram[DATA_PA + 2], 0xBB, "swl byte 2");
    CHECK_EQ(f.ram[DATA_PA + 3], 0xCC, "swl byte 3");
    CHECK_EQ(f.ram[DATA_PA + 4], 0xDD, "swr byte 4");
    fx_free(&f);
}

static void test_unaligned_load_faults(void)
{
    current_test = "unaligned load";
    fixture f; fx_init(&f);

    emit(&f, LUI(8, DATA_VA >> 16));
    emit(&f, I(0x23, 8, 2, 0x2001));    /* lw v0, 0x2001(t0) — misaligned */
    fx_run(&f, 2);

    CHECK_EQ((f.cpu.cp0[CP0_CAUSE] >> 2) & 0x1F, EXC_AdEL, "raises AdEL");
    CHECK_EQ(f.cpu.cp0[CP0_BADVADDR], DATA_VA + 1, "BadVAddr is the address");
    CHECK_EQ(f.cpu.cp0[CP0_EPC], CODE_VA + 4, "EPC is the faulting instruction");
    CHECK_EQ(f.cpu.pc, 0x80000080, "vectors to the general handler");
    fx_free(&f);
}

static void test_exception_in_delay_slot(void)
{
    current_test = "exception in delay slot";
    fixture f; fx_init(&f);

    emit(&f, LUI(8, DATA_VA >> 16));
    emit(&f, I(0x04, 0, 0, 4));         /* beq zero, zero, +4 (taken) */
    emit(&f, I(0x23, 8, 2, 0x2001));    /* delay slot: misaligned lw */
    fx_run(&f, 3);

    CHECK_EQ(f.cpu.cp0[CP0_EPC], CODE_VA + 4, "EPC points at the branch, not the slot");
    CHECK(f.cpu.cp0[CP0_CAUSE] & CAUSE_BD, "Cause.BD is set");
    fx_free(&f);
}

static void test_overflow(void)
{
    current_test = "arithmetic overflow";
    fixture f; fx_init(&f);

    emit(&f, LUI(8, 0x7FFF));
    emit(&f, ORI(8, 8, 0xFFFF));        /* t0 = INT_MAX */
    emit(&f, ADDIU(9, 8, 1));           /* addiu does NOT trap */
    emit(&f, I(0x08, 8, 10, 1));        /* addi  t2, t0, 1 — traps */
    fx_run(&f, 4);

    CHECK_EQ(f.cpu.r[9], 0x80000000, "addiu wraps silently");
    CHECK_EQ((f.cpu.cp0[CP0_CAUSE] >> 2) & 0x1F, EXC_Ov, "addi raises Overflow");
    fx_free(&f);
}

static void test_div_by_zero(void)
{
    current_test = "div by zero";
    fixture f; fx_init(&f);

    emit(&f, ORI(8, 0, 7));
    emit(&f, R(8, 0, 0, 0, 0x1B));      /* divu t0, zero */
    emit(&f, R(0, 0, 2, 0, 0x12));      /* mflo v0 */
    emit(&f, R(0, 0, 3, 0, 0x10));      /* mfhi v1 */
    fx_run(&f, 4);

    CHECK_EQ(f.cpu.r[2], 0xFFFFFFFF, "divu by zero leaves LO = -1");
    CHECK_EQ(f.cpu.r[3], 7, "and HI = the dividend");
    CHECK_EQ((f.cpu.cp0[CP0_CAUSE] >> 2) & 0x1F, 0, "no exception is raised");
    fx_free(&f);
}

static void test_interrupt_gating(void)
{
    current_test = "interrupt gating";
    fixture f; fx_init(&f);

    for (unsigned i = 0; i < 8; i++)
        emit(&f, NOP);

    /* IP2 asserted, but neither IE nor the mask bit is set: nothing happens. */
    mrc_cpu_set_irq(&f.cpu, 2, true);
    fx_run(&f, 2);
    CHECK_EQ(f.cpu.pc, CODE_VA + 8, "masked interrupt does not divert");

    /* Enable the mask but not IE: still nothing. */
    f.cpu.cp0[CP0_STATUS] |= (1u << (SR_IM_SHIFT + 2));
    fx_run(&f, 1);
    CHECK_EQ(f.cpu.pc, CODE_VA + 12, "IM alone is not enough");

    /* Now enable IE. */
    f.cpu.cp0[CP0_STATUS] |= SR_IEc;
    fx_run(&f, 1);
    CHECK_EQ(f.cpu.pc, 0x80000080, "IE + IM + IP delivers the interrupt");
    CHECK_EQ((f.cpu.cp0[CP0_CAUSE] >> 2) & 0x1F, EXC_Int, "Cause says Int");
    CHECK(!(f.cpu.cp0[CP0_STATUS] & SR_IEc), "IE is cleared on entry");
    fx_free(&f);
}

static void test_rfe_restores_status(void)
{
    current_test = "rfe";
    fixture f; fx_init(&f);

    /* Set up an old/previous/current stack we can watch shift back. */
    f.cpu.cp0[CP0_STATUS] = SR_IEo | SR_KUo | SR_IEp;   /* o=11 p=01 c=00 */
    emit(&f, 0x42000010);                               /* rfe */
    fx_run(&f, 1);

    uint32_t sr = f.cpu.cp0[CP0_STATUS];
    CHECK(sr & SR_IEc, "IEp moved into IEc");
    CHECK(!(sr & SR_KUc), "KUp moved into KUc");
    CHECK(sr & SR_IEp, "IEo moved into IEp");
    CHECK(sr & SR_KUp, "KUo moved into KUp");
    fx_free(&f);
}

static void test_reserved_instruction_is_reported(void)
{
    current_test = "reserved instruction";
    fixture f; fx_init(&f);

    /* An encoding the R3900 does not define. We must trap, not ignore it —
     * silently treating unknown encodings as NOPs is how an emulator drifts
     * away from the hardware without anyone noticing. */
    emit(&f, 0xFC000000);
    fx_run(&f, 1);

    CHECK_EQ((f.cpu.cp0[CP0_CAUSE] >> 2) & 0x1F, EXC_RI, "raises RI");
    fx_free(&f);
}

static void test_cop1_is_unusable(void)
{
    current_test = "cop1 unusable";
    fixture f; fx_init(&f);

    /* No FPU on this part. mfc1 must raise Coprocessor Unusable rather than
     * quietly returning zero, which is what the previous emulator did. */
    emit(&f, 0x44020000);               /* mfc1 v0, $f0 */
    fx_run(&f, 1);

    CHECK_EQ((f.cpu.cp0[CP0_CAUSE] >> 2) & 0x1F, EXC_CpU, "raises CpU");
    CHECK_EQ((f.cpu.cp0[CP0_CAUSE] >> 28) & 3, 1, "Cause.CE names coprocessor 1");
    fx_free(&f);
}

static void test_bus_error_on_unmapped(void)
{
    current_test = "bus error";
    fixture f; fx_init(&f);

    emit(&f, LUI(8, 0x8F00));           /* t0 = 0x8F000000, nothing there */
    emit(&f, I(0x23, 8, 2, 0));         /* lw v0, 0(t0) */
    fx_run(&f, 2);

    CHECK_EQ((f.cpu.cp0[CP0_CAUSE] >> 2) & 0x1F, EXC_DBE, "raises DBE");
    CHECK_EQ(f.bus.faults, 1, "the bus counted the fault");
    fx_free(&f);
}

typedef struct {
    fixture *f;
    uint8_t *replacement;
} remap_context;

static void remap_write(void *ctx, uint32_t off, unsigned size, uint32_t value)
{
    remap_context *r = ctx;
    r->f->bus.region[0].host = r->replacement;
    mrc_bus_invalidate_lookup(&r->f->bus);
}

static void test_fetch_remap(void)
{
    current_test = "MMIO remaps code during CPU batch";
    fixture f;
    fx_init(&f);
    uint8_t *replacement = calloc(1, TEST_RAM_SIZE);
    remap_context context = {&f, replacement};
    mrc_bus_add_mmio(&f.bus, "remapper", 0x20000, 4,
                     &context, NULL, remap_write);
    emit(&f, I(0x2b, 8, 0, 0));
    emit(&f, I(0x09, 0, 2, 42));
    /* A jump closes the block so a native engine can take all four
     * instructions at once; the remap must still stop it after the store. */
    emit(&f, J(2, CODE_VA + 8));
    emit(&f, NOP);
    memcpy(replacement, f.ram, TEST_RAM_SIZE);
    emit_at(&f, CODE_PA + 4, I(0x09, 0, 2, 7));
    f.cpu.r[8] = 0x80020000;
    fx_run(&f, 4);
    CHECK_EQ(f.cpu.r[2], 42, "fetch span discarded after MMIO remap");
    CHECK_EQ(f.cpu.pc, CODE_VA + 8, "jump taken after remap");
    free(replacement);
    fx_free(&f);
}

static void test_live_instruction_writes(void)
{
    current_test = "live instruction writes through alias";
    fixture f;
    fx_init(&f);
    mrc_bus_add_ram(&f.bus, "alias", 0x100000, f.ram, TEST_RAM_SIZE, TEST_RAM_SIZE);
    f.cpu.r[8] = 0x80101004;
    f.cpu.r[9] = I(0x09, 0, 2, 42); /* replacement addiu v0,zero,42 */
    emit(&f, I(0x2b, 8, 9, 0));     /* overwrite the next instruction */
    emit(&f, I(0x09, 0, 2, 7));
    f.cpu.pc = CODE_VA + 4;
    f.cpu.next_pc = CODE_VA + 8;
    fx_run(&f, 1); /* populate the decode entry before the alias store */
    CHECK_EQ(f.cpu.r[2], 7, "old instruction executed before modification");
    f.cpu.pc = CODE_VA;
    f.cpu.next_pc = CODE_VA + 4;
    fx_run(&f, 2);
    CHECK_EQ(f.cpu.r[2], 42, "fetch observes self modification in same batch");
    CHECK_EQ(f.bus.reads, 3, "fast fetch retains bus read count");
    fx_free(&f);
}

/* Compare independently scheduled execution, not just final boot output.
 * Inputs are deterministic; all data addresses stay inside the fixture. */
static uint32_t random_word(uint32_t *seed)
{
    *seed ^= *seed << 13; *seed ^= *seed >> 17; *seed ^= *seed << 5;
    return *seed;
}

static void test_differential_blocks(bool native)
{
    current_test = native ? "JIT/reference differential execution" : "decoded/reference differential execution";
    jit_execution = false;
    cached_execution = true;
    decoded_execution = false;
    fixture reference, fast;
    fx_init(&reference); fx_init(&fast);
    r3900_decode_cache *cache = mrc_cpu_decode_cache_create();
    if (!cache) abort();
    r3900_jit *jit = native ? mrc_cpu_jit_create() : NULL;
    if (native && !jit) abort();
    uint32_t seed = 0x4d495053;
    static const unsigned ops[] = {9, 10, 11, 12, 13, 14, 15, 0x23, 0x2b, 4, 5};
    for (unsigned i = 0; i < 256; i++) {
        uint32_t x = random_word(&seed);
        unsigned rs = 1 + (x % 24), rt = 1 + ((x >> 5) % 24);
        unsigned op = ops[(x >> 10) % (sizeof(ops) / sizeof(ops[0]))];
        uint32_t word = I(op, rs, rt, x >> 16);
        if (op == 0x23 || op == 0x2b) word = I(op, 28, rt, (x & 63) * 4);
        if (op == 4 || op == 5) word = I(op, rs, rt, (x >> 24) & 3);
        if (i % 13 == 0) word = (rs << 21) | (rt << 16) | (rt << 11) | 0x21;
        if (i % 29 == 0) word = 0x0000000c; /* syscall */
        emit(&reference, word);
    }
    /* Exception handler resumes at changing EPC+4; exercises MFC0, JR and
     * RFE in the delay slot without a guest-specific exception shortcut. */
    emit_at(&reference, 0x80, 0x401a7000); /* mfc0 k0,EPC */
    emit_at(&reference, 0x84, I(9, 26, 26, 4));
    emit_at(&reference, 0x88, (26u << 21) | 8); /* jr k0 */
    emit_at(&reference, 0x8c, 0x42000010); /* rfe */
    for (unsigned i = 256; i < 264; i++) emit_at(&reference, CODE_PA + i*4, 0);
    emit_at(&reference, CODE_PA + 264*4, (2u << 26) | (CODE_PA >> 2));
    memcpy(fast.ram, reference.ram, TEST_RAM_SIZE);
    reference.cpu.r[28] = fast.cpu.r[28] = DATA_VA;
    for (unsigned batch = 0; batch < 2000; batch++) {
        if (batch % 31 == 0) {
            /* Unannounced host/DMA modification of possibly cached code. */
            unsigned offset = CODE_PA + (random_word(&seed) % 256) * 4;
            uint32_t word = I(9, 0, 3, random_word(&seed));
            emit_at(&reference, offset, word); emit_at(&fast, offset, word);
        }
        if (batch % 47 == 0) {
            reference.cpu.cp0[CP0_STATUS] |= SR_IEc | (1u << 10);
            fast.cpu.cp0[CP0_STATUS] |= SR_IEc | (1u << 10);
            mrc_cpu_set_irq(&reference.cpu, 2, true);
            mrc_cpu_set_irq(&fast.cpu, 2, true);
        } else {
            mrc_cpu_set_irq(&reference.cpu, 2, false);
            mrc_cpu_set_irq(&fast.cpu, 2, false);
        }
        for (unsigned j = 0; j < 7; j++) mrc_cpu_step(&reference.cpu);
        if (native) mrc_cpu_run_jit(&fast.cpu, 7, jit);
        else mrc_cpu_run_decoded(&fast.cpu, 7, cache);
        r3900 a = reference.cpu, b = fast.cpu;
        a.bus = b.bus = NULL; a.log = b.log = NULL;
        CHECK(memcmp(&a, &b, sizeof(a)) == 0, "CPU diverged at batch %u", batch);
        CHECK(memcmp(reference.ram, fast.ram, TEST_RAM_SIZE) == 0,
              "memory diverged at batch %u", batch);
        CHECK(reference.bus.reads == fast.bus.reads &&
              reference.bus.writes == fast.bus.writes &&
              reference.bus.faults == fast.bus.faults,
              "bus effects diverged at batch %u", batch);
        if (failures) break;
    }
    if (native) {
        CHECK(mrc_cpu_jit_native_count(jit) > 0, "differential test never ran native code");
        mrc_cpu_jit_report(jit, stdout);
    }
    mrc_cpu_jit_free(jit);
    mrc_cpu_decode_cache_free(cache);
    fx_free(&reference); fx_free(&fast);
}

static void test_jit_integer(void)
{
    current_test = "native integer emission and live code replacement";
    jit_execution = decoded_execution = false;
    fixture reference, fast;
    fx_init(&reference); fx_init(&fast);
    r3900_jit *jit = mrc_cpu_jit_create();
    if (!jit) abort();
    uint32_t seed = 0x4141524d;
    const unsigned special[] = {0,2,3,4,6,7,0x21,0x23,0x24,0x25,0x26,0x27,0x2a,0x2b};
    for (unsigned batch = 0; batch < 1000; batch++) {
        for (unsigned k = 1; k < 32; k++)
            reference.cpu.r[k] = fast.cpu.r[k] = random_word(&seed);
        /* Sixteen random integer instructions, then a jump back to the
         * start with an empty slot: an 18-instruction block. Budgets below
         * that take the reference path; larger ones run it natively and
         * finish on the reference path, so both engines are exercised
         * against each other from every instruction boundary. */
        for (unsigned k = 0; k < 16; k++) {
            uint32_t z = random_word(&seed);
            unsigned rs = z & 31, rt = (z >> 5) & 31, rd = (z >> 10) & 31;
            uint32_t word = k & 1 ? I(9 + (k % 7), rs, rt, z >> 16) :
                R(rs, rt, rd, (z >> 15) & 31, special[(k / 2 + batch) % 14]);
            emit_at(&reference, CODE_PA + k*4, word);
            emit_at(&fast, CODE_PA + k*4, word);
        }
        emit_at(&reference, CODE_PA + 64, J(2, CODE_VA));
        emit_at(&fast, CODE_PA + 64, J(2, CODE_VA));
        emit_at(&reference, CODE_PA + 68, NOP);
        emit_at(&fast, CODE_PA + 68, NOP);
        reference.cpu.pc = fast.cpu.pc = CODE_VA;
        reference.cpu.next_pc = fast.cpu.next_pc = CODE_VA + 4;
        reference.cpu.branch_pending = fast.cpu.branch_pending = false;
        unsigned budget = 1 + batch % 40;
        for (unsigned k = 0; k < budget; k++) mrc_cpu_step(&reference.cpu);
        mrc_cpu_run_jit(&fast.cpu, budget, jit);
        r3900 a = reference.cpu, b = fast.cpu;
        a.bus = b.bus = NULL; a.log = b.log = NULL;
        CHECK(!memcmp(&a, &b, sizeof(a)), "CPU mismatch at batch %u", batch);
        CHECK(reference.bus.reads == fast.bus.reads, "fetch count at batch %u", batch);
        if (failures) {
            printf("budget %u; code:", budget);
            for (unsigned k = 0; k < 18; k++)
                printf(" %08X", load_word(&reference, CODE_PA + k * 4));
            printf("\nreference:\n"); mrc_cpu_dump(&reference.cpu, stdout);
            printf("native:\n"); mrc_cpu_dump(&fast.cpu, stdout);
            break;
        }
    }
    CHECK(mrc_cpu_jit_native_count(jit) > 1000, "native emission not exercised");
    mrc_cpu_jit_report(jit, stdout);
    mrc_cpu_jit_free(jit);
    fx_free(&reference); fx_free(&fast);
}

/* Exercise native control flow with overwritten target registers, virtual
 * aliases, faults in interpreted delay slots, and a scheduler boundary after
 * the branch. Reuse the cache while changing code without notification. */
static void test_jit_branches(void)
{
    current_test = "native branch/delay differential execution";
    jit_execution = decoded_execution = false;
    fixture reference, fast;
    fx_init(&reference); fx_init(&fast);
    r3900_jit *jit = mrc_cpu_jit_create();
    if (!jit) abort();
    const uint32_t values[] = {0, 1, 0xffffffff, 0x80000000, 0x7fffffff};
    for (unsigned kind = 0; kind < 12; kind++)
    for (unsigned value = 0; value < 5; value++)
    for (unsigned variant = 0; variant < 16; variant++) {
        uint32_t pc = (variant & 1) ? 0xa0001000 : CODE_VA;
        unsigned prefix = (variant >> 1) & 1;
        bool fault = (variant & 4) != 0, split = (variant & 8) != 0;
        mrc_cpu_reset(&reference.cpu, pc); mrc_cpu_reset(&fast.cpu, pc);
        reference.cpu.cp0[CP0_STATUS] = fast.cpu.cp0[CP0_STATUS] = 0;
        reference.cpu.r[8] = fast.cpu.r[8] = kind >= 10 ? pc + 0x40 : values[value];
        reference.cpu.r[9] = fast.cpu.r[9] = 1;
        uint32_t word;
        if (kind < 4) word = I(4 + kind, 8, kind < 2 ? 9 : 0, -4);
        else if (kind < 8) word = I(1, 8, (kind & 1) | (kind >= 6 ? 16 : 0), 7);
        else if (kind < 10) word = J(kind == 8 ? 2 : 3, pc + 0x40);
        else word = R(8, 0, 8, 0, kind == 10 ? 8 : 9);
        emit_at(&reference, CODE_PA, ADDIU(10, 10, 1));
        emit_at(&reference, CODE_PA + prefix * 4, word);
        emit_at(&reference, CODE_PA + (prefix + 1) * 4,
                fault ? I(0x23, 0, 2, 1) : ADDIU(8, 31, 3));
        memcpy(fast.ram, reference.ram, TEST_RAM_SIZE);
        for (unsigned part = 0; part < (split ? 2u : 1u); part++) {
            unsigned budget = split ? (part ? 1 : prefix + 1) : prefix + 2;
            for (unsigned k = 0; k < budget; k++) mrc_cpu_step(&reference.cpu);
            mrc_cpu_run_jit(&fast.cpu, budget, jit);
            r3900 a = reference.cpu, b = fast.cpu;
            a.bus = b.bus = NULL; a.log = b.log = NULL;
            CHECK(!memcmp(&a, &b, sizeof(a)),
                  "kind %u value %u variant %u part %u", kind, value, variant, part);
            CHECK(reference.bus.reads == fast.bus.reads &&
                  reference.bus.faults == fast.bus.faults, "branch bus effects");
        }
    }
    CHECK(mrc_cpu_jit_native_count(jit) > 500, "native branches not exercised");
    mrc_cpu_jit_report(jit, stdout);
    mrc_cpu_jit_free(jit);
    fx_free(&reference); fx_free(&fast);
}

/* A device whose reads and writes move an interrupt line, so blocks that
 * touch it must stop for the interrupt check the reference loop makes. */
typedef struct { r3900 *cpu; uint32_t value; unsigned writes; } chain_device;
static uint32_t chain_read(void *ctx, uint32_t off, unsigned size)
{
    chain_device *d = ctx;
    mrc_cpu_set_irq(d->cpu, 2, false);
    return d->value++ + off;
}
static void chain_write(void *ctx, uint32_t off, unsigned size, uint32_t value)
{
    chain_device *d = ctx;
    d->value = value;
    if (++d->writes % 3 == 0) mrc_cpu_set_irq(d->cpu, 2, true);
}

/* Native blocks chain into each other across taken and untaken branches,
 * loads and stores of every width, multiply/divide, interrupts raised by
 * a device access in mid-chain, live code rewrites and budgets that cut
 * chains at every possible point. The reference stepper must agree on the
 * complete CPU state, memory and bus counters after every run call. */
static void test_jit_chains(void)
{
    current_test = "native block chaining differential execution";
    jit_execution = decoded_execution = false;
    cached_execution = true;
    fixture reference, fast;
    fx_init(&reference); fx_init(&fast);
    r3900_jit *jit = mrc_cpu_jit_create();
    if (!jit) abort();
    chain_device ref_dev = {&reference.cpu, 0, 0}, fast_dev = {&fast.cpu, 0, 0};
    mrc_bus_add_mmio(&reference.bus, "dev", 0x20000, 16, &ref_dev, chain_read, chain_write);
    mrc_bus_add_mmio(&fast.bus, "dev", 0x20000, 16, &fast_dev, chain_read, chain_write);
    static const uint32_t program[] = {
        /* 0x1000 L0: */
        0x8F880000, /* lw   t0, 0(gp)        */
        0x25080001, /* addiu t0, t0, 1       */
        0xAF880000, /* sw   t0, 0(gp)        */
        0x31090007, /* andi t1, t0, 7        */
        0x11200015, /* beq  t1, zero, L2     */
        0x00000000, /* nop                   */
        /* 0x1018 L1: */
        0x978A0004, /* lhu  t2, 4(gp)        */
        0x838B0006, /* lb   t3, 6(gp)        */
        0xA38B0007, /* sb   t3, 7(gp)        */
        0xA78A0008, /* sh   t2, 8(gp)        */
        0x010A0018, /* mult t0, t2           */
        0x00006012, /* mflo t4               */
        0x0109001A, /* div  t0, t1           */
        0x00006810, /* mfhi t5               */
        0x01AC6821, /* addu t5, t5, t4       */
        0x018D7026, /* xor  t6, t4, t5       */
        0x000E7A02, /* srl  t7, t6, 8        */
        0xAF8F000C, /* sw   t7, 12(gp)       */
        0x8F980010, /* lw   t8, 16(gp)       */
        0x030F7823, /* subu t7, t8, t7       */
        0x05E10002, /* bgez t7, L1b (skip 2) */
        0x2718FFFF, /* addiu t8, t8, -1      */
        0x2718FFFE, /* addiu t8, t8, -2      */
        /* 0x105C L1b: */
        0xAF980010, /* sw   t8, 16(gp)       */
        0x08000400, /* j    L0               */
        0x2529FFFF, /* addiu t1, t1, -1      */
        /* 0x1068 L2: */
        0x8E0E0000, /* lw   t6, 0(s0)   device */
        0xAE080004, /* sw   t0, 4(s0)   device */
        0x8F8C0014, /* lw   t4, 20(gp)       */
        0x258C0001, /* addiu t4, t4, 1       */
        0xAF8C0014, /* sw   t4, 20(gp)       */
        0x0C000406, /* jal  L1               */
        0x00000000, /* nop                   */
    };
    for (unsigned k = 0; k < sizeof(program) / sizeof(program[0]); k++)
        emit_at(&reference, CODE_PA + k * 4, program[k]);
    /* Interrupt handler: read the device (drops the line), return. */
    emit_at(&reference, 0x80, 0x8E1A0000);              /* lw k0, 0(s0)   */
    emit_at(&reference, 0x84, 0x401A7000);              /* mfc0 k0, EPC   */
    emit_at(&reference, 0x88, 0x03400008);              /* jr k0          */
    emit_at(&reference, 0x8C, 0x42000010);              /* rfe            */
    memcpy(fast.ram, reference.ram, TEST_RAM_SIZE);
    r3900 *cpus[2] = {&reference.cpu, &fast.cpu};
    for (unsigned k = 0; k < 2; k++) {
        cpus[k]->r[28] = DATA_VA;
        cpus[k]->r[16] = 0x80020000;
        cpus[k]->cp0[CP0_STATUS] = SR_IEc | (1u << 10);
    }
    uint32_t seed = 0x43484149;
    for (unsigned batch = 0; batch < 3000; batch++) {
        unsigned budget = 1 + random_word(&seed) % 300;
        if (batch % 97 == 96) {
            /* Rewrite a word both engines will run: the block guard must
             * catch it wherever the chain reaches it. */
            unsigned offset = CODE_PA + (random_word(&seed) % 6) * 4 + 0x18;
            uint32_t word = I(9, 8, 8, random_word(&seed) & 0xff);
            emit_at(&reference, offset, word); emit_at(&fast, offset, word);
        }
        for (unsigned j = 0; j < budget; j++) mrc_cpu_step(&reference.cpu);
        mrc_cpu_run_jit(&fast.cpu, budget, jit);
        r3900 a = reference.cpu, b = fast.cpu;
        a.bus = b.bus = NULL; a.log = b.log = NULL;
        CHECK(memcmp(&a, &b, sizeof(a)) == 0, "CPU diverged at batch %u", batch);
        CHECK(memcmp(reference.ram, fast.ram, TEST_RAM_SIZE) == 0,
              "memory diverged at batch %u", batch);
        CHECK(reference.bus.reads == fast.bus.reads &&
              reference.bus.writes == fast.bus.writes &&
              reference.bus.mmio_reads == fast.bus.mmio_reads &&
              reference.bus.mmio_writes == fast.bus.mmio_writes &&
              reference.bus.faults == fast.bus.faults,
              "bus effects diverged at batch %u", batch);
        if (failures) {
            printf("budget %u\nreference:\n", budget);
            mrc_cpu_dump(&reference.cpu, stdout);
            printf("native:\n");
            mrc_cpu_dump(&fast.cpu, stdout);
            break;
        }
    }
    CHECK(reference.cpu.irqs_taken > 100, "interrupts were not exercised");
    CHECK(mrc_cpu_jit_native_count(jit) > 100000, "chains not exercised");
    mrc_cpu_jit_report(jit, stdout);
    mrc_cpu_jit_free(jit);
    fx_free(&reference); fx_free(&fast);
}

int main(void)
{
  r3900_jit *probe = mrc_cpu_jit_create();
  bool native_available = probe != NULL;
  mrc_cpu_jit_free(probe);
#if (defined(__aarch64__) && !defined(__AARCH64EB__)) || (defined(__x86_64__) && !defined(_WIN32))
  if (!native_available) { fprintf(stderr, "native JIT unavailable on test host\n"); return 1; }
#endif
  for (unsigned mode = 0; mode < (native_available ? 4u : 3u); mode++) {
    jit_execution = mode == 3;
    cached_execution = mode != 0;
    decoded_execution = mode == 2;
    test_alu();
    test_r0_is_hardwired();
    test_branch_delay_slot();
    test_branch_not_taken_runs_delay_slot();
    test_jal_link_register();
    test_jalr_link_register();
    test_j_keeps_pc_region();
    test_big_endian_memory();
    test_store_sizes();
    test_lwl_lwr();
    test_swl_swr();
    test_unaligned_load_faults();
    test_exception_in_delay_slot();
    test_overflow();
    test_div_by_zero();
    test_interrupt_gating();
    test_rfe_restores_status();
    test_reserved_instruction_is_reported();
    test_cop1_is_unusable();
    test_bus_error_on_unmapped();
    test_live_instruction_writes();
    test_fetch_remap();
  }

    test_differential_blocks(false);
    if (native_available) {
        test_differential_blocks(true);
        test_jit_integer();
        test_jit_branches();
        test_jit_chains();
    }

    if (failures) {
        printf("\n%d check(s) failed\n", failures);
        return 1;
    }
    printf("all CPU semantics tests passed\n");
    return 0;
}
