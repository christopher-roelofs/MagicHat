#include "cpu/m68k/core/m68k_block.h"
#include "cpu/m68k/core/m68k_emit.h"
#include "cpu/m68k/core/m68k_emit_policy.h"
#include "jit/code_arena.h"
#include "util/portable.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

/*
 * A four-way table keyed by the address a block starts at. Four ways
 * because two pieces of code that alias to the same set are common and
 * three that do are not, and because a set this small is still one cache
 * line to search.
 */
/*
 * Eight thousand blocks, which is sixteen megabytes.
 *
 * Sized from what a machine actually needs rather than from a round
 * number: a PIC-2000 boot builds under twenty thousand blocks over six
 * hundred million instructions and evicts twelve thousand, against three
 * million entries, so the cache is not the cost. Four times this size
 * cuts the evictions to three hundred and changes the wall clock by
 * nothing measurable, and these machines run on tablets.
 */
#define SETS 2048
#define WAYS 4

typedef struct entry_s {
    uint32_t va;
    uint32_t bytes;            /* how far the block reaches                */
    uint16_t count;
    uint16_t used;             /* for choosing which way to replace        */
    uint16_t translated;       /* how many of them the emitter itself runs */
    uint32_t entries;          /* how often it has been run                */
#ifdef MRC_M68K_BLOCK_PROFILE
    uint64_t hits;
#endif
    bool     self_guarded;     /* the emitted form checks its own bytes    */
    bool     refused;          /* stops in front of what this core will not run */
    uint16_t refused_op;       /* and which instruction it stopped in front of */
    /*
     * Whichever block ran after this one last time.
     *
     * A block here averages under four instructions, so the cost of
     * finding the next one was larger than the cost of running it. A
     * branch usually goes where it went before, and following a pointer
     * and comparing one address is most of a hash, a four-way scan and a
     * bytes comparison. It is only a guess: the address is checked, and a
     * wrong guess falls back to the lookup with nothing lost.
     *
     * The entries live in a fixed array and are never freed, so this can
     * point at one that has since been rebuilt for a different address.
     * That is what the address check is for.
     */
    struct entry_s *next;
    m68k_code code;            /* emitted form, or NULL                    */
    /*
     * Where the emitted form goes when it is done, and where it can be
     * entered without its prologue. `link` is in the writable view and
     * `link_exec` the same place as it runs; `chain` is where another
     * block's link is pointed. `linked` is the address it currently goes
     * to, so a link is only rewritten when the successor changes.
     */
    uint8_t *link;
    const uint8_t *link_exec, *chain;
    uint32_t linked;
    uint8_t  source[M68K_BLOCK_MAX * MRC_JIT_M68K_MAX_BYTES];
    m68k_insn insn[M68K_BLOCK_MAX];
} entry;

struct m68k_blocks {
    entry slot[SETS * WAYS];
    uint16_t clock;
    m68k_block_stats stats;
    mrc_code_arena arena;
    bool emitting;
};

m68k_blocks *m68k_blocks_create(void)
{
    m68k_blocks *blocks = calloc(1, sizeof(m68k_blocks));
    if (!blocks) return NULL;
    /* Native code is an optimisation, so a host that will not give out
     * executable memory loses speed and nothing else. MRC_M68K_EMIT=0
     * turns it off for an exact comparison against the same run without. */
    const char *want = getenv("MRC_M68K_EMIT");
    blocks->emitting = !(want && !strcmp(want, "0")) && m68k_emit_supported() &&
                       mrc_code_arena_open(&blocks->arena, 16u << 20);
    return blocks;
}

void m68k_blocks_free(m68k_blocks *blocks)
{
    if (!blocks) return;
    if (blocks->emitting) mrc_code_arena_close(&blocks->arena);
    free(blocks);
}

/*
 * How many times a block has to run before it is worth compiling.
 *
 * Compiling costs more than interpreting a block once, so a block used
 * once and never again is pure loss, and code that walks forward through
 * memory without looping is nothing but such blocks. Waiting until a block
 * has proved it repeats turns that from a heavy loss into none. The number
 * only has to separate "runs once" from "runs in a loop", and any small
 * number does that.
 */
#define COMPILE_AFTER 16

/* Emit for a block, if there is room and anything to gain. */
static void compile(m68k *c, m68k_blocks *blocks, entry *e)
{
    e->code = NULL;
    e->translated = 0;
    e->self_guarded = false;
    if (!blocks->emitting) return;

    /*
     * A block guards itself against the bytes it was built from, which
     * needs a host pointer to them. Where the mapping cannot give one, the
     * caller keeps checking instead and the block is not compiled: a block
     * that cannot be checked from inside could never be jumped to
     * directly, which is the point of compiling it.
     */
    const uint8_t *guest = NULL;
    static int self_guard = -1;
    if (self_guard < 0) {
        const char *v = getenv("MRC_M68K_SELFGUARD");
        self_guard = !(v && !strcmp(v, "0"));
    }
    if (self_guard && c->bus.read_pages) {
        uint32_t offset = e->va & 4095u;
        if (offset + e->bytes <= 4096u) {
            const uint8_t *page = c->bus.read_pages[e->va >> M68K_PAGE_BITS];
            if (page) guest = page + offset;
        }
    }
    if (!guest && self_guard) return;
    /*
     * Room for the longest thing any one instruction can turn into, which
     * is a memory access: an address, a page lookup, the access itself,
     * the condition codes, and the interpreter call it falls back to.
     * Being generous here costs address space and nothing else; being
     * mean costs a failed emission that looks exactly like a full arena.
     */
    const size_t cap = 320 * (size_t)e->count + 256;
    /*
     * The decoded instructions and the bytes they came from go into the
     * arena ahead of the code, and the code points at those copies rather
     * than at this entry.
     *
     * Emitted blocks jump straight to one another, so a block's code can
     * be reached after the entry it was built in has been evicted and
     * rebuilt for another address. Its interpreter calls point at the
     * entry's decoded instructions; with the entry reused, they would run
     * whatever block now lives there -- which is exactly what happened,
     * as zeros in low RAM where the reference had written data. The code
     * and what it depends on have to share a lifetime, and the arena is
     * that lifetime: both go when it is reset, and nothing else frees
     * either.
     */
    const size_t insns_size = (size_t)e->count * sizeof e->insn[0];
    const size_t data = (insns_size + e->bytes + 15u) & ~(size_t)15u;
    uint8_t *exec = NULL;
    uint8_t *out = mrc_code_arena_reserve(&blocks->arena, data + cap, &exec);
    if (!out) {
        /* Genuinely full: drop everything, because every pointer into the
         * arena is about to stop meaning anything. */
        for (unsigned k = 0; k < SETS * WAYS; k++) {
            blocks->slot[k].code = NULL;
            blocks->slot[k].link = NULL;
            blocks->slot[k].linked = 0;
            if (&blocks->slot[k] != e) blocks->slot[k].count = 0;
        }
        mrc_code_arena_reset(&blocks->arena);
        blocks->stats.arena_resets++;
        out = mrc_code_arena_reserve(&blocks->arena, data + cap, &exec);
        if (!out) return;
    }
    memcpy(out, e->insn, insns_size);
    memcpy(out + insns_size, e->source, e->bytes);
    const m68k_insn *insn_copy = (const m68k_insn *)exec;
    const uint8_t *expect_copy = exec + insns_size;
    unsigned translated = 0;
    m68k_emit_points points = { 0, 0 };
    size_t bytes = m68k_emit(out + data, exec + data, cap, insn_copy,
                             e->count, e->va, guest, expect_copy, e->bytes,
                             c->count_cycles, &translated, &points, e);
    /* No code for this block is an ordinary outcome, not an arena
     * problem: it runs interpreted and nothing is thrown away for it. */
    if (!bytes) return;
    mrc_code_arena_commit(&blocks->arena, out, data + bytes);
    e->code = (m68k_code)(exec + data);
    e->translated = (uint16_t)translated;
    e->self_guarded = guest != NULL;
    e->link = out + data + points.link;
    e->link_exec = exec + data + points.link;
    e->chain = exec + data + points.chain;
    e->linked = 0;
    blocks->stats.compiled++;
    blocks->stats.emitted_insns += e->count;
    blocks->stats.translated += translated;
}

void m68k_blocks_flush(m68k_blocks *blocks)
{
    for (unsigned k = 0; k < SETS * WAYS; k++) {
        blocks->slot[k].count = 0;
        blocks->slot[k].refused = false;
        blocks->slot[k].code = NULL;
        blocks->slot[k].link = NULL;
        blocks->slot[k].linked = 0;
        blocks->slot[k].next = NULL;
    }
    /*
     * And the arena with it. Emitted blocks jump to one another directly,
     * so code left behind is not merely unreachable: it is reachable from
     * whatever still points at it. Dropping all of it together is the only
     * state in which none of it points at anything.
     */
    if (blocks->emitting) {
        mrc_code_arena_reset(&blocks->arena);
        blocks->stats.arena_resets++;
    }
}

void m68k_blocks_report(const m68k_blocks *blocks, m68k_block_stats *out)
{
    *out = blocks->stats;
#ifdef MRC_M68K_BLOCK_PROFILE
    if (getenv("MRC_M68K_BLOCK_PROFILE")) {
        const entry *top[12] = {0};
        for (unsigned k = 0; k < SETS * WAYS; k++) {
            const entry *e = &blocks->slot[k];
            if (!e->hits) continue;
            for (unsigned i = 0; i < 12; i++)
                if (!top[i] || e->hits > top[i]->hits) {
                    for (unsigned j = 11; j > i; j--) top[j] = top[j - 1];
                    top[i] = e;
                    break;
                }
        }
        fprintf(stderr, "block profile: address, entries, instructions, "
                "last op, branch target\n");
        for (unsigned i = 0; i < 12 && top[i]; i++)
            fprintf(stderr, "  %08X %10llu  %2u insns  last %-8s -> %08X\n",
                    top[i]->va, (unsigned long long)top[i]->hits,
                    top[i]->count,
                    top[i]->count ? m68k_op_name(top[i]->insn[top[i]->count - 1].op) : "-",
                    top[i]->count ? top[i]->insn[top[i]->count - 1].extra : 0);
    }
#endif
}

void m68k_blocks_describe(const m68k_blocks *blocks, uint32_t pc,
                          m68k_block_view *out)
{
    memset(out, 0, sizeof *out);
    unsigned set = ((pc >> 1) ^ (pc >> 13)) % SETS * WAYS;
    for (unsigned k = set; k < set + WAYS; k++) {
        const entry *e = &blocks->slot[k];
        if (!e->count || e->va != pc) continue;
        out->found = true;
        out->va = e->va;
        out->bytes = e->bytes;
        out->count = e->count;
        out->entries = e->entries;
        out->compiled = e->code != NULL;
        out->first_word = e->insn[0].word;
        out->first_op = e->insn[0].op;
        return;
    }
}

/*
 * Read the bytes a block covers, as the guest holds them now. Everything
 * goes through the ordinary read path: a block may only be built from, and
 * checked against, memory that can be read without consequence, and the
 * caller has already established that by getting here.
 */
static void read_bytes(m68k *c, uint32_t va, uint32_t bytes, uint8_t *out)
{
    /* Straight from the host copy where the mapping says the whole span
     * is ordinary memory, which is the usual case for code. */
    if (c->bus.read_pages) {
        uint32_t offset = va & (4096u - 1);
        if (offset + bytes <= 4096u) {
            const uint8_t *page = c->bus.read_pages[va >> M68K_PAGE_BITS];
            if (page) { memcpy(out, page + offset, bytes); return; }
        }
    }
    for (uint32_t k = 0; k < bytes; k += 2) {
        uint32_t w = c->bus.read(c->bus.ctx, va + k, 2);
        out[k] = (uint8_t)(w >> 8);
        out[k + 1] = (uint8_t)w;
    }
}

/*
 * Decode forward from `va` until something ends the block. The bytes are
 * fetched a word at a time, so a block never reads past its own last
 * instruction: what follows a short instruction can be a device register,
 * and reading one has consequences whether or not the value is wanted.
 */
static void build(m68k *c, uint32_t va, entry *e)
{
    e->va = va;
    e->count = 0;
    e->bytes = 0;
    e->refused = false;
    e->next = NULL;
    while (e->count < M68K_BLOCK_MAX) {
        uint32_t at = e->bytes;
        /* The owner is watching for this address, so the block ends in
         * front of it and the runner returns there. */
        if (e->count && c->stop_pc == va + at) break;
        unsigned have = 0;
        m68k_insn insn;
        bool decoded = false;
        while (have < MRC_JIT_M68K_MAX_BYTES &&
               at + have + 2 <= sizeof e->source) {
            uint32_t w = c->bus.read(c->bus.ctx, va + at + have, 2);
            e->source[at + have] = (uint8_t)(w >> 8);
            e->source[at + have + 1] = (uint8_t)w;
            have += 2;
            if (m68k_decode(e->source + at, have, va + at, &insn) &&
                insn.length <= have) { decoded = true; break; }
        }
        if (!decoded) break;
        /* Under a machine, stop in front of what this core cannot run so
         * the caller can run it somewhere else. The block keeps whatever
         * it decoded before this point. */
        /*
         * MOVES reaches memory through the alternate function codes, and
         * this bus interface carries no function code at all: under a
         * machine that decodes CPU space, a plain access here would go to
         * the wrong address space entirely. Whatever owns the machine
         * drives those pins, so it runs this one.
         */
        /*
         * RTE used to be handed back here, because when this was written
         * the interpreter had no model of the twenty-four byte format C
         * frame a CPU32 builds for a fetch from an odd address. It grew
         * one -- formats 0, 1, 2 and C, and a format error for anything
         * else -- and nobody came back to lift the refusal. The note left
         * behind said handing every one back "costs a handover per
         * exception taken, which is nothing", and that was the expensive
         * part of the mistake: on a PIC-2000 resuming into the UI it was
         * 2,417,734 handovers in 300 million instructions, every single
         * refusal in the run. Lifting it leaves 29.
         */
        if (c->hand_back && !m68k_implemented(insn.op)) {
            e->refused = true;
            e->refused_op = (uint16_t)insn.op;
            /*
             * With nothing decoded ahead of it the entry covers no bytes,
             * and an entry that covers no bytes cannot notice the guest
             * replacing the instruction underneath it. Cover this one, so
             * the refusal is rechecked if the code changes and so the
             * decode is not repeated every time the machine arrives here.
             */
            if (!e->count) e->bytes = insn.length;
            break;
        }
        e->insn[e->count++] = insn;
        e->bytes = at + insn.length;
        /*
         * A block stops where straight-line execution does.
         *
         * It does not stop merely because an instruction might trap. An
         * earlier version did, and it cut the average block to under two
         * instructions for no benefit: the runner already checks after
         * every instruction whether the program counter went somewhere
         * other than the next one, which is exactly what a trap looks like
         * from here. Ending the block as well would only be paying twice
         * for the same guarantee.
         */
        if (insn.flags & (M68K_F_ENDS_BLOCK | M68K_F_ILLEGAL))
            break;
    }
}

uint64_t m68k_run_blocks(m68k *c, uint64_t budget, m68k_blocks *blocks)
{
    uint64_t done = 0;
    entry *prev = NULL;

    while (done < budget) {
        if (c->yield) return done;
        /* LPSTOP has retired and the part is waiting for an interrupt.
         * Nothing more will be fetched until the owner delivers one. */
        if (c->stopped) return done;
        /* The address the owner asked to be stopped in front of. It has
         * not run, and the owner is the one that decides what does. */
        if (MRC_UNLIKELY(c->stop_pc != 0) && c->pc == c->stop_pc) return done;
        if (c->pc & 1) {                      /* the interpreter's business */
            /* An odd program counter is an address error, and a machine
             * has its own frame format for one. Hand it over. */
            if (c->hand_back) return done;
            if (!m68k_step(c)) return done;
            done++;
            continue;
        }

        entry *chosen = NULL, *victim = NULL;
        bool linked = false;
        if (prev && prev->next && prev->next->va == c->pc &&
            (prev->next->count || prev->next->refused)) {
            chosen = prev->next;
            linked = true;
            blocks->stats.linked++;
        } else {
            unsigned set = ((c->pc >> 1) ^ (c->pc >> 13)) % SETS * WAYS;
            victim = &blocks->slot[set];
            for (unsigned k = set; k < set + WAYS; k++) {
                entry *e = &blocks->slot[k];
                if ((e->count || e->refused) && e->va == c->pc) { chosen = e; break; }
                if ((!e->count && !e->refused) || e->used < victim->used) victim = e;
            }
        }

        /*
         * A compiled block checks its own bytes, so the check here would
         * be a second one. It is still needed whenever the block is about
         * to be interpreted instead, which happens when the budget cannot
         * cover the whole of it: skipping it in that case ran code that
         * had been overwritten, and the ROM comparison found it nine
         * million instructions in.
         */
        bool native_now = chosen && chosen->code &&
                          budget - done >= chosen->count;
        if (chosen && !native_now) {
            /*
             * The bytes this block was built from, as they are now. A
             * guest that rewrote its own code, a loader that dropped new
             * code in, or a restored state all show up here, without
             * anything having had to announce itself. A compiled block
             * does this check itself and is left alone here.
             */
            uint8_t live[sizeof chosen->source];
            read_bytes(c, chosen->va, chosen->bytes, live);
            if (memcmp(live, chosen->source, chosen->bytes) != 0) {
                blocks->stats.stale++;
                chosen->count = 0;
                chosen->code = NULL;
                chosen->link = NULL;
                chosen->linked = 0;
                chosen->refused = false;
                chosen = NULL;
                if (linked) {
                    /* The guess was stale. Fall back to the lookup, which
                     * needs the victim this branch did not choose. */
                    unsigned set = ((c->pc >> 1) ^ (c->pc >> 13)) % SETS * WAYS;
                    victim = &blocks->slot[set];
                    for (unsigned k = set; k < set + WAYS; k++) {
                        entry *e = &blocks->slot[k];
                        if ((e->count || e->refused) && e->va == c->pc) {
                            chosen = e; break;
                        }
                        if ((!e->count && !e->refused) ||
                            e->used < victim->used) victim = e;
                    }
                    linked = false;
                }
            }
        }

        if (!chosen) {
            chosen = victim;
            if (chosen->count || chosen->refused) blocks->stats.evicted++;
            build(c, c->pc, chosen);
            blocks->stats.built++;
            chosen->entries = 0;
            if (!chosen->count) {            /* nothing this core runs here */
                if (chosen->refused) { chosen->used = ++blocks->clock;
                                       blocks->stats.refused++;
                                       if (chosen->refused_op < M68K_OP_COUNT)
                                           blocks->stats.refused_op[chosen->refused_op]++;
                                       return done; }
                if (!m68k_step(c)) return done;
                done++;
                continue;
            }
        }
        if (prev && !linked) prev->next = chosen;
        if (!chosen->count) {   /* a cached refusal: the caller's business */
            if (chosen->refused_op < M68K_OP_COUNT)
                blocks->stats.refused_op[chosen->refused_op]++;
            chosen->used = ++blocks->clock;
            blocks->stats.refused++;
            return done;
        }
        /*
         * Point the block that just ran at this one, so next time round
         * it goes there without coming back here.
         *
         * Only where both are compiled, only when the successor has
         * changed -- a link that is already right costs nothing to leave
         * alone -- and never while the owner is watching for an address,
         * because a chain jumps straight past the check for it below.
         */
        /*
         * A block has asked to be pointed at whoever runs next, and this
         * is whoever runs next. Never while the owner is watching for an
         * address, because a chain jumps straight past the check for it.
         */
        if (MRC_UNLIKELY(c->link_request != NULL)) {
            entry *from = (entry *)c->link_request;
            c->link_request = NULL;
            /*
             * Only to a block that checks its own bytes. One that cannot
             * -- its source is not one uniform host-backed span, so there
             * is no pointer to compare against -- is checked by the loop
             * below instead, and a chain jumps straight past that. The
             * guest rewrites the trampolines in low RAM, so this is not
             * hypothetical: without it a PIC-2000 diverges within twenty
             * million instructions, in those very bytes.
             */
            /* MRC_M68K_LINK=0 keeps every block returning here, for an
             * exact comparison against the same run without chaining. */
            static int linking = -1;
            if (linking < 0) {
                const char *v = getenv("MRC_M68K_LINK");
                linking = !(v && !strcmp(v, "0"));
            }
            if (linking && chosen->code && chosen->self_guarded &&
                from->link && !c->stop_pc) {
                m68k_patch_link(from->link, from->link_exec, chosen->chain);
                if (from->linked) blocks->stats.relinked++;
                else blocks->stats.chained++;
                from->linked = chosen->va;
            }
        }
        prev = chosen;
        chosen->used = ++blocks->clock;
        blocks->stats.entered++;
#ifdef MRC_M68K_BLOCK_PROFILE
        chosen->hits++;
#endif
        if (++chosen->entries == COMPILE_AFTER) compile(c, blocks, chosen);

        /* Emitted code runs the whole block or none of it, so it is only
         * entered when the budget can cover the whole of it. */
        if (chosen->code && budget - done >= chosen->count) {
            static int paranoid = -1;
            if (paranoid < 0) paranoid = getenv("MRC_M68K_PARANOID") != NULL;
            if (paranoid) {
                uint8_t live[sizeof chosen->source];
                read_bytes(c, chosen->va, chosen->bytes, live);
                if (chosen->va != c->pc)
                    fprintf(stderr, "block %08X entered at %08X\n",
                            chosen->va, c->pc);
                if (memcmp(live, chosen->source, chosen->bytes))
                    fprintf(stderr, "block %08X: source differs from memory, "
                            "%u bytes, first insn %04X\n", chosen->va,
                            chosen->bytes, chosen->insn[0].word);
            }
            uint32_t retired = chosen->code(c, (uint32_t)(budget - done));
            if (!retired) {          /* its own guard refused it */
                blocks->stats.stale++;
                chosen->count = 0;
                chosen->code = NULL;
                chosen->link = NULL;
                chosen->linked = 0;
                chosen->self_guarded = false;
                continue;
            }
            done += retired;
            blocks->stats.instructions += retired;
            blocks->stats.ran_native += retired;
            if (retired < chosen->count || c->exception) return done;
            continue;
        }

        uint32_t next = chosen->va;
        for (unsigned k = 0; k < chosen->count; k++) {
            if (done == budget) return done;
            /* The block is a straight line by construction, so anything
             * that moved the program counter elsewhere ends it. */
            if (c->pc != next) break;
            const m68k_insn *insn = &chosen->insn[k];
            next += insn->length;
            bool ok = m68k_execute(c, insn);
            done++;
            blocks->stats.instructions++;
            if (!ok || c->yield) return done;
        }
    }
    return done;
}

unsigned m68k_blocks_refusals(const m68k_block_stats *st,
                              m68k_refusal *out, unsigned max)
{
    unsigned n = 0;
    for (unsigned op = 0; op < M68K_OP_COUNT; op++) {
        if (!st->refused_op[op]) continue;
        uint64_t count = st->refused_op[op];
        unsigned at = n < max ? n : max;
        while (at && out[at - 1].count < count) {
            if (at < max) out[at] = out[at - 1];
            at--;
        }
        if (at < max) {
            out[at].name = m68k_op_name(op);
            out[at].count = count;
            if (n < max) n++;
        }
    }
    return n;
}
