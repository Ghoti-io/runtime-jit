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
 * @file natives.h
 * @stability free
 *
 * Registered, typed natives: the C functions compiled code calls with
 * `CALL_NATIVE` (AD-28, AD-17). Where ::GRJIT_OP_CALL is the engine's *trusted
 * helper* (leaf or GC point, no status, no signature, six integer arguments,
 * never re-enters), a native is registered in a ::GRJIT_NativeTable with a
 * descriptor, and a call is verified against it.
 *
 * **The C signature.** A native without ::GRJIT_NATIVE_STATUS is
 * `uint64_t fn(void * context, a0, a1, ...)`; one with it is
 * `GRJIT_NativeResult fn(void * context, a0, a1, ...)`. Every argument is one
 * 64-bit integer-class word. The context is the pointer the code was called
 * with and is the **implicit first argument**: natives allocate, poll and
 * re-enter through it, so it is not an operand of the IR. On x86-64 SysV the
 * pair comes back in `rax:rdx` with no hidden pointer, and on arm64 (AAPCS64) in
 * `x0:x1`. (Win64 returns a 16-byte struct through a hidden pointer; its emission,
 * which story 7b of the calls spec adds, adapts to that and the descriptor does not
 * change.) The context and the first words travel in registers, six on x86-64 and
 * eight on arm64, and the rest on the stack, in an area made by the call and popped
 * by the caller, so the stack pointer is 16-aligned at the call. A native with a
 * floating-point or variadic signature cannot be described, because the types a
 * descriptor can name are the three words.
 *
 * **What a native may do.** Every native call is a GC point: the walk finds the
 * calling compiled frame, and the compiled frames below it, precisely, from the
 * walk-start cell the call stores first. A reference a native receives, or
 * creates, and holds across a call that can collect is the native's to protect:
 * the arguments are raw words, not roots, and a moving collector does not
 * update them. It keeps the reference in a registered root or handle, or in its
 * own C frame under a `GRCORE_ACTIVATION_NATIVE` record whose segment is that
 * frame (conservative, so the object is pinned and not moved). **The library
 * opens no activation record for a native**, and pushes no guest frame: a native
 * called from compiled code is not a guest call. A native that re-enters guest
 * code opens a `GRCORE_ACTIVATION_REENTRY` record and, for compiled code, enters
 * through the entry adapter as the engine does. A native never lets a C++
 * exception or a `longjmp` cross a compiled frame (AD-28): it reports failure by
 * its status.
 *
 * **Status.** With ::GRJIT_NATIVE_STATUS a non-zero status means: the call is
 * complete, its result is valid, and compiled code leaves now, through the
 * function's chain deopt, with the frame state *after* the call. The `deopt`
 * hook is called once with `cause = ::GRJIT_CAUSE_NATIVE | status`, rebuilds
 * the chain, and every frame returns; the entry returns ::GRJIT_EXIT_DEOPT with
 * the cause in `out[0]`. The library names ::GRJIT_NATIVE_DEOPT and
 * ::GRJIT_NATIVE_UNWIND for the engine's hook to decide between a full rebuild
 * and a rebuild of the survivors of an unwind, and **never interprets a status
 * beyond "non-zero"**: any other value is the engine's. **A status is a 32-bit
 * value**, ::GRJIT_NativeResult says so with a `uint32_t` and the call tests exactly
 * those 32 bits (`edx`, or `w1` on arm64): the upper half of the register is padding that
 * a native returning a 32-bit status is free to leave as it likes, so it is never read,
 * and a status can neither be mistaken for zero nor alias another by a wide value. The
 * cause is `::GRJIT_CAUSE_NATIVE | status`, with nothing lost.
 */

#ifndef GHOTI_IO_GRJIT_NATIVES_H
#define GHOTI_IO_GRJIT_NATIVES_H

#include <ghoti.io/runtime-jit/macros.h>

#include <ghoti.io/runtime-jit/allocator.h>
#include <ghoti.io/runtime-jit/core.h>
#include <ghoti.io/runtime-jit/ir.h>
#include <ghoti.io/runtime-jit/limits.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief The result of a native with ::GRJIT_NATIVE_STATUS: the value, and the
 *  status (zero is ::GRJIT_NATIVE_OK). Two integer-class words, so on SysV they
 *  come back in `rax` and `rdx` and on arm64 in `x0` and `x1`; the second holds the
 *  32-bit status in its low half and padding above it, which the call ignores. */
typedef struct GRJIT_NativeResult {
  uint64_t value;    ///< The result; ignored for a native with no result.
  uint32_t status;   ///< Zero, or the reason compiled code leaves.
  uint32_t reserved; ///< Padding: never read, whatever it holds.
} GRJIT_NativeResult;

/** @brief The status that means "continue in compiled code". */
#define GRJIT_NATIVE_OK UINT32_C(0)
/** @brief Leave compiled code: the interpreter continues after the call (a
 *  pause the native left pending, or a nested run that left the state to the
 *  interpreter). The hook rebuilds the whole chain. */
#define GRJIT_NATIVE_DEOPT UINT32_C(1)
/** @brief A guest unwind is in progress: the hook rebuilds only the survivors
 *  (`grcore_compiled_rebuild`'s `keep_frames`) and pops the rest without
 *  converting them (AD-27). */
#define GRJIT_NATIVE_UNWIND UINT32_C(2)

/** @brief Bit 32. The `deopt` hook's `cause` for a native's status is
 *  `GRJIT_CAUSE_NATIVE | status`; a poll's cause is 32 bits, so the two never
 *  meet. */
#define GRJIT_CAUSE_NATIVE (UINT64_C(1) << 32)

/** @brief ::GRJIT_NativeDesc::flags: the native returns a ::GRJIT_NativeResult,
 *  and a call carries the frame state after it. */
#define GRJIT_NATIVE_STATUS UINT32_C(1)
/** @brief ::GRJIT_NativeDesc::flags: the native may re-enter guest code.
 *  Informational to the library (every native call is a GC point); binding on the
 *  engine, whose native then opens a `REENTRY` record. */
#define GRJIT_NATIVE_REENTERS UINT32_C(2)

/** @brief ::GRJIT_NativeDesc::result: the native has no result. */
#define GRJIT_NATIVE_NO_RESULT GRJIT_TYPE_COUNT

/**
 * @brief What the engine says about a native, once, when it registers it.
 *
 * Copied by ::grjit_native_table_add; the library keeps no pointer into it.
 */
typedef struct GRJIT_NativeDesc {
  uint64_t address;            ///< The function's absolute address; not zero. It
                               ///< must outlive every code that calls it.
  const GRJIT_Type * params;   ///< The parameter types, not counting the context:
                               ///< ::GRJIT_TYPE_I64, ::GRJIT_TYPE_REF or
                               ///< ::GRJIT_TYPE_PTR. May be NULL for none.
  size_t param_count;          ///< From zero to `GRJIT_Limits::max_native_arguments`.
  GRJIT_Type result;           ///< ::GRJIT_TYPE_I64, ::GRJIT_TYPE_REF,
                               ///< ::GRJIT_TYPE_PTR or ::GRJIT_NATIVE_NO_RESULT.
  uint32_t flags;              ///< ::GRJIT_NATIVE_STATUS, ::GRJIT_NATIVE_REENTERS.
  uint32_t stack_bytes;        ///< The most native stack the native itself uses
                               ///< before it calls anything that checks (its return
                               ///< address, its frame, what it calls that does not
                               ///< check): a call is made only if that much, and
                               ///< the stack arguments, lie above the stack limit.
                               ///< A re-entry checks for itself. At most
                               ///< `GRJIT_Limits::max_native_stack_bytes`.
                               ///< **The one formula, on every target:** the call is
                               ///< an exit before it unless `sp_before_call - S -
                               ///< stack_bytes` is not below the limit, `S` being the
                               ///< stack-argument area. On x86-64 the pushed return
                               ///< address is inside `stack_bytes`; on arm64 it is in
                               ///< `x30` and not on the stack, so a native there has
                               ///< eight bytes more than it declared. That is
                               ///< deliberate (the check is one `sub` and one compare
                               ///< on both), and the byte-exact tests name it.
} GRJIT_NativeDesc;

/** @brief The natives an engine has registered. Opaque. */
typedef struct GRJIT_NativeTable GRJIT_NativeTable;

/**
 * @brief Makes an empty table.
 *
 * @param limits The caps (`max_native_arguments`, `max_native_stack_bytes`); NULL
 *   for the defaults.
 * @param allocator For the table; NULL for the default. It must outlive it.
 * @param out_table Receives the table, only on success.
 * @return ::GRJIT_OK, ::GRJIT_ERR_INVALID for a NULL output, ::GRJIT_ERR_OOM.
 */
GRJIT_API GRJIT_Result grjit_native_table_create(const GRJIT_Limits * limits,
    const GRJIT_Allocator * allocator, GRJIT_NativeTable ** out_table);

/**
 * @brief Appends a descriptor, copied, and gives its id.
 *
 * The table is append-only and ids are stable: the first descriptor is 0, the next 1.
 * A refused descriptor changes nothing. Not thread-safe: a table is built by one
 * thread before it is shared, and read-only afterwards.
 *
 * @param table The table.
 * @param desc The descriptor. A NULL or zero `address`, a type that is not one
 *   of the three (or, for the result, none), a `params` that is NULL with a
 *   non-zero count, or a flag that is not defined, is ::GRJIT_ERR_INVALID; more
 *   parameters than `max_native_arguments`, or more `stack_bytes` than
 *   `max_native_stack_bytes`, is ::GRJIT_ERR_LIMIT.
 * @param out_id Receives the id, only on success; may be NULL.
 * @return ::GRJIT_OK, ::GRJIT_ERR_INVALID, ::GRJIT_ERR_LIMIT or ::GRJIT_ERR_OOM.
 */
GRJIT_API GRJIT_Result grjit_native_table_add(
    GRJIT_NativeTable * table, const GRJIT_NativeDesc * desc, uint32_t * out_id);

/** @brief How many descriptors the table holds. Zero for NULL. */
GRJIT_API size_t grjit_native_table_count(const GRJIT_NativeTable * table);

/**
 * @brief The descriptor of an id: the table's own copy, whose `params` the table
 *   owns, valid until the table is freed.
 *
 * @return The descriptor, or NULL for an id the table does not hold.
 */
GRJIT_API const GRJIT_NativeDesc * grjit_native_table_get(
    const GRJIT_NativeTable * table, uint32_t id);

/** @brief Frees a table. NULL is ignored. Code compiled from functions that
 *  named it does not need it (everything the emitter needs is copied into the
 *  code); the natives themselves must outlive that code. */
GRJIT_API void grjit_native_table_free(GRJIT_NativeTable * table);

#ifdef __cplusplus
}
#endif

#endif /* GHOTI_IO_GRJIT_NATIVES_H */
