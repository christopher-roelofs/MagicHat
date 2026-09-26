#include "soc/mc68349/duart.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>

static bool running(const Mc68349Duart *d)
{
    return !(d->mcr & 0x8000u);
}

unsigned Mc68349Duart::baud(bool channel_a, bool receive) const
{
    /* The ROM still owns the guest-visible divisor. The emulator defaults to
     * 115200 so large package transfers are practical; numeric values remain
     * available for timing experiments, and "guest" restores the divisor
     * selected by the ROM. */
    bool use_guest_rate = false;
    if (const char *forced = std::getenv("MH_68K_BAUD")) {
        if (std::strcmp(forced, "guest") == 0 ||
            std::strcmp(forced, "rom") == 0) {
            use_guest_rate = true;
        } else {
            char *end = nullptr;
            const unsigned long value = std::strtoul(forced, &end, 0);
            if (end != forced && *end == '\0' && value > 0 &&
                value <= 10000000)
                return static_cast<unsigned>(value);
        }
    }
    if (!use_guest_rate && !channel_a)
        return 115200;

    static const unsigned set1[] = {
        50, 110, 134, 200, 300, 600, 1200, 1050,
        2400, 4800, 7200, 9600, 38400, 76800, 0, 0,
    };
    static const unsigned set2[] = {
        75, 110, 134, 150, 300, 600, 1200, 2000,
        2400, 4800, 1800, 9600, 19200, 38400, 0, 0,
    };
    const unsigned csr = channel_a ? csra : csrb;
    const unsigned code = receive ? csr >> 4 : csr & 0x0F;
    return (acr & 0x80 ? set2 : set1)[code];
}

uint8_t Mc68349Duart::status_b() const
{
    uint8_t s = 0;
    if (rx_len) s |= 0x01;             /* RxRDY */
    if (tx_enabled && !tx_busy) s |= 0x04; /* TxRDY */
    if (tx_enabled && !tx_busy) s |= 0x08; /* TxEMP */
    if (rx_len == 4) s |= 0x02;        /* FIFO full */
    return s;
}

void Mc68349Duart::refresh_status()
{
    const uint8_t old = isr;
    isr &= uint8_t(~0x30u);
    const uint8_t s = status_b();
    if (s & 0x04) isr |= 0x10;          /* TxRDYB */
    if (s & 0x01) isr |= 0x20;          /* RxRDYB */
    if (a.enabled) {
        isr &= uint8_t(~3u);
        const uint8_t sa = status_a();
        if (sa & 4) isr |= 1;
        if (sa & (mr1a & 0x40 ? 2 : 1)) isr |= 2;
    }
    if (isr != old) dirty = true;
}

bool Mc68349Duart::open_a()
{
    if (link_a.fd < 0 && !mh_serial_open_pty(&link_a)) return false;
    a.enabled = true;
    dirty = true;
    refresh_status();
    return true;
}

void Mc68349Duart::attach_ppp(mh_ppp *endpoint)
{
    ppp = endpoint;
    if (ppp) a.enabled = true;
    dirty = true;
    refresh_status();
}

void Mc68349Duart::detach_ppp()
{
    mh_ppp_close(ppp);
    ppp = nullptr;
    dirty = true;
}

bool Mc68349Duart::active() const
{
    return link.peer || link.fd >= 0 || tx_busy ||
        (a.enabled && (dirty || ppp || link_a.fd >= 0 || link_a.peer ||
                       a.tx_busy || (isr & ier & 3)));
}

uint8_t Mc68349Duart::status_a() const
{
    if (!a.enabled) return 0;
    return (a.rx_len ? 1 : 0) | (a.rx_len == 3 ? 2 : 0) |
        (a.tx_enabled && !a.tx_busy ? 12 : 0);
}

void Mc68349Duart::command_a(uint8_t value)
{
    if (!a.enabled) return;
    switch (value >> 4) {
    case 2: a.rx_enabled = false; a.rx_at = a.rx_len = 0; break;
    case 3: a.tx_enabled = false; a.tx_busy = false; break;
    case 5: isr &= uint8_t(~4u); break;
    default: break;
    }
    if ((value & 3) == 1) a.rx_enabled = true;
    if ((value & 3) == 2) a.rx_enabled = false;
    if ((value & 12) == 4) a.tx_enabled = true;
    if ((value & 12) == 8) a.tx_enabled = false;
    refresh_status();
    dirty = true;
}

void Mc68349Duart::tick_a()
{
    if (!a.enabled) return;
    const bool cts = ppp || link_a.fd >= 0 || link_a.peer;
    if (ppp && insns && clock_hz) {
        const uint64_t slots = *insns;
        mh_ppp_poll(ppp, (slots / clock_hz) * 1000000000ull +
                           (slots % clock_hz) * 1000000000ull / clock_hz);
    }
    if (a.tx_busy && baud(true) && (!insns || *insns >= a.tx_done_at) &&
        (!(mr2a & 0x10) || cts)) {
        if (ppp) mh_ppp_write(ppp, a.tx_byte);
        else mh_serial_write(&link_a, a.tx_byte);
        a.tx_busy = false;
    }
    const unsigned rate = baud(true, true);
    if (a.rx_enabled && a.rx_len < 3 && rate &&
        (!insns || *insns >= a.rx_next_at)) {
        uint8_t byte;
        if ((ppp && mh_ppp_read(ppp, &byte)) ||
            (!ppp && mh_serial_read(&link_a, &byte))) {
            a.rx[(a.rx_at + a.rx_len) % 3] = byte;
            ++a.rx_len;
            a.rx_next_at = insns ? *insns +
                (uint64_t(clock_hz) * 10 + rate - 1) / rate : 0;
        }
    }
    refresh_status();
}

bool Mc68349Duart::irq() const
{
    if (!running(this) || !ilr) return false;
    return (isr & ier) != 0;
}

void Mc68349Duart::attach(struct mh_pclink *peer)
{
    mh_serial_attach(&link, peer);
    dirty = true;
}

void Mc68349Duart::detach()
{
    mh_serial_close(&link);
    rx_at = rx_len = 0;
    rx_next_at = 0;
    tx_busy = false;
    dirty = true;
}

void Mc68349Duart::command_b(uint8_t value)
{
    const uint8_t misc = value >> 4;
    switch (misc) {
    case 2: rx_at = rx_len = 0; break;       /* reset receiver */
    case 3: tx_busy = false; break;           /* reset transmitter */
    case 4: isr &= uint8_t(~0x60u); break;   /* reset errors */
    case 5: isr &= uint8_t(~0x40u); break;   /* reset break */
    default: break;
    }
    if ((value & 3) == 1) rx_enabled = true;
    if ((value & 3) == 2) rx_enabled = false;
    if ((value & 3) == 3) rx_enabled = false;
    if (((value >> 2) & 3) == 1) tx_enabled = true;
    if (((value >> 2) & 3) == 2) tx_enabled = false;
    if (((value >> 2) & 3) == 3) tx_enabled = false;
    refresh_status();
    dirty = true;
}

uint8_t Mc68349Duart::read_byte(unsigned off)
{
    switch (off) {
    case 0x00: return uint8_t(mcr >> 8);
    case 0x01: return uint8_t(mcr);
    case 0x04: return ilr;
    case 0x05: return ivr;
    case 0x10: return mr1a;
    case 0x11: return status_a();
    case 0x13: {
        if (!a.enabled || !a.rx_len) return 0;
        const uint8_t byte = a.rx[a.rx_at];
        a.rx_at = (a.rx_at + 1) % 3;
        --a.rx_len;
        refresh_status();
        dirty = true;
        return byte;
    }
    case 0x14: return a.enabled && (ppp || link_a.fd >= 0 || link_a.peer) ? 2 : 3;
    case 0x1D: return a.enabled ? (ppp || link_a.fd >= 0 || link_a.peer ? 2 : 3) : 0;
    case 0x15: return isr;
    case 0x18: return mr1b;
    case 0x19: return status_b();
    case 0x1B: {
        if (!rx_len) return 0;
        const uint8_t b = rx[rx_at];
        rx_at = (rx_at + 1) % 4;
        --rx_len;
        refresh_status();
        dirty = true;
        return b;
    }
    case 0x20: return mr2a;
    case 0x21: return mr2b;
    default: return 0;
    }
}

void Mc68349Duart::write_byte(unsigned off, uint8_t value)
{
    switch (off) {
    case 0x00: mcr = uint16_t((mcr & 0x00FFu) | (uint16_t(value) << 8)); break;
    case 0x01: mcr = uint16_t((mcr & 0xFF00u) | value); break;
    case 0x04: ilr = value & 7; break;
    case 0x05: ivr = value; break;
    case 0x10: mr1a = value; break;
    case 0x11: csra = value; break;
    case 0x12: cra = value; command_a(value); break;
    case 0x13:
        if (a.enabled && a.tx_enabled && !a.tx_busy) {
            a.tx_byte = value;
            a.tx_busy = true;
            const unsigned rate = baud(true);
            a.tx_done_at = insns && rate ? *insns +
                (uint64_t(clock_hz) * 10 + rate - 1) / rate : 0;
            refresh_status();
        }
        break;
    case 0x14: acr = value; break;
    case 0x15: ier = value; refresh_status(); break;
    case 0x18: mr1b = value; break;
    case 0x19: csrb = value; break;
    case 0x1A: crb = value; command_b(value); break;
    case 0x1B:
        if (tx_enabled && !tx_busy) {
            tx_byte = value;
            tx_busy = true;
            const unsigned rate = baud();
            const uint64_t bits = 10;
            tx_done_at = insns && rate ? *insns +
                (uint64_t(clock_hz) * bits + rate - 1) / rate : 0;
            /* Loading TBB clears TxRDY immediately.  Waiting until the next
             * board tick lets an interrupt handler observe stale ISR state
             * and overwrite a single-byte transmitter several times. */
            refresh_status();
            dirty = true;
        }
        break;
    case 0x20: mr2a = value; break;
    case 0x21: mr2b = value; break;
    default: break;
    }
    dirty = true;
}

uint32_t Mc68349Duart::read(uint32_t off, unsigned size)
{
    uint32_t out = 0;
    for (unsigned i = 0; i < size; ++i) out = (out << 8) | read_byte(off + i);
    return out;
}

void Mc68349Duart::write(uint32_t off, unsigned size, uint32_t value)
{
    for (unsigned i = 0; i < size; ++i)
        write_byte(off + size - 1 - i, uint8_t(value >> (i * 8)));
}

void Mc68349Duart::tick()
{
    if (!running(this)) return;
    tick_a();
    bool changed = dirty;
    dirty = false;
    if (tx_busy && (!insns || *insns >= tx_done_at)) {
        mh_serial_write(&link, tx_byte);
        tx_busy = false;
        changed = true;
    }
    if (rx_enabled && rx_len < 4 &&
        (!insns || *insns >= rx_next_at)) {
        uint8_t b;
        if (mh_serial_read(&link, &b)) {
            rx[(rx_at + rx_len) % 4] = b;
            ++rx_len;
            const unsigned rate = baud();
            rx_next_at = insns && rate ? *insns +
                (uint64_t(clock_hz) * 10 + rate - 1) / rate : 0;
            changed = true;
        }
    }
    if (changed) refresh_status();
}

uint32_t mc68349_duart_read(void *ctx, uint32_t off, unsigned size)
{
    return static_cast<Mc68349Duart *>(ctx)->read(off, size);
}

void mc68349_duart_write(void *ctx, uint32_t off, unsigned size, uint32_t value)
{
    static_cast<Mc68349Duart *>(ctx)->write(off, size, value);
}
