# Methodology

MagicHat exists because earlier Magic Cap emulation attempts produced
results nobody could trust. Not because their CPU cores were bad, but
because they carried high-level patches welded in: `BREAK` instructions
swapped in at named ROM addresses, an `LBU` skipped at `0x83C1F898`, context
fields rewritten inside a ROM function, ROM functions called directly with
synthesised stack frames, method dispatches injected into the Telescript
VM.

Once any of that is present, no observation means anything. You cannot tell
whether behaviour came from the machine or from the harness, and months can
go into chasing conclusions that turn out to be artefacts of the shims.

The rules below apply to every machine `mhat` runs: the DataRover 840
(MIPS) and the 68k machines. Where a document describes hardware --
[HARDWARE.md](HARDWARE.md), [ACCURACY.md](ACCURACY.md),
[OPEN_QUESTIONS.md](OPEN_QUESTIONS.md) -- it means the DataRover; the 68k
machines have their own documents, starting with [PIC2000.md](PIC2000.md).

## Explicit HIX recovery exception

At the user's request, HIX checksum recovery intercepts the recognized
checksum result when the loaded ROM fails its byte sum. This is a software
recovery intervention, not emulated hardware. It is automatic for
mismatched HIX images with the expected instructions, prints a deviation,
and changes neither the ROM file nor its in-memory bytes. See
[HIX300.md](HIX300.md) for the instruction and runtime guards. It is a
specific exception to rules 1 and 2 below, not permission for unrelated
guest hooks.

The inferred audio-divider model is likewise the HIX default pending better
hardware evidence. This is an unverified timing assumption, reported at
startup, and applies only to identified HIX ROMs. DMA completion still
follows buffer consumption; no completion event is forced.

## The rules

**1. No high-level emulation. None.**

There is no patching of ROM instructions, no forcing of interrupt or status
bits that no device produced, no calling ROM functions from outside, and no
injecting anything into the Telescript VM. If the ROM will not do
something, the bug is in a device model, and the job is to find it.

This rule is about *making stock functionality work*. It is not a ban on
modifying the machine on purpose -- see rule 6, which draws the line.

**2. Every deviation from hardware is opt-in, named, and printed.**

`--show-deviations` lists the DataRover's. Any that is active is printed at
startup. Both `--help` outputs list them in their own section.

The following exist to re-test an assumption rather than to make anything
work:

- `mhat --force-mmu` routes kuseg and kseg2/3 through the TLB, so the
  evidence that this part has none can be re-checked.
- `mhat --device pic2000 --force-irq <level,at>` asserts an interrupt line
  that no modelled device drove. It was written to answer one question --
  whether the machine was waiting for an interrupt or stuck for another
  reason, which look identical from outside -- and the answer was no. It
  should stay unused unless another question of that shape comes up.
- `mhat --device pic2000 --probe-preset dev21:EE=00C0` seeds an unknown
  register for a controlled comparison. It does not represent a detected
  device.

A deviation that starts being left switched on because the machine "gets
further" with it has become a shim, whatever the flag is called. The
interrupt test above is the example: forcing it did get further, into a
handler that then dispatched through an empty table and returned, which is
not progress.

**3. Unknown means unknown.**

An instruction encoding we do not implement raises Reserved Instruction and
logs. An address no region claims is a bus error and is counted. A TX39
register we do not decode is counted separately from ones we do. A chip we
have identified but not understood gets an `unknown_dev` register file
whose traffic is reported, so it cannot quietly become load-bearing.

The one place doing nothing is the accurate answer is the `CACHE`
instruction, because a machine with no caches has nothing for it to do.
That is argued for at the implementation site rather than assumed.

One hole in this was found late and is worth naming, because it defeats
the rule quietly. A write to a ROM region *succeeds* -- the hardware
ignores it and so do we -- while the bus counts it as a fault. So a run can
report zero undecoded accesses and be discarding half a million writes at
the same time, which is exactly what a retired-but-still-matching region
caused on the 68k side. The 68k frontend now reports ignored ROM writes,
with their sites, in its exit summary. The DataRover frontend logs them
only with `--log-unmapped`, and should report them too.

**4. Findings get sourced.**

Every non-obvious modelling decision carries a comment saying what it is
based on: a NetBSD register header, a specific ROM address, or an explicit
statement that it is a guess. [OPEN_QUESTIONS.md](OPEN_QUESTIONS.md) holds
the guesses.

**5. Diagnostics observe; they never intervene.**

`--trace`, `--log-mmio`, `--log-exceptions`, `--histogram` and the counters
in the exit summary change nothing about execution.

**6. Enhancements are allowed. Shims are not. The difference is why.**

Modifications to the emulator, the ROMs and the packages are fine when the
point is to build something the device never had -- more RAM, code in the
free flash, a feature it never shipped with. They are not fine when the
point is to get standard functionality working. Same edit, opposite intent,
and the intent is what decides it:

- A **shim** covers for something we have not understood. It makes a stock
  behaviour appear without the cause being found, and it destroys the value
  of every observation downstream, because you can no longer tell what came
  from the machine. Banned by rule 1, always.
- A **modification** changes what the machine *is*. The emulator then
  models that machine as faithfully as it models the stock one, and
  observations about it stay meaningful -- as long as everyone knows which
  machine they are looking at.

The test to apply: *if this works, what have I learned?* A modification
teaches you about a machine you built deliberately. A shim teaches you
nothing, because it answers a question you asked by assuming the answer.

Three obligations follow, and they are what keep the two apart in practice:

**Modified things announce themselves.** A patched ROM or package is never
silently substituted for a stock one. Snapshots store the whole flash image
and will otherwise carry a modification into a run whose command line names
the stock ROM, so experimental states are kept apart from stock ones.
`scripts/mkstates` and `scripts/regress` stay stock; experiments live
elsewhere.

**Modifications are documented apart from standard behaviour.** Anything
built for an enhancement -- an emulator option, a patch set, a modified
package -- is described in [MODIFICATIONS.md](MODIFICATIONS.md), not mixed
into the documents that record what the real device does.
[HARDWARE.md](HARDWARE.md), [ACCURACY.md](ACCURACY.md) and
[OPEN_QUESTIONS.md](OPEN_QUESTIONS.md) describe the DataRover 840 as it
shipped. A reader must never have to guess whether a documented behaviour
is the machine's or ours.

**Enhancement code is marked as such at the source.** A feature that
exists only for a modified machine says so in a comment, the same way rule
4 requires every modelling decision to be sourced.

## Why this works

The rules are not just hygiene -- they are the debugging method. Every step
of the DataRover boot bring-up came from a diagnostic pointing at something
specific:

- The reserved-instruction trap named `CACHE` at `0x83C008AC` instead of
  the emulator silently ignoring it and drifting.
- The exception log named the first fault as a kseg3 store at `0x83C00568`,
  which is what established that this part has no TLB.
- The register histogram's last-access-PC column named `0x13C06B34` as the
  site of a 95-million-iteration poll, which is what identified the MBUS
  block.

A shim at any of those points would have hidden the cause and moved the
symptom somewhere else.

## Measuring, and the ways it goes wrong

Every hard finding here came from a measurement, and the measurements have
been wrong more often, and more expensively, than the hypotheses. These
cases all produced a confident answer that was not true:

**Testing after the decision.** The auxiliary ADC channels were reported
ruled out as the battery sense because five different values produced an
identical Desk. They were set from a snapshot taken *after* the OS had
already decided about the batteries and posted its dialogs. Applied over
the whole boot, they work. `scripts/tryopts` runs the whole path with
options in force throughout, and exists because of this.

**An input the software rejects.** A coordinate sweep at the calibration
screen came back as thirty rejections. They were timeouts: the press was
held for 25M instructions and the calibration needs about 30M. The input
never reached the code being tested.

**A metric that cannot see the difference.** Framebuffer change was
measured by counting non-white pixels. All three calibration steps show the
same target and the same sentence in different places, so the count is
exactly 2744 for all of them while the screen changes completely.
`scripts/fbdiff` compares frames; never count them.

**A budget ample for one path but not another.** Driving the battery ADC to
a mid-range value was recorded as halting the boot, because a run with the
usual 1.5B-instruction budget ended with the boot rabbit still on screen.
Given 4B it reaches the boot screen normally -- that path is simply slower.
"It did not get there" and "it did not get there yet" are
indistinguishable from outside.

**Rebuilding mid-experiment.** A comparison sweep was left running across a
rebuild, so its runs used two different binaries and their hashes were not
comparable. The results were discarded.

Those five share one underlying error -- the input or the run did not reach
the thing being measured -- so the first question to ask of a negative
result is not "is the hypothesis wrong?" but "did the experiment actually
happen?" Before believing "X has no effect", check that X was present, in
force, at the moment the thing under test happened, and that the metric can
see the difference if there is one.

### A second family: the instrument meant something else

Later work produced failures of a different shape. The experiment did
happen; the reading of it was wrong, because the instrument did not measure
what it appeared to.

**Too few samples to distinguish two states.** A program counter sampled
twice landed in the same routine both times, and this was called a tight
loop. It was a hash lookup in the object dispatcher -- a hot function,
which from two samples is indistinguishable from a stall. Sustained bus
traffic does not separate them either, because a busy loop writes. A
*distribution* does: `mhat --device pic2000 --sample` buckets the PC by 64K
page, and ten pages with 88% of the time in one is a machine that has
settled, where a working boot ranges much wider.

**A diagnostic that named an innocent suspect.** An
unimplemented-instruction message was written to say "if this is TBLS,
LPSTOP or BGND it is a CPU32 instruction the core lacks". The first thing it
caught was opcode `FFFF`, which is not any of those: it was a CPU running
off into unmapped space and reading a bus that floats high. A diagnostic
that volunteers a culprit will be believed, so it must only name one when it
is sure.

**Counting byte patterns in an image that is mostly data.** Searching a
4 MB ROM for the CPU32-only opcodes found 20 `BGND` and 757 `TBL`;
searching for 480 and 320 as constants found 1,101 and 666. All noise. A
whole-ROM instruction sweep is worse than useless on a variable-length
architecture: starting at arbitrary offsets desynchronises the decode, and
one such sweep reported zero `movec` in a ROM whose reset path had been
*watched* executing `movec`. Disassemble from a known entry point, or
measure at runtime.

**The right pattern in the wrong place.** Searching four 68k ROMs for
`Magic Cap N.N` found exactly one hit in each, and three of them were
reported as the OS version. Every one was `Magic Cap 1.0 CIS` -- a
CompuServe service name sitting in a list of UI strings. The real version
was in a device record, and it made one of those machines a 1.5 rather than
a 1.0.

**Coverage granularity mistaken for a code path.** A coverage diff showed
187 instructions executed only in a failing run, clustered in
memory-manager code, and this was read as "the failure takes a distinct
path through the allocator". Coverage is per instruction, so a *different
branch inside a shared function* looks exactly the same. Watching those
addresses showed them being hit early and often, from a routine walking a
table.

**"It got further" read as progress.** Forcing an interrupt moved the
machine from its dispatch loop into an interrupt handler, which looked like
the blocker lifting. The handler dispatched through a table the OS had
never populated and returned. Getting further into code that then does
nothing is not progress, and the way to tell was available at the time:
the page spread did not change.

### A startup workaround mistaken for hardware behaviour

The DataRover was for a long time launched with a reset-PC override and a
delayed `go -c` command. Tests reached the Desk through that route, while
documentation claimed the ROM always entered the monitor. Neither claim
established normal cold-start behaviour. The outer startup routine jumps to
the monitor **if its initialization call returns**; inside that call, GPIO
input 3 selects an automatic client launch. A zero-filled IODIN register
selected the debug path.

The reset explanation also treated a disassembler's `j 83C0001C` label as
an absolute address. The instruction retains the current PC's high nibble:
at architectural reset it targets `B3C0001C`, an existing physical flash
alias. See [STARTUP.md](STARTUP.md) for the evidence and the remaining
decoder uncertainty.

This was a failure to apply rules 3 and 4 and to test the claimed path, not
a reason to invent another exception. For cold-start claims, run from
architectural reset without snapshots, injected commands or register
overrides. Trace calls that precede an apparent terminal branch, and
identify the input that selects the path. Keep monitor-assisted boot as a
separate diagnostic case. The full `scripts/regress` compares unassisted
startup with the verified boot screen.

### Tooling that lied

Two of the instruments were themselves faulty, which is worth separating
from misreading a sound one.

**`ostest --update` with a filter destroyed the golden file.** It wrote
only the fingerprints the run reached, so `--update keyboard` reduced the
file to one line and the next full run reported the losses as failures. It
merges now.

**`pgrep -f` and `pkill -f` match the shell that runs them.** A pattern on
the command line of the process doing the matching is in its own argv, so
`pkill -f uicrawl` kills the wrapper and `until ! pgrep -f "cmake --build"`
never becomes false. This happened three times in one session -- twice
killing a wrapper, once leaving three shells spinning for half an hour --
after the lesson had already been written down once. Match exact process
names via `ps`/`awk`, or keep the pattern out of the matching command line.
