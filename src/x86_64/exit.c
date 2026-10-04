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

void grjit_emit_refuse_stub(GRJIT_Emit * e) {
  GRJIT_Asm * a = &e->as;
  grjit_asm_bind(a, e->refuse);
  /* The refusing value is in eax, already zero-extended. */
  grjit_asm_load64(a, GRJIT_RDX, GRJIT_RBP, GRJIT_SLOT_OUT);
  grjit_asm_store64(a, GRJIT_RDX, 0, GRJIT_RAX);
  grjit_asm_mov_ri(a, GRJIT_RAX, GRJIT_EXIT_REFUSED);
  grjit_emit_epilogue(e);
}

void grjit_emit_poll_stub(GRJIT_Emit * e, const GRJIT_Pending * p) {
  GRJIT_Asm * a = &e->as;
  const GRJIT_FrameState * state = &e->c.f->states[p->op->state];
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
