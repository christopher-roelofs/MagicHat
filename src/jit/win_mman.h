/*
 * win_mman.h — the corner of sys/mman.h the code arenas use, on Windows.
 *
 * Anonymous private mappings and protection changes only: no files, so the
 * arenas take their single-mapping path, where the window being written is
 * made writable and put back to executable before it runs.
 */
#ifndef MH_JIT_WIN_MMAN_H
#define MH_JIT_WIN_MMAN_H

#include <windows.h>
#include <stddef.h>

#define PROT_READ     1
#define PROT_WRITE    2
#define PROT_EXEC     4
#define MAP_PRIVATE   2
#define MAP_ANONYMOUS 0x20
#define MAP_FAILED    ((void *)-1)

static inline DWORD mh_win_protection(int prot)
{
    if (prot & PROT_EXEC) return prot & PROT_WRITE ? PAGE_EXECUTE_READWRITE : PAGE_EXECUTE_READ;
    return prot & PROT_WRITE ? PAGE_READWRITE : PAGE_READONLY;
}

static inline void *mh_win_mmap(void *addr, size_t size, int prot, int flags,
                                 int fd, long offset)
{
    (void)addr; (void)flags; (void)fd; (void)offset;
    void *p = VirtualAlloc(NULL, size, MEM_RESERVE | MEM_COMMIT, mh_win_protection(prot));
    return p ? p : MAP_FAILED;
}

static inline int mh_win_munmap(void *addr, size_t size)
{
    (void)size;
    return VirtualFree(addr, 0, MEM_RELEASE) ? 0 : -1;
}

static inline int mh_win_mprotect(void *addr, size_t size, int prot)
{
    DWORD old;
    if (!VirtualProtect(addr, size, mh_win_protection(prot), &old)) return -1;
    if (prot & PROT_EXEC) FlushInstructionCache(GetCurrentProcess(), addr, size);
    return 0;
}

static inline long mh_win_page_size(void)
{
    SYSTEM_INFO info;
    GetSystemInfo(&info);
    return (long)info.dwPageSize;
}

#define mmap     mh_win_mmap
#define munmap   mh_win_munmap
#define mprotect mh_win_mprotect
#define sysconf(name) mh_win_page_size()
#ifndef _SC_PAGESIZE
#define _SC_PAGESIZE 0
#endif

#endif /* MH_JIT_WIN_MMAN_H */
