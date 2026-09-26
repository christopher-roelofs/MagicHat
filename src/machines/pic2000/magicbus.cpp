#include "machines/pic2000/board.h"
#include <algorithm>
#include <cstdlib>
#include <cstring>

static const uint16_t codes[] = {
    0,0xc018,0xc024,0xc028,0xc030,0xc03c,0xc044,0xc048,0xc050,
    0xc000,0xc00c,0xc014,0xc05c,0xc060,0xc06c,0xc074,0xc078,
    0xc084,0xc088,0xc090,0xc09c,0xd0e0,0xd0ec,0xd0a4,0xd0a8,
    0xd0b0,0xd0bc,0xd0c4,0xd0c8,0xd0d0,0xd0dc,0xd0f4,0xd0f8
};
static const uint16_t addresses[] = {0,0x204,0x404,0x600,0x804,0xa00,0xc00,0xe04};
static void put32(uint8_t *p, unsigned at, uint32_t v) {
    for (int i=3;i>=0;--i) { p[at+i]=uint8_t(v); v>>=8; }
}
static unsigned information(uint8_t *p, uint32_t id) {
    put32(p,4,id);
    put32(p,0xc,256000); put32(p,0x14,256000); put32(p,0x1c,256000);
    put32(p,0x24,16); put32(p,0x28,16);
    put32(p,0x2c,10000000); put32(p,0x30,10000000); p[0x40]=16;
    unsigned at=0x50;
    for (const char *s : {"Emulated AT keyboard","Keyboard","MagicHat"}) {
        unsigned n=unsigned(std::strlen(s)); p[at++]=uint8_t(n);
        std::memcpy(p+at,s,n); at+=n;
    }
    unsigned n=(at+1+2+3)&~3u;
    p[2]=uint8_t((n-4)>>8); p[3]=uint8_t(n-4);
    unsigned sum=1; for(unsigned i=0;i<n-2;++i) sum+=p[i];
    p[n-2]=uint8_t(sum>>8); p[n-1]=uint8_t(sum); return n;
}
void PicMagicBus::line(m68k_machine *m, bool high) {
    if (input_high==high) return;
    input_high=high;
    if(m->envoy) m->dev21.set_bit32(0xba,high?5:4);
    else m->dev21.set_bit32(0xb0,high?9:8);
}
void PicMagicBus::command(m68k_machine *m, uint16_t word) {
    ++commands;
    if (std::getenv("MH_PIC_MBUS_TRACE"))
        fprintf(m->log,"[pic-mbus] command %04X pc=%08X\n",word,m->core.pc);
    for(unsigned a=0;a<8;++a) for(unsigned op=1;op<33;++op) {
        if ((codes[op]^addresses[a])!=word) continue;
        if(op==31 && a==7) {
            assigned=notified=false; selection=pending_read=0; write_size=0;
            line(m,true); return;
        }
        if(op==24) { assigned=true; address=uint8_t(a); return; }
        if(!assigned || (a!=address && a!=7)) return;
        if(op==21) { line(m,false); return; }
        if(op==28) { line(m,keys.count && !notified); return; }
        switch(op) {
        case 1: case 2: pending_read=uint8_t(op); return;
        case 5: selection=5; write_size=0; return;
        case 12: case 13: selection=uint8_t(op); return;
        case 9: case 32: return;
        default: ++errors; return;
        }
    }
    ++errors;
}
void PicMagicBus::write(m68k_machine *m, unsigned off, unsigned size, uint32_t value) {
    if(!connected) return;
    auto &r=m->dev21;
    if(off==0x90 && size==2) {
        /* +B2 bit 11 follows transmitter enable; bit 10 signals ready.
         * ROM 0E082D0C/0E082D1A waits for those two clock phases. */
        if(value&1) r.set_bit32(0xb0,11);
        else r.reg[0xb2/2]&=~0x800u;
        r.set_bit32(0xb0,10);
    }
    if(off==0x96 && size==2) {
        command_high=uint8_t(value); command_high_valid=true;
        r.set_bit32(0xb0,10);
    }
    if(off==0x94 && size==2) {
        if(command_high_valid) {
            command_high_valid=false;
            command(m,uint16_t(unsigned(command_high)<<8 | (value&255)));
        } else { command_high=uint8_t(value); command_high_valid=true; }
        r.set_bit32(0xb0,10);
        if(r.reg[0x90/2]&1) r.set_bit32(0xb0,11);
    }
    if(off==0x98 && size==2 && selection==5 && write_size<=6) {
        write_data[write_size++]=uint8_t(value>>8);
        write_data[write_size++]=uint8_t(value);
    }
    r.tx_dirty=false;
}
static uint32_t sim32(const m68k_machine *m,unsigned off) {
    return uint32_t(m->sim.reg[off/2])<<16 | m->sim.reg[off/2+1];
}
static void sim32(m68k_machine *m,unsigned off,uint32_t v) {
    m->sim.reg[off/2]=uint16_t(v>>16); m->sim.reg[off/2+1]=uint16_t(v);
}
static void dma_status(m68k_machine *m,unsigned bits) {
    m->sim.reg[0x7a8/2]&=~1u;
    m->sim.reg[0x7aa/2]|=uint16_t((0x80u|bits)<<8);
    m->dev21.irq_dirty=true;
}
void PicMagicBus::service(m68k_machine *m) {
    if(!connected) return;
    auto &r=m->dev21;
    uint16_t &ctl=m->sim.reg[0x7a8/2];
    if(!(ctl&1) || !(r.reg[0x90/2]&8)) return;
    uint32_t src=sim32(m,0x7ac), dst=sim32(m,0x7b0), count=sim32(m,0x7b4);
    /* This connection supports the ROM's dual-address, word/word,
     * external-request transfers. Other DMA clients remain unmodeled. */
    if ((!pending_read || src!=0x21000092) &&
        (selection!=5 || dst!=0x21000098)) return;
    if(!count || (count&1) || (src&1) || (dst&1) ||
       (ctl&0x03c2)!=0x0280 || (ctl&0x30)!=0x20) {
        ++errors; dma_status(m,8); return;
    }
    if(pending_read && src==0x21000092) {
        uint8_t data[256]={}; unsigned n=0;
        mh_mb_keyboard after=keys;
        bool request=pending_read==1, scan=false;
        if(request) { data[1]=keys.count?0xe:0; n=2; }
        else if(selection==12) { put32(data,0,m->hix?0x41544b42:MH_MBKEY_ID); n=4; }
        else if(selection==13) {
            n=information(data,m->hix?0x41544b42:MH_MBKEY_ID)-2;
            /* The 16-bit PIC transport starts with the length; TX39's
             * longword transport has a leading zero halfword. */
            std::memmove(data,data+2,n);
        }
        else if(!selection) {
            n=unsigned(mh_mb_keyboard_read(&after,data,std::min(16u,count)));
            scan=true;
        }
        else { ++errors; pending_read=0; return; }
        n=std::min((n+1)&~1u,count);
        if(dst>UINT32_MAX-n || !(ctl&0x400) || (ctl&0x800)) {
            ++errors; dma_status(m,8); return;
        }
        for(unsigned i=0;i<n;i+=2) {
            bool ok;
            /* Never turn a malformed peripheral DMA target into recursive
             * controller accesses. This connection transfers to RAM. */
            ok=mh_bus_page_host(&m->bus,dst+i,true)!=nullptr;
            if(ok) mh_bus_write(&m->bus,dst+i,2,
                uint16_t(unsigned(data[i])<<8|data[i+1]),&ok);
            if(!ok) {
                sim32(m,0x7b0,dst+i); sim32(m,0x7b4,count-i);
                ++errors; pending_read=0; dma_status(m,0x10); return;
            }
        }
        sim32(m,0x7b0,dst+n); sim32(m,0x7b4,count-n);
        pending_read=0; ++reads;
        if(request) notified=true;
        else { selection=0; if(scan) { keys=after; notified=false; } }
        retry_releases(m);
        line(m,keys.count && !notified);
        ctl&=~1u;
        if(n==count) dma_status(m,0x40);
        /* Allow the guest to arm/acknowledge its end-command interrupt
         * after enabling receive DMA (0E082D28..0E082D36). */
        r.tx_busy=true; r.tx_done_at=m->cpu.insns+64;
        if(std::getenv("MH_PIC_MBUS_TRACE"))
            fprintf(m->log,"[pic-mbus] RX %u/%u @%08X pc=%08X\n",n,count,dst,m->core.pc);
    } else if(selection==5 && dst==0x21000098) {
        if(count>8-write_size || src>UINT32_MAX-count ||
           !(ctl&0x800) || (ctl&0x400)) { ++errors; dma_status(m,8); return; }
        for(unsigned i=0;i<count;i+=2) {
            bool ok;
            uint32_t span=0;
            ok=mh_bus_read_span(&m->bus,src+i,&span)!=nullptr && span>=2;
            uint16_t v=ok?uint16_t(mh_bus_read(&m->bus,src+i,2,&ok)):0;
            if(!ok) {
                sim32(m,0x7ac,src+i); sim32(m,0x7b4,count-i);
                ++errors; dma_status(m,0x20); return;
            }
            write(m,0x98,2,v);
        }
        sim32(m,0x7ac,src+count); sim32(m,0x7b4,0); ctl&=~1u;
        dma_status(m,0x40);
        if(std::getenv("MH_PIC_MBUS_TRACE")) {
            fprintf(m->log,"[pic-mbus] TX:");
            for(unsigned i=0;i<write_size;++i) fprintf(m->log," %02X",write_data[i]);
            fprintf(m->log,"\n");
        }
        // HIX 0E056FEC primes a zero-extended byte, then DMA starts at
        // buffer+2. Its wire header is 00,'K', not 'K',count. Interpret
        // only the three known AT commands for this legacy endpoint;
        // never repair or overwrite the guest's DMA source buffer.
        uint8_t packet[8]; std::memcpy(packet,write_data,8);
        if(m->hix && packet[0]==0 && packet[1]=='K') {
            packet[0]='K';
            packet[1]=packet[2]==0xff?1:packet[2]==0xed?2:packet[2]==0xf3?3:0;
        }
        if(write_size==8 && mh_mb_keyboard_write(&keys,packet,8)) ++writes;
        else ++errors;
        selection=0; write_size=0;
        if(!keys.count) notified=false;
        line(m,keys.count && !notified);
        r.set_bit32(0xb0,11);
        /* DMA normal completion uses the channel's programmed vector. */
    }
}

bool PicMagicBus::host_key(m68k_machine *m,unsigned usage,bool down,bool repeat) {
    uint8_t code; bool extended;
    if(!connected || usage>=256 || !mh_mb_keyboard_usage(usage,&code,&extended)) return false;
    unsigned at=usage/8; uint8_t mask=uint8_t(1u<<(usage&7));
    if(!down) {
        if(held[at]&mask) { held[at]&=uint8_t(~mask); release[at]|=mask; }
        retry_releases(m); return true;
    }
    if(release[at]&mask) return false;
    if((held[at]&mask) && !repeat) return true;
    if(repeat && !(held[at]&mask)) return true;
    if(!mh_mb_keyboard_key(&keys,code,extended,true)) return false;
    held[at]|=mask;
    if(assigned) line(m,keys.count && !notified);
    return true;
}
void PicMagicBus::release_keys(m68k_machine *m) {
    for(unsigned i=0;i<32;++i) { release[i]|=held[i]; held[i]=0; }
    retry_releases(m);
}
void PicMagicBus::retry_releases(m68k_machine *m) {
    for(unsigned u=0;u<256;++u) {
        unsigned at=u/8; uint8_t mask=uint8_t(1u<<(u&7));
        if(!(release[at]&mask)) continue;
        uint8_t code; bool ext;
        if(!mh_mb_keyboard_usage(u,&code,&ext)) { release[at]&=uint8_t(~mask); continue; }
        if(keys.count+(ext?3u:2u)>MH_MBKEY_CAPACITY) break;
        if(mh_mb_keyboard_key(&keys,code,ext,false)) release[at]&=uint8_t(~mask);
    }
    if(assigned) line(m,keys.count && !notified);
}
void PicMagicBus::encode(uint8_t *d) const {
    std::memset(d,0,state_size);
    d[0]=1; d[1]=connected; d[2]=input_high; d[3]=assigned; d[4]=notified;
    d[5]=command_high_valid; d[6]=command_high; d[7]=address;
    d[8]=selection; d[9]=pending_read; d[10]=uint8_t(write_size);
    d[11]=keys.leds; d[12]=keys.repeat;
    d[14]=uint8_t(keys.count>>8); d[15]=uint8_t(keys.count);
    for(unsigned i=0;i<keys.count;++i) d[16+i]=keys.queue[(keys.head+i)%MH_MBKEY_CAPACITY];
    std::memcpy(d+272,write_data,8);
    std::memcpy(d+280,held,32); std::memcpy(d+312,release,32);
}
bool PicMagicBus::decode(const uint8_t *d) {
    unsigned count=unsigned(d[14])<<8|d[15];
    if(d[0]!=1 || d[1]>1 || d[2]>1 || d[3]>1 || d[4]>1 || d[5]>1 ||
       d[7]>6 || (d[8]!=0 && d[8]!=5 && d[8]!=12 && d[8]!=13) ||
       d[9]>2 || d[10]>8 || (d[10]&1) || d[11]>7 || d[12]>127 ||
       count>MH_MBKEY_CAPACITY) return false;
    for(unsigned u=0;u<256;++u) {
        uint8_t code; bool ext;
        if(((d[280+u/8]|d[312+u/8])&(1u<<(u&7))) &&
           !mh_mb_keyboard_usage(u,&code,&ext)) return false;
    }
    *this={};
    connected=d[1]; input_high=d[2]; assigned=d[3]; notified=d[4];
    command_high_valid=d[5]; command_high=d[6]; address=d[7];
    selection=d[8]; pending_read=d[9]; write_size=d[10];
    keys.leds=d[11]; keys.repeat=d[12]; keys.count=count;
    std::memcpy(keys.queue,d+16,count); std::memcpy(write_data,d+272,8);
    std::memcpy(held,d+280,32); std::memcpy(release,d+312,32);
    return true;
}

bool PicMagicBus::supported(const m68k_machine *m) {
    return m && (m->dev0c.magicbus_empty_input ||
                 (m->envoy && !m->envoy_mc31) || m->hix);
}
extern "C" bool mh_m68k_keyboard_connect(m68k_machine *m,bool connected) {
    if(!PicMagicBus::supported(m)) return false;
    auto &k=m->magicbus;
    if(k.connected==connected) return true;
    bool previous=k.connected?k.input_high:true;
    k={}; k.connected=connected; k.input_high=previous;
    k.line(m,!connected);
    return true;
}
extern "C" bool mh_m68k_keyboard_key(m68k_machine *m,unsigned usage,bool down,bool repeat) {
    return m && m->magicbus.host_key(m,usage,down,repeat);
}
extern "C" void mh_m68k_keyboard_release(m68k_machine *m) {
    if(m && m->magicbus.connected) m->magicbus.release_keys(m);
}
uint32_t pic2000_dev21_read(void *ctx,uint32_t off,unsigned size) {
    auto *m=static_cast<m68k_machine *>(ctx);
    auto &r=m->dev21;
    if(!m->envoy && !m->hix) {
        /* PIC-2000 ROM 0E08672E tests EE bits 6/7 for slots 1/2;
         * 0E0867BC tests bits 8/9 for their ready lines. Supply slot-2
         * presence and ready only while the experimental card is attached;
         * keep the register latch and snapshot layout unchanged. */
        if(m->net_probe_enabled && off<=0xef && off+size>0xee) {
            uint16_t latch=r.reg[0xee/2];
            r.reg[0xee/2]=uint16_t(latch|0x0280u);
            uint32_t v=pic2000_probe_read(&r,off,size);
            r.reg[0xee/2]=latch;
            return v;
        }
        return pic2000_probe_read(&r,off,size);
    }
    if(!m->magicbus.connected && !m->hix) return pic2000_probe_read(&r,off,size);
    // Envoy's input is E7 bit 2, independent of EconoRAM presence bit 3.
    // HIX 0E0579A8 samples EE bit 6; D3 bit 2 is not this bus input.
    unsigned at=m->hix?0xee/2:0xe6/2, mask=m->hix?0x4000:4;
    bool high=!m->magicbus.connected || m->magicbus.input_high;
    uint16_t latch=r.reg[at]; bool empty=r.magicbus_empty_input;
    r.magicbus_empty_input=false;
    r.reg[at]=uint16_t((latch&~mask)|(high?mask:0));
    uint32_t v=pic2000_probe_read(&r,off,size);
    r.reg[at]=latch; r.magicbus_empty_input=empty;
    return v;
}
void pic2000_dev21_write(void *ctx,uint32_t off,unsigned size,uint32_t value) {
    auto *m=static_cast<m68k_machine *>(ctx);
    pic2000_probe_write(&m->dev21,off,size,value);
    if (off < 0xBC && off + size > 0xB8) {
        const uint16_t pending = m->dev21.reg[0xBA / 2];
        if (!(pending & 0x1000u)) m->card_event[0] = false;
        if (!(pending & 0x2000u)) m->card_event[1] = false;
    }
    /* Detect, ready, battery and lock-switch pins are physical inputs, not
     * guest-writable latch bits. Reassert the inputs after a register write. */
    if (off <= 0xEE && off + size > 0xEE) {
        uint16_t physical = 0;
        if (m->card[0].kind && m->card_present[0]) physical |= 0x3140u;
        if (m->card[1].kind && m->card_present[1]) physical |= 0xC280u;
        m->dev21.reg[0xEE / 2] |= physical;
    }
    if(m->magicbus.connected && off<0xa2 && off+size>0x90) {
        // Control responds to either byte. The low byte commits FIFO writes.
        for(unsigned at=off&~1u;at<off+size;at+=2)
            if(at>=0x90 && (at==0x90 || off+size>at+1))
                m->magicbus.write(m,at,2,m->dev21.reg[at/2]);
    }
}
uint32_t pic2000_dev0c_read(void *ctx,uint32_t off,unsigned size) {
    auto *m=static_cast<m68k_machine *>(ctx);
    if(!m->magicbus.connected || !m->dev0c.magicbus_empty_input)
        return pic2000_probe_read(&m->dev0c,off,size);
    // Supply the physical pin to the existing read/logging path, preserving
    // the output latch and snapshot layout of the unidentified register bank.
    auto &r=m->dev0c;
    uint16_t latch=r.reg[1]; bool empty=r.magicbus_empty_input;
    r.magicbus_empty_input=false;
    r.reg[1]=uint16_t((latch&~0x0400u)|(m->magicbus.input_high?0x0400:0));
    uint32_t v=pic2000_probe_read(&r,off,size);
    r.reg[1]=latch; r.magicbus_empty_input=empty;
    return v;
}
void pic2000_dev0c_write(void *ctx,uint32_t off,unsigned size,uint32_t value) {
    pic2000_probe_write(&static_cast<m68k_machine *>(ctx)->dev0c,off,size,value);
}
