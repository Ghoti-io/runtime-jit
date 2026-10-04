# Design

**Status:** In progress. Describes what exists: the IR with its builder,
verifier and printer, the x86-64 baseline backend with its assembler, the
stack maps and deopt records it emits in `runtime-core`'s format, W^X code
memory, the gates, the planted-defect proofs and the benchmark harness, taken
from the runtime stack's architecture spine (AD-2, AD-4, AD-9, AD-12, AD-13,
AD-14, AD-17 to AD-19, AD-21, AD-22, AD-26). What is not here is listed at the
end.

## What this library is

`runtime-jit` turns a function written in a low-level IR into machine code, and
says, for every place where a collection can run or a guard can fail, where the
live references are and where each slot of the interpreter frame could be
read. It does not run guest code, rebuild interpreter frames, schedule
anything or pick what to compile: an engine does those, and the first engine to
do so is `lang-tang` (story 15, "The baseline JIT" in its design). It depends on `cutil` and `runtime-core`
only, may include all of `runtime-core` (it emits against A's formats), and
accepts no collector type (AD-2): the barrier and allocation-fast-path code the
collector supplies will be passed in by the engine, in a later story.

## Where the formats live

The stack-map and deopt format is in `runtime-core` (`a/codemeta.h`), not
here, and so is the layout descriptor (`a/layout.h`), added in the same story
and in three commits of their own. AD-1 and AD-14 put them in the core:
the reader of a compiled frame (the collector's root source for JIT frames,
pause-time rebuild, the debugger's frame walk) is written against the format,
and a second backend (arm64, an ahead-of-time C backend with shadow frames)
fills the same table. The rejected alternative is a format owned by this
library, which would make the core depend on the JIT to read a frame, or make
the first writer the only definition. With the format in the core the reader
exists before any writer does, and this library is the first writer, checked
against the validator before `grjit_compile` returns (a failure is
`GRJIT_ERR_INTERNAL` and nothing is returned).

A `protect` member on the page provider (also `runtime-core`) is how code is
made executable. The rejected alternative is this library calling `mprotect`
itself: it would map memory the context's meter never sees, which AD-13 and
AD-20 forbid, and would put the Windows branch in two libraries.

## Non-SSA registers now, SSA when a pass wants it

The IR's virtual registers may be assigned more than once. SSA is what passes
want, and the passes arrive when a benchmark names a function the baseline
cannot serve (AD-26). Building SSA for a baseline that does no pass would be
cost with no reader; building the IR so that SSA can be constructed from it
later costs nothing (blocks, explicit terminators, a verifier that knows what
is assigned where). The verifier's must-be-assigned dataflow is the first
piece of that.

## The baseline: every value in a frame slot

A prologue `push rbp; mov rbp, rsp; sub rsp, N` with `N` a multiple of 16, so
`rsp` is aligned at every call (nothing is pushed afterwards). Below `rbp`: the
context pointer (-8), `out` (-16), `args` (-24), then one 8-byte slot per
register, register `v` at `-8 * (v + 4)`. Each operation loads its operands
into `rax` and `rcx`, does its work, and stores the result; a call loads its
arguments into the SysV argument registers straight from their slots. All of
those are caller-saved, so **no GC reference is ever in a callee-saved register**
and AD-17's rule holds by construction, with nothing to check.

Rejected:

- **Linear-scan allocation now.** AD-26 wants a benchmark naming a function the
  baseline cannot serve first, and no engine exists to supply one. A register
  allocator is also the part where a reference ends up in a callee-saved
  register, which is where AD-17's rule would have to be re-proved.
- **Values in callee-saved registers.** It breaks AD-17's rule and every
  consumer's reading of a frame.
- **A frame without `rbp`.** The metadata's "frame base" is `rbp`, and every
  consumer's frame walk (the collector's, the debugger's, a helper that reads
  its caller's slots, as the read-back test does) would need unwind info before
  anything needs it. Compiled code keeps `rbp` frames so that a native helper
  can walk to its caller's.

A frame over `max_frame_bytes` (default 1 MiB) or code over `max_code_bytes`
(default 16 MiB) is `GRJIT_ERR_LIMIT` with nothing changed.

## The calling convention, and the guard exit

Compiled code is `uint32_t (*)(void * context, const uint64_t * args, uint64_t * out)`
(SysV). It returns `GRJIT_EXIT_RETURNED`, `GRJIT_EXIT_DEOPT` or
`GRJIT_EXIT_REFUSED`, and `out` is a record of what happened: the value, the
reconstructed interpreter slots and the site's offset, or the refusing answer.
There is no activation record, no guest-stack frame and no native-depth
accounting in compiled code: the engine's entry trampoline pushes the
`GRCORE_ACTIVATION_JIT` record, which is what enters the native-depth budget
(AD-17, AD-21).

**A guard's exit is a return**, not an in-place trampoline. The exit stub copies
each frame-state slot (a register's frame slot, a constant, or zero for a dead
slot) into `out[i]`, stores the site's code offset after them, and returns. The
alternative, jumping into the interpreter from the middle of compiled code,
would need the interpreter's frame rebuilt while the compiled frame is still on
the native stack, and would put engine knowledge into this library. Returning
leaves the rebuild to the caller, which can read the same locations at a poll or
a call (the format is shared) with the compiled frame gone.

The **entry hook** is the stand-in for AD-21's "checked at every JIT prologue":
a function can name a `uint32_t (*)(void * context)` called before anything
else, and a non-zero answer makes it return `GRJIT_EXIT_REFUSED`. It runs before the
parameters are in their frame slots and no site is recorded for it, so **a hook
must not reach a GC point**: it may not allocate or poll, or the caller's
reference arguments would be unmapped while a collection ran. It exists so
that the check has a place and a test; the engine's trampoline owns the real
accounting.

## Polls

A poll is `mov rax, [ctx]; mov rax, [rax + request_word_offset]; test rax, rax;
jnz slow`, with the offset from `grcore_jit_layout()` read at compile time, and
an out-of-line slow path that calls the helper the function declared
(`grjit_builder_set_poll_helper`: `uint32_t (*)(void * context, uint64_t function, uint64_t offset)`)
and resumes after it. The poll's identity (AD-18) is the frame state's, is
recorded in the site, and is passed to the helper; the backend does not merge
polls in this version, but the identity is recorded anyway, which is what AD-18
requires of any later merge. A non-zero answer from the helper ends compiled
code as `GRJIT_EXIT_REFUSED`: **a pause or unwind verdict ends compiled code in
this story**, and rebuilding frames to continue in the interpreter is the next
story's, because that is where the interpreter and the rebuild exist. The
rejected alternative, pausing inside compiled code (the helper blocks, or the
code is resumable), would make compiled frames part of the pause protocol before
A can walk them.

A **layout descriptor** (`a/layout.h`) rather than `offsetof` in a shared
header: compiled code is shared between contexts and reaches state through a
context register (AD-22), so the offsets it may bake in must be stated rather
than inferred, and AD-19 says keyed state is found by key, never by position, so
the descriptor has exactly one entry. The offset is computed from the real
struct and a test reads the word through it. The offset is also stored in the
code, and `grjit_code_call` asserts it still matches (in a build without
`NDEBUG`): code is valid for the build that compiled it.

## Sites and metadata

One site per `POLL` (at the return address of its slow-path call), per
`GC_POINT` call (at its return address) and per `GUARD` (at the start of its
exit stub). The return address is the key because that is what a native helper
or a collector holds when it looks at a frame; the alternative, the address of the
call, would need the instruction's length to find. A plain `NO_GC` call is not a
site.

The stack map is every register of type REF that is live across the site, by a
backward dataflow over the blocks (`src/backend/liveness.c`). For a poll or a
call, live means "used after it, or named by its frame state, and not the
register the call assigns"; for a guard, the exit stub leaves the function, so
it means "named by its frame state". `PTR` and `I64` are never in a map. A
*derived pointer* is a `PTR` declared with `grjit_builder_derived`; a site where
it is live records `(slot, base slot, delta)`, and the verifier refuses a
function in which a derived pointer is live at a site where its base is not (the
base would be unrooted and the pointer could not be rewritten, AD-12). The
liveness is shared by the verifier and the backend, which is why it is a pass of
its own rather than part of the emitter.

The read-back test is the proof that this describes real frames: compiled code
calls a native helper at a GC-point call and at a poll, the helper follows the
saved-`rbp` chain to its caller's frame, computes the return address's offset,
finds the site, and reads every live slot and every deopt slot at the recorded
offsets, and the test compares the values with the ones the program holds. It
compares values, not offsets, so it needs no knowledge of the frame layout.

## What the backends share

Everything that is not instruction encoding is in `src/backend/` and is used by
every backend: the liveness pass (which the verifier uses too), the metadata
builder, the frame layout (`backend_internal.h`) and the compile state that is
not an instruction set's (`GRJIT_EmitCommon`: the sites the emitter records, the
stubs it queues, the sort that puts the sites in order). Each backend's
directory holds its assembler, its operation emitter and its stubs. They share
the frame layout because the stack-map and deopt format, and every consumer's
frame walk, are written against it; keeping the frame base and the slot offsets
makes the second backend a matter of encoding. `test_pin.cpp` records a hash of
every byte of the code of the differential's 2000 generated functions, so a
change that is meant to leave the code alone (the move to this directory was
one) fails if it does not.

## The assembler

`src/x86_64/asm.c` is new code (AD-9, spine Supersedes: not seeded from ctang's
`binary.h`). It encodes what the backend emits and no more, checked against byte
sequences written out in the test and, where `objdump` is present, against its
disassembly (its absence fails the test on this target). Forward branches are
`rel32` with a fixup patched when the label is bound; a backward branch within
128 bytes is `rel8`. Growth and the byte cap are remembered as a status instead
of being reported per instruction, so emission code is not a ladder of checks.

## Code memory

One `GRJIT_Code` owns one page-rounded mapping from the context's page provider
(so the pages are charged to the meter and a failed compile gives them back),
filled while read-write, then made read-execute once with `grcore_page_protect`.
It is never writable and executable at once, a test proves it from
`/proc/self/maps` (and, because Valgrind's `/proc/self/maps` is not the
kernel's, a second test that writes to the code and requires a fault runs
everywhere). Destroying code unmaps it: destroying code a thread still runs is
the caller's error. x86-64 needs no instruction-cache maintenance after the flip.
The code is assembled into a buffer from the caller's allocator and copied
into the mapping, because its size is not known until the assembler is done.

`GRJIT_Code` is single-owner. Reference counting of compiled code is the core's
(AD-13): `lang-tang` wraps each code in a `GRCORE_Code` (`a/code.h`), whose
release callback is `grjit_code_destroy`, and keeps it in a cache per execution.
A code cache shared between contexts is still not here.

## Why the IR has no `gc_store`

A store of a REF into a GC object is not expressible: the verifier refuses a
`STORE` whose value is a REF, and heap stores go through an engine-supplied
helper `CALL` (the engine's `gc_store`, AD-11). This library never emits a
barrier, so a barrier mode that changes how the collector works changes no code
here, and the IR needs no knowledge of the collector's types.

## Why `BITCAST`

A tagged value of a dynamic language is a word that may be a reference, so the
engine holds it in a `REF` register, and arithmetic is defined on `I64` and
`PTR` only: the verifier refuses an `ADD` of a `REF`, and `MOVE` refuses to
change a register's type. Without one more operation an engine could not
compute on a tagged value at all: no way existed to shift the tag off, add, and
tag the result. `BITCAST` copies the 64 bits between two registers of any of the
types `I64`, `REF` and `PTR`; the destination's type decides what the word is
from then on. It is a reinterpretation and not a conversion, which is the
narrowest thing that serves: it does not widen what `MOVE` accepts, so a type
change is still always spelled out. The liveness pass sees it as a use of the
source and a definition of the destination, so a `REF` source stays in a stack
map for as long as that register is live and the `I64` it was cast to never
enters one: **a bitcast from `REF` to `I64` never makes the computed value a
reference**, and the reverse makes a reference only by naming a value the
engine has built as one. The library still emits no barrier and still has no
`gc_store`; a `BITCAST` writes a register, never a heap word.

Two consequences for a consumer. An `I64` copy of a `REF` is plain bits: it is not
in any stack map, and a collector that moves objects rewrites `VALUE` slots only
(`grcore_deopt_write_back` covers nothing else), so the copy goes stale at a move
and must not be held across a GC point as if it were a reference. And a `REF`
register may hold a non-pointer tagged word (a small integer, a boolean), because
a `REF` is an engine value word, so a reader of stack maps must tolerate slots
that name no object.

## The IR is never interpreted

The library has no evaluator (AD-9: each language owns its interpreter, and the
IR is not a bytecode). The test-only `tests/ir_eval.h` evaluates it to be wrong
in a different way from the backend, and the differential compares them on 2000
generated functions per run (fixed seeds, acyclic and bounded-loop, every
operation, immediates at the edges, shifts by 0, 1, 63 and by registers holding
63 and 64, every comparison, every memory width, calls with 0 to 6 arguments,
GC-point calls, polls and guards that sometimes fail). It compares the result,
the memory image the function left, the helper call log, and, for a guard that
fails, the reconstructed slots and the kind and identity of the site the exit
names. `make check-planted` proves it can fail: the library is built with
`SHR` and `SAR` swapped, and the differential must fail on it (it does, on the
third function), as the read-back must fail on a stack-map slot 8 bytes off and
on a live reference left out.

## No fuzzer

AD-16 asks for a fuzzer where there is a parser of untrusted bytes. There is no
loader for the IR and none for compiled code here: a function is built through
the builder, and the compiled-code cache is never released (AD-14). So there is
nothing to fuzz in this version. The differential generator, which is in
`make test`, is the library's stand-in. The day a serialised form of the IR
exists, the story that adds it adds a harness.

## Gates

`make check-labels` requires every header to carry exactly one `@stability free`
label. `make check-edges` is an allowlist of `cutil`, `runtime-core` and this
library, over the `#include` lines and the shared object's `NEEDED` entries;
`runtime-heap`, `runtime-debug`, `lang-*`, `tang` and `text` are all edges it
refuses. `make check-gates` proves each by running the real scripts against a
planted fixture that must fail, naming what it found, a control that must pass,
and an empty tree that must fail rather than report success over nothing.
`check-planted` is the same idea for the backend. The direction gate is not
here: the headers are flat.

## Benchmarks

`make bench` has a calibration case, compiling a 100-operation function, and a
counting loop of 10 million iterations three ways: with no poll, with an inline
poll whose fast path is taken, and with a call to a no-op `NO_GC` helper on
every iteration. `make test` runs it once, small. No budget is asserted (AD-26).

First measurement, 2026-10-03, an Intel Core 7 150U, GCC 14.2, release build
(minimum of 5 repeats; medians in brackets):

| Case | ns per op |
| --- | ---: |
| calibration (a serial xorshift step) | 1.14 [1.16] |
| compile a 100-operation function, map, flip, unmap | 7870 [8100] |
| loop iteration, no poll | 0.68 [0.70] |
| loop iteration, inline poll, nothing pending | 0.75 [0.77] |
| loop iteration, call to a no-op `NO_GC` helper | 1.57 [1.59] |

**A poll with nothing pending costs about 0.07 ns per iteration** on this machine
(0.07 [0.07] from the two rows above): a load, a test and a branch, which the
processor overlaps with the loop's own work. The call costs about 0.9 ns. These
are statements about the baseline: every register is stored and reloaded each
iteration, and a modern core forwards that store to its load, which is why a loop
iteration is well under a nanosecond. A different machine will move all of the
numbers; the calibration row is what to read them against.

## What is not here

- **Code shared between contexts** and a compiler thread. (Wiring it into
  `lang-tang`, tier-up, a per-execution cache and reference counting by the core
  are story 15's, and exist.)
- **Rebuilding interpreter frames from compiled ones** is the engine's, and
  `lang-tang` does it with `runtime-core`'s `a/deopt.h` at every poll and on
  every guard exit. Walking native frames for roots is still not here: no frame
  walk of native frames exists in `runtime-core`, and `lang-tang` is built so that
  it needs none (compiled code has no GC point except a poll that first writes
  the guest frame).
- **Windows.** The page-protection path is `runtime-core`'s `VirtualProtect`
  branch (written, not run); unwind registration (`RtlAddFunctionTable`) is a
  stub that returns `GRJIT_ERR_UNSUPPORTED`, marked `TODO(windows)` in place;
  and compiled code is for Linux x86-64 only, so `grjit_backend_available()` is
  false elsewhere.
- **arm64**, an ahead-of-time C backend, a Wasm backend, JIT hardening, a
  register allocator, any pass, SSA, inlining, unboxing, floating point, SIMD,
  32-bit and 8-bit values.
- **An in-JIT pause.** A poll's non-zero answer ends the function; see above.
- **The AD-21 prologue check as more than a hook.**
- **A fuzzer**, for the reason above.
