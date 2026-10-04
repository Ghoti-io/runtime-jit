/**
 * @file
 *
 * The IR builder: what it records, what it refuses, and that a failure
 * changes nothing.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include "test_helpers.h"

TEST(Builder, RecordsBlocksRegistersAndOperationsAndTheAccessorsReadThemBack) {
  B b("sum", 2);
  GRJIT_VReg p0 = b.param(GRJIT_TYPE_PTR);
  GRJIT_VReg p1 = b.param(GRJIT_TYPE_REF);
  GRJIT_VReg r = b.reg(GRJIT_TYPE_I64);
  GRJIT_BlockId entry = b.block();
  GRJIT_BlockId other = b.block();
  b.at(entry);
  b.cnst(r, 42);
  b.br(other);
  b.at(other);
  b.ret(V(r));
  Fn f(b.finish());
  EXPECT_STREQ(grjit_function_name(f), "sum");
  EXPECT_EQ(grjit_function_param_count(f), 2u);
  EXPECT_EQ(grjit_function_interp_slot_count(f), 2u);
  EXPECT_EQ(grjit_function_vreg_count(f), 3u);
  EXPECT_EQ(grjit_function_vreg_type(f, p0), GRJIT_TYPE_PTR);
  EXPECT_EQ(grjit_function_vreg_type(f, p1), GRJIT_TYPE_REF);
  EXPECT_EQ(grjit_function_vreg_type(f, r), GRJIT_TYPE_I64);
  EXPECT_EQ(grjit_function_vreg_type(f, 99), GRJIT_TYPE_COUNT);
  EXPECT_EQ(grjit_function_block_count(f), 2u);
  EXPECT_EQ(grjit_function_op_count(f), 3u);
  size_t n = 0;
  const GRJIT_Op * ops = grjit_function_block_ops(f, entry, &n);
  ASSERT_EQ(n, 2u);
  EXPECT_EQ(ops[0].kind, GRJIT_OP_CONST);
  EXPECT_EQ(ops[0].dst, r);
  EXPECT_EQ(ops[0].a.imm, 42);
  EXPECT_EQ(ops[1].kind, GRJIT_OP_BR);
  EXPECT_EQ(ops[1].target, other);
  EXPECT_EQ(grjit_function_block_ops(f, 7, &n), nullptr);
}

TEST(Builder, AnUnnamedFunctionIsNamedFunction) {
  B b(nullptr);
  b.at(b.block());
  b.ret();
  Fn f(b.finish());
  EXPECT_STREQ(grjit_function_name(f), "function");
}

TEST(Builder, ParametersAreTheFirstRegistersOnly) {
  B b("f");
  b.param(GRJIT_TYPE_I64);
  b.reg();
  GRJIT_VReg v = 77;
  EXPECT_EQ(grjit_builder_param(b.b, GRJIT_TYPE_I64, &v), GRJIT_ERR_INVALID);
  EXPECT_EQ(v, 77u); // written only on success
  EXPECT_EQ(grjit_builder_vreg(b.b, static_cast<GRJIT_Type>(9), &v), GRJIT_ERR_INVALID);
  EXPECT_EQ(grjit_builder_vreg(b.b, GRJIT_TYPE_I64, nullptr), GRJIT_ERR_INVALID);
}

TEST(Builder, ADerivedPointerIsRecordedWithItsBaseAndDelta) {
  B b("f");
  GRJIT_VReg base = b.param(GRJIT_TYPE_REF);
  GRJIT_VReg d = b.reg(GRJIT_TYPE_PTR);
  b.derived(d, base, 24);
  EXPECT_EQ(grjit_builder_derived(b.b, d, 99, 0), GRJIT_ERR_INVALID);
  b.at(b.block());
  b.ret();
  Fn f(b.finish());
  GRJIT_VReg got_base = 0;
  int64_t delta = 0;
  EXPECT_TRUE(grjit_function_vreg_derived(f, d, &got_base, &delta));
  EXPECT_EQ(got_base, base);
  EXPECT_EQ(delta, 24);
  EXPECT_FALSE(grjit_function_vreg_derived(f, base, nullptr, nullptr));
}

TEST(Builder, AnOperationNeedsACurrentBlock) {
  B b("f");
  GRJIT_VReg v = b.reg();
  EXPECT_EQ(grjit_builder_const(b.b, v, 1), GRJIT_ERR_INVALID);
  b.block();
  EXPECT_EQ(grjit_builder_const(b.b, v, 1), GRJIT_ERR_INVALID); // not current yet
  EXPECT_EQ(grjit_builder_set_block(b.b, 5), GRJIT_ERR_INVALID);
  b.at(0);
  EXPECT_EQ(grjit_builder_const(b.b, v, 1), GRJIT_OK);
}

TEST(Builder, TheBinaryAndUnaryEntryPointsRefuseOtherKinds) {
  B b("f");
  GRJIT_VReg v = b.reg();
  b.at(b.block());
  EXPECT_EQ(grjit_builder_binary(b.b, GRJIT_OP_NEG, v, V(v), V(v)), GRJIT_ERR_INVALID);
  EXPECT_EQ(grjit_builder_binary(b.b, GRJIT_OP_CMP, v, V(v), V(v)), GRJIT_ERR_INVALID);
  EXPECT_EQ(grjit_builder_unary(b.b, GRJIT_OP_ADD, v, V(v)), GRJIT_ERR_INVALID);
  EXPECT_EQ(grjit_builder_unary(b.b, GRJIT_OP_NOT, v, V(v)), GRJIT_OK);
}

TEST(Builder, FourThousandNinetySevenBlocksIsALimitErrorAndChangesNothing) {
  B b("f");
  for (int i = 0; i < 4096; i++) {
    b.block();
  }
  GRJIT_BlockId id = 123;
  EXPECT_EQ(grjit_builder_block(b.b, &id), GRJIT_ERR_LIMIT);
  EXPECT_EQ(id, 123u);
  b.at(0);
  b.ret();
  Fn f(b.finish());
  EXPECT_EQ(grjit_function_block_count(f), 4096u);
}

TEST(Builder, TheOtherCapsAreLimitErrorsToo) {
  GRJIT_Limits limits{};
  limits.max_vregs = 3;
  limits.max_operations = 2;
  limits.max_frame_state_slots = 2;
  B b("f", 0, &limits);
  GRJIT_VReg v;
  for (int i = 0; i < 3; i++) {
    ASSERT_EQ(grjit_builder_vreg(b.b, GRJIT_TYPE_I64, &v), GRJIT_OK);
  }
  EXPECT_EQ(grjit_builder_vreg(b.b, GRJIT_TYPE_I64, &v), GRJIT_ERR_LIMIT);
  b.at(b.block());
  b.cnst(0, 1);
  b.cnst(0, 2);
  EXPECT_EQ(grjit_builder_const(b.b, 0, 3), GRJIT_ERR_LIMIT);
  GRJIT_FrameSlot slots[3] = {grjit_frame_slot_dead(), grjit_frame_slot_dead(), grjit_frame_slot_dead()};
  EXPECT_EQ(grjit_builder_poll(b.b, {0, 0}, slots, 3), GRJIT_ERR_LIMIT);
  GRJIT_Builder * small = nullptr;
  EXPECT_EQ(grjit_builder_create("h", 3, &limits, nullptr, &small), GRJIT_ERR_LIMIT);
  EXPECT_EQ(small, nullptr);
}

TEST(Builder, ACallWithSevenArgumentsIsALimitErrorAndSixIsFine) {
  B b("f");
  b.at(b.block());
  std::vector<GRJIT_Operand> args(7, I(1));
  EXPECT_EQ(grjit_builder_call(b.b, GRJIT_NO_VREG, 0x1000, GRJIT_CALL_NO_GC,
                GRCORE_SITE_GC_POINT_CALL, args.data(), 7, {0, 0}, nullptr, 0),
      GRJIT_ERR_LIMIT);
  EXPECT_EQ(grjit_builder_call(b.b, GRJIT_NO_VREG, 0x1000, GRJIT_CALL_NO_GC,
                GRCORE_SITE_GC_POINT_CALL, args.data(), 6, {0, 0}, nullptr, 0),
      GRJIT_OK);
  EXPECT_EQ(grjit_builder_call(b.b, GRJIT_NO_VREG, 0x1000, static_cast<GRJIT_CallAttr>(5),
                GRCORE_SITE_GC_POINT_CALL, args.data(), 0, {0, 0}, nullptr, 0),
      GRJIT_ERR_INVALID);
  EXPECT_EQ(grjit_builder_call(b.b, GRJIT_NO_VREG, 0x1000, GRJIT_CALL_NO_GC,
                GRCORE_SITE_GC_POINT_CALL, nullptr, 2, {0, 0}, nullptr, 0),
      GRJIT_ERR_INVALID);
}

TEST(Builder, AFrameStateIsCopiedSoTheCallersArrayCanChange) {
  B b("f", 2);
  GRJIT_VReg v = b.reg();
  b.at(b.block());
  GRJIT_FrameSlot slots[2] = {grjit_frame_slot_vreg(v), grjit_frame_slot_constant(9)};
  b.poll({4, 8}, {slots[0], slots[1]});
  std::vector<GRJIT_Operand> args = {V(v)};
  b.call_gc(GRJIT_NO_VREG, reinterpret_cast<const void *>(0x1000), args, {5, 6},
      {grjit_frame_slot_dead(), grjit_frame_slot_dead()});
  b.ret();
  slots[0] = grjit_frame_slot_dead();
  args[0] = I(0);
  Fn f(b.finish());
  ASSERT_EQ(grjit_function_frame_state_count(f), 2u);
  const GRJIT_FrameState * s = grjit_function_frame_state(f, 0);
  ASSERT_NE(s, nullptr);
  EXPECT_EQ(s->identity.function, 4u);
  EXPECT_EQ(s->identity.offset, 8u);
  EXPECT_EQ(s->slots[0].kind, GRJIT_FRAME_SLOT_VREG);
  EXPECT_EQ(s->slots[1].constant, 9);
  size_t n;
  const GRJIT_Op * ops = grjit_function_block_ops(f, 0, &n);
  EXPECT_EQ(ops[1].args[0].kind, GRJIT_OPERAND_VREG);
  EXPECT_EQ(grjit_function_frame_state(f, 2), nullptr);
}

TEST(Builder, FinishHandsOverTheFunctionAndWritesItOnlyOnSuccess) {
  GRJIT_Function * out = reinterpret_cast<GRJIT_Function *>(1);
  EXPECT_EQ(grjit_builder_finish(nullptr, &out), GRJIT_ERR_INVALID);
  B b("f");
  EXPECT_EQ(grjit_builder_finish(b.b, nullptr), GRJIT_ERR_INVALID);
  EXPECT_EQ(out, reinterpret_cast<GRJIT_Function *>(1));
  grjit_builder_destroy(nullptr);   // ignored
  grjit_function_destroy(nullptr);  // ignored
}

TEST(Builder, AnAllocationFailureAtEveryPointLeavesTheBuilderAsItWasAndLeaksNothing) {
  for (long fail = 1;; fail++) {
    TrackingAllocator t;
    t.fail_at = fail;
    GRJIT_Builder * b = nullptr;
    GRJIT_Result r = grjit_builder_create("sweep", 2, nullptr, t.get(), &b);
    bool created = r == GRJIT_OK;
    if (!created) {
      ASSERT_EQ(r, GRJIT_ERR_OOM);
      ASSERT_EQ(b, nullptr);
      ASSERT_EQ(t.live, 0);
      continue;
    }
    // Each step is retried until it succeeds; a failed one must change
    // nothing, which the final shape checks.
    t.fail_at = fail;
    auto retry = [&](auto step) {
      for (int tries = 0; tries < 3; tries++) {
        GRJIT_Result s = step();
        if (s == GRJIT_OK) {
          return;
        }
        ASSERT_EQ(s, GRJIT_ERR_OOM);
        t.fail_at = 0; // the one failure is spent
      }
    };
    GRJIT_VReg a = 0, c = 0;
    GRJIT_BlockId blk = 0;
    retry([&] { return grjit_builder_param(b, GRJIT_TYPE_I64, &a); });
    retry([&] { return grjit_builder_vreg(b, GRJIT_TYPE_REF, &c); });
    retry([&] { return grjit_builder_block(b, &blk); });
    retry([&] { return grjit_builder_set_block(b, blk); });
    GRJIT_FrameSlot slots[2] = {grjit_frame_slot_vreg(c), grjit_frame_slot_dead()};
    retry([&] { return grjit_builder_poll(b, {1, 2}, slots, 2); });
    GRJIT_Operand args[2] = {grjit_operand_vreg(a), grjit_operand_imm(3)};
    retry([&] {
      return grjit_builder_call(b, a, 0x2000, GRJIT_CALL_GC_POINT,
          GRCORE_SITE_GC_POINT_CALL, args, 2, {1, 3}, slots, 2);
    });
    retry([&] { return grjit_builder_ret(b, grjit_operand_vreg(a)); });
    GRJIT_Function * f = nullptr;
    ASSERT_EQ(grjit_builder_finish(b, &f), GRJIT_OK);
    EXPECT_EQ(grjit_function_op_count(f), 3u) << fail;
    EXPECT_EQ(grjit_function_frame_state_count(f), 2u) << fail;
    EXPECT_EQ(grjit_function_vreg_count(f), 2u) << fail;
    grjit_function_destroy(f);
    EXPECT_EQ(t.live, 0) << fail;
    if (t.calls < fail) {
      break; // the sweep has run past the last allocation
    }
  }
}

GRJIT_TEST_MAIN()
