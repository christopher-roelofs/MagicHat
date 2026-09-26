#pragma once

#include <cstddef>
#include <cstdint>

extern "C" {
#include "host/serial.h"
#include "host/ppp.h"
}

/* The MC68349 serial block is a two-channel MC68681-compatible DUART. The
 * PIC uses channel B for PC Link. Channel A can connect to a host PTY or the
 * experimental PPP/libslirp virtual modem. */
struct Mc68349Duart {
    struct ChannelA {
        bool enabled = false, rx_enabled = false, tx_enabled = false;
        bool tx_busy = false;
        uint8_t rx[3] = {}, rx_at = 0, rx_len = 0, tx_byte = 0;
        uint64_t tx_done_at = 0, rx_next_at = 0;
    } a;
    uint16_t mcr = 0;
    uint8_t ilr = 0, ivr = 0x0F, acr = 0;
    uint8_t mr1a = 0, mr2a = 0, csra = 0;
    uint8_t mr1b = 0, mr2b = 0, csrb = 0;
    uint8_t cra = 0, crb = 0, ier = 0, isr = 0;
    uint8_t rx[4] = {};
    unsigned rx_at = 0, rx_len = 0;
    bool rx_enabled = false, tx_enabled = false;
    bool tx_busy = false;
    uint8_t tx_byte = 0;
    uint64_t tx_done_at = 0;
    uint64_t rx_next_at = 0;
    uint64_t *insns = nullptr;
    unsigned clock_hz = 0;
    bool dirty = false;
    mh_serial link = {-1, {}, nullptr};
    mh_serial link_a = {-1, {}, nullptr};
    mh_ppp *ppp = nullptr;

    bool open_a();
    void attach_ppp(mh_ppp *);
    void detach_ppp();
    bool active() const;
    void attach(struct mh_pclink *peer);
    void detach();
    void tick();
    bool irq() const;
    uint32_t read(uint32_t off, unsigned size);
    void write(uint32_t off, unsigned size, uint32_t value);

private:
    uint8_t read_byte(unsigned off);
    void write_byte(unsigned off, uint8_t value);
    void command_b(uint8_t value);
    void command_a(uint8_t value);
    void tick_a();
    unsigned baud(bool channel_a = false, bool receive = false) const;
    uint8_t status_a() const;
    uint8_t status_b() const;
    void refresh_status();
};

uint32_t mc68349_duart_read(void *, uint32_t, unsigned);
void mc68349_duart_write(void *, uint32_t, unsigned, uint32_t);
