/**
 * @file
 *
 * The IR of native calls (AD-28, CAP-7): the table of registered natives and its
 * refusals, what the builder records, every refusal of the verifier (each naming
 * the operation), the printer, the liveness sites, and the refusal of every
 * backend that has no emitter for it. Emission is `test_natives.cpp`'s.
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

GRJIT_CallHooks hooks() {
  GRJIT_CallHooks h{};
  h.push = h_push;
  h.pop = h_pop;
  h.compile = h_compile;
  h.deopt = h_deopt;
  return h;
}

/* The addresses the descriptors name: nothing here calls them. */
uint64_t n_plain(void *) { return 0; }
GRJIT_NativeResult n_status(void *) { return {0, 0, 0}; }

const GRCORE_PollIdentity kId{7, 3};
const GRCORE_PollIdentity kAfter{7, 4};

constexpr GRJIT_Type TI = GRJIT_TYPE_I64;
constexpr GRJIT_Type TR = GRJIT_TYPE_REF;
constexpr GRJIT_Type TP = GRJIT_TYPE_PTR;
constexpr GRJIT_Type NONE_T = GRJIT_NATIVE_NO_RESULT;

std::vector<GRJIT_FrameSlot> st2(GRJIT_VReg a, GRJIT_VReg b) {
  return {grjit_frame_slot_vreg(a), grjit_frame_slot_vreg(b)};
}

/* A callable function with an I64 parameter x, a REF r, a PTR p and an I64 y,
 * four interpreter slots naming them; `body` builds the entry block. The result is
 * what the verifier says. */
struct Regs {
  GRJIT_VReg x, r, p, y;
  std::vector<GRJIT_FrameSlot> state() const {
    return {grjit_frame_slot_vreg(x), grjit_frame_slot_vreg(r), grjit_frame_slot_vreg(p),
        grjit_frame_slot_vreg(y)};
  }
};

template <typename F>
std::pair<GRJIT_Result, std::string> check(const GRJIT_NativeTable * table, F body,
    bool callable = true, const GRJIT_Limits * limits = nullptr) {
  B b("caller", 4);
  GRJIT_CallHooks h = hooks();
  Regs g{};
  g.x = b.param(TI);
  g.r = b.param(TR);
  g.p = b.param(TP);
  g.y = b.reg(TI);
  if (callable) {
    b.callable(h);
  }
  b.natives(table);
  b.at(b.block());
  b.cnst(g.y, 0);
  body(b, g);
  b.ret(V(g.y));
  Fn f(b.finish());
  GRJIT_Result res;
  std::string why = verify_reason(f, &res, limits);
  return {res, why};
}

} // namespace

/* ---- The table ---------------------------------------------------------------- */

TEST(NativeTable, DescriptorsAreCopiedGetStableIdsInOrderAndTheTableIsAppendOnly) {
  NativeTab t;
  EXPECT_EQ(grjit_native_table_count(t), 0u);
  EXPECT_EQ(grjit_native_table_get(t, 0), nullptr) << "an empty table holds no id";
  std::vector<GRJIT_Type> params = {TI, TR, TP};
  GRJIT_NativeDesc d{};
  d.address = reinterpret_cast<uintptr_t>(n_plain);
  d.params = params.data();
  d.param_count = 3;
  d.result = TR;
  d.flags = GRJIT_NATIVE_REENTERS;
  d.stack_bytes = 123;
  uint32_t id = 99;
  ASSERT_EQ(grjit_native_table_add(t, &d, &id), GRJIT_OK);
  EXPECT_EQ(id, 0u);
  // The caller's descriptor and its array are not kept.
  params[0] = TP;
  d.stack_bytes = 5;
  const GRJIT_NativeDesc * got = grjit_native_table_get(t, 0);
  ASSERT_NE(got, nullptr);
  EXPECT_EQ(got->address, reinterpret_cast<uintptr_t>(n_plain));
  EXPECT_EQ(got->param_count, 3u);
  EXPECT_EQ(got->params[0], TI);
  EXPECT_EQ(got->params[1], TR);
  EXPECT_EQ(got->params[2], TP);
  EXPECT_NE(got->params, params.data()) << "the parameter types are copied";
  EXPECT_EQ(got->result, TR);
  EXPECT_EQ(got->flags, GRJIT_NATIVE_REENTERS);
  EXPECT_EQ(got->stack_bytes, 123u);
  // Ids follow, in order, and a descriptor's pointer stays valid as the table grows.
  for (uint32_t k = 1; k < 40; k++) {
    GRJIT_NativeDesc e{};
    e.address = reinterpret_cast<uintptr_t>(n_status) + k;
    e.result = NONE_T;
    uint32_t again = 0;
    ASSERT_EQ(grjit_native_table_add(t, &e, &again), GRJIT_OK);
    EXPECT_EQ(again, k);
  }
  EXPECT_EQ(grjit_native_table_count(t), 40u);
  EXPECT_EQ(grjit_native_table_get(t, 0), got) << "ids are stable and so are the copies";
  EXPECT_EQ(got->params[1], TR);
  EXPECT_EQ(grjit_native_table_get(t, 40), nullptr);
  EXPECT_EQ(grjit_native_table_get(t, UINT32_MAX), nullptr);
  EXPECT_EQ(grjit_native_table_get(nullptr, 0), nullptr);
  EXPECT_EQ(grjit_native_table_count(nullptr), 0u);
  GRJIT_NativeDesc none{};
  none.address = 1;
  none.result = NONE_T;
  EXPECT_EQ(grjit_native_table_add(t, &none, nullptr), GRJIT_OK) << "the id output is optional";
  grjit_native_table_free(nullptr); // ignored
}

TEST(NativeTable, ADescriptorTheLibraryCannotDescribeOrWillNotAcceptIsRefusedAndChangesNothing) {
  NativeTab t;
  auto base = [](std::vector<GRJIT_Type> & params) {
    GRJIT_NativeDesc d{};
    d.address = reinterpret_cast<uintptr_t>(n_plain);
    d.params = params.data();
    d.param_count = params.size();
    d.result = TI;
    return d;
  };
  std::vector<GRJIT_Type> two = {TI, TR};
  GRJIT_NativeDesc good = base(two);
  ASSERT_EQ(grjit_native_table_add(t, &good, nullptr), GRJIT_OK) << "the control";
  ASSERT_EQ(grjit_native_table_count(t), 1u);

  GRJIT_NativeDesc d = base(two);
  d.address = 0;
  EXPECT_EQ(grjit_native_table_add(t, &d, nullptr), GRJIT_ERR_INVALID) << "no address";
  d = base(two);
  d.params = nullptr;
  EXPECT_EQ(grjit_native_table_add(t, &d, nullptr), GRJIT_ERR_INVALID) << "a count with no types";
  d = base(two);
  d.flags = 4;
  EXPECT_EQ(grjit_native_table_add(t, &d, nullptr), GRJIT_ERR_INVALID) << "a flag that is not defined";
  d = base(two);
  d.flags = GRJIT_NATIVE_STATUS | 0x80;
  EXPECT_EQ(grjit_native_table_add(t, &d, nullptr), GRJIT_ERR_INVALID);
  d = base(two);
  d.result = GRJIT_TYPE_COUNT; // spelled GRJIT_NATIVE_NO_RESULT: that one is allowed...
  EXPECT_EQ(grjit_native_table_add(t, &d, nullptr), GRJIT_OK) << "no result is a result of none";
  ASSERT_EQ(grjit_native_table_count(t), 2u);
  d = base(two);
  d.result = static_cast<GRJIT_Type>(GRJIT_TYPE_COUNT + 1);
  EXPECT_EQ(grjit_native_table_add(t, &d, nullptr), GRJIT_ERR_INVALID) << "a result that is no type";
  std::vector<GRJIT_Type> bad = {TI, static_cast<GRJIT_Type>(GRJIT_TYPE_COUNT)};
  d = base(bad);
  EXPECT_EQ(grjit_native_table_add(t, &d, nullptr), GRJIT_ERR_INVALID)
      << "a parameter of none is not a parameter (a floating-point native has no type to name)";
  std::vector<GRJIT_Type> wild = {TI, static_cast<GRJIT_Type>(77)};
  d = base(wild);
  EXPECT_EQ(grjit_native_table_add(t, &d, nullptr), GRJIT_ERR_INVALID);
  EXPECT_EQ(grjit_native_table_add(t, nullptr, nullptr), GRJIT_ERR_INVALID);
  EXPECT_EQ(grjit_native_table_add(nullptr, &good, nullptr), GRJIT_ERR_INVALID);
  EXPECT_EQ(grjit_native_table_count(t), 2u) << "a refusal adds nothing";

  // The caps: sixteen arguments, not seventeen; 64 KiB of stack, not one byte more.
  std::vector<GRJIT_Type> sixteen(16, TI);
  d = base(sixteen);
  EXPECT_EQ(grjit_native_table_add(t, &d, nullptr), GRJIT_OK);
  std::vector<GRJIT_Type> seventeen(17, TI);
  d = base(seventeen);
  EXPECT_EQ(grjit_native_table_add(t, &d, nullptr), GRJIT_ERR_LIMIT);
  d = base(two);
  d.stack_bytes = 64 * 1024;
  EXPECT_EQ(grjit_native_table_add(t, &d, nullptr), GRJIT_OK);
  d.stack_bytes = 64 * 1024 + 1;
  EXPECT_EQ(grjit_native_table_add(t, &d, nullptr), GRJIT_ERR_LIMIT);
  EXPECT_EQ(grjit_native_table_count(t), 4u);
}

TEST(NativeTable, TheCapsAreTheLimitsTheTableWasMadeWithAndTheArgumentCeilingIsSixteen) {
  GRJIT_Limits l{};
  l.max_native_arguments = 3;
  l.max_native_stack_bytes = 100;
  NativeTab t(&l);
  std::vector<GRJIT_Type> three(3, TI);
  std::vector<GRJIT_Type> four(4, TI);
  GRJIT_NativeDesc d{};
  d.address = 1;
  d.result = TI;
  d.params = three.data();
  d.param_count = 3;
  d.stack_bytes = 100;
  EXPECT_EQ(grjit_native_table_add(t, &d, nullptr), GRJIT_OK);
  d.params = four.data();
  d.param_count = 4;
  EXPECT_EQ(grjit_native_table_add(t, &d, nullptr), GRJIT_ERR_LIMIT);
  d.params = three.data();
  d.param_count = 3;
  d.stack_bytes = 101;
  EXPECT_EQ(grjit_native_table_add(t, &d, nullptr), GRJIT_ERR_LIMIT);

  GRJIT_Limits big{};
  big.max_native_arguments = 1000;
  big.max_native_stack_bytes = size_t{1} << 40;
  NativeTab t2(&big);
  std::vector<GRJIT_Type> seventeen(17, TI);
  d.params = seventeen.data();
  d.param_count = 17;
  d.stack_bytes = 0;
  EXPECT_EQ(grjit_native_table_add(t2, &d, nullptr), GRJIT_ERR_LIMIT) << "sixteen is the ceiling";
  d.param_count = 0;
  d.stack_bytes = 1u << 30;
  EXPECT_EQ(grjit_native_table_add(t2, &d, nullptr), GRJIT_OK);
  d.stack_bytes = (1u << 30) + 1;
  EXPECT_EQ(grjit_native_table_add(t2, &d, nullptr), GRJIT_ERR_LIMIT) << "so is a gigabyte of stack";

  GRJIT_Limits dflt;
  grjit_limits_default(&dflt);
  EXPECT_EQ(dflt.max_native_arguments, 16u);
  EXPECT_EQ(dflt.max_native_stack_bytes, size_t{64} * 1024);
}

TEST(NativeTable, AnAllocationThatFailsInAnyArmLeavesTheTableAsItWasAndItIsFreedWhole) {
  for (long fail = 1; fail <= 6; fail++) {
    TrackingAllocator alloc;
    {
      NativeTab t(nullptr, alloc.get());
      alloc.calls = 0;
      alloc.fail_at = fail;
      std::vector<GRJIT_Type> params = {TI, TR, TP};
      GRJIT_NativeDesc d{};
      d.address = 1;
      d.params = params.data();
      d.param_count = 3;
      d.result = TI;
      GRJIT_Result r = GRJIT_OK;
      size_t refused = 0;
      for (int k = 0; k < 20; k++) {
        r = grjit_native_table_add(t, &d, nullptr);
        if (r != GRJIT_OK) {
          EXPECT_EQ(r, GRJIT_ERR_OOM);
          refused++;
        }
      }
      EXPECT_EQ(refused, 1u) << "exactly the one allocation fails, at " << fail;
      EXPECT_EQ(grjit_native_table_count(t), 19u) << "the refused add is not in the table";
      for (uint32_t k = 0; k < 19; k++) {
        ASSERT_NE(grjit_native_table_get(t, k), nullptr);
        EXPECT_EQ(grjit_native_table_get(t, k)->params[2], TP);
      }
    }
    EXPECT_EQ(alloc.live, 0) << "every block is freed, failing at " << fail;
  }
  TrackingAllocator none;
  none.fail_at = 1;
  GRJIT_NativeTable * out = reinterpret_cast<GRJIT_NativeTable *>(0x1);
  EXPECT_EQ(grjit_native_table_create(nullptr, none.get(), &out), GRJIT_ERR_OOM);
  EXPECT_EQ(grjit_native_table_create(nullptr, nullptr, nullptr), GRJIT_ERR_INVALID);
}

/* ---- The builder ---------------------------------------------------------------- */

TEST(NativeIr, TheBuilderRecordsTheKindTheIdTheArgumentsAndBothStatesAndCountsTheSites) {
  NativeTab t;
  uint32_t plain = t.add(reinterpret_cast<const void *>(n_plain), {TI, TR}, TI);
  uint32_t stat = t.add(reinterpret_cast<const void *>(n_status), {TI}, TR, GRJIT_NATIVE_STATUS);
  B b("caller", 2);
  GRJIT_VReg x = b.param(TI);
  GRJIT_VReg r = b.param(TR);
  GRJIT_VReg d1 = b.reg(TI);
  GRJIT_VReg d2 = b.reg(TR);
  b.callable(hooks());
  b.natives(t);
  b.at(b.block());
  b.call_native(d1, plain, {V(x), V(r)}, kId, st2(x, r));
  b.call_native(d2, stat, {I(5)}, kId, st2(x, r), kAfter, {grjit_frame_slot_vreg(d2), grjit_frame_slot_dead()});
  b.ret(V(d1));
  Fn f(b.finish());
  size_t n = 0;
  const GRJIT_Op * ops = grjit_function_block_ops(f, 0, &n);
  ASSERT_EQ(n, 3u);
  EXPECT_EQ(ops[0].kind, GRJIT_OP_CALL_NATIVE);
  EXPECT_EQ(ops[0].native, plain);
  EXPECT_EQ(ops[0].dst, d1);
  ASSERT_EQ(ops[0].arg_count, 2u);
  EXPECT_EQ(ops[0].args[0].kind, GRJIT_OPERAND_VREG);
  EXPECT_EQ(ops[0].args[1].vreg, r);
  EXPECT_NE(ops[0].state, GRJIT_NO_STATE);
  EXPECT_EQ(ops[0].exit_state, GRJIT_NO_STATE) << "a native with no status has one state";
  EXPECT_EQ(ops[1].native, stat);
  ASSERT_EQ(ops[1].arg_count, 1u);
  EXPECT_EQ(ops[1].args[0].kind, GRJIT_OPERAND_IMM);
  EXPECT_EQ(ops[1].args[0].imm, 5);
  EXPECT_NE(ops[1].exit_state, GRJIT_NO_STATE);
  EXPECT_EQ(grjit_function_frame_state_count(f), 3u);
  const GRJIT_FrameState * before = grjit_function_frame_state(f, ops[1].state);
  const GRJIT_FrameState * after = grjit_function_frame_state(f, ops[1].exit_state);
  EXPECT_EQ(before->identity.offset, 3u);
  EXPECT_EQ(after->identity.offset, 4u);
  EXPECT_EQ(after->slots[0].vreg, d2) << "the state after the call names the result";
  EXPECT_EQ(after->slots[1].kind, GRJIT_FRAME_SLOT_DEAD);
  EXPECT_EQ(grjit_liveness_site_count(&ops[0]), 2u) << "the call and the exit before it";
  EXPECT_EQ(grjit_liveness_site_count(&ops[1]), 3u) << "and the exit a status takes";
  EXPECT_NE(ops[0].state, GRJIT_NO_STATE) << "a native call is a site";
}

TEST(NativeIr, TheBuilderHoldsAtMostSixteenArgumentsAndTheLimitItWasGiven) {
  NativeTab t;
  std::vector<GRJIT_Type> sixteen(16, TI);
  uint32_t id = t.add(reinterpret_cast<const void *>(n_plain), sixteen, TI);
  auto build = [&](size_t count, const GRJIT_Limits * limits) {
    B b("caller", 1, limits);
    GRJIT_VReg x = b.param(TI);
    b.callable(hooks());
    b.natives(t);
    b.at(b.block());
    std::vector<GRJIT_Operand> args(count, V(x));
    GRJIT_FrameState st{kId, 1, nullptr};
    GRJIT_FrameSlot slot = grjit_frame_slot_vreg(x);
    st.slots = &slot;
    return grjit_builder_call_native(b.b, GRJIT_NO_VREG, id, args.data(), args.size(), &st, nullptr);
  };
  EXPECT_EQ(build(16, nullptr), GRJIT_OK);
  EXPECT_EQ(build(17, nullptr), GRJIT_ERR_LIMIT) << "no native takes more than sixteen";
  GRJIT_Limits l{};
  l.max_native_arguments = 4;
  EXPECT_EQ(build(4, &l), GRJIT_OK);
  EXPECT_EQ(build(5, &l), GRJIT_ERR_LIMIT);
  // A NULL state, a NULL builder: refused, and nothing is recorded.
  B b("caller", 0);
  b.at(b.block());
  EXPECT_EQ(grjit_builder_call_native(b.b, GRJIT_NO_VREG, 0, nullptr, 0, nullptr, nullptr), GRJIT_ERR_INVALID);
  GRJIT_FrameState st{kId, 0, nullptr};
  EXPECT_EQ(grjit_builder_call_native(nullptr, GRJIT_NO_VREG, 0, nullptr, 0, &st, nullptr), GRJIT_ERR_INVALID);
  EXPECT_EQ(grjit_builder_set_natives(nullptr, t), GRJIT_ERR_INVALID);
  Fn f(b.finish());
  EXPECT_EQ(grjit_function_op_count(f), 0u);
}

/* ---- The verifier ----------------------------------------------------------------- */

TEST(NativeIr, AWellFormedNativeCallVerifiesWithOrWithoutAStatusAndWhateverItsTypes) {
  NativeTab t;
  uint32_t a = t.add(reinterpret_cast<const void *>(n_plain), {TI, TR, TP}, TI);
  uint32_t b = t.add(reinterpret_cast<const void *>(n_status), {TR}, TR, GRJIT_NATIVE_STATUS);
  uint32_t c = t.add(reinterpret_cast<const void *>(n_status), {}, TP, GRJIT_NATIVE_STATUS | GRJIT_NATIVE_REENTERS, 4096);
  uint32_t d = t.add(reinterpret_cast<const void *>(n_plain), {TI}, NONE_T);
  auto r = check(t, [&](B & bb, Regs g) {
    GRJIT_VReg pr = bb.reg(TP);
    GRJIT_VReg rr = bb.reg(TR);
    bb.call_native(g.y, a, {V(g.x), V(g.r), V(g.p)}, kId, g.state());
    bb.call_native(rr, b, {V(g.r)}, kId, g.state(), kAfter, g.state());
    bb.call_native(pr, c, {}, kId, g.state(), kAfter, g.state());
    bb.call_native(GRJIT_NO_VREG, d, {I(7)}, kId, g.state());
    bb.call_native(GRJIT_NO_VREG, a, {I(1), I(2), I(3)}, kId, g.state()); // a result discarded; immediates for any type
    bb.call_native(GRJIT_NO_VREG, b, {I(0)}, kId, g.state(), kAfter, g.state());
  });
  EXPECT_EQ(r.first, GRJIT_OK) << r.second;
}

TEST(NativeIr, EveryRefusalNamesTheOperationAndWhatIsWrong) {
  NativeTab t;
  uint32_t plain = t.add(reinterpret_cast<const void *>(n_plain), {TI, TR}, TI);
  uint32_t stat = t.add(reinterpret_cast<const void *>(n_status), {TI}, TR, GRJIT_NATIVE_STATUS);
  uint32_t nores = t.add(reinterpret_cast<const void *>(n_plain), {}, NONE_T);
  struct Case {
    const char * what;
    GRJIT_Result expect;
    const char * text;
    std::function<void(B &, Regs)> body;
  };
  std::vector<Case> cases = {
      {"an id the table does not hold", GRJIT_ERR_INVALID, "native #9 is not in the function's native table",
          [&](B & b, Regs g) { b.call_native(GRJIT_NO_VREG, 9, {}, kId, g.state()); }},
      {"the last id of the table, plus one", GRJIT_ERR_INVALID, "native #3 is not in",
          [&](B & b, Regs g) { b.call_native(GRJIT_NO_VREG, 3, {}, kId, g.state()); }},
      {"fewer arguments than the descriptor", GRJIT_ERR_INVALID, "takes 2 arguments, the call has 1",
          [&](B & b, Regs g) { b.call_native(g.y, plain, {V(g.x)}, kId, g.state()); }},
      {"more arguments than the descriptor", GRJIT_ERR_INVALID, "takes 2 arguments, the call has 3",
          [&](B & b, Regs g) { b.call_native(g.y, plain, {V(g.x), V(g.r), V(g.x)}, kId, g.state()); }},
      {"a register of the wrong type", GRJIT_ERR_INVALID, "argument 1 is v0, whose type is not the type native #0 takes",
          [&](B & b, Regs g) { b.call_native(g.y, plain, {V(g.x), V(g.x)}, kId, g.state()); }},
      {"a pointer where a reference is taken", GRJIT_ERR_INVALID, "argument 1 is v2",
          [&](B & b, Regs g) { b.call_native(g.y, plain, {V(g.x), V(g.p)}, kId, g.state()); }},
      {"a reference where an i64 is taken", GRJIT_ERR_INVALID, "argument 0 is v1",
          [&](B & b, Regs g) { b.call_native(g.y, plain, {V(g.r), V(g.r)}, kId, g.state()); }},
      {"a register that does not exist", GRJIT_ERR_INVALID, "argument 0 names v77",
          [&](B & b, Regs g) { b.call_native(g.y, plain, {V(77), V(g.r)}, kId, g.state()); }},
      {"a destination of the wrong type", GRJIT_ERR_INVALID, "destination v1 is not of the type native #0 returns",
          [&](B & b, Regs g) { b.call_native(g.r, plain, {V(g.x), V(g.r)}, kId, g.state()); }},
      {"a destination for a native with no result", GRJIT_ERR_INVALID, "native #2 has no result",
          [&](B & b, Regs g) { b.call_native(g.y, nores, {}, kId, g.state()); }},
      {"a destination that does not exist", GRJIT_ERR_INVALID, "destination v88 does not exist",
          [&](B & b, Regs g) { b.call_native(88, plain, {V(g.x), V(g.r)}, kId, g.state()); }},
      {"a state of the wrong length", GRJIT_ERR_INVALID, "frame state has 1 slots, the function has 4",
          [&](B & b, Regs g) { b.call_native(g.y, plain, {V(g.x), V(g.r)}, kId, {grjit_frame_slot_vreg(g.x)}); }},
      {"a state that names a register that does not exist", GRJIT_ERR_INVALID, "names v55",
          [&](B & b, Regs g) {
            auto s = g.state();
            s[2] = grjit_frame_slot_vreg(55);
            b.call_native(g.y, plain, {V(g.x), V(g.r)}, kId, s);
          }},
      {"a status native with no state after the call", GRJIT_ERR_INVALID, "returns a status, and the call has no state after it",
          [&](B & b, Regs g) { b.call_native(GRJIT_NO_VREG, stat, {I(1)}, kId, g.state()); }},
      {"a state after the call of the wrong length", GRJIT_ERR_INVALID, "frame state has 0 slots, the function has 4",
          [&](B & b, Regs g) { b.call_native(GRJIT_NO_VREG, stat, {I(1)}, kId, g.state(), kAfter, {}); }},
      {"a state after the call for a native with no status", GRJIT_ERR_INVALID, "returns no status, and the call has a state after it",
          [&](B & b, Regs g) { b.call_native(g.y, plain, {V(g.x), V(g.r)}, kId, g.state(), kAfter, g.state()); }},
      {"an argument no register assigned", GRJIT_ERR_INVALID, "may not have been assigned",
          [&](B & b, Regs g) {
            GRJIT_VReg late = b.reg(TR);
            b.call_native(g.y, plain, {V(g.x), V(late)}, kId, g.state());
          }},
      {"a state that names the result before the call assigns it", GRJIT_ERR_INVALID, "may not have been assigned",
          [&](B & b, Regs g) {
            GRJIT_VReg res = b.reg(TR);
            auto s = g.state();
            s[1] = grjit_frame_slot_vreg(res);
            b.call_native(res, stat, {I(1)}, kId, s, kAfter, g.state());
          }},
  };
  for (const Case & c : cases) {
    auto r = check(t, c.body);
    EXPECT_EQ(r.first, c.expect) << c.what << ": " << r.second;
    EXPECT_NE(r.second.find("op "), std::string::npos) << c.what << " names the operation: " << r.second;
    EXPECT_NE(r.second.find(c.text), std::string::npos) << c.what << ": " << r.second;
  }
  // Each of those is refused for what it is and not for the shape they share: the same
  // body with its one fault corrected is the control.
  auto ok = check(t, [&](B & b, Regs g) {
    b.call_native(g.y, plain, {V(g.x), V(g.r)}, kId, g.state());
    b.call_native(GRJIT_NO_VREG, stat, {I(1)}, kId, g.state(), kAfter, g.state());
    b.call_native(GRJIT_NO_VREG, nores, {}, kId, g.state());
  });
  EXPECT_EQ(ok.first, GRJIT_OK) << ok.second;
}

TEST(NativeIr, ANativeCallNeedsACallableFunctionATableAndTheDeoptHook) {
  NativeTab t;
  uint32_t plain = t.add(reinterpret_cast<const void *>(n_plain), {}, TI);
  auto call = [&](B & b, Regs g) { b.call_native(g.y, plain, {}, kId, g.state()); };
  auto ok = check(t, call);
  EXPECT_EQ(ok.first, GRJIT_OK) << ok.second;
  auto not_callable = check(t, call, false);
  EXPECT_EQ(not_callable.first, GRJIT_ERR_INVALID);
  EXPECT_NE(not_callable.second.find("a native call in a function that is not callable"), std::string::npos)
      << not_callable.second;
  EXPECT_NE(not_callable.second.find("op 1"), std::string::npos) << not_callable.second;
  auto no_table = check(nullptr, call);
  EXPECT_EQ(no_table.first, GRJIT_ERR_INVALID);
  EXPECT_NE(no_table.second.find("native #0 is not in the function's native table"), std::string::npos)
      << no_table.second;
  // A callable function without the deopt hook is refused whatever it contains: a
  // native call is one more way to leave a chain.
  {
    B b("caller", 4);
    GRJIT_CallHooks h{};
    h.push = h_push;
    h.pop = h_pop;
    Regs g{};
    g.x = b.param(TI);
    g.r = b.param(TR);
    g.p = b.param(TP);
    g.y = b.reg(TI);
    b.callable(h);
    b.natives(t);
    b.at(b.block());
    b.cnst(g.y, 0);
    call(b, g);
    b.ret(V(g.y));
    Fn f(b.finish());
    GRJIT_Result res;
    std::string why = verify_reason(f, &res);
    EXPECT_EQ(res, GRJIT_ERR_INVALID);
    EXPECT_NE(why.find("deopt"), std::string::npos) << why;
  }
  // The verifier's own cap on a native's arguments, given at verify time.
  std::vector<GRJIT_Type> three(3, TI);
  uint32_t wide = t.add(reinterpret_cast<const void *>(n_plain), three, TI);
  GRJIT_Limits l{};
  l.max_native_arguments = 2;
  auto capped = check(t, [&](B & b, Regs g) { b.call_native(g.y, wide, {V(g.x), V(g.x), V(g.x)}, kId, g.state()); },
      true, &l);
  EXPECT_EQ(capped.first, GRJIT_ERR_LIMIT) << capped.second;
  EXPECT_NE(capped.second.find("at most 2 are allowed"), std::string::npos) << capped.second;
}

TEST(NativeIr, ADerivedPointerLiveAcrossANativeCallNeedsItsBaseLiveToo) {
  NativeTab t;
  uint32_t plain = t.add(reinterpret_cast<const void *>(n_plain), {}, TI);
  auto build = [&](bool keep_base) {
    B b("caller", 3);
    GRJIT_VReg r = b.param(TR);
    GRJIT_VReg d = b.reg(TP);
    GRJIT_VReg y = b.reg(TI);
    b.derived(d, r, 16);
    b.callable(hooks());
    b.natives(t);
    b.at(b.block());
    b.bitcast(d, r);
    b.bin(GRJIT_OP_ADD, d, V(d), I(16));
    b.call_native(y, plain, {}, kId, {grjit_frame_slot_dead(), grjit_frame_slot_dead(), grjit_frame_slot_dead()});
    b.load(y, d, 0, 64); // the derived pointer is live after the call
    if (keep_base) {
      b.mov(y, V(y));
      b.load(y, r, 8, 64); // the base is read too, so it is live
    }
    b.ret(V(y));
    Fn f(b.finish());
    GRJIT_Result res;
    std::string why = verify_reason(f, &res);
    return std::make_pair(res, why);
  };
  auto good = build(true);
  EXPECT_EQ(good.first, GRJIT_OK) << good.second;
  auto bad = build(false);
  EXPECT_EQ(bad.first, GRJIT_ERR_INVALID);
  EXPECT_NE(bad.second.find("derived pointer v1 is live at block b0"), std::string::npos) << bad.second;
}

TEST(NativeIr, CompilingAFunctionWhoseNativeTheTableDoesNotHoldIsRefusedAndNothingIsEmitted) {
  GRJIT_REQUIRE_BACKEND();
  JitWorld w;
  NativeTab t;
  uint32_t plain = t.add(reinterpret_cast<const void *>(n_plain), {}, TI);
  auto compile = [&](uint32_t id, const GRJIT_NativeTable * table) {
    B b("caller", 1);
    GRJIT_VReg x = b.param(TI);
    b.callable(hooks());
    b.natives(table);
    b.at(b.block());
    b.call_native(x, id, {}, kId, {grjit_frame_slot_vreg(x)});
    b.ret(V(x));
    Fn f(b.finish());
    Compiled c(f, w.pages());
    return c.result;
  };
  EXPECT_EQ(compile(plain + 1, t), GRJIT_ERR_INVALID) << "an id the table does not hold";
  EXPECT_EQ(compile(plain, nullptr), GRJIT_ERR_INVALID) << "no table";
  EXPECT_EQ(compile(plain, t), grjit_backend_calls_available() ? GRJIT_OK : GRJIT_ERR_UNSUPPORTED)
      << "the control: the same call with its native";
  EXPECT_EQ(w.blocks_in_use(), 0u) << "a refusal leaves no memory behind";
}

/* ---- The printer ------------------------------------------------------------------- */

TEST(NativeIr, ThePrinterShowsTheIdTheArgumentsTheStateAndTheStateAfterTheCall) {
  NativeTab t;
  uint32_t plain = t.add(reinterpret_cast<const void *>(n_plain), {TI, TR}, TI);
  uint32_t stat = t.add(reinterpret_cast<const void *>(n_status), {TI}, TR, GRJIT_NATIVE_STATUS);
  B b("caller", 2);
  GRJIT_VReg x = b.param(TI);
  GRJIT_VReg r = b.param(TR);
  GRJIT_VReg d1 = b.reg(TI);
  GRJIT_VReg d2 = b.reg(TR);
  b.callable(hooks());
  b.natives(t);
  b.at(b.block());
  b.call_native(d1, plain, {V(x), V(r)}, kId, st2(x, r));
  b.call_native(d2, stat, {I(5)}, kId, st2(x, r), kAfter, {grjit_frame_slot_vreg(d2), grjit_frame_slot_constant(9)});
  b.call_native(GRJIT_NO_VREG, plain, {V(x), V(r)}, kId, st2(x, r));
  b.ret(V(d1));
  Fn f(b.finish());
  std::string text = print(f);
  EXPECT_NE(text.find("v2 = call_native #0(v0, v1) state fn=7 off=3 [v0, v1]\n"), std::string::npos) << text;
  EXPECT_NE(text.find("v3 = call_native #1(#5) state fn=7 off=3 [v0, v1] after state fn=7 off=4 [v3, #9]\n"),
      std::string::npos) << text;
  EXPECT_NE(text.find("\n  call_native #0(v0, v1) state"), std::string::npos) << "a discarded result has no destination: " << text;
  // A call without a status prints no "after".
  size_t first = text.find("call_native #0");
  EXPECT_EQ(text.find("after", first), text.find("after state fn=7 off=4"));
}

/* ---- The liveness sites ------------------------------------------------------------- */

namespace {
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

TEST(NativeIr, ANativeWithAStatusMakesThreeSitesTheCallTheExitBeforeItAndTheExitAfterItEachWithItsOwnSet) {
  NativeTab t;
  uint32_t stat = t.add(reinterpret_cast<const void *>(n_status), {TR}, TR, GRJIT_NATIVE_STATUS);
  // Registers: v0 live after the call and in no state; v1 only in the state before;
  // v2 only in the state after; v3 only an argument; v4 the result, named by the
  // state after; v5 dead; v6 the result's old value, named by the state before.
  B b("caller", 7);
  GRJIT_VReg live_after = b.param(TR);
  GRJIT_VReg in_before = b.param(TR);
  GRJIT_VReg in_after = b.param(TR);
  GRJIT_VReg arg = b.param(TR);
  GRJIT_VReg res = b.param(TR);
  GRJIT_VReg dead = b.param(TR);
  (void)dead;
  b.callable(hooks());
  b.natives(t);
  b.at(b.block());
  auto slot = [](GRJIT_VReg v) { return grjit_frame_slot_vreg(v); };
  std::vector<GRJIT_FrameSlot> before = {slot(in_before), slot(res), grjit_frame_slot_dead(), grjit_frame_slot_dead(),
      grjit_frame_slot_dead(), grjit_frame_slot_dead(), grjit_frame_slot_dead()};
  std::vector<GRJIT_FrameSlot> after = {slot(in_after), slot(res), grjit_frame_slot_dead(), grjit_frame_slot_dead(),
      grjit_frame_slot_dead(), grjit_frame_slot_dead(), grjit_frame_slot_dead()};
  b.call_native(res, stat, {V(arg)}, kId, before, kAfter, after);
  b.bitcast(res, live_after); // the result is overwritten later: a use of live_after
  b.ret(V(live_after));
  Fn f(b.finish());
  Sites s(f);
  ASSERT_EQ(s.live.count, 3u);
  // The call: live after it, less the result, plus what either state names but the
  // result (the state after is read from the frame by the exit, so a collection the
  // native causes must have updated it); the result's old value is named by the
  // state before and stays, as every site's does.
  EXPECT_EQ(s.at(0), (std::vector<GRJIT_VReg>{live_after, in_before, in_after, res}));
  // The exit before the call: only the state before.
  EXPECT_EQ(s.at(1), (std::vector<GRJIT_VReg>{in_before, res}));
  // The exit after it: only the state after, the result included.
  EXPECT_EQ(s.at(2), (std::vector<GRJIT_VReg>{in_after, res}));
}

TEST(NativeIr, ANativeWithNoStatusMakesTwoSitesAndTheResultIsNotLiveAtItsOwnCall) {
  NativeTab t;
  uint32_t plain = t.add(reinterpret_cast<const void *>(n_plain), {TR}, TR);
  B b("caller", 3);
  GRJIT_VReg keep = b.param(TR);
  GRJIT_VReg arg = b.param(TR);
  GRJIT_VReg res = b.reg(TR);
  b.callable(hooks());
  b.natives(t);
  b.at(b.block());
  b.cnst(res, 0);
  b.call_native(res, plain, {V(arg)}, kId, {grjit_frame_slot_dead(), grjit_frame_slot_dead(), grjit_frame_slot_dead()});
  b.cmp(GRJIT_CMP_EQ, b.reg(TI), V(keep), V(res)); // both are read afterwards
  b.ret(NONE());
  Fn f(b.finish());
  Sites s(f);
  ASSERT_EQ(s.live.count, 2u);
  EXPECT_EQ(s.at(0), (std::vector<GRJIT_VReg>{keep})) << "the argument is not mapped, and the result does not exist yet";
  EXPECT_TRUE(s.at(1).empty()) << "an exit before the call keeps only what its state names";
}

TEST(NativeIr, ADerivedPointerAndItsBaseAreInTheSetOfTheSitesThatKeepThem) {
  NativeTab t;
  uint32_t stat = t.add(reinterpret_cast<const void *>(n_status), {}, TI, GRJIT_NATIVE_STATUS);
  B b("caller", 2);
  GRJIT_VReg base = b.param(TR);
  GRJIT_VReg der = b.reg(TP);
  GRJIT_VReg y = b.reg(TI);
  b.derived(der, base, 8);
  b.callable(hooks());
  b.natives(t);
  b.at(b.block());
  b.bitcast(der, base);
  b.bin(GRJIT_OP_ADD, der, V(der), I(8));
  b.call_native(y, stat, {}, kId, {grjit_frame_slot_vreg(base), grjit_frame_slot_dead()}, kAfter,
      {grjit_frame_slot_vreg(base), grjit_frame_slot_vreg(y)});
  b.load(y, der, 0, 64);
  b.load(y, base, 0, 64);
  b.ret(V(y));
  Fn f(b.finish());
  Sites s(f);
  ASSERT_EQ(s.live.count, 3u);
  EXPECT_EQ(s.at(0), (std::vector<GRJIT_VReg>{base, der}));
  EXPECT_EQ(s.at(1), (std::vector<GRJIT_VReg>{base}));
  EXPECT_EQ(s.at(2), (std::vector<GRJIT_VReg>{base}));
}

/* ---- The backends --------------------------------------------------------------------- */

TEST(NativeIr, ArmAndWin64RefuseANativeCallBeforeEmittingAByteAndTheBytesOfOtherFunctionsAreUnchanged) {
  NativeTab t;
  uint32_t plain = t.add(reinterpret_cast<const void *>(n_plain), {TI}, TI);
  B b("caller", 1);
  GRJIT_VReg x = b.param(TI);
  b.callable(hooks());
  b.natives(t);
  b.at(b.block());
  b.call_native(x, plain, {V(x)}, kId, {grjit_frame_slot_vreg(x)});
  b.ret(V(x));
  Fn f(b.finish());
  GRJIT_Emitted e;
  for (GRJIT_Arch arch : {GRJIT_ARCH_ARM64, GRJIT_ARCH_X86_64_WIN64}) {
    EXPECT_EQ(grjit_emit_for(arch, f, grjit_allocator_default(), nullptr, nullptr, 0x40, &e),
        GRJIT_ERR_UNSUPPORTED);
    EXPECT_EQ(e.size, 0u);
    EXPECT_EQ(e.bytes, nullptr);
  }
}

GRJIT_TEST_MAIN()
