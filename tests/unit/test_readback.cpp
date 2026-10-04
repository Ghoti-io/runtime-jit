/**
 * @file
 *
 * The metadata, read back from a live native frame the way a collector or a
 * deoptimizer would read it.
 *
 * Compiled code calls a native helper at a GC-point call and at a poll's slow
 * path. The helper (built with frame pointers) follows the saved-`rbp` chain
 * to the frame of the compiled code that called it, turns the return address
 * into a code offset, finds the site with `grcore_codemeta_find`, and reads
 * every live reference and every deopt slot at the recorded offsets. The test
 * compares what it read with the values the program is known to hold.
 *
 * It compares values, not offsets, so a recorded slot that is off by one word
 * or a live reference that is missing is caught without the test knowing the
 * frame layout. `make check-planted` builds it against a library with each of
 * those two defects planted and requires it to fail.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include "test_helpers.h"

#include <algorithm>
#include <cstdint>

namespace {

constexpr uint64_t kR0 = 0x1111000000000001ull;
constexpr uint64_t kR1 = 0x2222000000000003ull;
constexpr uint64_t kRd = 0x4444000000000005ull; // assigned, then dead
constexpr uint64_t kRb = 0x5000000000000001ull; // the base of the derived pointer
constexpr int64_t kDelta = 24;
constexpr uint64_t kI = 0x7777;                 // an I64, live, never in a map

struct Seen {
  bool called = false;
  bool site_found = false;
  GRCORE_CodeSiteKind kind = GRCORE_SITE_KIND_COUNT;
  GRCORE_PollIdentity identity{0, 0};
  std::vector<uint64_t> live_values;
  std::vector<GRCORE_SlotKind> live_kinds;
  struct Derived {
    uint64_t value, base_value;
    int64_t delta;
  };
  std::vector<Derived> derived;
  struct State {
    GRCORE_CodeLocationKind kind;
    GRCORE_SlotKind slot_kind;
    uint64_t value;
  };
  std::vector<State> state;
  uint64_t args[2] = {0, 0};
};

const GRJIT_Code * g_code = nullptr;
std::vector<Seen> g_seen;
GRCORE_Context * g_ctx = nullptr;
GRCORE_RequestKind g_kind = 0;

uint64_t word_at(uintptr_t frame_base, int64_t offset) {
  return *reinterpret_cast<const uint64_t *>(frame_base + offset);
}

/* Reads the frame of the compiled code whose call is in progress, given the
 * frame address of the helper that was called. */
__attribute__((noinline)) void observe(const void * helper_frame, Seen * seen) {
  const uintptr_t * fp = static_cast<const uintptr_t *>(helper_frame);
  uintptr_t jit_base = fp[0]; // the saved rbp: the compiled code's frame base
  uintptr_t ret = fp[1];      // the return address into the compiled code
  uintptr_t base = reinterpret_cast<uintptr_t>(grjit_code_address(g_code));
  const GRCORE_CodeSite * site =
      ret >= base ? grcore_codemeta_find(grjit_code_meta(g_code), static_cast<uint32_t>(ret - base))
                  : nullptr;
  seen->called = true;
  seen->site_found = site != nullptr;
  if (site == nullptr) {
    return;
  }
  seen->kind = site->kind;
  seen->identity = site->identity;
  for (size_t i = 0; i < site->live_count; i++) {
    const GRCORE_CodeLocation & l = site->live[i];
    seen->live_values.push_back(word_at(jit_base, l.value));
    seen->live_kinds.push_back(l.slot_kind);
  }
  for (size_t i = 0; i < site->derived_count; i++) {
    const GRCORE_DerivedPointer & d = site->derived[i];
    seen->derived.push_back({word_at(jit_base, d.slot), word_at(jit_base, d.base_slot), d.delta});
  }
  for (size_t i = 0; i < site->frame_state_count; i++) {
    const GRCORE_CodeLocation & l = site->frame_state[i];
    uint64_t v = l.kind == GRCORE_LOC_FRAME_SLOT ? word_at(jit_base, l.value)
               : l.kind == GRCORE_LOC_CONSTANT   ? static_cast<uint64_t>(l.value)
                                                 : 0;
    seen->state.push_back({l.kind, l.slot_kind, v});
  }
}

__attribute__((noinline)) uint64_t gc_helper(uint64_t a, uint64_t b) {
  g_seen.emplace_back();
  g_seen.back().args[0] = a;
  g_seen.back().args[1] = b;
  observe(__builtin_frame_address(0), &g_seen.back());
  return a + b;
}

__attribute__((noinline)) uint32_t poll_helper(void * ctx, uint64_t fn, uint64_t off) {
  g_seen.emplace_back();
  g_seen.back().args[0] = fn;
  g_seen.back().args[1] = off;
  observe(__builtin_frame_address(0), &g_seen.back());
  // The request is served: the next poll finds nothing.
  EXPECT_EQ(grcore_context_clear_request(static_cast<GRCORE_Context *>(ctx), g_kind), GRCORE_OK);
  return 0;
}

const GRCORE_Key kKey = {"readback", GRCORE_CARDINALITY_MANY, GRCORE_PHASE_NONE, nullptr, nullptr};

/* The program: two live REFs, one dead REF, a live I64, and a derived pointer
 * with its base, then a site (a call or a poll), then a use of all the live
 * ones. interp slots: [r0, r1, constant 42, dead]. */
struct Program {
  GRJIT_Function * f = nullptr;
  ~Program() { grjit_function_destroy(f); }
};

void build(Program * p, bool with_poll) {
  B b("rb", 4);
  GRJIT_VReg r0 = b.reg(GRJIT_TYPE_REF), r1 = b.reg(GRJIT_TYPE_REF);
  GRJIT_VReg rd = b.reg(GRJIT_TYPE_REF), i = b.reg(), rb = b.reg(GRJIT_TYPE_REF);
  GRJIT_VReg q = b.reg(GRJIT_TYPE_PTR), t1 = b.reg(), t2 = b.reg(), t3 = b.reg(), t4 = b.reg(),
             sum = b.reg(), res = b.reg();
  b.derived(q, rb, kDelta);
  b.poll_helper(poll_helper);
  b.at(b.block());
  b.cnst(r0, static_cast<int64_t>(kR0));
  b.cnst(r1, static_cast<int64_t>(kR1));
  b.cnst(rd, static_cast<int64_t>(kRd));
  b.cnst(i, kI);
  b.cnst(rb, static_cast<int64_t>(kRb));
  b.cnst(q, static_cast<int64_t>(kRb) + kDelta);
  std::vector<GRJIT_FrameSlot> state = {grjit_frame_slot_vreg(r0), grjit_frame_slot_vreg(r1),
      grjit_frame_slot_constant(42), grjit_frame_slot_dead()};
  if (with_poll) {
    b.poll({9, 77}, state);
  } else {
    b.call_gc(res, reinterpret_cast<const void *>(gc_helper), {V(r0), V(i)}, {9, 77}, state);
  }
  // Everything that must be live across the site is used after it.
  b.cmp(GRJIT_CMP_EQ, t1, V(r0), V(r1));
  b.cmp(GRJIT_CMP_EQ, t2, V(rb), V(rb));
  b.bin(GRJIT_OP_ADD, t3, V(q), I(0));
  b.bin(GRJIT_OP_ADD, t4, V(i), I(1));
  b.bin(GRJIT_OP_ADD, sum, V(t1), V(t2));
  b.bin(GRJIT_OP_ADD, sum, V(sum), V(t3));
  b.bin(GRJIT_OP_ADD, sum, V(sum), V(t4));
  b.mov(res, V(sum));
  b.ret(V(res));
  p->f = b.finish();
}

void expect_site(const Seen & s, GRCORE_CodeSiteKind kind) {
  ASSERT_TRUE(s.called);
  ASSERT_TRUE(s.site_found) << "no site at the return address";
  EXPECT_EQ(s.kind, kind);
  EXPECT_EQ(s.identity.function, 9u);
  EXPECT_EQ(s.identity.offset, 77u);

  // Exactly the two live REFs and the derived pointer's base: three words,
  // with the values the program holds. The I64 and the dead REF are absent.
  std::vector<uint64_t> live = s.live_values;
  std::sort(live.begin(), live.end());
  std::vector<uint64_t> want = {kR0, kR1, kRb};
  std::sort(want.begin(), want.end());
  EXPECT_EQ(live, want) << "the stack map's slots do not hold the live references (a slot is "
                           "wrong, or a live reference is missing)";
  for (GRCORE_SlotKind k : s.live_kinds) {
    EXPECT_EQ(k, GRCORE_SLOT_VALUE);
  }
  EXPECT_EQ(std::count(s.live_values.begin(), s.live_values.end(), kI), 0);
  EXPECT_EQ(std::count(s.live_values.begin(), s.live_values.end(), kRd), 0);

  // The derived triple reproduces the pointer from its base.
  ASSERT_EQ(s.derived.size(), 1u);
  EXPECT_EQ(s.derived[0].base_value, kRb);
  EXPECT_EQ(s.derived[0].delta, kDelta);
  EXPECT_EQ(s.derived[0].value, s.derived[0].base_value + static_cast<uint64_t>(s.derived[0].delta));

  // The deopt frame state: [r0, r1, constant 42, dead].
  ASSERT_EQ(s.state.size(), 4u);
  EXPECT_EQ(s.state[0].kind, GRCORE_LOC_FRAME_SLOT);
  EXPECT_EQ(s.state[0].value, kR0) << "the deopt slot for r0 does not hold r0";
  EXPECT_EQ(s.state[0].slot_kind, GRCORE_SLOT_VALUE);
  EXPECT_EQ(s.state[1].kind, GRCORE_LOC_FRAME_SLOT);
  EXPECT_EQ(s.state[1].value, kR1) << "the deopt slot for r1 does not hold r1";
  EXPECT_EQ(s.state[2].kind, GRCORE_LOC_CONSTANT);
  EXPECT_EQ(s.state[2].value, 42u);
  EXPECT_EQ(s.state[3].kind, GRCORE_LOC_DEAD);
}

} // namespace

TEST(Readback, ACallThatCanReachAGcPointShowsExactlyTheLiveReferencesThroughItsFrame) {
  GRJIT_REQUIRE_BACKEND();
  JitWorld w;
  Program p;
  build(&p, false);
  Compiled c(p.f, w.pages());
  ASSERT_TRUE(c) << grjit_result_string(c.result);
  g_code = c.code;
  g_seen.clear();
  auto r = c.run(w.ctx);
  ASSERT_EQ(r.exit, static_cast<uint32_t>(GRJIT_EXIT_RETURNED));
  ASSERT_EQ(g_seen.size(), 1u);
  EXPECT_EQ(g_seen[0].args[0], kR0);
  EXPECT_EQ(g_seen[0].args[1], kI);
  expect_site(g_seen[0], GRCORE_SITE_GC_POINT_CALL);
  // The program still computes what it should after the helper returned.
  EXPECT_EQ(r.out[0], 1u /* r0 == r1 is false */ * 0 + 1u /* rb == rb */ + (kRb + kDelta) + (kI + 1));
}

TEST(Readback, ThePollSlowPathShowsTheSameThingsAndKeepsItsIdentity) {
  GRJIT_REQUIRE_BACKEND();
  JitWorld w;
  Program p;
  build(&p, true);
  Compiled c(p.f, w.pages());
  ASSERT_TRUE(c) << grjit_result_string(c.result);
  g_code = c.code;
  g_ctx = w.ctx;
  ASSERT_EQ(grcore_context_request_kind(w.ctx, &kKey, &g_kind), GRCORE_OK);
  GRCORE_Port * port;
  ASSERT_EQ(grcore_context_port(w.ctx, &port), GRCORE_OK);

  // Nothing pending: the slow path is not taken.
  g_seen.clear();
  auto quiet = c.run(w.ctx);
  EXPECT_EQ(quiet.exit, static_cast<uint32_t>(GRJIT_EXIT_RETURNED));
  EXPECT_TRUE(g_seen.empty());

  // A request is posted: the slow path runs once, with the poll's identity.
  ASSERT_EQ(grcore_port_post(port, g_kind), GRCORE_OK);
  auto loud = c.run(w.ctx);
  EXPECT_EQ(loud.exit, static_cast<uint32_t>(GRJIT_EXIT_RETURNED));
  EXPECT_EQ(loud.out[0], quiet.out[0]);
  ASSERT_EQ(g_seen.size(), 1u);
  EXPECT_EQ(g_seen[0].args[0], 9u);   // the helper was given the identity
  EXPECT_EQ(g_seen[0].args[1], 77u);
  expect_site(g_seen[0], GRCORE_SITE_GC_POINT_POLL);
  grcore_port_release(port);
}

TEST(Readback, ACallWhoseResultRegisterIsNamedByItsFrameStateKeepsTheOldValueInTheMap) {
  GRJIT_REQUIRE_BACKEND();
  JitWorld w;
  B b("dst", 1);
  GRJIT_VReg r = b.reg(GRJIT_TYPE_REF);
  GRJIT_VReg t = b.reg();
  b.at(b.block());
  b.cnst(r, static_cast<int64_t>(kR0));
  // r is the call's result and also the slot the frame state locates: at the
  // return address it still holds kR0, so the stack map must say so.
  b.call_gc(r, reinterpret_cast<const void *>(gc_helper), {I(1), I(2)}, {9, 77},
      {grjit_frame_slot_vreg(r)});
  b.cmp(GRJIT_CMP_EQ, t, V(r), V(r));
  b.ret(V(t));
  Fn f(b.finish());
  Compiled c(f, w.pages());
  ASSERT_TRUE(c) << grjit_result_string(c.result);
  g_code = c.code;
  g_seen.clear();
  c.run(w.ctx);
  ASSERT_EQ(g_seen.size(), 1u);
  ASSERT_TRUE(g_seen[0].site_found);
  ASSERT_EQ(g_seen[0].state.size(), 1u);
  EXPECT_EQ(g_seen[0].state[0].value, kR0);
  EXPECT_EQ(g_seen[0].state[0].slot_kind, GRCORE_SLOT_VALUE);
  EXPECT_EQ(g_seen[0].live_values, std::vector<uint64_t>{kR0})
      << "the deopt slot is a VALUE the collector was not told about";
}

TEST(Readback, TheTableHasASitePerGcPointCallAndPollAndGuardAndNothingElse) {
  GRJIT_REQUIRE_BACKEND();
  JitWorld w;
  B b("sites", 1);
  GRJIT_VReg x = b.param(GRJIT_TYPE_I64);
  GRJIT_VReg d = b.reg();
  b.poll_helper(poll_helper);
  b.at(b.block());
  std::vector<GRJIT_FrameSlot> st = {grjit_frame_slot_vreg(x)};
  b.call(d, reinterpret_cast<const void *>(gc_helper), {V(x), V(x)});            // not a site
  b.call_gc(d, reinterpret_cast<const void *>(gc_helper), {V(x), V(x)}, {1, 1}, st);
  b.poll({1, 2}, st);
  b.guard(V(d), {1, 3}, st);
  b.call_gc(d, reinterpret_cast<const void *>(gc_helper), {V(x), V(x)}, {1, 4}, st,
      GRCORE_SITE_GC_POINT_ALLOC_SLOW);
  b.ret(V(d));
  Fn f(b.finish());
  Compiled c(f, w.pages());
  ASSERT_TRUE(c);
  const GRCORE_CodeMeta * meta = grjit_code_meta(c.code);
  ASSERT_NE(meta, nullptr);
  EXPECT_EQ(meta->version, GRCORE_CODEMETA_FORMAT_VERSION);
  EXPECT_EQ(meta->code_bytes, grjit_code_size(c.code));
  EXPECT_EQ(grcore_codemeta_validate(meta, grjit_code_size(c.code), nullptr), GRCORE_OK);
  ASSERT_EQ(meta->site_count, 4u);
  size_t polls = 0, calls = 0, allocs = 0, guards = 0;
  for (size_t i = 0; i < meta->site_count; i++) {
    const GRCORE_CodeSite & s = meta->sites[i];
    polls += s.kind == GRCORE_SITE_GC_POINT_POLL;
    calls += s.kind == GRCORE_SITE_GC_POINT_CALL;
    allocs += s.kind == GRCORE_SITE_GC_POINT_ALLOC_SLOW;
    guards += s.kind == GRCORE_SITE_GUARD;
    EXPECT_EQ(s.frame_state_count, 1u);
    if (i > 0) {
      EXPECT_GT(s.code_offset, meta->sites[i - 1].code_offset);
    }
  }
  EXPECT_EQ(polls, 1u);
  EXPECT_EQ(calls, 1u);
  EXPECT_EQ(allocs, 1u);
  EXPECT_EQ(guards, 1u);
  // Every site is exactly where find says, and no byte between them is.
  size_t found = 0;
  for (uint32_t off = 0; off < meta->code_bytes; off++) {
    found += grcore_codemeta_find(meta, off) != nullptr;
  }
  EXPECT_EQ(found, 4u);
}

GRJIT_TEST_MAIN()
