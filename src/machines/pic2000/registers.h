#pragma once
#include <cstdint>
#include <cstdio>
#include <map>
#include <utility>
#include "devices/econoram/econoram.h"
struct Pic2000Registers {
    const char *name = "?";
    uint32_t    base = 0;
    uint16_t    reg[0x800] = {};
    uint64_t    reads[0x800] = {};
    uint64_t    writes[0x800] = {};
    FILE       *log = stderr;
    unsigned    logged = 0;
    unsigned    log_max = 40;
    bool        preset = false;   /* a value was seeded; see probe_preset */
    /* dev21+D0 reads physical inputs; a zero word write requests power-off.
     * ROM 0E088F36 masks IRQs, selects shutdown vectors, then repeatedly
     * clears D0 at 0E088F88 while waiting for the board to remove power.
     * The underlying ASIC and its other output bits remain unidentified. */
    bool        power_control = false, power_off = false;
    bool        audio_dirty = true, irq_dirty = true;
    /* PIC ROM 0E083738 samples dev0c byte +2 bit 2. High makes
     * discovery at 0E081EF6 return zero peripherals without probing.
     * This is a physical input, not a writable controller latch. */
    bool        magicbus_empty_input = false;
    uint16_t    magicbus_empty_offset = 2;
    EconoRam *econoram = nullptr;
    // Passive serial line without an attached identity device. The host
    // output still pulls it low; releasing it exposes the pull-up.
    bool serial_pullup = false;
    bool adapter_input = false, adapter_attached = false;
    void *clock_context = nullptr;
    uint64_t (*cpu_clock)(void *) = nullptr;
    unsigned clock_hz = 0;

    /*
     * A free-running counter inside an otherwise unknown register file.
     *
     * dev21 has one at +0xD4 and the ROM says so twice, in opposite
     * directions. Every reader takes the safe double read of a ripple
     * counter -- load, load again, compare, repeat until the two agree --
     * and then scales by 125/16 (0x0E08F6F8, 0x0E08F718). Every writer of a
     * deadline scales the other way, by 16/125, and adds the result to a
     * fresh read (0x0E085764, 0x0E08CA4C). Those are exact inverses, so the
     * scale alone does not establish the caller's unit. The battery-life
     * routine at 0x0E07A310 uses 3,600,000 units/hour and 1,000 units/second:
     * milliseconds. One counter tick is 125/16 ms, giving 128 Hz.
     */
    const uint64_t *insn_src = nullptr;    /* null: no counter here */
    uint32_t        counter_off = 0;
    uint64_t        counter_num = 0, counter_den = 1;

    /*
     * The compare register that goes with the counter, at +0xD8.
     *
     * Two routines write it and both do the same thing with interrupts
     * masked: take the head of a sorted timer list and copy its deadline
     * field into the register (0x0E08CE8C, 0x0E08D0FC). That is a one-shot
     * compare, reprogrammed each time the earliest deadline changes.
     */
    uint32_t        compare_off = 0;        /* 0: no compare here */
    bool            compare_dirty = false;  /* written; the deadline moved */

    /*
     * Interrupt-pending registers, where a write clears the bits it sets.
     *
     * Derived from the ROM, and it corrects an earlier reading here. Every
     * one-shot writer of these registers uses one idiom: mask a source out
     * of the enable register, then write exactly those bits to pending.
     *   0x0E010050  andi.w #$807F, $B4(a2)   disable bits 7..14
     *   0x0E010056  move.w #$7F80, $B0(a2)   write bits 7..14 to pending
     *   0x0E0828F0  andi.w #$FCFF, $B6(a2)   disable bits 8, 9
     *   0x0E0828F6  move.w #$300,  $B2(a2)   write bits 8, 9 to pending
     * Under a plain store that marks an interrupt pending for a source that
     * was just switched off. Under write-one-to-clear it is the ordinary way
     * to shut a source down without taking a stale interrupt later.
     *
     * The dispatcher at 0x0E0889C4 agrees once read this way. It clears each
     * bit it is about to service out of d3 before calling the handler, so
     * the writeback at 0x0E0889F6 does NOT touch the serviced bits -- the
     * handler deasserts the device itself -- and does clear whatever is left
     * in d3, which is the bits nobody handled. Discarding unhandled sources
     * and leaving handled ones to their handlers is exactly right.
     *
     * The ADC poll at 0x0E07A1D2 is the third witness: it writes 0x50, starts
     * a conversion and then waits for those same bits, which only means
     * anything if the write cleared them.
     */
    /*
     * The converter at +0xE4, whose protocol the ROM spells out at
     * 0x0E07A128 (Magic Cap's ReadAtoDChannel):
     *
     *   move.w  $26(a7), d7      the channel argument
     *   lsl.w   #$6, d7          channel << 6
     *   move.w  #$23, $E0(a2)    control: bits 0, 1 and 5
     *   ... 255-iteration settling delay ...
     *   move.w  #$50, $B2(a2)    clear completion bits 4 and 6
     *   move.w  d7, $E4(a2)      start the conversion
     *   ... poll $B2(a2) until (value & 0x50) ...
     *   add.w   $E4(a2), d5      read the sample; four of these, then >> 2
     *
     * So a write to +0xE4 starts a conversion and a read of it returns a
     * sample -- the register is not a plain store, and modelling it as one
     * handed the ROM back the channel number as if it were a measurement.
     * Neither completion bit is in the enable mask, so they satisfy the poll
     * without raising an interrupt.
     *
     * What the sample should BE is not modelled. Returning zero is the
     * honest answer for an input nothing drives, and the run says so once
     * rather than quietly averaging four zeros into a battery reading.
     */
    uint32_t        adc_off = 0;            /* 0: no converter here */
    bool            adc_dirty = false;      /* a conversion was started */
    bool            adc_busy = false;
    uint64_t        adc_done_at = 0;
    unsigned        adc_done_bits = 0;      /* set in the pending low half */
    bool            adc_warned = false;
    bool            trace_conv = false;
    unsigned        conv_seen = 0;
    int             adc_value = -1;         /* <0: not supplied, reads zero */
    /*
     * Per-channel readings, keyed by the channel the guest selects.
     *
     * The channel is the write to +0xE4 shifted right by six, matching the
     * ROM's own "channel << 6" on the way in and ">> 6" on the way out. It
     * is the channel and not the mux that picks the axis: the pen driver
     * converts twice under one mux setting, 0x69, writing 0x1000 and then
     * 0x0000, and keying readings on the mux gave both axes the same value.
     * The battery server uses channels 2 and 4 through the same register.
     * <0 falls back to adc_value.
     */
    int             adc_chan[1024];
    /*
     * Readings keyed by BOTH the mux and the channel, which is what the
     * driver actually varies. Keying on the channel alone is not enough:
     * tracking state 0 at 0x0E08BAAA sets mux 0x35 and converts channel
     * 0x1000, while other states convert channel 0 under a different mux, so
     * two axes can share a channel and be told apart only by the mux. Keying
     * on the mux alone is not enough either, since the pen converts twice
     * under mux 0x69. A short list beats a 128 x 1024 table.
     */
    struct { int mux, chan, value; } adc_pair[16];
    unsigned        adc_pairs = 0;
    // Acquisition phase follows guest conversion selectors, not host motion.
    int             pen_second_690 = -1;
    unsigned        pen_690_starts = 0;
    /*
     * Sample and hold. The reading belongs to the conversion that was
     * started, so it is latched when +0xE4 is written and under the mux in
     * force at that moment -- not recomputed when the result is read. The
     * difference is visible: the state at 0x0E08B944 reads the result twice
     * with another conversion started in between, and computing at read time
     * gave both reads the same value.
     */
    uint32_t        adc_latched = 0;

    /*
     * The byte-wide transmitter at +0x90..+0xA0.  The ROM enables it by
     * writing +0x90, waits for group-1 pending bit 10, writes the next word
     * to +0x94, acknowledges bit 10, and repeats (0x0E082B4A..0x0E082C60).
     * A control write therefore makes an idle transmitter ready, while a
     * data write starts a transfer whose completion makes it ready again.
     */
    bool            tx_dirty = false;
    bool            tx_busy = false;
    uint64_t        tx_done_at = 0;

    /* +0xDC is a countdown and +0xDE enables it.  Its expiry is group-1
     * pending bit 14; the ROM's handler at 0x0E08B730 stops and acknowledges
     * it before advancing the pen state machine. */
    bool            pen_timer_dirty = false;

    uint32_t        w1c_lo[2] = {0, 0}, w1c_hi[2] = {0, 0};
    bool is_w1c(unsigned o) const
    {
        for (int i = 0; i < 2; i++)
            if (w1c_hi[i] && o >= w1c_lo[i] && o < w1c_hi[i]) return true;
        return false;
    }

    uint32_t reg32(uint32_t o) const
    {
        return ((uint32_t)reg[o / 2] << 16) | reg[o / 2 + 1];
    }
    void set_bit32(uint32_t o, unsigned bit)
    {
        reg[o / 2 + (bit < 16 ? 1 : 0)] |= (uint16_t)(1u << (bit & 15));
        irq_dirty = true;
    }
    uint32_t counter_now() const
    {
        return (uint32_t)(*insn_src * counter_num / counter_den);
    }
    bool in_counter(unsigned o) const
    {
        return insn_src && o >= counter_off && o < counter_off + 4;
    }
    bool in_adc(unsigned o) const
    {
        return adc_off && o >= adc_off && o < adc_off + 2;
    }
};

uint32_t pic2000_probe_read(void *, uint32_t, unsigned);
void pic2000_probe_write(void *, uint32_t, unsigned, uint32_t);
void pic2000_probe_report(const Pic2000Registers *, FILE *);
