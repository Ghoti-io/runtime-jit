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
 * The out-of-line stubs: the refusal path, a poll's slow path and a guard's
 * deoptimizing exit. They are emitted after the blocks so the fast paths stay
 * straight-line.
 */

#include <ghoti.io/runtime-jit/macros.h>

#include "emit_internal.h"

#include "../ir/ir_internal.h"

#include <ghoti.io/runtime-core/a/compiled.h>
#include <ghoti.io/runtime-core/a/registry.h>

void grjit_emit_refuse_stub(GRJIT_Emit * e) {
  GRJIT_Asm * a = &e->as;
  grjit_asm_bind(a, e->refuse);
  /* The refusing value is in eax, already zero-extended. */
  grjit_asm_load64(a, GRJIT_RDX, GRJIT_RBP, GRJIT_SLOT_OUT);
  grjit_asm_store64(a, GRJIT_RDX, 0, GRJIT_RAX);
  grjit_asm_mov_ri(a, GRJIT_RAX, GRJIT_EXIT_REFUSED);
  grjit_emit_epilogue(e);
}

void grjit_emit_store_walk_cell(GRJIT_Emit * e, GRJIT_Label ret_label) {
  GRJIT_Asm * a = &e->as;
  grjit_asm_lea_rip(a, GRJIT_RAX, ret_label);
  grjit_asm_load64(a, GRJIT_RCX, GRJIT_RBP, GRJIT_SLOT_CTX);
  grjit_asm_store64(a, GRJIT_RCX, (int32_t)e->c.walk_cell_offset, GRJIT_RBP);
  grjit_asm_store64(a, GRJIT_RCX, (int32_t)e->c.walk_cell_offset + 8, GRJIT_RAX);
}

void grjit_emit_callable_epilogue(GRJIT_Emit * e) {
  grjit_asm_leave(&e->as);
  grjit_asm_ret_imm(&e->as, (uint16_t)e->c.shape.incoming_bytes);
}

void grjit_emit_ret_status(GRJIT_Emit * e) {
  GRJIT_Asm * a = &e->as;
  grjit_asm_bind(a, e->ret_failed);
  grjit_asm_mov_ri(a, GRJIT_RDX, GRJIT_STATUS_FAILED);
  grjit_asm_bind(a, e->ret_propagate);
  grjit_emit_callable_epilogue(e);
}

void grjit_emit_ret_deopted(GRJIT_Emit * e) {
  GRJIT_Asm * a = &e->as;
  grjit_asm_bind(a, e->ret_deopted);
  /* `rax` is the cause, and every frame returns it unchanged. */
  grjit_asm_mov_ri(a, GRJIT_RDX, GRJIT_STATUS_DEOPTED);
  grjit_emit_callable_epilogue(e);
}

/* The deopt hook (ctx, cause) after the walk start is stored for the site that
 * ends at `ret_label`; the cause is in `rsi` already. */
static void call_deopt_hook(GRJIT_Emit * e, GRJIT_Label ret_label) {
  GRJIT_Asm * a = &e->as;
  grjit_emit_store_walk_cell(e, ret_label);
  grjit_asm_mov_rr(a, GRJIT_RDI, GRJIT_RCX);
  grjit_asm_mov_ri(a, GRJIT_RAX, (uint64_t)(uintptr_t)e->c.f->hooks.deopt);
  grjit_asm_call_r(a, GRJIT_RAX);
  /* A refusal of the rebuild (non-zero) is not a deoptimization: every frame
   * returns FAILED with the answer, and the engine hears of it. */
  grjit_asm_mov32_rr(a, GRJIT_RAX, GRJIT_RAX);
  grjit_asm_test_rr(a, GRJIT_RAX, GRJIT_RAX);
  grjit_asm_jcc(a, GRJIT_COND_NE, e->ret_failed);
}

void grjit_emit_call_slow_stub(GRJIT_Emit * e, const GRJIT_Pending * p) {
  GRJIT_Asm * a = &e->as;
  grjit_asm_bind(a, p->entry);
  /* `rax` is the slot's word: one means refused, remembered, and the call exits
   * without asking again; zero means empty, and the engine is asked to compile
   * the callee and install it in the slot. */
  grjit_asm_cmp_ri(a, GRJIT_RAX, (int32_t)GRCORE_ENTRY_REFUSED);
  grjit_asm_jcc(a, GRJIT_COND_E, p->exit);
  grjit_asm_load64(a, GRJIT_RDI, GRJIT_RBP, GRJIT_SLOT_CTX);
  grjit_asm_mov_ri(a, GRJIT_RSI, p->op->callee);
  grjit_asm_mov_ri(a, GRJIT_RAX, (uint64_t)(uintptr_t)e->c.f->hooks.compile);
  grjit_asm_call_r(a, GRJIT_RAX);
  grjit_asm_mov32_rr(a, GRJIT_RAX, GRJIT_RAX);
  grjit_asm_test_rr(a, GRJIT_RAX, GRJIT_RAX);
  grjit_asm_jcc(a, GRJIT_COND_NE, p->exit);
  /* The hook says it installed: read the slot again, and if it is still empty
   * the hook lied and the call exits. */
  grjit_asm_mov_ri(a, GRJIT_RAX, p->op->address);
  grjit_asm_load64(a, GRJIT_RAX, GRJIT_RAX, 0);
  grjit_asm_cmp_ri(a, GRJIT_RAX, (int32_t)GRCORE_ENTRY_REFUSED);
  grjit_asm_jcc(a, GRJIT_COND_BE, p->exit);
  grjit_asm_jmp(a, p->back);
}

void grjit_emit_call_exit_stub(GRJIT_Emit * e, const GRJIT_Pending * p) {
  GRJIT_Asm * a = &e->as;
  /* A tail call has the one frame state, which is both what its hook's site
   * and its exit describe: the guest frame is still the caller's. */
  const uint32_t exit_state =
      p->op->exit_state != GRJIT_NO_STATE ? p->op->exit_state : p->op->state;
  const GRJIT_FrameState * state = &e->c.f->states[exit_state];
  grjit_asm_bind(a, p->entry);
  GRJIT_Label ret = grjit_asm_label(a);
  grjit_asm_mov_ri(a, GRJIT_RSI, 0);
  call_deopt_hook(e, ret);
  grjit_asm_bind(a, ret);
  /* The exit is a site like a guard's, at the return address of the hook call:
   * the walk starts here, in this frame, in the state before the call. */
  grjit_emit_add_site(&e->c, (uint32_t)grjit_asm_size(a), GRCORE_SITE_GUARD,
      state->identity, p->live_index, exit_state);
  grjit_asm_mov_ri(a, GRJIT_RAX, 0);
  grjit_asm_jmp(a, e->ret_deopted);
}

/* A native call's exit before the call: the native-stack check failed, nothing
 * has been moved or called, and the interpreter will make the call itself. It is
 * the call exit's stub over the call's own state (the operation's `exit_state` is
 * the state *after* the call, which is not this one). */
void grjit_emit_native_exit_stub(GRJIT_Emit * e, const GRJIT_Pending * p) {
  GRJIT_Asm * a = &e->as;
  const GRJIT_FrameState * state = &e->c.f->states[p->op->state];
  grjit_asm_bind(a, p->entry);
  GRJIT_Label ret = grjit_asm_label(a);
  grjit_asm_mov_ri(a, GRJIT_RSI, 0);
  call_deopt_hook(e, ret);
  grjit_asm_bind(a, ret);
  grjit_emit_add_site(&e->c, (uint32_t)grjit_asm_size(a), GRCORE_SITE_GUARD,
      state->identity, p->live_index, p->op->state);
  grjit_asm_mov_ri(a, GRJIT_RAX, 0);
  grjit_asm_jmp(a, e->ret_deopted);
}

/* The exit a native's non-zero status takes. The native has run and its result
 * is in `dst`, so the state is the one *after* the call: the interpreter
 * continues past it, and the native is never run twice. The status is in `rdx`;
 * the cause is the native bit and its low thirty-two bits, and every frame
 * returns it (the entry puts it in `out[0]`). The cause is kept in the frame's
 * `out` slot, which a callable frame does not otherwise use, across the hook. */
void grjit_emit_native_status_stub(GRJIT_Emit * e, const GRJIT_Pending * p) {
  GRJIT_Asm * a = &e->as;
  const GRJIT_FrameState * state = &e->c.f->states[p->op->exit_state];
  grjit_asm_bind(a, p->entry);
  GRJIT_Label ret = grjit_asm_label(a);
  grjit_asm_mov32_rr(a, GRJIT_RSI, GRJIT_RDX);
  grjit_asm_mov_ri(a, GRJIT_RAX, GRJIT_CAUSE_NATIVE);
  grjit_asm_alu_rr(a, GRJIT_ALU_OR, GRJIT_RSI, GRJIT_RAX);
  grjit_asm_store64(a, GRJIT_RBP, GRJIT_SLOT_OUT, GRJIT_RSI);
  call_deopt_hook(e, ret);
  grjit_asm_bind(a, ret);
  grjit_emit_add_site(&e->c, (uint32_t)grjit_asm_size(a), GRCORE_SITE_GUARD,
      state->identity, p->live_index, p->op->exit_state);
  grjit_asm_load64(a, GRJIT_RAX, GRJIT_RBP, GRJIT_SLOT_OUT);
  grjit_asm_jmp(a, e->ret_deopted);
}

void grjit_emit_overflow_stub(GRJIT_Emit * e) {
  GRJIT_Asm * a = &e->as;
  grjit_asm_bind(a, e->overflow);
  /* Entered with `push rbp; mov rbp, rsp` done and nothing else: the frame has
   * no slots yet, so this function has no state to rebuild, and its guest
   * frame, which the caller pushed, is at its entry. The compiled frames to
   * rebuild are the callers': the saved base and return address are exactly
   * the walk start, unless the caller is the entry adapter, which marks the end
   * of the chain with a word that is no frame base. */
  GRJIT_Label none = grjit_asm_label(a);
  GRJIT_Label go = grjit_asm_label(a);
  grjit_asm_load64(a, GRJIT_RCX, GRJIT_RBP, 0);
  grjit_asm_mov_ri64(a, GRJIT_RAX, (uint64_t)GRCORE_COMPILED_CHAIN_END);
  grjit_asm_alu_rr(a, GRJIT_ALU_CMP, GRJIT_RCX, GRJIT_RAX);
  grjit_asm_jcc(a, GRJIT_COND_E, none);
  grjit_asm_store64(a, GRJIT_R10, (int32_t)e->c.walk_cell_offset, GRJIT_RCX);
  grjit_asm_load64(a, GRJIT_RCX, GRJIT_RBP, 8);
  grjit_asm_store64(a, GRJIT_R10, (int32_t)e->c.walk_cell_offset + 8, GRJIT_RCX);
  grjit_asm_jmp(a, go);
  grjit_asm_bind(a, none);
  grjit_asm_mov_ri(a, GRJIT_RCX, 0);
  grjit_asm_store64(a, GRJIT_R10, (int32_t)e->c.walk_cell_offset, GRJIT_RCX);
  grjit_asm_store64(a, GRJIT_R10, (int32_t)e->c.walk_cell_offset + 8, GRJIT_RCX);
  grjit_asm_bind(a, go);
  grjit_asm_mov_rr(a, GRJIT_RDI, GRJIT_R10);
  grjit_asm_mov_ri(a, GRJIT_RSI, 0);
  grjit_asm_mov_ri(a, GRJIT_RAX, (uint64_t)(uintptr_t)e->c.f->hooks.deopt);
  grjit_asm_call_r(a, GRJIT_RAX);
  grjit_asm_mov32_rr(a, GRJIT_RAX, GRJIT_RAX);
  grjit_asm_test_rr(a, GRJIT_RAX, GRJIT_RAX);
  grjit_asm_jcc(a, GRJIT_COND_NE, e->ret_failed);
  grjit_asm_mov_ri(a, GRJIT_RAX, 0);
  grjit_asm_mov_ri(a, GRJIT_RDX, GRJIT_STATUS_DEOPTED);
  grjit_emit_callable_epilogue(e);
}

void grjit_emit_poll_stub(GRJIT_Emit * e, const GRJIT_Pending * p) {
  GRJIT_Asm * a = &e->as;
  const GRJIT_FrameState * state = &e->c.f->states[p->op->state];
  if (e->c.callable) {
    /* The walk starts here for the helper, which is a GC point, and for the
     * deoptimization a non-zero answer starts (AD-28). */
    GRJIT_Label ret = grjit_asm_label(a);
    GRJIT_Label deopt = grjit_asm_label(a);
    grjit_asm_bind(a, p->entry);
    grjit_emit_store_walk_cell(e, ret);
    grjit_asm_mov_rr(a, GRJIT_RDI, GRJIT_RCX);
    grjit_asm_mov_ri(a, GRJIT_RSI, state->identity.function);
    grjit_asm_mov_ri(a, GRJIT_RDX, state->identity.offset);
    grjit_asm_mov_ri(a, GRJIT_RAX, (uint64_t)(uintptr_t)e->c.f->poll_helper);
    grjit_asm_call_r(a, GRJIT_RAX);
    grjit_asm_bind(a, ret);
    grjit_emit_add_site(&e->c, (uint32_t)grjit_asm_size(a),
        GRCORE_SITE_GC_POINT_POLL, state->identity, p->live_index,
        p->op->state);
    grjit_asm_mov32_rr(a, GRJIT_RAX, GRJIT_RAX);
    grjit_asm_test_rr(a, GRJIT_RAX, GRJIT_RAX);
    grjit_asm_jcc(a, GRJIT_COND_NE, deopt);
    grjit_asm_jmp(a, p->back);
    /* A pause or an unwind verdict, or whatever the engine's helper decided:
     * the chain is rebuilt into its guest frames and every frame returns
     * DEOPTED, carrying the helper's answer as the cause. The cause is kept in
     * the slot the callable frame does not otherwise use. */
    grjit_asm_bind(a, deopt);
    grjit_asm_store64(a, GRJIT_RBP, GRJIT_SLOT_OUT, GRJIT_RAX);
    grjit_asm_mov_rr(a, GRJIT_RSI, GRJIT_RAX);
    call_deopt_hook(e, ret);
    grjit_asm_load64(a, GRJIT_RAX, GRJIT_RBP, GRJIT_SLOT_OUT);
    grjit_asm_jmp(a, e->ret_deopted);
    return;
  }
  grjit_asm_bind(a, p->entry);
  /* The helper is (context, function, offset): rdi, rsi, rdx on SysV; rcx,
   * rdx, r8 on Win64. */
  if (e->win64) {
    grjit_asm_load64(a, GRJIT_RCX, GRJIT_RBP, GRJIT_SLOT_CTX);
    grjit_asm_mov_ri(a, GRJIT_RDX, state->identity.function);
    grjit_asm_mov_ri(a, GRJIT_R8, state->identity.offset);
  } else {
    grjit_asm_load64(a, GRJIT_RDI, GRJIT_RBP, GRJIT_SLOT_CTX);
    grjit_asm_mov_ri(a, GRJIT_RSI, state->identity.function);
    grjit_asm_mov_ri(a, GRJIT_RDX, state->identity.offset);
  }
  grjit_asm_mov_ri(a, GRJIT_RAX, (uint64_t)(uintptr_t)e->c.f->poll_helper);
  grjit_asm_call_r(a, GRJIT_RAX);
  /* The return address is the site: the helper may be a GC point. */
  grjit_emit_add_site(&e->c, (uint32_t)grjit_asm_size(a),
      GRCORE_SITE_GC_POINT_POLL, state->identity, p->live_index,
      p->op->state);
  grjit_asm_mov32_rr(a, GRJIT_RAX, GRJIT_RAX);
  grjit_asm_test_rr(a, GRJIT_RAX, GRJIT_RAX);
  grjit_asm_jcc(a, GRJIT_COND_NE, e->refuse);
  grjit_asm_jmp(a, p->back);
}

void grjit_emit_guard_stub(GRJIT_Emit * e, const GRJIT_Pending * p) {
  GRJIT_Asm * a = &e->as;
  const GRJIT_FrameState * state = &e->c.f->states[p->op->state];
  if (e->c.callable) {
    /* A guard anywhere in a chain rebuilds every compiled frame, this one
     * included, from the walk start stored here, and returns DEOPTED. The site
     * is at the return address of the hook call. */
    GRJIT_Label ret = grjit_asm_label(a);
    grjit_asm_bind(a, p->entry);
    grjit_asm_mov_ri(a, GRJIT_RSI, 0);
    call_deopt_hook(e, ret);
    grjit_asm_bind(a, ret);
    grjit_emit_add_site(&e->c, (uint32_t)grjit_asm_size(a), GRCORE_SITE_GUARD,
        state->identity, p->live_index, p->op->state);
    grjit_asm_mov_ri(a, GRJIT_RAX, 0);
    grjit_asm_jmp(a, e->ret_deopted);
    return;
  }
  grjit_asm_bind(a, p->entry);
  uint32_t offset = (uint32_t)grjit_asm_size(a);
  grjit_emit_add_site(
      &e->c, offset, GRCORE_SITE_GUARD, state->identity, p->live_index,
      p->op->state);
  grjit_asm_load64(a, GRJIT_RDX, GRJIT_RBP, GRJIT_SLOT_OUT);
  for (size_t i = 0; i < state->slot_count; i++) {
    const GRJIT_FrameSlot * slot = &state->slots[i];
    if (slot->kind == GRJIT_FRAME_SLOT_VREG) {
      grjit_asm_load64(
          a, GRJIT_RAX, GRJIT_RBP, grjit_emit_slot(slot->vreg));
    } else if (slot->kind == GRJIT_FRAME_SLOT_CONSTANT) {
      grjit_asm_mov_ri(a, GRJIT_RAX, (uint64_t)slot->constant);
    } else {
      grjit_asm_mov_ri(a, GRJIT_RAX, 0);
    }
    grjit_asm_store64(a, GRJIT_RDX, (int32_t)(8 * i), GRJIT_RAX);
  }
  grjit_asm_mov_ri(a, GRJIT_RAX, offset);
  grjit_asm_store64(a, GRJIT_RDX, (int32_t)(8 * state->slot_count), GRJIT_RAX);
  grjit_asm_mov_ri(a, GRJIT_RAX, GRJIT_EXIT_DEOPT);
  grjit_emit_epilogue(e);
}
