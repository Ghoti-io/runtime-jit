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
 * @file backend_internal.h
 *
 * What every backend shares: the frame layout, the sites an emitter records,
 * the out-of-line stubs it queues, the state of a compile that is not an
 * instruction encoding, and the metadata builder. Never installed.
 *
 * Frame layout, below the frame base (`rbp` on x86-64, `x29` on arm64; it is
 * the frame base of the metadata): three fixed slots (the context at -8, `out`
 * at -16, `args` at -24) and then one slot per virtual register, register `v`
 * at `-8 * (v + 4)`. Nothing is ever pushed or allocated after the prologue, so
 * the stack pointer is 16-byte aligned at every call. Both backends use this
 * layout, which is why the stack-map and deopt format, the liveness pass and
 * every consumer's frame walk are the same for both. (The Windows x86-64 flavour
 * adds 48 bytes of outgoing area at the very bottom of the frame, below every
 * slot, for a callee's shadow space and the fifth and sixth arguments; the
 * metadata's frame size is the base frame without it.)
 */

#ifndef GHOTI_IO_GRJIT_SRC_BACKEND_BACKEND_INTERNAL_H
#define GHOTI_IO_GRJIT_SRC_BACKEND_BACKEND_INTERNAL_H

#include <ghoti.io/runtime-jit/macros.h>

#include <ghoti.io/runtime-jit/allocator.h>
#include <ghoti.io/runtime-jit/backend.h>
#include <ghoti.io/runtime-jit/ir.h>

#include <ghoti.io/runtime-core/a/codemeta.h>

#include "liveness_internal.h"

#ifdef __cplusplus
extern "C" {
#endif

#define GRJIT_SLOT_CTX (-8)
#define GRJIT_SLOT_OUT (-16)
#define GRJIT_SLOT_ARGS (-24)
#define GRJIT_FIXED_SLOTS 3

/** Where the instructions of the x86-64 prologue (`push rbp; mov rbp, rsp;
 *  sub rsp, N`) end, as offsets from the first byte of code, and `N`. The
 *  Windows unwind information names exactly these. */
typedef struct GRJIT_Prologue {
  uint32_t push_end;    ///< After `push rbp`.
  uint32_t setfp_end;   ///< After `mov rbp, rsp`.
  uint32_t alloc_end;   ///< After `sub rsp, N`.
  uint32_t alloc_bytes; ///< `N`: the base frame, and on Win64 the outgoing area.
} GRJIT_Prologue;

/** A label: an index into an assembler's label table. */
typedef size_t GRJIT_Label;

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

/** The part of a compile's state that is the same for every backend. */
typedef struct GRJIT_EmitCommon {
  const GRJIT_Function * f;
  const GRJIT_Allocator * allocator;
  uint32_t frame_bytes;
  uint32_t request_offset;
  GRJIT_EntryHook hook;
  GRJIT_SiteRec * sites;
  size_t site_count;
  size_t site_capacity;
  GRJIT_Pending * pending;
  size_t pending_count;
  size_t pending_capacity;
  const GRJIT_LiveSites * live;
  size_t live_cursor;
  GRJIT_Result error;
} GRJIT_EmitCommon;

/** Starts a compile's common state (zeroes it, then fills it). */
void grjit_emit_common_init(GRJIT_EmitCommon * c, const GRJIT_Function * f,
    const GRJIT_Allocator * allocator, GRJIT_EntryHook hook,
    uint32_t request_offset, uint32_t frame_bytes, const GRJIT_LiveSites * live);

/** Records a site at `offset`. Sets `error` on out-of-memory. */
void grjit_emit_add_site(GRJIT_EmitCommon * c, uint32_t offset,
    GRCORE_CodeSiteKind kind, GRCORE_PollIdentity identity, size_t live_index,
    uint32_t state);

/** Queues a stub. Sets `error` on out-of-memory. */
void grjit_emit_add_pending(GRJIT_EmitCommon * c, const GRJIT_Pending * p);

/** Sorts the recorded sites by offset, as the metadata builder needs. */
void grjit_emit_sort_sites(GRJIT_EmitCommon * c);

/** Frees the arrays the common state owns. */
void grjit_emit_common_free(GRJIT_EmitCommon * c);

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

#ifdef __cplusplus
}
#endif

#endif /* GHOTI_IO_GRJIT_SRC_BACKEND_BACKEND_INTERNAL_H */
