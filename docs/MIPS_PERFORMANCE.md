# MIPS performance

The DataRover's TMPR3902U ran at 36.864 MHz and the model retires one
instruction per cycle, so real time is 36.864 million execution slots per
second. This document records what has been measured and the levers that
move it. Every comparison below required identical guest RAM, framebuffer,
WAV capture and normalized diagnostics between the runs being compared; a
faster run that changed the guest's behaviour would not count.

## Summary of levers

- **Native engine.** `--cpu-engine jit` (the default where available) is
  2.8x-4.7x faster than the interpreter on ARM boards. See
  [MIPS_JIT.md](MIPS_JIT.md).
- **Interpreter fast paths.** A physical-page lookup cache and live
  instruction-fetch spans, below, gave 21-25% on a slow x86 host.
- **Exception outlining.** Keeping exception delivery out of the hot loop
  cut interpreted boot time by 8.7%.
- **Profile-guided build.** GCC PGO speeds up the interpreter by a further
  12-28%. It is an optional, host-specific build, not a default.
- **Decoded blocks** (`--cpu-engine blocks`) were measured slower than the
  interpreter and remain experimental.

## Interpreter on a slow x86 host

Intel Atom N450, 1.66 GHz, Debian 13, one physical core/two threads. The
DataRover window measured 0.172x emulated time with SDL's software
renderer; OpenGL on this machine uses llvmpipe, not GPU acceleration.

Profiling a fresh 50-million-slot headless boot with a separate GCC `-pg`
build attributed 44.8% of sampled time to bus reads, 4.0% to bus writes,
and 46.1% to the machine/interpreter loop. LTO combines several functions
into the latter. Instrumentation affects performance; these percentages
identify candidates, not an exact breakdown of the normal executable.

Normal optimized binaries, with the same ROM, fresh temporary sessions, no
GUI, and no concurrently running emulator/compiler:

| Workload | Before | Page lookup only | Page lookup + fetch spans |
| --- | ---: | ---: | ---: |
| 50 million slots, median of 3 | 6.283 s | -- | 5.031 s |
| Earlier 50 million slots, median of 3 | 6.289 s | 6.119 s | -- |
| 300 million slots, one run | 37.510 s | -- | 31.104 s |

The combined change improves throughput by 25% in the short sample and 21%
in the longer sample. It does **not** make the Atom real-time: the long
headless sample averages about 9.65 million execution slots/second against
the 36.864 million/second target.

## Interpreter fast paths

- An optional 256-entry physical-page lookup cache avoids repeated linear
  region searches. Only uniform pages qualify: a partial higher-priority
  overlay disqualifies the entire page. Cross-page accesses use the general
  lookup. DataRover enables it; the 68k machines retain the general lookup.
- The MIPS batch executor resolves contiguous instruction-fetch spans.
  Spans stop at backing-store mirror boundaries and higher-priority regions.
  MMIO and floating reads still use the bus. Every fetch reads live guest
  bytes; no instructions are predecoded or retained.
- Bus reads remain counted. Address translation, alignment checks,
  exceptions, delay slots, instruction diagnostics, interrupts and device
  scheduling retain their behaviour. `mh_cpu_step` remains the general
  interpreter reference path.
- The no-MMU TX39 path uses one short segment-map branch for DataRover's
  common configuration, for both data accesses and instruction fetches. The
  full TLB lookup remains available when MMU mode is enabled.
- Mirrored accesses beyond a power-of-two backing store use a mask instead
  of integer modulo. Non-power-of-two devices retain the general modulo
  path.
- Successful RAM/ROM bus accesses are marked as the common branch;
  unmapped, floating, MMIO, flash-write and ROM-write cases are marked cold
  for the compiler without changing counters or diagnostics.
- DataRover's post-chunk scheduler skips PC-card, option-key and GPIO event
  work when those queues are empty. Card IRQ recomputation remains active
  whenever a card is installed, preserving edge ordering.

Map owners opting into the cache must call `mh_bus_invalidate_lookup` after
changing geometry or replacing backing pointers. Region additions,
DataRover state rebinding and card mapping invalidate it. CPU spans are
local to each run call and also check the invalidation generation,
including after an MMIO instruction changes a mapping. They are not saved
in states. Writes into existing memory, through any alias or a direct
loader/DMA pointer, need no invalidation because subsequent fetches read
those bytes.

CPU semantics tests run with both cache modes and cover alias
self-modification within a batch and an MMIO write replacing the executing
memory mapping. Bus tests cover partial-page overlays, retirement and
restoration, mirror boundaries and backing replacement.

## Decoded-block engine

`--cpu-engine blocks` selects an experimental portable decoded engine. The
cache groups sixteen instruction addresses per block and fills entries on
demand. It retains register indices, immediates, jump bits and a flattened
opcode/SPECIAL dispatch selector. Translation, interrupt checks and
execution still proceed instruction by instruction. Shared `execute.inc`,
`special.inc` and `step.inc` keep semantics and exception/diagnostic
sequencing common to both engines.

Every cache use checks the live instruction word. A mismatch replaces the
decoded entry before execution, which provides lazy invalidation for RAM,
flash, physical and virtual aliases, direct host/DMA writes and state
replacement. Decoded entries contain neither host memory pointers nor
resolved virtual branch targets, and are not serialized.

| Workload | Prior build | Interpreter | Decoded blocks |
| --- | ---: | ---: | ---: |
| 50 million boot slots, median of 3 | 5.046 s | 5.081 s | 7.001 s |
| 200 million hallway-drag slots, one run | -- | 22.102 s | 30.544 s |

Measured on the Atom above. The decoded engine takes about 38% longer in
both workloads, so it stays **off by default**. It still performs a cache
lookup and live-word check per instruction, and its expanded operands
increase memory traffic; those are plausible reasons the saved bit-field
extraction does not pay for itself, but they have not been isolated.
Removing live-word guards would first require complete write tracking for
aliases, DMA/host writes and state restoration. The native engine amortizes
dispatch and validation over whole blocks instead.

## Native engine

`--cpu-engine jit` compiles whole basic blocks, guards each against the live
instruction words, and chains blocks so the dispatcher runs once per chain
instead of once per instruction. Three-run medians are 2.9x/3.5x/4.7x (cold
boot, floor tap, hallway drag) faster than the interpreter on an Orange Pi 5
and 2.9x/3.1x/2.8x on a Pi Zero 2 W. Design, exact timings and limits are in
[MIPS_JIT.md](MIPS_JIT.md).

## Exception outlining

An interpreter-only hallway profile on the Orange Pi 5 (CPU 4, GCC 10.2.1,
Release/LTO) places 99.58% of sampled user CPU cycles inside `mh_cpu_run`.
The flattened loop duplicated exception delivery at many fault sites.
Marking `raise_exc` as `cold,noinline` on GCC/Clang reduces its main symbol
from 36,892 to 18,504 bytes. The same exception function still runs; fault
checks, EPC/delay-slot state, counters and scheduling are unchanged.
Improved instruction locality is a hypothesis for the timing change, not a
measured cache-miss reduction.

Alternating baseline/candidate comparisons, both on the interpreter with CPU
affinity `{4}`:

| Workload | Baseline | Exception outlining | Sampling |
| --- | ---: | ---: | --- |
| 50M cold boot slots | 0.965 s | 0.881 s | Five pairs |
| 200M hallway/drag slots | 3.576 s | 3.523 s | Three pairs |
| 500M hallway/drag slots | 8.604 s | 8.581 s | Five pairs |

Boot takes 8.7% less elapsed time. The longer hallway difference is only
0.3%, within run variation, so this does not establish a hallway speedup.
The ordinary build includes this change.

## Profile-guided interpreter build

GCC profile training (100M fresh boot slots plus 200M hallway/drag slots)
and a second compilation with `-fprofile-use` improve interpreted execution
further. No JIT is selected in training or measurement.

| Workload | Original interpreter | PGO interpreter | Elapsed reduction | Pairs |
| --- | ---: | ---: | ---: | ---: |
| 50M boot slots | 0.967 s | 0.695 s | 28.1% | 5 |
| 200M hallway/drag slots | 3.615 s | 2.986 s | 17.4% | 3 |
| 500M hallway/drag slots | 8.583 s | 7.524 s | 12.3% | 3 |
| 100M floor-tap slots | 1.737 s | 1.530 s | 11.9% | 3 |

The floor tap was not part of profile training, and the longer hallway test
extends beyond the training duration. These are still a narrow set of
headless workloads with empty audio captures; GUI, network and package
workloads and other host CPUs have not been evaluated. Profile data is
specific to the source, compiler and build paths, so use a fresh profile
directory after source changes and keep the same build directory for both
stages:

```sh
profile_dir="$PWD/pgo-data"
cmake -S . -B build-pgo -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_FLAGS="-fprofile-generate=$profile_dir"
cmake --build build-pgo --target mhat -j4

build-pgo/mhat --rom roms/MagicCap-USA.image \
  --headless --fresh --temporary --no-host-battery \
  --cpu-engine interpreter -n 100000000
build-pgo/mhat --rom roms/MagicCap-USA.image \
  --load-state states/hallway.state --headless --temporary --no-host-battery \
  --cpu-engine interpreter --drag-px 400,255,100,255,5000000 \
  --tap-hold 50000000 -n 200000000

cmake -S . -B build-pgo -DCMAKE_C_FLAGS="-fprofile-use=$profile_dir"
cmake --build build-pgo --target mhat test_r3900 -j4
build-pgo/test_r3900
```

The final executable does not need the profiles at runtime. A
missing-profile warning for the test source is expected, because training
did not run that program; missing or mismatched profiles for emulator
sources need investigation. See the
[GCC profile-use documentation](https://gcc.gnu.org/onlinedocs/gcc/Optimize-Options.html#index-fprofile-use).

## Measuring

`scripts/benchmark-mips` times the interpreter against the JIT in one
binary, or with `--baseline-exe` two binaries both on the interpreter. It
alternates run order, records binary/ROM/state hashes, and stops if any
run's RAM, framebuffer, WAV or normalized log differs. Keep the binaries
unchanged while it runs, and preserve a baseline executable before
rebuilding:

```sh
cp build/mhat mhat-before
# ... change and rebuild ...
taskset -c 4 scripts/benchmark-mips --exe build/mhat \
  --baseline-exe mhat-before --state states/hallway.state --drag \
  --slots 500000000 --repeats 5 --out bench/interpreter-long
```

The hallway workloads start from a calibrated snapshot at the Hallway and
drag the floor from (400,255) to (100,255), starting at slot 5,000,000 and
lasting 50,000,000 slots. This exercises guest UI processing headlessly; it
does not measure SDL pointer delivery, rendering latency or subjective
audio, and the drag produces no sound, so these runs are not evidence of
equivalent audible playback.
