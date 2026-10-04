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
 * A guard that fails, and the slots it reconstructs.
 *
 *   f(x, y):  a = x * 3; b = y + 1;
 *             guard (x != 0)   state [a, b, constant 77, dead]
 *             return a + b
 *
 * A guard that holds falls through. One that fails leaves the function
 * through a deopt exit: it returns `GRJIT_EXIT_DEOPT`, writes the frame
 * state's slots into `out` (a register's value, a constant, zero for a dead
 * slot) and, after them, the code offset of the guard's site. That offset is a
 * key into the metadata the compile emitted in runtime-core's format, and
 * `grcore_codemeta_find` turns it into the site's record: the poll identity
 * a rebuild would return to, and where each slot was.
 *
 * Rebuilding an interpreter frame from those slots is the engine's work, not
 * this library's. Build and run with `make examples`.
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

int main(void) {
  GRJIT_Builder * b;
  CHECK(grjit_builder_create("f", 4, NULL, NULL, &b));
  GRJIT_VReg x, y, a, bb, c, r;
  CHECK(grjit_builder_param(b, GRJIT_TYPE_I64, &x));
  CHECK(grjit_builder_param(b, GRJIT_TYPE_I64, &y));
  CHECK(grjit_builder_vreg(b, GRJIT_TYPE_I64, &a));
  CHECK(grjit_builder_vreg(b, GRJIT_TYPE_I64, &bb));
  CHECK(grjit_builder_vreg(b, GRJIT_TYPE_I64, &c));
  CHECK(grjit_builder_vreg(b, GRJIT_TYPE_I64, &r));
  GRJIT_BlockId entry;
  CHECK(grjit_builder_block(b, &entry));
  CHECK(grjit_builder_set_block(b, entry));
  CHECK(grjit_builder_binary(b, GRJIT_OP_MUL, a, grjit_operand_vreg(x), grjit_operand_imm(3)));
  CHECK(grjit_builder_binary(b, GRJIT_OP_ADD, bb, grjit_operand_vreg(y), grjit_operand_imm(1)));
  CHECK(grjit_builder_cmp(b, GRJIT_CMP_NE, c, grjit_operand_vreg(x), grjit_operand_imm(0)));
  GRJIT_FrameSlot state[4] = {grjit_frame_slot_vreg(a), grjit_frame_slot_vreg(bb),
      grjit_frame_slot_constant(77), grjit_frame_slot_dead()};
  GRCORE_PollIdentity identity = {5, 99};
  CHECK(grjit_builder_guard(b, grjit_operand_vreg(c), identity, state, 4));
  CHECK(grjit_builder_binary(b, GRJIT_OP_ADD, r, grjit_operand_vreg(a), grjit_operand_vreg(bb)));
  CHECK(grjit_builder_ret(b, grjit_operand_vreg(r)));
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

  uint64_t out[8];
  uint64_t holds[2] = {5, 10};
  uint32_t exit_kind = grjit_code_call(code, context, holds, out);
  printf("f(5, 10): exit %u (returned), result %llu\n", exit_kind, (unsigned long long)out[0]);
  int ok = exit_kind == GRJIT_EXIT_RETURNED && out[0] == 26;

  uint64_t fails[2] = {0, 10};
  exit_kind = grjit_code_call(code, context, fails, out);
  printf("f(0, 10): exit %u (deopt)\n", exit_kind);
  printf("  slots: a=%llu b=%llu constant=%llu dead=%llu\n", (unsigned long long)out[0],
      (unsigned long long)out[1], (unsigned long long)out[2], (unsigned long long)out[3]);
  const GRCORE_CodeSite * site =
      grcore_codemeta_find(grjit_code_meta(code), (uint32_t)out[4]);
  if (site == NULL) {
    fprintf(stderr, "no site at offset %llu\n", (unsigned long long)out[4]);
    return 1;
  }
  printf("  site at code offset %u: a guard returning to (function %llu, offset %llu)\n",
      site->code_offset, (unsigned long long)site->identity.function,
      (unsigned long long)site->identity.offset);
  ok = ok && exit_kind == GRJIT_EXIT_DEOPT && out[0] == 0 && out[1] == 11 && out[2] == 77 &&
       out[3] == 0 && site->kind == GRCORE_SITE_GUARD && site->identity.offset == 99;

  grjit_code_destroy(code);
  grjit_function_destroy(f);
  grcore_context_destroy(context);
  grcore_group_destroy(group);
  return ok ? 0 : 1;
}
