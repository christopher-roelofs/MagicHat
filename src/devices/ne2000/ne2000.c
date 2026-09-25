#include "devices/ne2000/ne2000.h"
#include <string.h>

static void reset(ne2000 *n)
{
    n->cr = 0x21;
    n->isr = 0x80;
    n->imr = 0;
    n->rbcr = 0;
    n->tsr = n->rsr = 0;
    n->tx_pending = false;
    memset(n->tally, 0, sizeof(n->tally));
}

void mrc_ne2000_init(ne2000 *n, const uint8_t mac[6])
{
    memset(n, 0, sizeof(*n));
    n->dcr = 4; /* LAS is set on power-up (DP8390D section 10.3). */
    /* NE2000's 16-byte station PROM is read as 16 duplicated words. */
    for (unsigned i = 0; i < 16; i++) {
        uint8_t b = i < 6 ? mac[i] : i >= 14 ? 0x57 : 0;
        n->prom[i * 2] = n->prom[i * 2 + 1] = b;
    }
    reset(n);
}

bool mrc_ne2000_irq(const ne2000 *n)
{
    return (n->isr & n->imr & 0x7f) != 0;
}

/* Ethernet's reflected CRC supplies the FCS. The multicast hash uses the
 * top six bits of the non-reflected LFSR (DP8390D section 7). */
static uint32_t ethernet_crc(const uint8_t *data, size_t len)
{
    uint32_t crc = 0xffffffff;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (unsigned b = 0; b < 8; b++) crc = (crc >> 1) ^ ((crc & 1) ? 0xedb88320 : 0);
    }
    return ~crc;
}
static unsigned multicast_hash(const uint8_t *mac)
{
    uint32_t crc = 0xffffffff;
    for (unsigned i = 0; i < 6; i++) {
        uint8_t byte = mac[i];
        for (unsigned b = 0; b < 8; b++, byte >>= 1) {
            bool carry = ((crc >> 31) ^ byte) & 1;
            crc <<= 1;
            if (carry) crc ^= 0x04c11db7;
        }
    }
    return crc >> 26;
}
static void missed(ne2000 *n)
{
    if (++n->tally[2] == 0x80) n->isr |= 0x20;
}
bool mrc_ne2000_receive(ne2000 *n, const uint8_t *frame, size_t len)
{
    if ((n->cr & 3) != 2 || len < 14 || len > 1518)
        return false;
    /* Backends deliver complete frames without FCS; Ethernet pads to 60. */
    bool broadcast = true;
    for (unsigned i = 0; i < 6; i++) if (frame[i] != 255) broadcast = false;
    bool group = frame[0] & 1;
    unsigned hash = multicast_hash(frame);
    bool accept = broadcast ? (n->rcr & 4) :
        group ? ((n->rcr & 8) && (n->mar[hash / 8] & (1u << (hash % 8)))) :
        ((n->rcr & 0x10) || memcmp(frame, n->par, 6) == 0);
    if (!accept) { n->rx_filtered++; return false; }
    if (n->rcr & 0x20) { missed(n); return false; }
    if (n->pstart < 0x40 || n->pstop > 0x80 || n->pstart >= n->pstop ||
        n->curr < n->pstart || n->curr >= n->pstop) {
        n->unsupported++; return false;
    }
    size_t padded = len < 60 ? 60 : len;
    unsigned count = padded + 4; /* includes FCS, excludes ring header */
    unsigned pages = (count + 4 + 255) / 256;
    /* DP8390D section 7: wrap at PSTOP, then compare the next page to
     * BNRY. It is a comparator, not a range validator. Magic Cap writes
     * BNRY=PSTOP when consuming a packet ending exactly at the ring end;
     * that value cannot match a wrapped page and must not disable RX.
     * Equal CURR/BNRY is also the documented initial empty-ring state. */
    unsigned available = UINT32_MAX;
    if (n->bnry >= n->pstart && n->bnry < n->pstop) {
        available = n->bnry > n->curr ? n->bnry - n->curr :
                    n->pstop - n->curr + n->bnry - n->pstart;
    }
    if ((n->isr & 0x10) || pages >= available) {
        n->isr |= 0x10; n->rsr = 0x10; n->rx_overruns++; missed(n); return false;
    }
    unsigned next = n->curr + pages;
    if (next >= n->pstop) next -= n->pstop - n->pstart;
    uint8_t packet[1526] = {0};
    packet[0] = 1 | (group ? 0x20 : 0);
    packet[1] = next; packet[2] = count; packet[3] = count >> 8;
    memcpy(packet + 4, frame, len);
    uint32_t crc = ethernet_crc(packet + 4, padded);
    for (unsigned b = 0; b < 4; b++) packet[4 + padded + b] = crc >> (b * 8);
    unsigned addr = n->curr << 8;
    for (unsigned i = 0; i < count + 4; i++) {
        n->ram[addr - 0x4000] = packet[i];
        if (++addr == (unsigned)n->pstop << 8) addr = n->pstart << 8;
    }
    n->curr = next; n->rsr = packet[0]; n->isr |= 1; n->rx_packets++;
    return true;
}
static void transmit(ne2000 *n)
{
    if (n->tx_pending || (n->cr & 3) != 2) return;
    n->tx_requests++;
    unsigned addr = n->tpsr << 8;
    if (addr < 0x4000 || addr + n->tbcr > 0x8000 || n->tbcr < 14 ||
        n->tbcr > sizeof(n->tx_frame)) {
        n->unsupported++; n->tsr = 8; n->isr |= 8; n->cr &= ~4; return;
    }
    n->tx_len = n->tbcr;
    memcpy(n->tx_frame, n->ram + addr - 0x4000, n->tx_len);
    n->tx_pending = true;
    /* 10 Mbit/s, including preamble, FCS and inter-frame gap. */
    n->tx_due = n->now_ns + (n->tx_len + 24) * 800;
}
void mrc_ne2000_tick(ne2000 *n, uint64_t now_ns)
{
    n->now_ns = now_ns;
    if (!n->tx_pending || now_ns < n->tx_due) return;
    n->tx_pending = false; n->cr &= ~4;
    bool ok;
    if ((n->tcr & 6) == 2) {
        /* Internal loopback returns through the same receive-ring logic. */
        ok = true;
        mrc_ne2000_receive(n, n->tx_frame, n->tx_len);
    } else if (n->tcr & 6) {
        n->unsupported++; ok = false; /* external SNI loopback not modeled */
    } else ok = n->send && n->send(n->send_opaque, n->tx_frame, n->tx_len);
    n->tsr = ok ? 1 : 0x10; /* PTX or carrier lost */
    n->isr |= ok ? 2 : 8;
    if (ok) n->tx_packets++;
    if (n->cr & 1) n->isr |= 0x80;
}

static void advance(ne2000 *n)
{
    n->rsar++;
    if (n->pstop > n->pstart && n->rsar == (uint16_t)(n->pstop << 8))
        n->rsar = (uint16_t)(n->pstart << 8);
    if (--n->rbcr == 0) n->isr |= 0x40;
}

static uint8_t dma_read(ne2000 *n)
{
    if ((n->cr & 0x38) != 0x08 || !n->rbcr) return 0xff;
    unsigned a = n->rsar;
    uint8_t b = a < 32 ? n->prom[a] :
                a >= 0x4000 && a < 0x8000 ? n->ram[a - 0x4000] : 0xff;
    advance(n);
    n->dma_reads++;
    return b;
}

static void dma_write(ne2000 *n, uint8_t b)
{
    if ((n->cr & 0x38) != 0x10 || !n->rbcr) return;
    unsigned a = n->rsar;
    if (a >= 0x4000 && a < 0x8000) n->ram[a - 0x4000] = b;
    advance(n);
    n->dma_writes++;
}

uint32_t mrc_ne2000_read(ne2000 *n, unsigned port, unsigned size)
{
    if (port == 0x10 && (size == 1 || size == 2)) {
        uint32_t v = dma_read(n);
        if (size == 2) v |= (uint32_t)dma_read(n) << 8;
        return v;
    }
    if (size != 1) { n->unsupported++; return 0xffffffff; }
    if (port == 0x1f) { reset(n); return 0; }
    if (port == 0) return n->cr;
    if (port > 15) { n->unsupported++; return 0xff; }
    switch (n->cr >> 6) {
    case 0:
        switch (port) {
        case 3: return n->bnry;
        case 4: return n->tsr;
        case 5: case 6: return 0;
        case 7: return n->isr;
        case 8: return n->rsar & 255;
        case 9: return n->rsar >> 8;
        case 12: return n->rsr;
        case 13: case 14: case 15: {
            uint8_t tally = n->tally[port - 13]; n->tally[port - 13] = 0;
            return tally;
        }
        default: n->unsupported++; return 0xff;
        }
    case 1:
        if (port <= 6) return n->par[port - 1];
        if (port == 7) return n->curr;
        return n->mar[port - 8];
    case 2:
        switch (port) {
        case 1: return n->pstart;
        case 2: return n->pstop;
        case 4: return n->tpsr;
        case 12: return n->rcr;
        case 13: return n->tcr;
        case 14: return n->dcr;
        case 15: return n->imr;
        default: n->unsupported++; return 0xff;
        }
    default: n->unsupported++; return 0xff;
    }
}

void mrc_ne2000_write(ne2000 *n, unsigned port, unsigned size, uint32_t value)
{
    if (port == 0x10 && (size == 1 || size == 2)) {
        dma_write(n, (uint8_t)value);
        if (size == 2) dma_write(n, (uint8_t)(value >> 8));
        return;
    }
    if (size != 1) { n->unsupported++; return; }
    uint8_t v = (uint8_t)value;
    if (port == 0x1f) return; /* completes the board reset-port handshake */
    if (port == 0) {
        uint8_t started = n->cr & 2;
        n->cr = (v & ~4) | (n->tx_pending ? 4 : 0);
        if (v & 1) {
            /* STP preserves STA and lets an in-flight frame finish
             * (DP8390D section 10.3). The reset port cancels it instead. */
            n->cr |= started;
            if (!n->tx_pending) n->isr |= 0x80;
        } else if (v & 2) n->isr &= ~0x80;
        if ((v & 0x38) == 8 && !n->rbcr) n->isr |= 0x40;
        if ((v & 4) && (n->cr & 3) == 2) {
            n->cr |= 4; transmit(n);
        }
        return;
    }
    if (port > 15) { n->unsupported++; return; }
    switch (n->cr >> 6) {
    case 0:
        switch (port) {
        case 1: n->pstart = v; break;
        case 2: n->pstop = v; break;
        case 3: n->bnry = v; break;
        case 4: n->tpsr = v; break;
        case 5: n->tbcr = (n->tbcr & 0xff00) | v; break;
        case 6: n->tbcr = (n->tbcr & 0xff) | ((uint16_t)v << 8); break;
        case 7: n->isr &= ~(v & 0x7f); break; /* RST is not W1C */
        case 8: n->rsar = (n->rsar & 0xff00) | v; break;
        case 9: n->rsar = (n->rsar & 0xff) | ((uint16_t)v << 8); break;
        case 10: n->rbcr = (n->rbcr & 0xff00) | v; break;
        case 11: n->rbcr = (n->rbcr & 0xff) | ((uint16_t)v << 8); break;
        case 12: n->rcr = v; break;
        case 13: n->tcr = v; break;
        case 14: n->dcr = v; break;
        case 15: n->imr = v & 0x7f; break;
        }
        break;
    case 1:
        if (port <= 6) n->par[port - 1] = v;
        else if (port == 7) n->curr = v;
        else n->mar[port - 8] = v;
        break;
    default: n->unsupported++; break;
    }
}
