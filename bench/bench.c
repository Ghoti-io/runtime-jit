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
 * The benchmark harness (AD-26).
 *
 * Every library ships one from its first commit, so that "performant" is a
 * claim with a way to check it. Besides the calibration case it holds the cases
 * of the baseline JIT:
 *
 *  - compiling a function of 100 operations (verify, liveness, emit, metadata,
 *    map, flip to read-execute, and the destroy that unmaps it): ns per
 *    compile;
 *  - a counting loop with no poll, the same loop with an inline poll whose fast
 *    path is taken (nothing pending), and the same loop with a call to a no-op
 *    `NO_GC` helper on every iteration: ns per loop iteration. The difference
 *    between the first two is what a poll costs per iteration when nothing is
 *    pending, which is the figure AD-4 and AD-19 claim is one load, one test
 *    and one branch; the third is what the call costs;
 *  - the same loop calling a compiled function through an entry slot (AD-28),
 *    with the engine's push and pop hooks doing nothing: what the internal
 *    convention, the status test and the hooks' two C calls cost, with no
 *    engine's guest stack in the figure.
 *
 * The loops are compiled by the baseline, so every value lives in a frame slot
 * and each iteration is memory-bound by the store and reload of its registers.
 * The figures are a statement about the baseline, not about native speed.
 *
 * The calibration case is a fixed amount of integer work that touches no
 * library code, run the same way every real case will be, so a figure from a
 * real case can be read against the machine it was taken on.
 *
 * Usage:
 *   bench           run every case: several repeats, report min and median
 *   bench --smoke   run every case once with a tiny workload (what `make
 *                   test` does); proves the harness builds, links and runs
 *
 * No numeric budget is asserted here. AD-26 records budgets once a first
 * measurement of a real case exists, and documentation/design.md has the first.
 */

/* clock_gettime(CLOCK_MONOTONIC) is POSIX, and -std=c17 hides it. A benchmark
 * wants a clock that cannot step backwards, so it asks for it by name. */
#define _POSIX_C_SOURCE 200809L

#include <ghoti.io/runtime-jit/runtime-jit.h>

#include <ghoti.io/runtime-core/runtime-core.h>

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define REPEATS 5

typedef struct {
  const char * name;
  /** Performs @p iterations units of work; returns a value derived from all
   *  of it so the compiler cannot discard the loop. */
  uint64_t (*run)(uint64_t iterations);
  uint64_t iterations;       /* per repeat, full run */
  uint64_t smoke_iterations; /* per repeat, --smoke */
  int needs_calls;           /* only where calls between compiled functions exist */
} Case;

/* A case that cannot set itself up must not report a short, fast run as a
 * measurement. */
static _Noreturn void setup_failed(const char * what) {
  fprintf(stderr, "bench: setup failed: %s\n", what);
  abort();
}

static void check(GRJIT_Result r, const char * what) {
  if (r != GRJIT_OK) {
    fprintf(stderr, "bench: %s: %s\n", what, grjit_result_string(r));
    setup_failed(what);
  }
}

static double now_ns(void) {
  struct timespec ts;
  if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
    return 0.0;
  }
  return (double)ts.tv_sec * 1e9 + (double)ts.tv_nsec;
}

/* xorshift64: one dependent chain of shifts and xors per step. The chain is
 * serial on purpose, so the figure is latency-bound and does not move with
 * how wide the host's execution units are. */
static uint64_t calibration_run(uint64_t iterations) {
  uint64_t x = 0x9E3779B97F4A7C15ull;
  for (uint64_t i = 0; i < iterations; i++) {
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
  }
  return x;
}

/* ---- the world the compiled code runs in --------------------------------- */

static GRCORE_Group * g_group;
static GRCORE_Context * g_context;

static void world_open(void) {
  if (g_group != NULL) {
    return;
  }
  if (grcore_group_create(NULL, NULL, &g_group) != GRCORE_OK ||
      grcore_context_create(g_group, NULL, &g_context) != GRCORE_OK) {
    setup_failed("a group and a context");
  }
}

static uint64_t g_helper_calls;
static uint64_t no_op_helper(void) {
  g_helper_calls++;
  return 0;
}
static uint32_t never_called_poll_helper(void * context, uint64_t function, uint64_t offset) {
  (void)context;
  (void)function;
  (void)offset;
  g_helper_calls += 1000000;
  return 0;
}

typedef enum { LOOP_PLAIN, LOOP_POLL, LOOP_CALL } LoopKind;

/* sum(n): for i in 0..n { [poll] [call] sum += i } */
static GRJIT_Function * build_loop(LoopKind kind) {
  GRJIT_Builder * b;
  check(grjit_builder_create(kind == LOOP_PLAIN ? "loop" : kind == LOOP_POLL ? "loop_poll" : "loop_call",
            0, NULL, NULL, &b),
      "builder");
  GRJIT_VReg n, i, sum, t;
  GRJIT_BlockId entry, head, body, done;
  check(grjit_builder_param(b, GRJIT_TYPE_I64, &n), "param");
  check(grjit_builder_vreg(b, GRJIT_TYPE_I64, &i), "vreg");
  check(grjit_builder_vreg(b, GRJIT_TYPE_I64, &sum), "vreg");
  check(grjit_builder_vreg(b, GRJIT_TYPE_I64, &t), "vreg");
  check(grjit_builder_block(b, &entry), "block");
  check(grjit_builder_block(b, &head), "block");
  check(grjit_builder_block(b, &body), "block");
  check(grjit_builder_block(b, &done), "block");
  check(grjit_builder_set_poll_helper(b, never_called_poll_helper), "helper");
  check(grjit_builder_set_block(b, entry), "at");
  check(grjit_builder_const(b, i, 0), "op");
  check(grjit_builder_const(b, sum, 0), "op");
  check(grjit_builder_br(b, head), "op");
  check(grjit_builder_set_block(b, head), "at");
  check(grjit_builder_cmp(b, GRJIT_CMP_LT, t, grjit_operand_vreg(i), grjit_operand_vreg(n)), "op");
  check(grjit_builder_br_if(b, grjit_operand_vreg(t), body, done), "op");
  check(grjit_builder_set_block(b, body), "at");
  if (kind == LOOP_POLL) {
    GRCORE_PollIdentity id = {1, 1};
    check(grjit_builder_poll(b, id, NULL, 0), "poll");
  } else if (kind == LOOP_CALL) {
    GRCORE_PollIdentity id = {0, 0};
    check(grjit_builder_call(b, GRJIT_NO_VREG, (uint64_t)(uintptr_t)no_op_helper,
              GRJIT_CALL_NO_GC, GRCORE_SITE_GC_POINT_CALL, NULL, 0, id, NULL, 0),
        "call");
  }
  check(grjit_builder_binary(b, GRJIT_OP_ADD, sum, grjit_operand_vreg(sum), grjit_operand_vreg(i)), "op");
  check(grjit_builder_binary(b, GRJIT_OP_ADD, i, grjit_operand_vreg(i), grjit_operand_imm(1)), "op");
  check(grjit_builder_br(b, head), "op");
  check(grjit_builder_set_block(b, done), "at");
  check(grjit_builder_ret(b, grjit_operand_vreg(sum)), "op");
  GRJIT_Function * f;
  check(grjit_builder_finish(b, &f), "finish");
  return f;
}

static GRJIT_Code * g_loop[3];

static GRJIT_Code * compile(const GRJIT_Function * f) {
  GRJIT_CompileOptions o;
  memset(&o, 0, sizeof o);
  o.pages = grcore_context_page_provider(g_context);
  GRJIT_Code * code;
  check(grjit_compile(&o, f, &code), "compile");
  return code;
}

static uint64_t run_loop(LoopKind kind, uint64_t iterations) {
  world_open();
  if (g_loop[kind] == NULL) {
    GRJIT_Function * f = build_loop(kind);
    g_loop[kind] = compile(f);
    grjit_function_destroy(f);
  }
  uint64_t args[1] = {iterations};
  uint64_t out[1] = {0};
  if (grjit_code_call(g_loop[kind], g_context, args, out) != GRJIT_EXIT_RETURNED) {
    setup_failed("the loop did not return");
  }
  return out[0] ^ g_helper_calls;
}

static uint64_t loop_plain_run(uint64_t n) { return run_loop(LOOP_PLAIN, n); }
static uint64_t loop_poll_run(uint64_t n) { return run_loop(LOOP_POLL, n); }
static uint64_t loop_call_run(uint64_t n) { return run_loop(LOOP_CALL, n); }

/* ---- calls between compiled functions (AD-28) ---------------------------- */

static uint32_t call_push(void * context, uint64_t callee, const uint64_t * args, uint64_t n) {
  (void)context;
  (void)callee;
  (void)args;
  (void)n;
  return 0;
}
static void call_pop(void * context) { (void)context; }
static uint32_t call_compile(void * context, uint64_t callee) {
  (void)context;
  (void)callee;
  return 1;
}
static uint32_t call_deopt(void * context, uint64_t cause) {
  (void)context;
  (void)cause;
  setup_failed("a deopt in the call loop");
}

typedef struct {
  GRJIT_Code * callee;
  GRJIT_Code * caller;
  GRCORE_Code * handle;
  GRCORE_EntrySlot * slot;
} CallLoop;
static CallLoop g_call_loop;

static void call_release(void * payload) { (void)payload; }

/* inc(x) = x + 1, callable; loop(n): for i in 0..n { sum = inc(sum) }, with each
 * call through an entry slot and the engine's hooks doing nothing, so the figure
 * is the call, the status test and the four hook calls (push and pop) and not an
 * engine's guest stack. */
static GRJIT_Function * build_callable_inc(void) {
  GRJIT_Builder * b;
  GRJIT_CallHooks hooks = {call_push, call_pop, call_compile, call_deopt, NULL};
  check(grjit_builder_create("inc", 0, NULL, NULL, &b), "builder");
  GRJIT_VReg x, r;
  GRJIT_BlockId entry;
  check(grjit_builder_param(b, GRJIT_TYPE_I64, &x), "param");
  check(grjit_builder_vreg(b, GRJIT_TYPE_I64, &r), "vreg");
  check(grjit_builder_set_callable(b, &hooks), "callable");
  check(grjit_builder_block(b, &entry), "block");
  check(grjit_builder_set_block(b, entry), "at");
  check(grjit_builder_binary(b, GRJIT_OP_ADD, r, grjit_operand_vreg(x), grjit_operand_imm(1)), "op");
  check(grjit_builder_ret(b, grjit_operand_vreg(r)), "op");
  GRJIT_Function * f;
  check(grjit_builder_finish(b, &f), "finish");
  return f;
}

static GRJIT_Function * build_call_loop(const GRCORE_EntrySlot * slot) {
  GRJIT_Builder * b;
  GRJIT_CallHooks hooks = {call_push, call_pop, call_compile, call_deopt, NULL};
  check(grjit_builder_create("call_loop", 2, NULL, NULL, &b), "builder");
  GRJIT_VReg n, i, sum, t;
  GRJIT_BlockId entry, head, body, done;
  check(grjit_builder_param(b, GRJIT_TYPE_I64, &n), "param");
  check(grjit_builder_vreg(b, GRJIT_TYPE_I64, &i), "vreg");
  check(grjit_builder_vreg(b, GRJIT_TYPE_I64, &sum), "vreg");
  check(grjit_builder_vreg(b, GRJIT_TYPE_I64, &t), "vreg");
  check(grjit_builder_set_callable(b, &hooks), "callable");
  check(grjit_builder_block(b, &entry), "block");
  check(grjit_builder_block(b, &head), "block");
  check(grjit_builder_block(b, &body), "block");
  check(grjit_builder_block(b, &done), "block");
  check(grjit_builder_set_block(b, entry), "at");
  check(grjit_builder_const(b, i, 0), "op");
  check(grjit_builder_const(b, sum, 0), "op");
  check(grjit_builder_br(b, head), "op");
  check(grjit_builder_set_block(b, head), "at");
  check(grjit_builder_cmp(b, GRJIT_CMP_LT, t, grjit_operand_vreg(i), grjit_operand_vreg(n)), "op");
  check(grjit_builder_br_if(b, grjit_operand_vreg(t), body, done), "op");
  check(grjit_builder_set_block(b, body), "at");
  GRCORE_PollIdentity id = {1, 1};
  GRJIT_FrameSlot st[2] = {grjit_frame_slot_vreg(i), grjit_frame_slot_vreg(sum)};
  GRJIT_Operand args[1] = {grjit_operand_vreg(sum)};
  check(grjit_builder_call_slot(b, sum, (uint64_t)(uintptr_t)&slot->entry, 0, args, 1, id, st,
            2, id, st, 2),
      "call");
  check(grjit_builder_binary(b, GRJIT_OP_ADD, i, grjit_operand_vreg(i), grjit_operand_imm(1)), "op");
  check(grjit_builder_br(b, head), "op");
  check(grjit_builder_set_block(b, done), "at");
  check(grjit_builder_ret(b, grjit_operand_vreg(sum)), "op");
  GRJIT_Function * f;
  check(grjit_builder_finish(b, &f), "finish");
  return f;
}

static uint64_t loop_compiled_call_run(uint64_t iterations) {
  world_open();
  if (g_call_loop.caller == NULL) {
    static const GRCORE_EngineDescriptor descriptor = GRCORE_ENGINE_DESCRIPTOR_INIT(
        "bench", NULL, NULL, NULL, {NULL, NULL, NULL}, {0, 0, 0}, NULL, NULL, NULL, NULL);
    GRCORE_EngineId engine;
    if (grcore_engine_register(g_context, &descriptor, &engine) != GRCORE_OK ||
        grcore_entry_slot_create(g_context, &g_call_loop.slot) != GRCORE_OK ||
        grcore_code_create(NULL, NULL, call_release, &g_call_loop.handle) != GRCORE_OK) {
      setup_failed("an engine and a slot");
    }
    GRJIT_Function * inc = build_callable_inc();
    g_call_loop.callee = compile(inc);
    grjit_function_destroy(inc);
    if (grcore_entry_slot_set(g_context, g_call_loop.slot, g_call_loop.handle,
            grjit_code_internal_entry(g_call_loop.callee)) != GRCORE_OK) {
      setup_failed("the slot");
    }
    GRJIT_Function * loop = build_call_loop(g_call_loop.slot);
    g_call_loop.caller = compile(loop);
    grjit_function_destroy(loop);
  }
  uint64_t args[1] = {iterations};
  uint64_t out[3] = {0, 0, 0};
  if (grjit_code_call(g_call_loop.caller, g_context, args, out) != GRJIT_EXIT_RETURNED) {
    setup_failed("the call loop did not return");
  }
  return out[0];
}

/* ---- tail calls (AD-28) --------------------------------------------------- */

static uint32_t call_tail(void * context, uint64_t callee, const uint64_t * args, uint64_t n) {
  (void)context;
  (void)callee;
  (void)args;
  (void)n;
  return 0;
}

typedef struct {
  GRJIT_Code * code;
  GRCORE_Code * handle;
  GRCORE_EntrySlot * slot;
} TailLoop;
static TailLoop g_tail_loop;

/* tail_loop(n, sum): n == 0 ? sum : tail_loop(n - 1, sum + 1), the tail call
 * through the function's own entry slot with the engine's hook doing nothing, so
 * the figure is the dispatch, the staging, the hook's C call and the frame
 * replacement, and not an engine's guest stack. */
static GRJIT_Function * build_tail_loop(const GRCORE_EntrySlot * slot) {
  GRJIT_Builder * b;
  GRJIT_CallHooks hooks = {call_push, call_pop, call_compile, call_deopt, call_tail};
  check(grjit_builder_create("tail_loop", 2, NULL, NULL, &b), "builder");
  GRJIT_VReg n, sum, t, n1, s1;
  GRJIT_BlockId entry, body, done;
  check(grjit_builder_param(b, GRJIT_TYPE_I64, &n), "param");
  check(grjit_builder_param(b, GRJIT_TYPE_I64, &sum), "param");
  check(grjit_builder_vreg(b, GRJIT_TYPE_I64, &t), "vreg");
  check(grjit_builder_vreg(b, GRJIT_TYPE_I64, &n1), "vreg");
  check(grjit_builder_vreg(b, GRJIT_TYPE_I64, &s1), "vreg");
  check(grjit_builder_set_callable(b, &hooks), "callable");
  check(grjit_builder_block(b, &entry), "block");
  check(grjit_builder_block(b, &body), "block");
  check(grjit_builder_block(b, &done), "block");
  check(grjit_builder_set_block(b, entry), "at");
  check(grjit_builder_cmp(b, GRJIT_CMP_EQ, t, grjit_operand_vreg(n), grjit_operand_imm(0)), "op");
  check(grjit_builder_br_if(b, grjit_operand_vreg(t), done, body), "op");
  check(grjit_builder_set_block(b, body), "at");
  check(grjit_builder_binary(b, GRJIT_OP_SUB, n1, grjit_operand_vreg(n), grjit_operand_imm(1)), "op");
  check(grjit_builder_binary(b, GRJIT_OP_ADD, s1, grjit_operand_vreg(sum), grjit_operand_imm(1)), "op");
  GRCORE_PollIdentity id = {1, 1};
  GRJIT_FrameSlot st[2] = {grjit_frame_slot_vreg(n), grjit_frame_slot_vreg(sum)};
  GRJIT_Operand args[2] = {grjit_operand_vreg(n1), grjit_operand_vreg(s1)};
  check(grjit_builder_tail_call_slot(
            b, (uint64_t)(uintptr_t)&slot->entry, 0, args, 2, id, st, 2),
      "tail call");
  check(grjit_builder_set_block(b, done), "at");
  check(grjit_builder_ret(b, grjit_operand_vreg(sum)), "op");
  GRJIT_Function * f;
  check(grjit_builder_finish(b, &f), "finish");
  return f;
}

static uint64_t loop_tail_call_run(uint64_t iterations) {
  world_open();
  if (g_tail_loop.code == NULL) {
    static const GRCORE_EngineDescriptor descriptor = GRCORE_ENGINE_DESCRIPTOR_INIT(
        "bench-tail", NULL, NULL, NULL, {NULL, NULL, NULL}, {0, 0, 0}, NULL, NULL, NULL, NULL);
    GRCORE_EngineId engine;
    if (grcore_engine_register(g_context, &descriptor, &engine) != GRCORE_OK ||
        grcore_entry_slot_create(g_context, &g_tail_loop.slot) != GRCORE_OK ||
        grcore_code_create(NULL, NULL, call_release, &g_tail_loop.handle) != GRCORE_OK) {
      setup_failed("an engine and a slot");
    }
    GRJIT_Function * loop = build_tail_loop(g_tail_loop.slot);
    g_tail_loop.code = compile(loop);
    grjit_function_destroy(loop);
    if (grcore_entry_slot_set(g_context, g_tail_loop.slot, g_tail_loop.handle,
            grjit_code_internal_entry(g_tail_loop.code)) != GRCORE_OK) {
      setup_failed("the slot");
    }
  }
  uint64_t args[2] = {iterations, 0};
  uint64_t out[3] = {0, 0, 0};
  if (grjit_code_call(g_tail_loop.code, g_context, args, out) != GRJIT_EXIT_RETURNED) {
    setup_failed("the tail loop did not return");
  }
  if (out[0] != iterations) {
    setup_failed("the tail loop's sum is not its iteration count");
  }
  return out[0];
}

/* ---- calls to natives (AD-28, CAP-7) -------------------------------------- */

typedef enum { LOOP_NATIVE, LOOP_NATIVE_STATUS, LOOP_HELPER_GC } NativeLoopKind;

static uint64_t native_inc(void * context, uint64_t x) {
  (void)context;
  return x + 1;
}
static GRJIT_NativeResult native_inc_status(void * context, uint64_t x) {
  (void)context;
  GRJIT_NativeResult r = {x + 1, 0, 0};
  return r;
}
/* The trusted helper a CALL reaches: the same work, no context, no status. */
static uint64_t helper_inc(uint64_t x) { return x + 1; }

static GRJIT_NativeTable * g_native_table;
static uint32_t g_native_ids[2];
static GRJIT_Code * g_native_loop[3];

/* loop(n): for i in 0..n { sum = inc(sum) }, in a callable function, with the call
 * a registered native without a status (the C call with the context, the walk
 * start stored first, the native-stack check), a registered native with a status
 * (and the result pair tested), or the engine's trusted helper through GRJIT_OP_CALL
 * as a GC point (the walk start stored, no check, no status), so the three figures
 * are the cost of each protocol over the same work. */
static GRJIT_Function * build_native_loop(NativeLoopKind kind) {
  GRJIT_Builder * b;
  GRJIT_CallHooks hooks = {call_push, call_pop, call_compile, call_deopt, NULL};
  check(grjit_builder_create("native_loop", 2, NULL, NULL, &b), "builder");
  GRJIT_VReg n, i, sum, t;
  GRJIT_BlockId entry, head, body, done;
  check(grjit_builder_param(b, GRJIT_TYPE_I64, &n), "param");
  check(grjit_builder_vreg(b, GRJIT_TYPE_I64, &i), "vreg");
  check(grjit_builder_vreg(b, GRJIT_TYPE_I64, &sum), "vreg");
  check(grjit_builder_vreg(b, GRJIT_TYPE_I64, &t), "vreg");
  check(grjit_builder_set_callable(b, &hooks), "callable");
  check(grjit_builder_set_natives(b, g_native_table), "natives");
  check(grjit_builder_block(b, &entry), "block");
  check(grjit_builder_block(b, &head), "block");
  check(grjit_builder_block(b, &body), "block");
  check(grjit_builder_block(b, &done), "block");
  check(grjit_builder_set_block(b, entry), "at");
  check(grjit_builder_const(b, i, 0), "op");
  check(grjit_builder_const(b, sum, 0), "op");
  check(grjit_builder_br(b, head), "op");
  check(grjit_builder_set_block(b, head), "at");
  check(grjit_builder_cmp(b, GRJIT_CMP_LT, t, grjit_operand_vreg(i), grjit_operand_vreg(n)), "op");
  check(grjit_builder_br_if(b, grjit_operand_vreg(t), body, done), "op");
  check(grjit_builder_set_block(b, body), "at");
  GRJIT_FrameSlot st[2] = {grjit_frame_slot_vreg(i), grjit_frame_slot_vreg(sum)};
  GRJIT_FrameState before = {{1, 1}, 2, st};
  GRJIT_FrameState after = {{1, 2}, 2, st};
  GRJIT_Operand args[1] = {grjit_operand_vreg(sum)};
  if (kind == LOOP_HELPER_GC) {
    check(grjit_builder_call(b, sum, (uint64_t)(uintptr_t)helper_inc, GRJIT_CALL_GC_POINT,
              GRCORE_SITE_GC_POINT_CALL, args, 1, before.identity, st, 2),
        "call");
  } else {
    check(grjit_builder_call_native(b, sum, g_native_ids[kind == LOOP_NATIVE ? 0 : 1], args, 1,
              &before, kind == LOOP_NATIVE ? NULL : &after),
        "native call");
  }
  check(grjit_builder_binary(b, GRJIT_OP_ADD, i, grjit_operand_vreg(i), grjit_operand_imm(1)), "op");
  check(grjit_builder_br(b, head), "op");
  check(grjit_builder_set_block(b, done), "at");
  check(grjit_builder_ret(b, grjit_operand_vreg(sum)), "op");
  GRJIT_Function * f;
  check(grjit_builder_finish(b, &f), "finish");
  return f;
}

static uint64_t run_native_loop(NativeLoopKind kind, uint64_t iterations) {
  world_open();
  if (g_native_table == NULL) {
    GRJIT_Type one[1] = {GRJIT_TYPE_I64};
    GRJIT_NativeDesc d;
    memset(&d, 0, sizeof d);
    d.params = one;
    d.param_count = 1;
    d.result = GRJIT_TYPE_I64;
    d.stack_bytes = 64;
    if (grjit_native_table_create(NULL, NULL, &g_native_table) != GRJIT_OK) {
      setup_failed("a native table");
    }
    d.address = (uint64_t)(uintptr_t)native_inc;
    check(grjit_native_table_add(g_native_table, &d, &g_native_ids[0]), "native");
    d.address = (uint64_t)(uintptr_t)native_inc_status;
    d.flags = GRJIT_NATIVE_STATUS;
    check(grjit_native_table_add(g_native_table, &d, &g_native_ids[1]), "native");
  }
  if (g_native_loop[kind] == NULL) {
    GRJIT_Function * f = build_native_loop(kind);
    g_native_loop[kind] = compile(f);
    grjit_function_destroy(f);
  }
  uint64_t args[1] = {iterations};
  uint64_t out[3] = {0, 0, 0};
  if (grjit_code_call(g_native_loop[kind], g_context, args, out) != GRJIT_EXIT_RETURNED) {
    setup_failed("the native loop did not return");
  }
  if (out[0] != iterations) {
    setup_failed("the native loop's sum is not its iteration count");
  }
  return out[0];
}

static uint64_t loop_native_run(uint64_t n) { return run_native_loop(LOOP_NATIVE, n); }
static uint64_t loop_native_status_run(uint64_t n) { return run_native_loop(LOOP_NATIVE_STATUS, n); }
static uint64_t loop_helper_gc_run(uint64_t n) { return run_native_loop(LOOP_HELPER_GC, n); }

/* A function of 100 operations: alternating arithmetic on a few registers. */
static GRJIT_Function * build_hundred(void) {
  GRJIT_Builder * b;
  check(grjit_builder_create("hundred", 0, NULL, NULL, &b), "builder");
  GRJIT_VReg x, y, z;
  GRJIT_BlockId entry;
  check(grjit_builder_param(b, GRJIT_TYPE_I64, &x), "param");
  check(grjit_builder_vreg(b, GRJIT_TYPE_I64, &y), "vreg");
  check(grjit_builder_vreg(b, GRJIT_TYPE_I64, &z), "vreg");
  check(grjit_builder_block(b, &entry), "block");
  check(grjit_builder_set_block(b, entry), "at");
  check(grjit_builder_const(b, y, 1), "op");
  check(grjit_builder_const(b, z, 2), "op");
  static const GRJIT_OpKind kinds[] = {GRJIT_OP_ADD, GRJIT_OP_XOR, GRJIT_OP_MUL, GRJIT_OP_SUB};
  for (int i = 0; i < 96; i++) {
    GRJIT_VReg d = i % 3 == 0 ? y : i % 3 == 1 ? z : x;
    GRJIT_VReg a = i % 3 == 0 ? z : i % 3 == 1 ? x : y;
    check(grjit_builder_binary(b, kinds[i % 4], d, grjit_operand_vreg(a), grjit_operand_imm(i + 3)), "op");
  }
  check(grjit_builder_binary(b, GRJIT_OP_ADD, x, grjit_operand_vreg(x), grjit_operand_vreg(y)), "op");
  check(grjit_builder_binary(b, GRJIT_OP_ADD, x, grjit_operand_vreg(x), grjit_operand_vreg(z)), "op");
  check(grjit_builder_ret(b, grjit_operand_vreg(x)), "op");
  GRJIT_Function * f;
  check(grjit_builder_finish(b, &f), "finish");
  return f;
}

static uint64_t compile_run(uint64_t iterations) {
  world_open();
  GRJIT_Function * f = build_hundred();
  uint64_t sink = grjit_function_op_count(f);
  for (uint64_t i = 0; i < iterations; i++) {
    GRJIT_Code * code = compile(f);
    sink += grjit_code_size(code);
    grjit_code_destroy(code);
  }
  grjit_function_destroy(f);
  return sink;
}

#define SITES_N 12000u

/* SITES_N garbage-collection points, each with one reference live across it,
 * and as many reference registers as sites: ref_i = bitcast(i); call; use
 * ref_i. The compile of it is the cost of the liveness and metadata passes
 * with many sites and many tracked registers, where a pass that looks at
 * every register at every site is quadratic. An iteration is one compile. */
static GRJIT_Function * build_sites(void) {
  GRJIT_Builder * b;
  check(grjit_builder_create("sites", 0, NULL, NULL, &b), "builder");
  GRJIT_VReg acc, k, t;
  GRJIT_BlockId entry;
  check(grjit_builder_vreg(b, GRJIT_TYPE_I64, &acc), "vreg");
  check(grjit_builder_vreg(b, GRJIT_TYPE_I64, &k), "vreg");
  check(grjit_builder_vreg(b, GRJIT_TYPE_I64, &t), "vreg");
  check(grjit_builder_block(b, &entry), "block");
  check(grjit_builder_set_block(b, entry), "at");
  check(grjit_builder_const(b, acc, 0), "op");
  GRCORE_PollIdentity id = {0, 0};
  for (unsigned i = 0; i < SITES_N; i++) {
    GRJIT_VReg r;
    check(grjit_builder_vreg(b, GRJIT_TYPE_REF, &r), "vreg");
    check(grjit_builder_const(b, k, (int64_t)i), "op");
    check(grjit_builder_bitcast(b, r, k), "op");
    id.offset = i;
    check(grjit_builder_call(b, GRJIT_NO_VREG, (uint64_t)(uintptr_t)no_op_helper,
              GRJIT_CALL_GC_POINT, GRCORE_SITE_GC_POINT_CALL, NULL, 0, id, NULL, 0),
        "call");
    check(grjit_builder_bitcast(b, t, r), "op");
    check(grjit_builder_binary(b, GRJIT_OP_ADD, acc, grjit_operand_vreg(acc), grjit_operand_vreg(t)), "op");
  }
  check(grjit_builder_ret(b, grjit_operand_vreg(acc)), "op");
  GRJIT_Function * f;
  check(grjit_builder_finish(b, &f), "finish");
  return f;
}

static uint64_t compile_sites_run(uint64_t iterations) {
  world_open();
  GRJIT_Function * f = build_sites();
  uint64_t sink = grjit_function_op_count(f);
  for (uint64_t i = 0; i < iterations; i++) {
    GRJIT_Code * code = compile(f);
    sink += grjit_code_size(code);
    grjit_code_destroy(code);
  }
  grjit_function_destroy(f);
  return sink;
}

static int compare_double(const void * a, const void * b) {
  double x = *(const double *)a, y = *(const double *)b;
  return x < y ? -1 : x > y ? 1 : 0;
}

static const Case cases[] = {
    {"calibration", calibration_run, 50000000, 100000, 0},
    {"compile-100", compile_run, 2000, 3, 0},
    {"compile-12k-sites", compile_sites_run, 3, 1, 0},
    {"loop-plain", loop_plain_run, 10000000, 1000, 0},
    {"loop-poll", loop_poll_run, 10000000, 1000, 0},
    {"loop-call", loop_call_run, 10000000, 1000, 0},
    {"loop-compiled-call", loop_compiled_call_run, 10000000, 1000, 1},
    {"loop-tail-call", loop_tail_call_run, 10000000, 1000, 1},
    {"loop-helper-gc", loop_helper_gc_run, 10000000, 1000, 1},
    {"loop-native", loop_native_run, 10000000, 1000, 1},
    {"loop-native-status", loop_native_status_run, 10000000, 1000, 1},
};

int main(int argc, char ** argv) {
  int smoke = 0;
  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "--smoke") == 0) {
      smoke = 1;
    } else {
      fprintf(stderr, "bench: unknown argument '%s'\n", argv[i]);
      return 2;
    }
  }
  if (!grjit_backend_available()) {
    /* Exit status 77 is "skipped": the Makefile counts it and does not fail.
     * Only a target with no backend (Windows arm64, macOS) gets here. */
    printf("SKIP: bench: no native code backend on this target\n");
    return 77;
  }

  /* Naming the library's version proves the harness linked the library it
   * claims to measure, and ties every figure to the build that produced it. */
  printf("runtime-jit %s, %s run\n", grjit_version_string(), smoke ? "smoke" : "full");

  int repeats = smoke ? 1 : REPEATS;
  for (size_t c = 0; c < sizeof(cases) / sizeof(cases[0]); c++) {
    if (cases[c].needs_calls && !grjit_backend_calls_available()) {
      /* Named, not silently left out: this target refuses calls between
       * compiled functions (arm64 and Win64 have them in a later story). */
      printf("%-12s skipped: this target has no calls between compiled functions\n",
          cases[c].name);
      continue;
    }
    uint64_t n = smoke ? cases[c].smoke_iterations : cases[c].iterations;
    double ns[REPEATS];
    uint64_t sink = 0;
    for (int r = 0; r < repeats; r++) {
      double start = now_ns();
      sink ^= cases[c].run(n);
      ns[r] = now_ns() - start;
    }
    qsort(ns, (size_t)repeats, sizeof(ns[0]), compare_double);
    double best = ns[0] / (double)n;
    double median = ns[repeats / 2] / (double)n;
    if (!(best > 0.0)) {
      fprintf(stderr, "bench: %s measured no time; the clock is unusable\n", cases[c].name);
      return 1;
    }
    printf("%-12s best %10.3f ns/op  median %10.3f ns/op  (%llu ops x %d, check %016llx)\n",
        cases[c].name, best, median, (unsigned long long)n, repeats,
        (unsigned long long)sink);
  }
  for (int k = 0; k < 3; k++) {
    grjit_code_destroy(g_loop[k]);
  }
  if (g_call_loop.caller != NULL) {
    grcore_entry_slot_clear(g_context, g_call_loop.slot);
    grcore_code_release(g_call_loop.handle);
    grjit_code_destroy(g_call_loop.caller);
    grjit_code_destroy(g_call_loop.callee);
  }
  if (g_tail_loop.code != NULL) {
    grcore_entry_slot_clear(g_context, g_tail_loop.slot);
    grcore_code_release(g_tail_loop.handle);
    grjit_code_destroy(g_tail_loop.code);
  }
  for (int k = 0; k < 3; k++) {
    grjit_code_destroy(g_native_loop[k]);
  }
  grjit_native_table_free(g_native_table);
  if (g_context != NULL) {
    grcore_context_destroy(g_context);
    grcore_group_destroy(g_group);
  }
  return 0;
}
