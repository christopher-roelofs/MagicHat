#include "machines/datarover840/keyboard.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"line %d: %s\n",__LINE__,#x); exit(1); } } while (0)
static tx39 soc;
static mh_dr_keyboard keyboard;
static uint8_t ram[256];
static void wr(uint32_t off,uint32_t v) { CHECK(mh_mbus_write(&soc.mbus,off,v)); }
static void cmd(uint16_t word) {
    wr(TX39_MBUSCTRL,0x8a3); wr(TX39_MBUSCOMMAND,word);
}
static void read_packet(uint16_t command,unsigned length) {
    memset(ram,0xcc,sizeof(ram));
    cmd(command);
    wr(TX39_MBUSDMASTART,0x1000); wr(TX39_MBUSDMALENGTH,length-4);
    wr(TX39_MBUSCTRL,0x108ab);
    mh_dr_keyboard_service(&keyboard);
    CHECK(!keyboard.pending_read);
}
int main(void) {
    mh_bus bus; mh_bus_init(&bus);
    mh_bus_add_ram(&bus,"ram",0x1000,ram,sizeof(ram),sizeof(ram));
    r3900 cpu={0}; cpu.bus=&bus; mh_tx39_init(&soc,&cpu,36864000);
    mh_dr_keyboard_init(&keyboard,&soc.mbus);
    mh_mbus_connect(&soc.mbus,&keyboard.port);
    CHECK(!keyboard.port.input_high);
    cmd(0xdef0); CHECK(keyboard.port.input_high); // broadcast31
    cmd(0xdca8); CHECK(keyboard.assigned); // assign address0
    cmd(0xdce0); CHECK(!keyboard.port.input_high); // no next device
    cmd(0xcc5c); // select ID
    cmd(0xcc24); // read ID, defer until guest enables slave
    mh_dr_keyboard_service(&keyboard); CHECK(keyboard.pending_read==2);
    wr(TX39_MBUSCTRL,0x8ab); mh_dr_keyboard_service(&keyboard);
    CHECK(soc.mbus.reg[(TX39_MBUSPAYLOAD-TX39_MBUS_FIRST)/4]==MH_MBKEY_ID);
    cmd(0xcc60); read_packet(0xcc24,256); // descriptor
    unsigned length=(ram[2]<<8)|ram[3], sum=1;
    CHECK(length+4<256 && ram[4]=='M' && ram[7]=='B');
    for(unsigned i=0;i<length+2;++i)sum+=ram[i];
    CHECK((sum&0xffff)==(unsigned)((ram[length+2]<<8)|ram[length+3]));
    CHECK(ram[length+4]==0xcc); // no write outside packet
    // More than one scan read, with a prefix crossing the packet boundary.
    for(unsigned i=0;i<14;++i)CHECK(mh_dr_keyboard_key(&keyboard,0x1c,false,true));
    CHECK(mh_dr_keyboard_key(&keyboard,0x75,true,false));
    CHECK(keyboard.port.input_high);
    cmd(0xdcc8); CHECK(keyboard.port.input_high); // request poll
    read_packet(0xcc18,16); CHECK(ram[1]==0x0e);
    CHECK(!keyboard.port.input_high && keyboard.notified);
    CHECK(mh_dr_keyboard_key(&keyboard,0x32,false,true));
    CHECK(!keyboard.port.input_high); // wait for outstanding request to be read
    read_packet(0xcc24,16);
    CHECK(ram[0]==15 && ram[15]==0xe0 && keyboard.keys.count==3);
    CHECK(keyboard.port.input_high); // unread scan bytes request another read
    read_packet(0xcc18,16); read_packet(0xcc24,16);
    CHECK(ram[0]==3 && ram[1]==0xf0 && ram[2]==0x75 && ram[3]==0x32);
    CHECK(!keyboard.keys.count && !keyboard.port.input_high);
    // Real command5 payload, delivered through TX DMA, updates LED state.
    const uint8_t led[]={0x4b,2,0xed,5,0,0,0,0}; memcpy(ram,led,8);
    cmd(0xcc3c); wr(TX39_MBUSDMASTART,0x1000); wr(TX39_MBUSDMALENGTH,4);
    wr(TX39_MBUSCTRL,0x88a3); CHECK(keyboard.keys.leds==5);
    cmd(0xcc3c); wr(TX39_MBUSPAYLOAD,0x4b02ed80); wr(TX39_MBUSPAYLOAD,0);
    CHECK(keyboard.rejected_writes==1 && keyboard.keys.leds==5);
    CHECK(mh_dr_keyboard_key(&keyboard,0x12,false,true));
    cmd(0xcc3c); wr(TX39_MBUSPAYLOAD,0x4b01ff00); wr(TX39_MBUSPAYLOAD,0);
    CHECK(!keyboard.keys.count && !keyboard.keys.leds && !keyboard.port.input_high);
    // Failed receive doesn't consume queued keys or fabricate completion.
    CHECK(mh_dr_keyboard_key(&keyboard,0x1c,false,true));
    cmd(0xcc24); wr(TX39_MBUSDMASTART,0x9000); wr(TX39_MBUSDMALENGTH,12);
    wr(TX39_MBUSCTRL,0x108ab); mh_dr_keyboard_service(&keyboard);
    CHECK(keyboard.receive_errors==1 && keyboard.keys.count==1);
    cmd(0xcc24); wr(TX39_MBUSCTRL,0x8a9); // unsupported 16-bit receive
    mh_dr_keyboard_service(&keyboard);
    CHECK(keyboard.receive_errors==2 && keyboard.keys.count==1);
    CHECK(!keyboard.unknown_commands);
    cmd(0xffff); CHECK(keyboard.unknown_commands==1);
    // HID input preserves modifier order even within one SDL polling batch.
    mh_dr_keyboard_init(&keyboard,&soc.mbus);
    mh_mbus_connect(&soc.mbus,&keyboard.port);
    CHECK(mh_dr_keyboard_host_key(&keyboard,225,true,false)); // left Shift
    CHECK(mh_dr_keyboard_host_key(&keyboard,5,true,false)); // B position
    CHECK(mh_dr_keyboard_host_key(&keyboard,5,false,false));
    CHECK(mh_dr_keyboard_host_key(&keyboard,225,false,false));
    uint8_t packet[16];
    CHECK(mh_mb_keyboard_read(&keyboard.keys,packet,16)==7);
    CHECK(!memcmp(packet,(uint8_t[]){6,0x12,0x32,0xf0,0x32,0xf0,0x12},7));
    CHECK(!mh_dr_keyboard_host_key(&keyboard,72,true,false)); // Pause unsupported
    CHECK(mh_dr_keyboard_host_key(&keyboard,228,true,false)); // right Control E0
    // A full queue must not lose the release when the window loses focus.
    while(keyboard.keys.count<256) CHECK(mh_dr_keyboard_key(&keyboard,0x1c,false,true));
    mh_dr_keyboard_release(&keyboard);
    mh_dr_keyboard_service(&keyboard);
    CHECK(keyboard.release[228/8]&(1u<<(228&7)));
    uint8_t all[256]; mh_mb_keyboard_read(&keyboard.keys,all,256);
    mh_dr_keyboard_service(&keyboard);
    CHECK(!keyboard.release[228/8]);
    CHECK(mh_mb_keyboard_read(&keyboard.keys,packet,16)==5);
    CHECK(!memcmp(packet,(uint8_t[]){4,0x1c,0xe0,0xf0,0x14},5));
    // Pointer-free state restores pending transactions without a new IRQ edge.
    CHECK(mh_dr_keyboard_host_key(&keyboard,4,true,false));
    keyboard.assigned=true; keyboard.selection=13; keyboard.pending_read=2;
    keyboard.port.rx_bytes=12; keyboard.port.rx_complete=true;
    uint8_t state[MH_DR_KEYBOARD_STATE_SIZE], roundtrip[MH_DR_KEYBOARD_STATE_SIZE];
    mh_dr_keyboard_encode(&keyboard,state);
    uint32_t irq=soc.icu.status[1];
    mh_dr_keyboard_init(&keyboard,&soc.mbus);
    CHECK(mh_dr_keyboard_decode(&keyboard,state));
    CHECK(soc.mbus_port==&keyboard.port && soc.icu.status[1]==irq);
    mh_dr_keyboard_encode(&keyboard,roundtrip);
    CHECK(!memcmp(state,roundtrip,sizeof(state)));
    state[12]=2; CHECK(!mh_dr_keyboard_decode(&keyboard,state));
    CHECK(keyboard.keys.count==1 && keyboard.pending_read==2);
    mh_mbus_connect(&soc.mbus,NULL);
    puts("DataRover keyboard discovery, repeated requests, control DMA and failure handling passed");
}
