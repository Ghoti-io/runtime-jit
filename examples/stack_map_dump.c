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
 * Print the sites of a compiled function: where a collector would look.
 *
 * The function holds two references and a derived pointer into one of them,
 * polls, calls a helper that can reach a GC point, and has a guard. Each is a
 * *site* in the metadata the compile emitted in runtime-core's format: its code
 * offset (the return address of a call or of a poll's slow call, the start of
 * a guard's exit stub), its kind, the live references as frame-slot offsets
 * from the frame base (`rbp`), derived pointers as (slot, base slot, delta)
 * triples, and the deoptimization frame state.
 *
 * Build and run with `make examples`.
 */

#include <ghoti.io/runtime-jit/runtime-jit.h>

#include <ghoti.io/runtime-core/runtime-core.h>

#include <stdio.h>

#define CHECK(call)                                                            \
  do {                                                                         \
    GRJIT_Result r_ = (call);                                                  \
    if (r_ != GRJIT_OK) {                                                      \
      fprintf(stderr, "%s failed: %s\n", #call, grjit_result_string(r_));      \
      return 1;                                                                \
    }                                                                          \
  } while (0)

static uint64_t helper(uint64_t a) {
  return a;
}

static uint32_t poll_helper(void * context, uint64_t function, uint64_t offset) {
  (void)context;
  (void)function;
  (void)offset;
  return 0;
}

static const char * kind_name(GRCORE_CodeSiteKind k) {
  static const char * names[] = {"poll", "alloc-slow", "call", "frame-push", "nested-entry", "guard"};
  return (unsigned)k < GRCORE_SITE_KIND_COUNT ? names[k] : "?";
}

int main(void) {
  GRJIT_Builder * b;
  CHECK(grjit_builder_create("dump", 2, NULL, NULL, &b));
  GRJIT_VReg r0, r1, q, d, ok;
  CHECK(grjit_builder_vreg(b, GRJIT_TYPE_REF, &r0));
  CHECK(grjit_builder_vreg(b, GRJIT_TYPE_REF, &r1));
  CHECK(grjit_builder_vreg(b, GRJIT_TYPE_PTR, &q));
  CHECK(grjit_builder_vreg(b, GRJIT_TYPE_I64, &d));
  CHECK(grjit_builder_vreg(b, GRJIT_TYPE_I64, &ok));
  CHECK(grjit_builder_derived(b, q, r0, 16));
  CHECK(grjit_builder_set_poll_helper(b, poll_helper));
  GRJIT_BlockId entry;
  CHECK(grjit_builder_block(b, &entry));
  CHECK(grjit_builder_set_block(b, entry));
  CHECK(grjit_builder_const(b, r0, 0x1001));
  CHECK(grjit_builder_const(b, r1, 0x2001));
  CHECK(grjit_builder_const(b, q, 0x1011));
  GRJIT_FrameSlot state[2] = {grjit_frame_slot_vreg(r0), grjit_frame_slot_vreg(r1)};
  GRCORE_PollIdentity poll_at = {1, 10}, call_at = {1, 20}, guard_at = {1, 30};
  CHECK(grjit_builder_poll(b, poll_at, state, 2));
  GRJIT_Operand arg = grjit_operand_vreg(r0);
  CHECK(grjit_builder_call(b, d, (uint64_t)(uintptr_t)helper, GRJIT_CALL_GC_POINT,
      GRCORE_SITE_GC_POINT_CALL, &arg, 1, call_at, state, 2));
  CHECK(grjit_builder_cmp(b, GRJIT_CMP_EQ, ok, grjit_operand_vreg(r0), grjit_operand_vreg(r1)));
  CHECK(grjit_builder_guard(b, grjit_operand_vreg(d), guard_at, state, 2));
  CHECK(grjit_builder_binary(b, GRJIT_OP_ADD, d, grjit_operand_vreg(q), grjit_operand_vreg(ok)));
  CHECK(grjit_builder_ret(b, grjit_operand_vreg(d)));
  GRJIT_Function * f;
  CHECK(grjit_builder_finish(b, &f));

  GRCORE_Group * group;
  GRCORE_Context * context;
  if (grcore_group_create(NULL, NULL, &group) != GRCORE_OK ||
      grcore_context_create(group, NULL, &context) != GRCORE_OK) {
    return 1;
  }
  GRJIT_CompileOptions options = {0};
  options.pages = grcore_context_page_provider(context);
  GRJIT_Code * code;
  CHECK(grjit_compile(&options, f, &code));

  const GRCORE_CodeMeta * meta = grjit_code_meta(code);
  printf("%zu sites, %u bytes of code, frame of %u bytes (slots at negative offsets from rbp)\n",
      meta->site_count, meta->code_bytes, meta->frame_bytes);
  for (size_t i = 0; i < meta->site_count; i++) {
    const GRCORE_CodeSite * s = &meta->sites[i];
    printf("site at +%u: %s, identity (%llu, %llu)\n", s->code_offset, kind_name(s->kind),
        (unsigned long long)s->identity.function, (unsigned long long)s->identity.offset);
    printf("  live references:");
    for (size_t k = 0; k < s->live_count; k++) {
      printf(" [rbp%+lld]", (long long)s->live[k].value);
    }
    printf("\n");
    for (size_t k = 0; k < s->derived_count; k++) {
      printf("  derived: [rbp%+lld] = [rbp%+lld] + %lld\n", (long long)s->derived[k].slot,
          (long long)s->derived[k].base_slot, (long long)s->derived[k].delta);
    }
    printf("  deopt state:");
    for (size_t k = 0; k < s->frame_state_count; k++) {
      const GRCORE_CodeLocation * l = &s->frame_state[k];
      if (l->kind == GRCORE_LOC_FRAME_SLOT) {
        printf(" [rbp%+lld]", (long long)l->value);
      } else if (l->kind == GRCORE_LOC_CONSTANT) {
        printf(" #%lld", (long long)l->value);
      } else {
        printf(" dead");
      }
    }
    printf("\n");
  }
  int ok_result = meta->site_count == 3 &&
      grcore_codemeta_validate(meta, grjit_code_size(code), NULL) == GRCORE_OK;

  grjit_code_destroy(code);
  grjit_function_destroy(f);
  grcore_context_destroy(context);
  grcore_group_destroy(group);
  return ok_result ? 0 : 1;
}
