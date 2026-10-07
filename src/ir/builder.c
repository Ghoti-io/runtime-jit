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
 * The IR builder. It records and enforces the limits; the verifier judges.
 * Every append is all-or-nothing: allocation happens first, the counts change
 * last, so an out-of-memory failure leaves the builder as it was.
 */

#include <ghoti.io/runtime-jit/macros.h>

#include "ir_internal.h"

#include <string.h>

/* Grows `array` (capacity `*capacity`, `elem` bytes each) to hold at least
 * `need` elements and returns it, or returns NULL on failure with the array
 * and the capacity untouched. */
static void * grow(const GRJIT_Allocator * a, void * array, size_t * capacity,
    size_t need, size_t elem) {
  if (need <= *capacity) {
    return array;
  }
  size_t cap = *capacity == 0 ? 4 : *capacity;
  while (cap < need) {
    if (cap > SIZE_MAX / 2 / elem) {
      return NULL;
    }
    cap *= 2;
  }
  void * grown = a->realloc_fn(a->ctx, array, cap * elem);
  if (grown == NULL) {
    return NULL;
  }
  *capacity = cap;
  return grown;
}

GRJIT_Result grjit_builder_create(const char * name, size_t interp_slot_count,
    const GRJIT_Limits * limits, const GRJIT_Allocator * allocator,
    GRJIT_Builder ** out_builder) {
  if (out_builder == NULL) {
    return GRJIT_ERR_INVALID;
  }
  const GRJIT_Allocator * a = grjit_allocator_or_default(allocator);
  GRJIT_Limits resolved;
  grjit_limits_resolve(limits, &resolved);
  if (interp_slot_count > resolved.max_frame_state_slots) {
    return GRJIT_ERR_LIMIT;
  }
  GRJIT_Builder * b = a->malloc_fn(a->ctx, sizeof *b);
  if (b == NULL) {
    return GRJIT_ERR_OOM;
  }
  GRJIT_Function * f = a->calloc_fn(a->ctx, 1, sizeof *f);
  if (f == NULL) {
    a->free_fn(a->ctx, b);
    return GRJIT_ERR_OOM;
  }
  const char * text = name != NULL ? name : "function";
  size_t n = strlen(text) + 1;
  f->name = a->malloc_fn(a->ctx, n);
  if (f->name == NULL) {
    a->free_fn(a->ctx, f);
    a->free_fn(a->ctx, b);
    return GRJIT_ERR_OOM;
  }
  memcpy(f->name, text, n);
  f->allocator = a;
  f->interp_slots = interp_slot_count;
  b->function = f;
  b->limits = resolved;
  b->has_current = false;
  b->current = 0;
  *out_builder = b;
  return GRJIT_OK;
}

void grjit_builder_destroy(GRJIT_Builder * builder) {
  if (builder == NULL) {
    return;
  }
  const GRJIT_Allocator * a = builder->function->allocator;
  grjit_function_destroy(builder->function);
  a->free_fn(a->ctx, builder);
}

GRJIT_Result grjit_builder_finish(
    GRJIT_Builder * builder, GRJIT_Function ** out_function) {
  if (builder == NULL || out_function == NULL) {
    return GRJIT_ERR_INVALID;
  }
  const GRJIT_Allocator * a = builder->function->allocator;
  *out_function = builder->function;
  builder->function = NULL;
  a->free_fn(a->ctx, builder);
  return GRJIT_OK;
}

static GRJIT_Result add_vreg(
    GRJIT_Builder * b, GRJIT_Type type, GRJIT_VReg * out, bool param) {
  GRJIT_Function * f = b->function;
  if (out == NULL || (unsigned)type >= (unsigned)GRJIT_TYPE_COUNT) {
    return GRJIT_ERR_INVALID;
  }
  if (param && f->vreg_count != f->param_count) {
    return GRJIT_ERR_INVALID;
  }
  if (f->vreg_count >= b->limits.max_vregs) {
    return GRJIT_ERR_LIMIT;
  }
  GRJIT_VRegInfo * vregs = grow(f->allocator, f->vregs, &f->vreg_capacity,
      f->vreg_count + 1, sizeof *f->vregs);
  if (vregs == NULL) {
    return GRJIT_ERR_OOM;
  }
  f->vregs = vregs;
  GRJIT_VRegInfo * v = &f->vregs[f->vreg_count];
  v->type = type;
  v->derived = false;
  v->base = GRJIT_NO_VREG;
  v->delta = 0;
  *out = (GRJIT_VReg)f->vreg_count;
  f->vreg_count++;
  if (param) {
    f->param_count++;
  }
  return GRJIT_OK;
}

GRJIT_Result grjit_builder_param(
    GRJIT_Builder * builder, GRJIT_Type type, GRJIT_VReg * out_vreg) {
  if (builder == NULL) {
    return GRJIT_ERR_INVALID;
  }
  return add_vreg(builder, type, out_vreg, true);
}

GRJIT_Result grjit_builder_vreg(
    GRJIT_Builder * builder, GRJIT_Type type, GRJIT_VReg * out_vreg) {
  if (builder == NULL) {
    return GRJIT_ERR_INVALID;
  }
  return add_vreg(builder, type, out_vreg, false);
}

GRJIT_Result grjit_builder_derived(
    GRJIT_Builder * builder, GRJIT_VReg vreg, GRJIT_VReg base, int64_t delta) {
  if (builder == NULL) {
    return GRJIT_ERR_INVALID;
  }
  GRJIT_Function * f = builder->function;
  if (vreg >= f->vreg_count || base >= f->vreg_count) {
    return GRJIT_ERR_INVALID;
  }
  f->vregs[vreg].derived = true;
  f->vregs[vreg].base = base;
  f->vregs[vreg].delta = delta;
  return GRJIT_OK;
}

GRJIT_Result grjit_builder_block(
    GRJIT_Builder * builder, GRJIT_BlockId * out_block) {
  if (builder == NULL || out_block == NULL) {
    return GRJIT_ERR_INVALID;
  }
  GRJIT_Function * f = builder->function;
  if (f->block_count >= builder->limits.max_blocks) {
    return GRJIT_ERR_LIMIT;
  }
  GRJIT_BlockInfo * blocks = grow(f->allocator, f->blocks, &f->block_capacity,
      f->block_count + 1, sizeof *f->blocks);
  if (blocks == NULL) {
    return GRJIT_ERR_OOM;
  }
  f->blocks = blocks;
  GRJIT_BlockInfo * blk = &f->blocks[f->block_count];
  blk->ops = NULL;
  blk->count = 0;
  blk->capacity = 0;
  *out_block = (GRJIT_BlockId)f->block_count;
  f->block_count++;
  return GRJIT_OK;
}

GRJIT_Result grjit_builder_set_block(
    GRJIT_Builder * builder, GRJIT_BlockId block) {
  if (builder == NULL || block >= builder->function->block_count) {
    return GRJIT_ERR_INVALID;
  }
  builder->has_current = true;
  builder->current = block;
  return GRJIT_OK;
}

GRJIT_Result grjit_builder_set_poll_helper(
    GRJIT_Builder * builder, GRJIT_PollHelper helper) {
  if (builder == NULL) {
    return GRJIT_ERR_INVALID;
  }
  builder->function->poll_helper = helper;
  return GRJIT_OK;
}

/* Appends `*op` to the current block, with its arguments and its frame states
 * copied: `state` (with `slots`) and, for a guest call, the second state the
 * call exits through (`state2` with `slots2`; otherwise NULL). Everything that
 * can fail is done before anything is committed. */
static GRJIT_Result append_ex(GRJIT_Builder * b, const GRJIT_Op * op,
    const GRJIT_Operand * args, size_t arg_count,
    const GRJIT_FrameState * state, const GRJIT_FrameSlot * slots,
    size_t slot_count, const GRJIT_FrameState * state2,
    const GRJIT_FrameSlot * slots2, size_t slot_count2) {
  GRJIT_Function * f = b->function;
  if (!b->has_current) {
    return GRJIT_ERR_INVALID;
  }
  if (f->op_count >= b->limits.max_operations) {
    return GRJIT_ERR_LIMIT;
  }
  /* A helper call carries at most `max_call_arguments`; a call to another
   * compiled function, whose arguments the internal convention puts on the
   * stack past the sixth, is capped by its own limit. */
  bool guest_call = op->kind == GRJIT_OP_CALL_SLOT || op->kind == GRJIT_OP_CALL_PTR;
  size_t arg_cap = guest_call ? b->limits.max_guest_call_arguments
                              : b->limits.max_call_arguments;
  if (arg_count > arg_cap || arg_count > GRJIT_BUILDER_MAX_ARGS) {
    return GRJIT_ERR_LIMIT;
  }
  if (slot_count > b->limits.max_frame_state_slots ||
      slot_count2 > b->limits.max_frame_state_slots) {
    return GRJIT_ERR_LIMIT;
  }
  if ((arg_count != 0 && args == NULL) ||
      (state != NULL && slot_count != 0 && slots == NULL) ||
      (state2 != NULL && slot_count2 != 0 && slots2 == NULL)) {
    return GRJIT_ERR_INVALID;
  }
  const GRJIT_Allocator * a = f->allocator;
  GRJIT_BlockInfo * blk = &f->blocks[b->current];
  GRJIT_Operand * arg_copy = NULL;
  GRJIT_FrameSlot * slot_copy = NULL;
  GRJIT_FrameSlot * slot_copy2 = NULL;
  if (arg_count != 0) {
    arg_copy = a->malloc_fn(a->ctx, arg_count * sizeof *arg_copy);
    if (arg_copy == NULL) {
      return GRJIT_ERR_OOM;
    }
    memcpy(arg_copy, args, arg_count * sizeof *arg_copy);
  }
  if (state != NULL && slot_count != 0) {
    slot_copy = a->malloc_fn(a->ctx, slot_count * sizeof *slot_copy);
    if (slot_copy == NULL) {
      a->free_fn(a->ctx, arg_copy);
      return GRJIT_ERR_OOM;
    }
    memcpy(slot_copy, slots, slot_count * sizeof *slot_copy);
  }
  if (state2 != NULL && slot_count2 != 0) {
    slot_copy2 = a->malloc_fn(a->ctx, slot_count2 * sizeof *slot_copy2);
    if (slot_copy2 == NULL) {
      a->free_fn(a->ctx, arg_copy);
      a->free_fn(a->ctx, slot_copy);
      return GRJIT_ERR_OOM;
    }
    memcpy(slot_copy2, slots2, slot_count2 * sizeof *slot_copy2);
  }
  size_t new_states = (state != NULL ? 1u : 0u) + (state2 != NULL ? 1u : 0u);
  GRJIT_Op * ops = grow(a, blk->ops, &blk->capacity, blk->count + 1,
      sizeof *blk->ops);
  if (ops != NULL) {
    blk->ops = ops;
  }
  GRJIT_FrameState * states = f->states;
  if (ops != NULL && new_states != 0) {
    states = grow(a, f->states, &f->state_capacity,
        f->state_count + new_states, sizeof *f->states);
    if (states != NULL) {
      f->states = states;
    }
  }
  if (ops == NULL || (new_states != 0 && states == NULL)) {
    a->free_fn(a->ctx, arg_copy);
    a->free_fn(a->ctx, slot_copy);
    a->free_fn(a->ctx, slot_copy2);
    return GRJIT_ERR_OOM;
  }
  GRJIT_Op * dst = &blk->ops[blk->count];
  *dst = *op;
  dst->args = arg_copy;
  dst->arg_count = arg_count;
  if (state != NULL) {
    GRJIT_FrameState * s = &f->states[f->state_count];
    s->identity = state->identity;
    s->slot_count = slot_count;
    s->slots = slot_copy;
    dst->state = (uint32_t)f->state_count;
    f->state_count++;
  } else {
    dst->state = GRJIT_NO_STATE;
  }
  if (state2 != NULL) {
    GRJIT_FrameState * s = &f->states[f->state_count];
    s->identity = state2->identity;
    s->slot_count = slot_count2;
    s->slots = slot_copy2;
    dst->exit_state = (uint32_t)f->state_count;
    f->state_count++;
  } else {
    dst->exit_state = GRJIT_NO_STATE;
  }
  blk->count++;
  f->op_count++;
  return GRJIT_OK;
}

static GRJIT_Result append(GRJIT_Builder * b, const GRJIT_Op * op,
    const GRJIT_Operand * args, size_t arg_count,
    const GRJIT_FrameState * state, const GRJIT_FrameSlot * slots,
    size_t slot_count) {
  return append_ex(b, op, args, arg_count, state, slots, slot_count, NULL, NULL, 0);
}

static GRJIT_Op blank(GRJIT_OpKind kind) {
  GRJIT_Op op;
  memset(&op, 0, sizeof op);
  op.kind = kind;
  op.dst = GRJIT_NO_VREG;
  op.a = grjit_operand_none();
  op.b = grjit_operand_none();
  op.state = GRJIT_NO_STATE;
  op.exit_state = GRJIT_NO_STATE;
  return op;
}

GRJIT_Result grjit_builder_const(
    GRJIT_Builder * builder, GRJIT_VReg dst, int64_t imm) {
  if (builder == NULL) {
    return GRJIT_ERR_INVALID;
  }
  GRJIT_Op op = blank(GRJIT_OP_CONST);
  op.dst = dst;
  op.a = grjit_operand_imm(imm);
  return append(builder, &op, NULL, 0, NULL, NULL, 0);
}

GRJIT_Result grjit_builder_move(
    GRJIT_Builder * builder, GRJIT_VReg dst, GRJIT_Operand src) {
  if (builder == NULL) {
    return GRJIT_ERR_INVALID;
  }
  GRJIT_Op op = blank(GRJIT_OP_MOVE);
  op.dst = dst;
  op.a = src;
  return append(builder, &op, NULL, 0, NULL, NULL, 0);
}

GRJIT_Result grjit_builder_bitcast(
    GRJIT_Builder * builder, GRJIT_VReg dst, GRJIT_VReg src) {
  if (builder == NULL) {
    return GRJIT_ERR_INVALID;
  }
  GRJIT_Op op = blank(GRJIT_OP_BITCAST);
  op.dst = dst;
  op.a = grjit_operand_vreg(src);
  return append(builder, &op, NULL, 0, NULL, NULL, 0);
}

GRJIT_Result grjit_builder_binary(GRJIT_Builder * builder, GRJIT_OpKind kind,
    GRJIT_VReg dst, GRJIT_Operand a, GRJIT_Operand b) {
  if (builder == NULL || kind < GRJIT_OP_ADD || kind > GRJIT_OP_SAR) {
    return GRJIT_ERR_INVALID;
  }
  GRJIT_Op op = blank(kind);
  op.dst = dst;
  op.a = a;
  op.b = b;
  return append(builder, &op, NULL, 0, NULL, NULL, 0);
}

GRJIT_Result grjit_builder_unary(GRJIT_Builder * builder, GRJIT_OpKind kind,
    GRJIT_VReg dst, GRJIT_Operand a) {
  if (builder == NULL || (kind != GRJIT_OP_NEG && kind != GRJIT_OP_NOT)) {
    return GRJIT_ERR_INVALID;
  }
  GRJIT_Op op = blank(kind);
  op.dst = dst;
  op.a = a;
  return append(builder, &op, NULL, 0, NULL, NULL, 0);
}

GRJIT_Result grjit_builder_cmp(GRJIT_Builder * builder, GRJIT_Cmp cmp,
    GRJIT_VReg dst, GRJIT_Operand a, GRJIT_Operand b) {
  if (builder == NULL) {
    return GRJIT_ERR_INVALID;
  }
  GRJIT_Op op = blank(GRJIT_OP_CMP);
  op.dst = dst;
  op.a = a;
  op.b = b;
  op.cmp = cmp;
  return append(builder, &op, NULL, 0, NULL, NULL, 0);
}

GRJIT_Result grjit_builder_load(GRJIT_Builder * builder, GRJIT_VReg dst,
    GRJIT_VReg base, int32_t disp, uint32_t width_bits, bool sign_extend) {
  if (builder == NULL) {
    return GRJIT_ERR_INVALID;
  }
  GRJIT_Op op = blank(sign_extend ? GRJIT_OP_LOAD_S : GRJIT_OP_LOAD);
  op.dst = dst;
  op.a = grjit_operand_vreg(base);
  op.disp = disp;
  op.width = width_bits;
  return append(builder, &op, NULL, 0, NULL, NULL, 0);
}

GRJIT_Result grjit_builder_store(GRJIT_Builder * builder, GRJIT_VReg base,
    int32_t disp, uint32_t width_bits, GRJIT_Operand value) {
  if (builder == NULL) {
    return GRJIT_ERR_INVALID;
  }
  GRJIT_Op op = blank(GRJIT_OP_STORE);
  op.a = grjit_operand_vreg(base);
  op.b = value;
  op.disp = disp;
  op.width = width_bits;
  return append(builder, &op, NULL, 0, NULL, NULL, 0);
}

GRJIT_Result grjit_builder_call(GRJIT_Builder * builder, GRJIT_VReg dst,
    uint64_t address, GRJIT_CallAttr attr, GRCORE_CodeSiteKind site_kind,
    const GRJIT_Operand * args, size_t arg_count, GRCORE_PollIdentity identity,
    const GRJIT_FrameSlot * state_slots, size_t state_count) {
  if (builder == NULL || (unsigned)attr >= (unsigned)GRJIT_CALL_ATTR_COUNT) {
    return GRJIT_ERR_INVALID;
  }
  GRJIT_Op op = blank(GRJIT_OP_CALL);
  op.dst = dst;
  op.address = address;
  op.attr = attr;
  op.site_kind = site_kind;
  GRJIT_FrameState state;
  memset(&state, 0, sizeof state);
  state.identity = identity;
  bool has_state = attr == GRJIT_CALL_GC_POINT;
  return append(builder, &op, args, arg_count, has_state ? &state : NULL,
      state_slots, has_state ? state_count : 0);
}

GRJIT_Result grjit_builder_set_callable(
    GRJIT_Builder * builder, const GRJIT_CallHooks * hooks) {
  if (builder == NULL || hooks == NULL) {
    return GRJIT_ERR_INVALID;
  }
  builder->function->callable = true;
  builder->function->hooks = *hooks;
  return GRJIT_OK;
}

GRJIT_Result grjit_builder_set_token(GRJIT_Builder * builder, uint64_t token) {
  if (builder == NULL) {
    return GRJIT_ERR_INVALID;
  }
  builder->function->token = token;
  return GRJIT_OK;
}

GRJIT_Result grjit_builder_call_slot(GRJIT_Builder * builder, GRJIT_VReg dst,
    uint64_t slot_address, uint64_t callee, const GRJIT_Operand * args,
    size_t arg_count, GRCORE_PollIdentity identity,
    const GRJIT_FrameSlot * state_slots, size_t state_count,
    GRCORE_PollIdentity exit_identity, const GRJIT_FrameSlot * exit_slots,
    size_t exit_count) {
  if (builder == NULL) {
    return GRJIT_ERR_INVALID;
  }
  GRJIT_Op op = blank(GRJIT_OP_CALL_SLOT);
  op.dst = dst;
  op.address = slot_address;
  op.callee = callee;
  op.attr = GRJIT_CALL_GC_POINT;
  op.site_kind = GRCORE_SITE_GC_POINT_CALL;
  GRJIT_FrameState state, exit_state;
  memset(&state, 0, sizeof state);
  memset(&exit_state, 0, sizeof exit_state);
  state.identity = identity;
  exit_state.identity = exit_identity;
  return append_ex(builder, &op, args, arg_count, &state, state_slots,
      state_count, &exit_state, exit_slots, exit_count);
}

GRJIT_Result grjit_builder_call_ptr(GRJIT_Builder * builder, GRJIT_VReg dst,
    GRJIT_Operand target, uint64_t callee, const GRJIT_Operand * args,
    size_t arg_count, GRCORE_PollIdentity identity,
    const GRJIT_FrameSlot * state_slots, size_t state_count,
    GRCORE_PollIdentity exit_identity, const GRJIT_FrameSlot * exit_slots,
    size_t exit_count) {
  if (builder == NULL) {
    return GRJIT_ERR_INVALID;
  }
  GRJIT_Op op = blank(GRJIT_OP_CALL_PTR);
  op.dst = dst;
  op.a = target;
  op.callee = callee;
  op.attr = GRJIT_CALL_GC_POINT;
  op.site_kind = GRCORE_SITE_GC_POINT_CALL;
  GRJIT_FrameState state, exit_state;
  memset(&state, 0, sizeof state);
  memset(&exit_state, 0, sizeof exit_state);
  state.identity = identity;
  exit_state.identity = exit_identity;
  return append_ex(builder, &op, args, arg_count, &state, state_slots,
      state_count, &exit_state, exit_slots, exit_count);
}

GRJIT_Result grjit_builder_poll(GRJIT_Builder * builder,
    GRCORE_PollIdentity identity, const GRJIT_FrameSlot * state_slots,
    size_t state_count) {
  if (builder == NULL) {
    return GRJIT_ERR_INVALID;
  }
  GRJIT_Op op = blank(GRJIT_OP_POLL);
  GRJIT_FrameState state;
  memset(&state, 0, sizeof state);
  state.identity = identity;
  return append(builder, &op, NULL, 0, &state, state_slots, state_count);
}

GRJIT_Result grjit_builder_guard(GRJIT_Builder * builder, GRJIT_Operand cond,
    GRCORE_PollIdentity identity, const GRJIT_FrameSlot * state_slots,
    size_t state_count) {
  if (builder == NULL) {
    return GRJIT_ERR_INVALID;
  }
  GRJIT_Op op = blank(GRJIT_OP_GUARD);
  op.a = cond;
  GRJIT_FrameState state;
  memset(&state, 0, sizeof state);
  state.identity = identity;
  return append(builder, &op, NULL, 0, &state, state_slots, state_count);
}

GRJIT_Result grjit_builder_br(GRJIT_Builder * builder, GRJIT_BlockId target) {
  if (builder == NULL) {
    return GRJIT_ERR_INVALID;
  }
  GRJIT_Op op = blank(GRJIT_OP_BR);
  op.target = target;
  return append(builder, &op, NULL, 0, NULL, NULL, 0);
}

GRJIT_Result grjit_builder_br_if(GRJIT_Builder * builder, GRJIT_Operand cond,
    GRJIT_BlockId then_block, GRJIT_BlockId else_block) {
  if (builder == NULL) {
    return GRJIT_ERR_INVALID;
  }
  GRJIT_Op op = blank(GRJIT_OP_BR_IF);
  op.a = cond;
  op.target = then_block;
  op.target_else = else_block;
  return append(builder, &op, NULL, 0, NULL, NULL, 0);
}

GRJIT_Result grjit_builder_ret(GRJIT_Builder * builder, GRJIT_Operand value) {
  if (builder == NULL) {
    return GRJIT_ERR_INVALID;
  }
  GRJIT_Op op = blank(GRJIT_OP_RET);
  op.a = value;
  return append(builder, &op, NULL, 0, NULL, NULL, 0);
}
