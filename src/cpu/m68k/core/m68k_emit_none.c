/*
 * m68k_emit_none.c — the answer for a host with no backend.
 *
 * Saying so is the whole implementation. Native code is an optimisation:
 * a host without one runs every block through the interpreter and is
 * correct, slower, and indistinguishable in every other respect.
 */
#include "cpu/m68k/core/m68k_emit.h"

#if !defined(MRC_M68K_EMIT_X86) && !defined(MRC_M68K_EMIT_A64)

bool m68k_emit_supported(void) { return false; }

size_t m68k_emit(uint8_t *out, const uint8_t *exec, size_t cap,
                 const m68k_insn *insn, unsigned count, uint32_t va,
                 const uint8_t *guest, const uint8_t *expect,
                 unsigned guard_bytes, bool cycles, unsigned *native,
                 m68k_emit_points *points, void *owner)
{
    (void)out; (void)exec; (void)cap; (void)insn; (void)count; (void)va;
    (void)guest; (void)expect; (void)guard_bytes; (void)cycles;
    (void)points; (void)owner;
    if (native) *native = 0;
    return 0;
}

void m68k_patch_link(uint8_t *site, const uint8_t *site_exec,
                     const uint8_t *target)
{
    (void)site; (void)site_exec; (void)target;
}

#endif
