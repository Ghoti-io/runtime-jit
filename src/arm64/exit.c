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
 * The out-of-line stubs of the arm64 backend: the refusal path, a poll's slow
 * path and a guard's deoptimizing exit. They are emitted after the blocks so
 * the fast paths stay straight-line.
 */

#include <ghoti.io/runtime-jit/macros.h>

#include "emit_internal.h"

#include "../ir/ir_internal.h"

#include <ghoti.io/runtime-core/a/compiled.h>
#include <ghoti.io/runtime-core/a/registry.h>

void grjit_a64_emit_refuse_stub(GRJIT_A64Emit * e) {
  GRJIT_A64Asm * a = &e->as;
  grjit_a64_bind(a, e->refuse);
  /* The refusing value is in x0, already zero-extended. */
  grjit_a64_load(a, GRJIT_A64_MEM_X64, GRJIT_A64_X2, GRJIT_A64_FP, GRJIT_SLOT_OUT);
  grjit_a64_store(a, GRJIT_A64_MEM_X64, GRJIT_A64_X0, GRJIT_A64_X2, 0);
  grjit_a64_mov_ri(a, GRJIT_A64_X0, GRJIT_EXIT_REFUSED);
  grjit_a64_emit_epilogue(e);
}

/* ---- A callable function's exits ---------------------------------------------------- */

#define A64 GRJIT_A64Asm
#define XR(n) GRJIT_A64_X##n

void grjit_a64_emit_store_walk_cell(GRJIT_A64Emit * e, GRJIT_Label ret_label) {
  A64 * a = &e->as;
  grjit_a64_adr(a, GRJIT_A64_X16, ret_label);
  grjit_a64_load(a, GRJIT_A64_MEM_X64, GRJIT_A64_X15, GRJIT_A64_FP, GRJIT_SLOT_CTX);
  grjit_a64_store(a, GRJIT_A64_MEM_X64, GRJIT_A64_FP, GRJIT_A64_X15, (int32_t)e->c.walk_cell_offset);
  grjit_a64_store(a, GRJIT_A64_MEM_X64, GRJIT_A64_X16, GRJIT_A64_X15, (int32_t)e->c.walk_cell_offset + 8);
}

void grjit_a64_emit_callable_epilogue(GRJIT_A64Emit * e) {
  A64 * a = &e->as;
  grjit_a64_mov_sp(a, GRJIT_A64_SP, GRJIT_A64_FP);
  grjit_a64_pop_frame_record(a);
#if defined(GRJIT_TEST_PLANT_BUG) && GRJIT_TEST_PLANT_BUG == 23
  /* Planted defect 23 (tests only): the callee does not pop its stack arguments, so `sp` after
   * a return is lower than before the call by their area. */
#else
  if (e->c.shape.incoming_bytes != 0) {
    grjit_a64_add_sp(a, e->c.shape.incoming_bytes);
  }
#endif
  grjit_a64_ret(a);
}

void grjit_a64_emit_ret_status(GRJIT_A64Emit * e) {
  A64 * a = &e->as;
  grjit_a64_bind(a, e->ret_failed);
  grjit_a64_mov_ri(a, GRJIT_A64_X1, GRJIT_STATUS_FAILED);
  grjit_a64_bind(a, e->ret_propagate);
  grjit_a64_emit_callable_epilogue(e);
}

void grjit_a64_emit_ret_deopted(GRJIT_A64Emit * e) {
  A64 * a = &e->as;
  grjit_a64_bind(a, e->ret_deopted);
  /* `x0` is the cause, and every frame returns it unchanged. */
  grjit_a64_mov_ri(a, GRJIT_A64_X1, GRJIT_STATUS_DEOPTED);
  grjit_a64_emit_callable_epilogue(e);
}

/* The deopt hook (ctx, cause) after the walk start is stored for the site that ends at
 * `ret_label`; the cause is in `x1` already. A refusal of the rebuild (non-zero) is not a
 * deoptimization: every frame returns FAILED with the answer, and the engine hears of it. */
static void call_deopt_hook(GRJIT_A64Emit * e, GRJIT_Label ret_label) {
  A64 * a = &e->as;
  grjit_a64_emit_store_walk_cell(e, ret_label);
  grjit_a64_mov_rr(a, GRJIT_A64_X0, GRJIT_A64_X15);
  grjit_a64_mov_ri(a, GRJIT_A64_X16, (uint64_t)(uintptr_t)e->c.f->hooks.deopt);
  grjit_a64_blr(a, GRJIT_A64_X16);
  grjit_a64_mov32_rr(a, GRJIT_A64_X0, GRJIT_A64_X0);
  grjit_a64_cbnz(a, GRJIT_A64_X0, e->ret_failed);
}

void grjit_a64_emit_call_slow_stub(GRJIT_A64Emit * e, const GRJIT_Pending * p) {
  A64 * a = &e->as;
  grjit_a64_bind(a, p->entry);
  /* `x16` is the slot's word: one means refused, remembered, and the call exits without
   * asking again; zero means empty, and the engine is asked to compile the callee and
   * install it in the slot. */
  grjit_a64_cmp_imm(a, GRJIT_A64_X16, GRCORE_ENTRY_REFUSED);
  grjit_a64_bcond(a, GRJIT_A64_EQ, p->exit);
  grjit_a64_load(a, GRJIT_A64_MEM_X64, GRJIT_A64_X0, GRJIT_A64_FP, GRJIT_SLOT_CTX);
  grjit_a64_mov_ri(a, GRJIT_A64_X1, p->op->callee);
  grjit_a64_mov_ri(a, GRJIT_A64_X16, (uint64_t)(uintptr_t)e->c.f->hooks.compile);
  grjit_a64_blr(a, GRJIT_A64_X16);
  grjit_a64_mov32_rr(a, GRJIT_A64_X0, GRJIT_A64_X0);
  grjit_a64_cbnz(a, GRJIT_A64_X0, p->exit);
  /* The hook says it installed: read the slot again, and if it is still empty or refused
   * the hook lied and the call exits. */
  grjit_a64_mov_ri(a, GRJIT_A64_X16, p->op->address);
  grjit_a64_load(a, GRJIT_A64_MEM_X64, GRJIT_A64_X16, GRJIT_A64_X16, 0);
  grjit_a64_cmp_imm(a, GRJIT_A64_X16, GRCORE_ENTRY_REFUSED);
  grjit_a64_bcond(a, GRJIT_A64_LS, p->exit);
  grjit_a64_b(a, p->back);
}

void grjit_a64_emit_call_exit_stub(GRJIT_A64Emit * e, const GRJIT_Pending * p) {
  A64 * a = &e->as;
  /* A tail call has the one frame state, which is both what its hook's site and its exit
   * describe: the guest frame is still the caller's. */
  const uint32_t exit_state =
      p->op->exit_state != GRJIT_NO_STATE ? p->op->exit_state : p->op->state;
  const GRJIT_FrameState * state = &e->c.f->states[exit_state];
  grjit_a64_bind(a, p->entry);
  GRJIT_Label ret = grjit_a64_label(a);
  grjit_a64_mov_ri(a, GRJIT_A64_X1, 0);
  call_deopt_hook(e, ret);
  grjit_a64_bind(a, ret);
  /* The exit is a site like a guard's, at the address the walk starts from: this frame, in
   * the state before the call. */
  grjit_emit_add_site(&e->c, (uint32_t)grjit_a64_size(a), GRCORE_SITE_GUARD,
      state->identity, p->live_index, exit_state);
  grjit_a64_mov_ri(a, GRJIT_A64_X0, 0);
  grjit_a64_b(a, e->ret_deopted);
}

/* A native call's exit before the call: the native-stack check failed, nothing has been moved
 * or called, and the interpreter will make the call itself. */
void grjit_a64_emit_native_exit_stub(GRJIT_A64Emit * e, const GRJIT_Pending * p) {
  A64 * a = &e->as;
  const GRJIT_FrameState * state = &e->c.f->states[p->op->state];
  grjit_a64_bind(a, p->entry);
  GRJIT_Label ret = grjit_a64_label(a);
  grjit_a64_mov_ri(a, GRJIT_A64_X1, 0);
  call_deopt_hook(e, ret);
  grjit_a64_bind(a, ret);
  grjit_emit_add_site(&e->c, (uint32_t)grjit_a64_size(a), GRCORE_SITE_GUARD,
      state->identity, p->live_index, p->op->state);
  grjit_a64_mov_ri(a, GRJIT_A64_X0, 0);
  grjit_a64_b(a, e->ret_deopted);
}

/* The exit a native's non-zero status takes. The native has run and its result is in `dst`,
 * so the state is the one *after* the call: the interpreter continues past it, and the native
 * is never run twice. The status (its 32 bits, zero-extended) is in `x1`; the cause is the
 * native bit and the status, which every frame returns (the entry puts it in `out[0]`). The
 * cause is kept in the frame's `out` slot, which a callable frame does not otherwise use,
 * across the hook. */
void grjit_a64_emit_native_status_stub(GRJIT_A64Emit * e, const GRJIT_Pending * p) {
  A64 * a = &e->as;
  const GRJIT_FrameState * state = &e->c.f->states[p->op->exit_state];
  grjit_a64_bind(a, p->entry);
  GRJIT_Label ret = grjit_a64_label(a);
  grjit_a64_mov_ri(a, GRJIT_A64_X16, GRJIT_CAUSE_NATIVE);
  grjit_a64_alu(a, GRJIT_A64_ORR, GRJIT_A64_X1, GRJIT_A64_X1, GRJIT_A64_X16);
  grjit_a64_store(a, GRJIT_A64_MEM_X64, GRJIT_A64_X1, GRJIT_A64_FP, GRJIT_SLOT_OUT);
  call_deopt_hook(e, ret);
  grjit_a64_bind(a, ret);
  grjit_emit_add_site(&e->c, (uint32_t)grjit_a64_size(a), GRCORE_SITE_GUARD,
      state->identity, p->live_index, p->op->exit_state);
  grjit_a64_load(a, GRJIT_A64_MEM_X64, GRJIT_A64_X0, GRJIT_A64_FP, GRJIT_SLOT_OUT);
  grjit_a64_b(a, e->ret_deopted);
}

void grjit_a64_emit_overflow_stub(GRJIT_A64Emit * e) {
  A64 * a = &e->as;
  grjit_a64_bind(a, e->overflow);
  /* Entered with the frame record made and nothing else: the frame has no slots yet, so this
   * function has no state to rebuild, and its guest frame, which the caller pushed, is at its
   * entry. The compiled frames to rebuild are the callers': the saved base and the return
   * address are exactly the walk start, unless the caller is the entry adapter, which marks
   * the end of the chain with a word that is no frame base. The context is in `x9`. */
  GRJIT_Label none = grjit_a64_label(a);
  GRJIT_Label go = grjit_a64_label(a);
  grjit_a64_load(a, GRJIT_A64_MEM_X64, GRJIT_A64_X15, GRJIT_A64_FP, 0);
  grjit_a64_mov_ri(a, GRJIT_A64_X16, (uint64_t)GRCORE_COMPILED_CHAIN_END);
  grjit_a64_cmp(a, GRJIT_A64_X15, GRJIT_A64_X16);
  grjit_a64_bcond(a, GRJIT_A64_EQ, none);
  grjit_a64_store(a, GRJIT_A64_MEM_X64, GRJIT_A64_X15, GRJIT_A64_X9, (int32_t)e->c.walk_cell_offset);
  grjit_a64_load(a, GRJIT_A64_MEM_X64, GRJIT_A64_X15, GRJIT_A64_FP, 8);
  grjit_a64_store(a, GRJIT_A64_MEM_X64, GRJIT_A64_X15, GRJIT_A64_X9, (int32_t)e->c.walk_cell_offset + 8);
  grjit_a64_b(a, go);
  grjit_a64_bind(a, none);
  grjit_a64_store(a, GRJIT_A64_MEM_X64, GRJIT_A64_XZR, GRJIT_A64_X9, (int32_t)e->c.walk_cell_offset);
  grjit_a64_store(a, GRJIT_A64_MEM_X64, GRJIT_A64_XZR, GRJIT_A64_X9, (int32_t)e->c.walk_cell_offset + 8);
  grjit_a64_bind(a, go);
  grjit_a64_mov_rr(a, GRJIT_A64_X0, GRJIT_A64_X9);
  grjit_a64_mov_ri(a, GRJIT_A64_X1, 0);
  grjit_a64_mov_ri(a, GRJIT_A64_X16, (uint64_t)(uintptr_t)e->c.f->hooks.deopt);
  grjit_a64_blr(a, GRJIT_A64_X16);
  grjit_a64_mov32_rr(a, GRJIT_A64_X0, GRJIT_A64_X0);
  grjit_a64_cbnz(a, GRJIT_A64_X0, e->ret_failed);
  grjit_a64_mov_ri(a, GRJIT_A64_X0, 0);
  grjit_a64_mov_ri(a, GRJIT_A64_X1, GRJIT_STATUS_DEOPTED);
  grjit_a64_emit_callable_epilogue(e);
}

void grjit_a64_emit_poll_stub(GRJIT_A64Emit * e, const GRJIT_Pending * p) {
  A64 * a = &e->as;
  const GRJIT_FrameState * state = &e->c.f->states[p->op->state];
  if (e->c.callable) {
    /* The walk starts here for the helper, which is a GC point, and for the deoptimization a
     * non-zero answer starts (AD-28). */
    GRJIT_Label ret = grjit_a64_label(a);
    GRJIT_Label deopt = grjit_a64_label(a);
    grjit_a64_bind(a, p->entry);
    grjit_a64_emit_store_walk_cell(e, ret);
    grjit_a64_mov_rr(a, GRJIT_A64_X0, GRJIT_A64_X15);
    grjit_a64_mov_ri(a, GRJIT_A64_X1, state->identity.function);
    grjit_a64_mov_ri(a, GRJIT_A64_X2, state->identity.offset);
    grjit_a64_mov_ri(a, GRJIT_A64_X16, (uint64_t)(uintptr_t)e->c.f->poll_helper);
    grjit_a64_blr(a, GRJIT_A64_X16);
    grjit_a64_bind(a, ret);
    grjit_emit_add_site(&e->c, (uint32_t)grjit_a64_size(a), GRCORE_SITE_GC_POINT_POLL,
        state->identity, p->live_index, p->op->state);
    grjit_a64_mov32_rr(a, GRJIT_A64_X0, GRJIT_A64_X0);
    grjit_a64_cbnz(a, GRJIT_A64_X0, deopt);
    grjit_a64_b(a, p->back);
    /* A pause or an unwind verdict, or whatever the engine's helper decided: the chain is
     * rebuilt into its guest frames and every frame returns DEOPTED, carrying the helper's
     * answer as the cause. The cause is kept in the slot the callable frame does not
     * otherwise use. */
    grjit_a64_bind(a, deopt);
    grjit_a64_store(a, GRJIT_A64_MEM_X64, GRJIT_A64_X0, GRJIT_A64_FP, GRJIT_SLOT_OUT);
    grjit_a64_mov_rr(a, GRJIT_A64_X1, GRJIT_A64_X0);
    call_deopt_hook(e, ret);
    grjit_a64_load(a, GRJIT_A64_MEM_X64, GRJIT_A64_X0, GRJIT_A64_FP, GRJIT_SLOT_OUT);
    grjit_a64_b(a, e->ret_deopted);
    return;
  }
  grjit_a64_bind(a, p->entry);
  grjit_a64_load(a, GRJIT_A64_MEM_X64, GRJIT_A64_X0, GRJIT_A64_FP, GRJIT_SLOT_CTX);
  grjit_a64_mov_ri(a, GRJIT_A64_X1, state->identity.function);
  grjit_a64_mov_ri(a, GRJIT_A64_X2, state->identity.offset);
  grjit_a64_mov_ri(a, GRJIT_A64_X16, (uint64_t)(uintptr_t)e->c.f->poll_helper);
  grjit_a64_blr(a, GRJIT_A64_X16);
  /* The return address is the site: the helper may be a GC point. */
  grjit_emit_add_site(&e->c, (uint32_t)grjit_a64_size(a),
      GRCORE_SITE_GC_POINT_POLL, state->identity, p->live_index, p->op->state);
  grjit_a64_mov32_rr(a, GRJIT_A64_X0, GRJIT_A64_X0);
  grjit_a64_cbnz(a, GRJIT_A64_X0, e->refuse);
  grjit_a64_b(a, p->back);
}

void grjit_a64_emit_guard_stub(GRJIT_A64Emit * e, const GRJIT_Pending * p) {
  GRJIT_A64Asm * a = &e->as;
  const GRJIT_FrameState * state = &e->c.f->states[p->op->state];
  if (e->c.callable) {
    /* A guard anywhere in a chain rebuilds every compiled frame, this one included, from the
     * walk start stored here, and returns DEOPTED. The site is at the address the walk
     * starts from. */
    GRJIT_Label ret = grjit_a64_label(a);
    grjit_a64_bind(a, p->entry);
    grjit_a64_mov_ri(a, GRJIT_A64_X1, 0);
    call_deopt_hook(e, ret);
    grjit_a64_bind(a, ret);
    grjit_emit_add_site(&e->c, (uint32_t)grjit_a64_size(a), GRCORE_SITE_GUARD,
        state->identity, p->live_index, p->op->state);
    grjit_a64_mov_ri(a, GRJIT_A64_X0, 0);
    grjit_a64_b(a, e->ret_deopted);
    return;
  }
  grjit_a64_bind(a, p->entry);
  uint32_t offset = (uint32_t)grjit_a64_size(a);
  grjit_emit_add_site(&e->c, offset, GRCORE_SITE_GUARD, state->identity,
      p->live_index, p->op->state);
  grjit_a64_load(a, GRJIT_A64_MEM_X64, GRJIT_A64_X2, GRJIT_A64_FP, GRJIT_SLOT_OUT);
  for (size_t i = 0; i < state->slot_count; i++) {
    const GRJIT_FrameSlot * slot = &state->slots[i];
    if (slot->kind == GRJIT_FRAME_SLOT_VREG) {
      grjit_a64_load(a, GRJIT_A64_MEM_X64, GRJIT_A64_X0, GRJIT_A64_FP,
          grjit_emit_slot(slot->vreg));
    } else if (slot->kind == GRJIT_FRAME_SLOT_CONSTANT) {
      grjit_a64_mov_ri(a, GRJIT_A64_X0, (uint64_t)slot->constant);
    } else {
      grjit_a64_mov_ri(a, GRJIT_A64_X0, 0);
    }
    grjit_a64_store(
        a, GRJIT_A64_MEM_X64, GRJIT_A64_X0, GRJIT_A64_X2, (int32_t)(8 * i));
  }
  grjit_a64_mov_ri(a, GRJIT_A64_X0, offset);
  grjit_a64_store(a, GRJIT_A64_MEM_X64, GRJIT_A64_X0, GRJIT_A64_X2,
      (int32_t)(8 * state->slot_count));
  grjit_a64_mov_ri(a, GRJIT_A64_X0, GRJIT_EXIT_DEOPT);
  grjit_a64_emit_epilogue(e);
}
