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

void grjit_a64_emit_refuse_stub(GRJIT_A64Emit * e) {
  GRJIT_A64Asm * a = &e->as;
  grjit_a64_bind(a, e->refuse);
  /* The refusing value is in x0, already zero-extended. */
  grjit_a64_load(a, GRJIT_A64_MEM_X64, GRJIT_A64_X2, GRJIT_A64_FP, GRJIT_SLOT_OUT);
  grjit_a64_store(a, GRJIT_A64_MEM_X64, GRJIT_A64_X0, GRJIT_A64_X2, 0);
  grjit_a64_mov_ri(a, GRJIT_A64_X0, GRJIT_EXIT_REFUSED);
  grjit_a64_emit_epilogue(e);
}

void grjit_a64_emit_poll_stub(GRJIT_A64Emit * e, const GRJIT_Pending * p) {
  GRJIT_A64Asm * a = &e->as;
  const GRJIT_FrameState * state = &e->c.f->states[p->op->state];
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
