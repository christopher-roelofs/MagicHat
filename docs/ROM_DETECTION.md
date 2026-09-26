# ROM identification and machine selection

The main `mhat` launcher identifies the ROM contents before constructing a
machine. It calls the selected board's CLI in the same process; it does not
run a sibling executable or inspect the ROM filename, extension, or hash.

| ROM family | Architecture | Selected board / result |
| --- | --- | --- |
| DataRover USA and Japan | MIPS | DataRover 840 |
| Rosemary SDK | MIPS | DataRover 840 |
| Sony PIC-2000 | 68k | PIC-2000 |
| Sony PIC-1000 | 68k | Identified, board unsupported |
| Sony HIX-300 | 68k | HIX-300 board (experimental; checksum recovery for original image) |
| Motorola Envoy | 68k | Envoy board (experimental; mc31 has additional limitations) |

Envoy recognition includes the mc31 image's `1,0.31,MOTO,1,` version and
separate `Envoy` model string. Recognition does not imply keyboard support:
mc31 remains gated; 1.0/pt4 and HIX-300 are validated. See
[68k keyboards](68K_KEYBOARD.md).

Supported machines open a GUI by default when SDL can initialize video.
`--headless` disables it; `--gui` explicitly requests it. Board-specific
options still belong to their respective parsers. For example, the 68k
machines do not implement every DataRover networking option. Request 68k
options with `mhat --device hix300 --help`, `--device envoy --help`, or
`--device pic2000 --help`.

## Evidence used

The implementation is in [identify.c](../src/rom/identify.c).

MIPS recognition combines the initial MIPS jump (`08f00007`), the
`IDT MONITOR ` header at offset 12, CP0 Status/Cause initialization at offsets
32/36, and the Apollo platform string within the first 128 KiB of monitor
code/data. All three local MIPS images satisfy these checks. RAM-size
instruction immediates and total ROM length are not identity checks, so
changing the supported RAM constants does not change machine selection.
Existing RAM detection still runs after selecting the board.

68k recognition first validates a 4 MiB ROM, plausible aligned initial stack
and reset vectors, and the reset instruction sequence that loads SP, clears
A0, and initializes USP. Device metadata must agree with that layout:

- Sony ROMs map at `0x0e000000`. PIC-2000 uses `,SONY,2,` metadata together
  with `PIC-2000` text. PIC-1000 uses `,SONY,1,` and `PIC-1000` text.
- HIX-300 also uses `,SONY,1,`, distinguished by its `HIX-300` text.
- Envoy maps at `0x02400000`, with `,MOTO,1,` and `Motorola Envoy` text.

PIC-2000 contains legacy PIC-1000 diagnostic strings. Those strings alone
must not select PIC-1000; metadata and reset structure are checked together.
Conflicting manufacturer/model evidence is rejected as unknown/ambiguous.

These are conservative signatures derived from the local ROM corpus, not a
complete specification of all Magic Cap ROM headers. Text may occur in
bundled packages, and future or heavily modified images may be ambiguous or
unrecognized. Detection is not authenticity verification or a promise that
an identified ROM will boot correctly. Alternate byte order, ROM containers,
and other reset layouts are not automatically normalized.

## Explicit selection and builds

`--device auto` is the default. `--device datarover840`, `--device pic2000`,
`--device hix300`, or `--device envoy` allows an unrecognized experimental ROM
to use a supported board. A known
conflicting device is rejected, including attempts to run identified
PIC-1000/HIX-300/Envoy images as PIC-2000 through the unified launcher.

There is one `mhat` executable. It selects the DataRover, PIC-2000, HIX-300,
or Envoy board from the ROM, unless `--device` explicitly selects a compatible
board. PIC-1000 is identified but has no runnable board yet.

The PIC-2000, Envoy, and HIX-300 machines use the project-owned CPU32 core;
their build does not require Moira.

## Validation

`ctest --test-dir build -R 'rom_detection|datarover_ram|m68k_cli' --output-on-failure`
checks launcher behavior and the existing RAM/68k CLI tests. Detection tests
copy local ROMs to an arbitrary filename and alter a byte outside the
identification data; these still select the expected devices. Other checks
cover empty/truncated images, misleading filenames, text without valid reset
code, conflicting metadata, explicit overrides, and command arguments that
contain launcher-option text. Corpus-dependent tests skip if ROMs are absent.
