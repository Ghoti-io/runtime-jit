/*
 * SPDX-License-Identifier: LGPL-3.0-only
 *
 * Copyright (C) 2026 Corey Pennycuff
 *
 * This file is part of Ghoti.io Runtime-jit.
 *
 * Ghoti.io Runtime-jit is free software: you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public License version
 * 3 as published by the Free Software Foundation.
 *
 * Ghoti.io Runtime-jit is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU Lesser
 * General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

/**
 * @file emit_internal.h
 *
 * The state of one arm64 compile and the pieces of it that live in different
 * source files: the operation emitter and the exit stubs. Never installed.
 *
 * **Frame.** The prologue is `stp x29, x30, [sp, #-16]!`, `mov x29, sp`, then
 * the frame is allocated so that `sp` is a multiple of 16, and `sp` is never
 * moved again, so it is aligned at every call. The frame base of the metadata
 * is `x29`; slots are at the offsets ../backend/backend_internal.h gives, so
 * `[x29]` is the caller's frame pointer and `[x29 + 8]` the return address, the
 * pair a native helper follows to walk to its caller (the read-back test, and
 * `lang-tang`'s helpers). No GC reference is ever live in a callee-saved
 * register: every value is in a frame slot, and the scratch registers are the
 * caller-saved x0-x17 (AD-17 holds by construction). `x18`, the platform
 * register, is never touched.
 *
 * **Registers.** x0 and x1 carry the operands of an operation and its result is
 * x0; x2 holds `out` in the exits; the arguments of a helper call are x0-x5;
 * a call goes through x16 loaded by `movz`/`movk`; x17 holds a displacement
 * that does not fit an instruction.
 *
 * **A callable function** (AD-28; ../backend/backend_internal.h) uses the same
 * frame, with the internal convention between compiled functions:
 *
 *  - arguments in `x0`-`x7`, the rest at `[sp + 8 i]` at the call, in an area
 *    of whole 16-byte units that the *callee* pops (`mov sp, x29; ldp x29, x30,
 *    [sp], #16; add sp, sp, #in; ret`, the `add` omitted for no stack
 *    arguments), so a tail call to a callee with more stack arguments than its
 *    caller can move the frame (the reason is story 4's);
 *  - the context in `x9`, the result in `x0`, the status in `x1` (RETURNED 0,
 *    DEOPTED 1, FAILED 2; `x0` then holds the cause or the hook's answer);
 *  - `x10` is the copy temporary of a stack argument, `x15` holds the context
 *    while the walk-start cell is stored, `x16` is the call target and
 *    `adr`'s destination, and `x17` holds a displacement that fits no
 *    immediate;
 *  - **no callee-saved register is used** (`x19`-`x28`, `d8`-`d15`) and `x18`
 *    is never touched. The entry adapter sets `x29` to the chain-end marker
 *    before it calls the first compiled function and gives it back from its
 *    own frame record on the way out, so the caller of the entry sees `x29`
 *    as it left it.
 *
 * `sp` is a multiple of 16 at every instruction that uses it as a base: the
 * frame is made once, an argument area is whole 16-byte units, and nothing
 * else moves it.
 */

#ifndef GHOTI_IO_GRJIT_SRC_ARM64_EMIT_INTERNAL_H
#define GHOTI_IO_GRJIT_SRC_ARM64_EMIT_INTERNAL_H

#include <ghoti.io/runtime-jit/macros.h>

#include <ghoti.io/runtime-jit/allocator.h>
#include <ghoti.io/runtime-jit/backend.h>
#include <ghoti.io/runtime-jit/ir.h>

#include "../backend/backend_internal.h"

#include "asm_internal.h"

#ifdef __cplusplus
extern "C" {
#endif

/** One compile's state: the common part, the assembler and the labels. */
typedef struct GRJIT_A64Emit {
  GRJIT_EmitCommon c;
  GRJIT_A64Asm as;
  GRJIT_Label * blocks;
  GRJIT_Label refuse;
  /* A callable function. */
  GRJIT_Label internal;      ///< The internal entry.
  GRJIT_Label ret_deopted;   ///< Returns DEOPTED with `x0` untouched.
  GRJIT_Label ret_failed;    ///< Returns FAILED with the hook's answer in `x0`.
  GRJIT_Label ret_propagate; ///< Returns what a callee returned, `x1` and `x0` as they are.
  GRJIT_Label overflow;      ///< The prologue's native-stack exit.
  uint32_t internal_offset;  ///< Where the internal entry is, after the adapter and the tag.
} GRJIT_A64Emit;

/**
 * Emits the whole function into `out`: prologue, every block, then the stubs.
 * The recorded sites come back sorted by offset. A function whose conditional
 * branches do not all reach is assembled a second time with the long form (see
 * asm_internal.h). On failure `out` is still freed by ::grjit_a64_emit_free.
 *
 * @return ::GRJIT_OK, ::GRJIT_ERR_OOM, ::GRJIT_ERR_LIMIT (the byte cap, or a
 *   branch the cap lets be too far to reach) or ::GRJIT_ERR_INTERNAL.
 */
GRJIT_Result grjit_a64_emit_function(const GRJIT_Function * function,
    const GRJIT_Allocator * allocator, size_t max_code_bytes,
    GRJIT_EntryHook hook, uint32_t request_offset, uint32_t frame_bytes,
    const GRJIT_LiveSites * live, GRJIT_A64Emit * out);

/** Frees what ::grjit_a64_emit_function allocated. */
void grjit_a64_emit_free(GRJIT_A64Emit * e);

/** The shared refusal path: `out[0] = x0; return REFUSED`. */
void grjit_a64_emit_refuse_stub(GRJIT_A64Emit * e);
/** A poll's slow path: call the helper, refuse on non-zero, resume. */
void grjit_a64_emit_poll_stub(GRJIT_A64Emit * e, const GRJIT_Pending * p);
/** A guard's exit: write the frame state to `out`, return DEOPT. */
void grjit_a64_emit_guard_stub(GRJIT_A64Emit * e, const GRJIT_Pending * p);
/** The epilogue and `ret`. */
void grjit_a64_emit_epilogue(GRJIT_A64Emit * e);
/** A callable function's epilogue: the frame, then the callee pops its stack arguments. */
void grjit_a64_emit_callable_epilogue(GRJIT_A64Emit * e);
/** Records where the walk starts (a/layout.h): stores the frame base and the address of
 *  `ret_label`, the return address of the call about to be made, in the context's cell.
 *  Uses `x15` and `x16`, and leaves the context in `x15`. */
void grjit_a64_emit_store_walk_cell(GRJIT_A64Emit * e, GRJIT_Label ret_label);
/** The shared exits of a callable function: a call through an empty slot, the call's
 *  exit, a native call's two exits, the native-stack exit and the returns. */
void grjit_a64_emit_call_slow_stub(GRJIT_A64Emit * e, const GRJIT_Pending * p);
void grjit_a64_emit_call_exit_stub(GRJIT_A64Emit * e, const GRJIT_Pending * p);
void grjit_a64_emit_native_exit_stub(GRJIT_A64Emit * e, const GRJIT_Pending * p);
void grjit_a64_emit_native_status_stub(GRJIT_A64Emit * e, const GRJIT_Pending * p);
void grjit_a64_emit_overflow_stub(GRJIT_A64Emit * e);
void grjit_a64_emit_ret_deopted(GRJIT_A64Emit * e);
void grjit_a64_emit_ret_status(GRJIT_A64Emit * e);

#ifdef __cplusplus
}
#endif

#endif /* GHOTI_IO_GRJIT_SRC_ARM64_EMIT_INTERNAL_H */
