#include "machines/pic2000/audio.h"
#include <vector>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"line %d: %s\n",__LINE__,#x); exit(1); } } while(0)
static void sample(void *p, int16_t v) { ((std::vector<int16_t>*)p)->push_back(v); }
int main() {
    uint8_t ram[4096] = {};
    ram[0]=0x80; ram[2]=0x7f; ram[3]=0xff;
    mh_bus bus; mh_bus_init(&bus);
    mh_bus_add_ram(&bus,"audio",0,ram,sizeof(ram),sizeof(ram));
    Pic2000Registers r; r.w1c_lo[0]=0xb0; r.w1c_hi[0]=0xb4; Pic2000Audio a; std::vector<int16_t> out;
    a.approximate_output=false; // raw DMA/PCM checks below
    a.sink=sample; a.ctx=&out; r.reg[0x52/2]=0xb40; r.reg[0x4e/2]=0x1007;
    /* A 44.1-kHz test clock makes each cycle exactly one host sample. */
    a.tick(8,44100,r,bus);
    CHECK(out.size()==8 && out[0]==-32768 && out[3]==-32768);
    CHECK(out[4]==32767 && out[7]==32767);
    a.tick(4096,44100,r,bus);
    CHECK(a.samples==1024 && a.offset==2048);
    CHECK(r.reg32(0xb0)==(1u<<18));
    pic2000_probe_write(&r,0xb0,4,1u<<18);
    CHECK(r.reg32(0xb0)==0);
    a.tick(8192,44100,r,bus);
    CHECK(a.samples==2048 && a.offset==0 && r.reg32(0xb0)==(1u<<19));
    r.reg[0x54/2]=0x400; a.tick(8200,44100,r,bus);
    CHECK(out.back()==0 && a.samples==2050); /* mute doesn't stop DMA */
    uint64_t reads=bus.reads; r.power_off=true; a.tick(8300,44100,r,bus);
    CHECK(bus.reads==reads && out.back()==0 && !a.running);
    r.power_off=false; r.reg[0x4e/2]=0x1017;
    a.tick(8400,44100,r,bus);
    CHECK(bus.reads==reads && !a.running); /* unsupported recording isn't playback */
    CHECK(Pic2000Audio::rate(0xd20)==7350 && Pic2000Audio::rate(0xc30)==8820);
    CHECK(Pic2000Audio::rate(0xa50)==14700 && Pic2000Audio::rate(0x950)==17640);
    CHECK(Pic2000Audio::rate(0x960)==22050 && !Pic2000Audio::rate(0xffff));
    // Measure steady-state gain across every volume code, including endpoints.
    for (unsigned code=0; code<16; ++code) {
        for (unsigned i=0;i<4096;i+=2) { ram[i]=0x75; ram[i+1]=0x30; } // 30000
        Pic2000Audio v; CHECK(v.approximate_output); out.clear(); v.sink=sample; v.ctx=&out;
        r.reg[0x4e/2]=0x1007; r.reg[0x54/2]=0; r.reg[0x56/2]=code*0x110;
        v.tick(1000,44100,r,bus);
        CHECK(std::abs(out.back() - int(30000*(15-code)/15))<=1);
        CHECK(v.samples==250); // volume has no effect on the guest clock
    }
    // Response measurement: a high tone is reduced more than a low tone.
    double rms[2];
    for (unsigned tone=0;tone<2;++tone) {
        double freq=tone ? 4500 : 200;
        for (unsigned i=0;i<2048;++i) {
            int16_t v=std::lround(20000*std::sin(2*3.141592653589793*freq*i/11025));
            ram[2*i]=uint16_t(v)>>8; ram[2*i+1]=uint16_t(v)&255;
        }
        Pic2000Audio v; CHECK(v.approximate_output); out.clear(); v.sink=sample; v.ctx=&out;
        r.reg[0x56/2]=0; v.tick(8000,44100,r,bus);
        double sum=0; for (unsigned i=100;i<out.size();++i) sum+=double(out[i])*out[i];
        rms[tone]=std::sqrt(sum/(out.size()-100));
        CHECK(v.samples==2000 && v.half_irqs==1);
        r.reg[0x54/2]=0x400; v.tick(8004,44100,r,bus);
        CHECK(out.back()==0 && v.samples==2001);
        r.reg[0x54/2]=0;
        r.power_off=true; v.tick(8010,44100,r,bus);
        CHECK(out.back()==0 && v.filter1==0 && v.filter2==0);
        r.power_off=false;
    }
    CHECK(rms[0]>13500 && rms[0]<14500);
    CHECK(rms[1]<rms[0]*0.55);
    // Envoy direct mode clocks samples without fetching a DMA ring. Its
    // serial bit driver acknowledges each sample-ready event independently.
    Pic2000Registers direct; Pic2000Audio clocked;
    direct.w1c_lo[0]=0xb0; direct.w1c_hi[0]=0xb4;
    direct.reg[0x4e/2]=0x1001; direct.reg[0x52/2]=0x960;
    direct.reg[0x54/2]=0x400; // muted codec still clocks the serial interface
    clocked.sample_ready_irq=true;
    reads=bus.reads;
    clocked.tick(1,44100,direct,bus);
    CHECK(direct.reg32(0xb0)==(1u<<20));
    pic2000_probe_write(&direct,0xb0,2,0x10);
    clocked.tick(2,44100,direct,bus);
    CHECK(direct.reg32(0xb0)==0);
    clocked.tick(3,44100,direct,bus);
    CHECK(direct.reg32(0xb0)==(1u<<20));
    CHECK(bus.reads==reads && clocked.half_irqs==0 && clocked.samples==0);
    pic2000_probe_write(&direct,0xb0,2,0x10);
    direct.reg[0x4e/2]=8;
    clocked.tick(100,44100,direct,bus);
    CHECK(direct.reg32(0xb0)==0);
    direct.reg[0x4e/2]=0x1001; direct.reg[0x52/2]=0xffff;
    clocked.tick(200,44100,direct,bus);
    CHECK(direct.reg32(0xb0)==0);
    direct.reg[0x52/2]=0x960; direct.power_off=true;
    clocked.tick(300,44100,direct,bus);
    CHECK(direct.reg32(0xb0)==0 && bus.reads==reads);
    // Divider hypothesis must agree with every established ROM selection.
    for (uint16_t code : {0xd20,0xc30,0xb40,0xa50,0x950,0x960})
        CHECK(Pic2000Audio::divider_rate(code)==Pic2000Audio::rate(code));
    CHECK(Pic2000Audio::divider_rate(0x5a0)==7350);
    CHECK(!Pic2000Audio::divider_rate(0xffff));
    Pic2000Audio trial; Pic2000Registers hix;
    hix.reg[0x4e/2]=0x1007; hix.reg[0x52/2]=0x5a0;
    trial.tick(6144,44100,hix,bus);
    CHECK(trial.samples==0 && hix.reg32(0xb0)==0); // opt-in only
    trial.experimental_divider=true;
    trial.tick(12282,44100,hix,bus);
    CHECK(trial.samples==1023 && hix.reg32(0xb0)==0);
    trial.tick(12288,44100,hix,bus);
    CHECK(trial.samples==1024 && hix.reg32(0xb0)==(1u<<18));
    puts("PIC audio, Envoy sample clock and experimental HIX divider passed");
}
