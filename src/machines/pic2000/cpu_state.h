/* State and diagnostics for the project-owned CPU32 core. */
#ifndef MH_PIC2000_CPU_STATE_H
#define MH_PIC2000_CPU_STATE_H

#include "machines/pic2000/board.h"

namespace cpu_state {

struct Saved {
    uint32_t d[8], a[8];
    uint32_t usp, isp, msp, pc, pc0, vbr, sfc, dfc;
    uint32_t flags;
    int32_t exception;
    uint16_t sr, irc, ird;
    uint8_t ipl, sampled_ipl, fcl, fc_source;
    int64_t clock;
};

inline Saved save(const m68k_machine *m)
{
    Saved s = {};
    const m68k &c = m->core;
    for (int i = 0; i < 8; i++) { s.d[i] = c.d[i]; s.a[i] = c.a[i]; }
    s.usp = c.supervisor ? c.usp : c.a[7];
    s.isp = c.supervisor ? c.a[7] : c.ssp;
    s.pc = s.pc0 = c.pc;
    s.vbr = c.vbr; s.sfc = c.sfc; s.dfc = c.dfc;
    s.sr = m68k_get_sr(&c);
    s.irc = (uint16_t)m->cpu.rd(c.pc + 2, 2);
    s.ird = (uint16_t)m->cpu.rd(c.pc, 2);
    s.clock = c.count_cycles ? (int64_t)c.cycles : 0;
    s.fcl = 2;
    s.flags = (1u << 6) | (c.stopped ? (1u << 1) : 0u);
    s.ipl = s.sampled_ipl = (uint8_t)m->irq_now;
    return s;
}

inline void load(m68k_machine *m, const Saved &s)
{
    m68k &c = m->core;
    for (int i = 0; i < 8; i++) { c.d[i] = s.d[i]; c.a[i] = s.a[i]; }
    m68k_set_sr(&c, s.sr);
    c.usp = s.usp; c.ssp = s.isp;
    c.a[7] = c.supervisor ? s.isp : s.usp;
    c.pc = s.pc; c.vbr = s.vbr; c.sfc = s.sfc; c.dfc = s.dfc;
    c.stopped = (s.flags & (1u << 1)) != 0;
    c.cycles = (uint64_t)s.clock;
    c.exception = 0;
    c.yield = false;
    m->cpu.pc_src = &c.pc;
}

inline uint16_t opcode_at(const m68k_machine *m, uint32_t pc)
{
    Cpu &c = const_cast<Cpu &>(m->cpu);
    c.direct_prepare();
    if (c.direct_enabled && !c.direct_rd.empty() && (pc & 0xFFFu) <= 0xFFEu) {
        const uint8_t *page = c.direct_rd[pc >> 12];
        if (page) return (uint16_t)(page[pc & 0xFFFu] << 8 |
                                    page[(pc & 0xFFFu) + 1]);
    }
    bool ok = true;
    return (uint16_t)mh_bus_read(c.bus, pc, 2, &ok);
}

inline void count_insn_heat(m68k_machine *m, uint32_t pc)
{
    const uint16_t word = opcode_at(m, pc);
    m->cpu.insn_heat_counts[word]++;
    if (m->cpu.after_divide) m->cpu.after_divide_counts[word]++;
    uint8_t code[MH_JIT_M68K_MAX_BYTES];
    code[0] = (uint8_t)(word >> 8);
    code[1] = (uint8_t)word;
    for (unsigned k = 2; k < sizeof code; k++) code[k] = (uint8_t)(k * 7);
    m68k_insn insn;
    m->cpu.after_divide = m68k_decode(code, sizeof code, pc, &insn) &&
                          (insn.op == M68K_OP_DIVU || insn.op == M68K_OP_DIVS);
}

} // namespace cpu_state

#endif
