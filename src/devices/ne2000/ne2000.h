/* NE2000 register/DMA model. DP8390D July 1995 datasheet sections 9–11;
 * NE2000 PROM/data/reset ports: Linux drivers/net/ethernet/8390/ne.c.
 * Packet frames exclude the Ethernet FCS at the host transport boundary. */
#ifndef MH_NE2000_H
#define MH_NE2000_H
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
typedef bool (*mh_ne2000_send)(void *, const uint8_t *, size_t);
typedef struct {
    uint8_t cr, isr, imr, dcr, rcr, tcr, tsr, rsr;
    uint8_t pstart, pstop, bnry, curr, tpsr;
    uint8_t par[6], mar[8], prom[32], ram[16384];
    uint16_t rsar, rbcr, tbcr;
    uint64_t dma_reads, dma_writes, unsupported, tx_requests;
    uint64_t rx_packets, rx_filtered, rx_overruns, tx_packets;
    uint8_t tally[3];
    bool tx_pending;
    uint64_t now_ns, tx_due;
    uint8_t tx_frame[1518];
    size_t tx_len;
    mh_ne2000_send send;
    void *send_opaque;
} ne2000;
void mh_ne2000_init(ne2000 *n, const uint8_t mac[6]);
uint32_t mh_ne2000_read(ne2000 *n, unsigned port, unsigned size);
void mh_ne2000_write(ne2000 *n, unsigned port, unsigned size, uint32_t value);
bool mh_ne2000_irq(const ne2000 *n);
void mh_ne2000_tick(ne2000 *n, uint64_t now_ns);
bool mh_ne2000_receive(ne2000 *n, const uint8_t *frame, size_t len);
#endif
