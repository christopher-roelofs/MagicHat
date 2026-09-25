#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif
#include "jit/code_arena.h"

#if defined(__linux__)
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>
#define HAVE_MMAP 1
#else
#define HAVE_MMAP 0
#endif

#include <stdlib.h>
#include <string.h>

bool mrc_code_arena_open(mrc_code_arena *arena, size_t size)
{
    memset(arena, 0, sizeof *arena);
#if HAVE_MMAP
    long page = sysconf(_SC_PAGESIZE);
    if (page < 4096) return false;
    size = (size + (size_t)page - 1) & ~((size_t)page - 1);

    void *exec = MAP_FAILED, *write = MAP_FAILED;
    bool dual = false;
#ifdef __NR_memfd_create
    /*
     * The syscall rather than the library wrapper, which is gated behind
     * an API level newer than this builds against on Android. A kernel
     * without it answers ENOSYS and the single mapping below is used.
     */
    int fd = getenv("MRC_JIT_SINGLE_MAP") ? -1 :
             (int)syscall(__NR_memfd_create, "mrc-jit", 1u /* MFD_CLOEXEC */);
    if (fd >= 0) {
        if (!ftruncate(fd, (off_t)size)) {
            exec = mmap(NULL, size, PROT_READ | PROT_EXEC, MAP_SHARED, fd, 0);
            write = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        }
        close(fd);
        if (exec != MAP_FAILED && write != MAP_FAILED) dual = true;
        else {
            if (exec != MAP_FAILED) munmap(exec, size);
            if (write != MAP_FAILED) munmap(write, size);
            exec = write = MAP_FAILED;
        }
    }
#endif
    if (!dual) {
        exec = mmap(NULL, size, PROT_READ | PROT_EXEC,
                    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        write = exec;
    }
    if (exec == MAP_FAILED) return false;
    arena->exec = exec;
    arena->write = write;
    arena->size = size;
    arena->page = (size_t)page;
    arena->dual = dual;
    return true;
#else
    (void)size;
    return false;
#endif
}

void mrc_code_arena_close(mrc_code_arena *arena)
{
#if HAVE_MMAP
    if (arena->exec) munmap(arena->exec, arena->size);
    if (arena->dual && arena->write) munmap(arena->write, arena->size);
#endif
    memset(arena, 0, sizeof *arena);
}

void mrc_code_arena_reset(mrc_code_arena *arena)
{
    arena->used = 0;
    arena->flushes++;
}

uint8_t *mrc_code_arena_reserve(mrc_code_arena *arena, size_t most,
                                uint8_t **exec_at)
{
#if HAVE_MMAP
    size_t at = (arena->used + 15) & ~(size_t)15;
    if (at + most > arena->size) return NULL;
    if (!arena->dual) {
        /* Open only the window about to be written, and no further. */
        size_t first = at & ~(arena->page - 1);
        size_t last = (at + most + arena->page - 1) & ~(arena->page - 1);
        if (last > arena->size) last = arena->size;
        if (mprotect(arena->exec + first, last - first,
                     PROT_READ | PROT_WRITE))
            return NULL;
    }
    arena->used = at;
    *exec_at = arena->exec + at;
    return arena->write + at;
#else
    (void)arena; (void)most; (void)exec_at;
    return NULL;
#endif
}

void mrc_code_arena_commit(mrc_code_arena *arena, uint8_t *at, size_t bytes)
{
#if HAVE_MMAP
    size_t offset = (size_t)(at - arena->write);
    if (!arena->dual) {
        size_t first = offset & ~(arena->page - 1);
        size_t last = (offset + bytes + arena->page - 1) & ~(arena->page - 1);
        if (last > arena->size) last = arena->size;
        mprotect(arena->exec + first, last - first, PROT_READ | PROT_EXEC);
    } else {
        __builtin___clear_cache((char *)at, (char *)at + bytes);
    }
    __builtin___clear_cache((char *)arena->exec + offset,
                            (char *)arena->exec + offset + bytes);
    arena->used = offset + bytes;
#else
    (void)arena; (void)at; (void)bytes;
#endif
}
