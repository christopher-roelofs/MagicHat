# Accuracy: what is modelled, what is not

This document is about the DataRover 840. The 68k machines keep their own
account of what is measured and what is assumed; see
[PIC2000.md](PIC2000.md) and [68K_PARITY.md](68K_PARITY.md).

## Verified by the machine itself

`scripts/regress` walks cold ROM to the Magic Cap Desk and checks each
milestone against a recorded fingerprint. Most of those checks are the
machine measuring itself rather than us asserting anything.

Booting to the IDT monitor prompt cross-checks a good deal of the model,
because the monitor measures the hardware and prints what it found:

```
Memory: 4194304 (0x400000), Icache: 4096 (0x1000), Dcache: 1024 (0x400)
Toshiba Core - id: 0x2200
Platform: Apollo
```

- `4194304` proves nothing about the model. It is a literal in the ROM
  (`lui v0,0x40` at `0x83C002A0`), not a measurement the machine makes, so
  it holds whatever the ROM says regardless of the DRAM actually mapped; see
  [HARDWARE.md](HARDWARE.md).
- `4096` / `1024` match the cache-invalidation loops' extents.
- `0x2200` is compared against a literal in the ROM at `0x83C005E0`.

Reaching that point takes ~800 million instructions with **zero
exceptions**, which means no unimplemented instruction, no unmapped access,
and no mistranslated address along the way. Carrying on into Magic Cap adds
three more framebuffer fingerprints -- the boot screen, the touchscreen
calibration target, and the Desk -- plus a check that interrupts are
actually being delivered, which would catch an ICU wiring bug immediately.

Two further things the machine tells us it is happy about: the monitor's
`touch_init` programs eleven UCB1100 registers and reads every one back at
its expected value, and at the Desk there are zero accesses to TX39
registers we do not decode.

The CPU core additionally has semantics tests (`tests/cpu/test_r3900.c`)
covering delay-slot bookkeeping, link-register values, big-endian access,
`LWL`/`LWR`/`SWL`/`SWR`, EPC when a fault occurs in a delay slot, overflow,
divide-by-zero, interrupt gating, `RFE`, and the reserved-instruction and
coprocessor-unusable traps. The same suite runs against every CPU engine,
with differential tests comparing the native engine to the reference
interpreter; see [MIPS_JIT.md](MIPS_JIT.md).

Two of those tests caught real bugs while being written: the link register
was one instruction too high, and interrupts were being delivered with a
stale `cur_pc`, which would have put EPC on the wrong instruction.

## Speed

The TMPR3902U ran at 36.864 MHz, and the model retires one instruction per
cycle. The reference interpreter runs at roughly real time on a current
desktop core; the native engine, the default on x86-64 and AArch64, is
several times faster. Measured figures are in
[MIPS_PERFORMANCE.md](MIPS_PERFORMANCE.md).

## Not modelled

**Caches.** No instruction or data cache. `CACHE` is decoded and retired as
a no-op, which is accurate for a machine with no caches; the count is
reported. `Status.IsC`/`SwC` cache-isolation tricks are stored but do
nothing.

**Cycle timing.** Every instruction takes one cycle. See the cycle-timing
entry in [OPEN_QUESTIONS.md](OPEN_QUESTIONS.md).

**Load delay slots.** The R3900 has register scoreboarding, so unlike the
R3000A it interlocks loads and there is no architectural load delay slot.
We model none. If this is wrong the ROM will misbehave in ways that look
random.

**Write buffer.** Stores complete immediately and in order.

**LCD scanout.** The framebuffer is read out of DRAM when something asks
for it (the window, `--dump-fb`, a screenshot); there is no display timing,
no vsync, and no LCD interrupt.

**Audio and telecom.** SIB transmit DMA feeds a UCB1100 output model with
12-bit decoding, mute, attenuation and approximate reconstruction filtering.
Telecom and audio capture are not modelled. See the Sound section and
[AUDIO.md](AUDIO.md).

**Power.** Battery and charger electrical behaviour remain incomplete. F4
drives ONBUTN, its edge interrupts and the supply wake input; the guest's
normal power-off request suspends CPU execution while retaining RAM.
General STOPCPU/doze behaviour and other wake sources remain incomplete.
See [POWER_PERSISTENCE.md](POWER_PERSISTENCE.md).

**PC Cards.** The Glacier controllers model card detect, the interrupt
latches and the windows the ROM uses. SRAM storage cards, an NE2000
Ethernet card and a data modem card are modelled well enough for the stock
drivers; power/reset timing and full window decoding are not. See
[STORAGE.md](STORAGE.md) and [NETWORKING.md](NETWORKING.md).

## Deviations

Exactly one, and it is off by default: `--force-mmu` routes kuseg and
kseg2/kseg3 through a TLB this part does not have. It exists so the
evidence for the no-MMU default can be re-tested. `--show-deviations`
documents it.

There are no ROM patches, forced dispatches, or synthesised call frames,
and adding any to make stock functionality work would invalidate everything
above. See [METHODOLOGY.md](METHODOLOGY.md).

Everything in this document describes a stock DataRover 840 running an
unmodified ROM. Deliberate modifications -- extra RAM, code in the free
flash, features the device never had -- are a separate track with its own
rules, and are documented in [MODIFICATIONS.md](MODIFICATIONS.md) so they
are never mistaken for the real machine's behaviour. Nothing here applies to
a modified image.

## Sound

The SIB's transmit DMA is modelled and wired to the host speaker. Magic Cap
programs `SIBSNDTXSTART = 0x403ED638`, sets `ENSIB|ENSF0|ENSND` in `SIBCTRL`
and `ENDMATXSND` in `SIBDMACTRL`, and we walk the ring handing 16-bit
big-endian samples to the codec output model, raising `SND0_5INT` halfway
and `SND1_0INT|SNDDMACNTINT` on wrap.

Observed ring refills support the current 4 KiB buffer and half/wrap
interrupt implementation for this ROM. They do not establish full DMA or
codec accuracy. The idle buffer contains -768; this is a guest PCM offset,
not a verified analogue output voltage or proof of codec behaviour.

The ROM's usual SNDFSDIV=25 gives 11076.923 Hz under the assumed clock; the
API truncates that to 11076 Hz. NetBSD's TX3912 driver and the Philips codec
formula support this rate with SIBCLK=9.216 MHz. However, the calculation
ignores SCLKDIV and the codec's independent divider, and SIB control frames
are not clocked at their real frequency.

The SDL frontend opens at the first delivered sample, follows rate changes,
and queues two device periods of startup silence. Sound DMA runs with or
without a host sink. The codec decodes 12-bit samples and applies
attenuation, mute and DAC enable, and approximates its published filter
response at twice the guest sample rate. Exact internal filter coefficients
and some attenuation encodings remain unverified. See [AUDIO.md](AUDIO.md)
for evidence and limits.

## Input

The pen is modelled from the panel's resistance, including the MFIO
pen-down level the ROM requires before it dispatches a touch. The Option
key is TX39 IO3, active low, established from the ROM's own query; see
[HARDWARE.md](HARDWARE.md). The Magic Bus keyboard is described in
[KEYBOARD.md](KEYBOARD.md).

The power button is available through F4 (`PWRCTRL_ONBUTN` and button-edge
interrupts). Normal shutdown and wake retain the CPU context and RAM. Other
front-panel controls beyond these inputs have not been established.
