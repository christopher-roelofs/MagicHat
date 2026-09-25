/*
 * test_m68k_emit.cpp — the emitter against the interpreter.
 *
 * This compares the core against itself: an instruction run as generated
 * code, and the same
 * instruction from the same state run by the interpreter, have to leave
 * identical registers, flags and memory. That is the property the block
 * engine rests on, and until now nothing checked it directly -- it was
 * inferred from a ROM comparison, which only covers the instructions that
 * ROM happens to execute, in the states it happens to reach.
 *
 * The list of what to test is not written here. It is whatever
 * `m68k_emit_policy.h` says the emitter translates, found by sweeping the
 * whole opcode space, so a family added to the emitter is covered the
 * moment it is added and a family quietly dropped shows up as a drop in
 * the count rather than as silence.
 */
extern "C" {
#include "cpu/m68k/core/m68k.h"
#include "cpu/m68k/core/m68k_emit.h"
#include "cpu/m68k/core/m68k_emit_policy.h"
#include "jit/code_arena.h"
}

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace {

constexpr uint32_t MEM_SIZE = 0x10000;
constexpr uint32_t MEM_MASK = MEM_SIZE - 1;
constexpr uint32_t CODE_AT  = 0x2000;
constexpr uint32_t PAGES    = MEM_SIZE / 4096;

uint8_t mem_a[MEM_SIZE], mem_b[MEM_SIZE], pristine[MEM_SIZE];

uint32_t bus_read(void *ctx, uint32_t addr, unsigned size)
{
    const uint8_t *m = (const uint8_t *)ctx;
    uint32_t v = 0;
    for (unsigned k = 0; k < size; k++) v = v << 8 | m[(addr + k) & MEM_MASK];
    return v;
}
void bus_write(void *ctx, uint32_t addr, unsigned size, uint32_t value)
{
    uint8_t *m = (uint8_t *)ctx;
    for (unsigned k = 0; k < size; k++)
        m[(addr + size - 1 - k) & MEM_MASK] = (uint8_t)(value >> (k * 8));
}

/*
 * One host pointer per 4 KiB of the guest's whole address space, as the
 * machine gives the core. Only the pages the test's memory covers are
 * mapped; everything else is NULL, which is how a device looks from inside
 * generated code and sends it down the slow path.
 */
uint8_t **make_pages(uint8_t *mem)
{
    uint8_t **t = (uint8_t **)calloc(M68K_PAGE_COUNT, sizeof *t);
    for (uint32_t p = 0; p < PAGES; p++) t[p] = mem + p * 4096;
    return t;
}

uint32_t seed = 0x68020;
uint32_t rnd()
{
    seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5;
    return seed;
}

int failures = 0;
struct Group { unsigned count; uint16_t word; std::string detail; };
std::map<std::string, Group> groups;

void fail(const char *op, uint16_t word, const std::string &detail)
{
    failures++;
    auto it = groups.find(op);
    if (it == groups.end()) groups[op] = Group{1, word, detail};
    else it->second.count++;
}

std::string difference(const m68k &a, const m68k &b)
{
    char buf[160];
    for (unsigned k = 0; k < 8; k++) {
        if (a.d[k] != b.d[k]) {
            snprintf(buf, sizeof buf, "D%u %08X emitted, %08X interpreted",
                     k, a.d[k], b.d[k]);
            return buf;
        }
        if (a.a[k] != b.a[k]) {
            snprintf(buf, sizeof buf, "A%u %08X emitted, %08X interpreted",
                     k, a.a[k], b.a[k]);
            return buf;
        }
    }
    if (a.pc != b.pc) {
        snprintf(buf, sizeof buf, "pc %08X emitted, %08X interpreted",
                 a.pc, b.pc);
        return buf;
    }
    /*
     * And the cycle count, which the Envoy's battery RAM reads. Emitted
     * code adds what the encoding decided; the two instructions whose cost
     * also depends on an operand -- a branch taken, a loop whose counter
     * expires -- have to add their extra from inside the emitted form,
     * which is the part of this that can be got wrong.
     */
    if (a.cycles != b.cycles) {
        snprintf(buf, sizeof buf, "%llu cycles emitted, %llu interpreted",
                 (unsigned long long)a.cycles, (unsigned long long)b.cycles);
        return buf;
    }
    static const struct { const char *name; size_t off; } flags[] = {
        {"N", offsetof(m68k, n)}, {"Z", offsetof(m68k, z)},
        {"V", offsetof(m68k, v)}, {"C", offsetof(m68k, c)},
        {"X", offsetof(m68k, x)},
    };
    for (auto &f : flags) {
        uint8_t x = *((const uint8_t *)&a + f.off);
        uint8_t y = *((const uint8_t *)&b + f.off);
        if (x != y) {
            snprintf(buf, sizeof buf, "%s %u emitted, %u interpreted",
                     f.name, x, y);
            return buf;
        }
    }
    if (a.insn_count != b.insn_count) {
        snprintf(buf, sizeof buf, "instruction count %llu emitted, %llu interpreted",
                 (unsigned long long)a.insn_count,
                 (unsigned long long)b.insn_count);
        return buf;
    }
    if (a.exception != b.exception) {
        snprintf(buf, sizeof buf, "exception %u emitted, %u interpreted",
                 a.exception, b.exception);
        return buf;
    }
    if (memcmp(mem_a, mem_b, MEM_SIZE) != 0) {
        for (uint32_t k = 0; k < MEM_SIZE; k++)
            if (mem_a[k] != mem_b[k]) {
                snprintf(buf, sizeof buf,
                         "memory at %04X: %02X emitted, %02X interpreted",
                         k, mem_a[k], mem_b[k]);
                return buf;
            }
    }
    return "";
}

} // namespace

/*
 * Two blocks, pointed at one another, run as one.
 *
 * The single-instruction comparisons above enter a block from C with a
 * budget of one, so they never take the link at the end of it. This is the
 * other half: a loop compiled as a block, linked to itself, and left to go
 * round inside the generated code. What it checks is the arithmetic the
 * caller used to do -- the block stopping itself when the budget runs out,
 * and the retired count being the total for the whole chain rather than
 * for the last block through.
 */
int chaining(mrc_code_arena &arena, uint8_t **pages_a)
{
    /* ADDQ.L #1,D0 ; BRA back to the ADDQ. Two instructions, one block. */
    static const uint8_t code_bytes[] = { 0x52, 0x80, 0x60, 0xFC };
    memset(mem_a, 0, MEM_SIZE);
    memcpy(mem_a + CODE_AT, code_bytes, sizeof code_bytes);

    m68k_insn insn[2];
    unsigned at = 0;
    for (unsigned k = 0; k < 2; k++) {
        unsigned len = m68k_decode(mem_a + CODE_AT + at, 16, CODE_AT + at,
                                   &insn[k]);
        if (!len) { printf("FAIL: chaining fixture did not decode\n"); return 1; }
        at += len;
    }
    if (insn[1].extra != CODE_AT) {
        printf("FAIL: chaining fixture does not branch to itself\n");
        return 1;
    }

    uint8_t *exec = nullptr;
    uint8_t *out = mrc_code_arena_reserve(&arena, 4096, &exec);
    if (!out) { printf("FAIL: no room for the chaining fixture\n"); return 1; }
    unsigned native = 0;
    m68k_emit_points points = { 0, 0 };
    size_t bytes = m68k_emit(out, exec, 4096, insn, 2, CODE_AT,
                             nullptr, nullptr, 0, false, &native, &points,
                             nullptr);
    if (!bytes) { printf("FAIL: the chaining fixture did not compile\n"); return 1; }
    mrc_code_arena_commit(&arena, out, bytes);
    /* Its own successor, which is what a loop is. */
    if (!mrc_code_arena_unlock(&arena, out + points.link, 4)) {
        printf("FAIL: the chaining fixture could not be patched\n"); return 1;
    }
    m68k_patch_link(out + points.link, exec + points.link, exec + points.chain);
    mrc_code_arena_relock(&arena, out + points.link, 4);

    int failures = 0;
    for (unsigned budget = 1; budget <= 9; budget++) {
        m68k c;
        memset(&c, 0, sizeof c);
        c.pc = CODE_AT;
        c.supervisor = true;
        c.interrupt_mask = 7;
        c.bus = m68k_bus{ bus_read, bus_write, mem_a, pages_a, pages_a };
        uint32_t retired = ((m68k_code)exec)(&c, budget);
        /* Whole passes of a two-instruction block only. */
        const uint32_t want = budget - (budget % 2);
        if (retired != want || c.d[0] != want / 2 ||
            c.pc != (want ? CODE_AT : CODE_AT)) {
            printf("FAIL: budget %u retired %u (wanted %u), D0 %u\n",
                   budget, retired, want, c.d[0]);
            failures++;
        }
    }
    if (!failures) printf("chained blocks run inside the generated code\n");
    return failures ? 1 : 0;
}

int main(int argc, char **argv)
{
    unsigned rounds = argc > 1 ? (unsigned)atoi(argv[1]) : 24;

    if (!m68k_emit_supported()) {
        printf("no emitter for this host; nothing to compare\n");
        return 0;
    }
    mrc_code_arena arena;
    if (!mrc_code_arena_open(&arena, 4u << 20)) {
        printf("no executable memory on this host; nothing to compare\n");
        return 0;
    }

    uint8_t **pages_a = make_pages(mem_a);
    uint8_t **pages_b = make_pages(mem_b);

    /*
     * Every opcode the policy says is translated, found by asking it. The
     * extension words are pseudo-random but fixed per opcode, so a failure
     * names one instruction that can be reproduced.
     */
    struct Form { uint16_t word; uint8_t bytes[MRC_JIT_M68K_MAX_BYTES]; };
    std::vector<Form> forms;
    for (uint32_t w = 0; w < 0x10000; w++) {
        Form f;
        f.word = (uint16_t)w;
        f.bytes[0] = (uint8_t)(w >> 8);
        f.bytes[1] = (uint8_t)w;
        uint32_t s = w * 2654435761u;
        for (unsigned k = 2; k < sizeof f.bytes; k++) {
            s = s * 1103515245u + 12345u;
            f.bytes[k] = (uint8_t)(s >> 16);
        }
        m68k_insn insn;
        if (!m68k_decode(f.bytes, sizeof f.bytes, CODE_AT, &insn)) continue;
        if (insn.length > sizeof f.bytes) continue;
        if (!m68k_translatable(&insn) && !m68k_inline_memory(&insn)) continue;
        forms.push_back(f);
    }
    printf("%zu opcodes are translated by this build's emitter\n", forms.size());
    if (forms.empty()) {
        printf("FAIL: the emitter claims to translate nothing\n");
        return 1;
    }

    unsigned compared = 0, refused = 0;
    for (const Form &f : forms) {
        m68k_insn insn;
        m68k_decode(f.bytes, sizeof f.bytes, CODE_AT, &insn);

        /* One instruction, compiled on its own, with no guard: the guard is
         * about the guest rewriting itself and has its own coverage. */
        uint8_t *exec = nullptr;
        uint8_t *out = mrc_code_arena_reserve(&arena, 320 * 4 + 256, &exec);
        if (!out) { mrc_code_arena_reset(&arena); continue; }
        unsigned native = 0;
        size_t bytes = m68k_emit(out, exec, 320 * 4 + 256, &insn, 1, CODE_AT,
                                 nullptr, nullptr, 0, true, &native, nullptr, nullptr);
        if (!bytes) { refused++; continue; }
        mrc_code_arena_commit(&arena, out, bytes);
        m68k_code code = (m68k_code)exec;

        for (unsigned round = 0; round < rounds; round++) {
            for (uint32_t k = 0; k < MEM_SIZE; k++) pristine[k] = (uint8_t)rnd();
            memcpy(pristine + CODE_AT, f.bytes, sizeof f.bytes);

            m68k start;
            memset(&start, 0, sizeof start);
            for (unsigned k = 0; k < 8; k++) {
                start.d[k] = rnd();
                /* Address registers point into the test's memory often
                 * enough that the mapped fast path is what usually runs,
                 * and outside it often enough that the fall back to the
                 * interpreter is exercised too. */
                start.a[k] = (round & 1) ? rnd() : (rnd() & (MEM_MASK & ~1u));
            }
            /*
             * The stack is inside the mapped memory most of the time, so
             * that the inline call and return run their fast path, and
             * outside it the rest of the time, so that the fall back to
             * the interpreter is exercised. That second case is the one
             * that matters: a fast path which moved the stack pointer
             * before discovering it could not finish would move it twice.
             */
            start.a[7] = (round % 4 == 3) ? (0x40000000u | (rnd() & 0xFFF0u))
                                          : 0x8000;
            start.ssp = start.a[7];
            start.supervisor = true;
            start.interrupt_mask = 7;
            start.n = rnd() & 1; start.z = rnd() & 1; start.v = rnd() & 1;
            start.c = rnd() & 1; start.x = rnd() & 1;
            start.pc = CODE_AT;
            start.insn_count = 1000;
            start.cycles = 5000;
            start.count_cycles = true;

            m68k emitted = start, interpreted = start;
            memcpy(mem_a, pristine, MEM_SIZE);
            memcpy(mem_b, pristine, MEM_SIZE);

            m68k_bus bus_emitted = { bus_read, bus_write, mem_a,
                                     pages_a, pages_a };
            /* The interpreter never reads the mappings; it is given them so
             * that the only difference between the two runs is which
             * engine ran the instruction. */
            m68k_bus bus_plain = { bus_read, bus_write, mem_b, pages_b, pages_b };
            emitted.bus = bus_emitted;
            interpreted.bus = bus_plain;

            /* A budget of one, so a block that can loop does not: this
             * compares one instruction at a time. The looping is covered
             * by block execution checks, which run whole blocks. */
            uint32_t retired = code(&emitted, 1);
            bool ok = m68k_execute(&interpreted, &insn);
            (void)ok;

            if (retired != 1) {
                char buf[96];
                snprintf(buf, sizeof buf, "the block retired %u, not 1", retired);
                fail(m68k_op_name(insn.op), f.word, buf);
                break;
            }
            /*
             * The program counter of the instruction before this one is
             * not compared. Generated code records it only for branches,
             * which are the only instructions that can leave the program
             * counter at an odd address, and an odd address is the only
             * thing that ever reads it.
             */
            emitted.prev_pc = interpreted.prev_pc;
            emitted.bus = interpreted.bus;
            std::string d = difference(emitted, interpreted);
            if (!d.empty()) { fail(m68k_op_name(insn.op), f.word, d); break; }
            compared++;
        }
    }

    printf("%u single-instruction comparisons over %zu opcodes, "
           "%u the emitter declined\n", compared, forms.size(), refused);
    for (const auto &g : groups)
        printf("FAIL: %s disagreed %u times, first at opcode %04X: %s\n",
               g.first.c_str(), g.second.count, g.second.word,
               g.second.detail.c_str());
    if (chaining(arena, pages_a)) failures++;
    if (!failures) printf("all 68k emitter checks passed\n");
    mrc_code_arena_close(&arena);
    return failures ? 1 : 0;
}
