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
#ifndef MRC_CODE_ARENA_H
#define MRC_CODE_ARENA_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint8_t *exec;       /* where generated code runs                  */
    uint8_t *write;      /* where it is written; equal when not dual   */
    size_t   size, used, page;
    bool     dual;
    uint64_t flushes;
} mrc_code_arena;

/* Returns false when this host will not give out executable memory. */
bool mrc_code_arena_open(mrc_code_arena *arena, size_t size);
void mrc_code_arena_close(mrc_code_arena *arena);

/*
 * Reserve space for up to `most` bytes and hand back where to write them.
 * `*exec_at` receives the address the same bytes will run at, which is a
 * different address when the arena is dual-mapped and the emitter needs
 * both to resolve its own branches. Returns NULL when the arena is full;
 * the caller should discard what it has cached and call
 * mrc_code_arena_reset before trying again.
 */
uint8_t *mrc_code_arena_reserve(mrc_code_arena *arena, size_t most,
                                uint8_t **exec_at);

/* Commit `bytes` of the last reservation, making it runnable. */
void mrc_code_arena_commit(mrc_code_arena *arena, uint8_t *at, size_t bytes);

/* Forget everything. The caller must have dropped every pointer into it. */
void mrc_code_arena_reset(mrc_code_arena *arena);

#endif /* MRC_CODE_ARENA_H */
