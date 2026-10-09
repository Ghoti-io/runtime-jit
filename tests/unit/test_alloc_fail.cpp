/**
 * @file
 *
 * Allocation failure is a code path: the Nth allocation of `grjit_compile`
 * (the allocator and the page provider both failing), of the builder and of
 * the verifier fails, and each time the call reports `ERR_OOM`, changes
 * nothing, leaks no block, and leaves the page provider's meter where it
 * started.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include "test_helpers.h"

#include "../../src/code/code_internal.h"

#ifndef _WIN32
#include <unistd.h>
#endif

namespace {

// Arms an alarm that turns a hang into a failure of the test, and cancels it when
// the test leaves by any path (an early ASSERT included).
struct HangAlarm {
#ifndef _WIN32
  HangAlarm() { alarm(120); }
  ~HangAlarm() { alarm(0); }
#else
  HangAlarm() = default; // no alarm() on Windows; the per-test watchdog covers a hang
#endif
  HangAlarm(const HangAlarm &) = delete;
  HangAlarm & operator=(const HangAlarm &) = delete;
};

uint64_t helper(uint64_t a) { return a; }
uint32_t poll_helper(void *, uint64_t, uint64_t) { return 0; }
uint32_t deopt_hook(void *, uint64_t) { return 0; }

/* A function that reaches every allocating arm of the compiler: a derived
 * pointer, a poll, a GC-point call, a guard and a branch. */
GRJIT_Function * build(const GRJIT_Allocator * allocator, bool callable = false) {
  B b("sweep", 2, nullptr, allocator);
  if (callable) {
    GRJIT_CallHooks hooks{};
    hooks.deopt = deopt_hook;
    b.callable(hooks);
    EXPECT_EQ(grjit_builder_set_token(b.b, 7), GRJIT_OK);
  }
  GRJIT_VReg c = b.param(GRJIT_TYPE_I64);
  GRJIT_VReg r = b.reg(GRJIT_TYPE_REF), q = b.reg(GRJIT_TYPE_PTR), d = b.reg();
  b.derived(q, r, 8);
  b.poll_helper(poll_helper);
  GRJIT_BlockId e = b.block(), a = b.block(), o = b.block();
  b.at(e);
  b.cnst(r, 0x1001);
  b.cnst(q, 0x1009);
  b.cnst(d, 0);
  b.poll({1, 1}, {grjit_frame_slot_vreg(r), grjit_frame_slot_constant(3)});
  b.br_if(V(c), a, o);
  b.at(a);
  b.call_gc(d, reinterpret_cast<const void *>(helper), {V(r)}, {1, 2},
      {grjit_frame_slot_vreg(r), grjit_frame_slot_vreg(q)});
  b.guard(V(d), {1, 3}, {grjit_frame_slot_vreg(q), grjit_frame_slot_vreg(r)});
  b.br(o);
  b.at(o);
  b.cmp(GRJIT_CMP_EQ, d, V(r), V(r));
  b.bin(GRJIT_OP_ADD, d, V(d), V(q));
  b.ret(V(d));
  return b.finish();
}

} // namespace

TEST(AllocFail, EveryAllocationOfCompileFailsCleanlyAndTheSweepEndsInSuccess) {
  GRJIT_REQUIRE_BACKEND();
  JitWorld w;
  Fn f(build(nullptr));
  uint64_t bytes = w.bytes_in_use();
  uint64_t blocks = w.blocks_in_use();
  long failures = 0;
  for (long n = 1;; n++) {
    TrackingAllocator t;
    t.fail_at = n;
    GRJIT_CompileOptions o{};
    o.pages = w.pages();
    o.allocator = t.get();
    GRJIT_Code * out = reinterpret_cast<GRJIT_Code *>(1);
    GRJIT_Result r = grjit_compile(&o, f, &out);
    if (r == GRJIT_OK) {
      ASSERT_NE(out, reinterpret_cast<GRJIT_Code *>(1));
      grjit_code_destroy(out);
      EXPECT_EQ(t.live, 0);
      EXPECT_LT(t.calls, n) << "the sweep ended before the Nth allocation was reached";
      break;
    }
    failures++;
    ASSERT_EQ(r, GRJIT_ERR_OOM) << n;
    EXPECT_EQ(out, reinterpret_cast<GRJIT_Code *>(1)) << n;
    EXPECT_EQ(t.live, 0) << "a block leaked after failing allocation " << n;
    EXPECT_EQ(w.bytes_in_use(), bytes) << n;
    EXPECT_EQ(w.blocks_in_use(), blocks) << n;
    ASSERT_LT(n, 500);
  }
  EXPECT_GT(failures, 10); // the sweep really did fail things
}

TEST(AllocFail, EveryAllocationOfACallableCompileFailsCleanlyAndNoFailureLoopsInThePaddingOfTheInternalEntry) {
  GRJIT_REQUIRE_BACKEND();
  // The internal entry of a callable function is padded to a 16-byte boundary
  // with a loop that appends one byte (or one trap word) at a time, and an
  // assembler whose buffer could not grow appends nothing: the loop must end
  // when the assembler has failed, or a compile that runs out of memory at that
  // point never returns. The alarm turns a hang into a failure of this test.
  HangAlarm hang_alarm;
  JitWorld w;
  Fn f(build(nullptr, true));
  ASSERT_NE(f.f, nullptr);
  uint64_t bytes = w.bytes_in_use();
  long failures = 0;
  for (long n = 1;; n++) {
    TrackingAllocator t;
    t.fail_at = n;
    GRJIT_CompileOptions o{};
    o.pages = w.pages();
    o.allocator = t.get();
    GRJIT_Code * out = reinterpret_cast<GRJIT_Code *>(1);
    GRJIT_Result r = grjit_compile(&o, f, &out);
    if (r == GRJIT_OK) {
      ASSERT_NE(out, reinterpret_cast<GRJIT_Code *>(1));
      EXPECT_TRUE(grjit_code_callable(out));
      grjit_code_destroy(out);
      EXPECT_EQ(t.live, 0);
      EXPECT_LT(t.calls, n) << "the sweep ended before the Nth allocation was reached";
      break;
    }
    failures++;
    ASSERT_EQ(r, GRJIT_ERR_OOM) << n;
    EXPECT_EQ(t.live, 0) << "a block leaked after failing allocation " << n;
    EXPECT_EQ(w.bytes_in_use(), bytes) << n;
    ASSERT_LT(n, 500);
  }
  EXPECT_GT(failures, 10);
}

TEST(AllocFail, EveryAllocationOfACallableArm64EmissionFailsCleanlyAndTheTrapPaddingEnds) {
  // The arm64 emitter pads the internal entry with trap words in a loop of its
  // own, which carries the same guard as the x86-64 one above. Its buffer starts
  // at 256 bytes, so the padding never needs the allocator and this sweep does not
  // reach a failing buffer there: it shows the callable emission is clean under
  // allocation failure on arm64, which no sweep did, and the guard is by reading.
  // Emission needs no arm64 host. The alarm turns a hang into a failure.
  HangAlarm hang_alarm;
  Fn f(build(nullptr, true));
  ASSERT_NE(f.f, nullptr);
  long failures = 0;
  for (long n = 1;; n++) {
    TrackingAllocator t;
    t.fail_at = n;
    GRJIT_Emitted e{};
    GRJIT_Result r = grjit_emit_for(GRJIT_ARCH_ARM64, f, t.get(), nullptr, nullptr,
        grcore_jit_layout()->request_word_offset, &e);
    if (r == GRJIT_OK) {
      grjit_emitted_free(&e);
      EXPECT_EQ(t.live, 0);
      EXPECT_LT(t.calls, n);
      break;
    }
    failures++;
    ASSERT_EQ(r, GRJIT_ERR_OOM) << n;
    EXPECT_EQ(t.live, 0) << "a block leaked after failing allocation " << n;
    ASSERT_LT(n, 500);
  }
  EXPECT_GT(failures, 10);
}

TEST(AllocFail, AFailingPageProviderIsOomAndLeavesNothingMapped) {
  GRJIT_REQUIRE_BACKEND();
  JitWorld w;
  Fn f(build(nullptr));
  FakePages fake(w.pages());
  fake.fail_map_at = 1;
  GRJIT_CompileOptions o{};
  o.pages = &fake.vtable;
  GRJIT_Code * out = nullptr;
  EXPECT_EQ(grjit_compile(&o, f, &out), GRJIT_ERR_OOM);
  EXPECT_EQ(out, nullptr);
  EXPECT_EQ(fake.live, 0);
  EXPECT_EQ(w.bytes_in_use(), 0u);
  // The same provider, no longer failing, compiles.
  fake.fail_map_at = 0;
  EXPECT_EQ(grjit_compile(&o, f, &out), GRJIT_OK);
  grjit_code_destroy(out);
  EXPECT_EQ(fake.live, 0);
}

TEST(AllocFail, TheMeterRefusalOfAContextAtItsMemoryLimitIsOomToo) {
  GRJIT_REQUIRE_BACKEND();
  GRCORE_Group * g;
  ASSERT_EQ(grcore_group_create(nullptr, nullptr, &g), GRCORE_OK);
  GRCORE_Options * opts;
  ASSERT_EQ(grcore_options_create(nullptr, &opts), GRCORE_OK);
  grcore_options_set_memory_bytes(opts, 1); // less than one page
  grcore_options_set_memory_reserve(opts, 0);
  GRCORE_Context * ctx;
  ASSERT_EQ(grcore_context_create(g, opts, &ctx), GRCORE_OK);
  grcore_options_destroy(opts);
  Fn f(build(nullptr));
  GRJIT_CompileOptions o{};
  o.pages = grcore_context_page_provider(ctx);
  GRJIT_Code * out = nullptr;
  EXPECT_EQ(grjit_compile(&o, f, &out), GRJIT_ERR_OOM);
  EXPECT_EQ(out, nullptr);
  EXPECT_EQ(grcore_context_memory_blocks(ctx), 0u);
  EXPECT_EQ(grcore_context_destroy(ctx), GRCORE_OK);
  EXPECT_EQ(grcore_group_destroy(g), GRCORE_OK);
}

TEST(AllocFail, EveryAllocationOfTheVerifierFailsCleanly) {
  TrackingAllocator t;
  Fn f(build(t.get()));
  long live = t.live;
  for (long n = 1;; n++) {
    t.calls = 0;
    t.fail_at = n;
    GRJIT_Result r = grjit_function_verify(f, nullptr, nullptr, 0);
    t.fail_at = 0;
    EXPECT_EQ(t.live, live) << n;
    if (r == GRJIT_OK) {
      EXPECT_LT(t.calls, n);
      break;
    }
    ASSERT_EQ(r, GRJIT_ERR_OOM) << n;
    ASSERT_LT(n, 100);
  }
}

TEST(AllocFail, EveryAllocationOfCreatingABuilderFailsCleanly) {
  for (long n = 1; n <= 3; n++) {
    TrackingAllocator t;
    t.fail_at = n;
    GRJIT_Builder * b = reinterpret_cast<GRJIT_Builder *>(1);
    GRJIT_Result r = grjit_builder_create("x", 0, nullptr, t.get(), &b);
    if (r == GRJIT_OK) {
      grjit_builder_destroy(b);
    } else {
      EXPECT_EQ(r, GRJIT_ERR_OOM);
      EXPECT_EQ(b, reinterpret_cast<GRJIT_Builder *>(1));
    }
    EXPECT_EQ(t.live, 0) << n;
  }
}

GRJIT_TEST_MAIN()
