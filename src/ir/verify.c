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

static bool width_ok(uint32_t w) {
  return w == 8 || w == 16 || w == 32 || w == 64;
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

static GRJIT_Result check_state(
    Verify * v, const GRJIT_Op * op, GRJIT_BlockId block, size_t index) {
  if (op->state == GRJIT_NO_STATE || op->state >= v->f->state_count) {
    return refuse(v, GRJIT_ERR_INVALID,
        "block b%u op %zu: a site without a frame state", block, index);
  }
  const GRJIT_FrameState * s = &v->f->states[op->state];
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

static bool gc_site_kind(GRCORE_CodeSiteKind k) {
  return k == GRCORE_SITE_GC_POINT_POLL || k == GRCORE_SITE_GC_POINT_ALLOC_SLOW ||
         k == GRCORE_SITE_GC_POINT_CALL || k == GRCORE_SITE_GC_POINT_FRAME_PUSH ||
         k == GRCORE_SITE_GC_POINT_NESTED_ENTRY;
}

static GRJIT_Result check_op(
    Verify * v, const GRJIT_Op * op, GRJIT_BlockId block, size_t index) {
  GRJIT_Result r;
  switch (op->kind) {
    case GRJIT_OP_CONST:
      if (op->a.kind != GRJIT_OPERAND_IMM) {
        return refuse(v, GRJIT_ERR_INVALID,
            "block b%u op %zu: const needs an immediate", block, index);
      }
      return check_dst(v, op, false, block, index);
    case GRJIT_OP_MOVE:
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
      if ((r = check_operand(v, "left", &op->a, false, block, index)) !=
              GRJIT_OK ||
          (r = check_operand(v, "right", &op->b, false, block, index)) !=
              GRJIT_OK) {
        return r;
      }
      return check_dst(v, op, true, block, index);
    case GRJIT_OP_NEG:
    case GRJIT_OP_NOT:
      if ((r = check_operand(v, "operand", &op->a, false, block, index)) !=
          GRJIT_OK) {
        return r;
      }
      return check_dst(v, op, true, block, index);
    case GRJIT_OP_CMP: {
      if ((unsigned)op->cmp >= (unsigned)GRJIT_CMP_COUNT) {
        return refuse(v, GRJIT_ERR_INVALID,
            "block b%u op %zu: unknown comparison", block, index);
      }
      bool ref_ok = op->cmp == GRJIT_CMP_EQ || op->cmp == GRJIT_CMP_NE;
      if ((r = check_operand(v, "left", &op->a, ref_ok, block, index)) !=
              GRJIT_OK ||
          (r = check_operand(v, "right", &op->b, ref_ok, block, index)) !=
              GRJIT_OK ||
          (r = check_dst(v, op, false, block, index)) != GRJIT_OK) {
        return r;
      }
      if (type_of(v, op->dst) != GRJIT_TYPE_I64) {
        return refuse(v, GRJIT_ERR_INVALID,
            "block b%u op %zu: a comparison produces an i64", block, index);
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
    case GRJIT_OP_LOAD:
    case GRJIT_OP_LOAD_S:
    case GRJIT_OP_STORE: {
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
            type_of(v, op->b.vreg) == GRJIT_TYPE_REF) {
          return refuse(v, GRJIT_ERR_INVALID,
              "block b%u op %zu: a store of a reference is not expressible; "
              "stores into the heap go through the engine's helper",
              block, index);
        }
        return check_operand(v, "stored value", &op->b, false, block, index);
      }
      return check_dst(v, op, false, block, index);
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
        const GRJIT_Operand * o = &op->args[i];
        if (o->kind == GRJIT_OPERAND_VREG) {
          if (!vreg_ok(v, o->vreg)) {
            return refuse(v, GRJIT_ERR_INVALID,
                "block b%u op %zu: argument %zu names v%u, which the "
                "function does not have",
                block, index, i, o->vreg);
          }
        } else if (o->kind != GRJIT_OPERAND_IMM) {
          return refuse(v, GRJIT_ERR_INVALID,
              "block b%u op %zu: argument %zu is missing", block, index, i);
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
    case GRJIT_OP_POLL:
      if (v->f->poll_helper == NULL) {
        return refuse(v, GRJIT_ERR_INVALID,
            "block b%u op %zu: a poll in a function with no poll helper",
            block, index);
      }
      return check_state(v, op, block, index);
    case GRJIT_OP_GUARD:
      if ((r = check_operand(v, "condition", &op->a, false, block, index)) !=
          GRJIT_OK) {
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
      if ((r = check_operand(v, "condition", &op->a, false, block, index)) !=
          GRJIT_OK) {
        return r;
      }
      if (op->target >= v->f->block_count ||
          op->target_else >= v->f->block_count) {
        return refuse(v, GRJIT_ERR_INVALID,
            "block b%u op %zu: branch to a block that does not exist", block,
            index);
      }
      return GRJIT_OK;
    case GRJIT_OP_RET:
      if (op->a.kind == GRJIT_OPERAND_VREG && !vreg_ok(v, op->a.vreg)) {
        return refuse(v, GRJIT_ERR_INVALID,
            "block b%u op %zu: return of v%u, which does not exist", block,
            index, op->a.vreg);
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
  for (size_t r = 0; r < f->vreg_count; r++) {
    const GRJIT_VRegInfo * info = &f->vregs[r];
    if ((unsigned)info->type >= (unsigned)GRJIT_TYPE_COUNT) {
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

static void successor_list(const GRJIT_Function * f, const GRJIT_BlockInfo * b,
    GRJIT_BlockId out[2], size_t * count) {
  *count = 0;
  const GRJIT_Op * last = &b->ops[b->count - 1];
  (void)f;
  if (last->kind == GRJIT_OP_BR) {
    out[(*count)++] = last->target;
  } else if (last->kind == GRJIT_OP_BR_IF) {
    out[(*count)++] = last->target;
    out[(*count)++] = last->target_else;
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
        GRJIT_VReg d = grjit_op_def(&blk->ops[i]);
        if (d != GRJIT_NO_VREG) {
          cur[d / 64] |= UINT64_C(1) << (d % 64);
        }
      }
      GRJIT_BlockId succ[2];
      size_t sn;
      successor_list(f, blk, succ, &sn);
      for (size_t s = 0; s < sn; s++) {
        uint64_t * t = in + succ[s] * words;
        for (size_t w = 0; w < words; w++) {
          uint64_t n = t[w] & cur[w];
          if (n != t[w]) {
            t[w] = n;
            changed = true;
          }
        }
      }
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
      GRJIT_VReg d = grjit_op_def(&blk->ops[i]);
      if (d != GRJIT_NO_VREG) {
        cur[d / 64] |= UINT64_C(1) << (d % 64);
      }
    }
  }
  a->free_fn(a->ctx, in);
  return result;
}

/* A derived pointer that is live at a site needs its base live there too. */
static GRJIT_Result check_derived(Verify * v) {
  const GRJIT_Function * f = v->f;
  GRJIT_LiveSites live;
  GRJIT_Result r = grjit_liveness_compute(f, f->allocator, &live);
  if (r != GRJIT_OK) {
    return r;
  }
  for (size_t s = 0; s < live.count; s++) {
    const GRJIT_SiteLive * site = &live.sites[s];
    for (size_t i = 0; i < site->count; i++) {
      GRJIT_VReg d = site->vregs[i];
      if (!f->vregs[d].derived) {
        continue;
      }
      bool found = false;
      for (size_t k = 0; k < site->count; k++) {
        if (site->vregs[k] == f->vregs[d].base) {
          found = true;
          break;
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
  return check_derived(&v);
}
