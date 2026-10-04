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
 * Frame layout, below `rbp` (the frame base of the metadata): three fixed
 * slots (the context at -8, `out` at -16, `args` at -24) and then one slot per
 * virtual register, register `v` at `-8 * (v + 4)`. Nothing is ever pushed
 * after the prologue, so `rsp` is 16-byte aligned at every call.
 */

#ifndef GHOTI_IO_GRJIT_SRC_X86_64_EMIT_INTERNAL_H
#define GHOTI_IO_GRJIT_SRC_X86_64_EMIT_INTERNAL_H

#include <ghoti.io/runtime-jit/macros.h>

#include <ghoti.io/runtime-jit/allocator.h>
#include <ghoti.io/runtime-jit/backend.h>
#include <ghoti.io/runtime-jit/ir.h>

#include <ghoti.io/runtime-core/a/codemeta.h>

#include "asm_internal.h"
#include "liveness_internal.h"

#define GRJIT_SLOT_CTX (-8)
#define GRJIT_SLOT_OUT (-16)
#define GRJIT_SLOT_ARGS (-24)
#define GRJIT_FIXED_SLOTS 3

/** The frame offset of a register's slot. */
static inline int32_t grjit_emit_slot(GRJIT_VReg v) {
  return -8 * ((int32_t)v + GRJIT_FIXED_SLOTS + 1);
}

/** One site, recorded as it is emitted. */
typedef struct GRJIT_SiteRec {
  uint32_t offset;
  GRCORE_CodeSiteKind kind;
  GRCORE_PollIdentity identity;
  size_t live_index;  ///< Index into the liveness table.
  uint32_t state;     ///< The operation's frame state.
} GRJIT_SiteRec;

/** An out-of-line stub still to be emitted after the blocks. */
typedef enum GRJIT_PendingKind {
  GRJIT_PENDING_POLL,
  GRJIT_PENDING_GUARD
} GRJIT_PendingKind;

typedef struct GRJIT_Pending {
  GRJIT_PendingKind kind;
  GRJIT_Label entry;  ///< Where the fast path jumps to.
  GRJIT_Label back;   ///< Where a poll's slow path resumes.
  const GRJIT_Op * op;
  size_t live_index;
} GRJIT_Pending;

typedef struct GRJIT_Emit {
  const GRJIT_Function * f;
  const GRJIT_Allocator * allocator;
  GRJIT_Asm as;
  GRJIT_Label * blocks;
  uint32_t frame_bytes;
  uint32_t request_offset;
  GRJIT_EntryHook hook;
  GRJIT_Label refuse;
  GRJIT_SiteRec * sites;
  size_t site_count;
  size_t site_capacity;
  GRJIT_Pending * pending;
  size_t pending_count;
  size_t pending_capacity;
  const GRJIT_LiveSites * live;
  size_t live_cursor;
  GRJIT_Result error;
} GRJIT_Emit;

/** Records a site at `offset`. Sets `error` on out-of-memory. */
void grjit_emit_add_site(GRJIT_Emit * e, uint32_t offset,
    GRCORE_CodeSiteKind kind, GRCORE_PollIdentity identity, size_t live_index,
    uint32_t state);

/** Queues a stub. Sets `error` on out-of-memory. */
void grjit_emit_add_pending(GRJIT_Emit * e, const GRJIT_Pending * p);

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
    const GRJIT_LiveSites * live, GRJIT_Emit * out);

/** Frees what ::grjit_emit_function allocated. */
void grjit_emit_free(GRJIT_Emit * e);

/** The shared refusal path: `out[0] = rax; return REFUSED`. */
void grjit_emit_refuse_stub(GRJIT_Emit * e);
/** A poll's slow path: call the helper, refuse on non-zero, resume. */
void grjit_emit_poll_stub(GRJIT_Emit * e, const GRJIT_Pending * p);
/** A guard's exit: write the frame state to `out`, return DEOPT. */
void grjit_emit_guard_stub(GRJIT_Emit * e, const GRJIT_Pending * p);

/** The metadata of a compile, with the arrays its sites point into. */
typedef struct GRJIT_MetaStorage {
  const GRJIT_Allocator * allocator;
  GRCORE_CodeSite * sites;
  GRCORE_CodeLocation * locations;
  GRCORE_DerivedPointer * derived;
  GRCORE_CodeMeta meta;
} GRJIT_MetaStorage;

/** Builds the table from the recorded sites, which must be sorted by offset.
 *  On failure nothing is left allocated. */
GRJIT_Result grjit_metadata_build(const GRJIT_Function * function,
    const GRJIT_LiveSites * live, const GRJIT_SiteRec * recs, size_t count,
    uint32_t frame_bytes, uint32_t code_bytes, const GRJIT_Allocator * allocator,
    GRJIT_MetaStorage * out);

/** Frees what ::grjit_metadata_build allocated. */
void grjit_metadata_free(GRJIT_MetaStorage * storage);

#endif /* GHOTI_IO_GRJIT_SRC_X86_64_EMIT_INTERNAL_H */
