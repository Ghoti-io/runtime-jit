/**
 * @file
 *
 * The verifier: one test per way a function can be malformed, each with its
 * reason, and the well-formed controls that must pass.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include "test_helpers.h"

namespace {

GRJIT_Result verify(const GRJIT_Function * f, std::string * why,
    const GRJIT_Limits * limits = nullptr) {
  GRJIT_Result r;
  *why = verify_reason(f, &r, limits);
  return r;
}

/* A one-block function: `body` fills it. It must end in its own terminator. */
template <typename F>
std::pair<GRJIT_Result, std::string> check(F body, size_t slots = 0) {
  B b("t", slots);
  body(b);
  Fn f(b.finish());
  std::string why;
  GRJIT_Result r = verify(f, &why);
  return {r, why};
}

uint32_t poll_helper(void *, uint64_t, uint64_t) { return 0; }

} // namespace

TEST(Verify, AWellFormedFunctionPasses) {
  auto r = check([](B & b) {
    GRJIT_VReg x = b.param(GRJIT_TYPE_I64);
    GRJIT_VReg d = b.reg();
    b.at(b.block());
    b.bin(GRJIT_OP_ADD, d, V(x), I(1));
    b.ret(V(d));
  });
  EXPECT_EQ(r.first, GRJIT_OK) << r.second;
  EXPECT_EQ(r.second, "");
}

TEST(Verify, TheReasonIsWrittenOnlyWhenTheFunctionIsRefused) {
  B b("t");
  b.at(b.block());
  b.ret();
  Fn f(b.finish());
  char reason[64] = "untouched";
  EXPECT_EQ(grjit_function_verify(f, nullptr, reason, sizeof reason), GRJIT_OK);
  EXPECT_STREQ(reason, "untouched");
  EXPECT_EQ(grjit_function_verify(f, nullptr, nullptr, 0), GRJIT_OK);
}

TEST(Verify, AFunctionWithNoBlocksIsInvalid) {
  B b("t");
  Fn f(b.finish());
  std::string why;
  EXPECT_EQ(verify(f, &why), GRJIT_ERR_INVALID);
  EXPECT_NE(why.find("no blocks"), std::string::npos) << why;
}

TEST(Verify, ABlockWithNoTerminatorIsInvalid) {
  auto r = check([](B & b) {
    GRJIT_VReg d = b.reg();
    b.at(b.block());
    b.cnst(d, 1);
  });
  EXPECT_EQ(r.first, GRJIT_ERR_INVALID);
  EXPECT_NE(r.second.find("terminator"), std::string::npos) << r.second;
  auto empty = check([](B & b) { b.block(); });
  EXPECT_EQ(empty.first, GRJIT_ERR_INVALID);
  EXPECT_NE(empty.second.find("b0"), std::string::npos);
}

TEST(Verify, ATerminatorBeforeTheEndOfABlockIsInvalid) {
  auto r = check([](B & b) {
    GRJIT_VReg d = b.reg();
    b.at(b.block());
    b.ret();
    b.cnst(d, 1);
    b.ret();
  });
  EXPECT_EQ(r.first, GRJIT_ERR_INVALID);
  EXPECT_NE(r.second.find("before its end"), std::string::npos) << r.second;
}

TEST(Verify, ABranchToAMissingBlockIsInvalid) {
  auto r = check([](B & b) {
    b.at(b.block());
    b.br(5);
  });
  EXPECT_EQ(r.first, GRJIT_ERR_INVALID);
  EXPECT_NE(r.second.find("b5"), std::string::npos) << r.second;
  auto c = check([](B & b) {
    GRJIT_VReg v = b.reg();
    GRJIT_BlockId e = b.block();
    b.at(e);
    b.cnst(v, 1);
    b.br_if(V(v), e, 9);
  });
  EXPECT_EQ(c.first, GRJIT_ERR_INVALID);
}

TEST(Verify, ATypeMismatchIsInvalidAndNamesTheOperation) {
  auto ref_add = check([](B & b) {
    GRJIT_VReg r = b.param(GRJIT_TYPE_REF);
    GRJIT_VReg d = b.reg();
    b.at(b.block());
    b.bin(GRJIT_OP_ADD, d, V(r), I(1)); // ADD takes i64 or ptr
    b.ret(V(d));
  });
  EXPECT_EQ(ref_add.first, GRJIT_ERR_INVALID);
  EXPECT_NE(ref_add.second.find("v0"), std::string::npos) << ref_add.second;

  auto load_base = check([](B & b) {
    GRJIT_VReg x = b.param(GRJIT_TYPE_I64);
    GRJIT_VReg d = b.reg();
    b.at(b.block());
    b.load(d, x, 0, 64); // the base of a load is a ptr or a ref
    b.ret(V(d));
  });
  EXPECT_EQ(load_base.first, GRJIT_ERR_INVALID);

  auto move_type = check([](B & b) {
    GRJIT_VReg x = b.param(GRJIT_TYPE_I64);
    GRJIT_VReg d = b.reg(GRJIT_TYPE_REF);
    b.at(b.block());
    b.mov(d, V(x));
    b.ret();
  });
  EXPECT_EQ(move_type.first, GRJIT_ERR_INVALID);

  auto cmp_like = check([](B & b) {
    GRJIT_VReg x = b.param(GRJIT_TYPE_I64);
    GRJIT_VReg r = b.param(GRJIT_TYPE_REF);
    GRJIT_VReg d = b.reg();
    b.at(b.block());
    b.cmp(GRJIT_CMP_EQ, d, V(x), V(r)); // not like types
    b.ret(V(d));
  });
  EXPECT_EQ(cmp_like.first, GRJIT_ERR_INVALID);
  EXPECT_NE(cmp_like.second.find("like types"), std::string::npos) << cmp_like.second;

  auto ref_order = check([](B & b) {
    GRJIT_VReg r = b.param(GRJIT_TYPE_REF);
    GRJIT_VReg s = b.param(GRJIT_TYPE_REF);
    GRJIT_VReg d = b.reg();
    b.at(b.block());
    b.cmp(GRJIT_CMP_LT, d, V(r), V(s)); // refs compare for equality only
    b.ret(V(d));
  });
  EXPECT_EQ(ref_order.first, GRJIT_ERR_INVALID);

  auto ref_eq = check([](B & b) {
    GRJIT_VReg r = b.param(GRJIT_TYPE_REF);
    GRJIT_VReg s = b.param(GRJIT_TYPE_REF);
    GRJIT_VReg d = b.reg();
    b.at(b.block());
    b.cmp(GRJIT_CMP_EQ, d, V(r), V(s));
    b.ret(V(d));
  });
  EXPECT_EQ(ref_eq.first, GRJIT_OK) << ref_eq.second;

  auto bad_width = check([](B & b) {
    GRJIT_VReg p = b.param(GRJIT_TYPE_PTR);
    GRJIT_VReg d = b.reg();
    b.at(b.block());
    b.load(d, p, 0, 24);
    b.ret(V(d));
  });
  EXPECT_EQ(bad_width.first, GRJIT_ERR_INVALID);
}

TEST(Verify, AStoreOfAReferenceIsNotExpressible) {
  auto r = check([](B & b) {
    GRJIT_VReg p = b.param(GRJIT_TYPE_REF);
    GRJIT_VReg v = b.param(GRJIT_TYPE_REF);
    b.at(b.block());
    b.store(p, 8, 64, V(v));
    b.ret();
  });
  EXPECT_EQ(r.first, GRJIT_ERR_INVALID);
  EXPECT_NE(r.second.find("store of a reference"), std::string::npos) << r.second;
  auto raw = check([](B & b) {
    GRJIT_VReg p = b.param(GRJIT_TYPE_REF);
    GRJIT_VReg v = b.param(GRJIT_TYPE_I64);
    b.at(b.block());
    b.store(p, 8, 64, V(v)); // a raw word into a GC object is fine
    b.ret();
  });
  EXPECT_EQ(raw.first, GRJIT_OK) << raw.second;
}

TEST(Verify, AUseOfAPossiblyUnassignedRegisterNamesTheRegisterAndTheBlock) {
  auto r = check([](B & b) {
    GRJIT_VReg x = b.reg();
    GRJIT_VReg y = b.reg();
    b.at(b.block());
    b.bin(GRJIT_OP_ADD, y, V(x), I(1));
    b.ret(V(y));
  });
  EXPECT_EQ(r.first, GRJIT_ERR_INVALID);
  EXPECT_NE(r.second.find("v0"), std::string::npos) << r.second;
  EXPECT_NE(r.second.find("b0"), std::string::npos) << r.second;
  EXPECT_NE(r.second.find("assigned"), std::string::npos) << r.second;
}

TEST(Verify, ARegisterAssignedOnOnlyOnePathIsPossiblyUnassignedAtTheJoin) {
  auto r = check([](B & b) {
    GRJIT_VReg c = b.param(GRJIT_TYPE_I64);
    GRJIT_VReg x = b.reg();
    GRJIT_BlockId e = b.block(), a = b.block(), j = b.block();
    b.at(e);
    b.br_if(V(c), a, j);
    b.at(a);
    b.cnst(x, 1);
    b.br(j);
    b.at(j);
    b.ret(V(x));
  });
  EXPECT_EQ(r.first, GRJIT_ERR_INVALID);
  EXPECT_NE(r.second.find("b2"), std::string::npos) << r.second;
  // Assigned on both paths, it passes.
  auto ok = check([](B & b) {
    GRJIT_VReg c = b.param(GRJIT_TYPE_I64);
    GRJIT_VReg x = b.reg();
    GRJIT_BlockId e = b.block(), a = b.block(), o = b.block(), j = b.block();
    b.at(e);
    b.br_if(V(c), a, o);
    b.at(a);
    b.cnst(x, 1);
    b.br(j);
    b.at(o);
    b.cnst(x, 2);
    b.br(j);
    b.at(j);
    b.ret(V(x));
  });
  EXPECT_EQ(ok.first, GRJIT_OK) << ok.second;
}

TEST(Verify, ALoopAssignedBeforeTheLoopPasses) {
  auto ok = check([](B & b) {
    GRJIT_VReg n = b.param(GRJIT_TYPE_I64);
    GRJIT_VReg i = b.reg(), t = b.reg();
    GRJIT_BlockId e = b.block(), h = b.block(), body = b.block(), d = b.block();
    b.at(e);
    b.cnst(i, 0);
    b.br(h);
    b.at(h);
    b.cmp(GRJIT_CMP_LT, t, V(i), V(n));
    b.br_if(V(t), body, d);
    b.at(body);
    b.bin(GRJIT_OP_ADD, i, V(i), I(1));
    b.br(h);
    b.at(d);
    b.ret(V(i));
  });
  EXPECT_EQ(ok.first, GRJIT_OK) << ok.second;
  // The same loop with `i` assigned only inside it is not.
  auto bad = check([](B & b) {
    GRJIT_VReg n = b.param(GRJIT_TYPE_I64);
    GRJIT_VReg i = b.reg(), t = b.reg();
    GRJIT_BlockId e = b.block(), h = b.block(), body = b.block(), d = b.block();
    b.at(e);
    b.br(h);
    b.at(h);
    b.cmp(GRJIT_CMP_LT, t, V(i), V(n));
    b.br_if(V(t), body, d);
    b.at(body);
    b.cnst(i, 0);
    b.br(h);
    b.at(d);
    b.ret();
  });
  EXPECT_EQ(bad.first, GRJIT_ERR_INVALID);
}

TEST(Verify, AFrameStateOfTheWrongLengthIsInvalid) {
  auto r = check([](B & b) {
    GRJIT_VReg v = b.param(GRJIT_TYPE_I64);
    b.poll_helper(poll_helper);
    b.at(b.block());
    b.poll({1, 2}, {grjit_frame_slot_vreg(v)}); // the function has 2 slots
    b.ret();
  }, 2);
  EXPECT_EQ(r.first, GRJIT_ERR_INVALID);
  EXPECT_NE(r.second.find("interpreter slots"), std::string::npos) << r.second;
}

TEST(Verify, AFrameStateNamingARegisterTheFunctionLacksIsInvalid) {
  auto r = check([](B & b) {
    b.poll_helper(poll_helper);
    b.at(b.block());
    b.guard(I(1), {1, 2}, {grjit_frame_slot_vreg(40)});
    b.ret();
  }, 1);
  EXPECT_EQ(r.first, GRJIT_ERR_INVALID);
  EXPECT_NE(r.second.find("v40"), std::string::npos) << r.second;
}

TEST(Verify, AFrameStateNamingAnUnassignedRegisterIsAnUnassignedUse) {
  auto r = check([](B & b) {
    GRJIT_VReg v = b.reg();
    b.at(b.block());
    b.guard(I(1), {1, 2}, {grjit_frame_slot_vreg(v)});
    b.ret();
  }, 1);
  EXPECT_EQ(r.first, GRJIT_ERR_INVALID);
  EXPECT_NE(r.second.find("assigned"), std::string::npos) << r.second;
}

TEST(Verify, APollNeedsAHelperAndAGcPointCallNeedsAKind) {
  auto no_helper = check([](B & b) {
    b.at(b.block());
    b.poll({1, 2});
    b.ret();
  });
  EXPECT_EQ(no_helper.first, GRJIT_ERR_INVALID);
  EXPECT_NE(no_helper.second.find("poll helper"), std::string::npos) << no_helper.second;
  auto bad_kind = check([](B & b) {
    b.at(b.block());
    b.call_gc(GRJIT_NO_VREG, reinterpret_cast<const void *>(0x1000), {}, {1, 2}, {},
        GRCORE_SITE_GUARD);
    b.ret();
  });
  EXPECT_EQ(bad_kind.first, GRJIT_ERR_INVALID);
  EXPECT_NE(bad_kind.second.find("site kinds"), std::string::npos) << bad_kind.second;
}

TEST(Verify, ADerivedPointerWithADeadBaseIsInvalid) {
  // The derived pointer is live across the call; its base is not.
  auto bad = check([](B & b) {
    GRJIT_VReg base = b.reg(GRJIT_TYPE_REF);
    GRJIT_VReg der = b.reg(GRJIT_TYPE_PTR);
    GRJIT_VReg sink = b.reg();
    b.derived(der, base, 16);
    b.at(b.block());
    b.cnst(base, 0x1000);
    b.cnst(der, 0x1010);
    b.call_gc(GRJIT_NO_VREG, reinterpret_cast<const void *>(0x1000), {}, {1, 2}, {});
    b.load(sink, der, 0, 64); // der is used after the call, base is not
    b.ret(V(sink));
  });
  EXPECT_EQ(bad.first, GRJIT_ERR_INVALID);
  EXPECT_NE(bad.second.find("derived pointer v1"), std::string::npos) << bad.second;
  EXPECT_NE(bad.second.find("base v0"), std::string::npos) << bad.second;
  // Keep the base live too and it passes.
  auto ok = check([](B & b) {
    GRJIT_VReg base = b.reg(GRJIT_TYPE_REF);
    GRJIT_VReg der = b.reg(GRJIT_TYPE_PTR);
    GRJIT_VReg sink = b.reg();
    GRJIT_VReg again = b.reg(GRJIT_TYPE_REF);
    b.derived(der, base, 16);
    b.at(b.block());
    b.cnst(base, 0x1000);
    b.cnst(der, 0x1010);
    b.call_gc(GRJIT_NO_VREG, reinterpret_cast<const void *>(0x1000), {}, {1, 2}, {});
    b.load(sink, der, 0, 64);
    b.mov(again, V(base));
    b.ret(V(sink));
  });
  EXPECT_EQ(ok.first, GRJIT_OK) << ok.second;
}

TEST(Verify, ADerivedDeclarationMustBeAPtrOverARef) {
  auto not_ptr = check([](B & b) {
    GRJIT_VReg base = b.param(GRJIT_TYPE_REF);
    GRJIT_VReg der = b.reg(GRJIT_TYPE_I64);
    b.derived(der, base, 8);
    b.at(b.block());
    b.ret();
  });
  EXPECT_EQ(not_ptr.first, GRJIT_ERR_INVALID);
  auto not_ref = check([](B & b) {
    GRJIT_VReg base = b.param(GRJIT_TYPE_I64);
    GRJIT_VReg der = b.reg(GRJIT_TYPE_PTR);
    b.derived(der, base, 8);
    b.at(b.block());
    b.ret();
  });
  EXPECT_EQ(not_ref.first, GRJIT_ERR_INVALID);
}

TEST(Verify, ALimitAboveWhatTheBackendPassesInRegistersIsClampedToSix) {
  // Asking for eight arguments gets six: the builder refuses a seventh with
  // ERR_LIMIT, so the verifier never sees one it would have to refuse.
  GRJIT_Limits eight{};
  eight.max_call_arguments = 8;
  B b("t", 0, &eight);
  b.at(b.block());
  std::vector<GRJIT_Operand> args(7, I(1));
  EXPECT_EQ(grjit_builder_call(b.b, GRJIT_NO_VREG, 0x1000, GRJIT_CALL_NO_GC,
                GRCORE_SITE_GC_POINT_CALL, args.data(), 7, {0, 0}, nullptr, 0),
      GRJIT_ERR_LIMIT);
  EXPECT_EQ(grjit_builder_call(b.b, GRJIT_NO_VREG, 0x1000, GRJIT_CALL_NO_GC,
                GRCORE_SITE_GC_POINT_CALL, args.data(), 6, {0, 0}, nullptr, 0),
      GRJIT_OK);
}

TEST(Verify, TheCapsOfTheLimitsStructAreEnforcedAndChangeNothing) {
  B b("t");
  b.at(b.block());
  GRJIT_VReg v = b.reg();
  for (int i = 0; i < 10; i++) {
    b.cnst(v, i);
  }
  b.ret();
  Fn f(b.finish());
  std::string why;
  GRJIT_Limits ops{};
  ops.max_operations = 5;
  EXPECT_EQ(verify(f, &why, &ops), GRJIT_ERR_LIMIT);
  GRJIT_Limits blocks{};
  blocks.max_blocks = 1;
  EXPECT_EQ(verify(f, &why, &blocks), GRJIT_OK);
  B many("m");
  for (int i = 0; i < 3; i++) {
    many.at(many.block());
    many.ret();
  }
  Fn g(many.finish());
  EXPECT_EQ(verify(g, &why, &blocks), GRJIT_ERR_LIMIT);
  GRJIT_Limits regs{};
  regs.max_vregs = 0;
  EXPECT_EQ(verify(f, &why, &regs), GRJIT_OK);
  EXPECT_EQ(print(f), print(f)); // untouched
}

TEST(Verify, ACallToNowhereAndMissingOperandsAreInvalid) {
  auto null_addr = check([](B & b) {
    b.at(b.block());
    b.call(GRJIT_NO_VREG, nullptr);
    b.ret();
  });
  EXPECT_EQ(null_addr.first, GRJIT_ERR_INVALID);
  auto missing = check([](B & b) {
    GRJIT_VReg d = b.reg();
    b.at(b.block());
    b.bin(GRJIT_OP_ADD, d, NONE(), I(1));
    b.ret();
  });
  EXPECT_EQ(missing.first, GRJIT_ERR_INVALID);
  auto bad_dst = check([](B & b) {
    b.at(b.block());
    b.cnst(33, 1);
    b.ret();
  });
  EXPECT_EQ(bad_dst.first, GRJIT_ERR_INVALID);
}

GRJIT_TEST_MAIN()
