/*
 * code_arena.h — executable memory for generated code.
 *
 * One shared-memory object mapped twice: a writable view an emitter fills
 * and an executable view the processor runs. No page is ever writable and
 * executable at once, and no protection change happens per block, which is
 * what the per-block mprotect pair in the first MIPS engine cost as much
 * system time as the whole run.
 *
 * Where that is unavailable, because the kernel is old or a sandbox
 * refuses an executable file mapping, one anonymous mapping is used and
 * the window being written is made writable and put back before anything
 * runs from it. That is slower per compiled block and identical in every
 * other respect.
 */
#ifndef MH_CODE_ARENA_H
#define MH_CODE_ARENA_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint8_t *exec;       /* where generated code runs                  */
    uint8_t *write;      /* where it is written; equal when not dual   */
    size_t   size, used, page;
    bool     dual;
    uint64_t flushes;
} mh_code_arena;

/* Returns false when this host will not give out executable memory. */
bool mh_code_arena_open(mh_code_arena *arena, size_t size);
void mh_code_arena_close(mh_code_arena *arena);

/*
 * Reserve space for up to `most` bytes and hand back where to write them.
 * `*exec_at` receives the address the same bytes will run at, which is a
 * different address when the arena is dual-mapped and the emitter needs
 * both to resolve its own branches. Returns NULL when the arena is full;
 * the caller should discard what it has cached and call
 * mh_code_arena_reset before trying again.
 */
uint8_t *mh_code_arena_reserve(mh_code_arena *arena, size_t most,
                                uint8_t **exec_at);

/* Commit `bytes` of the last reservation, making it runnable. */
void mh_code_arena_commit(mh_code_arena *arena, uint8_t *at, size_t bytes);

/*
 * Rewrite `bytes` of committed code at `at`, a writable-view address, as a
 * link patch does: unlock, write, relock. With one mapping the bytes are
 * executable and not writable once committed, so unlock opens their pages
 * and relock closes them; the dual mapping's writable view always is, and
 * both do nothing there. False from unlock means the patch must not be made.
 */
bool mh_code_arena_unlock(mh_code_arena *arena, uint8_t *at, size_t bytes);
void mh_code_arena_relock(mh_code_arena *arena, uint8_t *at, size_t bytes);

/* Forget everything. The caller must have dropped every pointer into it. */
void mh_code_arena_reset(mh_code_arena *arena);

#endif /* MH_CODE_ARENA_H */
