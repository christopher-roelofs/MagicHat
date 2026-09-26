# Native MIPS engine (`--cpu-engine jit`)

The native engine runs the TX39's user-visible instruction set as generated
host code on x86-64 (Linux and Windows) and little-endian AArch64 (Linux and
Android). It is the default where executable memory is available (`--cpu-engine
auto`); hosts that cannot create JIT code fall back to the interpreter. The
interpreter (`--cpu-engine interpreter`) remains the reference: the two
engines must produce byte-identical RAM, framebuffer, audio and normalized
diagnostics for the same inputs, and the benchmark harness refuses a result
where they do not. A first prototype (AArch64 only, integer blocks of at
most 16 instructions with per-block dispatch from C) was 6-16% slower than
the interpreter; the current design replaces it.

## Organization

- `src/jit/mips_ir.c` decodes every MIPS word into a small host-neutral IR.
  Instructions the backends do not implement become `J_EXEC`, which runs
  the reference interpreter's exec body from inside the block.
- `src/jit/x86_64.c` and `src/jit/aarch64.c` are the backends. They share
  the block shape, the exit protocol and the helper interface in
  `src/jit/jit.h`; only instruction selection differs. The x86-64 backend
  speaks both the System V and Windows x64 calling conventions.
- `src/jit/cache.c` owns the block table, the code arena and the direct
  page tables, and resolves links between blocks.
- `mh_cpu_run_jit` and the helpers `mh_jit_exec`/`mh_jit_exec_delay` in
  `src/cpu/mips/r3900.c` integrate native execution with the existing
  exception, interrupt, pipeline and scheduling behaviour.

## What a block is

A block is straight-line code from an entry address through a branch and
its delay slot (or to 64 instructions, or to an instruction after which an
interrupt check is due). It contains:

- integer ALU, shifts, LUI, SLT, MULT/MULTU, MFHI/MFLO/MTHI/MTLO natively;
- ADDI/ADD/SUB natively, with the host overflow flag selecting the
  reference path when the exception must be raised;
- LB/LBU/LH/LHU/LW/SB/SH/SW natively through direct page tables (below);
  misaligned addresses and pages that are not plain memory take the
  reference path for that one instruction;
- all branches, with the delay slot inside the block; a link register is
  written before the slot executes, as in the reference sequence;
- everything else (LWL/LWR/SWL/SWR, DIV/DIVU, CP0, SYSCALL/BREAK, CACHE,
  reserved encodings) through `mh_jit_exec`, which sets `cur_pc`, `pc`,
  `next_pc` and `in_delay` exactly as the step routine would, runs `exec`,
  and reports whether the block must stop: an exception, halt or power stop
  changed the control flow, an access changed the interrupt inputs, or a
  device write changed the bus map. CP0 writes, RFE and exception-raising
  instructions always end the block so the dispatcher's interrupt check
  follows them, where the reference loop would make it.

Guest registers, hi/lo and CP0 live in the `r3900` structure. Each backend
keeps the first guest registers a block touches in host registers (eleven
on AArch64, six on x86-64) and writes them back at every exit and before
every helper call; after a helper nothing is assumed cached, so helpers
always see and change the same machine the interpreter would. A slow path
that rejoins its fast path reloads the fast path's live registers.

CP0 Count is `cycle_count`, so a block retires its own instruction, cycle
and fetch counts, and a helper advances them by the instructions the block
has already completed for the duration of the instruction; MFC0 Count reads
the same value in either engine.

## Code validity and dispatch

Every block guards itself: its prologue compares the guest instruction
words it was compiled from against live memory (x86-64: one compare per
word; AArch64: one 64-bit compare per two words). Guest stores through any
alias, direct DMA/loader writes and restored states are therefore all
caught at the next block entry without write notification. A block whose
guard fails returns a status telling the dispatcher to recompile it in
place, and the stale code's own exit is redirected to the new block so
predecessors that were already chained to it follow.

Blocks chain. When a block ends at a statically known successor (fall
through, J/JAL, either arm of a conditional branch) it jumps to a link
site, a patchable branch that first returns to the dispatcher and is
redirected to the successor's chain entry the first time it is taken. A
register-indirect jump looks its destination up in the block table itself
(four ways, one compare each) and continues in a cached block's chain
entry; on a miss it, blocks that must be followed by an interrupt check, and
branches whose delay slot could not be compiled with them return to the
dispatcher. The dispatcher samples interrupts once per entry; that is every
point the reference loop could take one, because device inputs only change
between run calls and anything inside a chain that could change interrupt
visibility ends its block. The slot budget travels in a host register: each
block checks that the whole block fits before touching state, so a chain
stops exactly where the interpreter would tick devices, and the last few
slots of a chunk run through the reference step.

Direct memory access uses two tables of 1M host pointers indexed by guest
virtual page, following the CPU's own segment rules (no MMU: kseg0/kseg1
strip the top bits and everything else maps straight through; with an MMU
only kseg0/kseg1 are direct). A page has an entry only when every byte of
it is served by one host-backed region with no earlier overlay and no
mirror seam inside the page; writes additionally require RAM (flash writes
are counted, ROM writes are faults). Everything else takes the reference
path. The tables and all code follow the bus map generation: a change
rebuilds the tables and flushes the code, since blocks embed host pointers
to guest bytes.

The code arena is 32 MiB. On Linux and Android it is one shared-memory
object mapped twice, writable for the emitter and executable for the CPU,
so no page is ever writable and executable at once and no protection
changes happen per block; a per-block `mprotect` pair once cost as much
system time as the whole run. Where that dual mapping is unavailable
(Windows, or a kernel without `memfd_create`) a single mapping is used and
unlocked only while code is patched. `MH_JIT_SINGLE_MAP=1` forces that path
for testing. The block table is four-way associative with 16,384 sets. When
the arena fills everything is discarded and recompiled on demand.

## Known limits

- A guest store that changes a later instruction of the block that is
  currently executing is not seen until the next block entry; the reference
  interpreter would execute the new word. Real TX39 hardware has an
  instruction cache with the same visibility question, and no Magic Cap
  code seen so far writes instructions it is about to execute.
- Diagnostic modes (`--trace`, `--coverage`, PC watchpoints,
  `--watch-write`) force the reference step for every instruction.
- MMU-translated fetches and user-mode execution with `--force-mmu` use the
  reference step; kseg0/kseg1 kernel code still compiles.
- `MH_JIT_STATS=1` prints, at exit, which instructions reached the helpers.
  That histogram is how ADDI was found to be 29% of the hallway workload.

## Tests

`test_r3900` runs the CPU semantics suite in native mode, plus four
differential tests against the reference stepper comparing complete CPU
state, RAM and bus counters at every run boundary: mixed code with
interrupts and unannounced instruction writes; random integer blocks with
every supported ALU family and short budgets; every branch family with
signed boundaries, overwritten targets, faulting delay slots and split
scheduling; and a chaining test with loads and stores of every width,
multiply/divide, a device whose accesses raise an interrupt mid-chain, live
code rewrites and 3,000 random budgets. A fetch-remap test checks that a
device write that changes the bus map stops the block after that store.
Cross-compiled AArch64 tests can run under `qemu-aarch64-static` before
anything goes to a device.

## Results

Both engines from the same binary, pinned to one core, three alternating
runs per engine, medians. Every pair compared identical RAM, framebuffer,
WAV and normalized diagnostics.

Orange Pi 5 (Cortex-A76, CPU 4, GCC 10.2.1, Release):

| Workload | Interpreter | JIT | Speed-up |
| --- | ---: | ---: | ---: |
| 50 million cold boot slots | 0.855 s | 0.296 s | 2.9x |
| 100 million floor-tap slots | 1.730 s | 0.489 s | 3.5x |
| 200 million hallway drag slots | 3.518 s | 0.754 s | 4.7x |

Raspberry Pi Zero 2 W (Cortex-A53 at 1 GHz, CPU 3, fully static
`-mcpu=cortex-a53` build, 416 MiB RAM):

| Workload | Interpreter | JIT | Speed-up |
| --- | ---: | ---: | ---: |
| 50 million cold boot slots | 5.887 s | 2.021 s | 2.9x |
| 100 million floor-tap slots | 11.651 s | 3.753 s | 3.1x |
| 200 million hallway drag slots | 25.807 s | 9.251 s | 2.8x |

The guest clock is 36.864 MHz, so 200 million slots are 5.4 s of device
time: the Zero runs the hallway drag at about 0.6x real time against 0.2x
interpreted. Native execution covers 99.7% of slots in every workload; the
dispatcher is entered once per 160-590 instructions thanks to direct and
indirect chaining (before indirect chaining, once per 25). The boot figure
includes ROM loading, the 8 MiB page-table build and the compilation of
11,842 blocks. The hallway workload gained most from native ADDI: 29% of
its instructions had been reaching the helper.

On an x86-64 development machine (i7-8650U, heavily loaded, so absolute
times vary) a 200 million slot boot takes 1.0-2.0 s natively against 4.5-8 s
interpreted, again with identical RAM and diagnostics. An A/B of the x86-64
register cache against a one-slot build on that machine showed no
measurable difference (three alternating pairs within noise): memory
operands are cheap on that core, and the cache is kept there for
uniformity with AArch64, where it is part of the measured result.

Candidates for the next round, if the Zero needs to reach real time: keep
hi/lo and CP0 Status/Cause readable natively (MFC0 is now the largest
helper class), cache guest registers across chained blocks, and spend fewer
instructions on the per-block guard on the in-order A53.

## Reproduction

`scripts/benchmark-mips` alternates engine order, records binary/ROM/state
hashes and CPU affinity, and requires identical final RAM, framebuffer, WAV
and normalized diagnostics; it rejects a silently disabled JIT. On a
heterogeneous part, pin it to one known core with `taskset`:

```sh
./build/test_r3900
taskset -c 4 scripts/benchmark-mips --exe build/mhat \
  --rom roms/MagicCap-USA.image --out bench/boot50m
taskset -c 4 scripts/benchmark-mips --exe build/mhat \
  --rom roms/MagicCap-USA.image --out bench/tap100m \
  --tap 400 255 5000000 --slots 100000000
taskset -c 4 scripts/benchmark-mips --exe build/mhat \
  --rom roms/MagicCap-USA.image --out bench/hallway200m \
  --state states/hallway.state --drag --slots 200000000
```

The hallway workload starts from a calibrated snapshot at the Hallway and
drags the floor from (400,255) to (100,255), starting at slot 5,000,000 and
lasting 50,000,000 slots. Any saved state that shows the Hallway will do.

## References

A64 encoding: [Arm Architecture Reference Manual](https://developer.arm.com/documentation/ddi0487/latest).
x86-64 encoding: Intel SDM volume 2.
Host cache synchronization: [GCC clear-cache builtin](https://gcc.gnu.org/onlinedocs/gcc/Other-Builtins.html).
