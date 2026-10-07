/**
 * @file
 *
 * Calls between compiled functions (AD-28, CAP-1 to CAP-4) on x86-64 SysV: the
 * internal convention, the entry adapter, dispatch through an entry slot and a
 * code pointer, the stack maps at call sites, the chain deoptimization, the
 * native stack in bytes and the lifetime of code a frame returns into. They run
 * through the fixture engine (`calls_fixture.h`), which interprets and compiles
 * the same programs and checks them against C++ references.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#define FX_ASM_SENTINELS
#include "../calls_asm.h"
#include "../calls_fixture.h"

#include "../../src/code/code_internal.h"

#include <csignal>
#include <random>
#ifndef _WIN32
#include <sys/wait.h>
#include <unistd.h>
#endif

using namespace fx;

namespace {

int64_t fib_ref(int64_t n) { return n < 2 ? n : fib_ref(n - 1) + fib_ref(n - 2); }

/* fib(n) = n < 2 ? n : fib(n-1) + fib(n-2), calling itself through its slot. */
int add_fib(Engine & e) {
  int fib = e.reserve();
  P p("fib", {GRJIT_TYPE_I64});
  int n = 0, one = p.local(), two = p.local(), t = p.local(), a = p.local(),
      b = p.local(), r1 = p.local(), r2 = p.local(), r = p.local();
  p.cnst(one, 1);
  p.cnst(two, 2);
  p.bin(K::LT, t, n, two);
  int br = p.brz(t);
  p.ret(n);
  p.patch(br, p.here());
  p.bin(K::SUB, a, n, one);
  p.call(r1, fib, {a});
  p.bin(K::SUB, b, n, two);
  p.call(r2, fib, {b});
  p.bin(K::ADD, r, r1, r2);
  p.ret(r);
  e.set(fib, p.done());
  return fib;
}

} // namespace

/* Calls are emitted for x86-64 SysV only (arm64 and Win64 are story 7's). Where
 * they are not, a test does not skip, which would count as a test that proved
 * nothing: it shows the refusal instead, which is what those targets promise. */
#if defined(__x86_64__) && defined(__linux__)
#define CALLS_ONLY_ON_X86_64_SYSV() (void)0
#else
namespace {
void expect_calls_refused_here() {
  B b("ident", 0);
  GRJIT_VReg x = b.param(GRJIT_TYPE_I64);
  GRJIT_CallHooks h{};
  h.deopt = [](void *, uint64_t) -> uint32_t { return 0; };
  b.callable(h);
  b.at(b.block());
  b.ret(V(x));
  Fn f(b.finish());
  JitWorld w;
  Compiled c(f, w.pages());
  EXPECT_EQ(c.result, GRJIT_ERR_UNSUPPORTED) << "a callable function is refused with a clear error";
  EXPECT_FALSE(c);
}
} // namespace
#define CALLS_ONLY_ON_X86_64_SYSV() \
  do { \
    expect_calls_refused_here(); \
    return; \
  } while (0)
#endif

TEST(Calls, FibCompiledMatchesTheInterpreterAndTheCReferenceAndNeverLeavesCompiledCode) {
  CALLS_ONLY_ON_X86_64_SYSV();
  Engine e;
  int fib = add_fib(e);
  for (int64_t n = 0; n <= 15; n++) {
    SCOPED_TRACE(n);
    Outcome i = e.run_interpreted(fib, {static_cast<u64>(n)});
    ASSERT_TRUE(i.finished);
    EXPECT_EQ(static_cast<int64_t>(i.value), fib_ref(n));
    long pushes = e.st.pushes;
    Outcome c = e.run_compiled(fib, {static_cast<u64>(n)});
    ASSERT_TRUE(c.finished);
    EXPECT_EQ(c.exit, uint32_t{GRJIT_EXIT_RETURNED});
    EXPECT_FALSE(c.interpreted_rest);
    EXPECT_EQ(static_cast<int64_t>(c.value), fib_ref(n));
    EXPECT_EQ(c.frames_left, 0u);
    // Every call pushed a guest frame, and every return popped it: the entry's,
    // and one for each of the 2 * fib(n + 1) - 2 calls inside.
    EXPECT_EQ(e.st.pushes - pushes, 2 * fib_ref(n + 1) - 1);
  }
  EXPECT_EQ(e.st.pushes, e.st.pops);
  EXPECT_EQ(e.st.deopts, 0);
  EXPECT_EQ(e.heap.poisoned_reads, 0);
}


/* ---- Arguments: every count, every type ------------------------------------ */

namespace {

/* argsN(a0 .. a{N-1}) = sum of (i + 1) * a_i + N, and callerN(x) calls it with
 * a_i = x + i and returns that plus one. */
void add_args_pair(Engine & e, int n, int * out_callee, int * out_caller) {
  int callee = e.reserve();
  {
    std::vector<GRJIT_Type> params(static_cast<size_t>(n), GRJIT_TYPE_I64);
    P p("args", params);
    int acc = p.local(), c = p.local(), t = p.local();
    p.cnst(acc, n);
    for (int i = 0; i < n; i++) {
      p.cnst(c, i + 1);
      p.bin(K::MUL, t, i, c);
      p.bin(K::ADD, acc, acc, t);
    }
    p.ret(acc);
    e.set(callee, p.done());
  }
  int caller = e.reserve();
  {
    P p("caller", {GRJIT_TYPE_I64});
    int x = 0, one = p.local(), r = p.local();
    std::vector<int> args;
    p.cnst(one, 1);
    for (int i = 0; i < n; i++) {
      int a = p.local(), k = p.local();
      p.cnst(k, i);
      p.bin(K::ADD, a, x, k);
      args.push_back(a);
    }
    p.call(r, callee, args);
    p.bin(K::ADD, r, r, one);
    p.ret(r);
    e.set(caller, p.done());
  }
  *out_callee = callee;
  *out_caller = caller;
}

int64_t args_ref(int n, int64_t x) {
  int64_t acc = n;
  for (int i = 0; i < n; i++) {
    acc += (i + 1) * (x + i);
  }
  return acc;
}

} // namespace

TEST(Calls, ZeroToSixteenArgumentsPassedInRegistersAndOnTheStackMatchTheReference) {
  CALLS_ONLY_ON_X86_64_SYSV();
  Engine e;
  for (int n = 0; n <= 16; n++) {
    SCOPED_TRACE(n);
    int callee, caller;
    add_args_pair(e, n, &callee, &caller);
    for (int64_t x : {0, 1, 5, -3, 1000003}) {
      SCOPED_TRACE(x);
      Outcome i = e.run_interpreted(caller, {static_cast<u64>(x)});
      ASSERT_TRUE(i.finished);
      EXPECT_EQ(static_cast<int64_t>(i.value), args_ref(n, x) + 1);
      Outcome c = e.run_compiled(caller, {static_cast<u64>(x)});
      ASSERT_TRUE(c.finished);
      EXPECT_EQ(c.exit, uint32_t{GRJIT_EXIT_RETURNED});
      EXPECT_EQ(static_cast<int64_t>(c.value), args_ref(n, x) + 1);
    }
    // The callee is also an entry: the adapter loads the same arguments from the
    // array the C ABI hands it.
    std::vector<u64> args;
    for (int k = 0; k < n; k++) {
      args.push_back(static_cast<u64>(7 * k + 2));
    }
    int64_t want = n;
    for (int k = 0; k < n; k++) {
      want += (k + 1) * (7 * k + 2);
    }
    Outcome d = e.run_compiled(callee, args);
    ASSERT_TRUE(d.finished);
    EXPECT_EQ(static_cast<int64_t>(d.value), want);
  }
  EXPECT_EQ(e.st.deopts, 0);
  EXPECT_EQ(e.st.pushes, e.st.pops);
}

/* ---- Types: I64, REF and PTR through a call --------------------------------- */

namespace {

int add_echo(Engine & e, GRJIT_Type t, const char * name) {
  P p(name, {t});
  p.ret(0);
  return e.add(p.done());
}

} // namespace

TEST(Calls, AnI64AReferenceAndAPointerEachGoThroughACallAndComeBackIntact) {
  CALLS_ONLY_ON_X86_64_SYSV();
  Engine e;
  e.torture = true; // a moving collection at every push, new object and poll
  int echo_i = add_echo(e, GRJIT_TYPE_I64, "echo_i64");
  int echo_r = add_echo(e, GRJIT_TYPE_REF, "echo_ref");
  int echo_p = add_echo(e, GRJIT_TYPE_PTR, "echo_ptr");
  // An integer, including the words that look like pointers and like nothing.
  int caller_i = e.reserve();
  {
    P p("call_i64", {GRJIT_TYPE_I64});
    int r = p.local();
    p.call(r, echo_i, {0});
    p.ret(r);
    e.set(caller_i, p.done());
  }
  for (u64 v : {u64{0}, u64{1}, ~u64{0}, u64{0x8000000000000000ull}, u64{0x00007fffdeadbeefull}}) {
    SCOPED_TRACE(v);
    Outcome o = e.run_compiled(caller_i, {v});
    ASSERT_TRUE(o.finished);
    EXPECT_EQ(o.value, v);
    EXPECT_EQ(o.exit, uint32_t{GRJIT_EXIT_RETURNED});
  }
  // A raw pointer is passed and returned as the word it is, and is never moved.
  int caller_p = e.reserve();
  {
    P p("call_ptr", {GRJIT_TYPE_PTR});
    int r = p.local(GRJIT_TYPE_PTR);
    p.call(r, echo_p, {0});
    p.ret(r);
    e.set(caller_p, p.done());
  }
  static int cell = 5;
  u64 ptr = reinterpret_cast<u64>(&cell);
  Outcome po = e.run_compiled(caller_p, {ptr});
  ASSERT_TRUE(po.finished);
  EXPECT_EQ(po.value, ptr);
  // A reference is a root across the call and across a collection: the callee
  // returns it, and the caller reads the object it names.
  int caller_r = e.reserve();
  {
    P p("call_ref", {});
    int obj = p.local(GRJIT_TYPE_REF), back = p.local(GRJIT_TYPE_REF), v = p.local();
    p.nw(obj, 4242);
    p.call(back, echo_r, {obj});
    p.collect();
    p.get(v, back);
    p.ret(v);
    e.set(caller_r, p.done());
  }
  Outcome ro = e.run_compiled(caller_r, {});
  ASSERT_TRUE(ro.finished);
  EXPECT_EQ(ro.value, 4242u);
  Outcome ri = e.run_interpreted(caller_r, {});
  ASSERT_TRUE(ri.finished);
  EXPECT_EQ(ri.value, 4242u);
  EXPECT_EQ(e.heap.poisoned_reads, 0);
  EXPECT_GT(e.heap.moved, 0) << "objects did move";
  EXPECT_GT(e.heap.collections, 4);
  EXPECT_EQ(e.st.deopts, 0);
}

/* ---- Deep chains, seen precisely ------------------------------------------- */

namespace {

} // namespace

TEST(Calls, AFiftyDeepChainWithACollectionAtTheBottomKeepsEveryFramesReferenceAndMovesIt) {
  CALLS_ONLY_ON_X86_64_SYSV();
  Engine e;
  // Each frame's object holds the value 7; the sum over a chain of n+1 frames is
  // 7 * (n + 1), and a frame whose reference was not updated reads the poison.
  int deep = e.reserve();
  {
    P p("deep", {GRJIT_TYPE_I64});
    int n = 0, obj = p.local(GRJIT_TYPE_REF), zero = p.local(), one = p.local(), t = p.local(),
        nn = p.local(), r = p.local(), v = p.local(), sum = p.local();
    p.nw(obj, 7);
    p.cnst(zero, 0);
    p.cnst(one, 1);
    p.bin(K::EQ, t, n, zero);
    int br = p.brz(t);
    p.collect();
    p.get(v, obj);
    p.ret(v);
    p.patch(br, p.here());
    p.bin(K::SUB, nn, n, one);
    p.call(r, deep, {nn});
    p.get(v, obj);
    p.bin(K::ADD, sum, r, v);
    p.ret(sum);
    e.set(deep, p.done());
  }
  size_t compiled_frames = 0;
  std::set<u64 *> slots;
  e.on_collect = [&](Engine & en) {
    GRCORE_CompiledWalk w;
    ASSERT_EQ(grcore_compiled_walk_begin(en.ctx, &w), GRCORE_OK);
    GRCORE_CompiledFrame f;
    while (grcore_compiled_walk_next(&w, &f) == GRCORE_CWALK_FRAME) {
      compiled_frames++;
    }
    struct V { std::set<u64 *> * slots; size_t dup = 0; } v{&slots};
    GRCORE_RootVisitor rv = {};
    rv.user = &v;
    rv.slot = [](void * user, uint64_t * slot) {
      auto * v = static_cast<V *>(user);
      if (!v->slots->insert(slot).second) {
        v->dup++;
      }
    };
    ASSERT_EQ(grcore_context_enumerate_roots(en.ctx, &rv), GRCORE_OK);
    EXPECT_EQ(v.dup, 0u) << "every slot is visited once";
  };
  Outcome o = e.run_compiled(deep, {49});
  ASSERT_TRUE(o.finished);
  EXPECT_EQ(o.value, 7u * 50u);
  EXPECT_EQ(compiled_frames, 50u) << "the walk saw every compiled frame";
  // One reference per frame, 50 of them, each a VALUE slot reported once: the
  // guest frames' own (stale) slots are not reported beside them.
  EXPECT_EQ(slots.size(), 50u) << "50 frames, one reference each";
  EXPECT_EQ(e.heap.poisoned_reads, 0);
  EXPECT_GT(e.heap.moved, 0);
  EXPECT_EQ(e.st.deopts, 0);
}

/* ---- Dispatch: through a slot, through a code pointer ----------------------- */

namespace {

/* inc(x) = x + 1 */
int add_inc(Engine & e) {
  P p("inc", {GRJIT_TYPE_I64});
  int one = p.local(), r = p.local();
  p.cnst(one, 1);
  p.bin(K::ADD, r, 0, one);
  p.ret(r);
  return e.add(p.done());
}

/* via_slot(x) = inc(inc(x)) * 2 */
int add_via_slot(Engine & e, int inc) {
  P p("via_slot", {GRJIT_TYPE_I64});
  int a = p.local(), b = p.local(), two = p.local(), r = p.local();
  p.call(a, inc, {0});
  p.call(b, inc, {a});
  p.cnst(two, 2);
  p.bin(K::MUL, r, b, two);
  p.ret(r);
  return e.add(p.done());
}

} // namespace

TEST(Calls, ACallThroughACodePointerOfRegisteredCompiledCodeGivesTheSameResults) {
  CALLS_ONLY_ON_X86_64_SYSV();
  Engine e;
  int inc = add_inc(e);
  ASSERT_TRUE(e.compile_fn(inc));
  ASSERT_NE(e.code_of(inc).internal, 0u);
  // The same function by pointer: the entry is found at run time, and no slot
  // is read, which the slot being empty shows.
  int via_ptr = e.reserve();
  {
    P p("via_ptr", {GRJIT_TYPE_I64});
    int ptr = p.local(GRJIT_TYPE_PTR), a = p.local(), b = p.local(), two = p.local(),
        r = p.local();
    p.entryof(ptr, inc);
    p.callp(a, ptr, inc, {0});
    p.callp(b, ptr, inc, {a});
    p.cnst(two, 2);
    p.bin(K::MUL, r, b, two);
    p.ret(r);
    e.set(via_ptr, p.done());
  }
  ASSERT_EQ(grcore_entry_slot_clear(e.ctx, e.slots[inc]), GRCORE_OK);
  EXPECT_EQ(e.slots[inc]->entry, 0u);
  for (int64_t x : {0, 1, 41, -7}) {
    Outcome i = e.run_interpreted(via_ptr, {static_cast<u64>(x)});
    Outcome c = e.run_compiled(via_ptr, {static_cast<u64>(x)});
    ASSERT_TRUE(i.finished && c.finished);
    EXPECT_EQ(c.exit, uint32_t{GRJIT_EXIT_RETURNED});
    EXPECT_EQ(static_cast<int64_t>(c.value), (x + 2) * 2);
    EXPECT_EQ(c.value, i.value);
  }
  EXPECT_EQ(e.slots[inc]->entry, 0u) << "no slot was read, so none was filled";
  EXPECT_EQ(e.st.compile_calls, 0);
  EXPECT_EQ(e.st.deopts, 0);
}

TEST(Calls, ACodePointerThatIsNotTheInternalEntryOfRegisteredCodeIsNeverEntered) {
  CALLS_ONLY_ON_X86_64_SYSV();
  Engine e;
  int inc = add_inc(e);
  ASSERT_TRUE(e.compile_fn(inc));
  const uintptr_t good = e.code_of(inc).internal;
  static char outside[64];
  struct Case {
    const char * name;
    uintptr_t target;
  };
  const Case cases[] = {
      {"null", 0},
      {"an address in no registered code", reinterpret_cast<uintptr_t>(outside)},
      {"the C-ABI entry of registered code, not the internal one", e.code_of(inc).start},
      {"the middle of registered code", good + 3},
      {"one past the entry's code", e.code_of(inc).start + grjit_code_size(e.code_of(inc).code)},
  };
  for (const Case & c : cases) {
    SCOPED_TRACE(c.name);
    int f = e.reserve();
    {
      P p("via_bad_ptr", {GRJIT_TYPE_I64});
      int ptr = p.local(GRJIT_TYPE_PTR), r = p.local();
      p.cnst(ptr, static_cast<int64_t>(c.target));
      p.callp(r, ptr, inc, {0});
      p.ret(r);
      e.set(f, p.done());
    }
    long before = e.st.deopts;
    Outcome o = e.run_compiled(f, {10});
    ASSERT_TRUE(o.finished);
    EXPECT_EQ(o.value, 11u) << "the interpreter finished it with the same answer";
    EXPECT_EQ(o.exit, uint32_t{GRJIT_EXIT_DEOPT}) << "an exit at the call site, never an entry";
    EXPECT_EQ(e.st.deopts - before, 1);
    EXPECT_EQ(e.st.rebuild, GRCORE_OK);
    EXPECT_EQ(o.frames_left, 0u);
  }
  // The control: the real internal entry is entered.
  int f = e.reserve();
  {
    P p("via_good_ptr", {GRJIT_TYPE_I64});
    int ptr = p.local(GRJIT_TYPE_PTR), r = p.local();
    p.cnst(ptr, static_cast<int64_t>(good));
    p.callp(r, ptr, inc, {0});
    p.ret(r);
    e.set(f, p.done());
  }
  long before = e.st.deopts;
  Outcome o = e.run_compiled(f, {10});
  ASSERT_TRUE(o.finished);
  EXPECT_EQ(o.value, 11u);
  EXPECT_EQ(o.exit, uint32_t{GRJIT_EXIT_RETURNED});
  EXPECT_EQ(e.st.deopts, before);
}

TEST(Calls, AnEmptySlotIsCompiledAtTheFirstCallAndCalledDirectlyAfterwards) {
  CALLS_ONLY_ON_X86_64_SYSV();
  Engine e;
  int inc = add_inc(e);
  int top = add_via_slot(e, inc);
  ASSERT_EQ(e.slots[inc]->entry, 0u);
  Outcome first = e.run_compiled(top, {5});
  ASSERT_TRUE(first.finished);
  EXPECT_EQ(first.value, 14u);
  EXPECT_EQ(first.exit, uint32_t{GRJIT_EXIT_RETURNED});
  EXPECT_EQ(e.st.compile_calls, 1) << "asked once, at the first call; the second is direct";
  EXPECT_EQ(e.st.compiles, 2);
  EXPECT_NE(e.slots[inc]->entry, 0u);
  Outcome again = e.run_compiled(top, {6});
  ASSERT_TRUE(again.finished);
  EXPECT_EQ(again.value, 16u);
  EXPECT_EQ(e.st.compile_calls, 1);
  EXPECT_EQ(e.st.deopts, 0);
}

TEST(Calls, ACalleeThatTiersUpLaterIsCalledDirectlyWithoutRecompilingItsCallers) {
  CALLS_ONLY_ON_X86_64_SYSV();
  Engine e;
  int inc = add_inc(e);
  int top = add_via_slot(e, inc);
  ASSERT_TRUE(e.compile_fn(top));
  const GRJIT_Code * caller_code = e.code_of(top).code;
  // The callee is compiled after its caller, as when it tiers up from the
  // interpreter, and installed in its slot.
  ASSERT_TRUE(e.compile_fn(inc));
  Outcome o = e.run_compiled(top, {1});
  ASSERT_TRUE(o.finished);
  EXPECT_EQ(o.value, 6u);
  EXPECT_EQ(e.st.compile_calls, 0) << "the slot was filled, so no one was asked";
  EXPECT_EQ(e.code_of(top).code, caller_code) << "the caller's code is the same code";
  EXPECT_EQ(e.st.compiles, 2);
}

TEST(Calls, ACalleeThatCannotBeCompiledIsAnExitRememberedInItsSlotAndNotAskedAgain) {
  CALLS_ONLY_ON_X86_64_SYSV();
  Engine e;
  int inc = add_inc(e);
  int top = add_via_slot(e, inc);
  e.uncompilable.insert(inc);
  Outcome first = e.run_compiled(top, {5});
  ASSERT_TRUE(first.finished);
  EXPECT_EQ(first.value, 14u) << "the interpreter made the call and finished the run";
  EXPECT_EQ(first.exit, uint32_t{GRJIT_EXIT_DEOPT});
  EXPECT_TRUE(first.interpreted_rest);
  EXPECT_EQ(e.st.compile_calls, 1);
  EXPECT_EQ(e.st.compile_refusals, 1);
  EXPECT_EQ(e.slots[inc]->entry, GRCORE_ENTRY_REFUSED);
  EXPECT_EQ(e.st.rebuild, GRCORE_OK);
  EXPECT_EQ(e.st.deopt_frames, 1) << "only the caller was running; the callee was never pushed";
  EXPECT_EQ(e.st.pushes, e.st.pops);
  // The next call exits at the cost of one compare: no hook, no new refusal.
  Outcome second = e.run_compiled(top, {6});
  ASSERT_TRUE(second.finished);
  EXPECT_EQ(second.value, 16u);
  EXPECT_EQ(second.exit, uint32_t{GRJIT_EXIT_DEOPT});
  EXPECT_EQ(e.st.compile_calls, 1) << "the hook was not called again";
  EXPECT_EQ(e.st.compile_refusals, 1);
  // The callee compiles after all, and a tier-up replaces the mark.
  e.uncompilable.clear();
  ASSERT_TRUE(e.compile_fn(inc));
  EXPECT_GT(e.slots[inc]->entry, GRCORE_ENTRY_REFUSED);
  Outcome third = e.run_compiled(top, {7});
  ASSERT_TRUE(third.finished);
  EXPECT_EQ(third.value, 18u);
  EXPECT_EQ(third.exit, uint32_t{GRJIT_EXIT_RETURNED});
}

TEST(Calls, AHookThatSaysItInstalledButLeftTheSlotEmptyIsAnExitNotAnEntry) {
  CALLS_ONLY_ON_X86_64_SYSV();
  Engine e;
  int inc = add_inc(e);
  int top = add_via_slot(e, inc);
  // The engine's hook reports success and installs nothing: the call exits
  // rather than call through a zero.
  e.lie_about_installing = true;
  Outcome o = e.run_compiled(top, {5});
  ASSERT_TRUE(o.finished);
  EXPECT_EQ(o.value, 14u);
  EXPECT_EQ(o.exit, uint32_t{GRJIT_EXIT_DEOPT});
  EXPECT_EQ(e.st.compile_calls, 1);
}

/* ---- A guard, a poll and a refused push, deep in a chain -------------------- */

namespace {

/* gchain(n) = n + (n-1) + ... + 0, a guard at n == fail_at that fails. */
int add_gchain(Engine & e, int64_t fail_at, bool with_poll = false) {
  int g = e.reserve();
  P p("gchain", {GRJIT_TYPE_I64});
  int n = 0, zero = p.local(), one = p.local(), k = p.local(), hit = p.local(),
      ok = p.local(), t = p.local(), nn = p.local(), r = p.local(), sum = p.local();
  p.cnst(zero, 0);
  p.cnst(one, 1);
  p.cnst(k, fail_at);
  p.bin(K::EQ, hit, n, k);
  p.bin(K::EQ, ok, hit, zero); // zero exactly where the guard must fail
  p.guard(ok);
  if (with_poll) {
    p.poll();
  }
  p.bin(K::EQ, t, n, zero);
  int br = p.brz(t);
  p.ret(zero);
  p.patch(br, p.here());
  p.bin(K::SUB, nn, n, one);
  p.call(r, g, {nn});
  p.bin(K::ADD, sum, r, n);
  p.ret(sum);
  e.set(g, p.done());
  return g;
}

} // namespace

TEST(Calls, AGuardFailingThreeFramesDownRebuildsEveryCompiledFrameAndTheInterpreterFinishes) {
  CALLS_ONLY_ON_X86_64_SYSV();
  Engine e;
  int g = add_gchain(e, /*fail_at=*/7);
  // Ten frames deep with the guard failing in the fourth (n == 7): frames for
  // n = 10, 9, 8 are callers and n = 7 itself fails.
  Outcome uninterrupted = e.run_interpreted(g, {10});
  ASSERT_TRUE(uninterrupted.finished);
  EXPECT_EQ(uninterrupted.value, 55u);
  e.st = Stats{};
  Outcome o = e.run_compiled(g, {10});
  ASSERT_TRUE(o.finished) << "failed=" << o.failed;
  EXPECT_EQ(o.value, uninterrupted.value);
  EXPECT_EQ(o.exit, uint32_t{GRJIT_EXIT_DEOPT});
  EXPECT_TRUE(o.interpreted_rest);
  EXPECT_EQ(e.st.deopts, 1) << "one rebuild for the whole chain";
  EXPECT_EQ(e.st.deopt_frames, 4) << "n = 10, 9, 8 waiting at their calls, and n = 7";
  EXPECT_EQ(e.st.rebuild, GRCORE_OK);
  EXPECT_EQ(e.st.last_cause, 0u);
  EXPECT_EQ(o.frames_left, 0u);
  EXPECT_EQ(e.st.pushes, e.st.pops);
}

TEST(Calls, AGuardAtTheEntryFunctionAndAtTheBottomOfTheChainBothFinishCorrectly) {
  CALLS_ONLY_ON_X86_64_SYSV();
  for (int64_t fail_at : {10, 0, 5}) {
    SCOPED_TRACE(fail_at);
    Engine e;
    int g = add_gchain(e, fail_at);
    Outcome o = e.run_compiled(g, {10});
    ASSERT_TRUE(o.finished);
    EXPECT_EQ(o.value, 55u);
    EXPECT_EQ(o.exit, uint32_t{GRJIT_EXIT_DEOPT});
    EXPECT_EQ(e.st.deopt_frames, 10 - fail_at + 1);
    EXPECT_EQ(e.st.rebuild, GRCORE_OK);
    EXPECT_EQ(o.frames_left, 0u);
  }
}

TEST(Calls, AFailedPollInACalleeRebuildsTheChainAndCarriesThePollHelpersAnswer) {
  CALLS_ONLY_ON_X86_64_SYSV();
  Engine e;
  int g = add_gchain(e, /*fail_at=*/-1, /*with_poll=*/true);
  // A request is pending, so every compiled poll takes its slow path; the third
  // one's helper answers non-zero (a pause, an unwind, a stop).
  uint64_t * request = reinterpret_cast<uint64_t *>(
      reinterpret_cast<unsigned char *>(e.ctx) + grcore_jit_layout()->request_word_offset);
  *request = 1;
  e.poll_slow_calls_to_fail = 3;
  Outcome o = e.run_compiled(g, {10});
  *request = 0;
  ASSERT_TRUE(o.finished);
  EXPECT_EQ(o.value, 55u);
  EXPECT_EQ(o.exit, uint32_t{GRJIT_EXIT_DEOPT});
  EXPECT_EQ(e.st.poll_slow, 3);
  EXPECT_EQ(e.st.deopts, 1);
  EXPECT_EQ(e.st.last_cause, 1u) << "the helper's answer is the cause";
  EXPECT_EQ(e.st.deopt_frames, 3) << "n = 10, 9 waiting and n = 8 polling";
  EXPECT_EQ(e.st.rebuild, GRCORE_OK);
}

TEST(Calls, APushThatIsRefusedIsAnExitAtTheCallSiteAndTheInterpreterMakesTheCall) {
  CALLS_ONLY_ON_X86_64_SYSV();
  Engine e;
  int g = add_gchain(e, -1);
  e.refuse_push_at = 4; // the fourth call, three frames down
  Outcome o = e.run_compiled(g, {10});
  ASSERT_TRUE(o.finished);
  EXPECT_EQ(o.value, 55u);
  EXPECT_EQ(o.exit, uint32_t{GRJIT_EXIT_DEOPT});
  EXPECT_EQ(e.st.refused_pushes, 1);
  EXPECT_EQ(e.st.deopt_frames, 4) << "the entry and three callees, the fourth callee never pushed";
  EXPECT_EQ(e.st.rebuild, GRCORE_OK);
  EXPECT_EQ(o.frames_left, 0u);
}

TEST(Calls, TheGuestDepthBudgetRefusesAPushAtTheSameDepthCompiledAndInterpreted) {
  CALLS_ONLY_ON_X86_64_SYSV();
  for (uint64_t depth : {uint64_t{3}, uint64_t{10}, uint64_t{25}}) {
    SCOPED_TRACE(depth);
    Engine e(depth);
    int g = add_gchain(e, -1);
    Outcome i = e.run_interpreted(g, {100});
    ASSERT_TRUE(i.limit);
    Outcome c = e.run_compiled(g, {100});
    EXPECT_TRUE(c.limit) << "the budget's verdict is the same in both tiers";
    EXPECT_EQ(c.limit_depth, i.limit_depth);
    EXPECT_EQ(i.limit_depth, depth);
    EXPECT_EQ(c.exit, uint32_t{GRJIT_EXIT_DEOPT});
    EXPECT_EQ(e.st.rebuild, GRCORE_OK);
    EXPECT_EQ(c.frames_left, 0u);
    // Within the budget the compiled run finishes with no exit.
    Outcome fits = e.run_compiled(g, {static_cast<u64>(depth - 2)});
    ASSERT_TRUE(fits.finished);
    EXPECT_EQ(fits.exit, uint32_t{GRJIT_EXIT_RETURNED});
  }
}

/* ---- Native stack, in bytes -------------------------------------------------- */

namespace {

/* rec(n) = n + (n-1) + ... + 0, probing the frame it runs in at every level. */
int add_rec(Engine & e) {
  int rec = e.reserve();
  P p("rec", {GRJIT_TYPE_I64});
  int n = 0, zero = p.local(), one = p.local(), t = p.local(), nn = p.local(), r = p.local(),
      sum = p.local();
  p.cnst(zero, 0);
  p.cnst(one, 1);
  p.probe(n);
  p.bin(K::EQ, t, n, zero);
  int br = p.brz(t);
  p.ret(zero);
  p.patch(br, p.here());
  p.bin(K::SUB, nn, n, one);
  p.call(r, rec, {nn});
  p.bin(K::ADD, sum, r, n);
  p.ret(sum);
  e.set(rec, p.done());
  return rec;
}

} // namespace

TEST(Calls, ATinyNativeStackDeoptimizesTheChainAtTheCallSiteAndTheInterpreterFinishes) {
  CALLS_ONLY_ON_X86_64_SYSV();
  Engine e(GRCORE_UNLIMITED, /*native_bytes=*/6000);
  int rec = add_rec(e);
  Outcome i = e.run_interpreted(rec, {400});
  ASSERT_TRUE(i.finished);
  EXPECT_EQ(i.value, 400u * 401u / 2u);
  e.st = Stats{};
  Outcome c = e.run_compiled(rec, {400});
  ASSERT_TRUE(c.finished) << "no fault, and the interpreted run's output";
  EXPECT_EQ(c.value, i.value);
  EXPECT_EQ(c.exit, uint32_t{GRJIT_EXIT_DEOPT});
  EXPECT_TRUE(c.interpreted_rest);
  EXPECT_EQ(e.st.deopts, 1);
  EXPECT_EQ(e.st.rebuild, GRCORE_OK);
  EXPECT_GT(e.st.deopt_frames, 10);
  EXPECT_LT(e.st.deopt_frames, 400) << "the chain stopped where the stack ran out";
  EXPECT_EQ(c.frames_left, 0u);
  EXPECT_EQ(e.st.pushes, e.st.pops);
}

TEST(Calls, TheStackLimitIsMeasuredInBytesAndTheFirstFrameThatDoesNotFitIsTheOneRefused) {
  CALLS_ONLY_ON_X86_64_SYSV();
  // The budget swept across a frame's width, so the boundary falls at every
  // alignment: every frame admitted lies wholly above the limit, and the next one
  // would not have.
  for (uint64_t bytes = 5000; bytes < 5000 + 24 * 8; bytes += 8) {
    SCOPED_TRACE(bytes);
    Engine e(GRCORE_UNLIMITED, bytes);
    int rec = add_rec(e);
    ASSERT_TRUE(e.compile_fn(rec));
    const GRCORE_CodeMeta * meta = grjit_code_meta(e.code_of(rec).code);
    const uintptr_t frame = meta->frame_bytes;
    Outcome c = e.run_compiled(rec, {400});
    ASSERT_TRUE(c.finished);
    ASSERT_EQ(c.exit, uint32_t{GRJIT_EXIT_DEOPT});
    ASSERT_GE(e.st.bases.size(), 3u);
    // The limit run set from its own stack pointer, which the entry's frame base
    // is a little below: derive it from the first probed frame instead.
    uintptr_t limit = 0;
    // The adapter's caller is run_compiled's call, so the limit is whatever
    // grcore_context_native_limit_here computed: recomputed here the same way.
    (void)limit;
    uintptr_t lowest_bottom = UINTPTR_MAX;
    for (uintptr_t base : e.st.bases) {
      lowest_bottom = std::min(lowest_bottom, base - frame);
    }
    EXPECT_GE(lowest_bottom, e.last_native_limit) << "an admitted frame lies above the limit";
    EXPECT_LT(lowest_bottom - 16 - frame, e.last_native_limit)
        << "the next frame would have crossed it, so it was the one refused";
  }
}

TEST(Calls, NoNativeStackBudgetMeansNoLimitAndADeepChainRunsToTheEndCompiled) {
  CALLS_ONLY_ON_X86_64_SYSV();
  Engine e;
  int rec = add_rec(e);
  Outcome c = e.run_compiled(rec, {2000});
  ASSERT_TRUE(c.finished);
  EXPECT_EQ(c.exit, uint32_t{GRJIT_EXIT_RETURNED});
  EXPECT_EQ(c.value, 2000u * 2001u / 2u);
  EXPECT_EQ(e.st.deopts, 0);
}

TEST(Calls, AStackLimitTheEntryFunctionItselfCannotMeetLeavesItsGuestFrameAtItsEntry) {
  CALLS_ONLY_ON_X86_64_SYSV();
  // So small that not even the entry function's own frame fits: it deoptimizes
  // before it starts, with no compiled frame to rebuild, and the interpreter runs
  // the function from its entry.
  Engine e(GRCORE_UNLIMITED, /*native_bytes=*/64);
  int rec = add_rec(e);
  Outcome c = e.run_compiled(rec, {20});
  ASSERT_TRUE(c.finished);
  EXPECT_EQ(c.value, 210u);
  EXPECT_EQ(c.exit, uint32_t{GRJIT_EXIT_DEOPT});
  EXPECT_EQ(e.st.deopts, 1);
  EXPECT_EQ(e.st.deopt_frames, 0);
  EXPECT_EQ(e.st.rebuild, GRCORE_OK);
}

/* ---- The reservation ---------------------------------------------------------- */

TEST(Calls, EveryCallExtendsTheReservationByTheCalleesMaximumAndAChainRebuildNeverFails) {
  CALLS_ONLY_ON_X86_64_SYSV();
  // An engine whose I64 locals are held converted in its frames: the IR has no
  // converting type, so the engine's table says its frame-state words are raw
  // I64 values, and a rebuild converts them from the reservation.
  Engine e(GRCORE_UNLIMITED, GRCORE_UNLIMITED, /*conv=*/true);
  int g = add_gchain(e, /*fail_at=*/0);
  Outcome o = e.run_compiled(g, {49});
  ASSERT_TRUE(o.finished);
  EXPECT_EQ(o.value, 49u * 50u / 2u);
  EXPECT_EQ(o.exit, uint32_t{GRJIT_EXIT_DEOPT});
  EXPECT_EQ(e.st.deopt_frames, 50);
  EXPECT_EQ(e.st.rebuild, GRCORE_OK) << "a chain of fifty cannot fail for want of a cell";
  EXPECT_GT(e.code_of(g).max_converting, 0u);
  EXPECT_EQ(e.st.extends, 49) << "one per call";
  EXPECT_EQ(e.st.retracts, 0) << "the callees never returned: they were rebuilt";
  EXPECT_EQ(o.frames_left, 0u);
  // The same chain with no guard: every callee returns, giving its cells back.
  Engine f(GRCORE_UNLIMITED, GRCORE_UNLIMITED, true);
  int h = add_gchain(f, -1);
  Outcome n = f.run_compiled(h, {49});
  ASSERT_TRUE(n.finished);
  EXPECT_EQ(n.exit, uint32_t{GRJIT_EXIT_RETURNED});
  EXPECT_EQ(f.st.extends, f.st.retracts);
  EXPECT_EQ(grcore_deopt_reservation_capacity(f.reservation), 0u);
}

TEST(Calls, AReservationExtensionOneCellShortIsRefusedUpFrontAndNothingIsWritten) {
  CALLS_ONLY_ON_X86_64_SYSV();
  // A reservation sized one cell short is an engine's defect, not a resource
  // that ran out (that is the allocator refusing an extension, below, which is
  // an exit at the call site). The chain rebuild refuses it before writing
  // anything, the hook reports the refusal, and the run ends in the fatal exit.
  for (long short_by : {0L, 1L}) {
    SCOPED_TRACE(short_by);
    Engine e(GRCORE_UNLIMITED, GRCORE_UNLIMITED, /*conv=*/true);
    int g = add_gchain(e, /*fail_at=*/0);
    ASSERT_TRUE(e.compile_fn(g));
    e.short_by = short_by;
    Outcome o = e.run_compiled(g, {49});
    if (short_by == 0) {
      EXPECT_EQ(e.st.rebuild, GRCORE_OK);
      EXPECT_TRUE(o.finished);
      EXPECT_FALSE(o.rebuild_failed);
    } else {
      EXPECT_EQ(e.st.rebuild, GRCORE_ERR_INVALID);
      EXPECT_TRUE(o.rebuild_failed);
      EXPECT_EQ(o.failed_with, static_cast<u64>(GRCORE_ERR_INVALID));
      ASSERT_EQ(o.pcs_after_failure.size(), 50u);
      for (u64 pc : o.pcs_after_failure) {
        EXPECT_EQ(pc, 0u) << "not one guest frame was written";
      }
    }
  }
}

/* ---- Code a frame returns into ------------------------------------------------ */

TEST(Calls, ACodeFunctionWhoseSlotIsClearedUnderItsOwnFrameKeepsRunningAndIsReleasedAfterTheLastJitActivation) {
  CALLS_ONLY_ON_X86_64_SYSV();
  Engine e;
  // inner() clears its own slot, then runs on: a helper that allocates and a
  // read of what it allocated, all in code whose slot is empty and whose range
  // is unregistered. outer() calls it and uses the result.
  int inner = e.reserve();
  {
    P p("inner", {});
    int obj = p.local(GRJIT_TYPE_REF), v = p.local(), k = p.local(), r = p.local();
    p.clearslot(inner);
    p.nw(obj, 30);
    p.collect();
    p.get(v, obj);
    p.cnst(k, 12);
    p.probe(k);
    p.bin(K::ADD, r, v, k);
    p.ret(r);
    e.set(inner, p.done());
  }
  int outer = e.reserve();
  {
    P p("outer", {});
    int a = p.local(), one = p.local(), r = p.local();
    p.cnst(one, 1);
    p.call(a, inner, {});
    p.bin(K::ADD, r, a, one);
    p.ret(r);
    e.set(outer, p.done());
  }
  std::vector<int> released_at_probe;
  e.on_probe = [&](Engine & en) { released_at_probe.push_back(*en.released); };
  ASSERT_TRUE(e.compile_fn(inner));
  ASSERT_TRUE(e.compile_fn(outer));
  Outcome o = e.run_compiled(outer, {});
  ASSERT_TRUE(o.finished);
  EXPECT_EQ(o.value, 43u);
  EXPECT_EQ(o.exit, uint32_t{GRJIT_EXIT_RETURNED});
  ASSERT_EQ(released_at_probe.size(), 1u);
  EXPECT_EQ(released_at_probe[0], 0) << "inner's frame was still running in code whose slot was cleared";
  EXPECT_EQ(*e.released, 1) << "released when the last JIT activation left";
  EXPECT_EQ(e.slots[inner]->entry, 0u);
  EXPECT_EQ(grcore_code_retired_count(e.ctx), 0u);
  EXPECT_EQ(e.heap.poisoned_reads, 0);
}

TEST(Calls, RepeatedReplacementUnderOneLongLivedActivationRetainsEachReplacedFunction) {
  CALLS_ONLY_ON_X86_64_SYSV();
  Engine e;
  int inc = add_inc(e);
  GRCORE_ActivationRef jit;
  ASSERT_EQ(grcore_activation_enter(e.stack, GRCORE_ACTIVATION_JIT, e.engine, false, nullptr, &jit),
      GRCORE_OK);
  const uint64_t base_bytes = grcore_context_memory_in_use(e.ctx);
  constexpr int kReplacements = 200;
  for (int i = 0; i < kReplacements; i++) {
    ASSERT_TRUE(e.compile_fn(inc));
    e.discard(inc);
  }
  const size_t peak = grcore_code_retired_peak(e.ctx);
  const uint64_t held_bytes = grcore_context_memory_in_use(e.ctx) - base_bytes;
  EXPECT_EQ(peak, static_cast<size_t>(2 * kReplacements)) << "a retired slot reference and a retired range each";
  EXPECT_EQ(grcore_code_retired_count(e.ctx), peak);
  EXPECT_EQ(*e.released, 0);
  // Each replaced function holds its page: this is the figure design.md records.
  EXPECT_GE(held_bytes, static_cast<uint64_t>(kReplacements) * 4096u);
  std::printf("retired-list peak %zu references over %d replacements; %llu bytes held "
              "(%llu per replaced function)\n",
      peak, kReplacements, static_cast<unsigned long long>(held_bytes),
      static_cast<unsigned long long>(held_bytes / kReplacements));
  ASSERT_EQ(grcore_activation_leave(e.stack, jit), GRCORE_OK);
  EXPECT_EQ(*e.released, kReplacements) << "every one is released when the activation leaves";
  EXPECT_EQ(grcore_code_retired_count(e.ctx), 0u);
  EXPECT_EQ(grcore_code_retired_peak(e.ctx), peak) << "the high-water mark stays";
  // The pages went back; what is left is the registry's own bookkeeping.
  EXPECT_LT(grcore_context_memory_in_use(e.ctx) - base_bytes, 64u * 1024u);
}

/* ---- Registers a callee must not touch ------------------------------------------ */

TEST(Calls, NoCalleeSavedRegisterIsEverChangedByCompiledCodeThroughTheEntryOrAChain) {
  CALLS_ONLY_ON_X86_64_SYSV();
#if FX_HAVE_CALLS_ASM
  Engine e;
  int g = add_gchain(e, -1);
  int deep = add_gchain(e, 4);
  // A chain that returns, and one that deoptimizes in the middle of it: the C
  // caller's rbx and r12-r15 come back as they went in either way.
  for (int fn : {g, deep}) {
    SCOPED_TRACE(fn);
    ASSERT_TRUE(e.compile_fn(fn));
    e.base_frames = grcore_stack_frame_count(e.stack);
    ASSERT_TRUE(e.push_frame(fn, std::vector<u64>{9}.data(), 1, false));
    grcore_context_native_limit_here(e.ctx);
    GRCORE_ActivationRef rec;
    ASSERT_EQ(grcore_activation_enter(e.stack, GRCORE_ACTIVATION_JIT, e.engine, false, nullptr, &rec),
        GRCORE_OK);
    const GRJIT_Code * code = e.code_of(fn).code;
    uint64_t in[1] = {9};
    std::vector<uint64_t> out(grjit_code_out_words(code), 0);
    uint64_t regs[fx::kSentinelCount];
    for (size_t i = 0; i < fx::kSentinelCount; i++) {
      regs[i] = fx::sentinel_value(i);
    }
    // Through a trampoline that sets the registers and reads them back: calling
    // a callee that does not preserve them directly would corrupt this very
    // function, which is what the test is for.
    fx_call_with_sentinels(grjit_code_entry(code), e.ctx, in, out.data(), regs);
    for (size_t i = 0; i < fx::kSentinelCount; i++) {
      EXPECT_EQ(regs[i], fx::sentinel_value(i)) << fx::sentinel_name(i);
    }
    ASSERT_EQ(grcore_activation_leave(e.stack, rec), GRCORE_OK);
    grcore_unwind_all(e.stack, nullptr);
    e.reset_reservation();
  }
#endif
}

/* ---- A collection at every GC point, in a deep chain ---------------------------- */

TEST(Calls, CollectingAtEveryGcPointInAFiftyDeepChainWithAGuardFailingMidwayLosesNothing) {
  CALLS_ONLY_ON_X86_64_SYSV();
  Engine e;
  e.torture = true;
  int deep = e.reserve();
  {
    P p("deep", {GRJIT_TYPE_I64});
    int n = 0, obj = p.local(GRJIT_TYPE_REF), zero = p.local(), one = p.local(),
        t = p.local(), nn = p.local(), r = p.local(), v = p.local(), sum = p.local(),
        k = p.local(), hit = p.local(), ok = p.local();
    p.nw(obj, 3);
    p.cnst(zero, 0);
    p.cnst(one, 1);
    p.cnst(k, 20); // the guard fails at n == 20, thirty frames down from 49
    p.bin(K::EQ, hit, n, k);
    p.bin(K::EQ, ok, hit, zero);
    p.guard(ok);
    p.poll();
    p.bin(K::EQ, t, n, zero);
    int br = p.brz(t);
    p.collect();
    p.get(v, obj);
    p.ret(v);
    p.patch(br, p.here());
    p.bin(K::SUB, nn, n, one);
    p.call(r, deep, {nn});
    p.collect();
    p.get(v, obj);
    p.bin(K::ADD, sum, r, v);
    p.ret(sum);
    e.set(deep, p.done());
  }
  uint64_t * request = reinterpret_cast<uint64_t *>(
      reinterpret_cast<unsigned char *>(e.ctx) + grcore_jit_layout()->request_word_offset);
  *request = 1; // every compiled poll takes its slow path, a GC point
  Outcome o = e.run_compiled(deep, {49});
  *request = 0;
  ASSERT_TRUE(o.finished);
  EXPECT_EQ(o.value, 3u * 50u);
  EXPECT_EQ(o.exit, uint32_t{GRJIT_EXIT_DEOPT});
  EXPECT_EQ(e.st.deopt_frames, 30);
  EXPECT_EQ(e.heap.poisoned_reads, 0);
  EXPECT_GT(e.heap.collections, 100);
  EXPECT_GT(e.heap.moved, 100);
}

/* ---- A corrupt chain is never skipped --------------------------------------------- */

namespace {

struct Child {
  bool aborted = false;
  std::string err;
};

Child in_child(const std::function<void()> & fn) {
  Child r;
#ifdef _WIN32
  (void)fn; // no fork here; the callers that need one do not run on this target
#else
  int fds[2];
  EXPECT_EQ(pipe(fds), 0);
  std::fflush(nullptr);
  pid_t pid = fork();
  if (pid == 0) {
    close(fds[0]);
    dup2(fds[1], 2);
    fn();
    _exit(0);
  }
  close(fds[1]);
  char buf[256];
  ssize_t n;
  while ((n = read(fds[0], buf, sizeof buf)) > 0) {
    r.err.append(buf, static_cast<size_t>(n));
  }
  close(fds[0]);
  int status = 0;
  EXPECT_EQ(waitpid(pid, &status, 0), pid);
  r.aborted = WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT;
#endif
  return r;
}

} // namespace

TEST(Calls, ACorruptLinkOrReturnWordInARealChainStopsEnumerationWithAMessage) {
  CALLS_ONLY_ON_X86_64_SYSV();
  Engine e;
  int deep = e.reserve();
  {
    P p("deep", {GRJIT_TYPE_I64});
    int n = 0, obj = p.local(GRJIT_TYPE_REF), zero = p.local(), one = p.local(),
        t = p.local(), nn = p.local(), r = p.local(), v = p.local(), sum = p.local();
    p.nw(obj, 1);
    p.cnst(zero, 0);
    p.cnst(one, 1);
    p.bin(K::EQ, t, n, zero);
    int br = p.brz(t);
    p.collect();
    p.get(v, obj);
    p.ret(v);
    p.patch(br, p.here());
    p.bin(K::SUB, nn, n, one);
    p.call(r, deep, {nn});
    p.get(v, obj);
    p.bin(K::ADD, sum, r, v);
    p.ret(sum);
    e.set(deep, p.done());
  }
  struct Corruption {
    const char * name;
    std::function<void(uintptr_t base)> corrupt; // applied to the fifth frame's words
    const char * message;
  };
  const Corruption cases[] = {
      {"a return address that is in no registered code",
          [](uintptr_t base) { *reinterpret_cast<uintptr_t *>(base + 8) = 0x1234560; },
          "broken"},
      {"a return address that is in registered code at no site",
          [](uintptr_t base) { *reinterpret_cast<uintptr_t *>(base + 8) += 1; }, "broken"},
      {"a link that is not word-aligned",
          [](uintptr_t base) { *reinterpret_cast<uintptr_t *>(base) += 4; }, "broken"},
      {"a link that is below the frame it belongs to",
          [](uintptr_t base) { *reinterpret_cast<uintptr_t *>(base) = base - 64; }, "broken"},
      {"a link that is zero where the chain-end marker should be",
          [](uintptr_t) {}, "broken"},
  };
  bool ran = false;
  e.on_collect = [&](Engine & en) {
    ran = true;
    std::vector<uintptr_t> bases;
    GRCORE_CompiledWalk w;
    ASSERT_EQ(grcore_compiled_walk_begin(en.ctx, &w), GRCORE_OK);
    GRCORE_CompiledFrame f;
    while (grcore_compiled_walk_next(&w, &f) == GRCORE_CWALK_FRAME) {
      bases.push_back(f.frame_base);
    }
    ASSERT_EQ(bases.size(), 10u);
    for (const Corruption & c : cases) {
      SCOPED_TRACE(c.name);
      bool last = std::string(c.name).find("marker") != std::string::npos;
      Child r = in_child([&] {
        c.corrupt(bases[4]);
        if (last) {
          *reinterpret_cast<uintptr_t *>(bases[9]) = 0; // the entry stub's marker
        }
        GRCORE_RootVisitor v = {};
        v.slot = [](void *, uint64_t *) {};
        v.range = [](void *, const GRCORE_ConservativeRange *) {};
        grcore_context_enumerate_roots(en.ctx, &v);
      });
      EXPECT_TRUE(r.aborted) << "enumeration went on past a broken chain";
      EXPECT_NE(r.err.find("chain of compiled frames is broken"), std::string::npos) << r.err;
    }
    // And the control: the same enumeration, uncorrupted, in a child, goes through.
    Child ok = in_child([&] {
      GRCORE_RootVisitor v = {};
      v.slot = [](void *, uint64_t *) {};
      grcore_context_enumerate_roots(en.ctx, &v);
    });
    EXPECT_FALSE(ok.aborted) << ok.err;
  };
  Outcome o = e.run_compiled(deep, {9});
  ASSERT_TRUE(o.finished);
  EXPECT_TRUE(ran);
  EXPECT_EQ(o.value, 10u);
}

/* ---- The entry hook, the layout, the other backends ------------------------------- */

namespace {

uint32_t refuse_with_seven(void *) { return 7; }
uint32_t accept(void *) { return 0; }
void h_unused_pop(void *) {}
uint32_t h_unused_deopt(void *, uint64_t) { return 0; }

GRJIT_Function * callable_identity(GRJIT_Type t) {
  B b("ident", 0);
  GRJIT_VReg x = b.param(t);
  GRJIT_CallHooks h{};
  h.deopt = h_unused_deopt;
  h.pop = h_unused_pop;
  b.callable(h);
  b.at(b.block());
  b.ret(V(x));
  return b.finish();
}

} // namespace

TEST(Calls, TheEntryHookRunsInTheAdapterBeforeAnythingAndItsRefusalIsReportedAsRefused) {
  CALLS_ONLY_ON_X86_64_SYSV();
  JitWorld w;
  Fn f(callable_identity(GRJIT_TYPE_I64));
  {
    Compiled c(f, w.pages(), refuse_with_seven);
    ASSERT_TRUE(c) << c.result;
    // The context is the one the code was built against only for its layout; a
    // refusal runs before any of the code does.
    auto r = c.run(w.ctx, {123});
    EXPECT_EQ(r.exit, uint32_t{GRJIT_EXIT_REFUSED});
    EXPECT_EQ(r.out[0], 7u);
  }
  {
    Compiled c(f, w.pages(), accept);
    ASSERT_TRUE(c) << c.result;
    auto r = c.run(w.ctx, {123});
    EXPECT_EQ(r.exit, uint32_t{GRJIT_EXIT_RETURNED});
    EXPECT_EQ(r.out[0], 123u);
  }
}

TEST(Calls, ACallableFunctionHasAnInternalEntryAndAPlainOneHasNone) {
  CALLS_ONLY_ON_X86_64_SYSV();
  JitWorld w;
  Fn f(callable_identity(GRJIT_TYPE_I64));
  Compiled c(f, w.pages());
  ASSERT_TRUE(c) << c.result;
  EXPECT_TRUE(grjit_code_callable(c.code));
  uintptr_t internal = grjit_code_internal_entry(c.code);
  uintptr_t start = reinterpret_cast<uintptr_t>(grjit_code_address(c.code));
  EXPECT_GT(internal, start) << "after the adapter";
  EXPECT_LT(internal, start + grjit_code_size(c.code));
  EXPECT_EQ(internal % 16, 0u);
  uint64_t tag[2];
  std::memcpy(tag, reinterpret_cast<const void *>(internal - 16), 16);
  EXPECT_EQ(tag[0], (UINT64_C(0x4752494E) << 32) | 1u) << "the magic and the parameter count";
  EXPECT_EQ(tag[1], 0u) << "the token, zero by default";
  EXPECT_EQ(reinterpret_cast<uintptr_t>(grjit_code_entry(c.code)), start);
  B plain("plain", 0);
  plain.at(plain.block());
  plain.ret();
  Fn pf(plain.finish());
  Compiled pc(pf, w.pages());
  ASSERT_TRUE(pc);
  EXPECT_FALSE(grjit_code_callable(pc.code));
  EXPECT_EQ(grjit_code_internal_entry(pc.code), 0u);
  EXPECT_EQ(grjit_code_internal_entry(nullptr), 0u);
  EXPECT_FALSE(grjit_code_callable(nullptr));
}

TEST(Calls, EveryArchitectureButX86SysVRefusesACallableFunctionBeforeEmittingAByte) {
  Fn f(callable_identity(GRJIT_TYPE_I64));
  GRCORE_JitLayout layout = *grcore_jit_layout();
  for (GRJIT_Arch arch : {GRJIT_ARCH_ARM64, GRJIT_ARCH_X86_64_WIN64}) {
    SCOPED_TRACE(arch);
    GRJIT_Emitted out;
    TrackingAllocator alloc;
    EXPECT_EQ(grjit_emit_for(arch, f, alloc.get(), nullptr, nullptr, layout.request_word_offset, &out),
        GRJIT_ERR_UNSUPPORTED);
    EXPECT_EQ(alloc.live, 0) << "nothing was left allocated";
    EXPECT_EQ(alloc.calls, 0) << "and nothing was even asked for";
    EXPECT_EQ(out.size, 0u);
  }
  // And the same function is emitted for x86-64 SysV on any host.
  GRJIT_Emitted out;
  EXPECT_EQ(grjit_emit_for(GRJIT_ARCH_X86_64, f, grjit_allocator_default(), nullptr, nullptr,
                layout.request_word_offset, &out),
      GRJIT_OK);
  EXPECT_GT(out.size, 0u);
  EXPECT_GT(out.internal_offset, 0u);
  grjit_emitted_free(&out);
}

TEST(Calls, AFunctionWithoutTheNewOperationsCarriesNoCallMachineryOnAnyArchitecture) {
  // That its bytes are exactly what they were is the pins' claim (test_pin, over a
  // large corpus). What this holds is the part of it that is visible from here:
  // a plain function has no internal entry and none of the call machinery's tag.
  B b("plain", 0);
  GRJIT_VReg x = b.param(GRJIT_TYPE_I64);
  GRJIT_VReg y = b.reg();
  b.at(b.block());
  b.bin(GRJIT_OP_ADD, y, V(x), I(5));
  b.ret(V(y));
  Fn f(b.finish());
  GRCORE_JitLayout layout = *grcore_jit_layout();
  for (GRJIT_Arch arch : {GRJIT_ARCH_X86_64, GRJIT_ARCH_ARM64, GRJIT_ARCH_X86_64_WIN64}) {
    SCOPED_TRACE(arch);
    GRJIT_Emitted a;
    ASSERT_EQ(grjit_emit_for(arch, f, grjit_allocator_default(), nullptr, nullptr,
                  layout.request_word_offset, &a), GRJIT_OK);
    EXPECT_EQ(a.internal_offset, 0u);
    const uint32_t magic = 0x4752494E;
    bool found = false;
    for (size_t i = 0; i + 4 <= a.size; i++) {
      uint32_t w;
      std::memcpy(&w, a.bytes + i, 4);
      found = found || w == magic;
    }
    EXPECT_FALSE(found) << "no tag, so no adapter and no internal entry";
    grjit_emitted_free(&a);
  }
}

TEST(Calls, AnOverflowingCalleesGuestFrameHoldsTheArgumentsItWasCalledWith) {
  CALLS_ONLY_ON_X86_64_SYSV();
  // The push hook is handed the arguments in the caller's frame, in order. A
  // callee that cannot start (its stack check fails) is finished by the
  // interpreter from the guest frame the hook made, so the arguments must have
  // arrived there in their own places.
  Engine e(GRCORE_UNLIMITED, /*native_bytes=*/7000);
  int r = e.reserve();
  {
    P p("rec4", {GRJIT_TYPE_I64, GRJIT_TYPE_I64, GRJIT_TYPE_I64, GRJIT_TYPE_I64});
    int n = 0, a = 1, b = 2, c = 3, zero = p.local(), one = p.local(), two = p.local(),
        three = p.local(), t = p.local(), nn = p.local(), a1 = p.local(), b1 = p.local(),
        c1 = p.local(), res = p.local(), s1 = p.local(), s2 = p.local(), s3 = p.local();
    p.cnst(zero, 0);
    p.cnst(one, 1);
    p.cnst(two, 2);
    p.cnst(three, 3);
    p.bin(K::EQ, t, n, zero);
    int br = p.brz(t);
    p.bin(K::ADD, s1, a, b);
    p.bin(K::ADD, s2, s1, c);
    p.ret(s2);
    p.patch(br, p.here());
    p.bin(K::SUB, nn, n, one);
    p.bin(K::ADD, a1, a, one);
    p.bin(K::ADD, b1, b, two);
    p.bin(K::ADD, c1, c, three);
    p.call(res, r, {nn, a1, b1, c1});
    p.bin(K::ADD, s3, res, n);
    p.ret(s3);
    e.set(r, p.done());
  }
  auto ref = [](int64_t n, int64_t a, int64_t b, int64_t c) {
    int64_t acc = 0;
    for (; n > 0; n--) {
      acc += n;
      a += 1;
      b += 2;
      c += 3;
    }
    return acc + a + b + c;
  };
  Outcome i = e.run_interpreted(r, {300, 10, 20, 30});
  ASSERT_TRUE(i.finished);
  EXPECT_EQ(static_cast<int64_t>(i.value), ref(300, 10, 20, 30));
  Outcome c = e.run_compiled(r, {300, 10, 20, 30});
  ASSERT_TRUE(c.finished);
  EXPECT_EQ(c.exit, uint32_t{GRJIT_EXIT_DEOPT});
  EXPECT_EQ(static_cast<int64_t>(c.value), ref(300, 10, 20, 30));
  EXPECT_EQ(e.st.rebuild, GRCORE_OK);
}

TEST(Calls, ArgumentsOfAllThreeTypesMixedAtEveryCountFromOneToSixteenArriveIntactUnderCollection) {
  CALLS_ONLY_ON_X86_64_SYSV();
  // callee(a0 ... a{n-1}) with a_i an I64, a REF or a PTR in turn; it returns the
  // sum of (i + 1) times each one's value: the integer, the integer an object
  // holds, or the pointer's low byte. The caller makes them, and every push
  // collects and moves, so a reference passed in a register or on the stack is
  // read after the collection from where the collector put it.
  Engine e;
  e.torture = true;
  for (int n = 1; n <= 16; n++) {
    SCOPED_TRACE(n);
    std::vector<GRJIT_Type> types;
    for (int i = 0; i < n; i++) {
      types.push_back(i % 3 == 0 ? GRJIT_TYPE_I64 : i % 3 == 1 ? GRJIT_TYPE_REF : GRJIT_TYPE_PTR);
    }
    int callee = e.reserve();
    {
      P p("mixed", types);
      int acc = p.local(), c = p.local(), t = p.local(), mask = p.local();
      p.cnst(acc, 0);
      p.cnst(mask, 255);
      for (int i = 0; i < n; i++) {
        if (types[i] == GRJIT_TYPE_I64) {
          p.mov(t, i);
        } else if (types[i] == GRJIT_TYPE_REF) {
          p.get(t, i);
        } else {
          p.bin(K::AND, t, i, mask);
        }
        p.cnst(c, i + 1);
        p.bin(K::MUL, t, t, c);
        p.bin(K::ADD, acc, acc, t);
      }
      p.ret(acc);
      e.set(callee, p.done());
    }
    int caller = e.reserve();
    {
      P p("mixed_caller", {GRJIT_TYPE_I64});
      int x = 0, r = p.local();
      std::vector<int> args;
      for (int i = 0; i < n; i++) {
        int a = p.local(types[i]);
        if (types[i] == GRJIT_TYPE_I64) {
          int k = p.local();
          p.cnst(k, i);
          p.bin(K::ADD, a, x, k);
        } else if (types[i] == GRJIT_TYPE_REF) {
          p.nw(a, 1000 + i);
        } else {
          p.cnst(a, i * 16 + 3);
        }
        args.push_back(a);
      }
      p.call(r, callee, args);
      p.ret(r);
      e.set(caller, p.done());
    }
    for (int64_t x : {int64_t{5}, int64_t{-2}}) {
      int64_t want = 0;
      for (int i = 0; i < n; i++) {
        int64_t v = types[i] == GRJIT_TYPE_I64 ? x + i
            : types[i] == GRJIT_TYPE_REF       ? 1000 + i
                                               : (i * 16 + 3) & 255;
        want += (i + 1) * v;
      }
      Outcome o = e.run_compiled(caller, {static_cast<u64>(x)});
      ASSERT_TRUE(o.finished);
      EXPECT_EQ(o.exit, uint32_t{GRJIT_EXIT_RETURNED});
      EXPECT_EQ(static_cast<int64_t>(o.value), want);
      Outcome i = e.run_interpreted(caller, {static_cast<u64>(x)});
      ASSERT_TRUE(i.finished);
      EXPECT_EQ(i.value, o.value);
    }
  }
  EXPECT_EQ(e.heap.poisoned_reads, 0);
  EXPECT_GT(e.heap.moved, 50);
  EXPECT_EQ(e.st.deopts, 0);
}

TEST(Calls, AnExitAtAnUncompilableCalleeIsNotCountedAgainstTheCallersDiscardLimitButAGuardIs) {
  CALLS_ONLY_ON_X86_64_SYSV();
  {
    Engine e;
    int inc = add_inc(e);
    int top = add_via_slot(e, inc);
    e.uncompilable.insert(inc);
    for (int i = 0; i < 20; i++) {
      Outcome o = e.run_compiled(top, {static_cast<u64>(i)});
      ASSERT_TRUE(o.finished);
      EXPECT_EQ(o.value, static_cast<u64>(i + 2) * 2);
      EXPECT_EQ(o.exit, uint32_t{GRJIT_EXIT_DEOPT});
      EXPECT_TRUE(e.st.last_was_call_exit) << "the exit started at the call instruction";
    }
    EXPECT_EQ(e.st.deopts, 20);
    EXPECT_EQ(e.st.counted[top], 0) << "twenty exits at the call, none counted against the caller";
    EXPECT_NE(e.cur[static_cast<size_t>(top)], nullptr) << "so its code is still the caller's";
  }
  {
    // The control: a guard that fails each time is the caller's, and is counted
    // until the limit, when the code is discarded.
    Engine e;
    int g = add_gchain(e, /*fail_at=*/0);
    for (int i = 0; i < 12; i++) {
      if (e.cur[static_cast<size_t>(g)] == nullptr && e.never_compile.count(g) == 0) {
        ASSERT_TRUE(e.compile_fn(g));
      }
      if (e.never_compile.count(g) != 0) {
        break;
      }
      Outcome o = e.run_compiled(g, {3});
      ASSERT_TRUE(o.finished);
      EXPECT_EQ(o.value, 6u);
      EXPECT_FALSE(e.st.last_was_call_exit);
    }
    EXPECT_EQ(e.st.counted[g], 8);
    EXPECT_EQ(e.cur[static_cast<size_t>(g)], nullptr) << "discarded at the limit";
    EXPECT_EQ(e.st.deopts, 8);
  }
}

TEST(Calls, TheBackendSaysWhetherItCanCompileCallsAndOnlyX86SysVCan) {
#if defined(__x86_64__) && defined(__linux__)
  EXPECT_TRUE(grjit_backend_calls_available());
#else
  EXPECT_FALSE(grjit_backend_calls_available());
#endif
  // The answer agrees with what compiling does.
  B b("ident", 0);
  GRJIT_VReg x = b.param(GRJIT_TYPE_I64);
  GRJIT_CallHooks h{};
  h.deopt = [](void *, uint64_t) -> uint32_t { return 0; };
  b.callable(h);
  b.at(b.block());
  b.ret(V(x));
  Fn f(b.finish());
  JitWorld w;
  Compiled c(f, w.pages());
  EXPECT_EQ(static_cast<bool>(c), grjit_backend_calls_available());
  if (!c) {
    EXPECT_EQ(c.result, GRJIT_ERR_UNSUPPORTED);
  }
}

TEST(Calls, AReservationExtensionTheAllocatorRefusesIsAnExitAtTheCallSiteAndNothingIsHalfDone) {
  CALLS_ONLY_ON_X86_64_SYSV();
  TrackingAllocator tracker;
  Engine e(GRCORE_UNLIMITED, GRCORE_UNLIMITED, /*conv=*/true, tracker.get());
  int g = add_gchain(e, /*fail_at=*/-1);
  ASSERT_TRUE(e.compile_fn(g));
  ASSERT_GT(e.code_of(g).max_converting, 0u);
  e.refuse_extend_with = &tracker;
  // The reservation grows by doubling, so the extensions of the first, second,
  // fourth and eighth calls allocate; the fourth call's is refused.
  e.refuse_extend_at = 4;
  Outcome o = e.run_compiled(g, {20});
  ASSERT_TRUE(o.finished);
  EXPECT_EQ(o.value, 210u) << "the interpreter made the call and finished the run";
  EXPECT_EQ(o.exit, uint32_t{GRJIT_EXIT_DEOPT});
  EXPECT_NE(e.st.extend_result, GRCORE_OK);
  EXPECT_EQ(e.st.refused_pushes, 1);
  EXPECT_EQ(e.st.rebuild, GRCORE_OK) << "the rebuild never allocates, and it did not fail";
  EXPECT_EQ(e.st.deopt_frames, 4) << "the entry and three callees; the fourth was never pushed";
  EXPECT_EQ(o.frames_left, 0u);
}

/* ---- Generated programs: compiled, interpreted, with and without collections ---- */

namespace {

/* A random program of `n` functions, f_i calling only f_j with j < i, so it
 * terminates. Each takes two integers, allocates an object, does some
 * arithmetic, may call lower functions on either side of a branch, may collect,
 * may hit a guard that fails for some inputs, reads its object back after its
 * calls, and returns a mix of everything. */
void generate(Engine & e, std::mt19937 & rng, int n, std::vector<int> * fns, bool mix_ptr = false) {
  auto pick = [&](int lo, int hi) { return std::uniform_int_distribution<int>(lo, hi)(rng); };
  for (int i = 0; i < n; i++) {
    fns->push_back(e.reserve());
  }
  for (int i = 0; i < n; i++) {
    P p("gen", {GRJIT_TYPE_I64, GRJIT_TYPE_I64});
    int x = 0, y = 1, a = p.local(), b = p.local(), c = p.local(), t = p.local(),
        zero = p.local(), obj = p.local(GRJIT_TYPE_REF), v = p.local(), g = p.local(),
        r1 = p.local(), r2 = p.local(), sum = p.local();
    p.cnst(zero, 0);
    p.nw(obj, pick(1, 50));
    p.cnst(c, pick(1, 9));
    p.bin(K::ADD, a, x, c);
    p.bin(K::MUL, b, y, c);
    if (pick(0, 3) == 0) {
      p.collect();
    }
    int ptr = mix_ptr ? p.local(GRJIT_TYPE_PTR) : -1;
    if (i > 0 && pick(0, 2) != 0) {
      int target = (*fns)[pick(0, i - 1)];
      if (mix_ptr && pick(0, 1) == 0) {
        p.entryof(ptr, target);
        p.callp(r1, ptr, target, {a, b});
      } else {
        p.call(r1, target, {a, b});
      }
    } else {
      p.bin(K::ADD, r1, a, b);
    }
    if (pick(0, 2) == 0) {
      // A guard that fails for some inputs: compiled code deoptimizes here, and
      // the interpreter, for which a guard is nothing, carries on.
      int k = p.local();
      p.cnst(k, pick(0, 30));
      p.bin(K::LT, t, x, k);
      p.guard(t);
    }
    if (pick(0, 3) == 0) {
      p.poll();
    }
    if (i > 0) {
      // A branch with a call on one side.
      int k = p.local();
      p.cnst(k, pick(0, 12));
      p.bin(K::LT, t, x, k);
      int br = p.brz(t);
      int target2 = (*fns)[pick(0, i - 1)];
      if (mix_ptr && pick(0, 1) == 0) {
        p.entryof(ptr, target2);
        p.callp(r2, ptr, target2, {b, a});
      } else {
        p.call(r2, target2, {b, a});
      }
      int over = p.br();
      p.patch(br, p.here());
      p.bin(K::SUB, r2, a, r1);
      p.patch(over, p.here());
    } else {
      p.mov(r2, a);
    }
    if (pick(0, 3) == 0) {
      p.collect();
    }
    p.get(v, obj);
    p.bin(K::ADD, sum, r1, r2);
    p.bin(K::ADD, sum, sum, v);
    p.bin(K::MUL, g, sum, c);
    p.ret(g);
    e.set((*fns)[i], p.done());
  }
}

} // namespace

TEST(Calls, GeneratedCallGraphsGiveTheSameResultsCompiledAndInterpretedWithAndWithoutCollections) {
  CALLS_ONLY_ON_X86_64_SYSV();
  long deopted = 0, direct = 0;
  for (unsigned seed = 0; seed < 150; seed++) {
    SCOPED_TRACE(seed);
    std::mt19937 rng(seed);
    Engine e;
    e.torture = seed % 2 == 0;
    std::vector<int> fns;
    int n = 2 + static_cast<int>(seed % 5);
    generate(e, rng, n, &fns);
    for (int top : {n - 1, n / 2}) {
      for (u64 x : {u64{0}, u64{3}, u64{15}, u64{40}}) {
        Outcome i = e.run_interpreted(fns[top], {x, x + 1});
        ASSERT_TRUE(i.finished);
        Outcome c = e.run_compiled(fns[top], {x, x + 1});
        ASSERT_TRUE(c.finished) << "failed=" << c.failed;
        EXPECT_EQ(c.value, i.value);
        EXPECT_EQ(c.frames_left, 0u);
        EXPECT_EQ(e.st.rebuild, GRCORE_OK);
        (c.exit == GRJIT_EXIT_DEOPT ? deopted : direct)++;
        e.st.rebuild = GRCORE_OK;
      }
    }
    EXPECT_EQ(e.heap.poisoned_reads, 0);
    EXPECT_EQ(e.st.pushes, e.st.pops);
  }
  // Both kinds of run happened, in numbers: the generator reaches the paths.
  EXPECT_GT(deopted, 100);
  EXPECT_GT(direct, 100);
}

/* ---- A derived pointer passed as an argument ------------------------------------ */

TEST(Calls, ADerivedPointerPassedAsAnArgumentFollowsItsBaseWhenThePushCollects) {
  CALLS_ONLY_ON_X86_64_SYSV();
  // The push hook collects (a moving collection at the push), so the object the
  // pointer is into moves between the arguments being copied and the callee
  // being called. The pointer is read from the arguments area afterwards, so the
  // area must be rewritten from the base like every other derived pointer. Seen
  // with the argument in a register and past the sixth, on the stack.
  for (int extra : {0, 6}) {
    SCOPED_TRACE(extra);
    Engine e;
    e.torture = true;
    int c = e.reserve();
    {
      std::vector<GRJIT_Type> params(static_cast<size_t>(extra), GRJIT_TYPE_I64);
      params.push_back(GRJIT_TYPE_PTR);
      P p("deref", params);
      int r = p.local();
      p.load(r, extra);
      p.ret(r);
      e.set(c, p.done());
    }
    int m = e.reserve();
    {
      P p("m", {});
      int obj = p.local(GRJIT_TYPE_REF), pp = p.local(GRJIT_TYPE_PTR), r = p.local(),
          v = p.local(), s = p.local(), z = p.local();
      std::vector<int> args;
      p.cnst(z, 0);
      for (int i = 0; i < extra; i++) {
        args.push_back(z);
      }
      p.nw(obj, 4242);
      p.derive(pp, obj, 8);
      args.push_back(pp);
      p.call(r, c, args);
      p.get(v, obj);
      p.bin(K::ADD, s, r, v);
      p.ret(s);
      e.set(m, p.done());
    }
    Outcome o = e.run_compiled(m, {});
    ASSERT_TRUE(o.finished);
    EXPECT_EQ(o.value, 8484u);
    EXPECT_EQ(e.heap.poisoned_reads, 0);
    EXPECT_GT(e.heap.moved, 0);
  }
}

/* ---- A code pointer is bound to the function and arity the call means --------- */

namespace {

/* A caller that calls the code of `target` through a pointer, naming `token` as
 * the callee and passing `n` arguments. */
int add_ptr_caller(Engine & e, int target, int token, int n) {
  int f = e.reserve();
  P p("via_ptr", {GRJIT_TYPE_I64});
  int ptr = p.local(GRJIT_TYPE_PTR), r = p.local();
  std::vector<int> args(static_cast<size_t>(n), 0);
  p.entryof(ptr, target);
  p.callp(r, ptr, token, args);
  p.ret(r);
  e.set(f, p.done());
  return f;
}

} // namespace

TEST(Calls, ACodePointerToAnotherFunctionOrOfAnotherArityIsRefusedAndNeverEntered) {
  CALLS_ONLY_ON_X86_64_SYSV();
  Engine e;
  int c2, c2caller, c8, c8caller;
  add_args_pair(e, 2, &c2, &c2caller);
  add_args_pair(e, 8, &c8, &c8caller);
  ASSERT_TRUE(e.compile_fn(c2));
  ASSERT_TRUE(e.compile_fn(c8));
  struct Case {
    const char * name;
    int target, token, n;
    bool enters;
  };
  // Another function of the same parameter count, whose answer is not c2's: only
  // the token tells them apart.
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
  ASSERT_TRUE(e.compile_fn(other));
  const Case cases[] = {
      {"the right function with the right count", c2, c2, 2, true},
      {"another function of the same parameter count", other, c2, 2, false},
      {"a function of eight parameters called as the one of two", c8, c2, 2, false},
      {"the right token but two arguments for eight parameters", c8, c8, 2, false},
      {"the right token but eight arguments for two parameters", c2, c2, 8, false},
  };
  for (const Case & c : cases) {
    SCOPED_TRACE(c.name);
    int f = add_ptr_caller(e, c.target, c.token, c.n);
    long before = e.st.deopts;
    Outcome i = e.run_interpreted(f, {5});
    Outcome o = e.run_compiled(f, {5});
    ASSERT_TRUE(o.finished) << "no fault, and the interpreter's answer";
    EXPECT_EQ(o.value, i.value);
    EXPECT_EQ(o.exit, c.enters ? uint32_t{GRJIT_EXIT_RETURNED} : uint32_t{GRJIT_EXIT_DEOPT});
    EXPECT_EQ(e.st.deopts - before, c.enters ? 0 : 1);
    EXPECT_EQ(e.st.rebuild, GRCORE_OK);
  }
}

TEST(Calls, CodeInstalledInASlotMustBeTheFunctionTheSlotIsForAndTakeItsParameterCount) {
  CALLS_ONLY_ON_X86_64_SYSV();
  Engine e;
  int c2, c2caller, c8, c8caller;
  add_args_pair(e, 2, &c2, &c2caller);
  add_args_pair(e, 8, &c8, &c8caller);
  ASSERT_TRUE(e.compile_fn(c8));
  e.discard(c2); // c2's slot is empty
  ASSERT_EQ(e.slots[static_cast<size_t>(c2)]->entry, 0u);
  const CompiledFn & code = e.code_of(c8);
  GRCORE_EntrySlot * slot = e.slots[static_cast<size_t>(c2)];
  GRCORE_Code * h = code.handle;
  EXPECT_EQ(grjit_entry_slot_install(e.ctx, slot, h, code.code, static_cast<u64>(c2), 2), GRJIT_ERR_INVALID)
      << "another function";
  EXPECT_EQ(grjit_entry_slot_install(e.ctx, slot, h, code.code, static_cast<u64>(c8), 2), GRJIT_ERR_INVALID)
      << "the right token and the wrong parameter count";
  EXPECT_EQ(grjit_entry_slot_install(e.ctx, slot, h, nullptr, static_cast<u64>(c8), 8), GRJIT_ERR_INVALID);
  EXPECT_EQ(slot->entry, 0u) << "a refusal installs nothing";
  // A function that is not callable has no internal entry to install at all.
  B plain("plain", 0);
  plain.at(plain.block());
  plain.ret();
  Fn pf(plain.finish());
  Compiled pc(pf, e.pages());
  ASSERT_TRUE(pc);
  EXPECT_EQ(grjit_entry_slot_install(e.ctx, slot, h, pc.code, 0, 0), GRJIT_ERR_INVALID);
  EXPECT_EQ(slot->entry, 0u);
  EXPECT_EQ(grjit_entry_slot_install(e.ctx, slot, h, code.code, static_cast<u64>(c8), 8), GRJIT_OK)
      << "the control: its own token and count";
  EXPECT_EQ(slot->entry, code.internal);
  EXPECT_EQ(grjit_code_token(code.code), static_cast<u64>(c8));
  ASSERT_EQ(grcore_entry_slot_clear(e.ctx, slot), GRCORE_OK);
}

/* ---- A refused rebuild is a distinct, unmissable exit -------------------------- */

TEST(Calls, ARebuildTheEngineRefusesIsAFatalExitWithNothingWrittenAndNeverAnInterpreterRun) {
  CALLS_ONLY_ON_X86_64_SYSV();
  for (int refuse : {0, 1}) {
    SCOPED_TRACE(refuse);
    Engine e;
    int g = add_gchain(e, /*fail_at=*/3);
    e.refuse_rebuild_with = refuse ? 77u : 0u;
    Outcome o = e.run_compiled(g, {10});
    if (!refuse) {
      ASSERT_TRUE(o.finished);
      EXPECT_EQ(o.value, 55u);
      EXPECT_FALSE(o.rebuild_failed);
      continue;
    }
    EXPECT_TRUE(o.rebuild_failed);
    EXPECT_FALSE(o.finished) << "the interpreter is not run on frames nothing rebuilt";
    EXPECT_EQ(o.exit, uint32_t{GRJIT_EXIT_REBUILD_FAILED});
    EXPECT_EQ(o.failed_with, 77u) << "the hook's answer arrives in out[0]";
    EXPECT_EQ(e.st.deopts, 1) << "the hook was asked once, and every frame then returned";
    ASSERT_EQ(o.pcs_after_failure.size(), 8u) << "the entry and seven callees were pushed (n = 10 down to 3)";
    for (u64 pc : o.pcs_after_failure) {
      EXPECT_EQ(pc, 0u) << "no guest frame was written";
    }
    EXPECT_EQ(grcore_stack_frame_count(e.stack), 0u) << "the engine unwound what the run pushed";
  }
}

TEST(Calls, ARebuildTheEngineRefusesAtANativeStackOverflowIsTheSameFatalExit) {
  CALLS_ONLY_ON_X86_64_SYSV();
  // The overflow stub calls the deopt hook from its own code, apart from the
  // guard and call exits, so it is a separate place to ignore the answer.
  for (int refuse : {0, 1}) {
    SCOPED_TRACE(refuse);
    Engine e(GRCORE_UNLIMITED, /*native_bytes=*/6000);
    int rec = add_rec(e);
    e.refuse_rebuild_with = refuse ? 77u : 0u;
    Outcome o = e.run_compiled(rec, {400});
    if (!refuse) {
      ASSERT_TRUE(o.finished);
      EXPECT_EQ(o.exit, uint32_t{GRJIT_EXIT_DEOPT});
      EXPECT_FALSE(o.rebuild_failed);
      continue;
    }
    EXPECT_TRUE(o.rebuild_failed);
    EXPECT_FALSE(o.finished);
    EXPECT_EQ(o.exit, uint32_t{GRJIT_EXIT_REBUILD_FAILED});
    EXPECT_EQ(o.failed_with, 77u);
    EXPECT_EQ(e.st.deopts, 1);
  }
}

/* ---- CALL_PTR, in chains ------------------------------------------------------------ */

namespace {

/* gchain through code pointers: every call is `ENTRYOF p, g; CALLP r, p, g, ...`. */
int add_gchain_ptr(Engine & e, int64_t fail_at, bool with_obj = false) {
  int g = e.reserve();
  P p("gchain_ptr", {GRJIT_TYPE_I64});
  int n = 0, zero = p.local(), one = p.local(), k = p.local(), hit = p.local(),
      ok = p.local(), t = p.local(), nn = p.local(), r = p.local(), sum = p.local(),
      ptr = p.local(GRJIT_TYPE_PTR), obj = p.local(GRJIT_TYPE_REF), v = p.local();
  p.cnst(zero, 0);
  p.cnst(one, 1);
  p.cnst(k, fail_at);
  if (with_obj) {
    p.nw(obj, 3);
  }
  p.bin(K::EQ, hit, n, k);
  p.bin(K::EQ, ok, hit, zero);
  p.guard(ok);
  p.bin(K::EQ, t, n, zero);
  int br = p.brz(t);
  if (with_obj) {
    p.collect();
  }
  p.ret(zero);
  p.patch(br, p.here());
  p.bin(K::SUB, nn, n, one);
  p.entryof(ptr, g);
  p.callp(r, ptr, g, {nn});
  if (with_obj) {
    p.collect();
    p.get(v, obj);
    p.bin(K::ADD, sum, r, v);
    p.bin(K::ADD, sum, sum, n);
  } else {
    p.bin(K::ADD, sum, r, n);
  }
  p.ret(sum);
  e.set(g, p.done());
  return g;
}

} // namespace

TEST(Calls, AGuardBelowCodePointerCallsDeoptimizesThroughEveryFrame) {
  CALLS_ONLY_ON_X86_64_SYSV();
  Engine e;
  int g = add_gchain_ptr(e, /*fail_at=*/7);
  ASSERT_TRUE(e.compile_fn(g));
  Outcome o = e.run_compiled(g, {10});
  ASSERT_TRUE(o.finished) << "failed=" << o.failed;
  EXPECT_EQ(o.value, 55u);
  EXPECT_EQ(o.exit, uint32_t{GRJIT_EXIT_DEOPT});
  EXPECT_EQ(e.st.deopts, 1);
  EXPECT_EQ(e.st.deopt_frames, 4) << "n = 10, 9, 8 waiting at their calls, and n = 7";
  EXPECT_EQ(e.st.rebuild, GRCORE_OK);
  EXPECT_EQ(o.frames_left, 0u);
}

TEST(Calls, AFiftyDeepChainOfCodePointerCallsIsSeenPreciselyWithAReferenceLiveAcrossEachCall) {
  CALLS_ONLY_ON_X86_64_SYSV();
  Engine e;
  e.torture = true; // a moving collection at every push, new object and collect
  int g = add_gchain_ptr(e, /*fail_at=*/-1, /*with_obj=*/true);
  ASSERT_TRUE(e.compile_fn(g));
  size_t compiled_frames = 0;
  size_t slots_seen = 0;
  bool bottom = false;
  e.on_collect = [&](Engine & en) {
    if (bottom) {
      return;
    }
    bottom = true;
    GRCORE_CompiledWalk w;
    GRCORE_CompiledFrame f;
    ASSERT_EQ(grcore_compiled_walk_begin(en.ctx, &w), GRCORE_OK);
    while (grcore_compiled_walk_next(&w, &f) == GRCORE_CWALK_FRAME) {
      compiled_frames++;
    }
    GRCORE_RootVisitor rv = {};
    rv.user = &slots_seen;
    rv.slot = [](void * user, uint64_t *) { ++*static_cast<size_t *>(user); };
    ASSERT_EQ(grcore_context_enumerate_roots(en.ctx, &rv), GRCORE_OK);
  };
  Outcome o = e.run_compiled(g, {49});
  ASSERT_TRUE(o.finished);
  EXPECT_EQ(o.value, 3u * 49u + 49u * 50u / 2u) << "the bottom frame adds nothing";
  EXPECT_EQ(o.exit, uint32_t{GRJIT_EXIT_RETURNED});
  EXPECT_EQ(compiled_frames, 50u);
  EXPECT_EQ(slots_seen, 50u) << "one reference per frame, reported once";
  EXPECT_EQ(e.heap.poisoned_reads, 0);
  EXPECT_GT(e.heap.moved, 100);
}

TEST(Calls, APointerIntoCodeThatWasRetiredIsRefusedAndNeverEnteredEvenWhileItStillMaps) {
  CALLS_ONLY_ON_X86_64_SYSV();
  Engine e;
  int target = add_inc(e);
  ASSERT_TRUE(e.compile_fn(target));
  int top = e.reserve();
  {
    // The pointer is taken, the code is retired (its slot cleared and its range
    // unregistered, as a replacement does), and the call goes through the old
    // pointer. The pages still map, because a JIT record is open, so entering
    // would work: the only thing that refuses is the retired flag.
    P p("stale", {GRJIT_TYPE_I64});
    int ptr = p.local(GRJIT_TYPE_PTR), r = p.local();
    p.entryof(ptr, target);
    p.clearslot(target);
    p.callp(r, ptr, target, {0});
    p.ret(r);
    e.set(top, p.done());
  }
  Outcome o = e.run_compiled(top, {41});
  ASSERT_TRUE(o.finished);
  EXPECT_EQ(o.value, 42u);
  EXPECT_EQ(o.exit, uint32_t{GRJIT_EXIT_DEOPT}) << "refused at the call, finished by the interpreter";
  EXPECT_EQ(e.st.deopts, 1);
  EXPECT_EQ(*e.released, 1) << "and released once the activation left";
}

TEST(Calls, GeneratedCallGraphsMixingCodePointerCallsAndSlotCallsAgreeWithTheInterpreter) {
  CALLS_ONLY_ON_X86_64_SYSV();
  long deopted = 0, direct = 0;
  for (unsigned seed = 1000; seed < 1100; seed++) {
    SCOPED_TRACE(seed);
    std::mt19937 rng(seed);
    Engine e;
    e.torture = seed % 2 == 0;
    std::vector<int> fns;
    int n = 2 + static_cast<int>(seed % 5);
    generate(e, rng, n, &fns, /*mix_ptr=*/true);
    for (int fn : fns) {
      ASSERT_TRUE(e.compile_fn(fn)); // so every ENTRYOF has an entry to take
    }
    for (u64 x : {u64{0}, u64{3}, u64{15}, u64{40}}) {
      Outcome i = e.run_interpreted(fns[n - 1], {x, x + 1});
      ASSERT_TRUE(i.finished);
      Outcome c = e.run_compiled(fns[n - 1], {x, x + 1});
      ASSERT_TRUE(c.finished) << "failed=" << c.failed;
      EXPECT_EQ(c.value, i.value);
      EXPECT_EQ(c.frames_left, 0u);
      (c.exit == GRJIT_EXIT_DEOPT ? deopted : direct)++;
    }
    EXPECT_EQ(e.heap.poisoned_reads, 0);
  }
  EXPECT_GT(deopted, 40);
  EXPECT_GT(direct, 40);
}

TEST(Calls, AnInnerFunctionThatClearsItsOuterCallersSlotLeavesTheOuterCodeAliveUntilTheActivationLeaves) {
  CALLS_ONLY_ON_X86_64_SYSV();
  Engine e;
  // top -> outer -> inner. inner clears OUTER's slot (retiring the code outer is
  // running in), collects, and returns; outer then fails a guard, so the chain is
  // rebuilt from frames whose code was retired under them.
  int inner = e.reserve(), outer = e.reserve(), top = e.reserve();
  {
    P p("inner", {GRJIT_TYPE_I64});
    int obj = p.local(GRJIT_TYPE_REF), v = p.local(), r = p.local();
    p.nw(obj, 5);
    p.clearslot(outer);
    p.collect();
    p.get(v, obj);
    p.bin(K::ADD, r, v, 0);
    p.ret(r);
    e.set(inner, p.done());
  }
  {
    P p("outer", {GRJIT_TYPE_I64});
    int a = p.local(), zero = p.local(), r = p.local();
    p.call(a, inner, {0});
    p.cnst(zero, 0);
    p.guard(zero); // fails: the chain is rebuilt
    p.bin(K::ADD, r, a, 0);
    p.ret(r);
    e.set(outer, p.done());
  }
  {
    P p("top", {GRJIT_TYPE_I64});
    int a = p.local(), one = p.local(), r = p.local();
    p.cnst(one, 1);
    p.call(a, outer, {0});
    p.bin(K::ADD, r, a, one);
    p.ret(r);
    e.set(top, p.done());
  }
  std::vector<int> released_at_collect;
  e.on_collect = [&](Engine & en) { released_at_collect.push_back(*en.released); };
  ASSERT_TRUE(e.compile_fn(inner));
  ASSERT_TRUE(e.compile_fn(outer));
  Outcome o = e.run_compiled(top, {10});
  ASSERT_TRUE(o.finished) << "failed=" << o.failed;
  EXPECT_EQ(o.value, 26u);
  EXPECT_EQ(o.exit, uint32_t{GRJIT_EXIT_DEOPT});
  EXPECT_EQ(e.st.rebuild, GRCORE_OK) << "the rebuild read frames in code that was retired";
  EXPECT_EQ(e.st.deopt_frames, 2) << "top and outer";
  ASSERT_EQ(released_at_collect.size(), 1u);
  EXPECT_EQ(released_at_collect[0], 0) << "outer's code was still under its frame";
  EXPECT_EQ(*e.released, 1) << "released when the activation left, and not before";
  EXPECT_EQ(e.slots[static_cast<size_t>(outer)]->entry, 0u);
  EXPECT_EQ(e.heap.poisoned_reads, 0);
}

TEST(Calls, CallableCodeBuiltForAnotherLayoutIsRefusedBeforeAnyOfItRuns) {
  CALLS_ONLY_ON_X86_64_SYSV();
  JitWorld w;
  Fn f(callable_identity(GRJIT_TYPE_I64));
  Compiled c(f, w.pages());
  ASSERT_TRUE(c) << c.result;
  uint64_t args[1] = {7};
  uint64_t out[2] = {0xAAAA, 0xBBBB};
  ASSERT_EQ(grjit_code_call(c.code, w.ctx, args, out), uint32_t{GRJIT_EXIT_RETURNED});
  EXPECT_EQ(out[0], 7u);
  // A core and a jit built from different versions disagree on where the
  // walk-start cell and the limit word are; compiled code would store through
  // the wrong offsets. The two are checked, each by itself.
  uint32_t * offsets[2] = {&c.code->walk_cell_offset, &c.code->native_limit_offset};
  for (uint32_t * field : offsets) {
    uint32_t saved = *field;
    *field += 8;
    out[0] = 0xAAAA;
    EXPECT_EQ(grjit_code_call(c.code, w.ctx, args, out), uint32_t{GRJIT_EXIT_REFUSED});
    EXPECT_EQ(out[0], static_cast<uint64_t>(GRCORE_ERR_INVALID)) << "refused before anything ran";
    *field = saved;
  }
  EXPECT_EQ(grjit_code_call(c.code, w.ctx, args, out), uint32_t{GRJIT_EXIT_RETURNED});
}

GRJIT_TEST_MAIN()
