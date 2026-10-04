/**
 * @file
 *
 * Guards: one that holds falls through and changes nothing; one that fails
 * leaves through its deopt exit with exactly its frame state's slots and the
 * offset of its site, which `grcore_codemeta_find` turns into the record.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include "test_helpers.h"

#include "../ir_eval.h"

namespace {

/* f(a, b): v = a * 3; w = b + 1; guard(a != 0) state [v, w, const 77, dead]; return v + w. */
GRJIT_Function * build(bool with_guard) {
  B b("g", 4);
  GRJIT_VReg a = b.param(GRJIT_TYPE_I64);
  GRJIT_VReg bb = b.param(GRJIT_TYPE_I64);
  GRJIT_VReg v = b.reg(), w = b.reg(), c = b.reg(), r = b.reg();
  b.at(b.block());
  b.bin(GRJIT_OP_MUL, v, V(a), I(3));
  b.bin(GRJIT_OP_ADD, w, V(bb), I(1));
  b.cmp(GRJIT_CMP_NE, c, V(a), I(0));
  if (with_guard) {
    b.guard(V(c), {5, 99},
        {grjit_frame_slot_vreg(v), grjit_frame_slot_vreg(w),
            grjit_frame_slot_constant(77), grjit_frame_slot_dead()});
  }
  b.bin(GRJIT_OP_ADD, r, V(v), V(w));
  b.ret(V(r));
  return b.finish();
}

} // namespace

TEST(Guard, AGuardThatHoldsFallsThroughAndTheResultIsAsWithoutIt) {
  GRJIT_REQUIRE_BACKEND();
  JitWorld w;
  Fn guarded(build(true)), plain(build(false));
  Compiled g(guarded, w.pages()), p(plain, w.pages());
  ASSERT_TRUE(g);
  ASSERT_TRUE(p);
  auto with = g.run(w.ctx, {5, 10});
  auto without = p.run(w.ctx, {5, 10});
  EXPECT_EQ(with.exit, static_cast<uint32_t>(GRJIT_EXIT_RETURNED));
  EXPECT_EQ(with.out[0], without.out[0]);
  EXPECT_EQ(with.out[0], 15u + 11u);
}

TEST(Guard, AGuardThatFailsReturnsTheFrameStateAndTheSiteOffset) {
  GRJIT_REQUIRE_BACKEND();
  JitWorld w;
  Fn f(build(true));
  Compiled c(f, w.pages());
  ASSERT_TRUE(c);
  ASSERT_EQ(grjit_code_out_words(c.code), 5u); // 4 slots and the site offset
  auto r = c.run(w.ctx, {0, 10});
  ASSERT_EQ(r.exit, static_cast<uint32_t>(GRJIT_EXIT_DEOPT));
  EXPECT_EQ(r.out[0], 0u);   // v = 0 * 3
  EXPECT_EQ(r.out[1], 11u);  // w = 10 + 1
  EXPECT_EQ(r.out[2], 77u);  // the constant
  EXPECT_EQ(r.out[3], 0u);   // dead slots are zero
  const GRCORE_CodeSite * site =
      grcore_codemeta_find(grjit_code_meta(c.code), static_cast<uint32_t>(r.out[4]));
  ASSERT_NE(site, nullptr);
  EXPECT_EQ(site->kind, GRCORE_SITE_GUARD);
  EXPECT_EQ(site->identity.function, 5u);
  EXPECT_EQ(site->identity.offset, 99u);
  ASSERT_EQ(site->frame_state_count, 4u);
  EXPECT_EQ(site->frame_state[0].kind, GRCORE_LOC_FRAME_SLOT);
  EXPECT_EQ(site->frame_state[1].kind, GRCORE_LOC_FRAME_SLOT);
  EXPECT_EQ(site->frame_state[2].kind, GRCORE_LOC_CONSTANT);
  EXPECT_EQ(site->frame_state[2].value, 77);
  EXPECT_EQ(site->frame_state[3].kind, GRCORE_LOC_DEAD);
  // The same function, the evaluator's view.
  ireval::Result e = ireval::eval(f, w.ctx, nullptr, {0, 10});
  EXPECT_EQ(e.exit, static_cast<uint32_t>(GRJIT_EXIT_DEOPT));
  EXPECT_EQ(e.guard_identity.function, 5u);
  for (size_t i = 0; i < 4; i++) {
    EXPECT_EQ(e.out[i], r.out[i]) << i;
  }
}

TEST(Guard, TheFailingGuardSitesOffsetIsTheStartOfItsExitStub) {
  GRJIT_REQUIRE_BACKEND();
  JitWorld w;
  Fn f(build(true));
  Compiled c(f, w.pages());
  ASSERT_TRUE(c);
  auto r = c.run(w.ctx, {0, 0});
  uint32_t off = static_cast<uint32_t>(r.out[4]);
  const unsigned char * code = static_cast<const unsigned char *>(grjit_code_address(c.code));
  ASSERT_LT(off, grjit_code_size(c.code));
  // The stub begins by loading `out` from its frame slot into rdx: mov rdx,[rbp-16].
  const unsigned char want[] = {0x48, 0x8B, 0x55, 0xF0};
  EXPECT_EQ(std::memcmp(code + off, want, sizeof want), 0);
}

TEST(Guard, ManyGuardsEachHaveTheirOwnSiteAndExit) {
  GRJIT_REQUIRE_BACKEND();
  JitWorld w;
  B b("many", 1);
  GRJIT_VReg x = b.param(GRJIT_TYPE_I64);
  GRJIT_VReg c = b.reg();
  b.at(b.block());
  for (int i = 0; i < 5; i++) {
    b.cmp(GRJIT_CMP_NE, c, V(x), I(i));
    b.guard(V(c), {1, static_cast<uint64_t>(10 + i)}, {grjit_frame_slot_constant(i)});
  }
  b.ret(V(x));
  Fn f(b.finish());
  Compiled code(f, w.pages());
  ASSERT_TRUE(code);
  for (int i = 0; i < 5; i++) {
    auto r = code.run(w.ctx, {static_cast<uint64_t>(i)});
    ASSERT_EQ(r.exit, static_cast<uint32_t>(GRJIT_EXIT_DEOPT));
    EXPECT_EQ(r.out[0], static_cast<uint64_t>(i));
    const GRCORE_CodeSite * s =
        grcore_codemeta_find(grjit_code_meta(code.code), static_cast<uint32_t>(r.out[1]));
    ASSERT_NE(s, nullptr);
    EXPECT_EQ(s->identity.offset, static_cast<uint64_t>(10 + i));
  }
  EXPECT_EQ(code.run(w.ctx, {99}).exit, static_cast<uint32_t>(GRJIT_EXIT_RETURNED));
}

GRJIT_TEST_MAIN()
