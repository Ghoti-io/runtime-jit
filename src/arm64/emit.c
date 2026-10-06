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
 * The arm64 baseline emitter: one operation at a time, every value in its
 * frame slot, fixed caller-saved scratch registers (see emit_internal.h). The
 * semantics are the IR's, not the instruction set's: shifts take the count
 * modulo 64 (`lslv`, `lsrv` and `asrv` already do), `MUL` wraps, a comparison
 * yields 0 or 1, loads of 8, 16 and 32 bits zero- or sign-extend as `LOAD` and
 * `LOAD_S` say, narrow stores store the low bits, an immediate is any 64-bit
 * value and a displacement is any `int32`.
 */

#include <ghoti.io/runtime-jit/macros.h>

#include "emit_internal.h"

#include "../ir/ir_internal.h"

#include <string.h>

#define A GRJIT_A64Asm
#define XR(n) GRJIT_A64_X##n

static const GRJIT_A64Reg arg_regs[GRJIT_BACKEND_MAX_ARGS] = {
    GRJIT_A64_X0, GRJIT_A64_X1, GRJIT_A64_X2, GRJIT_A64_X3, GRJIT_A64_X4,
    GRJIT_A64_X5};

/* A value from the frame: the frame base plus a (negative) slot offset, which
 * is one `ldur` for the first 32 slots and a materialised offset beyond. */
static void load_slot(GRJIT_A64Emit * e, GRJIT_A64Reg r, int32_t offset) {
  grjit_a64_load(&e->as, GRJIT_A64_MEM_X64, r, GRJIT_A64_FP, offset);
}

static void store_slot(GRJIT_A64Emit * e, GRJIT_A64Reg r, int32_t offset) {
  grjit_a64_store(&e->as, GRJIT_A64_MEM_X64, r, GRJIT_A64_FP, offset);
}

/* Loads an operand into a register: a register's slot, or an immediate. */
static void load_operand(GRJIT_A64Emit * e, GRJIT_A64Reg r, const GRJIT_Operand * o) {
  if (o->kind == GRJIT_OPERAND_VREG) {
    load_slot(e, r, grjit_emit_slot(o->vreg));
  } else {
    grjit_a64_mov_ri(&e->as, r, (uint64_t)o->imm);
  }
}

static void store_result(GRJIT_A64Emit * e, GRJIT_VReg dst, GRJIT_A64Reg r) {
  store_slot(e, r, grjit_emit_slot(dst));
}

static GRJIT_A64Cond cmp_cond(GRJIT_Cmp c) {
  switch (c) {
    case GRJIT_CMP_EQ: return GRJIT_A64_EQ;
    case GRJIT_CMP_NE: return GRJIT_A64_NE;
    case GRJIT_CMP_LT: return GRJIT_A64_LT;
    case GRJIT_CMP_LE: return GRJIT_A64_LE;
    case GRJIT_CMP_GT: return GRJIT_A64_GT;
    case GRJIT_CMP_GE: return GRJIT_A64_GE;
    case GRJIT_CMP_ULT: return GRJIT_A64_LO;
    case GRJIT_CMP_ULE: return GRJIT_A64_LS;
    case GRJIT_CMP_UGT: return GRJIT_A64_HI;
    case GRJIT_CMP_UGE:
    case GRJIT_CMP_COUNT:
      break;
  }
  return GRJIT_A64_HS;
}

static void emit_load(GRJIT_A64Emit * e, const GRJIT_Op * op, bool sign) {
  load_operand(e, GRJIT_A64_X1, &op->a);
  GRJIT_A64Mem kind;
  switch (op->width) {
    case 8: kind = sign ? GRJIT_A64_MEM_S8 : GRJIT_A64_MEM_U8; break;
    case 16: kind = sign ? GRJIT_A64_MEM_S16 : GRJIT_A64_MEM_U16; break;
    case 32: kind = sign ? GRJIT_A64_MEM_S32 : GRJIT_A64_MEM_U32; break;
    default: kind = GRJIT_A64_MEM_X64; break;
  }
  grjit_a64_load(&e->as, kind, GRJIT_A64_X0, GRJIT_A64_X1, op->disp);
  store_result(e, op->dst, GRJIT_A64_X0);
}

static void emit_store(GRJIT_A64Emit * e, const GRJIT_Op * op) {
  load_operand(e, GRJIT_A64_X1, &op->a);
  load_operand(e, GRJIT_A64_X0, &op->b);
  GRJIT_A64Mem kind;
  switch (op->width) {
    case 8: kind = GRJIT_A64_MEM_U8; break;
    case 16: kind = GRJIT_A64_MEM_U16; break;
    case 32: kind = GRJIT_A64_MEM_U32; break;
    default: kind = GRJIT_A64_MEM_X64; break;
  }
  grjit_a64_store(&e->as, kind, GRJIT_A64_X0, GRJIT_A64_X1, op->disp);
}

static void emit_binary(GRJIT_A64Emit * e, const GRJIT_Op * op) {
  A * a = &e->as;
  load_operand(e, GRJIT_A64_X0, &op->a);
  load_operand(e, GRJIT_A64_X1, &op->b);
  GRJIT_OpKind kind = op->kind;
#if defined(GRJIT_TEST_PLANT_BUG) && GRJIT_TEST_PLANT_BUG == 4
  /* Planted defect 4 (tests only): a logical and an arithmetic right shift
   * trade places in the arm64 emitter. The differential must catch it under
   * qemu-aarch64. */
  if (kind == GRJIT_OP_SHR) {
    kind = GRJIT_OP_SAR;
  } else if (kind == GRJIT_OP_SAR) {
    kind = GRJIT_OP_SHR;
  }
#endif
  GRJIT_A64Alu alu;
  switch (kind) {
    case GRJIT_OP_ADD: alu = GRJIT_A64_ADD; break;
    case GRJIT_OP_SUB: alu = GRJIT_A64_SUB; break;
    case GRJIT_OP_MUL: alu = GRJIT_A64_MUL; break;
    case GRJIT_OP_AND: alu = GRJIT_A64_AND; break;
    case GRJIT_OP_OR: alu = GRJIT_A64_ORR; break;
    case GRJIT_OP_XOR: alu = GRJIT_A64_EOR; break;
    case GRJIT_OP_SHL: alu = GRJIT_A64_LSLV; break;
    case GRJIT_OP_SHR: alu = GRJIT_A64_LSRV; break;
    default: alu = GRJIT_A64_ASRV; break;
  }
  grjit_a64_alu(a, alu, GRJIT_A64_X0, GRJIT_A64_X0, GRJIT_A64_X1);
  store_result(e, op->dst, GRJIT_A64_X0);
}

static void emit_call(GRJIT_A64Emit * e, const GRJIT_Op * op) {
  A * a = &e->as;
  for (size_t i = 0; i < op->arg_count; i++) {
    load_operand(e, arg_regs[i], &op->args[i]);
  }
  grjit_a64_mov_ri(a, GRJIT_A64_X16, op->address);
  grjit_a64_blr(a, GRJIT_A64_X16);
  if (op->attr == GRJIT_CALL_GC_POINT) {
    /* The return address is the site. */
    const GRJIT_FrameState * s = &e->c.f->states[op->state];
    grjit_emit_add_site(&e->c, (uint32_t)grjit_a64_size(a), op->site_kind,
        s->identity, e->c.live_cursor++, op->state);
  }
  if (op->dst != GRJIT_NO_VREG) {
    store_result(e, op->dst, GRJIT_A64_X0);
  }
}

static void emit_op(GRJIT_A64Emit * e, const GRJIT_Op * op, size_t block) {
  A * a = &e->as;
  switch (op->kind) {
    case GRJIT_OP_CONST:
    case GRJIT_OP_MOVE:
    case GRJIT_OP_BITCAST:
      load_operand(e, GRJIT_A64_X0, &op->a);
      store_result(e, op->dst, GRJIT_A64_X0);
      break;
    case GRJIT_OP_ADD:
    case GRJIT_OP_SUB:
    case GRJIT_OP_MUL:
    case GRJIT_OP_AND:
    case GRJIT_OP_OR:
    case GRJIT_OP_XOR:
    case GRJIT_OP_SHL:
    case GRJIT_OP_SHR:
    case GRJIT_OP_SAR:
      emit_binary(e, op);
      break;
    case GRJIT_OP_NEG:
    case GRJIT_OP_NOT:
      load_operand(e, GRJIT_A64_X0, &op->a);
      if (op->kind == GRJIT_OP_NEG) {
        grjit_a64_neg(a, GRJIT_A64_X0, GRJIT_A64_X0);
      } else {
        grjit_a64_mvn(a, GRJIT_A64_X0, GRJIT_A64_X0);
      }
      store_result(e, op->dst, GRJIT_A64_X0);
      break;
    case GRJIT_OP_CMP:
      load_operand(e, GRJIT_A64_X0, &op->a);
      load_operand(e, GRJIT_A64_X1, &op->b);
      grjit_a64_cmp(a, GRJIT_A64_X0, GRJIT_A64_X1);
      grjit_a64_cset(a, GRJIT_A64_X0, cmp_cond(op->cmp));
      store_result(e, op->dst, GRJIT_A64_X0);
      break;
    case GRJIT_OP_LOAD:
      emit_load(e, op, false);
      break;
    case GRJIT_OP_LOAD_S:
      emit_load(e, op, true);
      break;
    case GRJIT_OP_STORE:
      emit_store(e, op);
      break;
    case GRJIT_OP_CALL:
      emit_call(e, op);
      break;
    case GRJIT_OP_POLL: {
      /* ldr x0,[ctx]; ldr x0,[x0+request_word]; cbnz x0, slow */
      GRJIT_Pending p;
      p.kind = GRJIT_PENDING_POLL;
      p.entry = grjit_a64_label(a);
      p.back = grjit_a64_label(a);
      p.op = op;
      p.live_index = e->c.live_cursor++;
      load_slot(e, GRJIT_A64_X0, GRJIT_SLOT_CTX);
      grjit_a64_load(a, GRJIT_A64_MEM_X64, GRJIT_A64_X0, GRJIT_A64_X0,
          (int32_t)e->c.request_offset);
      grjit_a64_cbnz(a, GRJIT_A64_X0, p.entry);
      grjit_a64_bind(a, p.back);
      grjit_emit_add_pending(&e->c, &p);
      break;
    }
    case GRJIT_OP_GUARD: {
      GRJIT_Pending p;
      p.kind = GRJIT_PENDING_GUARD;
      p.entry = grjit_a64_label(a);
      p.back = 0;
      p.op = op;
      p.live_index = e->c.live_cursor++;
      load_operand(e, GRJIT_A64_X0, &op->a);
      grjit_a64_cbz(a, GRJIT_A64_X0, p.entry);
      grjit_emit_add_pending(&e->c, &p);
      break;
    }
    case GRJIT_OP_BR:
      if (op->target != block + 1) {
        grjit_a64_b(a, e->blocks[op->target]);
      }
      break;
    case GRJIT_OP_BR_IF:
      load_operand(e, GRJIT_A64_X0, &op->a);
      if (op->target_else == block + 1) {
        grjit_a64_cbnz(a, GRJIT_A64_X0, e->blocks[op->target]);
      } else if (op->target == block + 1) {
        grjit_a64_cbz(a, GRJIT_A64_X0, e->blocks[op->target_else]);
      } else {
        grjit_a64_cbnz(a, GRJIT_A64_X0, e->blocks[op->target]);
        grjit_a64_b(a, e->blocks[op->target_else]);
      }
      break;
    case GRJIT_OP_RET:
      if (op->a.kind != GRJIT_OPERAND_NONE) {
        load_operand(e, GRJIT_A64_X0, &op->a);
        load_slot(e, GRJIT_A64_X2, GRJIT_SLOT_OUT);
        grjit_a64_store(a, GRJIT_A64_MEM_X64, GRJIT_A64_X0, GRJIT_A64_X2, 0);
      }
      grjit_a64_mov_ri(a, GRJIT_A64_X0, GRJIT_EXIT_RETURNED);
      grjit_a64_emit_epilogue(e);
      break;
    case GRJIT_OP_CALL_SLOT:
    case GRJIT_OP_CALL_PTR:
      /* Refused before emission (grjit_emit_for), so never reached. */
    case GRJIT_OP_COUNT:
      e->c.error = GRJIT_ERR_INTERNAL;
      break;
  }
}

void grjit_a64_emit_epilogue(GRJIT_A64Emit * e) {
  A * a = &e->as;
  grjit_a64_mov_sp(a, GRJIT_A64_SP, GRJIT_A64_FP);
  grjit_a64_pop_frame_record(a);
  grjit_a64_ret(a);
}

/* One assembly of the function. */
static GRJIT_Result emit_pass(const GRJIT_Function * f, GRJIT_A64Emit * e,
    size_t max_code_bytes, bool long_branches) {
  const GRJIT_Allocator * allocator = e->c.allocator;
  GRJIT_A64Asm * a = &e->as;
  grjit_a64_init(a, allocator, max_code_bytes);
  grjit_a64_set_long_branches(a, long_branches);
  e->blocks = allocator->malloc_fn(allocator->ctx, f->block_count * sizeof *e->blocks);
  if (e->blocks == NULL) {
    return GRJIT_ERR_OOM;
  }
  for (size_t b = 0; b < f->block_count; b++) {
    e->blocks[b] = grjit_a64_label(a);
  }
  e->refuse = grjit_a64_label(a);

  /* Prologue: frame record, frame, the three fixed slots, the entry hook, the
   * parameters. */
  grjit_a64_push_frame_record(a);
  grjit_a64_mov_sp(a, GRJIT_A64_FP, GRJIT_A64_SP);
  grjit_a64_sub_sp(a, e->c.frame_bytes);
  store_slot(e, GRJIT_A64_X0, GRJIT_SLOT_CTX);
  store_slot(e, GRJIT_A64_X2, GRJIT_SLOT_OUT);
  store_slot(e, GRJIT_A64_X1, GRJIT_SLOT_ARGS);
  if (e->c.hook != NULL) {
    grjit_a64_mov_ri(a, GRJIT_A64_X16, (uint64_t)(uintptr_t)e->c.hook);
    grjit_a64_blr(a, GRJIT_A64_X16);
    /* The hook returns a uint32_t; the upper half of x0 is not defined. */
    grjit_a64_mov32_rr(a, GRJIT_A64_X0, GRJIT_A64_X0);
    grjit_a64_cbnz(a, GRJIT_A64_X0, e->refuse);
  }
  if (f->param_count != 0) {
    load_slot(e, GRJIT_A64_X1, GRJIT_SLOT_ARGS);
    for (size_t i = 0; i < f->param_count; i++) {
      grjit_a64_load(a, GRJIT_A64_MEM_X64, GRJIT_A64_X0, GRJIT_A64_X1,
          (int32_t)(8 * i));
      store_slot(e, GRJIT_A64_X0, grjit_emit_slot((GRJIT_VReg)i));
    }
  }

  for (size_t b = 0; b < f->block_count && e->c.error == GRJIT_OK; b++) {
    grjit_a64_bind(a, e->blocks[b]);
    const GRJIT_BlockInfo * blk = &f->blocks[b];
    for (size_t i = 0; i < blk->count; i++) {
      emit_op(e, &blk->ops[i], b);
    }
  }
  /* A pending stub may add sites but never another pending stub. */
  for (size_t i = 0; i < e->c.pending_count && e->c.error == GRJIT_OK; i++) {
    if (e->c.pending[i].kind == GRJIT_PENDING_POLL) {
      grjit_a64_emit_poll_stub(e, &e->c.pending[i]);
    } else {
      grjit_a64_emit_guard_stub(e, &e->c.pending[i]);
    }
  }
  grjit_a64_emit_refuse_stub(e);
  grjit_a64_finish(a);
  if (e->c.error != GRJIT_OK) {
    return e->c.error;
  }
  switch (grjit_a64_status(a)) {
    case GRJIT_A64_OK:
      return GRJIT_OK;
    case GRJIT_A64_OOM:
      return GRJIT_ERR_OOM;
    case GRJIT_A64_LIMIT:
      return GRJIT_ERR_LIMIT;
    default:
      return GRJIT_ERR_INTERNAL;
  }
}

/* Drops what one pass made so another can start (the common state is
 * rebuilt). */
static void reset_pass(GRJIT_A64Emit * e, const GRJIT_Function * f,
    const GRJIT_Allocator * allocator, GRJIT_EntryHook hook,
    uint32_t request_offset, uint32_t frame_bytes, const GRJIT_LiveSites * live) {
  allocator->free_fn(allocator->ctx, e->blocks);
  e->blocks = NULL;
  grjit_a64_free(&e->as);
  grjit_emit_common_free(&e->c);
  grjit_emit_common_init(&e->c, f, allocator, hook, request_offset, frame_bytes, live);
}

GRJIT_Result grjit_a64_emit_function(const GRJIT_Function * f,
    const GRJIT_Allocator * allocator, size_t max_code_bytes,
    GRJIT_EntryHook hook, uint32_t request_offset, uint32_t frame_bytes,
    const GRJIT_LiveSites * live, GRJIT_A64Emit * e) {
  memset(e, 0, sizeof *e);
  grjit_emit_common_init(&e->c, f, allocator, hook, request_offset, frame_bytes, live);
  GRJIT_Result r = emit_pass(f, e, max_code_bytes, false);
  if (r == GRJIT_ERR_INTERNAL && grjit_a64_status(&e->as) == GRJIT_A64_FAR) {
    /* Some forward conditional branch is more than 1 MiB from its label. */
    reset_pass(e, f, allocator, hook, request_offset, frame_bytes, live);
    r = emit_pass(f, e, max_code_bytes, true);
  }
  if (r != GRJIT_OK) {
    return r;
  }
  grjit_emit_sort_sites(&e->c);
  return GRJIT_OK;
}

void grjit_a64_emit_free(GRJIT_A64Emit * e) {
  if (e->c.allocator == NULL) {
    return;
  }
  e->c.allocator->free_fn(e->c.allocator->ctx, e->blocks);
  grjit_a64_free(&e->as);
  grjit_emit_common_free(&e->c);
  memset(e, 0, sizeof *e);
}
