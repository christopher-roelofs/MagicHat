# External keyboards

## DataRover implementation status

The DataRover GUI now routes host keydown/up events through an emulated
Magic Bus AT keyboard and the stock ROM driver. A real SDL event-loop probe
produces visible `aBc` in the Notebook, including uppercase input and recovery
from focus loss. It sends no UART bytes and does not modify guest RAM or ROM.

Normal GUI startup attaches the accessory automatically. `--monitor` keeps
console typing by default; `--keyboard` explicitly attaches it in any mode,
and `--no-keyboard` disconnects it and leaves SDL typing routed to UART A.
Headless startup does not add an accessory unless requested. A keyboard already
saved in a snapshot or standby context is restored, including for headless runs,
unless explicitly disconnected. The original debugging monitor and Magic Cap's
PCLink service both use UART A at different times; this keyboard uses MBUS.

The profile is reconstructed from the ROM/SDK, not a dump from an iBiz or Sony
keyboard. Precise wire timing, autonomous device typematic and special
Pause/Print Screen sequences remain fidelity work. PIC-2000, Envoy 1.0/pt4,
and HIX-300 use the shared AT endpoint with a separate 68k controller; see
[68k keyboards](68K_KEYBOARD.md) for per-ROM status, evidence, and limitations.

## Hardware and driver evidence

Icras lists the iBiz KeySync as a DataRover 840 accessory:
[archived manufacturer page](https://web.archive.org/web/20001208204900/http://www.icras.com/keysynch.html).
That establishes product support, not that KeySync uses the same wire protocol
as the built-in Magic Bus AT keyboard driver. No KeySync-specific driver has
been identified in our downloaded packages.

The local Rosemary SDK has a separate, built-in path suitable for emulation:

- `MagicBusATKeyboard.cdef` in the original Magic Developer SDK (kept outside
  this emulator repository)
- The adjacent `MagicBusClient.cdef` and `MagicBus.cdef` describe peripheral
  discovery, requests and read/write operations.
- `Interfaces/PlatformDefines.h` enables `MAGICBUSKEYBOARD` for the relevant
  device builds.
- `Interfaces/MagicCap.cx` assigns selector `0x12FC` to `DispatchATKeys`.

The stock `roms/Data Rover 840/DataRover-840-USA.image` binds that selector to
`13C2763C` (binding at ROM file offset `2EE2D0`). Its native method table also
contains the methods below. Addresses use the ROM's `13C...` code alias;
subtract `13C00000` for file offsets. These are observations, never addresses
used by the device model to modify execution.

| Function | ROM address | Observed behavior |
| --- | --- | --- |
| CanHandlePeripheral | `13C27984` | Accepts `4D424B42` (`MBKB`) or `41544B42` (`ATKB`); creates class `4C1`. |
| Attached | `13C27594` | Calls base attachment, enables realtime mode, initializes keyboard, marks keyboard attached. |
| DispatchATKeys | `13C2763C` | Decodes AT set 2: F0 release, E0/E1 prefix, 12/59 Shift, 14 Control, 11 Alt, 58 Caps Lock, 77 Num Lock. |
| PeripheralRequest | `13C27B20` | Handles request byte 1 = `0E`; reads up to 16 bytes with ReadPeripheral command 2. First returned byte counts following scan bytes. |
| ResetKeyboard | `13C27C38` | WritePeripheral command 5, eight-byte packet `4B 01 FF 00 00 00 00 00`. |
| SetLedStatus | `13C27C84` | Command 5 packet `4B 02 ED leds 00 00 00 00`, LEDs masked to 3 bits. |
| SetRepeatRate | `13C27CD4` | Command 5 packet `4B 03 F3 rate F4 00 00 00`, rate = `(delay & 3)<<5 \| (repeat & 31)`. |

The endpoint queues already-encoded set 2 key events, including extended
make/break sequences. Queue overflow rejects an entire event rather than
emitting a partial prefix. Reads return a count followed by queued bytes;
modifier/prefix state persists in the guest, so transactions may split a
sequence. Unknown or malformed control packets return failure without
changing state. The endpoint stores LED/repeat settings and clears its queue
on reset. Autonomous typematic generation and Pause/Print Screen sequences
are not implemented. SDL supplies host repeat make events; the stored guest
repeat setting does not yet control their timing. Reset defaults beyond clearing
local endpoint state have not been established from hardware.

## Controller findings and next implementation work

The serial accessory connector is not proof that plain UART terminal bytes
will reach this driver. The ROM uses the dedicated TX39 MBUS registers for the
built-in AT keyboard path. The separately selectable console route sends
terminal bytes to UART A and can drop a byte if its receive holding register
is occupied. Accessory typing has its own scan queue and never falls through
to that UART route, including for unsupported host keys.

The SDK `Interfaces/Dino.h` and `Dino.asm.h` establish register names that the
old empty-bus model did not have:

| Offset | SDK role |
| --- | --- |
| E0 | Control 1: enable bit 0, long word bit 1, slave bit 3, TX DMA bit 15, RX DMA bit 16, input status bit 29, empty status bit 30, enabled status bit 31. |
| E4 | Control 2: delay bits 31:24, baud-rate bits 23:16. **Not payload data.** |
| E8 / EC / F0 | DMA start / length / count. |
| F4 / F8 | Command / payload data. |

Named aliases and histogram labels now reflect these findings. Empty-bus
execution behavior is unchanged. An attached port reports SDK enabled/empty
status, accepts incoming words and commands, performs RX/TX DMA through the
physical bus, and reports interrupt events. Its historical `MBUSCTRL_BUSY`
name for bit 31 must not be mistaken for the SDK's enabled-status definition.

Additional ROM traces establish the following starting points:

1. `13C2A794` samples input status and starts discovery if low.
   `13C2A8EC` sends broadcast command 31, requires input high, sends address
   assignment command 24 and command 21. `13C2A974` checks the chain and stops
   when the input goes low after assignment. These transitions need a real
   peripheral state machine, not a constant present bit.
2. `13C2B8E8` requests ID with command 12, then reads four bytes using command 2.
   `13C29284` requests the information block with command 13, reads it using
   command 2, validates its declared size and checksum.
3. `13C2848C` starts transfers; `13C28BA4` writes the encoded command to F4.
   Receive setup uses control `08A9`/`08AB`, adding bit 16 for DMA. The ROM
   programs DMA length as requested bytes minus four. `13C28830` collects
   receive results from F8 or DMA count, depending on transfer size.
4. Command words are constructed by XORing command encodings at ROM offset
   `296B70` with address encodings at `296BB4`. These tables describe the
   driver's wire encoding; the emulator must not look them up from guest ROM.
5. Key delivery must produce the peripheral-request transaction consumed by
   `13C27B20`, followed by a normal controller read. Calling DispatchATKeys or
   injecting a high-level key event into guest RAM would bypass the hardware.

The controller connection API is `tx39_mbus_port` in
`src/soc/tx39/tx39_mbus.h`; board wiring is per machine, not global.
`mrc_mbus_receive_word` writes payload data or DMA memory, while
`mrc_mbus_receive_command` latches command-detect. Physical request-line edges
are separate from command-detect. Incoming events do not reassert after W1C
acknowledgement; TX empty/available retain the existing level behavior.
DMA bounds and bus faults are checked. The receive count matches the ROM's
length-minus-four convention for a completed buffer.

Remaining controller fidelity limits: transfers currently complete
instantaneously; baud/delay timing, DMA loop mode, detailed FIFO/overrun
behavior and half-buffer interrupt boundary semantics are not hardware-verified.
The standalone tests cover the implemented mode, not all controller modes.

### Host input and lifecycle

USB HID keyboard usages (SDL scancodes) map to AT set 2 in
`src/devices/magicbus/host_keys.c`. Protocol assignments were cross-checked with
[Linux's AT keyboard driver](https://raw.githubusercontent.com/torvalds/linux/master/drivers/input/keyboard/atkbd.c).
Letters, digits, punctuation, modifiers, navigation and keypad keys are mapped;
Magic Cap applies its own keyboard layout. This is physical-key input, not
Unicode text, host-layout translation, clipboard paste or an input method.
Frontend shortcuts F2/F3/F4/F11, Alt+Enter and Ctrl+Q retain their existing roles.

Focus loss, hiding/minimizing and GUI exit release host-held keys. Break codes
that cannot fit in the scan queue are retained and retried, preventing a stuck
modifier under queue pressure. Merely moving the pointer out of the window
does not release keyboard keys. Supported 68k boards also install runtime
keyboard callbacks and route SDL events through their separate controller.

On DataRover, the board-owned port pointer stays outside the serialized SoC prefix. With a
keyboard connected, snapshots use version 6 and append a 384-byte explicit
byte encoding: queued scans, LED/repeat settings, request/transfer state, DMA
receive progress and host-held/deferred-release keys. No device pointers are
serialized in that trailer. Version-5 snapshots remain readable and are still
written without an attached keyboard. Unknown MBUS peripherals remain rejected.

Standby context similarly uses version 2 with the keyboard trailer, retaining
version 1 for sessions without it. Loading restores port wiring without a new
edge, preserving pending transactions. Starting a new host session schedules
releases for keys held by the previous host session; it does not reset the
keyboard or inject guest modifier state. `.bram` remains coupled to suspended
CPU/RAM state and follows the existing guest power-off policy.

## Verification

`ctest --test-dir build -R 'magicbus|mbus|datarover_keyboard|uart|datarover_boot|bram|datarover_ram' --output-on-failure`
passes eight suites (including `datarover_keyboard` in the filter). A preexisting version-5 power-off snapshot also loads
successfully with the new build. Endpoint tests check scan order, transaction boundaries,
queue wrap/overflow, control packets and reset. Controller tests check command
reception, non-DMA reads, DMA endianness/count/bounds, transmit, request-line
edges, W1C acknowledgement and DMA bus faults.

The reproducible fresh-ROM experiment is checked in separately:

```sh
cmake -S . -B build
cmake --build build --target magicbus_keyboard_probe -j6
./build/magicbus_keyboard_probe 'roms/Data Rover 840/DataRover-840-USA.image'
```

It reads the ROM only, creates no BRAM/state files, and never alters ROM
instructions, guest variables or OS callbacks. ROM-specific PC observations
check attachment, request dispatch and decoded characters, then terminate the experiment. They are diagnostic only.
The harness implements one peripheral's reconstructed discovery responses:
broadcast 31 drives the request line high, assignment 24 selects address zero,
and command 21 leaves the line low because there is no next device. ID command
12 selects MBKB; command 2 returns it through the controller. Command 13 then
selects the information block, which is also delivered through the controller,
this time using RX DMA. Received end-command `DCF8` terminates the transfer;
its use is inferred from the ROM's command/address tables and validated for
this transaction sequence, not from a logic-analyzer capture.

The constructed information block uses the ROM-observed structure: leading
zero word, length at +2, ID at +4, rate fields +C/+14/+1C, three Pascal strings
starting at +50, a zero extension terminator and a trailing 16-bit byte sum
seeded with one. The profile supplies nonzero rates (256000), block/latency
values and zero current demands for the emulated accessory. Those advertised
values are choices for this experiment, not claims about a physical keyboard.
The guest validates the length/checksum, stores its names/capabilities, and
finishes its existing Attached method at `13C275F8` after approximately
197.46 million instructions. The probe then sends `a` make/break followed by
Shift + `b` make/break and Shift release in a separate request. It checks the
guest's key-map results (`61`, `42`) at `13C278C8`, not just entry to the scan
decoder. A visit to the enqueue intrinsic at `13C27900` can repeat, so its hit
count alone is not a reliable character count. Exit status zero requires two
requests, exactly those two decoded characters, an empty endpoint queue and
no unknown commands, rejected writes or receive errors. This is **not yet
visible text/UI validation**.

### Peripheral requests and the reusable adapter

`src/machines/datarover840/keyboard.c` owns the connection between the shared
AT endpoint and the TX39 controller. The experiment now uses this same adapter;
its only ROM-specific code is observational. Normal machine execution services
the same adapter after CPU/device chunks.

The ROM request scheduler at `13C298C4` tests the input line for high, sends
command 28 to poll an address, then command 1 to receive the request. The
receive result is queued by `13C28F08`, called from `13C29B44`; later
`DispatchPeripheralRequests` invokes the registered keyboard client. The
adapter returns a four-byte request with byte 1 = `0E` and lowers the line.
The guest then issues command 2 to fetch a count-prefixed scan packet. After
that read, any remaining scan bytes raise another request. A key arriving
between request delivery and scan read joins the pending scan queue.

The header's unused bytes are zero, and transfers are padded to controller
words. These are reconstructed choices validated with this ROM, not physical
wire captures. Request-line polarity changes with discovery versus assigned
operation; holding it permanently at a presence level cannot deliver keys.

The adapter receives the driver's eight-byte command-5 writes through the
controller TX path and applies reset, LED and repeat settings. Unknown commands
and malformed writes have separate counters. Failed receive delivery preserves
queued scan bytes. The device test covers descriptor checksum, deferred reads,
repeated requests with an extended break prefix split across packets, keys
arriving while a request is outstanding, DMA LED writes, invalid writes, reset,
and receive bus faults. Autonomous typematic and precise wire timing remain
unimplemented.

Logs/disassembly: `out/datarover-keyboard/attachment.log`, `discovery.log`,
`info-probe.log` and the observational disassembly files in that directory.

## SDL integration proof

```sh
cmake --build build --target magicbus_keyboard_sdl_probe -j6
SDL_VIDEODRIVER=dummy ./build/magicbus_keyboard_sdl_probe \
  'roms/Data Rover 840/DataRover-840-USA.image' \
  out/datarover-keyboard/note.state out/datarover-keyboard/typed.pgm
```

The input snapshot is a stock guest at a blank Notebook page with the keyboard
attached. The probe pushes SDL key events, followed by a window focus-loss event
while Shift is held, then another ordinary key. It runs the normal SDL event
loop and board runtime, not direct scan-code injection. `dummy` is only for this
automated test; normal desktop launches require no SDL driver setting.

The observed Notebook displays `aBc`. ROM key-map observations show `61`, `42`,
`63`, and the test checks three decoded keys, zero UART bytes, no held/release
keys and no unknown commands or receive errors. Output is in
`out/datarover-keyboard/sdl-probe.log` and `typed.pgm`/`typed.png`. The device
unit test additionally covers overflow-safe focus release and state validation;
BRAM/CLI tests cover GUI defaults, monitor selection, explicit disable,
snapshot and standby restore, truncated trailers and invalid queue counts.

## PIC-2000

Not implemented in this work. Its distinct controller and ROM driver must be
traced before reusing the DataRover transport. A shared AT key encoder may be
appropriate only if its driver confirms the same scan-code convention.
