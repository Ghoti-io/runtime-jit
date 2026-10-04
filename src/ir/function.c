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
 * The function's accessors, its destruction, and the two questions every pass
 * over the IR asks of an operation: what it reads and what it writes.
 */

#include <ghoti.io/runtime-jit/macros.h>

#include "ir_internal.h"

const char * grjit_function_name(const GRJIT_Function * function) {
  return function == NULL ? NULL : function->name;
}

size_t grjit_function_param_count(const GRJIT_Function * function) {
  return function == NULL ? 0 : function->param_count;
}

size_t grjit_function_interp_slot_count(const GRJIT_Function * function) {
  return function == NULL ? 0 : function->interp_slots;
}

size_t grjit_function_vreg_count(const GRJIT_Function * function) {
  return function == NULL ? 0 : function->vreg_count;
}

GRJIT_Type grjit_function_vreg_type(
    const GRJIT_Function * function, GRJIT_VReg vreg) {
  if (function == NULL || vreg >= function->vreg_count) {
    return GRJIT_TYPE_COUNT;
  }
  return function->vregs[vreg].type;
}

bool grjit_function_vreg_derived(const GRJIT_Function * function,
    GRJIT_VReg vreg, GRJIT_VReg * base, int64_t * delta) {
  if (function == NULL || vreg >= function->vreg_count ||
      !function->vregs[vreg].derived) {
    return false;
  }
  if (base != NULL) {
    *base = function->vregs[vreg].base;
  }
  if (delta != NULL) {
    *delta = function->vregs[vreg].delta;
  }
  return true;
}

size_t grjit_function_block_count(const GRJIT_Function * function) {
  return function == NULL ? 0 : function->block_count;
}

const GRJIT_Op * grjit_function_block_ops(
    const GRJIT_Function * function, GRJIT_BlockId block, size_t * count) {
  if (function == NULL || block >= function->block_count) {
    return NULL;
  }
  if (count != NULL) {
    *count = function->blocks[block].count;
  }
  return function->blocks[block].ops;
}

size_t grjit_function_frame_state_count(const GRJIT_Function * function) {
  return function == NULL ? 0 : function->state_count;
}

const GRJIT_FrameState * grjit_function_frame_state(
    const GRJIT_Function * function, uint32_t index) {
  if (function == NULL || index >= function->state_count) {
    return NULL;
  }
  return &function->states[index];
}

GRJIT_PollHelper grjit_function_poll_helper(const GRJIT_Function * function) {
  return function == NULL ? NULL : function->poll_helper;
}

size_t grjit_function_op_count(const GRJIT_Function * function) {
  return function == NULL ? 0 : function->op_count;
}

void grjit_function_destroy(GRJIT_Function * function) {
  if (function == NULL) {
    return;
  }
  const GRJIT_Allocator * a = function->allocator;
  for (size_t b = 0; b < function->block_count; b++) {
    GRJIT_BlockInfo * block = &function->blocks[b];
    for (size_t i = 0; i < block->count; i++) {
      if (block->ops[i].args != NULL) {
        a->free_fn(a->ctx, (void *)block->ops[i].args);
      }
    }
    if (block->ops != NULL) {
      a->free_fn(a->ctx, block->ops);
    }
  }
  for (size_t s = 0; s < function->state_count; s++) {
    if (function->states[s].slots != NULL) {
      a->free_fn(a->ctx, (void *)function->states[s].slots);
    }
  }
  a->free_fn(a->ctx, function->blocks);
  a->free_fn(a->ctx, function->states);
  a->free_fn(a->ctx, function->vregs);
  a->free_fn(a->ctx, function->name);
  a->free_fn(a->ctx, function);
}

const GRJIT_Allocator * grjit_allocator_or_default(
    const GRJIT_Allocator * allocator) {
  return allocator != NULL ? allocator : grjit_allocator_default();
}

bool grjit_op_is_terminator(GRJIT_OpKind kind) {
  return kind == GRJIT_OP_BR || kind == GRJIT_OP_BR_IF || kind == GRJIT_OP_RET;
}

bool grjit_op_has_state(const GRJIT_Op * op) {
  return op->kind == GRJIT_OP_POLL || op->kind == GRJIT_OP_GUARD ||
         (op->kind == GRJIT_OP_CALL && op->attr == GRJIT_CALL_GC_POINT);
}

static void visit_operand(
    const GRJIT_Operand * o, GRJIT_UseVisitor visit, void * user) {
  if (o->kind == GRJIT_OPERAND_VREG) {
    visit(user, o->vreg);
  }
}

void grjit_op_visit_uses(const GRJIT_Function * function, const GRJIT_Op * op,
    GRJIT_UseVisitor visit, void * user) {
  switch (op->kind) {
    case GRJIT_OP_CONST:
    case GRJIT_OP_BR:
      break;
    case GRJIT_OP_CALL:
      for (size_t i = 0; i < op->arg_count; i++) {
        visit_operand(&op->args[i], visit, user);
      }
      break;
    default:
      visit_operand(&op->a, visit, user);
      visit_operand(&op->b, visit, user);
      break;
  }
  if (grjit_op_has_state(op) && op->state != GRJIT_NO_STATE &&
      op->state < function->state_count) {
    const GRJIT_FrameState * s = &function->states[op->state];
    for (size_t i = 0; i < s->slot_count; i++) {
      if (s->slots[i].kind == GRJIT_FRAME_SLOT_VREG) {
        visit(user, s->slots[i].vreg);
      }
    }
  }
}

GRJIT_VReg grjit_op_def(const GRJIT_Op * op) {
  switch (op->kind) {
    case GRJIT_OP_STORE:
    case GRJIT_OP_POLL:
    case GRJIT_OP_GUARD:
    case GRJIT_OP_BR:
    case GRJIT_OP_BR_IF:
    case GRJIT_OP_RET:
      return GRJIT_NO_VREG;
    default:
      return op->dst;
  }
}
