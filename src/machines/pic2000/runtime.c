#include "machines/pic2000/machine68k.h"
#include "runtime/machine.h"
#include "host/pclink.h"
static uint64_t run(void *p,uint64_t n) { return mrc_m68k_run(p,n); }
static uint64_t slots(void *p) { return mrc_m68k_insns(p); }
static uint64_t elapsed(void *p) { return mrc_m68k_elapsed_ns(p); }
static bool stopped(void *p) { return mrc_m68k_stopped(p); }
static bool frame(void *p,uint8_t *out,unsigned *w,unsigned *h) {
    mrc_m68k_lcd(p,out); *w=PIC2000_SCREEN_W; *h=PIC2000_SCREEN_H; return true;
}
static bool pen(void *p,bool down,unsigned x,unsigned y) { return mrc_m68k_set_pen(p,down,x,y); }
static void option(void *p,bool down) { mrc_m68k_set_option(p,down); }
static bool option_held(void *p) { return mrc_m68k_option_held(p); }
static void power(void *p,bool down) { mrc_m68k_power_button(p,down); }
static unsigned rate(void *p) { return mrc_m68k_audio_rate(p); }
static void sink(void *p,mrc_runtime_audio_sink fn,void *ctx) { mrc_m68k_audio_sink(p,fn,ctx); }
static bool adapter(void *p,bool attached) { return mrc_m68k_host_adapter(p,attached); }
static bool battery(void *p,int percent) { return mrc_m68k_host_battery(p,percent); }
static bool keyboard(void *p,unsigned usage,bool down,bool repeat) { return mrc_m68k_keyboard_key(p,usage,down,repeat); }
static void release(void *p) { mrc_m68k_keyboard_release(p); }
static bool card_present(void *p,unsigned slot) {
    return mrc_m68k_card_present(p,slot);
}
static const char *card_name(void *p,unsigned slot) {
    return mrc_m68k_card_path(p,slot);
}
static const char *card_type(void *p,unsigned slot) {
    return mrc_m68k_card_present(p,slot) ? "SRAM" : NULL;
}
static bool card_insert_sram(void *p,unsigned slot,const char *path) {
    return mrc_m68k_insert_sram(p,slot,path,2u*1024u*1024u);
}
static bool card_eject(void *p,unsigned slot) { return mrc_m68k_eject_card(p,slot); }
static bool save_state(void *p,const char *path) { return mrc_m68k_save_state(p,path); }
static mrc_pclink *g_installer;
static struct mrc_pclink *install(void *p,const char *path) {
    m68k_machine *m=p;
    if (g_installer) {
        mrc_pclink_state at=mrc_pclink_state_of(g_installer);
        if (at != MRC_PCLINK_DONE && at != MRC_PCLINK_FAILED) return NULL;
        mrc_pclink_close(g_installer); g_installer=NULL;
    }
    g_installer=mrc_pclink_open(path);
    if (!g_installer) return NULL;
    mrc_m68k_attach_pclink(m, g_installer);
    return g_installer;
}
mrc_runtime mrc_pic2000_runtime(m68k_machine *m) {
    static const mrc_runtime_ops ops={.run_slots=run,.slots=slots,.elapsed_ns=elapsed,.stopped=stopped,.frame=frame,.pen=pen,.option=option,.option_held=option_held,.audio_rate=rate,.audio_sink=sink,.save_state=save_state,.install=install,.power_button=power,.keyboard_key=keyboard,.keyboard_release=release,.card_present=card_present,.card_name=card_name,.card_type=card_type,.card_insert_sram=card_insert_sram,.card_eject=card_eject,.host_battery=battery,.host_adapter=adapter};
    return (mrc_runtime){m,mrc_m68k_is_envoy(m)?"envoy":mrc_m68k_is_hix(m)?"hix300":"pic2000",PIC2000_CPU_HZ,&ops,MRC_FRAME_LCD2};
}
