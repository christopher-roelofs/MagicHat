#include "machines/pic2000/machine68k.h"
#include "machines/pic2000/board.h"
#include "machines/pic2000/magicbus.h"
#include "machines/pic2000/registers.h"
#include "soc/mc68349/duart.h"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <vector>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>
static void check(bool ok,const char *why) { if(!ok) { std::fprintf(stderr,"FAIL: %s\n",why); std::exit(1); } }
int main() {
    {
        Mc68349Duart d;
        check(d.link.fd == -1 && d.link_a.fd == -1, "serial endpoints start disconnected");
        d.write(0x12, 1, 5);
        check(d.read(0x11, 1) == 0, "channel A remains opt-in");
        check(d.open_a(), "open channel-A PTY");
        const int peer = open(d.link_a.path, O_RDWR | O_NOCTTY | O_NONBLOCK);
        check(peer >= 0, "open channel-A peer");
        uint64_t now = 0;
        d.insns = &now; d.clock_hz = 9600;
        d.write(0x11, 1, 0xbb); // 9600 baud, 10 slots per character
        d.write(4, 1, 5);
        d.write(0x12, 1, 5);
        d.write(0x15, 1, 1);
        check(d.irq() && d.active(), "A transmitter readiness drives interrupt and scheduler");
        check(d.read(0x14, 1) == 2 && d.read(0x1d, 1) == 2,
              "A peer asserts CTS without asserting channel B CTS");
        d.write(0x13, 1, 'A');
        d.write(0x13, 1, 'X'); // Busy transmitter must retain A.
        check(!d.irq(), "A transmit load immediately clears ready interrupt");
        unsigned char byte = 0;
        now = 9; d.tick();
        check(read(peer, &byte, 1) < 0, "A byte waits for baud deadline");
        now = 10; d.tick();
        check(read(peer, &byte, 1) == 1 && byte == 'A', "A output reaches PTY unchanged");
        check(d.irq(), "A transmit completion rearms ready interrupt");
        d.write(0x12, 1, 0x30);
        check(!d.a.tx_enabled && !d.irq(), "reset A transmitter disables it");
        d.write(0x15, 1, 0x22); // Both receivers, independent ISR lanes.
        check(write(peer, "1234", 4) == 4, "queue A receive bytes");
        // PTY delivery can be scheduled asynchronously by the kernel.
        for (unsigned i = 0; i < 1000 && d.a.rx_len < 3; ++i) {
            now += 10; d.tick();
            usleep(1000);
        }
        check(d.a.rx_len == 3 && d.read(0x11, 1) == 3,
              "A receiver has the manual's three-entry FIFO");
        d.rx[0] = 'B'; d.rx_len = 1; d.tick();
        check((d.read(0x15, 1) & 0x22) == 0x22, "both receive interrupts coexist");
        d.write(0x10, 1, 0x40); // FIFO-full interrupt mode
        check(d.read(0x13, 1) == '1', "A receive FIFO is ordered");
        check((d.read(0x15, 1) & 0x22) == 0x20,
              "A FIFO threshold leaves B receive interrupt untouched");
        check(d.read(0x1b, 1) == 'B', "A activity does not alter B bytes");
        d.write(0x12, 1, 0x20);
        check(!d.a.rx_enabled && !d.a.rx_len, "reset A receiver clears and disables FIFO");
        d.write(0x12, 1, 5);
        d.write(0x13, 1, 'S');
        d.write(0, 2, 0x8000);
        now += 100; d.tick();
        check(d.a.tx_busy && !d.irq(), "module stop suspends A transmission and IRQ");
        d.write(0, 2, 0); d.tick();
        check(read(peer, &byte, 1) == 1 && byte == 'S', "A resumes after module stop");
        close(peer);
        mrc_serial_close(&d.link_a);
    }
    Mc68349Duart uart;
    uart.write(4, 1, 5);
    uart.write(0x15, 1, 0x20);
    uart.rx[0] = 0x7e;
    uart.rx_len = 1;
    uart.tick();
    check(uart.irq(), "channel B receive interrupts without transmit activity");
    uart.write(0x15, 1, 0);
    check(!uart.irq(), "receive interrupt obeys guest mask");
    uart.write(0x15, 1, 0x20);
    check(uart.read(0x1b, 1) == 0x7e, "receive FIFO supplies the byte");
    check(!uart.irq(), "draining receive FIFO clears interrupt");
    auto dir=std::filesystem::temp_directory_path()/
        ("mrc-runtime-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    check(std::filesystem::create_directory(dir),"create test directory");
    auto path=dir/"idle.rom";
    std::vector<unsigned char> rom(PIC2000_ROM_SIZE);
    auto word=[&](unsigned a,unsigned v) {rom[a]=v>>8;rom[a+1]=v;};
    word(0,0x0010); word(2,0); word(4,0x0e00); word(6,0x0200);
    word(0x200,0xf800); word(0x202,0x01c0); word(0x204,0x2000); // LPSTOP
    { std::ofstream f(path,std::ios::binary); f.write((char*)rom.data(),rom.size()); }
    FILE *log=std::tmpfile(); check(log!=nullptr,"open log");
    Pic2000Registers changes; changes.log=log;
    changes.audio_dirty=changes.irq_dirty=false;
    pic2000_probe_write(&changes,0x4E,2,0x1007);
    check(changes.audio_dirty && !changes.irq_dirty,"audio controls notify scheduler");
    changes.audio_dirty=false;
    pic2000_probe_write(&changes,0xC0,2,8);
    check(changes.irq_dirty && !changes.audio_dirty,"interrupt enables notify arbitration");
    changes.irq_dirty=false;
    changes.set_bit32(0xB8,19);
    check(changes.irq_dirty,"device interrupt edges notify arbitration");
    Pic2000Registers pen;
    pen.log=log; pen.adc_off=0xE4; pen.pen_second_690=2000;
    pen.adc_pairs=1; pen.adc_pair[0]={0x69,0,1000};
    pic2000_probe_write(&pen,0xE2,2,0x69);
    pic2000_probe_write(&pen,0xE4,2,0);
    check(pen.adc_latched==1000,"acquisition first conversion samples X");
    pen.adc_pair[0].value=1100; pen.pen_second_690=2100;
    check(pen.adc_latched==1000,"motion preserves conversion already sampled");
    pic2000_probe_write(&pen,0xE4,2,0);
    check(pen.adc_latched==2100,"motion does not restart acquisition before Y");
    pic2000_probe_write(&pen,0xE2,2,0x35);
    pic2000_probe_write(&pen,0xE4,2,0x1000);
    pic2000_probe_write(&pen,0xE2,2,0x69);
    pic2000_probe_write(&pen,0xE4,2,0);
    check(pen.adc_latched==1100,"guest selectors restart next acquisition X");
    pic2000_probe_write(&pen,0xE4,2,0);
    check(pen.adc_latched==2100,"repeated acquisition still supplies Y");
    Pic2000Registers accessory;
    accessory.log=log;
    check(pic2000_probe_read(&accessory,2,1)==0,"other boards retain undecoded input");
    accessory.magicbus_empty_input=true;
    check(pic2000_probe_read(&accessory,2,1)==4,"PIC empty MagicBus input is high");
    check(pic2000_probe_read(&accessory,2,2)==0x0400,"input has correct big-endian word position");
    check(pic2000_probe_read(&accessory,0,4)==0x0400,"input has correct longword position");
    pic2000_probe_write(&accessory,2,2,0);
    check(pic2000_probe_read(&accessory,2,1)==4,"controller write cannot clear physical input");
    accessory.reg[1]=0x1234;
    check(pic2000_probe_read(&accessory,2,2)==0x1634,"empty input preserves unrelated register bits");
    accessory.magicbus_empty_offset=0xE7;
    pic2000_probe_write(&accessory,0xE6,2,0);
    check(pic2000_probe_read(&accessory,0xE7,1)==4,"Envoy empty input uses E7");
    check(pic2000_probe_read(&accessory,0xE6,2)==4,"Envoy input is in low word byte");
    check(pic2000_probe_read(&accessory,0xE4,4)==4,"Envoy input has correct longword position");
    check(pic2000_probe_read(&accessory,2,2)==0x1234,"Envoy input does not assert PIC pin");
    Pic2000Registers serial;
    serial.log=log; serial.serial_pullup=true;
    check(pic2000_probe_read(&serial,0xE7,1)==8,"unpopulated serial line releases high");
    pic2000_probe_write(&serial,0xE6,2,0x0800);
    check(pic2000_probe_read(&serial,0xE7,1)==0,"host serial output drives input low");
    pic2000_probe_write(&serial,0xE6,2,0x0200);
    check(pic2000_probe_read(&serial,0xE6,2)==0x0208,"release preserves other control bits");
    check(pic2000_probe_read(&serial,0xE4,4)==0x0208,"serial input has correct longword position");
    serial.adapter_input=true; serial.adapter_attached=true;
    pic2000_probe_write(&serial,0xD1,1,0);
    check(pic2000_probe_read(&serial,0xD0,2)==0x40,"byte writes cannot clear attached adapter input");
    serial.adapter_attached=false;
    pic2000_probe_write(&serial,0xD1,1,0x44);
    check(pic2000_probe_read(&serial,0xD0,2)==4,"adapter absence preserves unrelated physical bits");
    m68k_machine *board=mrc_m68k_new(path.string().c_str(),4,log);
    check(board!=nullptr,"construct PIC-2000");
    auto card_path = dir / "pic-card.img";
    check(mrc_m68k_insert_sram(board, 0, card_path.string().c_str(), 65536),
          "attach 68k SRAM card");
    check(pic2000_dev21_read(board,0xEE,2)==0x3140,
          "slot-1 SRAM asserts presence, ready, and healthy battery");
    pic2000_dev21_write(board,0xEE,2,0);
    check(pic2000_dev21_read(board,0xEE,2)==0x3140,
          "physical SRAM status inputs survive guest writes");
    check(mrc_pccard_read(&board->card_port[0][MRC_PCCARD_WINDOW_A], 0, 1) == 1,
          "68k SRAM attribute window exposes CIS");
    mrc_pccard_write(&board->card_port[0][MRC_PCCARD_WINDOW_B], 0x1234, 2,
                     0xBEEF);
    check(mrc_pccard_read(&board->card_port[0][MRC_PCCARD_WINDOW_B], 0x1234, 2)
              == 0xBEEF,
          "68k SRAM common window is writable");
    check(mrc_m68k_eject_card(board, 0), "eject 68k SRAM card");
    check(pic2000_dev21_read(board,0xEE,2)==0,
          "ejected 68k SRAM releases detect and ready inputs");
    check(board->card_event[0] && (board->dev21.reg[0xBA / 2] & 0x1000),
          "card removal raises the slot-1 status event latch");
    pic2000_dev21_write(board,0xC2,2,0x3000);
    mrc_m68k_run(board,1);
    check(board->dev21_irq_now==6,
          "enabled card removal status reaches the IPL6 line");
    pic2000_dev21_write(board,0xBA,2,0x1000);
    check(!board->card_event[0], "guest acknowledges the card status event");
    mrc_m68k_free(board);
    std::ifstream card_file(card_path, std::ios::binary);
    card_file.seekg(0x1234);
    unsigned char card_bytes[2] = {};
    card_file.read((char *)card_bytes, 2);
    check(card_bytes[0] == 0xBE && card_bytes[1] == 0xEF,
          "68k SRAM writes persist to the image");
    board=mrc_m68k_new(path.string().c_str(),4,log);
    check(board!=nullptr,"reconstruct PIC-2000 after card test");
    check(pic2000_dev21_read(board,0xEE,2)==0,"empty PIC card slots stay absent");
    board->net_probe_enabled=true;
    check(pic2000_dev21_read(board,0xEF,1)==0x80,
          "opt-in NE2000 attachment asserts only slot-2 presence");
    check(pic2000_dev21_read(board,0xEE,2)==0x0280,
          "slot-2 presence and ready have the ROM's word bit positions");
    check(pic2000_dev21_read(board,0xEE,1)==0x02,
          "slot-2 ready is visible on the high input byte");
    pic2000_dev21_write(board,0xEE,2,0);
    check(pic2000_dev21_read(board,0xEE,2)==0x0280,
          "guest register writes cannot clear the attached card inputs");
    board->net_probe_enabled=false;
    check(pic2000_dev21_read(board,0xEE,2)==0,
          "detached card input releases without changing the latch");
    check(mrc_m68k_host_battery(board,50),"PIC maps valid host charge");
    check(!mrc_m68k_host_battery(board,-1) && !mrc_m68k_host_battery(board,101),
          "unknown or invalid host charge does not change sensor");
    mrc_m68k_set_adc_chan(board,2,777);
    check(!mrc_m68k_host_battery(board,100),"explicit ADC override takes precedence");
    auto view=mrc_pic2000_runtime(board);
    check(view.ops->audio_sink && view.ops->audio_rate &&
          view.ops->audio_rate(view.board)==44100, "PIC speaker output available");
    check(view.ops->save_state!=nullptr, "state save reaches the board");
    check(view.ops->run_slots(view.board,1000)==1000,"idle consumes execution budget");
    check(!view.ops->stopped(view.board),"LPSTOP is not terminal halt");
    check(view.ops->slots(view.board)==1000,"legacy slot accounting preserved");
    check(view.ops->elapsed_ns(view.board)==mrc_time_ns(1000,PIC2000_CPU_HZ),
          "elapsed time advances while idle");
    mrc_m68k_set_cpi(board,2);
    // CPI is a whole-run diagnostic setting, not an emulated clock change.
    check(view.ops->elapsed_ns(view.board)==mrc_time_ns(2000,PIC2000_CPU_HZ),
          "elapsed time respects configured CPI");
    check(!view.ops->pen(view.board,true,480,320),"board rejects out-of-panel input");
    std::vector<uint8_t> frame(480*320); unsigned w=0,h=0;
    check(view.ops->frame(view.board,frame.data(),&w,&h) && w==480 && h==320,
          "display available through runtime");
    mrc_m68k_free(board);

    // Tiny firmware checks the physical power edge through the CPU's IRQ6
    // vector, including acknowledgement and repeated host key-down events.
    auto save_rom=[&]() { std::ofstream f(path,std::ios::binary); f.write((char*)rom.data(),rom.size()); };
    auto code=[&](unsigned at,std::initializer_list<unsigned> words) {
        for (unsigned v:words) { word(at,v); at+=2; }
    };
    // Reset selects ROM over low memory. Program CS0, then enter the fixture
    // at its normal ROM address and use a ROM-based vector table.
    word(6,0x0300);
    code(0x300,{0x23fc,0x0e00,0x00f9,0x3c00,0x0044,
                0x203c,0x0e00,0,0x4e7b,0x0801, // MOVEC D0,VBR
                0x4ef9,0x0e00,0x0200});
    code(0x78,{0x0e00,0x0240});
    code(0x200,{0x33fc,8,0x2100,0x00c0, // enable power press
                0xf800,0x01c0,0x2000,0x60f8}); // LPSTOP; loop
    code(0x240,{0x31f9,0x2100,0x00d0,0x1008, // sample input
                0x33fc,8,0x2100,0x00b8, // acknowledge
                0x52b8,0x1000,0x4e73}); // addq.l #1,$1000; RTE
    save_rom();
    board=mrc_m68k_new(path.string().c_str(),4,log);
    check(board!=nullptr,"construct interrupt fixture");
    view=mrc_pic2000_runtime(board);
    mrc_m68k_region regions[2]; mrc_m68k_retained_regions(board,regions);
    auto ram=(uint8_t*)regions[0].data;
    view.ops->run_slots(board,100);
    check(ram[0x1003]==0,"no unsolicited power interrupt");
    check(view.ops->power_button!=nullptr,"runtime exposes power button");
    view.ops->power_button(board,true);
    view.ops->run_slots(board,100);
    check(ram[0x1003]==1 && ram[0x1009]==4,"press reaches IRQ6 with physical input high");
    view.ops->power_button(board,true);
    view.ops->run_slots(board,100);
    check(ram[0x1003]==1,"acknowledged held button does not interrupt again");
    view.ops->power_button(board,false);
    view.ops->power_button(board,true);
    view.ops->run_slots(board,100);
    check(ram[0x1003]==2,"next press creates a new edge");
    mrc_m68k_free(board);

    // Power-off must stop instruction execution without losing RAM. A new
    // press starts at the reset vector, rather than continuing past OFF.
    code(0x200,{0x52b8,0x1000,0x4279,0x2100,0x00d0,
                0x52b8,0x1004,0x60fe});
    save_rom();
    board=mrc_m68k_new(path.string().c_str(),4,log);
    check(board!=nullptr,"construct power latch fixture");
    mrc_m68k_retained_regions(board,regions); ram=(uint8_t*)regions[0].data;
    mrc_m68k_run(board,100);
    check(mrc_m68k_powered_off(board),"firmware removes CPU power");
    check(ram[0x1003]==1 && ram[0x1007]==0,"CPU cannot execute past power-off");
    mrc_m68k_lcd(board,frame.data());
    check(frame[0]==3,"powered-off display is black");
    mrc_m68k_power_button(board,true);
    mrc_m68k_run(board,100);
    check(ram[0x1003]==2 && ram[0x1007]==0,"wake resets CPU and retains RAM");
    check(mrc_m68k_power_off(board),"closing an off device does not wake it");
    mrc_m68k_free(board);

    // Envoy reset enters at 024xxxxx and jumps into its 004xxxxx alias.
    // Alternate RAM stores and instruction fetches to exercise bus priority.
    word(4,0x0240);
    code(0x300,{0x23fc,0x0040,0x00f9,0x3c00,0x0044,
                0x4ef9,0x0040,0x0200});
    code(0x200,{0x52b8,0x1000,0x52b8,0x1004,0x60fe});
    save_rom();
    board=mrc_m68k_new(path.string().c_str(),4,log);
    check(board!=nullptr,"construct experimental Envoy mapping fixture");
    mrc_m68k_retained_regions(board,regions); ram=(uint8_t*)regions[0].data;
    mrc_m68k_run(board,100);
    check(ram[0x1003]==1 && ram[0x1007]==1,
          "Envoy executes through ROM alias across RAM accesses");
    mrc_m68k_free(board);
    code(0x200,{0x33fc,0x2000,0x2100,0x0048,
                0x33fc,0x0010,0x2100,0x0040,0x60fe});
    save_rom();
    board=mrc_m68k_new(path.string().c_str(),4,log);
    check(board!=nullptr,"construct programmable LCD fixture");
    mrc_m68k_retained_regions(board,regions); ram=(uint8_t*)regions[0].data;
    ram[0x2800]=0xff; ram[0x8000]=0x1b;
    mrc_m68k_lcd(board,frame.data());
    check(frame[0]==3 && frame[1]==3 && frame[2]==3 && frame[3]==3,
          "disabled LCD hides retained framebuffer before initialization");
    mrc_m68k_run(board,100);
    mrc_m68k_lcd(board,frame.data());
    check(frame[0]==0 && frame[1]==1 && frame[2]==2 && frame[3]==3,
          "LCD follows programmed HIX base rather than PIC default");
    mrc_m68k_free(board);

    // Identify an Envoy fixture without depending on a proprietary ROM.
    code(0x300,{0x2e7c,0x0010,0,0x91c8,0x4e60,0x60fe});
    constexpr char envoy_identity[] = ",MOTO,1,Motorola Envoy";
    std::memcpy(rom.data()+0x400,envoy_identity,sizeof(envoy_identity));
    save_rom();
    board=mrc_m68k_new(path.string().c_str(),4,log);
    check(board && mrc_m68k_is_envoy(board),"identify Envoy battery fixture");
    check(!mrc_m68k_host_battery(board,50),"Envoy does not borrow PIC charge thresholds");
    uint8_t *context=nullptr; uint32_t context_size=0;
    check(mrc_m68k_save_battery_ram(board,&context,&context_size) && context_size==36,
          "serialize battery memory separately from CPU context");
    check(!std::memcmp(context,"ECR1",4),"battery context has a format tag");
    for(unsigned i=4;i<36;i++) context[i]=uint8_t(i*37);
    check(mrc_m68k_load_battery_ram(board,context,context_size),"load retained battery bytes");
    mrc_m68k_free(board);
    board=mrc_m68k_new(path.string().c_str(),4,log);
    check(mrc_m68k_load_battery_ram(board,context,context_size),"restore into a new board");
    check(mrc_m68k_save_battery_ram(board,&context,&context_size),"save restored memory");
    for(unsigned i=4;i<36;i++) check(context[i]==uint8_t(i*37),"all battery bytes survive restart");
    context[0]='?';
    check(!mrc_m68k_load_battery_ram(board,context,context_size),"reject unknown battery context");
    check(!mrc_m68k_load_battery_ram(board,context,35),"reject truncated battery context");
    std::free(context); mrc_m68k_free(board);

    // Envoy adapter transitions reach the firmware through IRQ6, with the
    // level sampled independently of the write-one-to-clear edge flags.
    code(0x300,{0x2e7c,0x0010,0,0x91c8,0x4e60,
                0x23fc,0x0040,0x00f9,0x3c00,0x0044,
                0x203c,0x0040,0,0x4e7b,0x0801,
                0x4ef9,0x0040,0x0200});
    code(0x78,{0x0040,0x0240});
    code(0x200,{0x33fc,0x0030,0x2100,0x00c0,
                0xf800,0x01c0,0x2000,0x60f8});
    code(0x240,{0x31f9,0x2100,0x00d0,0x1008,
                0x33fc,0x0030,0x2100,0x00b8,
                0x52b8,0x1000,0x4e73});
    save_rom();
    board=mrc_m68k_new(path.string().c_str(),4,log);
    check(board!=nullptr,"construct Envoy adapter fixture");
    mrc_m68k_retained_regions(board,regions); ram=(uint8_t*)regions[0].data;
    mrc_m68k_run(board,100);
    check(ram[0x1003]==0,"disconnected adapter has no unsolicited event");
    check(mrc_m68k_host_adapter(board,true),"Envoy accepts host adapter");
    mrc_m68k_run(board,100);
    check(ram[0x1003]==1 && (ram[0x1009]&0x40),"attach IRQ samples connected input");
    mrc_m68k_host_adapter(board,true); mrc_m68k_run(board,100);
    check(ram[0x1003]==1,"unchanged adapter does not retrigger");
    mrc_m68k_host_adapter(board,false); mrc_m68k_run(board,100);
    check(ram[0x1003]==2 && !(ram[0x1009]&0x40),"remove IRQ samples disconnected input");
    check(mrc_m68k_set_adapter(board,true),"explicit adapter connection accepted");
    check(!mrc_m68k_host_adapter(board,false),"explicit adapter overrides host state");
    mrc_m68k_free(board);

    // HIX firmware reads main and backup ADCs after programming CS0.
    std::fill(rom.begin(),rom.end(),0);
    word(0,0x0010); word(4,0x0e00); word(6,0x0300);
    constexpr char hix_identity[] = ",SONY,1,HIX-300";
    std::memcpy(rom.data()+0x400,hix_identity,sizeof(hix_identity));
    code(0x300,{0x2e7c,0x0010,0,0x91c8,0x4e60,
                0x23fc,0x0e00,0x00f9,0x3c00,0x0044,
                0x33fc,0x0080,0x2100,0x00e4,
                0x31f9,0x2100,0x00e4,0x1000,
                0x33fc,0x0100,0x2100,0x00e4,
                0x31f9,0x2100,0x00e4,0x1002,0x60fe});
    save_rom();
    for (int percent : {-1,0,50,100}) {
        board=mrc_m68k_new(path.string().c_str(),4,log);
        check(board!=nullptr,"construct HIX battery fixture");
        if(percent>=0) check(mrc_m68k_host_battery(board,percent),"HIX host charge mapping available");
        mrc_m68k_retained_regions(board,regions); ram=(uint8_t*)regions[0].data;
        mrc_m68k_run(board,100);
        unsigned main=(unsigned(ram[0x1000])<<8)|ram[0x1001];
        unsigned backup=(unsigned(ram[0x1002])<<8)|ram[0x1003];
        check(main==unsigned(percent<0?832:745+(63*percent+50)/100),"guest sees HIX main charge endpoints");
        check(backup==640,"HIX backup is healthy independently of host main charge");
        mrc_m68k_set_adc_chan(board,2,700);
        check(!mrc_m68k_host_battery(board,100),"HIX explicit main sensor overrides host charge");
        mrc_m68k_free(board);
    }
    // The block engine has to be the same machine as the reference, not
    // an approximation of it.
    //
    // The fixture is built to reach the places where the two could part.
    // It runs in user mode, so every exception swaps stack pointers, which
    // is where the first version of the handover went wrong. It traps, so
    // the block engine builds a frame the reference has to be able to
    // return through. It returns from exception, which the block engine
    // refuses and hands back. And it samples the device counter, so the
    // record covers what the machine thought the time was and not only
    // what the processor computed.
    std::fill(rom.begin(),rom.end(),0);
    word(0,0x0010); word(4,0x0e00); word(6,0x0300);
    word(0x80,0x0e00); word(0x82,0x0500);              // TRAP #0 vector
    code(0x300,{0x2e7c,0x0010,0,                       // MOVEA.L #$100000,A7
                0x23fc,0x0e00,0x00f9,0x3c00,0x0044,    // program CS0
                0x203c,0x0e00,0x0000,0x4e7b,0x0801,    // MOVEC D0,VBR
                0x227c,0x0009,0x0000,0x4e61,           // user stack
                0x46fc,0x0000,                         // MOVE #0,SR: to user
                0x4ef9,0x0e00,0x0420});                // JMP $0E000420
    code(0x420,{0x41f9,0x0000,0x1000,                  // LEA $1000,A0
                0x7000,0x7400,                         // MOVEQ #0,D0/D2
                0x323c,0x7fff,                         // outer: MOVE.W #$7FFF,D1
                0x5680,                                // inner: ADDQ.L #3,D0
                0x51c9,0xfffc,                         // DBF D1, inner
                0x20c0,                                // MOVE.L D0,(A0)+
                0x2639,0x2100,0x00d4,                  // MOVE.L $210000D4,D3
                0x20c3,                                // MOVE.L D3,(A0)+
                0x4e40,                                // TRAP #0
                0x5282,                                // ADDQ.L #1,D2
                0x0c82,0x0000,0x0008,                  // CMPI.L #8,D2
                0x66e0,                                // BNE outer
                0x60fe});                              // BRA self
    code(0x500,{0x52b9,0x0000,0x1100,0x4e73});         // ADDQ.L #1,$1100; RTE
    save_rom();
    std::vector<uint8_t> image[2];
    for (int engine = 0; engine < 2; engine++) {
        board=mrc_m68k_new(path.string().c_str(),4,log);
        check(board!=nullptr,"construct block engine fixture");
        check(mrc_m68k_set_engine(board,engine?"blocks":"interpreter"),
              "both engines are offered by name");
        check(!mrc_m68k_set_engine(board,"nonsense"),
              "an engine this machine does not have is refused");
        check(mrc_m68k_set_engine(board,engine?"blocks":"interpreter"),
              "and asking again restores the one that was wanted");
        mrc_m68k_retained_regions(board,regions);
        long mark = std::ftell(log);
        mrc_m68k_run(board,2000000);
        image[engine].assign((uint8_t*)regions[0].data,
                             (uint8_t*)regions[0].data + 0x1200);
        mrc_m68k_engine_report(board);
        if (engine) {
            /*
             * A comparison between two runs of the same engine would pass
             * whatever the block engine did, so the run has to be shown to
             * have used it.
             */
            long end = std::ftell(log);
            std::string said(size_t(end - mark), '\0');
            std::fseek(log, mark, SEEK_SET);
            said.resize(std::fread(said.data(), 1, said.size(), log));
            std::fseek(log, 0, SEEK_END);
            check(said.find("block engine ran") != std::string::npos,
                  "the run really went through the block engine");
        }
        mrc_m68k_free(board);
    }
    auto word32=[&](const std::vector<uint8_t> &v,unsigned at) {
        return (uint32_t)v[at]<<24 | (uint32_t)v[at+1]<<16 |
               (uint32_t)v[at+2]<<8 | v[at+3];
    };
    /* The single-step interpreter and block engine must leave identical RAM. */
    const std::vector<uint8_t> &ran = image[1];
    check(word32(ran,0x1038)==8u*32768u*3u,
          "the fixture ran its loop to the end");
    check(word32(ran,0x1100)==8u,"and took every trap through the handler");
    check(word32(ran,0x103c)!=0,
          "and recorded a device clock that moved while it ran");
    check(image[0]==image[1],
          "the block engine leaves the same memory behind");

    std::fclose(log); std::filesystem::remove_all(dir);
}
