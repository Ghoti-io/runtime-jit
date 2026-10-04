/**
 * @file
 *
 * The differential: 2000 generated functions, each compiled and also run by
 * the test-only evaluator, which must agree on the result, the memory the
 * function wrote, the helper calls it made (in order, with their arguments)
 * and, for a guard that fails, the reconstructed interpreter slots and the
 * kind and identity of the site the exit names.
 *
 * `make check-planted` builds this test against a library with a planted
 * defect (a swapped SHR and SAR) and requires it to fail.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include "test_helpers.h"

#include "../ir_eval.h"

#include <cstdio>
#include <random>

namespace {

constexpr int kFunctions = 2000;
constexpr size_t kArena = 512;
constexpr size_t kArenaBase = 160; // the PTR parameter points here

std::vector<uint64_t> g_calls; // every helper call: id, argument count, arguments
uint32_t g_polls = 0;

template <size_t N>
uint64_t hash_args(uint64_t id, const uint64_t (&a)[N]) {
  g_calls.push_back(id);
  g_calls.push_back(N);
  uint64_t h = 0x9E3779B97F4A7C15ull * (id + 1);
  for (size_t i = 0; i < N; i++) {
    g_calls.push_back(a[i]);
    h = (h ^ a[i]) * 0xBF58476D1CE4E5B9ull + i;
  }
  g_calls.push_back(h);
  return h;
}

uint64_t h0() { const uint64_t a[1] = {0}; return hash_args(0, a); }
uint64_t h1(uint64_t a) { const uint64_t v[1] = {a}; return hash_args(1, v); }
uint64_t h2(uint64_t a, uint64_t b) { const uint64_t v[2] = {a, b}; return hash_args(2, v); }
uint64_t h3(uint64_t a, uint64_t b, uint64_t c) { const uint64_t v[3] = {a, b, c}; return hash_args(3, v); }
uint64_t h4(uint64_t a, uint64_t b, uint64_t c, uint64_t d) { const uint64_t v[4] = {a, b, c, d}; return hash_args(4, v); }
uint64_t h5(uint64_t a, uint64_t b, uint64_t c, uint64_t d, uint64_t e) { const uint64_t v[5] = {a, b, c, d, e}; return hash_args(5, v); }
uint64_t h6(uint64_t a, uint64_t b, uint64_t c, uint64_t d, uint64_t e, uint64_t f) { const uint64_t v[6] = {a, b, c, d, e, f}; return hash_args(6, v); }

const void * kHelpers[7] = {
    reinterpret_cast<const void *>(h0), reinterpret_cast<const void *>(h1),
    reinterpret_cast<const void *>(h2), reinterpret_cast<const void *>(h3),
    reinterpret_cast<const void *>(h4), reinterpret_cast<const void *>(h5),
    reinterpret_cast<const void *>(h6)};

uint32_t poll_helper(void *, uint64_t, uint64_t) {
  g_polls++;
  return 0;
}

const int64_t kEdge[] = {0, 1, -1, INT64_MIN, INT64_MAX, 2, -2, 255, -256, 0x7FFFFFFF,
    -0x80000000LL, 0xFFFFFFFFLL, 0x123456789ABCDEF0LL};

struct Gen {
  std::mt19937_64 rng;
  B b;
  GRJIT_VReg p0, p1, p2, p3;
  GRJIT_VReg q, c63, c64;
  GRJIT_VReg d[8];
  GRJIT_VReg r[2];
  GRJIT_VReg t;

  explicit Gen(uint64_t seed) : rng(seed), b("gen", 3) {}

  uint64_t pick(uint64_t n) { return rng() % n; }
  int64_t edge() {
    return pick(4) == 0 ? static_cast<int64_t>(rng())
                        : kEdge[pick(sizeof kEdge / sizeof *kEdge)];
  }
  GRJIT_VReg dreg() { return d[pick(8)]; }
  GRJIT_VReg rreg() { return r[pick(2)]; }
  GRJIT_Operand val() { return pick(3) == 0 ? I(edge()) : V(dreg()); }
  GRJIT_Operand shift_count() {
    switch (pick(6)) {
      case 0: return I(0);
      case 1: return I(1);
      case 2: return I(63);
      case 3: return V(c63);
      case 4: return V(c64);
      default: return V(dreg());
    }
  }
  std::vector<GRJIT_FrameSlot> state() {
    return {grjit_frame_slot_vreg(pick(4) == 0 ? rreg() : dreg()),
        grjit_frame_slot_constant(edge()), grjit_frame_slot_dead()};
  }

  void one_op() {
    static const GRJIT_OpKind bins[] = {GRJIT_OP_ADD, GRJIT_OP_SUB, GRJIT_OP_MUL,
        GRJIT_OP_AND, GRJIT_OP_OR, GRJIT_OP_XOR, GRJIT_OP_SHL, GRJIT_OP_SHR,
        GRJIT_OP_SAR};
    static const uint32_t widths[] = {8, 16, 32, 64};
    switch (pick(18)) {
      case 0: case 1: case 2: case 3: {
        GRJIT_OpKind k = bins[pick(9)];
        bool shift = k == GRJIT_OP_SHL || k == GRJIT_OP_SHR || k == GRJIT_OP_SAR;
        b.bin(k, dreg(), pick(5) == 0 ? I(edge()) : V(dreg()),
            shift ? shift_count() : val());
        break;
      }
      case 4:
        b.un(pick(2) ? GRJIT_OP_NEG : GRJIT_OP_NOT, dreg(), V(dreg()));
        break;
      case 5:
        b.cmp(static_cast<GRJIT_Cmp>(pick(GRJIT_CMP_COUNT)), dreg(), V(dreg()), val());
        break;
      case 6:
        b.cnst(dreg(), edge());
        break;
      case 7:
        b.mov(dreg(), pick(4) == 0 ? I(edge()) : V(dreg()));
        break;
      case 8: case 9:
        b.load(dreg(), q, static_cast<int32_t>(pick(260)) - 100, widths[pick(4)], pick(2) != 0);
        break;
      case 10: case 11:
        b.store(q, static_cast<int32_t>(pick(260)) - 100, widths[pick(4)], val());
        break;
      case 12:
        if (pick(2)) {
          b.mov(rreg(), V(rreg()));
        } else {
          b.cnst(rreg(), (static_cast<int64_t>(pick(1000)) << 4) | 1);
        }
        break;
      case 13:
        b.cmp(pick(2) ? GRJIT_CMP_EQ : GRJIT_CMP_NE, dreg(), V(rreg()), V(rreg()));
        break;
      case 14: case 15: {
        size_t n = pick(7);
        std::vector<GRJIT_Operand> args;
        for (size_t i = 0; i < n; i++) {
          args.push_back(pick(5) == 0 ? V(rreg()) : val());
        }
        GRJIT_VReg dst = pick(4) == 0 ? GRJIT_NO_VREG : dreg();
        if (pick(2)) {
          b.call(dst, kHelpers[n], args);
        } else {
          const GRCORE_CodeSiteKind kinds[] = {GRCORE_SITE_GC_POINT_POLL,
              GRCORE_SITE_GC_POINT_ALLOC_SLOW, GRCORE_SITE_GC_POINT_CALL,
              GRCORE_SITE_GC_POINT_FRAME_PUSH, GRCORE_SITE_GC_POINT_NESTED_ENTRY};
          b.call_gc(dst, kHelpers[n], args, {1, pick(100)}, state(), kinds[pick(5)]);
        }
        break;
      }
      case 16:
        b.poll({2, pick(100)}, state());
        break;
      default:
        // A guard that holds about half the time.
        b.cmp(static_cast<GRJIT_Cmp>(pick(GRJIT_CMP_COUNT)), t, V(dreg()), val());
        if (pick(3) == 0) {
          b.guard(V(t), {3, pick(100)}, state());
        }
        break;
    }
  }

  void straight(int n) {
    for (int i = 0; i < n; i++) {
      one_op();
    }
  }

  /* A segment of the function, continuing in the current block, which is
   * where control is when this returns. */
  GRJIT_BlockId segment(GRJIT_BlockId cur, int depth) {
    b.at(cur);
    straight(1 + static_cast<int>(pick(8)));
    int kind = depth >= 2 ? 0 : static_cast<int>(pick(4));
    if (kind == 1) { // if / else
      GRJIT_BlockId then_b = b.block(), else_b = b.block(), join = b.block();
      b.at(cur);
      b.cmp(static_cast<GRJIT_Cmp>(pick(GRJIT_CMP_COUNT)), t, V(dreg()), val());
      b.br_if(V(t), then_b, else_b);
      GRJIT_BlockId t_end = segment(then_b, depth + 1);
      b.at(t_end);
      b.br(join);
      GRJIT_BlockId e_end = segment(else_b, depth + 1);
      b.at(e_end);
      b.br(join);
      return join;
    }
    if (kind == 2) { // a counted loop of 1..4 iterations
      GRJIT_VReg i = b.reg();
      GRJIT_BlockId head = b.block(), body = b.block(), after = b.block();
      b.at(cur);
      b.cnst(i, 0);
      b.br(head);
      b.at(head);
      b.cmp(GRJIT_CMP_LT, t, V(i), I(1 + static_cast<int64_t>(pick(4))));
      b.br_if(V(t), body, after);
      GRJIT_BlockId body_end = segment(body, depth + 1);
      b.at(body_end);
      b.bin(GRJIT_OP_ADD, i, V(i), I(1));
      b.br(head);
      return after;
    }
    return cur;
  }

  GRJIT_Function * build() {
    p0 = b.param(GRJIT_TYPE_PTR);
    p1 = b.param(GRJIT_TYPE_I64);
    p2 = b.param(GRJIT_TYPE_I64);
    p3 = b.param(GRJIT_TYPE_REF);
    q = b.reg(GRJIT_TYPE_PTR);
    c63 = b.reg();
    c64 = b.reg();
    for (auto & v : d) {
      v = b.reg();
    }
    for (auto & v : r) {
      v = b.reg(GRJIT_TYPE_REF);
    }
    t = b.reg();
    b.poll_helper(poll_helper);
    GRJIT_BlockId entry = b.block();
    b.at(entry);
    b.mov(q, V(p0));
    b.cnst(c63, 63);
    b.cnst(c64, 64);
    b.mov(d[0], V(p1));
    b.mov(d[1], V(p2));
    for (int i = 2; i < 8; i++) {
      b.cnst(d[i], edge());
    }
    b.mov(r[0], V(p3));
    b.cnst(r[1], (static_cast<int64_t>(pick(1000)) << 4) | 1);
    b.cnst(t, 0);
    GRJIT_BlockId end = segment(entry, 0);
    if (pick(2)) {
      end = segment(end, 1);
    }
    b.at(end);
    // The result folds every data register, so a wrong one cannot hide.
    GRJIT_VReg acc = t;
    b.mov(acc, I(0));
    for (auto v : d) {
      b.bin(GRJIT_OP_XOR, acc, V(acc), V(v));
      b.bin(GRJIT_OP_MUL, acc, V(acc), I(0x100000001B3));
    }
    b.cmp(GRJIT_CMP_EQ, dreg(), V(r[0]), V(r[1]));
    b.ret(V(acc));
    return b.finish();
  }
};

struct Outcome {
  uint32_t exit = 99;
  std::vector<uint64_t> out;
  std::vector<uint8_t> arena;
  std::vector<uint64_t> calls;
  uint32_t polls = 0;
  GRCORE_PollIdentity identity{0, 0};
};

std::string describe(const GRJIT_Function * f, uint64_t seed, const std::string & what) {
  char head[96];
  std::snprintf(head, sizeof head, "seed %llu: ", static_cast<unsigned long long>(seed));
  return std::string(head) + what + "\n" + print(f);
}

} // namespace

TEST(Differential, GeneratedFunctionsRunTheSameCompiledAndEvaluated) {
  GRJIT_REQUIRE_BACKEND();
  JitWorld w;
  size_t exits[3] = {0, 0, 0};
  size_t with_calls = 0, guards_failed = 0;
  for (uint64_t seed = 1; seed <= kFunctions; seed++) {
    Gen gen(seed);
    Fn f(gen.build());
    GRJIT_Result vr;
    std::string why = verify_reason(f, &vr);
    ASSERT_EQ(vr, GRJIT_OK) << describe(f, seed, "the generator built a malformed function: " + why);
    Compiled c(f, w.pages());
    ASSERT_TRUE(c) << describe(f, seed, grjit_result_string(c.result));
    for (int input = 0; input < 3; input++) {
      std::mt19937_64 irng(seed * 7 + static_cast<uint64_t>(input));
      std::vector<uint8_t> image(kArena);
      for (auto & byte : image) {
        byte = static_cast<uint8_t>(irng());
      }
      uint64_t a1 = static_cast<uint64_t>(kEdge[irng() % (sizeof kEdge / sizeof *kEdge)]);
      uint64_t a2 = irng();
      uint64_t a3 = (irng() % 1000 << 4) | 1;
      Outcome got[2];
      for (int which = 0; which < 2; which++) {
        Outcome & r = got[which];
        r.arena = image;
        std::vector<uint64_t> args = {
            reinterpret_cast<uint64_t>(r.arena.data() + kArenaBase), a1, a2, a3};
        g_calls.clear();
        g_polls = 0;
        if (which == 0) {
          auto run = c.run(w.ctx, args);
          r.exit = run.exit;
          r.out = run.out;
        } else {
          ireval::Result e = ireval::eval(f, w.ctx, nullptr, args);
          ASSERT_FALSE(e.timed_out) << describe(f, seed, "the evaluator did not finish");
          r.exit = e.exit;
          r.out = e.out;
          r.identity = e.guard_identity;
        }
        r.calls = g_calls;
        r.polls = g_polls;
      }
      const Outcome & compiled = got[0];
      const Outcome & eval = got[1];
      ASSERT_EQ(compiled.exit, eval.exit) << describe(f, seed, "the exits differ");
      size_t slots = grjit_function_interp_slot_count(f);
      if (eval.exit == GRJIT_EXIT_RETURNED) {
        ASSERT_EQ(compiled.out[0], eval.out[0]) << describe(f, seed, "the results differ");
      } else {
        ASSERT_EQ(eval.exit, static_cast<uint32_t>(GRJIT_EXIT_DEOPT));
        guards_failed++;
        for (size_t i = 0; i < slots; i++) {
          ASSERT_EQ(compiled.out[i], eval.out[i]) << describe(f, seed, "the deopt slots differ");
        }
        const GRCORE_CodeSite * site = grcore_codemeta_find(
            grjit_code_meta(c.code), static_cast<uint32_t>(compiled.out[slots]));
        ASSERT_NE(site, nullptr) << describe(f, seed, "the exit names no site");
        ASSERT_EQ(site->kind, GRCORE_SITE_GUARD) << describe(f, seed, "the exit names a site that is not a guard");
        ASSERT_EQ(site->identity.function, eval.identity.function) << describe(f, seed, "wrong guard identity");
        ASSERT_EQ(site->identity.offset, eval.identity.offset) << describe(f, seed, "wrong guard identity");
      }
      ASSERT_EQ(compiled.arena, eval.arena) << describe(f, seed, "the memory the function wrote differs");
      ASSERT_EQ(compiled.calls, eval.calls) << describe(f, seed, "the helper call logs differ");
      ASSERT_EQ(compiled.polls, 0u) << describe(f, seed, "a poll with nothing pending called its helper");
      ASSERT_EQ(eval.polls, 0u);
      exits[compiled.exit]++;
      with_calls += compiled.calls.empty() ? 0 : 1;
    }
  }
  // The generator must actually reach what it is meant to cover.
  EXPECT_GT(exits[GRJIT_EXIT_RETURNED], 1000u);
  EXPECT_GT(exits[GRJIT_EXIT_DEOPT], 100u);
  EXPECT_GT(with_calls, 1000u);
  EXPECT_GT(guards_failed, 100u);
}

GRJIT_TEST_MAIN()
