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
 * @file ir.h
 * @stability free
 *
 * The low-level IR (AD-9, AD-14).
 *
 * A function is a list of basic blocks over *virtual registers*. The
 * registers are not SSA: a register may be assigned more than once, and the
 * passes that will want SSA build it when a benchmark names a function the
 * baseline cannot serve (AD-26). The IR is not Tang bytecode, not WebAssembly
 * and not JavaScript bytecode: a frontend lowers into it, and this library
 * never interprets it. A test-only evaluator checks the backend against it.
 *
 * Every value is one 64-bit word with a type that says what the collector
 * must do with it:
 *
 * - ::GRJIT_TYPE_I64: plain bits. Never in a stack map.
 * - ::GRJIT_TYPE_REF: an engine value word, which may be a reference. In a
 *   stack map it has slot kind `VALUE`.
 * - ::GRJIT_TYPE_PTR: a raw non-GC word (an address in the native heap, a
 *   buffer). Never in a stack map, except as a *derived pointer*, which is
 *   declared with ::grjit_builder_derived and recorded as a (slot, base slot,
 *   delta) triple.
 *
 * There is no floating point and there are no 32-bit or 8-bit values: memory
 * access has widths 8, 16, 32 and 64 bits, a narrow load zero-extends
 * (`LOAD`) or sign-extends (`LOAD_S`) into the 64-bit register, and a narrow
 * store writes the low bits.
 *
 * Arithmetic wraps modulo 2^64. A shift takes its count modulo 64, as x86-64
 * does, and the evaluator in the tests does the same. `CMP` produces an `I64`
 * that is 0 or 1; `LT`, `LE`, `GT` and `GE` are signed, `ULT`, `ULE`, `UGT`
 * and `UGE` unsigned.
 *
 * A `STORE` of a `REF` into a GC object is not expressible: stores into the
 * heap go through an engine-supplied helper `CALL` (the engine's `gc_store`),
 * and this library never emits a barrier (AD-11).
 *
 * A *frame state* is a poll identity `(function, bytecode offset)` plus, for
 * each of the function's `interp_slot_count` interpreter slots, a register, a
 * 64-bit constant or "dead". It is attached to each `POLL`, to each call that
 * can reach a GC point, and to each `GUARD`, and is what a deoptimization
 * rebuilds an interpreter frame from.
 */

#ifndef GHOTI_IO_GRJIT_IR_H
#define GHOTI_IO_GRJIT_IR_H

#include <ghoti.io/runtime-jit/macros.h>

#include <ghoti.io/runtime-jit/core.h>
#include <ghoti.io/runtime-jit/limits.h>

#include <ghoti.io/runtime-core/a/codemeta.h>
#include <ghoti.io/runtime-core/a/stack.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief What a value is, for the collector. */
typedef enum GRJIT_Type {
  GRJIT_TYPE_I64,  ///< Plain 64-bit bits.
  GRJIT_TYPE_REF,  ///< An engine value word that may be a reference.
  GRJIT_TYPE_PTR,  ///< A raw, non-GC word.
  GRJIT_TYPE_COUNT ///< Not a type; closes the enum.
} GRJIT_Type;

/** @brief A virtual register: an index into the function's register table. */
typedef uint32_t GRJIT_VReg;
/** @brief A basic block: an index into the function's block list. */
typedef uint32_t GRJIT_BlockId;

/** @brief "No register": a call with no result. */
#define GRJIT_NO_VREG UINT32_MAX
/** @brief "No frame state" in an operation that has none. */
#define GRJIT_NO_STATE UINT32_MAX

/** @brief What an operand is. */
typedef enum GRJIT_OperandKind {
  GRJIT_OPERAND_NONE, ///< Absent (a `RET` with no value).
  GRJIT_OPERAND_VREG, ///< A register.
  GRJIT_OPERAND_IMM   ///< A 64-bit immediate.
} GRJIT_OperandKind;

/** @brief An operand: a register or a 64-bit immediate. */
typedef struct GRJIT_Operand {
  GRJIT_OperandKind kind; ///< Which of the three.
  GRJIT_VReg vreg;        ///< The register, for ::GRJIT_OPERAND_VREG.
  int64_t imm;            ///< The immediate, for ::GRJIT_OPERAND_IMM.
} GRJIT_Operand;

/** @brief An absent operand. */
static inline GRJIT_Operand grjit_operand_none(void) {
  GRJIT_Operand o = {GRJIT_OPERAND_NONE, GRJIT_NO_VREG, 0};
  return o;
}

/** @brief A register operand. */
static inline GRJIT_Operand grjit_operand_vreg(GRJIT_VReg vreg) {
  GRJIT_Operand o = {GRJIT_OPERAND_VREG, vreg, 0};
  return o;
}

/** @brief An immediate operand. */
static inline GRJIT_Operand grjit_operand_imm(int64_t imm) {
  GRJIT_Operand o = {GRJIT_OPERAND_IMM, GRJIT_NO_VREG, imm};
  return o;
}

/** @brief The operations. Terminators are `BR`, `BR_IF` and `RET`. */
typedef enum GRJIT_OpKind {
  GRJIT_OP_CONST,  ///< `dst = a.imm`.
  GRJIT_OP_MOVE,   ///< `dst = a`.
  GRJIT_OP_ADD,    ///< `dst = a + b`.
  GRJIT_OP_SUB,    ///< `dst = a - b`.
  GRJIT_OP_MUL,    ///< `dst = a * b`, low 64 bits.
  GRJIT_OP_AND,    ///< `dst = a & b`.
  GRJIT_OP_OR,     ///< `dst = a | b`.
  GRJIT_OP_XOR,    ///< `dst = a ^ b`.
  GRJIT_OP_SHL,    ///< `dst = a << (b mod 64)`.
  GRJIT_OP_SHR,    ///< `dst = a >> (b mod 64)`, logical.
  GRJIT_OP_SAR,    ///< `dst = a >> (b mod 64)`, arithmetic.
  GRJIT_OP_NEG,    ///< `dst = -a`.
  GRJIT_OP_NOT,    ///< `dst = ~a`.
  GRJIT_OP_CMP,    ///< `dst = (a cmp b) ? 1 : 0`.
  GRJIT_OP_LOAD,   ///< `dst = zero-extended load of width bits at a + disp`.
  GRJIT_OP_LOAD_S, ///< The same, sign-extended.
  GRJIT_OP_STORE,  ///< Store the low width bits of `b` at `a + disp`.
  GRJIT_OP_CALL,   ///< Call the native helper at `address` with `args`.
  GRJIT_OP_POLL,   ///< A poll: a GC point with a frame state.
  GRJIT_OP_GUARD,  ///< If `a` is zero, deoptimize through the frame state.
  GRJIT_OP_BR,     ///< Jump to `target`.
  GRJIT_OP_BR_IF,  ///< Jump to `target` if `a` is non-zero, else `target_else`.
  GRJIT_OP_RET,    ///< Return `a`, or nothing.
  GRJIT_OP_COUNT   ///< Not an operation; closes the enum.
} GRJIT_OpKind;

/** @brief The conditions of `CMP`. */
typedef enum GRJIT_Cmp {
  GRJIT_CMP_EQ,  ///< Equal.
  GRJIT_CMP_NE,  ///< Not equal.
  GRJIT_CMP_LT,  ///< Signed less.
  GRJIT_CMP_LE,  ///< Signed less or equal.
  GRJIT_CMP_GT,  ///< Signed greater.
  GRJIT_CMP_GE,  ///< Signed greater or equal.
  GRJIT_CMP_ULT, ///< Unsigned less.
  GRJIT_CMP_ULE, ///< Unsigned less or equal.
  GRJIT_CMP_UGT, ///< Unsigned greater.
  GRJIT_CMP_UGE, ///< Unsigned greater or equal.
  GRJIT_CMP_COUNT ///< Not a condition; closes the enum.
} GRJIT_Cmp;

/** @brief What a helper call may do. */
typedef enum GRJIT_CallAttr {
  GRJIT_CALL_NO_GC,    ///< Cannot reach a GC point: not a site.
  GRJIT_CALL_GC_POINT, ///< Can reach one: a site, with a frame state.
  GRJIT_CALL_ATTR_COUNT ///< Not an attribute; closes the enum.
} GRJIT_CallAttr;

/** @brief What one slot of a frame state is. */
typedef enum GRJIT_FrameSlotKind {
  GRJIT_FRAME_SLOT_VREG,     ///< The value of a register.
  GRJIT_FRAME_SLOT_CONSTANT, ///< A 64-bit constant.
  GRJIT_FRAME_SLOT_DEAD,     ///< Not needed.
  GRJIT_FRAME_SLOT_COUNT     ///< Not a kind; closes the enum.
} GRJIT_FrameSlotKind;

/** @brief One interpreter slot of a frame state. */
typedef struct GRJIT_FrameSlot {
  GRJIT_FrameSlotKind kind; ///< Which of the three.
  GRJIT_VReg vreg;          ///< The register, for ::GRJIT_FRAME_SLOT_VREG.
  int64_t constant;         ///< The constant, for ::GRJIT_FRAME_SLOT_CONSTANT.
} GRJIT_FrameSlot;

/** @brief A register slot. */
static inline GRJIT_FrameSlot grjit_frame_slot_vreg(GRJIT_VReg vreg) {
  GRJIT_FrameSlot s = {GRJIT_FRAME_SLOT_VREG, vreg, 0};
  return s;
}

/** @brief A constant slot. */
static inline GRJIT_FrameSlot grjit_frame_slot_constant(int64_t constant) {
  GRJIT_FrameSlot s = {GRJIT_FRAME_SLOT_CONSTANT, GRJIT_NO_VREG, constant};
  return s;
}

/** @brief A dead slot. */
static inline GRJIT_FrameSlot grjit_frame_slot_dead(void) {
  GRJIT_FrameSlot s = {GRJIT_FRAME_SLOT_DEAD, GRJIT_NO_VREG, 0};
  return s;
}

/**
 * @brief A frame state: a poll identity and one entry per interpreter slot.
 *
 * `slot_count` should equal the function's `interp_slot_count`; the verifier
 * refuses a state that does not.
 */
typedef struct GRJIT_FrameState {
  GRCORE_PollIdentity identity;   ///< (function, bytecode offset), AD-18.
  size_t slot_count;              ///< Entries of `slots`.
  const GRJIT_FrameSlot * slots;  ///< One per interpreter slot.
} GRJIT_FrameState;

/**
 * @brief The engine's poll slow path: called with the context and the poll
 *   identity when the request word is non-zero.
 *
 * A non-zero return ends the compiled function with ::GRJIT_EXIT_REFUSED and
 * the value in `out[0]`: a pause or unwind verdict ends compiled code in this
 * version, and rebuilding frames is the engine's later work.
 */
typedef uint32_t (*GRJIT_PollHelper)(
    void * context, uint64_t function, uint64_t offset);

/**
 * @brief One operation. Which fields mean something depends on `kind`; the
 *   others are zero.
 *
 * Read it, do not build it: use the builder.
 */
typedef struct GRJIT_Op {
  GRJIT_OpKind kind;          ///< The operation.
  GRJIT_VReg dst;             ///< Result register, or ::GRJIT_NO_VREG.
  GRJIT_Operand a;            ///< First operand; the base of a memory access.
  GRJIT_Operand b;            ///< Second operand; the value of a `STORE`.
  int32_t disp;               ///< Displacement of a memory access.
  uint32_t width;             ///< Width in bits of a memory access: 8..64.
  GRJIT_Cmp cmp;              ///< The condition of a `CMP`.
  GRJIT_BlockId target;       ///< Branch target (the taken one of `BR_IF`).
  GRJIT_BlockId target_else;  ///< The other target of a `BR_IF`.
  uint64_t address;           ///< The helper's absolute address, for `CALL`.
  GRJIT_CallAttr attr;        ///< The call's attribute.
  GRCORE_CodeSiteKind site_kind; ///< The site kind of a `GC_POINT` call.
  size_t arg_count;           ///< Arguments of a call.
  const GRJIT_Operand * args; ///< The arguments.
  uint32_t state;             ///< Index of the frame state, or
                              ///< ::GRJIT_NO_STATE.
} GRJIT_Op;

/** @brief A function. Opaque; built by ::GRJIT_Builder. */
typedef struct GRJIT_Function GRJIT_Function;

/** @brief The name the function was created with. Owned by it. */
GRJIT_API const char * grjit_function_name(const GRJIT_Function * function);
/** @brief The number of parameters, which are the first registers. */
GRJIT_API size_t grjit_function_param_count(const GRJIT_Function * function);
/** @brief The number of interpreter slots a frame state describes. */
GRJIT_API size_t grjit_function_interp_slot_count(
    const GRJIT_Function * function);
/** @brief The number of registers. */
GRJIT_API size_t grjit_function_vreg_count(const GRJIT_Function * function);
/**
 * @brief The type of a register.
 * @return The type, or ::GRJIT_TYPE_COUNT for a register that does not exist.
 */
GRJIT_API GRJIT_Type grjit_function_vreg_type(
    const GRJIT_Function * function, GRJIT_VReg vreg);
/**
 * @brief Whether a register is a derived pointer, and from what.
 *
 * @param base Receives the base register; may be NULL.
 * @param delta Receives the byte delta; may be NULL.
 * @return True for a derived pointer.
 */
GRJIT_API bool grjit_function_vreg_derived(const GRJIT_Function * function,
    GRJIT_VReg vreg, GRJIT_VReg * base, int64_t * delta);
/** @brief The number of blocks. Block 0 is the entry. */
GRJIT_API size_t grjit_function_block_count(const GRJIT_Function * function);
/**
 * @brief The operations of a block.
 *
 * @param count Receives the number of operations.
 * @return The operations, or NULL for a block that does not exist.
 */
GRJIT_API const GRJIT_Op * grjit_function_block_ops(
    const GRJIT_Function * function, GRJIT_BlockId block, size_t * count);
/** @brief The number of frame states. */
GRJIT_API size_t grjit_function_frame_state_count(
    const GRJIT_Function * function);
/** @brief A frame state, or NULL when the index is out of range. */
GRJIT_API const GRJIT_FrameState * grjit_function_frame_state(
    const GRJIT_Function * function, uint32_t index);
/** @brief The poll helper the function declared; NULL for none. */
GRJIT_API GRJIT_PollHelper grjit_function_poll_helper(
    const GRJIT_Function * function);
/** @brief The number of operations in all blocks. */
GRJIT_API size_t grjit_function_op_count(const GRJIT_Function * function);

/**
 * @brief Frees a function made by ::grjit_builder_finish.
 *
 * @param function The function; NULL is ignored.
 */
GRJIT_API void grjit_function_destroy(GRJIT_Function * function);

#ifdef __cplusplus
}
#endif

#endif /* GHOTI_IO_GRJIT_IR_H */
