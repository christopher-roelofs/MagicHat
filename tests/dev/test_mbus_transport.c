#include "soc/tx39/tx39.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"line %d: %s\n",__LINE__,#x); exit(1); } } while (0)
struct capture { uint16_t command; uint32_t words[4]; unsigned count, bits; };
static void command(void *ctx,uint16_t word) { ((struct capture*)ctx)->command=word; }
static void transmit(void *ctx,uint32_t word,unsigned bits) {
    struct capture *c=ctx; CHECK(c->count<4); c->words[c->count++]=word; c->bits=bits;
}
static void wr(tx39 *s,uint32_t reg,uint32_t val) { mh_tx39_write(s,reg,4,val); }
static uint32_t rd(tx39 *s,uint32_t reg) { return mh_tx39_read(s,reg,4); }
int main(void) {
    uint8_t ram[32]; memset(ram,0xcc,sizeof(ram));
    mh_bus bus; mh_bus_init(&bus); mh_bus_add_ram(&bus,"ram",0x1000,ram,sizeof(ram),sizeof(ram));
    r3900 cpu={0}; cpu.bus=&bus; tx39 soc; mh_tx39_init(&soc,&cpu,36864000);
    struct capture c={0}; tx39_mbus_port port={.ctx=&c,.command=command,.transmit=transmit,.input_high=true};
    mh_mbus_connect(&soc.mbus,&port);
    CHECK(!mh_mbus_receive_word(&soc.mbus,123));
    CHECK(!mh_mbus_receive_command(&soc.mbus,123));
    wr(&soc,TX39_MBUSCTRL,0x8a3); wr(&soc,TX39_MBUSCOMMAND,0xcc24);
    CHECK(c.command==0xcc24 && (rd(&soc,TX39_MBUSCTRL)&0x80000000));
    wr(&soc,TX39_MBUSPAYLOAD,0x01020304); CHECK(c.count==1 && c.words[0]==0x01020304 && c.bits==32);
    // Receive a non-DMA short word, followed by the peripheral's end command.
    wr(&soc,TX39_MBUSCTRL,0x8a9);
    CHECK(mh_mbus_receive_word(&soc.mbus,0xabcd1234));
    CHECK(rd(&soc,TX39_MBUSPAYLOAD)==0x1234);
    CHECK(mh_mbus_receive_command(&soc.mbus,0xc000));
    CHECK(rd(&soc,TX39_MBUSCOMMAND)==0xc000);
    CHECK(rd(&soc,TX39_INTRSTATUS(2))&INT2_MBUSDET);
    wr(&soc,TX39_INTRCLEAR(2),INT2_MBUSDET|INT2_MBUSRXBUFAVAIL);
    mh_mbus_update(&soc.mbus);
    CHECK(!(rd(&soc,TX39_INTRSTATUS(2))&(INT2_MBUSDET|INT2_MBUSRXBUFAVAIL)));
    // Three DMA words: short receive count, then length-minus-four endpoint.
    wr(&soc,TX39_MBUSCTRL,0); wr(&soc,TX39_MBUSDMASTART,0x1004); wr(&soc,TX39_MBUSDMALENGTH,8);
    wr(&soc,TX39_MBUSCTRL,0x108ab);
    CHECK(mh_mbus_receive_word(&soc.mbus,0x01020304));
    CHECK(rd(&soc,TX39_MBUSDMACOUNT)==4 && !(rd(&soc,TX39_INTRSTATUS(2))&0x20));
    CHECK(mh_mbus_receive_word(&soc.mbus,0x05060708));
    CHECK(rd(&soc,TX39_INTRSTATUS(2))&0x10);
    CHECK(mh_mbus_receive_word(&soc.mbus,0x090a0b0c));
    CHECK(rd(&soc,TX39_MBUSDMACOUNT)==8 && (rd(&soc,TX39_INTRSTATUS(2))&0x20));
    CHECK(!mh_mbus_receive_word(&soc.mbus,0xdeadbeef));
    for(unsigned i=0;i<12;++i) CHECK(ram[4+i]==i+1);
    CHECK(ram[3]==0xcc && ram[16]==0xcc);
    // DMA transmit follows the same physical buffer and stops at its boundary.
    c.count=0; wr(&soc,TX39_MBUSCTRL,0); wr(&soc,TX39_MBUSCTRL,0x88a3);
    CHECK(c.count==3 && c.words[0]==0x01020304 && c.words[2]==0x090a0b0c);
    wr(&soc,TX39_MBUSCTRL,0x88a3); CHECK(c.count==3); // no replay on unchanged enable
    wr(&soc,TX39_MBUSCTRL,0); CHECK(!(rd(&soc,TX39_MBUSCTRL)&0x80000000));
    CHECK(!mh_mbus_receive_word(&soc.mbus,0));
    wr(&soc,TX39_INTRCLEAR(2),INT2_MBUS_MASK);
    mh_mbus_input(&soc.mbus,false); CHECK(rd(&soc,TX39_INTRSTATUS(2))&4);
    wr(&soc,TX39_INTRCLEAR(2),4); mh_mbus_input(&soc.mbus,false); CHECK(!(rd(&soc,TX39_INTRSTATUS(2))&4));
    mh_mbus_input(&soc.mbus,true); CHECK(rd(&soc,TX39_INTRSTATUS(2))&8);
    // A DMA fault raises an error, not a successful completion.
    wr(&soc,TX39_INTRCLEAR(2),INT2_MBUS_MASK);
    wr(&soc,TX39_MBUSDMASTART,0x9000); wr(&soc,TX39_MBUSCTRL,0x108ab);
    CHECK(!mh_mbus_receive_word(&soc.mbus,0));
    CHECK(rd(&soc,TX39_INTRSTATUS(2))&INT2_MBUSRXERR);
    CHECK(!(rd(&soc,TX39_INTRSTATUS(2))&0x20));
    mh_mbus_connect(&soc.mbus,NULL);
    CHECK(rd(&soc,TX39_MBUSCTRL)&MBUSCTRL_INPUT_HIGH);
    puts("MBUS command, RX/TX DMA, boundaries, faults and physical line edges passed");
}
