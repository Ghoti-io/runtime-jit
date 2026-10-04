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
 * Backward liveness over the IR's blocks, for the registers a collector must
 * be told about (REF registers and derived pointers).
 */

#include <ghoti.io/runtime-jit/macros.h>

#include "liveness_internal.h"

#include "../ir/ir_internal.h"

#include <string.h>

typedef struct Ctx {
  const GRJIT_Function * f;
  const uint32_t * index; /* vreg -> bit, or UINT32_MAX when not tracked */
  uint64_t * set;         /* the set being built */
} Ctx;

static void set_bit(uint64_t * s, uint32_t bit) {
  s[bit / 64] |= UINT64_C(1) << (bit % 64);
}

static void clear_bit(uint64_t * s, uint32_t bit) {
  s[bit / 64] &= ~(UINT64_C(1) << (bit % 64));
}

static bool test_bit(const uint64_t * s, uint32_t bit) {
  return (s[bit / 64] >> (bit % 64)) & 1u;
}

static void add_use(void * user, GRJIT_VReg v) {
  Ctx * c = user;
  if (v < c->f->vreg_count && c->index[v] != UINT32_MAX) {
    set_bit(c->set, c->index[v]);
  }
}

static void successors(const GRJIT_Function * f, const GRJIT_BlockInfo * b,
    GRJIT_BlockId out[2], size_t * count) {
  *count = 0;
  if (b->count == 0) {
    return;
  }
  const GRJIT_Op * last = &b->ops[b->count - 1];
  if (last->kind == GRJIT_OP_BR && last->target < f->block_count) {
    out[(*count)++] = last->target;
  } else if (last->kind == GRJIT_OP_BR_IF) {
    if (last->target < f->block_count) {
      out[(*count)++] = last->target;
    }
    if (last->target_else < f->block_count) {
      out[(*count)++] = last->target_else;
    }
  }
}

/* Applies one operation backward to `live`. */
static void transfer(Ctx * c, const GRJIT_Op * op, uint64_t * live) {
  GRJIT_VReg def = grjit_op_def(op);
  if (def != GRJIT_NO_VREG && def < c->f->vreg_count &&
      c->index[def] != UINT32_MAX) {
    clear_bit(live, c->index[def]);
  }
  c->set = live;
  grjit_op_visit_uses(c->f, op, add_use, c);
}

GRJIT_Result grjit_liveness_compute(const GRJIT_Function * f,
    const GRJIT_Allocator * a, GRJIT_LiveSites * out) {
  memset(out, 0, sizeof *out);
  out->allocator = a;
  size_t site_total = 0;
  for (size_t b = 0; b < f->block_count; b++) {
    for (size_t i = 0; i < f->blocks[b].count; i++) {
      if (grjit_op_has_state(&f->blocks[b].ops[i])) {
        site_total++;
      }
    }
  }
  if (site_total == 0) {
    return GRJIT_OK;
  }
  uint32_t * index = a->malloc_fn(a->ctx, (f->vreg_count + 1) * sizeof *index);
  if (index == NULL) {
    return GRJIT_ERR_OOM;
  }
  uint32_t tracked = 0;
  for (size_t v = 0; v < f->vreg_count; v++) {
    bool t = f->vregs[v].type == GRJIT_TYPE_REF ||
             (f->vregs[v].type == GRJIT_TYPE_PTR && f->vregs[v].derived);
    index[v] = t ? tracked++ : UINT32_MAX;
  }
  size_t words = ((size_t)tracked + 63) / 64;
  if (words == 0) {
    words = 1;
  }
  /* live_in per block, then two scratch sets and one per-site set. */
  uint64_t * live_in = a->calloc_fn(a->ctx, f->block_count * words + 3 * words,
      sizeof *live_in);
  GRJIT_SiteLive * sites = a->calloc_fn(a->ctx, site_total, sizeof *sites);
  GRJIT_VReg * pool = NULL;
  if (live_in == NULL || sites == NULL) {
    a->free_fn(a->ctx, index);
    a->free_fn(a->ctx, live_in);
    a->free_fn(a->ctx, sites);
    return GRJIT_ERR_OOM;
  }
  uint64_t * scratch = live_in + f->block_count * words;
  uint64_t * cur = scratch;
  uint64_t * site_set = scratch + words;
  uint64_t * state_set = scratch + 2 * words;
  Ctx c = {f, index, NULL};

  bool changed = true;
  while (changed) {
    changed = false;
    for (size_t bi = f->block_count; bi-- > 0;) {
      const GRJIT_BlockInfo * b = &f->blocks[bi];
      GRJIT_BlockId succ[2];
      size_t sn;
      successors(f, b, succ, &sn);
      memset(cur, 0, words * sizeof *cur);
      for (size_t s = 0; s < sn; s++) {
        for (size_t w = 0; w < words; w++) {
          cur[w] |= live_in[succ[s] * words + w];
        }
      }
      for (size_t i = b->count; i-- > 0;) {
        transfer(&c, &b->ops[i], cur);
      }
      if (memcmp(cur, &live_in[bi * words], words * sizeof *cur) != 0) {
        memcpy(&live_in[bi * words], cur, words * sizeof *cur);
        changed = true;
      }
    }
  }

  /* Final pass: record the set at each site. Two sweeps, the first to size the
   * pool, the second to fill it. */
  size_t pool_size = 0;
  for (int pass = 0; pass < 2; pass++) {
    size_t at = 0;
    size_t fill = 0;
    for (size_t bi = 0; bi < f->block_count; bi++) {
      const GRJIT_BlockInfo * b = &f->blocks[bi];
      GRJIT_BlockId succ[2];
      size_t sn;
      successors(f, b, succ, &sn);
      memset(cur, 0, words * sizeof *cur);
      for (size_t s = 0; s < sn; s++) {
        for (size_t w = 0; w < words; w++) {
          cur[w] |= live_in[succ[s] * words + w];
        }
      }
      /* Sites in a block are met backward; their slots in `sites` are in
       * forward order, so count the block's sites to place them. */
      size_t block_sites = 0;
      for (size_t i = 0; i < b->count; i++) {
        if (grjit_op_has_state(&b->ops[i])) {
          block_sites++;
        }
      }
      size_t slot = at + block_sites;
      for (size_t i = b->count; i-- > 0;) {
        const GRJIT_Op * op = &b->ops[i];
        if (grjit_op_has_state(op)) {
          slot--;
          memset(state_set, 0, words * sizeof *state_set);
          c.set = state_set;
          if (op->state != GRJIT_NO_STATE && op->state < f->state_count) {
            const GRJIT_FrameState * st = &f->states[op->state];
            for (size_t k = 0; k < st->slot_count; k++) {
              if (st->slots[k].kind == GRJIT_FRAME_SLOT_VREG) {
                add_use(&c, st->slots[k].vreg);
              }
            }
          }
          if (op->kind == GRJIT_OP_GUARD) {
            memcpy(site_set, state_set, words * sizeof *site_set);
          } else {
            /* The call's own result is not yet assigned at the return
             * address, so it leaves what is live after the call; but a frame
             * state that names it names the value it held before, which is
             * still in its slot and must stay in the map. */
            memcpy(site_set, cur, words * sizeof *site_set);
            GRJIT_VReg def = grjit_op_def(op);
            if (def != GRJIT_NO_VREG && def < f->vreg_count &&
                index[def] != UINT32_MAX) {
              clear_bit(site_set, index[def]);
            }
            for (size_t w = 0; w < words; w++) {
              site_set[w] |= state_set[w];
            }
          }
          if (pass == 0) {
            for (size_t v = 0; v < f->vreg_count; v++) {
              if (index[v] != UINT32_MAX && test_bit(site_set, index[v])) {
                pool_size++;
              }
            }
          } else {
            sites[slot].block = (GRJIT_BlockId)bi;
            sites[slot].op_index = i;
            sites[slot].vregs = pool + fill;
            size_t n = 0;
            for (size_t v = 0; v < f->vreg_count; v++) {
              if (index[v] != UINT32_MAX && test_bit(site_set, index[v])) {
                pool[fill++] = (GRJIT_VReg)v;
                n++;
              }
            }
            sites[slot].count = n;
          }
        }
        transfer(&c, op, cur);
      }
      at += block_sites;
    }
    if (pass == 0) {
      pool = a->malloc_fn(a->ctx, (pool_size + 1) * sizeof *pool);
      if (pool == NULL) {
        a->free_fn(a->ctx, index);
        a->free_fn(a->ctx, live_in);
        a->free_fn(a->ctx, sites);
        return GRJIT_ERR_OOM;
      }
    }
  }
  a->free_fn(a->ctx, index);
  a->free_fn(a->ctx, live_in);
  out->sites = sites;
  out->count = site_total;
  out->pool = pool;
  return GRJIT_OK;
}

void grjit_liveness_free(GRJIT_LiveSites * sites) {
  if (sites == NULL || sites->allocator == NULL) {
    return;
  }
  sites->allocator->free_fn(sites->allocator->ctx, sites->sites);
  sites->allocator->free_fn(sites->allocator->ctx, sites->pool);
  memset(sites, 0, sizeof *sites);
}
