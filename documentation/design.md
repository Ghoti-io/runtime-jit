# Design

**Status:** In progress. Describes what exists: the IR with its builder,
verifier and printer, the x86-64 (Linux and Windows) and arm64 baseline backends
with their assemblers, the stack maps and deopt records it emits in
`runtime-core`'s format, W^X code memory and the Windows unwind registration,
the gates, the planted-defect proofs and the benchmark harness, taken
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
arguments into the argument registers of the target's calling convention
straight from their slots. All of those are caller-saved, so **no GC reference
is ever in a callee-saved register** and AD-17's rule holds by construction, with
nothing to check. (On Windows `N` also holds a 48-byte outgoing area; see "The
Windows x86-64 flavour".)

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
in the calling convention of the target: SysV on Linux (the three arguments in
`rdi`, `rsi`, `rdx`), Microsoft x64 on Windows (`rcx`, `rdx`, `r8`), AAPCS64 on
arm64. It returns `GRJIT_EXIT_RETURNED`, `GRJIT_EXIT_DEOPT` or
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

## The arm64 backend

`src/arm64/` is the second backend (CAP-14), with the same shape as the first and
the same frame, which is the whole idea: the stack-map and deopt format, the
liveness pass, the metadata builder and every consumer's frame walk are written
against that layout, so a second instruction set is a matter of encoding.

**Frame and registers.** The prologue is `stp x29, x30, [sp, #-16]!`, `mov x29,
sp`, then the frame is allocated (`sub sp`, in one or two instructions) so that
`sp` is a multiple of 16 and is never moved again, so it is aligned at every
call. The frame base of the metadata is `x29`. Slots are at the offsets x86-64
uses: context at `-8`, `out` at `-16`, `args` at `-24`, register `v` at `-8 * (v
+ 4)`. `[x29]` is the caller's frame pointer and `[x29 + 8]` the return address,
exactly the pair `[rbp]` and `[rbp + 8]` are, which is what a native helper
(`lang-tang`'s poll helper, the read-back test) follows to walk to its caller.
Scratch registers are the caller-saved `x0`-`x17` only, so **no reference is
ever live in a callee-saved register** (AD-17 holds by construction, as on
x86-64). The roles: `x0` and `x1` carry the operands and `x0` the result; `x2`
holds `out` in the exits; the arguments of a helper call are `x0`-`x5` (the IR's
six); a call is through `x16` loaded by `movz`/`movk` and `blr`; `x17` holds a
displacement that does not fit an instruction. The platform register `x18` is
never touched. A helper that returns a `uint32_t` may leave the upper half of
`x0` undefined (AAPCS64), so the entry hook's and the poll helper's answers are
cleared with `mov w0, w0` before they are tested or stored.

**The semantics are the IR's, not the instruction set's.** `lslv`, `lsrv` and
`asrv` take the count modulo 64 as the IR says; `mul` wraps; a comparison is
`cmp` and `cset` (0 or 1); a load of 8, 16 or 32 bits zero- or sign-extends as
`LOAD` and `LOAD_S` say and a narrow store stores the low bits; an immediate is
any 64-bit value and a displacement any `int32`. Unaligned access is allowed by
the IR and by AArch64 normal memory, and nothing extra is done for it.

**What a fixed-width instruction set costs, and what each hazard cost.**

| Hazard | What was done |
| --- | --- |
| A slot offset beyond the signed 9-bit unscaled range (`ldur`, -256..255) or any `disp` beyond the scaled 12-bit range | One instruction when it fits (scaled if the displacement is non-negative, a multiple of the access size and in range; unscaled if it is -256..255); otherwise the displacement is built in `x17` and the register-offset form is used. Slots beyond `-256` are register 29 and up, so a function with more than 28 registers has some. Never truncated: a test sweeps every width and sign around every boundary and runs it. |
| A frame over 4095 bytes, up to the 1 MiB cap | `sub sp, sp, #hi, lsl #12` then `sub sp, sp, #lo`, each a multiple of 16 so `sp` is aligned between them. Over 16 MiB (a raised cap) is `LIMIT`, not a truncation. At the cap runs; one register over is `LIMIT` as on x86-64. |
| A conditional branch reaches only +-1 MiB (`b.cond`, `cbz`, `cbnz`) while `b` reaches +-128 MiB and the code cap defaults to 16 MiB | See below. |
| A 64-bit immediate | `movz` or `movn` (whichever leaves fewer pieces) and `movk` for each 16-bit piece that differs: one to four instructions. |
| The instruction cache | After the code is written and before the mapping is made executable, `__builtin___clear_cache` runs over it (arm64 only). |

**Branches.** Every `b` reaches every block of code up to the cap (a cap set
above 128 MiB makes an unreachable `b` a `LIMIT`). A conditional branch is
assembled short, with a fixup, and the whole function is assembled a second time
in *long* mode if a fixup finds a label out of reach: the long form is the
inverted test over the next instruction, then a `b`. The second assembly happens
only for a function with a conditional branch more than 1 MiB long, which is a
function of more than a mebibyte; every other function pays nothing, and a
backward branch that is out of reach is already long in the first assembly.
Poll and guard stubs are emitted after all the blocks, so a function over 1 MiB
reaches them by the long form too. A test builds functions with a forward branch
over more than a mebibyte, a backward one, and a poll and a guard whose stubs are
that far away, and runs them.

**The instruction cache, and exactly what `qemu-user` cannot prove.** AArch64's
instruction and data caches are not coherent. Code written through a data
address is not visible to instruction fetch until the line is cleaned to the
point of unification and the instruction cache invalidated. `grjit_memory_create`
calls `grjit_icache_sync` (a wrapper over `__builtin___clear_cache`) between the
last write and the flip to read-execute, once per mapping. **`qemu-user` translates
lazily, one block at a time when it first runs it, and does not model the cache:
code it runs would work with or without that call.** So no run under the emulator
can prove the call is right. What is tested is that the line is *reached*: the
function counts its calls, and a test shows that an arm64 mapping is synced once,
before `protect` is called (a page provider that records the count when its
`protect` runs), and an x86-64 mapping is not. Real arm64 hardware has not been
exercised by this library, and that line is the one thing that would fail
there and not here.

**How it is tested without a machine to run it on.** The assembler is C that
emits bytes, and it is compiled and tested on every host: each instruction is
compared with the bytes `aarch64-linux-gnu-as` produces for the same text
(recorded, with the disassembly `aarch64-linux-gnu-objdump` gives back, which
`tools/xarch/jit-arm64.sh` re-checks). `tests/a64_sim.h` is a small simulator of
exactly the subset the assembler emits: it decodes only those encodings (any
other word stops it), executes them against real memory, and checks what AAPCS64
asks of compiled code: the callee-saved registers, `x29`, `x30` and `sp` are what
they were on return, `x18` is never touched, `sp` is aligned at every call, and
after a call the caller-saved registers are noise. The differential runs the
arm64 emitter's output in it against the evaluator on the same 2000 functions as
the native one. And `tools/xarch/jit-arm64.sh` runs the real thing: it builds the
stack for aarch64 and runs every test of this library, and `lang-tang`'s JIT arm,
under `qemu-aarch64`, with the arm64 planted defect (`SHR` and `SAR` swapped in
the arm64 emitter) caught by the native differential there.

**Pointer authentication and BTI.** Compiled code and the frame-record walk
assume the saved link register at `[x29 + 8]` is a plain return address: nothing
signs it (no `paciasp`/`autiasp`, no `pac-ret`), and nothing marks the code with
branch-target landing pads. A build that signs return addresses (`-mbranch-protection=pac-ret`),
or a process that enforces BTI on executable pages, would make the walk read a
signed address and would trap on entry, so **PAC-enabled and BTI-enforcing
builds are unsupported and untested**. The helper's walk
(`lang-tang/src/jit/helpers.c`) would need to strip the signature (`xpaclri`)
before using the return address.

**Rejected:**

- **Values in callee-saved registers** (`x19`-`x28`). They are why a second
  backend is usually fast, and they break AD-17 and every consumer's reading of a
  frame: a collector would have to find a reference in a register a callee saved.
- **A register allocator now.** The same reason as on x86-64: AD-26 wants a
  benchmark naming a function the baseline cannot serve first.
- **`sp`-relative slots.** The metadata's frame base must be the frame pointer,
  because a helper that walks the chain reaches its caller's `x29`, not its `sp`.
- **Refusing long branches by shrinking `max_code_bytes`.** A 1 MiB cap would
  reject functions x86-64 compiles, and the second assembly costs nothing for the
  functions that fit.
- **Branch relaxation in place.** Growing a branch moves every later label, so the
  fixups have to be recomputed; assembling twice is simpler and is only paid by
  a function over a mebibyte.

## The Windows x86-64 flavour

Windows x86-64 (`_WIN64 && __x86_64__`, MSYS2 with GCC) has a backend, and it is
the x86-64 emitter with different register roles, not a second emitter: the
operation emitters differ only in which registers carry the context, `out`,
`args`, the six helper arguments and the scratch for the parameter loads, so the
SysV output is the table's other column and did not move (the pin of the 2000
generated functions is unchanged). The flavour is an architecture value of its
own, `GRJIT_ARCH_X86_64_WIN64`, which `grjit_emit_for` can emit on any host. So
what it emits, the unwind bytes and where they live are tested on Linux, with the
bytes of a function pinned (`tests/unit/test_win64.cpp`, `test_pin.cpp`), and
only *running* it needs Windows, which here means wine
(`tools/xwin/m1-run.sh` in the workspace). Nothing below has run on a Windows
machine.

**Calling convention.** Entry is `uint32_t (void * context, const uint64_t * args,
uint64_t * out)` in `rcx`, `rdx`, `r8`; the prologue stores them in the same three
frame slots as SysV does. A helper call passes arguments 1 to 4 in `rcx`, `rdx`,
`r8`, `r9` and 5 and 6 at `[rsp + 32]` and `[rsp + 40]`, above the callee's 32
bytes of shadow space; the poll helper is called with the context in `rcx`, the
function in `rdx` and the offset in `r8`, and the entry hook with the context in
`rcx`. The frame is `N` = the base frame rounded to 16 plus **48 bytes at the
bottom** (32 of shadow space and the two argument words), so `rsp` is 16-aligned
at every call and no slot overlaps a callee's shadow space, which is the thing a
SysV frame gets wrong on Windows (ctang's JIT had exactly that bug). The frame cap
(`max_frame_bytes`) applies to the base frame, as on Linux, and the metadata names
the base frame: the layout below `rbp` is unchanged, `[rbp]` is the saved frame
pointer and `[rbp + 8]` the return address.

**Scratch registers** are `rax`, `rcx`, `rdx`, `r8`, `r9`, `r10` and `r11`. `rsi`
and `rdi`, which SysV uses to load the parameters and to pass arguments, are
callee-saved on Win64, as are `rbx` and `r12`-`r15`; none of them is ever encoded,
which the assembler records (`regs_used`) and a test reads for 2000 generated
functions, and which objdump confirms from the bytes. The parameters are loaded
through `r10`.

**Stack probes.** Windows commits a thread's stack a page at a time, by touching
the guard page just below what is committed, and an access further down than one
page is a stack overflow. A frame of more than a page (`N + 8` over 4096; the
default cap allows a megabyte) is therefore probed before `rsp` moves: a loop of
`lea`, `cmp`, `jbe`, `test [r11], al`, `jmp` that touches each page from the return
address down and ends exactly at the new `rsp`, between `mov rbp, rsp` and `sub rsp,
N`. It is a loop and not an unrolled run because `SizeOfProlog` is a byte and the
prologue, probes included, must fit in it. A test compiles and runs the largest
frame the cap allows (131069 registers, a base frame of exactly 1 MiB).

**The epilogue** is `lea rsp, [rbp]; pop rbp; ret` and not `leave; ret`: they do
the same work, and the first is one of the forms Windows' unwinder recognises when
it stops inside an epilogue.

**Unwind information** (AD-14: `codegen` owns it) is a `RUNTIME_FUNCTION` for
`[0, code size)` and the `UNWIND_INFO` it names: version 1, no flags, frame
register `rbp` at offset 0, and the codes, latest first, `ALLOC_SMALL` (8 to 128
bytes), `ALLOC_LARGE` with a 16-bit count of 8-byte units (up to 512 KiB minus 8)
or a 32-bit size, `SET_FPREG` and `PUSH_NONVOL rbp`, each at the offset of the end
of its instruction as the emitter recorded it. The builder is pure and its bytes
are tested for every size from 0 to 2 MiB against a decoder written from the
format. Both live **in the code's own mapping**, 4-byte aligned after the code,
because `RtlAddFunctionTable(table, 1, base)` addresses everything by a 32-bit RVA
from one base and an allocation elsewhere could be out of reach; they are written
while the pages are read-write and covered by the same flip to read-execute. The
table is registered after the flip and `grjit_code_destroy` calls
`RtlDeleteFunctionTable` **before** it unmaps, so the operating system never holds
a table that points at unmapped pages. A registration the system refuses fails the
compile with `GRJIT_ERR_IO`, the pages unmapped and the meter where it started.
What registers and removes a table is a two-function seam
(`grjit_unwind_set_ops`, for tests), so the order, the failure and the leak are
tested on a host that has no unwinder; on any target that is not Windows x86-64
nothing is registered.

**Rejected:**

- **`RtlInstallFunctionTableCallback`.** A callback is more machinery than one
  static table for one function.
- **A table per function, in the allocator.** One code object is one function, and
  separate memory might be out of 32-bit reach of the code.
- **A frame without `rbp`.** The reason is the one above: consumers walk `rbp`, and
  `lang-tang`'s poll helper follows the frame-pointer chain.
- **Unrolled stack probes.** They do not fit `SizeOfProlog` for a megabyte frame.
- **`__attribute__((ms_abi))` to run the flavour on Linux.** It would let the
  executing tests run on any host, and it would also hide a mismatch between what
  the emitter assumes and what a Windows compiler does. The flavour is run by a
  Windows compiler's code or not at all.

**What wine has and has not shown.** Under wine, the library's whole suite runs
against the Win64 backend: the differential (2000 generated functions, calls with
up to six arguments, polls and guards, against the evaluator), the read-back, a
six-argument call whose helper reads `rcx`, `rdx`, `r8`, `r9`, `[rsp + 40]` and
`[rsp + 48]` and overwrites its own shadow space while the caller's live slots are
checked, sentinels in `rbx`, `rsi`, `rdi` and `r12`-`r15` across a return, a deopt,
two refusals and a poll, a helper that walks up from compiled code with
`RtlLookupFunctionEntry` and `RtlVirtualUnwind` and finds the generated frame with
the right begin and end and the test's own function above it, and a destroy after
which the lookup of the old address finds nothing. Planted defects (a callee-saved
register used, no outgoing area, an unwind table never registered) are each caught
on Linux by the structural tests and under wine by the executing ones. Wine's
`ntdll` is wine's own: a stack walk that works there has not been shown to work
under a real Windows kernel's exception dispatch, `VirtualProtect` returning
success there is not evidence about data-execution prevention, and the guard-page
growth that the probes exist for is wine's implementation of it.

## The assembler

`src/x86_64/asm.c` is new code (AD-9, spine Supersedes: not seeded from ctang's
`binary.h`), and `src/arm64/asm.c` is another, written for this library and not
seeded from either (the arm64 one is described above). It encodes what the backend emits and no more, checked against byte
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
everywhere); on Windows x86-64 the same tests read `VirtualQuery` over the whole
address space and catch the access violation of a write with a vectored handler. Destroying code unmaps it: destroying code a thread still runs is
the caller's error. x86-64 needs no instruction-cache maintenance after the flip; arm64 does it
between the last write and the flip (above). On Windows x86-64 the unwind
information shares the mapping and is registered after the flip (above).
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
- **Windows arm64 and macOS.** No backend: `grjit_backend_available()` is false,
  `grjit_compile` returns `GRJIT_ERR_UNSUPPORTED`, and every test that needs
  compiled code is reported SKIPPED (`GRJIT_REQUIRE_BACKEND`), the encoders and
  the IR still being tested, the three examples and the benchmark exit 77, which
  the Makefile counts as skipped. Windows x86-64 has a backend (above); the
  page-protection path under it is `runtime-core`'s `VirtualProtect` branch.
- **A real Windows machine.** Everything about the Windows backend has run under
  wine and in the structural tests, and nowhere else: the stack walk, the
  registration, the probes, and `check-planted` (which is a Linux target; the
  workspace's `tools/xwin/m1-controls.sh` runs the Win64 planted defects against
  the built executables instead).
- **Pointer authentication and BTI** (above): unsupported and untested.
- **Real arm64 hardware.** The arm64 backend's code runs under `qemu-aarch64`
  (every test of this library and `lang-tang`'s JIT arm) and in a simulator, and
  nowhere else. Instruction-cache coherence, memory ordering and a real kernel's
  W^X are not exercised.
- An ahead-of-time C backend, a Wasm backend, JIT hardening, a
  register allocator, any pass, SSA, inlining, unboxing, floating point, SIMD,
  32-bit and 8-bit values.
- **An in-JIT pause.** A poll's non-zero answer ends the function; see above.
- **The AD-21 prologue check as more than a hook.**
- **A fuzzer**, for the reason above.
