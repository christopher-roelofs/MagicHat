# Modifications: changing the machine on purpose

This document is about deliberately building a machine General Magic never
shipped -- more RAM, code in the free flash, features the device never had
-- and doing it without spoiling the emulator's ability to tell the truth
about the machine it did ship.

It is not about making stock functionality work. That distinction is the
whole subject, and [METHODOLOGY.md](METHODOLOGY.md) rule 1 draws it: a
**shim** covers for something we have not understood, and is banned; a
**modification** changes what the machine is, and is fine as long as it is
declared. If the stock ROM will not do something, the bug is in a device
model.

The RAM work below used the shipping USA DataRover image
(`DataRover-840-USA.image`, Magic Cap 3.1.2j) and the DataRover 840
flasher. Measured findings and unverified possibilities are identified
separately. It is about the DataRover; the 68k machines have
not been examined for any of this, and none of these findings should be
assumed to carry across to a different board running an operating system
four years older.

## Nothing verifies the ROM

There is no integrity check anywhere in the boot path. The only checksum
code in the image is the monitor's interactive `checksum` command and
S-record download validation (`record %d: checksum error, calculated 0x%x,
received 0x%x`). Neither runs unless asked.

The update container is equally open. `DataRover840FRomFlasher.gz` from the
archive decompresses to exactly 8,388,608 bytes:

```
00000000: 426f 7773 6572 4c69 7665 7300 0000 0001  BowserLives.....
00000010: 0000 0000 b3c0 0000 0045 1817 0000 0000  .........E......
                    ^load addr  ^length = 4,528,151
```

Header at `0x000`, payload at `0x400`, byte-identical to the ROM image. A
magic, a version, a load address and a length -- and no checksum field. A
modified image of a different size needs one 32-bit header edit to flash
onto real hardware.

## A screen deeper than two bits

The panel is greyscale and the LCD controller has no colour table, but the
screen depth is one `AllocPixels` argument in the ROM and Magic Cap's
raster layer really does support colour. See [DEEP_SCREEN.md](DEEP_SCREEN.md)
for the constants, what 8-, 15- and 24-bit screens produce, and the one
thing that still goes wrong.

## The flash is 8 MB and 3.68 MB of it is empty

Everything above the payload in that file is `0xFF`: 3,859,433 bytes of
erased flash, addressable at `0x84051818`-`0x843FFFFF` and again at
`0x14051818` on the second chip select. The parts really are 8 MB -- the
Japanese image is 5.8 MB, which the 4 MB reading cannot explain.

The image itself has almost no slack. A scan for runs of a repeated byte
found exactly one over 1 KB: 1,256 zero bytes at image offset `0x2A49E8`.
So injected code goes above `0x451817`, and something has to reach it -- a
patched call site, or a class-table entry.

One emulator caveat. `mh_bus_add_flash` mirrors the 4.32 MB image through
the 8 MB window, so reads above the image return mirrored data rather than
`0xFF`. Pad an experimental image out to 8 MB with `0xFF` and the mapping
becomes direct, which is what the board does.

## RAM size is three constants

The ROM does not size DRAM by writing a pattern and watching it alias. With
a stock ROM, exposing 8, 16 or 32 MiB still prints 4 MB, because:

```
83c002a0:  3c020040   lui  v0,0x40        # 0x00400000, a literal
83c002a8:  ac22c180   sw   v0,-16000(at)  # -> RAM 0x0000C180
```

and `Memory: %d` at `0x83C0A054` reads `0xC180`. The monitor reports a
constant.

Magic Cap has its own, separate notion -- a board-ID to size table:

```
83c1ec88:  jal 0x83c25c24        # board id
83c1ec94:  bne v0,v1,0x83c1eca0  # v1 = 5
83c1ec98:  lui a0,0x20           # 2 MB, in the delay slot
83c1ec9c:  lui a0,0x40           # 4 MB
```

`0x83C25C24` is `li v0,-1; li v0,5; jr ra` -- hardcoded, with a dead first
instruction that looks like a build-time override. The DataRover is board
5, so 4 MB.

Three words, and the machine has 8 MB:

| image offset | was | now | what it sets |
|---|---|---|---|
| `0x0002A0` | `3c020040` | `3c020080` | monitor's reported DRAM size |
| `0x0002AC` | `3c020040` | `3c020080` | companion global at `0xC1C4` |
| `0x01EC9C` | `3c040040` | `3c040080` | board-ID to size table |

With 8 MiB exposed the monitor prints `Memory: 8388608 (0x800000)` and
reaches its prompt with zero exceptions and zero bus faults, and the full
boot -- boot screen, calibration, Desk -- completes normally.

The OS really uses it. The framebuffer moves:

```
stock 4 MB:   framebuffer 480x320 @ 2bpp from 003F6A00
patched 8 MB: framebuffer 480x320 @ 2bpp from 007F6A00
```

Exactly `+0x400000`. Magic Cap places the framebuffer at top-of-RAM minus
`0x9600`, so it read the new size, believed it, and allocated into 4 MB
that had never been touched. The Desk renders identically apart from clock
hands and splash timing.

A fresh-boot comparison at **4, 16, 32 and 60 MiB** confirmed that the
Storeroom reports the extra memory as available built-in storage: 3,369K,
15,657K, 32,041K and 60,713K respectively. See
[the RAM experiment](RAM_EXPERIMENT.md) for the procedure, guest-activity
checks and limits of that evidence.

## Where patching the ROM is the right tool, and where it is not

Reset does not belong in this list: the ROM's existing jump works through
the boot flash alias, without patching or adding a stub. See
[STARTUP.md](STARTUP.md).

Worth doing:

- **Strings.** Data is addressed through the second chip select
  (`lui a0,0x13c2`), so a string's file offset is `addr - 0x13C00000`
  exactly. Equal-or-shorter in-place edits are safe and trivial.
- **Hardcoded board identity.** Changing what `0x83C25C24` returns
  exercises the ROM's other machine configurations, which is a way to find
  out what else the image supports.
- **Unreachable diagnostics.** The UCB1100 register self-test noted in
  [HARDWARE.md](HARDWARE.md) would give a second opinion on the whole codec
  model, and a patched call site can invoke it.

Not worth doing:

- **TLS.** [NETWORKING.md](NETWORKING.md) establishes there is no crypto of
  any kind in the image. Nothing about ROM patching changes the handshake
  cost on a 36.864 MHz R3900, and the browser is a package
  (`WebBrowser40.mc2`), so proxy support is a package patch. The proven
  route on this hardware is a TLS-terminating proxy in front of a modified
  browser. Free flash could host a crypto blob, but the browser would still
  have to call it.

## RAM exposure and offline ROM patches

The emulator's responsibility stops at exposing RAM. It chooses the
allocation in this order: explicit `--ram`, saved-state header, recognized
ROM size constants, then the stock 4 MiB fallback. The selected size and
source are printed at startup. Detection reads metadata and instructions;
it never patches the loaded ROM, a saved state, or guest RAM to persuade
the OS to use memory.

ROM detection recognizes the IDT entry/signature, the board-5 size routine
and its three agreeing size constants at the offsets above. Unrecognized or
inconsistent layouts use 4 MiB unless overridden. This is not a generic RAM
size field present in every ROM. A snapshot's stored RAM size takes
precedence because the snapshot restores both RAM and flash; an explicit
conflicting `--ram` fails the snapshot checks rather than resizing the
state. The mapped DRAM window allows 1..60 MiB through the command line.
That is an address-space limit, **not** a claim that the supplied ROM or
original hardware supports every size.

The emulator has no option to patch a ROM. ROM modifications happen in a
separate offline operation before the emulator is launched:

```sh
mkdir -p roms/experimental
scripts/patch-rom patches/datarover-usa-8mb.json \
    roms/DataRover-840-USA.image roms/experimental/DataRover-840-USA-8mb.image
./build/mhat --rom roms/experimental/DataRover-840-USA-8mb.image
# --ram 8 is optional here: the patched constants are detected.
```

[The patch manifest](../patches/datarover-usa-8mb.json) identifies the
exact source SHA-256, offsets, original bytes and replacements. The tool
checks the source hash and all original bytes, then creates a **new**
output file. It refuses existing destinations, including the original ROM.
It has no interface to an emulator process or snapshot and is not invoked
by `mhat`. Manifests for 16, 32 and 60 MiB sit beside it.

For the shipping USA image:

- Source SHA-256:
  `94785cb334f14eac00ed200af014c35972b4f25694103bc6a49b3afa280a6f1b`
- Patched SHA-256:
  `c4ffafe4b75bd936bfa5a8d9eb635c70857dc8725e64d38657a414ee3fd25f05`
- Exactly the three instruction words listed above are changed; image
  length stays 4,528,151 bytes. This manifest does not apply to the
  Japanese image.

For another RAM size, prepare and validate a separate ROM patch
beforehand. Do not infer allocator/object-store compatibility from
successful allocation of host backing memory, or generalize this three-word
patch beyond tested ROMs.

### Snapshots carry their flash

A snapshot stores the flash along with RAM. Loading a state saved from a
patched machine with a command line naming the stock ROM restores the
patched flash, because the snapshot check compares sizes, not contents. So
`--rom stock.image --ram 8 --load-state patched8.state` appears to run an
8 MiB stock system; it is not `--ram` applying a patch.

Use a **fresh boot of the explicit patched ROM file** to test a RAM
expansion. Do not edit a snapshot or use one as the distribution format for
a ROM patch. Keep experimental ROMs and states separate from stock
regression fixtures. Snapshot ROM identity checking is not implemented.

The guest's own writes to flash during normal operation are a separate
issue: Magic Cap legitimately modifies its runtime flash contents, and
snapshots preserve those contents. The emulator does not identify or name
known ROM patch sets at launch; the explicit ROM filename and offline
manifest are the provenance for this workflow.

### Validation of the offline workflow

The manifest-generated 8 MiB ROM was freshly booted with no `--ram`, saved
state or serial input. The emulator detected 8,388,608 bytes from the ROM;
Magic Cap reached its startup screen with the framebuffer at `007F6A00`.
That screen was byte-identical to the stock 4 MiB startup screen. The
original ROM's SHA-256 remained unchanged, and the CTest suite covers patch
validation and RAM selection, override and snapshot-conflict behaviour.
These checks do not by themselves establish that Magic Cap's object store
grows with the added DRAM; the [Storeroom comparison](RAM_EXPERIMENT.md)
addresses that question.

## Reconstructed ROMs

A ROM rebuilt on purpose from a damaged dump plus donor code from other
machines is a modification in the sense of rule 6. It is not the machine
the manufacturer shipped, it is never substituted for the original dump,
and the emulator has no knowledge of it and no workaround for the damage.
The PIC-1000 reconstruction is described in [PIC1000.md](PIC1000.md).
