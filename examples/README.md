# Examples

Indexed by task. `make examples` builds and runs every one; a failing example
fails `make test`.

| I want to... | Example | What it shows |
| --- | --- | --- |
| Turn a function into machine code and run it | [`sum_loop.c`](sum_loop.c) | The whole life of a function: build the IR for a summation loop, verify and print it, compile it into pages from a context's counting page provider (flipped to read-execute), run it, and destroy it |
| See what a failed guard hands back | [`guard_deopt.c`](guard_deopt.c) | A guard that holds falls through; one that fails returns `GRJIT_EXIT_DEOPT` with the frame state's slots and the offset of its site, which `grcore_codemeta_find` turns into a record |
| Read the stack maps a compile emitted | [`stack_map_dump.c`](stack_map_dump.c) | Prints every site of a compiled function: its offset, its kind, the live references and the derived pointer as frame-slot offsets, and the deoptimization state |

All three use `runtime-jit` and `runtime-core` only, and are C. The context is
there to supply the page provider the code is mapped from (so that the code's
memory is counted) and the request word a compiled poll loads.

None of them rebuilds an interpreter frame from a deopt exit or walks a native
frame to find roots: those are the engine's and the collector's work, written
against the format these examples print. `documentation/design.md` says which
story does them.
