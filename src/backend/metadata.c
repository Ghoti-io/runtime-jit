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
 * The metadata builder: turns the sites the emitter recorded and the liveness
 * the verifier's pass computed into runtime-core's table (a/codemeta.h).
 *
 * A live REF register is a stack-map entry at its frame slot with slot kind
 * VALUE; a live derived pointer is a (slot, base slot, delta) triple; the frame
 * state is one location per interpreter slot. Nothing here is specific to a
 * register or an instruction set: the offsets are from the frame base, which is
 * the frame pointer of whichever backend emitted the code.
 */

#include <ghoti.io/runtime-jit/macros.h>

#include "backend_internal.h"

#include "../ir/ir_internal.h"

#include <stdint.h>
#include <string.h>

static GRCORE_CodeLocation slot_location(GRJIT_VReg v, GRJIT_Type type) {
  GRCORE_CodeLocation l;
  l.kind = GRCORE_LOC_FRAME_SLOT;
  l.slot_kind = type == GRJIT_TYPE_REF ? GRCORE_SLOT_VALUE : GRCORE_SLOT_RAW;
  l.value = grjit_emit_slot(v);
  l.representation = GRCORE_REPR_BITS;
  return l;
}

/* The arguments a push site's hook reads from the arguments area of the frame
 * are references the collector must update before it does (AD-28): each REF
 * argument is a VALUE entry at its area slot. */
static bool rec_is_push(const GRJIT_SiteRec * rec) {
  return rec->op != NULL && rec->kind == GRCORE_SITE_GC_POINT_FRAME_PUSH;
}

#if defined(GRJIT_TEST_PLANT_BUG) && GRJIT_TEST_PLANT_BUG == 12
/* Planted defect 12 (tests only): the site of a tail call's hook leaves its
 * arguments area out of the stack map, so a collection in the hook leaves the
 * references it copied stale for the hook to read. */
static bool rec_omits_area(const GRJIT_SiteRec * rec) {
  return rec->op->kind == GRJIT_OP_TAIL_CALL_SLOT || rec->op->kind == GRJIT_OP_TAIL_CALL_PTR;
}
#else
static bool rec_omits_area(const GRJIT_SiteRec * rec) {
  (void)rec;
  return false;
}
#endif

static size_t push_ref_args(const GRJIT_Function * f, const GRJIT_SiteRec * rec) {
  size_t n = 0;
  if (rec->op == NULL || rec->kind != GRCORE_SITE_GC_POINT_FRAME_PUSH) {
    return 0;
  }
  if (rec_omits_area(rec)) {
    return 0;
  }
  for (size_t k = 0; k < rec->op->arg_count; k++) {
    const GRJIT_Operand * o = &rec->op->args[k];
    if (o->kind == GRJIT_OPERAND_VREG && f->vregs[o->vreg].type == GRJIT_TYPE_REF) {
      n++;
    }
  }
  return n;
}

/* A derived pointer passed as an argument is copied into the arguments area, and
 * the push hook's collection may move its base: the copy is a derived pointer of
 * its own, rewritten from the same base, or the callee is handed the old address
 * (AD-12). Its base is live at the push, as the verifier requires of any live
 * derived pointer. */
static size_t push_derived_args(const GRJIT_Function * f, const GRJIT_SiteRec * rec) {
  size_t n = 0;
  if (!rec_is_push(rec) || rec_omits_area(rec)) {
    return 0;
  }
  for (size_t k = 0; k < rec->op->arg_count; k++) {
    const GRJIT_Operand * o = &rec->op->args[k];
    if (o->kind == GRJIT_OPERAND_VREG && f->vregs[o->vreg].derived) {
      n++;
    }
  }
  return n;
}

GRJIT_Result grjit_metadata_build(const GRJIT_Function * f,
    const GRJIT_CallableShape * callable_shape, const GRJIT_LiveSites * live,
    const GRJIT_SiteRec * recs, size_t count, uint32_t frame_bytes,
    uint32_t code_bytes, const GRJIT_Allocator * a, GRJIT_MetaStorage * out) {
  memset(out, 0, sizeof *out);
  out->allocator = a;
  const GRJIT_CallableShape shape = *callable_shape;
  size_t loc_total = 0;
  size_t der_total = 0;
  for (size_t i = 0; i < count; i++) {
    const GRJIT_SiteLive * sl = &live->sites[recs[i].live_index];
    for (size_t k = 0; k < sl->count; k++) {
      if (f->vregs[sl->vregs[k]].type == GRJIT_TYPE_REF) {
        loc_total++;
      } else {
        der_total++;
      }
    }
    loc_total += f->states[recs[i].state].slot_count;
    loc_total += push_ref_args(f, &recs[i]);
    der_total += push_derived_args(f, &recs[i]);
  }
  if (count > SIZE_MAX / sizeof *out->sites ||
      loc_total > SIZE_MAX / sizeof *out->locations ||
      der_total > SIZE_MAX / sizeof *out->derived) {
    return GRJIT_ERR_LIMIT;
  }
  if (count != 0) {
    out->sites = a->calloc_fn(a->ctx, count, sizeof *out->sites);
    if (out->sites == NULL) {
      return GRJIT_ERR_OOM;
    }
  }
  if (loc_total != 0) {
    out->locations = a->calloc_fn(a->ctx, loc_total, sizeof *out->locations);
    if (out->locations == NULL) {
      grjit_metadata_free(out);
      return GRJIT_ERR_OOM;
    }
  }
  if (der_total != 0) {
    out->derived = a->calloc_fn(a->ctx, der_total, sizeof *out->derived);
    if (out->derived == NULL) {
      grjit_metadata_free(out);
      return GRJIT_ERR_OOM;
    }
  }
  size_t loc_at = 0;
  size_t der_at = 0;
  for (size_t i = 0; i < count; i++) {
    const GRJIT_SiteRec * r = &recs[i];
    const GRJIT_SiteLive * sl = &live->sites[r->live_index];
    GRCORE_CodeSite * s = &out->sites[i];
    s->code_offset = r->offset;
    s->kind = r->kind;
    s->identity = r->identity;
    /* NULL + 0 is undefined in C: the arrays are not allocated when empty. */
    s->live = out->locations != NULL ? out->locations + loc_at : NULL;
    size_t refs = 0;
    s->derived = out->derived != NULL ? out->derived + der_at : NULL;
    size_t ders = 0;
#if defined(GRJIT_TEST_PLANT_BUG) && GRJIT_TEST_PLANT_BUG == 3
    /* Planted defect 3 (tests only): the first live REF that no derived
     * pointer is based on is left out of the stack map. */
    bool dropped = false;
#endif
#if defined(GRJIT_TEST_PLANT_BUG) && GRJIT_TEST_PLANT_BUG == 8
    /* Planted defect 8 (tests only): the first live REF of the site a call to
     * another compiled function returns to is left out of its stack map. */
    bool dropped_at_call = false;
#endif
    for (size_t k = 0; k < sl->count; k++) {
      GRJIT_VReg v = sl->vregs[k];
      const GRJIT_VRegInfo * info = &f->vregs[v];
      if (info->type == GRJIT_TYPE_REF) {
#if defined(GRJIT_TEST_PLANT_BUG) && GRJIT_TEST_PLANT_BUG == 8
        if (!dropped_at_call && r->op != NULL && r->kind == GRCORE_SITE_GC_POINT_CALL &&
            (r->op->kind == GRJIT_OP_CALL_SLOT || r->op->kind == GRJIT_OP_CALL_PTR)) {
          dropped_at_call = true;
          continue;
        }
#endif
#if defined(GRJIT_TEST_PLANT_BUG) && GRJIT_TEST_PLANT_BUG == 3
        bool is_base = false;
        for (size_t j = 0; j < sl->count; j++) {
          if (f->vregs[sl->vregs[j]].derived &&
              f->vregs[sl->vregs[j]].base == v) {
            is_base = true;
          }
        }
        if (!dropped && !is_base) {
          dropped = true;
          continue;
        }
#endif
        GRCORE_CodeLocation l = slot_location(v, info->type);
#if defined(GRJIT_TEST_PLANT_BUG) && GRJIT_TEST_PLANT_BUG == 2
        /* Planted defect 2 (tests only): the first stack-map slot of a site
         * is recorded 8 bytes too high. */
        if (refs == 0) {
          l.value += 8;
        }
#endif
        out->locations[loc_at + refs++] = l;
      } else {
        GRCORE_DerivedPointer * d = &out->derived[der_at + ders++];
        d->slot = grjit_emit_slot(v);
        d->base_slot = grjit_emit_slot(info->base);
        d->delta = info->delta;
      }
    }
    if (rec_is_push(r) && !rec_omits_area(r)) {
      for (size_t k = 0; k < r->op->arg_count; k++) {
        const GRJIT_Operand * o = &r->op->args[k];
        if (o->kind == GRJIT_OPERAND_VREG && f->vregs[o->vreg].type == GRJIT_TYPE_REF) {
          GRCORE_CodeLocation l;
          l.kind = GRCORE_LOC_FRAME_SLOT;
          l.slot_kind = GRCORE_SLOT_VALUE;
          l.value = GRJIT_ARGS_SLOT(GRJIT_SHAPE_REGS(f, shape), shape.args_area, k);
          l.representation = GRCORE_REPR_BITS;
          out->locations[loc_at + refs++] = l;
        }
      }
    }
    if (rec_is_push(r) && !rec_omits_area(r)) {
      for (size_t k = 0; k < r->op->arg_count; k++) {
        const GRJIT_Operand * o = &r->op->args[k];
        if (o->kind == GRJIT_OPERAND_VREG && f->vregs[o->vreg].derived) {
          GRCORE_DerivedPointer * d = &out->derived[der_at + ders++];
          d->slot = GRJIT_ARGS_SLOT(GRJIT_SHAPE_REGS(f, shape), shape.args_area, k);
          d->base_slot = grjit_emit_slot(f->vregs[o->vreg].base);
          d->delta = f->vregs[o->vreg].delta;
        }
      }
    }
    s->live_count = refs;
    s->derived_count = ders;
    loc_at += refs;
    der_at += ders;
    const GRJIT_FrameState * st = &f->states[r->state];
    s->frame_state = out->locations != NULL ? out->locations + loc_at : NULL;
    s->frame_state_count = st->slot_count;
    for (size_t k = 0; k < st->slot_count; k++) {
      const GRJIT_FrameSlot * slot = &st->slots[k];
      GRCORE_CodeLocation l;
      if (slot->kind == GRJIT_FRAME_SLOT_VREG) {
        l = slot_location(slot->vreg, f->vregs[slot->vreg].type);
      } else if (slot->kind == GRJIT_FRAME_SLOT_CONSTANT) {
        l.kind = GRCORE_LOC_CONSTANT;
        l.slot_kind = GRCORE_SLOT_RAW;
        l.value = slot->constant;
        l.representation = GRCORE_REPR_BITS;
      } else {
        l.kind = GRCORE_LOC_DEAD;
        l.slot_kind = GRCORE_SLOT_RAW;
        l.value = 0;
        l.representation = GRCORE_REPR_BITS;
      }
      out->locations[loc_at + k] = l;
    }
    loc_at += st->slot_count;
  }
  out->meta.version = GRCORE_CODEMETA_FORMAT_VERSION;
  out->meta.frame_bytes = frame_bytes;
  out->meta.code_bytes = code_bytes;
  out->meta.site_count = count;
  out->meta.sites = out->sites;
  return GRJIT_OK;
}

void grjit_metadata_free(GRJIT_MetaStorage * m) {
  if (m->allocator == NULL) {
    return;
  }
  m->allocator->free_fn(m->allocator->ctx, m->sites);
  m->allocator->free_fn(m->allocator->ctx, m->locations);
  m->allocator->free_fn(m->allocator->ctx, m->derived);
  memset(m, 0, sizeof *m);
}
