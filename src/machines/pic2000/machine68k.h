/*
 * machine68k.h — shared PIC-2000, HIX-300, and Envoy 68k machine API.
 *
 * Separate from machine.h, which is the DataRover 840: a different CPU, a
 * different board and a different major version of the operating system.
 * What the two share is the bus, the snapshot discipline and the habit of
 * making every unknown access say so.
 *
 * PIC-2000 was the first target. The shared implementation now selects
 * board-specific behavior for HIX-300 and Envoy ROMs as well; PIC-1000 is
 * recognized but has no runnable board. The legacy directory and function
 * names do not imply PIC-2000-only support.
 *
 * What is established about it, all measured from the image rather than
 * taken from documentation:
 *
 *   - 68300-family with a CPU32 core. The reset path executes movec and
 *     moves and loads the VBR, none of which a 68000 has, and it programs a
 *     System Integration Module -- chip selects at +0x40 through +0x58 --
 *     which is the 68340/68349 signature.
 *   - Big-endian, ROM at 0x0E000000, and the SIM at 0x3C000000.
 *   - Reset SP 0x00100000 and reset PC 0x0E00021E, from the vector pair at
 *     the start of the image.
 *
 * The MC68349 manual identifies the core as CPU32+. The project-owned core
 * models its odd instruction-address fault and format-C return.
 * BGND has been observed on assertion paths. General bus-error recovery,
 * other CPU32-only instructions and accurate cycle timing remain incomplete.
 */
#ifndef MH_MACHINE68K_H
#define MH_MACHINE68K_H

#include "runtime/machine.h"
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>

struct mh_pclink;

#ifdef __cplusplus
extern "C" {
#endif

/* The board, as far as it is known. Each of these is a measurement or a
 * deliberate guess, and the guesses are marked as such where they are used. */
#define PIC2000_ROM_BASE    0x0E000000u
#define PIC2000_ROM_SIZE    0x00400000u    /* the image is exactly 4 MiB */
#define PIC2000_SIM_BASE    0x3C000000u    /* movea.l #$3c000000,a0 at reset */
#define PIC2000_SIM_SIZE    0x00001000u
/* MC68349 IDR=$31 selects SYNCR=CF83: W=1, X=1, Y=15, Z=1.
 * Assuming a 32.768 kHz reference, MC68349UM 4.3.3 gives 16.777216 MHz.
 * The board reference/clock-mode wiring still needs independent verification. */
#define PIC2000_CPU_HZ      16777216u
/* dev21 +0xD4. The ROM scales ticks to milliseconds by 125/16 and durations
 * back by 16/125, so the period is 125/16 ms exactly. */
#define PIC2000_COUNTER_HZ  128u
/* Instruction-count approximation until CPU cycle accounting is available. */
#define PIC2000_DEFAULT_CPI 1u
#define PIC2000_SCREEN_W    480u
#define PIC2000_SCREEN_H    320u
#define PIC2000_FB_ADDR     0x00002800u
#define PIC2000_FB_SIZE     (PIC2000_SCREEN_W * PIC2000_SCREEN_H / 4u)

typedef struct m68k_machine m68k_machine;
/*
 * The board's two banks of memory, as host bytes.
 *
 * This named what a retained store had to keep when these machines kept one
 * separately from everything else. They do not any more -- a saved state is
 * the whole machine -- but the board's own memory is still worth being able
 * to point at, and the tests reach the framebuffer through it.
 */
typedef struct { uint8_t *data; uint32_t size; } mh_m68k_region;
void mh_m68k_retained_regions(m68k_machine *, mh_m68k_region regions[2]);
bool mh_m68k_is_envoy(const m68k_machine *);
bool mh_m68k_is_hix(const m68k_machine *);
mh_runtime mh_pic2000_runtime(m68k_machine *m);
uint64_t mh_m68k_elapsed_ns(const m68k_machine *m);

m68k_machine *mh_m68k_new(const char *rom_path, unsigned ram_mb, FILE *log);
void          mh_m68k_free(m68k_machine *m);
/* Experimental modem bring-up port. This is a byte transport, not an ISP. */
bool mh_m68k_open_serial_a(m68k_machine *m);
bool mh_m68k_open_ppp(m68k_machine *m, const char *pcap);
/* Experimental PIC-2000 NE2000 aperture for a guest driver probe. */
bool mh_m68k_open_ne2000_probe(m68k_machine *m, const char *pcap);
/* Attach a writable SRAM PC Card. Existing images retain their size; a new
 * image is created at the requested size. */
bool mh_m68k_insert_sram(m68k_machine *m, unsigned slot, const char *path,
                          uint32_t create_size);
bool mh_m68k_eject_card(m68k_machine *m, unsigned slot);
bool mh_m68k_card_present(const m68k_machine *m, unsigned slot);
const char *mh_m68k_card_path(const m68k_machine *m, unsigned slot);

/* Run up to `insns` execution slots or until terminal halt. Slots include
 * LPSTOP waits: this is the legacy CLI budget, not a retired-instruction count. */
uint64_t      mh_m68k_run(m68k_machine *m, uint64_t insns);
void          mh_m68k_report(const m68k_machine *m);
bool          mh_m68k_stopped(const m68k_machine *m);
uint64_t      mh_m68k_insns(const m68k_machine *m);

/* Live front-end access. Pixels are unpacked to 0..3, where 0 is white and
 * 3 is black. Pen coordinates are panel pixels. */
void          mh_m68k_lcd(const m68k_machine *m, uint8_t *pixels);
bool          mh_m68k_set_pen(m68k_machine *m, bool down,
                               unsigned x, unsigned y);
void          mh_m68k_power_button(m68k_machine *m, bool down);
/* Magic Bus keyboard: identified PIC-2000, Envoy 1.0/pt4 and HIX-300 firmware. */
bool          mh_m68k_keyboard_connect(m68k_machine *, bool connected);
bool          mh_m68k_keyboard_key(m68k_machine *, unsigned usage, bool down, bool repeat);
void          mh_m68k_keyboard_release(m68k_machine *);
void          mh_m68k_start(m68k_machine *m);
bool          mh_m68k_powered_off(const m68k_machine *m);
bool          mh_m68k_power_off(m68k_machine *m);
void          mh_m68k_schedule_power(m68k_machine *m, uint64_t at, uint64_t hold);

/* Average cycles per instruction, used only to turn retired instructions into
 * elapsed time for the free-running counter. See the note in the .cpp. */
/*
 * Which engine runs the guest: "interpreter" for single-step execution,
 * "blocks" or "jit" for cached/native execution. Returns false
 * for a name this machine has no engine for.
 */
bool          mh_m68k_set_engine(m68k_machine *m, const char *name);
/* What that engine did, for deciding whether it is worth having. */
void          mh_m68k_engine_report(const m68k_machine *m);

/* The hardware option key: a level the guest samples, so it is held rather
 * than pressed. */
void          mh_m68k_set_option(m68k_machine *m, bool down);
/* Whether it is held now. The line lives in the machine and travels in its
 * state, so a window asks rather than keeping a copy that a restore would
 * silently contradict. */
bool          mh_m68k_option_held(const m68k_machine *m);

/*
 * The whole machine, to a file and back.
 *
 * Everything the guest can observe: both banks of RAM, the processor, the
 * devices and the battery-backed store. Not the ROM, which this board cannot
 * write, and not the block cache, which rebuilds itself. A state refuses to
 * load into a machine built from a different ROM or a different amount of
 * memory rather than producing one that looks alive and is not.
 */
bool          mh_m68k_save_state(m68k_machine *m, const char *path);
bool          mh_m68k_load_state(m68k_machine *m, const char *path);
void          mh_m68k_attach_pclink(m68k_machine *m, struct mh_pclink *link);

void          mh_m68k_set_cpi(m68k_machine *m, unsigned cpi);
/* DEVIATION, for testing only: make the converter at dev21 +0xE4 return this
 * reading for every channel. Negative means "not supplied" and reads zero. */
void          mh_m68k_set_adc(m68k_machine *m, int value);
bool          mh_m68k_host_battery(m68k_machine *m, int percent);
bool          mh_m68k_host_adapter(m68k_machine *m, bool attached);
bool          mh_m68k_set_adapter(m68k_machine *m, bool attached);
bool mh_m68k_load_battery_ram(m68k_machine *, const uint8_t *, uint32_t);
bool mh_m68k_save_battery_ram(m68k_machine *, uint8_t **, uint32_t *);
/* DEVIATION, for testing only: assert pen-down (IPL5 bit 1) at instruction
 * `at`. With trace, print every conversion the pen driver then starts. */
void          mh_m68k_touch(m68k_machine *m, uint64_t at, uint64_t len,
                            bool trace);
/* DEVIATION: schedule a tap at a 480x320 screen coordinate. */
bool          mh_m68k_tap(m68k_machine *m, uint64_t at, uint64_t len,
                          unsigned x, unsigned y, bool trace);
/* DEVIATION, for testing only: a reading for one converter channel. */
void          mh_m68k_set_adc_chan(m68k_machine *m, unsigned chan, int value);
/* DEVIATION, for testing only: a reading for one mux-and-channel pair. */
void          mh_m68k_set_adc_pair(m68k_machine *m, unsigned mux,
                                    unsigned chan, int value);

/* Diagnostics, observation only. */
void          mh_m68k_trace(m68k_machine *m, uint64_t n);
/* Start tracing when the named PC is first reached. */
void          mh_m68k_trace_after(m68k_machine *m, uint32_t pc, uint64_t n);
/* Start tracing on the selected execution of pc (first execution is 1). */
void          mh_m68k_trace_after_hit(m68k_machine *m, uint32_t pc,
                                      uint64_t hit, uint64_t n);
/* Sample the PC every n instructions and report where the time went. */
void          mh_m68k_sample(m68k_machine *m, uint64_t every);
/* DEVIATION, for testing only: assert an interrupt no device drove. */
void          mh_m68k_force_irq(m68k_machine *m, unsigned level, uint64_t at);
/* DEVIATION, for testing only: seed a register of an unnamed probe device so
 * it reads back non-zero. Answers "does this gate matter" without inventing a
 * device -- what it reports is whatever the caller says, and is not a model. */
bool          mh_m68k_probe_preset(m68k_machine *m, const char *dev,
                                    uint32_t off, uint16_t val);
/* Count executions of an address; watch_log prints registers the first n times. */
/* Count bytes written per 4K page; a framebuffer shows up as a hot run. */
void          mh_m68k_heat(m68k_machine *m, bool on);
/* Record which 256-byte blocks executed; two such files diff into "what did
 * this run reach that the other did not". */
void          mh_m68k_cover(m68k_machine *m, const char *path);
/* Count reads per RAM address; a spin loop's condition is the hottest. */
void          mh_m68k_readheat(m68k_machine *m, bool on);
/* Count executed instructions by opcode word; reported at exit. */
void          mh_m68k_insnheat(m68k_machine *m, bool on);
/* Report which instructions read a given address. */
void          mh_m68k_watch_read(m68k_machine *m, uint32_t addr);
/* Report which instructions write anywhere in [lo, hi). */
void          mh_m68k_watch_write(m68k_machine *m, uint32_t lo, uint32_t hi);
/* Write len bytes from addr to a file at exit; low RAM is built at run time. */
void          mh_m68k_dump(m68k_machine *m, uint32_t addr, uint32_t len,
                            const char *path);
void          mh_m68k_watch(m68k_machine *m, uint32_t pc);
void          mh_m68k_watch_log(m68k_machine *m, unsigned n);
void          mh_m68k_log_unknown(m68k_machine *m, bool on);
void          mh_m68k_audio_divider(m68k_machine *m, bool enabled);
void          mh_m68k_audio_approx(m68k_machine *m, bool enabled);
unsigned      mh_m68k_audio_rate(const m68k_machine *m);
void          mh_m68k_audio_sink(m68k_machine *m, void (*sink)(void *, int16_t), void *ctx);

#ifdef __cplusplus
}
#endif
#endif /* MH_MACHINE68K_H */
