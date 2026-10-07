/**
 * @file
 *
 * Tail calls between compiled functions (AD-28, CAP-8) on x86-64 SysV: the
 * frame replacement for every pair of stack-argument counts, constant native
 * and guest stack in a million-deep recursion, the `tail` hook's refusals, the
 * deoptimization of a function a tail call entered, collections in the hook,
 * and what the walk sees. They run through the fixture engine
 * (`calls_fixture.h`), which interprets and compiles the same programs and
 * checks them against C++ references.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include "../calls_fixture.h"

#include "../../src/code/code_internal.h"

#include <random>

using namespace fx;

/* Tail calls are emitted for x86-64 SysV only (arm64 and Win64 are story 7's).
 * Where they are not, a test does not skip, which would count as a test that
 * proved nothing: it shows the refusal instead, which is what those targets
 * promise. */
#if defined(__x86_64__) && defined(__linux__)
#define TAIL_ONLY_ON_X86_64_SYSV() (void)0
#else
namespace {
void expect_tail_refused_here() {
  B b("tail", 0);
  GRJIT_VReg x = b.param(GRJIT_TYPE_I64);
  GRJIT_CallHooks h{};
  h.deopt = [](void *, uint64_t) -> uint32_t { return 0; };
  h.tail = [](void *, uint64_t, const uint64_t *, uint64_t) -> uint32_t { return 0; };
  h.compile = [](void *, uint64_t) -> uint32_t { return 0; };
  b.callable(h);
  b.at(b.block());
  static uint64_t word;
  b.tail_call_slot(&word, 1, {V(x)}, GRCORE_PollIdentity{0, 0}, {});
  Fn f(b.finish());
  JitWorld w;
  Compiled c(f, w.pages());
  EXPECT_EQ(c.result, GRJIT_ERR_UNSUPPORTED) << "a tail call is refused with a clear error";
  EXPECT_FALSE(c);
}
} // namespace
#define TAIL_ONLY_ON_X86_64_SYSV() \
  do { \
    expect_tail_refused_here(); \
    return; \
  } while (0)
#endif

namespace {

/* loop(n, acc): the sum of 1..n, by a tail call to itself: PROBE at the first
 * iteration (n == first) and at the last, in the same frame size, which is how
 * "constant stack" is measured. */
int add_countdown(Engine & e, int64_t first) {
  int loop = e.reserve();
  P p("loop", {GRJIT_TYPE_I64, GRJIT_TYPE_I64});
  int n = 0, acc = 1, z = p.local(), k = p.local(), t = p.local(), c = p.local(),
      one = p.local(), n1 = p.local(), a1 = p.local();
  p.cnst(z, 0);
  p.cnst(k, first);
  p.bin(K::EQ, t, n, k);
  int skip = p.brz(t);
  p.probe(n);
  p.patch(skip, p.here());
  p.bin(K::EQ, c, n, z);
  int more = p.brz(c);
  p.probe(n);
  p.ret(acc);
  p.patch(more, p.here());
  p.cnst(one, 1);
  p.bin(K::SUB, n1, n, one);
  p.bin(K::ADD, a1, acc, n);
  p.tailcall(loop, {n1, a1});
  e.set(loop, p.done());
  return loop;
}

constexpr int64_t kMillion = 1000000;

/* callee_T(a0 .. a{T-1}) = sum of (i + 1) * a_i, plus 12345. */
int64_t callee_ref(const std::vector<int64_t> & a) {
  int64_t r = 12345;
  for (size_t i = 0; i < a.size(); i++) {
    r += static_cast<int64_t>(i + 1) * a[i];
  }
  return r;
}

int add_callee(Engine & e, int t_args) {
  std::vector<GRJIT_Type> params(static_cast<size_t>(t_args), GRJIT_TYPE_I64);
  P p("callee", params);
  int acc = p.local(), c = p.local(), t = p.local();
  p.cnst(acc, 12345);
  for (int i = 0; i < t_args; i++) {
    p.cnst(c, i + 1);
    p.bin(K::MUL, t, i, c);
    p.bin(K::ADD, acc, acc, t);
  }
  p.ret(acc);
  return e.add(p.done());
}

/* What the caller passes the callee: its own parameters, last to first, as far
 * as it has them, and immediates after. */
std::vector<int64_t> tail_args(int params, int t_args, const std::vector<int64_t> & given) {
  std::vector<int64_t> a;
  for (int i = 0; i < t_args; i++) {
    a.push_back(i < params ? given[static_cast<size_t>(params - 1 - i)] : 1000 + 7 * i);
  }
  return a;
}

/* caller_P(p0 .. p{P-1}) = callee(p{P-1}, ..., p0, then immediates) by a tail
 * call, through a slot or through a code pointer in a register: no local but
 * the pointer, so the frame is as small as a frame can be and the arguments
 * area is staged right at its bottom. */
int add_tail_caller(Engine & e, int params, int callee, int t_args, bool through_pointer) {
  std::vector<GRJIT_Type> types(static_cast<size_t>(params), GRJIT_TYPE_I64);
  P p("tail_caller", types);
  std::vector<int> args;
  for (int i = 0; i < t_args; i++) {
    args.push_back(i < params ? params - 1 - i : p.imm(1000 + 7 * i));
  }
  if (through_pointer) {
    int ptr = p.local(GRJIT_TYPE_PTR);
    p.entryof(ptr, callee);
    p.tailcallp(ptr, callee, args);
  } else {
    p.tailcall(callee, args);
  }
  return e.add(p.done());
}

/* outer(p0 .. p{P-1}) = caller(p0 .. ) + y + callee(immediates): a call that
 * the tail call returns into, and a call after it, so the stack is as the
 * original caller expects it, and the frame's own locals are intact. */
int add_outer(Engine & e, int params, int caller, int callee, int t_args) {
  std::vector<GRJIT_Type> types(static_cast<size_t>(params), GRJIT_TYPE_I64);
  P p("outer", types);
  int y = p.local(), r = p.local(), r2 = p.local(), s = p.local();
  p.cnst(y, 31);
  std::vector<int> pass;
  for (int i = 0; i < params; i++) {
    pass.push_back(i);
  }
  p.call(r, caller, pass);
  std::vector<int> imms;
  for (int i = 0; i < t_args; i++) {
    imms.push_back(p.imm(5 + 3 * i));
  }
  p.call(r2, callee, imms);
  p.bin(K::ADD, s, r, y);
  p.bin(K::ADD, s, s, r2);
  p.ret(s);
  return e.add(p.done());
}

} // namespace

TEST(Tail, EveryPairOfCallerParametersAndCalleeArgumentsFromZeroToSixteenArrivesIntactInConstantStack) {
  TAIL_ONLY_ON_X86_64_SYSV();
  long pairs = 0;
  for (int through_pointer = 0; through_pointer < 2; through_pointer++) {
    for (int params = 0; params <= 16; params++) {
      for (int t_args = 0; t_args <= 16; t_args++) {
        SCOPED_TRACE(testing::Message() << (through_pointer ? "pointer" : "slot") << " params="
                                        << params << " callee args=" << t_args);
        Engine e;
        int callee = add_callee(e, t_args);
        ASSERT_TRUE(e.compile_fn(callee));
        int caller = add_tail_caller(e, params, callee, t_args, through_pointer != 0);
        int outer = add_outer(e, params, caller, callee, t_args);
        std::vector<u64> given;
        std::vector<int64_t> given_signed;
        for (int i = 0; i < params; i++) {
          given.push_back(static_cast<u64>(100 + 13 * i));
          given_signed.push_back(100 + 13 * i);
        }
        const int64_t via_tail = callee_ref(tail_args(params, t_args, given_signed));
        std::vector<int64_t> imm_args;
        for (int i = 0; i < t_args; i++) {
          imm_args.push_back(5 + 3 * i);
        }
        // The entry function tail-calls: its callee's return goes through the
        // adapter.
        Outcome direct = e.run_compiled(caller, given);
        ASSERT_TRUE(direct.finished);
        EXPECT_EQ(direct.exit, uint32_t{GRJIT_EXIT_RETURNED});
        EXPECT_EQ(static_cast<int64_t>(direct.value), via_tail);
        EXPECT_EQ(direct.frames_left, 0u);
        // And called from compiled code that carries on after it.
        Outcome o = e.run_compiled(outer, given);
        ASSERT_TRUE(o.finished);
        EXPECT_EQ(o.exit, uint32_t{GRJIT_EXIT_RETURNED});
        EXPECT_EQ(static_cast<int64_t>(o.value), via_tail + 31 + callee_ref(imm_args));
        EXPECT_FALSE(o.interpreted_rest);
        EXPECT_EQ(e.st.deopts, 0);
        EXPECT_EQ(e.st.tail_refusals, 0);
        EXPECT_EQ(e.st.tails, 2);
        EXPECT_EQ(e.st.pushes, e.st.pops);
        EXPECT_EQ(o.frames_left, 0u);
        // The interpreter agrees.
        Outcome i = e.run_interpreted(outer, given);
        ASSERT_TRUE(i.finished);
        EXPECT_EQ(i.value, o.value);
        pairs++;
      }
    }
  }
  EXPECT_EQ(pairs, 2 * 17 * 17);
}

TEST(Tail, ASelfRecursionAMillionDeepRunsInConstantNativeStackAndGuestDepth) {
  TAIL_ONLY_ON_X86_64_SYSV();
  Engine e(GRCORE_UNLIMITED, /*native_bytes=*/64 * 1024);
  int loop = add_countdown(e, kMillion);
  Outcome i = e.run_interpreted(loop, {static_cast<u64>(kMillion), 0});
  ASSERT_TRUE(i.finished);
  EXPECT_EQ(i.value, static_cast<u64>(kMillion) * (kMillion + 1) / 2);
  e.st = Stats{};
  Outcome c = e.run_compiled(loop, {static_cast<u64>(kMillion), 0});
  ASSERT_TRUE(c.finished);
  EXPECT_EQ(c.value, i.value) << "the C reference and the interpreter agree";
  EXPECT_EQ(c.exit, uint32_t{GRJIT_EXIT_RETURNED}) << "never left compiled code";
  EXPECT_FALSE(c.interpreted_rest);
  EXPECT_EQ(e.st.deopts, 0);
  EXPECT_EQ(e.st.tails, kMillion);
  EXPECT_EQ(e.st.tail_refusals, 0);
  ASSERT_EQ(e.st.sps.size(), 2u);
  EXPECT_EQ(e.st.sps[0], e.st.sps[1]) << "the native stack pointer, first and last";
  EXPECT_EQ(e.st.bases[0], e.st.bases[1]) << "and the frame base";
  EXPECT_EQ(e.st.saved[0], e.st.saved[1]);
  EXPECT_EQ(e.st.rets[0], e.st.rets[1]);
  EXPECT_EQ(e.st.frames[0], e.st.frames[1]) << "guest frames";
  EXPECT_EQ(e.st.depths[0], e.st.depths[1]) << "guest depth";
  EXPECT_EQ(e.st.depths[0], 1u) << "the one frame, counted (a depth that read as a budget would be vacuous)";
  EXPECT_EQ(e.st.caps[0], e.st.caps[1]) << "reservation capacity";
  EXPECT_EQ(e.st.pushes, e.st.pops);
  EXPECT_EQ(c.frames_left, 0u);
  // What constant means, for design.md: the figures at the first and the last probe.
  std::printf("tail: %lld tail calls in a %u-byte native budget: native sp %#llx -> %#llx, frame base "
              "%#llx -> %#llx, guest frames %zu -> %zu, guest depth %llu -> %llu, reservation "
              "capacity %zu -> %zu\n",
      static_cast<long long>(kMillion), 64 * 1024,
      static_cast<unsigned long long>(e.st.sps[0] & 0xFFF), static_cast<unsigned long long>(e.st.sps[1] & 0xFFF),
      static_cast<unsigned long long>(e.st.bases[0] & 0xFFF), static_cast<unsigned long long>(e.st.bases[1] & 0xFFF),
      e.st.frames[0], e.st.frames[1], static_cast<unsigned long long>(e.st.depths[0]),
      static_cast<unsigned long long>(e.st.depths[1]), e.st.caps[0], e.st.caps[1]);
}

/* ---- Mutual recursion, through slots and through code pointers ---------------- */

namespace {

/* even(n) = n == 0 ? 1 : odd(n - 1); odd(n) = n == 0 ? 0 : even(n - 1), each a
 * tail call to the other. `even` probes at the first iteration and the last. */
void add_even_odd(Engine & e, int64_t first, bool through_pointer, int * even_out, int * odd_out) {
  int even = e.reserve();
  int odd = e.reserve();
  {
    P p("odd", {GRJIT_TYPE_I64});
    int n = 0, z = p.local(), c = p.local(), one = p.local(), n1 = p.local(),
        ptr = p.local(GRJIT_TYPE_PTR);
    p.cnst(z, 0);
    p.bin(K::EQ, c, n, z);
    int more = p.brz(c);
    p.ret(z);
    p.patch(more, p.here());
    p.cnst(one, 1);
    p.bin(K::SUB, n1, n, one);
    if (through_pointer) {
      p.entryof(ptr, even);
      p.tailcallp(ptr, even, {n1});
    } else {
      p.tailcall(even, {n1});
    }
    e.set(odd, p.done());
  }
  {
    P p("even", {GRJIT_TYPE_I64});
    int n = 0, z = p.local(), k = p.local(), t = p.local(), c = p.local(), one = p.local(),
        n1 = p.local(), ptr = p.local(GRJIT_TYPE_PTR);
    p.cnst(z, 0);
    p.cnst(k, first);
    p.bin(K::EQ, t, n, k);
    int skip = p.brz(t);
    p.probe(n);
    p.patch(skip, p.here());
    p.bin(K::EQ, c, n, z);
    int more = p.brz(c);
    p.probe(n);
    p.cnst(one, 1);
    p.ret(one);
    p.patch(more, p.here());
    p.cnst(one, 1);
    p.bin(K::SUB, n1, n, one);
    if (through_pointer) {
      p.entryof(ptr, odd);
      p.tailcallp(ptr, odd, {n1});
    } else {
      p.tailcall(odd, {n1});
    }
    e.set(even, p.done());
  }
  *even_out = even;
  *odd_out = odd;
}

void expect_constant(const Engine & e, size_t probes) {
  ASSERT_EQ(e.st.sps.size(), probes);
  EXPECT_EQ(e.st.sps.front(), e.st.sps.back()) << "native stack pointer";
  EXPECT_EQ(e.st.bases.front(), e.st.bases.back()) << "frame base";
  EXPECT_EQ(e.st.saved.front(), e.st.saved.back()) << "the original caller's base, still";
  EXPECT_EQ(e.st.rets.front(), e.st.rets.back()) << "and its return address";
  EXPECT_EQ(e.st.frames.front(), e.st.frames.back()) << "guest frames";
  EXPECT_EQ(e.st.depths.front(), e.st.depths.back()) << "guest depth";
  EXPECT_GE(e.st.depths.front(), 1u) << "a depth that is counted: at least the one frame";
  EXPECT_LT(e.st.depths.front(), 1000u) << "and not a budget read as one";
  EXPECT_EQ(e.st.caps.front(), e.st.caps.back()) << "reservation capacity";
}

} // namespace

TEST(Tail, MutualRecursionAMillionDeepThroughSlotsAndThroughPointersRunsInConstantStack) {
  TAIL_ONLY_ON_X86_64_SYSV();
  for (int through_pointer = 0; through_pointer < 2; through_pointer++) {
    for (int64_t n : {kMillion, kMillion + 1}) {
      SCOPED_TRACE(testing::Message() << (through_pointer ? "pointer " : "slot ") << n);
      Engine e(GRCORE_UNLIMITED, 64 * 1024);
      int even, odd;
      add_even_odd(e, kMillion, through_pointer != 0, &even, &odd);
      ASSERT_TRUE(e.compile_fn(even));
      ASSERT_TRUE(e.compile_fn(odd)); // so every ENTRYOF has an entry to take
      Outcome o = e.run_compiled(even, {static_cast<u64>(n)});
      ASSERT_TRUE(o.finished);
      EXPECT_EQ(o.value, n % 2 == 0 ? 1u : 0u) << "the C reference: even";
      EXPECT_EQ(o.exit, uint32_t{GRJIT_EXIT_RETURNED});
      EXPECT_EQ(e.st.deopts, 0);
      EXPECT_EQ(e.st.tails, n);
      EXPECT_EQ(e.st.tail_refusals, 0);
      if (n == kMillion) {
        expect_constant(e, 2);
      }
      EXPECT_EQ(e.st.pushes, e.st.pops);
      EXPECT_EQ(o.frames_left, 0u);
    }
  }
}

namespace {

/* narrow(n, a) = n == 0 ? a : wide(n - 1, a + 1, 14 more arguments), and
 * wide(n, a, b0 .. b13) = narrow(n - 1, a + 1 + 1000003 * (the sum of how far
 * each b_k is from 100 + k)): the one is 2 parameters, the other 16, each tail
 * calling the other, so the incoming area widens and narrows on every call. */
void add_narrow_wide(Engine & e, int64_t first, int * narrow_out, int * wide_out) {
  int narrow = e.reserve();
  int wide = e.reserve();
  {
    P p("narrow", {GRJIT_TYPE_I64, GRJIT_TYPE_I64});
    int n = 0, a = 1, z = p.local(), k = p.local(), t = p.local(), c = p.local(),
        one = p.local(), n1 = p.local(), a1 = p.local();
    p.cnst(z, 0);
    p.cnst(k, first);
    p.bin(K::EQ, t, n, k);
    int skip = p.brz(t);
    p.probe(n);
    p.patch(skip, p.here());
    p.bin(K::EQ, c, n, z);
    int more = p.brz(c);
    p.probe(n);
    p.ret(a);
    p.patch(more, p.here());
    p.cnst(one, 1);
    p.bin(K::SUB, n1, n, one);
    p.bin(K::ADD, a1, a, one);
    std::vector<int> args = {n1, a1};
    for (int i = 0; i < 14; i++) {
      args.push_back(p.imm(100 + i));
    }
    p.tailcall(wide, args);
    e.set(narrow, p.done());
  }
  {
    std::vector<GRJIT_Type> types(16, GRJIT_TYPE_I64);
    P p("wide", types);
    int n = 0, a = 1, c = p.local(), t = p.local(), dev = p.local(), m = p.local(),
        one = p.local(), n1 = p.local(), a1 = p.local();
    p.cnst(dev, 0);
    for (int i = 0; i < 14; i++) {
      p.cnst(c, 100 + i);
      p.bin(K::SUB, t, 2 + i, c);
      p.bin(K::ADD, dev, dev, t);
    }
    p.cnst(m, 1000003);
    p.bin(K::MUL, dev, dev, m);
    p.cnst(one, 1);
    p.bin(K::SUB, n1, n, one);
    p.bin(K::ADD, a1, a, one);
    p.bin(K::ADD, a1, a1, dev);
    p.tailcall(narrow, {n1, a1});
    e.set(wide, p.done());
  }
  *narrow_out = narrow;
  *wide_out = wide;
}

} // namespace

TEST(Tail, APingPongBetweenANarrowAndAWideFunctionAMillionDeepLeavesTheStackWhereItWas) {
  TAIL_ONLY_ON_X86_64_SYSV();
  Engine e(GRCORE_UNLIMITED, 64 * 1024);
  int narrow, wide;
  add_narrow_wide(e, kMillion, &narrow, &wide);
  Outcome i = e.run_interpreted(narrow, {static_cast<u64>(kMillion), 0});
  ASSERT_TRUE(i.finished);
  EXPECT_EQ(i.value, static_cast<u64>(kMillion)) << "every argument intact: nothing but the +1 steps";
  e.st = Stats{};
  Outcome o = e.run_compiled(narrow, {static_cast<u64>(kMillion), 0});
  ASSERT_TRUE(o.finished);
  EXPECT_EQ(o.value, static_cast<u64>(kMillion)) << "a clobbered argument shows as a large deviation";
  EXPECT_EQ(o.exit, uint32_t{GRJIT_EXIT_RETURNED});
  EXPECT_EQ(e.st.deopts, 0);
  EXPECT_EQ(e.st.tails, kMillion);
  expect_constant(e, 2);
  EXPECT_EQ(e.st.pushes, e.st.pops);
}

/* ---- The forms: an empty slot, a refused slot, a pointer in a register or an immediate ---- */

namespace {

/* inc2(x) = x + 2, and a function that returns inc2(x) by a tail call. */
int add_inc2(Engine & e) {
  P p("inc2", {GRJIT_TYPE_I64});
  int two = p.local(), r = p.local();
  p.cnst(two, 2);
  p.bin(K::ADD, r, 0, two);
  p.ret(r);
  return e.add(p.done());
}

int add_tail_to(Engine & e, int target) {
  P p("tail_to", {GRJIT_TYPE_I64});
  p.tailcall(target, {0});
  return e.add(p.done());
}

} // namespace

TEST(Tail, AnEmptySlotIsCompiledAtTheTailCallAndEnteredDirectlyAfterwards) {
  TAIL_ONLY_ON_X86_64_SYSV();
  Engine e;
  int target = add_inc2(e);
  int top = add_tail_to(e, target);
  ASSERT_TRUE(e.compile_fn(top));
  EXPECT_EQ(e.st.compiles, 1) << "only the caller so far";
  EXPECT_EQ(e.code_of(target).handle, nullptr);
  Outcome o = e.run_compiled(top, {40});
  ASSERT_TRUE(o.finished);
  EXPECT_EQ(o.value, 42u);
  EXPECT_EQ(o.exit, uint32_t{GRJIT_EXIT_RETURNED});
  EXPECT_EQ(e.st.compile_calls, 1) << "compiled at the tail call";
  EXPECT_NE(e.code_of(target).handle, nullptr);
  Outcome again = e.run_compiled(top, {50});
  ASSERT_TRUE(again.finished);
  EXPECT_EQ(again.value, 52u);
  EXPECT_EQ(e.st.compile_calls, 1) << "and directly after: the slot is full";
  EXPECT_EQ(e.st.deopts, 0);
}

TEST(Tail, ASlotThatCannotBeFilledIsAnExitRememberedAndTheInterpreterMakesTheTailCall) {
  TAIL_ONLY_ON_X86_64_SYSV();
  Engine e;
  int target = add_inc2(e);
  int top = add_tail_to(e, target);
  e.uncompilable.insert(target);
  for (int round = 1; round <= 3; round++) {
    SCOPED_TRACE(round);
    Outcome o = e.run_compiled(top, {40});
    ASSERT_TRUE(o.finished);
    EXPECT_EQ(o.value, 42u) << "the interpreted verdict";
    EXPECT_EQ(o.exit, uint32_t{GRJIT_EXIT_DEOPT});
    EXPECT_EQ(e.st.compile_calls, 1) << "asked once, and the refusal remembered in the slot";
    EXPECT_EQ(e.st.deopts, round);
    EXPECT_EQ(e.st.tails, 0) << "the hook is never reached: the exit is before it";
    EXPECT_TRUE(e.st.last_was_call_exit);
    EXPECT_EQ(e.st.deopt_frames, round);
    EXPECT_EQ(o.frames_left, 0u);
  }
  EXPECT_TRUE(e.st.counted.empty()) << "an exit at a tail site is not the caller's fault";
}

TEST(Tail, AHookThatSaysItInstalledButLeftTheSlotEmptyIsAnExitNotAnEntry) {
  TAIL_ONLY_ON_X86_64_SYSV();
  Engine e;
  int target = add_inc2(e);
  int top = add_tail_to(e, target);
  e.lie_about_installing = true;
  Outcome o = e.run_compiled(top, {40});
  ASSERT_TRUE(o.finished);
  EXPECT_EQ(o.value, 42u);
  EXPECT_EQ(o.exit, uint32_t{GRJIT_EXIT_DEOPT});
  EXPECT_EQ(e.st.tails, 0);
}

TEST(Tail, APointerInARegisterOrAnImmediateIsEnteredAndTheResultIsTheInterpreters) {
  TAIL_ONLY_ON_X86_64_SYSV();
  Engine e;
  int target = add_inc2(e);
  ASSERT_TRUE(e.compile_fn(target));
  int via_reg = e.reserve();
  {
    P p("via_reg", {GRJIT_TYPE_I64});
    int ptr = p.local(GRJIT_TYPE_PTR);
    p.entryof(ptr, target);
    p.tailcallp(ptr, target, {0});
    e.set(via_reg, p.done());
  }
  int via_imm = e.reserve();
  {
    P p("via_imm", {GRJIT_TYPE_I64});
    p.tailcallp(p.imm(static_cast<int64_t>(e.code_of(target).internal)), target, {0});
    e.set(via_imm, p.done());
  }
  for (int f : {via_reg, via_imm}) {
    SCOPED_TRACE(f);
    long tails = e.st.tails;
    Outcome i = e.run_interpreted(f, {40});
    Outcome o = e.run_compiled(f, {40});
    ASSERT_TRUE(o.finished);
    EXPECT_EQ(o.value, 42u);
    EXPECT_EQ(o.value, i.value);
    EXPECT_EQ(o.exit, uint32_t{GRJIT_EXIT_RETURNED});
    EXPECT_EQ(e.st.tails - tails, 1);
  }
  EXPECT_EQ(e.st.deopts, 0);
}

/* ---- A target that is not what the call means is refused and never entered ------ */

namespace {

/* A function that tail-calls the code of `target` through a pointer, naming
 * `token` as the callee and passing `n` arguments. */
int add_tail_ptr_caller(Engine & e, int target, int token, int n) {
  int f = e.reserve();
  P p("tail_via_ptr", {GRJIT_TYPE_I64});
  int ptr = p.local(GRJIT_TYPE_PTR);
  std::vector<int> args(static_cast<size_t>(n), 0);
  p.entryof(ptr, target);
  p.tailcallp(ptr, token, args);
  e.set(f, p.done());
  return f;
}

} // namespace

TEST(Tail, APointerToAnotherFunctionOrOfAnotherArityIsRefusedAtTheTailSiteAndNeverEntered) {
  TAIL_ONLY_ON_X86_64_SYSV();
  Engine e;
  int c2 = add_callee(e, 2);
  int c8 = add_callee(e, 8);
  int other = e.reserve();
  {
    P p("other", {GRJIT_TYPE_I64, GRJIT_TYPE_I64});
    int t = p.local(), k = p.local(), r = p.local();
    p.bin(K::ADD, t, 0, 1);
    p.cnst(k, 1000);
    p.bin(K::ADD, r, t, k);
    p.ret(r);
    e.set(other, p.done());
  }
  ASSERT_TRUE(e.compile_fn(c2));
  ASSERT_TRUE(e.compile_fn(c8));
  ASSERT_TRUE(e.compile_fn(other));
  struct Case {
    const char * name;
    int target, token, n;
    bool enters;
  };
  const Case cases[] = {
      {"the right function with the right count", c2, c2, 2, true},
      {"another function of the same parameter count", other, c2, 2, false},
      {"a function of eight parameters called as the one of two", c8, c2, 2, false},
      {"the right token but two arguments for eight parameters", c8, c8, 2, false},
      {"the right token but eight arguments for two parameters", c2, c2, 8, false},
  };
  for (const Case & c : cases) {
    SCOPED_TRACE(c.name);
    int f = add_tail_ptr_caller(e, c.target, c.token, c.n);
    long deopts = e.st.deopts;
    long tails = e.st.tails;
    Outcome i = e.run_interpreted(f, {5});
    Outcome o = e.run_compiled(f, {5});
    ASSERT_TRUE(o.finished) << "no fault, and the interpreter's answer";
    EXPECT_EQ(o.value, i.value);
    EXPECT_EQ(o.exit, c.enters ? uint32_t{GRJIT_EXIT_RETURNED} : uint32_t{GRJIT_EXIT_DEOPT});
    EXPECT_EQ(e.st.deopts - deopts, c.enters ? 0 : 1);
    EXPECT_EQ(e.st.tails - tails, c.enters ? 1 : 0) << "the hook is never reached for a refused target";
    EXPECT_EQ(e.st.rebuild, GRCORE_OK);
  }
  // A pointer that is no compiled code at all, in a register and as an immediate.
  for (int imm = 0; imm < 2; imm++) {
    SCOPED_TRACE(imm);
    int f = e.reserve();
    P p("junk", {GRJIT_TYPE_I64});
    static uint64_t junk[8] = {0};
    if (imm) {
      p.tailcallp(p.imm(static_cast<int64_t>(reinterpret_cast<uintptr_t>(&junk[4]))), c2, {0, 0});
    } else {
      int ptr = p.local(GRJIT_TYPE_PTR);
      p.cnst(ptr, static_cast<int64_t>(reinterpret_cast<uintptr_t>(&junk[4])));
      p.tailcallp(ptr, c2, {0, 0});
    }
    e.set(f, p.done());
    long tails = e.st.tails;
    Outcome o = e.run_compiled(f, {5});
    ASSERT_TRUE(o.finished);
    EXPECT_EQ(o.exit, uint32_t{GRJIT_EXIT_DEOPT});
    EXPECT_EQ(e.st.tails, tails);
  }
}

TEST(Tail, APointerIntoCodeThatWasRetiredIsRefusedAtTheTailSiteEvenWhileItStillMaps) {
  TAIL_ONLY_ON_X86_64_SYSV();
  Engine e;
  int target = add_inc2(e);
  ASSERT_TRUE(e.compile_fn(target));
  int top = e.reserve();
  {
    P p("stale", {GRJIT_TYPE_I64});
    int ptr = p.local(GRJIT_TYPE_PTR);
    p.entryof(ptr, target);
    p.clearslot(target);
    p.tailcallp(ptr, target, {0});
    e.set(top, p.done());
  }
  Outcome o = e.run_compiled(top, {40});
  ASSERT_TRUE(o.finished);
  EXPECT_EQ(o.value, 42u);
  EXPECT_EQ(o.exit, uint32_t{GRJIT_EXIT_DEOPT}) << "refused at the tail site, finished by the interpreter";
  EXPECT_EQ(e.st.tails, 0);
  EXPECT_EQ(*e.released, 1) << "and released once the activation left";
}

TEST(Tail, ASlotClearedBeforeTheTailCallIsCompiledAgainAtTheCall) {
  TAIL_ONLY_ON_X86_64_SYSV();
  Engine e;
  int target = add_inc2(e);
  ASSERT_TRUE(e.compile_fn(target));
  int top = e.reserve();
  {
    P p("cleared", {GRJIT_TYPE_I64});
    p.clearslot(target);
    p.tailcall(target, {0});
    e.set(top, p.done());
  }
  Outcome o = e.run_compiled(top, {40});
  ASSERT_TRUE(o.finished);
  EXPECT_EQ(o.value, 42u);
  EXPECT_EQ(o.exit, uint32_t{GRJIT_EXIT_RETURNED});
  EXPECT_EQ(e.st.compile_calls, 1);
  EXPECT_EQ(e.st.tails, 1);
}

/* ---- A guard, a poll or a pause in a function a tail call entered -------------- */

namespace {

/* tchain(n, acc) = acc + n + (n-1) + ... + 1, by tail calls to itself, a guard
 * that fails at n == fail_at (-1: never) and, optionally, a poll, before the
 * test of n. */
int add_tchain(Engine & e, int64_t fail_at, bool with_poll = false, bool through_pointer = false) {
  int t = e.reserve();
  P p("tchain", {GRJIT_TYPE_I64, GRJIT_TYPE_I64});
  int n = 0, acc = 1, zero = p.local(), one = p.local(), k = p.local(), hit = p.local(),
      ok = p.local(), c = p.local(), n1 = p.local(), a1 = p.local(),
      ptr = p.local(GRJIT_TYPE_PTR);
  p.cnst(zero, 0);
  p.cnst(one, 1);
  p.cnst(k, fail_at);
  p.bin(K::EQ, hit, n, k);
  p.bin(K::EQ, ok, hit, zero); // zero exactly where the guard must fail
  p.guard(ok);
  if (with_poll) {
    p.poll();
  }
  p.bin(K::EQ, c, n, zero);
  int more = p.brz(c);
  p.ret(acc);
  p.patch(more, p.here());
  p.bin(K::SUB, n1, n, one);
  p.bin(K::ADD, a1, acc, n);
  if (through_pointer) {
    p.entryof(ptr, t); // its own entry: compiled before it runs
    p.tailcallp(ptr, t, {n1, a1});
  } else {
    p.tailcall(t, {n1, a1});
  }
  e.set(t, p.done());
  return t;
}

/* top(n) = callee(n, 0) + 7: a call that the tail chain returns into. */
int add_top(Engine & e, int callee, const char * name = "top") {
  P p(name, {GRJIT_TYPE_I64});
  int z = p.local(), r = p.local(), k = p.local(), s = p.local();
  p.cnst(z, 0);
  p.call(r, callee, {0, z});
  p.cnst(k, 7);
  p.bin(K::ADD, s, r, k);
  p.ret(s);
  return e.add(p.done());
}

uint64_t tsum(int64_t n) { return static_cast<uint64_t>(n) * (n + 1) / 2 + 7; }

} // namespace

TEST(Tail, AGuardInTheFirstOperationOfAFunctionATailCallEnteredRebuildsTheChainAndTheInterpreterFinishes) {
  TAIL_ONLY_ON_X86_64_SYSV();
  for (int through_pointer = 0; through_pointer < 2; through_pointer++) {
    SCOPED_TRACE(through_pointer ? "pointer" : "slot");
    Engine e;
    int tc = add_tchain(e, /*fail_at=*/19, false, through_pointer != 0);
    int top = add_top(e, tc);
    ASSERT_TRUE(e.compile_fn(tc));
    Outcome i = e.run_interpreted(top, {20});
    ASSERT_TRUE(i.finished);
    EXPECT_EQ(i.value, tsum(20));
    e.st = Stats{};
    Outcome o = e.run_compiled(top, {20});
    ASSERT_TRUE(o.finished) << "failed=" << o.failed;
    EXPECT_EQ(o.value, i.value) << "the uninterrupted output";
    EXPECT_EQ(o.exit, uint32_t{GRJIT_EXIT_DEOPT});
    EXPECT_TRUE(o.interpreted_rest);
    EXPECT_EQ(e.st.tails, 1) << "n = 20 passed its guard and tail-called n = 19, whose first operation failed";
    EXPECT_EQ(e.st.deopts, 1);
    EXPECT_EQ(e.st.deopt_frames, 2) << "top, waiting at its call, and the callee, in place of n = 20";
    EXPECT_EQ(e.st.rebuild, GRCORE_OK);
    EXPECT_EQ(o.frames_left, 0u);
    EXPECT_EQ(e.st.pushes, e.st.pops);
  }
}

TEST(Tail, AGuardAfterAHundredThousandTailCallsFindsExactlyTheCallersAndTheCalleeInTheChain) {
  TAIL_ONLY_ON_X86_64_SYSV();
  const int64_t n = 100010;
  for (int through_pointer = 0; through_pointer < 2; through_pointer++) {
    SCOPED_TRACE(through_pointer ? "pointer" : "slot");
    Engine e;
    int tc = add_tchain(e, /*fail_at=*/10, false, through_pointer != 0);
    int mid = add_top(e, tc, "mid");
    int top = add_top(e, mid);
    ASSERT_TRUE(e.compile_fn(tc));
    Outcome i = e.run_interpreted(top, {static_cast<u64>(n)});
    ASSERT_TRUE(i.finished);
    e.st = Stats{};
    Outcome o = e.run_compiled(top, {static_cast<u64>(n)});
    ASSERT_TRUE(o.finished) << "failed=" << o.failed;
    EXPECT_EQ(o.value, i.value);
    EXPECT_EQ(o.value, static_cast<u64>(n) * (n + 1) / 2 + 14);
    EXPECT_EQ(o.exit, uint32_t{GRJIT_EXIT_DEOPT});
    EXPECT_EQ(e.st.tails, n - 10) << "a hundred thousand tail calls before the guard failed";
    EXPECT_EQ(e.st.deopt_frames, 3) << "top, mid and the one frame every tail call replaced";
    EXPECT_EQ(e.st.rebuild, GRCORE_OK);
    EXPECT_EQ(o.frames_left, 0u) << "a replaced frame left in the stack would show here";
    EXPECT_EQ(e.st.pushes, e.st.pops);
  }
}

TEST(Tail, APollThatPausesInAFunctionATailCallEnteredCarriesTheAnswerAndRebuildsTheChain) {
  TAIL_ONLY_ON_X86_64_SYSV();
  for (int through_pointer = 0; through_pointer < 2; through_pointer++) {
    SCOPED_TRACE(through_pointer ? "pointer" : "slot");
    Engine e;
    int tc = add_tchain(e, /*fail_at=*/-1, /*with_poll=*/true, through_pointer != 0);
    int top = add_top(e, tc);
    ASSERT_TRUE(e.compile_fn(tc));
    uint64_t * request = reinterpret_cast<uint64_t *>(
        reinterpret_cast<unsigned char *>(e.ctx) + grcore_jit_layout()->request_word_offset);
    *request = 1;
    e.poll_slow_calls_to_fail = 5;
    Outcome o = e.run_compiled(top, {30});
    *request = 0;
    ASSERT_TRUE(o.finished);
    EXPECT_EQ(o.value, tsum(30));
    EXPECT_EQ(o.exit, uint32_t{GRJIT_EXIT_DEOPT});
    EXPECT_EQ(e.st.poll_slow, 5);
    EXPECT_EQ(e.st.tails, 4) << "four tail calls, and the fifth iteration's poll paused";
    EXPECT_EQ(e.st.last_cause, 1u) << "the helper's answer is the cause";
    EXPECT_EQ(e.st.deopt_frames, 2);
    EXPECT_EQ(e.st.rebuild, GRCORE_OK);
    EXPECT_EQ(o.frames_left, 0u);
  }
}

TEST(Tail, ARebuildTheEngineRefusesAtATailSitesExitIsTheFatalExitWithNothingWritten) {
  TAIL_ONLY_ON_X86_64_SYSV();
  for (int through_pointer = 0; through_pointer < 2; through_pointer++) {
    for (int refuse : {0, 1}) {
      SCOPED_TRACE(testing::Message() << (through_pointer ? "pointer " : "slot ") << refuse);
      Engine e;
      int tc = add_tchain(e, -1, false, through_pointer != 0);
      int top = add_top(e, tc);
      ASSERT_TRUE(e.compile_fn(tc));
      e.refuse_tail_at = 3; // an exit at the third tail site
      e.refuse_rebuild_with = refuse ? 77u : 0u;
      Outcome o = e.run_compiled(top, {10});
      if (!refuse) {
        ASSERT_TRUE(o.finished);
        EXPECT_EQ(o.value, tsum(10));
        EXPECT_FALSE(o.rebuild_failed);
        continue;
      }
      EXPECT_TRUE(o.rebuild_failed);
      EXPECT_FALSE(o.finished) << "the interpreter is not run on frames nothing rebuilt";
      EXPECT_EQ(o.exit, uint32_t{GRJIT_EXIT_REBUILD_FAILED});
      EXPECT_EQ(o.failed_with, 77u);
      EXPECT_EQ(e.st.deopts, 1);
      ASSERT_EQ(o.pcs_after_failure.size(), 2u) << "top and the one frame the tail calls replaced";
      for (u64 pc : o.pcs_after_failure) {
        EXPECT_EQ(pc, 0u) << "no guest frame was written";
      }
      EXPECT_EQ(grcore_stack_frame_count(e.stack), 0u);
    }
  }
}

TEST(Tail, EveryTailCallReplacesTheReservationExtensionSoARebuildNeverFailsAndOneCellShortIsRefused) {
  TAIL_ONLY_ON_X86_64_SYSV();
  for (long short_by : {0L, 1L}) {
    SCOPED_TRACE(short_by);
    Engine e(GRCORE_UNLIMITED, GRCORE_UNLIMITED, /*conv=*/true);
    int tc = add_tchain(e, /*fail_at=*/0);
    int top = add_top(e, tc);
    ASSERT_TRUE(e.compile_fn(tc));
    ASSERT_GT(e.code_of(tc).max_converting, 0u);
    e.short_by = short_by;
    Outcome o = e.run_compiled(top, {49});
    if (short_by == 0) {
      ASSERT_TRUE(o.finished);
      EXPECT_EQ(o.value, tsum(49));
      EXPECT_EQ(e.st.rebuild, GRCORE_OK) << "after 49 tail calls the extension is one callee's, not 49";
      EXPECT_EQ(e.st.deopt_frames, 2);
      EXPECT_FALSE(o.rebuild_failed);
    } else {
      EXPECT_EQ(e.st.rebuild, GRCORE_ERR_INVALID);
      EXPECT_TRUE(o.rebuild_failed);
      ASSERT_EQ(o.pcs_after_failure.size(), 2u);
      for (u64 pc : o.pcs_after_failure) {
        EXPECT_EQ(pc, 0u) << "not one guest frame was written";
      }
    }
  }
}

/* ---- The hook refuses ------------------------------------------------------------- */

TEST(Tail, AFrameThatOwnsAScopeRefusesItsTailCallThroughAnExitAndTheInterpreterMakesItAsACall) {
  TAIL_ONLY_ON_X86_64_SYSV();
  Engine e;
  int tc = add_tchain(e, -1);
  int scoped = e.reserve();
  {
    P p("scoped", {GRJIT_TYPE_I64, GRJIT_TYPE_I64});
    p.tailcall(tc, {0, 1});
    Func f = p.done();
    f.owns_scope = true;
    e.set(scoped, f);
  }
  int top = add_top(e, scoped);
  Outcome i = e.run_interpreted(top, {25});
  ASSERT_TRUE(i.finished);
  EXPECT_EQ(i.value, tsum(25));
  e.st = Stats{};
  Outcome o = e.run_compiled(top, {25});
  ASSERT_TRUE(o.finished);
  EXPECT_EQ(o.value, i.value) << "the interpreted verdict";
  EXPECT_EQ(o.exit, uint32_t{GRJIT_EXIT_DEOPT});
  EXPECT_EQ(e.st.tails, 1);
  EXPECT_EQ(e.st.tail_refusals, 1);
  EXPECT_TRUE(e.st.last_was_call_exit);
  EXPECT_TRUE(e.st.counted.empty()) << "not counted against the caller's discard limit";
  EXPECT_EQ(e.st.deopt_frames, 2) << "top and the scope-owning frame, which is still there";
  EXPECT_EQ(e.st.rebuild, GRCORE_OK);
  EXPECT_EQ(o.frames_left, 0u);
  EXPECT_EQ(e.st.pushes, e.st.pops);
  // The control: the same program, not owning a scope, is a tail call all the way.
  Engine e2;
  int tc2 = add_tchain(e2, -1);
  int plain = e2.reserve();
  {
    P p("plain", {GRJIT_TYPE_I64, GRJIT_TYPE_I64});
    p.tailcall(tc2, {0, 1});
    e2.set(plain, p.done());
  }
  int top2 = add_top(e2, plain);
  Outcome c = e2.run_compiled(top2, {25});
  ASSERT_TRUE(c.finished);
  EXPECT_EQ(c.value, tsum(25));
  EXPECT_EQ(c.exit, uint32_t{GRJIT_EXIT_RETURNED});
  EXPECT_EQ(e2.st.tail_refusals, 0);
}

TEST(Tail, TheNthTailCallRefusedIsAnExitThatIsNotCountedAndTheInterpreterReachesTheSameValue) {
  TAIL_ONLY_ON_X86_64_SYSV();
  for (long nth : {1L, 2L, 5L, 19L}) {
    SCOPED_TRACE(nth);
    Engine e;
    int tc = add_tchain(e, -1);
    int top = add_top(e, tc);
    e.refuse_tail_at = nth;
    Outcome o = e.run_compiled(top, {20});
    ASSERT_TRUE(o.finished);
    EXPECT_EQ(o.value, tsum(20));
    EXPECT_EQ(o.exit, uint32_t{GRJIT_EXIT_DEOPT});
    EXPECT_EQ(e.st.tails, nth);
    EXPECT_EQ(e.st.tail_refusals, 1);
    EXPECT_TRUE(e.st.last_was_call_exit);
    EXPECT_TRUE(e.st.counted.empty());
    EXPECT_EQ(e.st.deopt_frames, 2);
    EXPECT_EQ(o.frames_left, 0u);
  }
}

TEST(Tail, AReservationExtensionTheAllocatorRefusesIsAnExitAtTheTailSiteAndNothingIsHalfDone) {
  TAIL_ONLY_ON_X86_64_SYSV();
  TrackingAllocator tracker;
  Engine e(GRCORE_UNLIMITED, GRCORE_UNLIMITED, /*conv=*/true, tracker.get());
  int tc = add_tchain(e, -1);
  int top = add_top(e, tc);
  ASSERT_TRUE(e.compile_fn(tc));
  ASSERT_GT(e.code_of(tc).max_converting, 0u);
  e.refuse_extend_with = &tracker;
  // The reservation grows by doubling, so only some extensions allocate: sweep
  // which one is refused, and require that some tail call's was.
  int refused_tail = 0;
  for (long at = 1; at <= 6; at++) {
    SCOPED_TRACE(at);
    e.refuse_extend_at = at;
    e.extend_calls = 0;
    long refusals = e.st.tail_refusals;
    long pushes_refused = e.st.refused_pushes;
    long frames_before = e.st.deopt_frames;
    Outcome o = e.run_compiled(top, {20});
    ASSERT_TRUE(o.finished);
    EXPECT_EQ(o.value, tsum(20)) << "the interpreter made the call and finished the run";
    EXPECT_EQ(e.st.rebuild, GRCORE_OK);
    EXPECT_EQ(o.frames_left, 0u);
    if (e.st.tail_refusals > refusals) {
      refused_tail++;
      EXPECT_EQ(o.exit, uint32_t{GRJIT_EXIT_DEOPT});
      EXPECT_NE(e.st.extend_result, GRCORE_OK);
      EXPECT_EQ(e.st.deopt_frames - frames_before, 2) << "top and the caller the hook left as it was";
    } else if (e.st.refused_pushes > pushes_refused) {
      EXPECT_EQ(o.exit, uint32_t{GRJIT_EXIT_DEOPT});
    } else {
      EXPECT_EQ(o.exit, uint32_t{GRJIT_EXIT_RETURNED});
    }
    EXPECT_EQ(e.st.pushes, e.st.pops);
  }
  EXPECT_GE(refused_tail, 1) << "an extension made by a tail hook was refused, and that is the case";
}

TEST(Tail, ARefusedTailHookCostsTheCallerNothingAgainstItsDiscardLimitButAGuardDoes) {
  TAIL_ONLY_ON_X86_64_SYSV();
  Engine e;
  e.discard_limit = 3;
  int tc = add_tchain(e, -1);
  int top = add_top(e, tc);
  e.refuse_tail_at = 1;
  for (int round = 0; round < 5; round++) {
    e.tail_calls = 0;
    Outcome o = e.run_compiled(top, {10});
    ASSERT_TRUE(o.finished);
    EXPECT_EQ(o.value, tsum(10));
    EXPECT_EQ(o.exit, uint32_t{GRJIT_EXIT_DEOPT}) << "the code is still compiled: never discarded";
  }
  EXPECT_TRUE(e.never_compile.empty());
  EXPECT_TRUE(e.st.counted.empty());
  EXPECT_EQ(e.st.tail_refusals, 5);
}

/* ---- Guest depth: counted as the interpreter's tail call counts it -------------- */

namespace {

/* rec(n) = n + rec(n - 1), not a tail call: the control. */
int add_plain_rec(Engine & e) {
  int r = e.reserve();
  P p("plain_rec", {GRJIT_TYPE_I64});
  int zero = p.local(), one = p.local(), c = p.local(), n1 = p.local(), v = p.local(),
      sum = p.local();
  p.cnst(zero, 0);
  p.cnst(one, 1);
  p.bin(K::EQ, c, 0, zero);
  int more = p.brz(c);
  p.ret(zero);
  p.patch(more, p.here());
  p.bin(K::SUB, n1, 0, one);
  p.call(v, r, {n1});
  p.bin(K::ADD, sum, v, 0);
  p.ret(sum);
  e.set(r, p.done());
  return r;
}

} // namespace

TEST(Tail, ATailRecursionAMillionDeepFitsADepthLimitOfOneHundredAndANonTailOneHitsItAtTheSameDepthInBothTiers) {
  TAIL_ONLY_ON_X86_64_SYSV();
  Engine e(/*guest_depth=*/100);
  int loop = add_countdown(e, kMillion);
  Outcome i = e.run_interpreted(loop, {static_cast<u64>(kMillion), 0});
  ASSERT_TRUE(i.finished) << "the interpreter's tail call does not grow the depth";
  e.st = Stats{};
  Outcome c = e.run_compiled(loop, {static_cast<u64>(kMillion), 0});
  ASSERT_TRUE(c.finished);
  EXPECT_EQ(c.value, i.value);
  EXPECT_EQ(c.exit, uint32_t{GRJIT_EXIT_RETURNED}) << "no exit: finished as interpreted would";
  EXPECT_FALSE(c.limit);
  EXPECT_EQ(e.st.tails, kMillion);
  // The non-tail recursion hits the limit, at the same depth, in both tiers.
  int rec = add_plain_rec(e);
  Outcome ri = e.run_interpreted(rec, {1000});
  ASSERT_TRUE(ri.limit);
  Outcome rc = e.run_compiled(rec, {1000});
  EXPECT_TRUE(rc.limit);
  EXPECT_EQ(rc.limit_depth, ri.limit_depth);
  EXPECT_EQ(ri.limit_depth, 100u);
}

/* ---- Native stack, in bytes ---------------------------------------------------------- */

namespace {

/* How many bytes of native budget fit the first frame of `fn`'s code exactly: the
 * budget is measured from the stack pointer a run starts at, which a first run
 * under a generous budget reveals together with where its first frame was. */
uint64_t exact_budget(const std::function<int(Engine &)> & build, const std::vector<u64> & args) {
  const uint64_t generous = 1 << 20;
  Engine e(GRCORE_UNLIMITED, generous);
  int fn = build(e);
  Outcome o = e.run_compiled(fn, args);
  EXPECT_TRUE(o.finished);
  EXPECT_FALSE(e.st.bases.empty());
  const uintptr_t sp0 = e.last_native_limit + generous;
  const uintptr_t frame = grjit_code_meta(e.code_of(fn).code)->frame_bytes;
  return sp0 - (e.st.bases.front() - frame);
}

} // namespace

TEST(Tail, ABudgetThatFitsTheFirstFrameExactlyNeverDeoptimizesAMillionTailCallsAndOneByteLessDoes) {
  TAIL_ONLY_ON_X86_64_SYSV();
  auto build = [](Engine & e) { return add_countdown(e, kMillion); };
  const uint64_t exact = exact_budget(build, {static_cast<u64>(kMillion), 0});
  {
    Engine e(GRCORE_UNLIMITED, exact);
    int loop = build(e);
    Outcome o = e.run_compiled(loop, {static_cast<u64>(kMillion), 0});
    ASSERT_TRUE(o.finished);
    EXPECT_EQ(o.exit, uint32_t{GRJIT_EXIT_RETURNED}) << "the first frame fits, so every one does";
    EXPECT_EQ(e.st.deopts, 0);
    EXPECT_EQ(e.st.tails, kMillion);
  }
  {
    // The control: a frame's width less, the entry itself does not fit.
    Engine e(GRCORE_UNLIMITED, exact - 16);
    int loop = build(e);
    Outcome o = e.run_compiled(loop, {static_cast<u64>(kMillion), 0});
    ASSERT_TRUE(o.finished);
    EXPECT_EQ(o.exit, uint32_t{GRJIT_EXIT_DEOPT});
    EXPECT_EQ(e.st.deopts, 1);
  }
}

namespace {

/* small(n) tail-calls big(n), which has the same single parameter and many
 * locals, so a much larger frame; top(n) calls small. `big_locals` of them. */
int add_small_big(Engine & e, int big_locals, int * small_out) {
  int big = e.reserve();
  {
    P p("big", {GRJIT_TYPE_I64});
    int acc = p.local();
    std::vector<int> ls;
    for (int i = 0; i < big_locals; i++) {
      ls.push_back(p.local());
    }
    p.mov(acc, 0);
    for (int l : ls) {
      p.cnst(l, 1);
      p.bin(K::ADD, acc, acc, l);
    }
    p.ret(acc);
    e.set(big, p.done());
  }
  int small = e.reserve();
  {
    P p("small", {GRJIT_TYPE_I64});
    p.probe(0);
    p.tailcall(big, {0});
    e.set(small, p.done());
  }
  *small_out = small;
  P p("top", {GRJIT_TYPE_I64});
  int r = p.local();
  p.call(r, small, {0});
  p.ret(r);
  return e.add(p.done());
}

} // namespace

TEST(Tail, ACalleeWhoseFrameDoesNotFitDeoptimizesOnceAtItsPrologueAndTheInterpreterFinishes) {
  TAIL_ONLY_ON_X86_64_SYSV();
  // The budget is exactly small's first frame.
  uint64_t budget = 0;
  {
    Engine probe(GRCORE_UNLIMITED, 1 << 20);
    int small;
    int top = add_small_big(probe, 40, &small);
    Outcome o = probe.run_compiled(top, {5});
    ASSERT_TRUE(o.finished);
    ASSERT_FALSE(probe.st.bases.empty());
    const uintptr_t sp0 = probe.last_native_limit + (1 << 20);
    const uintptr_t frame = grjit_code_meta(probe.code_of(small).code)->frame_bytes;
    budget = sp0 - (probe.st.bases.front() - frame);
  }
  for (int big_locals : {0, 40}) {
    SCOPED_TRACE(big_locals);
    Engine e(GRCORE_UNLIMITED, budget);
    int small;
    int top = add_small_big(e, big_locals, &small);
    Outcome i = e.run_interpreted(top, {5});
    ASSERT_TRUE(i.finished);
    e.st = Stats{};
    Outcome o = e.run_compiled(top, {5});
    ASSERT_TRUE(o.finished) << "never a fault";
    EXPECT_EQ(o.value, i.value);
    EXPECT_EQ(o.value, static_cast<u64>(5 + big_locals));
    if (big_locals == 0) {
      // A callee whose frame is no larger than the caller's passes whenever the
      // caller did.
      EXPECT_EQ(o.exit, uint32_t{GRJIT_EXIT_RETURNED});
      EXPECT_EQ(e.st.deopts, 0);
    } else {
      EXPECT_EQ(o.exit, uint32_t{GRJIT_EXIT_DEOPT});
      EXPECT_EQ(e.st.deopts, 1) << "once, at the callee's prologue";
      EXPECT_EQ(e.st.deopt_frames, 1) << "only top is left to rebuild: small's frame was replaced, and big never made its own";
      EXPECT_EQ(e.st.rebuild, GRCORE_OK);
      EXPECT_EQ(e.st.tails, 1);
    }
    EXPECT_EQ(o.frames_left, 0u);
    EXPECT_EQ(e.st.pushes, e.st.pops);
  }
}

/* ---- A frame replaced while its code is retired ----------------------------------------- */

TEST(Tail, AFunctionThatClearsItsOwnSlotAndTailCallsLeavesItsCodeRetiredUntilTheActivationLeaves) {
  TAIL_ONLY_ON_X86_64_SYSV();
  Engine e;
  int target = add_inc2(e);
  ASSERT_TRUE(e.compile_fn(target));
  int self = e.reserve();
  {
    P p("self_clear", {GRJIT_TYPE_I64});
    p.clearslot(self);
    p.tailcall(target, {0});
    e.set(self, p.done());
  }
  Outcome o = e.run_compiled(self, {40});
  ASSERT_TRUE(o.finished);
  EXPECT_EQ(o.value, 42u);
  EXPECT_EQ(o.exit, uint32_t{GRJIT_EXIT_RETURNED});
  EXPECT_EQ(*e.released, 1) << "the replaced function's code is let go once the activation leaves";
}

/* ---- Collections in the hook: every kind of argument, every reference updated ----- */

TEST(Tail, ArgumentsOfAllThreeTypesTailCalledAtEveryCountAndCallerWidthArriveIntactUnderCollection) {
  TAIL_ONLY_ON_X86_64_SYSV();
  // callee(a0 ... a{n-1}) with a_i an I64, a REF or a PTR (a pointer derived from
  // a reference, into the object) in turn, returning the sum of (i + 1) times
  // each one's value. The caller makes them and tail-calls, and the tail hook
  // collects and moves, so a reference or a derived pointer passed in a register
  // or on the stack is read after the collection from where the collector put
  // it. The caller has extra I64 parameters, so the incoming area narrows and
  // widens in every combination.
  Engine e;
  e.torture = true;
  for (int through_pointer = 0; through_pointer < 2; through_pointer++) {
    for (int extra : {0, 4, 9, 15}) {
      for (int n = 1; n <= 16; n++) {
        SCOPED_TRACE(testing::Message() << (through_pointer ? "pointer" : "slot") << " extra=" << extra << " n=" << n);
        std::vector<GRJIT_Type> types;
        for (int i = 0; i < n; i++) {
          types.push_back(i % 3 == 0 ? GRJIT_TYPE_I64 : i % 3 == 1 ? GRJIT_TYPE_REF : GRJIT_TYPE_PTR);
        }
        int callee = e.reserve();
        {
          P p("mixed", types);
          int acc = p.local(), c = p.local(), t = p.local();
          p.cnst(acc, 0);
          for (int i = 0; i < n; i++) {
            if (types[i] == GRJIT_TYPE_I64) {
              p.mov(t, i);
            } else if (types[i] == GRJIT_TYPE_REF) {
              p.get(t, i);
            } else {
              p.load(t, i); // the object's value, through the derived pointer
            }
            p.cnst(c, i + 1);
            p.bin(K::MUL, t, t, c);
            p.bin(K::ADD, acc, acc, t);
          }
          p.ret(acc);
          e.set(callee, p.done());
        }
        ASSERT_TRUE(e.compile_fn(callee));
        int caller = e.reserve();
        {
          std::vector<GRJIT_Type> ptypes(static_cast<size_t>(extra), GRJIT_TYPE_I64);
          ptypes.push_back(GRJIT_TYPE_I64);
          P p("mixed_tail", ptypes);
          int x = extra;
          std::vector<int> args;
          for (int i = 0; i < n; i++) {
            // A derived pointer's base is a local of lower index, so that the
            // frame states naming every local never hold the pointer live
            // while its base is assigned (the verifier's rule).
            int obj = types[i] == GRJIT_TYPE_PTR ? p.local(GRJIT_TYPE_REF) : -1;
            int a = p.local(types[i]);
            if (types[i] == GRJIT_TYPE_I64) {
              int k = p.local();
              p.cnst(k, i);
              p.bin(K::ADD, a, x, k);
            } else if (types[i] == GRJIT_TYPE_REF) {
              p.nw(a, 1000 + i);
            } else {
              p.nw(obj, 2000 + i);
              p.derive(a, obj, 8);
            }
            args.push_back(a);
          }
          if (through_pointer) {
            int ptr = p.local(GRJIT_TYPE_PTR);
            p.entryof(ptr, callee);
            p.tailcallp(ptr, callee, args);
          } else {
            p.tailcall(callee, args);
          }
          e.set(caller, p.done());
        }
        std::vector<u64> given(static_cast<size_t>(extra), 0);
        given.push_back(5);
        int64_t want = 0;
        for (int i = 0; i < n; i++) {
          int64_t v = types[i] == GRJIT_TYPE_I64 ? 5 + i
              : types[i] == GRJIT_TYPE_REF       ? 1000 + i
                                                 : 2000 + i;
          want += (i + 1) * v;
        }
        Outcome o = e.run_compiled(caller, given);
        ASSERT_TRUE(o.finished);
        EXPECT_EQ(o.exit, uint32_t{GRJIT_EXIT_RETURNED});
        EXPECT_EQ(static_cast<int64_t>(o.value), want);
        // The interpreter has no derived pointers: a raw one it made would be
        // stale after the collection another allocation causes, so it runs
        // without the torture.
        e.torture = false;
        Outcome i = e.run_interpreted(caller, given);
        e.torture = true;
        ASSERT_TRUE(i.finished);
        EXPECT_EQ(i.value, o.value);
      }
    }
  }
  EXPECT_EQ(e.heap.poisoned_reads, 0);
  EXPECT_GT(e.heap.moved, 1000);
  EXPECT_EQ(e.st.deopts, 0);
  EXPECT_EQ(e.st.tail_refusals, 0);
}

TEST(Tail, ARefusedTailHookThatCollectedLeavesTheExitStatesReferencesUpdated) {
  TAIL_ONLY_ON_X86_64_SYSV();
  // The hook collects, moving every object, and then refuses: the exit rebuilds
  // the frame from the slots the collection updated, because the hook's site map
  // names everything the exit's state does.
  Engine e;
  e.torture = true;
  int callee = e.add([&] {
    P p("callee", {GRJIT_TYPE_REF, GRJIT_TYPE_I64});
    int v = p.local(), r = p.local();
    p.get(v, 0);
    p.bin(K::ADD, r, v, 1);
    p.ret(r);
    return p.done();
  }());
  int caller = e.add([&] {
    P p("holder", {GRJIT_TYPE_I64});
    int a = p.local(GRJIT_TYPE_REF), b = p.local(GRJIT_TYPE_REF), x = 0, v = p.local(),
        w = p.local(), s = p.local();
    p.nw(a, 100);
    p.nw(b, 7);
    // `b` is live only in the frame state here, the tail call passing `a` and x.
    p.tailcall(callee, {a, x});
    (void)v; (void)w; (void)s;
    return p.done();
  }());
  e.refuse_tail_at = 1;
  Outcome o = e.run_compiled(caller, {5});
  ASSERT_TRUE(o.finished);
  EXPECT_EQ(o.value, 105u) << "100 + 5, read from a reference that was moved and then rebuilt into the frame";
  EXPECT_EQ(o.exit, uint32_t{GRJIT_EXIT_DEOPT});
  EXPECT_EQ(e.st.tail_refusals, 1);
  EXPECT_EQ(e.heap.poisoned_reads, 0);
  EXPECT_GT(e.heap.moved, 0);
}

/* ---- The fixture's two planted defects, each with a control ------------------------ */

TEST(Tail, AHookThatKeepsTheCallersFrameIsSeenByTheDepthBudgetAndTheControlIsNot) {
  TAIL_ONLY_ON_X86_64_SYSV();
  for (int planted = 0; planted < 2; planted++) {
    SCOPED_TRACE(planted ? "planted: the frame is kept" : "control");
    Engine e(/*guest_depth=*/100);
    e.tail_keeps_frame = planted != 0;
    // The kept frames are not what a rebuild expects, so the interpreter is not
    // let finish the planted run: the exit is what is observed.
    e.interpreter_cannot_recover = planted != 0;
    int loop = add_countdown(e, 5000);
    Outcome o = e.run_compiled(loop, {5000, 0});
    if (planted) {
      EXPECT_EQ(o.exit, uint32_t{GRJIT_EXIT_DEOPT}) << "a miscount shows as the limit, at depth 100";
      EXPECT_FALSE(o.finished);
      EXPECT_EQ(e.st.tail_refusals, 1);
      EXPECT_EQ(e.st.tails, 100) << "refused by the depth budget at the hundredth";
    } else {
      ASSERT_TRUE(o.finished);
      EXPECT_EQ(o.value, 5000u * 5001u / 2u);
      EXPECT_EQ(o.exit, uint32_t{GRJIT_EXIT_RETURNED});
      EXPECT_EQ(e.st.tail_refusals, 0);
    }
  }
}

TEST(Tail, AHookThatKeepsTheCallersReservationExtensionIsSeenInTheCapacityAndTheControlIsNot) {
  TAIL_ONLY_ON_X86_64_SYSV();
  for (int planted = 0; planted < 2; planted++) {
    SCOPED_TRACE(planted ? "planted: the extension is kept" : "control");
    Engine e(GRCORE_UNLIMITED, GRCORE_UNLIMITED, /*conv=*/true);
    e.tail_keeps_extension = planted != 0;
    int loop = add_countdown(e, 2000);
    ASSERT_TRUE(e.compile_fn(loop));
    ASSERT_GT(e.code_of(loop).max_converting, 0u) << "an engine whose extensions are not nothing";
    Outcome o = e.run_compiled(loop, {2000, 0});
    ASSERT_TRUE(o.finished);
    EXPECT_EQ(o.value, 2000u * 2001u / 2u);
    ASSERT_EQ(e.st.caps.size(), 2u);
    if (planted) {
      EXPECT_GT(e.st.caps[1], e.st.caps[0]) << "the capacity grows with every tail call";
    } else {
      EXPECT_EQ(e.st.caps[1], e.st.caps[0]);
    }
  }
}

TEST(Tail, TheCapacityIsConstantInAMillionTailCallsForAnEngineWhoseExtensionsAreNotNothing) {
  TAIL_ONLY_ON_X86_64_SYSV();
  Engine e(GRCORE_UNLIMITED, 64 * 1024, /*conv=*/true);
  int loop = add_countdown(e, kMillion);
  ASSERT_TRUE(e.compile_fn(loop));
  ASSERT_GT(e.code_of(loop).max_converting, 0u);
  Outcome o = e.run_compiled(loop, {static_cast<u64>(kMillion), 0});
  ASSERT_TRUE(o.finished);
  EXPECT_EQ(o.exit, uint32_t{GRJIT_EXIT_RETURNED});
  expect_constant(e, 2);
  EXPECT_GT(e.st.caps[0], 0u);
}

/* ---- The walk after tail calls that widen and narrow the incoming area ------------- */

namespace {

/* A(n): an object; v = W(n, 11 more immediates) by a call; ret v + object's
 * value. W(n, ...): tail-calls B(n). B(n): an object; n == 0: collect and return
 * its value; else r = A(n - 1) by a call; ret r + its value. So the chain from
 * A(L) is A(L), B(L), A(L - 1), B(L - 1), ..., A(0), B(0): every B is a callee
 * of a tail call from a wider function, and every A calls a function with 12
 * parameters (96 bytes of stack arguments, narrowed to B's none). */
void add_walk_chain(Engine & e, int * a_out, int * b_out, int * w_out, bool through_pointer) {
  int a = e.reserve();
  int w = e.reserve();
  int b = e.reserve();
  {
    P p("A", {GRJIT_TYPE_I64});
    int obj = p.local(GRJIT_TYPE_REF), r = p.local(), v = p.local(), s = p.local();
    p.nw(obj, 3);
    std::vector<int> args = {0};
    for (int i = 0; i < 11; i++) {
      args.push_back(p.imm(50 + i));
    }
    p.call(r, w, args);
    p.get(v, obj);
    p.bin(K::ADD, s, r, v);
    p.ret(s);
    e.set(a, p.done());
  }
  {
    std::vector<GRJIT_Type> types(12, GRJIT_TYPE_I64);
    P p("W", types);
    if (through_pointer) {
      int ptr = p.local(GRJIT_TYPE_PTR);
      p.entryof(ptr, b);
      p.tailcallp(ptr, b, {0});
    } else {
      p.tailcall(b, {0});
    }
    e.set(w, p.done());
  }
  {
    P p("B", {GRJIT_TYPE_I64});
    int obj = p.local(GRJIT_TYPE_REF), zero = p.local(), one = p.local(), c = p.local(),
        n1 = p.local(), r = p.local(), v = p.local(), s = p.local();
    p.nw(obj, 4);
    p.cnst(zero, 0);
    p.cnst(one, 1);
    p.bin(K::EQ, c, 0, zero);
    int more = p.brz(c);
    p.collect();
    p.get(v, obj);
    p.ret(v);
    p.patch(more, p.here());
    p.bin(K::SUB, n1, 0, one);
    p.call(r, a, {n1});
    p.get(v, obj);
    p.bin(K::ADD, s, r, v);
    p.ret(s);
    e.set(b, p.done());
  }
  *a_out = a;
  *b_out = b;
  *w_out = w;
}

} // namespace

TEST(Tail, ACollectionInAFiftyDeepChainAfterWideningAndNarrowingTailCallsSeesEveryFrameOnceWithItsReferences) {
  TAIL_ONLY_ON_X86_64_SYSV();
  for (int through_pointer = 0; through_pointer < 2; through_pointer++) {
    SCOPED_TRACE(through_pointer ? "pointer" : "slot");
    Engine e;
    e.torture = true;
    int a, b, w;
    add_walk_chain(e, &a, &b, &w, through_pointer != 0);
    ASSERT_TRUE(e.compile_fn(b));
    struct Seen {
      std::vector<GRCORE_CompiledFrame> frames;
      std::vector<uintptr_t> saved;
      size_t roots = 0;
      bool broken = false;
      bool done = false;
    } seen;
    e.on_collect = [&](Engine & en) {
      if (seen.done) {
        return;
      }
      seen.done = true;
      GRCORE_CompiledWalk walk;
      ASSERT_EQ(grcore_compiled_walk_begin(en.ctx, &walk), GRCORE_OK);
      GRCORE_CompiledFrame f;
      GRCORE_CompiledWalkStatus r;
      while ((r = grcore_compiled_walk_next(&walk, &f)) == GRCORE_CWALK_FRAME) {
        seen.frames.push_back(f);
        seen.saved.push_back(*reinterpret_cast<const uintptr_t *>(f.frame_base));
      }
      seen.broken = r == GRCORE_CWALK_BROKEN;
      GRCORE_RootVisitor rv = {};
      rv.user = &seen.roots;
      rv.slot = [](void * user, uint64_t *) { ++*static_cast<size_t *>(user); };
      ASSERT_EQ(grcore_context_enumerate_roots(en.ctx, &rv), GRCORE_OK);
    };
    Outcome o = e.run_compiled(a, {24});
    ASSERT_TRUE(o.finished);
    EXPECT_EQ(o.value, 25u * 3u + 25u * 4u) << "A adds 3 and B adds 4, 25 levels each";
    EXPECT_EQ(o.exit, uint32_t{GRJIT_EXIT_RETURNED});
    ASSERT_FALSE(seen.broken) << "a broken chain is reported";
    ASSERT_EQ(seen.frames.size(), 50u) << "each frame once: A(24), B(24), ..., A(0), B(0)";
    for (size_t k = 0; k < seen.frames.size(); k++) {
      SCOPED_TRACE(k);
      const GRCORE_CompiledFrame & f = seen.frames[k];
      // Innermost first: B(0), A(0), B(1), A(1), ...
      EXPECT_EQ(f.identity.function, static_cast<u64>(k % 2 == 0 ? b : a)) << "its own identity, and never W's";
      EXPECT_EQ(f.depth, k);
      if (k + 1 < seen.frames.size()) {
        EXPECT_GT(seen.frames[k + 1].frame_base, f.frame_base) << "outward";
        EXPECT_EQ(seen.saved[k], seen.frames[k + 1].frame_base) << "the saved base is the next frame's";
      } else {
        EXPECT_EQ(seen.saved[k], uintptr_t{GRCORE_COMPILED_CHAIN_END}) << "the last caller's base is the marker";
      }
    }
    EXPECT_EQ(seen.roots, 50u) << "one reference per frame, reported once";
    EXPECT_EQ(e.heap.poisoned_reads, 0);
    EXPECT_GT(e.heap.moved, 100);
    EXPECT_EQ(e.st.tails, 25);
    EXPECT_EQ(e.st.deopts, 0);
  }
}

/* ---- Generated programs: tail calls, calls, every argument type, collections ------- */

namespace {

struct Sig {
  std::vector<GRJIT_Type> types;
};

/* A random program of `n` functions, f_i calling and tail-calling only f_j with
 * j < i, so it terminates. Each takes a random number of parameters of random
 * types (the last, the entry, two integers), reads every one, allocates an
 * object, may collect, may call a lower function, may hit a guard that fails for
 * some inputs or a poll, and ends by returning a mix of everything or by tail
 * calling a lower function through a slot or a code pointer, with arguments of
 * the callee's types built from what it holds: integers, immediates, its own
 * object or a reference parameter, and a pointer derived from its object. A few functions own a scope, so the engine refuses a tail
 * call from them. */
void generate_tail(Engine & e, std::mt19937 & rng, int n, std::vector<int> * fns,
    std::vector<Sig> * sigs, bool with_scopes) {
  auto pick = [&](int lo, int hi) { return std::uniform_int_distribution<int>(lo, hi)(rng); };
  for (int i = 0; i < n; i++) {
    Sig sig;
    if (i == n - 1) {
      sig.types = {GRJIT_TYPE_I64, GRJIT_TYPE_I64};
    } else {
      int np = pick(0, 11);
      for (int k = 0; k < np; k++) {
        int r = pick(0, 19);
        sig.types.push_back(r < 12 ? GRJIT_TYPE_I64 : r < 17 ? GRJIT_TYPE_REF : GRJIT_TYPE_PTR);
      }
    }
    sigs->push_back(sig);
    fns->push_back(e.reserve());
  }
  for (int i = 0; i < n; i++) {
    const Sig & sg = (*sigs)[static_cast<size_t>(i)];
    P p("gt", sg.types);
    int obj = p.local(GRJIT_TYPE_REF), dp = p.local(GRJIT_TYPE_PTR), s = p.local(),
        c = p.local(), t = p.local(), r = p.local(), v = p.local(), k = p.local(),
        m = p.local(), ptr = p.local(GRJIT_TYPE_PTR);
    // Every parameter is read before anything that can collect: a pointer
    // parameter is raw (not a derived pointer of this function), and the
    // collector moves the object it points into.
    p.cnst(s, 100 + i);
    for (size_t q = 0; q < sg.types.size(); q++) {
      int param = static_cast<int>(q);
      if (sg.types[q] == GRJIT_TYPE_I64) {
        p.mov(t, param);
      } else if (sg.types[q] == GRJIT_TYPE_REF) {
        p.get(t, param);
      } else {
        p.load(t, param);
      }
      p.cnst(c, static_cast<int64_t>(q) + 1);
      p.bin(K::MUL, t, t, c);
      p.bin(K::ADD, s, s, t);
    }
    p.nw(obj, pick(1, 50));
    p.derive(dp, obj, 8); // right after its base, as the verifier's rule needs; it follows the base from here
    if (pick(0, 3) == 0) {
      p.collect();
    }
    // The arguments for a call of function `j`, from what this function holds.
    auto make_args = [&](int j) {
      std::vector<int> args;
      for (GRJIT_Type ty : (*sigs)[static_cast<size_t>(j)].types) {
        if (ty == GRJIT_TYPE_I64) {
          int w = pick(0, 4);
          if (w == 0) {
            args.push_back(p.imm(pick(-20, 20)));
          } else if (w == 1) {
            args.push_back(s);
          } else {
            int ip = -1;
            for (size_t q = 0; q < sg.types.size(); q++) {
              if (sg.types[q] == GRJIT_TYPE_I64 && pick(0, 1) == 0) {
                ip = static_cast<int>(q);
              }
            }
            args.push_back(ip >= 0 ? ip : s);
          }
        } else if (ty == GRJIT_TYPE_REF) {
          int rp = -1;
          for (size_t q = 0; q < sg.types.size(); q++) {
            if (sg.types[q] == GRJIT_TYPE_REF && pick(0, 1) == 0) {
              rp = static_cast<int>(q);
            }
          }
          args.push_back(rp >= 0 ? rp : obj);
        } else {
          args.push_back(dp);
        }
      }
      return args;
    };
    if (i > 0 && pick(0, 2) == 0) {
      int j = pick(0, i - 1);
      std::vector<int> args = make_args(j);
      if (pick(0, 1) == 0) {
        p.entryof(ptr, (*fns)[static_cast<size_t>(j)]);
        p.callp(r, ptr, (*fns)[static_cast<size_t>(j)], args);
      } else {
        p.call(r, (*fns)[static_cast<size_t>(j)], args);
      }
      p.bin(K::ADD, s, s, r);
    }
    if (pick(0, 2) == 0) {
      p.cnst(m, 63);
      p.bin(K::AND, t, s, m);
      p.cnst(k, pick(0, 70));
      p.bin(K::LT, t, t, k);
      p.guard(t);
    }
    if (pick(0, 3) == 0) {
      p.poll();
    }
    if (i > 0 && pick(0, 1) == 0) {
      int j = pick(0, i - 1);
      std::vector<int> args = make_args(j);
      if (pick(0, 1) == 0) {
        p.entryof(ptr, (*fns)[static_cast<size_t>(j)]);
        p.tailcallp(ptr, (*fns)[static_cast<size_t>(j)], args);
      } else {
        p.tailcall((*fns)[static_cast<size_t>(j)], args);
      }
    } else {
      p.get(v, obj);
      p.bin(K::ADD, s, s, v);
      p.ret(s);
    }
    Func f = p.done();
    f.owns_scope = with_scopes && pick(0, 7) == 0;
    e.set((*fns)[static_cast<size_t>(i)], f);
  }
}

} // namespace

TEST(Tail, GeneratedProgramsOfCallsAndTailCallsAgreeWithTheInterpreterAcrossCollectionsGuardsAndRefusals) {
  TAIL_ONLY_ON_X86_64_SYSV();
  long deopted = 0, direct = 0, tails = 0, refused = 0, pointer_tails = 0;
  for (unsigned seed = 0; seed < 200; seed++) {
    SCOPED_TRACE(seed);
    std::mt19937 rng(seed);
    Engine e;
    e.torture = seed % 2 == 0;
    e.interpreter_never_moves = true;
    std::vector<int> fns;
    std::vector<Sig> sigs;
    int n = 3 + static_cast<int>(seed % 6);
    generate_tail(e, rng, n, &fns, &sigs, /*with_scopes=*/seed % 3 == 0);
    for (int fn : fns) {
      for (const Ins & in : e.funcs[static_cast<size_t>(fn)].code) {
        pointer_tails += in.k == K::TAILCALLP ? 1 : 0;
      }
      ASSERT_TRUE(e.compile_fn(fn)); // so every ENTRYOF has an entry to take
    }
    for (u64 x : {u64{0}, u64{3}, u64{15}, u64{40}}) {
      if (seed % 5 == 1) {
        e.tail_calls = 0;
        e.refuse_tail_at = 1 + static_cast<long>(x % 3);
      }
      // The interpreter has no derived pointers (interpreter_never_moves), and a
      // run of it has no torture: nothing in it moves.
      bool torture = e.torture;
      e.torture = false;
      Outcome i = e.run_interpreted(fns[n - 1], {x, x + 1});
      e.torture = torture;
      ASSERT_TRUE(i.finished);
      Outcome c = e.run_compiled(fns[n - 1], {x, x + 1});
      ASSERT_TRUE(c.finished) << "failed=" << c.failed;
      EXPECT_EQ(c.value, i.value);
      EXPECT_EQ(c.frames_left, 0u);
      EXPECT_EQ(e.st.rebuild, GRCORE_OK);
      (c.exit == GRJIT_EXIT_DEOPT ? deopted : direct)++;
      e.st.rebuild = GRCORE_OK;
    }
    tails += e.st.tails;
    refused += e.st.tail_refusals;
    EXPECT_EQ(e.heap.poisoned_reads, 0);
    EXPECT_EQ(e.st.pushes, e.st.pops);
  }
  // Every kind of run happened, in numbers: the generator reaches the paths.
  EXPECT_GT(deopted, 100);
  EXPECT_GT(direct, 100);
  EXPECT_GT(tails, 500);
  EXPECT_GT(refused, 50);
  EXPECT_GT(pointer_tails, 100);
}

/* ---- The frame's shape: the pad is what the staging area needs, and no more ------- */

namespace {

uint32_t h_noop_push(void *, uint64_t, const uint64_t *, uint64_t) { return 0; }
void h_noop_pop(void *) {}
uint32_t h_noop(void *, uint64_t) { return 0; }

/* The metadata's frame size of a callable function of `params` parameters and no
 * other register whose only operation is a tail call with `t_args` immediate
 * arguments, or, for `t_args < 0`, a return. */
uint32_t frame_bytes_of(int params, int t_args) {
  B b("shape", 1);
  GRJIT_CallHooks h{};
  h.push = h_noop_push;
  h.pop = h_noop_pop;
  h.compile = h_noop;
  h.deopt = h_noop;
  h.tail = h_noop_push;
  b.callable(h);
  for (int i = 0; i < params; i++) {
    b.param(GRJIT_TYPE_I64);
  }
  b.at(b.block());
  static uint64_t word;
  if (t_args >= 0) {
    std::vector<GRJIT_Operand> args(static_cast<size_t>(t_args), I(1));
    b.tail_call_slot(&word, 1, args, GRCORE_PollIdentity{0, 0}, {grjit_frame_slot_dead()});
  } else {
    b.ret();
  }
  Fn f(b.finish());
  GRJIT_Emitted e;
  EXPECT_EQ(grjit_emit_for(GRJIT_ARCH_X86_64, f, grjit_allocator_default(), nullptr, nullptr,
                0x40, &e),
      GRJIT_OK);
  uint32_t bytes = e.meta.meta.frame_bytes;
  grjit_emitted_free(&e);
  return bytes;
}

} // namespace

TEST(Tail, TheFramesPaddingIsExactlyWhatTheStagingAreaNeedsAndNoFunctionWithoutATailCallHasAny) {
  // A function with `params` parameters has 3 fixed slots and one per register;
  // its tail call to a callee of `t` arguments stages them in an area of `t`
  // slots and keeps the entry in one more. The area must end at or below where the
  // return address goes, `8 + in_A - in_T` above the frame base; the padding is the
  // least that makes it so, which is shown here from the frame sizes alone.
  for (int params = 0; params <= 16; params++) {
    for (int t = 0; t <= 16; t++) {
      SCOPED_TRACE(testing::Message() << "params=" << params << " t=" << t);
      auto in_bytes = [](int n) { return n > 6 ? (n - 6) * 8 + ((n - 6) % 2) * 8 : 0; };
      const int in_a = in_bytes(params);
      const int in_t = in_bytes(t);
      const int slots_without_pad = params + 3 + t + 1;
      const int want_pad = std::max(0, (in_t - in_a) / 8 - (params + 4));
      const uint32_t frame = frame_bytes_of(params, t);
      EXPECT_EQ(frame, static_cast<uint32_t>((slots_without_pad + want_pad) * 8 + 15) / 16 * 16);
      // The area's end is `8 * (params + 3 + pad)` below the base; the return
      // address goes `8 + in_a - in_t` above it. The area ends at or below it...
      EXPECT_LE(-8 * (params + 3 + want_pad), 8 + in_a - in_t);
      // ...and with one slot less of padding it would not (when there is any).
      if (want_pad > 0) {
        EXPECT_GT(-8 * (params + 3 + want_pad - 1), 8 + in_a - in_t);
      }
    }
    // A function with no tail call has no padding at all: the frame of 3 slots, its
    // registers, and nothing else.
    EXPECT_EQ(frame_bytes_of(params, -1), static_cast<uint32_t>((params + 3) * 8 + 15) / 16 * 16);
  }
}

/* ---- The guest stack has no room for the callee's larger frame: a memory refusal ----- */

TEST(Tail, AMemoryBudgetThatCannotGrowTheGuestStackForALargerCalleeRefusesTheHookAndTheInterpreterReachesTheSameVerdict) {
  TAIL_ONLY_ON_X86_64_SYSV();
  // small(n) tail-calls big(n), whose guest frame (2000 locals) is far larger than
  // small's, so the hook has to make room (grcore_stack_reserve) before it
  // replaces the frame. The memory budget is swept from what the context holds
  // when the tail call is made, a kilobyte at a time: wherever the room for the larger frame is refused, the
  // hook refuses (nothing half done), the exit is taken, and the interpreter,
  // whose own pop and push needs the same room, reaches the budget's verdict, as
  // the interpreted run does. Where there is room the tail call is made.
  auto build = [](Engine & e, int * top) {
    int small;
    *top = add_small_big(e, 2000, &small);
    for (size_t fn = 0; fn < e.funcs.size(); fn++) {
      EXPECT_TRUE(e.compile_fn(static_cast<int>(fn)));
    }
  };
  // What the context holds when small makes its tail call, learnt from a run with
  // no budget (the same compiles, the same records, the same first frames).
  uint64_t base = 0;
  {
    Engine probe;
    int top;
    build(probe, &top);
    probe.on_probe = [&](Engine & en) { base = grcore_context_memory_in_use(en.ctx); };
    Outcome o = probe.run_compiled(top, {5});
    ASSERT_TRUE(o.finished);
    ASSERT_GT(base, 0u);
  }
  long refused = 0, made = 0, both_limit = 0;
  for (uint64_t slack = 0; slack <= 64 * 1024; slack += 1024) {
    SCOPED_TRACE(slack);
    Engine e(GRCORE_UNLIMITED, GRCORE_UNLIMITED, false, nullptr, base + slack);
    int top;
    build(e, &top);
    Outcome i = e.run_interpreted(top, {5});
    e.st = Stats{};
    Outcome c = e.run_compiled(top, {5});
    EXPECT_EQ(c.limit, i.limit) << "the budget's verdict is the same in both tiers";
    EXPECT_EQ(c.finished, i.finished);
    if (i.finished) {
      EXPECT_EQ(c.value, i.value);
      EXPECT_EQ(c.value, 5u + 2000u);
    }
    if (e.st.reserve_refusals > 0) {
      refused++;
      EXPECT_EQ(c.exit, uint32_t{GRJIT_EXIT_DEOPT}) << "an exit at the tail site";
      EXPECT_EQ(e.st.tail_refusals, e.st.reserve_refusals);
      EXPECT_EQ(e.st.deopt_frames, 2) << "top and small, as the hook left them";
      both_limit += c.limit ? 1 : 0;
    } else if (e.st.tails > 0) {
      made++;
      EXPECT_TRUE(c.finished);
      EXPECT_EQ(c.exit, uint32_t{GRJIT_EXIT_RETURNED});
    }
    EXPECT_EQ(c.frames_left, 0u);
  }
  EXPECT_GT(refused, 0) << "the room was refused for some budgets";
  EXPECT_GT(made, 0) << "and given for others";
  EXPECT_GT(both_limit, 0) << "and where it was refused the interpreter's own pop and push met the same limit";
}

GRJIT_TEST_MAIN()
