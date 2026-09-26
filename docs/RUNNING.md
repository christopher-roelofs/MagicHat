# Running MagicHat

This is a usage guide for `mhat`: choosing a machine, devices and saved
sessions, the window's controls, installing software, sound, and the
helper scripts. Building is covered in the [README](../README.md). ROM
paths below such as `roms/MagicCap-USA.image` are placeholders for wherever
you keep your own firmware; ROMs are not part of this repository.

For every option the current binary accepts:

```sh
./build/mhat --help                   # DataRover 840 (MIPS)
./build/mhat --device pic2000 --help  # 68k machines
```

## Choosing a machine

`mhat --rom <path>` detects the architecture and device from ROM contents,
not the filename or a file hash. DataRover 840 ROMs (including the Rosemary
SDK image) run on the MIPS board; PIC-2000, Envoy and HIX-300 ROMs run on
the 68k board. PIC-1000 is recognized but not yet runnable. See
[ROM_DETECTION.md](ROM_DETECTION.md) for the evidence and limits.

Unknown or ambiguous ROMs need an explicit selection:

```sh
./build/mhat --device datarover840 --rom roms/experimental.image
./build/mhat --device pic2000 --rom roms/experimental.rom
```

`--device` accepts `auto` (the default), `datarover840`, `pic2000`,
`envoy` and `hix300`. An override that conflicts with a recognized device
is rejected.

### DataRover 840

```sh
./build/mhat --rom roms/DataRover-840-USA.image
```

The first launch boots from the ROM and asks for calibration; click the
targets as they appear. Later launches resume from the saved session. Host
battery reporting and sound are on by default. `--no-host-battery` reports
a healthy battery instead, for repeatable tests; `--no-audio` mutes the
window.

`--monitor` boots to the IDT monitor prompt instead of Magic Cap (see
[STARTUP.md](STARTUP.md)), and `--console` bridges the host terminal to
UART A for it.

### 68k machines

```sh
./build/mhat --rom roms/PIC-2000.rom
```

This selects the 68k emulator and opens a window. Envoy and HIX-300 have
ROM-specific limits; see [ENVOY.md](ENVOY.md), [HIX300.md](HIX300.md) and
[PIC2000.md](PIC2000.md). `--quiet-unknown` stops undecoded device accesses
being printed as they happen. `--ac-adapter on|off` sets the Envoy's
adapter connection instead of following the host.

The 68k path uses its native block/JIT engine by default. Use
`--cpu-engine interpreter` for single-step execution through the CPU32
core, or `--cpu-engine blocks` for cached interpreted blocks. See
[M68K_CORE.md](M68K_CORE.md).

## Devices

A device is a machine you own, not a file you open. The devices button on
the control rail lists them and makes new ones. Choosing **New device**
opens a file picker, and the firmware you choose is *copied into* the
device, so the original can be moved, renamed or deleted afterwards
without affecting it.

A new device is named after the machine the firmware turns out to be --
"Sony PIC-2000", "Motorola Envoy", "Oki DataRover 840" -- rather than after
the file. A second device of the same kind counts up. An image the emulator
cannot identify is refused rather than filed as a device that will not
start; experimental images still run from a path with `--device`.

Run the emulator with no arguments and it resumes the most recently used
device, or opens the chooser if there are none:

```sh
./build/mhat
```

Running a ROM straight from a path is still just running a ROM: it keeps
its own `.state` beside the image and never becomes a device, so a one-off
experiment does not change what `mhat` opens next time.

Each device keeps everything it needs in a directory of its own:

```
devices/<id>/rom        the firmware, copied in when it was made
devices/<id>/rom.state  the machine, written whenever it is put down
devices/<id>/name       what to call it
devices/<id>/machine    which board it is
```

The `devices` directory is `$MH_DEVICES_DIR` if set, else
`$XDG_DATA_HOME/magichat/devices`, else `~/.local/share/magichat/devices`
(`%LOCALAPPDATA%\magichat\devices` on Windows). A device can also be run
from the command line by its firmware path:

```sh
./build/mhat --rom ~/.local/share/magichat/devices/sony-pic-2000/rom
```

Choosing another device from the list puts the running one down -- saving
it -- and starts the other in the same window, whether or not it is the
same kind of machine.

## Putting a device down and picking it up

A device saves everything it was doing when you close it, and starts from
there next time. Nothing needs to be switched off or confirmed: closing the
window, or pressing Ctrl+Q, saves the whole machine as it is and stops.
Reopening finds it awake, mid-screen, where you left it.

The file sits beside the firmware with `.state` in place of its extension:
`PIC-2000.rom` keeps `PIC-2000.state`, `DataRover-840-USA.image` keeps
`DataRover-840-USA.state`. It is the whole machine -- memory, processor,
every device and the battery-backed store. The ROM is still required and is
never changed. A state refuses to load into a machine whose ROM size,
memory size or emulator build layout differs, but it does not check that
the ROM is the same image, so keep each state with its ROM. On Android,
going to the background saves as well. An abrupt process kill cannot save
the file. See [POWER_PERSISTENCE.md](POWER_PERSISTENCE.md) for the details
and the hardware behind them.

To start from the ROM instead, replacing the saved session when the run
ends:

```sh
./build/mhat --rom roms/DataRover-840-USA.image --fresh
```

To run without keeping anything, leaving the saved session alone:

```sh
./build/mhat --rom roms/DataRover-840-USA.image --temporary
./build/mhat --rom roms/PIC-2000.rom --fresh --temporary
```

`--load-state` and `--save-state` are for driving experiments from the
outside. A run that names either of them is managing its state itself, so
the device's own `.state` is neither read nor written. On the DataRover,
`--reset-pc` and `--monitor` bypass it too, since both start somewhere
other than where the machine stopped.

```sh
mkdir -p states
./build/mhat --rom roms/DataRover-840-USA.image \
  --save-state states/my-datarover.state
./build/mhat --rom roms/DataRover-840-USA.image \
  --load-state states/my-datarover.state \
  --save-state states/my-datarover.state
```

Close the window normally to let `--save-state` finish. Always pair a
state with the ROM that made it: the Rosemary SDK and shipping DataRover
ROMs need different states. State compatibility across emulator revisions
is not guaranteed. Two sessions on one device should not overlap: both
write the same file when they end, and the last one wins.

### Cards

A card left in a slot is still there when you come back:

```sh
mkdir -p cards
./build/mhat --rom roms/DataRover-840-USA.image --sram1 cards/mycard.img
./build/mhat --rom roms/DataRover-840-USA.image   # still in
```

The state records the card's *name*, never a copy of it. An SRAM card's
contents live in its own image file, which the emulator writes through as
the guest works. A network card is remembered the same way, along with what
it was plugged into. The card is reinserted on resume, so the guest sees a
departure and an arrival and re-reads the card.

Naming a card on the command line overrides what was remembered for that
slot. `--fresh` starts with both slots empty. If a remembered image has been
moved or deleted, the slot is simply empty and the emulator says so. A card
named explicitly is different: `--card1` on a missing file is an error,
while `--sram1` on a missing file creates a blank 2 MiB card, which is how
you make one. Cards can also be inserted, ejected and created from the
Settings panel's **PC Cards** entry. See [STORAGE.md](STORAGE.md) and
[NETWORKING.md](NETWORKING.md) for storage, NE2000 and modem cards.

## The control rail

Both windows carry a vertical strip of buttons down one edge, drawn by the
emulator itself, so it looks and behaves the same on the desktop as on
Android. The rail takes space from the guest rather than floating over it,
because Magic Cap uses its screen out to the edges.

- **Devices** lists devices, switches between them and makes new ones.
- **Install** sends a package to the guest (below).
- **Settings** chooses which side the rail sits on, opens the PC Cards
  panel, and presses the device's power button.
- **Rotate** turns the panel a quarter turn each press.
- **Display** opens the dot structure, backlight tint and scaling
  settings; press it again, or tap the guest, to put it away.
- **Save** writes the device's state immediately (shown when the session
  has a state to write).
- **Option** holds or releases the Option key, on machines that have one.

The display settings are also on both command lines, so they mean the same
thing whichever machine is running: `--lcd` (dot structure and STN
ghosting), `--tint green|amber|grey|none`, `--smooth` and `--integer`.

```sh
./build/mhat --rom roms/PIC-2000.rom --lcd --tint green
```

The rail sits on the left unless the Settings panel or the environment says
otherwise:

```sh
MH_UI_RAIL_SIDE=right ./build/mhat --rom roms/PIC-2000.rom
```

## Window controls

| Action | DataRover | 68k machines |
| --- | --- | --- |
| Touch the screen | Left click; hold and drag for a pen stroke | Left click; hold and drag |
| Option key | Right mouse button, or the rail | The rail, where offered |
| Screenshot | F2 (`shotNNNN.pgm`) | F2 (`<machine>-NNNN.pgm`) |
| Save a numbered state | F3 (`guiNNNN.state`) | Unavailable |
| Power button | F4 | F4 |
| Fullscreen | F11 or Alt+Enter | F11 |
| Quit (saves the device) | Close the window or Ctrl+Q | Close the window or Ctrl+Q |

Screenshots and F3 states are written to the current directory, and their
names are printed in the terminal. F3 numbering restarts each launch; use
`--save-state` to keep a named session. F4 turns the DataRover off with a
blank display, and pressing it again resumes the guest; the window stays
open while the device is off. `--touch-debug` draws a cross where the
emulator puts the pen.

## Keyboard

Host typing goes to an emulated Magic Bus keyboard by default in the window.
Click into a guest text field and type normally; Shift, Control,
navigation and keypad keys travel through the stock keyboard driver, and
the guest chooses the layout. The on-screen keyboard remains available.
This does not occupy a card slot.

- `--keyboard` attaches the keyboard explicitly, for example in a headless
  run.
- `--no-keyboard` disconnects it. On the DataRover, window typing then goes
  to UART A instead.
- `--monitor` on the DataRover keeps UART console typing by default.

PC Link uses UART A on the DataRover, so use the keyboard route while
running PC Link. A saved state restores an attached keyboard. See
[KEYBOARD.md](KEYBOARD.md) and [68K_KEYBOARD.md](68K_KEYBOARD.md).

## Installing software

The guest receives packages over its own serial link, and the emulator is
the computer at the other end -- there is no second program to run and no
port to wire up.

From the window: the Install button on the rail opens a file list, and
choosing a package starts the transfer. The panel then shows what the link
is doing and how many bytes have gone. Magic Cap only listens once someone
opens its Storeroom and taps the computer, so a transfer that has not
started yet is waiting on the guest, and the panel says so.

From a command line, `--install` does the same thing headlessly. It reports
what happened and exits non-zero if the guest never took the package:

```sh
./build/mhat --rom roms/MagicCap-USA.image \
  --load-state states/install-room.state --headless --temporary \
  --install packages/Reversi.pkg -n 5000000000 \
  --tap-px 46 162 50000000 --tap-hold 1500000
```

The state must show the Storeroom with its computer at the tapped
coordinates. The 68k form is similar; it stops after the transfer completes
and one emulated second of guest settling, and `-n` is the maximum budget:

```sh
./build/mhat --rom roms/PIC-2000.rom \
  --load-state states/pic2000-storeroom.state --headless --temporary \
  --install packages/Counter.pkg \
  --tap 5000000,1000000,44,157 -n 2000000000
```

`scripts/install-package` and `scripts/install-package-68k` wrap these and
write a log, framebuffer and state to an output directory. Transfer
completion does not establish that installation succeeded: inspect the
guest screen and launch the app.

Packages are published under several extensions -- `.pkg`, `.package`,
`.mc2` -- and what decides is the container, checked before a single byte
goes on the wire:

- MIPS packages use the `SALTCOD` container.
- 68k packages are a flat `CLUS` data fork; PC Link wraps it in an
  `MPkg`/Wireline `FrozenPackage` stream and sends it over PPP/GMTP.
- An already wrapped `MPkg` stream is accepted as is, including one inside
  a version-0 `MCap` distribution envelope (`.cap`). The envelope's name
  header is removed and the embedded stream is sent unchanged.

A file that is none of these is refused immediately. Accepting a file does
not establish the package's compatibility with a particular ROM or satisfy
its dependencies.

### Transfer speed

The DataRover link is a 9600-baud serial port and it is emulated
faithfully, so a 126 KB package really is about two minutes of device time.
The emulator's own speed is not the limit: the native CPU engine, the
default where executable memory is available, took 16 s for that transfer
against 1m58s with `--cpu-engine interpreter`.

The 68k PC Link runs at 115200 baud by default, which keeps larger packages
practical without changing the guest-visible DUART divisor. For timing
experiments, set `MH_68K_BAUD` to a numeric rate, or to `guest` to use the
ROM-programmed rate. This changes emulator timing only; with `guest`,
larger packages can exhaust the instruction budget before the guest
acknowledges the final transfer.

## Sound

Sound is on by default in both windows.

- DataRover: `--no-audio` silences the window; `--audio-wav <path>`
  captures the codec output to a WAV file. See [AUDIO.md](AUDIO.md).
- 68k: `--audio` / `--no-audio` switch SDL speaker output; `--wav <path>`
  captures it in a headless run. `--no-pic-audio-approx` disables the
  approximate volume/filter response to compare against raw PCM. See
  [PIC2000_AUDIO.md](PIC2000_AUDIO.md).

```sh
./build/mhat --rom roms/PIC-2000.rom \
  --temporary --headless -n 200000000 --wav pic-speaker.wav
```

Sounds must be requested by the guest; an idle session can produce silence.

## If no window appears

- Launch from a terminal on your graphical desktop. Both frontends prefer
  Wayland when `WAYLAND_DISPLAY` is set; an explicit `SDL_VIDEODRIVER`
  setting still overrides this.
- Check that CMake reported SDL2 when the build was configured, and rebuild
  after installing SDL2 development files if needed.
- Omit `--headless`, and omit `-n` for interactive use: it limits execution
  and can close the emulator before startup finishes.

Without a display the emulator runs headless; read the terminal output for
the reason.

## Helper scripts

These live in `scripts/` and assume `build/mhat` and the ROM and state
paths shown, each overridable. Scripts that run the emulator pass
`--temporary` or name their own states, so they never touch a device's
saved session.

- `scripts/mkstates [rom] [dir]` boots the DataRover to the monitor, boot
  screen, calibration, Desk and clean Desk, saving `states/*.state`. Most
  other scripts start from these.
- `scripts/regress` walks a cold ROM to the Desk and checks each milestone
  fingerprint; `scripts/ostest` checks OS-level fingerprints.
- `scripts/mon "COMMAND" [budget]` runs one IDT monitor command from
  `states/monitor.state` and prints the result.
- `scripts/sweep "OPTS" ["OPTS" ...]` runs whole-boot probes in parallel,
  one per option string, and prints boot pixels and a Desk hash for each.
  Output goes to `sweep-out/`.
- `scripts/tryopts` runs the whole boot path with options in force
  throughout.
- `scripts/uicrawl` maps the Magic Cap UI by tapping a grid on each screen,
  starting from `states/cleandesk.state`, and writes screens and
  `graph.tsv` to `tests/uimap/`. `--coverage` records per-edge coverage
  for `scripts/covdiff`. `scripts/test_tolerance` unit-tests its
  screen-matching tolerance against `tests/fixtures/`.
- `scripts/benchmark-mips` times the MIPS interpreter against the JIT, or
  two builds against each other, and refuses results whose guest artifacts
  differ. See [MIPS_PERFORMANCE.md](MIPS_PERFORMANCE.md).
- `scripts/fbdiff`, `scripts/covdiff` and `scripts/ramdiff` compare
  framebuffers, coverage files and RAM dumps between runs.
- `scripts/type` turns text into taps on the on-screen keyboard;
  `scripts/calibrate` finds each calibration target and taps its centre,
  the way a person would.
- `scripts/patch-rom` applies a patch manifest to a copy of a ROM; see
  [MODIFICATIONS.md](MODIFICATIONS.md).

Run each with `--help` or read its header comment for the full usage.
