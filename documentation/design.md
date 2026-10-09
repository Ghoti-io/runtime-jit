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
AD-20 forbid, and would put the Windows branch in two libraries. A provider states its own
`size` (runtime-core's `b/page.h`, which has the rule), and `protect` is the
member after the first generation: `grjit_compile` refuses with `ERR_INVALID` a
provider that is not valid, treats one whose size ends before `protect` as
having none (`ERR_UNSUPPORTED`, with nothing mapped), and never reads past the
size. `test_page_size.cpp` has an older-layout provider as a heap block exactly
as large as its size, an armed `protect` beyond the stated size, and a full-size
control for each case. `GRJIT_CompileOptions` and the limits are not given a
size: this library's headers are `free` (AD-14), so a consumer is built against
the same release.

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
(default 16 MiB) is `GRJIT_ERR_LIMIT` with nothing changed. So is a function whose
stack maps would hold more than `max_site_entries` registers (default 4 Mi) summed
over all its sites: neither of the other caps bounds that, since sites times live
registers can be many times the code (at the default operation and register limits
it is 262,144 times 65,536), and it is what the working memory of the liveness pass
and the metadata are made of. The liveness pass reads a site's live set by its set
bits through a bit-to-register table, so its cost is sites times the words of a set
plus the entries recorded; reading every register at every site was quadratic, and
the `compile-12k-sites` bench case (12,000 sites, as many tracked registers) went
from 373 ms to 25 ms. The verifier also refuses an operation that assigns a
register while a derived pointer based on it is live after it (the pointer would be
rebuilt from the new value by a collector that reads derived triples, AD-12); that a
derived register is written only as base plus delta is not checked, because it
changes what the IR means and nothing consumes triples yet.

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
uses: context at `-8`, `out` at `-16`, `args` at `-24`, register `v` at
`-8 * (v + 4)`. `[x29]` is the caller's frame pointer and `[x29 + 8]` the return address,
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
| A frame over 4095 bytes, up to the 1 MiB cap | `sub sp, sp, #hi, lsl #12` then `sub sp, sp, #lo`, each a multiple of 16 so `sp` is aligned between them. From 16 MiB (a raised cap, up to the 1 GiB ceiling) the immediates cannot hold it: the amount goes in `x17` and one extended-register instruction (`sub sp, sp, x17`, the form that can name `sp`) moves it, so no frame is refused that x86-64 compiles; the slot offsets beyond it are built in `x17` as every large offset is. At the cap runs; one register over is `LIMIT` as on x86-64. **This was a difference between the targets** (arm64 refused a frame over 2^24 bytes, 2,097,200 registers, with `LIMIT` where x86-64 compiled it; a native whose call needed 2^24 bytes or more of stack, declared use plus stack arguments, was refused the same way, though `limits.c` and `natives.h` allow 1 GiB), and was removed rather than documented: a limit that is a property of an instruction encoding and not of the function is a defect, and the cost of removing it is one instruction in a case no function reaches by accident. Tests run the sizes 2^24 - 16 to 2^30 on every target and read the emitted words for both. |
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
`protect` runs), and an x86-64 mapping is not. **The maintenance is once per mapping, before
the flip to read-execute, and that suffices only because the engine that compiles also runs the code,
on one thread**: the thread that wrote the lines is the thread whose instruction fetch sees them after
the cache maintenance and the context synchronization of its own `mprotect`. Publishing code to another
thread (a compiler thread that hands a mapping to a running one) would need more: the executing
thread must discard any instructions it already fetched (`isb`, or `membarrier(MEMBARRIER_CMD_PRIVATE_EXPEDITED_SYNC_CORE)`
from the publisher, on Linux), and nothing here does it. That is not a limit of the library today (the
context, and the code it runs, belong to one thread at a time) and is the first thing a background
compiler would have to add. Real arm64 hardware has not been
exercised by this library, and that line is the one thing that would fail
there and not here.

**How it is tested without a machine to run it on.** The assembler is C that
emits bytes, and it is compiled and tested on every host: each instruction is
compared with the bytes `aarch64-linux-gnu-as` produces for the same text
(recorded, with the disassembly `aarch64-linux-gnu-objdump` gives back, which
`suite/tools/xarch/jit-arm64.sh` re-checks). `tests/a64_sim.h` is a small simulator of
exactly the subset the assembler emits: it decodes only those encodings (any
other word stops it), executes them against real memory, and checks what AAPCS64
asks of compiled code: the callee-saved registers, `x29`, `x30` and `sp` are what
they were on return, `x18` is never touched, `sp` is aligned at every call, and
after a call the caller-saved registers are noise. The differential runs the
arm64 emitter's output in it against the evaluator on the same 2000 functions as
the native one. And `suite/tools/xarch/jit-arm64.sh` runs the real thing: it builds the
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
(`suite/tools/xwin/m1-run.sh` in the workspace). Nothing below has run on a Windows
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

## Calls between compiled functions (AD-28, CAP-1, CAP-4)

Milestone 1's baseline JIT called only C helpers at a fixed address, so a compiled
function could not call another and lang-tang left compiled code at every `CALL`.
**Calls as deoptimization exits were milestone 1's defect**: `fib(15)` ran 12%
slower with the JIT on than off, and `fib(22)` 1.2% slower (lang-tang's
`design.md`, "Calls, measured", which also has the current figures), because every call left compiled code, deoptimized the
caller into the interpreter, and re-entered the callee's compiled code from the
top. The restriction was story 15's own rule ("no JIT frame calls a JIT frame"),
not the spine's. This section is the protocol that removes it, as built and
described for x86-64 SysV; arm64 has its own convention of the same shape (the
section "The arm64 convention for calls, tail calls and natives", below). Win64 has
its own (the section "The Win64 convention for calls, tail calls and natives", below), and
`grjit_backend_calls_available()`, true wherever a backend exists (Linux x86-64, Linux arm64,
Windows x86-64), lets an engine ask once instead of finding out at its first compile; only a
build with no backend refuses the new operations, with `GRJIT_ERR_UNSUPPORTED`.
`runtime-core`'s `design.md` ("A, part 5") holds the other half: the walk, the
rebuild, the native-stack limit and the entry slot.

**The IR.** A function is *callable* (`grjit_builder_set_callable`) and carries
four engine hooks. `CALL_SLOT` calls through an entry slot and `CALL_PTR`
through a code pointer; each takes up to sixteen arguments (`I64`, `REF` or
`PTR`), a result of any type or none, the engine's token for the callee, and
**two frame states**: the guest frame as it stands while the callee runs (the
interpreter resumes the caller there once the callee returns), which the stack
maps and the chain rebuild use, and the guest frame as it stands before the call,
which an exit at the call site uses. They differ for an engine whose callee
frame is pushed by the call itself. Liveness makes three site sets per call (the
push, the call and the exit), because what is live differs: the arguments are
live at the push and not at the return address, and an exit leaves the frame.

**The hooks** are C functions the engine supplies and compiled code calls with
the context. `push` pushes the callee's guest frame, counts guest depth and
memory as the interpreter's own push does, and extends the reservation by the
callee's maximum; it is the frame-push GC point. `pop` pops it after a normal
return. `compile` compiles an empty slot's function and installs it, or marks
the slot refused. `deopt` rebuilds the whole chain (`grcore_compiled_rebuild`) and
returns the rebuild's result: zero, or the code it refused with.
The library knows no engine: the fixture in `tests/calls_fixture.h` implements
them over `runtime-core`, and story 8 replaces it with Tang's.

*Derived arguments.* A `PTR` argument that is derived from a `REF` (a pointer into
an object) is copied into the arguments area like the others, so the area holds a
second copy of it, and a collection the push hook triggers moves its base. The push
site's map therefore carries, for each such argument, a derived entry at its area
slot against the base's slot, as it does for a live derived register; without it
the callee would be handed an address into the object's old place.

*Where this differs from the story's wording.* `pop` cannot refuse. After a
callee returned normally the call is complete, no frame state describes
"complete but not yet popped", and a refusal would leave the interpreter to
re-run the callee; so the hook returns nothing and must not reach a GC point.
`deopt` is different, and is not void: the rebuild can refuse (a reservation too short, a
frame that cannot be read), and compiled frames are already gone from the point of view
of anyone who would carry on, so the refusal cannot be a quiet return. The hook returns
its result; non-zero makes every frame return `FAILED`, never `DEOPTED`, and the adapter
exit with `GRJIT_EXIT_REBUILD_FAILED`, a fatal exit of its own (`out[0]` is the code), which
an engine cannot mistake for a deoptimization it should finish in the interpreter. A
hook that cannot fail returns zero.
A refusal of `push`, which leaves nothing pushed, is an exit through the exit
state, where the interpreter makes the call itself and reaches the verdict it
would have (a depth limit, a memory budget).

**The internal convention** (x86-64 SysV; `backend/backend_internal.h`):

| | |
| --- | --- |
| integer arguments | `rdi, rsi, rdx, rcx, r8, r9`, then the stack above the return address, in order, in whole 16-byte units |
| context | `r10` (caller-saved, and no argument register) |
| result | `rax` |
| status | `rdx`: `RETURNED` (0), `DEOPTED` (1) or `FAILED` (2); after `DEOPTED`, `rax` is the cause, returned unchanged by every frame; after `FAILED`, it is the code the rebuild refused with |
| callee-saved | none is used and none needs saving: `rbx`, `r12`-`r15` come back as they went in, and `rbp` is the frame link |
| frame | `rbp`-linked as `a/compiled.h` requires; the metadata's frame size includes the arguments area |
| stack arguments | **popped by the callee** (`ret imm16`) |

*Who pops the stack arguments* was decided with story 5's tail call in mind. A
tail call to a callee with more stack arguments than its caller has must reuse
the caller's frame in constant stack. With the callee popping its own arguments,
the tail-calling function knows its caller expects exactly its own incoming area
popped, moves the return address and builds the callee's arguments below it, and
the final `ret` pops the callee's area, leaving the stack where the original
caller expects it; with the caller popping, the original caller would have to
know what the tail callee takes, which it cannot (indirect calls). The cost is
that the caller makes the area with a `sub rsp` before each call that has stack
arguments, which a caller-pops frame would preallocate once; calls with six or
fewer arguments, which is nearly all of them, have none. A callee's parameter
count and its callers' argument count must agree (an engine's types do that).

**The entry adapter** is `GRJIT_EntryFn` for a callable function, unchanged for
a caller, and it is first in the code, so `grjit_code_entry` is still the start of
the mapping. It saves `rbp`, keeps `out` and the context in its own frame, runs
the entry hook (so a refusal is still `GRJIT_EXIT_REFUSED` and still the hook's),
loads the arguments from `args`, sets `rbp` to the chain-end marker, calls the
internal entry, clears the walk-start cell, and turns the status into an exit.
For callable code `GRJIT_EXIT_DEOPT` means every compiled frame, this function's
included, has been rebuilt into its guest frame, `out[0]` is the cause, and `out`
holds no frame state. The internal entry follows, on a 16-byte boundary with a
sixteen-byte tag before it: a word that holds a magic number and the function's
parameter count, then the engine's token for the function (`grjit_builder_set_token`).
A `FAILED` status (below) reaches the adapter as the fatal exit
`GRJIT_EXIT_REBUILD_FAILED`.

**A call, in order.** (1) *Dispatch*: for a slot, load its word and compare it
once: above one, call it; zero, ask the `compile` hook, which installs or marks
the slot refused; one (`GRCORE_ENTRY_REFUSED`), exit without asking. For a code
pointer, call `grjit_call_target_ok`, which requires the address to be inside
registered, non-retired code and to have the tag the adapter leaves before every
internal entry **with this call's parameter count and the callee's token**, so an
unregistered address, the adapter, the middle of code, null, retired code, and a
function compiled for another arity or another callee are each refused and never
entered (an exit through the exit state, where the interpreter makes the call and
reaches the verdict a call of that pointer would have). A slot is held to the same
rule when code is put in it: `grjit_entry_slot_install` installs only code whose
binding (token and parameter count) is the one the slot was made for, so a call
through a slot needs no check at run time. (2) *Arguments* are copied into the
frame's arguments area. (3) *Push*: the hook is handed the address of the area,
whose `REF` arguments are in the push site's stack map, so a collection the push
triggers has updated them by the time the hook reads them; this is why the area
exists, and a hook must read its arguments after any collection it causes.
(4) *Call*, with the stack arguments made just before it. (5) *Test the status*:
`DEOPTED` returns `DEOPTED` through this frame, untouched, because the chain was
rebuilt before any frame returned. (6) *Store the result, then pop.* Dispatch
comes before the push so that no push is ever undone: a callee that cannot be
entered costs a compare and an exit, never a pushed-and-popped guest frame.
Every way out before step (4) is an exit through the exit state.

**Stack maps and the walk start.** Every call that can reach a GC point in a
callable function stores its frame base and the return address of the call in
the context's cell first (`lea rax, [rip + after_call]`, two stores), so a walk
from the helper, the push hook or a poll finds this frame and the compiled
frames below it by the rule of `a/compiled.h`. A guard's exit, a call's exit and
a poll's deoptimization are sites at the return address of the hook call that
starts the rebuild, so the one rebuild reads each frame at the site it is
stopped at. Non-callable functions store nothing and are byte-for-byte what
they were.

**Chain deoptimization.** A guard that fails, a poll whose helper answers
non-zero (a pause, a step, a breakpoint, an unwind: the helper's answer is the
cause), an exit at a call site, and a native stack that would run out all call
the `deopt` hook once, which rebuilds every compiled frame down to the innermost
activation record into its guest frame; then each frame returns `DEOPTED` to its
caller, nothing is inserted and no return address is patched, and the adapter
turns it into `GRJIT_EXIT_DEOPT`. An unwind passes the engine's survivors to the
rebuild, which converts only those.

**The native stack, in bytes.** Each callable function's prologue computes the
lowest address its frame will use and compares it with the context's limit word
(`a/layout.h`; `runtime-core` sets it at every run, resume and re-entry from the
byte budget). Below it, a stub, entered with only `push rbp; mov rbp, rsp` done,
stores the caller's base and return address as the walk start (or clears it when
the caller is the adapter, whose frame link is the marker) and calls `deopt`.
The callee's own guest frame, which the caller's push hook made, is at its entry
and complete, so the interpreter runs the callee from it with its arguments.

**Rejected.** *Calls as deopt exits*: milestone 1's defect, above. *A C-ABI call
between compiled functions*: it passes the status through an `out` pointer and
needs callee-saved registers or a spill per call, and the adapter then exists
for nothing; the C ABI is used only at the entry and at natives and helpers.
*A shadow stack of frame records*: a store per call on the path this feature
exists to make fast, and a second source of truth. *Callee-saved registers for
references*: AD-17 forbids it (a moving collector must update them). *A patched
return address* to a stub that pops a side record: breaks the return predictor
on every call and leaves a native unwinder or profiler looking at addresses in
no registered code. *Caller-pops stack arguments*: above.

**Measured.** (CAP-6, AD-26; re-measured 2026-10-09, runtime-jit `b2a40da`.) The
instrument is `bench/bench.c`, the loop of "Benchmarks" calling a callable function
through an entry slot with `push` and `pop` doing nothing, so the figure is the
convention, the status test and the hooks' two C calls and not an engine. The machine is
the EVO-X2 (AMD Ryzen AI MAX+ 395, Linux 7.2.9, otherwise idle) through `tools/evo`, in
the build image: GCC 16.2.0, `-std=c17 -O2 -g`, release, x86-64 SysV. The command is
`make -C libs/runtime-jit build/linux/release/apps/bench/bench PREFIX=<prefix>` and then
`build/linux/release/apps/bench/bench`, run three times in turn; each figure is the best
of five repeats inside a run, and the range below is the lowest and highest of the three.
Calibration 1.17 ns a step in all three.

| Case (ns per iteration) | Range of three runs |
| --- | --- |
| `loop-plain`, no poll | 0.479 to 0.500 |
| `loop-poll`, an inline poll, nothing pending | 0.594 to 0.636 |
| `loop-call`, a call of a no-op `NO_GC` C helper | 1.012 to 1.073 |
| `loop-compiled-call`, a call and return through an entry slot, hooks included | **2.641 to 2.823** |
| `loop-tail-call`, a tail call through a slot | 1.670 to 1.790 |
| `loop-helper-gc`, `loop-native`, `loop-native-status` | 1.020 to 1.091, 1.021 to 1.107, 1.027 to 1.114 |

So a compiled call and return through a slot, hooks included, costs about 2.2 ns more
than the plain loop (1.9 times the calibration step), a tail call about 1.2 ns, and a C
helper about 0.5 ns. Against the first recording of this figure (2026-10-06, an Intel
Core 7 150U, GCC 14.2, loaded by other jobs: 4.65 to 4.69 for the compiled call, 0.74 to
0.77 plain, 1.74 to 1.76 for the helper, calibration 1.35) the absolute figures are lower
on this machine: the call's cost over the plain loop was 3.9 ns then, 2.9 times that day's
calibration step, and is 2.2 ns now, 1.9 times this one's. The two
machines are not one measurement, and no ratio between them is claimed. An engine's
`push` and `pop` are what dominate a call in practice. **There is no interpreted-against-compiled
`fib` here, and none is possible in this library:** it has no interpreter, so there is no
interpreted run to compare with. That figure belongs to the first engine that has both,
and it is recorded once in lang-tang's `design.md` ("Calls, measured"): `fib(22)` 36% faster
compiled, with no call exit, and the `fib(15)` and library-call figures beside it. (Compile
of a 100-operation function: 5.5 us here, against 7.9 us on the first machine.)

A retired function costs one page
(4,163 bytes with bookkeeping) while a compiled run stays open: 200 replacements
under one open JIT record held 832,640 bytes, 400 retired references, all
released when it left (`Calls.RepeatedReplacementUnderOneLongLivedActivation...`
prints it; runtime-core's `design.md` says why no bound or epoch is added, and that
Corey accepted that and documented it as an open risk on 2026-10-09).

## Tail calls (AD-28, CAP-8)

A compiled function can call another (above); this is the call that replaces the
caller. Wasm 3.0's `return_call`, `return_call_indirect` and `return_call_ref`
need it, and so does any tail-recursive guest program: without it a loop written
as a tail call grows the native stack and the guest stack at every iteration. It
is described here for x86-64 SysV and exists on arm64 and Win64 too (the sections below);
nothing about a function without one changes (the three pins, and a fourth, over callable
functions with calls and no tail call, were recorded at the commit before and hold).

**The IR.** `TAIL_CALL_SLOT` and `TAIL_CALL_PTR`: terminators with no result and
*one* frame state, up to sixteen arguments (registers or immediates), the
callee's token, and an entry slot or a code pointer (a `ptr` register or a
non-null immediate: `return_call_indirect`, and `return_call_ref` once the engine
has read the pointer out of its function object). Dispatch, the compile-at-call
helper, the refused-slot compare and the pointer check (registered, not retired,
tagged with the callee's token and *this call's* argument count) are a call's,
unchanged, and share their code (`emit_dispatch`). A callee that cannot be
entered is an exit. The verifier requires a callable function with the `tail` and
`deopt` hooks (and `compile` for a slot), a last-in-block position (the operation
is a terminator, so "not last" is the existing refusal, naming the operation), at
most `max_guest_call_arguments`, a `ptr` register or non-null immediate, and a
frame state of the function's length. **The IR declares no signature**, so the
library does not check argument types against the callee or the result type
against the caller's: what a tail call is held to at run time is the callee's
token and parameter count, which the tag and the slot's binding check, and that
types agree is the engine's compiler's, exactly as for a call. (A library that
checked would carry an engine's types, or a table by token that duplicates the
engine's own.)

**The `tail` hook** (`GRJIT_CallHooks::tail`, `push`'s signature; the struct has no size
field and `tail` was added at its end, so a client built before story 5 is rebuilt: no
engine has shipped against it, and a size field is for the first that does) replaces the top
guest frame, the caller's, with the callee's, and the reservation extension, as
one step: extend by the callee's maximum, then give back the caller's, so a
refusal leaves everything as it was. Everything that can fail or collect (the
extension, the stack's room, `grcore_stack_reserve` for a callee whose frame is
larger) is done before the guest stack changes; the pop and push that follow
cannot fail, and the hook reaches no GC point after it commits. It reads its
arguments after any collection it causes. A non-zero return changes nothing and
is an exit at the tail site, where the interpreter makes the tail call itself. A
tail call from a frame that owns a budget scope or an engine call record is
refused by the hook (the engine knows; the library has no notion of scopes), so it
goes through that exit and the interpreter makes it as a call whose result the
frame returns. The guest depth does not change, and memory is counted as the
interpreter's own tail call counts it. `pop` is not called for a tail call: the
pop of the original call, when the callee returns, pops the guest frame the hook
left on top.

**The sites.** The hook is a `GRCORE_SITE_GC_POINT_FRAME_PUSH` site of the caller,
whose guest frame is still the caller's: its map names the live references, the
frame state's registers *and the arguments area* (so a collection in the hook
updates the arguments, derived pointers rewritten from their bases, before it
reads them); the exit before the replacement is a `GRCORE_SITE_GUARD` site in the
same state and goes through the same `deopt` hook and `DEOPTED` return as a call's.
No new site kind and no change to the metadata format (the version stays 1).
Liveness makes two site sets per tail call (the hook, the exit), and the hook's
includes everything the exit's state names: a hook that collected and then refused
leaves the references the exit's rebuild reads updated
(`Tail.ARefusedTailHookThatCollectedLeavesTheExitStatesReferencesUpdated`).

**The replacement.** After the hook succeeds, with no GC point until the jump. Let
`ra` be the address of the caller's return address (`rbp + 8`), `in_A` and `in_T`
the bytes of stack arguments the caller's and the callee's convention take (whole
16-byte units), `R = ra + 8 + in_A`, where the original caller expects `rsp` after
the return, and `ra' = R - 8 - in_T`. In order: load the context into `r10`, the
return address into `r11` and the caller's saved base into `rax`; copy the callee's
stack arguments from the arguments area to `[ra' + 8, R)` through `rdi`; store
`r11` at `ra'`; load the register arguments from the area; load the entry from the
entry-save slot into `r11`; `lea rsp, [ra']`; `mov rbp, rax`; `jmp r11`. The
callee's prologue then finds exactly what a call leaves, and its `ret imm16`
leaves `rsp` at `R`: that is why **the callee pops its own stack arguments** (above)
and what makes the original caller's stack right whatever the widths in between.
Alignment holds because `in_A` and `in_T` are multiples of 16. The signal safety
argument is that every write is above `rsp` until `rsp` moves, and nothing below
the new `rsp` is read after.

One emitted tail call (`A(v0, v1)` tail-calling a callee of nine arguments, three of
them on the stack: `in_A = 0`, `in_T = 32`, so `ra' = ra - 32` = `rbp - 0x18`; the area is `rbp - 0x70` to `rbp - 0x28`, which ends
below `ra'` and below the registers' slots; the printed IR is
`tail_call.slot 0x... callee=2(v0, v1, #2, #3, #4, #5, #6, #7, #8) state fn=1 off=7 [v0, v1]`):

```
  184: mov  r10,[rbp-0x8]      ; the context, before the copies reach its slot
  188: mov  r11,[rbp+0x8]      ; the return address
  18c: mov  rax,[rbp+0x0]      ; the caller's saved base
  190: mov  rdi,[rbp-0x40]     ; stack argument 6 ...
  194: mov  [rbp-0x10],rdi     ; ... to [ra'+8]
  198: mov  rdi,[rbp-0x38]     ; 7
  19c: mov  [rbp-0x8],rdi
  1a0: mov  rdi,[rbp-0x30]     ; 8: lands on the saved-base word, already in rax
  1a4: mov  [rbp+0x0],rdi
  1a8: mov  [rbp-0x18],r11     ; the return address, at ra'
  1ac: mov  rdi,[rbp-0x70] ... r9,[rbp-0x48]   ; the six register arguments
  1c4: mov  r11,[rbp-0x78]     ; the entry the dispatch saved
  1c8: lea  rsp,[rbp-0x18]
  1cc: mov  rbp,rax
  1cf: jmp  r11
```

**The staging area never overlaps the destination.** The arguments area is the
staging area, and the stack arguments' destination reaches below the caller's own
incoming area when `in_T > in_A` (`ra'` is below `ra`). Moving arguments in place
would need a copy direction chosen from the frame's shape, and the register
arguments' sources can lie in the destination, so the registers would have to be
loaded first, and there are not enough scratch registers to hold the context, the
return address, the caller's base and a copy temporary at once. With the area kept
at or below `ra'`, nothing is order-sensitive and every one of those is loaded
before the first write. A function with tail calls therefore gets `pad` slots
between its register slots and the area, `max over its tail calls of
max(0, (in_T - in_A) / 8 - (vreg_count + 4))`; it is zero for every function that
has none, and the macros and the metadata take it as part of the register count.
The frame is small (a function of two parameters tail-calling sixteen arguments
needs six slots of padding; most functions need none), and a test derives it from
the frame sizes and shows that one slot less would overlap
(`Tail.TheFramesPaddingIsExactlyWhatTheStagingAreaNeeds...`). It is representation
independent: a float argument is a spilled word loaded into an xmm register last.

**The walk is unchanged, and so is runtime-core.** After the jump the callee's
frame has the caller's saved base and return address in the two words at its base,
so the walk meets it as any frame: it ends at the same chain-end marker or caller,
and sees one compiled frame paired with one guest frame (the callee's), because the
hook replaced the guest frame as the jump replaced the native one. Compiled frames
and guest frames still correspond one for one, which is what the positional pairing
of `grcore_compiled_guest_index` asks. The walk-start cell holds a dead frame after
the jump and is not cleared: it is read only after a store that precedes every call
that can reach a GC point, and the native-stack stub stores it itself. Read from a
probe in a callee that a widening tail call entered, the word at its base is the
original caller's base, constant over a million tail calls
(`Tail.MutualRecursion...`: `saved` and `rets` at the first probe and the last).
The callee's guard, poll or pause deoptimizes as any frame's: the chain, which no
longer holds the replaced caller, is rebuilt into its guest frames and every frame
returns `DEOPTED`; a million-call chain found by a guard has exactly its callers' and
the callee's frames (`Tail.AGuardAfterAHundredThousandTailCalls...`).

**Native stack in bytes.** No check at the tail site, which makes no stack: the
replacement writes only inside the caller's own frame (`pad` guarantees `ra'` is above
its bottom). The callee's prologue check is the only one and is judged as for a call.
The callee's lowest address is the caller's moved by `(in_A - in_T) - (F_callee -
F_caller)` bytes (`F` the frames' sizes), so it is no deeper than the caller's when its frame
is no larger *and* it takes no more stack arguments; a callee that takes more is
`in_T - in_A` bytes deeper for the same frame, and one that does not fit deoptimizes
once, at its prologue, as a call's callee would, with the guest frame the hook made at its
entry, and the interpreter continues
(`Tail.ACalleeWhoseFrameDoesNotFit...`; a budget that fits the first frame exactly
runs a million tail calls with no exit, and one frame's width less deoptimizes at the
entry).

**Rejected.** *A trampoline or a self-loop in the adapter*: only self calls, and the
frame state of the loop would differ from the callee's; the story's mutual recursion
through slots and pointers would not run. *The existing `push` and `pop` hooks as two
steps*: a refusal between them leaves no guest frame under compiled code. *A core
"replace frame" API*: reserve, pop and push over today's API cannot fail after the
reserve, and it is one fewer thing for every engine to call. *A new site kind*: every
reader of the metadata would change for a site that is a frame push. *The callee
signature in the IR*: above. *Clearing the walk-start cell at the jump*: two stores per
tail call for a value nothing reads before a fresh store. *Caller-pops stack arguments*:
above, and the reason the replacement can leave `rsp` at `R` at all.

**Measured.** (2026-10-06, x86-64, GCC 14.2; a loaded machine, so the nanoseconds are
only good against the rows next to them.) *Constant*: a million tail calls through a
function's own slot, in a native budget of 64 KiB (a frame kept would run out in a few
hundred), leave the native stack pointer, the frame base, the guest frame count, the
guest depth and the reservation capacity where they were at the first call, and the
caller's saved base and return address in the callee's frame the same, for self
recursion, for `even`/`odd` mutual recursion through slots and through code pointers,
and for a ping-pong between a function of two parameters and one of sixteen, whose
incoming area widens and narrows every time (`test_tail.cpp`; the figures it prints:
`1000000 tail calls ... native sp 0x7f -> 0x7f, frame base 0x140 -> 0x140, guest
frames 1 -> 1, guest depth 1 -> 1, reservation capacity 9 -> 9`, low 12 bits of the
addresses; these runs use an engine whose I64 locals are converting, so the
reservation's capacity is a real count (9 cells) and not zero, which would be constant for
any program; and a test that plants the hook keeping its extension sees the capacity
grow). *Cost*: `loop-tail-call` in `make bench`, a loop of
`n == 0 ? sum : tail_call(n - 1, sum + 1)` through the function's own entry slot with
the `tail` hook doing nothing, against the same loop's other forms:
`loop-tail-call` 3.51 to 3.85 ns per iteration in the three quiet runs of four (6.2 and
7.3 in the two loaded ones), against 0.92 to 1.06 for the plain loop, 5.55 to 5.99 for
`loop-compiled-call` (a call and return through a slot, `push` and `pop` doing nothing),
2.13 to 2.27 for the loop calling a no-op C helper, and 1.63 to 2.26 for the
calibration step. So a tail call through a slot, with the `tail` hook doing nothing,
costs about 2.6 to 2.8 ns over the loop's own work (the compare, the subtraction and the
addition it also does): about 1.6 times the calibration step and 0.6 of a call and
return, which is what it should be, since it does the same dispatch, staging and one
hook call and neither makes a stack frame nor pops one. An engine's `tail` hook (a pop
and a push of guest frames, an extension and a retraction) is what dominates it in
practice, as `push` and `pop` do a call.

## Calls to natives (AD-28, AD-17, CAP-7)

A compiled function could call another compiled function (above) and tail-call one,
but the only way it reached C was `GRJIT_OP_CALL`: a helper at a fixed address, at
most six integer arguments, no result status, no signature, no native-stack
accounting, and no way to say "this one may allocate, pause, fail or re-enter guest
code". lang-tang's library calls therefore still left compiled code. This is the call
that does not. It is described here for x86-64 SysV and exists on arm64 too (the arm64
section, below, with AAPCS64's eight register words and the pair back in `x0:x1`) and on Win64
(four register words, 32 bytes of shadow space, and the pair through a hidden pointer, so the
*emission* adapts there and the descriptor does not); the pins do not move.

**`CALL` and `CALL_NATIVE` are two operations, decided.** `CALL` stays the *trusted
helper*: the engine's `gc_store`, a barrier, the poll's slow path. It is a leaf or a
GC point, returns one word, takes at most six integer arguments, has no signature, no
stack accounting and no exit, and never re-enters guest code. `CALL_NATIVE` is the
engine's native: registered and typed, up to sixteen arguments, an optional status, a
native-stack check, and an exit state. Extending `CALL` would have changed its
verifier rules, its arm64 and Win64 emission and its bytes (the pins), for the sake of
every caller of the helper.

**The table and the verifier.** `GRJIT_NativeTable` (create, add, count, get, free;
`natives.h`) holds `GRJIT_NativeDesc`: the function's address, the parameter types
(`I64`, `REF`, `PTR`, from none to `max_native_arguments`, default and ceiling 16, not
counting the context), the result type (the three, or none), flags (`STATUS`,
`REENTERS`) and `stack_bytes`, the most native stack the native itself uses before it
calls anything that checks (at most `max_native_stack_bytes`, default 64 KiB). It is
append-only, descriptors are copied and individually allocated (so a pointer to one
stays valid and an id never changes), and a refused `add` changes nothing (a
parameter list that is NULL with a count, an unknown flag, a type that is no type: all
`INVALID`; too many parameters or too much stack: `LIMIT`; a failed allocation in any
arm: `OOM`, the table as it was). A native with a floating-point or variadic signature
*cannot be described*: the three types are the three words. The builder is given a
table (`grjit_builder_set_natives`) and keeps the pointer, not a copy: the table
must outlive the function's verification and compilation, and the code copies what it
needs, so it need not outlive the code (the native must). The verifier refuses,
naming the operation: an id the table does not hold, a function that is not callable
(a native call can leave through the `deopt` hook, which only a callable function
has), a wrong argument count, a register argument whose type is not the parameter's (an
immediate is accepted for any type), a destination of the wrong type or for a native
with no result, a state of the wrong length, a status native without its state after
the call and a state after the call for a native without a status. The emitter checks
the arity again against the descriptor and refuses with `INTERNAL`: a descriptor cannot
change under a verified function, so that is the library's own invariant. **There is no
run-time target check, and that is deliberate**: a native's address is a constant of its
descriptor, baked into the code at compile time, never a value the code loads, so there
is nothing a forged or unregistered target could be at run time. (A call through a code
pointer to *compiled* code needs the check because the pointer is a run-time value;
that is `grjit_call_target_ok`, above.) A native called through a runtime pointer is
excluded by the interface for the same reason.

**The C signature.** `uint64_t fn(void * context, a0, a1, ...)`, or with `STATUS`
`GRJIT_NativeResult fn(void * context, ...)`, where the result is `{ uint64_t value;
uint64_t status; }`. The context is the pointer the code was called with and is the
*implicit first argument*: natives allocate, poll and re-enter through it, so it is not
an operand. On SysV the pair comes back in `rax:rdx` with no hidden pointer; the tests
call real C functions of this exact signature, from C and from compiled code, so the ABI
is measured and not assumed.

**The call sequence**, in order, and after it nothing is reordered (the printed IR and
the emitted bytes of one call with eight arguments and a status, `native #0` of
`stack_bytes` 256 at a made-up address):

```
v2 = call_native #0(v0, v1, v0, #77, v0, v0, v0, v0) state fn=1 off=2 [v0, v1] after state fn=1 off=3 [v2, v1]

lea  rax, [rsp - 0x120]            ; S + stack_bytes = 0x20 + 0x100
mov  rcx, [rbp - 8]                ; the context, from its frame slot
cmp  rax, [rcx + 0x100]            ; the context's native-stack limit word (zero: none)
jb   exit_before                   ; unsigned: below it, an exit; the native is not called
lea  rax, [rip + ret]              ; the walk start, FIRST: before the area is made,
mov  rcx, [rbp - 8]                ;   before any argument is moved, before the call
mov  [rcx + cell], rbp
mov  [rcx + cell + 8], rax
sub  rsp, 0x20                     ; N = 9 words (the context and eight), six in registers,
mov  rax, [rbp - 0x20]             ;   three on the stack: S = round_up_16(24) = 32
mov  [rsp], rax                    ; stack argument 0 .. 2 at [rsp + 8 i], ascending
mov  [rsp + 8], rax
mov  [rsp + 0x10], rax
mov  rdi, [rbp - 8]                ; the context, reloaded
mov  rsi, [rbp - 0x20]             ; arguments 0 .. 4 in rsi, rdx, rcx, r8, r9
mov  rdx, [rbp - 0x28]
mov  rcx, [rbp - 0x20]
mov  r8d, 0x4d                     ; an immediate
mov  r9, [rbp - 0x20]
mov  rax, 0x1122334455667788       ; the call goes through rax, loaded with the address
call rax
ret:                               ; the return address is the site
add  rsp, 0x20                     ; popped by the caller (C ABI)
mov  [rbp - 0x30], rax             ; the result is stored BEFORE the status is looked at
test rdx, rdx
jne  exit_after                    ; a status stub: cause = 1<<32 | (status & 0xffffffff)
```

Every operand is read from a frame slot or is an immediate, so the order of the loads
cannot clobber one, and nothing is assumed preserved: the context, every operand and
every live value are read from the frame again after the call (`r10`, `rax`, `rcx`,
`rdx`, `rsi`, `rdi` and `r8` to `r11` are all taken to be clobbered; the test's native
clobbers every one of them and the vector registers, and the caller's values and the
next call's context come back intact). `rsp` is 16-aligned at the call because it is
aligned in every callable frame and the area is the only thing that moves it, in whole
sixteens; the stub that records the stack pointer at the native's entry shows
`(rsp + 8) % 16 == 0` for every count of stack arguments from zero to eleven, and the
same `rsp` after ten calls in a row.

**The native-stack check** is `lea rax, [rsp - (S + stack_bytes)]` against the
context's limit word, unsigned: the lowest address the native may reach is where the
stack pointer will be at its entry, past its return address, less its own declared use.
Below the limit is an *exit before the call*: a `GRCORE_SITE_GUARD` site in the call's
state, the same stub and `deopt` hook as a call's exit, the native not called, the
interpreter continuing and making the call itself. The check is exact to the byte: the
test measures the native's entry stack pointer with a stub that records it, derives the
budget at which the call just fits, and runs a budget one byte either side for every
argument count from zero to sixteen and for two declared uses, requiring the exit one
byte short and the call at and above; a planted check that leaves out the stack-argument
area, and one that leaves out the declared use, are each caught.

**The site** is a `GRCORE_SITE_GC_POINT_CALL` site of this frame at the return address,
with the call's state's identity (the interpreter's state with the call still to be
made, which is what a debugger stopped in a native shows), a stack map naming every
reference and derived pointer live *after* the call with the result's destination
excluded, and the frame state. The arguments are *not* mapped: a native receives raw
words, and a reference it holds across a GC point is the native's to protect (below).
No new site kind, and the metadata format version stays 1. **Liveness makes two site
sets per native call, three with a status**, in this order: the call, the exit before
it, and the exit after. The call's set is what is live after it, less its result, plus
what *either* state names but the result: the state after the call is read from the
frame by the exit, so a collection the native triggers must have updated every register
it names (`NativeIr.ANativeWithAStatusMakesThreeSites...` gives each set). An exit leaves
the frame, so each exit's set is only what its own state names.

**Status and chain deopt.** `GRJIT_NATIVE_OK` is 0. Any other status means: the call is
complete, `dst` is valid, and compiled code leaves now, through the same chain deopt as
a guard. The status exit is a `GRCORE_SITE_GUARD` site at the return address of the
`deopt` hook call, in the **state after the call** (the interpreter's state with the
call done and the result in place), because the native has run and cannot be run again;
a stub that built it from the state before the call would have the interpreter make the
call, and the test that counts what the natives logged sees it run twice (and the harness
plants exactly that). The hook is called once with `cause = GRJIT_CAUSE_NATIVE | status`
(bit 32: a poll's cause is 32 bits, so the two cannot meet; **a status is a 32-bit value**:
`GRJIT_NativeResult` is `{ uint64_t value; uint32_t status; uint32_t reserved; }`, the call tests
exactly `edx`, and the cause is `NATIVE | status` with nothing lost. The first version tested all
of `rdx` and masked the cause to 32 bits, which was wrong twice: under SysV the upper half of
`rdx` is padding for a 32-bit status, so a native could leave garbage there and exit by mistake,
and a wide status aliased (`0x100000002` read as `NATIVE | 2`). Declaring the status 64-bit
instead would have needed a cause wider than the poll's and a descriptor that refuses what it
cannot carry, for no use: a status is a reason, not a value. The tests have a native written in
assembly that returns garbage above a zero status (no exit) and above a three (exit, cause
`NATIVE | 3`), and statuses at bit 16 and bit 31; testing 64 bits or 16 bits each fails one),
rebuilds the chain through `grcore_compiled_rebuild`, and every
frame returns `DEOPTED`; the entry returns `GRJIT_EXIT_DEOPT` with the cause in `out[0]`.
The library names `GRJIT_NATIVE_DEOPT` (leave compiled code; the interpreter continues
after the call, as for a pause the native left pending or a nested run that left the
state to the interpreter) and `GRJIT_NATIVE_UNWIND` (a guest unwind is in progress: the
hook rebuilds only the survivors, `keep_frames`, and pops the rest without converting
them, AD-27) and passes any other value through for the engine; **it never interprets a
status beyond non-zero**. A refused rebuild is the fatal `GRJIT_EXIT_REBUILD_FAILED`, at
both exits, and the tests show it with nothing written and the native run once (after)
or never (before).

**What the engine's hook does, played by the fixture.** For `UNWIND` it finds the nearest
caller (not the frame that called the native, which is unwound too) whose function is a
scope, rebuilds with `keep_frames` one past it, pops every frame above it and resumes it
after its call with the scope's value; with no scope in the run nothing survives, and the
hook pops down to, but not including, the run's entry frame, because `grcore_activation_leave`
refuses to leave a rebuilt record with fewer guest frames than it began with: the entry
frame goes after the record is left. A converting engine counts what was converted, and the
test requires only the survivor's locals (4) for an unwind and all three frames' (8) for a
`DEOPT` status.

**What a native must do (AD-17, AD-23).** A native that can reach a GC point, which is
every native a call reaches, holds each reference it was passed or creates only where the
collector sees it: a registered root or handle, or its own C frame under a
`GRCORE_ACTIVATION_NATIVE` record that gives that frame as a segment (conservative, so the
object is pinned and not moved). It must not rely on an argument being updated: the
arguments are copies in registers and on the stack, not roots, and the test that holds one
across a collection without a record reads the collector's poison, which is also what shows
that the pin in the test that has the record is the record's doing and not luck. **The
library opens no activation record for a native**, because that would put a core call on every
native call and a leaf-ish native needs none; and **a native called from compiled code pushes
no guest frame**, because it is not a guest call (AD-28's "every guest call pushes" is about
guest functions): an engine that wants a native record in its interpreter does it inside the
shared wrapper, so both tiers do the same. A call extends no reservation and counts no guest
depth, and the test requires the guest frame count, the guest depth and the reservation's
capacity (a converting engine, so it is not zero) to be equal before and after ten native calls.
The segment the fixture gives, and why no core helper makes it, is in `runtime-core`'s
`design.md`.

**Re-entry.** A native that runs guest code opens a `GRCORE_ACTIVATION_REENTRY` record
(nested, so a pause verdict inside it becomes a limit unwind, AD-5) and, for compiled code,
enters through the adapter as the engine does, with its own chain-end marker; the nested run's
frames are walked from its own record, the cell having been moved into the record below at the
native's first `grcore_activation_enter`, so a native that records an activation leaves the
compiled run below it described. A nested run that deoptimizes rebuilds *its own* run, down to
its own record; its frames return `DEOPTED` to its adapter, the native's wrapper finishes them in
the interpreter, and the native returns `OK` or a status. The outer compiled frames are untouched
until the native returns: the two-level test (a compiled chain of three, a native, a nested
compiled chain of three whose native returns `DEOPT`) rebuilds the nested three only and the outer
three finish compiled, and with `UNWIND` the nested run is unwound whole, the native under the
outer chain returns `UNWIND`, and only the outer survivor is rebuilt. Code freed under a native:
nested guest code that clears the entry slot of the outer function whose compiled frame waits
under the native retires the code, which is released when the last JIT record leaves, not before
(the early-free mutations of `runtime-core` are re-run against `testNatives` alone and are caught
by it). The native-depth budget: a refused `grcore_activation_enter` makes the native return
`UNWIND`, for the REENTRY record and, in the fixture, for the JIT record of a compiled nested run (which
never runs the code it was refused: it gives back the frame and the reservation it had pushed and unwinds).
Records enter the budget in AD-21's accounting, so the tiers' depths differ and the first version of this
section understated it: an interpreted nesting costs one unit a level (REENTRY); **a compiled nesting costs
two** (REENTRY and JIT), so with `reenter_c` at every level and a budget of 4 the interpreted outer run
reaches three runs of the function and the compiled one two, about half the depth, and a compiled *outer* run
costs one unit more than an interpreted one for the same interpreted nesting (the same nesting is reached with
a budget one larger). Both are tested, with the verdict (T's scope catches the unwind), every record left and no
fault.

**The pause**, as the fixture plays it: a native leaves a pause pending and returns `DEOPT`, the
chain is rebuilt, the interpreter pauses at its next poll, and the run is resumed on another thread
(`grcore_context_release` and `acquire`; under TSan the hand-over is what is checked) and prints what
the uninterrupted run prints. Inside a nested run the same pending pause is a limit unwind: no pause
reaches the host from inside, in either tier.

**Rejected.** *Extending `CALL`* (above). *The status in a context word or in the poll's request word*:
a second memory location every native must write and the call must read, and a stale value is a silent
miss; a second return word is one `test rdx`. *No status*: a native could not report a fatal error or an
unwind without a pending flag only the interpreter reads. *The library pushing a guest frame or opening
an activation record per native*: a core call on every native, and most natives need neither. *Mapping
the argument copies*: they are not roots, the native holds raw words, and a moving collector would
update a copy that nothing reads. *A runtime-pointer native call*: a Wasm import is a descriptor known
at instantiation; the address is a constant of the code, which is also why there is no run-time target
check. *A walk start stored after the call*: a native's collection would find the previous call's
(planted defect 15 shows exactly that, and the walk test reads it from inside the native).

**Measured.** (2026-10-07, x86-64, GCC 14.2, on a quiet machine, load average 0.7; `make bench`'s minimum of five
repeats, ns per loop iteration, five runs, the figures below the first of them to within 0.05 except where
said; the first measurement, on a loaded machine, was wrong in its conclusions and is replaced.) The same loop,
`sum = inc(sum)`, in a callable function: the loop with no call 0.61; the existing `GRJIT_OP_CALL` to a leaf `NO_GC`
helper 1.42; `CALL` as a `GC_POINT` (`loop-helper-gc`: the walk start stored, no check, no status) 1.78 (1.76 to 1.80);
`CALL_NATIVE` without a status (`loop-native`) 1.62 (1.62 to 1.65); with a status (`loop-native-status`) 1.54 (1.51 to
1.57); a call and return between compiled functions through a slot (`loop-compiled-call`, hooks doing nothing) 3.65;
a tail call 2.2. Over the loop's own work, then: the leaf helper costs 0.8 ns, the helper as a GC point 1.2, a native
1.0, a native with a status 0.9. **A native call is no dearer than the helper called as a GC point** (it is 0.15 ns
cheaper: the helper's result takes the same path and the stack check, three instructions and a predicted branch, does
not show), **about 0.2 ns dearer than the leaf helper** that stores no walk start, and **the status costs nothing
measurable**: the `test` and the branch on `edx` are hidden by the call's own latency, and the status loop measures
0.05 to 0.1 ns *faster* than the plain one, which is noise in the compiler's placement and not a gain. (An earlier
version of this paragraph, measured with other sessions building, said 3.3 ns, 1.0 over the leaf helper and 0.4 for the
status; those figures contradicted the tables and are withdrawn.) Each loop checks its own sum (the plain, poll and call loops the sum of 0 to n - 1 and the call loop's helper count, the others n).
The cost of taking an exit (a status, or the stack check) is a `deopt` hook call and a chain rebuild, which is the
engine's and is measured by its tests, not here.

## The arm64 convention for calls, tail calls and natives (AD-28, story 7 of the calls spec)

arm64 refused a callable function and each of the five operations (`CALL_SLOT`, `CALL_PTR`,
`TAIL_CALL_SLOT`, `TAIL_CALL_PTR`, `CALL_NATIVE`) until story 7 of the calls spec, so CAP-1, CAP-7 and
CAP-8 held on x86-64 only and every call, tail-call and native test on arm64 refused and passed,
proving the refusal and nothing else. `src/arm64/` now has its own internal convention, on the frame
record `x29`/`x30` that `runtime-core`'s `a/compiled.h` already names for arm64, with the same
protocol, hooks, stack maps and metadata as x86-64: what differs is the registers, and where the
stack arguments and the return address are.

| | x86-64 SysV | arm64 (AAPCS64's registers) |
| --- | --- | --- |
| Register arguments | `rdi rsi rdx rcx r8 r9` | `x0`-`x7` |
| Context | `r10` | `x9` |
| Result; status | `rax`; `rdx` | `x0`; `x1` |
| Stack arguments, and who pops | above the return address; the callee, `ret imm16` | at `[sp + 8i]` at the call, in whole 16-byte units; the callee, `mov sp, x29; ldp x29, x30, [sp], #16; add sp, sp, #in_A; ret` (the `add` omitted for none) |
| Callee-saved registers | none used | none used: `x19`-`x28` and `d8`-`d15` are never named, and `x18` (the platform register) is never touched; the adapter sets `x29` to the chain-end marker and restores it from its own frame record, never `mov sp, x29` |
| Scratch | `rax rcx rdx` and `r11` | `x10` copy temporary, `x15` the context while the walk-start cell is stored, `x16` the call target and `adr`'s destination, `x17` a displacement that fits no immediate |
| Walk-start cell | frame base and `lea rax, [rip + after_call]` | `x29` and `adr x16, after_call` (a new assembler instruction: 21 bits, and `adrp`/`add` where it does not reach, under the assembler's reach rule) |
| The return address | pushed by `call` | in `x30`, saved by the callee's frame record |
| Native pair | `rax:rdx` | `x0:x1`, only `w1` looked at (`reserved` is the upper half of `x1`) |
| Native stack words | context and five arguments in registers, the rest at `[rsp + 8i]` | context and seven arguments in registers, the rest at `[sp + 8i]`; the area is `S = round_up_16(8k)`, made by `sub sp`, popped by the caller |

The internal entry is on a 16-byte boundary with the 16-byte tag before it (the magic and the
parameter count, then the engine's token), padded to it by `brk #0` words; `grjit_call_target_ok`
checks the boundary as well as the registry, the magic, the count and the token, because a branch to a
misaligned address faults on arm64 and lands in the code's own data everywhere. The native-stack check
is in every callable prologue and ends in a chain deopt before a frame is made: `sub x16, sp, #alloc;
ldr x15, [x9, #limit]; cmp x16, x15; b.lo overflow`. There is no unwind information, as before (nothing
unwinds natively through a compiled frame). **`sp` is a multiple of 16 at every instruction that uses it
as a base**: the frame is made once and is a multiple of 16, an argument area is whole 16-byte units,
and nothing else moves it. `qemu-user` does not fault on a misaligned `sp`, so each alignment claim is a
recorded `sp` asserted explicitly: at every native's entry (a stub records it), at every hook and
helper the fixture engine supplies (their frame address, which is `sp` less a multiple of sixteen), and
in the code itself (every instruction that writes `sp` is one of five forms, and moves it by sixteens:
`test_arm64_calls.cpp`).

**The sequences** (`i` indexes the argument, `in_A` and `in_T` are the stack-argument bytes of this
function and of the tail callee):

```
call:    ldr x16, =slot ; ldr x16, [x16] ; cmp x16, #1 ; b.ls slow ; str x16, [x29, ENTRY] ; stage the arguments
         adr x16, push_ret ; ldr x15, [x29, CTX] ; str x29, [x15, cell] ; str x16, [x15, cell + 8]
         x0 = ctx, x1 = callee, x2 = &area, x3 = n ; blr hook ; push_ret: (the frame-push site) ; cbnz w0, exit
         sub sp, sp, #S ; the stack arguments through x10 ; x0..x7 from the area ; x9 = ctx ; x16 = ENTRY ; blr x16
         (the call site) ; cbnz x1, ret_propagate ; str x0, [dst] ; the pop hook
native:  sub x16, sp, #(S + stack_bytes) ; compare with the limit ; b.lo exit_before
         adr x16, ret ; store the cell ; sub sp, sp, #S ; stack arguments through x10 ; x0 = ctx ; x1..x7 = args
         mov x16, #addr ; blr x16 ; ret: (the call site) ; add sp, sp, #S ; str x0, [dst] ; mov w1, w1 ; cbnz x1, exit_after
adapter: stp x29, x30 ; sub sp ; save ctx, out, args ; the entry hook ; arguments to x0-x7 and [sp..] ; x9 = ctx
         x29 = MARKER ; adr x16, internal ; blr x16 ; clear the cell ; ldp x29, x30 from the adapter's own frame ; ret
```

**A tail call** replaces the frame with `sp` moved last. With `R = x29 + 16 + in_A` where the original
caller expects `sp` after the return, the callee is entered with `sp = R - in_T`, its stack arguments
at `[sp, R)`, and `x29` and `x30` as they were for this function (the caller's frame record and the
return address, which the callee's prologue pushes again). The arguments area is the staging place
(the shape's `pad` keeps its end below the lowest address the copy writes), everything that is read
from the frame after the first write (the context, the return address, the caller's saved base) is
loaded first, and the register arguments come from the area last. An emitted one, for a function of ten
parameters that tail-calls a callee of twelve arguments (four of them on the stack, where its caller has
two, so the copy overwrites the frame record, which was loaded first), disassembled with the cross
`objdump` (the offsets are in the function's own code):

```
  1d4:	ldur	x9, [x29, #-8]
  1d8:	ldr	x30, [x29, #8]
  1dc:	ldr	x15, [x29]
  1e0:	ldur	x10, [x29, #-136]
  1e4:	str	x10, [x29]
  1e8:	ldur	x10, [x29, #-128]
  1ec:	str	x10, [x29, #8]
  1f0:	ldur	x10, [x29, #-120]
  1f4:	str	x10, [x29, #16]
  1f8:	ldur	x10, [x29, #-112]
  1fc:	str	x10, [x29, #24]
  200:	ldur	x0, [x29, #-200]
  204:	ldur	x1, [x29, #-192]
  208:	ldur	x2, [x29, #-184]
  20c:	ldur	x3, [x29, #-176]
  210:	ldur	x4, [x29, #-168]
  214:	ldur	x5, [x29, #-160]
  218:	ldur	x6, [x29, #-152]
  21c:	ldur	x7, [x29, #-144]
  220:	ldur	x16, [x29, #-208]
  224:	mov	sp, x29
  228:	mov	x29, x15
  22c:	br	x16
```

`16 + in_A - in_T` is negative when the callee is wider and the copy then reaches below the caller's
incoming area, as on x86-64, and the old slots above the area are dead by then. The shared shape
(`grjit_callable_shape`) counts a return-address word arm64 does not have, so its padding is
conservative by up to one slot; an arm64-only formula would have been a second shape for a slot, so
there is one, taking the target's count of register arguments (`grjit_stack_arg_bytes` and
`grjit_callable_shape` take it: 6, 8, and Win64's 4 in story 7b), which is why the pad rule is tested
for both counts from the frame sizes alone.

**The native-stack check is one-sided on arm64, deliberately.** The check on every target is: the
call is an exit before it unless `sp_before_call - S - stack_bytes` is not below the limit. On x86-64
the return address the call pushes is inside `stack_bytes` (the native's whole use from its entry);
on arm64 it is in `x30` and not on the stack, so a native has eight bytes *more* than it declared. A
bound that said "as declared on both" would be one `sub` and one compare on x86-64 and an extra
instruction on arm64 for a figure nobody can state to the byte (a native's own use varies with its
compiler), so the formula stays one and `natives.h` says so. The byte-exact tests derive the budget from
the entry stack pointer a stub records, name the adjustment (`kNativeEntrySpBias`: 8 on x86-64, 0 on
arm64) and show the call is made at the budget and an exit before it one byte below.

**Rejected.** *Caller pops*: a tail call to a callee with more stack arguments could not move the
return address without telling the original caller (story 5's reason). *AAPCS64's own stack convention
as the internal one*: the same, and it would put `x30` handling in the caller. *A thunk adapting natives*:
a second call and a frame between every compiled function and every native. *An arm64-only padding
formula*: one slot at most, a second shape. *Expecting the alignment fault under qemu*: not modelled, so
the claim is asserted. *The `adrp`/`add` form for every `adr`*: the walk start is always a few
instructions from its label, and the long form exists so that the reach rule is the assembler's for
every label, not because an emitter needs it.

**What each instrument gives, and where each runs.** Pins and cross emission run on the host for all
three architectures and say bytes did not move; only `qemu-aarch64` says the bytes are right. The
structural test reads the convention out of the code of all 1,016 callable functions the pin test
generates, and the executing tests (`testCalls`, `testTail`, `testNatives`) run it; both are kept,
because a decoder written by the emitter's author agreeing with it is not an execution, and an execution
that passes does not say which claim it leaned on. The planted defects 20 to 29 and the mutations of
`src/arm64/` (`tools/arm64-plants.txt`, `check-planted-calls.py --target=arm64`) each run under qemu in
the container of `suite/tools/xarch/jit-arm64.sh`, with their controls; `check-planted.sh` names them and does
not count them. The harness accepts a signal as a catch (code freed under a frame faults the process, and a
hang is the tests' watchdog's signal), which says a defect was reached and not which: seven of the arm64
mutations that only a crash caught are now also caught by the words of the code (`testArm64_calls` reads the
dispatch and the tail-call sequence), and two stay caught by their crash alone, for want of a cheap assertion (the
entry a slot call saves for the callee's return, and the arguments staged in the area one slot late: a run
reads either as a wrong argument or a fault in the hook, and the words that would show it are the emitter's own
offsets over again). The mutations of the arm64 emitter's sites, indices, kinds and large offsets are run on the host
too, through what `grjit_emit_for` makes for arm64, since a defect in code only arm64 runs must not wait for qemu to
be seen. `runtime-core`'s, `runtime-heap`'s (with `RELOCATE=yes` and its relocation gates) and
`runtime-debug`'s suites run there too. **Not shown, and not claimed:** real arm64 hardware, the
instruction cache (`qemu-user` translates lazily and does not model it), and the alignment fault.
The "pause resumed on another thread" test runs under qemu without a sanitizer; its race-freedom claim
is TSan's, on x86-64. No timing is taken under qemu: the benchmark runs `--smoke` there, every case once,
each loop case checking its own sum inside the program and `jit-arm64.sh` reading the check word each prints
against the one its iteration count must give. Each test binary is bounded in time there (`timeout`, 30 minutes
by default), and the gates of the script are tried on a pass and a planted failure by `suite/tools/xarch/jit-arm64-selftest.sh`.

## The Win64 convention for calls, tail calls and natives (AD-28, story 7b of the calls spec)

Win64 refused a callable function and each of the five operations until story 7b, so CAP-1, CAP-7
and CAP-8 ("on all three backends") held on two of them. It now has its own internal convention in
the x86-64 emitter, by the same ABI table that story 7 made for SysV: the Win64 column of
`GRJIT_X86Abi` (`src/x86_64/emit.c`) says which registers carry what, and the callable, dispatch,
call, tail-call and stub code read it. **Every earlier pin is unchanged** (x86-64, arm64, Win64
without the new operations, callable, native-call): SysV and Win64 share `emit.c`, so the commits
that touched it are the ones that could have moved a SysV byte, and none did.

| | x86-64 SysV | arm64 | Win64 |
|---|---|---|---|
| Register arguments (internal) | `rdi rsi rdx rcx r8 r9` | `x0`-`x7` | `rcx rdx r8 r9` |
| Context; result, status | `r10`; `rax`, `rdx` | `x9`; `x0`, `x1` | `r10`; `rax`, `rdx` |
| Stack arguments, who pops | above the return address, callee (`ret imm16`) | at `[sp]` at the call, callee (`add sp`) | above the return address, **no shadow space**, callee (`ret imm16`) |
| Native arguments (C ABI) | `rdi rsi rdx rcx r8 r9`, then `[rsp + 8 i]` | `x0`-`x7`, then `[sp + 8 i]` | `rcx rdx r8 r9`, then `[rsp + 32 + 8 i]` above 32 bytes of shadow space the caller reserves |
| Native pair `{value, status}` | `rax:rdx` | `x0:x1` | **hidden pointer in `rcx`**, the context in `rdx`; a 16-byte struct is never returned in registers here |
| Tail scratch | `r10 r11 rax rdi` | `x9 x10 x15 x16 x30` | `r10 r11 rax rcx` |
| Cause in an exit stub | `rsi` | `x1` | `rdx` |

**Registers.** `rbx`, `rbp` (as a register), `rdi`, `rsi`, `r12`-`r15` and `xmm6`-`xmm15` are
callee-saved on Win64 and are never encoded by compiled code, the adapter included: the tail call's
copy of a stack argument goes through `rcx` (the SysV sequence uses `rdi`) and is done before the
register arguments are loaded, and the exit stubs pass the cause in `rdx` (SysV uses `rsi`). Calls
to the engine's hooks, to `grjit_call_target_ok` and to helpers use `rcx`, `rdx`, `r8`, `r9`. A
test reads the registers named by the code of all 1,016 generated callable functions (the assembler's
`regs_used`, and objdump on the host), and the sentinel tests of every call, tail and native test run
with values in `rbx`, `rbp`, `rdi`, `rsi`, `r12`-`r15` and `xmm6`-`xmm15`.

**No shadow space between compiled functions.** Nothing in the internal convention spills there, so
reserving it would cost 32 bytes a frame, and it would not make `ret imm16` an epilogue the unwinder
knows. The C-ABI calls (hooks, helpers, natives) have it: the frame has the 48-byte outgoing area at
its bottom, outside the metadata's frame size as in a plain function (32 bytes of shadow and two
words for a helper's fifth and sixth arguments), so `rsp` is 16-aligned at every call and no slot
overlaps a callee's shadow space. The native-stack check in the prologue counts it.

**Four register arguments, the rest popped by the callee.** An argument past the fourth is above the
return address in whole 16-byte units that the *callee* pops with `ret imm16` (a plain `ret` for none),
so a tail call to a callee with more stack arguments than its caller can move the return address, as on
the other targets. `GRJIT_WIN64_INTERNAL_REG_ARGS` (4) is what `grjit_callable_shape` and
`grjit_stack_arg_bytes` take for Win64; nothing read it for an emitted function until this story, so a
copy of SysV's six survived every gate (story 7's review ran that mutation and it was missed). It is
asserted now from the code: `Win64Calls` tests emit functions of 0 to 16 parameters, calls and tail calls
of 0 to 16 arguments (every pair, for tail calls) and natives of every arity, and read the `ret imm16` of
the epilogue, the `sub rsp` of the argument area, the displacement of the tail call's `lea rsp, [rbp + ra']`
and the stores of the four register parameters, against numbers worked from the convention and not from the
library's constants. `tools/check-planted-calls.py` carries the edit (6, 3 and 8) as three mutations, on the
host and under wine, and each is CAUGHT.

**Natives.** Four C words in `rcx`, `rdx`, `r8`, `r9`, the rest at `[rsp + 32 + 8 i]`. A native with
`GRJIT_NATIVE_STATUS` returns its 16-byte pair `{value, status, reserved}` **through a hidden pointer
in `rcx`**, the context moving to `rdx`, so the area is `round_up_16(32 + 8 k + 16)` with the buffer
after the `k` stack words. The call reads the value and the 32-bit status from the buffer **before
`rsp` is restored**, stores the result, and only then looks at the status; `reserved` is never read, and
the native may leave it, and `rax` and `rdx`, as it likes (tests plant garbage in all three). Story 6's
rules hold unchanged: the native-stack check first (`sp_before_call - S - stack_bytes`, `S` including the
shadow and the buffer, the one formula on every target), the walk start before anything moves, nothing
assumed preserved. Natives are written once for every target; the tests call real C functions of the exact
signature, which is what proves the ABI under a Windows compiler, and two assembly natives: one that spills
its four register arguments to its home space and adds three stack words (which a missing shadow area
overwrites) and one that writes garbage to the buffer's reserved half, `rax` and `rdx`.

**The entry adapter has a static frame.** Its job is to put `GRCORE_COMPILED_CHAIN_END` in `rbp`, so
that the first compiled function's saved caller base is the marker, which ends a walk. An unwinder that
read `UWOP_SET_FPREG` for the adapter would take that marker for a base and compute the frame's `rsp` from
it. So the adapter is `push rbp; sub rsp, N'` with everything addressed by `rsp`, and **its unwind
information has no frame register**. `N' = max(32, in_A) + 32`, rounded to 16: the lowest `max(32, in_A)`
bytes are the 32-byte shadow space of the entry hook and the stack-argument area of the internal call at once
(so one allocation is right at both), and above them are `args`, the context and `out`. The callee's `ret
imm16` returns `rsp` above the area, so the adapter reloads from `rsp`-relative slots and frees `N' - in_A`.
A body frame is correct for the unwinder whatever the callee pops, because it recomputes `rsp` from its own
`rbp`; the adapter cannot, which is why it is static. Walking up from the first compiled frame the
unwinder's `rsp` is `rbp + 16`, the address of the stack arguments, which is exactly the bottom of the
adapter's frame.

**Unwind information.** A callable code has two `RUNTIME_FUNCTION`s, the adapter's `[0, adapter_end)` and
the body's `[internal entry, end)` (the 16-byte tag and its padding before the entry are in neither), with
two `UNWIND_INFO`s in the mapping after the code, sorted by begin address and registered together by one
`RtlAddFunctionTable` of a count of two; destroy removes the table, both entries. The body's is the plain
function's (push `rbp`, set frame register `rbp`, allocate `N`); a function with no calls and not callable
still has the one entry for all its code. The prologue of a body, as of a plain function, has the byte check
(`lea rax, [rsp - N]; cmp rax, [r10 + limit]; jb overflow`), then the page probe for a frame of a page or more
(through `rax` and `r11`, since `r10` is the context and the four argument registers are live), then the
allocation. The overflow stub makes 32 bytes of room for the hook's shadow space below `rsp` (the frame
register keeps the frame right for an unwinder wherever `rsp` is).

**The `ret imm16` probe.** Before the callable emitter was built, a throwaway program (not committed) asked
the unwinder under wine to unwind from every instruction of `lea rsp, [rbp]; pop rbp; ret 16`. Wine's
`ntdll` reads `ret imm16` as an epilogue end, and, having emulated the epilogue, **applies the immediate**:
from `lea`, `pop` and `ret 16` it reports `rsp` as the address of the stack arguments *plus* their size,
sixteen bytes more than the unwind codes give from any other instruction. The two agree when a function has no
stack arguments (which is why Win64 ends such a function with a plain `ret`, the one form every unwinder knows)
and differ by `in_A` otherwise. For a caller with a frame register the difference does not matter (it
recomputes `rsp` from `rbp`); for the adapter, the only caller without one, an *asynchronous* unwind that
starts at one of the callee's last three instructions, and at the instructions between a callee's return and
the adapter's `add rsp`, recovers the adapter's frame `in_A` bytes off. No exception crosses a compiled frame
(AD-28) and nothing unwinds natively through one except a debugger or a profiler, so this is recorded as the
one place where an asynchronous unwind is not supported, and every other instruction of both prologues and
epilogues is tested (`Win64CallsUnwind`, with the answer at those three instructions asserted as wine's, in
the test's own words). The answer is wine's and says nothing about a real Windows kernel.

That a body frame is right for the unwinder whatever its callee pops holds for every instruction but its own last three: they
are the epilogue rows, and their answer depends on the unwinder reading `ret imm16` as the end of an epilogue, as wine's does
(a function with no stack arguments ends in a plain `ret` and does not depend on it). It was shown **only under wine**; whether
a real Windows unwinder reads `ret imm16` that way, and what `rsp` it then reports, is not shown.

**Accepted limit (Corey, 2026-10-08).** Microsoft's documentation describes an epilogue as ending in `ret` or a `jmp` and does
not name `ret imm16`. If a real Windows unwinder does not read it as an epilogue end, an asynchronous unwind (a debugger, a
profiler or an ETW stack sample) that lands on the last instructions of *any* callee that pops stack arguments (`lea rsp, [rbp]`,
`pop rbp`, `ret N`), a body as much as the adapter's callee, recovers the wrong caller; if it reads it as wine does, the adapter is
recovered `in_A` bytes off there. Only asynchronous unwinds are affected (no exception crosses a compiled frame, AD-28), and a
callee with no stack arguments ends in a plain `ret` and is not. This is accepted rather than designed away: the only way to
remove it is a plain `ret` everywhere, which means giving up callee-pops, and story 5 chose callee-pops for tail calls. The check
on a real Windows machine is item 1 of `planning/specs/spec-runtime-calls/checks-for-corey.md`.

**One emitted function, read.** A callable function of six parameters whose only operation is a tail call, through a slot,
to a callee of seven arguments (`grjit_emit_for` for `GRJIT_ARCH_X86_64_WIN64`, bytes through the host's `objdump -D -b
binary -mi386:x86-64 -M intel`; 758 bytes, the internal entry at 208, the adapter `[0, 178)`). So `in_A` is 16 (the fifth and
sixth parameters), `in_T` is 32 (three stack arguments, rounded) and the return address moves to `ra' = 8 + 16 - 32 = -8`:

```
adapter  0:   push rbp ; sub rsp,0x40                  ; N' = max(32, 16) + 32, rounded: 0x40, one allocation
         14:  mov r11,[rsp+0x28]                       ; args
              mov rax,[r11+0x20] ; mov [rsp],rax       ; the fifth parameter, to [rsp]  (above the callee's return address)
              mov rax,[r11+0x28] ; mov [rsp+8],rax     ; the sixth
              mov rcx,[r11] ; mov rdx,[r11+8] ; mov r8,[r11+0x10] ; mov r9,[r11+0x18]
              mov r10,[rsp+0x30]                       ; the context
         3e:  mov rbp,0x47524a4954454e01 ; lea rax,[rip+0x81] ; call rax        ; the marker, and the internal entry
         51:  mov rcx,[rsp+0x28]  ; mov r8,[rsp+0x20]  ; ...                    ; reloaded 16 lower: the callee popped them
         99:  add rsp,0x30 ; pop rbp ; ret                                      ; N' - in_A
body     d0:  push rbp ; mov rbp,rsp ; lea rax,[rsp-0xc0] ; cmp rax,[r10+0x100] ; jb overflow ; sub rsp,0xc0
         f0:  mov [rbp-8],r10 ; mov [rbp-0x20],rcx ; ... r9 ; mov rax,[rbp+0x10] -> slot ; mov rax,[rbp+0x18] -> slot
tail     1a8: mov r10,[rbp-8] ; mov r11,[rbp+8] ; mov rax,[rbp]            ; context, return address, caller's base
         1b4: mov rcx,[rbp-0x60] ; mov [rbp],rcx                          ; stack argument 4 -> where the callee finds it: ra' + 8
              mov rcx,[rbp-0x58] ; mov [rbp+8],rcx ; mov rcx,[rbp-0x50] ; mov [rbp+0x10],rcx
         1cc: mov [rbp-8],r11                                              ; the return address at ra'
              mov rcx,[rbp-0x80] ; mov rdx,[rbp-0x78] ; mov r8,[rbp-0x70] ; mov r9,[rbp-0x68]
         1e0: mov r11,[rbp-0x88] ; lea rsp,[rbp-8] ; mov rbp,rax ; jmp r11
ret      274: lea rsp,[rbp] ; pop rbp ; ret 0x10                           ; the form the unwinder knows as an epilogue
```

The copy goes through `rcx` and is done before `rcx` is loaded as the first argument; nothing reads `rdi`, `rsi` or
`rbx`; `rsp` moves last and nothing below it is read after.

**Rejected.** *The SysV registers on Win64* (six arguments would take `rsi` and `rdi`, callee-saved here;
planted defect 5 is why they are forbidden). *A shadow area between compiled functions* (nothing spills there;
32 bytes a frame, and it would not make `ret imm16` an epilogue the unwinder knows). *Caller pops* (a tail call
to a callee with more stack arguments could not move the return address without telling the original caller;
story 5's reason). *One `GRJIT_NativeResult` layout per target* (every native written three times). *The pair
in `rax` with the status in a second location* (a second location every native must write and the call must
read; a stale value is a silent miss, story 6's reason against a status in the context). *`SET_FPREG` on the
adapter* (above; planted defect 33). *A thunk adapting natives* (a second frame between every compiled function
and every native). *`ms_abi` natives on Linux in place of wine* (it hides a mismatch with a Windows compiler's
code, as the flavour section says).

**Planted defects 30 to 37** (`tools/win64-plants.txt`, run by `suite/tools/xwin/m1-controls.sh`, which builds
each into a scratch copy of the cross-built tree and requires the named test to fail by an assertion, with its
control passing on the real executable first; `check-planted.sh` lists them by name and does not count them):
**30** the pair read from `rax:rdx`, not the hidden buffer; **31** a native's area without its 32 bytes of shadow
space; **32** the tail copy through `rdi`; **33** the adapter's unwind information with `rbp` as its frame
register (the structural test fails by an assertion; the executing unwind test also dies, wine's unwinder reading
the marker as a base and faulting, which is seen and not counted as the catch); **34** the callee returning with
`ret` and not `ret imm16`; **35** only the adapter's `RUNTIME_FUNCTION` registered; **36** the walk start stored
after the call; **37** the callable frame without its outgoing area (a hook that scribbles on its shadow space
finds live slots). `check-planted-calls.py --target=win64` carries mutations of the Windows paths of
`src/x86_64/*.c` and `src/code/memory.c`; each is CAUGHT by a named test or documented below.

**What ran where.** Pins and cross emission run on the host and say the bytes did not move; wine says the
bytes run. The two new Win64 pins (callable with calls, 336 functions, and native-call, 512) are measured and
held in `test_pin.cpp`. Under wine (`suite/tools/xwin/m1-run.sh`, `m1-controls.sh`, in
`ghoti-cross-mingw64:deb13`) every test program of `runtime-core`, `runtime-heap`, `runtime-jit`,
`runtime-debug` and lang-tang runs, the call, tail and native tests for real: calls and natives of every arity,
fifty-deep chains under relocation torture, pauses, guards and `DEOPT`/`UNWIND` verdicts, a million-deep tail
recursion with `rsp` and `rbp` equal at the first and last probe, and the walk-abort cases of `runtime-core`
through a Windows child helper (the test binary run again with `--gtest_filter` and an environment variable, the
parent reading the exit status and `stderr`; `tests/test_helpers.h`). `runtime-heap` is built a second time with
`RELOCATE=yes` in a prefix of its own and its whole suite run with `GRHEAP_RELOCATE=1`, and
`check-relocation-gates` runs there (a planted stale address dies with an access violation under wine, exit
status 5, which the gate accepts as the Windows form of a signal). Every skipped test is named, with its reason,
in `suite/tools/xwin/m1-skips.txt` and counted both ways by `m1-lib.sh`; each is a test that needs a POSIX tool,
a host disassembler or a descriptor transport, or another architecture. Wine's `ntdll` is wine's own: a walk that
works there has not been shown to work under a real Windows kernel's exception dispatch, `RtlVirtualUnwind` is
wine's implementation, the guard-page growth the probes exist for is wine's, and no real Windows machine was used.
No timing is taken under wine (the benchmark runs `--smoke`).

`tools/check-planted-calls.py --target=win64` runs inside the container and plants 17 edits in the Windows paths
of `src/x86_64/*.c`, `src/code/memory.c` and the register-argument count (6, 3 and 8, the carried edit), builds each
with mingw and runs the named tests under wine: 16 are CAUGHT, 13 of them by an assertion naming what it saw and three
by a crash that wine reports (a callee that popped the wrong amount, or an adapter that left `rbp` the marker, ends the
process of the test with status 5 or without a summary, which the harness reads as a catch the way it reads a signal),
and one is documented as equivalent under wine: **a callable frame of a mebibyte entered without its page probes
runs**, because wine commits a stack on any touch in its reserved range. The probes serve the guard-page discipline of
Windows, which this run does not have, and the probe is therefore run and not shown to be needed. (The pin tests
that hash the code of a call through a pointer mask the address of `grjit_call_target_ok`, which is 8 bytes only
where the process puts the library above 4 GiB; they were measured, as the earlier ones, in a position-independent
build.)
## Gates

`make check-labels` requires every header to carry exactly one `@stability free`
label. `make check-edges` is an allowlist of `cutil`, `runtime-core` and this
library, over the `#include` lines and the shared object's `NEEDED` entries;
`runtime-heap`, `runtime-debug`, `lang-*`, `tang` and `text` are all edges it
refuses. `make check-gates` proves each by running the real scripts against a
planted fixture that must fail, naming what it found, a control that must pass,
and an empty tree that must fail rather than report success over nothing.
`check-planted` is the same idea for the backend (including three planted defects of tail calls: the last stack argument not copied, the hook's site leaving the arguments area out of the stack map, no padding, and five of calls to natives: the walk start stored after the call, the stack arguments not popped, their area not rounded to sixteen, the native-stack check without that area, an argument one word too high), and `check-planted-calls` for the call protocol: it plants, in a scratch copy, the defects of the story
(a frame missed in a deep rebuild, an early free under a waiting frame, a short reservation, a status not tested after a call through a pointer, references left out of a call site's map, the walk start not stored, retired code entered, a token or an arity not checked, a refused rebuild not noticed, a derived argument not recorded; and for tail calls a refused hook ignored, the walk start not stored before the hook, the return address left where it was, the verifier not needing the hook; and for calls to natives a status not tested, the status exit built from the state before the call, the result stored after the status test, the cause without the native bit, the stack area not rounded or the stack arguments not popped, an argument at the wrong offset, the references or the derived pointers left out of a native site's map, the stack check without the area or without the native's own use, the verifier not checking an arity, an argument's type, a destination's type or the state after a status call, and the two early frees of `runtime-core` run against `testNatives` alone) and requires a test of this library or of `runtime-core` to fail on each, the native ones each by `testNatives` or `testNative_ir` run alone. Its verdicts are CAUGHT, MISSED, TIMEOUT and BUILD FAILED, and only the first is a catch; it runs the unplanted tree first and requires it to pass, and `--self-test` runs it against edits of known outcome (one that changes nothing, one that does not compile, one that hangs) so a harness that calls everything a catch fails. The direction gate is not
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

The compiled-call, tail-call and native-call loops of `bench/bench.c` are recorded with
their machine, compiler and date under "Calls between compiled functions", "Measured",
and the call-heavy `fib` against the interpreter is lang-tang's (this library has no
interpreter).

## What is not here

- **Code shared between contexts** and a compiler thread. (Wiring it into
  `lang-tang`, tier-up, a per-execution cache and reference counting by the core
  are story 15's, and exist.)
- **Rebuilding interpreter frames from compiled ones** is the engine's, and
  `lang-tang` does it with `runtime-core`'s `a/deopt.h` at every poll and on
  every guard exit; for a chain of compiled frames it is `grcore_compiled_rebuild`
  through the `deopt` hook (above). Walking native frames for roots is
  `runtime-core`'s walk (`a/compiled.h`), which a callable function feeds by
  storing its walk start before every call that can reach a GC point.
- **Calls, tail calls and calls to natives on Win64**: story 7b of the calls spec (arm64
  has them, below). (A native with a floating-point or variadic signature cannot be described;
  resumable natives, which are called only through an exit (AD-23), and the policy of
  an opaque native under a pause are the engine's, story 9.)
- **Windows arm64 and macOS.** No backend: `grjit_backend_available()` is false,
  `grjit_compile` returns `GRJIT_ERR_UNSUPPORTED`, and every test that needs
  compiled code is reported SKIPPED (`GRJIT_REQUIRE_BACKEND`), the encoders and
  the IR still being tested, the three examples and the benchmark exit 77, which
  the Makefile counts as skipped. Windows x86-64 has a backend (above); the
  page-protection path under it is `runtime-core`'s `VirtualProtect` branch.
- **A real Windows machine.** Everything about the Windows backend has run under
  wine and in the structural tests, and nowhere else: the stack walk, the
  registration, the probes, and `check-planted` (which is a Linux target; the
  workspace's `suite/tools/xwin/m1-controls.sh` runs the Win64 planted defects against
  the built executables instead).
- **Pointer authentication and BTI** (above): unsupported and untested.
- **Real arm64 hardware.** The arm64 backend's code runs under `qemu-aarch64`
  (every test of this library, including its calls, tail calls and natives, of
  `runtime-core`, `runtime-heap` and `runtime-debug`, and `lang-tang`'s JIT arm) and
  in a simulator, and nowhere else. Instruction-cache coherence, memory ordering, the
  fault a misaligned `sp` takes on hardware (`qemu-user` does not model it: alignment is
  asserted, recorded at every hook and native, and never expected as a fault) and a real
  kernel's W^X are not exercised.
- An ahead-of-time C backend, a Wasm backend, JIT hardening, a
  register allocator, any pass, SSA, inlining, unboxing, floating point, SIMD,
  32-bit and 8-bit values.
- **An in-JIT pause.** A poll's non-zero answer ends the function; see above.
- **The AD-21 prologue check as more than a hook.**
- **A fuzzer**, for the reason above.
