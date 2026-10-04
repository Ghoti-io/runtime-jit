# Ghoti.io Runtime-jit

The baseline JIT of the Ghoti.io language runtime stack, in C: a low-level IR
with a builder, a verifier and a printer; an x86-64 backend and an arm64 backend,
each with its own assembler; and the stack maps and deoptimization records the backend emits, in
the format [`runtime-core`](../runtime-core) owns (`a/codemeta.h`). An engine
builds a function in the IR, `grjit_compile` turns it into machine code in pages
taken from a context's counting page provider, and the code is run through one
C calling convention.

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
(`tools/xwin/m1-run.sh` in the workspace), not yet on a Windows machine. The arm64 backend's
encodings and hazards are tested on every host (the assembler is plain C that
emits bytes, and a small simulator in the tests executes what it emits); running
its code for real is `tools/xarch/jit-arm64.sh` in the workspace, under
`qemu-aarch64`. Every header is labelled `free`: a consumer requires
the exact version it was built against.

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

uint64_t args[1] = {100}, out[1];
uint32_t exit = grjit_code_call(code, context, args, out); /* GRJIT_EXIT_RETURNED */
/* out[0] == 4950; grjit_code_meta(code) holds the stack maps and deopt records */
grjit_code_destroy(code);
```

[examples/](examples/README.md) has three complete programs: a summation loop, a
guard that fails and the slots it reconstructs, and a dump of a compiled
function's sites.

## What the exits mean

Compiled code is `uint32_t (*)(void * context, const uint64_t * args, uint64_t * out)`.

| Return | Means | `out` |
| --- | --- | --- |
| `GRJIT_EXIT_RETURNED` (0) | a `RET` ran | `out[0]` is the value, if any |
| `GRJIT_EXIT_DEOPT` (1) | a guard failed | `out[0..interp_slot_count)` are the frame state's slots, `out[interp_slot_count]` the site's code offset, which `grcore_codemeta_find` turns into the site's record |
| `GRJIT_EXIT_REFUSED` (2) | the entry hook or a poll's helper answered non-zero | `out[0]` is that answer; nothing after it ran |

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
| `test` | build, `check-symbols`, `check-aliasing` (gcc only), `check-stamps`, the gates below, the planted-defect builds, the examples, the unit tests (the differential among them), and one smoke run of the benchmark |
| `examples` | build each program under `examples/` and run it; a failing example fails `test` |
| `check-labels` | fail if a public header has no `@stability free` label (every header here is `free`) |
| `check-edges` | fail on any `#include` or shared-object dependency on a Ghoti library other than `cutil`, `runtime-core` and this one |
| `check-gates` | run each gate against a planted defect and a control, and against an empty tree, and fail unless each behaves |
| `check-planted` | build the library with a planted backend defect (`SHR` and `SAR` swapped; every stack-map slot 8 bytes off; a live reference left out; and in the Windows flavour a callee-saved register used, no shadow space, an unwind table never registered) and require the differential, the read-back or the Win64 structural tests to fail on it, and to pass without it |
| `bench` | run the benchmark harness in full; it prints a calibration result first |
| `test-asan`, `test-tsan`, `test-valgrind-quiet` | the same tests under ASan+UBSan, ThreadSanitizer and Valgrind |
| `coverage` | instrumented run and line report |

`tools/check-install.sh <prefix>` installs nothing: it checks that what
`make install` left in a prefix can be consumed, by compiling, linking and
running a program that includes only the umbrella header.

## Status

Scaffolded. The IR, the verifier, the printer, the x86-64 and arm64 baseline
backends (the Windows one with its unwind registration) and their assemblers, the metadata, W^X code memory, the gates, the examples and the
benchmark harness are in. Not here: wiring it into an engine, tier-up and
compiled-code reference counting, rebuilding interpreter frames from compiled
ones, Windows arm64 and macOS. `documentation/design.md` says why each is where
it is.
