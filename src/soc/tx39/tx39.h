/*
 * tx39.h — the TMPR3902U on-chip peripheral block.
 *
 * One 1 KiB register window at physical 0x10C00000 holds every on-chip
 * peripheral, so this is modelled as a single MMIO region that dispatches by
 * offset to the submodule that owns it — which is how the silicon is laid out.
 */
#ifndef MH_TX39_H
#define MH_TX39_H

#include "soc/tx39/tx39_mbus.h"
#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "soc/tx39/tx39_regs.h"
#include "soc/tx39/ucb1100_audio.h"
#include "cpu/mips/r3900.h"

/*
 * ICU -> CPU interrupt line assignment.
 *
 * NetBSD's tx39icu.c puts banks 1..5 on IP2 and bank 6 (IRQHIGH/IRQLOW) on
 * IP4 for the TX3912 machines it supports. **The DataRover is wired
 * differently**, and its own ROM says so. The exception handler at
 * 0x83C25678 decodes Cause like this:
 *
 *     andi t8, t9, 0x4000     ; IP6 -> one callback from RAM[0xE4D8]
 *     andi t8, t9, 0x1000     ; IP4 -> s1 = 0xB0C00100 (INTRSTATUS1), then
 *                             ;        five calls to the bank walker, with
 *                             ;        tables at RAM[0xE0A0, E1A0, E2A0,
 *                             ;        E3A0, E4A0]
 *
 * and the walker at 0x83C25930 reads status from 0(s1) and enable from
 * 0x18(s1) — INTRSTATUS1 at 0x100 and INTRENABLE1 at 0x118 are exactly
 * 0x18 apart. So banks 1..5 drive IP4 here.
 *
 * This matters more than it looks. Magic Cap never unmasks IP2 at all, so
 * with the NetBSD mapping every peripheral interrupt was asserted onto a
 * line nothing was listening to and the OS simply never received one.
 *
 * IP6 is inferred rather than proven: its handler dispatches a single
 * callback and never reads the ICU, which looks like a dedicated line, and
 * bank 6's sources are named IRQHIGH/IRQLOW as though they were external IRQ
 * pins. See docs/OPEN_QUESTIONS.md.
 */
#define TX39_IP_NORMAL  4
#define TX39_IP_HIGH    6

typedef struct tx39 tx39;

/*
 * Post-codec sound samples at mh_sib_output_rate_hz(). A NULL sink disables
 * delivery only; guest DMA, codec state and interrupts still advance.
 */
typedef void (*mh_audio_sink)(void *ctx, int16_t sample);

/* ---- UART ---- */
typedef struct {
    tx39    *soc;
    unsigned index;         /* 0 = A, 1 = B */
    uint32_t ctrl1, ctrl2;
    uint32_t dmactrl1, dmactrl2, dmacnt;
    uint8_t  rx;
    bool     rx_full;
    /* Host side: UART A is the IDT monitor console. */
    bool     console;
    /* Diagnostics. */
    uint64_t tx_bytes, rx_delivered, rxhold_reads, ctrl1_reads;
} tx39_uart;

typedef struct {
    uint64_t tx_done, rx_next;
    uint8_t shift, hold;
    bool shifting, holding;
} tx39_uart_timing;

/* ---- SIB / UCB1100 ---- */
typedef struct {
    uint16_t reg[16];       /* UCB1100 control registers 0..15 */
    /* Touchscreen panel state, driven by the host. */
    bool     pen_down;
    uint16_t pen_x, pen_y;  /* raw 10-bit ADC counts */
    /* The codec's IRQ pin, which the SIB presents to the host. */
    bool     irq_out;
    /* Auxiliary ADC channels AD0..AD3 — board voltages. AD2 is the main
     * battery and AD3 the backup; see mh_ucb_init() for the defaults. */
    uint16_t aux[4];
} ucb1100;

typedef struct {
    tx39    *soc;
    uint32_t size, ctrl, dmactrl;
    uint32_t sndrxstart, sndtxstart, telrxstart, teltxstart;
    uint32_t sndhold, telhold;
    uint32_t sf0ctrl, sf1ctrl;
    uint32_t sf0stat, sf1stat;
    ucb1100  codec;

    /*
     * Sound transmit ring. The SIB plays from a buffer in DRAM at the rate
     * its frame sync runs; we pull samples from it on the same schedule and
     * hand them to the host.
     */
    uint32_t snd_read_off;      /* byte offset into the ring */
    uint64_t snd_cycle_ref;     /* CPU cycle the last sample was taken at */
    uint64_t snd_samples;       /* total samples pulled, for reporting */
    bool     snd_running;

    bool     irq_seen;      /* last codec IRQ level, for edge detection */
    /* Diagnostics: which codec registers software actually touches. */
    uint64_t codec_reads[16], codec_writes[16];
    mh_audio_sink audio_sink;
    void          *audio_ctx;
    unsigned log_codec;     /* log this many codec transactions, then stop */
} tx39_sib;

/* ---- Video ---- */
typedef struct {
    tx39    *soc;
    uint32_t ctrl[14];      /* VIDEOCTRL1..14 */
} tx39_video;

/* ---- Timer / RTC ---- */
typedef struct {
    tx39    *soc;
    uint32_t control, periodic;
    uint32_t alarm_hi, alarm_lo;
    uint64_t rtc;           /* 40-bit counter, 32.768 kHz */
    /* Exposed so other blocks with 32.768 kHz timers can read the clock. */
    uint64_t rtc_cycle_ref; /* CPU cycle at which `rtc` was last recomputed */
    uint32_t periodic_reload;
    uint64_t last_period;   /* index of the last periodic tick delivered */
} tx39_timer;

/* ---- ICU ---- */
typedef struct {
    tx39    *soc;
    uint32_t status[TX39_ICU_NBANK];   /* latched sources */
    uint32_t enable[TX39_ICU_NBANK];
} tx39_icu;

/* ---- Power ---- */
typedef struct {
    tx39    *soc;
    uint32_t ctrl;
    bool     stptimer_armed;
    bool warm;   /* say WARMSTART on the first read: retained RAM under reset */
    uint64_t stptimer_deadline;   /* in RTC ticks */
} tx39_power;

/* ---- MBUS ---- */
typedef struct {
    tx39    *soc;
    uint32_t reg[(TX39_MBUS_LAST - TX39_MBUS_FIRST) / 4 + 1];
    uint64_t transfers;
} tx39_mbus;

struct tx39 {
    r3900      *cpu;

    tx39_icu    icu;
    tx39_uart   uart[2];
    tx39_sib    sib;
    tx39_video  video;
    tx39_timer  timer;
    tx39_mbus   mbus;
    tx39_power  power;

    /* Registers we store faithfully but whose behaviour is not yet modelled.
     * Reads return what was written; every such access is countable. */
    uint32_t    memconfig[9];
    uint32_t    io_ctrl, io_dataout, io_datadir, io_datain, io_datasel;
    uint32_t    io_powerdwn, io_mfiopowerdwn;
    uint32_t    clockctrl;

    /* 0x1C8..0x1FC — present on this part, undocumented. Stored so reads are
     * consistent, counted separately so they stay visible. */
    uint32_t    ext[(TX39_EXT_LAST - TX39_EXT_FIRST) / 4 + 1];
    uint64_t    ext_accesses;
    uint32_t    spictrl, spihold;
    uint32_t    irctrl1, irctrl2, irtxhold;



    /* CPU clock, for the RTC/timer relationship. 36.864 MHz on this board. */
    uint32_t    cpu_hz;

    /* Per-register access histogram (offset/4). Diagnostics only. */
    uint64_t    hist_read[TX39_CFG_SIZE / 4];
    uint64_t    hist_write[TX39_CFG_SIZE / 4];
    uint32_t    hist_pc[TX39_CFG_SIZE / 4];   /* last access site */
    const uint32_t *pc_hint;

    FILE       *log;
    bool        log_unknown;     /* report reads/writes we do not decode */
    uint64_t    unknown_reads, unknown_writes;
    /* Appended so version-2 snapshots can restore the preceding layout. */
    tx39_uart_timing uart_timing[2];
    ucb1100_audio audio; /* appended in snapshot version 4 */
    /* Host/board wiring after the serialized hardware prefix. */
    tx39_mbus_port *mbus_port;
};

#define MH_TX39_STATE_BYTES offsetof(tx39, mbus_port)

void mh_tx39_init(tx39 *s, r3900 *cpu, uint32_t cpu_hz);

/* MMIO entry points, registered with the bus. */
uint32_t mh_tx39_read(void *ctx, uint32_t off, unsigned size);
void     mh_tx39_write(void *ctx, uint32_t off, unsigned size, uint32_t val);

/* Called once per emulated instruction batch to advance time-based devices. */
void mh_tx39_tick(tx39 *s);
void mh_tx39_print_histogram(const tx39 *s, FILE *f, unsigned top);

/* ---- submodule interfaces ---- */

/* ICU: raise a source in bank (1..6). Recomputes the CPU interrupt lines. */
bool mh_icu_pending(const tx39_icu *icu);
void mh_icu_raise(tx39_icu *icu, unsigned bank, uint32_t bits);
void mh_icu_update(tx39_icu *icu);
uint32_t mh_icu_read(tx39_icu *icu, uint32_t off, bool *decoded);
bool     mh_icu_write(tx39_icu *icu, uint32_t off, uint32_t val);

/* UART */
uint32_t mh_uart_read(tx39_uart *u, uint32_t off, bool *decoded);
bool     mh_uart_write(tx39_uart *u, uint32_t off, uint32_t val);
void     mh_uart_set_link(tx39_uart *u, void *link);
void    *mh_uart_link(const tx39_uart *u);
void     mh_uart_rx_byte(tx39_uart *u, uint8_t byte);
void     mh_uart_update(tx39_uart *u);
uint64_t mh_uart_frame_cycles(const tx39_uart *u);

/* Timer */
uint32_t mh_timer_read(tx39_timer *t, uint32_t off, bool *decoded);
bool     mh_timer_write(tx39_timer *t, uint32_t off, uint32_t val);
void     mh_timer_tick(tx39_timer *t);
uint64_t mh_timer_rtc_now(const tx39_timer *t);

/* Video */
uint32_t mh_video_read(tx39_video *v, uint32_t off, bool *decoded);
bool     mh_video_write(tx39_video *v, uint32_t off, uint32_t val);
/* Current framebuffer geometry as programmed. Returns false when video is off. */
bool     mh_video_geometry(const tx39_video *v, uint32_t *fb_pa,
                            unsigned *w, unsigned *h, unsigned *bpp);

/* Power */
uint32_t mh_power_read(tx39_power *p, uint32_t off, bool *decoded);
bool     mh_power_write(tx39_power *p, uint32_t off, uint32_t val);
void     mh_power_tick(tx39_power *p);
void     mh_power_set_button(tx39_power *p, bool pressed);

/* MBUS */
void mh_mbus_connect(tx39_mbus *m, tx39_mbus_port *port);
void mh_mbus_input(tx39_mbus *m, bool high);
bool mh_mbus_receive_word(tx39_mbus *m, uint32_t word);
bool mh_mbus_receive_command(tx39_mbus *m, uint16_t word);
uint32_t mh_mbus_read(tx39_mbus *m, uint32_t off, bool *decoded);
bool     mh_mbus_write(tx39_mbus *m, uint32_t off, uint32_t val);
void     mh_mbus_update(tx39_mbus *m);

/* SIB */
uint32_t mh_sib_read(tx39_sib *sib, uint32_t off, bool *decoded);
void     mh_sib_update(tx39_sib *sib);

uint32_t mh_sib_snd_rate_hz(const tx39_sib *sib);
uint32_t mh_sib_output_rate_hz(const tx39_sib *sib);
void     mh_sib_set_audio_sink(tx39_sib *sib, mh_audio_sink fn, void *ctx);
void     mh_sib_pump_audio(tx39_sib *sib);
void     mh_sib_print_codec_traffic(const tx39_sib *sib, FILE *f);
bool     mh_sib_write(tx39_sib *sib, uint32_t off, uint32_t val);

/* UCB1100 codec register access, as seen from the SIB. */
uint16_t mh_ucb_read(ucb1100 *u, unsigned reg);
void     mh_ucb_write(ucb1100 *u, unsigned reg, uint16_t val);
void     mh_ucb_init(ucb1100 *u);
void     mh_ucb_set_pen(ucb1100 *u, bool down, uint16_t x, uint16_t y);
/* Raw codec GPIO inputs; DataRover Option is TX39 IO3, not a codec pin. */
void     mh_ucb_set_gpio_in(ucb1100 *u, uint16_t level);

/*
 * Panel geometry: where on the digitiser a given screen pixel is.
 *
 * A real panel's active area covers the display with some margin, so its raw
 * ADC readings span part of the converter's range rather than all of it. We
 * have to choose that span, because the emulated panel has no physical
 * dimensions of its own — and the choice is not free, because the OS
 * validates each calibration touch against a coarse default mapping built
 * into the ROM and rejects one that lands too far from the target.
 *
 * The span below is derived from three touches the ROM accepted, at the
 * three calibration targets whose centres are at screen (22.5, 22.5),
 * (456.5, 296.5) and (239.5, 159.5). Those three points fit one straight
 * line per axis exactly, and the resulting mapping therefore sits inside the
 * ROM's tolerance everywhere. It is a convention we picked, satisfying a real
 * constraint — not a measurement. See docs/OPEN_QUESTIONS.md.
 */
#define PANEL_SCREEN_W  480u
#define PANEL_SCREEN_H  320u
#define PANEL_RAW_X0     85u    /* raw reading at screen x = 0   */
#define PANEL_RAW_X1    836u    /* raw reading at screen x = 479 */
#define PANEL_RAW_Y0     69u    /* raw reading at screen y = 0   */
#define PANEL_RAW_Y1    791u    /* raw reading at screen y = 319 */

void     mh_panel_px_to_raw(unsigned px, unsigned py,
                             uint16_t *rx, uint16_t *ry);

#endif /* MH_TX39_H */
