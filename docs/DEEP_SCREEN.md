# A screen deeper than two bits

The DataRover's panel shows four grey levels and Magic Cap dithers
everything into them. This is how much of that is the hardware and how
much is a handful of constants, what a deeper screen actually produces, and
the one thing that still goes wrong. It is a modification in the sense of
[MODIFICATIONS.md](MODIFICATIONS.md); nothing here describes the shipped
machine.

Everything here was measured on the **shipping** image,
`DataRover-840-USA.image`, Magic Cap 3.1.2j. Nothing was inferred from the
SDK build alone. MagicHat has no patch manifest for these edits; a deeper
screen needs an offline-patched ROM (the edits below) run with at least
8 MiB exposed, for example `--device datarover840 --ram 8` together with
the 8 MiB manifest.

## Magic Cap supports colour, and this ROM has the code

The Package Development Guide says so outright, in its "Colors" section:
"Although current Magic Cap communicators do not have color displays, Magic
Cap supports true color." That is a claim about the platform, so it was
checked against the image.

A small C test package makes its own offscreen `PixelMap`, fills it with
`rgbRed` and reads the pixel back; a second copy with one constant changed
serves as its control. Built with the Hatter toolchain, installed over
PC Link and opened through the guest's own UI:

    depth  2  ->  readback non-zero but not red   quantised to grey
    depth 24  ->  readback is EXACTLY rgbRed      colour preserved

Both on the SDK ROM and on the shipping one. The `pix888Color` raster
functions are there. Colour is lost at the screen, not in the software.

A useful by-product: an SDK-built package's class and operation numbers
carry to the shipping ROM unchanged, so installation failures on the
shipping ROM can be read as package problems rather than numbering
mismatches.

## The hardware cannot help

`src/soc/tx39/tx39_video.c` is a DMA scanout with no CLUT. `VID1_BITSEL`
selects 1, 2, 4 or 8 bits and nothing more, so even eight bits is 256 greys
on a real panel, and every device in scope -- DataRover 840, PIC-2000,
Envoy, HIX-300 -- has a greyscale STN. Colour has to come from above the
framebuffer, which is where the canvas is.

## The screen is one `AllocPixels` call

`DisplayServer.SetBuffer` at `0x13C1FEE0` is four instructions -- program
BITSEL and return. It never touches a `PixelMap`, which is why it stops at
eight bits: it is only the controller.

The screen canvas is built by an ordinary `AllocPixels`, and its depth is
the fourth argument:

    13c1d538:  li a3,2      screen canvas depth      file offset 0x1D53A
    13c1d53c:  li s1,18     Pixels_, the store class
    13c1d554:  jal ...      -> AllocPixels

Found by watching the `AllocPixels` entry `0x13D15184` through a cold boot:
call #1 (`ra=0x13C1D55C`, `a2=0x01000100`, resolution 256,256) is the
screen, immediately before `portDepth` is written. The depth argument is
not in the watch log, so read the caller.

Because `AllocPixels` sizes the `Pixels` object itself, the ROM allocates
the whole framebuffer from its own heap. **No package is needed** -- the
depth is one immediate.

## Read the geometry off the guest, do not compute it

After patching, the canvases carry `portRow` 1920 and `portPower` 5 at
depth 24: a 24-bit canvas is stored **32 bits a pixel, padded**, so the
screen needs 614,400 bytes and a renderer steps four bytes and skips the
first of each. Computing 480x3 gives the wrong answer twice over -- a short
buffer and a wrong stride. The object fields say what the ROM did:

| depth | portDepth | portPower | portRow | bytes |
|---|---|---|---|---|
| 2 | 2 | 1 | 120 | 38,400 |
| 8 | 8 | 3 | 480 | 153,600 |
| 15 | 15 | 4 | 960 | 307,200 |
| 24 | 24 | 5 | 1920 | 614,400 |

## Two things stopped it booting

**A bug that shipped in the ROM.** The `PixelMap` edge-scan method computes
bits-per-pixel as `1 << portPower` and shifts a word by it until the word
is empty. MIPS keeps five bits of a variable shift count, so at 32 it
shifts by nothing: `a0` sat at `0x00FFFFFF` on every iteration while the
counter wrapped. A greyscale device can never reach it. Two same-size loop
rewrites -- shift by `bpp-1`, then by 1 -- fix the right-edge `srlv` at
`0x13D17B24` and the left-edge `sllv` at `0x13D17B94` without changing any
depth that already worked. Only depth 24 needs them.

**A memory-layout collision.** The ROM keeps some 180 KB of live structures
just under the framebuffer, placed relative to *top of RAM*: `RAM[0xE8F4]`
is `0x3ED638` whether the framebuffer sits at `0x3F6A00` or `0x36A000`. A
bigger framebuffer at top-minus-size lands on them; the garbage band along
the bottom of every early render was those structures being read as pixels,
and drawing over them left the idle task with a stack pointer of
`0x19CED2D0`. Enlarging RAM alone does not help -- they move up with the
top. The framebuffer placement is computed at `0x13C1F358`:

    lui v0,0x1 ; lw v0,-8224(v0)        top of RAM
    lui v1,0xffff ; ori v1,v1,0x6a00    -38400
    addu v0,v0,v1 ; and v0,v0,-16       (top - 38400) & ~15

so with the documented 8 MB patch (three constants,
[MODIFICATIONS.md](MODIFICATIONS.md)) put it at top minus 4 MB =
`0x400000`, the upper half, which nothing top-relative or heap-relative
touches.

## What a deeper screen produces

Boot completes, calibration runs, the Desk draws. **Fills are correct at
every depth**, checked numerically rather than by eye: at 24 the toolbar
holds `00333333` and the wall `00555555`, and `0x555555` is exactly
`rgbLtGray` stored as `0RGB` in Magic Cap's inverted ink convention; at 15
the same regions hold `18c6`, which is (6,6,6)/31, the 5-5-5 equivalent.

**Depth 8 is the one worth having.** 221 grey levels in use, smooth shading
where there was dither, and the Desk is plainly itself.

## What is still wrong

Text and images are replicated horizontally by **(destination bytes per
pixel) / (source bytes per pixel)** -- the magic-hat logo is drawn four
times at depth 8 and sixteen at depth 24, counted on screen. Destination
stride and layout are correct; only the source advance is wrong. The ROM's
image blit assumes the source and the screen share a depth, which is true
of a machine whose assets match its screen and false here, where every
shipped `Image` is one or two bits.

It is not colour-specific and not a 32-bit limitation, which is what the
first two hypotheses supposed: `pix555Color` at 16 bits fails identically,
and so does plain 8-bit grey. Fixing it means making that blit
depth-convert the source -- new raster code, not another constant.

The instrument for that already exists. The test package can `CopyPixels`
a known one- or two-bit image into a deeper offscreen canvas and read the
result back, so the conversion can be got right in isolation before
anything in the boot path is touched.

## Method

Every constant here was found by watching the guest, not by reading the ROM
cold: `--watch-write` on the field or the pointer, `--watch-pc` with
`--watch-log` on a function entry to catch its arguments and caller,
`--save-state` at a stall then `--trace` to histogram the loop, and
`--coverage` with `scripts/covdiff` to see which blocks a patched boot
never reaches. Three guesses made from the disassembly alone were wrong and
each was corrected in minutes by asking the guest instead.
