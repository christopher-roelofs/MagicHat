/*
 * portable.h — the few places this needs to know which compiler it is under.
 *
 * The emulator itself is plain C11 and portable; these are the only builtins
 * used in hot or counting paths, and MSVC spells them differently.
 */
#ifndef MRC_PORTABLE_H
#define MRC_PORTABLE_H

#if defined(__GNUC__) || defined(__clang__)
#  define MRC_LIKELY(x)   __builtin_expect(!!(x), 1)
#  define MRC_UNLIKELY(x) __builtin_expect(!!(x), 0)
#  define MRC_POPCOUNT(x) __builtin_popcount(x)
#else
#  define MRC_LIKELY(x)   (x)
#  define MRC_UNLIKELY(x) (x)
static inline unsigned mrc_popcount(unsigned v)
{
    unsigned n = 0;
    while (v) { v &= v - 1; n++; }
    return n;
}
#  define MRC_POPCOUNT(x) mrc_popcount(x)
#endif

#endif
