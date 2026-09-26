# Motorola Envoy bring-up

Status: experimental **cold boot through touch calibration to the Desk**.
This is not a complete device model; battery, radio and peripheral
behavior remain experimental. The launcher detects Envoy from ROM contents
(see [ROM_DETECTION.md](ROM_DETECTION.md)) and opens its GUI by default.
The window and screenshots use the Envoy name. A saved state records the
ROM's checksum, so a state saved from another 68k ROM is refused rather
than silently reused.

The 1.0 and pt4 ROMs support external Magic Bus keyboards. The mc31 image
has guarded checksum recovery (below) but remains keyboard-gated after
failed scan-delivery tests. See [68k keyboards](68K_KEYBOARD.md).

```sh
./build/mhat --rom roms/envoy-1.0.rom
```

For a fresh temporary headless trial:

```sh
./build/mhat --device envoy --rom roms/envoy-1.0.rom \
  --fresh --temporary --headless --quiet-unknown -n 80000000 \
  --sample 1000000
```

## ROM decode and shared bus correction

The stock image's reset vector is `02400216`. Reset writes CS0 mask
`027FFFF4` and base/control `004000F9`. At `024003A6` it constructs an
address from the low 20 bits of a PC-relative label plus `00400000`, then
jumps to `004003BE`. At that destination it narrows CS0's mask to
`003FFFF4`. This proves the need for the `00400000-007FFFFF` ROM window;
it is not a firmware patch. MC68349 User's Manual section 4.3.4.2
describes mask bits as address don't-cares and explicitly permits
noncontiguous masks and aliases.

The board constants live in `src/machines/envoy/board.h`. The current map
exposes both ROM windows permanently. It does **not** yet implement
complete dynamic CS0 mask/function-code decoding, or retire the reset
window when firmware narrows the mask.

Adding this window uncovered a shared bus defect: the last-hit cache could
select mirrored RAM over an earlier ROM region after a RAM access. Firmware
then fetched different instructions depending on previous bus traffic.
Region lookup now consistently honors first-registered priority, including
retirement and reactivation. Bus tests cover reads, writes and overlay
changes; a synthetic CPU fixture executes Envoy's alias transition and
alternating RAM writes and ROM fetches. No stock image is modified by these
tests.

## Resolved initialization wait: sample-ready clock

After the decode fix, an 80-million-instruction cold run had no undecoded
accesses, discarded ROM writes or CPU exceptions, but drew no usable
screen. A PC watch records the first call to `006E4670` at instruction
262170. The call chain includes `006E2844`, `006E470C`, and `006E43F6`:

- `006E470C` writes `0960` to `21000052`, `0400` to `21000054`, and
  `1001` to `2100004E`, then sets bit 1 of `210000D2`.
- `006E43F6` manipulates output bits at `21000056` and calls `006E4670`.
- `006E4670` polls word `210000B0` bit 4, acknowledging it by writing
  `0010`, then waits for another occurrence. With no source for that bit,
  execution stayed at `006E4690-006E469A`.
- Later instructions clock output bit 2 at `21000056` and sample input
  bit 3 at `2100005A` sixteen times: a serial hardware transaction. The
  sample-ready source is now identified; the attached serial component is
  still not modeled.

The second witness is `006E2C70`: it waits on the same event, clears it,
and reads audio byte `21000061`, repeated 1024 times. Together with the
register block shared by PIC audio, this identifies a per-sample readiness
event rather than a DMA half/full boundary. The shared audio model
([PIC2000_AUDIO.md](PIC2000_AUDIO.md)) produces B0 longword bit 20 at
sample cadence for Envoy. `1001` is direct-access mode; `1007` enables the
modeled DMA ring. Direct mode consumes no RAM and produces no DMA refill
events. Acknowledgment is ordinary write-one-to-clear, and the line is
asserted only if firmware enables a pending modeled source.

Timing uses the shared rate encoding `0960` = 22050 Hz established for
PIC. Applying that rate to Envoy is an inference from the shared register
protocol, not a measured Envoy oscillator. The 44.1 kHz host output clock
quantizes sample events. The serial output sequence resembles an EEPROM
read (command, address, sixteen input bits), but no EEPROM identity or
content is invented: the unknown input reads zero and firmware proceeds
with its fallback. No forced IRQ, ROM patch or injected OS call is used.

## Calibration

With that change, the 80-million-slot cold run draws "Touch the screen to
begin" at the same `2800` framebuffer address and 480x320 2-bpp layout as
the PIC-2000. Ordinary scheduled panel taps complete all three targets and
reach the Desk:

```sh
./build/mhat --device envoy --rom roms/envoy-1.0.rom \
  --fresh --temporary --headless -n 180000000 \
  --tap 90000000,1000000,240,160 \
  --tap 120000000,1000000,23,23 \
  --tap 145000000,1000000,456,297 \
  --tap 170000000,1000000,240,160 \
  --dump 2800,38400,envoy-calibration.fb
```

This validates the pen converter and event path for calibration; it does
not establish all touch accuracy, persistence, or power behavior. An
earlier model showed a "problem with the ram in your battery" warning; the
EconoRAM model now lets a fresh boot write and read battery memory and
reach the Desk without that panel. See [68k parity](68K_PARITY.md).

A 210-million-slot replay with additional taps at `(413,45)` at 190
million and `(395,23)` at 200 million dismisses the battery and onboarding
panels. The disconnected Magic Bus input is byte `210000E7` bit 2, distinct
from PIC's pin (see [PIC2000_MAGICBUS.md](PIC2000_MAGICBUS.md)); with it
modeled, the cold replay reaches a clear Desk. The triangular status icon
is the wireless-modem antenna, not a warning.

## Unidentified peripheral windows

Decoded code accesses unmapped `34000000` and `38000000` windows. At
`006F9F82`, firmware checks byte `380000A0` against `0C`, then writes and
reads back `12 34 56 78` at `3800002C..2F`. Both checks currently fail.
`006F8160` also polls bit 6 of `3800009B`. Initialization at `006FA0BA`
writes `10` to `34000080` at `006FA3B2`, then calls the `38000000`
self-test at `006FA3BA`; the runtime dispatch reaches that initialization
from `006FA9AA` through selector `0445`. This ties both windows to the same
path without identifying the physical chip. Returning expected IDs or
clearing status bits would hide the missing hardware, so the failed checks
are retained.

Audio tests cover direct-mode event cadence, write-one-to-clear, mute,
stop, unknown rates and power-off, including the absence of DMA reads and
refills in direct mode.

## Pen contact versus Option key

The shared 68k pen path used to set `210000D1` bit 1 for contact. That bit
is actually `HardwareOptionKey`, so ordinary touches became Option-touches.
The SDK declares that intrinsic as selector `0x1E`; Envoy's intrinsic table
at `005CFA4E` routes it to `0046EEC6`, which reads this bit. The
corresponding PIC function is `0E08AF5A`. Pen contact must use digitizer
interrupts and pressure readings without changing this separate key input.

This explained a Desk navigation problem: the manual's pointing-hand
Option-tap opens a list of recent places, while a normal tap goes to the
named place. After removing the false key assertion, the same `(440,12)`
input in a retained Envoy session enters the Hallway. The raw-coordinate
transform was already correct; this was a modifier problem (*Using Magic
Cap*, printed page 18).

## Stale shutdown message at startup

The immediate "Your communicator is about to shut off" image on retained
startup was the saved framebuffer, not a newly posted shutdown warning:
the frame at 1M slots exactly matched the retained bytes at `2800..BDFF`.
Envoy leaves LCD register `21000040` bit 4 clear until display setup around
5.45M slots. Its display on/off routines at `0046A3FE` and `0046A412` set
and clear that bit. The frontend honors it, displaying black while the LCD
is disabled instead of exposing retained framebuffer contents, and power-on
clears LCD enable before the reset sequence. A runtime regression checks
disabled retained pixels and subsequent enabled output.

## Adapter and retained restart

Envoy models adapter presence and its IRQ6 change events. The GUI follows
host AC state; `--ac-adapter on|off` supplies an explicit connection. A
power-off, save and relaunch returns directly to the Desk without
calibration. The charging symbol and gradual software charge accumulation
are verified. Host-percentage mirroring, analog charger accuracy and charge
writeback remain unresolved. See [68k parity](68K_PARITY.md).

```sh
./build/mhat --rom roms/envoy-1.0.rom --ac-adapter on
```

## mc31 checksum recovery

At the user's request, mc31 uses a guarded runtime checksum-result bypass,
analogous to HIX-300's ([HIX300.md](HIX300.md)). The ROM stays unchanged on
disk and in memory. This is recovery behavior, not a repair of unknown ROM
damage.

The checked file range is `[001130,002E0F28)`. Its byte sum is `0DC79B5E`;
the stored value at `4C` is `0DC79B3E`. Without recovery the ROM asserts
at `00400E96`. Recovery requires:

- An identified Envoy mc31 image with a checksum mismatch.
- Exact recognized call/comparison/assertion bytes at `E82..E97`.
- The expected range metadata at `34` and `1124`.
- Execution at `00400E88`, with A3=`0040000C` and D0 equal to the
  independently calculated checksum.

Only then is D0 replaced with the stored checksum; the guest's CMP/BEQ
instructions execute normally. Startup and interception print `DEVIATION`
messages. Matching checksums, unfamiliar check instructions or ranges, and
unexpected register values do not trigger substitution.

The one-shot guard is removed after use and rearmed on CPU power reset.
Snapshots retain pending/completed status without changing their format;
the stop address and checksum values are derived from the loaded ROM. A
fresh boot is required to revisit a check already passed in an older state.

Synthetic CLI tests cover success, matching checksum, changed instructions,
changed range, wrong result, and wrong header pointer in both engines, and
verify that the input file is unchanged. The runtime test covers snapshots
before and after interception and power-reset rearming.

The bypass removes the startup checksum assertion but does not by itself
establish keyboard support; see the mc31 delivery results in
[68k keyboards](68K_KEYBOARD.md).

## Next

Identify and model the `34000000`/`38000000` peripherals, and establish
Envoy power and battery behavior. Use `--fresh --temporary` for bring-up
experiments that should not change the retained device.
