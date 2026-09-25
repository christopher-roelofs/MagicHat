#include "machines/datarover840/machine.h"
#include "runtime/machine.h"
#include "host/pclink.h"
#include "host/serial.h"
static uint64_t run(void *p, uint64_t n) {
    machine *m=p; uint64_t before=m->cpu.insn_count;
    mrc_machine_run(m,n,1024); return m->cpu.insn_count-before;
}
static uint64_t slots(void *p) { return ((machine*)p)->cpu.insn_count; }
static uint64_t elapsed(void *p) { return mrc_time_ns(((machine*)p)->cpu.cycle_count,DR840_CPU_HZ); }
static bool stopped(void *p) { return ((machine*)p)->cpu.halted; }
static bool frame(void *p,uint8_t *out,unsigned *w,unsigned *h) { return mrc_machine_read_fb(p,out,w,h); }
static bool pen(void *p,bool down,unsigned x,unsigned y) {
    machine *m=p; uint16_t rx=0,ry=0;
    if(down) mrc_panel_px_to_raw(x,y,&rx,&ry);
    mrc_ucb_set_pen(&m->soc.sib.codec,down,rx,ry); return true;
}
static void option(void *p,bool down) { mrc_machine_set_option(p,down); }
static bool option_held(void *p) { return mrc_machine_option_held(p); }
static void key(void *p,uint8_t b) { machine *m=p; if(!m->soc.uart[0].rx_full) mrc_uart_rx_byte(&m->soc.uart[0],b); }
static unsigned rate(void *p) { return mrc_sib_output_rate_hz(&((machine*)p)->soc.sib); }
static void sink(void *p,mrc_runtime_audio_sink fn,void *ctx) { mrc_sib_set_audio_sink(&((machine*)p)->soc.sib,fn,ctx); }
static bool save(void *p,const char *path) { return mrc_snapshot_save(p,path); }
/*
 * Send a package the way a PC would have: the emulator is the computer at the
 * other end of the guest's serial port. One transfer at a time, because the
 * guest has one link and a second offer mid-transfer would interleave on the
 * wire.
 */
static mrc_pclink *g_installer;
static mrc_serial  g_install_link;
static struct mrc_pclink *install(void *p, const char *path) {
    machine *m = p;
    if (g_installer) {
        mrc_pclink_state at = mrc_pclink_state_of(g_installer);
        if (at != MRC_PCLINK_DONE && at != MRC_PCLINK_FAILED) return NULL;
        mrc_pclink_close(g_installer);
        g_installer = NULL;
    }
    g_installer = mrc_pclink_open(path);
    if (!g_installer) return NULL;
    mrc_serial_attach(&g_install_link, g_installer);
    mrc_uart_set_link(&m->soc.uart[0], &g_install_link);
    return g_installer;
}
static bool screen(void *p,const char *path) { return mrc_machine_dump_fb(p,path); }
static void power(void *p, bool down) { mrc_power_set_button(&((machine*)p)->soc.power, down); }
static bool keyboard_key(void *p,unsigned usage,bool down,bool repeat) {
    machine *m=p;
    if(m->soc.mbus_port!=&m->keyboard.port) return false;
    mrc_dr_keyboard_host_key(&m->keyboard,usage,down,repeat);
    return true;
}
static void keyboard_release(void *p) { mrc_dr_keyboard_release(&((machine*)p)->keyboard); }
static bool card_present(void *p,unsigned slot) {
    machine *m=p;
    return slot < 2 && m->card[slot].kind != NULL;
}
static const char *card_name(void *p,unsigned slot) {
    machine *m=p;
    if (slot < 2 && m->card[slot].kind == &mrc_pccard_ne2000)
        return "Internet";
    return slot < 2 ? m->card_path[slot] : NULL;
}
static const char *card_type(void *p,unsigned slot) {
    machine *m=p;
    if (slot >= 2 || !m->card[slot].kind) return NULL;
    return m->card[slot].kind == &mrc_pccard_ne2000 ? "NE2000" : "SRAM";
}
static bool card_insert_sram(void *p,unsigned slot,const char *path) {
    return mrc_machine_insert_sram(p,slot,path,2u*1024u*1024u);
}
static bool card_eject(void *p,unsigned slot) { return mrc_machine_eject_card(p,slot); }
static bool card_insert_ne2000(void *p,unsigned slot) {
    machine *m=p;
    if (!mrc_machine_insert_ne2000(m,slot)) return false;
    if (mrc_machine_network(m,slot,NULL)) return true;
    mrc_machine_eject_card(m,slot);
    return false;
}
mrc_runtime mrc_datarover_runtime(machine *m) {
    static const mrc_runtime_ops ops={
        .run_slots=run,.slots=slots,.elapsed_ns=elapsed,.stopped=stopped,
        .frame=frame,.pen=pen,.option=option,.option_held=option_held,.key_byte=key,.audio_rate=rate,
        .audio_sink=sink,.save_state=save,.install=install,.save_screen=screen,
        .power_button=power,.keyboard_key=keyboard_key,
        .keyboard_release=keyboard_release,.card_present=card_present,
        .card_name=card_name,
        .card_type=card_type,.card_insert_sram=card_insert_sram,
        .card_insert_ne2000=card_insert_ne2000,.card_eject=card_eject};
    return (mrc_runtime){m,"datarover840",DR840_CPU_HZ,&ops,MRC_FRAME_GRAY8};
}
