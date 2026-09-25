#include "devices/ne2000/ne2000.h"
#include "devices/pccard/pccard.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "line %d: %s\n", __LINE__, #x); exit(1); } } while (0)
static void wr(ne2000 *n, unsigned port, unsigned value) { mrc_ne2000_write(n, port, 1, value); }
static void dma(ne2000 *n, unsigned addr, unsigned count, unsigned cmd)
{
    wr(n, 0, 0x22); wr(n, 8, addr); wr(n, 9, addr >> 8);
    wr(n, 10, count); wr(n, 11, count >> 8); wr(n, 0, cmd);
}
static unsigned sent;
static bool send_frame(void *opaque, const uint8_t *frame, size_t len)
{
    CHECK(len == 60 && frame[12] == 8 && frame[13] == 6);
    sent++; return true;
}
static void packets(void)
{
    ne2000 n;
    const uint8_t mac[6] = {2,0,0,0x84,0,1};
    uint8_t frame[300] = {0}; memcpy(frame, mac, 6);
    frame[12] = 8; frame[13] = 6;
    mrc_ne2000_init(&n, mac);
    memcpy(n.par, mac, 6); n.pstart = 0x46; n.pstop = 0x80;
    n.curr = 0x7f; n.bnry = 0x50; n.cr = 0x22; n.imr = 0x3f; n.isr = 0;
    CHECK(mrc_ne2000_receive(&n, frame, sizeof(frame)));
    CHECK(n.curr == 0x47 && n.ram[0x3f00] == 1 && n.ram[0x3f01] == 0x47);
    CHECK(n.ram[0x3f02] == 0x30 && n.ram[0x3f03] == 1); /* 300 + FCS */
    CHECK(!memcmp(n.ram + 0x3f04, frame, 252));
    CHECK(!memcmp(n.ram + 0x600, frame + 252, 48));
    CHECK(mrc_ne2000_irq(&n)); wr(&n, 7, 1); CHECK(!mrc_ne2000_irq(&n));
    uint8_t before[16384]; memcpy(before, n.ram, sizeof(before));
    n.bnry = 0x48;
    CHECK(!mrc_ne2000_receive(&n, frame, 60));
    CHECK(n.curr == 0x47 && n.rx_overruns == 1 && (n.isr & 0x10));
    CHECK(!memcmp(before, n.ram, sizeof(before))); /* never overwrite unread data */
    CHECK(mrc_ne2000_read(&n, 15, 1) == 1 && mrc_ne2000_read(&n, 15, 1) == 0);
    wr(&n, 0, 0x21); wr(&n, 7, 0x10); n.bnry = 0x50; wr(&n, 0, 0x22);
    frame[0] = 4;
    CHECK(!mrc_ne2000_receive(&n, frame, 60));
    n.rcr = 0x10; CHECK(mrc_ne2000_receive(&n, frame, 60));
    memset(frame, 255, 6); n.rcr = 0;
    CHECK(!mrc_ne2000_receive(&n, frame, 60));
    n.rcr = 4; CHECK(mrc_ne2000_receive(&n, frame, 60));
    CHECK(n.rsr == 0x21);
    frame[0] = 1; frame[1] = 0; frame[2] = 0x5e; frame[3] = frame[4] = 0; frame[5] = 1;
    n.rcr = 0x10; CHECK(!mrc_ne2000_receive(&n, frame, 60));
    n.rcr = 8; CHECK(!mrc_ne2000_receive(&n, frame, 60));
    n.mar[3] = 0x80; /* Ethernet CRC hash of 01:00:5e:00:00:01 is 31. */
    CHECK(mrc_ne2000_receive(&n, frame, 60));
    n.rcr = 0x20; CHECK(!mrc_ne2000_receive(&n, frame, 60));
    memcpy(n.ram, frame, 60); n.tpsr = 0x40; n.tbcr = 60; n.isr = 0;
    n.send = send_frame; wr(&n, 0, 0x26);
    CHECK(n.tx_pending && (n.cr & 4) && !sent);
    wr(&n, 0, 0x62); CHECK(n.cr & 4); /* page switch cannot clear hardware TXP */
    mrc_ne2000_tick(&n, 67199); CHECK(!sent);
    mrc_ne2000_tick(&n, 67200); CHECK(sent == 1 && !n.tx_pending && !(n.cr & 4));
    CHECK(n.tsr == 1 && (n.isr & 2) && mrc_ne2000_irq(&n));
    wr(&n, 0, 0x22); wr(&n, 7, 2); CHECK(!mrc_ne2000_irq(&n));
    n.send = NULL; wr(&n, 0, 0x26); mrc_ne2000_tick(&n, 134400);
    CHECK(n.tsr == 0x10 && (n.isr & 8) && !(n.isr & 2));
    wr(&n, 0, 0x26); wr(&n, 0, 0x21);
    CHECK(n.tx_pending && (n.cr & 3) == 3); /* STP preserves STA until restart */
    mrc_ne2000_tick(&n, 300000);
    CHECK(!n.tx_pending && sent == 1 && (n.isr & 0x80));
    wr(&n, 0, 0x22); n.rcr = 0x28; n.tally[2] = 0x7f; n.isr = 0;
    CHECK(!mrc_ne2000_receive(&n, frame, 60));
    CHECK(n.tally[2] == 0x80 && (n.isr & 0x20));
}
static void boundary_comparator(void)
{
    ne2000 n;
    const uint8_t mac[6] = {2,0,0,0x84,0,1};
    uint8_t frame[60] = {0}; memcpy(frame,mac,6);
    mrc_ne2000_init(&n,mac); memcpy(n.par,mac,6);
    n.cr=0x22; n.isr=0; n.pstart=0x46; n.pstop=0x80; n.curr=0x7f;
    /* Real guest writes BNRY=PSTOP at wrap. The NIC wraps the next page
     * before comparing it to BNRY; an out-of-ring value cannot match. */
    wr(&n,3,0x80);
    CHECK(mrc_ne2000_receive(&n,frame,sizeof(frame)) && n.curr==0x46);
    CHECK(mrc_ne2000_receive(&n,frame,sizeof(frame)) && n.curr==0x47);
    CHECK(!n.unsupported && !n.rx_overruns);
    n.curr=0x7f; wr(&n,3,0x46);
    CHECK(!mrc_ne2000_receive(&n,frame,sizeof(frame)) && n.curr==0x7f);
    CHECK(n.isr&0x10);
    wr(&n,7,0x10);
    /* The datasheet permits equal CURR/BNRY at initialization. Protection
     * occurs on reaching that page again, not before storing any data. */
    n.pstart=0x40; n.pstop=0x42; n.curr=0x40; wr(&n,3,0x40);
    CHECK(mrc_ne2000_receive(&n,frame,sizeof(frame)) && n.curr==0x41);
    CHECK(!mrc_ne2000_receive(&n,frame,sizeof(frame)) && n.curr==0x41);
}
int main(void)
{
    packets();
    boundary_comparator();
    ne2000 n;
    const uint8_t mac[] = {2,0,0,0x84,0,1};
    mrc_ne2000_init(&n, mac);
    CHECK(mrc_ne2000_read(&n, 0, 1) == 0x21);
    wr(&n, 7, 0xff);
    CHECK(mrc_ne2000_read(&n, 7, 1) == 0x80); /* reset isn't W1C */
    wr(&n, 14, 0x49);
    dma(&n, 0, 32, 0x0a);
    for (unsigned i = 0; i < 16; i++) {
        unsigned b = i < 6 ? mac[i] : i >= 14 ? 0x57 : 0;
        CHECK(mrc_ne2000_read(&n, 0x10, 2) == b * 0x101);
    }
    CHECK(n.rbcr == 0 && (n.isr & 0x40));
    CHECK(!mrc_ne2000_irq(&n));
    wr(&n, 15, 0x40); CHECK(mrc_ne2000_irq(&n));
    wr(&n, 7, 0x40); CHECK(!mrc_ne2000_irq(&n));
    CHECK(mrc_ne2000_read(&n, 0x10, 2) == 0xffff && n.rbcr == 0);
    dma(&n, 0x4000, 16384, 0x12);
    for (unsigned i = 0; i < 8192; i++) mrc_ne2000_write(&n, 0x10, 2, i ^ 0x5a5a);
    CHECK(n.rbcr == 0 && n.rsar == 0x8000);
    dma(&n, 0x4000, 16384, 0x0a);
    for (unsigned i = 0; i < 8192; i++) CHECK(mrc_ne2000_read(&n, 0x10, 2) == (i ^ 0x5a5a));
    wr(&n, 1, 0x40); wr(&n, 2, 0x80);
    dma(&n, 0x7fff, 3, 0x12);
    mrc_ne2000_write(&n, 0x10, 2, 0x1234);
    mrc_ne2000_write(&n, 0x10, 2, 0x5678);
    CHECK(n.ram[0x3fff] == 0x34 && n.ram[0] == 0x12 && n.ram[1] == 0x78);
    CHECK(n.rbcr == 0 && n.rsar == 0x4002);
    wr(&n, 0, 0x62);
    for (unsigned i = 0; i < 6; i++) wr(&n, i + 1, mac[i]);
    wr(&n, 7, 0x46);
    CHECK(memcmp(n.par, mac, 6) == 0 && mrc_ne2000_read(&n, 7, 1) == 0x46);
    wr(&n, 0, 0xa2);
    CHECK(mrc_ne2000_read(&n, 1, 1) == 0x40);
    CHECK(mrc_ne2000_read(&n, 2, 1) == 0x80);
    CHECK(!n.unsupported);

    mrc_pccard c;
    mrc_pccard_init(&c, 0, &mrc_pccard_ne2000);
    mrc_ne2000_init(&c.nic, mac);
    uint32_t value;
    CHECK(c.kind->read(&c, MRC_PCCARD_WINDOW_A, 0, 1, &value) && value == 1);
    CHECK(!c.kind->read(&c, MRC_PCCARD_WINDOW_A, 1, 1, &value));
    CHECK(!c.kind->read(&c, MRC_PCCARD_WINDOW_B, 0x300, 1, &value));
    bool present = false;
    mrc_pccard_port port = { .card = &c, .window = MRC_PCCARD_WINDOW_A,
                            .present = &present };
    CHECK(mrc_pccard_read(&port, 0, 1) == 0xff);
    mrc_pccard_write(&port, 0x3f8, 1, 0x60);
    CHECK(c.config == 0);
    present = true;
    CHECK(mrc_pccard_read(&port, 0, 1) == 1);
    CHECK(c.kind->write(&c, MRC_PCCARD_WINDOW_A, 0x3f8, 1, 0x60));
    CHECK(c.kind->read(&c, MRC_PCCARD_WINDOW_A, 0x300, 1, &value) && value == 0x21);
    CHECK(!c.kind->read(&c, MRC_PCCARD_WINDOW_B, 0x300, 1, &value));
    CHECK(!c.kind->read(&c, MRC_PCCARD_WINDOW_A, 0x320, 1, &value));
    /* The 68k-only CIS extension must leave MIPS bytes unchanged and carry
     * the CRC format the PIC-2000 ROM validates for a MagicCapTuple. */
    unsigned magic_at = 0;
    uint8_t byte = 0;
    for (unsigned at = 0; at < 200; at += 2) {
        if (!mrc_ne2000_magic_attribute_byte(at, 0, &byte)) break;
        if (byte == 0xa0) { magic_at = at; break; }
    }
    CHECK(magic_at != 0);
    CHECK(mrc_ne2000_magic_attribute_byte(magic_at + 2, 0, &byte) && byte == 32);
    uint8_t tuple[32];
    for (unsigned i = 0; i < 32; ++i)
        CHECK(mrc_ne2000_magic_attribute_byte(magic_at + 4 + i * 2, 0, &tuple[i]));
    CHECK(memcmp(tuple, "GMMC\0\1\0\1IOCD", 12) == 0);
    uint32_t crc = 0;
    for (unsigned i = 0; i < 28; ++i) {
        crc ^= tuple[i];
        for (unsigned bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ ((crc & 1) ? 0xedb88320u : 0);
    }
    CHECK(crc == ((uint32_t)tuple[28] << 24 | (uint32_t)tuple[29] << 16 |
                  (uint32_t)tuple[30] << 8 | tuple[31]));
    CHECK(mrc_ne2000_card_attribute_byte(magic_at, 0, &byte) && byte == 0xff);
    /* Guest's BE EtherType store must retain Ethernet byte order in NIC RAM. */
    dma(&c.nic, 0x4000, 2, 0x12);
    CHECK(c.kind->write(&c, MRC_PCCARD_WINDOW_A, 0x310, 2, 0x0806));
    CHECK(c.nic.ram[0] == 8 && c.nic.ram[1] == 6);
    dma(&c.nic, 0x4000, 2, 0x0a);
    CHECK(c.kind->read(&c, MRC_PCCARD_WINDOW_A, 0x310, 2, &value) && value == 0x0806);
    wr(&c.nic, 0, 0x26);
    CHECK(c.nic.tx_requests == 1 && !(c.nic.isr & 2)); /* no fabricated TX success */
    CHECK(c.kind->write(&c, MRC_PCCARD_WINDOW_A, 0x3f8, 1, 0x80));
    CHECK(!c.kind->read(&c, MRC_PCCARD_WINDOW_B, 0x300, 1, &value));
    CHECK(!c.kind->read(&c, MRC_PCCARD_WINDOW_A, 0x300, 1, &value));
    puts("NE2000 station PROM, DMA, register pages, IRQ and CIS configuration passed");
    return 0;
}
