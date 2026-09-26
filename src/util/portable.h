/*
 * portable.h — the few places this needs to know which compiler it is under.
 *
 * The emulator itself is plain C11 and portable; these are the only builtins
 * used in hot or counting paths, and MSVC spells them differently.
 */
#ifndef MH_PORTABLE_H
#define MH_PORTABLE_H

#if defined(__GNUC__) || defined(__clang__)
#  define MH_LIKELY(x)   __builtin_expect(!!(x), 1)
#  define MH_UNLIKELY(x) __builtin_expect(!!(x), 0)
#  define MH_POPCOUNT(x) __builtin_popcount(x)
#else
#  define MH_LIKELY(x)   (x)
#  define MH_UNLIKELY(x) (x)
static inline unsigned mh_popcount(unsigned v)
{
    unsigned n = 0;
    while (v) { v &= v - 1; n++; }
    return n;
}
#  define MH_POPCOUNT(x) mh_popcount(x)
#endif

#endif
