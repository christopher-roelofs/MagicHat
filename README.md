# MagicHat

MagicHat is the low-level Magic Cap emulator for DataRover 840 (MIPS) and
Sony PIC-2000, Sony HIX-300, and Motorola Envoy (68k). It models the CPUs,
boards, peripherals, PC Cards, host I/O, and SDL/Android frontends. HIX-300
and Envoy support has ROM-specific experimental limits; PIC-1000 is recognized
but does not yet have a working board. See [ROM support](docs/ROM_DETECTION.md)
for details. Magic Cap application-development tools live in the separate
[Hatter](https://github.com/christopher-roelofs/Hatter) repo.

## Screenshots

| PIC-2000 (one of the 68k targets) | Magic Cap 3 on MIPS, running TicTacToe |
| --- | --- |
| ![PIC-2000 Magic Cap touch-to-begin screen](docs/images/pic2000-touch-to-begin.png) | ![MIPS Magic Cap TicTacToe screen](docs/images/mips-tictactoe.png) |

The [networking guide](docs/NETWORKING.md) shows the DataRover's connection
setup and browser running through the emulated NE2000 card.

## Build and run

On Linux, install a C/C++ compiler, CMake, SDL2 development files, and
optionally libslirp for user-mode networking. Then:

```sh
cmake -S . -B build
cmake --build build -j
ctest --test-dir build --output-on-failure
./build/mhat --rom /path/to/your/rom
```

On Windows, build in the [MSYS2](https://www.msys2.org/) UCRT64 environment.
From an "MSYS2 UCRT64" shell:

```sh
pacman -S --needed mingw-w64-ucrt-x86_64-{gcc,cmake,ninja,pkgconf,SDL2,libslirp,python}
cmake -S . -B build -G Ninja
cmake --build build
ctest --test-dir build --output-on-failure
./build/mhat.exe --rom /path/to/your/rom
```

That `mhat.exe` needs `C:\msys64\ucrt64\bin` on `PATH` for SDL2, GLib and
the compiler runtime. To build one that runs on any Windows 10 or 11 machine
with nothing else installed, link everything in statically. Use the patched
libslirp (see below) and add `diffutils` to the packages above:

```sh
scripts/build-slirp
PKG_CONFIG_PATH="$PWD/build/slirp-fixed/lib/pkgconfig" \
    cmake -S . -B build-static -G Ninja -DMH_STATIC_DEPENDENCIES=ON
cmake --build build-static
```

`build-static/mhat.exe` is then the whole emulator in a single file. It
imports only DLLs that are part of Windows. Each Windows build also makes
`mhatw.exe`, the same program without a console window, for starting from
Explorer or a shortcut. Use `mhat.exe` from a terminal, where its messages
appear.

The Windows build has a few differences from Linux:

- `--serial a` needs a pseudo-terminal, which Windows lacks. The in-process
  PC Link transfer still works.
- Stock libslirp has a TCP MSS bug that the `network` test catches. Install
  `git` and `mingw-w64-ucrt-x86_64-meson`, then run `scripts/build-slirp`
  exactly as on Linux. CMake copies the patched `libslirp-0.dll` next to the
  programs.

ROMs, memory cards, saved states, and packages are user data. They are not
included in this repository or required to compile it. Keep them outside
the checkout or under the ignored `roms/`, `cards/`, and `states/` directories.
The emulator detects supported machines from ROM contents; see
[ROM support](docs/ROM_DETECTION.md).

For Android, run `scripts/build_android.sh` with the Android SDK and NDK
installed. The app identifier is `org.magichat.app`. Earlier builds used a
different identifier, so Android installs this one as a new app rather than
as an update, and does not carry over the old app's data. See
[Android setup](docs/ANDROID.md).

## Documentation

Using the emulator:

- [Running MagicHat](docs/RUNNING.md): the control rail, devices, saving,
  installing packages, and helper scripts.
- [ROM support](docs/ROM_DETECTION.md), [storage cards](docs/STORAGE.md),
  [networking](docs/NETWORKING.md), [Android](docs/ANDROID.md).

DataRover 840 (MIPS):

- [Hardware](docs/HARDWARE.md), [startup](docs/STARTUP.md),
  [accuracy](docs/ACCURACY.md), [audio](docs/AUDIO.md),
  [keyboard](docs/KEYBOARD.md), [power and persistence](docs/POWER_PERSISTENCE.md).
- [MIPS JIT](docs/MIPS_JIT.md), [MIPS performance](docs/MIPS_PERFORMANCE.md),
  [SDL display](docs/SDL_DISPLAY.md).
- Machine modifications: [overview](docs/MODIFICATIONS.md),
  [RAM size](docs/RAM_EXPERIMENT.md), [deeper screen](docs/DEEP_SCREEN.md).

68k machines:

- [PIC-2000](docs/PIC2000.md), [Envoy](docs/ENVOY.md), [HIX-300](docs/HIX300.md),
  [PIC-1000](docs/PIC1000.md), [parity between boards](docs/68K_PARITY.md).
- [Magic Bus controller](docs/PIC2000_MAGICBUS.md),
  [keyboards](docs/68K_KEYBOARD.md), [audio](docs/PIC2000_AUDIO.md),
  [networking](docs/68K_NETWORKING.md).
- [68k core](docs/M68K_CORE.md), [68k performance](docs/M68K_PERFORMANCE.md).

Project:

- [Methodology](docs/METHODOLOGY.md): how the machines are modelled without
  patching the guest.
- [Open questions](docs/OPEN_QUESTIONS.md): what is still unknown.

## Repository layout

- `src/`: emulator CPU/JIT, buses, devices, machines, host services, and UI.
- `tests/`: emulator unit and integration tests; no ROMs or saved states.
- `android/`: Android wrapper and Gradle build files.
- `scripts/`: emulator build, testing, and runtime utilities.
- `patches/`: optional ROM and network-dependency patch recipes.
- `assets/licenses/`: licenses for embedded UI fonts.
- `docs/`: emulator operation and implementation notes.
