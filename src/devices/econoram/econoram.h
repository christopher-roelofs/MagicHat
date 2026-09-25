#pragma once
#include <cstdint>

/* DS2223-compatible 256-bit SRAM, not a fabricated battery-information ROM.
 * DS2223/DS2224 datasheet pp.2-3: 264 slots, command LSB-first, F9 writes;
 * other valid commands read. Zero slots synchronize an idle command parser.
 * Callers provide CPU clocks, not instruction counts. No presence pulse:
 * this part uses 264 zero slots, unlike reset/presence 1-Wire devices. */
struct EconoRam {
    uint8_t bytes[32] = {};
    uint8_t command = 0;
    unsigned command_bits = 0, position = 0;
    bool data = false, writing = false, host_low = false;
    bool read_slot = false, output = true;
    uint64_t low_at = 0;
    uint64_t transactions = 0, written_bits = 0;

    void receive(bool bit)
    {
        if (data) {
            if (writing) {
                uint8_t mask = uint8_t(1u << (position & 7));
                if (bit) bytes[position >> 3] |= mask;
                else bytes[position >> 3] &= uint8_t(~mask);
                written_bits++;
            }
            if (++position == 256) data = false;
            return;
        }
        if (!command_bits && !bit) return;
        if (!command_bits) command = 0;
        if (bit) command |= uint8_t(1u << command_bits);
        if (++command_bits == 8) {
            command_bits = 0;
            if ((command & 7) != 1) return;
            data = true;
            writing = (command & 0xf8) == 0xf8;
            position = 0;
            transactions++;
        }
    }

    void drive(bool low, uint64_t now, unsigned hz)
    {
        if (low == host_low) return;
        host_low = low;
        if (low) {
            low_at = now;
            read_slot = data && !writing;
            if (read_slot) {
                output = (bytes[position >> 3] >> (position & 7)) & 1;
                if (++position == 256) data = false;
            } else output = true;
        } else if (!read_slot) {
            /* Sample-window boundary: short release writes 1, sustained
             * low writes 0. MC68349 cycle estimates remain an approximation. */
            receive(now >= low_at && (now - low_at) * 1000000ull < uint64_t(hz) * 15);
        }
    }

    bool line(uint64_t now, unsigned hz) const
    {
        if (host_low) return false;
        if (read_slot && now >= low_at &&
            (now - low_at) * 1000000ull < uint64_t(hz) * 45)
            return output;
        return true;
    }
};
