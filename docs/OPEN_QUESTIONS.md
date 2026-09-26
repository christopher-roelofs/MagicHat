# Open questions

Things the emulator guesses at, or knows it does not know. Each is a place
where a wrong answer could produce a misleading result, so each is written
down rather than left implicit in the code. Addresses are DataRover 840 ROM
addresses unless a PIC-2000 address is given; the `83C...` and `13C...`
forms are the same code through two aliases.

## Known unknowns

### Reset window decode and boot-select source

Automatic startup works: the first jump at `BFC00000` targets `B3C0001C`,
and TX39 IOCTRL input 3 high lets the ROM launch Magic Cap (low selects the
IDT monitor). The exact extent of the reset alias and the physical source of
IO3 are inferred from the working ROM path, not measured. See
[STARTUP.md](STARTUP.md).

### The device at 0xFF000000

The reset code at `0x83C00560` writes a control word at `+0x10`, zeroes
eight 16-byte entries at `+0x20`..`+0x90`, then writes `0x100` to `+0x10`.
Eight 16-byte entries look like a descriptor table (DMA channels or bank
configuration). It is modelled as a counted register file so it cannot
silently become important. Next step: find code that reads the entries
back.

### PC Card windows and Glacier controller semantics

Each slot has two windows 64 MB apart: `0x08000000`/`0x0C000000` (window A)
and `0x24000000`/`0x28000000` (window B). The ROM reads attribute CIS
tuples from window A at even byte addresses, and configured NE2000 I/O also
reaches window A at `0x300..0x31F`. The monitor's firmware-update check at
`0x83C06F70` looks for the `"BowserLives"` magic that heads the 840F
flasher file at `0x24000000`, which is consistent with window B being
card 1. Controller-dependent space selection and aliases are not fully
mapped.

The two Glacier controllers at `0x10400000` and `0x10800000` have no
datasheet. Known: status `+0x0C` bits 10/11 are the card-detect lines
(active low), bit 3 is write-protect for SRAM cards, bit 2 readiness,
bit 1 part of the card battery input; `+0x10..0x16` enable the W1C pending
bits at `+0x18..0x1E`; NIC drivers enable falling bit 2 and mask bit 3.
Unknown: power/reset timing, the routing controls at `+0x20` (bit 3
toggles around odd-byte I/O at `0x13C34A50`/`0x13C34B34`), and full
memory/I/O window selection. Undecoded registers are stored and counted.
See [NETWORKING.md](NETWORKING.md) and [STORAGE.md](STORAGE.md).

### MBUS with a real accessory

The empty bus reports MBUSCTRL bit 29 high, which the discovery check at
`0x83C2A794` reads as "nothing attached" (this removed a spurious "A
problem happened while using an attached device" dialog; see below). The
Magic Bus keyboard is implemented on top of the SDK-identified DMA and
command registers. The input polarity is inferred from the ROM; electrical
behaviour with other accessories, `MBUSDET` as a physical attach signal,
precise wire timing and guest-controlled typematic are open. See
[KEYBOARD.md](KEYBOARD.md).

### Unnamed TX39 registers

`0x0E8`, `0x0EC`, `0x0F0`, `0x0F8`, `0x0FC` sit in the MBUS block but have
no known roles. `0x1D8` and `0x1F0` are read on every pass of the idle
power scan (`0x13C3A17C`, `0x13C3A14C`) and never written; the OS proceeds
with zero. The whole `0x1C8..0x1FC` block is stored and returned. These
are the parts of this TX39 variant that the TX3912 documentation does not
cover.

### LCD interrupts

INTRSTATUS1 has `LCDINT` (bit 31) and `DFINT` (bit 30). Neither is raised,
because nothing establishes what triggers them. An earlier conclusion that
this SoC has no LCD interrupt was reached without the register map and
should not be repeated.

### Cycle timing and idle power states

There is no cycle model. CP0 Count ticks once per two instructions, which
is a placeholder; there is no cache, memory latency or multiply timing. The
RTC and timers run from the instruction count against a declared
36.864 MHz, so timer rates are self-consistent. See
[ACCURACY.md](ACCURACY.md).

`POWERCTRL.STOPCPU` is not implemented. At idle the OS calls `0x83C3B28C`,
which walks DRAM to refresh it and then sets STOPCPU to halt until an
interrupt; the emulator continues, so the caller re-enters (about 32K
instructions per pass). The stop timer interrupt (`STPTIMERINT`, bank 5
bit 28) is enabled but never cleared by the OS, which on hardware implies
it is not set in this state. Implementing STOPCPU needs the wake sources for
a stopped CPU established by measurement first; two plausible models (a
pending-summary IRQLOW that never cleared, and halt-until-interrupt) were
wrong. Ordinary power-button shutdown is modelled separately; see
[POWER_PERSISTENCE.md](POWER_PERSISTENCE.md).

### UCB1100 device ID and touch pressure

The monitor's `touch_init` prints `bettyID = 0x0` because the real ID
register value is unknown (Linux knows `0x1004` for the UCB1200 and
`0x1005` for the UCB1300). Nothing has depended on it.

The ROM accepts a sample set only if all four cross-driven readings are at
or below `RAM[0xEB84]` (500 in the running system; `0x83C665D0`). The model
reports a steady value under that while the pen is down. Real panel
pressure behaviour is unmeasured; if the OS ever rejects a touch as too
light or too heavy, look here first.

### The `break 0xf` service call at 0x83DDA3C0

A service-call trampoline into the "Hosted Version" monitor, called once
during startup with `v0 = 2`, `a0` pointing at a stack word holding 2, and
`a1 = 4`. No service is implemented; the monitor's own handler takes the
exception. What service 2 is, and whether anything depends on it, is
unknown.

### What drives CPU interrupt line IP6

ICU banks 1-5 drive IP4 on this board (`0x83C25678` decodes Cause bit
`0x1000`, then walks five banks from INTRSTATUS1), unlike the TX3912
machines NetBSD supports, where they drive IP2. Magic Cap enables IP6, and
its handler dispatches a single callback from `RAM[0xE4D8]` without reading
the ICU. The emulator routes ICU bank 6 (IRQHIGH/IRQLOW) there because
those names read like external IRQ pins. Next step: find what installs the
callback.

### Main battery (AD2) ladder

AD2's low-battery warning threshold is in (660, 690]: at 660 and below
Magic Cap offers to turn off illumination, communication and sound. Driving
AD2 at 400-600 during boot makes the guest do much more work (not a CPU
clock change: CLOCKCTRL writes are the same). Whether AD2 has further steps,
and what a fresh cell reads, are unknown; the default is a choice inside the
healthy band.

### Codec GPIO8 touch effect

The Option key is TX39 IOCTRL input 3, active low (`13C64CD8` to
`13C268E0`). Earlier codec GPIO8 experiments, with an apparent 57 ms touch
threshold, did not measure Option and must not be used to add an Option
debounce. What GPIO8 does is open. See [HARDWARE.md](HARDWARE.md).

### Panel geometry

Pixel taps use a geometry fitted to three accepted calibration touches
(raw 85..836 across, 69..791 down). It is inside the ROM's own tolerance
but is a convention, not the ROM's default mapping, which is still unknown.

### Program storage

Magic Cap rewrites a placeholder gp-setup stub inside the ROM image
(image offset `0x296410`, written from `0x83C7D79C`), so the model lets
stores to the program image take effect. The 840 is documented with mask
ROM and the 840F with flash; NOR program/erase commands are not modelled
and it is not established that ordinary packages install into firmware.
See [POWER_PERSISTENCE.md](POWER_PERSISTENCE.md).

### PIC-2000 power ASIC and audio response

The dev21 power-control interpretation (zero written at `+D0` removes CPU
power) comes from the ROM's shutdown routine; the ASIC's identity, other
output encodings and physical RAM bank sizes are unknown. The dev21 audio
ASIC is also unidentified: the attenuation fields at `+56` are established,
their gain curve and routing are not, and the default speaker response is
an approximation. See [PIC2000_AUDIO.md](PIC2000_AUDIO.md).

## Resolved findings

- **Touch path.** MFIO pin 13 carries pen-down (`0x83C26760` drops samples
  without it), and the pressure readings must sit at or below 500. A
  calibration point needs roughly 30M instructions of hold; framebuffer
  pixel counts cannot distinguish calibration steps, hence
  `scripts/fbdiff`.
- **Battery inputs.** AD2 (input 6) is the main battery and AD3 (input 7)
  the backup; AD0/AD1 play no part. The ROM switches the AD2 divider in
  only while converting (`0x83C24C84`). AD3 ladder: 0 "out of power or
  missing"; 200-275 "completely out"; 290-325 "almost out"; 340 and above
  healthy. The backup check is gated behind a healthy main battery.
- **SRAM card battery.** `13C346CC` reads TX39 IO1 (slot 1) or IO0 (slot 2)
  and Glacier `+0C` bit 1; both high is a healthy card.
- **Empty-bus accessory warning.** It came from an MBUS probe failing on
  bit 29 low (`0x83C28364`, thrown at `0x83C2A9B4`, retried after
  60,000 ms at `0x83C29C78`, disturbing UART A). Reporting the empty bus
  removed it without ROM changes.
- **Touch lost after sound.** The codec interrupt edge check sat behind the
  SIB enable; the OS powers the SIB down at idle and waits for exactly that
  interrupt. The edge check now runs regardless. A press and release in one
  host event batch are also held apart by the tap-hold time.
- **Sound ring.** The ring is 4 KiB at about 11.077 kHz in the usual
  configuration and is refilled; earlier 32 KiB / 22050 Hz figures were
  wrong. See [AUDIO.md](AUDIO.md).
- **PC Link.** Beneath the `ChMa` frames is a transport of escaped bytes in
  blocks of at most 256, each with a CRC-32. `Cnct` needs two `Cntd`
  replies (the ROM waits twice, 10,000 ms then 20,000 ms, from
  `0x83E8207C`/`0x83E8208C`); with one, the second wait closed UART A
  mid-transfer at 37,953 bytes. Sessions end with `Abrt` then `GBye`, or
  the guest reports lost data. Guest-to-host `MPkg` backups are received by
  `scripts/pclink --receive DIR`.
- **Power-button shutdown.** DataRover suspends at `13C3B1C4` and resumes on
  ONBUTN; PIC-2000 powers off through dev21 `+D0`. See
  [POWER_PERSISTENCE.md](POWER_PERSISTENCE.md).
