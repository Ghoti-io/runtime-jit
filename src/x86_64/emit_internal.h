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
 * The state of one compile and the pieces of it that live in different
 * source files: the operation emitter, the exit stubs and the metadata
 * builder. Never installed.
 *
 * The frame base of the metadata is `rbp`; the layout below it is shared with
 * the other backend and is described in ../backend/backend_internal.h.
 */

#ifndef GHOTI_IO_GRJIT_SRC_X86_64_EMIT_INTERNAL_H
#define GHOTI_IO_GRJIT_SRC_X86_64_EMIT_INTERNAL_H

#include <ghoti.io/runtime-jit/macros.h>

#include <ghoti.io/runtime-jit/allocator.h>
#include <ghoti.io/runtime-jit/backend.h>
#include <ghoti.io/runtime-jit/ir.h>

#include <ghoti.io/runtime-core/a/codemeta.h>

#include "../backend/backend_internal.h"

#include "asm_internal.h"

#ifdef __cplusplus
extern "C" {
#endif

/** The registers of the internal convention between compiled functions
 *  (backend_internal.h): the arguments, the context, and the scratch registers
 *  the call and tail-call sequences are written against. */
typedef struct GRJIT_X86Internal {
  const GRJIT_Reg * args;   ///< The register arguments, in order.
  unsigned reg_args;        ///< How many (`GRJIT_*_INTERNAL_REG_ARGS`).
  GRJIT_Reg ctx;            ///< The context, caller-saved and no argument register.
  GRJIT_Reg scratch;        ///< A register no argument uses: the tail call's return
                            ///< address and entry, the adapter's pointer to `args`.
  GRJIT_Reg tail_copy;      ///< The temporary a tail call copies a stack argument through.
  GRJIT_Reg status;         ///< The status the callee returns (the result is `rax`).
  GRJIT_Reg c_ret2;         ///< Where a C callee puts the second word of a two-word
                            ///< result (a native's status): `rdx`.
} GRJIT_X86Internal;

/** One x86-64 calling convention's registers: the C ABI's, which a helper, a
 *  hook or a native is called by, and the internal convention's, which a
 *  callable function is written against. */
typedef struct GRJIT_X86Abi {
  const GRJIT_Reg * c_args;        ///< The C ABI's integer argument registers.
  unsigned c_reg_args;             ///< How many.
  uint32_t shadow_bytes;           ///< The callee's shadow space, which is where the first
                                   ///< C stack argument is at `rsp`: 32 on Win64, else 0.
  bool hidden_pair;                ///< A 16-byte struct result (a native's value and status)
                                   ///< comes back through a hidden pointer in the first C
                                   ///< argument register, with the context second (Win64),
                                   ///< and not in `rax:rdx` (SysV).
  const GRJIT_X86Internal * internal;
} GRJIT_X86Abi;

extern const GRJIT_X86Abi grjit_x86_abi_sysv;
extern const GRJIT_X86Abi grjit_x86_abi_win64;

/** The `k`th C-ABI argument register, an internal-convention argument register, the
 *  context register and the status register of an emitter `e`. */
#define C_ARG(e, k) ((e)->abi->c_args[k])
#define IARG(e, k) ((e)->abi->internal->args[k])
#define ICTX(e) ((e)->abi->internal->ctx)
#define ISCRATCH(e) ((e)->abi->internal->scratch)
#define ISTATUS(e) ((e)->abi->internal->status)

/** One compile's state: the common part, the assembler and the labels. */
typedef struct GRJIT_Emit {
  GRJIT_EmitCommon c;
  GRJIT_Asm as;
  const GRJIT_X86Abi * abi; ///< The target's registers.
  GRJIT_Label * blocks;
  GRJIT_Label refuse;
  bool win64;              ///< The Microsoft x64 flavour (see ::GRJIT_WIN64_OUTGOING).
  GRJIT_Prologue prologue; ///< Where the prologue's instructions end (and, for a callable
                           ///< function, the adapter's and the body's records).
  /* A callable function (AD-28; backend_internal.h for the convention). */
  GRJIT_Label internal;     ///< The internal entry.
  GRJIT_Label ret_deopted;  ///< Returns DEOPTED with `rax` untouched.
  GRJIT_Label ret_failed;   ///< Returns FAILED with the hook's answer in `rax`.
  GRJIT_Label ret_propagate; ///< Returns what a callee returned, `rdx` and `rax` as they are.
  GRJIT_Label overflow;     ///< The prologue's native-stack exit.
  uint32_t internal_offset; ///< Where the internal entry is, after the adapter.
} GRJIT_Emit;

/** The outgoing area at the bottom of a Win64 frame: 32 bytes of shadow space
 *  for the callee and two words for the fifth and sixth arguments, at
 *  `[rsp + 32]` and `[rsp + 40]`. The frame's `N` is the base frame plus this,
 *  so `rsp` is 16-aligned at every call and no slot overlaps a callee's
 *  shadow space. */
#define GRJIT_WIN64_OUTGOING 48

/** The size of the page a Win64 frame commits one at a time: a frame this big
 *  (with the return address and the saved `rbp`) is probed a page at a time,
 *  downwards, so the guard page is hit in order. */
#define GRJIT_WIN64_PAGE 4096

/**
 * Emits the whole function into `out`: prologue, every block, then the stubs.
 * The recorded sites come back sorted by offset. On failure `out` is still
 * freed by ::grjit_emit_free.
 *
 * @return ::GRJIT_OK, ::GRJIT_ERR_OOM, ::GRJIT_ERR_LIMIT (the byte cap) or
 *   ::GRJIT_ERR_INTERNAL.
 */
GRJIT_Result grjit_emit_function(const GRJIT_Function * function,
    const GRJIT_Allocator * allocator, size_t max_code_bytes,
    GRJIT_EntryHook hook, uint32_t request_offset, uint32_t frame_bytes,
    bool win64, const GRJIT_LiveSites * live, GRJIT_Emit * out);

/** The frame's end: `leave; ret` for SysV, and for Win64 the form its unwinder
 *  recognises as an epilogue, `lea rsp, [rbp]; pop rbp; ret`. */
void grjit_emit_epilogue(GRJIT_Emit * e);

/** Frees what ::grjit_emit_function allocated. */
void grjit_emit_free(GRJIT_Emit * e);

/** The shared refusal path: `out[0] = rax; return REFUSED`. */
void grjit_emit_refuse_stub(GRJIT_Emit * e);
/** Records where the walk starts (a/layout.h): stores the frame base and the
 *  address of `ret_label`, the return address of the call about to be made, in
 *  the context's cell. Uses `rax` and `rcx`, and leaves the context in `rcx`. */
void grjit_emit_store_walk_cell(GRJIT_Emit * e, GRJIT_Label ret_label);
/** The shared exits of a callable function: a call through an empty slot, the
 *  call's exit, the native-stack exit, and the return of `DEOPTED`. */
void grjit_emit_call_slow_stub(GRJIT_Emit * e, const GRJIT_Pending * p);
void grjit_emit_call_exit_stub(GRJIT_Emit * e, const GRJIT_Pending * p);
void grjit_emit_overflow_stub(GRJIT_Emit * e);
/** A native call's two exits: before the call (the state, cause zero) and after it
 *  (the state after the call, the cause `GRJIT_CAUSE_NATIVE | status`, the status
 *  in `rdx` on entry). */
void grjit_emit_native_exit_stub(GRJIT_Emit * e, const GRJIT_Pending * p);
void grjit_emit_native_status_stub(GRJIT_Emit * e, const GRJIT_Pending * p);
void grjit_emit_ret_deopted(GRJIT_Emit * e);
/** The other two returns: FAILED, and a callee's non-zero status passed on. */
void grjit_emit_ret_status(GRJIT_Emit * e);
/** `leave; ret` of a callable function: the callee pops its stack arguments. */
void grjit_emit_callable_epilogue(GRJIT_Emit * e);
/** A poll's slow path: call the helper, refuse on non-zero, resume. */
void grjit_emit_poll_stub(GRJIT_Emit * e, const GRJIT_Pending * p);
/** A guard's exit: write the frame state to `out`, return DEOPT. */
void grjit_emit_guard_stub(GRJIT_Emit * e, const GRJIT_Pending * p);

#ifdef __cplusplus
}
#endif

#endif /* GHOTI_IO_GRJIT_SRC_X86_64_EMIT_INTERNAL_H */
