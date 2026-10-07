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

#include <ghoti.io/runtime-core/a/compiled.h>

#include <string.h>

#define A GRJIT_A64Asm
#define XR(n) GRJIT_A64_X##n

/* A helper call's arguments (the IR's six) and the internal convention's eight, which are
 * AAPCS64's too: x0-x7. */
static const GRJIT_A64Reg arg_regs_internal[GRJIT_ARM64_INTERNAL_REG_ARGS] = {
    GRJIT_A64_X0, GRJIT_A64_X1, GRJIT_A64_X2, GRJIT_A64_X3, GRJIT_A64_X4,
    GRJIT_A64_X5, GRJIT_A64_X6, GRJIT_A64_X7};
#define arg_regs arg_regs_internal

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
  /* In a callable function every call that can reach a GC point records where the walk starts
   * first (AD-28), so a collection under the helper sees this frame and the compiled frames
   * below it. It uses x15 and x16, which the arguments and the call are loaded over after. */
  GRJIT_Label walk_ret = 0;
  const bool record = e->c.callable && op->attr == GRJIT_CALL_GC_POINT;
  if (record) {
    walk_ret = grjit_a64_label(a);
    grjit_a64_emit_store_walk_cell(e, walk_ret);
  }
  for (size_t i = 0; i < op->arg_count; i++) {
    load_operand(e, arg_regs[i], &op->args[i]);
  }
  grjit_a64_mov_ri(a, GRJIT_A64_X16, op->address);
  grjit_a64_blr(a, GRJIT_A64_X16);
  if (record) {
    grjit_a64_bind(a, walk_ret);
  }
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


/* ---- Calls between compiled functions, tail calls and natives (AD-28) -------------------- */

/* `rd = base + offset` for any offset a frame can have: one instruction when it fits an
 * immediate, two when it needs the shifted high part. `base` is `sp` or `x29`. */
static void lea(GRJIT_A64Emit * e, GRJIT_A64Reg rd, GRJIT_A64Reg base, int64_t offset) {
  A * a = &e->as;
  const bool neg = offset < 0;
  const uint64_t mag = neg ? (uint64_t)(-offset) : (uint64_t)offset;
  if (mag >= (UINT64_C(1) << 24)) {
    e->c.error = GRJIT_ERR_LIMIT;
    return;
  }
  GRJIT_A64Reg from = base;
  if (mag >> 12) {
    (neg ? grjit_a64_sub_imm : grjit_a64_add_imm)(a, rd, from, (uint32_t)(mag >> 12), true);
    from = rd;
  }
  if ((mag & 0xFFFu) != 0 || from == base) {
    (neg ? grjit_a64_sub_imm : grjit_a64_add_imm)(a, rd, from, (uint32_t)(mag & 0xFFFu), false);
  }
}

/* The stack-argument bytes the internal convention puts for `n` arguments. */
static uint32_t stack_bytes(size_t n) {
  return grjit_stack_arg_bytes(n, GRJIT_ARM64_INTERNAL_REG_ARGS);
}

/* Finds the callee's entry for a call or a tail call and leaves it in `x16` and in the
 * entry-save slot, or goes to the returned exit label: through the slot (one compare; the
 * compile hook for an empty slot; an exit for a refused one) or through the code pointer
 * (the registered, not retired, tagged, aligned check). `live` is the first of the
 * operation's liveness sites and `exit_live` the one the exit's stub is made from. */
static GRJIT_Label emit_dispatch(
    GRJIT_A64Emit * e, const GRJIT_Op * op, size_t live, size_t exit_live) {
  A * a = &e->as;
  const GRJIT_Function * f = e->c.f;
  const bool slot = op->kind == GRJIT_OP_CALL_SLOT || op->kind == GRJIT_OP_TAIL_CALL_SLOT;
  const size_t n = op->arg_count;
  const int32_t entry_slot = GRJIT_ENTRY_SAVE_SLOT(GRJIT_SHAPE_REGS(f, e->c.shape), e->c.shape);
  GRJIT_Label exit = grjit_a64_label(a);
  GRJIT_Pending p;
  memset(&p, 0, sizeof p);
  p.op = op;
  if (slot) {
    GRJIT_Label slow = grjit_a64_label(a);
    GRJIT_Label back = grjit_a64_label(a);
    grjit_a64_mov_ri(a, GRJIT_A64_X16, op->address);
    grjit_a64_load(a, GRJIT_A64_MEM_X64, GRJIT_A64_X16, GRJIT_A64_X16, 0);
    /* One compare tells compiled code (above one) from empty (zero) and from refused (one). */
    grjit_a64_cmp_imm(a, GRJIT_A64_X16, GRCORE_ENTRY_REFUSED);
    grjit_a64_bcond(a, GRJIT_A64_LS, slow);
    grjit_a64_bind(a, back);
    p.kind = GRJIT_PENDING_CALL_SLOW;
    p.entry = slow;
    p.back = back;
    p.exit = exit;
    p.live_index = live;
    grjit_emit_add_pending(&e->c, &p);
  } else {
    load_operand(e, GRJIT_A64_X16, &op->a);
    store_slot(e, GRJIT_A64_X16, entry_slot);
    /* The target must be the internal entry of code registered in this context: an address
     * that is not is never entered. */
    load_slot(e, GRJIT_A64_X0, GRJIT_SLOT_CTX);
    grjit_a64_mov_rr(a, GRJIT_A64_X1, GRJIT_A64_X16);
    grjit_a64_mov_ri(a, GRJIT_A64_X2, op->callee);
    grjit_a64_mov_ri(a, GRJIT_A64_X3, n);
    grjit_a64_mov_ri(a, GRJIT_A64_X16, (uint64_t)(uintptr_t)grjit_call_target_ok);
    grjit_a64_blr(a, GRJIT_A64_X16);
    grjit_a64_mov32_rr(a, GRJIT_A64_X0, GRJIT_A64_X0);
    grjit_a64_cbz(a, GRJIT_A64_X0, exit);
  }
  memset(&p, 0, sizeof p);
  p.kind = GRJIT_PENDING_CALL_EXIT;
  p.entry = exit;
  p.op = op;
  p.live_index = exit_live;
  grjit_emit_add_pending(&e->c, &p);
  if (slot) {
    store_slot(e, GRJIT_A64_X16, entry_slot);
  }
  return exit;
}

/* The arguments of a call or a tail call, into the arguments area. */
static void emit_stage_arguments(GRJIT_A64Emit * e, const GRJIT_Op * op) {
  const GRJIT_Function * f = e->c.f;
  const size_t regs = GRJIT_SHAPE_REGS(f, e->c.shape);
  for (size_t i = 0; i < op->arg_count; i++) {
    load_operand(e, GRJIT_A64_X0, &op->args[i]);
    store_slot(e, GRJIT_A64_X0, GRJIT_ARGS_SLOT(regs, e->c.shape.args_area, i));
  }
}

/* The hook call of a push or a tail call, after the arguments are staged: stores the walk
 * start for the site that ends at `ret`, then calls `hook(ctx, callee, area, n)`. */
static void emit_frame_hook(GRJIT_A64Emit * e, const GRJIT_Op * op, GRJIT_Label ret, uint64_t hook) {
  A * a = &e->as;
  const size_t regs = GRJIT_SHAPE_REGS(e->c.f, e->c.shape);
  grjit_a64_emit_store_walk_cell(e, ret);
  grjit_a64_mov_rr(a, GRJIT_A64_X0, GRJIT_A64_X15);
  grjit_a64_mov_ri(a, GRJIT_A64_X1, op->callee);
  lea(e, GRJIT_A64_X2, GRJIT_A64_FP, GRJIT_ARGS_SLOT(regs, e->c.shape.args_area, 0));
  grjit_a64_mov_ri(a, GRJIT_A64_X3, op->arg_count);
  grjit_a64_mov_ri(a, GRJIT_A64_X16, hook);
  grjit_a64_blr(a, GRJIT_A64_X16);
}

/* A call to another compiled function (AD-28), through an entry slot or a code pointer. In
 * order: find the entry; copy the arguments into the frame's arguments area, where the push
 * hook reads them and a moving collector updates them; push the callee's guest frame (the
 * frame-push GC point); call through the internal convention; test the status for DEOPTED;
 * store the result; pop. Every way out before the call is an exit through the exit state;
 * every way out after it is DEOPTED. */
static void emit_guest_call(GRJIT_A64Emit * e, const GRJIT_Op * op) {
  A * a = &e->as;
  const GRJIT_Function * f = e->c.f;
  const GRJIT_FrameState * st = &f->states[op->state];
  const size_t n = op->arg_count;
  const size_t regs = GRJIT_SHAPE_REGS(f, e->c.shape);
  const int32_t entry_slot = GRJIT_ENTRY_SAVE_SLOT(regs, e->c.shape);
  const size_t live = e->c.live_cursor;
  e->c.live_cursor += 3; /* the push, the call and the exit */

  GRJIT_Label exit = emit_dispatch(e, op, live, live + 2);
  emit_stage_arguments(e, op);

  /* The push: the callee's guest frame, counted as the interpreter counts it. */
  GRJIT_Label push_ret = grjit_a64_label(a);
  emit_frame_hook(e, op, push_ret, (uint64_t)(uintptr_t)f->hooks.push);
  grjit_a64_bind(a, push_ret);
  grjit_emit_add_site_for(&e->c, (uint32_t)grjit_a64_size(a),
      GRCORE_SITE_GC_POINT_FRAME_PUSH, st->identity, live, op->state, op);
  grjit_a64_mov32_rr(a, GRJIT_A64_X0, GRJIT_A64_X0);
  grjit_a64_cbnz(a, GRJIT_A64_X0, exit);

  /* The call. The stack arguments are made just before it, at `[sp]`, and popped by the
   * callee, which keeps `sp` 16-aligned (the area is whole 16-byte units). */
  const unsigned ireg = GRJIT_ARM64_INTERNAL_REG_ARGS;
  const uint32_t area = stack_bytes(n);
  if (area != 0) {
    grjit_a64_sub_sp(a, area);
    for (size_t i = ireg; i < n; i++) {
      load_slot(e, GRJIT_A64_X10, GRJIT_ARGS_SLOT(regs, e->c.shape.args_area, i));
      grjit_a64_store(a, GRJIT_A64_MEM_X64, GRJIT_A64_X10, GRJIT_A64_SP, (int32_t)(8 * (i - ireg)));
    }
  }
  for (size_t i = 0; i < n && i < ireg; i++) {
    load_slot(e, arg_regs_internal[i], GRJIT_ARGS_SLOT(regs, e->c.shape.args_area, i));
  }
  load_slot(e, GRJIT_A64_X9, GRJIT_SLOT_CTX);
  load_slot(e, GRJIT_A64_X16, entry_slot);
  grjit_a64_blr(a, GRJIT_A64_X16);
  /* The return address is the site: this frame, with the callee running. */
  grjit_emit_add_site_for(&e->c, (uint32_t)grjit_a64_size(a),
      GRCORE_SITE_GC_POINT_CALL, st->identity, live + 1, op->state, op);
  grjit_a64_cbnz(a, GRJIT_A64_X1, e->ret_propagate);
  if (op->dst != GRJIT_NO_VREG) {
    store_result(e, op->dst, GRJIT_A64_X0);
  }
  /* The call is complete: pop the callee's guest frame. */
  load_slot(e, GRJIT_A64_X0, GRJIT_SLOT_CTX);
  grjit_a64_mov_ri(a, GRJIT_A64_X16, (uint64_t)(uintptr_t)f->hooks.pop);
  grjit_a64_blr(a, GRJIT_A64_X16);
}

/* A tail call (AD-28): the callee replaces this function's frame, native and guest, and
 * returns to this function's caller. In order: find the entry (as a call does); the arguments
 * into the arguments area; the engine's `tail` hook, which replaces the guest frame as one
 * step and is the frame-push GC point; then, with no GC point until the jump, the replacement
 * of the native frame.
 *
 * Let `in_A` be the bytes of stack arguments this function takes and `in_T` those the callee
 * takes (whole 16-byte units). `R = x29 + 16 + in_A` is where the original caller expects `sp`
 * after the return, so the callee's `add sp, sp, #in_T` must leave it there: the callee is
 * entered with `sp = R - in_T`, its stack arguments at `[sp, R)`, and with `x29` and `x30` as
 * they were for this function (its caller's frame record and the return address, which the
 * callee's prologue pushes again). `sp` moves last and nothing below it is read after, so no
 * signal can see a half-moved frame.
 *
 * The arguments area is the staging place: its end is at or below the lowest address the copy
 * writes (the shape's `pad` makes it so; the shared shape counts a return-address word arm64
 * does not have, so it is conservative by up to one slot), so no copy overwrites a source and
 * the copies are in any order. Everything that is read from this frame after the first write
 * (the context, the return address, the caller's saved base) is loaded first. The walk is
 * unchanged: after the jump the callee's frame record holds this function's saved base and
 * return address, so it is one compiled frame beside one guest frame, as any callee of the
 * original caller is. */
static void emit_tail_call(GRJIT_A64Emit * e, const GRJIT_Op * op) {
  A * a = &e->as;
  const GRJIT_Function * f = e->c.f;
  const GRJIT_FrameState * st = &f->states[op->state];
  const size_t n = op->arg_count;
  const size_t regs = GRJIT_SHAPE_REGS(f, e->c.shape);
  const int32_t entry_slot = GRJIT_ENTRY_SAVE_SLOT(regs, e->c.shape);
  const size_t live = e->c.live_cursor;
  e->c.live_cursor += 2; /* the hook and the exit */

  GRJIT_Label exit = emit_dispatch(e, op, live, live + 1);
  emit_stage_arguments(e, op);

  GRJIT_Label hook_ret = grjit_a64_label(a);
  emit_frame_hook(e, op, hook_ret, (uint64_t)(uintptr_t)f->hooks.tail);
  grjit_a64_bind(a, hook_ret);
  /* The hook is a frame-push GC point of this frame, whose guest frame is still this
   * function's: the state is the caller's, and the area is in the map. */
  grjit_emit_add_site_for(&e->c, (uint32_t)grjit_a64_size(a),
      GRCORE_SITE_GC_POINT_FRAME_PUSH, st->identity, live, op->state, op);
  grjit_a64_mov32_rr(a, GRJIT_A64_X0, GRJIT_A64_X0);
  grjit_a64_cbnz(a, GRJIT_A64_X0, exit);

  const unsigned ireg = GRJIT_ARM64_INTERNAL_REG_ARGS;
#if defined(GRJIT_TEST_PLANT_BUG) && GRJIT_TEST_PLANT_BUG == 24
  /* Planted defect 24 (tests only): the tail call is made as a call, and its frame kept: the
   * answer is the same and the native stack grows with every tail call, which no result shows
   * and the stack pointer does. */
  {
    const uint32_t area_bytes = stack_bytes(n);
    if (area_bytes != 0) {
      grjit_a64_sub_sp(a, area_bytes);
      for (size_t i = ireg; i < n; i++) {
        load_slot(e, GRJIT_A64_X10, GRJIT_ARGS_SLOT(regs, e->c.shape.args_area, i));
        grjit_a64_store(a, GRJIT_A64_MEM_X64, GRJIT_A64_X10, GRJIT_A64_SP, (int32_t)(8 * (i - ireg)));
      }
    }
    for (size_t i = 0; i < n && i < ireg; i++) {
      load_slot(e, arg_regs_internal[i], GRJIT_ARGS_SLOT(regs, e->c.shape.args_area, i));
    }
    load_slot(e, GRJIT_A64_X9, GRJIT_SLOT_CTX);
    load_slot(e, GRJIT_A64_X16, entry_slot);
    grjit_a64_blr(a, GRJIT_A64_X16);
    grjit_a64_emit_callable_epilogue(e);
    return;
  }
#endif
  /* The frame replacement. */
  const int64_t in_a = e->c.shape.incoming_bytes;
  const int64_t in_t = stack_bytes(n);
  const int64_t sp_new = 16 + in_a - in_t; /* from x29 */
  load_slot(e, GRJIT_A64_X9, GRJIT_SLOT_CTX);
  grjit_a64_load(a, GRJIT_A64_MEM_X64, GRJIT_A64_X30, GRJIT_A64_FP, 8);
  grjit_a64_load(a, GRJIT_A64_MEM_X64, GRJIT_A64_X15, GRJIT_A64_FP, 0);
  size_t copy_to = n;
#if defined(GRJIT_TEST_PLANT_BUG) && GRJIT_TEST_PLANT_BUG == 25
  /* Planted defect 25 (tests only): the last stack argument is not copied, so the callee
   * reads what the destination held. */
  if (copy_to > ireg) {
    copy_to--;
  }
#endif
  for (size_t i = ireg; i < copy_to; i++) {
    load_slot(e, GRJIT_A64_X10, GRJIT_ARGS_SLOT(regs, e->c.shape.args_area, i));
    store_slot(e, GRJIT_A64_X10, (int32_t)(sp_new + 8 * (int64_t)(i - ireg)));
  }
  for (size_t i = 0; i < n && i < ireg; i++) {
    load_slot(e, arg_regs_internal[i], GRJIT_ARGS_SLOT(regs, e->c.shape.args_area, i));
  }
  load_slot(e, GRJIT_A64_X16, entry_slot);
  lea(e, GRJIT_A64_SP, GRJIT_A64_FP, sp_new);
  grjit_a64_mov_rr(a, GRJIT_A64_FP, GRJIT_A64_X15);
  grjit_a64_br(a, GRJIT_A64_X16);
}

/* A call to a registered native (AD-28, AD-17; natives.h for the contract). In order, and
 * after it nothing is reordered:
 *
 *  1. the native-stack check: `sub x16, sp, #(S + stack_bytes)` against the context's limit
 *     word (zero means none); below it is an exit before the call, the native is not called
 *     and the interpreter makes the call itself. `S` is the stack-argument area, below. The
 *     one formula is the same on every target; on arm64 the return address is not on the
 *     stack, so a native has eight bytes more than it declared (natives.h);
 *  2. the walk start, first: the frame base and the address after the call go in the
 *     context's cell before the area is made, before any argument is moved and before the
 *     call, so every GC point the native reaches sees this frame and the compiled frames
 *     below it;
 *  3. the arguments by AAPCS64: with `N = 1 + n` words (the context first), the first eight
 *     in `x0`-`x7`, the rest, `k = N - 8`, at `[sp + 8 i]` in an area of `S = round_up_16(8
 *     k)` bytes made by `sub sp, sp, #S`, so `sp` is 16-aligned at the call. Every operand
 *     is read from a frame slot or is an immediate, so the order of the loads cannot clobber
 *     one;
 *  4. the call, through `x16`; the caller pops (`add sp, sp, #S`);
 *  5. the result is stored to `dst` before the status is looked at, so the state after the
 *     call sees it; with a status, a non-zero `w1` (the upper half of `x1` is the pair's
 *     `reserved` word, which is not looked at) is the exit through the state after the call;
 *  6. nothing is assumed preserved: the context, every operand and every live value is read
 *     from the frame again, and every caller-saved register is clobbered.
 *
 * The address after the call is the call's site: this frame in the call's state, with the
 * stack map of what is live after the call (the result excluded). The arguments are not
 * mapped: a native receives raw words, and what it holds across a GC point it protects
 * itself. */
static void emit_native_call(GRJIT_A64Emit * e, const GRJIT_Op * op) {
  A * a = &e->as;
  const GRJIT_Function * f = e->c.f;
  const GRJIT_NativeDesc * d = grjit_native_table_get(f->natives, op->native);
  if (d == NULL || d->param_count != op->arg_count) {
    /* The verifier refused this already; a descriptor cannot change under a verified
     * function (the table is append-only), so this is the library's own invariant. */
    e->c.error = GRJIT_ERR_INTERNAL;
    return;
  }
  const bool status = (d->flags & GRJIT_NATIVE_STATUS) != 0;
  const size_t n = op->arg_count;
  const size_t words = n + 1; /* the context first */
  const unsigned creg = GRJIT_ARM64_INTERNAL_REG_ARGS; /* AAPCS64 passes eight words in registers too */
  const size_t stack_words = words > creg ? words - creg : 0;
  uint32_t area = (uint32_t)((stack_words * 8 + 15) / 16 * 16);
#if defined(GRJIT_TEST_PLANT_BUG) && GRJIT_TEST_PLANT_BUG == 21
  /* Planted defect 21 (tests only): the stack-argument area is not rounded to sixteen bytes, so
   * an odd count of stack words leaves `sp` misaligned at the call. */
  area = (uint32_t)(stack_words * 8);
#endif
  const size_t live = e->c.live_cursor;
  e->c.live_cursor += status ? 3 : 2; /* the call, the exit before it, the status exit */
  const GRJIT_FrameState * st = &f->states[op->state];

  /* 1. The check. */
  GRJIT_Label exit = grjit_a64_label(a);
  {
    GRJIT_Pending p;
    memset(&p, 0, sizeof p);
    p.kind = GRJIT_PENDING_NATIVE_EXIT;
    p.entry = exit;
    p.op = op;
    p.live_index = live + 1;
    grjit_emit_add_pending(&e->c, &p);
  }
  uint64_t need = (uint64_t)area + d->stack_bytes;
#if defined(GRJIT_TEST_PLANT_BUG) && GRJIT_TEST_PLANT_BUG == 28
  /* Planted defect 28 (tests only): the check leaves out the stack-argument area. */
  need = d->stack_bytes;
#endif
  lea(e, GRJIT_A64_X16, GRJIT_A64_SP, -(int64_t)need);
  load_slot(e, GRJIT_A64_X15, GRJIT_SLOT_CTX);
  grjit_a64_load(a, GRJIT_A64_MEM_X64, GRJIT_A64_X15, GRJIT_A64_X15, (int32_t)e->c.native_limit_offset);
  grjit_a64_cmp(a, GRJIT_A64_X16, GRJIT_A64_X15);
  grjit_a64_bcond(a, GRJIT_A64_LO, exit);

  /* 2. The walk start, before anything moves. */
  GRJIT_Label ret = grjit_a64_label(a);
#if defined(GRJIT_TEST_PLANT_BUG) && GRJIT_TEST_PLANT_BUG == 20
  /* Planted defect 20 (tests only): the walk start is stored after the call, so a collection
   * the native triggers finds the previous call's. */
#else
  grjit_a64_emit_store_walk_cell(e, ret);
#endif

  /* 3. The arguments. */
  if (area != 0) {
    grjit_a64_sub_sp(a, area);
  }
  for (size_t i = creg - 1; i < n; i++) {
    /* IR argument i is C argument i + 1; the last register word is the one IR argument before. */
    load_operand(e, GRJIT_A64_X10, &op->args[i]);
    grjit_a64_store(a, GRJIT_A64_MEM_X64, GRJIT_A64_X10, GRJIT_A64_SP,
        (int32_t)(8 * (i - (creg - 1))));
  }
  load_slot(e, GRJIT_A64_X0, GRJIT_SLOT_CTX);
  for (size_t i = 0; i < n && i + 1 < creg; i++) {
    load_operand(e, arg_regs_internal[i + 1], &op->args[i]);
  }

  /* 4. The call; the address after it is the site. */
  grjit_a64_mov_ri(a, GRJIT_A64_X16, d->address);
  grjit_a64_blr(a, GRJIT_A64_X16);
#if defined(GRJIT_TEST_PLANT_BUG) && GRJIT_TEST_PLANT_BUG == 20
  /* The late store, through registers that leave the native's x0 and x1 alone. */
  grjit_a64_adr(a, GRJIT_A64_X16, ret);
  load_slot(e, GRJIT_A64_X15, GRJIT_SLOT_CTX);
  grjit_a64_store(a, GRJIT_A64_MEM_X64, GRJIT_A64_FP, GRJIT_A64_X15, (int32_t)e->c.walk_cell_offset);
  grjit_a64_store(a, GRJIT_A64_MEM_X64, GRJIT_A64_X16, GRJIT_A64_X15, (int32_t)e->c.walk_cell_offset + 8);
#endif
  grjit_a64_bind(a, ret);
  grjit_emit_add_site_for(&e->c, (uint32_t)grjit_a64_size(a), GRCORE_SITE_GC_POINT_CALL,
      st->identity, live, op->state, op);
  if (area != 0) {
#if defined(GRJIT_TEST_PLANT_BUG) && GRJIT_TEST_PLANT_BUG == 22
    /* Planted defect 22 (tests only): the caller does not pop the stack arguments, so `sp`
     * drifts by their area with every call. */
#else
    grjit_a64_add_sp(a, area);
#endif
  }

  /* 5. The result before the status; a non-zero status leaves. */
  if (op->dst != GRJIT_NO_VREG) {
    store_result(e, op->dst, GRJIT_A64_X0);
  }
  if (status) {
    GRJIT_Label leave = grjit_a64_label(a);
    GRJIT_Pending p;
    memset(&p, 0, sizeof p);
    p.kind = GRJIT_PENDING_NATIVE_STATUS;
    p.entry = leave;
    p.op = op;
    p.live_index = live + 2;
    grjit_emit_add_pending(&e->c, &p);
    /* The status is the 32 bits of w1: the half above is the pair's `reserved` word, which a
     * native may leave as it likes (natives.h), so it is cleared and never read. */
#if defined(GRJIT_TEST_PLANT_BUG) && GRJIT_TEST_PLANT_BUG == 26
    /* Planted defect 26 (tests only): the status is tested on all of x1, so garbage in the
     * upper half exits a call whose status is zero. */
#else
    grjit_a64_mov32_rr(a, GRJIT_A64_X1, GRJIT_A64_X1);
#endif
    grjit_a64_cbnz(a, GRJIT_A64_X1, leave);
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
      if (e->c.callable) {
        /* The internal convention: the result in x0 (zero for none), the status in x1, and
         * the callee pops its own stack arguments. */
        if (op->a.kind != GRJIT_OPERAND_NONE) {
          load_operand(e, GRJIT_A64_X0, &op->a);
        } else {
          grjit_a64_mov_ri(a, GRJIT_A64_X0, 0);
        }
        grjit_a64_mov_ri(a, GRJIT_A64_X1, GRJIT_STATUS_RETURNED);
        grjit_a64_emit_callable_epilogue(e);
        break;
      }
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
      emit_guest_call(e, op);
      break;
    case GRJIT_OP_TAIL_CALL_SLOT:
    case GRJIT_OP_TAIL_CALL_PTR:
      emit_tail_call(e, op);
      break;
    case GRJIT_OP_CALL_NATIVE:
      emit_native_call(e, op);
      break;
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

/* The entry adapter of a callable function (AD-28): the C ABI of ::GRJIT_EntryFn on one side
 * and the internal convention on the other. It makes a frame record and keeps `out`, the
 * context and `args` in its own frame, runs the entry hook, loads the arguments from the
 * `args` array into the registers and the stack area, sets `x29` to the chain-end marker (so
 * the first compiled frame's saved caller base is the marker, which ends a walk;
 * a/compiled.h), calls the internal entry, and turns its status into an exit. It uses no
 * callee-saved register but `x29`, which it restores from its own frame record (never from
 * `x29`, which is the marker by then). It clears the walk-start cell on the way out, so a
 * walk after the run reads no dead frames. Its frame is `sp` + 0 .. 32: `out` at 0, the
 * context at 8, `args` at 16. */
static void emit_adapter(GRJIT_A64Emit * e) {
  A * a = &e->as;
  const GRJIT_Function * f = e->c.f;
  const size_t params = f->param_count;
  const uint32_t in_bytes = e->c.shape.incoming_bytes;
  const unsigned ireg = GRJIT_ARM64_INTERNAL_REG_ARGS;
  GRJIT_Label deopted = grjit_a64_label(a);
  GRJIT_Label not_failed = grjit_a64_label(a);
  GRJIT_Label done = grjit_a64_label(a);
  grjit_a64_push_frame_record(a);
  grjit_a64_sub_sp(a, 32);
  grjit_a64_store(a, GRJIT_A64_MEM_X64, GRJIT_A64_X2, GRJIT_A64_SP, 0);  /* out */
  grjit_a64_store(a, GRJIT_A64_MEM_X64, GRJIT_A64_X0, GRJIT_A64_SP, 8);  /* context */
  grjit_a64_store(a, GRJIT_A64_MEM_X64, GRJIT_A64_X1, GRJIT_A64_SP, 16); /* args */
#if defined(GRJIT_TEST_PLANT_BUG) && GRJIT_TEST_PLANT_BUG == 27 && defined(GRJIT_TEST_PLANT_FORM) && GRJIT_TEST_PLANT_FORM == 2
  /* Planted defect 27, second form (tests only): a callee-saved register is used and never
   * restored. The caller's own value must survive the call. */
  grjit_a64_mov_rr(a, GRJIT_A64_X19, GRJIT_A64_X1);
#endif
  if (e->c.hook != NULL) {
    grjit_a64_mov_ri(a, GRJIT_A64_X16, (uint64_t)(uintptr_t)e->c.hook);
    grjit_a64_blr(a, GRJIT_A64_X16);
    /* The hook returns a uint32_t; the upper half of x0 is not defined. */
    grjit_a64_mov32_rr(a, GRJIT_A64_X0, GRJIT_A64_X0);
    grjit_a64_cbnz(a, GRJIT_A64_X0, e->refuse);
  }
  grjit_a64_load(a, GRJIT_A64_MEM_X64, GRJIT_A64_X15, GRJIT_A64_SP, 16);
  if (in_bytes != 0) {
    grjit_a64_sub_sp(a, in_bytes);
    for (size_t i = ireg; i < params; i++) {
      grjit_a64_load(a, GRJIT_A64_MEM_X64, GRJIT_A64_X10, GRJIT_A64_X15, (int32_t)(8 * i));
      grjit_a64_store(a, GRJIT_A64_MEM_X64, GRJIT_A64_X10, GRJIT_A64_SP, (int32_t)(8 * (i - ireg)));
    }
  }
  for (size_t i = 0; i < params && i < ireg; i++) {
    grjit_a64_load(a, GRJIT_A64_MEM_X64, arg_regs_internal[i], GRJIT_A64_X15, (int32_t)(8 * i));
  }
  grjit_a64_load(a, GRJIT_A64_MEM_X64, GRJIT_A64_X9, GRJIT_A64_SP, (int32_t)(in_bytes + 8));
  grjit_a64_mov_ri(a, GRJIT_A64_X29, (uint64_t)GRCORE_COMPILED_CHAIN_END);
  grjit_a64_adr(a, GRJIT_A64_X16, e->internal);
  grjit_a64_blr(a, GRJIT_A64_X16);
  /* sp is where it was after the frame was made: the callee popped the stack arguments. */
  grjit_a64_load(a, GRJIT_A64_MEM_X64, GRJIT_A64_X2, GRJIT_A64_SP, 0);
  grjit_a64_load(a, GRJIT_A64_MEM_X64, GRJIT_A64_X15, GRJIT_A64_SP, 8);
  grjit_a64_store(a, GRJIT_A64_MEM_X64, GRJIT_A64_XZR, GRJIT_A64_X15, (int32_t)e->c.walk_cell_offset);
  grjit_a64_store(a, GRJIT_A64_MEM_X64, GRJIT_A64_XZR, GRJIT_A64_X15, (int32_t)e->c.walk_cell_offset + 8);
  grjit_a64_cbnz(a, GRJIT_A64_X1, deopted);
  grjit_a64_store(a, GRJIT_A64_MEM_X64, GRJIT_A64_X0, GRJIT_A64_X2, 0);
  grjit_a64_mov_ri(a, GRJIT_A64_X0, GRJIT_EXIT_RETURNED);
  grjit_a64_b(a, done);
  /* DEOPTED: every compiled frame was rebuilt into its guest frame before any of them
   * returned; out[0] is the cause. */
  grjit_a64_bind(a, deopted);
  grjit_a64_store(a, GRJIT_A64_MEM_X64, GRJIT_A64_X0, GRJIT_A64_X2, 0);
  grjit_a64_mov_ri(a, GRJIT_A64_X0, GRJIT_EXIT_DEOPT);
  /* FAILED: the engine's hook refused the rebuild; out[0] is its answer. */
  grjit_a64_cmp_imm(a, GRJIT_A64_X1, GRJIT_STATUS_FAILED);
  grjit_a64_bcond(a, GRJIT_A64_NE, not_failed);
  grjit_a64_mov_ri(a, GRJIT_A64_X0, GRJIT_EXIT_REBUILD_FAILED);
  grjit_a64_bind(a, not_failed);
  grjit_a64_bind(a, done);
#if defined(GRJIT_TEST_PLANT_BUG) && GRJIT_TEST_PLANT_BUG == 27 && !(defined(GRJIT_TEST_PLANT_FORM) && GRJIT_TEST_PLANT_FORM == 2)
  /* Planted defect 27 (tests only): the adapter gives the frame record back but leaves `x29`
   * as the chain-end marker the caller of the entry then sees. */
  grjit_a64_add_sp(a, 32);
  grjit_a64_load(a, GRJIT_A64_MEM_X64, GRJIT_A64_X30, GRJIT_A64_SP, 8);
  grjit_a64_add_sp(a, 16);
  grjit_a64_ret(a);
#else
  grjit_a64_add_sp(a, 32);
  grjit_a64_pop_frame_record(a);
  grjit_a64_ret(a);
#endif
  /* The entry hook refused (w0 is its answer). */
  grjit_a64_bind(a, e->refuse);
  grjit_a64_load(a, GRJIT_A64_MEM_X64, GRJIT_A64_X2, GRJIT_A64_SP, 0);
  grjit_a64_store(a, GRJIT_A64_MEM_X64, GRJIT_A64_X0, GRJIT_A64_X2, 0);
  grjit_a64_mov_ri(a, GRJIT_A64_X0, GRJIT_EXIT_REFUSED);
  grjit_a64_add_sp(a, 32);
  grjit_a64_pop_frame_record(a);
  grjit_a64_ret(a);
}

/* The stubs the blocks queued. A stub may add sites but never another stub. */
static void emit_pending(GRJIT_A64Emit * e) {
  for (size_t i = 0; i < e->c.pending_count && e->c.error == GRJIT_OK; i++) {
    const GRJIT_Pending * p = &e->c.pending[i];
    switch (p->kind) {
      case GRJIT_PENDING_POLL:
        grjit_a64_emit_poll_stub(e, p);
        break;
      case GRJIT_PENDING_GUARD:
        grjit_a64_emit_guard_stub(e, p);
        break;
      case GRJIT_PENDING_CALL_SLOW:
        grjit_a64_emit_call_slow_stub(e, p);
        break;
      case GRJIT_PENDING_CALL_EXIT:
        grjit_a64_emit_call_exit_stub(e, p);
        break;
      case GRJIT_PENDING_NATIVE_EXIT:
        grjit_a64_emit_native_exit_stub(e, p);
        break;
      case GRJIT_PENDING_NATIVE_STATUS:
        grjit_a64_emit_native_status_stub(e, p);
        break;
    }
  }
}

/* The internal entry of a callable function: a frame record, the native-stack check (in bytes,
 * AD-28: the lowest address this frame will use against the context's limit word, `x9` holding
 * the context; below it, the chain deopts at the call site and the interpreter continues), the
 * frame, the context and the parameters from `x0`-`x7` and the caller's stack arguments. */
static void emit_internal_prologue(GRJIT_A64Emit * e) {
  A * a = &e->as;
  const GRJIT_Function * f = e->c.f;
  const uint32_t alloc = e->c.frame_bytes;
  const unsigned ireg = GRJIT_ARM64_INTERNAL_REG_ARGS;
  grjit_a64_push_frame_record(a);
  grjit_a64_mov_sp(a, GRJIT_A64_FP, GRJIT_A64_SP);
#if defined(GRJIT_TEST_PLANT_BUG) && GRJIT_TEST_PLANT_BUG == 29
  /* Planted defect 29 (tests only): the frame is not counted, so a function whose frame
   * crosses the limit is let in. */
  grjit_a64_mov_sp(a, GRJIT_A64_X16, GRJIT_A64_SP);
#else
  lea(e, GRJIT_A64_X16, GRJIT_A64_SP, -(int64_t)alloc);
#endif
  grjit_a64_load(a, GRJIT_A64_MEM_X64, GRJIT_A64_X15, GRJIT_A64_X9, (int32_t)e->c.native_limit_offset);
  grjit_a64_cmp(a, GRJIT_A64_X16, GRJIT_A64_X15);
  grjit_a64_bcond(a, GRJIT_A64_LO, e->overflow);
  grjit_a64_sub_sp(a, alloc);
  store_slot(e, GRJIT_A64_X9, GRJIT_SLOT_CTX);
  for (size_t i = 0; i < f->param_count; i++) {
    if (i < ireg) {
      store_slot(e, arg_regs_internal[i], grjit_emit_slot((GRJIT_VReg)i));
    } else {
      grjit_a64_load(a, GRJIT_A64_MEM_X64, GRJIT_A64_X10, GRJIT_A64_FP, (int32_t)(16 + 8 * (i - ireg)));
      store_slot(e, GRJIT_A64_X10, grjit_emit_slot((GRJIT_VReg)i));
    }
  }
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

  if (f->callable) {
    e->internal = grjit_a64_label(a);
    e->overflow = grjit_a64_label(a);
    e->ret_deopted = grjit_a64_label(a);
    e->ret_failed = grjit_a64_label(a);
    e->ret_propagate = grjit_a64_label(a);
    emit_adapter(e);
    /* The internal entry on a 16-byte boundary with the tag just before it: the magic and
     * parameter count, then the function's token. The padding is trap words. */
    while ((grjit_a64_size(a) + GRJIT_ENTRY_TAG_BYTES) % 16 != 0) {
      grjit_a64_brk(a, 0);
    }
    grjit_a64_data64(a, GRJIT_ENTRY_TAG_WORD(f->param_count));
    grjit_a64_data64(a, f->token);
    grjit_a64_bind(a, e->internal);
    e->internal_offset = (uint32_t)grjit_a64_size(a);
    emit_internal_prologue(e);
  } else {
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
  }

  for (size_t b = 0; b < f->block_count && e->c.error == GRJIT_OK; b++) {
    grjit_a64_bind(a, e->blocks[b]);
    const GRJIT_BlockInfo * blk = &f->blocks[b];
    for (size_t i = 0; i < blk->count; i++) {
      emit_op(e, &blk->ops[i], b);
    }
  }
  emit_pending(e);
  if (f->callable) {
    grjit_a64_emit_ret_deopted(e);
    grjit_a64_emit_ret_status(e);
    grjit_a64_emit_overflow_stub(e);
  } else {
    grjit_a64_emit_refuse_stub(e);
  }
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
  grjit_emit_common_init(&e->c, f, GRJIT_ARM64_INTERNAL_REG_ARGS, allocator, hook,
      request_offset, frame_bytes, live);
}

GRJIT_Result grjit_a64_emit_function(const GRJIT_Function * f,
    const GRJIT_Allocator * allocator, size_t max_code_bytes,
    GRJIT_EntryHook hook, uint32_t request_offset, uint32_t frame_bytes,
    const GRJIT_LiveSites * live, GRJIT_A64Emit * e) {
  memset(e, 0, sizeof *e);
  grjit_emit_common_init(&e->c, f, GRJIT_ARM64_INTERNAL_REG_ARGS, allocator, hook,
      request_offset, frame_bytes, live);
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
