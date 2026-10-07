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

#include <stdint.h>
#include <string.h>

typedef struct Ctx {
  const GRJIT_Function * f;
  const uint32_t * index; /* vreg -> bit, or UINT32_MAX when not tracked */
  uint64_t * set;         /* the set being built */
} Ctx;

static void set_bit(uint64_t * s, uint32_t bit) {
  s[bit / 64] |= UINT64_C(1) << (bit % 64);
}

static bool test_bit(const uint64_t * s, uint32_t bit) {
  return (s[bit / 64] >> (bit % 64)) & 1u;
}

static void clear_bit(uint64_t * s, uint32_t bit) {
  s[bit / 64] &= ~(UINT64_C(1) << (bit % 64));
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

/* How many sites an operation makes. A call to another compiled function is
 * three: the push of the callee's frame, the call itself, and the exit before
 * it (backend/backend_internal.h). A tail call is two: the hook that replaces
 * the guest frame, and the exit before it. A native call is the call itself, the
 * exit before it (the native-stack check failed) and, for a native with a status,
 * the exit a non-zero status takes, in that order. Every other operation with a
 * frame state is one. */
size_t grjit_liveness_site_count(const GRJIT_Op * op) {
  if (op->kind == GRJIT_OP_CALL_NATIVE) {
    return op->exit_state != GRJIT_NO_STATE ? 3u : 2u;
  }
  if (op->kind == GRJIT_OP_CALL_SLOT || op->kind == GRJIT_OP_CALL_PTR) {
    return 3u;
  }
  if (op->kind == GRJIT_OP_TAIL_CALL_SLOT || op->kind == GRJIT_OP_TAIL_CALL_PTR) {
    return 2u;
  }
  return 1u;
}

GRJIT_Result grjit_liveness_compute(const GRJIT_Function * f,
    const GRJIT_Allocator * a, size_t max_entries, GRJIT_LiveSites * out) {
  memset(out, 0, sizeof *out);
  out->allocator = a;
  size_t site_total = 0;
  for (size_t b = 0; b < f->block_count; b++) {
    for (size_t i = 0; i < f->blocks[b].count; i++) {
      if (grjit_op_has_state(&f->blocks[b].ops[i])) {
        site_total += grjit_liveness_site_count(&f->blocks[b].ops[i]);
      }
    }
  }
  if (site_total == 0) {
    return GRJIT_OK;
  }
  /* `index` maps a register to its bit, and the second half of the block maps
   * a bit back to its register, so that a set is read by its set bits and not
   * by testing every register. */
  uint32_t * index =
      a->malloc_fn(a->ctx, (4 * (f->vreg_count + 1)) * sizeof *index);
  if (index == NULL) {
    return GRJIT_ERR_OOM;
  }
  uint32_t * bit_vreg = index + f->vreg_count + 1;
  /* The derived pointers based on each register, as a list threaded through
   * `next_derived`: `first_derived[v]` is the first, UINT32_MAX for none. */
  uint32_t * first_derived = bit_vreg + f->vreg_count + 1;
  uint32_t * next_derived = first_derived + f->vreg_count + 1;
  uint32_t tracked = 0;
  for (size_t v = 0; v < f->vreg_count; v++) {
    first_derived[v] = UINT32_MAX;
    next_derived[v] = UINT32_MAX;
  }
  for (size_t v = 0; v < f->vreg_count; v++) {
    bool t = f->vregs[v].type == GRJIT_TYPE_REF ||
             (f->vregs[v].type == GRJIT_TYPE_PTR && f->vregs[v].derived);
    index[v] = t ? tracked : UINT32_MAX;
    if (t) {
      bit_vreg[tracked++] = (uint32_t)v;
    }
    if (f->vregs[v].type == GRJIT_TYPE_PTR && f->vregs[v].derived &&
        f->vregs[v].base < f->vreg_count) {
      next_derived[v] = first_derived[f->vregs[v].base];
      first_derived[f->vregs[v].base] = (uint32_t)v;
    }
  }
  size_t words = ((size_t)tracked + 63) / 64;
  if (words == 0) {
    words = 1;
  }
  /* live_in per block, then the scratch sets: the running set, the site's, the
   * frame state's and the one before the operation. */
  uint64_t * live_in = a->calloc_fn(a->ctx, f->block_count * words + 4 * words,
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
  uint64_t * before_set = scratch + 3 * words;
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
          block_sites += grjit_liveness_site_count(&b->ops[i]);
        }
      }
      size_t slot = at + block_sites;
      for (size_t i = b->count; i-- > 0;) {
        const GRJIT_Op * op = &b->ops[i];
        if (pass == 0 && !out->base_redefined) {
          /* `cur` is what is live after this operation. */
          GRJIT_VReg def = grjit_op_def(op);
          if (def != GRJIT_NO_VREG && def < f->vreg_count) {
            for (uint32_t d = first_derived[def]; d != UINT32_MAX;
                d = next_derived[d]) {
              if (index[d] != UINT32_MAX && test_bit(cur, index[d])) {
                out->base_redefined = true;
                out->redefined_block = (GRJIT_BlockId)bi;
                out->redefined_op = i;
                out->redefined_base = def;
                out->redefined_derived = d;
                break;
              }
            }
          }
        }
        if (grjit_op_has_state(op)) {
          size_t mult = grjit_liveness_site_count(op);
          slot -= mult;
          /* What is live before the operation, for a push: its own uses (the
           * arguments, the code pointer, both frame states) and what is live
           * after it, less what it assigns. */
          if (mult >= 2) {
            memcpy(before_set, cur, words * sizeof *before_set);
            transfer(&c, op, before_set);
          }
          for (size_t k = 0; k < mult; k++) {
            if (op->kind == GRJIT_OP_CALL_NATIVE) {
              /* 0 is the call, 1 the exit before it (the state), 2 the exit a
               * status takes (the state after). An exit leaves the frame: only what
               * its own state names. The call's set is what is live after it, less
               * its result, plus what either state names: the state after the call
               * is read from the frame by the exit, so a collection the native
               * triggers must have updated every register it names but the result,
               * which does not exist until the call returns. */
              memset(state_set, 0, words * sizeof *state_set);
              c.set = state_set;
              const uint32_t which_state = k == 2 ? op->exit_state : op->state;
              const GRJIT_VReg result = grjit_op_def(op);
              for (int pass_state = 0; pass_state < (k == 0 ? 2 : 1); pass_state++) {
                uint32_t w = pass_state == 0 ? which_state : op->exit_state;
                if (w == GRJIT_NO_STATE || w >= f->state_count) {
                  continue;
                }
                const GRJIT_FrameState * st = &f->states[w];
                for (size_t q = 0; q < st->slot_count; q++) {
                  if (st->slots[q].kind == GRJIT_FRAME_SLOT_VREG &&
                      !(pass_state == 1 && st->slots[q].vreg == result)) {
                    add_use(&c, st->slots[q].vreg);
                  }
                }
              }
              if (k == 0) {
                memcpy(site_set, cur, words * sizeof *site_set);
                if (result != GRJIT_NO_VREG && result < f->vreg_count &&
                    index[result] != UINT32_MAX) {
                  clear_bit(site_set, index[result]);
                }
                for (size_t w = 0; w < words; w++) {
                  site_set[w] |= state_set[w];
                }
              } else {
                memcpy(site_set, state_set, words * sizeof *site_set);
              }
            } else {
            memset(state_set, 0, words * sizeof *state_set);
            c.set = state_set;
            /* Variant 0 is the only one of an ordinary site; for a guest call
             * 0 is the push, 1 the call and 2 the exit; for a tail call 0 is the
             * hook and 1 the exit. The frame state each names: a call's exit's
             * is `exit_state`, every other site's `state` (a tail call's exit
             * leaves the frame in the state the hook found it in). */
            const bool exit_site = mult == 3 ? k == 2 : (mult == 2 && k == 1);
            uint32_t which = (mult == 3 && k == 2) ? op->exit_state : op->state;
            if (which != GRJIT_NO_STATE && which < f->state_count) {
              const GRJIT_FrameState * st = &f->states[which];
              for (size_t q = 0; q < st->slot_count; q++) {
                if (st->slots[q].kind == GRJIT_FRAME_SLOT_VREG) {
                  add_use(&c, st->slots[q].vreg);
                }
              }
            }
            if (op->kind == GRJIT_OP_GUARD || exit_site) {
              /* An exit leaves the frame: only what its state names. */
              memcpy(site_set, state_set, words * sizeof *site_set);
            } else if (mult >= 2 && k == 0) {
              memcpy(site_set, before_set, words * sizeof *site_set);
              for (size_t w = 0; w < words; w++) {
                site_set[w] |= state_set[w];
              }
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
            }
            if (pass == 0) {
              for (size_t w = 0; w < words; w++) {
                pool_size += (size_t)__builtin_popcountll(site_set[w]);
              }
              if (pool_size > max_entries) {
                a->free_fn(a->ctx, index);
                a->free_fn(a->ctx, live_in);
                a->free_fn(a->ctx, sites);
                return GRJIT_ERR_LIMIT;
              }
            } else {
              sites[slot + k].block = (GRJIT_BlockId)bi;
              sites[slot + k].op_index = i;
              sites[slot + k].vregs = pool + fill;
              size_t n = 0;
              for (size_t w = 0; w < words; w++) {
                for (uint64_t bits = site_set[w]; bits != 0; bits &= bits - 1) {
                  pool[fill++] =
                      (GRJIT_VReg)bit_vreg[w * 64 + (size_t)__builtin_ctzll(bits)];
                  n++;
                }
              }
              sites[slot + k].count = n;
            }
          }
        }
        transfer(&c, op, cur);
      }
      at += block_sites;
    }
    if (pass == 0) {
      if (pool_size > SIZE_MAX / sizeof *pool - 1) {
        a->free_fn(a->ctx, index);
        a->free_fn(a->ctx, live_in);
        a->free_fn(a->ctx, sites);
        return GRJIT_ERR_LIMIT;
      }
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
