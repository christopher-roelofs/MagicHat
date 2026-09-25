/* Shared per-instruction sequencing. Engine choice is compile-time so the
 * reference loop carries no decode-cache branch. Intentionally no guard. */
static void STEP_NAME(STEP_PARAMETERS)
{
    if (MRC_UNLIKELY(c->halted))
        return;
    if (MRC_UNLIKELY(c->power_stopped)) {
        /* Keep the host execution-slot/time budget moving while the core
         * is stopped. No instruction fetch or interrupt exception occurs. */
        c->insn_count++;
        c->cycle_count++;
        return;
    }

    /*
     * Establish which instruction we are about to run before anything can
     * raise an exception, so EPC and Cause.BD describe that instruction and
     * not the previous one.
     */
    c->cur_pc   = c->pc;
    c->in_delay = c->branch_pending;

    /* An interrupt is taken in place of the instruction, not alongside it. */
    if (MRC_UNLIKELY(check_interrupts(c)))
        return;

    if (MRC_UNLIKELY(c->pc & 3)) {
        raise_exc(c, EXC_AdEL, c->pc, true);
        return;
    }

    uint32_t pa;
    if (MRC_LIKELY(!c->has_mmu)) {
        /* DataRover's normal TX39 configuration has no TLB. Keep instruction
         * fetches on the short segment-map path; data accesses still use the
         * shared load/store helpers. */
        pa = (c->pc >= 0x80000000u && c->pc < 0xC0000000u)
            ? c->pc & 0x1FFFFFFFu : c->pc;
    } else if (!translate(c, c->pc, false, &pa)) {
        return;                       /* translate() already raised */
    }

    bool ok = true;
    uint32_t insn;
    if (span && (span->generation != c->bus->lookup_generation ||
                 span->length < 4 || pa - span->base > span->length - 4)) {
        span->bytes = mrc_bus_read_span(c->bus, pa, &span->length);
        span->base = pa;
        span->generation = c->bus->lookup_generation;
    }
    if (span && span->length >= 4 && pa - span->base <= span->length - 4) {
        /* Read live bytes: stores through any alias and DMA/loader writes
         * are immediately visible. No guest instructions are cached. */
        const uint8_t *p = span->bytes + (pa - span->base);
        insn = (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 |
               (uint32_t)p[2] << 8 | p[3];
        c->bus->reads++;
    } else {
        insn = mrc_bus_read(c->bus, pa, 4, &ok);
    }
    if (!ok) {
        /* Recorded so another implementation can be told to fail the same
         * fetch; keyed by address rather than by position, because a
         * pipelined core fetches ahead of the accesses around it. */
        if (MRC_UNLIKELY(g_bus_trace != NULL))
            bus_trace_emit(c, pa, 4, 0, BUS_F_IFETCH | BUS_F_ERROR);
        raise_exc(c, EXC_IBE, c->pc, true);
        return;
    }

    c->branch_pending = false;
    c->pc      = c->next_pc;
    c->next_pc = c->pc + 4;

    if (c->trace_after_pc && c->cur_pc == c->trace_after_pc &&
        ++c->trace_after_hits == c->trace_after_hit)
        c->trace = true;
    /* --trace-from: by instruction count rather than by pc, for lining up
     * with another machine that has run the same instructions. */
    if (c->trace_from && c->insn_count >= c->trace_from) {
        c->trace = true;
        c->trace_after_pc = 1;           /* so the count below stops it */
        c->trace_from = 0;               /* once: an interrupt taken on the
                                            exact count returns before this */
    }
    if (c->trace) {
        fprintf(c->log, "%08X: %08X\n", c->cur_pc, insn);
        if (c->trace_after_pc && c->trace_after_n && --c->trace_after_n == 0)
            c->trace = false;
    }

    if (c->coverage) {
        uint32_t word = pa >> 2;
        if (word < c->coverage_words)
            c->coverage[word >> 3] |= (uint8_t)(1u << (word & 7));
    }

    for (unsigned i = 0; i < c->watch_n; i++) {
        /* Match on the physical address so a watchpoint set on one of the
         * ROM's two mappings catches execution through either. */
        if ((c->cur_pc & 0x1FFFFFFF) != (c->watch_pc[i] & 0x1FFFFFFF) &&
            (c->cur_pc & 0x00FFFFFF) != (c->watch_pc[i] & 0x00FFFFFF))
            continue;
        c->watch_hits[i]++;
        if (c->watch_log && c->insn_count >= c->watch_after) {
            c->watch_log--;
            /* t7 is the selector for Magic Cap's object dispatcher, so it
             * is worth having in every watch line. */
            fprintf(c->log, "[watch] %08X #%llu at +%llu ra=%08X a0=%08X a1=%08X "
                    "a2=%08X t7=%08X v0=%08X s0=%08X s1=%08X s2=%08X gp=%08X sp=%08X\n",
                    c->cur_pc, (unsigned long long)c->watch_hits[i],
                    (unsigned long long)c->insn_count,
                    c->r[31], c->r[4], c->r[5], c->r[6], c->r[15], c->r[2],
                    c->r[16], c->r[17], c->r[18], c->r[28], c->r[29]);
        }
    }

    EXECUTE_INSTRUCTION(c, pa, insn);

    c->r[0] = 0;
    if (MRC_UNLIKELY(g_trace_state != NULL))
        trace_state_emit(c, insn);
    c->insn_count++;
    c->cycle_count++;
}
