#pragma once
#include <cstdint>
extern "C" {
#include "devices/magicbus/keyboard.h"
}
struct m68k_machine;

/* Shared 68k dev21 serial shifter and a single Magic Bus keyboard.
 * Board wiring and fidelity limits: docs/68K_KEYBOARD.md. */
struct PicMagicBus {
    static bool supported(const m68k_machine *);
    bool connected = false, input_high = false, assigned = false;
    bool notified = false, command_high_valid = false;
    uint8_t command_high = 0, address = 6, selection = 0, pending_read = 0;
    uint8_t write_data[8] = {};
    unsigned write_size = 0;
    mrc_mb_keyboard keys = {};
    uint8_t held[32] = {}, release[32] = {};
    uint64_t commands = 0, reads = 0, writes = 0, errors = 0;
    void write(m68k_machine *, unsigned off, unsigned size, uint32_t value);
    void service(m68k_machine *);
    void line(m68k_machine *, bool high);
    void command(m68k_machine *, uint16_t);
    bool host_key(m68k_machine *, unsigned usage, bool down, bool repeat);
    void release_keys(m68k_machine *);
    void retry_releases(m68k_machine *);
    static constexpr unsigned state_size = 384;
    void encode(uint8_t *) const;
    bool decode(const uint8_t *);
};
uint32_t pic2000_dev21_read(void *, uint32_t, unsigned);
void pic2000_dev21_write(void *, uint32_t, unsigned, uint32_t);
uint32_t pic2000_dev0c_read(void *, uint32_t, unsigned);
void pic2000_dev0c_write(void *, uint32_t, unsigned, uint32_t);
