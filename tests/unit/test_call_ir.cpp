/**
 * @file
 *
 * The IR of calls between compiled functions (AD-28, CAP-1): the callable
 * attribute, the two call operations, the hooks, what the verifier refuses, and
 * the printer. Emission is `test_calls.cpp`'s; here the point is that the
 * function can be built, judged and printed.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include "test_helpers.h"
#include "../cross_meta.h"

namespace {

uint32_t h_push(void *, void *, uint64_t, const uint64_t *, uint64_t, uint64_t) { return 0; }
void h_pop(void *) {}
uint32_t h_compile(void *, uint64_t) { return 0; }
uint32_t h_deopt(void *, uint64_t) { return 0; }
uint32_t h_catch(void *, void *, uint64_t, uint32_t) { return 0; }

GRJIT_CallHooks all_hooks() {
  GRJIT_CallHooks h{};
  h.push = h_push;
  h.pop = h_pop;
  h.compile = h_compile;
  h.deopt = h_deopt;
  h.catch_exception = h_catch;
  return h;
}

const GRCORE_PollIdentity kId{7, 3};
const GRCORE_PollIdentity kExit{7, 2};
uint64_t g_entry_word = 0;

/* A callable function with one call through a slot; `tweak` may break it. */
template <typename F>
std::pair<GRJIT_Result, std::string> check_call(F tweak, size_t slots = 2) {
  B b("caller", slots);
  GRJIT_CallHooks h = all_hooks();
  GRJIT_VReg x = b.param(GRJIT_TYPE_I64);
  GRJIT_VReg r = b.reg(GRJIT_TYPE_REF);
  tweak(b, h, x, r);
  Fn f(b.finish());
  std::string why;
  GRJIT_Result res;
  why = verify_reason(f, &res);
  return {res, why};
}

/* The common body: one slot call over a two-slot frame state. */
void well_formed(B & b, GRJIT_CallHooks & h, GRJIT_VReg x, GRJIT_VReg r) {
  b.callable(h);
  b.at(b.block());
  b.call_slot(r, &g_entry_word, 99, {V(x), I(5)}, kId,
      {grjit_frame_slot_vreg(x), grjit_frame_slot_dead()}, kExit,
      {grjit_frame_slot_vreg(x), grjit_frame_slot_constant(5)});
  b.ret(V(r));
}

} // namespace

TEST(CallIr, ACallableFunctionWithACallThroughASlotIsWellFormed) {
  auto r = check_call(well_formed);
  EXPECT_EQ(r.first, GRJIT_OK) << r.second;
}

TEST(CallIr, TheBuilderRecordsTheAttributeTheHooksTheTokenAndBothStates) {
  B b("caller", 2);
  GRJIT_CallHooks h = all_hooks();
  GRJIT_VReg x = b.param(GRJIT_TYPE_I64);
  GRJIT_VReg r = b.reg(GRJIT_TYPE_REF);
  EXPECT_EQ(grjit_function_callable(nullptr), false);
  well_formed(b, h, x, r);
  Fn f(b.finish());
  EXPECT_TRUE(grjit_function_callable(f));
  const GRJIT_CallHooks * got = grjit_function_call_hooks(f);
  ASSERT_NE(got, nullptr);
  EXPECT_EQ(got->push, h_push);
  EXPECT_EQ(got->pop, h_pop);
  EXPECT_EQ(got->compile, h_compile);
  EXPECT_EQ(got->deopt, h_deopt);
  size_t n = 0;
  const GRJIT_Op * ops = grjit_function_block_ops(f, 0, &n);
  ASSERT_EQ(n, 2u);
  EXPECT_EQ(ops[0].kind, GRJIT_OP_CALL_SLOT);
  EXPECT_EQ(ops[0].address, reinterpret_cast<uintptr_t>(&g_entry_word));
  EXPECT_EQ(ops[0].callee, 99u);
  EXPECT_EQ(ops[0].arg_count, 2u);
  EXPECT_EQ(ops[0].dst, r);
  ASSERT_NE(ops[0].state, GRJIT_NO_STATE);
  ASSERT_NE(ops[0].exit_state, GRJIT_NO_STATE);
  EXPECT_NE(ops[0].state, ops[0].exit_state);
  const GRJIT_FrameState * st = grjit_function_frame_state(f, ops[0].state);
  const GRJIT_FrameState * ex = grjit_function_frame_state(f, ops[0].exit_state);
  EXPECT_EQ(st->identity.offset, 3u);
  EXPECT_EQ(ex->identity.offset, 2u);
  EXPECT_EQ(st->slots[1].kind, GRJIT_FRAME_SLOT_DEAD);
  EXPECT_EQ(ex->slots[1].kind, GRJIT_FRAME_SLOT_CONSTANT);
  EXPECT_EQ(grjit_function_frame_state_count(f), 2u);
  EXPECT_EQ(grjit_function_call_hooks(nullptr), nullptr);
}

TEST(CallIr, SlotAndPointerCallsRecordTheExplicitCalleeEnvironment) {
  for (bool through_pointer : {false, true}) {
    B b("call-environment", 2);
    GRJIT_CallHooks h = all_hooks();
    const GRJIT_VReg x = b.param(GRJIT_TYPE_I64);
    const GRJIT_VReg environment = b.param(GRJIT_TYPE_PTR);
    const GRJIT_VReg target = b.reg(GRJIT_TYPE_PTR);
    const GRJIT_VReg result = b.reg();
    b.callable(h);
    b.at(b.block());
    if (through_pointer) b.cnst(target, 0x1000);
    const GRJIT_Operand args[] = {V(x)};
    const GRJIT_FrameSlot state[] = {grjit_frame_slot_vreg(x), grjit_frame_slot_dead()};
    const GRCORE_PollIdentity id{7, 3};
    GRJIT_Result r = through_pointer
        ? grjit_builder_call_ptr_results_env(b.b, &result, 1, V(target), 99,
            V(environment), args, 1, id, state, 2, id, state, 2)
        : grjit_builder_call_slot_results_env(b.b, &result, 1,
            reinterpret_cast<uintptr_t>(&g_entry_word), 99, V(environment),
            args, 1, id, state, 2, id, state, 2);
    ASSERT_EQ(r, GRJIT_OK);
    Fn f(b.finish());
    size_t count = 0;
    const GRJIT_Op * ops = grjit_function_block_ops(f, 0, &count);
    ASSERT_EQ(count, through_pointer ? 2u : 1u);
    const GRJIT_Op & call = ops[count - 1];
    EXPECT_EQ(call.kind, through_pointer ? GRJIT_OP_CALL_PTR : GRJIT_OP_CALL_SLOT);
    EXPECT_EQ(call.environment.kind, GRJIT_OPERAND_VREG);
    EXPECT_EQ(call.environment.vreg, environment);
  }
}

TEST(CallIr, CatchingCallsRecordExplicitEnvironmentAndLandingBlock) {
  for (int kind = 0; kind != 3; ++kind) {
    B b("catch-call-environment");
    GRJIT_CallHooks h = all_hooks();
    const GRJIT_VReg environment = b.param(GRJIT_TYPE_PTR);
    const GRJIT_VReg slot_entry = b.param(GRJIT_TYPE_PTR);
    const GRJIT_VReg target = b.param(GRJIT_TYPE_PTR);
    b.callable(h, 0);
    const auto body = b.block();
    const auto landing = b.block();
    b.at(body);
    const GRCORE_PollIdentity id{4, 2};
    GRJIT_Result r;
    if (kind == 0) {
      r = grjit_builder_call_slot_catch_results_env(b.b, nullptr, 0,
          reinterpret_cast<uintptr_t>(&g_entry_word), 99, V(environment),
          nullptr, 0, id, nullptr, 0, id, nullptr, 0, landing);
    } else if (kind == 1) {
      r = grjit_builder_call_slot_operand_catch_results_env(b.b, nullptr, 0,
          V(slot_entry), 99, V(environment), nullptr, 0, id, nullptr, 0,
          id, nullptr, 0, landing);
    } else {
      r = grjit_builder_call_ptr_catch_results_env(b.b, nullptr, 0,
          V(target), 99, V(environment), nullptr, 0, id, nullptr, 0,
          id, nullptr, 0, landing);
    }
    ASSERT_EQ(r, GRJIT_OK);
    ASSERT_EQ(grjit_builder_ret(b.b, grjit_operand_none()), GRJIT_OK);
    b.at(landing);
    ASSERT_EQ(grjit_builder_ret(b.b, grjit_operand_none()), GRJIT_OK);
    Fn f(b.finish());
    std::string why;
    GRJIT_Result verified;
    why = verify_reason(f, &verified);
    ASSERT_EQ(verified, GRJIT_OK) << why;
    size_t count = 0;
    const GRJIT_Op * ops = grjit_function_block_ops(f, body, &count);
    ASSERT_EQ(count, 2u);
    const GRJIT_Op & call = ops[0];
    EXPECT_EQ(call.landing_target, landing);
    EXPECT_EQ(call.environment.kind, GRJIT_OPERAND_VREG);
    EXPECT_EQ(call.environment.vreg, environment);
    EXPECT_EQ(call.kind, kind == 2 ? GRJIT_OP_CALL_PTR : GRJIT_OP_CALL_SLOT);
    if (kind == 1) {
      EXPECT_EQ(call.address, 0u);
      EXPECT_EQ(call.a.kind, GRJIT_OPERAND_VREG);
      EXPECT_EQ(call.a.vreg, slot_entry);
    }
  }
}

TEST(CallIr, CatchLandingCannotReadARegisterUnassignedBeforeTheCall) {
  B b("catch-unassigned-register");
  GRJIT_CallHooks h = all_hooks();
  b.callable(h);
  const auto body = b.block();
  const auto landing = b.block();
  const GRJIT_VReg unassigned = b.reg(GRJIT_TYPE_I64);
  b.at(body);
  const GRCORE_PollIdentity id{5, 1};
  ASSERT_EQ(grjit_builder_call_slot_catch_results(b.b, nullptr, 0,
      reinterpret_cast<uintptr_t>(&g_entry_word), 99, nullptr, 0, id,
      nullptr, 0, id, nullptr, 0, landing), GRJIT_OK);
  ASSERT_EQ(grjit_builder_ret(b.b, grjit_operand_none()), GRJIT_OK);
  b.at(landing);
  ASSERT_EQ(grjit_builder_ret(b.b, V(unassigned)), GRJIT_OK);
  Fn f(b.finish());
  std::string why;
  GRJIT_Result verified;
  why = verify_reason(f, &verified);
  EXPECT_EQ(verified, GRJIT_ERR_INVALID) << "catch landing used an unassigned vreg; " << why;
}

TEST(CallIr, CatchLandingCannotReadCallResults) {
  B b("catch-call-result-unassigned");
  GRJIT_CallHooks h = all_hooks();
  b.callable(h);
  const auto body = b.block();
  const auto landing = b.block();
  const GRJIT_VReg result = b.reg(GRJIT_TYPE_I64);
  b.at(body);
  const GRCORE_PollIdentity id{5, 2};
  ASSERT_EQ(grjit_builder_call_slot_catch_results(b.b, &result, 1,
      reinterpret_cast<uintptr_t>(&g_entry_word), 99, nullptr, 0, id,
      nullptr, 0, id, nullptr, 0, landing), GRJIT_OK);
  ASSERT_EQ(grjit_builder_ret(b.b, V(result)), GRJIT_OK);
  b.at(landing);
  ASSERT_EQ(grjit_builder_ret(b.b, V(result)), GRJIT_OK);
  Fn f(b.finish());
  std::string why;
  GRJIT_Result verified;
  why = verify_reason(f, &verified);
  EXPECT_EQ(verified, GRJIT_ERR_INVALID) << "catch landing used a call result; " << why;
}

uint64_t gc_helper(uint64_t) { return 0; }

/* A catch call whose result register is one its landing block reads: the exception edge
 * leaves before the call's results are written, so the landing reads the value the register
 * held before the call, and that value is live from the function's entry until the call.
 * Liveness applied the landing's live-in before the call's definition cleared it, so the
 * value was live before nothing: an earlier collection point left it out of its stack map
 * and a collection there moved or freed the object the landing then returned. */
TEST(CallIr, ARegisterTheCatchLandingReadsIsLiveAcrossAnEarlierCollectionPointEvenWhenTheCatchCallDefinesIt) {
  for (GRJIT_Arch arch : {GRJIT_ARCH_X86_64, GRJIT_ARCH_X86_64_WIN64, GRJIT_ARCH_ARM64}) {
    // The helper call and the catch call in one block, and the helper call in the block before the one
    // that has the catch call (the register is then live into that block, which only the block-level
    // sweep can say).
    for (bool split : {false, true}) {
      SCOPED_TRACE(xm::arch_name(arch));
      SCOPED_TRACE(split ? "helper call and catch call in two blocks" : "one block");
      B b("catch-live-across", 0);
      GRJIT_CallHooks h = all_hooks();
      b.callable(h);
      const GRJIT_VReg r = b.param(GRJIT_TYPE_REF);
      const GRJIT_VReg helper_result = b.reg(GRJIT_TYPE_I64);
      const auto body = b.block();
      const auto catching = split ? b.block() : body;
      const auto landing = b.block();
      b.at(body);
      b.call_gc(helper_result, reinterpret_cast<const void *>(gc_helper), {I(1)}, {3, 1}, {});
      if (split) {
        ASSERT_EQ(grjit_builder_br(b.b, catching), GRJIT_OK);
        b.at(catching);
      }
      const GRCORE_PollIdentity id{3, 2};
      ASSERT_EQ(grjit_builder_call_slot_catch_results(b.b, &r, 1,
          reinterpret_cast<uintptr_t>(&g_entry_word), 99, nullptr, 0, id, nullptr, 0, id, nullptr, 0,
          landing), GRJIT_OK);
      ASSERT_EQ(grjit_builder_ret(b.b, V(r)), GRJIT_OK);
      b.at(landing);
      ASSERT_EQ(grjit_builder_ret(b.b, V(r)), GRJIT_OK);
      Fn f(b.finish());
      GRJIT_Result verified;
      const std::string why = verify_reason(f, &verified);
      ASSERT_EQ(verified, GRJIT_OK) << why;
      xm::Emit em(arch, f.f);
      ASSERT_TRUE(em.ok()) << grjit_result_string(em.result);
      const GRCORE_CodeMeta * meta = em.meta();
      size_t helper_sites = 0;
      int64_t slot_of_r = 0;
      for (size_t i = 0; i < meta->site_count; i++) {
        const GRCORE_CodeSite & s = meta->sites[i];
        if (s.kind == GRCORE_SITE_GC_POINT_CALL && helper_sites == 0) {
          helper_sites++;
          ASSERT_EQ(s.live_count, 1u) << "r is live across the helper call: the landing reads it";
          EXPECT_EQ(s.live[0].slot_kind, GRCORE_SLOT_VALUE);
          slot_of_r = s.live[0].value;
        }
      }
      EXPECT_EQ(helper_sites, 1u);
      // The catch call's own sites carry r too (the push and the call itself), in the same slot.
      bool in_catch_sites = false;
      for (size_t i = 0; i < meta->site_count; i++) {
        const GRCORE_CodeSite & s = meta->sites[i];
        if (s.kind == GRCORE_SITE_GC_POINT_FRAME_PUSH || (s.kind == GRCORE_SITE_GC_POINT_CALL && i > 0)) {
          in_catch_sites = true;
          bool has = false;
          for (size_t k = 0; k < s.live_count; k++) has = has || s.live[k].value == slot_of_r;
          EXPECT_TRUE(has) << "site " << i << " of kind " << s.kind;
        }
      }
      EXPECT_TRUE(in_catch_sites);
    }
  }
}

TEST(CallIr, AFunctionThatIsNotCallableHasNoHooksAndMayNotCallCompiledCode) {
  auto r = check_call([](B & b, GRJIT_CallHooks &, GRJIT_VReg x, GRJIT_VReg r) {
    b.at(b.block());
    b.call_slot(r, &g_entry_word, 1, {V(x)}, kId,
        {grjit_frame_slot_vreg(x), grjit_frame_slot_dead()}, kExit,
        {grjit_frame_slot_vreg(x), grjit_frame_slot_dead()});
    b.ret(V(r));
  });
  EXPECT_EQ(r.first, GRJIT_ERR_INVALID);
  EXPECT_NE(r.second.find("not callable"), std::string::npos) << r.second;
  B plain("plain");
  plain.at(plain.block());
  plain.ret();
  Fn f(plain.finish());
  EXPECT_FALSE(grjit_function_callable(f));
  EXPECT_EQ(grjit_function_call_hooks(f), nullptr);
}

TEST(CallIr, EachHookACallNeedsIsRequired) {
  struct Case {
    const char * name;
    std::function<void(GRJIT_CallHooks &)> drop;
    GRJIT_Result expect;
    const char * why;
  };
  const Case cases[] = {
      {"no deopt hook", [](GRJIT_CallHooks & h) { h.deopt = nullptr; },
          GRJIT_ERR_INVALID, "deopt hook"},
      {"no push hook", [](GRJIT_CallHooks & h) { h.push = nullptr; },
          GRJIT_ERR_INVALID, "push"},
      {"no pop hook", [](GRJIT_CallHooks & h) { h.pop = nullptr; },
          GRJIT_ERR_INVALID, "pop"},
      {"no compile hook for a call through a slot",
          [](GRJIT_CallHooks & h) { h.compile = nullptr; }, GRJIT_ERR_INVALID,
          "compile"},
  };
  for (const Case & c : cases) {
    SCOPED_TRACE(c.name);
    auto r = check_call([&](B & b, GRJIT_CallHooks & h, GRJIT_VReg x, GRJIT_VReg r) {
      c.drop(h);
      well_formed(b, h, x, r);
    });
    EXPECT_EQ(r.first, c.expect);
    EXPECT_NE(r.second.find(c.why), std::string::npos) << r.second;
  }
  // A call through a code pointer has no use for compile.
  auto ok = check_call([](B & b, GRJIT_CallHooks & h, GRJIT_VReg x, GRJIT_VReg r) {
    h.compile = nullptr;
    b.callable(h);
    b.at(b.block());
    b.call_ptr(r, I(0x1000), 3, {V(x)}, kId,
        {grjit_frame_slot_vreg(x), grjit_frame_slot_dead()}, kExit,
        {grjit_frame_slot_vreg(x), grjit_frame_slot_dead()});
    b.ret(V(r));
  });
  EXPECT_EQ(ok.first, GRJIT_OK) << ok.second;
}

TEST(CallIr, ACallThroughACodePointerNeedsAPtrRegisterOrANonNullImmediate) {
  struct Case {
    const char * name;
    std::function<GRJIT_Operand(B &)> target;
    GRJIT_Result expect;
  };
  const Case cases[] = {
      {"an i64 register", [](B & b) { return V(b.reg(GRJIT_TYPE_I64)); },
          GRJIT_ERR_INVALID},
      {"a ref register", [](B & b) { return V(b.reg(GRJIT_TYPE_REF)); },
          GRJIT_ERR_INVALID},
      {"a null immediate", [](B &) { return I(0); }, GRJIT_ERR_INVALID},
      {"no target", [](B &) { return NONE(); }, GRJIT_ERR_INVALID},
      {"a ptr register", [](B & b) { return V(b.reg(GRJIT_TYPE_PTR)); },
          GRJIT_OK},
      {"a non-null immediate", [](B &) { return I(0x4000); }, GRJIT_OK},
  };
  for (const Case & c : cases) {
    SCOPED_TRACE(c.name);
    B b("caller", 2);
    GRJIT_CallHooks h = all_hooks();
    GRJIT_VReg x = b.param(GRJIT_TYPE_I64);
    GRJIT_VReg r = b.reg();
    GRJIT_Operand target = c.target(b);
    b.callable(h);
    b.at(b.block());
    if (target.kind == GRJIT_OPERAND_VREG) {
      // The register is assigned first, or it is not a use-before-assignment
      // that is being judged.
      b.cnst(target.vreg, 1);
    }
    b.call_ptr(r, target, 3, {V(x)}, kId,
        {grjit_frame_slot_vreg(x), grjit_frame_slot_dead()}, kExit,
        {grjit_frame_slot_vreg(x), grjit_frame_slot_dead()});
    b.ret(V(r));
    Fn f(b.finish());
    GRJIT_Result res;
    std::string why = verify_reason(f, &res);
    EXPECT_EQ(res, c.expect) << why;
  }
}

TEST(CallIr, ASlotCallNeedsTheSlotAddressAndACodePointerCallHasNone) {
  B a("a", 2);
  GRJIT_CallHooks h = all_hooks();
  GRJIT_VReg x = a.param(GRJIT_TYPE_I64);
  GRJIT_VReg r = a.reg();
  a.callable(h);
  a.at(a.block());
  EXPECT_EQ(grjit_builder_call_slot(a.b, r, 0, 1, nullptr, 0, kId, nullptr, 0, kExit,
                nullptr, 0),
      GRJIT_OK);
  a.ret(V(r));
  Fn fa(a.finish());
  GRJIT_Result res;
  std::string why = verify_reason(fa, &res);
  EXPECT_EQ(res, GRJIT_ERR_INVALID) << why;
  (void)x;
}

TEST(CallIr, BothFrameStatesMustHaveTheFunctionsSlotCountAndRegistersThatExist) {
  auto wrong_exit = check_call([](B & b, GRJIT_CallHooks & h, GRJIT_VReg x, GRJIT_VReg r) {
    b.callable(h);
    b.at(b.block());
    b.call_slot(r, &g_entry_word, 1, {V(x)}, kId,
        {grjit_frame_slot_vreg(x), grjit_frame_slot_dead()}, kExit,
        {grjit_frame_slot_vreg(x)}); // one slot, the function has two
    b.ret(V(r));
  });
  EXPECT_EQ(wrong_exit.first, GRJIT_ERR_INVALID);
  EXPECT_NE(wrong_exit.second.find("frame state"), std::string::npos) << wrong_exit.second;
  auto wrong_state = check_call([](B & b, GRJIT_CallHooks & h, GRJIT_VReg x, GRJIT_VReg r) {
    b.callable(h);
    b.at(b.block());
    b.call_slot(r, &g_entry_word, 1, {V(x)}, kId,
        {grjit_frame_slot_vreg(x), grjit_frame_slot_dead(), grjit_frame_slot_dead()}, kExit,
        {grjit_frame_slot_vreg(x), grjit_frame_slot_dead()});
    b.ret(V(r));
  });
  EXPECT_EQ(wrong_state.first, GRJIT_ERR_INVALID);
  auto ghost = check_call([](B & b, GRJIT_CallHooks & h, GRJIT_VReg x, GRJIT_VReg r) {
    b.callable(h);
    b.at(b.block());
    b.call_slot(r, &g_entry_word, 1, {V(x)}, kId,
        {grjit_frame_slot_vreg(x), grjit_frame_slot_dead()}, kExit,
        {grjit_frame_slot_vreg(77), grjit_frame_slot_dead()});
    b.ret(V(r));
  });
  EXPECT_EQ(ghost.first, GRJIT_ERR_INVALID);
  EXPECT_NE(ghost.second.find("v77"), std::string::npos) << ghost.second;
}

TEST(CallIr, SixteenArgumentsAreAllowedAndSeventeenAreNot) {
  for (size_t n : {size_t{0}, size_t{1}, size_t{6}, size_t{7}, size_t{16}}) {
    SCOPED_TRACE(n);
    B b("caller", 0);
    GRJIT_CallHooks h = all_hooks();
    GRJIT_VReg r = b.reg();
    b.callable(h);
    b.at(b.block());
    std::vector<GRJIT_Operand> args(n, I(1));
    EXPECT_EQ(grjit_builder_call_slot(b.b, r, reinterpret_cast<uintptr_t>(&g_entry_word), 1,
                  args.data(), n, kId, nullptr, 0, kExit, nullptr, 0),
        GRJIT_OK);
    b.ret(V(r));
    Fn f(b.finish());
    GRJIT_Result res;
    std::string why = verify_reason(f, &res);
    EXPECT_EQ(res, GRJIT_OK) << why;
  }
  B b("caller", 0);
  GRJIT_CallHooks h = all_hooks();
  b.callable(h);
  b.at(b.block());
  std::vector<GRJIT_Operand> args(17, I(1));
  EXPECT_EQ(grjit_builder_call_slot(b.b, GRJIT_NO_VREG,
                reinterpret_cast<uintptr_t>(&g_entry_word), 1, args.data(), 17, kId,
                nullptr, 0, kExit, nullptr, 0),
      GRJIT_ERR_LIMIT);
}

TEST(CallIr, ALowerGuestCallLimitIsEnforcedByTheBuilderAndTheVerifier) {
  GRJIT_Limits limits{};
  limits.max_guest_call_arguments = 3;
  B b("caller", 0, &limits);
  GRJIT_CallHooks h = all_hooks();
  b.callable(h);
  b.at(b.block());
  std::vector<GRJIT_Operand> four(4, I(1));
  EXPECT_EQ(grjit_builder_call_slot(b.b, GRJIT_NO_VREG,
                reinterpret_cast<uintptr_t>(&g_entry_word), 1, four.data(), 4, kId,
                nullptr, 0, kExit, nullptr, 0),
      GRJIT_ERR_LIMIT);
  // A function built with the default and verified against a lower one.
  B c("caller", 0);
  c.callable(h);
  c.at(c.block());
  EXPECT_EQ(grjit_builder_call_slot(c.b, GRJIT_NO_VREG,
                reinterpret_cast<uintptr_t>(&g_entry_word), 1, four.data(), 4, kId,
                nullptr, 0, kExit, nullptr, 0),
      GRJIT_OK);
  c.ret();
  Fn f(c.finish());
  GRJIT_Result res;
  std::string why = verify_reason(f, &res, &limits);
  EXPECT_EQ(res, GRJIT_ERR_LIMIT) << why;
}

TEST(CallIr, ACallableFunctionMayHaveNoMoreParametersThanTheCallLimit) {
  B b("wide", 0);
  GRJIT_CallHooks h = all_hooks();
  for (int i = 0; i < 17; i++) {
    b.param(GRJIT_TYPE_I64);
  }
  b.callable(h);
  b.at(b.block());
  b.ret();
  Fn f(b.finish());
  GRJIT_Result res;
  std::string why = verify_reason(f, &res);
  EXPECT_EQ(res, GRJIT_ERR_LIMIT) << why;
}

TEST(CallIr, TheCallsResultMayBeAnyTypeOrNone) {
  for (GRJIT_Type t : {GRJIT_TYPE_I64, GRJIT_TYPE_REF, GRJIT_TYPE_PTR}) {
    SCOPED_TRACE(t);
    B b("caller", 1);
    GRJIT_CallHooks h = all_hooks();
    GRJIT_VReg x = b.param(GRJIT_TYPE_I64);
    GRJIT_VReg r = b.reg(t);
    b.callable(h);
    b.at(b.block());
    b.call_slot(r, &g_entry_word, 1, {V(x)}, kId, {grjit_frame_slot_vreg(x)}, kExit,
        {grjit_frame_slot_vreg(x)});
    b.ret(V(r));
    Fn f(b.finish());
    GRJIT_Result res;
    std::string why = verify_reason(f, &res);
    EXPECT_EQ(res, GRJIT_OK) << why;
  }
  B b("caller", 1);
  GRJIT_CallHooks h = all_hooks();
  GRJIT_VReg x = b.param(GRJIT_TYPE_I64);
  b.callable(h, 0);
  b.at(b.block());
  b.call_slot(GRJIT_NO_VREG, &g_entry_word, 1, {V(x)}, kId, {grjit_frame_slot_vreg(x)},
      kExit, {grjit_frame_slot_vreg(x)});
  b.ret();
  Fn f(b.finish());
  GRJIT_Result res;
  std::string why = verify_reason(f, &res);
  EXPECT_EQ(res, GRJIT_OK) << why;
}

TEST(CallIr, AnArgumentMustHaveBeenAssignedLikeAnyOtherUse) {
  auto r = check_call([](B & b, GRJIT_CallHooks & h, GRJIT_VReg x, GRJIT_VReg r) {
    GRJIT_VReg later = b.reg();
    b.callable(h);
    b.at(b.block());
    b.call_slot(r, &g_entry_word, 1, {V(later)}, kId,
        {grjit_frame_slot_vreg(x), grjit_frame_slot_dead()}, kExit,
        {grjit_frame_slot_vreg(x), grjit_frame_slot_dead()});
    b.ret(V(r));
  });
  EXPECT_EQ(r.first, GRJIT_ERR_INVALID);
  EXPECT_NE(r.second.find("assigned"), std::string::npos) << r.second;
}

TEST(CallIr, ACodePointerThatNothingAssignedIsRefusedAsAnyUnassignedUse) {
  auto r = check_call([](B & b, GRJIT_CallHooks & h, GRJIT_VReg x, GRJIT_VReg r) {
    GRJIT_VReg p = b.reg(GRJIT_TYPE_PTR);
    b.callable(h);
    b.at(b.block());
    b.call_ptr(r, V(p), 1, {V(x)}, kId, {grjit_frame_slot_vreg(x), grjit_frame_slot_dead()},
        kExit, {grjit_frame_slot_vreg(x), grjit_frame_slot_dead()});
    b.ret(V(r));
  });
  EXPECT_EQ(r.first, GRJIT_ERR_INVALID);
  EXPECT_NE(r.second.find("assigned"), std::string::npos) << r.second;
  auto ok = check_call([](B & b, GRJIT_CallHooks & h, GRJIT_VReg x, GRJIT_VReg r) {
    GRJIT_VReg p = b.reg(GRJIT_TYPE_PTR);
    b.callable(h);
    b.at(b.block());
    b.cnst(p, 0x1000);
    b.call_ptr(r, V(p), 1, {V(x)}, kId, {grjit_frame_slot_vreg(x), grjit_frame_slot_dead()},
        kExit, {grjit_frame_slot_vreg(x), grjit_frame_slot_dead()});
    b.ret(V(r));
  });
  EXPECT_EQ(ok.first, GRJIT_OK) << ok.second;
}

TEST(CallIr, TheExitStatesRegistersAreUsesToo) {
  // A register that only the exit state names must be assigned before the call.
  auto r = check_call([](B & b, GRJIT_CallHooks & h, GRJIT_VReg x, GRJIT_VReg r) {
    GRJIT_VReg later = b.reg();
    b.callable(h);
    b.at(b.block());
    b.call_slot(r, &g_entry_word, 1, {V(x)}, kId,
        {grjit_frame_slot_vreg(x), grjit_frame_slot_dead()}, kExit,
        {grjit_frame_slot_vreg(later), grjit_frame_slot_dead()});
    b.ret(V(r));
  });
  EXPECT_EQ(r.first, GRJIT_ERR_INVALID);
}

TEST(CallIr, APushedDerivedPointerNeedsItsBaseLiveAtThePushToo) {
  // A derived pointer passed as an argument is live at the push, where the
  // collector may move its base; the verifier's rule for every site holds.
  B b("caller", 1);
  GRJIT_CallHooks h = all_hooks();
  GRJIT_VReg base = b.param(GRJIT_TYPE_REF);
  GRJIT_VReg d = b.reg(GRJIT_TYPE_PTR);
  GRJIT_VReg r = b.reg();
  b.derived(d, base, 16);
  b.callable(h);
  b.at(b.block());
  b.bitcast(d, base);
  b.bin(GRJIT_OP_ADD, d, V(d), I(16));
  // The base is not used after this point, and the derived pointer is.
  b.call_slot(r, &g_entry_word, 1, {V(d)}, kId, {grjit_frame_slot_dead()}, kExit,
      {grjit_frame_slot_dead()});
  b.ret(V(r));
  Fn f(b.finish());
  GRJIT_Result res;
  std::string why = verify_reason(f, &res);
  // `d` is live at the push and `base` is not: refused.
  EXPECT_EQ(res, GRJIT_ERR_INVALID) << why;
  EXPECT_NE(why.find("derived pointer"), std::string::npos) << why;
}

TEST(CallIr, ThePrinterShowsBothCallsTheirTokensAndBothStates) {
  B b("caller", 2);
  GRJIT_CallHooks h = all_hooks();
  GRJIT_VReg x = b.param(GRJIT_TYPE_I64);
  GRJIT_VReg p = b.reg(GRJIT_TYPE_PTR);
  GRJIT_VReg r = b.reg(GRJIT_TYPE_REF);
  b.callable(h);
  b.at(b.block());
  b.cnst(p, 0x1000);
  b.call_slot(r, reinterpret_cast<void *>(0x2000), 5, {V(x), I(9)}, kId,
      {grjit_frame_slot_vreg(x), grjit_frame_slot_dead()}, kExit,
      {grjit_frame_slot_vreg(x), grjit_frame_slot_constant(7)});
  b.call_ptr(r, V(p), 6, {}, kId, {grjit_frame_slot_dead(), grjit_frame_slot_dead()},
      kExit, {grjit_frame_slot_dead(), grjit_frame_slot_dead()});
  b.ret(V(r));
  Fn f(b.finish());
  std::string text = print(f);
  EXPECT_NE(text.find("function caller(v0:i64) slots=2 callable"), std::string::npos) << text;
  EXPECT_NE(text.find("v2 = call.slot 0x2000 callee=5(v0, #9) state fn=7 off=3 [v0, dead] "
                      "exit state fn=7 off=2 [v0, #7]"),
      std::string::npos) << text;
  EXPECT_NE(text.find("v2 = call.ptr v1 callee=6() state"), std::string::npos) << text;
  // And a function that is not callable prints as before.
  B plain("plain", 0);
  plain.at(plain.block());
  plain.ret();
  Fn pf(plain.finish());
  EXPECT_EQ(print(pf).find("callable"), std::string::npos);
}

GRJIT_TEST_MAIN()
