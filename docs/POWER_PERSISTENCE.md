# Power, retained memory, and persistence

This document separates two things: what the real devices retain when they
are switched off, and what MagicHat keeps between sessions. The hardware
findings constrain the emulator; the host mechanism is described at the end.

## Finding

Discarding RAM on normal emulator exit would not reproduce normal handheld
power-off. Persistent user storage is primarily battery-supported RAM, not
necessarily writable firmware. Saving only the emulator's current program
image would therefore not preserve the device's user data reliably.

The Magic Cap Package Development Guide explains three distinct behaviors:

- Persistent RAM survives main power off while a main battery, backup
  battery, or AC supply remains available.
- Software reset rebuilds transient clusters; persistent objects survive,
  but recent uncommitted changes can be lost.
- Normal power-off commits pending changes to persistent RAM. On power-on,
  the OS calls `ResetClass` to notify package code.

These are OS storage semantics, not proof that persistent and transient
clusters occupy separate physical power domains. The emulator must preserve
physical memory and let the ROM manage its clusters; it must not serialize
selected guest objects or invoke these OS methods directly.

Source: Package Development Guide, printed pp. 2, 11-12, 49
([publisher document mirrored by Josh Carter](https://joshcarter.com/magic_cap/docs/MagicSDK_Package_Dev_Guide.pdf)).

## Device evidence

| Device | Established | Still unverified |
| --- | --- | --- |
| DataRover 840 | Its user guide says the lithium backup battery preserves information when the main battery is low or removed. Power-off may finish housekeeping before the device switches off. | Exact retained physical ranges, refresh/power control, CPU resume/reset sequence and RTC behavior during backup-only operation. |
| Sony PIC-2000 | Sony specifies 2 MB of battery-backed RAM. The ROM contains a warm-start check for the `XRAM` signature. | Which physical banks are retained and their real sizes; exact wake/reset sequencing. The emulator's 4 MiB low RAM plus 4 MiB XRAM allocations are not the documented physical capacity. |
| Sony PIC-1000 | Sony's launch press kit describes battery-backed DRAM for data storage. | Board memory map, retention domains and wake/reset sequencing; this board is not implemented. |
| Motorola Envoy | Motorola's launch announcement explicitly includes both main and backup batteries. | The announcement does not specify the retained memory domains. Do not equate battery presence with a verified RAM map. |
| Sony HIX-300 | No model-specific retention documentation has been found. | Retention must remain unknown rather than copied from another Sony board. |

Sources:

- DataRover: *Using Magic Cap*, printed p. 204 (housekeeping at power-off),
  pp. 209-210 (main/backup batteries). See the
  [source catalogue](https://joshcarter.com/magic_cap/magic_cap_developer_docs/).
- PIC-2000: Sony Applications Guide, printed p. 45, available as a
  [Sony manual exhibit](https://ptacts.uspto.gov/ptacts/public-informations/petitions/1514335/download-documents?artifactId=qehFmH8Y0q3IZyGYl62KscS8loRhm74AZjrU774H0EcxFOzAKCIdgnw)
  and a [manual mirror](https://www.manualslib.com/manual/3500503/Sony-Magic-Link-Pic-2000.html?page=45).
- PIC-1000: [Sony September 1994 press kit](https://usermanual.wiki/Pdf/SonyMagicLinkPressKitSep1994.705626060.pdf).
- Envoy: [Motorola launch announcement, posted by Motorola Wireless Data Group](https://groups.google.com/g/comp.dcom.telecom/c/1VnLzGLGVF0).

## Firmware is a separate issue

The DataRover model accepts direct writes to its loaded program image, and
saved states include those bytes. Magic Cap rewrites a gp-setup stub in the
image during startup (see [OPEN_QUESTIONS.md](OPEN_QUESTIONS.md)), so a
discard-writes ROM would break it. That does not establish that ordinary
installed packages reside in physical flash.

A firsthand [DataRover board teardown](https://oldvcr.blogspot.com/2022/12/magic-cap-from-magic-link-to-datarover.html)
identifies mask-ROM chips in an 840 and distinguishes that model from the
flash-equipped 840F. Firmware storage is therefore a board variant, not a
property of every DataRover image. ROM contents alone need not distinguish
two boards running the same firmware. The direct-write model still lacks
NOR erase/program commands. The ROM file on disk is never rewritten.

The PIC-2000 model maps its program image read-only.

## Guest power-off paths

These are modelled from ROM behaviour and are what the power button (F4, or
the control rail) exercises.

### DataRover

A short power press from the Desk reaches the ROM shutdown routine:
`13C3B1C4` writes POWERCTRL with VCCON and PWRCS cleared. The power
controller suspends instruction execution when software lowers both supply
controls from an asserted state. The CPU and RAM are retained; nothing is
reset and no guest routine is invoked. The power button asserts PWRCS and
resumes the suspended instruction stream at `13C3B1C8`, in addition to
raising the ONBUTN edge. The display is blank while suspended; guest code
restores video on wake. This models the observed shutdown path, not every
STOPCPU/doze mode or alarm/peripheral wake source.

The Toshiba TMPR3912 product brief, pp. 13-14, describes the standby
supply, retained memory, and ONBUTN-to-PWRCS behaviour. The exact DataRover
supply wiring is inferred from ROM behaviour; analog rail settling and the
external SPI power-supply controller are not modelled. Execution-slot
budgets and emulated time continue while suspended, with no instruction
fetches. A saved state retains the suspended CPU.

A reset with RAM retained from a completed shutdown shows a short "Cleaning
up" broom bitmap on the boot screen, then reaches the Desk with installed
packages intact. That bitmap is not the longer "Now cleaning up so that
information will be stored more efficiently" text at ROM offset `0x2f8748`.
The decision is at `13C1DF60`, which calls the predicate at `13C1D8BC` with
reason `504F4646` (`POFF`); `POFF` takes the default true result and
`13C1DF6C` enters the drawing path through `13C1DE14`, `13C1DD44`,
`13C1F860` and `13C6DDAC` (source bitmap at ROM offset `0x3636d8`). The
screen alone therefore does not diagnose an interrupted shutdown or corrupt
RAM. Resuming a suspended machine from a full state does not pass through
this path.

### PIC-2000

- `0x0e07fc2e` reads the ON button at dev21 `+D1` bit 2, active high.
- The IPL6 handlers at `0x0e088cb8`/`0x0e088ccc` deliver press/release from
  `+B8` word bits 3/2. `0x0e07faf8` dispatches these to the button object;
  `0x0e07fbc2` acknowledges both edges and re-enables them at `+C0`.
- At shutdown the ROM sets `POFF`, commits its objects, masks interrupts
  and installs shutdown vectors, then repeatedly clears `+D0` at
  `0x0e088f88`. A zero word write there removes CPU power. Reads remain
  physical input levels. The ASIC's identity and other output encodings
  are unknown; this power-control interpretation is inferred from the
  shutdown routine.
- Power-on resets the CPU and restores the CS0 boot ROM overlay while
  keeping both RAM arrays. Reset without a wake input can legitimately
  return to the powered-off loop.

Restarting from RAM retained after a completed guest power-off reaches the
Desk with calibration intact; the ROM accepts its `RSET` restart block and
persistent object heap (`0x0e0004a8`, `0x0e000c76`, `0x0e09db64`). RAM
captured while the guest was still running did not preserve calibration,
which is consistent with the SDK's description of power-off committing
transient changes. The early `XRAM` comparison at `0x0e000486` only selects
a stack.

## What MagicHat keeps between sessions

A device saves its whole machine, and nothing else: CPU, devices, RAM, and
on DataRover the in-memory program image. There is no separate
retained-RAM file. Because the state is exact, a device resumes where it was
put down rather than rebooting; it does not need to be switched off first,
and closing the window does not press the power button.

- **Where.** The state sits beside the firmware with `.state` in place of
  its extension: `roms/MagicCap-USA.image` gets `roms/MagicCap-USA.state`.
  Devices made in the window's device list live in one directory each,
  `devices/<id>/rom` (a copy of the firmware), `devices/<id>/rom.state`,
  `name` and `machine`, under `$MH_DEVICES_DIR`, else
  `$XDG_DATA_HOME/magichat/devices`, else
  `~/.local/share/magichat/devices` (`%LOCALAPPDATA%\magichat\devices` on
  Windows).
- **When it is read.** At start, if the state file exists. A state this
  build cannot read is reported and the device boots from its ROM instead.
- **When it is written.** When the run ends normally, windowed or headless,
  and from the control rail's save button. On Android it is also written
  when the app goes to the background, since the system may end the
  process afterwards.
- **What turns it off.** `--temporary` neither reads nor writes the
  device's own state. `--fresh` ignores the saved state and starts from the
  ROM; the state is overwritten when the run ends. `--load-state` and
  `--save-state` mean the caller is managing states, so the device's own is
  left alone. On DataRover, `--reset-pc` and `--monitor` also bypass it.

`--temporary` still allows explicit `--load-state` and `--save-state`. Scripts
and measurements should use `--temporary` (or name their states), because
an ordinary headless run otherwise resumes from and overwrites the state
beside the ROM.

PC Cards are not part of the machine. A state records which card was in
each slot and where its image is, never the image itself. On resume, a
remembered card is reopened as its file is now; a card named on the command
line wins for its slot; a remembered card whose image has gone leaves the
slot empty. Writable SRAM images are written through to their own files.
A saved network card is restored with its host connection re-created, not
its sockets. On PIC-2000 a state cannot be saved while the experimental
`--net ne2000` probe is attached.

Save-state files begin with the magic `MHAT` and a version; a build refuses
versions it does not know. In the DataRover window, F3 also writes numbered
`gui0000.state` files to the current directory for exploration.

What is not modelled: elapsed real-time-clock time while the host process
is absent, external wake events during that time, and a hardware-accurate
distinction between warm reset and loss of all power.
