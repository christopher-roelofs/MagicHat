# 68k audio, battery and accessory parity

This covers PIC-2000, HIX-300 and Motorola Envoy. PIC-1000 has a damaged
ROM and is not a working parity target ([PIC1000.md](PIC1000.md)). Board
bring-up is in [PIC2000.md](PIC2000.md), [HIX300.md](HIX300.md) and
[ENVOY.md](ENVOY.md).

## Host battery mapping

The 68k SDL frontend polls the cross-platform host-power reader once per
second, as the DataRover frontend does. On PIC-2000 a known host charge
maps linearly to main-battery ADC channel 2 between 689 and 805. These
endpoints come from PIC's threshold table at ROM offset `26B42A`
(805, 756, 689, 679, 660) and percentage routine `0E07A2AA`; the
interpolation is a presentation approximation, not a measured discharge
curve. HIX uses its own range, 745..808 (below). Backup battery data
stays separate and fixed. A desktop without a host battery keeps the fixed
readings. `--no-host-battery` disables polling; headless runs never poll
host charge; explicit ADC overrides take precedence. Envoy does not borrow
PIC's thresholds: `mh_m68k_host_battery` deliberately rejects it, because
its charge is software-accounted state (below).

## Envoy battery memory

ROM method `EconoRAMTransaction` at `0045C6F8` emits 264 synchronization
slots, an eight-bit command and 256 data bits. It uses command `F9` to
write and `01` to read. Register `210000E6` bit 11 drives the wire low;
byte `210000E7` bit 3 samples it. The output helpers start at `0045C5C0`,
the input helper at `0045C626`. This is distinct from the serial interface
used by the audio startup code.

The protocol matches the
[DS2223/DS2224 datasheet](https://www.analog.com/media/en/technical-documentation/data-sheets/DS2223-DS2224.pdf),
pages 2-4: 264-slot transactions, least-significant command bit first,
`F9` writes, and zero-slot synchronization. The model implements 32 bytes
of writable SRAM with an open-drain signal and timed read release. This
establishes protocol compatibility, not the exact chip in the battery. No
factory serial number or calibration contents are fabricated.

New SRAM starts at zero, an explicit emulator initialization choice; guest
writes supply its later contents. Pulse timing uses the CPU core's cycle
counter at the modeled CPU frequency, not the execution-slot count (see
[M68K_CORE.md](M68K_CORE.md), "A cycle count").

Battery memory is part of the machine's saved state, so it survives
process exit with the rest of the device. It is device memory, not a
suspended CPU. `--temporary` does not write the state; `--fresh` ignores
the saved state, including battery memory. Use both to preserve an
existing session during experiments.

HIX has reset/presence helpers around `0E059ACC` and different bit
routines at `0E059B96`/`0E059C5C`. Their caller is a Read ROM identity
transaction, not battery RAM, so they are not wired to the EconoRAM model.

A fresh Envoy replay through calibration performs three EconoRAM
transactions, including 256 written bits. At 185 million slots it shows
the ordinary onboarding panel with no battery-RAM warning; at 210 million,
after dismissing onboarding, it shows a clear Desk. An older retained
image can still show an already-recorded battery warning; it is not
silently cleared. A save-and-reopen test confirmed battery-memory
restoration: continuing the restart replay through 210 million slots and
normal taps returns to a clear Desk, and the restored memory is read once
with no new write and no warning.

## Accessory input

Envoy reader `00463B8A` reads byte `210000E7` bit 2. Discovery at
`004622B6` takes the high-input stop/arm path through `00461CA2` and
returns zero devices; `00462D28` gates discovery independently. The model
presents that disconnected level and preserves the battery-memory input on
the adjacent bit. PIC keeps its `0C000002` bit 2 input. See
[PIC2000_MAGICBUS.md](PIC2000_MAGICBUS.md) and
[68k keyboards](68K_KEYBOARD.md).

## Sound

All three devices use the shared 68k DMA/audio engine
([PIC2000_AUDIO.md](PIC2000_AUDIO.md)), including mute, intermediate
attenuation and filtering. SDL audio diagnostics use the actual device
name. No new codec or measured analog accuracy is claimed. The
attenuation and filter approximations are on by default; HIX's inferred
divider is its board default. Envoy's sample-ready interrupt is separate
from its DMA refill events.

Fresh PIC, Envoy and HIX calibration replays each captured 551,998 mono
frames at 44,100 Hz. PIC produced 64,267 nonzero samples, Envoy 49,133 and
HIX 55,272. All peaked at 10,953, below signed-16-bit clipping. Silence in
a quiet retained PIC session is not a playback failure; test with actual
guest sound activity.

## HIX battery sensors and identity chip

HIX `CalculateLevel` at `0E052610` clamps the measured ADC sample between
its empty and full thresholds before returning a 16.16 percentage. A cold
trace at `0E052638` observes main thresholds 745/808 and backup thresholds
510/613. Backup ROM data at `2579D6` contains 613, 549, 512, 510, 128. The
shared backup sample of 512 was therefore almost empty for HIX despite
being healthy for PIC. HIX now supplies 640, above its full threshold.
Main stays 832 by default, and GUI host-charge mapping uses HIX's 745..808
range. Diagnostic ADC overrides still win. A synthetic ROM verifies the
CPU-visible readings at host charge 0%, 50% and 100%.

The HIX serial routine at `0E059DAE` calls reset/presence at `0E059ACC`,
sends command `33`, reads eight bytes and checks the CRC. With a register
latch returning zero, the ROM saw false presence and an all-zero identity
with an accidentally valid CRC, and accepted it. The passive line now reads
high when released and low while E6 bit 11 is asserted. No identity chip is
simulated, so it supplies neither presence nor invented serial bytes, and
startup reports the limitation. The ROM makes 16 presence attempts, skips
Read ROM, and still reaches a clear Desk. The exact part is unverified.

## Envoy status icons and pack charge

Envoy's top-bar triangle at `(343,12)` is an antenna: a tap opens **Quick
wireless modem controls**. A battery-icon tap at `(297,11)` opens **Power
and battery symbols**, confirming that the empty battery image means low
charge.

The main pack's charge is not a PIC-style ADC percentage. Envoy
`0045CFCE` reads byte 12 of its battery record and clamps it to 100 before
converting it to a level. `InitializeEconoRAM` at `0045CEA8` clears that
byte; factory/header fields feed pack configuration. A zero-initialized
pack can therefore pass the checksums and report zero charge. Forcing that
byte would obscure the missing model, so it is left to guest writes.
`ACAdapterAttached` at `00466816` reads byte `210000D1` bit 6.

## Envoy adapter connection

The runtime supplies Envoy's adapter input (`D1` bit 6) and change events.
The IRQ6 fallback table at `0046C916` maps pending longword bits 21/20 to
handlers `0046C5EA`/`0046C5FE`, which set deferred flags 2/3 and schedule
power processing (`00466324`); that code reads `ACAdapterAttached` and
calls `ACAdapterStatusChanged`. These sources participate in IRQ6
arbitration only for Envoy. Guest writes cannot change the physical input,
and unchanged host polls produce no edge. Earlier candidate bits 27/26 were
rejected after tracing the paired AC-change handlers.

The GUI polls host adapter state even on desktops without a battery;
`--no-host-battery` disables both charge and adapter polling. Headless
runs start disconnected unless `--ac-adapter on` is given; either explicit
setting overrides host updates. The option applies to Envoy only. Adapter
presence is external state and is not restored from saved state.

```sh
./build/mhat --rom roms/envoy-1.0.rom --ac-adapter on
```

A cold calibration replay with the adapter attached reaches the Desk with
the charging symbol. `BatteryChargerEnabled` at `004666BC` first checks
adapter presence, then interprets its output control shadow's charge mode;
the emulator forces neither that result nor the guest's percentage. A
scheduled power press at 225 million slots reaches hardware power-off by
240 million, and the diagnostic summary prints the modeled power and
adapter state (a raw framebuffer dump still holds the old Desk after the
LCD turns off). Relaunching from the state saved after power-off returns
directly to the Desk with calibration retained and the AC symbol shown.
Unit tests cover IRQ delivery on both edges, no repeated events for a
steady connection, physical input protection, and override precedence.

Live adapter transitions have a limitation: the observed Envoy
configuration leaves the paired adapter sources masked (see the mc31
traces in [68k keyboards](68K_KEYBOARD.md)). The model latches the edge and
respects guest masks; it does not force an IRQ through a disabled source.

## Charge accumulation

A fresh, temporary replay with the adapter connected measures values at
the battery percentage setter. At `0045D510`, D3 holds the requested 16.16
percentage and D0 the integer about to be written to the battery record at
A2+12. Five billion slots produce 22 setter observations, including:

| Execution slot | Fixed-point percentage | Record percentage |
| ---: | ---: | ---: |
| 53,642,249 | 0 | 0 |
| 1,250,328,689 | 0.307678 | 0 |
| 2,424,380,038 | 0.615356 | 1 |
| 3,431,103,494 | 0.923035 | 1 |
| 4,773,189,850 | 1.230713 | 1 |

A final raw RAM inspection finds byte 12 equal to 1 at record address
`000B3F9C`. The Desk stays clear with the charging symbol, so the
unchanged icon did **not** indicate stalled charge accounting.

The increment trace at `0045C1D6` observes capacity 650 (`D0=028A`),
increment 2 (`D3=2`), and fixed-point increment `D1=4EC4`. The routine
waits for a time difference greater than 59,999, computes an integer
`elapsed * 125 / 3,600,000`, converts that through capacity into a
percentage, and accumulates it. These are observed guest calculations, not
proof of real pack capacity or current; four increments remain a low
percentage, so a nearly empty icon is expected.

The run performs one EconoRAM transaction and no wire writes: updating the
RAM record is distinct from persisting charge into battery SRAM. No guest
RAM, ROM instruction, return value or percentage was forced.

## Remaining work

- Envoy pack configuration, ADC feedback, charge writeback, and adapter
  event enabling. Mirroring host percentage into the guest would be a
  separate host-gauge feature, not an accurate charger model.
- Envoy's unmapped `34000000`/`38000000` windows ([ENVOY.md](ENVOY.md)).
  There is not enough evidence to fake a peripheral ID.
- HIX's identity chip, accessory inputs and auxiliary latch.
