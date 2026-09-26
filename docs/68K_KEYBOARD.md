# 68k Magic Bus keyboards

PIC-2000, Envoy **1.0 and pt4**, and HIX-300 now share an emulated Magic Bus
keyboard transport. GUI startup attaches it automatically. `--keyboard`
also attaches it headlessly; `--no-keyboard` disconnects it. It does not use
a card slot, UART, guest OS injection, or keyboard-related ROM patches.

| ROM | Keyboard status |
| --- | --- |
| PIC-2000.rom | Supported. |
| envoy-1.0.rom | Supported. |
| envoy-1.0-pt4.rom | Supported. |
| hix300-mc19-c2.rom | Supported with the board's existing checksum recovery. |
| envoy-1.0-mc31-b10.rom | Experimental core probe passes with AC attached; production attachment remains gated pending battery/power validation. |
| PIC-1000.rom | Gated; the damaged ROM does not boot, so the keyboard cannot be validated. |

## Board wiring

The controller and DMA live in `src/machines/pic2000/magicbus.cpp`; the AT
scan-code endpoint is shared with MIPS. Board selection comes from ROM content,
not the filename. Pin reads preserve output latches and neighboring inputs.

| Board | Accessory input | Falling/rising pending bits | Enable | IRQ |
| --- | --- | --- | --- | --- |
| PIC-2000 | `0C000002` bit 2 | `210000B2` bits 8/9 | `210000B6` | IPL5 |
| Envoy | `210000E7` bit 2 | `210000BC` bits 4/5 | `210000C4` | IPL5 |
| HIX-300 | `210000EE` bit 6 | `210000B2` bits 8/9 | `210000B6` | IPL5 |

All three use the dev21 shifter at `21000090..210000A0`, receive data at
`21000092`, transmit payload at `21000098`, and MC68349 DMA channel 2.
Envoy's extra interrupt bank must not be routed to IPL6. Its EconoRAM battery
pack remains on the separate E7 bit 3 input. HIX's previously investigated
`210000D3` bit 2 is **not** the Magic Bus input: the scheduler calls `0E0579A8`,
which samples EE bit 6.

Envoy 1.0/pt4 scheduler `00462E6C` programs the same `B6A5`/`FAA5` DMA modes
as PIC-2000. HIX scheduler `0E056CB6` uses `36A1`/`7AA1`, also word-to-word,
external-request transfers. It programs DMA vector 64 at IPL6 rather than
PIC/Envoy's IPL5; the emulator uses the programmed DMA interrupt register.

HIX's acceptance routine at `0E05BFEC` recognizes `ATKB`, not `MBKB`.
The emulated accessory therefore identifies itself as ATKB on HIX and MBKB
on PIC/Envoy, in both the ID response and descriptor checksum.

HIX's command routine constructs the usual eight-byte `K,count,command...`
buffer, but `0E056FEC..0E056FF4` primes a zero-extended byte before DMA starts
at buffer+2. The observed transmitted LED packet begins `00 4B ED 04`.
The HIX-only endpoint adapter accepts this legacy header for reset, LED, and
repeat commands and validates the remaining command bytes through the shared
endpoint. This is an emulation compatibility behavior inferred from the ROM,
not verified physical keyboard firmware. Guest source buffers are never repaired.

## Validation and remaining limits

The ROM probes observe actual guest attachment, requests, decoded `aBc`,
modifier release, and Caps Lock LED writes. Both the interpreter and JIT are
tested. SDL probes exercise real keydown/up and focus-loss events. These are
guest-decoder checks, not assertions about text in every application.

Five images, the four supported above and a patched HIX-300 variant, passed
ten fresh-boot core probes (JIT and interpreter for each) and five SDL
probes. Each core probe decoded `61,42,63`, serviced four keyboard requests,
and completed a Caps Lock LED write with no controller errors. Each SDL
probe decoded three characters and released Shift on focus loss. In this
repository, `ctest -R 'pic_magicbus|rom_detection'` runs the keyboard device
and ROM detection tests.

The synthetic device test covers each supported board's pin and IRQ routing,
ID/descriptor, DMA, legacy header isolation, queue pressure, held-key release,
and attached-state round trips. Unknown boards, mc31, and PIC-1000 are rejected.
Attached states use the existing version-3 keyboard trailer; disconnected
states remain version 2. Board wiring is derived from the loaded ROM rather
than serialized pointers. MIPS keyboard behavior is unchanged.

The mc31 image independently fails its startup checksum: bytes
`001130..2E0F27` sum to `0DC79B5E`, whereas offset `4C` stores `0DC79B3E`.
Without recovery the guest computes the same sum and asserts at `00400E96`,
before discovery. A guarded checksum bypass
now substitutes the result at `00400E88`, leaving all ROM bytes unchanged.
Experimental attached runs reach CanHandle `0046AB82` and Attached return
`0046A93C`, but the bus worker later stops through `00461F90 -> 00461B68`,
arms the falling edge, and fails subsequent scan delivery. Both boot-attached
and delayed-attachment runs failed before recovery. Fresh JIT and interpreter
runs with the bypass each reached 220 million slots, attached once, and
serviced zero keyboard requests with no decoded characters. The checksum
assertion disappeared, but the delivery failure remained. mc31 therefore
stays keyboard-gated. The experimental attachment runs used a standalone
probe, without changing production keyboard gating or injecting guest
keyboard events.

Further investigation isolated the worker shutdown to power state, not a
failed keyboard DMA transaction. On a fresh battery-only mc31 boot, the ROM
calls its power-notification handler at `00463636` with event `4` at roughly
52.76 million instructions. That path sets Magic Bus field `FC0A01EF` to one.
The later event `1` does not clear it. After attachment, the worker reads one
at `00461F2C`, skips servicing, and exits through `00461F90`. The equivalent
Envoy 1.0 check at `0046205A` reads zero and continues servicing.

Using the existing emulated AC-adapter input avoids event `4`: mc31 reads
zero at the worker check and both JIT and interpreter probes pass all three characters
(`a`, shifted `B`, `c`), release-all, and the Caps Lock LED command, with four
requests and zero bus errors. This changes neither guest code nor bus logic.
The three keyboard regression tests (`pic_magicbus`, `magicbus_keyboard`,
and `datarover_keyboard`) also pass.

Follow-up battery tracing confirms the low-power decision:
the main battery update returns `00010000` (1% in 16.16 fixed point) at
`00464AB8`. The check at `00464AE2` compares it with a 1% threshold and
continues; `00464AF8` compares it with `000A0000` (10%) and selects warning
state 2. That dispatch reaches `004653E4`, which broadcasts event 4 and
disables bus servicing. Forcing channel 2 to 1023 does not change the outcome or boot instruction
counts. Envoy's pack accounting is not simply the PIC ADC percentage.

Connecting the physical AC input at 120 million instructions, after
enumeration and worker shutdown, does not recover it either. This
battery-first experiment still has
zero keyboard requests at 220 million instructions. At exit, pending word
`B8=0020` contains the adapter edge, but enable word `C0=0B0E` masks it;
the adapter handler `0046C3EC` was never reached. The emulator therefore
correctly leaves IRQ6 deasserted for this source. Why the ROM leaves that
source masked at this boot stage remains open; cold AC success does not
establish hot-plug recovery. Battery-only operation, AC transitions, and Android delivery need validation
before lifting the production mc31 gate. No battery contents or ROM policy
are patched by these diagnostics.

The initial 1% is a guest calculation, not a host-injected battery value:
at `0045C4AA`, mc31 reads ADC `0340` (832); the first comparison in
`0045C454` receives a raw threshold of zero and a minimum level of
`00010000`. It raises the initially zero estimate to 1%, which the guest
stores into battery-record byte 12 at `0045D784` around 15.26 million
instructions. Envoy 1.0 initially performs the same adjustment.

The important later difference is the low-charge voltage recheck. Envoy
1.0 at `0045C2EA..0045C30E` checks the low-charge threshold before the
scheduled deadline and can force an immediate sample. A fresh battery-only
trace raises its estimate to 10% at `0045D510` around 53.87 million
instructions, before keyboard enumeration. mc31 at `0045C6F8..0045C708`
only checks the deadline: at its first power poll, `D0=0000F6A4` (63140 ms)
and `D4=00000C44` (3140 ms). Its voltage recheck is still 60 seconds away,
so it retains 1% and issues the low-power notification. This is a concrete
ROM policy difference, not evidence that the interpreter/JIT miscomputes
the percentage. The longer recovery replay below establishes what happens
after that guest-time deadline.

AC mask writes also predate the low-power notification. mc31 initializes
`C0` to `0B00` at `00466396`, then other initialization adds bits to reach
`0B0E`; Envoy 1.0 initializes it to `0F00` at `00466470` and reaches
`0F0E`. Neither enables the currently modeled adapter bits `0030` during
these cold-boot probes. Thus the observed mask is not a mc31-only reaction
to low battery. The adapter-event mapping and later enable lifecycle need
independent validation; do not force the interrupt through the guest mask.

### mc31 delayed battery recovery

A fresh battery-only JIT replay ran 1.6 billion slots (about 95.37 guest
seconds at the unchanged default CPI of 1). The scheduled voltage check
actually ran at `0045C710`, slot 1,088,534,465: guest time `0000FD6B`
(64875 ms) had passed the deadline `0000F6A4` (63140 ms). This rules out
a stuck timer or merely ending the original probe before the deadline.
The run still finished with one keyboard attachment, zero requests, zero
decoded characters, and no Magic Bus errors.

A second fresh 1.2-billion-slot trace watched the check's return and the
power policy. `0045C714` returned true at slot 1,088,542,447, and the main
pack update returned `000A0000` (10%) at `00464AB8`, slot 1,088,542,887.
Thus the scheduled calculation does recover the battery estimate. However,
`0046546E` (the routine that broadcasts power event 3) was never executed,
and the bus notification handler `00463636` still received only the two
boot notifications (4, then 1). Magic Bus remained disabled. Later battery
recovery is therefore insufficient to undo the early low-power shutdown
in this replay; the next unresolved path is the ROM's low-power exit and
its notification/adapter lifecycle, not a missed DMA transfer.

These runs changed no ROM bytes, saved states, battery contents, or the
emulated clock rate. The long traces used JIT; interpreter recovery beyond the deadline has not been checked.

Android/AArch64 delivery still needs on-device validation. Exact wire timing,
full FIFO/error behavior, bus power/wake, multi-accessory chains, autonomous
typematic, and Pause/Print Screen remain outside the verified profile.
