#include "devices/pccard/pccard.h"

#include <string.h>

static const char *const window_name[MRC_PCCARD_NWINDOW] = { "A", "B" };

void mrc_pccard_init(mrc_pccard *c, unsigned slot, const mrc_pccard_kind *kind)
{
    memset(c, 0, sizeof(*c));
    c->kind = kind;
    c->slot = slot;
    c->log = stderr;
}

/* Legacy read-only image probe. It deliberately retains the original
 * mirrored A/B mapping used by --card1/--card2. Writable SRAM has its own
 * device model: attribute/CIS in A and common memory in B (sram_card.c). */
static bool memory_read(mrc_pccard *c, mrc_pccard_window w, uint32_t off,
                        unsigned size, uint32_t *out)
{
    (void)w;
    if (!c->image || !c->image_len)
        return false;

    uint32_t v = 0;
    for (unsigned i = 0; i < size; i++)
        v = (v << 8) | c->image[(off + i) % c->image_len];
    *out = v;
    return true;
}

static bool memory_write(mrc_pccard *c, mrc_pccard_window w, uint32_t off,
                         unsigned size, uint32_t val)
{
    (void)c; (void)w; (void)off; (void)size; (void)val;
    return false;       /* a memory card we present read-only */
}

const mrc_pccard_kind mrc_pccard_memory = {
    .name = "memory", .read = memory_read, .write = memory_write,
};

static void note(mrc_pccard *c, mrc_pccard_window w, uint32_t off)
{
    if (!c->touched[w]) {
        c->touched[w] = true;
        c->first_off[w] = off;
        c->high_off[w] = off;
    } else if (off > c->high_off[w]) {
        c->high_off[w] = off;
    }
}

uint32_t mrc_pccard_read(void *ctx, uint32_t off, unsigned size)
{
    mrc_pccard_port *p = ctx;
    mrc_pccard *c = p->card;
    uint32_t v = 0;

    note(c, p->window, off);
    c->reads[p->window]++;

    bool ok = (!p->present || *p->present) && c->kind && c->kind->read &&
              c->kind->read(c, p->window, off, size, &v);
    if (!ok) {
        /* Nothing drives it. A PCMCIA bus with nothing on those lines reads
         * as all ones, which is how host software concludes there is no card
         * -- so that, and not a zero we made up. */
        v = size == 4 ? 0xFFFFFFFFu : (1u << (size * 8)) - 1u;
    }

    if (c->log_first) {
        c->log_first--;
        fprintf(c->log, "[card%u] R%u window %s +%06X = %08X @%08X%s\n",
                c->slot + 1, size * 8, window_name[p->window], off, v,
                p->pc_hint ? *p->pc_hint : 0, ok ? "" : "  (not driven)");
    }
    return v;
}

void mrc_pccard_write(void *ctx, uint32_t off, unsigned size, uint32_t val)
{
    mrc_pccard_port *p = ctx;
    mrc_pccard *c = p->card;

    note(c, p->window, off);
    c->writes[p->window]++;

    bool ok = (!p->present || *p->present) && c->kind && c->kind->write &&
              c->kind->write(c, p->window, off, size, val);

    if (c->log_first) {
        c->log_first--;
        fprintf(c->log, "[card%u] W%u window %s +%06X = %08X @%08X%s\n",
                c->slot + 1, size * 8, window_name[p->window], off, val,
                p->pc_hint ? *p->pc_hint : 0, ok ? "" : "  (not driven)");
    }
}

void mrc_pccard_report(const mrc_pccard *c)
{
    if (!c->kind)
        return;
    if (c->kind == &mrc_pccard_ne2000) {
        fprintf(c->log, "ne2000: COR=%02X CR=%02X ISR=%02X IMR=%02X "
                "DMA=%llu read/%llu written, %llu unsupported accesses\n",
                c->config, c->nic.cr, c->nic.isr, c->nic.imr,
                (unsigned long long)c->nic.dma_reads,
                (unsigned long long)c->nic.dma_writes,
                (unsigned long long)c->nic.unsupported);
        fprintf(c->log, "ne2000: MAC=%02X:%02X:%02X:%02X:%02X:%02X "
                "ring=%02X..%02X BNRY=%02X CURR=%02X TX requests=%llu\n",
                c->nic.par[0], c->nic.par[1], c->nic.par[2], c->nic.par[3],
                c->nic.par[4], c->nic.par[5], c->nic.pstart, c->nic.pstop,
                c->nic.bnry, c->nic.curr, (unsigned long long)c->nic.tx_requests);
    }
    if (c->kind == &mrc_pccard_ne2000)
        fprintf(c->log, "ne2000: TX=%llu RX=%llu filtered=%llu overruns=%llu\n",
                (unsigned long long)c->nic.tx_packets, (unsigned long long)c->nic.rx_packets,
                (unsigned long long)c->nic.rx_filtered, (unsigned long long)c->nic.rx_overruns);
    for (unsigned w = 0; w < MRC_PCCARD_NWINDOW; w++) {
        if (!c->touched[w])
            continue;
        fprintf(c->log,
                "card%u window %s: %llu reads, %llu writes, "
                "offsets %06X..%06X\n",
                c->slot + 1, window_name[w],
                (unsigned long long)c->reads[w],
                (unsigned long long)c->writes[w],
                c->first_off[w], c->high_off[w]);
    }
    for (unsigned w = 0; w < MRC_PCCARD_NWINDOW; w++)
        if (!c->touched[w])
            fprintf(c->log, "card%u window %s: never accessed\n",
                    c->slot + 1, window_name[w]);
}
