/**
 * @file
 *
 * The IR of tail calls (AD-28, CAP-8): the two operations, the `tail` hook, what
 * the builder records, what the verifier refuses, the printer, the liveness
 * sites, and the refusal of every backend until the one that has them. Emission
 * is `test_tail.cpp`'s.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include "test_helpers.h"

#include "../../src/backend/liveness_internal.h"
#include "../../src/code/code_internal.h"

namespace {

uint32_t h_push(void *, uint64_t, const uint64_t *, uint64_t) { return 0; }
void h_pop(void *) {}
uint32_t h_compile(void *, uint64_t) { return 0; }
uint32_t h_deopt(void *, uint64_t) { return 0; }
uint32_t h_tail(void *, uint64_t, const uint64_t *, uint64_t) { return 0; }

GRJIT_CallHooks all_hooks() {
  GRJIT_CallHooks h{};
  h.push = h_push;
  h.pop = h_pop;
  h.compile = h_compile;
  h.deopt = h_deopt;
  h.tail = h_tail;
  return h;
}

const GRCORE_PollIdentity kId{7, 3};
uint64_t g_entry_word = 0;

/* A callable function of one I64 parameter, a REF and a PTR register, with its
 * body supplied; verified and the reason returned. */
template <typename F>
std::pair<GRJIT_Result, std::string> check(F body, size_t slots = 2) {
  B b("caller", slots);
  GRJIT_CallHooks h = all_hooks();
  GRJIT_VReg x = b.param(GRJIT_TYPE_I64);
  GRJIT_VReg r = b.reg(GRJIT_TYPE_REF);
  GRJIT_VReg p = b.reg(GRJIT_TYPE_PTR);
  body(b, h, x, r, p);
  Fn f(b.finish());
  GRJIT_Result res;
  std::string why = verify_reason(f, &res);
  return {res, why};
}

std::vector<GRJIT_FrameSlot> st2(GRJIT_VReg x) {
  return {grjit_frame_slot_vreg(x), grjit_frame_slot_dead()};
}

void tail_slot(B & b, GRJIT_CallHooks & h, GRJIT_VReg x, GRJIT_VReg, GRJIT_VReg) {
  b.callable(h);
  b.at(b.block());
  b.tail_call_slot(&g_entry_word, 99, {V(x), I(5)}, kId, st2(x));
}

} // namespace

TEST(TailIr, ATailCallThroughASlotEndsItsBlockAndIsWellFormed) {
  auto r = check(tail_slot);
  EXPECT_EQ(r.first, GRJIT_OK) << r.second;
}

TEST(TailIr, ATailCallThroughAPointerTakesAPtrRegisterOrANonNullImmediate) {
  auto reg = check([](B & b, GRJIT_CallHooks & h, GRJIT_VReg x, GRJIT_VReg, GRJIT_VReg p) {
    b.callable(h);
    b.at(b.block());
    b.cnst(p, 0x1000);
    b.tail_call_ptr(V(p), 99, {V(x)}, kId, st2(x));
  });
  EXPECT_EQ(reg.first, GRJIT_OK) << reg.second;
  auto imm = check([](B & b, GRJIT_CallHooks & h, GRJIT_VReg x, GRJIT_VReg, GRJIT_VReg) {
    b.callable(h);
    b.at(b.block());
    b.tail_call_ptr(I(0x1000), 99, {V(x)}, kId, st2(x));
  });
  EXPECT_EQ(imm.first, GRJIT_OK) << imm.second;
}

TEST(TailIr, TheBuilderRecordsTheKindTheTargetTheTokenTheArgumentsAndOneState) {
  B b("caller", 2);
  GRJIT_CallHooks h = all_hooks();
  GRJIT_VReg x = b.param(GRJIT_TYPE_I64);
  GRJIT_VReg p = b.reg(GRJIT_TYPE_PTR);
  b.callable(h);
  GRJIT_BlockId b0 = b.block();
  GRJIT_BlockId b1 = b.block();
  b.at(b0);
  b.cnst(p, 0x1000);
  b.br_if(V(x), b1, b1);
  b.at(b1);
  b.tail_call_ptr(V(p), 77, {V(x), I(9)}, kId, {grjit_frame_slot_vreg(x), grjit_frame_slot_constant(4)});
  Fn f(b.finish());
  EXPECT_EQ(grjit_function_call_hooks(f)->tail, h_tail);
  size_t n = 0;
  const GRJIT_Op * ops = grjit_function_block_ops(f, b1, &n);
  ASSERT_EQ(n, 1u);
  EXPECT_EQ(ops[0].kind, GRJIT_OP_TAIL_CALL_PTR);
  EXPECT_EQ(ops[0].a.kind, GRJIT_OPERAND_VREG);
  EXPECT_EQ(ops[0].callee, 77u);
  EXPECT_EQ(ops[0].arg_count, 2u);
  EXPECT_EQ(ops[0].dst, GRJIT_NO_VREG);
  EXPECT_NE(ops[0].state, GRJIT_NO_STATE);
  EXPECT_EQ(ops[0].exit_state, GRJIT_NO_STATE);
  EXPECT_EQ(grjit_function_frame_state_count(f), 1u);
  const GRJIT_FrameState * st = grjit_function_frame_state(f, ops[0].state);
  EXPECT_EQ(st->identity.offset, 3u);
  EXPECT_EQ(st->slots[1].kind, GRJIT_FRAME_SLOT_CONSTANT);
  EXPECT_EQ(grjit_liveness_site_count(&ops[0]), 2u);
}

TEST(TailIr, ATailCallIsRefusedAnywhereButLastInItsBlockNamingTheOperation) {
  auto r = check([](B & b, GRJIT_CallHooks & h, GRJIT_VReg x, GRJIT_VReg, GRJIT_VReg) {
    b.callable(h);
    b.at(b.block());
    b.tail_call_slot(&g_entry_word, 99, {V(x)}, kId, st2(x));
    b.ret(V(x)); // after a terminator
  });
  EXPECT_EQ(r.first, GRJIT_ERR_INVALID);
  EXPECT_NE(r.second.find("terminator before its end (op 0)"), std::string::npos) << r.second;
  // The same call, last, with an operation before it, is the control.
  auto ok = check([](B & b, GRJIT_CallHooks & h, GRJIT_VReg x, GRJIT_VReg, GRJIT_VReg) {
    b.callable(h);
    b.at(b.block());
    b.bin(GRJIT_OP_ADD, x, V(x), I(1));
    b.tail_call_slot(&g_entry_word, 99, {V(x)}, kId, st2(x));
  });
  EXPECT_EQ(ok.first, GRJIT_OK) << ok.second;
  // A block that ends in a tail call needs no return, and a block without a
  // terminator is still refused.
  auto none = check([](B & b, GRJIT_CallHooks & h, GRJIT_VReg x, GRJIT_VReg, GRJIT_VReg) {
    b.callable(h);
    b.at(b.block());
    b.bin(GRJIT_OP_ADD, x, V(x), I(1));
  });
  EXPECT_EQ(none.first, GRJIT_ERR_INVALID) << none.second;
}

TEST(TailIr, EachHookATailCallNeedsIsRequiredAndNamedWithTheOperation) {
  struct Case {
    const char * name;
    std::function<void(GRJIT_CallHooks &)> drop;
    bool slot;
    GRJIT_Result expect;
    const char * why;
  };
  const Case cases[] = {
      {"no tail hook, slot", [](GRJIT_CallHooks & h) { h.tail = nullptr; }, true,
          GRJIT_ERR_INVALID, "tail"},
      {"no tail hook, pointer", [](GRJIT_CallHooks & h) { h.tail = nullptr; }, false,
          GRJIT_ERR_INVALID, "tail"},
      {"no compile hook, slot", [](GRJIT_CallHooks & h) { h.compile = nullptr; }, true,
          GRJIT_ERR_INVALID, "compile"},
      {"no deopt hook", [](GRJIT_CallHooks & h) { h.deopt = nullptr; }, true,
          GRJIT_ERR_INVALID, "deopt"},
  };
  for (const Case & c : cases) {
    SCOPED_TRACE(c.name);
    auto r = check([&](B & b, GRJIT_CallHooks & h, GRJIT_VReg x, GRJIT_VReg, GRJIT_VReg) {
      c.drop(h);
      b.callable(h);
      b.at(b.block());
      if (c.slot) {
        b.tail_call_slot(&g_entry_word, 1, {V(x)}, kId, st2(x));
      } else {
        b.tail_call_ptr(I(0x1000), 1, {V(x)}, kId, st2(x));
      }
    });
    EXPECT_EQ(r.first, c.expect) << r.second;
    EXPECT_NE(r.second.find(c.why), std::string::npos) << r.second;
  }
  // The controls: a pointer tail call needs neither compile, push nor pop.
  auto r = check([](B & b, GRJIT_CallHooks & h, GRJIT_VReg x, GRJIT_VReg, GRJIT_VReg) {
    h.compile = nullptr;
    h.push = nullptr;
    h.pop = nullptr;
    b.callable(h);
    b.at(b.block());
    b.tail_call_ptr(I(0x1000), 1, {V(x)}, kId, st2(x));
  });
  EXPECT_EQ(r.first, GRJIT_OK) << r.second;
}

TEST(TailIr, ATailCallNeedsACallableFunction) {
  auto r = check([](B & b, GRJIT_CallHooks &, GRJIT_VReg x, GRJIT_VReg, GRJIT_VReg) {
    b.at(b.block());
    b.tail_call_slot(&g_entry_word, 99, {V(x)}, kId, st2(x));
  });
  EXPECT_EQ(r.first, GRJIT_ERR_INVALID);
  EXPECT_NE(r.second.find("a tail call"), std::string::npos) << r.second;
  EXPECT_NE(r.second.find("not callable"), std::string::npos) << r.second;
}

TEST(TailIr, SixteenArgumentsAreAllowedSeventeenAreALimitErrorAndTheVerifierHoldsTheLimitItIsGiven) {
  std::vector<GRJIT_Operand> sixteen(16, I(1));
  auto ok = check([&](B & b, GRJIT_CallHooks & h, GRJIT_VReg x, GRJIT_VReg, GRJIT_VReg) {
    b.callable(h);
    b.at(b.block());
    b.tail_call_slot(&g_entry_word, 1, sixteen, kId, st2(x));
  });
  EXPECT_EQ(ok.first, GRJIT_OK) << ok.second;
  // The builder refuses a seventeenth.
  B b("caller", 2);
  GRJIT_CallHooks h = all_hooks();
  GRJIT_VReg x = b.param(GRJIT_TYPE_I64);
  b.callable(h);
  b.at(b.block());
  std::vector<GRJIT_Operand> seventeen(17, I(1));
  auto fs = st2(x);
  EXPECT_EQ(grjit_builder_tail_call_slot(b.b, reinterpret_cast<uintptr_t>(&g_entry_word), 1,
                seventeen.data(), seventeen.size(), kId, fs.data(), fs.size()),
      GRJIT_ERR_LIMIT);
  // And the verifier, judging a function by a lower limit than it was built
  // under, refuses it, naming the operation and the count.
  std::vector<GRJIT_Operand> nine(9, I(1));
  B b2("caller", 2);
  GRJIT_VReg y = b2.param(GRJIT_TYPE_I64);
  b2.callable(h);
  b2.at(b2.block());
  b2.tail_call_slot(&g_entry_word, 1, nine, kId, st2(y));
  Fn f(b2.finish());
  GRJIT_Limits lower{};
  grjit_limits_default(&lower);
  lower.max_guest_call_arguments = 8;
  GRJIT_Result res;
  std::string why = verify_reason(f, &res, &lower);
  EXPECT_EQ(res, GRJIT_ERR_LIMIT) << why;
  EXPECT_NE(why.find("tail call has 9 arguments, at most 8"), std::string::npos) << why;
  lower.max_guest_call_arguments = 9;
  EXPECT_EQ(grjit_function_verify(f, &lower, nullptr, 0), GRJIT_OK);
}

TEST(TailIr, ACodePointerMustBeAPtrRegisterOrANonNullImmediate) {
  struct Case {
    const char * name;
    std::function<GRJIT_Operand(GRJIT_VReg, GRJIT_VReg, GRJIT_VReg)> target;
  };
  const Case cases[] = {
      {"an I64 register", [](GRJIT_VReg x, GRJIT_VReg, GRJIT_VReg) { return V(x); }},
      {"a REF register", [](GRJIT_VReg, GRJIT_VReg r, GRJIT_VReg) { return V(r); }},
      {"a null immediate", [](GRJIT_VReg, GRJIT_VReg, GRJIT_VReg) { return I(0); }},
      {"no target", [](GRJIT_VReg, GRJIT_VReg, GRJIT_VReg) { return NONE(); }},
  };
  for (const Case & c : cases) {
    SCOPED_TRACE(c.name);
    auto r = check([&](B & b, GRJIT_CallHooks & h, GRJIT_VReg x, GRJIT_VReg r, GRJIT_VReg p) {
      b.callable(h);
      b.at(b.block());
      b.cnst(r, 0);
      b.tail_call_ptr(c.target(x, r, p), 1, {V(x)}, kId, st2(x));
    });
    EXPECT_EQ(r.first, GRJIT_ERR_INVALID) << r.second;
    EXPECT_NE(r.second.find("code pointer"), std::string::npos) << r.second;
  }
}

TEST(TailIr, AFrameStateOfTheWrongLengthOrNamingAMissingRegisterIsRefused) {
  auto shortstate = check([](B & b, GRJIT_CallHooks & h, GRJIT_VReg x, GRJIT_VReg, GRJIT_VReg) {
    b.callable(h);
    b.at(b.block());
    b.tail_call_slot(&g_entry_word, 1, {V(x)}, kId, {grjit_frame_slot_vreg(x)});
  });
  EXPECT_EQ(shortstate.first, GRJIT_ERR_INVALID);
  EXPECT_NE(shortstate.second.find("interpreter slots"), std::string::npos) << shortstate.second;
  auto missing = check([](B & b, GRJIT_CallHooks & h, GRJIT_VReg x, GRJIT_VReg, GRJIT_VReg) {
    b.callable(h);
    b.at(b.block());
    b.tail_call_slot(&g_entry_word, 1, {V(x)}, kId, {grjit_frame_slot_vreg(77), grjit_frame_slot_dead()});
  });
  EXPECT_EQ(missing.first, GRJIT_ERR_INVALID) << missing.second;
  auto argument = check([](B & b, GRJIT_CallHooks & h, GRJIT_VReg x, GRJIT_VReg, GRJIT_VReg) {
    b.callable(h);
    b.at(b.block());
    b.tail_call_slot(&g_entry_word, 1, {V(77)}, kId, st2(x));
  });
  EXPECT_EQ(argument.first, GRJIT_ERR_INVALID) << argument.second;
  // An argument that nothing assigned is refused as for any use.
  auto unassigned = check([](B & b, GRJIT_CallHooks & h, GRJIT_VReg x, GRJIT_VReg, GRJIT_VReg) {
    GRJIT_VReg later = b.reg();
    b.callable(h);
    b.at(b.block());
    b.tail_call_slot(&g_entry_word, 1, {V(later)}, kId, st2(x));
  });
  EXPECT_EQ(unassigned.first, GRJIT_ERR_INVALID);
  EXPECT_NE(unassigned.second.find("assigned"), std::string::npos) << unassigned.second;
}

TEST(TailIr, AnAddressForASlotAndNoneForAPointerAreRequiredOfTheOperation) {
  // The builder takes the address of a slot and a target as separate
  // functions, so the verifier's rule is shown on a function built the one way
  // and judged as the other: a slot call with no address.
  auto r = check([](B & b, GRJIT_CallHooks & h, GRJIT_VReg x, GRJIT_VReg, GRJIT_VReg) {
    b.callable(h);
    b.at(b.block());
    EXPECT_EQ(grjit_builder_tail_call_slot(b.b, 0, 1, nullptr, 0, kId,
                  st2(x).data(), 2),
        GRJIT_OK);
  });
  EXPECT_EQ(r.first, GRJIT_ERR_INVALID);
  EXPECT_NE(r.second.find("slot"), std::string::npos) << r.second;
}

TEST(TailIr, ACodePointerThatNothingAssignedIsRefusedAsAnyUnassignedUse) {
  auto r = check([](B & b, GRJIT_CallHooks & h, GRJIT_VReg x, GRJIT_VReg, GRJIT_VReg p) {
    b.callable(h);
    b.at(b.block());
    b.tail_call_ptr(V(p), 99, {V(x)}, kId, st2(x)); // p was never assigned
  });
  EXPECT_EQ(r.first, GRJIT_ERR_INVALID);
  EXPECT_NE(r.second.find("assigned"), std::string::npos) << r.second;
  // The control: assigned on every path, it is well formed.
  auto ok = check([](B & b, GRJIT_CallHooks & h, GRJIT_VReg x, GRJIT_VReg, GRJIT_VReg p) {
    b.callable(h);
    b.at(b.block());
    b.cnst(p, 0x1000);
    b.tail_call_ptr(V(p), 99, {V(x)}, kId, st2(x));
  });
  EXPECT_EQ(ok.first, GRJIT_OK) << ok.second;
  // And assigned on one path only is refused too.
  auto one_path = check([](B & b, GRJIT_CallHooks & h, GRJIT_VReg x, GRJIT_VReg, GRJIT_VReg p) {
    b.callable(h);
    GRJIT_BlockId b0 = b.block(), yes = b.block(), no = b.block(), join = b.block();
    b.at(b0);
    b.br_if(V(x), yes, no);
    b.at(yes);
    b.cnst(p, 0x1000);
    b.br(join);
    b.at(no);
    b.br(join);
    b.at(join);
    b.tail_call_ptr(V(p), 99, {V(x)}, kId, st2(x));
  });
  EXPECT_EQ(one_path.first, GRJIT_ERR_INVALID) << one_path.second;
}

TEST(TailIr, ThePrinterShowsBothFormsWithTheirTokensAndTheOneState) {
  B b("caller", 2);
  GRJIT_CallHooks h = all_hooks();
  GRJIT_VReg x = b.param(GRJIT_TYPE_I64);
  GRJIT_VReg p = b.reg(GRJIT_TYPE_PTR);
  b.callable(h);
  GRJIT_BlockId b0 = b.block();
  GRJIT_BlockId b1 = b.block();
  GRJIT_BlockId b2 = b.block();
  b.at(b0);
  b.cnst(p, 0x1000);
  b.br_if(V(x), b1, b2);
  b.at(b1);
  b.tail_call_slot(reinterpret_cast<void *>(0x2000), 5, {V(x), I(9)}, kId,
      {grjit_frame_slot_vreg(x), grjit_frame_slot_dead()});
  b.at(b2);
  b.tail_call_ptr(V(p), 6, {}, kId, {grjit_frame_slot_dead(), grjit_frame_slot_constant(7)});
  Fn f(b.finish());
  std::string text = print(f);
  EXPECT_NE(text.find("tail_call.slot 0x2000 callee=5(v0, #9) state fn=7 off=3 [v0, dead]\n"),
      std::string::npos) << text;
  EXPECT_NE(text.find("tail_call.ptr v1 callee=6() state fn=7 off=3 [dead, #7]\n"),
      std::string::npos) << text;
  EXPECT_EQ(text.find("exit"), std::string::npos) << "a tail call has no exit state: " << text;
}

namespace {

/* The sites of a function with one tail call, as liveness makes them. */
struct Sites {
  GRJIT_LiveSites live{};
  explicit Sites(const GRJIT_Function * f) {
    EXPECT_EQ(grjit_liveness_compute(f, grjit_allocator_default(), 1000, &live), GRJIT_OK);
  }
  ~Sites() { grjit_liveness_free(&live); }
  std::vector<GRJIT_VReg> at(size_t i) const {
    return std::vector<GRJIT_VReg>(live.sites[i].vregs, live.sites[i].vregs + live.sites[i].count);
  }
};

} // namespace

TEST(TailIr, ATailCallMakesTwoSitesTheHookWithItsArgumentsAndTheExitWithOnlyItsState) {
  B b("caller", 3);
  GRJIT_CallHooks h = all_hooks();
  GRJIT_VReg x = b.param(GRJIT_TYPE_I64);
  GRJIT_VReg keep = b.param(GRJIT_TYPE_REF);   // named by the state
  GRJIT_VReg arg = b.param(GRJIT_TYPE_REF);    // only an argument
  GRJIT_VReg dead = b.param(GRJIT_TYPE_REF);   // neither
  (void)dead;
  b.callable(h);
  b.at(b.block());
  b.tail_call_slot(&g_entry_word, 1, {V(x), V(arg)}, kId,
      {grjit_frame_slot_vreg(x), grjit_frame_slot_vreg(keep), grjit_frame_slot_dead()});
  Fn f(b.finish());
  Sites s(f);
  ASSERT_EQ(s.live.count, 2u);
  // The hook is a frame-push site: the references it may move, the state's and
  // the arguments' (the arguments are also named by the area, in the metadata).
  EXPECT_EQ(s.at(0), (std::vector<GRJIT_VReg>{keep, arg}));
  // The exit leaves the frame: what its state names.
  EXPECT_EQ(s.at(1), (std::vector<GRJIT_VReg>{keep}));
}

TEST(TailIr, EveryBackendEmitsATailCallOnAnyHost) {
  B b("caller", 2);
  GRJIT_CallHooks h = all_hooks();
  GRJIT_VReg x = b.param(GRJIT_TYPE_I64);
  b.callable(h);
  b.at(b.block());
  b.tail_call_slot(&g_entry_word, 1, {V(x)}, kId, st2(x));
  Fn f(b.finish());
  GRJIT_Emitted e;
  for (GRJIT_Arch arch : {GRJIT_ARCH_X86_64, GRJIT_ARCH_ARM64, GRJIT_ARCH_X86_64_WIN64}) {
    EXPECT_EQ(grjit_emit_for(arch, f, grjit_allocator_default(), nullptr, nullptr, 0x40, &e), GRJIT_OK) << arch;
    EXPECT_GT(e.size, 0u);
    grjit_emitted_free(&e);
  }
}

GRJIT_TEST_MAIN()
