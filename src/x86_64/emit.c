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
 * The baseline emitter: one operation at a time, every value in its frame
 * slot, fixed caller-saved scratch registers (rax, rcx, rdx, rsi, rdi, r8,
 * r9). No GC reference is ever left in a callee-saved register, which is
 * AD-17's rule held by construction rather than checked.
 *
 * The register roles are fixed: `rax` and `rcx` carry the operands of an
 * operation, `rdx` the `out` pointer in the exits, and `rdi`, `rsi`, `rdx`,
 * `rcx`, `r8`, `r9` the arguments of a helper call (SysV).
 */

#include <ghoti.io/runtime-jit/macros.h>

#include "emit_internal.h"

#include "../ir/ir_internal.h"

#include <ghoti.io/runtime-core/a/compiled.h>

#include <stdlib.h>
#include <string.h>

/* The registers that carry a helper's arguments. SysV: all six of the IR's.
 * Win64: the first four; the fifth and sixth go at [rsp + 32] and [rsp + 40],
 * above the callee's shadow space. */
static const GRJIT_Reg arg_regs[GRJIT_BACKEND_MAX_ARGS] = {
    GRJIT_RDI, GRJIT_RSI, GRJIT_RDX, GRJIT_RCX, GRJIT_R8, GRJIT_R9};
static const GRJIT_Reg win64_arg_regs[4] = {
    GRJIT_RCX, GRJIT_RDX, GRJIT_R8, GRJIT_R9};
#define GRJIT_WIN64_REG_ARGS 4

/* Loads an operand into a register: a register's slot, or an immediate. */
static void load_operand(GRJIT_Emit * e, GRJIT_Reg r, const GRJIT_Operand * o) {
  if (o->kind == GRJIT_OPERAND_VREG) {
    grjit_asm_load64(&e->as, r, GRJIT_RBP, grjit_emit_slot(o->vreg));
  } else {
    grjit_asm_mov_ri(&e->as, r, (uint64_t)o->imm);
  }
}

static void store_result(GRJIT_Emit * e, GRJIT_VReg dst, GRJIT_Reg r) {
  grjit_asm_store64(&e->as, GRJIT_RBP, grjit_emit_slot(dst), r);
}

static GRJIT_Cond cmp_cond(GRJIT_Cmp c) {
  switch (c) {
    case GRJIT_CMP_EQ:
      return GRJIT_COND_E;
    case GRJIT_CMP_NE:
      return GRJIT_COND_NE;
    case GRJIT_CMP_LT:
      return GRJIT_COND_L;
    case GRJIT_CMP_LE:
      return GRJIT_COND_LE;
    case GRJIT_CMP_GT:
      return GRJIT_COND_G;
    case GRJIT_CMP_GE:
      return GRJIT_COND_GE;
    case GRJIT_CMP_ULT:
      return GRJIT_COND_B;
    case GRJIT_CMP_ULE:
      return GRJIT_COND_BE;
    case GRJIT_CMP_UGT:
      return GRJIT_COND_A;
    case GRJIT_CMP_UGE:
    case GRJIT_CMP_COUNT:
      break;
  }
  return GRJIT_COND_AE;
}

static void emit_load(GRJIT_Emit * e, const GRJIT_Op * op, bool sign) {
  GRJIT_Asm * a = &e->as;
  load_operand(e, GRJIT_RCX, &op->a);
  switch (op->width) {
    case 8:
      (sign ? grjit_asm_load8s : grjit_asm_load8u)(a, GRJIT_RAX, GRJIT_RCX, op->disp);
      break;
    case 16:
      (sign ? grjit_asm_load16s : grjit_asm_load16u)(a, GRJIT_RAX, GRJIT_RCX, op->disp);
      break;
    case 32:
      (sign ? grjit_asm_load32s : grjit_asm_load32u)(a, GRJIT_RAX, GRJIT_RCX, op->disp);
      break;
    default:
      grjit_asm_load64(a, GRJIT_RAX, GRJIT_RCX, op->disp);
      break;
  }
  store_result(e, op->dst, GRJIT_RAX);
}

static void emit_store(GRJIT_Emit * e, const GRJIT_Op * op) {
  GRJIT_Asm * a = &e->as;
  load_operand(e, GRJIT_RCX, &op->a);
  load_operand(e, GRJIT_RAX, &op->b);
  switch (op->width) {
    case 8:
      grjit_asm_store8(a, GRJIT_RCX, op->disp, GRJIT_RAX);
      break;
    case 16:
      grjit_asm_store16(a, GRJIT_RCX, op->disp, GRJIT_RAX);
      break;
    case 32:
      grjit_asm_store32(a, GRJIT_RCX, op->disp, GRJIT_RAX);
      break;
    default:
      grjit_asm_store64(a, GRJIT_RCX, op->disp, GRJIT_RAX);
      break;
  }
}

static void emit_binary(GRJIT_Emit * e, const GRJIT_Op * op) {
  GRJIT_Asm * a = &e->as;
  load_operand(e, GRJIT_RAX, &op->a);
  load_operand(e, GRJIT_RCX, &op->b);
  GRJIT_OpKind kind = op->kind;
#if defined(GRJIT_TEST_PLANT_BUG) && GRJIT_TEST_PLANT_BUG == 1
  /* Planted defect 1 (tests only): a logical and an arithmetic right shift
   * trade places. The differential must catch it. */
  if (kind == GRJIT_OP_SHR) {
    kind = GRJIT_OP_SAR;
  } else if (kind == GRJIT_OP_SAR) {
    kind = GRJIT_OP_SHR;
  }
#endif
  switch (kind) {
    case GRJIT_OP_ADD:
      grjit_asm_alu_rr(a, GRJIT_ALU_ADD, GRJIT_RAX, GRJIT_RCX);
      break;
    case GRJIT_OP_SUB:
      grjit_asm_alu_rr(a, GRJIT_ALU_SUB, GRJIT_RAX, GRJIT_RCX);
      break;
    case GRJIT_OP_MUL:
      grjit_asm_imul_rr(a, GRJIT_RAX, GRJIT_RCX);
      break;
    case GRJIT_OP_AND:
      grjit_asm_alu_rr(a, GRJIT_ALU_AND, GRJIT_RAX, GRJIT_RCX);
      break;
    case GRJIT_OP_OR:
      grjit_asm_alu_rr(a, GRJIT_ALU_OR, GRJIT_RAX, GRJIT_RCX);
      break;
    case GRJIT_OP_XOR:
      grjit_asm_alu_rr(a, GRJIT_ALU_XOR, GRJIT_RAX, GRJIT_RCX);
      break;
    case GRJIT_OP_SHL:
      grjit_asm_shl_cl(a, GRJIT_RAX);
      break;
    case GRJIT_OP_SHR:
      grjit_asm_shr_cl(a, GRJIT_RAX);
      break;
    default:
      grjit_asm_sar_cl(a, GRJIT_RAX);
      break;
  }
  store_result(e, op->dst, GRJIT_RAX);
}

static void emit_call(GRJIT_Emit * e, const GRJIT_Op * op) {
  GRJIT_Asm * a = &e->as;
  /* In a callable function every call that can reach a GC point records where
   * the walk starts first (AD-28), so a collection under the helper sees this
   * frame and the compiled frames below it. It uses rax and rcx, which the
   * arguments are loaded over afterwards. */
  GRJIT_Label walk_ret = 0;
  bool record = e->c.callable && op->attr == GRJIT_CALL_GC_POINT;
  if (record) {
    walk_ret = grjit_asm_label(a);
    grjit_emit_store_walk_cell(e, walk_ret);
  }
  if (e->win64) {
    /* The stack arguments first, through rax: they are stores to the outgoing
     * area, and the register arguments are loads straight from slots, so no
     * order between them can clobber an operand. */
    for (size_t i = GRJIT_WIN64_REG_ARGS; i < op->arg_count; i++) {
      load_operand(e, GRJIT_RAX, &op->args[i]);
      grjit_asm_store64(a, GRJIT_RSP,
          (int32_t)(32 + 8 * (i - GRJIT_WIN64_REG_ARGS)), GRJIT_RAX);
    }
    for (size_t i = 0; i < op->arg_count && i < GRJIT_WIN64_REG_ARGS; i++) {
      load_operand(e, win64_arg_regs[i], &op->args[i]);
    }
  } else {
    for (size_t i = 0; i < op->arg_count; i++) {
      load_operand(e, arg_regs[i], &op->args[i]);
    }
  }
  grjit_asm_mov_ri(a, GRJIT_RAX, op->address);
  grjit_asm_call_r(a, GRJIT_RAX);
  if (record) {
    grjit_asm_bind(a, walk_ret);
  }
  if (op->attr == GRJIT_CALL_GC_POINT) {
    /* The return address is the site. */
    const GRJIT_FrameState * s = &e->c.f->states[op->state];
    grjit_emit_add_site(&e->c, (uint32_t)grjit_asm_size(a), op->site_kind,
        s->identity, e->c.live_cursor++, op->state);
  }
  if (op->dst != GRJIT_NO_VREG) {
    store_result(e, op->dst, GRJIT_RAX);
  }
}

/* A call to another compiled function (AD-28), through an entry slot or a code
 * pointer. In order: find the entry (the slot's word, or the checked pointer);
 * copy the arguments into the frame's arguments area, where the push hook reads
 * them and a moving collector updates them; push the callee's guest frame (the
 * frame-push GC point); call through the internal convention; test the status
 * for DEOPTED; store the result; pop. Every way out before the call is an
 * exit through the exit state; every way out after it is DEOPTED. */
static void emit_guest_call(GRJIT_Emit * e, const GRJIT_Op * op) {
  GRJIT_Asm * a = &e->as;
  const GRJIT_Function * f = e->c.f;
  const GRJIT_FrameState * st = &f->states[op->state];
  const bool slot = op->kind == GRJIT_OP_CALL_SLOT;
  const size_t n = op->arg_count;
  const int32_t entry_slot = GRJIT_ENTRY_SAVE_SLOT(f->vreg_count, e->c.shape);
  const size_t live = e->c.live_cursor;
  e->c.live_cursor += 3; /* the push, the call and the exit */

  GRJIT_Label exit = grjit_asm_label(a);
  GRJIT_Pending p;
  memset(&p, 0, sizeof p);
  p.op = op;
  if (slot) {
    GRJIT_Label slow = grjit_asm_label(a);
    GRJIT_Label back = grjit_asm_label(a);
    grjit_asm_mov_ri(a, GRJIT_RAX, op->address);
    grjit_asm_load64(a, GRJIT_RAX, GRJIT_RAX, 0);
    /* One compare tells compiled code (above one) from empty (zero) and from
     * refused (one). */
    grjit_asm_cmp_ri(a, GRJIT_RAX, (int32_t)GRCORE_ENTRY_REFUSED);
    grjit_asm_jcc(a, GRJIT_COND_BE, slow);
    grjit_asm_bind(a, back);
    p.kind = GRJIT_PENDING_CALL_SLOW;
    p.entry = slow;
    p.back = back;
    p.exit = exit;
    p.live_index = live;
    grjit_emit_add_pending(&e->c, &p);
  } else {
    load_operand(e, GRJIT_RAX, &op->a);
    grjit_asm_store64(a, GRJIT_RBP, entry_slot, GRJIT_RAX);
    /* The target must be the internal entry of code registered in this context:
     * an address that is not is never entered. */
    grjit_asm_load64(a, GRJIT_RDI, GRJIT_RBP, GRJIT_SLOT_CTX);
    grjit_asm_mov_rr(a, GRJIT_RSI, GRJIT_RAX);
    grjit_asm_mov_ri(a, GRJIT_RAX, (uint64_t)(uintptr_t)grjit_call_target_ok);
    grjit_asm_call_r(a, GRJIT_RAX);
    grjit_asm_mov32_rr(a, GRJIT_RAX, GRJIT_RAX);
    grjit_asm_test_rr(a, GRJIT_RAX, GRJIT_RAX);
    grjit_asm_jcc(a, GRJIT_COND_E, exit);
  }
  memset(&p, 0, sizeof p);
  p.kind = GRJIT_PENDING_CALL_EXIT;
  p.entry = exit;
  p.op = op;
  p.live_index = live + 2;
  grjit_emit_add_pending(&e->c, &p);
  if (slot) {
    grjit_asm_store64(a, GRJIT_RBP, entry_slot, GRJIT_RAX);
  }

  /* The arguments, into the area. */
  for (size_t i = 0; i < n; i++) {
    load_operand(e, GRJIT_RAX, &op->args[i]);
    grjit_asm_store64(a, GRJIT_RBP, GRJIT_ARGS_SLOT(f->vreg_count, e->c.shape.args_area, i), GRJIT_RAX);
  }

  /* The push: the callee's guest frame, counted as the interpreter counts it. */
  GRJIT_Label push_ret = grjit_asm_label(a);
  grjit_emit_store_walk_cell(e, push_ret);
  grjit_asm_mov_rr(a, GRJIT_RDI, GRJIT_RCX);
  grjit_asm_mov_ri(a, GRJIT_RSI, op->callee);
  grjit_asm_lea(a, GRJIT_RDX, GRJIT_RBP, GRJIT_ARGS_SLOT(f->vreg_count, e->c.shape.args_area, 0));
  grjit_asm_mov_ri(a, GRJIT_RCX, n);
  grjit_asm_mov_ri(a, GRJIT_RAX, (uint64_t)(uintptr_t)f->hooks.push);
  grjit_asm_call_r(a, GRJIT_RAX);
  grjit_asm_bind(a, push_ret);
  grjit_emit_add_site_for(&e->c, (uint32_t)grjit_asm_size(a),
      GRCORE_SITE_GC_POINT_FRAME_PUSH, st->identity, live, op->state, op);
  grjit_asm_mov32_rr(a, GRJIT_RAX, GRJIT_RAX);
  grjit_asm_test_rr(a, GRJIT_RAX, GRJIT_RAX);
  grjit_asm_jcc(a, GRJIT_COND_NE, exit);

  /* The call. The stack arguments are pushed just before it and popped by the
   * callee, which keeps the stack 16-aligned (the area is whole 16-byte units). */
  const uint32_t stack_args = n > GRJIT_INTERNAL_REG_ARGS ? (uint32_t)(n - GRJIT_INTERNAL_REG_ARGS) : 0;
  const uint32_t area = (stack_args * 8 + 15) / 16 * 16;
  if (area != 0) {
    grjit_asm_sub_rsp(a, area);
    for (size_t i = GRJIT_INTERNAL_REG_ARGS; i < n; i++) {
      grjit_asm_load64(a, GRJIT_RAX, GRJIT_RBP, GRJIT_ARGS_SLOT(f->vreg_count, e->c.shape.args_area, i));
      grjit_asm_store64(a, GRJIT_RSP,
          (int32_t)(8 * (i - GRJIT_INTERNAL_REG_ARGS)), GRJIT_RAX);
    }
  }
  for (size_t i = 0; i < n && i < GRJIT_INTERNAL_REG_ARGS; i++) {
    grjit_asm_load64(a, arg_regs[i], GRJIT_RBP, GRJIT_ARGS_SLOT(f->vreg_count, e->c.shape.args_area, i));
  }
  grjit_asm_load64(a, GRJIT_R10, GRJIT_RBP, GRJIT_SLOT_CTX);
  grjit_asm_load64(a, GRJIT_RAX, GRJIT_RBP, entry_slot);
  grjit_asm_call_r(a, GRJIT_RAX);
  /* The return address is the site: this frame, with the callee running. */
  grjit_emit_add_site_for(&e->c, (uint32_t)grjit_asm_size(a),
      GRCORE_SITE_GC_POINT_CALL, st->identity, live + 1, op->state, op);
  grjit_asm_test_rr(a, GRJIT_RDX, GRJIT_RDX);
  grjit_asm_jcc(a, GRJIT_COND_NE, e->ret_deopted);
  if (op->dst != GRJIT_NO_VREG) {
    store_result(e, op->dst, GRJIT_RAX);
  }
  /* The call is complete: pop the callee's guest frame. */
  grjit_asm_load64(a, GRJIT_RDI, GRJIT_RBP, GRJIT_SLOT_CTX);
  grjit_asm_mov_ri(a, GRJIT_RAX, (uint64_t)(uintptr_t)f->hooks.pop);
  grjit_asm_call_r(a, GRJIT_RAX);
}

static void emit_op(GRJIT_Emit * e, const GRJIT_Op * op, size_t block) {
  GRJIT_Asm * a = &e->as;
  switch (op->kind) {
    case GRJIT_OP_CONST:
    case GRJIT_OP_MOVE:
    case GRJIT_OP_BITCAST:
      load_operand(e, GRJIT_RAX, &op->a);
      store_result(e, op->dst, GRJIT_RAX);
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
      load_operand(e, GRJIT_RAX, &op->a);
      if (op->kind == GRJIT_OP_NEG) {
        grjit_asm_neg(a, GRJIT_RAX);
      } else {
        grjit_asm_not(a, GRJIT_RAX);
      }
      store_result(e, op->dst, GRJIT_RAX);
      break;
    case GRJIT_OP_CMP:
      load_operand(e, GRJIT_RAX, &op->a);
      load_operand(e, GRJIT_RCX, &op->b);
      grjit_asm_alu_rr(a, GRJIT_ALU_CMP, GRJIT_RAX, GRJIT_RCX);
      grjit_asm_setcc(a, cmp_cond(op->cmp), GRJIT_RAX);
      grjit_asm_movzx_r8(a, GRJIT_RAX, GRJIT_RAX);
      store_result(e, op->dst, GRJIT_RAX);
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
      /* mov rax,[ctx]; mov rax,[rax+request_word]; test; jnz slow */
      GRJIT_Pending p;
      p.kind = GRJIT_PENDING_POLL;
      p.entry = grjit_asm_label(a);
      p.back = grjit_asm_label(a);
      p.op = op;
      p.live_index = e->c.live_cursor++;
      grjit_asm_load64(a, GRJIT_RAX, GRJIT_RBP, GRJIT_SLOT_CTX);
      grjit_asm_load64(a, GRJIT_RAX, GRJIT_RAX, (int32_t)e->c.request_offset);
      grjit_asm_test_rr(a, GRJIT_RAX, GRJIT_RAX);
      grjit_asm_jcc(a, GRJIT_COND_NE, p.entry);
      grjit_asm_bind(a, p.back);
      grjit_emit_add_pending(&e->c, &p);
      break;
    }
    case GRJIT_OP_GUARD: {
      GRJIT_Pending p;
      p.kind = GRJIT_PENDING_GUARD;
      p.entry = grjit_asm_label(a);
      p.back = 0;
      p.op = op;
      p.live_index = e->c.live_cursor++;
      load_operand(e, GRJIT_RAX, &op->a);
      grjit_asm_test_rr(a, GRJIT_RAX, GRJIT_RAX);
      grjit_asm_jcc(a, GRJIT_COND_E, p.entry);
      grjit_emit_add_pending(&e->c, &p);
      break;
    }
    case GRJIT_OP_BR:
      if (op->target != block + 1) {
        grjit_asm_jmp(a, e->blocks[op->target]);
      }
      break;
    case GRJIT_OP_BR_IF:
      load_operand(e, GRJIT_RAX, &op->a);
      grjit_asm_test_rr(a, GRJIT_RAX, GRJIT_RAX);
      if (op->target_else == block + 1) {
        grjit_asm_jcc(a, GRJIT_COND_NE, e->blocks[op->target]);
      } else if (op->target == block + 1) {
        grjit_asm_jcc(a, GRJIT_COND_E, e->blocks[op->target_else]);
      } else {
        grjit_asm_jcc(a, GRJIT_COND_NE, e->blocks[op->target]);
        grjit_asm_jmp(a, e->blocks[op->target_else]);
      }
      break;
    case GRJIT_OP_RET:
      if (e->c.callable) {
        /* The internal convention: the result in rax (zero for none), the
         * status in rdx, and the callee pops its own stack arguments. */
        if (op->a.kind != GRJIT_OPERAND_NONE) {
          load_operand(e, GRJIT_RAX, &op->a);
        } else {
          grjit_asm_mov_ri(a, GRJIT_RAX, 0);
        }
        grjit_asm_mov_ri(a, GRJIT_RDX, GRJIT_STATUS_RETURNED);
        grjit_emit_callable_epilogue(e);
        break;
      }
      if (op->a.kind != GRJIT_OPERAND_NONE) {
        load_operand(e, GRJIT_RAX, &op->a);
        grjit_asm_load64(a, GRJIT_RDX, GRJIT_RBP, GRJIT_SLOT_OUT);
        grjit_asm_store64(a, GRJIT_RDX, 0, GRJIT_RAX);
      }
      grjit_asm_mov_ri(a, GRJIT_RAX, GRJIT_EXIT_RETURNED);
      grjit_emit_epilogue(e);
      break;
    case GRJIT_OP_CALL_SLOT:
    case GRJIT_OP_CALL_PTR:
      emit_guest_call(e, op);
      break;
    case GRJIT_OP_COUNT:
      e->c.error = GRJIT_ERR_INTERNAL;
      break;
  }
}

void grjit_emit_epilogue(GRJIT_Emit * e) {
  GRJIT_Asm * a = &e->as;
  if (e->win64) {
    /* The forms a Win64 unwinder recognises when it stops inside an epilogue:
     * `lea rsp, [fp + 0]` for a frame with a frame register, then the pops,
     * then `ret`. `leave` does the same work and is not one of them. */
    grjit_asm_lea(a, GRJIT_RSP, GRJIT_RBP, 0);
    grjit_asm_pop(a, GRJIT_RBP);
    grjit_asm_ret(a);
  } else {
    grjit_asm_leave(a);
    grjit_asm_ret(a);
  }
}

/* Win64: a frame of a page or more is probed a page at a time, downwards from
 * the return address, before rsp moves, so that the guard page is touched in
 * order and the stack grows to cover the frame (a single access further down
 * than the guard page is a stack overflow). The last probe is exactly at the
 * new rsp. r10 and r11 are volatile on Win64. */
static void emit_stack_probe(GRJIT_Emit * e, uint32_t alloc) {
  GRJIT_Asm * a = &e->as;
  GRJIT_Label loop = grjit_asm_label(a);
  GRJIT_Label tail = grjit_asm_label(a);
  grjit_asm_lea(a, GRJIT_R10, GRJIT_RSP, -(int32_t)alloc);
  grjit_asm_lea(a, GRJIT_R11, GRJIT_RSP, 8);
  grjit_asm_bind(a, loop);
  grjit_asm_lea(a, GRJIT_R11, GRJIT_R11, -(int32_t)GRJIT_WIN64_PAGE);
  grjit_asm_alu_rr(a, GRJIT_ALU_CMP, GRJIT_R11, GRJIT_R10);
  grjit_asm_jcc(a, GRJIT_COND_BE, tail);
  grjit_asm_probe(a, GRJIT_R11, 0);
  grjit_asm_jmp(a, loop);
  grjit_asm_bind(a, tail);
  grjit_asm_probe(a, GRJIT_R10, 0);
}

/* The stubs the blocks queued. A stub may add sites but never another stub. */
static void emit_pending(GRJIT_Emit * e) {
  for (size_t i = 0; i < e->c.pending_count && e->c.error == GRJIT_OK; i++) {
    const GRJIT_Pending * p = &e->c.pending[i];
    switch (p->kind) {
      case GRJIT_PENDING_POLL:
        grjit_emit_poll_stub(e, p);
        break;
      case GRJIT_PENDING_GUARD:
        grjit_emit_guard_stub(e, p);
        break;
      case GRJIT_PENDING_CALL_SLOW:
        grjit_emit_call_slow_stub(e, p);
        break;
      case GRJIT_PENDING_CALL_EXIT:
        grjit_emit_call_exit_stub(e, p);
        break;
    }
  }
}

/* The entry adapter of a callable function (AD-28): the C ABI of
 * ::GRJIT_EntryFn on one side and the internal convention on the other. It
 * saves the caller's `rbp`, keeps `out` and the context in its own frame,
 * runs the entry hook, loads the arguments from the `args` array into the
 * registers and the stack area, sets `rbp` to the chain-end marker (so the
 * first compiled frame's saved caller base is the marker, which ends a walk;
 * a/compiled.h), calls the internal entry, and turns its status into an exit.
 * It uses no callee-saved register but `rbp`, which it restores. It clears the
 * walk-start cell on the way out, so a walk after the run reads no dead frames. */
static void emit_adapter(GRJIT_Emit * e) {
  GRJIT_Asm * a = &e->as;
  const GRJIT_Function * f = e->c.f;
  const size_t params = f->param_count;
  const uint32_t in_bytes = e->c.shape.incoming_bytes;
  GRJIT_Label deopted = grjit_asm_label(a);
  GRJIT_Label done = grjit_asm_label(a);
  grjit_asm_push(a, GRJIT_RBP);
  grjit_asm_mov_rr(a, GRJIT_RBP, GRJIT_RSP);
  grjit_asm_sub_rsp(a, 32);
  grjit_asm_store64(a, GRJIT_RSP, 24, GRJIT_RDX); /* out */
  grjit_asm_store64(a, GRJIT_RSP, 16, GRJIT_RDI); /* context */
  grjit_asm_store64(a, GRJIT_RSP, 8, GRJIT_RSI);  /* args */
  if (e->c.hook != NULL) {
    grjit_asm_mov_ri(a, GRJIT_RAX, (uint64_t)(uintptr_t)e->c.hook);
    grjit_asm_call_r(a, GRJIT_RAX);
    grjit_asm_mov32_rr(a, GRJIT_RAX, GRJIT_RAX);
    grjit_asm_test_rr(a, GRJIT_RAX, GRJIT_RAX);
    grjit_asm_jcc(a, GRJIT_COND_NE, e->refuse);
  }
  grjit_asm_load64(a, GRJIT_R11, GRJIT_RSP, 8);
  if (in_bytes != 0) {
    grjit_asm_sub_rsp(a, in_bytes);
    for (size_t i = GRJIT_INTERNAL_REG_ARGS; i < params; i++) {
      grjit_asm_load64(a, GRJIT_RAX, GRJIT_R11, (int32_t)(8 * i));
      grjit_asm_store64(a, GRJIT_RSP,
          (int32_t)(8 * (i - GRJIT_INTERNAL_REG_ARGS)), GRJIT_RAX);
    }
  }
  for (size_t i = 0; i < params && i < GRJIT_INTERNAL_REG_ARGS; i++) {
    grjit_asm_load64(a, arg_regs[i], GRJIT_R11, (int32_t)(8 * i));
  }
  grjit_asm_load64(a, GRJIT_R10, GRJIT_RSP, (int32_t)(in_bytes + 16));
  grjit_asm_mov_ri64(a, GRJIT_RBP, (uint64_t)GRCORE_COMPILED_CHAIN_END);
  grjit_asm_lea_rip(a, GRJIT_RAX, e->internal);
  grjit_asm_call_r(a, GRJIT_RAX);
  /* rsp is where it was after the frame was made: the callee popped the stack
   * arguments. */
  grjit_asm_load64(a, GRJIT_RCX, GRJIT_RSP, 24);
  grjit_asm_load64(a, GRJIT_R8, GRJIT_RSP, 16);
  grjit_asm_alu_rr(a, GRJIT_ALU_XOR, GRJIT_R9, GRJIT_R9);
  grjit_asm_store64(a, GRJIT_R8, (int32_t)e->c.walk_cell_offset, GRJIT_R9);
  grjit_asm_store64(a, GRJIT_R8, (int32_t)e->c.walk_cell_offset + 8, GRJIT_R9);
  grjit_asm_test_rr(a, GRJIT_RDX, GRJIT_RDX);
  grjit_asm_jcc(a, GRJIT_COND_NE, deopted);
  grjit_asm_store64(a, GRJIT_RCX, 0, GRJIT_RAX);
  grjit_asm_mov_ri(a, GRJIT_RAX, GRJIT_EXIT_RETURNED);
  grjit_asm_jmp(a, done);
  /* DEOPTED: every compiled frame was rebuilt into its guest frame before any
   * of them returned; out[0] is the cause. */
  grjit_asm_bind(a, deopted);
  grjit_asm_store64(a, GRJIT_RCX, 0, GRJIT_RAX);
  grjit_asm_mov_ri(a, GRJIT_RAX, GRJIT_EXIT_DEOPT);
  grjit_asm_bind(a, done);
  grjit_asm_add_rsp(a, 32);
  grjit_asm_pop(a, GRJIT_RBP);
  grjit_asm_ret(a);
  /* The entry hook refused (eax is its answer). */
  grjit_asm_bind(a, e->refuse);
  grjit_asm_load64(a, GRJIT_RCX, GRJIT_RSP, 24);
  grjit_asm_store64(a, GRJIT_RCX, 0, GRJIT_RAX);
  grjit_asm_mov_ri(a, GRJIT_RAX, GRJIT_EXIT_REFUSED);
  grjit_asm_add_rsp(a, 32);
  grjit_asm_pop(a, GRJIT_RBP);
  grjit_asm_ret(a);
}

/* A callable function: the adapter, the tag, the internal entry with the
 * stack-limit check, the blocks and the stubs. */
static GRJIT_Result emit_callable(const GRJIT_Function * f, GRJIT_Emit * e,
    uint32_t frame_bytes) {
  GRJIT_Asm * a = &e->as;
  e->internal = grjit_asm_label(a);
  e->overflow = grjit_asm_label(a);
  e->ret_deopted = grjit_asm_label(a);
  emit_adapter(e);
  /* The internal entry on a 16-byte boundary with the tag just before it. */
  static const uint8_t trap = 0xCC;
  while ((grjit_asm_size(a) + 8) % 16 != 0) {
    grjit_asm_raw(a, &trap, 1);
  }
  const uint64_t tag = GRJIT_ENTRY_TAG;
  grjit_asm_raw(a, &tag, sizeof tag);
  grjit_asm_bind(a, e->internal);
  e->internal_offset = (uint32_t)grjit_asm_size(a);

  const uint32_t alloc = frame_bytes;
  grjit_asm_push(a, GRJIT_RBP);
  grjit_asm_mov_rr(a, GRJIT_RBP, GRJIT_RSP);
  /* The native-stack check, in bytes (AD-28): the lowest address this frame
   * will use, against the context's limit word (r10 holds the context). Below
   * it, the chain deopts at the call site and the interpreter continues. */
  grjit_asm_lea(a, GRJIT_RAX, GRJIT_RSP, -(int32_t)alloc);
  grjit_asm_cmp_rm(a, GRJIT_RAX, GRJIT_R10, (int32_t)e->c.native_limit_offset);
  grjit_asm_jcc(a, GRJIT_COND_B, e->overflow);
  grjit_asm_sub_rsp(a, alloc);
  grjit_asm_store64(a, GRJIT_RBP, GRJIT_SLOT_CTX, GRJIT_R10);
  for (size_t i = 0; i < f->param_count; i++) {
    if (i < GRJIT_INTERNAL_REG_ARGS) {
      grjit_asm_store64(a, GRJIT_RBP, grjit_emit_slot((GRJIT_VReg)i), arg_regs[i]);
    } else {
      grjit_asm_load64(a, GRJIT_RAX, GRJIT_RBP,
          (int32_t)(16 + 8 * (i - GRJIT_INTERNAL_REG_ARGS)));
      grjit_asm_store64(a, GRJIT_RBP, grjit_emit_slot((GRJIT_VReg)i), GRJIT_RAX);
    }
  }
  for (size_t b = 0; b < f->block_count && e->c.error == GRJIT_OK; b++) {
    grjit_asm_bind(a, e->blocks[b]);
    const GRJIT_BlockInfo * blk = &f->blocks[b];
    for (size_t i = 0; i < blk->count; i++) {
      emit_op(e, &blk->ops[i], b);
    }
  }
  emit_pending(e);
  grjit_emit_ret_deopted(e);
  grjit_emit_overflow_stub(e);
  return e->c.error;
}

GRJIT_Result grjit_emit_function(const GRJIT_Function * f,
    const GRJIT_Allocator * allocator, size_t max_code_bytes,
    GRJIT_EntryHook hook, uint32_t request_offset, uint32_t frame_bytes,
    bool win64, const GRJIT_LiveSites * live, GRJIT_Emit * e) {
  memset(e, 0, sizeof *e);
  e->win64 = win64;
  grjit_emit_common_init(
      &e->c, f, allocator, hook, request_offset, frame_bytes, live);
  GRJIT_Asm * a = &e->as;
  grjit_asm_init(a, allocator, max_code_bytes);
  e->blocks = allocator->malloc_fn(allocator->ctx, f->block_count * sizeof *e->blocks);
  if (e->blocks == NULL) {
    return GRJIT_ERR_OOM;
  }
  for (size_t b = 0; b < f->block_count; b++) {
    e->blocks[b] = grjit_asm_label(a);
  }
  e->refuse = grjit_asm_label(a);

  if (f->callable) {
    GRJIT_Result cr = emit_callable(f, e, frame_bytes);
    if (cr != GRJIT_OK) {
      return cr;
    }
    goto finish;
  }

  /* Prologue: frame, the three fixed slots, the entry hook, the parameters. */
  uint32_t alloc = frame_bytes;
  if (win64) {
#if defined(GRJIT_TEST_PLANT_BUG) && GRJIT_TEST_PLANT_BUG == 6
    /* Planted defect 6 (tests only): no outgoing area, so a callee's shadow
     * space and the stack arguments land on the frame's own slots. */
#else
    alloc += GRJIT_WIN64_OUTGOING;
#endif
  }
  if (win64 && alloc > (uint32_t)INT32_MAX - GRJIT_WIN64_PAGE) {
    /* The allocation and the probe displacements are signed 32-bit fields of
     * the encodings; a cap raised past 2 GiB is a limit, not a wrapped value. */
    return GRJIT_ERR_LIMIT;
  }
  grjit_asm_push(a, GRJIT_RBP);
  e->prologue.push_end = (uint32_t)grjit_asm_size(a);
  grjit_asm_mov_rr(a, GRJIT_RBP, GRJIT_RSP);
  e->prologue.setfp_end = (uint32_t)grjit_asm_size(a);
  if (win64 && (uint64_t)alloc + 8 > GRJIT_WIN64_PAGE) {
    emit_stack_probe(e, alloc);
  }
  grjit_asm_sub_rsp(a, alloc);
  e->prologue.alloc_end = (uint32_t)grjit_asm_size(a);
  e->prologue.alloc_bytes = alloc;
  if (win64) {
    /* Microsoft x64: context in rcx, args in rdx, out in r8. */
    grjit_asm_store64(a, GRJIT_RBP, GRJIT_SLOT_CTX, GRJIT_RCX);
    grjit_asm_store64(a, GRJIT_RBP, GRJIT_SLOT_OUT, GRJIT_R8);
    grjit_asm_store64(a, GRJIT_RBP, GRJIT_SLOT_ARGS, GRJIT_RDX);
  } else {
    grjit_asm_store64(a, GRJIT_RBP, GRJIT_SLOT_CTX, GRJIT_RDI);
    grjit_asm_store64(a, GRJIT_RBP, GRJIT_SLOT_OUT, GRJIT_RDX);
    grjit_asm_store64(a, GRJIT_RBP, GRJIT_SLOT_ARGS, GRJIT_RSI);
  }
  if (hook != NULL) {
    grjit_asm_mov_ri(a, GRJIT_RAX, (uint64_t)(uintptr_t)hook);
    grjit_asm_call_r(a, GRJIT_RAX);
    grjit_asm_mov32_rr(a, GRJIT_RAX, GRJIT_RAX);
    grjit_asm_test_rr(a, GRJIT_RAX, GRJIT_RAX);
    grjit_asm_jcc(a, GRJIT_COND_NE, e->refuse);
  }
  if (f->param_count != 0) {
    /* The pointer to the arguments is held in a scratch register: rsi on SysV,
     * where it is caller-saved, and r10 on Win64, where rsi is callee-saved. */
    GRJIT_Reg args_reg = win64 ? GRJIT_R10 : GRJIT_RSI;
#if defined(GRJIT_TEST_PLANT_BUG) && GRJIT_TEST_PLANT_BUG == 5
    /* Planted defect 5 (tests only): the SysV register, callee-saved on Win64,
     * is used and never restored. */
    args_reg = GRJIT_RSI;
#endif
    grjit_asm_load64(a, args_reg, GRJIT_RBP, GRJIT_SLOT_ARGS);
    for (size_t i = 0; i < f->param_count; i++) {
      grjit_asm_load64(a, GRJIT_RAX, args_reg, (int32_t)(8 * i));
      grjit_asm_store64(
          a, GRJIT_RBP, grjit_emit_slot((GRJIT_VReg)i), GRJIT_RAX);
    }
  }

  for (size_t b = 0; b < f->block_count && e->c.error == GRJIT_OK; b++) {
    grjit_asm_bind(a, e->blocks[b]);
    const GRJIT_BlockInfo * blk = &f->blocks[b];
    for (size_t i = 0; i < blk->count; i++) {
      emit_op(e, &blk->ops[i], b);
    }
  }
  /* A pending stub may add sites but never another pending stub. */
  emit_pending(e);
  grjit_emit_refuse_stub(e);
finish:
  grjit_asm_finish(a);
  if (e->c.error != GRJIT_OK) {
    return e->c.error;
  }
  switch (grjit_asm_status(a)) {
    case GRJIT_ASM_OK:
      break;
    case GRJIT_ASM_OOM:
      return GRJIT_ERR_OOM;
    case GRJIT_ASM_LIMIT:
      return GRJIT_ERR_LIMIT;
    default:
      return GRJIT_ERR_INTERNAL;
  }
  grjit_emit_sort_sites(&e->c);
  return GRJIT_OK;
}

void grjit_emit_free(GRJIT_Emit * e) {
  if (e->c.allocator == NULL) {
    return;
  }
  e->c.allocator->free_fn(e->c.allocator->ctx, e->blocks);
  grjit_asm_free(&e->as);
  grjit_emit_common_free(&e->c);
  memset(e, 0, sizeof *e);
}
