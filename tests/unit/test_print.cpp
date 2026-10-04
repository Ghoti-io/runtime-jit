/**
 * @file
 *
 * The printer: deterministic text, which is how tests assert a function's
 * shape.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include "test_helpers.h"

TEST(Print, AFunctionPrintsEveryOperationInAFixedForm) {
  B b("shape", 2);
  GRJIT_VReg p = b.param(GRJIT_TYPE_PTR);
  GRJIT_VReg r = b.param(GRJIT_TYPE_REF);
  GRJIT_VReg a = b.reg(), c = b.reg(), q = b.reg(GRJIT_TYPE_PTR);
  b.derived(q, r, -8);
  GRJIT_BlockId b0 = b.block(), b1 = b.block(), b2 = b.block();
  b.poll_helper(reinterpret_cast<GRJIT_PollHelper>(0x10));
  b.at(b0);
  b.cnst(a, -5);
  b.bin(GRJIT_OP_ADD, c, V(a), I(7));
  b.cmp(GRJIT_CMP_ULT, a, V(a), V(c));
  b.load(c, p, -4, 16, true);
  b.store(p, 12, 32, V(c));
  b.call(c, reinterpret_cast<const void *>(0x1000), {V(a), I(2)});
  b.call_gc(GRJIT_NO_VREG, reinterpret_cast<const void *>(0x2000), {V(r)}, {3, 4},
      {grjit_frame_slot_vreg(r), grjit_frame_slot_dead()}, GRCORE_SITE_GC_POINT_ALLOC_SLOW);
  b.poll({5, 6}, {grjit_frame_slot_constant(-1), grjit_frame_slot_dead()});
  b.guard(V(a), {7, 8}, {grjit_frame_slot_dead(), grjit_frame_slot_vreg(c)});
  b.un(GRJIT_OP_NOT, c, V(c));
  b.mov(c, V(a));
  b.br_if(V(a), b1, b2);
  b.at(b1);
  b.br(b2);
  b.at(b2);
  b.ret(V(c));
  Fn f(b.finish());
  const char * want =
      "function shape(v0:ptr, v1:ref) slots=2\n"
      "  vregs: v2:i64 v3:i64 v4:ptr\n"
      "  derived v4 = v1-8\n"
      "b0:\n"
      "  v2 = const -5\n"
      "  v3 = add v2, #7\n"
      "  v2 = cmp.ult v2, v3\n"
      "  v3 = load_s.16 [v0-4]\n"
      "  store.32 [v0+12], v3\n"
      "  v3 = call.nogc 0x1000(v2, #2)\n"
      "  call.gc.alloc_slow 0x2000(v1) state fn=3 off=4 [v1, dead]\n"
      "  poll state fn=5 off=6 [#-1, dead]\n"
      "  guard v2 state fn=7 off=8 [dead, v3]\n"
      "  v3 = not v3\n"
      "  v3 = move v2\n"
      "  br_if v2, b1, b2\n"
      "b1:\n"
      "  br b2\n"
      "b2:\n"
      "  ret v3\n";
  EXPECT_EQ(print(f), want);
  EXPECT_EQ(print(f), print(f)); // deterministic
}

TEST(Print, ASmallBufferGetsATruncatedTerminatedTextAndTheFullLength) {
  B b("tiny");
  b.at(b.block());
  b.ret();
  Fn f(b.finish());
  size_t full = 0;
  ASSERT_EQ(grjit_function_print(f, nullptr, 0, &full), GRJIT_OK);
  char small[8];
  size_t n = 0;
  ASSERT_EQ(grjit_function_print(f, small, sizeof small, &n), GRJIT_OK);
  EXPECT_EQ(n, full);
  EXPECT_EQ(std::strlen(small), 7u);
  EXPECT_EQ(std::string(small), print(f).substr(0, 7));
  EXPECT_EQ(grjit_function_print(f, nullptr, 4, &n), GRJIT_ERR_INVALID);
  EXPECT_EQ(grjit_function_print(f, small, sizeof small, nullptr), GRJIT_ERR_INVALID);
}

GRJIT_TEST_MAIN()
