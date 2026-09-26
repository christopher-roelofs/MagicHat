/*
 * tx39_mbus.c — the TX39 MBUS controller.
 *
 * The DataRover wires up MBUS; the Windows CE machines NetBSD supports do
 * not, so there is no reference driver and no register header for it. What
 * is known comes from the interrupt controller's documented MBUS sources
 * plus the ROM's own use of the block, and is written down in tx39_regs.h.
 *
 * With no board-owned peripheral connected, the model represents an empty
 * bus: transfers complete immediately and the input status stays high. The ROM's accessory discovery routine at
 * 0x83C2A794 reads CTRL bit 29 before probing. High ends discovery with no
 * devices; low starts enumeration, which fails when nothing answers.
 * This polarity is inferred from the ROM, not a TX39 register manual.
 *
 * Connected-port ingress and DMA follow SDK Dino.asm.h and the ROM receive
 * collector at 13C28830. The port supplies real received words/commands;
 * it does not synthesize guest callbacks. Wire timing remains instantaneous.
 * See docs/KEYBOARD.md for the remaining peripheral protocol work.
 */
#include "soc/tx39/tx39.h"

/* SDK control masks. MBUSDET is command detection, not physical attachment. */
#define EN 1u
#define LONG_WORD 2u
#define SLAVE 8u
#define DMA_TX 0x8000u
#define DMA_RX 0x10000u
#define ENABLED 0x80000000u
#define EMPTY 0x40000000u
#define DMA_END 0x20u
#define DMA_HALF 0x10u
#define DMA_MASK 0xffffcu

static unsigned index_of(uint32_t off)
{
    return (off - TX39_MBUS_FIRST) / 4;
}

void mh_mbus_connect(tx39_mbus *m, tx39_mbus_port *port)
{
    bool old_high = !m->soc->mbus_port || m->soc->mbus_port->input_high;
    bool high = !port || port->input_high;
    m->soc->mbus_port = port;
    if (port) { port->rx_complete = false; port->rx_bytes = 0; }
    if (high != old_high) mh_icu_raise(&m->soc->icu, 2, high ? 8u : 4u);
}

void mh_mbus_input(tx39_mbus *m, bool high)
{
    tx39_mbus_port *p = m->soc->mbus_port;
    if (!p || p->input_high == high) return;
    p->input_high = high;
    /* SDK kIntMbusPosMask / kIntMbusNegMask, physical request-line edges. */
    mh_icu_raise(&m->soc->icu, 2, high ? 8u : 4u);
}

bool mh_mbus_receive_command(tx39_mbus *m, uint16_t word)
{
    if (!m->soc->mbus_port || !(m->reg[0] & EN)) return false;
    m->reg[index_of(TX39_MBUSCOMMAND)] = word;
    mh_icu_raise(&m->soc->icu, 2, INT2_MBUSDET);
    return true;
}

bool mh_mbus_receive_word(tx39_mbus *m, uint32_t word)
{
    tx39_mbus_port *p = m->soc->mbus_port;
    uint32_t ctrl = m->reg[0];
    if (!p || (ctrl & (EN | SLAVE)) != (EN | SLAVE)) return false;
    if (ctrl & DMA_RX) {
        if (p->rx_complete || !(ctrl & LONG_WORD)) return false;
        uint32_t limit = (m->reg[index_of(TX39_MBUSDMALENGTH)] & DMA_MASK) + 4;
        uint32_t start = m->reg[index_of(TX39_MBUSDMASTART)] & ~3u;
        bool ok;
        mh_bus_write(m->soc->cpu->bus, start + p->rx_bytes, 4, word, &ok);
        if (!ok) {
            p->rx_complete = true;
            mh_icu_raise(&m->soc->icu, 2, INT2_MBUSRXERR);
            return false;
        }
        p->rx_bytes += 4;
        /* At completion the ROM adds four to F0 when DMA_END is set;
         * partial completion reads the transferred count directly. */
        m->reg[index_of(TX39_MBUSDMACOUNT)] = p->rx_bytes;
        if (p->rx_bytes >= limit) {
            p->rx_complete = true;
            m->reg[index_of(TX39_MBUSDMACOUNT)] = limit - 4;
            mh_icu_raise(&m->soc->icu, 2, DMA_END);
        } else if (p->rx_bytes >= (limit + 1) / 2 && p->rx_bytes - 4 < (limit + 1) / 2) {
            mh_icu_raise(&m->soc->icu, 2, DMA_HALF);
        }
    } else {
        m->reg[index_of(TX39_MBUSPAYLOAD)] = (ctrl & LONG_WORD) ? word : word & 0xffff;
        mh_icu_raise(&m->soc->icu, 2, INT2_MBUSRXBUFAVAIL);
    }
    return true;
}

static void transmit_dma(tx39_mbus *m)
{
    tx39_mbus_port *p = m->soc->mbus_port;
    uint32_t length = (m->reg[index_of(TX39_MBUSDMALENGTH)] & DMA_MASK) + 4;
    uint32_t start = m->reg[index_of(TX39_MBUSDMASTART)] & ~3u;
    for (uint32_t at = 0; at < length; at += 4) {
        bool ok;
        uint32_t word = mh_bus_read(m->soc->cpu->bus, start + at, 4, &ok);
        if (!ok) { mh_icu_raise(&m->soc->icu, 2, INT2_MBUSTXERR); return; }
        m->reg[index_of(TX39_MBUSDMACOUNT)] = at;
        if (p->transmit) p->transmit(p->ctx, word, 32);
    }
    mh_icu_raise(&m->soc->icu, 2, DMA_END | INT2_MBUSEMPTY | INT2_MBUSTXBUFAVAIL);
}

uint32_t mh_mbus_read(tx39_mbus *m, uint32_t off, bool *decoded)
{
    if (off < TX39_MBUS_FIRST || off > TX39_MBUS_LAST) {
        *decoded = false;
        return 0;
    }
    *decoded = true;

    uint32_t v = m->reg[index_of(off)];
    if (off == TX39_MBUSCTRL) {
        /* The SDK identifies bit 31 as enabled status. Preserve the old
         * empty-bus behavior when no active port is connected. */
        tx39_mbus_port *p = m->soc->mbus_port;
        if (p) {
            v &= ~(ENABLED | EMPTY | MBUSCTRL_INPUT_HIGH);
            v |= EMPTY | ((v & EN) ? ENABLED : 0);
            if (p->input_high) v |= MBUSCTRL_INPUT_HIGH;
        } else v = (v & ~MBUSCTRL_BUSY) | MBUSCTRL_INPUT_HIGH;
    }
    return v;
}

/*
 * The transmit buffer is empty whenever we are not mid-transfer, which is
 * always. Both "buffer available" and "empty" are level conditions, so they
 * re-assert after software clears them through INTRCLEAR2.
 */
void mh_mbus_update(tx39_mbus *m)
{
    mh_icu_raise(&m->soc->icu, 2, INT2_MBUSTXBUFAVAIL | INT2_MBUSEMPTY);
}

bool mh_mbus_write(tx39_mbus *m, uint32_t off, uint32_t val)
{
    if (off < TX39_MBUS_FIRST || off > TX39_MBUS_LAST)
        return false;

    uint32_t old_ctrl = m->reg[0];
    /* Input status is supplied by the bus, not the command register. In
     * particular, the ROM writes zero here when stopping the controller. */
    m->reg[index_of(off)] = off == TX39_MBUSCTRL
        ? val & ~MBUSCTRL_INPUT_HIGH : val;

    if (off == TX39_MBUSCTRL) {
        /* Writing the control word starts a transfer. With nothing on the
         * bus it finishes at once and frees the transmit buffer. */
        m->transfers++;
        mh_mbus_update(m);
    }
    tx39_mbus_port *p = m->soc->mbus_port;
    if (p) {
        uint32_t ctrl = m->reg[0];
        if (off == TX39_MBUSCTRL) {
            m->reg[0] &= ~(ENABLED | EMPTY);
            if ((ctrl & (EN | SLAVE)) == (EN | SLAVE) &&
                (old_ctrl & (EN | SLAVE)) != (EN | SLAVE)) {
                p->rx_bytes = 0; p->rx_complete = false;
                m->reg[index_of(TX39_MBUSDMACOUNT)] = 0;
            }
            if ((ctrl & (EN | DMA_TX | SLAVE | LONG_WORD)) == (EN | DMA_TX | LONG_WORD) &&
                (old_ctrl & (EN | DMA_TX)) != (EN | DMA_TX)) transmit_dma(m);
        }
        if (off == TX39_MBUSCOMMAND && (ctrl & EN) && !(ctrl & SLAVE) && p->command)
            p->command(p->ctx, (uint16_t)val);
        if (off == TX39_MBUSPAYLOAD && (ctrl & EN) && !(ctrl & SLAVE) && p->transmit)
            p->transmit(p->ctx, val, (ctrl & LONG_WORD) ? 32 : 16);
    }
    return true;
}
