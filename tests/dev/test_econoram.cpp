#include "devices/econoram/econoram.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>

static void check(bool b, const char *why) { if (!b) { std::fprintf(stderr,"FAIL: %s\n",why); std::exit(1); } }
int main()
{
    EconoRam ram;
    uint64_t now = 0;
    constexpr unsigned hz = 1000000;
    auto bit = [&](bool value) {
        ram.drive(true,now,hz);
        check(!ram.line(now,hz),"host drive overrides device output");
        ram.drive(false,now+(value?5:65),hz);
        now += 80;
    };
    auto command = [&](unsigned value) { for (unsigned i=0;i<8;i++) bit((value>>i)&1); };
    for (unsigned i=0;i<264;i++) bit(false);
    check(ram.transactions==0,"synchronization does not invent a transaction");
    uint8_t expected[32];
    for (unsigned i=0;i<32;i++) expected[i]=uint8_t(i*73+19);
    command(0xf9);
    for (unsigned i=0;i<256;i++) bit((expected[i/8]>>(i&7))&1);
    check(!std::memcmp(expected,ram.bytes,32),"write all 256 bits in wire order");
    check(ram.written_bits==256,"exact transaction length");
    for (unsigned repeat=0;repeat<2;repeat++) {
        command(0x01);
        for (unsigned i=0;i<256;i++) {
            ram.drive(true,now,hz);
            ram.drive(false,now+5,hz);
            check(ram.line(now+15,hz)==bool((expected[i/8]>>(i&7))&1),"serial read returns stored bit");
            check(ram.line(now+60,hz),"open-drain output releases after read slot");
            now+=80;
        }
    }
    check(ram.written_bits==256,"reads never modify memory");
    command(0xfb); /* nonzero chip address: not this device */
    for (unsigned i=0;i<264;i++) bit(false);
    check(!std::memcmp(expected,ram.bytes,32),"unselected address cannot write RAM");
    check(ram.transactions==3,"invalid address does not start a transaction");
    std::puts("EconoRAM protocol tests passed");
}
