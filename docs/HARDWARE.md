# DataRover 840 hardware

Board codename "Apollo" -- the IDT monitor prints it. Everything below is
either confirmed by the ROM's own behaviour under emulation or cited to a
source; anything unresolved is in [OPEN_QUESTIONS.md](OPEN_QUESTIONS.md).

## Chips

| Codename | Part | Role |
|---|---|---|
| Dino | Toshiba TMPR3902U | CPU, R3900 core, 36.864 MHz |
| Betty | Philips UCB1100BE | audio + telecom codec, touchscreen ADC |
| Glacier | GMI-Japan custom | PC Card controller, two of them |

The TMPR3902U is a custom TX39-family part built for General Magic. That
matters in both directions: the standard TX39 peripheral block is present
and is documented, but the part also has blocks the TX3912 does not.

## CPU

R3900: MIPS-I instruction set, 32-bit, big-endian, plus the `CACHE`
instruction (opcode 0x2F). 4 KB instruction cache, 1 KB data cache -- the
monitor prints both, and the ROM's cache-invalidation loops at `0x83C008AC`
and `0x83C008D4` walk exactly those sizes in 16-byte lines.

**No TLB.** Established two ways, both re-checkable at run time:

- The ROM never executes a TLB instruction. The exit summary reports this.
- Within a few hundred instructions of reset it stores to kuseg
  `0x0000C1BC` (ROM `0x83C004EC`) and kseg3 `0xFF000010` (ROM
  `0x83C00568`), neither of which could work through a TLB nothing has
  programmed.

So kseg0/kseg1 mask off the top three bits as always, and kuseg and
kseg2/kseg3 map straight through. `--force-mmu` re-tests this.

Other CP0 details that differ from the R4000 and that the ROM depends on:

- **Config is CP0 $3**, not $16. ROM `0x83C00330` sets bit 11 of `$3` and
  `0x83C00358` clears bits 11:10; both are cache-enable controls.
- **Status.PE (bit 20) is a write-one-to-clear hardware latch**, not a
  stored bit. The ROM's second instruction writes `0x00100000` to Status to
  clear it. Storing that bit verbatim makes the monitor print "Parity ERROR
  detected" after every character it transmits.
- **PRId reads 0x2200.** This is not just what the monitor prints back at
  us: ROM `0x83C005E0` compares the saved PRId against the literal `0x2200`
  and takes a different path if it does not match.

## Memory map (physical)

| Range | What |
|---|---|
| `0x00000000`-`0x03BFFFFF` | DRAM chip select. 4 MB fitted; mirrors through the rest. |
| `0x03C00000`+ | ROM, 8 MB window |
| `0x10400000` | Glacier PC Card controller, slot 0 |
| `0x10800000` | Glacier PC Card controller, slot 1 |
| `0x10C00000` | TX39 on-chip peripheral registers, 1 KB |
| `0x13C00000`+ | ROM again, second chip select |
| `0x1FC00000`-`0x1FFFFFFF` | reset alias of the flash (see [STARTUP.md](STARTUP.md)) |
| `0x24000000` | firmware-update image probe; empty on a stock machine |
| `0xFF000000` | unidentified chip (see [OPEN_QUESTIONS.md](OPEN_QUESTIONS.md)) |

The DRAM mirrors within its chip select, as the board does with only 4 MB
fitted across a 60 MB decode.

It does **not** follow that the ROM discovers the size that way. It does
not discover it at all: the monitor's `Memory: 4194304 (0x400000)` comes
from a literal, `lui v0,0x40` at `0x83C002A0`, stored to `0x0000C180` and
read back by the print at `0x83C0A054`. Magic Cap keeps its own figure,
likewise hardcoded -- a board-ID to size table at `0x83C1EC80`, where
`0x83C25C24` returns a constant 5 and 5 means 4 MB.

Established by measurement: exposing more DRAM with a stock ROM still
prints 4 MB, and patching those constants makes the monitor print 8 MB and
Magic Cap allocate its framebuffer above `0x00400000`. There is no
pattern-and-alias sizing loop. See [MODIFICATIONS.md](MODIFICATIONS.md).

The ROM answers at two addresses. `0x03C00000` is where its own code
expects to run: the image's first instruction is `j 0x83C0001C`, which
resolves inside kseg0 based at `0x83C00000`. `0x13C00000` is where the
DataRover840F flasher writes, per its header's load address of
`0xB3C00000`. The OS runs from the second one -- poll sites appear at
addresses like `0x13C06B34`.

## TX39 peripheral registers

The block sits at exactly `TX39_SYSADDR_CONFIG_REG` (`0x10C00000`, kseg1
`0xB0C00000`) from NetBSD's hpcmips port, so `sys/arch/hpcmips/tx/*reg.h`
is authoritative for it. MagicHat's derived header is
`src/soc/tx39/tx39_regs.h`.

| Offsets | Block |
|---|---|
| `0x000`-`0x024` | BIU memory / chip-select configuration |
| `0x028`-`0x05C` | LCD controller |
| `0x060`-`0x090` | SIB (the bus to the UCB1100) |
| `0x0A0`-`0x0A8` | IrDA |
| `0x0B0`-`0x0C4` | UART A -- the debug port and monitor console |
| `0x0C8`-`0x0DC` | UART B |
| `0x0E0`-`0x0FC` | **MBUS** -- not in the TX3912 documentation |
| `0x100`-`0x12C` | interrupt controller |
| `0x140`-`0x154` | RTC, alarm, periodic timer |
| `0x160`-`0x164` | SPI |
| `0x180`-`0x198` | GPIO / MFIO |
| `0x1C0` | clock control |
| `0x1C4` | power control |

### Interrupts

ICU banks 1-5 drive CPU interrupt line **IP2**; bank 6 (IRQHIGH/IRQLOW)
drives **IP4**. From NetBSD `tx39icu.c`, which dispatches on
`MIPS_INT_MASK_2` and `MIPS_INT_MASK_4`.

Status banks 1-5 are write-one-to-clear at the same offsets as the status
reads. Bank 6 is read-only.

### UART

Transmit-ready and transmit-empty are **level** conditions, not edges:
while the UART is enabled and its holding register is empty, both stand,
and they re-assert immediately after software clears them.

This is what gets the machine through reset. ROM `0x83C011F0` is a generic
"wait for UART interrupt N, with a 1000-iteration timeout" helper; its mask
table at ROM offset `0x178C8` selects UART A's TX bit (`0x04000000`) for
the first call. The ROM enables the UART and waits for that bit without
ever writing the holding register, which only works if the level asserts on
its own.

### SIB and the UCB1100

Subframe 0 is the codec's control-register channel and behaves as an
indexed register port:

```
write: SIBSF0CTRL = (regaddr << 27) | SIBSF0_WRITE | data
read:  SIBSF0CTRL = (regaddr << 27)
       poll SIBSF0STAT until its regaddr field echoes back
       take data from its low 16 bits
```

From NetBSD `txsibsf0_reg_read` / `txsibsf0_reg_write`. Earlier
reverse-engineering called this bus "MBUS" and derived the same
`0x080`/`0x088` layout by observation, which cross-checks both -- but the
name was wrong, and the real MBUS is a different block entirely (below).

### The ROM names the codec's registers itself

The UCB1100 register map came from the Linux and NetBSD drivers for that
part. The ROM confirms it independently: a diagnostic in it prints
registers by name, and the names line up with the numbers we use.

| ours | the ROM's | reg |
|---|---|---|
| `IO_DATA` | `IOData` | 0 |
| `IO_DIR` | `IODir` | 1 |
| `IE_RIS` | `PosIntEn` | 2 |
| `IE_FAL` | `NegIntEn` | 3 |
| `TC_A`, `TC_B` | `TelecomCfgA`, `TelecomCfgB` | 5, 6 |
| `AC_A`, `AC_B` | `SoundCfgA`, `SoundCfgB` | 7, 8 |
| `TS_CR` | `TouchCfg` | 9 |
| `ADC_CR` | `AdcCfg` | 10 |

Ten of the fourteen, in our order. It also settles the edge registers as
positive- and negative-edge enables.

The strings are all of the form `IOData readback: 0x%x (0x%x)` -- a value
and something to compare it against -- so there is a self-test in there
that writes a register and reads it back. Finding how to run it would give
a second opinion on the whole codec model.

### Option key and codec GPIO

The DataRover Option key is **TX39 IO3, active low**. ROM `13C268E0` reads
`IOCTRL` at `B0C00180`, shifts by 3, and inverts the low bit. The
higher-level Option query (`13C64CD8` / global selector `005B`) reaches that
hardware read when there is no current input event. Card insertion uses
this query to decide whether to force reformatting. IO3 high also selects
normal automatic startup; holding Option at reset selects the monitor path
described in [STARTUP.md](STARTUP.md).

In the window, the right mouse button and the rail's Option button drive
this board input. Headless runs use `--option-key "1,10000000;0,200000000"`.
Times are relative instruction counts. The frontend releases Option on
focus loss and at startup.

Earlier experiments attributed Option to UCB1100 GPIO8 because driving it
changed subsequent touch behaviour. That did not establish the wiring:
driving GPIO8 does **not** make the ROM's direct Option query return true.
Its board role and the previously observed touch effect remain unresolved.
`--codec-gpio` remains available for raw codec-input experiments; it is not
the Option-key interface.

### MBUS

`0x0E0`-`0x0FC`. The TX39 interrupt controller documents a full set of MBUS
sources, but NetBSD has no MBUS driver or register header because the
machines it supports do not wire it up. The DataRover does.

What the ROM does with it, at `0x83C06B08`:

```
poll  0x0E0 until bit 31 clears          (busy)
write 0x0E4 = 0x64080000                 (data)
write INTRCLEAR2 = 0x800                 (MBUSTXBUFAVAIL)
write 0x0E0 = 0x000008A3                 (command; starts the transfer)
poll  INTRSTATUS2 until 0x800 sets       (completion)
write 0x0F4 = <16-bit value>
```

`INTRSTATUS2` bit `0x800` is `MBUSTXBUFAVAILINT` in the NetBSD ICU header,
which is what identifies the block. Boot blocks here for tens of millions
of instructions until the completion source asserts.

The runtime accessory driver also reads `0x0E0` bit 29 through
`0x83C28364`. In discovery at `0x83C2A794`, high ends the scan with no
devices; low enters enumeration. An empty bus is therefore modelled with
this input high, independent of command-register writes. Returning zero
caused spurious accessory errors and a 60-second retry cycle that also
toggled UART A. This polarity is inferred from the ROM, not a register
datasheet. A populated bus -- the Magic Bus keyboard -- is described in
[KEYBOARD.md](KEYBOARD.md).

## Sources

- NetBSD `sys/arch/hpcmips/tx/` -- register headers and drivers for the TX39
  peripheral block.
- Old Vintage Computing Research, "Magic Cap, from the Magic Link to the
  DataRover" -- the TMPR3902U identification and board teardown.
- Philips UCB1300 datasheet -- pin- and register-compatible with the
  UCB1100.
- The ROM itself, cited by address throughout.

## Starting Magic Cap

The reset path initializes the IDT monitor, which can launch Magic Cap
before returning to its command loop. IOCTRL.IODIN[3] high selects automatic
startup; low selects the prompt (`--monitor`). The ROM dispatches
`g 0x13C1D120` itself on the automatic path. Manual `go -c` remains
available at the monitor. The reset flash alias allows architectural
`BFC00000` startup without a PC override. See [STARTUP.md](STARTUP.md) for
evidence and remaining wiring limits.

Things Magic Cap needs that the monitor alone does not:

- **SIB sound input.** `0x83C231B4` waits on `INTRSTATUS1.SNDININT` and
  does not return without it. The SIB carries a sample every frame while
  the sound channel is enabled, so the source is periodic like the subframe
  sources.
- **No user/kernel segmentation enforcement.** Magic Cap runs with
  `Status.KUc` set and still reads kseg1 MMIO (the RTC, from `0x13CBF994`).
  On a part with an MMU that would be an address error; this part has no
  MMU, and the segmentation check is the same hardware as the translation it
  guards.
- **All four PC Card windows decoded.** The card probe at `0x13C34288` reads
  bytes at `0x08000000` and `0x28000000`; unmapped, those are bus errors.

With those, Magic Cap boots far enough to program the LCD controller --
480x320 at 2 bpp with the framebuffer at `0x003F6A00`, all read out of the
registers it writes rather than assumed -- and render the General Magic
boot rabbit.

### `break 0xf` at 0x83DDA3C0 is not a panic

Earlier emulation attempts patched this out as a "panic stub bypass". It is
`break 0xf` followed immediately by `jr ra`, and it is reached by `jal` -- a
service-call trampoline into the monitor, which the banner tells us is a
"Hosted Version". Treating it as a panic and skipping it discards whatever
service was being requested.

## Touchscreen

The panel is four-wire resistive, read through the UCB1100. Two things
about it are easy to get backwards, and both are settled by watching what
the ROM does at `0x13C25CBC`:

**A touch arrives as an interrupt, not a poll.** With `TS_CR`'s mode field
at 0 the panel sits biased and idle; a pen pulls TSPX down, which is the
codec's TSPX interrupt source. Software enables it on the *falling* edge
(`IE_FAL = 0x1100`). The codec's IRQ pin reaches the host through the SIB,
as a level in `SIBCTRL.SIBIRQ` and as edges in `INTRSTATUS1`'s
`SIBIRQPOSINT` / `SIBIRQNEGINT`.

**The axis you are measuring is the one being driven, not the one the ADC
is looking at.** Two resistive layers touch at a point; you put a gradient
across one and measure the other, which floats to the potential at the
contact. The ROM takes four readings per sample:

| `TS_CR` | plates | mode | ADC input | reads |
|---|---|---|---|---|
| `0x0982` | TSPX_POW, TSPY_GND | pressure | TSPX | cross-driven |
| `0x0918` | TSPY_POW, TSMX_GND | pressure | TSPY | cross-driven |
| `0x0A12` | TSPX_POW, TSMX_GND | position | TSMY | **X** |
| `0x0A48` | TSPY_POW, TSMY_GND | position | TSMX | **Y** |

Note the last two: the X coordinate is read on a Y plate and vice versa.
Selecting the reported axis from the ADC input field instead of from
`TS_CR` gets X and Y exactly swapped.

**The ADC has four auxiliary channels beyond the panel.** The input-select
field is three bits, and inputs 4..7 are the UCB1x00's AD0..AD3. On this
board they carry voltages: the OS selects input 7 continuously while idle,
and code at ROM `0x83C24C84` tests for input 6 and, when it sees it, clears
bit 0 of the codec's `IO_DATA` GPIO before converting -- which looks like
switching a divider or reference in for that measurement.

**Magic Cap draws a battery gauge in its title bar, and it tracks AD2
live.** That makes it by far the best instrument for anything to do with
the main battery: it is continuous rather than a set of discrete dialogs,
it is always on screen, and it updates without a reboot, so a sample costs
seconds from a booted Desk instead of a full boot.

Its travel, swept against AD2: empty at 600 and below, a sliver at 650,
about a third at 700, most of the way at 800, full from 900.

**It repaints only when the OS samples**, which is roughly every eight
seconds of emulated time. Changing AD2 does not redraw it -- so from a
snapshot the gauge shows whatever was painted when that snapshot was taken,
for several seconds, before catching up. That is worth knowing before
concluding a change had no effect.

**AD2 (input 6) is the main battery and AD3 (input 7) the backup.** Left at
zero both read flat and Magic Cap says so at startup; driving them
(`--aux-adc`) over the whole boot clears the dialogs. In the window the
host's own battery is reported through these channels unless
`--no-host-battery` is given.

The sense is graded, not a single threshold, and the OS's own wording
tracks it. With the main battery healthy, the backup reports as:

| AD3 | reported |
|---|---|
| 0 | "out of power **or missing**" |
| 200 | "**completely** out of power" |
| 600 | healthy, no dialog |

Reading zero as *missing* rather than merely flat is exactly right for a
divider with no cell across it, and the ROM's string table carries "almost
out of power", "completely out of power" and "out of power or missing" as
three separate messages. See [OPEN_QUESTIONS.md](OPEN_QUESTIONS.md) for
where the boundaries sit.

**Pen-down is also a GPIO.** MFIO pin 13 carries the panel's pen-down sense
as a level, separately from the codec's interrupt. The ROM reads it three
ways and they agree: `0x83C2645C` returns `(MFIODATAIN & 0x2000) != 0` as a
predicate, `0x83C2639C` samples it twice with dispatches in between and
compares (a debounce, which only makes sense for a level), and `0x83C26760`
shifts it down by 13 and dispatches the pen event -- selector 0x49 -- only
if it is set. `IOMFIODATADIR` is programmed to `0xF607D002` at reset, whose
bit 13 is clear, so the pin is an input.

This is the difference between the OS noticing a touch and acting on one.
Without it the driver samples coordinates correctly and then drops them,
because `0x83C26760` reads zero and returns without dispatching.

**The OS stores its calibration where we can read it.** After the three
targets it keeps the derived constants at `RAM[0xEB72]`, `[0xEB76]`,
`[0xEB7A]`, `[0xEB7E]` and `[0xEB82]`. With the geometry above they come
out as 83, 67, 836 and 791 -- the raw readings the OS has worked out for
the screen edges, matching `PANEL_RAW_X0/X1/Y0/Y1` (85, 836, 69, 791) to
within the rounding of its own fit. The mapping round-trips, which is a
useful check that the calibration is doing what we think.

The last touch's raw coordinates sit next door at `[0xEBA4]`..`[0xEBAA]`.

The same trace settles the ADC control layout: `ADC_CR` is written as
`0xD001`, `0xD009`, `0xD00D`, `0xD005`, each immediately repeated with bit 7
added. Bit 7 is the conversion start, bit 15 is the enable, and the
input-select field is **bits 3:2** -- those four values give 0, 2, 3, 1,
the four plates each used once.

## PC Card slots

Two Glacier controllers, register-identical, at `0x10400000` and
`0x10800000`. The reset code programs both with the same sequence at
`0x83C003F0`..`0x83C00468`, differing only in the value written to `+0x20`
(0 and 1). This does not establish a slot-number register: the IRQ handler
also toggles its low two bits, and odd-byte card cycles toggle bit 3.

**Register `+0x0C` is slot status, and bits 10 and 11 are the two
card-detect lines.** The ROM says so at `0x83C33844`: it points a debounce
helper at slot 0's `+0x0C` and calls it three times, masking `0x0400`, then
`0x0800`, then both together, comparing the two single-pin results. Testing
two detect pins separately and then jointly is how a PCMCIA host tells a
fully seated card from a partly inserted one.

Card detect is active low -- a card grounds the pins -- so an empty slot
reads them **high**. Returning zero presents two pins pulled down with
nothing behind them, and the debounce never settles: it polls the register
195,000 times over 120M instructions instead of 65,000.

The networking investigation also established interrupt-enable words at
`+0x10..0x16` paired with pending/W1C words at `+0x18..0x1E`. The ROM's
shared handler at `0x13C1FFEC` acknowledges TX39 ICU bank 3 bit 2 and
dispatches those enabled pending bits from both controllers. Modelling these
latches makes live insertion reach the ROM's debounce and CIS-reading
routines. The ROM checks readiness on status bit 3; the NIC driver enables
falling bit 2 for I/O-card interrupts and masks bit 3. No Glacier datasheet
is available; power/reset timing and complete window decoding remain
unverified. CIS and configured NE2000 I/O both use window A in the measured
OS path, with I/O at offsets `0x300..0x31F`. See
[NETWORKING.md](NETWORKING.md) and [STORAGE.md](STORAGE.md).
