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
 * The part of a compile that no instruction set decides: the sites the emitter
 * records as it goes, the stubs it queues for after the blocks, and the sort
 * that puts the sites in the order the metadata builder needs.
 */

#include <ghoti.io/runtime-jit/macros.h>

#include "backend_internal.h"

#include "../ir/ir_internal.h"

#include <ghoti.io/runtime-core/a/layout.h>

#include <stdlib.h>
#include <string.h>

void grjit_emit_common_init(GRJIT_EmitCommon * c, const GRJIT_Function * f,
    const GRJIT_Allocator * allocator, GRJIT_EntryHook hook,
    uint32_t request_offset, uint32_t frame_bytes, const GRJIT_LiveSites * live) {
  memset(c, 0, sizeof *c);
  c->f = f;
  c->allocator = allocator;
  c->frame_bytes = frame_bytes;
  c->request_offset = request_offset;
  c->hook = hook;
  c->live = live;
  c->error = GRJIT_OK;
  c->callable = f->callable;
  if (f->callable) {
    const GRCORE_JitLayout * layout = grcore_jit_layout();
    c->walk_cell_offset = layout->walk_cell_offset;
    c->native_limit_offset = layout->native_limit_offset;
    grjit_callable_shape(f, &c->shape);
  }
}

void grjit_callable_shape(const GRJIT_Function * f, GRJIT_CallableShape * out) {
  memset(out, 0, sizeof *out);
  if (!f->callable) {
    return;
  }
  /* Whole 16-byte units, so the stack stays 16-aligned across the call. */
  out->incoming_bytes = grjit_stack_arg_bytes(f->param_count);
  for (size_t b = 0; b < f->block_count; b++) {
    for (size_t i = 0; i < f->blocks[b].count; i++) {
      const GRJIT_Op * op = &f->blocks[b].ops[i];
      bool tail = op->kind == GRJIT_OP_TAIL_CALL_SLOT || op->kind == GRJIT_OP_TAIL_CALL_PTR;
      if (op->kind == GRJIT_OP_CALL_SLOT || op->kind == GRJIT_OP_CALL_PTR || tail) {
        out->has_calls = true;
        if (op->arg_count > out->args_area) {
          out->args_area = op->arg_count;
        }
      }
      if (tail) {
        /* The replacement writes down to `ra' = ra + in_A - in_T` (emit.c), and
         * the area, which holds the sources, must end at or below it: the area
         * ends `pad + vreg_count + 3` slots below the frame base and `ra'` is
         * `8 + in_A - in_T` above it. */
        int64_t in_t = grjit_stack_arg_bytes(op->arg_count);
        int64_t want = (in_t - (int64_t)out->incoming_bytes) / 8 - ((int64_t)f->vreg_count + 4);
        if (want > 0 && (size_t)want > out->pad) {
          out->pad = (size_t)want;
        }
      }
    }
  }
  out->extra_slots = out->pad + out->args_area + (out->has_calls ? 1u : 0u);
}

void grjit_emit_add_site(GRJIT_EmitCommon * c, uint32_t offset,
    GRCORE_CodeSiteKind kind, GRCORE_PollIdentity identity, size_t live_index,
    uint32_t state) {
  grjit_emit_add_site_for(c, offset, kind, identity, live_index, state, NULL);
}

void grjit_emit_add_site_for(GRJIT_EmitCommon * c, uint32_t offset,
    GRCORE_CodeSiteKind kind, GRCORE_PollIdentity identity, size_t live_index,
    uint32_t state, const GRJIT_Op * op) {
  if (c->error != GRJIT_OK) {
    return;
  }
  if (c->site_count == c->site_capacity) {
    size_t cap = c->site_capacity == 0 ? 16 : c->site_capacity * 2;
    GRJIT_SiteRec * grown =
        c->allocator->realloc_fn(c->allocator->ctx, c->sites, cap * sizeof *grown);
    if (grown == NULL) {
      c->error = GRJIT_ERR_OOM;
      return;
    }
    c->sites = grown;
    c->site_capacity = cap;
  }
  GRJIT_SiteRec * s = &c->sites[c->site_count++];
  s->offset = offset;
  s->kind = kind;
  s->identity = identity;
  s->live_index = live_index;
  s->state = state;
  s->op = op;
}

void grjit_emit_add_pending(GRJIT_EmitCommon * c, const GRJIT_Pending * p) {
  if (c->error != GRJIT_OK) {
    return;
  }
  if (c->pending_count == c->pending_capacity) {
    size_t cap = c->pending_capacity == 0 ? 16 : c->pending_capacity * 2;
    GRJIT_Pending * grown = c->allocator->realloc_fn(
        c->allocator->ctx, c->pending, cap * sizeof *grown);
    if (grown == NULL) {
      c->error = GRJIT_ERR_OOM;
      return;
    }
    c->pending = grown;
    c->pending_capacity = cap;
  }
  c->pending[c->pending_count++] = *p;
}

static int site_order(const void * x, const void * y) {
  const GRJIT_SiteRec * a = x;
  const GRJIT_SiteRec * b = y;
  return a->offset < b->offset ? -1 : a->offset > b->offset ? 1 : 0;
}

void grjit_emit_sort_sites(GRJIT_EmitCommon * c) {
  if (c->site_count > 1) {
    qsort(c->sites, c->site_count, sizeof *c->sites, site_order);
  }
}

void grjit_emit_common_free(GRJIT_EmitCommon * c) {
  if (c->allocator == NULL) {
    return;
  }
  c->allocator->free_fn(c->allocator->ctx, c->sites);
  c->allocator->free_fn(c->allocator->ctx, c->pending);
  memset(c, 0, sizeof *c);
}
