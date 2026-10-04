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

#ifdef __cplusplus
}
#endif

#endif /* GHOTI_IO_GRJIT_SRC_ARM64_EMIT_INTERNAL_H */
