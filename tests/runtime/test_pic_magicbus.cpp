#include "machines/pic2000/board.h"
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <vector>

static void check(bool ok,const char *why) {
    if(!ok) { std::fprintf(stderr,"FAIL: %s\n",why); std::exit(1); }
}
static void exercise(unsigned board) {
    const auto dir=std::filesystem::temp_directory_path()/
        ("pic-mbus-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directory(dir);
    std::vector<uint8_t> rom(PIC2000_ROM_SIZE);
    const uint8_t vectors[]={0,0x10,0,0,0x0e,0,2,0};
    const uint8_t reset[]={0x2e,0x7c,0,0x10,0,0,0x91,0xc8,0x4e,0x60,0x60,0xfe};
    std::memcpy(rom.data(),vectors,sizeof(vectors));
    std::memcpy(rom.data()+0x200,reset,sizeof(reset));
    const char *identity=board==0?",SONY,2,PIC-2000":
        board==1?",MOTO,1,Motorola Envoy":
        board==2?",SONY,1,HIX-300":board==3?"1,0.31,MOTO,1,Envoy":",SONY,1,PIC-1000";
    std::memcpy(rom.data()+0x300,identity,std::strlen(identity)+1);
    if(board==1 || board==3) { rom[4]=2; rom[5]=0x40; }
    if(board==3) {
        const uint8_t signature[]={0x4e,0xba,0xf8,0x86,0x50,0x4f,0xb0,0xab,0,0x40,
            0x67,0x0a,0x48,0x7a,0x02,0x8e,0x21,0xdf,0,0x10,0x4a,0xfa};
        std::memcpy(rom.data()+0xe82,signature,sizeof(signature));
        rom[0x36]=0x11; rom[0x37]=0x30;
        rom[0x1125]=0x2d; rom[0x1126]=0xfe; rom[0x1127]=4;
        rom[0x4f]=1; // Independently calculated checksum is zero.
    }
    auto path=dir/"fixture.rom";
    { std::ofstream f(path,std::ios::binary); f.write((char*)rom.data(),rom.size()); }
    FILE *log=std::tmpfile(); check(log!=nullptr,"log");
    std::unique_ptr<m68k_machine,decltype(&mrc_m68k_free)> owner(
        mrc_m68k_new(path.string().c_str(),4,log),mrc_m68k_free);
    auto *m=owner.get(); check(m!=nullptr,"construct fixture");
    auto &k=m->magicbus;
    check(m->envoy==(board==1 || board==3) && m->hix==(board==2),"board identification");
    if(board>=3) {
        check(m->envoy_mc31==(board==3) && !mrc_m68k_keyboard_connect(m,true),"unverified ROM stays gated");
        if(board==3) {
            check(m->hix_checksum_pending && m->stop_at==0x400e88,"mc31 guarded intercept armed");
            auto state=dir/"mc31.state";
            m->core.pc=0x400e88; m->core.a[3]=0x40000c; m->core.d[0]=0;
            check(mrc_m68k_save_state(m,state.string().c_str()),"save pending mc31 checksum");
            m->hix_checksum_pending=false; mrc_m68k_set_stop_at(m,0);
            check(mrc_m68k_load_state(m,state.string().c_str()) && m->hix_checksum_pending &&
                  m->stop_at==0x400e88,"restore mc31-specific stop address");
            mrc_m68k_run(m,1);
            check(!m->hix_checksum_pending && !m->stop_at && m->core.d[0]==1 &&
                  m->core.z,"mc31 executes normal successful comparison");
            check(mrc_m68k_save_state(m,state.string().c_str()),"save completed mc31 checksum");
            check(mrc_m68k_load_state(m,state.string().c_str()) && !m->hix_checksum_pending &&
                  !m->stop_at,"completed checksum does not rearm on state load");
            m->dev21.power_off=true; mrc_m68k_power_button(m,true);
            check(m->hix_checksum_pending && m->stop_at==0x400e88,"power reset rearms mc31 checksum");
        }
        owner.reset(); std::fclose(log); std::filesystem::remove_all(dir);
        return;
    }
    const char *id=m->hix?"ATKB":"MBKB";
    const uint16_t rx=m->hix?0x36a1:0xb6a5, tx=m->hix?0x7aa1:0xfaa5;
    auto pin=[&]() {
        return m->envoy?(pic2000_dev21_read(m,0xe7,1)&4):
            m->hix?(pic2000_dev21_read(m,0xee,1)&0x40):
            (pic2000_dev0c_read(m,2,1)&4);
    };
    auto w=[&](unsigned off,uint16_t value) { pic2000_dev21_write(m,off,2,value); };
    auto send=[&](uint16_t command) { w(0x96,command>>8); w(0x94,command&255); };
    auto dma=[&](uint32_t src,uint32_t dst,uint32_t count,uint16_t ctl) {
        mc68349_sim_write(&m->sim,0x7aa,1,0x7c);
        mc68349_sim_write(&m->sim,0x7ac,4,src);
        mc68349_sim_write(&m->sim,0x7b0,4,dst);
        mc68349_sim_write(&m->sim,0x7b4,4,count);
        mc68349_sim_write(&m->sim,0x7a8,2,ctl);
        w(0x90,0x6f); k.service(m);
    };
    // Use external RAM so the reset boot overlay cannot intercept DMA.
    constexpr uint32_t memory=0x04001000;
    uint8_t *data=m->xram+0x1000;
    check(pin()!=0,"disconnected input");
    check(mrc_m68k_keyboard_connect(m,true),"identified board supports keyboard");
    check(pin()==0,"connected input");
    if(m->envoy || m->hix) {
        unsigned at=m->envoy?0xe6:0xee, mask=m->envoy?4:0x4000;
        w(at,0xa5a5);
        unsigned expected_pin=(0xa5a5&~mask);
        unsigned other=m->envoy?0xfff7:0xffff; // EconoRAM drives its own input.
        check((pic2000_dev21_read(m,at,2)&other)==(expected_pin&other),"pin preserves other inputs");
        check((pic2000_dev21_read(m,at,4)>>16 & other)==(expected_pin&other),"longword pin position");
        check(m->dev21.reg[at/2]==0xa5a5,"pin reads preserve output latch");
        check(pic2000_dev0c_read(m,2,2)==0,"other boards do not override PIC input");
        w(at,0);
    } else check(pic2000_dev0c_read(m,0,4)==0,"PIC longword pin position");
    w(0xb2,0xffff); w(0xbc,0xffff);
    k.line(m,true);
    check(m->envoy?(m->dev21.reg[0xbc/2]==0x20 && !m->dev21.reg[0xb2/2]):
        (m->dev21.reg[0xb2/2]==0x200 && !m->dev21.reg[0xbc/2]),"board-specific rising edge bank");
    k.line(m,false);
    check(m->envoy?m->dev21.reg[0xbc/2]==0x30:m->dev21.reg[0xb2/2]==0x300,"falling edge");
    w(0xb2,0xffff); w(0xbc,0xffff);
    w(0xb4,0); w(0xb6,m->envoy?0:0x200);
    w(0xc0,0); w(0xc2,0); w(0xc4,m->envoy?0x20:0);
    k.line(m,true); m68k_set_sr(&m->core,0x2700); mrc_m68k_run(m,1);
    check(m->irq_now==5,"accessory edge uses IPL5, including Envoy's second bank");
    w(m->envoy?0xbc:0xb2,m->envoy?0x20:0x200); mrc_m68k_run(m,1);
    check(m->irq_now==0,"W1C acknowledgement releases accessory IRQ");
    k.line(m,false);
    send(0xdef0); check(k.input_high && !k.assigned,"broadcast reset response");
    send(0xdca8); send(0xdce0);
    check(k.assigned && k.address==6 && !k.input_high,"address six and end of chain");
    send(0xcc5c); send(0xcc24); dma(0x21000092,memory,4,rx);
    check(std::memcmp(data,id,4)==0,"ID arrives through receive DMA");
    check(mc68349_sim_read(&m->sim,0x7b4,4)==0,"receive DMA count");
    check(mc68349_sim_read(&m->sim,0x7aa,1)==0xc0,"DMA completion status");
    mc68349_sim_write(&m->sim,0x7aa,1,0);
    check(mc68349_sim_read(&m->sim,0x7aa,1)==0xc0,"zero cannot acknowledge DMA");
    mc68349_sim_write(&m->sim,0x7aa,1,0x40);
    check(mc68349_sim_read(&m->sim,0x7aa,1)==0,"DMA W1C and summary IRQ");
    send(0xcc60); send(0xcc24); dma(0x21000092,memory,2096,rx);
    unsigned n=unsigned(data[0])<<8|data[1], sum=1;
    for(unsigned i=0;i<n;++i) sum+=data[i];
    check(n<254 && uint16_t(sum)==uint16_t(unsigned(data[n])<<8|data[n+1]),"PIC descriptor checksum");
    check(std::memcmp(data+2,id,4)==0 && data[0x4e]!=0,"descriptor field positions");

    mrc_runtime runtime=mrc_pic2000_runtime(m);
    check(runtime.ops->keyboard_key(m,4,true,false),"host a make");
    runtime.ops->keyboard_key(m,4,true,false); // duplicate make
    runtime.ops->keyboard_key(m,4,false,false);
    check(k.keys.count==3 && k.input_high,"duplicate suppressed; make and break queued");
    send(0xcc18); dma(0x21000092,memory,2,0xb6a5);
    check(data[1]==0xe && k.notified && !k.input_high,"request header and line acknowledge");
    send(0xcc24); dma(0x21000092,memory,2,0xb6a5);
    check(data[0]==1 && data[1]==0x1c && k.keys.count==2 && k.input_high,"small read retains remaining scans");
    send(0xcc24); dma(0x21000092,memory,16,0xb6a5);
    check(data[0]==2 && data[1]==0xf0 && data[2]==0x1c,"break prefix survives split reads");

    const uint8_t leds[]={0x4b,2,0xed,4,0,0,0,0};
    std::memcpy(data,leds,8); send(0xcc3c); w(0x98,m->hix?0x004b:0x4b02);
    dma(memory+2,0x21000098,6,tx);
    check(k.keys.leds==4 && k.writes==1,"primed word plus DMA LED write");
    check(std::memcmp(data,leds,8)==0,"legacy adaptation never changes guest source");
    check(mc68349_sim_read(&m->sim,0x7ac,4)==memory+8,"transmit DMA source advances");
    if(m->hix) {
        const uint8_t repeat[]={0x4b,3,0xf3,0x25,0xf4,0,0,0};
        std::memcpy(data,repeat,8); send(0xcc3c); w(0x98,0x004b);
        dma(memory+2,0x21000098,6,tx);
        check(k.keys.repeat==0x25,"HIX legacy repeat envelope");
        data[2]=0xaa; send(0xcc3c); w(0x98,0x004b);
        dma(memory+2,0x21000098,6,tx);
        check(k.errors==1 && k.keys.repeat==0x25 && k.keys.leds==4,"unknown legacy command rejected");
    } else {
        send(0xcc3c); w(0x98,0x004b); dma(memory+2,0x21000098,6,tx);
        check(k.errors==1 && k.keys.leds==4,"legacy header is HIX-only");
    }

    runtime.ops->keyboard_key(m,0xe1,true,false);
    while(k.keys.count<MRC_MBKEY_CAPACITY) mrc_mb_keyboard_key(&k.keys,0x1c,false,true);
    runtime.ops->keyboard_release(m);
    check(k.release[0xe1/8]!=0,"full queue retains focus-loss break");
    send(0xcc24); dma(0x21000092,memory,16,0xb6a5);
    check(k.release[0xe1/8]==0 && !k.held[0xe1/8],"deferred break retried after read");

    const auto saved=dir/"keyboard.state";
    w(0x96,0xcc); // persist an in-flight command high byte
    check(mrc_m68k_save_state(m,saved.string().c_str()),"save attached keyboard");
    std::array<uint8_t,PicMagicBus::state_size> expected{},actual{};
    k.encode(expected.data());
    k={};
    check(mrc_m68k_load_state(m,saved.string().c_str()),"restore attached keyboard");
    k.encode(actual.data()); check(expected==actual,"snapshot preserves peripheral and host lifecycle");
    w(0x94,0x24); check(k.pending_read==2,"resume command assembled across snapshot");
    unsigned queued=k.keys.count;
    dma(0x21000092,0x30000000,16,0xb6a5);
    check(k.keys.count==queued && mc68349_sim_read(&m->sim,0x7aa,1)==0x90,
          "bad DMA target preserves scans and raises destination error");
    send(0xcc24); dma(0x21000092,memory,3,0xb6a5);
    check(mc68349_sim_read(&m->sim,0x7aa,1)==0x88,"odd DMA count gives configuration error");

    auto invalid=expected; invalid[14]=2;
    k.encode(actual.data());
    check(!k.decode(invalid.data()),"invalid queue count rejected");
    std::array<uint8_t,PicMagicBus::state_size> unchanged{}; k.encode(unchanged.data());
    check(actual==unchanged,"invalid keyboard state leaves live device intact");
    std::filesystem::resize_file(saved,std::filesystem::file_size(saved)-1);
    check(!mrc_m68k_load_state(m,saved.string().c_str()),"truncated keyboard trailer rejected");
    k.encode(unchanged.data()); check(actual==unchanged,"short state load is atomic");
    check(mrc_m68k_keyboard_connect(m,false),"detach keyboard");
    check(pin()!=0,"disconnected input restored");
    check(!runtime.ops->keyboard_key(m,4,true,false),"disconnected host input rejected");
    m->dev0c.magicbus_empty_input=false;
    m->envoy=m->hix=false;
    check(!mrc_m68k_keyboard_connect(m,true),"unverified board rejected");
    owner.reset(); std::fclose(log); std::filesystem::remove_all(dir);
}
int main() {
    for(unsigned board=0;board<5;++board) exercise(board);
}
