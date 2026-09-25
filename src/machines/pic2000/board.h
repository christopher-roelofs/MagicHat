/*
 * board.h — shared 68k board state for PIC-2000, HIX-300, and Envoy.
 *
 * This is not part of the board's public interface: machine68k.h is. It
 * exists because saving and restoring the machine has to see every field,
 * and putting that code in machine68k.cpp alongside everything else would
 * have buried it. Nothing outside src/machines/pic2000 should include it.
 */
#ifndef MRC_PIC2000_BOARD_H
#define MRC_PIC2000_BOARD_H

#include "machines/envoy/board.h"
#include "soc/mc68349/bus.h"
using Cpu = mrc::Mc68349Bus;
#include "soc/mc68349/sim.h"
#include "machines/pic2000/registers.h"
#include "machines/pic2000/audio.h"
#include "machines/pic2000/magicbus.h"
#include "machines/pic2000/machine68k.h"

extern "C" {
#include "core/bus/bus.h"
#include "devices/pccard/pccard.h"
#include "devices/ne2000/ne2000.h"
#include "host/network.h"
#include "host/card_image.h"
}

extern "C" {
#include "cpu/m68k/core/m68k.h"
#include "cpu/m68k/core/m68k_block.h"
}

#include <vector>

/* PIC-2000 board assembly and wiring. Register behavior lives in registers.*. */
struct m68k_machine {
    mrc_bus  bus;
    Mc68349Sim      sim;
    Pic2000Registers    dev21, dev0c;
    Pic2000Audio        audio;
    PicMagicBus        magicbus;
    EconoRam           battery_ram;
    uint64_t audio_at = 0;
    Cpu      cpu;
    Mc68349Duart duart;
    ne2000 net_probe_nic = {};
    mrc_network *net_probe_link = nullptr;
    bool net_probe_enabled = false;
    uint8_t net_slot2_config = 0;
    unsigned net_probe_logged = 0;
    unsigned net_slot2_attr_logged = 0;
    unsigned dev21_irq_now = 0, serial_irq_now = 0;
    uint8_t *rom = nullptr;
    uint8_t *ram = nullptr;
    uint32_t ram_len = 0;
    uint8_t *xram = nullptr;
    uint32_t xram_len = 0;
    mrc_region *xram_region = nullptr;
    mrc_region *testimg_region = nullptr;
    mrc_pccard card[2] = {};
    mrc_pccard_port card_port[2][MRC_PCCARD_NWINDOW] = {};
    mrc_card_image card_storage[2] = {{-1, nullptr, 0}, {-1, nullptr, 0}};
    char *card_path[2] = {};
    bool card_present[2] = {};
    bool card_event[2] = {};
    const char *cover_path = nullptr;
    bool envoy = false;
    bool envoy_mc31 = false; // Identified, but keyboard bring-up is unverified.
    bool hix = false;
    bool battery_override = false;
    bool adapter_override = false;
    // Historical names also cover mc31; snapshot field layout stays unchanged.
    bool hix_checksum_intercept = false;
    /*
     * Whether the substitution is still ahead of us.
     *
     * The intercept is one instruction in a whole run, and testing for it
     * kept the machine on the slow loop from start to finish. It is
     * cleared once it fires and re-armed at reset, because a machine that
     * powers on again checks its ROM again.
     */
    bool hix_checksum_pending = false;
    uint32_t hix_checksum_actual = 0, hix_checksum_stored = 0;
    /*
     * The address the run loop has to be standing on, or zero.
     *
     * The interception above is one instruction in a whole run, and the
     * only way to catch it used to be to look at every program counter,
     * which meant interpreting every instruction: nine million of them on
     * a HIX-300 to reach one address. The engine can
     * be told the address instead and will stop in front of it. Set this
     * through stop_at(), which flushes the block cache, because a block
     * built under a different answer may run straight through it.
     */
    uint32_t stop_at = 0;
    unsigned cpi = PIC2000_DEFAULT_CPI;
    /* The timer's one-shot compare, expressed in our own instruction count
     * so the run loop tests an integer rather than redoing the counter
     * arithmetic on every instruction. */
    bool     timer_armed = false;
    uint64_t timer_at = 0;
    bool     pen_timer_armed = false;
    uint64_t pen_timer_at = 0;
    struct TouchEvent {
        uint64_t at, release_at;
        unsigned x, y;
        bool screen_point;
    };
    std::vector<TouchEvent> touches;
    size_t   touch_next = 0;
    bool     touch_down = false;
    bool     power_down = false, power_scheduled = false;
    bool     idle_fast_enabled = true;
    bool     quiet_fast_enabled = true;
    bool     power_key_down = false;
    uint64_t power_at = 0, power_release_at = 0;
    uint64_t boot_release_at = 0;
    unsigned irq_now = 0;
    uint64_t timer_fires = 0;
    /* The project-owned CPU32 state, shared by the interpreter and block/JIT
     * engines. */
    m68k         core;
    m68k_blocks *blocks = nullptr;
    bool         core_enabled = false;
    uint64_t     core_calls = 0, core_ran = 0, core_empty = 0;
    /* Interrupts accepted by the CPU32 core. */
    uint64_t     core_interrupts = 0;
    uint64_t     core_irq_seen = 0, core_irq_refused = 0;
    uint64_t     core_skipped_short = 0, core_skipped_irq = 0;
    uint64_t     irq_raises = 0;
    /* Fetches from an odd address, which is a fault on this part. */
    uint64_t     core_odd = 0;
    /* How many of our core's exceptions have been named on the log. */
    uint64_t     core_exceptions_seen = 0;
    uint64_t     noquiet_stopped = 0, noquiet_hix = 0, noquiet_power = 0,
                 noquiet_diag = 0, noquiet_off = 0;
    /* Instruction trace emitted while the block engine runs. */
    uint64_t     core_trace = 0;
    FILE    *log = stderr;
};

/*
 * Ask the engine to stop in front of an address, or stop asking (0).
 * Defined in machine68k.cpp; shared because restoring a machine has to
 * arm it again.
 */
extern "C" void mrc_m68k_set_stop_at(m68k_machine *m, uint32_t pc);
#define MRC_M68K_HIX_CHECKSUM_PC 0x0E000A82u
inline uint32_t mrc_m68k_checksum_pc(const m68k_machine *m) {
    return m->envoy_mc31 ? 0x00400E88u : MRC_M68K_HIX_CHECKSUM_PC;
}

#endif /* MRC_PIC2000_BOARD_H */
