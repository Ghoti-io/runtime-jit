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
 * @file ir_internal.h
 *
 * The IR's private representation, shared by the builder, the verifier, the
 * printer and the backend. Never installed.
 */

#ifndef GHOTI_IO_GRJIT_SRC_IR_IR_INTERNAL_H
#define GHOTI_IO_GRJIT_SRC_IR_IR_INTERNAL_H

#include <ghoti.io/runtime-jit/macros.h>

#include <ghoti.io/runtime-jit/allocator.h>
#include <ghoti.io/runtime-jit/builder.h>
#include <ghoti.io/runtime-jit/ir.h>
#include <ghoti.io/runtime-jit/limits.h>
#include <ghoti.io/runtime-jit/natives.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/** The most a backend passes to a helper in registers. */
#define GRJIT_BACKEND_MAX_ARGS 6u
/** The most a builder will ever hold for one call. */
#define GRJIT_BUILDER_MAX_ARGS 16u

typedef struct GRJIT_VRegInfo {
  GRJIT_Type type;
  bool derived;
  GRJIT_VReg base;
  int64_t delta;
} GRJIT_VRegInfo;

typedef struct GRJIT_BlockInfo {
  GRJIT_Op * ops;
  size_t count;
  size_t capacity;
} GRJIT_BlockInfo;

struct GRJIT_Function {
  const GRJIT_Allocator * allocator;
  char * name;
  size_t interp_slots;
  size_t param_count;
  GRJIT_PollHelper poll_helper;
  bool callable;            ///< Has an internal entry and the hooks below.
  uint64_t token;           ///< The engine's token for this function, which a call
                            ///< through a code pointer must name (AD-28).
  GRJIT_CallHooks hooks;    ///< Valid when `callable`.
  const GRJIT_NativeTable * natives; ///< What `CALL_NATIVE` names; borrowed.
  GRJIT_VRegInfo * vregs;
  size_t vreg_count;
  size_t vreg_capacity;
  GRJIT_BlockInfo * blocks;
  size_t block_count;
  size_t block_capacity;
  GRJIT_FrameState * states;
  size_t state_count;
  size_t state_capacity;
  size_t op_count;
};

struct GRJIT_Builder {
  GRJIT_Function * function;
  GRJIT_Limits limits;
  bool has_current;
  GRJIT_BlockId current;
};

/** Fills `out` with `in` where non-zero and the defaults elsewhere. */
void grjit_limits_resolve(const GRJIT_Limits * in, GRJIT_Limits * out);

/** The allocator to use for a possibly-NULL one. */
const GRJIT_Allocator * grjit_allocator_or_default(
    const GRJIT_Allocator * allocator);

/** Whether `t` is a type a register can have: the value 3 is the native-result
 *  sentinel and is no type, and ::GRJIT_TYPE_COUNT and above are none. */
static inline bool grjit_type_valid(GRJIT_Type t) {
  return t == GRJIT_TYPE_I64 || t == GRJIT_TYPE_REF || t == GRJIT_TYPE_PTR ||
         t == GRJIT_TYPE_I32 || t == GRJIT_TYPE_F32 || t == GRJIT_TYPE_F64;
}

/** Whether `t` is a float type. */
static inline bool grjit_type_is_float(GRJIT_Type t) {
  return t == GRJIT_TYPE_F32 || t == GRJIT_TYPE_F64;
}

/** Whether `t` is a type whose value is narrower than its word and kept zero-extended
 *  in it (`I32` and `F32`). */
static inline bool grjit_type_is_narrow(GRJIT_Type t) {
  return t == GRJIT_TYPE_I32 || t == GRJIT_TYPE_F32;
}

/** Whether an operand is a typed float immediate (a call argument). */
static inline bool grjit_operand_is_float_imm(const GRJIT_Operand * o) {
  return o->kind == GRJIT_OPERAND_F32_IMM || o->kind == GRJIT_OPERAND_F64_IMM;
}

/** Whether the function has anything the arm64 and Win64 emitters do not yet
 *  emit: a register of type `I32`, `F32` or `F64`, a typed float immediate, or one of
 *  the integer and float operations added for numeric-operations.md (story 5 of
 *  spec-runtime-float replaces the refusal). */
bool grjit_function_needs_numeric_ext(const GRJIT_Function * function);

/** Whether an operation is one of the integer or float operations added after `CALL_NATIVE`. */
static inline bool grjit_op_is_numeric_ext(GRJIT_OpKind kind) {
  return kind >= GRJIT_OP_CLZ && kind < GRJIT_OP_COUNT;
}

/** Whether an operation ends a block. */
bool grjit_op_is_terminator(GRJIT_OpKind kind);

/** Whether an operation is a site (carries a frame state). */
bool grjit_op_has_state(const GRJIT_Op * op);

/** The registers an operation reads, in a fixed order, through `visit`. A
 *  frame state's registers are read by the operation that carries it. */
typedef void (*GRJIT_UseVisitor)(void * user, GRJIT_VReg vreg);
void grjit_op_visit_uses(const GRJIT_Function * function, const GRJIT_Op * op,
    GRJIT_UseVisitor visit, void * user);

/** The register an operation assigns, or ::GRJIT_NO_VREG. */
GRJIT_VReg grjit_op_def(const GRJIT_Op * op);

#endif /* GHOTI_IO_GRJIT_SRC_IR_IR_INTERNAL_H */
