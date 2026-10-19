# Ghoti.io Runtime-jit

The baseline JIT of the Ghoti.io language runtime stack, in C: a low-level IR
with a builder, a verifier and a printer; an x86-64 backend and an arm64 backend,
each with its own assembler; and the stack maps and deoptimization records the backend emits, in
the format [`runtime-core`](../runtime-core) owns (`a/codemeta.h`). An engine
builds a function in the IR, `grjit_compile` turns it into machine code in pages
taken from a context's counting page provider, and the code is run through one
C calling convention.

The current Wasm-facing IR additions include typed trap exits (`out[0]` carries
the trap kind), unsigned `br_table` dispatch with a default target, an `INSTANCE`
operation for reading the active language environment, and indexed loads/stores using
`base + index * scale + displacement` with scales 1, 2, 4 or 8. The verifier
checks trap kinds, branch targets, index types and scales before emission. These
operations are emitted by the SysV x86-64, Win64 x86-64 and arm64 backends.

Callable IR supports typed multi-result calls and returns: the first result is
in the internal return register and remaining results use the caller-owned
buffer. Zero and one-result calls use the single return register alone.
Catchable slot and pointer calls add an explicit landing block for the
engine's pending exception; `RETHROW` propagates that exception status. The JIT exposes call-return offsets,
landing block IDs, and landing code offsets through `grjit_code_exception_sites`, separately from native unwind and
GC/deoptimization metadata. Tag matching and exception values remain engine
owned; `catch_exception` performs matching and unwinds guest activations before
returning zero for a match. Operating-system and C++ exceptions do not cross
generated code.

The baseline is deliberately dull. Every register of the IR lives in its own
frame slot, each operation is emitted by itself through fixed caller-saved
scratch registers, and there is no register allocator and no pass, so no GC
reference is ever held in a callee-saved register across a call and the stack
map of a site is the set of live reference slots. The one clever thing is the
poll, which is a load, a test and a branch. A benchmark that names a function the
baseline cannot serve is what justifies anything more (AD-26).

The library depends on `cutil` and `runtime-core` and nothing else, accepts no
collector type, and never interprets the IR: a test-only evaluator checks the
backend against it, on 2000 generated functions per run.

Nothing is released. Linux x86-64, Linux arm64 and Windows x86-64 are
implemented, chosen by the compiler's target when the library is built (never at
run time), and with the same frame layout, so the stack maps, the deopt records
and every consumer's frame walk are the same; on any other target (Windows arm64,
macOS) `grjit_backend_available()` is false and `grjit_compile` returns
`GRJIT_ERR_UNSUPPORTED`. The Windows flavour is the x86-64 emitter in the
Microsoft x64 calling convention, with its unwind information registered with the
system; it is emitted and its bytes tested on every host, and run under wine
(`suite/tools/xwin/m1-run.sh` in the workspace), not yet on a Windows machine. The arm64 backend's
encodings and hazards are tested on every host (the assembler is plain C that
emits bytes, and a small simulator in the tests executes what it emits); running
its code for real is `suite/tools/xarch/jit-arm64.sh` in the workspace, under
`qemu-aarch64` (a regression in the arm64 branch of `grjit_compile` passes
`make test` and fails only that script, so `suite/tools/m1-prerelease.sh` runs it,
and the Windows run, as the step before a release). Calls between compiled
functions (a callable function, `CALL_SLOT` and `CALL_PTR`, AD-28), tail calls and calls to
natives are emitted for Linux x86-64 SysV, Linux arm64 and Windows x86-64, each with its own
internal convention: `grjit_backend_calls_available()` is true wherever a backend exists, and only a
build with none refuses them with `GRJIT_ERR_UNSUPPORTED`. The Windows convention has run under wine
and not on a Windows machine. Every header is labelled `free`: a consumer requires
the exact version it was built against.

## The integer operations

Besides `I64`, `REF` and `PTR` there is `I32`, a 32-bit integer kept zero-extended in its
64-bit word, and the integer table of WebAssembly at both widths: `const`, `add`, `sub`,
`mul`, `and`, `or`, `xor`, `shl`, `shr_s`/`shr_u` (`SAR`/`SHR`), `eqz` (`CMP EQ x, #0`),
`eq`, `ne` and the signed and unsigned orderings (`CMP`), `clz`, `ctz`, `popcnt`, `rotl`,
`rotr`, `extend8_s`, `extend16_s`, `i64.extend32_s`, `div_s`, `div_u`, `rem_s`, `rem_u`, and
the conversions `i32.wrap_i64` (`WRAP`) and `i64.extend_i32_s`/`_u` (`EXTEND_S`, `EXTEND_U`).
The width of an operation is its destination's; shift and rotate counts are taken modulo the
width. The four divisions are *partial*: the frontend writes the guards in front of the
operation (a `GUARD` that the divisor is not zero, and for `div_s` that not (`a == MIN` and
`b == -1`)) and the verifier refuses a division without them; `rem_s` of `MIN % -1` is 0.
They are emitted on all three backends as real 32-bit and 64-bit instructions (x86-64 SysV, arm64
and the Windows flavour), the `I32` always zero-extended in its word. `documentation/design.md`
has the decisions and what was rejected.

They are checked against the WebAssembly spec suite: `tools/wasm/fetch.sh` fetches `i32.wast`
and `i64.wast` at the commit of the tag `wg-3.0` into `third_party/` (the harness, in
`tests/unit/test_wast.cpp`, fails without them on every host, and checks each file against the
digest in `tools/wasm/SHA256SUMS`; it finds the library from a compile-time define, so the working
directory does not matter, and `GRJIT_WASM_SUITE_DIR` names another directory that holds `test/core/`), and every `assert_return` and `assert_trap`
runs through the IR. Counts at that tag: `i32.wast` 374 passed, 85 excluded (the
`assert_invalid` and `assert_malformed` it cannot run), 0 failed; `i64.wast` 384, 31, 0. They are the
same on every backend: natively on x86-64, under `qemu-aarch64` and on a Raspberry Pi 4 for arm64,
and under wine for Win64.

## CPU features

On x86-64 the code is x86-64-v1 (SSE2), and uses SSE4.1 (`roundss`/`roundsd` for `ceil`,
`floor`, `trunc` and `nearest`), POPCNT, LZCNT and BMI1 (`tzcnt`) where the CPU has them. They
are detected from CPUID (`grjit_features_detected`), and `GRJIT_CompileOptions.disabled_features`
switches any of them off for one compile: zero disables nothing, `GRJIT_FEATURES_ALL` gives the
v1 sequences, and no mask can enable a feature the CPU lacks (`features.h`). The library reads
no environment variable. `grjit_code_features` says which features the compiled code actually
needs, and `grjit_code_usable_with(code, running)` says whether it may run where the running
context's effective set is `running`: exactly when the recorded set is a subset of it.
`grjit_features_stamp` writes the text a code-cache stamp carries for a set (`x86_64-v1`,
`x86_64-v1+sse4.1+popcnt+lzcnt+bmi1`); **no code cache exists yet**, so the stamp and the
predicate are the contract it will use, not a feature anything consumes today. A CPU without
LZCNT or BMI1 runs `lzcnt` and `tzcnt` as `bsr` and `bsf` instead of faulting, which is why
detection is a pure decoder over the CPUID words with a test for each rule, and is also run under
qemu CPU models (`suite/tools/xarch/jit-x86-cpu.sh` in the workspace). arm64 needs none of
this (`clz`, `rbit`, `frint*` and `cnt` are baseline ARMv8-A): its detected, effective and recorded
sets are always empty, the stamp is `arm64-v8` and a mask is accepted and ignored. The Win64 flavour
takes the same four features as SysV. `documentation/design.md` has the decisions and what was
rejected.

## The float operations

`F32` and `F64` are the IEEE binary32 and binary64 types, held as their **bits** in the 64-bit
word (an `F32` zero-extended, as an `I32` is). The float table of WebAssembly is there: `FADD`,
`FSUB`, `FMUL`, `FDIV`, `FMIN`, `FMAX`, `FCOPYSIGN` (binary), `FNEG`, `FABS`, `FSQRT`, `FCEIL`,
`FFLOOR`, `FTRUNC`, `FNEAREST` (unary), `CONST`, `MOVE`, `LOAD` and `STORE` as bit moves, `CMP`
over two float registers with the ordered conditions (`eq ne lt le gt ge`, false for a NaN except
`ne`), and the conversions `CONVERT_S`/`CONVERT_U` (`f32/f64.convert_i32/i64_s/u`), `TRUNC_S`/`TRUNC_U`
(`i32/i64.trunc_f32/f64_s/u`, **partial**: the frontend writes a NaN guard and a range guard in
front, and the verifier refuses the operation without the range guard), `TRUNC_SAT_S`/`TRUNC_SAT_U`,
`DEMOTE`, `PROMOTE` and `REINTERPRET`. A helper `CALL` takes and returns floats by the platform's
convention (SysV: integers in `rdi`..`r9`, floats in `xmm0`..`xmm7`, each among themselves; AAPCS64:
integers in `x0`..`x5`, floats in `d0`..`d7`, each among themselves; Microsoft x64: by position,
`rcx rdx r8 r9` or `xmm0`..`xmm3`, then two stack words); between compiled functions a float travels
as its bit word. All three backends emit every operation (x86-64 with SSE2, with SSE4.1 where the CPU
has it), and none encodes a callee-saved vector register (`xmm6`-`xmm15` on Win64, `d8`-`d15` on arm64).
Compiled code assumes the default MXCSR / FPCR and never writes it; the library and its tests are built
with `-ffp-contract=off`.
`documentation/design.md` has the decisions, the NaN rule and what was rejected.

Valgrind 3.24/VEX rounds the exact `u64` input `0x8000008000000001` to
`f32` through a binary64 halfway value: an ordinary standalone GCC conversion
and the ordinary JIT tests produce `0x5F000001`, while the same standalone
conversion under Memcheck produces `0x5F000000` with zero reported memory
errors (EVO proof `cutil-rjit-valgrind-conversion-probe-20261018-0341`).
`make test-valgrind` first runs the four affected float test bodies
ordinarily on the same binaries, requiring two selected passing tests in each
executable. During Memcheck, a test-only conversion probe activates a counted
accommodation only if VEX still has this behavior. It omits this one worked
input and substitutes its adjacent representable input in generated argument
sets; every other float case continues under Memcheck. The gate prints and
checks exactly two worked omissions, two boundary-sweep omissions, and 802
generated argument substitutions in each differential arm. If VEX is
corrected, the probe leaves all inputs intact. Production conversion semantics
and exact-bit ordinary tests are unaffected.

The pinned `conversions.wast` suite exposes eight more inputs in the same VEX
rounding family: four signed and four unsigned `i64` to `f32` conversions.
An independent GCC 16.2/Valgrind 3.24 probe reproduced the spec's eight bit
patterns ordinarily and the eight observed VEX patterns under Memcheck
(`cutil-valgrind-wast-convert-probe-20261018-0441`). Both feature arms still
execute all 593 assertions under Memcheck. While the active Valgrind probe
detects the defect, the test requires exactly 585 passes and the eight exact
line, function, expected-bit, and observed-bit divergence records in each arm;
every other failure remains fatal. Ordinary execution requires 593 passes.
Zero Memcheck memory errors is checked separately from these semantic results.

The full ordinary and Memcheck targets run through `tools/run-gtest-matrix.py`.
It clears inherited Google Test filters and shards, requires every selected test
to start and complete once, audits named skips, and compares the Memcheck test
inventory with the ordinary run. `make test` also runs a planted inherited
filter, shard, and repeat control against that selection guard.
The calls fixture scans arbitrary native-stack words for conservative roots;
its Memcheck annotation defines only the local candidate word after an ABI
load. It leaves the source stack untouched so unrelated uninitialized reads
remain visible.

The eight scalar float and conversion files of the same suite commit run too (`f32`, `f64`,
`f32_cmp`, `f64_cmp`, `f32_bitwise`, `f64_bitwise`, `conversions`, `float_misc`; `tools/wasm/fetch.sh`
fetches them): a NaN result is checked against the suite's `nan:canonical` or `nan:arithmetic` and
everything else bit for bit. Counts at that tag: 2,500, 2,500, 2,400, 2,400, 360, 360, 593 (526
returns and 67 traps) and 470 assertions pass, with the `assert_invalid` and `assert_malformed` it
cannot run counted as excluded, and 0 failed.

## Example

```c
#include <ghoti.io/runtime-jit/runtime-jit.h>
#include <ghoti.io/runtime-core/runtime-core.h>

/* sum(n): i = 0; sum = 0; while (i < n) { sum += i; i += 1; } return sum */
GRJIT_Builder * b;
grjit_builder_create("sum", 0, NULL, NULL, &b);
/* ... param, registers, blocks, operations ... */
GRJIT_Function * f;
grjit_builder_finish(b, &f);

char reason[256];
grjit_function_verify(f, NULL, reason, sizeof reason);   /* the backend requires it */

GRJIT_CompileOptions options = {0};
options.pages = grcore_context_page_provider(context);   /* counted, AD-13 */
GRJIT_Code * code;
grjit_compile(&options, f, &code);                       /* RW, filled, then R-X */

void * environment = language_instance;
uint64_t args[1] = {100}, out[1];
uint32_t exit = grjit_code_call(code, context, environment, args, out); /* GRJIT_EXIT_RETURNED */
/* out[0] == 4950; grjit_code_meta(code) holds the stack maps and deopt records */
grjit_code_destroy(code);
```

[examples/](examples/README.md) has three complete programs: a summation loop, a
guard that fails and the slots it reconstructs, and a dump of a compiled
function's sites.

## What the exits mean

Compiled code is `uint32_t (*)(void * context, void * environment, const uint64_t * args, uint64_t * out)`.
`context` is the runtime-core context used by polls and runtime services; `environment` is the opaque language
instance for this invocation. It is saved in each activation and may differ between callers and callees. It is
never stored in shared compiled code. `INSTANCE` reads the active activation's environment.

| Return | Means | `out` |
| --- | --- | --- |
| `GRJIT_EXIT_RETURNED` (0) | a `RET` ran | `out[0]` is the value, if any |
| `GRJIT_EXIT_DEOPT` (1) | a guard failed | `out[0..interp_slot_count)` are the frame state's slots, `out[interp_slot_count]` the site's code offset, which `grcore_codemeta_find` turns into the site's record |
| `GRJIT_EXIT_REFUSED` (2) | the entry hook or a poll's helper answered non-zero | `out[0]` is that answer; nothing after it ran |
| `GRJIT_EXIT_REBUILD_FAILED` (3) | a callable function's deopt hook reported that the chain rebuild was refused | `out[0]` is the rebuild's result. Fatal: some compiled frames are gone and their guest frames were not completely rebuilt, so nothing may carry on from it |

## Building

`runtime-jit` depends on `cutil` and `runtime-core`, found through pkg-config
only. Build the suite first from the workspace root (`./bootstrap.sh`), then:

```bash
export PKG_CONFIG_PATH="$PWD/.local/share/pkgconfig"
make -C libs/runtime-jit test PREFIX="$PWD/.local"
```

Pass `PREFIX=` to every `make`, including a throwaway one: the rpath is added
only when it is set. `make help` lists the targets. The ones particular to
this library:

| Target | Does |
| --- | --- |
| `test` | (needs `tools/wasm/fetch.sh` run once, for the Wasm suite) build, `check-symbols`, `check-aliasing` (gcc only), `check-stamps`, `check-fp-contract`, the gates below, the planted-defect builds, the examples, the unit tests (the differential, the call tests and the tail-call tests among them), and one smoke run of the benchmark |
| `examples` | build each program under `examples/` and run it; a failing example fails `test` |
| `check-labels` | fail if a public header has no `@stability free` label (every header here is `free`) |
| `check-edges` | fail on any `#include` or shared-object dependency on a Ghoti library other than `cutil`, `runtime-core` and this one |
| `check-fp-contract` | fail if any compile-flag set (the C and C++ flags, the library's, the sanitizer and fuzz builds') lacks `-ffp-contract=off`, ends with another contraction mode or uses `-ffast-math`: a fused multiply-add rounds once where the interpreter's C rounds twice, so a float computed by compiled code would differ on arm64 or under clang |
| `check-gates` | run each gate against a planted defect and a control, and against an empty tree, and fail unless each behaves |
| `check-planted` | build the library with a planted backend defect (`SHR` and `SAR` swapped; every stack-map slot 8 bytes off; a live reference left out; and in the Windows flavour a callee-saved register used, no shadow space, an unwind table never registered; for calls, a reference left out of a call site's map, a callee-saved register clobbered, a native-stack check that omits the frame; for tail calls, a stack argument not copied, the hook's arguments area left out of its map, no padding; for calls to natives, the walk start stored after the call, the stack arguments not popped, their area not rounded, the native-stack check without it, an argument one word too high; for the integer operations, the verifier not requiring a division's guards, `idiv` reached on `MIN % -1`, an i32 shift count masked to 64, an i32 addition done in 64 bits, swapped operands of an i32 subtraction and comparison; the same defects of the arm64 convention, 20 to 29, and the arm64 numeric ones, 69 to 87 and 92 to 100 (69 to 86, 92, 93 and 95 to 100 are also caught on any host through the simulator in `tests/a64_sim.h`), are listed in `tools/arm64-plants.txt` and run, by their tests, by `suite/tools/xarch/jit-arm64.sh` under qemu; the Win64 float ones, 88 to 91, are caught by the structural and byte tests on any host and, with the shared defects of the integer, float and CPU-feature stories rerun on Win64, listed in `tools/win64-plants.txt` for `suite/tools/xwin/m1-controls.sh` under wine) and require the differential, the read-back, the call tests, the tail-call tests, the native-call tests or the Win64 structural tests to fail on it, and to pass without it; defects 101 to 109 are in the benchmark harness (the C reference of one kernel, or one family of kernels, one off) and `bench --smoke` must abort on each |
| `check-planted-calls` | plant, in a scratch copy of `runtime-core` or of this library, the defects of the call protocol (a frame missed in a rebuild, a rebuilt frame left its pushed identity, an early free, a short reservation, no status test after a pointer call, a reference left out of a call site's map, the walk start not stored, retired code entered, a token or an arity not checked, a refused rebuild ignored, a derived argument not recorded; for tail calls, a refused hook ignored, the walk start not stored before the hook, the return address left where it was, the verifier not needing the hook; for calls to natives, a status not tested, the status exit built from the state before the call, the result stored after the status test, the cause without the native bit, the stack area not rounded or not popped, an argument at the wrong offset, references or derived pointers left out of a native site's map, the stack check without the area or without the native's use, the verifier not checking an arity, a type or a state, and the two early frees run against the native tests alone; the narrowing of every hook's and the poll helper's 32-bit answer, in each place a function tests it; and, for the arm64 emitter, the indices and kinds of a call's, a tail call's and a native's sites and its large offsets, which the host runs through the metadata and the words it emits for arm64) and require a test to fail on each; first run it against edits of known outcome so that a missed one, a build failure and a hang each show as what they are, not as a catch. `--target=arm64` does the same for the mutations of `src/arm64/`, built with the cross compiler and run under qemu (in the container of `suite/tools/xarch/jit-arm64.sh`, which runs it); a catch by the pin test alone is PIN-ONLY, which is not one |
| `bench` | run the benchmark harness in full; it prints a calibration result first |
| `test-asan`, `test-tsan`, `test-valgrind-quiet` | the same tests under ASan+UBSan, ThreadSanitizer and Valgrind |
| `coverage` | instrumented run and line report |

`tools/check-install.sh <prefix>` installs nothing: it checks that what
`make install` left in a prefix can be consumed, by compiling, linking and
running a program that includes only the umbrella header.

## Status

The IR (with `I32`, `F32`, `F64` and the integer and float operations, on all three backends), the verifier, the printer, the x86-64 and arm64 baseline
backends (the Windows one with its unwind registration) and their assemblers, the metadata, W^X code memory, the gates, the examples and the
benchmark harness are in, and so are the parts that need an engine: wiring it
into `lang-tang` (story 15), tier-up, compiled-code reference counting and the
rebuilding of interpreter frames from compiled ones at a poll and at a guard
exit (the last is the engine's, done with `runtime-core`'s `a/deopt.h`), and,
on x86-64 SysV and arm64, calls between compiled functions: a callable function with an
internal entry, calls through an entry slot or a code pointer, stack maps at the
call sites, a chain deoptimization and a native-stack check in bytes (AD-28),
tail calls through an entry slot or a code pointer that replace the caller's
frame, native and guest, in constant stack (AD-28), and calls to registered, typed
natives (`natives.h`): a C call by the platform's ABI with the context first, a
status that leaves compiled code through the chain deopt with the state after the
call, a native-stack check in bytes, and the walk start stored first, so a
collection under the native sees every compiled frame below it (AD-28, AD-17).
Not here: those calls, tail calls and native calls on Win64 (story 7b),
Windows arm64 and macOS. `documentation/design.md` says why each is where
it is.
