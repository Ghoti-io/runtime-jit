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
}

void grjit_emit_add_site(GRJIT_EmitCommon * c, uint32_t offset,
    GRCORE_CodeSiteKind kind, GRCORE_PollIdentity identity, size_t live_index,
    uint32_t state) {
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
