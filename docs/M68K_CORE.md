# The project-owned 68k core

The 68k machines use the project-owned CPU32 interpreter and JIT. The former
Moira integration and differential tests have been removed. Historical
comparisons against Moira below record development validation only.
`--cpu-engine interpreter` selects single-step execution through our
core, `blocks` selects cached interpretation, and `jit`/`auto` enable native
blocks where supported.

The detailed Moira comparisons below are a historical record. Their test
programs and generator have been removed; the current tree has no Moira source
or build dependency. Maintained CPU checks exercise interpreter, block, and
emitted execution against each other and through machine runtime tests.

The decoder covers the CPU32 encodings, and the interpreter implements every
instruction observed in the PIC-2000, HIX-300, and Envoy ROM workloads. The
block engine and native emitters build on that same core. Measurements and
Comparisons in the sections below record earlier development and validation.

## What the decoder produces

`src/cpu/m68k/core/m68k_decode.{h,c}`. One call decodes one instruction from
a span of guest bytes and fills in a structure shaped for an emitter rather
than for an interpreter's switch:

- **Length in bytes.** The central obligation. A variable-length instruction
  set turns one wrong length into a wrong decode of everything after it.
- **Fully unpacked effective addresses**, extension words included, so that
  nothing downstream parses an extension word a second time.
- **Flags** saying what a block must do about the instruction without
  re-deriving its meaning: whether it ends the block, whether it can trap,
  whether it needs supervisor state.

The internal addressing-mode values preserve the numbering used by the former
reference comparison; they are private to this decoder.

## Historical decoder comparison

The removed `test_m68k_decode` program decoded every one of the 65,536 opcode
words with both cores and compared the instruction and the length. It did
that five times over, with a different set of following words each time,
because an extension word decides both what an instruction is and how long
it is: a single sweep of brief-format index words never reaches the full
format the CPU32 added, where a base displacement is what makes the
instruction longer.

    311,095 opcode/extension pairs agreed on the instruction
    233,555 lengths compared
     15,305 deliberate 68020-only differences
      1,280 deliberate CPU32-only differences
          5 memory-indirect extension words refused

Memory indirection is tested separately and directly, because it is the one
case where accepting an encoding would be worse than mis-naming it: a
decoder that computed a length for an instruction the hardware traps on
would leave a translator walking into the middle of the next one.

The test fails if a whole class of deliberate difference ever stops
appearing. A deviation that quietly disappears is as much a surprise as a
new one, and usually means the test stopped reaching that part of the space.

## Historical CPU32 and 68020 differences

Moira emulates a 68020 and the MC68349 is a CPU32, so they differ in both
directions.

**On the 68020 and not on this part.** These trap here. The bit field
instructions, `CALLM` and `RTM`, `CAS` and `CAS2`, `PACK` and `UNPK`,
`MOVE16`, the cache and MMU instructions, the whole coprocessor interface,
and the memory-indirect addressing formats.

**On this part and not in Moira.** `LPSTOP`, the table lookup instructions,
and `BGND`. Moira reports coprocessor or illegal encodings for all three,
which is what a 68020 does with them.

**Where Moira cannot serve as an oracle at all.** Its disassembler does not
implement `MOVES` at any model, although `getInstrInfo()` names it and the
CPU32 has it. Its length is measured instead through a `TST` naming the same
address mode, which Moira does disassemble, plus the two words `MOVES`
always adds. That keeps the measurement independent of the decoder being
tested rather than asserting the decoder's own answer back at it.

## Execution

`src/cpu/m68k/core/m68k.{h,c}` runs what it decodes: the moves, the
arithmetic and logic including their extended, immediate and decimal forms,
the comparisons, the bit operations, the shifts and rotates, the word and
long multiply and divide, the branches, the subroutine and frame
instructions, the register-list and peripheral moves, the control and
alternate-space moves, and exceptions with their frames.

**It covers 100% of what the three ROMs execute.** That is measured rather
than claimed: `mhat --insn-heat` decodes every opcode the machine ran
through the new core and asks it whether it implements it, weighted by how
often it ran. The core is the only thing that knows what it implements, so
it is the only thing that answers; a second list kept in a test would drift
and report coverage that was not there.

The measurement is also what set the order of work. After the first
tranche the gap was eight instructions, and it was not the eight anyone
would have guessed:

| Missing then | Times it ran |
| --- | ---: |
| `MULL` | 690,045 |
| `TRAPcc` | 50,570 |
| `DIVL` | 10,079 |
| `RTE` | 3,926 |
| `ABCD` | 215 |
| `MOVEC` | 27 |
| `MOVEP` | 24 |
| `MOVES` | 8 |

A long multiply nobody had thought about was 1.2% of the workload, and
`TRAPcc` turned out to run fifty thousand times because its always-false
form is a compact way to skip the words that follow it.

Two decisions in the interface are there for the translator that comes
later rather than for the interpreter:

- **Execution takes an already decoded instruction.** A translator that
  decoded once when it compiled does not decode again when it runs.
- **An effective address is resolved to an operand first and read or
  written after.** An instruction that reads and writes one address has to
  apply a pre-decrement or post-increment exactly once, and separating the
  two steps is what makes that true for an emitter as well as here.

Condition codes are five separate bytes rather than bits packed into the
status register, which is how the reference holds them, keeps the common
case to a byte store, and is the representation a translator wants before
it starts keeping them in host flags.

An instruction the core does not implement raises rather than doing
nothing. A missing instruction quietly treated as a no-operation is the one
failure this core must not have, because it would leave every later
comparison against the reference passing while the guest drifted.

## Historical instruction comparison

The removed `test_m68k_exec` program gave both cores the same registers, the
same condition codes and the same 64 KiB of memory, runs one instruction on
each, and compares every register, every flag and every byte afterwards.

    278,479 instructions compared
    121,521 encodings skipped as out of the sweep's scope
         78 distinct instructions covered
          9 exceptions raised and returned from

The instructions come from an explicit list of what the core claims to
implement, so adding one to the core means adding it to the test
deliberately. An instruction that silently stopped being covered would
otherwise look exactly like one that passes.

Exceptions are tested as a round trip rather than as an event: cause one,
compare the frame the two cores built byte for byte along with where they
vectored to, then return through it and compare again. A frame is only
correct if something can come back out through it, so building it and
unwinding it belong in one test. Nine of them, across the short frames and
the six-word ones that also record the instruction that faulted, agree
exactly.

Returning from an exception is deliberately left out of the random sweep.
A frame made of random bytes compares two cores' opinions of a malformed
frame, and a 68020 has frame formats this part does not, so the
disagreement there would be about the reference's instruction set rather
than about this core.

Four bugs surfaced and are worth recording, because each was invisible to
the decoder test that preceded it. `EXG` had two of its
three register forms crossed, which only an execution comparison could see.
`LINK` with the stack pointer as its own operand stored the lowered pointer
rather than the original, an encoding that is rare enough to never appear
in ordinary code and specified plainly enough to get wrong. And the test
itself wrote the stack pointer before the status register, so setting
supervisor mode afterwards put the value in the wrong one of the two
registers A7 names; the disagreement that produced looked like a fault in a
shift instruction, three families away from the actual mistake. And `MOVES`
storing an address register through its own pre-decrement wrote the value
from before the decrement rather than after it, the same class of ordering
question as `LINK`, in the one instruction where nothing else would ever
have exercised it.

## A defect in the reference

The reference sets the zero flag after `DIVU` and `DIVS` to the opposite of
what the architecture specifies. It assigns the quotient to the flag
instead of testing the quotient for zero, in both its signed and unsigned
Musashi-compatible divides, so the flag comes out set exactly when it
should be clear.

Demonstrated without reading either core's source, by dividing zero by
seven:

| Case | Quotient | Architecture | Reference |
| --- | ---: | ---: | ---: |
| 0 ÷ 7 | 0 | Z set | Z clear |
| 100 ÷ 7 | 14 | Z clear | Z set |

Both cores compute identical quotients and remainders; only the flag
differs. The new core follows the architecture, and the differential test
compares that flag inverted for divides and counts the cases, so that the
day the reference is fixed the test says so rather than quietly passing.

**Whether it matters to these machines was measured rather than argued.**
`mhat --insn-heat` now also reports what runs immediately after a
divide, which is where a wrong zero flag would be read if anywhere:

| ROM | Instructions after a divide | Of those, reading the zero flag |
| --- | ---: | ---: |
| PIC-2000, 60M instructions with two taps | 39 | 0 |
| Envoy, 40M instructions | 50 | 0 |
| HIX-300, 40M instructions | 13 | 0 |

Divides are rare in all three, and nothing in these workloads reads the
flag before it is set again. The defect is real and latent rather than
active, so the existing emulation is not known to be wrong because of it,
and patching the reference is a decision about risk rather than a repair of
something observed to be broken.

## What the machines actually execute

`mhat --insn-heat` counts executed instructions by opcode and reports
them at exit through the project core's instruction metadata. This is
the 68k equivalent of the MIPS engine's helper histogram, and it exists for
the same reason: the order to implement things in should be measured, not
guessed. On the MIPS side that measurement is what found a single
instruction to be 29% of a workload after a day of guessing.

PIC-2000, 57 million instructions across a boot and two screen taps:

| Form | Share | Cumulative |
| --- | ---: | ---: |
| `DBcc` | 7.5% | 7.5% |
| `ADD.b (An)+` | 5.8% | 13.3% |
| `BNE` | 5.2% | 18.5% |
| `MOVE.l Dn` | 4.2% | 22.7% |
| `MOVE.l (d,An)` | 3.7% | 26.3% |
| `RTS` | 3.3% | 29.6% |
| `MOVEQ` | 3.1% | 32.7% |

The shape matters more than the order. 468 distinct instruction, mode and
size combinations execute, the top 40 account for 74% of them, and the
remaining 428 account for 26%. Envoy and HIX-300 are similar, at 406 and 397
forms. That tail is the argument for the MIPS engine's design rather than
against it: a translator that must implement everything before it can run
anything would still be unfinished, while one that compiles common forms and
falls back to the project interpreter for unsupported translations can run
from the first instruction it supports.

## Historical real-ROM comparison

The removed `test_m68k_rom` program was the harsh comparison. Random
instructions in isolation reach the states a generator stumbles on; a program reaches the
states it builds up, with a stack that means something, registers holding
pointers into structures it made earlier, and loops that run until a
condition it computed comes true. A divergence that needs that history is
invisible to a random sweep and ordinary here.

Both cores start at the ROM's own entry, see identical memory, and are
compared after **every** instruction on all sixteen registers, the program
counter, the whole status register, and what memory holds.

Each ROM runs twice, because the two passes ask different questions. The
first runs undisturbed and asks how long the two cores can agree. The
second interrupts constantly and asks whether they agree about being
interrupted, which is the only exception a program cannot cause on purpose
and therefore the only one an undisturbed run never reaches.

| ROM | Undisturbed | Interrupted | Interrupts |
| --- | ---: | ---: | ---: |
| PIC-2000 | 20,000,000 | 66,079 | 617 |
| Envoy | 20,000,000 | 20,000,000 | 198,311 |
| HIX-300 | 20,000,000 | 20,000,000 | 198,310 |

Around 120 million instructions and 400,000 interrupts, with every register,
the whole status register and every byte of memory compared after each one.

This is not the machine: there are no devices, so every address outside ROM
and RAM reads as zero for both cores alike, and the ROMs eventually wander
somewhere a working board would not have sent them. That does not weaken
the comparison. The question is whether two processors agree, and they are
being asked it about real code with real history behind it.

PIC-2000's interrupted pass ends early because the ROM, interrupted six
hundred times with no devices to explain why, eventually follows a pointer
it assembled from a register that never answered, and fetches from an odd
address. A CPU32 faults on that and the reference has address-error
emulation compiled out, so the two become different machines on purpose
and the run stops rather than reporting a disagreement that is the point.

Interrupts are offered only at levels the current mask would admit, and the
line is left asserted until the reference answers it, with this core then
told to answer the same one at the same boundary. Choosing that boundary
here instead was the first attempt and it diverged within fifteen thousand
instructions: a processor samples the interrupt line at the end of an
instruction, not when a test would like it to. Asserting a level the mask
would refuse was the second mistake, and it was worse, because the line
then stayed up forever and no further interrupt was ever offered. The test
now fails if an asserted line goes unanswered for two hundred instructions,
so that particular silence cannot come back.

Comparing what memory *holds* rather than what each core *touched* took a
wrong turn first. Checksumming the access stream is the obvious design and
it fails immediately: one core fetches a whole instruction and moves a long
in a single access, the other prefetches through a sixteen-bit bus, so the
two streams differ everywhere while the machines agree perfectly. What has
to match is the result, and an incremental checksum over memory contents
says so after every instruction without walking four megabytes to find out.

Fixing the instrument also fixed the core: reading a fixed block of bytes
to decode from would have read past short instructions into whatever
follows them, and on a real board what follows can be a device register
that does something when read. The core now fetches one word at a time
until the instruction is whole.

## A second defect in the reference

The frame the reference builds when it takes an interrupt carries the wrong
vector word. Its interrupt path multiplies the vector number by four, and
then the frame writer shifts it left by two again, so the field holds
sixteen times the vector where the architecture wants four.

| | Level 6 autovector |
| --- | ---: |
| Architecture | 0x078 |
| Reference | 0x1E0 |

Only that field is affected. The handler address comes from the vector
number itself and is right, and a return from exception reads only the
format nibble, which is why machines built on the reference work at all.
What would notice is software that reads the field to learn which interrupt
it is holding, and whether any of these ROMs does that is not yet measured.

The lockstep test corrects the word in the reference's own image so the run
can go on comparing everything else, counts every frame it corrects, and
fails if the correction ever stops being needed. It was needed 397,238
times.

## Blocks

`src/cpu/m68k/core/m68k_block.{h,c}` is the layer between the interpreter
and a translator, and the one that decides whether a translator is
possible. It answers the three questions an emitter has to have answered
before it can emit anything.

**Where does a block end?** At a branch, a return, or anything else that
leaves straight-line execution. Not at an instruction that merely might
trap: the runner already checks after each instruction whether the program
counter went somewhere other than the next one, which is what a trap looks
like from here, and ending the block as well would pay twice for the same
guarantee.

**How is a compiled block kept honest when the guest rewrites the code
under it?** By comparing the bytes it was built from, on every entry,
against what is there now. Nothing has to announce a change: a program
modifying itself, a loader dropping new code in, and a restored state all
look the same and are all caught. The HIX-300 run below finds two.

**What does a block do about an instruction it cannot translate?** It calls
the interpreter for that one and carries on. That is what let the MIPS
engine be useful from the first instruction it supported rather than the
last, and it is the reason this core's semantics were written to take an
already-decoded instruction.

On its own the layer already earns its place, because the decode of a hot
loop happens once instead of on every iteration. It is validated by the
same lockstep comparison, run twice more: once a single instruction at a
time, so that every instruction is still checked against the reference, and
once with a real budget, so the cache is exercised the way it would be used.

| ROM | Instructions | Blocks built | Entries | Per block | Stale |
| --- | ---: | ---: | ---: | ---: | ---: |
| PIC-2000 | 20,000,000 | 189 | 10,545,200 | 1.9 | 0 |
| Envoy | 20,000,000 | 640,000 | 640,000 | 31.2 | 0 |
| HIX-300 | 9,756,193 | 8,495 | 5,429,765 | 1.8 | 2 |

Two of those numbers look wrong and are not. Blocks average under two
instructions on PIC-2000 and HIX-300 because twenty million instructions
are being spent inside 189 and 8,495 distinct blocks respectively: the
machines are sitting in tight polling loops, waiting on devices that are
not there, and a poll loop is a load and a branch. Envoy builds a block per
entry because it has walked off into unprogrammed memory, where a word of
zeroes is a valid instruction that never ends a block, so every block
begins at an address never seen before and no cache could hold it. The
reuse assertion is therefore made across the ROMs rather than for each one.

## Native code

`src/cpu/m68k/core/m68k_emit_x86.c` translates the instructions it knows
and calls the interpreter for the rest, from inside the same block. That is
the arrangement that made the MIPS engine tractable: no instruction has to
be translated before the thing works, a block never ends early because an
instruction is rare, and every family added is a straight win over the call
it replaces.

The emitted function keeps no guest state in a host register across an
instruction boundary. Everything lives in the `m68k` structure, so an
interpreter call in the middle of a block sees exactly the machine it would
have seen on its own. Holding state in registers is a later optimisation
and a separate argument.

Translated so far: `NOP`, `MOVEQ`, register-to-register `MOVE`, the
branches `BRA`, `Bcc` and `DBcc`, and `MOVE` between a data register and
ordinary memory.

Memory is reached through direct mappings rather than a call: one host
pointer per 4 KiB of guest space, supplied by whoever owns the bus, with a
page left out wherever an access has to go through the functions because
something happens when it does. A device register does not qualify however
ordinary it looks. An access to a page that is not mapped, or one that
would run off the end of a page, falls through to the interpreter for that
one instruction, which is why the emitted fast path forms the address and
looks the page up before it changes anything at all.

Condition codes being five separate bytes holding zero or one is what keeps
the emitted condition tests short. Half the conditions *are* a flag, and
the rest are one or two operations on two of them. A packed status register
would need a mask and a shift before any of that could start.

| ROM | Run by emitted code | Translated rather than called |
| --- | ---: | ---: |
| PIC-2000 | 19,985,563 | 52.7% |
| HIX-300 | 9,749,229 | 54.7% |
| Envoy | 19,840,000 | 0.0% |

Every one of those instructions was still compared against the reference,
because native emission is on for the lockstep runs: this is not a separate
benchmark but the same comparison with the emitter underneath it.

Envoy translates nothing because it is executing unprogrammed memory, where
every word is a zero and a zero decodes to `ORI.B`. Before branches were
translated the other two ROMs sat at 0.0% as well, for a related reason:
they are in tight polling loops, and a poll loop is a load and a branch, so
until the branch could be translated there was nothing in the loop to
translate. The measurement said which family to do next, and doing it moved
the figure from nothing to half.

The code arena is shared with the MIPS engine's design and lives in
`src/jit/code_arena.{h,c}`: one shared-memory object mapped twice, writable
for the emitter and executable for the processor, so no page is ever both
and no protection change happens per block.

One mistake there is worth recording because of how it presented. The space
reserved for a block was sized for the short instruction forms, and when
the memory ones arrived the emitter began reporting that it had run out of
room. The caller could not tell that apart from a full arena, so it threw
away every compiled block and started again: four million blocks built
where there are one hundred and eighty-nine, and three and a half million
arena resets, while every test still passed, because correctness was never
what had broken. A generous bound and telling the two conditions apart
fixed it. Being generous costs address space and nothing else.

## How fast any of this is

Measured rather than assumed, on one core of the development machine,
fifty million instructions per configuration:

| ROM | Interpreter | Blocks | Blocks and native code |
| --- | ---: | ---: | ---: |
| PIC-2000 | 8.9 M/s | 15.4 M/s | 18.4 M/s |
| Envoy | 6.9 M/s | 5.3 M/s | 5.1 M/s |

PIC-2000 is the representative figure: code that loops runs about twice as
fast as the plain interpreter. Envoy is slower than the interpreter because
it has walked into unprogrammed memory and every block it builds is used
once, so the building is pure overhead. A machine with its devices present
does not behave that way, and the two numbers together say something more
useful than either alone.

Two measurements changed what was built next, and both would have been
guessed wrong. The first time this was measured, native code was worth
nothing at all over plain blocks, and compiling was catastrophic on Envoy:

| ROM | Blocks | Blocks and native code |
| --- | ---: | ---: |
| PIC-2000 | 15.0 M/s | 15.1 M/s |
| Envoy | 6.2 M/s | 2.7 M/s |

The Envoy collapse was compiling every block on first sight, which for
code that never repeats is all cost. Waiting until a block has run sixteen
times fixed it. The PIC-2000 figure was the dispatcher: with blocks
averaging under two instructions, the work between blocks dominated
everything inside them, so translating the instructions inside changed
nothing measurable. Moving each block's own bytes check into the emitted
code took the dispatcher out of that path, and native code went from worth
nothing to worth a further 20%.

That is also the reason to do chaining next rather than translate more
instruction families: the measurement says the remaining cost is between
blocks, not inside them.

## A cycle count

The Envoy's battery RAM is a DS2223 on a one-wire line, and the difference
between writing a one and writing a zero is how long the host held that
line low, in processor *cycles*. This core counts instructions, so that
machine stood aside from the block engine entirely — and once the reference
became optional, from the build.

Measured before modelled. The guest's low pulses are 604 cycles for a zero
and 22 for a one against a 251-cycle threshold, which is a wide margin. Its
read slots sample at 726 and 772 cycles against a 755-cycle window, which
is a 2% one. A flat cycles-per-instruction rate does reproduce every
decision in a 200-million-instruction run — the window is 3.63 to 3.79 —
but only because those spans all run the same delay loop. That is luck, not
a model.

The retained table came from the former reference model, rather than from
measured MC68349 bus timings. Its key settles the cost for 1,522 entries.
Five rules cover what an encoding does not decide:

| | extra |
|---|---|
| a conditional branch, taken | 2, and only the byte form |
| a loop whose counter expires | 4 |
| MOVEM, per register past the first | 4 |
| a shift with its count in a register, per shift | 1 |
| a full-format indexed address | 2 or 6, by displacement size |

The removed `test_m68k_rom` comparison checked the two cores' counts
instruction by instruction on real ROM code, alongside the registers. That found
MOVEC costing twice as much writing a control register as reading one,
MOVES measured in only one direction, and a table emitted out of order for
the binary search that reads it.

Two things the numbers are not. They are a fact about the reference core
rather than about an MC68349: it is a 68020 model standing in for a CPU32
and its bus timing is not the real part's. And they are charged when an
instruction *finishes*, not when it starts — a device read made by the
instruction has to see the clock as it was before it, or every span is off
by the difference between the two instructions at its ends, which showed up
as 605 against 604.

Emitted code charges cycles too. Only two of the translated families have
an operand-dependent cost — a byte-displacement branch, which costs two
more taken, and a loop, which costs four more when its counter expires —
and each adds its extra where it decides it. Shifts are the third and are
translated only with an immediate count, which is flat. A core whose count
is read sets `count_cycles`; the rest do not pay the add.

The maintained `tests/cpu/test_m68k_emit.cpp` compares emitted execution
with the project interpreter for translated opcodes. Real-ROM cycle checks
and the Envoy reference-run comparison belonged to the removed differential
suite.

Interleaved on a loaded desktop, best of three, 200M instructions:

| | seconds |
|---|---|
| reference core | 11.17 |
| block engine, no native code | 8.86 |
| block engine with it | 6.78 |

The machine had no engine at all before this.

## Known gaps

`CHK2`, `CMP2` and the CPU32-only `TBL` decode but do not execute; none of
the three ROMs has been seen to use them.

Trace exceptions are not modelled; nothing in these ROMs sets the T bit.
And a halt — a vector whose handler address is itself odd — ends the
machine on the reference and does not here.

Interrupt entry, `LPSTOP`, `MOVES` and the odd-fetch address error used to
be here and are not any more. The address error builds the twenty-four byte
CPU32 format C frame in `m68k_address_error`, recorded against the
instruction that made the jump rather than the address it landed on; a
PIC-2000 boot takes 2,509 of them and its RAM is identical to the
reference's afterwards, which is what says the frame is right.

`LPSTOP` and `MOVES`: `MOVES` carries
a function code beside the access now, in `m68k.fc`, set only across the
memory end of the move and cleared immediately after, because resolving an
operand can read an extension word and that fetch belongs in the ordinary
space. The owner of the bus acts on it; on these boards the code that
matters is 7, CPU space, which is how the reset path reaches the MBAR.

## An unexplained refusal

Each compiled block begins by checking that it was entered at the address
it was compiled for. That check is needed anyway once blocks jump straight
to one another, because a block reached that way has no dispatcher to have
looked it up.

It was added while chasing a disagreement that appeared nine million
instructions into HIX-300, and it fixed it, which is not the same as
explaining it. The block being run was the right one for the address by
every measure available from C: its address matched the program counter and
its recorded bytes matched memory, checked immediately before entry. Yet
the emitted check refuses three entries in a run of nine million, and
without it those three ran code belonging to somewhere else.

So the check stays, and so does this paragraph. `MH_M68K_PARANOID=1`
re-runs the C-side version of it on every entry, for whoever picks this up
next.

## Running a machine on it

The project-owned core runs the PIC-2000 board by default. `--cpu-engine
interpreter` selects its single-step path, `blocks` selects cached
interpretation, and `auto`/`jit` select native blocks where supported.

This core holds the registers and runs all instructions observed in the
three ROM workloads. Historical comparisons reported no refused
instructions during cold boots; the fallback and handover code described
below belongs to the earlier optional-reference integration.

Device writes still yield to the board scheduler at the instruction that
caused them. The old reference handover path is compiled out; an unsupported
instruction is handled by the project-owned interpreter and its exception
logic.

### Stopping in front of an address

A machine watching for a program counter — to substitute a result, to start
a trace, to count a routine — otherwise has to look at every instruction,
which means running every instruction somewhere that looks. That was the
last thing keeping the reference busy: the HIX-300's ROM checksum does not
match and one recognised call is intercepted, and finding that one
instruction cost nine million on the reference.

`m68k.stop_pc` is the answer. A block is never built across it, the runner
returns with the program counter sitting on it having retired nothing, and
the machine does whatever it was watching for. `m68k_blocks_flush()` goes
with it, because a block built while nothing was being watched for may
cover the address now being watched and its bytes have not changed, so
nothing else would notice.

| HIX-300, 100M | reference | block engine |
|---|---|---|
| before | 9,274,514 | 35,492,865 |
| after | 262,956 | 44,504,423 |

The 262,956 that remained were the idle case — the part stopped on an
`LPSTOP`, which is where an idle machine spends nearly all of its time, and
which the machine was asking the reference about because the reference
owned the flag. Running that on this core took it to nought.

Going the other way, the machine asks for the processor back the moment a
device is touched. Its bus sets a flag the block runner tests, and emitted
code tests it after every instruction it handed to the interpreter, which
is the only kind that can have reached a device. The machine then services
the device at that instruction, exactly as its own loop would have.

Two things about time had to move with the processor. The instruction count
is a clock here -- dev21's counter register is that count scaled -- so the
count advances inside a block rather than at the end of one, which cost the
emitter a memory increment per instruction. And the Envoy is refused the
engine entirely: its battery RAM is a one-wire part whose data line is a
function of elapsed processor *cycles*, which only the reference counts.
An approximate rate would have hidden that rather than fixed it.

### Whether it is the same machine

Byte for byte, over the whole of RAM, at the end of runs of 200 million
instructions on PIC-2000 and 100 million on HIX-300: yes. The comparison is
`--dump 0,4194304` from a run on each engine, and it is a strict one --
the guest's own hash tables, timers and framebuffer are all in there.

Getting there found four things, all of them in the handover rather than in
the core:

- Both stack pointers were written back. In supervisor mode the inactive
  one is a stale copy, and writing it landed on top of the live one; the
  machine's stack moved half a megabyte, 120 instructions in.
- The device counter stopped while a block ran, because the count it is
  derived from was only updated when the block returned.
- An odd fetch was attributed to the address landed on rather than to the
  instruction that jumped there, and the ROM's own handler reads that field
  out of the frame.
- `RTE` was translated rather than handed back, so a format C frame was
  returned through as if it were a format 0.

`tests/runtime/test_pic2000.cpp` now builds a fixture that reaches those
places on purpose: it runs in user mode so exceptions swap stack pointers,
traps so the engine builds a frame the reference returns through, returns
from exception so the engine has to hand back, and samples the device
counter so the record covers what the machine thought the time was.

### What it is worth

On this desktop, PIC-2000 booting to the same state, wall clock, best of
three:

| engine | seconds |
| --- | --- |
| interpreter (Moira) | 0.61 |
| blocks, no native code | 0.52 |
| blocks with the x86-64 emitter | 0.48 |

So the block layer alone is worth a seventh and the emitter a good deal more
-- less than either is worth in the harness, because a machine spends most
of its slots in the idle path where no engine runs at all, and because a
third of the instructions inside blocks are still handed to the
interpreter.

Measured on an Orange Pi 5, the same run, best of three:

| engine | seconds |
| --- | --- |
| interpreter (Moira) | 3.26 |
| blocks, no native code | 2.65 |
| blocks with the AArch64 emitter | 1.99 |

Thirty-nine percent off, with byte-identical memory at the end of six
hundred million instructions. That is what settled `auto`: it is not a bet
on the engine being better, it is the measurement plus the comparison.

The cache is eight thousand blocks, sixteen megabytes, allocated only when
the engine is actually selected. A PIC-2000 boot builds under twenty
thousand blocks and evicts two thousand of them against three million
entries, so the cache is not where the remaining cost is; four times the
size cuts evictions to three hundred and changes the wall clock by nothing
measurable, and these machines run on tablets.

### What it costs in diagnostics

An exception the engine raises does not pass through the reference core, so
it does not reach the `[68k] privilege violation at pc=...` lines that have
been how this machine's boot was read. The engine reports a count of them
at exit instead, and `--cpu-engine interpreter` names them again. Worth
remembering before reading an exception log taken with the engine on: the
log is not wrong, it is partial, and the reference's own budget of
thirty-two lines is then spent on a different mix.

### Two hosts

There are two backends, `m68k_emit_x86.c` and `m68k_emit_a64.c`, and a
third file that answers for everything else by declining. Which one a build
gets is decided once, in `m68k_emit.h`, so that the three cannot all claim
the host or all refuse it. A big-endian AArch64 is not one of ours: the
byte reversal in the generated loads and stores assumes it is not.

The two are written to be read side by side, register for register:

| role | x86-64 | AArch64 |
| --- | --- | --- |
| the m68k structure | rbx | x19 |
| instructions retired, and the result | r12d | w20 |
| readable pages | r13 | x21 |
| writable pages | r14 | x22 |

What each translates is *not* written twice. `m68k_emit_policy.h` holds the
list, because it is a policy and not an encoding: the same instructions are
worth translating whatever the processor underneath. A copy per host would
let the two drift, and a drift there is the worst bug this code can have --
one machine running a different set of instructions natively than the
other, so that a disagreement appears only on the hardware you are not
holding.

AArch64 is the better fit for this guest in one specific way. Every load
and store has to change byte order, and `rev` and `rev16` do it in one
instruction on a register the value is already in; the interpreter
assembles the same value a byte at a time. The guard is the other
difference: x86-64 compares against literals built inline, which is up to
seven instructions per eight bytes, while AArch64 keeps a pointer to the
bytes it expects -- they live in the block cache entry, whose lifetime is
already the code's -- and compares memory against memory in four.

Before any of it ran anywhere, the generated code was disassembled. A
block with one of each translated form was emitted on the desktop with the
AArch64 backend and read back through `objdump`, which is how a wrong bit
in a hand-assembled instruction gets found in a minute instead of by
bisecting a divergence.

### Switches, for when the two disagree

`--cpu-engine interpreter` puts the reference back, which is the first
thing to try when a machine misbehaves. `MH_M68K_EMIT=0` keeps the block
layer and drops the native code, which separates an emitter bug from a
core one in a single run.

`MH_68K_CORE_TRACE=<n>` prints the first n instructions with their stack
pointer, from whichever engine ran each one, so a trace taken on the block
engine can be diffed line for line against one taken on the reference.
`--trace` cannot do this: the loop that prints is the one the engine
replaces, so both of its traces are the reference's. Stepping the engine
one instruction at a time to print is slow and changes when it hands back,
so a disagreement that only appears at full speed will not reproduce under
it -- but a disagreement in what the instructions computed will, and that
is what it found.

### What the emitter covers, and why that list and not another

Two thirds of the instructions inside a compiled block now run as native
code; the rest are calls into the interpreter. The list grew by
measurement, not by working through the instruction set: `--insn-heat`
ranks what the machine actually executes, and each round took the largest
entries that needed no new machinery.

| step | translated inside blocks | Orange Pi 5 |
| --- | ---: | ---: |
| moves, branches, the quick constants | 42% | 2.34 s |
| the arithmetic, comparison and logical register forms | 58% | 2.09 s |
| constant shifts and rotates, and jumps | 62% | 2.05 s |
| calls and returns | 67% | 1.99 s |

The arithmetic step is the one that paid. All of it is emitted the same
way: both operands go to the top of a host register before the operation,
so the processor's own carry, overflow, sign and zero come out at the
guest's width rather than the host's. That is why byte, word and long need
no separate cases, and why the flags are exact rather than reconstructed
from the result.

The two hosts disagree about one thing and it is worth knowing: after a
subtract, AArch64 leaves carry set when there was *no* borrow, the
opposite of what the guest means by it. The AArch64 backend takes the
guest's carry from the inverse condition, and the emitter test is what
would have caught it if it did not.

What is still handed over, by share of everything the machine executes:
`cpGEN` at 12.4%, which is software 68881 emulation and raises rather than
computing, so translating it means emulating the coprocessor natively;
`MULL` at 5.5%; and the two status-register moves at 5.5%, one of which is
privileged and therefore deliberately out of reach -- generated code does
not make the privilege check, and the policy header refuses anything
marked privileged so that adding one cannot go wrong quietly.

### Where the cost is not

A block averages under four instructions, so the obvious next move was to
stop paying for a lookup per block. Each entry now remembers whichever
block ran after it last time, and following that pointer -- one address
comparison -- replaces the hash, the four-way scan and the bytes
comparison. It hits on 96.4% of entries.

It bought about 2%. Which says the dispatcher was not the cost, and that
the earlier note here claiming it was had measured the standalone harness,
where a ROM with no machine under it sits in a two-instruction poll loop
and does nothing else. Under a real machine the time is in the
instructions, in the devices, and in the handovers. The link stays because
it is small and it is free, but the next thing to do is not more of that.

## Next

### Where the time goes

The 68k engine is slower than the MIPS one, and on this desktop by a
factor that is worth writing down. 200M instructions, interleaved, best of
three:

| | seconds |
|---|---|
| MIPS, `--cpu-engine jit` | 1.15 |
| 68k, before chaining | 2.80 |
| 68k, after | 2.16 |

The difference was never the code the two emit. It was how often they stop
emitting it. The MIPS engine dispatches once per 341 instructions; the 68k
engine dispatched once per **2.8**, because every emitted block returned to
C and 68k code is branch-dense enough that a block is three instructions
long. Chaining took that to 15.1 — see [Chaining](#chaining) below.

A note on a number that used to appear here as evidence that chaining was
already done: 84.7% of block entries were "reached from the block before".
That is the C-side pointer guess, which saves the cache lookup and nothing
else — the block still returned to C. The two are easy to confuse and were.

The other thing the switches say, best of three on a PIC-2000 boot:

| | seconds |
|---|---|
| block engine, native code and translation | 4.40 |
| block engine, no native code at all | 5.67 |
| native blocks, every instruction an interpreter call | 6.54 |

That last line is a finding of its own. Emitting a block and then calling
the interpreter for every instruction in it is *slower* than not emitting
at all — the call stores a program counter and checks two conditions, which
the runner's own loop does more cheaply. So the fraction translated is what
makes the arrangement pay, and raising it is worth doing.

Registers in host registers across a block, which used to head this list,
is not: blocks average three instructions, so there is nothing to hold.

### Chaining

A block already checked its own program counter and guarded its own bytes
on entry, so that one block could later jump straight to another. What was
missing was a budget, somewhere to jump to, and a way to find out where.

A block now has a *chain entry* — the program counter, the budget and the
guard — and ends with a jump that starts out going to a stub and is
redirected at whichever block followed it. The frame and the retired count
belong to whichever block was called, so a chain returns one count for all
of them. `MH_M68K_LINK=0` turns it off for an exact comparison.

Four things have to be right, and three of them were found by being wrong:

- **The exiting block names its own link.** Working it out from the
  caller's side does not survive chaining, because the block the caller
  dispatched and the block that finished are different blocks. Guessing
  cost 2.5M relinks and a sixty-fold slowdown.
- **A chain checks `stopped` as well as `yield`.** LPSTOP retires like any
  other instruction; without the check the machine never idled, running
  199M instructions where 57M should have.
- **A link may only point at a block that guards its own bytes.** One whose
  source is not a single host-backed span has no guard of its own and is
  checked by the caller instead, which a chain jumps past. The guest
  rewrites its trampolines in low RAM, so this showed up there.
- **Emitted code owns the decoded instructions it calls the interpreter
  with.** They used to live in the cache entry, which is evicted and
  rebuilt for other addresses while code that jumps to it is still
  reachable; they go in the arena with the code now.

What is left between 15.1 and the MIPS engine's 341 is the successors a
link cannot predict. A link holds one target, and `RTS` returns to a
different caller each time; the MIPS engine searches its block table from
inside generated code for those. That is the next piece of work.

`--insn-heat` reports that fraction and what is below it, weighted by how
often it runs, through the same predicate the emitter uses. It is worth
about 0.03 s per point of coverage on this workload. As of the last
measurement 70.5% is translated, and the top of what is not:

| | share of what ran |
|---|---|
| `ADD (An)+` | 5.7% |
| `MOVEA (xxx).w` | 2.1% |
| `CMP (d16,An)` | 1.3% |
| `MOVETSR Dn` | 1.3% |
| `BSR` | 1.2% |

which names the next piece of work: ALU with a memory source, sharing the
inline memory path the moves use. The rule that path has to keep is that
it changes nothing until the page lookup has succeeded, so an access it
cannot finish can still start over in the interpreter — which is why the
modes with a side effect, `(An)+` and `-(An)`, are the awkward ones and
why indexed addressing was easy.

None of this is pressing. These machines run about thirty times faster
than the hardware already.

1. Following a successor a link cannot predict, by searching the block
   table from inside generated code — which is what stands between this
   engine dispatching every 15.1 instructions and the MIPS engine's 341.
2. ALU with a memory source, as above.

Four things that used to be on this list are done. `--insn-heat`, the
per-vector exception counts, the naming of each exception as it is taken
and the exception stack frames all work in either build now, through our
own decoder and our own core. Emitted code charges cycles, so the Envoy has native code as well as
a clock. And interrupt entry: `run_on_core` and
`core_step_one` both take an interrupt that is already due *before*
running, not after, because taking it after lets a stretch run past the
point the part would have vectored — a difference in HIX-300's RAM within a
hundred million instructions. That is done too. Across a whole run these machines raise six
to twelve interrupts, the engine sees every one and takes what the mask
allows, and the number of stretches skipped because an interrupt was
waiting is zero.

`CORE_MIN_RUN` is 1. It was 64 to keep the state exchange from costing more
than the engine saved, and to stop a hot loop built around an unimplemented
instruction paying that exchange every iteration — and the second hazard
stopped existing when the refusal list emptied. Measured, whole of RAM
identical at every setting:

| `CORE_MIN_RUN` | on this engine | skipped short | best of three |
|---|---|---|---|
| 64 | 292,112,564 | 7,098,845 | 9.80 s |
| 16 | 298,760,644 | 450,765 | |
| 4 | 299,188,873 | 22,536 | 9.58 s |
| 1 | 299,211,409 | 0 | |

It sat at 4 for a day because the give-up guard was derived from it and 1
would have retired the guard. The guard has its own constant now —
`CORE_GIVE_UP_AVERAGE` — because the two say different things: the gate is
how short a stretch is worth offering, and the guard is how badly the
engine has to be doing before the machine stops offering at all.
`MH_68K_MIN_RUN` overrides the gate, which is how that table was made.

The old profile note about time spent in the reference core's virtual memory
API is obsolete. The current emulator has no reference-core dependency.
