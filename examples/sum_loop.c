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
 * Build the IR for a summation loop, compile it, run it, print the result.
 *
 *   sum(n):  sum = 0; i = 0; while (i < n) { sum += i; i += 1; } return sum
 *
 * The steps are the whole life of a function in this library: a builder
 * records blocks, registers and operations; the verifier judges them; the
 * printer shows the IR; `grjit_compile` emits x86-64 into pages taken from
 * the context's counting page provider and flips them to read-execute; and
 * `grjit_code_call` runs them. The context is only there to supply those
 * pages and the request word a poll would load: this function has no poll.
 *
 * Build and run with `make examples`.
 */

#include <ghoti.io/runtime-jit/runtime-jit.h>

#include <ghoti.io/runtime-core/runtime-core.h>

#include <stdio.h>
#include <stdlib.h>

#define CHECK(call)                                                            \
  do {                                                                         \
    GRJIT_Result r_ = (call);                                                  \
    if (r_ != GRJIT_OK) {                                                      \
      fprintf(stderr, "%s failed: %s\n", #call, grjit_result_string(r_));      \
      return 1;                                                                \
    }                                                                          \
  } while (0)

int main(void) {
  if (!grjit_backend_available()) {
    /* Exit status 77 is "skipped": the Makefile counts it and does not fail. */
    printf("SKIP: no native code backend on this target\n");
    return 77;
  }

  GRJIT_Builder * b;
  CHECK(grjit_builder_create("sum", 0, NULL, NULL, &b));
  GRJIT_VReg n, i, sum, t;
  GRJIT_BlockId entry, head, body, done;
  CHECK(grjit_builder_param(b, GRJIT_TYPE_I64, &n));
  CHECK(grjit_builder_vreg(b, GRJIT_TYPE_I64, &i));
  CHECK(grjit_builder_vreg(b, GRJIT_TYPE_I64, &sum));
  CHECK(grjit_builder_vreg(b, GRJIT_TYPE_I64, &t));
  CHECK(grjit_builder_block(b, &entry));
  CHECK(grjit_builder_block(b, &head));
  CHECK(grjit_builder_block(b, &body));
  CHECK(grjit_builder_block(b, &done));
  CHECK(grjit_builder_set_block(b, entry));
  CHECK(grjit_builder_const(b, i, 0));
  CHECK(grjit_builder_const(b, sum, 0));
  CHECK(grjit_builder_br(b, head));
  CHECK(grjit_builder_set_block(b, head));
  CHECK(grjit_builder_cmp(b, GRJIT_CMP_LT, t, grjit_operand_vreg(i), grjit_operand_vreg(n)));
  CHECK(grjit_builder_br_if(b, grjit_operand_vreg(t), body, done));
  CHECK(grjit_builder_set_block(b, body));
  CHECK(grjit_builder_binary(b, GRJIT_OP_ADD, sum, grjit_operand_vreg(sum), grjit_operand_vreg(i)));
  CHECK(grjit_builder_binary(b, GRJIT_OP_ADD, i, grjit_operand_vreg(i), grjit_operand_imm(1)));
  CHECK(grjit_builder_br(b, head));
  CHECK(grjit_builder_set_block(b, done));
  CHECK(grjit_builder_ret(b, grjit_operand_vreg(sum)));
  GRJIT_Function * f;
  CHECK(grjit_builder_finish(b, &f));

  char reason[256];
  GRJIT_Result verdict = grjit_function_verify(f, NULL, reason, sizeof reason);
  if (verdict != GRJIT_OK) {
    fprintf(stderr, "verify: %s\n", reason);
    return 1;
  }
  size_t length;
  CHECK(grjit_function_print(f, NULL, 0, &length));
  char * text = malloc(length + 1);
  CHECK(grjit_function_print(f, text, length + 1, &length));
  printf("%s\n", text);
  free(text);

  GRCORE_Group * group;
  GRCORE_Context * context;
  if (grcore_group_create(NULL, NULL, &group) != GRCORE_OK ||
      grcore_context_create(group, NULL, &context) != GRCORE_OK) {
    fprintf(stderr, "no context\n");
    return 1;
  }
  GRJIT_CompileOptions options = {0};
  options.pages = grcore_context_page_provider(context);
  GRJIT_Code * code;
  CHECK(grjit_compile(&options, f, &code));
  printf("compiled to %zu bytes, mapped as %zu, %zu bytes in use by the context\n",
      grjit_code_size(code), grjit_code_mapped_size(code),
      (size_t)grcore_context_memory_in_use(context));

  uint64_t args[1] = {100};
  uint64_t out[1];
  uint32_t exit_kind = grjit_code_call(code, context, args, out);
  printf("sum(100) exit %u, result %llu\n", exit_kind, (unsigned long long)out[0]);
  int ok = exit_kind == GRJIT_EXIT_RETURNED && out[0] == 4950;

  grjit_code_destroy(code);
  grjit_function_destroy(f);
  grcore_context_destroy(context);
  grcore_group_destroy(group);
  return ok ? 0 : 1;
}
