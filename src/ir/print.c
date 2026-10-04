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
    case GRJIT_TYPE_COUNT:
      break;
  }
  return "?";
}

static const char * op_name(GRJIT_OpKind k) {
  static const char * const names[] = {"const", "move", "add", "sub", "mul",
      "and", "or", "xor", "shl", "shr", "sar", "neg", "not", "cmp", "load",
      "load_s", "store", "call", "poll", "guard", "br", "br_if", "ret"};
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
    default:
      put(o, "_");
      break;
  }
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
  if (op->disp < 0) {
    put(o, "-%" PRId64, -(int64_t)op->disp);
  } else {
    put(o, "+%" PRId64, (int64_t)op->disp);
  }
  put(o, "]");
}

static void put_op(Out * o, const GRJIT_Function * f, const GRJIT_Op * op) {
  put(o, "  ");
  GRJIT_VReg def = grjit_op_def(op);
  if (def != GRJIT_NO_VREG) {
    put(o, "v%u = ", def);
  }
  switch (op->kind) {
    case GRJIT_OP_CONST:
      put(o, "const %" PRId64, op->a.imm);
      break;
    case GRJIT_OP_MOVE:
    case GRJIT_OP_NEG:
    case GRJIT_OP_NOT:
      put(o, "%s ", op_name(op->kind));
      put_operand(o, &op->a);
      break;
    case GRJIT_OP_CMP:
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
      if (op->attr == GRJIT_CALL_GC_POINT) {
        put_state(o, f, op->state);
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
    case GRJIT_OP_RET:
      put(o, "ret");
      if (op->a.kind != GRJIT_OPERAND_NONE) {
        put(o, " ");
        put_operand(o, &op->a);
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
  put(&o, ") slots=%zu\n", function->interp_slots);
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
