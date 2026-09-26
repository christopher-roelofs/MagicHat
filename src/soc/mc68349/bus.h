#pragma once
/*
 * bus.h — the MC68349's CPU-space registers, on the machine's bus.
 *
 * Split from cpu.h the same way M68kBus was split from the reference core:
 * none of this needs a 68k instruction engine to exist. MBAR is a register
 * reached through CPU space, and CPU space is a separate address space
 * rather than a region of memory, so it does not go through the bus at all.
 */
#include "cpu/m68k/bus.h"

namespace mh {

struct Mc68349Bus : M68kBus {
    mutable uint32_t mbar = 0;
    mutable uint64_t cpu_space_other = 0;
    mutable uint16_t irq_vector = 0;
    /* The module space follows MBAR rather than being asserted. */
    mh_region *module_region = nullptr;

    uint32_t cpu_space_read(uint32_t addr, unsigned size) const override
    {
        auto *self = const_cast<Mc68349Bus *>(this);
        if ((addr & ~3u) == 0x0003FF00u)
            return size == 4 ? mbar : (uint16_t)(mbar >> (addr & 2 ? 0 : 16));
        self->cpu_space_other++;
        return size == 1 ? 0xFFu : 0xFFFFu;
    }

    void cpu_space_write(uint32_t addr, unsigned size, uint32_t val) const override
    {
        auto *self = const_cast<Mc68349Bus *>(this);
        if ((addr & ~3u) == 0x0003FF00u) {
            if (size == 4) self->mbar = val;
            else if (addr & 2) self->mbar = (mbar & 0xFFFF0000u) | (val & 0xFFFF);
            else               self->mbar = (mbar & 0x0000FFFFu) | (val << 16);
            uint32_t base = mbar & 0xFFFFF000u;
            if (module_region && base && module_region->base != base) {
                fprintf(log, "[68k] MBAR = %08X: module space moves from %08X "
                        "to %08X\n", mbar, module_region->base, base);
                module_region->base = base;
                mh_bus_invalidate_lookup(self->bus);
            } else {
                fprintf(log, "[68k] MBAR = %08X, module base %08X\n",
                        mbar, base);
            }
            return;
        }
        self->cpu_space_other++;
        if (log_unknown && unknown_shown < 24) {
            self->unknown_shown++;
            fprintf(log, "[68k] CPU-space write%u to %08X = %0*X, not a "
                    "register we know  (pc=%08X)\n", size * 8, addr,
                    (int)size * 2, val, at_pc());
        }
    }
};

} /* namespace */
