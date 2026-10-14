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
 * @file
 *
 * The IR verifier. It reads the function, allocates only the dataflow sets it
 * needs, and writes its reason only when it refuses.
 */

#include <ghoti.io/runtime-jit/macros.h>

#include <ghoti.io/runtime-jit/verify.h>

#include "ir_internal.h"

#include "../backend/liveness_internal.h"

#include <inttypes.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

typedef struct Verify {
  const GRJIT_Function * f;
  GRJIT_Limits limits;
  char * reason;
  size_t reason_size;
} Verify;

static GRJIT_Result refuse(
    Verify * v, GRJIT_Result code, const char * format, ...) {
  if (v->reason != NULL && v->reason_size != 0) {
    va_list args;
    va_start(args, format);
    vsnprintf(v->reason, v->reason_size, format, args);
    va_end(args);
  }
  return code;
}

static bool int_like(GRJIT_Type t) {
  return t == GRJIT_TYPE_I64 || t == GRJIT_TYPE_PTR;
}

static bool vreg_ok(const Verify * v, GRJIT_VReg r) {
  return r < v->f->vreg_count;
}

static GRJIT_Type type_of(const Verify * v, GRJIT_VReg r) {
  return vreg_ok(v, r) ? v->f->vregs[r].type : GRJIT_TYPE_COUNT;
}

static GRJIT_Type return_operand_type(const Verify * v, const GRJIT_Operand * o) {
  switch (o->kind) {
    case GRJIT_OPERAND_VREG: return type_of(v, o->vreg);
    case GRJIT_OPERAND_IMM: return GRJIT_TYPE_I64;
    case GRJIT_OPERAND_F32_IMM: return GRJIT_TYPE_F32;
    case GRJIT_OPERAND_F64_IMM: return GRJIT_TYPE_F64;
    default: return GRJIT_TYPE_COUNT;
  }
}

static bool width_ok(uint32_t w) {
  return w == 8 || w == 16 || w == 32 || w == 64;
}

/* The width of the integer operations (ir.h): the destination's type decides. */
static bool is_i32(GRJIT_Type t) {
  return t == GRJIT_TYPE_I32;
}

/* An immediate read as an `I32` is its low 32 bits, and must say them in one of the
 * two spellings of a 32-bit pattern, signed or unsigned. */
static bool imm32_ok(int64_t imm) {
  return imm >= INT32_MIN && imm <= (int64_t)UINT32_MAX;
}

/* ---- Floats (spec-runtime-float, story 3) ------------------------------------------------- */

/* An `F32` immediate is a 32-bit pattern; unlike an `I32` one it has no second spelling. */
static bool f32imm_ok(int64_t imm) {
  return imm >= 0 && imm <= (int64_t)UINT32_MAX;
}

/* An operand of a float operation of type `want`: a register of exactly that type, or an
 * immediate that is the operand's bit pattern (a typed float immediate of the same type is
 * accepted too, where an immediate is). */
static GRJIT_Result check_float_operand(Verify * v, const char * what,
    const GRJIT_Operand * o, GRJIT_Type want, GRJIT_BlockId block, size_t index) {
  if (o->kind == GRJIT_OPERAND_IMM || o->kind == GRJIT_OPERAND_F32_IMM ||
      o->kind == GRJIT_OPERAND_F64_IMM) {
    if ((o->kind == GRJIT_OPERAND_F32_IMM && want != GRJIT_TYPE_F32) ||
        (o->kind == GRJIT_OPERAND_F64_IMM && want != GRJIT_TYPE_F64)) {
      return refuse(v, GRJIT_ERR_INVALID,
          "block b%u op %zu: %s is a float immediate of the other width", block, index, what);
    }
    if (want == GRJIT_TYPE_F32 && !f32imm_ok(o->imm)) {
      return refuse(v, GRJIT_ERR_INVALID,
          "block b%u op %zu: %s immediate does not fit the 32 bits of an f32", block, index,
          what);
    }
    return GRJIT_OK;
  }
  if (o->kind != GRJIT_OPERAND_VREG) {
    return refuse(v, GRJIT_ERR_INVALID,
        "block b%u op %zu: %s operand is missing", block, index, what);
  }
  if (!vreg_ok(v, o->vreg)) {
    return refuse(v, GRJIT_ERR_INVALID,
        "block b%u op %zu: %s names v%u, which the function does not have",
        block, index, what, o->vreg);
  }
  if (type_of(v, o->vreg) != want) {
    return refuse(v, GRJIT_ERR_INVALID,
        "block b%u op %zu: %s v%u has the wrong type for this operation (a float operation "
        "takes float operands of its own width)",
        block, index, what, o->vreg);
  }
  return GRJIT_OK;
}

/* The source of a conversion: a register of exactly one of the types in `a` or `b`. An
 * immediate has no width to say which conversion it is the source of, so it is refused. */
static GRJIT_Result check_source_reg(Verify * v, const GRJIT_Operand * o, GRJIT_Type a,
    GRJIT_Type b, const char * what, GRJIT_BlockId block, size_t index) {
  if (o->kind != GRJIT_OPERAND_VREG) {
    return refuse(v, GRJIT_ERR_INVALID,
        "block b%u op %zu: the source of %s is a register (an immediate has no width to say "
        "which conversion it is the source of; make it with a const first)",
        block, index, what);
  }
  if (!vreg_ok(v, o->vreg)) {
    return refuse(v, GRJIT_ERR_INVALID,
        "block b%u op %zu: the source of %s names v%u, which the function does not have",
        block, index, what, o->vreg);
  }
  GRJIT_Type t = type_of(v, o->vreg);
  if (t != a && t != b) {
    return refuse(v, GRJIT_ERR_INVALID,
        "block b%u op %zu: the source of %s, v%u, has the wrong type", block, index, what,
        o->vreg);
  }
  return GRJIT_OK;
}

/* An argument of a call: a register of any type, a plain immediate (integer class), or a typed
 * float immediate whose pattern fits its width. */
static GRJIT_Result check_call_argument(Verify * v, const GRJIT_Operand * o, size_t i,
    GRJIT_BlockId block, size_t index) {
  if (o->kind == GRJIT_OPERAND_VREG) {
    if (!vreg_ok(v, o->vreg)) {
      return refuse(v, GRJIT_ERR_INVALID,
          "block b%u op %zu: argument %zu names v%u, which the function does not have",
          block, index, i, o->vreg);
    }
  } else if (o->kind == GRJIT_OPERAND_F32_IMM) {
    if (!f32imm_ok(o->imm)) {
      return refuse(v, GRJIT_ERR_INVALID,
          "block b%u op %zu: argument %zu is an f32 immediate that does not fit 32 bits", block,
          index, i);
    }
  } else if (o->kind != GRJIT_OPERAND_IMM && o->kind != GRJIT_OPERAND_F64_IMM) {
    return refuse(v, GRJIT_ERR_INVALID,
        "block b%u op %zu: argument %zu is missing", block, index, i);
  }
  return GRJIT_OK;
}

/* An operand of an integer operation of width `want` (`I32` or `I64`): a register of
 * that type (a `PTR` also counts as 64 bits, as it always has), or an immediate. */
static GRJIT_Result check_int_operand(Verify * v, const char * what,
    const GRJIT_Operand * o, GRJIT_Type want, GRJIT_BlockId block, size_t index) {
  if (o->kind == GRJIT_OPERAND_IMM) {
    if (want == GRJIT_TYPE_I32 && !imm32_ok(o->imm)) {
      return refuse(v, GRJIT_ERR_INVALID,
          "block b%u op %zu: %s immediate does not fit 32 bits", block, index, what);
    }
    return GRJIT_OK;
  }
  if (o->kind != GRJIT_OPERAND_VREG) {
    return refuse(v, GRJIT_ERR_INVALID,
        "block b%u op %zu: %s operand is missing", block, index, what);
  }
  if (!vreg_ok(v, o->vreg)) {
    return refuse(v, GRJIT_ERR_INVALID,
        "block b%u op %zu: %s names v%u, which the function does not have",
        block, index, what, o->vreg);
  }
  GRJIT_Type t = type_of(v, o->vreg);
  if (want == GRJIT_TYPE_I32 ? t != GRJIT_TYPE_I32 : !int_like(t)) {
    return refuse(v, GRJIT_ERR_INVALID,
        "block b%u op %zu: %s v%u has the wrong type for this operation (an i32 "
        "operation takes i32 operands, an i64 one i64 operands)",
        block, index, what, o->vreg);
  }
  return GRJIT_OK;
}

/* The destination of an integer operation: a register that exists, of type `I32` or
 * `I64`. A `PTR` destination keeps working for the operations that always had it. */
static GRJIT_Result check_int_dst(Verify * v, const GRJIT_Op * op, bool allow_ptr,
    GRJIT_BlockId block, size_t index) {
  if (!vreg_ok(v, op->dst)) {
    return refuse(v, GRJIT_ERR_INVALID,
        "block b%u op %zu: destination v%u does not exist", block, index, op->dst);
  }
  GRJIT_Type t = type_of(v, op->dst);
  if (t != GRJIT_TYPE_I32 && t != GRJIT_TYPE_I64 && !(allow_ptr && t == GRJIT_TYPE_PTR)) {
    return refuse(v, GRJIT_ERR_INVALID,
        "block b%u op %zu: destination v%u has the wrong type", block, index, op->dst);
  }
  return GRJIT_OK;
}

/* An operand that must be a register or an immediate of a type `ok` accepts.
 * An absent operand is never acceptable here. */
static GRJIT_Result check_operand(Verify * v, const char * what,
    const GRJIT_Operand * o, bool allow_ref, GRJIT_BlockId block, size_t index) {
  if (o->kind == GRJIT_OPERAND_IMM) {
    return GRJIT_OK;
  }
  if (o->kind != GRJIT_OPERAND_VREG) {
    return refuse(v, GRJIT_ERR_INVALID,
        "block b%u op %zu: %s operand is missing", block, index, what);
  }
  if (!vreg_ok(v, o->vreg)) {
    return refuse(v, GRJIT_ERR_INVALID,
        "block b%u op %zu: %s names v%u, which the function does not have",
        block, index, what, o->vreg);
  }
  GRJIT_Type t = type_of(v, o->vreg);
  if (!int_like(t) && !(allow_ref && t == GRJIT_TYPE_REF)) {
    return refuse(v, GRJIT_ERR_INVALID,
        "block b%u op %zu: %s v%u has the wrong type for this operation",
        block, index, what, o->vreg);
  }
  return GRJIT_OK;
}

/* A condition (a guard's, a branch's): any integer register, either width. */
static GRJIT_Result check_condition(Verify * v, const GRJIT_Operand * o,
    GRJIT_BlockId block, size_t index) {
  if (o->kind == GRJIT_OPERAND_VREG && vreg_ok(v, o->vreg) &&
      type_of(v, o->vreg) == GRJIT_TYPE_I32) {
    return GRJIT_OK;
  }
  return check_operand(v, "condition", o, false, block, index);
}

static GRJIT_Result check_dst(Verify * v, const GRJIT_Op * op,
    bool need_int, GRJIT_BlockId block, size_t index) {
  if (!vreg_ok(v, op->dst)) {
    return refuse(v, GRJIT_ERR_INVALID,
        "block b%u op %zu: destination v%u does not exist", block, index,
        op->dst);
  }
  if (need_int && !int_like(type_of(v, op->dst))) {
    return refuse(v, GRJIT_ERR_INVALID,
        "block b%u op %zu: destination v%u has the wrong type", block, index,
        op->dst);
  }
  return GRJIT_OK;
}

static GRJIT_Result check_state_at(Verify * v, uint32_t which,
    GRJIT_BlockId block, size_t index) {
  if (which == GRJIT_NO_STATE || which >= v->f->state_count) {
    return refuse(v, GRJIT_ERR_INVALID,
        "block b%u op %zu: a site without a frame state", block, index);
  }
  const GRJIT_FrameState * s = &v->f->states[which];
  if (s->slot_count > v->limits.max_frame_state_slots) {
    return refuse(v, GRJIT_ERR_LIMIT,
        "block b%u op %zu: frame state has %zu slots, the limit is %zu", block,
        index, s->slot_count, v->limits.max_frame_state_slots);
  }
  if (s->slot_count != v->f->interp_slots) {
    return refuse(v, GRJIT_ERR_INVALID,
        "block b%u op %zu: frame state has %zu slots, the function has %zu "
        "interpreter slots",
        block, index, s->slot_count, v->f->interp_slots);
  }
  for (size_t i = 0; i < s->slot_count; i++) {
    const GRJIT_FrameSlot * slot = &s->slots[i];
    if ((unsigned)slot->kind >= (unsigned)GRJIT_FRAME_SLOT_COUNT) {
      return refuse(v, GRJIT_ERR_INVALID,
          "block b%u op %zu: frame state slot %zu has an unknown kind", block,
          index, i);
    }
    if (slot->kind == GRJIT_FRAME_SLOT_VREG && !vreg_ok(v, slot->vreg)) {
      return refuse(v, GRJIT_ERR_INVALID,
          "block b%u op %zu: frame state slot %zu names v%u, which the "
          "function does not have",
          block, index, i, slot->vreg);
    }
  }
  return GRJIT_OK;
}

static GRJIT_Result check_state(
    Verify * v, const GRJIT_Op * op, GRJIT_BlockId block, size_t index) {
  return check_state_at(v, op->state, block, index);
}

static bool gc_site_kind(GRCORE_CodeSiteKind k) {
  return k == GRCORE_SITE_GC_POINT_POLL || k == GRCORE_SITE_GC_POINT_ALLOC_SLOW ||
         k == GRCORE_SITE_GC_POINT_CALL || k == GRCORE_SITE_GC_POINT_FRAME_PUSH ||
         k == GRCORE_SITE_GC_POINT_NESTED_ENTRY;
}

static GRJIT_Result check_op(
    Verify * v, const GRJIT_Op * op, GRJIT_BlockId block, size_t index) {
  GRJIT_Result r;
  switch (op->kind) {
    case GRJIT_OP_INSTANCE:
      if ((r = check_dst(v, op, false, block, index)) != GRJIT_OK) {
        return r;
      }
      if (type_of(v, op->dst) != GRJIT_TYPE_PTR) {
        return refuse(v, GRJIT_ERR_INVALID,
            "block b%u op %zu: instance destination must be a pointer", block, index);
      }
      return GRJIT_OK;
    case GRJIT_OP_CONST:
      if (op->a.kind != GRJIT_OPERAND_IMM) {
        return refuse(v, GRJIT_ERR_INVALID,
            "block b%u op %zu: const needs an immediate", block, index);
      }
      if ((r = check_dst(v, op, false, block, index)) != GRJIT_OK) {
        return r;
      }
      if (is_i32(type_of(v, op->dst)) && !imm32_ok(op->a.imm)) {
        return refuse(v, GRJIT_ERR_INVALID,
            "block b%u op %zu: const %" PRId64 " does not fit an i32", block, index,
            op->a.imm);
      }
      if (type_of(v, op->dst) == GRJIT_TYPE_F32 && !f32imm_ok(op->a.imm)) {
        return refuse(v, GRJIT_ERR_INVALID,
            "block b%u op %zu: const %" PRId64 " is not the 32-bit pattern of an f32", block,
            index, op->a.imm);
      }
      return GRJIT_OK;
    case GRJIT_OP_MOVE:
      if (vreg_ok(v, op->dst) &&
          (grjit_type_is_float(type_of(v, op->dst)) ||
              (op->a.kind == GRJIT_OPERAND_VREG && vreg_ok(v, op->a.vreg) &&
                  grjit_type_is_float(type_of(v, op->a.vreg))))) {
        /* A float moves only into a float of its own type. */
        if (!grjit_type_is_float(type_of(v, op->dst))) {
          return refuse(v, GRJIT_ERR_INVALID,
              "block b%u op %zu: move from float v%u to v%u changes the type", block, index,
              op->a.vreg, op->dst);
        }
        return check_float_operand(v, "source", &op->a, type_of(v, op->dst), block, index);
      }
      if (vreg_ok(v, op->dst) &&
          (is_i32(type_of(v, op->dst)) ||
              (op->a.kind == GRJIT_OPERAND_VREG && is_i32(type_of(v, op->a.vreg))))) {
        /* An i32 moves only into an i32, and only an i32 or a 32-bit immediate. */
        if (!is_i32(type_of(v, op->dst))) {
          return refuse(v, GRJIT_ERR_INVALID,
              "block b%u op %zu: move from v%u to v%u changes the type", block, index,
              op->a.vreg, op->dst);
        }
        return check_int_operand(v, "source", &op->a, GRJIT_TYPE_I32, block, index);
      }
      if ((r = check_operand(v, "source", &op->a, true, block, index)) !=
              GRJIT_OK ||
          (r = check_dst(v, op, false, block, index)) != GRJIT_OK) {
        return r;
      }
      if (op->a.kind == GRJIT_OPERAND_VREG &&
          type_of(v, op->a.vreg) != type_of(v, op->dst)) {
        return refuse(v, GRJIT_ERR_INVALID,
            "block b%u op %zu: move from v%u to v%u changes the type", block,
            index, op->a.vreg, op->dst);
      }
      return GRJIT_OK;
    case GRJIT_OP_BITCAST:
      // A reinterpretation between registers of any of the three types: the
      // source is a register (an immediate has no type to reinterpret) and
      // the destination's type decides what the word is from here on.
      if (op->a.kind != GRJIT_OPERAND_VREG) {
        return refuse(v, GRJIT_ERR_INVALID,
            "block b%u op %zu: bitcast needs a register source", block, index);
      }
      if ((r = check_operand(v, "source", &op->a, true, block, index)) !=
              GRJIT_OK ||
          (r = check_dst(v, op, false, block, index)) != GRJIT_OK) {
        return r;
      }
      if (is_i32(type_of(v, op->dst))) {
        return refuse(v, GRJIT_ERR_INVALID,
            "block b%u op %zu: a bitcast cannot make an i32 (it would not be "
            "zero-extended); use wrap",
            block, index);
      }
      if (grjit_type_is_float(type_of(v, op->dst))) {
        return refuse(v, GRJIT_ERR_INVALID,
            "block b%u op %zu: a bitcast cannot make a float; use reinterpret", block, index);
      }
      return GRJIT_OK;
    case GRJIT_OP_ADD:
    case GRJIT_OP_SUB:
    case GRJIT_OP_MUL:
    case GRJIT_OP_AND:
    case GRJIT_OP_OR:
    case GRJIT_OP_XOR:
    case GRJIT_OP_SHL:
    case GRJIT_OP_SHR:
    case GRJIT_OP_SAR:
      if (vreg_ok(v, op->dst) && is_i32(type_of(v, op->dst))) {
        if ((r = check_int_operand(v, "left", &op->a, GRJIT_TYPE_I32, block, index)) !=
            GRJIT_OK) {
          return r;
        }
        return check_int_operand(v, "right", &op->b, GRJIT_TYPE_I32, block, index);
      }
      if ((r = check_operand(v, "left", &op->a, false, block, index)) !=
              GRJIT_OK ||
          (r = check_operand(v, "right", &op->b, false, block, index)) !=
              GRJIT_OK) {
        return r;
      }
      return check_dst(v, op, true, block, index);
    case GRJIT_OP_NEG:
    case GRJIT_OP_NOT:
      if (vreg_ok(v, op->dst) && is_i32(type_of(v, op->dst))) {
        return check_int_operand(v, "operand", &op->a, GRJIT_TYPE_I32, block, index);
      }
      if ((r = check_operand(v, "operand", &op->a, false, block, index)) !=
          GRJIT_OK) {
        return r;
      }
      return check_dst(v, op, true, block, index);
    case GRJIT_OP_CLZ:
    case GRJIT_OP_CTZ:
    case GRJIT_OP_POPCNT:
    case GRJIT_OP_EXT8_S:
    case GRJIT_OP_EXT16_S:
    case GRJIT_OP_EXT32_S:
      /* The width is the destination's; the operand has it too. `i64.extend32_s`
       * has no 32-bit form (a 32-bit value is already one word of 32 bits). */
      if ((r = check_int_dst(v, op, false, block, index)) != GRJIT_OK) {
        return r;
      }
      if (op->kind == GRJIT_OP_EXT32_S && is_i32(type_of(v, op->dst))) {
        return refuse(v, GRJIT_ERR_INVALID,
            "block b%u op %zu: ext32_s is an i64 operation", block, index);
      }
      return check_int_operand(v, "operand", &op->a, type_of(v, op->dst), block, index);
    case GRJIT_OP_ROTL:
    case GRJIT_OP_ROTR:
    case GRJIT_OP_DIV_S:
    case GRJIT_OP_DIV_U:
    case GRJIT_OP_REM_S:
    case GRJIT_OP_REM_U:
      if ((r = check_int_dst(v, op, false, block, index)) != GRJIT_OK ||
          (r = check_int_operand(v, "left", &op->a, type_of(v, op->dst), block, index)) !=
              GRJIT_OK) {
        return r;
      }
      /* The guards of a division are judged by check_partial, after the types. */
      return check_int_operand(v, "right", &op->b, type_of(v, op->dst), block, index);
    case GRJIT_OP_WRAP:
      if ((r = check_int_dst(v, op, false, block, index)) != GRJIT_OK) {
        return r;
      }
      if (!is_i32(type_of(v, op->dst))) {
        return refuse(v, GRJIT_ERR_INVALID,
            "block b%u op %zu: wrap makes an i32", block, index);
      }
      return check_int_operand(v, "operand", &op->a, GRJIT_TYPE_I64, block, index);
    case GRJIT_OP_EXTEND_S:
    case GRJIT_OP_EXTEND_U:
      if ((r = check_int_dst(v, op, false, block, index)) != GRJIT_OK) {
        return r;
      }
      if (type_of(v, op->dst) != GRJIT_TYPE_I64) {
        return refuse(v, GRJIT_ERR_INVALID,
            "block b%u op %zu: extend makes an i64", block, index);
      }
      return check_int_operand(v, "operand", &op->a, GRJIT_TYPE_I32, block, index);
    case GRJIT_OP_CMP: {
      if ((unsigned)op->cmp >= (unsigned)GRJIT_CMP_COUNT) {
        return refuse(v, GRJIT_ERR_INVALID,
            "block b%u op %zu: unknown comparison", block, index);
      }
      bool ref_ok = op->cmp == GRJIT_CMP_EQ || op->cmp == GRJIT_CMP_NE;
      const bool float_cmp =
          (op->a.kind == GRJIT_OPERAND_VREG && grjit_type_is_float(type_of(v, op->a.vreg))) ||
          (op->b.kind == GRJIT_OPERAND_VREG && grjit_type_is_float(type_of(v, op->b.vreg)));
      if (float_cmp) {
        /* A float comparison: both register operands are floats of one type (the other
         * operand may be an immediate, which is that type's bit pattern), the ordered
         * conditions only, and the result 0 or 1 in an i32 or an i64. */
        if (op->cmp > GRJIT_CMP_GE) {
          return refuse(v, GRJIT_ERR_INVALID,
              "block b%u op %zu: the unsigned conditions do not exist on floats", block, index);
        }
        const GRJIT_Operand * reg = op->a.kind == GRJIT_OPERAND_VREG &&
                                            grjit_type_is_float(type_of(v, op->a.vreg))
                                        ? &op->a
                                        : &op->b;
        const GRJIT_Type want = type_of(v, reg->vreg);
        if ((r = check_float_operand(v, "left", &op->a, want, block, index)) != GRJIT_OK ||
            (r = check_float_operand(v, "right", &op->b, want, block, index)) != GRJIT_OK) {
          return r;
        }
        return check_int_dst(v, op, false, block, index);
      }
      const bool wide32 =
          (op->a.kind == GRJIT_OPERAND_VREG && is_i32(type_of(v, op->a.vreg))) ||
          (op->b.kind == GRJIT_OPERAND_VREG && is_i32(type_of(v, op->b.vreg)));
      if (wide32) {
        /* A 32-bit comparison: both register operands are i32, the result 0 or 1 in
         * an i32 or an i64. */
        if ((r = check_int_operand(v, "left", &op->a, GRJIT_TYPE_I32, block, index)) !=
                GRJIT_OK ||
            (r = check_int_operand(v, "right", &op->b, GRJIT_TYPE_I32, block, index)) !=
                GRJIT_OK) {
          return r;
        }
        return check_int_dst(v, op, false, block, index);
      }
      if ((r = check_operand(v, "left", &op->a, ref_ok, block, index)) !=
              GRJIT_OK ||
          (r = check_operand(v, "right", &op->b, ref_ok, block, index)) !=
              GRJIT_OK ||
          (r = check_dst(v, op, false, block, index)) != GRJIT_OK) {
        return r;
      }
      if (type_of(v, op->dst) != GRJIT_TYPE_I64 && !is_i32(type_of(v, op->dst))) {
        return refuse(v, GRJIT_ERR_INVALID,
            "block b%u op %zu: a comparison produces an i64 or an i32", block, index);
      }
      if (op->a.kind == GRJIT_OPERAND_VREG &&
          op->b.kind == GRJIT_OPERAND_VREG &&
          type_of(v, op->a.vreg) != type_of(v, op->b.vreg)) {
        return refuse(v, GRJIT_ERR_INVALID,
            "block b%u op %zu: comparison of v%u and v%u, which are not of "
            "like types",
            block, index, op->a.vreg, op->b.vreg);
      }
      return GRJIT_OK;
    }
    case GRJIT_OP_FADD:
    case GRJIT_OP_FSUB:
    case GRJIT_OP_FMUL:
    case GRJIT_OP_FDIV:
    case GRJIT_OP_FMIN:
    case GRJIT_OP_FMAX:
    case GRJIT_OP_FCOPYSIGN:
    case GRJIT_OP_FNEG:
    case GRJIT_OP_FABS:
    case GRJIT_OP_FSQRT:
    case GRJIT_OP_FCEIL:
    case GRJIT_OP_FFLOOR:
    case GRJIT_OP_FTRUNC:
    case GRJIT_OP_FNEAREST: {
      if (!vreg_ok(v, op->dst)) {
        return refuse(v, GRJIT_ERR_INVALID,
            "block b%u op %zu: destination v%u does not exist", block, index, op->dst);
      }
      const GRJIT_Type t = type_of(v, op->dst);
      if (!grjit_type_is_float(t)) {
        return refuse(v, GRJIT_ERR_INVALID,
            "block b%u op %zu: a float operation writes an f32 or an f64", block, index);
      }
      if ((r = check_float_operand(v, "left", &op->a, t, block, index)) != GRJIT_OK) {
        return r;
      }
      if (op->kind <= GRJIT_OP_FCOPYSIGN) {
        return check_float_operand(v, "right", &op->b, t, block, index);
      }
      return GRJIT_OK;
    }
    case GRJIT_OP_CONVERT_S:
    case GRJIT_OP_CONVERT_U:
      if (!vreg_ok(v, op->dst) || !grjit_type_is_float(type_of(v, op->dst))) {
        return refuse(v, GRJIT_ERR_INVALID,
            "block b%u op %zu: convert makes an f32 or an f64", block, index);
      }
      return check_source_reg(
          v, &op->a, GRJIT_TYPE_I32, GRJIT_TYPE_I64, "a convert", block, index);
    case GRJIT_OP_TRUNC_S:
    case GRJIT_OP_TRUNC_U:
    case GRJIT_OP_TRUNC_SAT_S:
    case GRJIT_OP_TRUNC_SAT_U:
      if (!vreg_ok(v, op->dst) ||
          (type_of(v, op->dst) != GRJIT_TYPE_I32 && type_of(v, op->dst) != GRJIT_TYPE_I64)) {
        return refuse(v, GRJIT_ERR_INVALID,
            "block b%u op %zu: trunc makes an i32 or an i64", block, index);
      }
      return check_source_reg(
          v, &op->a, GRJIT_TYPE_F32, GRJIT_TYPE_F64, "a trunc", block, index);
    case GRJIT_OP_DEMOTE:
    case GRJIT_OP_PROMOTE: {
      const GRJIT_Type from = op->kind == GRJIT_OP_DEMOTE ? GRJIT_TYPE_F64 : GRJIT_TYPE_F32;
      const GRJIT_Type to = op->kind == GRJIT_OP_DEMOTE ? GRJIT_TYPE_F32 : GRJIT_TYPE_F64;
      if (!vreg_ok(v, op->dst) || type_of(v, op->dst) != to) {
        return refuse(v, GRJIT_ERR_INVALID,
            "block b%u op %zu: %s makes an %s", block, index,
            op->kind == GRJIT_OP_DEMOTE ? "demote" : "promote",
            to == GRJIT_TYPE_F32 ? "f32" : "f64");
      }
      return check_source_reg(v, &op->a, from, from,
          op->kind == GRJIT_OP_DEMOTE ? "a demote" : "a promote", block, index);
    }
    case GRJIT_OP_REINTERPRET: {
      if (!vreg_ok(v, op->dst)) {
        return refuse(v, GRJIT_ERR_INVALID,
            "block b%u op %zu: destination v%u does not exist", block, index, op->dst);
      }
      GRJIT_Type from;
      switch (type_of(v, op->dst)) {
        case GRJIT_TYPE_I32: from = GRJIT_TYPE_F32; break;
        case GRJIT_TYPE_F32: from = GRJIT_TYPE_I32; break;
        case GRJIT_TYPE_I64: from = GRJIT_TYPE_F64; break;
        case GRJIT_TYPE_F64: from = GRJIT_TYPE_I64; break;
        default:
          return refuse(v, GRJIT_ERR_INVALID,
              "block b%u op %zu: reinterpret is between i32 and f32, or i64 and f64", block,
              index);
      }
      return check_source_reg(v, &op->a, from, from, "a reinterpret", block, index);
    }
    case GRJIT_OP_LOAD:
    case GRJIT_OP_LOAD_S:
    case GRJIT_OP_STORE: {
      if (op->index.kind != GRJIT_OPERAND_NONE) {
        if (op->index.kind != GRJIT_OPERAND_VREG || !vreg_ok(v, op->index.vreg) ||
            (type_of(v, op->index.vreg) != GRJIT_TYPE_I32 &&
                type_of(v, op->index.vreg) != GRJIT_TYPE_I64) ||
            (op->scale != 1 && op->scale != 2 && op->scale != 4 && op->scale != 8)) {
          return refuse(v, GRJIT_ERR_INVALID,
              "block b%u op %zu: indexed memory needs an integer index and scale 1, 2, 4 or 8",
              block, index);
        }
      }
      if (op->a.kind != GRJIT_OPERAND_VREG || !vreg_ok(v, op->a.vreg) ||
          (type_of(v, op->a.vreg) != GRJIT_TYPE_PTR &&
              type_of(v, op->a.vreg) != GRJIT_TYPE_REF)) {
        return refuse(v, GRJIT_ERR_INVALID,
            "block b%u op %zu: the base of a memory access must be a ptr or "
            "ref register",
            block, index);
      }
      if (!width_ok(op->width)) {
        return refuse(v, GRJIT_ERR_INVALID,
            "block b%u op %zu: width %u is not 8, 16, 32 or 64", block, index,
            op->width);
      }
      if (op->kind == GRJIT_OP_STORE) {
        if (op->b.kind == GRJIT_OPERAND_VREG && vreg_ok(v, op->b.vreg) &&
            grjit_type_is_float(type_of(v, op->b.vreg))) {
          /* A float is stored as its bits: exactly its own width. */
          if (op->width != (type_of(v, op->b.vreg) == GRJIT_TYPE_F32 ? 32u : 64u)) {
            return refuse(v, GRJIT_ERR_INVALID,
                "block b%u op %zu: a float is stored at its own width (32 for an f32, 64 for "
                "an f64)",
                block, index);
          }
          return GRJIT_OK;
        }
        if (op->b.kind == GRJIT_OPERAND_VREG && vreg_ok(v, op->b.vreg) &&
            type_of(v, op->b.vreg) == GRJIT_TYPE_REF) {
          return refuse(v, GRJIT_ERR_INVALID,
              "block b%u op %zu: a store of a reference is not expressible; "
              "stores into the heap go through the engine's helper",
              block, index);
        }
        if (op->b.kind == GRJIT_OPERAND_VREG && vreg_ok(v, op->b.vreg) &&
            is_i32(type_of(v, op->b.vreg))) {
          return GRJIT_OK; /* the low width bits of an i32 */
        }
        return check_operand(v, "stored value", &op->b, false, block, index);
      }
      if ((r = check_dst(v, op, false, block, index)) != GRJIT_OK) {
        return r;
      }
      if (grjit_type_is_float(type_of(v, op->dst)) &&
          (op->kind == GRJIT_OP_LOAD_S ||
              op->width != (type_of(v, op->dst) == GRJIT_TYPE_F32 ? 32u : 64u))) {
        return refuse(v, GRJIT_ERR_INVALID,
            "block b%u op %zu: a float register takes a zero-extending load of exactly its "
            "own width (32 bits for an f32, 64 for an f64)",
            block, index);
      }
      if (is_i32(type_of(v, op->dst)) && (op->kind == GRJIT_OP_LOAD_S || op->width == 64)) {
        return refuse(v, GRJIT_ERR_INVALID,
            "block b%u op %zu: an i32 register takes a zero-extending load of at "
            "most 32 bits (a sign-extending or 64-bit load would leave its upper "
            "half non-zero)",
            block, index);
      }
      return GRJIT_OK;
    }
    case GRJIT_OP_CALL: {
      size_t max_args = v->limits.max_call_arguments < GRJIT_BACKEND_MAX_ARGS
                            ? v->limits.max_call_arguments
                            : GRJIT_BACKEND_MAX_ARGS;
      if (op->arg_count > max_args) {
        return refuse(v, GRJIT_ERR_LIMIT,
            "block b%u op %zu: call has %zu arguments, at most %zu are "
            "allowed",
            block, index, op->arg_count, max_args);
      }
      if ((unsigned)op->attr >= (unsigned)GRJIT_CALL_ATTR_COUNT ||
          op->address == 0) {
        return refuse(v, GRJIT_ERR_INVALID,
            "block b%u op %zu: call has no address or an unknown attribute",
            block, index);
      }
      for (size_t i = 0; i < op->arg_count; i++) {
        if ((r = check_call_argument(v, &op->args[i], i, block, index)) != GRJIT_OK) {
          return r;
        }
      }
      if (op->dst != GRJIT_NO_VREG &&
          (r = check_dst(v, op, false, block, index)) != GRJIT_OK) {
        return r;
      }
      if (op->attr == GRJIT_CALL_GC_POINT) {
        if (!gc_site_kind(op->site_kind)) {
          return refuse(v, GRJIT_ERR_INVALID,
              "block b%u op %zu: a GC-point call needs one of the five "
              "GC-point site kinds",
              block, index);
        }
        return check_state(v, op, block, index);
      }
      return GRJIT_OK;
    }
    case GRJIT_OP_CALL_SLOT:
    case GRJIT_OP_CALL_PTR:
    case GRJIT_OP_TAIL_CALL_SLOT:
    case GRJIT_OP_TAIL_CALL_PTR: {
      const bool slot = op->kind == GRJIT_OP_CALL_SLOT || op->kind == GRJIT_OP_TAIL_CALL_SLOT;
      const bool tail = op->kind == GRJIT_OP_TAIL_CALL_SLOT || op->kind == GRJIT_OP_TAIL_CALL_PTR;
      if (op->environment.kind != GRJIT_OPERAND_NONE &&
          op->environment.kind != GRJIT_OPERAND_IMM &&
          !(op->environment.kind == GRJIT_OPERAND_VREG &&
              vreg_ok(v, op->environment.vreg) &&
              type_of(v, op->environment.vreg) == GRJIT_TYPE_PTR)) {
        return refuse(v, GRJIT_ERR_INVALID,
            "block b%u op %zu: callee environment must be a ptr register or immediate",
            block, index);
      }
      if (op->landing_target != GRJIT_NO_BLOCK &&
          (tail || op->landing_target >= v->f->block_count)) {
        return refuse(v, GRJIT_ERR_INVALID,
            "block b%u op %zu: catch landing target is invalid", block, index);
      }
      if (op->landing_target != GRJIT_NO_BLOCK && v->f->hooks.catch_exception == NULL) {
        return refuse(v, GRJIT_ERR_INVALID,
            "block b%u op %zu: a catch edge needs the exception matching hook", block, index);
      }
      const char * what = tail ? "a tail call" : "a call";
      const GRJIT_CallHooks * h = &v->f->hooks;
      if (!v->f->callable) {
        return refuse(v, GRJIT_ERR_INVALID,
            "block b%u op %zu: %s to another compiled function in a "
            "function that is not callable",
            block, index, what);
      }
      if (tail) {
        if (op->callee_result_count > GRJIT_BUILDER_MAX_ARGS) {
          return refuse(v, GRJIT_ERR_LIMIT,
              "block b%u op %zu: a tail call declares too many results",
              block, index);
        }
        if (h->tail == NULL || (slot && h->compile == NULL)) {
          return refuse(v, GRJIT_ERR_INVALID,
              "block b%u op %zu: a tail call needs the tail%s hook of a "
              "callable function",
              block, index, slot ? " and compile" : "");
        }
        if (op->dst != GRJIT_NO_VREG || op->exit_state != GRJIT_NO_STATE) {
          return refuse(v, GRJIT_ERR_INVALID,
              "block b%u op %zu: a tail call has no result and one frame state",
              block, index);
        }
      } else if (h->push == NULL || h->pop == NULL || (slot && h->compile == NULL)) {
        return refuse(v, GRJIT_ERR_INVALID,
            "block b%u op %zu: this call needs the push%s and pop hooks of a "
            "callable function",
            block, index, slot ? ", compile" : "");
      }
      size_t max_args = v->limits.max_guest_call_arguments < GRJIT_BUILDER_MAX_ARGS
                            ? v->limits.max_guest_call_arguments
                            : GRJIT_BUILDER_MAX_ARGS;
      if (op->arg_count > max_args) {
        return refuse(v, GRJIT_ERR_LIMIT,
            "block b%u op %zu: %s has %zu arguments, at most %zu are "
            "allowed",
            block, index, what, op->arg_count, max_args);
      }
      if (slot) {
        const bool dynamic_slot = op->address == 0 &&
            (op->a.kind == GRJIT_OPERAND_IMM ||
                (op->a.kind == GRJIT_OPERAND_VREG && vreg_ok(v, op->a.vreg) &&
                    type_of(v, op->a.vreg) == GRJIT_TYPE_PTR));
        if ((!dynamic_slot && op->address == 0) ||
            (op->address != 0 && op->a.kind != GRJIT_OPERAND_NONE)) {
          return refuse(v, GRJIT_ERR_INVALID,
              "block b%u op %zu: a call through a slot needs the address of "
              "the slot's entry word or a ptr operand",
              block, index);
        }
      } else if (op->address != 0) {
        return refuse(v, GRJIT_ERR_INVALID,
            "block b%u op %zu: a call through a code pointer has no slot",
            block, index);
      } else {
        if (op->a.kind == GRJIT_OPERAND_VREG) {
          if (!vreg_ok(v, op->a.vreg) || type_of(v, op->a.vreg) != GRJIT_TYPE_PTR) {
            return refuse(v, GRJIT_ERR_INVALID,
                "block b%u op %zu: the code pointer must be a ptr register or "
                "an immediate",
                block, index);
          }
        } else if (op->a.kind != GRJIT_OPERAND_IMM || op->a.imm == 0) {
          return refuse(v, GRJIT_ERR_INVALID,
              "block b%u op %zu: the code pointer is missing or null", block,
              index);
        }
      }
      for (size_t i = 0; i < op->arg_count; i++) {
        if ((r = check_call_argument(v, &op->args[i], i, block, index)) != GRJIT_OK) {
          return r;
        }
      }
      if (op->dst != GRJIT_NO_VREG &&
          (r = check_dst(v, op, false, block, index)) != GRJIT_OK) {
        return r;
      }
      if (!tail) {
        if (op->result_count > GRJIT_BUILDER_MAX_ARGS ||
            (op->result_count != 0 && op->results == NULL))
          return refuse(v, GRJIT_ERR_INVALID, "block b%u op %zu: invalid call result list", block, index);
        for (size_t i = 0; i < op->result_count; i++) {
          if (!vreg_ok(v, op->results[i]))
            return refuse(v, GRJIT_ERR_INVALID, "block b%u op %zu: result %zu names an invalid register", block, index, i);
          for (size_t j = 0; j < i; j++) if (op->results[j] == op->results[i])
            return refuse(v, GRJIT_ERR_INVALID, "block b%u op %zu: duplicate call result register", block, index);
        }
      }
      if ((r = check_state_at(v, op->state, block, index)) != GRJIT_OK) {
        return r;
      }
      return tail ? GRJIT_OK : check_state_at(v, op->exit_state, block, index);
    }
    case GRJIT_OP_CALL_NATIVE: {
      /* A native is registered and typed (natives.h): the call is judged against
       * its descriptor, so a call the descriptor does not describe is never made. */
      if (!v->f->callable) {
        return refuse(v, GRJIT_ERR_INVALID,
            "block b%u op %zu: a native call in a function that is not callable "
            "(a native call can leave compiled code through the deopt hook)",
            block, index);
      }
      const GRJIT_NativeDesc * d = grjit_native_table_get(v->f->natives, op->native);
      if (d == NULL) {
        return refuse(v, GRJIT_ERR_INVALID,
            "block b%u op %zu: native #%u is not in the function's native table",
            block, index, op->native);
      }
      if (op->arg_count != d->param_count) {
        return refuse(v, GRJIT_ERR_INVALID,
            "block b%u op %zu: native #%u takes %zu arguments, the call has %zu",
            block, index, op->native, d->param_count, op->arg_count);
      }
      if (op->arg_count > v->limits.max_native_arguments) {
        return refuse(v, GRJIT_ERR_LIMIT,
            "block b%u op %zu: native #%u call has %zu arguments, at most %zu are "
            "allowed",
            block, index, op->native, op->arg_count, v->limits.max_native_arguments);
      }
      for (size_t i = 0; i < op->arg_count; i++) {
        const GRJIT_Operand * o = &op->args[i];
        if (o->kind == GRJIT_OPERAND_VREG) {
          if (!vreg_ok(v, o->vreg)) {
            return refuse(v, GRJIT_ERR_INVALID,
                "block b%u op %zu: argument %zu names v%u, which the "
                "function does not have",
                block, index, i, o->vreg);
          }
          if (type_of(v, o->vreg) != d->params[i]) {
            return refuse(v, GRJIT_ERR_INVALID,
                "block b%u op %zu: argument %zu is v%u, whose type is not the "
                "type native #%u takes there",
                block, index, i, o->vreg, op->native);
          }
        } else if (o->kind != GRJIT_OPERAND_IMM) {
          return refuse(v, GRJIT_ERR_INVALID,
              "block b%u op %zu: argument %zu is missing", block, index, i);
        }
      }
      if (op->dst != GRJIT_NO_VREG) {
        if (d->result == GRJIT_NATIVE_NO_RESULT) {
          return refuse(v, GRJIT_ERR_INVALID,
              "block b%u op %zu: native #%u has no result, and the call names "
              "a destination",
              block, index, op->native);
        }
        if ((r = check_dst(v, op, false, block, index)) != GRJIT_OK) {
          return r;
        }
        if (type_of(v, op->dst) != d->result) {
          return refuse(v, GRJIT_ERR_INVALID,
              "block b%u op %zu: destination v%u is not of the type native #%u "
              "returns",
              block, index, op->dst, op->native);
        }
      }
      if ((r = check_state_at(v, op->state, block, index)) != GRJIT_OK) {
        return r;
      }
      if ((d->flags & GRJIT_NATIVE_STATUS) != 0) {
        if (op->exit_state == GRJIT_NO_STATE) {
          return refuse(v, GRJIT_ERR_INVALID,
              "block b%u op %zu: native #%u returns a status, and the call has "
              "no state after it for the exit a non-zero status takes",
              block, index, op->native);
        }
        return check_state_at(v, op->exit_state, block, index);
      }
      if (op->exit_state != GRJIT_NO_STATE) {
        return refuse(v, GRJIT_ERR_INVALID,
            "block b%u op %zu: native #%u returns no status, and the call has a "
            "state after it",
            block, index, op->native);
      }
      return GRJIT_OK;
    }
    case GRJIT_OP_POLL:
      if (v->f->poll_helper == NULL) {
        return refuse(v, GRJIT_ERR_INVALID,
            "block b%u op %zu: a poll in a function with no poll helper",
            block, index);
      }
      return check_state(v, op, block, index);
    case GRJIT_OP_GUARD:
      if ((r = check_condition(v, &op->a, block, index)) != GRJIT_OK) {
        return r;
      }
      return check_state(v, op, block, index);
    case GRJIT_OP_BR:
      if (op->target >= v->f->block_count) {
        return refuse(v, GRJIT_ERR_INVALID,
            "block b%u op %zu: branch to b%u, which does not exist", block,
            index, op->target);
      }
      return GRJIT_OK;
    case GRJIT_OP_BR_IF:
      if ((r = check_condition(v, &op->a, block, index)) != GRJIT_OK) {
        return r;
      }
      if (op->target >= v->f->block_count ||
          op->target_else >= v->f->block_count) {
        return refuse(v, GRJIT_ERR_INVALID,
            "block b%u op %zu: branch to a block that does not exist", block,
            index);
      }
      return GRJIT_OK;
    case GRJIT_OP_BR_TABLE:
      if ((op->a.kind != GRJIT_OPERAND_VREG || !vreg_ok(v, op->a.vreg) ||
              (type_of(v, op->a.vreg) != GRJIT_TYPE_I32 &&
                  type_of(v, op->a.vreg) != GRJIT_TYPE_I64)) &&
          op->a.kind != GRJIT_OPERAND_IMM) {
        return refuse(v, GRJIT_ERR_INVALID,
            "block b%u op %zu: br_table index must be an integer", block, index);
      }
      if (op->default_target >= v->f->block_count) {
        return refuse(v, GRJIT_ERR_INVALID,
            "block b%u op %zu: br_table default target does not exist", block, index);
      }
      for (size_t i = 0; i < op->target_count; i++) {
        if (op->targets == NULL || op->targets[i] >= v->f->block_count) {
          return refuse(v, GRJIT_ERR_INVALID,
              "block b%u op %zu: br_table target %zu does not exist", block, index, i);
        }
      }
      return GRJIT_OK;
    case GRJIT_OP_TRAP:
      if ((unsigned)op->trap_kind == 0 ||
          (unsigned)op->trap_kind >= (unsigned)GRJIT_TRAP_KIND_COUNT) {
        return refuse(v, GRJIT_ERR_INVALID,
            "block b%u op %zu: trap kind is invalid", block, index);
      }
      return GRJIT_OK;
    case GRJIT_OP_RETHROW:
      if (!v->f->callable) {
        return refuse(v, GRJIT_ERR_INVALID,
            "block b%u op %zu: rethrow requires a callable function", block, index);
      }
      return GRJIT_OK;
    case GRJIT_OP_RET:
      if (op->result_count > GRJIT_BUILDER_MAX_ARGS ||
          (op->result_count != 0 && op->returns == NULL))
        return refuse(v, GRJIT_ERR_INVALID, "block b%u op %zu: invalid return list", block, index);
      for (size_t i = 0; i < op->result_count; i++) {
        if (op->returns[i].kind == GRJIT_OPERAND_VREG && !vreg_ok(v, op->returns[i].vreg))
          return refuse(v, GRJIT_ERR_INVALID, "block b%u op %zu: return %zu names an invalid register", block, index, i);
        if (op->returns[i].kind == GRJIT_OPERAND_F32_IMM && !f32imm_ok(op->returns[i].imm))
          return refuse(v, GRJIT_ERR_INVALID,
              "block b%u op %zu: return %zu is an f32 immediate that does not fit 32 bits",
              block, index, i);
        if (op->returns[i].kind != GRJIT_OPERAND_VREG && op->returns[i].kind != GRJIT_OPERAND_IMM &&
            op->returns[i].kind != GRJIT_OPERAND_F32_IMM && op->returns[i].kind != GRJIT_OPERAND_F64_IMM)
          return refuse(v, GRJIT_ERR_INVALID, "block b%u op %zu: return %zu is not a value", block, index, i);
      }
      return GRJIT_OK;
    case GRJIT_OP_COUNT:
      break;
  }
  return refuse(v, GRJIT_ERR_INVALID, "block b%u op %zu: unknown operation",
      block, index);
}

static GRJIT_Result check_structure(Verify * v) {
  const GRJIT_Function * f = v->f;
  if (f->block_count == 0) {
    return refuse(v, GRJIT_ERR_INVALID, "the function has no blocks");
  }
  if (f->param_count > f->vreg_count) {
    return refuse(v, GRJIT_ERR_INVALID, "more parameters than registers");
  }
  if (f->callable) {
    if (f->hooks.deopt == NULL) {
      return refuse(v, GRJIT_ERR_INVALID,
          "a callable function needs the deopt hook: a guard, a poll or a "
          "call in it can leave through a chain");
    }
    if (f->param_count > v->limits.max_guest_call_arguments ||
        f->param_count > GRJIT_BUILDER_MAX_ARGS) {
      return refuse(v, GRJIT_ERR_LIMIT,
          "a callable function has %zu parameters, at most %zu are allowed",
          f->param_count, v->limits.max_guest_call_arguments);
    }
  }
  for (size_t r = 0; r < f->vreg_count; r++) {
    const GRJIT_VRegInfo * info = &f->vregs[r];
    if (!grjit_type_valid(info->type)) {
      return refuse(v, GRJIT_ERR_INVALID, "v%zu has an unknown type", r);
    }
    if (!info->derived) {
      continue;
    }
    if (info->type != GRJIT_TYPE_PTR) {
      return refuse(v, GRJIT_ERR_INVALID,
          "v%zu is declared derived but is not a ptr", r);
    }
    if (info->base >= f->vreg_count ||
        f->vregs[info->base].type != GRJIT_TYPE_REF) {
      return refuse(v, GRJIT_ERR_INVALID,
          "the base of derived pointer v%zu is not a ref register", r);
    }
  }
  for (size_t b = 0; b < f->block_count; b++) {
    const GRJIT_BlockInfo * blk = &f->blocks[b];
    if (blk->count == 0) {
      return refuse(
          v, GRJIT_ERR_INVALID, "block b%zu has no terminator", b);
    }
    for (size_t i = 0; i < blk->count; i++) {
      const GRJIT_Op * op = &blk->ops[i];
      bool last = i + 1 == blk->count;
      if (grjit_op_is_terminator(op->kind) && !last) {
        return refuse(v, GRJIT_ERR_INVALID,
            "block b%zu has a terminator before its end (op %zu)", b, i);
      }
      if (last && !grjit_op_is_terminator(op->kind)) {
        return refuse(
            v, GRJIT_ERR_INVALID, "block b%zu does not end in a terminator", b);
      }
      GRJIT_Result r = check_op(v, op, (GRJIT_BlockId)b, i);
      if (r != GRJIT_OK) {
        return r;
      }
    }
  }
  size_t return_count = SIZE_MAX;
  const GRJIT_Op * first_return = NULL;
  for (size_t b = 0; b < f->block_count; b++) {
    const GRJIT_BlockInfo * blk = &f->blocks[b];
    const GRJIT_Op * last = &blk->ops[blk->count - 1];
    bool return_reachable = true;
    for (size_t i = 0; i + 1 < blk->count; i++) {
      const GRJIT_Op * op = &blk->ops[i];
      if (op->kind == GRJIT_OP_GUARD && op->a.kind == GRJIT_OPERAND_IMM &&
          op->a.imm == 0) {
        return_reachable = false;
        break;
      }
    }
    if ((last->kind != GRJIT_OP_RET && last->kind != GRJIT_OP_TAIL_CALL_SLOT &&
            last->kind != GRJIT_OP_TAIL_CALL_PTR) ||
        !return_reachable) continue;
    if (return_count == SIZE_MAX) {
      return_count = last->kind == GRJIT_OP_RET
          ? last->result_count : last->callee_result_count;
    } else if ((last->kind == GRJIT_OP_RET
                    ? last->result_count : last->callee_result_count) != return_count) {
      return refuse(v, GRJIT_ERR_INVALID,
          "block b%zu: return or tail call has %zu results but other returns or tail calls have %zu",
          b, last->kind == GRJIT_OP_RET
              ? last->result_count : last->callee_result_count, return_count);
    }
    if (last->kind == GRJIT_OP_RET) {
      if (first_return == NULL) {
        first_return = last;
      } else {
        for (size_t i = 0; i < return_count; i++) {
          if (return_operand_type(v, &last->returns[i]) !=
              return_operand_type(v, &first_return->returns[i])) {
            return refuse(v, GRJIT_ERR_INVALID,
                "block b%zu: return result %zu has a different type than other returns",
                b, i);
          }
        }
      }
    }
  }
  if (f->callable && return_count != SIZE_MAX &&
      return_count != f->declared_result_count) {
    return refuse(v, GRJIT_ERR_INVALID,
        "callable declares %zu results but reachable return or tail call has %zu",
        f->declared_result_count, return_count);
  }
  return GRJIT_OK;
}

typedef struct UseCheck {
  const uint64_t * assigned;
  GRJIT_VReg bad;
  bool failed;
} UseCheck;

static void check_use(void * user, GRJIT_VReg r) {
  UseCheck * u = user;
  if (!u->failed && !((u->assigned[r / 64] >> (r % 64)) & 1u)) {
    u->failed = true;
    u->bad = r;
  }
}

static void mark_def(void * user, GRJIT_VReg r) {
  uint64_t * assigned = user;
  assigned[r / 64] |= UINT64_C(1) << (r % 64);
}

static void intersect_successor(const GRJIT_Function * f, GRJIT_BlockId target,
    const uint64_t * cur, size_t words, uint64_t * in, bool * changed) {
  if (target >= f->block_count) return;
  uint64_t * t = in + target * words;
  for (size_t w = 0; w < words; w++) {
    uint64_t n = t[w] & cur[w];
    if (n != t[w]) { t[w] = n; *changed = true; }
  }
}

static void propagate_successors(const GRJIT_Function * f,
    const GRJIT_BlockInfo * blk, const uint64_t * cur, size_t words,
    uint64_t * in, bool * changed) {
  if (blk->count == 0) return;
  const GRJIT_Op * last = &blk->ops[blk->count - 1];
  if (last->kind == GRJIT_OP_BR) {
    intersect_successor(f, last->target, cur, words, in, changed);
  } else if (last->kind == GRJIT_OP_BR_IF) {
    intersect_successor(f, last->target, cur, words, in, changed);
    intersect_successor(f, last->target_else, cur, words, in, changed);
  } else if (last->kind == GRJIT_OP_BR_TABLE) {
    intersect_successor(f, last->default_target, cur, words, in, changed);
    for (size_t t = 0; t < last->target_count; t++) {
      intersect_successor(f, last->targets[t], cur, words, in, changed);
    }
  }
}

/* A register must be assigned on every path to a use. */
static GRJIT_Result check_assigned(Verify * v) {
  const GRJIT_Function * f = v->f;
  const GRJIT_Allocator * a = f->allocator;
  size_t words = (f->vreg_count + 63) / 64;
  if (words == 0) {
    words = 1;
  }
  uint64_t * in = a->malloc_fn(a->ctx, (f->block_count + 2) * words * sizeof *in);
  if (in == NULL) {
    return GRJIT_ERR_OOM;
  }
  uint64_t * cur = in + f->block_count * words;
  uint64_t * ones = cur + words;
  memset(ones, 0xFF, words * sizeof *ones);
  for (size_t b = 0; b < f->block_count; b++) {
    memcpy(in + b * words, ones, words * sizeof *ones);
  }
  memset(in, 0, words * sizeof *in);
  for (size_t p = 0; p < f->param_count; p++) {
    in[p / 64] |= UINT64_C(1) << (p % 64);
  }
  bool changed = true;
  while (changed) {
    changed = false;
    for (size_t b = 0; b < f->block_count; b++) {
      const GRJIT_BlockInfo * blk = &f->blocks[b];
      memcpy(cur, in + b * words, words * sizeof *cur);
      for (size_t i = 0; i < blk->count; i++) {
        const GRJIT_Op * op = &blk->ops[i];
        /* A caught call can transfer control before defining its results.
         * Propagate the pre-call assignment set to the landing block so uses
         * there are checked against every exceptional predecessor. */
        if ((op->kind == GRJIT_OP_CALL_SLOT || op->kind == GRJIT_OP_CALL_PTR) &&
            op->landing_target != GRJIT_NO_BLOCK) {
          intersect_successor(f, op->landing_target, cur, words, in, &changed);
        }
        grjit_op_visit_defs(op, mark_def, cur);
      }
      propagate_successors(f, blk, cur, words, in, &changed);
    }
  }
  GRJIT_Result result = GRJIT_OK;
  for (size_t b = 0; b < f->block_count && result == GRJIT_OK; b++) {
    const GRJIT_BlockInfo * blk = &f->blocks[b];
    memcpy(cur, in + b * words, words * sizeof *cur);
    for (size_t i = 0; i < blk->count; i++) {
      UseCheck u = {cur, 0, false};
      grjit_op_visit_uses(f, &blk->ops[i], check_use, &u);
      if (u.failed) {
        result = refuse(v, GRJIT_ERR_INVALID,
            "v%u is used in block b%zu (op %zu) where it may not have been "
            "assigned",
            u.bad, b, i);
        break;
      }
      grjit_op_visit_defs(&blk->ops[i], mark_def, cur);
    }
  }
  a->free_fn(a->ctx, in);
  return result;
}

/* A derived pointer that is live at a site needs its base live there too. */
static GRJIT_Result check_derived(Verify * v) {
  const GRJIT_Function * f = v->f;
  GRJIT_LiveSites live;
  GRJIT_Result r = grjit_liveness_compute(f, f->allocator, v->limits.max_site_entries, &live);
  if (r != GRJIT_OK) {
    return r;
  }
  if (live.base_redefined) {
    GRJIT_Result bad = refuse(v, GRJIT_ERR_INVALID,
        "v%u is assigned at block b%u op %zu while derived pointer v%u, based "
        "on it, is live",
        live.redefined_base, live.redefined_block, live.redefined_op,
        live.redefined_derived);
    grjit_liveness_free(&live);
    return bad;
  }
  for (size_t s = 0; s < live.count; s++) {
    const GRJIT_SiteLive * site = &live.sites[s];
    for (size_t i = 0; i < site->count; i++) {
      GRJIT_VReg d = site->vregs[i];
      if (!f->vregs[d].derived) {
        continue;
      }
      /* The registers of a site are ascending, so the base is a search. */
      bool found = false;
      size_t lo = 0;
      size_t hi = site->count;
      while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (site->vregs[mid] == f->vregs[d].base) {
          found = true;
          break;
        }
        if (site->vregs[mid] < f->vregs[d].base) {
          lo = mid + 1;
        } else {
          hi = mid;
        }
      }
      if (!found) {
        r = refuse(v, GRJIT_ERR_INVALID,
            "derived pointer v%u is live at block b%u op %zu but its base v%u "
            "is not",
            d, site->block, site->op_index, f->vregs[d].base);
        break;
      }
    }
    if (r != GRJIT_OK) {
      break;
    }
  }
  grjit_liveness_free(&live);
  return r;
}


/* ---- The guards of the partial operations ------------------------------------------------
 *
 * A division or a remainder faults in x86 hardware on a zero divisor, and `idiv` also on
 * `MIN / -1`, and is a trap in WebAssembly. The operation never traps by itself (the IR has
 * no operation that traps or has undefined behaviour in hardware): the frontend puts the
 * guards in front of it as ordinary IR, and this pass refuses a division with no guard.
 *
 * The rule is about one basic block, read in order. A fact is recorded when a `GUARD` passes
 * and holds until a register it is about is assigned again:
 *
 *   non-zero(d)     the guard's operand is `d`, or a `CMP NE d, #0` / `CMP UGT d, #0` /
 *                   `CMP ULT #0, d` of it, which was not changed between the comparison and
 *                   the guard;
 *   no-overflow(a, b)  the guard's operand is a `CMP EQ t, #0` of `t = AND(e1, e2)` with
 *                   `e1 = CMP EQ a, #MIN` and `e2 = CMP EQ b, #-1`, or `OR(n1, n2)` with
 *                   `n1 = CMP NE a, #MIN` and `n2 = CMP NE b, #-1` (the two in either order);
 *   not-MIN(a)      for a constant divisor -1: a `CMP NE a, #MIN`.
 *
 * A fact is a *tick* (the position of the guard in the run of operations), and "assigned
 * again" is a later tick of definition, so one comparison answers it. A constant divisor is
 * judged as it stands: zero is refused, and a constant that is not -1 cannot overflow. */

/* What the range guards of a float register proved about it: every value that got past them lies
 * in the interval `lo .. hi` (an end is open when the compare was strict). The ticks are those of
 * the partial analysis below, so an assignment of the register retires the interval. */
typedef struct Range {
  uint64_t tick; /* when the latest fact was recorded; 0 if none */
  double lo, hi;
  bool lo_open, hi_open;
} Range;

typedef struct Partial {
  uint64_t * tick;       /* per register: the tick of its latest definition anywhere */
  const GRJIT_Op ** def; /* per register: the operation of that definition */
  uint64_t * nonzero;    /* per register: the tick of the guard that proved it non-zero */
  uint64_t * no_ovf;     /* per divisor: the tick of the guard that proved `a / b` cannot overflow */
  GRJIT_VReg * ovf_a;    /* ... and the dividend it was about */
  uint64_t * not_min;    /* per dividend: the tick of a guard that proved it is not MIN */
  Range * range;  /* per float register: the interval its range guards proved */
  uint64_t now;
  uint64_t block_start;
} Partial;

static bool is_partial_op(GRJIT_OpKind k) {
  return k == GRJIT_OP_DIV_S || k == GRJIT_OP_DIV_U || k == GRJIT_OP_REM_S ||
         k == GRJIT_OP_REM_U;
}

static bool is_trunc_op(GRJIT_OpKind k) {
  return k == GRJIT_OP_TRUNC_S || k == GRJIT_OP_TRUNC_U;
}

static bool imm_is(const GRJIT_Operand * o, GRJIT_Type width, int64_t sixty_four,
    uint32_t thirty_two) {
  if (o->kind != GRJIT_OPERAND_IMM) {
    return false;
  }
  return width == GRJIT_TYPE_I32 ? (uint32_t)o->imm == thirty_two && imm32_ok(o->imm)
                                 : o->imm == sixty_four;
}

static bool imm_zero(const GRJIT_Operand * o, GRJIT_Type w) { return imm_is(o, w, 0, 0); }
static bool imm_min(const GRJIT_Operand * o, GRJIT_Type w) {
  return imm_is(o, w, INT64_MIN, UINT32_C(0x80000000));
}
static bool imm_neg1(const GRJIT_Operand * o, GRJIT_Type w) {
  return imm_is(o, w, -1, UINT32_C(0xFFFFFFFF));
}

/* The register `x` was defined in this block and not again after `tick`. */
static bool defined_here(const Partial * p, GRJIT_VReg x) {
  return p->tick[x] >= p->block_start && p->def[x] != NULL;
}

static bool vreg_operand(const GRJIT_Operand * o) { return o->kind == GRJIT_OPERAND_VREG; }

/* `cmp` is a `CMP` of the shape `x cmp #value` (or its mirror) in which `x` is a register
 * not changed since the comparison was made at `at`; answers `x`, or NO_VREG. */
static GRJIT_VReg cmp_reg_against(const Verify * v, const Partial * p, const GRJIT_Op * cmp,
    uint64_t at, bool (*imm_ok)(const GRJIT_Operand *, GRJIT_Type), bool mirror_ok,
    GRJIT_Cmp want, GRJIT_Cmp mirror) {
  const GRJIT_Operand * x = NULL;
  const GRJIT_Operand * k = NULL;
  if (cmp->cmp == want && vreg_operand(&cmp->a)) {
    x = &cmp->a;
    k = &cmp->b;
  } else if (mirror_ok && cmp->cmp == mirror && vreg_operand(&cmp->b)) {
    x = &cmp->b;
    k = &cmp->a;
  } else {
    return GRJIT_NO_VREG;
  }
  if (!vreg_ok(v, x->vreg) || !imm_ok(k, type_of(v, x->vreg)) || p->tick[x->vreg] >= at) {
    return GRJIT_NO_VREG;
  }
  return x->vreg;
}

/* `e` is a register defined in this block, before `before`, by `CMP cmp r, #imm_ok` whose
 * register is unchanged since: answers `r`. */
static GRJIT_VReg flag_of(const Verify * v, const Partial * p, GRJIT_VReg e, uint64_t before,
    GRJIT_Cmp cmp, bool (*imm_ok)(const GRJIT_Operand *, GRJIT_Type)) {
  if (!vreg_ok(v, e) || !defined_here(p, e) || p->tick[e] >= before ||
      p->def[e]->kind != GRJIT_OP_CMP) {
    return GRJIT_NO_VREG;
  }
  return cmp_reg_against(v, p, p->def[e], p->tick[e], imm_ok, false, cmp, cmp);
}

/* The two registers of `join` (an AND or an OR defined here before `before`) are flags of
 * `cmp_a r_a, #imm_a` and `cmp_b r_b, #imm_b` in either order: answers both registers. */
static bool joined_flags(const Verify * v, const Partial * p, const GRJIT_Op * join,
    uint64_t join_tick, GRJIT_Cmp cmp, GRJIT_VReg * a, GRJIT_VReg * b) {
  if (!vreg_operand(&join->a) || !vreg_operand(&join->b)) {
    return false;
  }
  for (int swap = 0; swap < 2; swap++) {
    GRJIT_VReg e1 = swap ? join->b.vreg : join->a.vreg;
    GRJIT_VReg e2 = swap ? join->a.vreg : join->b.vreg;
    GRJIT_VReg ra = flag_of(v, p, e1, join_tick, cmp, imm_min);
    GRJIT_VReg rb = flag_of(v, p, e2, join_tick, cmp, imm_neg1);
    if (ra != GRJIT_NO_VREG && rb != GRJIT_NO_VREG) {
      *a = ra;
      *b = rb;
      return true;
    }
  }
  return false;
}

/* A float compare of a register against an immediate, as a bound on the register: `x > k` or
 * `x >= k` is a lower bound, `x < k` or `x <= k` an upper one, and the mirrored spellings
 * (`k < x`) are the same. The register must not have been assigned since the compare at `at`.
 * A NaN bound, an equality and a compare of anything else are not bounds. */
/* An immediate in a float position: plain bits, or a typed float immediate (checked against the
 * register's width by check_float_operand). */
static bool float_imm_operand(const GRJIT_Operand * o) {
  return o->kind == GRJIT_OPERAND_IMM || o->kind == GRJIT_OPERAND_F32_IMM ||
         o->kind == GRJIT_OPERAND_F64_IMM;
}

static bool float_bound(const Verify * v, const Partial * p, const GRJIT_Op * m, uint64_t at,
    GRJIT_VReg * x, bool * lower, double * k, bool * open) {
  GRJIT_Cmp c = m->cmp;
  const GRJIT_Operand * r;
  const GRJIT_Operand * im;
  if (vreg_operand(&m->a) && float_imm_operand(&m->b)) {
    r = &m->a;
    im = &m->b;
  } else if (vreg_operand(&m->b) && float_imm_operand(&m->a)) {
    r = &m->b;
    im = &m->a;
    c = c == GRJIT_CMP_LT ? GRJIT_CMP_GT
        : c == GRJIT_CMP_GT ? GRJIT_CMP_LT
        : c == GRJIT_CMP_LE ? GRJIT_CMP_GE
        : c == GRJIT_CMP_GE ? GRJIT_CMP_LE : c;
  } else {
    return false;
  }
  if (!vreg_ok(v, r->vreg) || !grjit_type_is_float(type_of(v, r->vreg)) || p->tick[r->vreg] >= at) {
    return false;
  }
  double val;
  if (type_of(v, r->vreg) == GRJIT_TYPE_F32) {
    uint32_t bits = (uint32_t)im->imm;
    float f;
    memcpy(&f, &bits, sizeof f);
    val = (double)f;
  } else {
    uint64_t bits = (uint64_t)im->imm;
    memcpy(&val, &bits, sizeof val);
  }
  if (val != val) {
    return false;
  }
  switch (c) {
    case GRJIT_CMP_GT: *lower = true; *open = true; break;
    case GRJIT_CMP_GE: *lower = true; *open = false; break;
    case GRJIT_CMP_LT: *lower = false; *open = true; break;
    case GRJIT_CMP_LE: *lower = false; *open = false; break;
    default: return false;
  }
  *x = r->vreg;
  *k = val;
  return true;
}

/* The bounds a passed guard on `c` proves: `c` is a float compare against an immediate, or an AND
 * of such (and of ANDs of them), all defined in this block with nothing they read assigned since.
 * A bound narrows the interval already proved for the register, if that one is still standing. */
static void collect_bounds(const Verify * v, Partial * p, GRJIT_VReg c, uint64_t before,
    unsigned depth, uint64_t now) {
  if (depth > 4 || !vreg_ok(v, c) || !defined_here(p, c) || p->tick[c] >= before) {
    return;
  }
  const GRJIT_Op * m = p->def[c];
  const uint64_t at = p->tick[c];
  if (m->kind == GRJIT_OP_AND) {
    if (vreg_operand(&m->a)) {
      collect_bounds(v, p, m->a.vreg, at, depth + 1, now);
    }
    if (vreg_operand(&m->b)) {
      collect_bounds(v, p, m->b.vreg, at, depth + 1, now);
    }
    return;
  }
  GRJIT_VReg x;
  bool lower, open;
  double k;
  if (m->kind != GRJIT_OP_CMP || !float_bound(v, p, m, at, &x, &lower, &k, &open)) {
    return;
  }
  Range * r = &p->range[x];
  if (!(r->tick != 0 && r->tick >= p->block_start && r->tick > p->tick[x])) {
    r->lo = -INFINITY;
    r->hi = INFINITY;
    r->lo_open = true;
    r->hi_open = true;
  }
  if (lower) {
    if (k > r->lo || (k == r->lo && open)) {
      r->lo = k;
      r->lo_open = open;
    }
  } else if (k < r->hi || (k == r->hi && open)) {
    r->hi = k;
    r->hi_open = open;
  }
  r->tick = now;
}

static void record_guard(const Verify * v, Partial * p, const GRJIT_Operand * cond) {
  if (!vreg_operand(cond) || !vreg_ok(v, cond->vreg)) {
    return;
  }
  const GRJIT_VReg c = cond->vreg;
  const uint64_t now = p->now;
  p->nonzero[c] = now; /* the guard passed: its operand is not zero */
  collect_bounds(v, p, c, now, 0, now);
  if (!defined_here(p, c)) {
    return;
  }
  const GRJIT_Op * m = p->def[c];
  const uint64_t at = p->tick[c];
  GRJIT_VReg a = GRJIT_NO_VREG, b = GRJIT_NO_VREG;
  switch (m->kind) {
    case GRJIT_OP_CMP: {
      GRJIT_VReg x = cmp_reg_against(v, p, m, at, imm_zero, true, GRJIT_CMP_NE, GRJIT_CMP_NE);
      if (x == GRJIT_NO_VREG) {
        x = cmp_reg_against(v, p, m, at, imm_zero, true, GRJIT_CMP_UGT, GRJIT_CMP_ULT);
      }
      if (x != GRJIT_NO_VREG) {
        p->nonzero[x] = now;
        return;
      }
      x = cmp_reg_against(v, p, m, at, imm_min, false, GRJIT_CMP_NE, GRJIT_CMP_NE);
      if (x != GRJIT_NO_VREG) {
        p->not_min[x] = now;
        return;
      }
      /* AND shape: c = CMP EQ t, #0 with t = AND(CMP EQ a, #MIN; CMP EQ b, #-1). */
      GRJIT_VReg t = cmp_reg_against(v, p, m, at, imm_zero, true, GRJIT_CMP_EQ, GRJIT_CMP_EQ);
      if (t != GRJIT_NO_VREG && defined_here(p, t) && p->def[t]->kind == GRJIT_OP_AND &&
          joined_flags(v, p, p->def[t], p->tick[t], GRJIT_CMP_EQ, &a, &b)) {
        p->no_ovf[b] = now;
        p->ovf_a[b] = a;
      }
      return;
    }
    case GRJIT_OP_OR:
      /* OR shape: c = OR(CMP NE a, #MIN; CMP NE b, #-1). */
      if (joined_flags(v, p, m, at, GRJIT_CMP_NE, &a, &b)) {
        p->no_ovf[b] = now;
        p->ovf_a[b] = a;
      }
      return;
    default:
      return;
  }
}

static bool fresh(const Partial * p, uint64_t proof, GRJIT_VReg x) {
  return proof != 0 && proof >= p->block_start && proof > p->tick[x];
}

static GRJIT_Result check_one_partial(Verify * v, const Partial * p, const GRJIT_Op * op,
    GRJIT_BlockId block, size_t index) {
  const GRJIT_Type w = type_of(v, op->dst);
  const char * name = op->kind == GRJIT_OP_DIV_S   ? "div_s"
                      : op->kind == GRJIT_OP_DIV_U ? "div_u"
                      : op->kind == GRJIT_OP_REM_S ? "rem_s"
                                                   : "rem_u";
  if (op->b.kind == GRJIT_OPERAND_IMM) {
    if (imm_zero(&op->b, w)) {
      return refuse(v, GRJIT_ERR_INVALID,
          "block b%u op %zu: %s by the constant zero (a trap, to be written as one)", block,
          index, name);
    }
  } else if (!fresh(p, p->nonzero[op->b.vreg], op->b.vreg)) {
    return refuse(v, GRJIT_ERR_INVALID,
        "block b%u op %zu: %s needs, earlier in the same block and with v%u not assigned in "
        "between, a guard that the divisor v%u is not zero (a GUARD of it, or of a CMP NE or "
        "UGT of it with #0)",
        block, index, name, op->b.vreg, op->b.vreg);
  }
  if (op->kind != GRJIT_OP_DIV_S) {
    return GRJIT_OK;
  }
  /* Signed division also overflows: MIN / -1. */
  bool ok;
  if (op->b.kind == GRJIT_OPERAND_IMM) {
    if (!imm_neg1(&op->b, w)) {
      ok = true;
    } else if (op->a.kind == GRJIT_OPERAND_IMM) {
      ok = !imm_min(&op->a, w);
    } else {
      ok = fresh(p, p->not_min[op->a.vreg], op->a.vreg);
    }
  } else if (op->a.kind == GRJIT_OPERAND_IMM) {
    ok = !imm_min(&op->a, w); /* a constant dividend other than MIN cannot overflow */
  } else {
    const GRJIT_VReg b = op->b.vreg;
    ok = p->ovf_a[b] == op->a.vreg && fresh(p, p->no_ovf[b], b) &&
         fresh(p, p->no_ovf[b], op->a.vreg);
  }
  if (!ok) {
    return refuse(v, GRJIT_ERR_INVALID,
        "block b%u op %zu: div_s needs, earlier in the same block and with its operands not "
        "assigned in between, a guard that the dividend is not MIN or the divisor is not -1 "
        "(a CMP EQ a, #MIN and a CMP EQ b, #-1 joined by AND and tested EQ #0, or the two "
        "CMP NE joined by OR)",
        block, index);
  }
  return GRJIT_OK;
}

/* The exact bounds of a float to integer truncation, as reals: the operand must lie strictly
 * between them for the truncated value to fit the destination. `-2^63 - 1` is no double, so for
 * the signed 64-bit lower bound the double just above it, `-2^63`, stands for it: a closed bound
 * there is exact, and anything lower is wider. */
static void trunc_bounds(bool is_unsigned, bool wide, double * lo, bool * lo_next_below, double * hi) {
  *lo_next_below = false;
  if (is_unsigned) {
    *lo = -1.0;
    *hi = wide ? 18446744073709551616.0 : 4294967296.0;
  } else if (wide) {
    *lo = -9223372036854775808.0;
    *lo_next_below = true;
    *hi = 9223372036854775808.0;
  } else {
    *lo = -2147483649.0;
    *hi = 2147483648.0;
  }
}

static const char * trunc_name(const Verify * v, const GRJIT_Op * op) {
  const bool wide = type_of(v, op->dst) == GRJIT_TYPE_I64;
  const bool f32 = type_of(v, op->a.vreg) == GRJIT_TYPE_F32;
  const bool uns = op->kind == GRJIT_OP_TRUNC_U;
  return wide ? (f32 ? (uns ? "i64.trunc_f32_u" : "i64.trunc_f32_s")
                     : (uns ? "i64.trunc_f64_u" : "i64.trunc_f64_s"))
              : (f32 ? (uns ? "i32.trunc_f32_u" : "i32.trunc_f32_s")
                     : (uns ? "i32.trunc_f64_u" : "i32.trunc_f64_s"));
}

static GRJIT_Result check_one_trunc(Verify * v, const Partial * p, const GRJIT_Op * op,
    GRJIT_BlockId block, size_t index) {
#if defined(GRJIT_TEST_PLANT_BUG) && GRJIT_TEST_PLANT_BUG == 44
  /* Planted defect 44 (tests only): the verifier does not require the range guard of a
   * truncation. The negative tests and the Wasm harness must catch it. */
  (void)v; (void)p; (void)op; (void)block; (void)index;
  return GRJIT_OK;
#else
  const GRJIT_VReg x = op->a.vreg;
  double lo, hi;
  bool next_below;
  trunc_bounds(op->kind == GRJIT_OP_TRUNC_U, type_of(v, op->dst) == GRJIT_TYPE_I64, &lo,
      &next_below, &hi);
  if (defined_here(p, x) && p->def[x]->kind == GRJIT_OP_CONST) {
    /* A constant operand is judged as it stands. */
    double val;
    const int64_t imm = p->def[x]->a.imm;
    if (type_of(v, x) == GRJIT_TYPE_F32) {
      uint32_t bits = (uint32_t)imm;
      float f;
      memcpy(&f, &bits, sizeof f);
      val = (double)f;
    } else {
      uint64_t bits = (uint64_t)imm;
      memcpy(&val, &bits, sizeof val);
    }
    if (val == val && (next_below ? val >= lo : val > lo) && val < hi) {
      return GRJIT_OK;
    }
    return refuse(v, GRJIT_ERR_INVALID,
        "block b%u op %zu: %s of the constant v%u, which is a NaN or does not truncate to a "
        "value of the destination (a trap, to be written as one)",
        block, index, trunc_name(v, op), x);
  }
  const Range * r = &p->range[x];
  const bool standing = r->tick != 0 && r->tick >= p->block_start && r->tick > p->tick[x];
  bool ok = false;
  if (standing) {
    const bool lo_ok = r->lo_open ? r->lo >= lo : (next_below ? r->lo >= lo : r->lo > lo);
    const bool hi_ok = r->hi_open ? r->hi <= hi : r->hi < hi;
    ok = lo_ok && hi_ok;
  }
  if (!ok) {
    return refuse(v, GRJIT_ERR_INVALID,
        "block b%u op %zu: %s needs, earlier in the same block and with v%u not assigned in "
        "between, a range guard: a GUARD of an AND of an ordered lower compare and an ordered "
        "upper compare of v%u against immediates no wider than the exact bounds (%s%.0f and "
        "< %.0f), and the NaN guard alone is not that%s",
        block, index, trunc_name(v, op), x, x, next_below ? ">= " : "> ", lo, hi,
        standing ? " (the guard there is wider than the exact bounds)" : "");
  }
  return GRJIT_OK;
#endif
}

static GRJIT_Result check_partial(Verify * v) {
  const GRJIT_Function * f = v->f;
  bool any = false;
  for (size_t b = 0; b < f->block_count && !any; b++) {
    for (size_t i = 0; i < f->blocks[b].count; i++) {
      if (is_partial_op(f->blocks[b].ops[i].kind) || is_trunc_op(f->blocks[b].ops[i].kind)) {
        any = true;
        break;
      }
    }
  }
  if (!any) {
    return GRJIT_OK;
  }
#if defined(GRJIT_TEST_PLANT_BUG) && GRJIT_TEST_PLANT_BUG == 38
  /* Planted defect 38 (tests only): the verifier does not require the guards of a
   * partial operation. The negative tests and the Wasm harness must catch it. */
  return GRJIT_OK;
#endif
  const GRJIT_Allocator * a = f->allocator;
  const size_t n = f->vreg_count;
  Partial p;
  memset(&p, 0, sizeof p);
  uint64_t * words = a->calloc_fn(a->ctx, n == 0 ? 1 : n, 4 * sizeof *words);
  const GRJIT_Op ** defs = a->calloc_fn(a->ctx, n == 0 ? 1 : n, sizeof *defs);
  GRJIT_VReg * dividends = a->calloc_fn(a->ctx, n == 0 ? 1 : n, sizeof *dividends);
  Range * ranges = a->calloc_fn(a->ctx, n == 0 ? 1 : n, sizeof *ranges);
  if (words == NULL || defs == NULL || dividends == NULL || ranges == NULL) {
    a->free_fn(a->ctx, words);
    a->free_fn(a->ctx, defs);
    a->free_fn(a->ctx, dividends);
    a->free_fn(a->ctx, ranges);
    return GRJIT_ERR_OOM;
  }
  p.tick = words;
  p.nonzero = words + n;
  p.no_ovf = words + 2 * n;
  p.not_min = words + 3 * n;
  p.def = defs;
  p.ovf_a = dividends;
  p.range = ranges;
  GRJIT_Result result = GRJIT_OK;
  for (size_t b = 0; b < f->block_count && result == GRJIT_OK; b++) {
    const GRJIT_BlockInfo * blk = &f->blocks[b];
    p.block_start = ++p.now;
    for (size_t i = 0; i < blk->count && result == GRJIT_OK; i++) {
      const GRJIT_Op * op = &blk->ops[i];
      p.now++;
      if (is_partial_op(op->kind)) {
        result = check_one_partial(v, &p, op, (GRJIT_BlockId)b, i);
      } else if (is_trunc_op(op->kind)) {
        result = check_one_trunc(v, &p, op, (GRJIT_BlockId)b, i);
      } else if (op->kind == GRJIT_OP_GUARD) {
        record_guard(v, &p, &op->a);
      }
      GRJIT_VReg d = grjit_op_def(op);
      if (d != GRJIT_NO_VREG && d < n) {
        p.tick[d] = p.now;
        p.def[d] = op;
      }
    }
  }
  a->free_fn(a->ctx, words);
  a->free_fn(a->ctx, defs);
  a->free_fn(a->ctx, dividends);
  a->free_fn(a->ctx, ranges);
  return result;
}

GRJIT_Result grjit_function_verify(const GRJIT_Function * function,
    const GRJIT_Limits * limits, char * reason, size_t reason_size) {
  if (function == NULL) {
    return GRJIT_ERR_INVALID;
  }
  Verify v;
  v.f = function;
  grjit_limits_resolve(limits, &v.limits);
  v.reason = reason;
  v.reason_size = reason_size;
  if (function->block_count > v.limits.max_blocks) {
    return refuse(&v, GRJIT_ERR_LIMIT, "%zu blocks, the limit is %zu",
        function->block_count, v.limits.max_blocks);
  }
  if (function->vreg_count > v.limits.max_vregs) {
    return refuse(&v, GRJIT_ERR_LIMIT, "%zu registers, the limit is %zu",
        function->vreg_count, v.limits.max_vregs);
  }
  if (function->op_count > v.limits.max_operations) {
    return refuse(&v, GRJIT_ERR_LIMIT, "%zu operations, the limit is %zu",
        function->op_count, v.limits.max_operations);
  }
  if (function->interp_slots > v.limits.max_frame_state_slots) {
    return refuse(&v, GRJIT_ERR_LIMIT,
        "%zu interpreter slots, the frame-state limit is %zu",
        function->interp_slots, v.limits.max_frame_state_slots);
  }
  GRJIT_Result r = check_structure(&v);
  if (r != GRJIT_OK) {
    return r;
  }
  r = check_assigned(&v);
  if (r != GRJIT_OK) {
    return r;
  }
  r = check_partial(&v);
  if (r != GRJIT_OK) {
    return r;
  }
  return check_derived(&v);
}
