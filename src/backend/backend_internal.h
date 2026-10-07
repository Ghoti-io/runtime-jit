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
#include <ghoti.io/runtime-core/a/registry.h>

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
  const GRJIT_Op * op; ///< The operation, for the sites of a call to compiled
                       ///< code (the push's arguments live in the frame); NULL
                       ///< for every other site.
} GRJIT_SiteRec;

/* ---- Calls between compiled functions (AD-28), x86-64 SysV ----------------
 *
 * The internal calling convention:
 *
 *  - integer-class arguments in `rdi, rsi, rdx, rcx, r8, r9`, the rest on the
 *    stack above the return address, in order, in an area of whole 16-byte
 *    units that the *callee* pops (`ret imm16`). The callee knowing its own
 *    parameter count is what lets a tail call (story 5) to a callee with more
 *    stack arguments than its caller has shift the return address and reuse
 *    the frame; with a caller that pops, the caller would have to be told;
 *  - the context in `r10`, which is caller-saved and no argument register;
 *  - the result in `rax`, the status in `rdx` (RETURNED or DEOPTED; `rax` then
 *    holds the deopt's cause, which every frame returns unchanged);
 *  - nothing callee-saved is used or needs saving: `rbx`, `rbp` (as a
 *    register), `r12`-`r15` come back as they went in;
 *  - the frame is `rbp`-linked as a/compiled.h requires.
 *
 * The frame of a callable function has, below the vreg slots, the *arguments
 * area* (one slot for each argument of its widest call or tail call: the push
 * or tail hook reads the arguments there, and a moving collector updates them,
 * which is why the area is in the stack map of that site) and one slot that
 * holds the entry address a call loaded across the hook.
 *
 * A function with a tail call may have *pad* slots between its vreg slots and
 * the area, so that the area ends at or below the lowest address a tail call's
 * frame replacement writes (see emit.c, emit_tail_call): the area is the
 * staging place for the callee's arguments, and a callee with more stack
 * arguments than the function has reaches below the function's own incoming
 * area. The area macros take the pad as part of the register count
 * (GRJIT_SHAPE_REGS). */
#define GRJIT_STATUS_RETURNED 0u
#define GRJIT_STATUS_DEOPTED 1u
/** The engine's `deopt` hook refused the rebuild; `rax` is its answer. */
#define GRJIT_STATUS_FAILED 2u
/** The sixteen bytes before every internal entry: a word holding the magic in
 *  its high half and the parameter count in its low half, then the function's
 *  token. A call through a code pointer checks all three, so a target that is
 *  not compiled code, or not the function the call means, or takes a different
 *  number of arguments, is never entered. */
#define GRJIT_ENTRY_TAG_MAGIC UINT32_C(0x4752494E)
#define GRJIT_ENTRY_TAG_WORD(params) \
  ((UINT64_C(0x4752494E) << 32) | (uint64_t)(uint32_t)(params))
#define GRJIT_ENTRY_TAG_BYTES 16u
/** The most arguments an internal convention passes in registers, by target:
 *  `rdi, rsi, rdx, rcx, r8, r9` on x86-64 SysV, `x0`-`x7` on arm64. (Win64's,
 *  four, is story 7b's; the shape and the stack-argument area take the count as
 *  a parameter so that each backend's own is the one used.) */
#define GRJIT_SYSV_INTERNAL_REG_ARGS 6u
#define GRJIT_ARM64_INTERNAL_REG_ARGS 8u
#define GRJIT_WIN64_INTERNAL_REG_ARGS 4u

/** How a callable function's frame is shaped. */
typedef struct GRJIT_CallableShape {
  size_t args_area;    ///< Slots in the arguments area: the widest call.
  bool has_calls;      ///< Whether it calls compiled code at all (a tail call
                       ///< included).
  size_t pad;          ///< Slots between the vreg slots and the arguments area
                       ///< that a tail call to a callee with more stack
                       ///< arguments needs: zero for every function without one.
  size_t extra_slots;  ///< Slots below the vreg slots: pad, area, plus one for
                       ///< the saved entry when it has calls.
  uint32_t incoming_bytes; ///< Bytes of stack arguments its callers push.
} GRJIT_CallableShape;

/** The bytes of stack arguments the internal convention puts for `n` arguments
 *  when `reg_args` of them travel in registers (the target's count): the rest, in
 *  whole 16-byte units so the stack stays aligned. They are above the return
 *  address on x86-64 and at the stack pointer at the call on arm64. */
static inline uint32_t grjit_stack_arg_bytes(size_t n, unsigned reg_args) {
  size_t stack_args = n > reg_args ? n - reg_args : 0;
  return (uint32_t)((stack_args * 8 + 15) / 16 * 16);
}

/** The register count the area macros below are given: the function's own and
 *  the shape's padding, which sits between them and the area. */
#define GRJIT_SHAPE_REGS(f, shape) ((f)->vreg_count + (shape).pad)

/** Whether `target` is the internal entry of compiled code registered in the
 *  context (src/code/target.c): the check a call through a code pointer makes.
 *  Called from compiled code, through the C ABI. It also requires the tag
 *  before the entry to name `callee` and `arg_count` parameters. */
uint32_t grjit_call_target_ok(
    void * context, uint64_t target, uint64_t callee, uint64_t arg_count);

/** The shape of `f`, which need not be callable (then all zero), for a target
 *  whose internal convention passes `reg_args` arguments in registers. */
void grjit_callable_shape(const GRJIT_Function * f, unsigned reg_args, GRJIT_CallableShape * out);

/** The frame offset of argument `k` in the arguments area of a function with
 *  `vreg_count` registers and `area` slots in it: the area follows the registers'
 *  slots and is laid out as an array, `k` rising with the address, so the push
 *  hook is handed `&area[0]` and reads `arg_count` words. */
#define GRJIT_ARGS_SLOT(vreg_count, area, k) \
  (-8 * ((int32_t)(vreg_count) + GRJIT_FIXED_SLOTS + (int32_t)(area)) + 8 * (int32_t)(k))
/** The slot that holds the entry across the push: the one below the area. */
#define GRJIT_ENTRY_SAVE_SLOT(vreg_count, shape) \
  (-8 * ((int32_t)(vreg_count) + GRJIT_FIXED_SLOTS + (int32_t)(shape).args_area + 1))

/** An out-of-line stub still to be emitted after the blocks. */
typedef enum GRJIT_PendingKind {
  GRJIT_PENDING_POLL,
  GRJIT_PENDING_GUARD,
  GRJIT_PENDING_CALL_SLOW, ///< A call through an empty or refused slot.
  GRJIT_PENDING_CALL_EXIT, ///< A call's exit before the call.
  GRJIT_PENDING_NATIVE_EXIT,   ///< A native call's exit before the call (its
                               ///< native-stack check failed): in `op->state`.
  GRJIT_PENDING_NATIVE_STATUS  ///< The exit a native's non-zero status takes: in
                               ///< `op->exit_state`, the state after the call.
} GRJIT_PendingKind;

typedef struct GRJIT_Pending {
  GRJIT_PendingKind kind;
  GRJIT_Label entry;  ///< Where the fast path jumps to.
  GRJIT_Label back;   ///< Where a poll's (or a slot call's) slow path resumes.
  GRJIT_Label exit;   ///< A slot call's exit stub, for its slow path.
  const GRJIT_Op * op;
  size_t live_index;
} GRJIT_Pending;

/** The part of a compile's state that is the same for every backend. */
typedef struct GRJIT_EmitCommon {
  const GRJIT_Function * f;
  unsigned reg_args;           ///< Register arguments of the internal convention.
  const GRJIT_Allocator * allocator;
  uint32_t frame_bytes;
  uint32_t request_offset;
  GRJIT_EntryHook hook;
  uint32_t walk_cell_offset;   ///< From the layout descriptor (callable only).
  uint32_t native_limit_offset;
  bool callable;               ///< The function uses the internal convention.
  GRJIT_CallableShape shape;
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
    unsigned reg_args, const GRJIT_Allocator * allocator, GRJIT_EntryHook hook,
    uint32_t request_offset, uint32_t frame_bytes, const GRJIT_LiveSites * live);

/** Records a site at `offset`. Sets `error` on out-of-memory. */
void grjit_emit_add_site(GRJIT_EmitCommon * c, uint32_t offset,
    GRCORE_CodeSiteKind kind, GRCORE_PollIdentity identity, size_t live_index,
    uint32_t state);

/** ::grjit_emit_add_site, for a site of a call to compiled code, which keeps
 *  the operation so the metadata can add the arguments the push reads. */
void grjit_emit_add_site_for(GRJIT_EmitCommon * c, uint32_t offset,
    GRCORE_CodeSiteKind kind, GRCORE_PollIdentity identity, size_t live_index,
    uint32_t state, const GRJIT_Op * op);

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
    const GRJIT_CallableShape * shape, const GRJIT_LiveSites * live,
    const GRJIT_SiteRec * recs, size_t count, uint32_t frame_bytes,
    uint32_t code_bytes, const GRJIT_Allocator * allocator, GRJIT_MetaStorage * out);

/** Frees what ::grjit_metadata_build allocated. */
void grjit_metadata_free(GRJIT_MetaStorage * storage);

#ifdef __cplusplus
}
#endif

#endif /* GHOTI_IO_GRJIT_SRC_BACKEND_BACKEND_INTERNAL_H */
