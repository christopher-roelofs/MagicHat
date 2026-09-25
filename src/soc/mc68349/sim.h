#pragma once
extern "C" {
#include "core/bus/bus.h"
}
#include "soc/mc68349/duart.h"
#include <cstdio>
struct Mc68349Sim {
    uint16_t reg[0x1000u / 2] = {};
    FILE *log = stderr;
    unsigned logged = 0;
    /*
     * The boot overlay lives and dies here. A 68300's boot chip select
     * answers the whole address space out of reset so the vector pair can be
     * fetched from address 0; the moment software gives that chip select a
     * real window, the overlay stops and whatever the other selects name
     * appears underneath. Programming CS0's base is that moment.
     */
    void (*boot_select_programmed)(void *) = nullptr;
    void *board = nullptr;
    mrc_bus *bus = nullptr;
    bool overlay_off = false;
    Mc68349Duart *duart = nullptr;
};

uint32_t mc68349_sim_read(void *, uint32_t, unsigned);
void mc68349_sim_write(void *, uint32_t, unsigned, uint32_t);
void mc68349_sim_report_chip_selects(const Mc68349Sim *, FILE *);
