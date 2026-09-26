# 68k performance

This covers the host-side performance of the 68k machines (PIC-2000,
HIX-300, Envoy). The CPU core, block engine, native emitters and their
measurements are in [M68K_CORE.md](M68K_CORE.md); the MIPS engine is in
[MIPS_PERFORMANCE.md](MIPS_PERFORMANCE.md). Measurements taken on the
earlier, removed 68k core are not repeated here: they do not describe the
current implementation.

## Board-loop mechanisms

These surround the CPU core and apply whichever engine runs. Each has an
environment switch that selects the reference behavior for an exact A/B
comparison; the fast paths are on by default.

- **Bus page cache** (`MH_68K_BUS_CACHE=0` disables). The shared 256-page
  lookup cache is enabled once all regions are installed. CS0 overlay
  retirement, power-on restoration of the overlay, and MBAR relocation
  invalidate it. Entries require a uniform page and keep first-registered
  region priority, so overlapping ROM/RAM maps such as Envoy's are not
  redirected. A 4096-entry table measured no better than 256 entries.
- **Direct memory** (`MH_68K_DIRECT=0` disables). Two tables of host page
  pointers, one per 4 KiB of the 32-bit space, are built from the bus map
  with the same uniform-page rule the MIPS engine uses and rebuilt when the
  map generation changes. ROM and RAM accesses use host bytes directly and
  keep the bus counters; MMIO, floating, unmapped, page-crossing and
  ROM-write accesses take the bus path. `MH_68K_DIRECT_DEBUG=1` prints the
  table build and hit/miss counts.
- **Stopped-core advancement** (`MH_68K_IDLE_FAST=0` disables). While the
  CPU is in `LPSTOP` and no per-slot diagnostic is active, the board
  advances the execution-slot clock to one slot before the nearest modeled
  deadline (audio, timer compare, pen timer, scheduled power, boot release,
  queued touches, caller budget), then runs the wakeup slot normally. No
  instruction is claimed to retire during skipped slots.
- **Quiet run loop** (`MH_68K_QUIET_FAST=0` disables). With no diagnostic
  active and the core running, the loop executes straight to the slot
  before the nearest deadline, running full per-slot device processing only
  when a device write sets a dirty flag. A stopped core, scheduled power
  presses, power-off, and the diagnostics below fall back to the one-slot
  loop. Checksum interception ([HIX300.md](HIX300.md)) does not: the engine
  is told the address and stops in front of it.
- **Audio and interrupt servicing.** Audio runs at output-sample deadlines
  or register changes, and interrupt levels are recalculated only when
  pending sources or enables change ([PIC2000_AUDIO.md](PIC2000_AUDIO.md),
  [HIX300.md](HIX300.md)).

The fast paths disable themselves for `--trace`, `--trace-after`,
`--sample`, `--force-irq`, `--coverage`, `--insn-heat` and watchpoints,
so those observations keep per-slot behavior. Idle advancement also waits
while an interrupt is pending or the DUART is active, and both paths are
off while the experimental `--net ne2000` aperture is attached.
Regression tests run a synthetic LPSTOP firmware with a scheduled pen
event through both idle modes and require byte-identical output; the machine's final device report
is the check for the other switches. `MH_68K_ENGINE_STATS` and
`MH_M68K_BLOCK_PROFILE` report engine and block statistics.

Host profiling answers a different question from the guest's `--sample`,
which samples emulated PCs. The CLI's `-n` budget counts execution slots,
including LPSTOP waits, not retired instructions; an idle machine spends
most slots stopped, so an idle profile says little about active work such
as Hallway dragging. Profile scripted-input workloads before choosing
optimizations.

## AArch64 JIT status (2026-09-22)

The Android app selects `--cpu-engine auto`, so an AArch64 tablet uses the
native block engine where available (see [ANDROID.md](ANDROID.md)).

An Android startup regression was investigated by narrowing the newly
added AArch64 emitter forms. With memory-source `MOVEA` disabled, PIC-2000
reached its touchscreen calibration screen again. Tablet tests also reached
calibration with the recent `BSR`, `MULL`, `SCC` and immediate-register
`BTST` forms enabled. This makes memory-source `MOVEA` the leading suspect,
but the tests were not a fully controlled cold-boot differential, so the
diagnosis is not conclusive. The shared emitter policy
(`m68k_memory_movea` in `src/cpu/m68k/core/m68k_emit_policy.h`) keeps
memory-source `MOVEA` disabled on every host pending a clean AArch64
differential test; its emitter implementation exists but is not selected.
The Android app stays on automatic engine selection rather than being
forced to the interpreter.

68k performance on AArch64 is reported to be much closer to MIPS, possibly
comparable. There is no controlled same-tablet, same-workload 68k versus
MIPS timing yet, so this is a qualitative observation, not a measured
parity claim.

Tradeoffs to keep in mind when interpreting such a comparison:

- The emitter keeps architectural state in the `m68k` object across guest
  instructions rather than in host registers across a block. With blocks
  averaging about three instructions, M68K_CORE.md finds little to gain
  from register caching; chaining and successor lookup matter more.
- Instructions the emitter does not translate call the interpreter from
  generated code. The value of a new native form depends on its
  execution-weighted frequency (`--insn-heat`), not how often it appears in
  decoded blocks.
- `MH_M68K_EMIT=0` keeps blocks but drops native code, and
  `--cpu-engine interpreter` selects the reference core; together these
  separate an emitter bug from a core one.

## Next

1. Record repeatable active PIC-2000 timings on the same tablet, keeping
   the final device state and report for each run.
2. Run a clean cold-boot AArch64 differential with memory-source `MOVEA`
   enabled against the interpreter, and re-enable it only if it agrees.
3. Collect an execution-weighted fallback profile on the tablet. Only then
   prioritize broader native translation, successor lookup for `RTS`
   (see M68K_CORE.md, "Next"), or guest-state caching; current evidence
   does not establish which limits the tablet.
