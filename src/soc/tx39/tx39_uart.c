/*
 * tx39_uart.c — TX39 UART A/B.
 *
 * UART A is wired to the DataRover's debug serial port, which is where the
 * IDT monitor prints its banner and reads commands. Transmitted bytes go to
 * the host's stdout; host input is injected with mh_uart_rx_byte().
 */
#include "soc/tx39/tx39.h"
#include "host/serial.h"

/*
 * A host serial port attached to a UART, kept outside the device struct on
 * purpose. Everything in `tx39` is written into a snapshot whole, so adding
 * a field here would change its size and make every state file already on
 * disk unloadable -- the banked milestones, and the hundreds of screens the
 * UI crawler saved. Host-side plumbing does not belong in the machine's
 * state anyway.
 */
static void *g_uart_link[2];

static unsigned uart_index(const tx39_uart *u)
{
    return (unsigned)(u - u->soc->uart);
}

void mh_uart_set_link(tx39_uart *u, void *link)
{
    g_uart_link[uart_index(u)] = link;
}

void *mh_uart_link(const tx39_uart *u)
{
    return g_uart_link[uart_index(u)];
}

#include <stdio.h>

/* Which INTRSTATUS2 bits belong to this UART. */
static uint32_t rx_int_bit(const tx39_uart *u)
{
    return u->index == 0 ? INT2_UARTARXINT : INT2_UARTBRXINT;
}

static uint32_t tx_int_bits(const tx39_uart *u)
{
    return u->index == 0 ? (INT2_UARTATXINT | INT2_UARTAEMPTYINT)
                         : (INT2_UARTBTXINT | INT2_UARTBEMPTYINT);
}

/* NetBSD tx39uartreg.h: 3.6864 MHz / (16 * (divisor + 1)).
 * A frame includes start, 7/8 data, optional parity, and 1/2 stop bits. */
uint64_t mh_uart_frame_cycles(const tx39_uart *u)
{
    unsigned bits = 1 + ((u->ctrl1 & UART_CTRL1_BIT7) ? 7 : 8) +
                    ((u->ctrl1 & UART_CTRL1_ENPARITY) ? 1 : 0) +
                    ((u->ctrl1 & UART_CTRL1_TWOSTOP) ? 2 : 1);
    return ((uint64_t)u->soc->cpu_hz * 16 * ((u->ctrl2 & 0x3ff) + 1) * bits
            + 3686399) / 3686400;
}

/* TXHOLD and the serial shift register are distinct. TXINT means the former
 * has room; EMPTY means both are empty. Interrupts latch transitions and
 * remain acknowledged until another event: NetBSD txcom.c establishes TX
 * as IST_EDGE (https://github.com/NetBSD/src/blob/trunk/sys/arch/hpcmips/tx/txcom.c).
 * Reasserting TX every tick corrupted the guest's PCLink block CRCs; timed
 * transmission with latched events produces a fully CRC-valid Cnct stream. */
void mh_uart_update(tx39_uart *u)
{
    tx39_uart_timing *t = &u->soc->uart_timing[u->index];
    uint64_t now = u->soc->cpu->cycle_count;
    while (t->shifting && now >= t->tx_done) {
        void *lnk = mh_uart_link(u);
        if (lnk)
            mh_serial_write(lnk, t->shift);
        else if (u->console) {
            fputc(t->shift, stdout);
            fflush(stdout);
        }
        if (t->holding) {
            t->shift = t->hold;
            t->holding = false;
            t->tx_done += mh_uart_frame_cycles(u);
            mh_icu_raise(&u->soc->icu, 2,
                          u->index == 0 ? INT2_UARTATXINT : INT2_UARTBTXINT);
        } else {
            t->shifting = false;
            mh_icu_raise(&u->soc->icu, 2,
                          u->index == 0 ? INT2_UARTAEMPTYINT : INT2_UARTBEMPTYINT);
        }
    }
}

uint32_t mh_uart_read(tx39_uart *u, uint32_t off, bool *decoded)
{
    *decoded = true;

    switch (off) {
    case TX39_UART_CTRL1: {
        u->ctrl1_reads++;
        uint32_t v = u->ctrl1;
        /* EMPTY is the transmitter's own state; UARTON reports that the
         * block has actually started, which follows the software enable. */
        mh_uart_update(u);
        if (!u->soc->uart_timing[u->index].shifting)
            v |= UART_CTRL1_EMPTY;
        if (u->ctrl1 & UART_CTRL1_ENUART)
            v |= UART_CTRL1_UARTON;
        if (u->rx_full)
            v |= UART_CTRL1_RXHOLDFULL;
        return v;
    }
    case TX39_UART_CTRL2:    return u->ctrl2;
    case TX39_UART_DMACTRL1: return u->dmactrl1;
    case TX39_UART_DMACTRL2: return u->dmactrl2;
    case TX39_UART_DMACNT:   return u->dmacnt;
    case TX39_UART_RXHOLD:
        u->rxhold_reads++;
        u->rx_full = false;
        u->soc->icu.status[1] &= ~rx_int_bit(u);
        mh_icu_update(&u->soc->icu);
        return u->rx;
    default:
        *decoded = false;
        return 0;
    }
}

bool mh_uart_write(tx39_uart *u, uint32_t off, uint32_t val)
{
    switch (off) {
    case TX39_UART_CTRL1: {
        bool was_enabled = (u->ctrl1 & UART_CTRL1_ENUART) != 0;
        /* UARTON / EMPTY / RXHOLDFULL are status bits, not settable. */
        u->ctrl1 = val & ~(UART_CTRL1_UARTON | UART_CTRL1_EMPTY |
                           UART_CTRL1_RXHOLDFULL | UART_CTRL1_PRXHOLDFULL);
        if (!was_enabled && (u->ctrl1 & UART_CTRL1_ENUART))
            /* Initial ready event: the monitor polls TXINT at 0x83C011F0
             * immediately after enabling, before transmitting a byte. */
            mh_icu_raise(&u->soc->icu, 2, tx_int_bits(u));
        mh_uart_update(u);
        return true;
    }
    case TX39_UART_CTRL2:    u->ctrl2 = val; return true;
    case TX39_UART_DMACTRL1: u->dmactrl1 = val; return true;
    case TX39_UART_DMACTRL2: u->dmactrl2 = val; return true;
    case TX39_UART_DMACNT:   u->dmacnt = val; return true;
    case TX39_UART_TXHOLD: {
        mh_uart_update(u);
        tx39_uart_timing *t = &u->soc->uart_timing[u->index];
        u->tx_bytes++;
        if (!t->shifting) {
            t->shift = (uint8_t)val;
            t->shifting = true;
            t->tx_done = u->soc->cpu->cycle_count + mh_uart_frame_cycles(u);
            mh_icu_raise(&u->soc->icu, 2,
                          u->index == 0 ? INT2_UARTATXINT : INT2_UARTBTXINT);
        } else {
            if (t->holding)
                mh_icu_raise(&u->soc->icu, 2, u->index == 0 ?
                              INT2_UARTATXOVERRUNINT : INT2_UARTBTXOVERRUNINT);
            t->hold = (uint8_t)val;
            t->holding = true;
        }
        mh_uart_update(u);
        return true;
    }
    default:
        return false;
    }
}

void mh_uart_rx_byte(tx39_uart *u, uint8_t byte)
{
    u->rx_delivered++;
    u->rx = byte;
    u->rx_full = true;
    mh_icu_raise(&u->soc->icu, 2, rx_int_bit(u));
}
