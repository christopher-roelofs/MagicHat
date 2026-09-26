# DataRover reset and automatic startup

## Current behaviour

`./build/mhat --rom roms/MagicCap-USA.image` starts at architectural reset
PC `0xBFC00000` and lets the ROM start Magic Cap. No saved state, injected
`go -c`, fixed input delay, or ROM patch is required.

Use `--monitor` for the IDT prompt. It selects the other state of the
board's boot-select input; it does not redirect the PC or alter firmware
instructions. `--reset-pc` remains available as a diagnostic override. Boot
selection applies to fresh starts; loading a snapshot restores the saved
GPIO/register state.

## Reset alias

The first ROM word is `08F00007`. A MIPS J instruction retains the upper
four bits of PC+4. Therefore its target depends on where it executes:

| Execution address | Jump target | Physical target |
|---|---|---|
| `83C00000` (PC override) | `83C0001C` | `03C0001C` |
| `BFC00000` (reset) | `B3C0001C` | `13C0001C` |

Both targets refer to existing flash aliases. The claim that this word
could only execute from `83C00000` was incorrect. The board exposes flash at
physical `1FC00000..1FFFFFFF` as well, sharing backing storage with the
other aliases. This lets reset fetch the existing jump; there is no
synthesized first-stage stub.

**Evidence limit:** this boot alias is inferred from the CPU reset vector,
ROM instruction and successful execution. Its full decode extent and the
TMPR3902U/board chip-select circuitry have not been verified against a
board schematic. We model a 4 MiB reset window, not a recovered complete
decoder.

## The boot-select input

The ROM's `83C040C4` routine takes the Apollo path at `83C04120` and reads
TX39 IOCTRL (`B0C00180`). It returns `((IOCTRL >> 3) ^ 1) & 1`.

- `83C00068..84`: return zero sets RAM `0000C140` to -1; nonzero leaves it
  zero.
- `83C09F30`: initializes the monitor, but is also responsible for
  auto-start.
- `83C0A174..B4`: nonzero `C140` formats `g 0x%x`, with entry `13C1D120`,
  and calls the command dispatcher at `83C0A6D8`.
- `13C1D120`: Magic Cap's entry initializes its stack and falls into the OS
  startup at `13C1D12C`. It does not return normally to the monitor prompt.

Thus IO3 **high** selects automatic startup; IO3 **low** selects the
monitor. The unconditional jump to `83C000F4` after initialization only
matters if initialization returns.

NetBSD's TX39 IO definitions (`tx39ioreg.h`) identify IOCTRL bits 6:0 as
IODIN, with separate output and direction fields. A zero-initialized
read/write register would present IO3 low and always stop at the monitor.
The board defaults IO3 high; `--monitor` drives it low. CPU register writes
preserve IODIN rather than overwriting external pin levels. The same pin is
the Option key (see [HARDWARE.md](HARDWARE.md)).

**Evidence limit:** the ROM establishes this input's software function and
polarity. Whether the physical source is a pull-up, jumper, connector
signal or another circuit remains unknown. Other GPIO electrical behaviour,
direction and output feedback are not comprehensively modelled.

## Firmware comparison

The archive's decompressed `DataRover840FRomFlasher.gz` is 8,388,608 bytes.
It contains the shipping USA image (`DataRover-840-USA.image`) verbatim at
offset `0x400`, length 4,528,151:

SHA-256: `94785cb334f14eac00ed200af014c35972b4f25694103bc6a49b3afa280a6f1b`

The preceding 1 KiB is a `BowserLives` card header and padding, not a
missing reset stub (the last nonzero header byte is at offset 27). The
header's image address is `B3C00000`. There is no separate production
payload in this card that needs to be substituted for the image.

The Japanese archive image has the same initial reset jump and identical
boot-select routine at `40C4..4147`; its full startup has not been
validated.

## Validation

- A synthetic-ROM test executes the real reset jump and boot-pin read from
  `BFC00000`, checks both input levels and read-only IODIN, and verifies
  shared flash backing across aliases.
- Fresh USA ROM execution without any input or reset override reaches the
  480x320 "Touch the screen to begin" screen within 2 billion execution
  slots. UART A and B transmit counts are both zero on this path.
- `--monitor` is separately exercised from architectural reset to retain
  the debug workflow. `scripts/mkstates` and `scripts/regress` select it
  explicitly when they need the intermediate monitor state.

These bounds are emulator execution counts, not measured physical boot
times. CPU and peripheral timing still have the limitations in
[ACCURACY.md](ACCURACY.md).
