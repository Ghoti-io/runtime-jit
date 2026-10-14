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
 * The IR printer. The text is deterministic: the same function always prints
 * the same bytes, which is what lets a test assert the shape of a function.
 */

#include <ghoti.io/runtime-jit/macros.h>

#include <ghoti.io/runtime-jit/print.h>

#include "ir_internal.h"

#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>

typedef struct Out {
  char * buffer;
  size_t size;
  size_t length;
} Out;

static void put(Out * o, const char * format, ...) {
  va_list args;
  va_start(args, format);
  size_t room = o->length < o->size ? o->size - o->length : 0;
  int n = vsnprintf(
      room != 0 && o->buffer != NULL ? o->buffer + o->length : NULL, room,
      format, args);
  va_end(args);
  if (n > 0) {
    o->length += (size_t)n;
  }
}

static const char * type_name(GRJIT_Type t) {
  switch (t) {
    case GRJIT_TYPE_I64:
      return "i64";
    case GRJIT_TYPE_REF:
      return "ref";
    case GRJIT_TYPE_PTR:
      return "ptr";
    case GRJIT_TYPE_I32:
      return "i32";
    case GRJIT_TYPE_F32:
      return "f32";
    case GRJIT_TYPE_F64:
      return "f64";
    case GRJIT_TYPE_RESERVED_NO_RESULT:
    case GRJIT_TYPE_COUNT:
      break;
  }
  return "?";
}

static const char * op_name(GRJIT_OpKind k) {
  static const char * const names[] = {"const", "move", "add", "sub", "mul",
      "and", "or", "xor", "shl", "shr", "sar", "neg", "not", "cmp", "load",
      "load_s", "store", "call", "poll", "guard", "br", "br_if", "ret",
      "bitcast", "call_slot", "call_ptr", "tail_call_slot", "tail_call_ptr",
      "call_native", "clz", "ctz", "popcnt", "rotl", "rotr", "ext8_s", "ext16_s",
      "ext32_s", "div_s", "div_u", "rem_s", "rem_u", "wrap", "extend_s", "extend_u",
      /* The float operations: the type prefix (`f32.`, `i64.`) is put before the name. */
      "add", "sub", "mul", "div", "min", "max", "copysign", "neg", "abs", "sqrt", "ceil",
      "floor", "trunc", "nearest", "convert_i_s", "convert_i_u", "trunc_f_s", "trunc_f_u",
      "trunc_sat_f_s", "trunc_sat_f_u", "demote_f64", "promote_f32", "reinterpret",
      "instance", "br_table", "trap", "rethrow"};
  _Static_assert(sizeof names / sizeof names[0] == (size_t)GRJIT_OP_COUNT,
      "one name for every operation");
  return (unsigned)k < (unsigned)GRJIT_OP_COUNT ? names[k] : "?";
}

static const char * cmp_name(GRJIT_Cmp c) {
  static const char * const names[] = {
      "eq", "ne", "lt", "le", "gt", "ge", "ult", "ule", "ugt", "uge"};
  return (unsigned)c < (unsigned)GRJIT_CMP_COUNT ? names[c] : "?";
}

static const char * site_name(GRCORE_CodeSiteKind k) {
  static const char * const names[] = {
      "poll", "alloc_slow", "call", "frame_push", "nested_entry", "guard"};
  return (unsigned)k < (unsigned)GRCORE_SITE_KIND_COUNT ? names[k] : "?";
}

static void put_operand(Out * o, const GRJIT_Operand * a) {
  switch (a->kind) {
    case GRJIT_OPERAND_VREG:
      put(o, "v%u", a->vreg);
      break;
    case GRJIT_OPERAND_IMM:
      put(o, "#%" PRId64, a->imm);
      break;
    case GRJIT_OPERAND_F32_IMM:
      put(o, "#f32:0x%08" PRIx64, (uint64_t)a->imm);
      break;
    case GRJIT_OPERAND_F64_IMM:
      put(o, "#f64:0x%016" PRIx64, (uint64_t)a->imm);
      break;
    default:
      put(o, "_");
      break;
  }
}

/* An operand of a float operation: an immediate is the operand's bit pattern, printed as hex. */
static void put_float_operand(Out * o, const GRJIT_Operand * a) {
  if (a->kind == GRJIT_OPERAND_IMM) {
    put(o, "#0x%" PRIx64, (uint64_t)a->imm);
  } else {
    put_operand(o, a);
  }
}

static bool vreg_is_float(const GRJIT_Function * f, const GRJIT_Operand * a) {
  return a->kind == GRJIT_OPERAND_VREG && a->vreg < f->vreg_count &&
         grjit_type_is_float(f->vregs[a->vreg].type);
}

static GRJIT_Type type_of_operand(const GRJIT_Function * f, const GRJIT_Operand * a) {
  return a->kind == GRJIT_OPERAND_VREG && a->vreg < f->vreg_count ? f->vregs[a->vreg].type
                                                                  : GRJIT_TYPE_COUNT;
}

static void put_state(Out * o, const GRJIT_Function * f, uint32_t index) {
  if (index >= f->state_count) {
    put(o, " state ?");
    return;
  }
  const GRJIT_FrameState * s = &f->states[index];
  put(o, " state fn=%" PRIu64 " off=%" PRIu64 " [", s->identity.function,
      s->identity.offset);
  for (size_t i = 0; i < s->slot_count; i++) {
    const GRJIT_FrameSlot * slot = &s->slots[i];
    put(o, i == 0 ? "" : ", ");
    if (slot->kind == GRJIT_FRAME_SLOT_VREG) {
      put(o, "v%u", slot->vreg);
    } else if (slot->kind == GRJIT_FRAME_SLOT_CONSTANT) {
      put(o, "#%" PRId64, slot->constant);
    } else {
      put(o, "dead");
    }
  }
  put(o, "]");
}

static void put_mem(Out * o, const GRJIT_Op * op) {
  put(o, "[");
  put_operand(o, &op->a);
  if (op->index.kind != GRJIT_OPERAND_NONE) {
    put(o, "+");
    put_operand(o, &op->index);
    if (op->scale != 1) put(o, "*%u", (unsigned)op->scale);
  }
  if (op->disp < 0) {
    put(o, "-%" PRId64, -(int64_t)op->disp);
  } else {
    put(o, "+%" PRId64, (int64_t)op->disp);
  }
  put(o, "]");
}

/* The name of a float operation as WebAssembly spells it: `f64.add`, `i32.trunc_f64_s`,
 * `f32.convert_i64_u`, `i64.reinterpret_f64`. */
static void put_float_op_name(Out * o, const GRJIT_Function * f, const GRJIT_Op * op) {
  const GRJIT_Type dst = op->dst < f->vreg_count ? f->vregs[op->dst].type : GRJIT_TYPE_COUNT;
  const GRJIT_Type src = type_of_operand(f, &op->a);
  const char * d = type_name(dst);
  const char * s = type_name(src);
  switch (op->kind) {
    case GRJIT_OP_INSTANCE:
      put(o, "instance");
      break;
    case GRJIT_OP_CONVERT_S:
    case GRJIT_OP_CONVERT_U:
      put(o, "%s.convert_%s_%c", d, s, op->kind == GRJIT_OP_CONVERT_S ? 's' : 'u');
      break;
    case GRJIT_OP_TRUNC_S:
    case GRJIT_OP_TRUNC_U:
      put(o, "%s.trunc_%s_%c", d, s, op->kind == GRJIT_OP_TRUNC_S ? 's' : 'u');
      break;
    case GRJIT_OP_TRUNC_SAT_S:
    case GRJIT_OP_TRUNC_SAT_U:
      put(o, "%s.trunc_sat_%s_%c", d, s, op->kind == GRJIT_OP_TRUNC_SAT_S ? 's' : 'u');
      break;
    case GRJIT_OP_DEMOTE:
      put(o, "f32.demote_f64");
      break;
    case GRJIT_OP_PROMOTE:
      put(o, "f64.promote_f32");
      break;
    case GRJIT_OP_REINTERPRET:
      put(o, "%s.reinterpret_%s", d, s);
      break;
    default:
      put(o, "%s.%s", d, op_name(op->kind));
      break;
  }
}

static void put_op(Out * o, const GRJIT_Function * f, const GRJIT_Op * op) {
  put(o, "  ");
  GRJIT_VReg def = grjit_op_def(op);
  if ((op->kind == GRJIT_OP_CALL_SLOT || op->kind == GRJIT_OP_CALL_PTR) && op->result_count != 0) {
    for (size_t i = 0; i < op->result_count; i++) put(o, "%sv%u", i == 0 ? "" : ", ", op->results[i]);
    put(o, " = ");
  } else if (def != GRJIT_NO_VREG) {
    put(o, "v%u = ", def);
  }
  switch (op->kind) {
    case GRJIT_OP_CONST:
      if (def != GRJIT_NO_VREG && def < f->vreg_count &&
          grjit_type_is_float(f->vregs[def].type)) {
        put(o, "const 0x%" PRIx64, (uint64_t)op->a.imm);
      } else {
        put(o, "const %" PRId64, op->a.imm);
      }
      break;
    case GRJIT_OP_FADD:
    case GRJIT_OP_FSUB:
    case GRJIT_OP_FMUL:
    case GRJIT_OP_FDIV:
    case GRJIT_OP_FMIN:
    case GRJIT_OP_FMAX:
    case GRJIT_OP_FCOPYSIGN:
      put_float_op_name(o, f, op);
      put(o, " ");
      put_float_operand(o, &op->a);
      put(o, ", ");
      put_float_operand(o, &op->b);
      break;
    case GRJIT_OP_FNEG:
    case GRJIT_OP_FABS:
    case GRJIT_OP_FSQRT:
    case GRJIT_OP_FCEIL:
    case GRJIT_OP_FFLOOR:
    case GRJIT_OP_FTRUNC:
    case GRJIT_OP_FNEAREST:
      put_float_op_name(o, f, op);
      put(o, " ");
      put_float_operand(o, &op->a);
      break;
    case GRJIT_OP_CONVERT_S:
    case GRJIT_OP_CONVERT_U:
    case GRJIT_OP_TRUNC_S:
    case GRJIT_OP_TRUNC_U:
    case GRJIT_OP_TRUNC_SAT_S:
    case GRJIT_OP_TRUNC_SAT_U:
    case GRJIT_OP_DEMOTE:
    case GRJIT_OP_PROMOTE:
    case GRJIT_OP_REINTERPRET:
      put_float_op_name(o, f, op);
      put(o, " ");
      put_operand(o, &op->a);
      break;
    case GRJIT_OP_MOVE:
    case GRJIT_OP_BITCAST:
    case GRJIT_OP_NEG:
    case GRJIT_OP_NOT:
    case GRJIT_OP_CLZ:
    case GRJIT_OP_CTZ:
    case GRJIT_OP_POPCNT:
    case GRJIT_OP_EXT8_S:
    case GRJIT_OP_EXT16_S:
    case GRJIT_OP_EXT32_S:
    case GRJIT_OP_WRAP:
    case GRJIT_OP_EXTEND_S:
    case GRJIT_OP_EXTEND_U:
      put(o, "%s ", op_name(op->kind));
      if (op->kind == GRJIT_OP_MOVE && def != GRJIT_NO_VREG && def < f->vreg_count &&
          grjit_type_is_float(f->vregs[def].type)) {
        put_float_operand(o, &op->a);
      } else {
        put_operand(o, &op->a);
      }
      break;
    case GRJIT_OP_CMP:
      if (vreg_is_float(f, &op->a) || vreg_is_float(f, &op->b)) {
        /* A float comparison names the width, and its immediate is a bit pattern. */
        put(o, "cmp.%s.%s ", type_name(vreg_is_float(f, &op->a) ? type_of_operand(f, &op->a)
                                                                  : type_of_operand(f, &op->b)),
            cmp_name(op->cmp));
        put_float_operand(o, &op->a);
        put(o, ", ");
        put_float_operand(o, &op->b);
        break;
      }
      put(o, "cmp.%s ", cmp_name(op->cmp));
      put_operand(o, &op->a);
      put(o, ", ");
      put_operand(o, &op->b);
      break;
    case GRJIT_OP_LOAD:
    case GRJIT_OP_LOAD_S:
      put(o, "%s.%u ", op_name(op->kind), op->width);
      put_mem(o, op);
      break;
    case GRJIT_OP_STORE:
      put(o, "store.%u ", op->width);
      put_mem(o, op);
      put(o, ", ");
      put_operand(o, &op->b);
      break;
    case GRJIT_OP_CALL:
      if (op->attr == GRJIT_CALL_GC_POINT) {
        put(o, "call.gc.%s 0x%" PRIx64 "(", site_name(op->site_kind),
            op->address);
      } else {
        put(o, "call.nogc 0x%" PRIx64 "(", op->address);
      }
      for (size_t i = 0; i < op->arg_count; i++) {
        put(o, i == 0 ? "" : ", ");
        put_operand(o, &op->args[i]);
      }
      put(o, ")");
      if (op->result_count != 0) {
        put(o, " -> ");
        for (size_t i = 0; i < op->result_count; i++) put(o, "%sv%u", i == 0 ? "" : ", ", op->results[i]);
      }
      if (op->attr == GRJIT_CALL_GC_POINT) {
        put_state(o, f, op->state);
      }
      break;
    case GRJIT_OP_CALL_SLOT:
    case GRJIT_OP_CALL_PTR:
      if (op->kind == GRJIT_OP_CALL_SLOT) {
        put(o, "call.slot 0x%" PRIx64, op->address);
      } else {
        put(o, "call.ptr ");
        put_operand(o, &op->a);
      }
      put(o, " callee=%" PRIu64 "(", op->callee);
      for (size_t i = 0; i < op->arg_count; i++) {
        put(o, i == 0 ? "" : ", ");
        put_operand(o, &op->args[i]);
      }
      put(o, ")");
      put_state(o, f, op->state);
      put(o, " exit");
      put_state(o, f, op->exit_state);
      if (op->landing_target != GRJIT_NO_BLOCK) put(o, " catch b%u", op->landing_target);
      break;
    case GRJIT_OP_TAIL_CALL_SLOT:
    case GRJIT_OP_TAIL_CALL_PTR:
      if (op->kind == GRJIT_OP_TAIL_CALL_SLOT) {
        put(o, "tail_call.slot 0x%" PRIx64, op->address);
      } else {
        put(o, "tail_call.ptr ");
        put_operand(o, &op->a);
      }
      put(o, " callee=%" PRIu64 " results=%zu(", op->callee,
          op->callee_result_count);
      for (size_t i = 0; i < op->arg_count; i++) {
        put(o, i == 0 ? "" : ", ");
        put_operand(o, &op->args[i]);
      }
      put(o, ")");
      put_state(o, f, op->state);
      break;
    case GRJIT_OP_CALL_NATIVE:
      put(o, "call_native #%u(", op->native);
      for (size_t i = 0; i < op->arg_count; i++) {
        put(o, i == 0 ? "" : ", ");
        put_operand(o, &op->args[i]);
      }
      put(o, ")");
      put_state(o, f, op->state);
      if (op->exit_state != GRJIT_NO_STATE) {
        put(o, " after");
        put_state(o, f, op->exit_state);
      }
      break;
    case GRJIT_OP_POLL:
      put(o, "poll");
      put_state(o, f, op->state);
      break;
    case GRJIT_OP_GUARD:
      put(o, "guard ");
      put_operand(o, &op->a);
      put_state(o, f, op->state);
      break;
    case GRJIT_OP_BR:
      put(o, "br b%u", op->target);
      break;
    case GRJIT_OP_BR_IF:
      put(o, "br_if ");
      put_operand(o, &op->a);
      put(o, ", b%u, b%u", op->target, op->target_else);
      break;
    case GRJIT_OP_BR_TABLE:
      put(o, "br_table ");
      put_operand(o, &op->a);
      put(o, " [");
      for (size_t i = 0; i < op->target_count; i++) {
        put(o, "%sb%u", i == 0 ? "" : ", ", op->targets[i]);
      }
      put(o, "], default b%u", op->default_target);
      break;
    case GRJIT_OP_TRAP:
      put(o, "trap %u", (unsigned)op->trap_kind);
      break;
    case GRJIT_OP_RETHROW:
      put(o, "rethrow");
      break;
    case GRJIT_OP_RET:
      put(o, "ret");
      if (op->result_count != 0) {
        put(o, " ");
        for (size_t i = 0; i < op->result_count; i++) {
          put(o, "%s", i == 0 ? "" : ", ");
          put_operand(o, &op->returns[i]);
        }
      }
      break;
    default:
      put(o, "%s ", op_name(op->kind));
      put_operand(o, &op->a);
      put(o, ", ");
      put_operand(o, &op->b);
      break;
  }
  put(o, "\n");
}

GRJIT_Result grjit_function_print(const GRJIT_Function * function,
    char * buffer, size_t size, size_t * out_length) {
  if (function == NULL || out_length == NULL ||
      (buffer == NULL && size != 0)) {
    return GRJIT_ERR_INVALID;
  }
  Out o = {buffer, size, 0};
  put(&o, "function %s(", function->name);
  for (size_t i = 0; i < function->param_count; i++) {
    put(&o, "%sv%zu:%s", i == 0 ? "" : ", ", i, type_name(function->vregs[i].type));
  }
  put(&o, ") slots=%zu%s\n", function->interp_slots,
      function->callable ? " callable" : "");
  if (function->vreg_count > function->param_count) {
    put(&o, "  vregs:");
    for (size_t i = function->param_count; i < function->vreg_count; i++) {
      put(&o, " v%zu:%s", i, type_name(function->vregs[i].type));
    }
    put(&o, "\n");
  }
  for (size_t i = 0; i < function->vreg_count; i++) {
    if (function->vregs[i].derived) {
      put(&o, "  derived v%zu = v%u%+" PRId64 "\n", i, function->vregs[i].base,
          function->vregs[i].delta);
    }
  }
  for (size_t b = 0; b < function->block_count; b++) {
    put(&o, "b%zu:\n", b);
    for (size_t i = 0; i < function->blocks[b].count; i++) {
      put_op(&o, function, &function->blocks[b].ops[i]);
    }
  }
  *out_length = o.length;
  if (o.buffer != NULL && o.size != 0 && o.length >= o.size) {
    o.buffer[o.size - 1] = '\0';
  }
  return GRJIT_OK;
}
