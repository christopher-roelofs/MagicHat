#include "machines/datarover840/power.h"
#include "runtime/machine.h"
/*
 * machine.h — the DataRover 840 as a whole.
 */
#ifndef MH_MACHINE_H
#define MH_MACHINE_H

#include "core/bus/bus.h"
#include "cpu/mips/r3900.h"
#include "devices/glacier/glacier.h"
#include "devices/pccard/pccard.h"
#include "devices/unknown_dev/unknown_dev.h"
#include "soc/tx39/tx39.h"
#include "machines/datarover840/keyboard.h"
#include "host/console.h"
#include "host/power.h"
#include "host/network.h"
#include "host/card_image.h"

/* Board facts, from docs/HARDWARE.md. */
#define DR840_CPU_HZ      36864000u
/* Option is active-low IO3; holding it at reset also selects the monitor. */
#define DR840_IO_BOOT_NORMAL (1u << 3)
#define DR840_BOOT_BASE 0x1FC00000u
#define DR840_BOOT_WINDOW 0x00400000u
#define DR840_RAM_BASE    0x00000000u
#define DR840_RAM_SIZE    (4u * 1024u * 1024u)
/*
 * The DRAM chip select decodes everything below the ROM window. Only 4 MB is
 * fitted (two Hitachi 51W16160TT), so the array mirrors through the rest,
 * as the board does. The ROM does not probe for the size — the monitor's
 * figure and Magic Cap's are both literals; see docs/HARDWARE.md — so what
 * is mapped here is what the guest gets.
 */
#define DR840_RAM_WINDOW  DR840_ROM_BASE_A

/*
 * PC Card address windows. Four of them, in two pairs 64 MB apart:
 *
 *   0x08000000 / 0x0C000000   card 1 / card 2
 *   0x24000000 / 0x28000000   card 1 / card 2
 *
 * The first pair is where NetBSD's TX39 header puts CARD1 and CARD2, so the
 * second pair is most likely the other of the two spaces a PC Card exposes —
 * common memory and attribute memory. The ROM treats them as a set: the card
 * probe at 0x13C34288 reads a byte from 0x08000000 and then from
 * 0x28000000, and the monitor's firmware-update check at 0x83C06F70 looks
 * for the "BowserLives" magic at 0x24000000, which is exactly what you would
 * do to spot an update card in slot 1.
 *
 * With no cards fitted all four float high, which is what card-detect logic
 * expects to see for an empty slot.
 *
 * Which pair is common and which is attribute is not established; see
 * docs/OPEN_QUESTIONS.md.
 */
#define DR840_CARD_A1_BASE  0x08000000u
#define DR840_CARD_A2_BASE  0x0C000000u
#define DR840_CARD_B1_BASE  0x24000000u
#define DR840_CARD_B2_BASE  0x28000000u
#define DR840_CARD_SIZE     0x04000000u

/* The two Glacier PC Card controllers, one per slot. */
#define DR840_PCMCIA0_BASE  0x10400000u
#define DR840_PCMCIA1_BASE  0x10800000u

#define DR840_KSEG3_DEV_BASE  0xFF000000u
#define DR840_KSEG3_DEV_SIZE  0x00001000u

/*
 * The ROM answers at two physical addresses. 0x03C00000 is where the image's
 * own code expects to run (its first instruction is a J to 0x83C0001C, which
 * only resolves inside kseg0 at 0x83C00000). 0x13C00000 is the address the
 * DataRover840F flasher writes to, per its header's load field of 0xB3C00000.
 */
#define DR840_ROM_BASE_A  0x03C00000u
#define DR840_ROM_BASE_B  0x13C00000u
#define DR840_ROM_WINDOW  (8u * 1024u * 1024u)   /* 8 MB of flash address space */

typedef struct machine machine;
typedef struct {
    machine *owner;
    unsigned slot, window;
} datarover_card_port;

struct machine {
    mh_bus  bus;
    r3900    cpu;
    r3900_jit *jit; /* host-only optional native engine */
    r3900_decode_cache *decode_cache; /* host-only optional engine */
    tx39     soc;
    mh_dr_keyboard keyboard;
    glacier  pcmcia[2];
    uint8_t    *card_image[2];
    /*
     * The cards and one bus port per window are outside the legacy snapshot
     * layout. Saving with a NIC attached is rejected until card hardware
     * state has a serializer; bus pointers themselves are host wiring.
     */
    /*
     * Insertion is an event, not a state. The OS probes a slot when the
     * card-detect lines change and never again, so a card that is simply
     * present from the first instruction is one the OS has already decided
     * about. card_at is when to close those lines; 0 means "already in".
     */
    uint64_t        card_at[2];
    bool            card_in[2];
    mh_pccard      card[2];
    /*
     * What is in each slot, so that a state can put it back.
     *
     * A card's contents are not part of the machine -- they live on the card,
     * in its own file, which the emulator maps and writes through as the
     * guest works. But *which* card was in *which* slot is part of the
     * session, and a device picked up again is expected to still have it.
     * So this is what a state records: a name, not a copy.
     */
    mh_card_kind   card_kind[2];
    char           *card_path[2];   /* resolved, so a later run finds it */
    mh_network    *network[2];
    mh_card_image storage[2];
    mh_pccard_port card_port[2][MH_PCCARD_NWINDOW];
    datarover_card_port socket_port[2][MH_PCCARD_NWINDOW];
    unknown_dev kseg3;
    console  con;
    bool     con_active;

    /*
     * Scripted touch. Coordinates are raw 10-bit ADC counts, not pixels —
     * the OS maps them through its own calibration and we must not do that
     * for it. Times are relative to the start of the run.
     */
#define MH_MAX_TAPS 256
    struct {
        uint64_t at;
        uint16_t x, y, end_x, end_y;
        bool drag;
    }        tap[MH_MAX_TAPS];
    unsigned tap_n;
    /* Keys, by instruction count: AT set 2 code, extended, make or break. */
    struct { uint8_t code; bool ext, down; uint64_t at; } key[64];
    unsigned key_n, key_i;
    unsigned tap_i;         /* next tap to deliver */
    bool power_scheduled;
    uint64_t power_at;
    unsigned power_phase;
    unsigned tap_phase;     /* 0 = pending, 1 = pressed */
    uint64_t tap_hold;
    uint64_t tap_period;    /* repeat the whole sequence; 0 = once */

    /* Probe: drive the codec's input pins on a schedule. */
    struct { uint16_t level; uint64_t at; } gpio[MH_MAX_TAPS];
    unsigned gpio_n, gpio_i;
    /* Scheduled MFIO input words: each change raises the edge interrupts
     * (INTSTATUS3 rising, INTSTATUS4 falling) for the pins that moved. */
    struct { uint32_t level; uint64_t at; } mfio_sched[MH_MAX_TAPS];
    unsigned mfio_n, mfio_i;
    /* Scheduled IO pin levels (IOCTRL's IODIN, pins 0..6): each change
     * raises INTSTATUS5's IOPOSINT (bit 7+n) or IONEGINT (bit n). */
    struct { uint32_t level; uint64_t at; } io_sched[MH_MAX_TAPS];
    unsigned io_n, io_i;
    /* Scheduled PWRINT pin levels: POSPWRINT / NEGPWRINT on each change. */
    struct { uint32_t level; uint64_t at; } pwrint_sched[MH_MAX_TAPS];
    unsigned pwrint_n, pwrint_i;
    struct { bool down; uint64_t at; } option_key[MH_MAX_TAPS];
    unsigned option_n, option_i;
    bool input_epoch_set;
    uint64_t input_epoch;

    /*
     * Track the host's battery on the emulated main battery. The backup is a
     * coin cell in the real device and has no host analogue, so it is left
     * alone.
     */
    bool     track_host_battery;
    uint64_t host_battery_next;   /* next instruction count to re-read at */

    /* Periodic framebuffer capture; 0 disables. */
    uint64_t    fb_watch_every;
    const char *fb_watch_prefix;
    unsigned    fb_watch_seq;

    uint8_t *ram;
    uint32_t ram_size;
    uint8_t *rom;
    uint32_t rom_size;
    uint8_t *rom_original; /* Firmware baseline for retained guest-written bytes. */
    uint64_t power_release_at; /* Host launch's physical ON-button release. */
    uint64_t power_close_after; /* Host grace period before an automatic OFF. */
};

/* ram_size=0 detects known ROM constants, falling back to stock RAM. */
bool mh_machine_init(machine *m, const char *rom_path, uint32_t ram_size);
void mh_machine_free(machine *m);
void mh_machine_reset(machine *m, uint32_t reset_pc);

/* Re-establish the pointers that wire the machine together. Called by init,
 * and again after a snapshot load overwrites the structs wholesale. */
void mh_machine_rebind(machine *m);

/*
 * Put a PC Card in a slot (0 or 1). The image appears in the card's memory
 * window, which is where the ROM looks: the probe at 0x83C06F38 powers the
 * slot and then reads a byte from 0x24000000 for slot 1.
 */
bool mh_machine_insert_sram(machine *m, unsigned slot, const char *path, uint32_t create_size);
/*
 * Take the card out. The controller announces a departure on the same
 * card-detect edge it announces an arrival on, which is what the guest needs
 * to stop believing whatever it cached about the card. Its image is flushed
 * and closed, so the file is complete the moment this returns.
 */
bool mh_machine_eject_card(machine *m, unsigned slot);
bool mh_machine_insert_ne2000(machine *m, unsigned slot);
bool mh_machine_insert_modem(machine *m, unsigned slot, void *link);
bool mh_machine_network(machine *m, unsigned slot, const char *pcap);
bool mh_machine_insert_card(machine *m, unsigned slot, const char *path);

bool mh_snapshot_save(machine *m, const char *path);
bool mh_snapshot_load(machine *m, const char *path);
bool mh_snapshot_ram_size(const char *path, uint32_t *size);

/* Run `insns` instructions, servicing devices as it goes. */
void mh_machine_run(machine *m, uint64_t insns, uint32_t tick_interval);
/* Request guest shutdown through the power button; bounded host-side wait. */
bool mh_machine_power_off(machine *m);
bool mh_suspend_save(machine *m, uint8_t **data, uint32_t *size);
bool mh_suspend_load(machine *m, const uint8_t *data, uint32_t size);
void mh_machine_wake(machine *m);
/* ROM 13C268E0 samples IOCTRL input 3, low when Option is pressed. */
void mh_machine_set_option(machine *m, bool down);
/* Whether it is held now. The line lives in the machine and travels in its
 * state, so a window asks rather than keeping a copy that a restore would
 * silently contradict. */
bool mh_machine_option_held(const machine *m);

/*
 * Read the framebuffer out as 8-bit greyscale, one byte per pixel, 0 = black.
 * `out` must hold at least PANEL_SCREEN_W * PANEL_SCREEN_H bytes. Returns
 * false when the video controller is disabled or the framebuffer it points
 * at is outside RAM.
 */
bool mh_machine_read_fb(machine *m, uint8_t *out, unsigned *w, unsigned *h);

/* Write the current framebuffer to a PGM file. Returns false if video is off. */
bool mh_machine_dump_fb(machine *m, const char *path);

mh_runtime mh_datarover_runtime(machine *m);
#endif /* MH_MACHINE_H */
