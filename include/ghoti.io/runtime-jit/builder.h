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
 * @file builder.h
 * @stability free
 *
 * The IR builder.
 *
 * The builder records; the verifier judges (verify.h). It checks only what it
 * cannot record without: the limits of ::GRJIT_Limits, and that there is a
 * current block. A register that does not exist, a branch to a missing block
 * or a frame state of the wrong length is recorded as given and refused by
 * ::grjit_function_verify, so that the verifier can be shown each of them.
 *
 * Every call returns a ::GRJIT_Result and, on failure, changes nothing (an
 * out-of-memory failure leaves the builder as it was). Operations are
 * appended to the *current* block, which ::grjit_builder_set_block chooses; a
 * new block does not become current.
 */

#ifndef GHOTI_IO_GRJIT_BUILDER_H
#define GHOTI_IO_GRJIT_BUILDER_H

#include <ghoti.io/runtime-jit/macros.h>

#include <ghoti.io/runtime-jit/allocator.h>
#include <ghoti.io/runtime-jit/core.h>
#include <ghoti.io/runtime-jit/ir.h>
#include <ghoti.io/runtime-jit/limits.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief A function under construction. Opaque. */
typedef struct GRJIT_Builder GRJIT_Builder;

/**
 * @brief Starts a function.
 *
 * @param name A name for the printer and for diagnostics; copied. May be
 *   NULL, which names it "function".
 * @param interp_slot_count The number of interpreter slots its frame states
 *   describe.
 * @param limits The caps; NULL for the defaults.
 * @param allocator The allocator for the function; NULL for the default. It
 *   must outlive the function.
 * @param out_builder Receives the builder, only on success.
 * @return ::GRJIT_OK, ::GRJIT_ERR_INVALID for a NULL output, ::GRJIT_ERR_OOM.
 */
GRJIT_API GRJIT_Result grjit_builder_create(const char * name,
    size_t interp_slot_count, const GRJIT_Limits * limits,
    const GRJIT_Allocator * allocator, GRJIT_Builder ** out_builder);

/**
 * @brief Frees a builder and, if it was never finished, its function.
 *
 * @param builder The builder; NULL is ignored.
 */
GRJIT_API void grjit_builder_destroy(GRJIT_Builder * builder);

/**
 * @brief Ends construction and hands the function to the caller.
 *
 * On success the builder is destroyed and must not be used again; the
 * function is the caller's, freed with ::grjit_function_destroy. On failure
 * nothing changes.
 *
 * @param builder The builder.
 * @param out_function Receives the function, only on success.
 * @return ::GRJIT_OK or ::GRJIT_ERR_INVALID.
 */
GRJIT_API GRJIT_Result grjit_builder_finish(
    GRJIT_Builder * builder, GRJIT_Function ** out_function);

/**
 * @brief Declares the next parameter, which arrives as a 64-bit word.
 *
 * Parameters are the first registers, so this is refused with
 * ::GRJIT_ERR_INVALID once any other register exists.
 *
 * @param type ::GRJIT_TYPE_I64, ::GRJIT_TYPE_REF or ::GRJIT_TYPE_PTR.
 * @param out_vreg Receives the register, only on success.
 * @return ::GRJIT_OK, ::GRJIT_ERR_INVALID, ::GRJIT_ERR_LIMIT or
 *   ::GRJIT_ERR_OOM.
 */
GRJIT_API GRJIT_Result grjit_builder_param(
    GRJIT_Builder * builder, GRJIT_Type type, GRJIT_VReg * out_vreg);

/** @brief Declares a register. Same results as ::grjit_builder_param. */
GRJIT_API GRJIT_Result grjit_builder_vreg(
    GRJIT_Builder * builder, GRJIT_Type type, GRJIT_VReg * out_vreg);

/**
 * @brief Declares `vreg` a derived pointer: `base + delta`.
 *
 * `vreg` should be a ::GRJIT_TYPE_PTR and `base` a ::GRJIT_TYPE_REF; the
 * verifier refuses otherwise, and refuses a site at which `vreg` is live and
 * `base` is not.
 *
 * @return ::GRJIT_OK, or ::GRJIT_ERR_INVALID for a register that does not
 *   exist.
 */
GRJIT_API GRJIT_Result grjit_builder_derived(GRJIT_Builder * builder,
    GRJIT_VReg vreg, GRJIT_VReg base, int64_t delta);

/**
 * @brief Adds a block. The first one is the entry. It does not become
 *   current.
 *
 * @return ::GRJIT_OK, ::GRJIT_ERR_LIMIT or ::GRJIT_ERR_OOM.
 */
GRJIT_API GRJIT_Result grjit_builder_block(
    GRJIT_Builder * builder, GRJIT_BlockId * out_block);

/** @brief Makes `block` current. ::GRJIT_ERR_INVALID if it does not exist. */
GRJIT_API GRJIT_Result grjit_builder_set_block(
    GRJIT_Builder * builder, GRJIT_BlockId block);

/** @brief Declares the poll slow-path helper that `POLL` calls. */
GRJIT_API GRJIT_Result grjit_builder_set_poll_helper(
    GRJIT_Builder * builder, GRJIT_PollHelper helper);

/** @brief `dst = imm`. */
GRJIT_API GRJIT_Result grjit_builder_const(
    GRJIT_Builder * builder, GRJIT_VReg dst, int64_t imm);

/** @brief `dst = src`. */
GRJIT_API GRJIT_Result grjit_builder_move(
    GRJIT_Builder * builder, GRJIT_VReg dst, GRJIT_Operand src);

/**
 * @brief `dst = src`, reinterpreted: copies the word between two registers of
 *   any of the types `I64`, `REF` and `PTR`.
 *
 * `MOVE` keeps refusing a change of type; this is the one operation that
 * changes it. It is not a conversion: the 64 bits are the same. A bitcast from
 * a `REF` to an `I64` never makes the computed value a reference, and the
 * destination's type is what decides whether a stack map names it.
 */
GRJIT_API GRJIT_Result grjit_builder_bitcast(
    GRJIT_Builder * builder, GRJIT_VReg dst, GRJIT_VReg src);

/**
 * @brief `dst = a op b` for ADD, SUB, MUL, AND, OR, XOR, SHL, SHR and SAR.
 *
 * @return ::GRJIT_ERR_INVALID for another kind.
 */
GRJIT_API GRJIT_Result grjit_builder_binary(GRJIT_Builder * builder,
    GRJIT_OpKind kind, GRJIT_VReg dst, GRJIT_Operand a, GRJIT_Operand b);

/**
 * @brief `dst = op a` for NEG and NOT.
 *
 * @return ::GRJIT_ERR_INVALID for another kind.
 */
GRJIT_API GRJIT_Result grjit_builder_unary(
    GRJIT_Builder * builder, GRJIT_OpKind kind, GRJIT_VReg dst, GRJIT_Operand a);

/** @brief `dst = (a cmp b) ? 1 : 0`. */
GRJIT_API GRJIT_Result grjit_builder_cmp(GRJIT_Builder * builder,
    GRJIT_Cmp cmp, GRJIT_VReg dst, GRJIT_Operand a, GRJIT_Operand b);

/**
 * @brief `dst = load of width_bits at base + disp`, zero- or sign-extended.
 *
 * @param width_bits 8, 16, 32 or 64 (the verifier refuses others).
 * @param sign_extend True for `LOAD_S`.
 */
GRJIT_API GRJIT_Result grjit_builder_load(GRJIT_Builder * builder,
    GRJIT_VReg dst, GRJIT_VReg base, int32_t disp, uint32_t width_bits,
    bool sign_extend);

/** @brief Stores the low `width_bits` of `value` at `base + disp`. */
GRJIT_API GRJIT_Result grjit_builder_store(GRJIT_Builder * builder,
    GRJIT_VReg base, int32_t disp, uint32_t width_bits, GRJIT_Operand value);

/**
 * @brief Calls the native helper at `address` with integer or pointer
 *   arguments.
 *
 * A `GRJIT_CALL_NO_GC` call carries no frame state (`state_slots` is
 * ignored). A `GRJIT_CALL_GC_POINT` call is a site of kind `site_kind` and
 * carries `identity` and the `state_count` slots in `state_slots`, copied.
 *
 * @param dst The result register, or ::GRJIT_NO_VREG for none.
 * @param address The helper's absolute address.
 * @param attr Whether the helper can reach a GC point.
 * @param site_kind One of the five GC-point kinds, for a GC-point call.
 * @param args The arguments, copied.
 * @param arg_count How many; more than the limit is ::GRJIT_ERR_LIMIT.
 * @return ::GRJIT_OK, ::GRJIT_ERR_INVALID, ::GRJIT_ERR_LIMIT or
 *   ::GRJIT_ERR_OOM.
 */
GRJIT_API GRJIT_Result grjit_builder_call(GRJIT_Builder * builder,
    GRJIT_VReg dst, uint64_t address, GRJIT_CallAttr attr,
    GRCORE_CodeSiteKind site_kind, const GRJIT_Operand * args,
    size_t arg_count, GRCORE_PollIdentity identity,
    const GRJIT_FrameSlot * state_slots, size_t state_count);

/** @brief A poll, with its frame state (copied). */
GRJIT_API GRJIT_Result grjit_builder_poll(GRJIT_Builder * builder,
    GRCORE_PollIdentity identity, const GRJIT_FrameSlot * state_slots,
    size_t state_count);

/**
 * @brief A guard: if `cond` is zero, leave through a deopt exit described by
 *   the frame state (copied).
 */
GRJIT_API GRJIT_Result grjit_builder_guard(GRJIT_Builder * builder,
    GRJIT_Operand cond, GRCORE_PollIdentity identity,
    const GRJIT_FrameSlot * state_slots, size_t state_count);

/** @brief Jump to `target`. Terminates the block. */
GRJIT_API GRJIT_Result grjit_builder_br(
    GRJIT_Builder * builder, GRJIT_BlockId target);

/** @brief Jump to `then_block` if `cond` is non-zero, else `else_block`. */
GRJIT_API GRJIT_Result grjit_builder_br_if(GRJIT_Builder * builder,
    GRJIT_Operand cond, GRJIT_BlockId then_block, GRJIT_BlockId else_block);

/** @brief Return `value`, or nothing for ::grjit_operand_none. */
GRJIT_API GRJIT_Result grjit_builder_ret(
    GRJIT_Builder * builder, GRJIT_Operand value);

#ifdef __cplusplus
}
#endif

#endif /* GHOTI_IO_GRJIT_BUILDER_H */
