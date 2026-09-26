# Writable PCMCIA storage

Internal `.bram` persistence is enabled. Full save states and PC Card images
are separate storage mechanisms; the card behavior is described below.

DataRover supports a persistent SRAM card in either PCMCIA slot. Use
`--sram1 PATH` or `--sram2 PATH`. Slot 1 can hold the NE2000 Ethernet card
while slot 2 holds storage:

```sh
mkdir -p cards
./build/mhat --rom roms/MagicCap-USA.image \
  --load-state states/desk.state \
  --net user --sram2 cards/storage.img
```

Use any saved Desk state; `scripts/mkstates` makes `states/desk.state`.
The same card options also work without a snapshot. The NIC still requires
its guest driver and provider configuration; see [NETWORKING.md](NETWORKING.md).
The 68k frontend now accepts the same `--sram1` / `--sram2` options. Its ROM
declares slot common-memory windows at `0x04000000` / `0x08000000` and
attribute windows at `0x24000000` / `0x2C000000`; the emulator maps the SRAM
card's CIS to attribute space and its writable image to common memory. This
is the first 68k card implementation and is intentionally limited to SRAM:
Card-detect, ready, and healthy-card-battery inputs are asserted through the
ROM-observed `dev21+EE` bits for a seated card. The generated SRAM CIS is
writable, so its write-protect tuple is clear and the lock-switch input is
reported unlocked. The internal API can eject a card, which releases those
inputs and gates the windows. Full controller IRQ and guest-visible
hot-removal event behavior still need board measurement. It applies to the
PIC-2000, Envoy, and HIX ROM layouts listed in the 68k card-slot inventory.

## First setup

A missing image is created as a blank, zero-filled **2 MiB** file. It is not
preformatted by the emulator. Magic Cap must format it and create its own
storage structures.

For this card's valid SRAM CIS, the stock ROM rejects a blank common-memory
header unless Option is held during insertion. The supported guest procedure
is documented under “Erase everything from a storage card” in *Using Magic
Cap*, printed page 213. Schedule that physical key press and insertion when
starting from an awake Desk:

```sh
./build/mhat --rom roms/MagicCap-USA.image \
  --load-state states/desk.state \
  --sram2 cards/storage.img --card-at 50000000 \
  --option-key '1,10000000;0,200000000'
```

Tap **set it up**, type a name, optionally check **new items go here**, and
tap **done**. This erases the selected card, so use the first-setup command
only when formatting is intended. On later launches omit `--option-key`
and `--card-at`; the existing image is reopened without reformatting.
SDL right-button hold also drives Option. Scheduled inputs retain their
instruction-count origin across SDL's short execution calls.

The card appears as shelves in the Storeroom. Magic Cap stores its native
packages there; the image is not a host folder or a FAT disk. Existing
`--card1` / `--card2` options still expose the legacy read-only image model.
They are separate from `--sram1` / `--sram2`.

## Persistence and limits

- The image holds common-memory bytes only. Its size determines capacity;
  the attribute CIS is generated from that size.
- Existing files are never resized. The backend accepts powers of two from
  64 KiB through 64 MiB. **Guest formatting has been verified at 2 MiB**;
  acceptance by the backend does not establish every ROM's capacity limit.
- Guest writes update a shared file mapping. Normal exit flushes the image
  with `msync` and `fsync`, and reports failure. An exclusive file lock
  prevents cooperating emulator instances from writing the same image.
  Copy or back up the image with the emulator stopped.
- DataRover's whole-machine suspend still rejects unsupported card-state
  combinations as described by its frontend. The 68k snapshot retains the
  machine state separately from the external SRAM image; reopen the same
  image with `--sram1`/`--sram2` when restoring a 68k state.
- ROM/flash persistence and the snapshot format have not been reorganized.
  Card timing, full Glacier window remapping, battery discharge and dynamic
  removal are not complete hardware models.

## Hardware evidence

No ROM instructions, guest RAM, or snapshot contents are patched to make
storage work. The stock ROM performs formatting, naming, allocation and
capacity detection.

| Input or space | Evidence in stock USA ROM |
| --- | --- |
| Attribute CIS in window A, even byte addresses | `13C34288` probe and tuple-reader traces |
| Common memory in window B | `13C32A78` header copy; `13C32074..13C32094` alias-based size probe |
| SRAM DEVICE tuple, type 6 | Classifier at `13C329FC..13C32A04` |
| Option: TX39 IOCTRL input 3, active low | `13C268E0`, called from global selector `005B` / `13C64CD8` |
| Ready: Glacier status `+0C`, bit 2 | `13C338D4`, readiness poll at `13C34244` |
| Write protect: Glacier status bit 3 | `13C33924`, selector `0511`, stored by `13C32B88` |
| Card battery | `13C346CC`: Glacier status bit 1 plus TX39 IO1 for slot 1 / IO0 for slot 2; both high return healthy state 3 |

The earlier identification of codec GPIO8 as Option was not supported by the
ROM's direct key query. The frontend now uses IO3. The cause of the older
GPIO8 touch effect remains open; its framebuffer-difference assertions were
retired as Option tests. CPU tests exercise the actual IO3 read, release,
and input scheduling instead.

## Validation, 2026-09-11

With the stock USA ROM and the local card-free browser Desk fixture:

1. Inserted a new 2 MiB image while holding physical Option, then tapped the
   guest's setup button. Magic Cap probed memory aliases, wrote its own CIS
   into common memory and an `MCAP` header at offset `0x58`, and reported
   **2,009 KB available**.
2. Used the onscreen keyboard to name the card **Card**, enabled new items,
   and completed setup. The Storeroom showed a **new items** package.
3. Closed the process, reopened the same image from the original card-free
   Desk snapshot, and entered the Storeroom. The name and package survived.
4. Reopened the same image in slot 1; the guest accepted it there too.
5. Attached NE2000 in slot 1 and SRAM in slot 2. Web Browser loaded the
   host's HTTP page through Ethernet/TCP while storage remained attached.

Host tests cover big-endian byte lanes, capacity wrapping, read-only
attribute memory, file reopening, exclusive locking, invalid file sizes,
Glacier input preservation and edges, and physical Option scheduling.
