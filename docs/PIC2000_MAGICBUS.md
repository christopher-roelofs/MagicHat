# PIC-2000 Magic Bus controller and keyboard

PIC-2000 supports a single emulated Magic Bus AT keyboard through its stock
ROM driver. GUI launches attach it automatically; `--keyboard` enables it
explicitly, including headless runs, and `--no-keyboard` disconnects it.
Envoy 1.0/pt4 and HIX-300 share this transport with separate board wiring;
see [68k keyboard status](68K_KEYBOARD.md). Unverified boards remain gated,
and their disconnected-input models are independent of this
implementation.

The implementation uses the dedicated accessory bus, without consuming a
PCMCIA slot. It reconstructs the serial controller in `dev21`, the bus
input in `dev0c`, and the ROM's transfers through MC68349 DMA channel 2.
There are no ROM patches, guest OS calls, or high-level keyboard-event
injections. The code is in `src/machines/pic2000/magicbus.cpp`;
`MH_PIC_MBUS_TRACE=1` logs controller traffic.

## Disconnected input

The PIC-2000's "A problem happened while using an accessory" warning was
caused by an incorrectly low bus input with no accessory connected. This
was a controller-input modeling bug, not evidence that a package or a
physical accessory was needed. The first correction modeled the empty bus;
the attached keyboard described below extends it.

### Evidence from stock firmware

The warning text is at ROM offset `2723EA`. With the old input value:

- `0E083734` reads **byte** `0C000002`, tests bit 2 at `0E083738`, and
  returns its Boolean value.
- Discovery at `0E081EF6` calls that reader. High selects `0E081EFE`,
  which stops the transmitter, arms the bus-change input, and returns zero
  peripherals. Low instead starts assignment and probing.
- In the failing run the reader returned zero at instruction 30,683,478.
  Assignment then returned `FF`, and `0E081A2E` raised guest error `FB` at
  instruction 30,684,158. The failure path reached `0E081AEC` at
  instruction 30,687,205, then the ROM's retry delay of `EA60`
  (60,000 ms).
- There is also a bus-change gate: `0E082930` reads the same input; high
  takes `0E08293A`, arms the empty-bus change source and returns. Low at
  `0E08294A` schedules the discovery worker.
- The corrected startup encounters an even earlier gate at `0E08337E`, at
  instruction 9,057,963. High selects `0E083392`, calling the stop/arm
  helper `0E081914`; low schedules discovery through `0E081D24`. The
  corrected run therefore never needs to enter the discovery worker.

Both gates establish the disconnected polarity. The ASIC identity and the
physical circuit have not been verified.

### Implementation

`Pic2000Registers::magicbus_empty_input` makes bit 2 of byte offset 2 a
read-only high input in the `dev0c` register block. It is applied while
assembling read values, independently of stored register words, so word
and longword accesses see it in the correct big-endian position and
controller writes cannot clear the physical input. Other bits are
preserved.

The board enables this only when ROM identification returns PIC-2000.
With no accessory attached, no peripheral reply or bus-change interrupt is
generated except an explicit disconnect edge.

The DataRover had a similar empty-bus mistake, but uses TX39 MBUSCTRL bit
29. Its fix and attached-keyboard transport are separate implementations;
see [KEYBOARD.md](KEYBOARD.md) and [OPEN_QUESTIONS.md](OPEN_QUESTIONS.md).

The same 40-million-slot retained-RAM startup produces the accessory
dialog before the correction and a clear Desk afterward, with no undecoded
accesses or discarded ROM writes in either trace. A 1.2-billion-slot run
(about 71.5 guest seconds at the default clock and CPI) ends at a clear
Desk with zero hits on the assignment error and failure handler, well past
the former one-minute retry interval.

### Envoy and HIX inputs

Envoy uses byte `210000E7` bit 2, not PIC's `0C000002` bit 2. Reader
`00463B8A` feeds discovery at `004622B6`; a high input takes the stop/arm
path through `00461CA2` and returns zero devices. `00462D28` independently
gates discovery on the same input. The register model presents this
disconnected level for identified Envoy ROMs and preserves unrelated bits,
including the EconoRAM input at bit 3. HIX's Magic Bus input is `210000EE`
bit 6; see [68k keyboards](68K_KEYBOARD.md) and [68k parity](68K_PARITY.md).

## Attached controller reconstruction

The ROM's request scheduler is `0E082A66`; the synchronous peripheral API
at `0E0834C2` queues work and waits for this scheduler. Following that
call chain reveals the hardware interface:

| Register | Observed role |
| --- | --- |
| `21000090` | Serial control; low bits change between command transmission, payload transmission, and receive DMA. |
| `21000092` | Receive data port selected as DMA source. |
| `21000094` | Command-byte transmission. |
| `21000096` | Primes the first command byte; the next byte goes to `+94`. |
| `21000098` | Transmit payload; ROM primes one word, then DMA sends the remainder. |
| `2100009C`, `210000A0` | Transfer clock/delay settings; stored, but their timing is not simulated. |
| `210000B2` | Pending events: bit 8 falling bus input, bit 9 rising input, bit 10 transmitter/receive-end completion, bit 11 transmitter phase/ready. |
| `210000B6` | Corresponding interrupt enables. |
| `0C000002`, bit 2 | Physical accessory input; polarity depends on discovery versus assigned operation. |

Discovery at `0E08182C` sends broadcast `DEF0`, expects input high,
assigns address 6 with `DCA8`, and sends `DCE0`. A single peripheral then
drives low to signal the end of the chain. PIC's address encoding table at
`0E088F26` numbers addresses in the opposite order to the DataRover table;
the first device's actual wire encoding is the same.

ID selection/read at `0E08342A` sends `CC5C` then `CC24`. The four-byte
reply is `MBKB`. Information selection/read at `0E082230` sends `CC60` then
`CC24`. PIC expects the 16-bit length at byte zero, ID at +2, Pascal
strings at +4E, and a trailing 16-bit byte-sum checksum seeded with one.
Its descriptor is 126 bytes, compared with the DataRover profile's 128
bytes including a leading zero halfword. Names, rates, and capacities are a
constructed emulated accessory profile, not a physical keyboard dump.

`0E082D4A` configures MC68349 DMA channel 2 at module offset `7A0`: source
`7AC`, destination `7B0`, remaining bytes `7B4`, and control `7A8`.
Receive uses word-sized peripheral-to-memory transfers (`B6A5`); transmit
uses memory-to-peripheral transfers (`FAA5`). The model implements those
external-request modes and checks alignment, RAM destinations, readable
sources, and byte counts. DMA status at byte `7AA` is write-one-to-clear.
Normal and error interrupts use the level and vector in the interrupt
register `7A4` (the ROM programs `0540`, IPL5/vector 64). Byte `7AB` is the
function-code register, not the interrupt vector (MC68349 User's Manual
sections 7.7.1-7.7.3 and the DMA register map).

US patent 5,675,811 describes the single-master, daisy-chained bus with
separate data, clock, interrupt, power, and ground signals. This agrees
with the command and input-line behavior above; it does not establish
PIC's MMIO register map.

## Key delivery and lifecycle

The ROM accepts MBKB/ATKB at `0E087882`, completes attachment at
`0E08764E`, receives peripheral requests at `0E087972`, and decodes AT set
2 at `0E087678`. The key-map result is observable at `0E08782E`.

Host physical key usages enter the shared AT scan queue. An assigned
keyboard raises the bus input; the guest polls with `DCC8`, reads a
two-byte request using `CC18`, then fetches count-prefixed scan bytes using
`CC24`. Request byte 1 is `0E`. The guest's command-5 writes (`CC3C`)
implement reset, LED status, and repeat configuration through the shared
endpoint.

The SDL frontend sends make/break and host repeat events through runtime
callbacks. Focus loss, window hiding or minimizing, backgrounding, and exit
release held keys. Queue pressure defers break codes until space is
available. This is physical-key input; Unicode text, clipboard paste, and
host keyboard-layout translation are not implemented.

Connected keyboards append an explicit 384-byte trailer to version-3
states. It preserves scan bytes, host-held and deferred-release keys,
protocol selection, request state, partial command and payload bytes, LEDs,
and repeat settings. Controller registers, pending completion deadlines,
and DMA state remain in the existing device records. Disconnected machines
write version 2. New host sessions release restored held keys. Invalid or
truncated trailers fail before changing the live machine.

## Verification and limits

Fresh-ROM probes on the project interpreter and x86-64 JIT observed
attachment, four requests, decoded `aBc` (`61,42,63`) including a
focus-loss Shift release, and the guest's Caps Lock LED write, with no
controller errors. An observation is counted only when the decoder
instruction executes, so interrupt entry and re-entry cannot double-count a
character. An SDL probe with dummy drivers verified that real host events
reach the guest decoder, leaving no held or deferred keys behind; it does
not assert visible text in a particular application.

```sh
ctest --test-dir build -R 'pic_magicbus|magicbus_keyboard|m68k_state|pic2000|m68k_cli' \
  --output-on-failure
```

The device tests cover descriptors, short DMA reads, split scan sequences,
LED writes, deferred key releases, DMA errors, state round-trips, invalid
state rejection, and board gating.

This is a functional reconstruction of the observed ROM protocol. Exact
wire timing, the full serial FIFO/error behavior, unrelated DMA modes,
peripheral power/wake behavior, autonomous typematic, Pause/Print Screen
sequences, and multi-device chains are not implemented or
hardware-verified. Receive-end completion uses a 64-slot delay so the ROM
can arm and acknowledge its interrupt; payload DMA otherwise completes
synchronously. The ASIC's identity remains unknown.
