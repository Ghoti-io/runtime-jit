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

/** One compile's state: the common part, the assembler and the labels. */
typedef struct GRJIT_Emit {
  GRJIT_EmitCommon c;
  GRJIT_Asm as;
  GRJIT_Label * blocks;
  GRJIT_Label refuse;
  bool win64;              ///< The Microsoft x64 flavour (see ::GRJIT_WIN64_OUTGOING).
  GRJIT_Prologue prologue; ///< Where the prologue's instructions end.
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
/** A poll's slow path: call the helper, refuse on non-zero, resume. */
void grjit_emit_poll_stub(GRJIT_Emit * e, const GRJIT_Pending * p);
/** A guard's exit: write the frame state to `out`, return DEOPT. */
void grjit_emit_guard_stub(GRJIT_Emit * e, const GRJIT_Pending * p);

#ifdef __cplusplus
}
#endif

#endif /* GHOTI_IO_GRJIT_SRC_X86_64_EMIT_INTERNAL_H */
