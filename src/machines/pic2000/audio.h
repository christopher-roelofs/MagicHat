#pragma once
#include <cstdint>
#include "machines/pic2000/registers.h"
extern "C" {
#include "core/bus/bus.h"
}
/* ROM-derived playback path; see docs/PIC2000_AUDIO.md. */
struct Pic2000Audio {
    static constexpr unsigned output_rate = 44100;
    uint64_t cycles = 0, phase = 0, samples = 0, half_irqs = 0;
    unsigned offset = 0, source_phase = 0;
    bool running = false;
    bool experimental_divider = false;
    /* Envoy 006E4670 waits on B0 word bit 4 between serial output edges;
     * 006E2C70 waits on the same source before reading audio byte +61.
     * This identifies a sample-ready source, separate from DMA boundaries. */
    bool sample_ready_irq = false;
    bool approximate_output = true;
    unsigned filter_rate = 0;
    double filter_alpha = 0, filter1 = 0, filter2 = 0;
    int16_t held = 0;
    void (*sink)(void *, int16_t) = nullptr;
    void *ctx = nullptr;
    static unsigned rate(uint16_t reg);
    static unsigned divider_rate(uint16_t reg);
    void tick(uint64_t now, unsigned clock, Pic2000Registers &r, mh_bus &bus);
};
