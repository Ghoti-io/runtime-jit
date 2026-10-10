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

#include "../int_fixture.h"

#include "../ir_eval.h"
#include "../a64_sim.h"
#include "../ir_gen.h"
#include "../../src/code/code_internal.h"

#include <cstdio>
#include <random>

namespace {

constexpr int kFunctions = 2000;
using irgen::kArena;
using irgen::kArenaBase;
using irgen::kEdge;
using irgen::Gen;

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

/* The two things that can run a function: the machine code the library maps
 * for this build (where there is a backend), and the code the arm64 backend
 * emits, executed by the simulator in tests/a64_sim.h on any host. Each gives
 * the differential the same three things: whether it could be built, how to
 * run it, and the metadata its exit names a site in. */
struct NativeTarget {
  Compiled c;
  NativeTarget(const GRJIT_Function * f, JitWorld & w) : c(f, w.pages()) {}
  bool ok() const { return static_cast<bool>(c); }
  const char * why() const { return grjit_result_string(c.result); }
  struct Run {
    uint32_t exit;
    std::vector<uint64_t> out;
  };
  Run run(JitWorld & w, const std::vector<uint64_t> & args) const {
    auto r = c.run(w.ctx, args);
    return {r.exit, r.out};
  }
  const GRCORE_CodeMeta * meta() const { return grjit_code_meta(c.code); }
};

struct SimTarget {
  GRJIT_Emitted e{};
  GRJIT_Result result;
  size_t out_words;
  size_t params;
  explicit SimTarget(const GRJIT_Function * f, JitWorld &) {
    result = grjit_emit_for(GRJIT_ARCH_ARM64, f, grjit_allocator_default(), nullptr,
        nullptr, grcore_jit_layout()->request_word_offset, &e);
    out_words = grjit_function_interp_slot_count(f) + 1;
    params = grjit_function_param_count(f);
  }
  SimTarget(const SimTarget &) = delete;
  SimTarget & operator=(const SimTarget &) = delete;
  ~SimTarget() { grjit_emitted_free(&e); }
  bool ok() const { return result == GRJIT_OK; }
  const char * why() const { return grjit_result_string(result); }
  using Run = NativeTarget::Run;
  Run run(JitWorld & w, std::vector<uint64_t> args) const {
    Run r;
    r.out.assign(out_words, 0xDEADBEEFu);
    args.resize(std::max<size_t>(args.size(), params));
    a64sim::Config cfg;
    cfg.code = e.bytes;
    cfg.size = e.size;
    cfg.args[0] = reinterpret_cast<uint64_t>(w.ctx);
    cfg.args[1] = reinterpret_cast<uint64_t>(args.data());
    cfg.args[2] = reinterpret_cast<uint64_t>(r.out.data());
    cfg.returns32.insert(reinterpret_cast<uint64_t>(&poll_helper));
    a64sim::Cpu cpu;
    a64sim::Result res = cpu.run(cfg);
    EXPECT_TRUE(res.ok) << res.error;
    r.exit = static_cast<uint32_t>(res.x0);
    return r;
  }
  const GRCORE_CodeMeta * meta() const { return &e.meta.meta; }
};

template <class Target>
void run_differential(JitWorld & w) {
  size_t exits[3] = {0, 0, 0};
  size_t with_calls = 0, guards_failed = 0;
  for (uint64_t seed = 1; seed <= kFunctions; seed++) {
    Gen gen(seed, kHelpers, poll_helper);
    Fn f(gen.build());
    GRJIT_Result vr;
    std::string why = verify_reason(f, &vr);
    ASSERT_EQ(vr, GRJIT_OK) << describe(f, seed, "the generator built a malformed function: " + why);
    Target c(f, w);
    ASSERT_TRUE(c.ok()) << describe(f, seed, c.why());
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
          auto run = c.run(w, args);
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
            c.meta(), static_cast<uint32_t>(compiled.out[slots]));
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



TEST(Differential, GeneratedFunctionsRunTheSameCompiledAndEvaluated) {
  GRJIT_REQUIRE_BACKEND();
  JitWorld w;
  run_differential<NativeTarget>(w);
}

/* The arm64 emitter's output, executed by the simulator, against the same
 * evaluator: on every host, with no AArch64 machine and no emulator. It is the
 * arm64 backend's differential before qemu-aarch64 runs the real thing. */
TEST(Differential, GeneratedFunctionsRunTheSameWhenArm64CodeIsSimulatedAndEvaluated) {
  JitWorld w;
  run_differential<SimTarget>(w);
}

/* ---- Generated integer programs (spec-runtime-float, story 2) --------------------------------
 *
 * The same comparison for the i32 type and the integer operations of numeric-operations.md: a
 * random straight-line program over i32 and i64 registers - arithmetic, shifts and rotates (any
 * count), comparisons, bit counting, sign extension, wrap and extend, and the four divisions with
 * the guards a frontend writes (the divisor's, and for div_s the overflow one in either shape) -
 * whose operands come from the boundary values (0, 1, -1, MIN, MAX, the widths and one either
 * side, alternating bit patterns) or from earlier results, folded into one result at the end so
 * that every register is observed. A guard that fires is an exit with the frame state it names,
 * which the evaluator reconstructs. It does not use the generators of ir_gen.h, whose draw order
 * every pin depends on. */

namespace {

struct IntGen {
  std::mt19937_64 rng;
  explicit IntGen(uint64_t seed) : rng(seed) {}
  uint64_t pick(uint64_t n) { return rng() % n; }

  static const std::vector<uint64_t> & edges(bool narrow) {
    static const std::vector<uint64_t> e32 = {0, 1, 2, 3, 7, 8, 15, 16, 31, 32, 33, 63, 64, 65,
        0x7F, 0x80, 0xFF, 0x7FFF, 0x8000, 0xFFFF, 0x7FFFFFFE, 0x7FFFFFFF, 0x80000000u,
        0x80000001u, 0xFFFFFF80u, 0xFFFF8000u, 0xFFFFFFFEu, 0xFFFFFFFFu, 0x55555555u,
        0xAAAAAAAAu};
    static const std::vector<uint64_t> e64 = [] {
      std::vector<uint64_t> v = e32;
      for (uint64_t x : std::vector<uint64_t>{0x100000000ull, 0x7FFFFFFFFFFFFFFEull,
               0x7FFFFFFFFFFFFFFFull, 0x8000000000000000ull, 0x8000000000000001ull,
               0xFFFFFFFF80000000ull, 0xFFFFFFFFFFFFFF80ull, 0xFFFFFFFFFFFFFFFEull,
               0xFFFFFFFFFFFFFFFFull, 0x5555555555555555ull, 0xAAAAAAAAAAAAAAAAull}) {
        v.push_back(x);
      }
      return v;
    }();
    return narrow ? e32 : e64;
  }
  uint64_t edge(bool narrow) {
    const auto & e = edges(narrow);
    return e[pick(e.size())];
  }
};

/* An immediate operand: any 64-bit pattern for an i64, a 32-bit pattern for an i32. */
int64_t imm_for(IntGen & g, bool narrow) {
  uint64_t v = g.edge(narrow);
  return narrow ? static_cast<int64_t>(v) : static_cast<int64_t>(v);
}

struct IntProgram {
  GRJIT_Function * f = nullptr;
  size_t params = 0;
};

IntProgram generate_int_program(IntGen & g) {
  B b("ints", 2);
  std::vector<GRJIT_VReg> p32, p64;     // every i32 / i64 register assigned so far
  const size_t n32 = 3, n64 = 3;
  for (size_t i = 0; i < n32; i++) p32.push_back(b.param(GRJIT_TYPE_I32));
  for (size_t i = 0; i < n64; i++) p64.push_back(b.param(GRJIT_TYPE_I64));
  b.at(b.block());
  uint64_t site = 0;
  auto operand = [&](bool narrow) -> GRJIT_Operand {
    auto & pool = narrow ? p32 : p64;
    return g.pick(4) == 0 ? I(imm_for(g, narrow)) : V(pool[g.pick(pool.size())]);
  };
  auto reg_of = [&](bool narrow) { return (narrow ? p32 : p64)[g.pick((narrow ? p32 : p64).size())]; };
  auto fresh = [&](bool narrow) {
    GRJIT_VReg r = b.reg(narrow ? GRJIT_TYPE_I32 : GRJIT_TYPE_I64);
    return r;
  };
  auto state = [&]() {
    return std::vector<GRJIT_FrameSlot>{grjit_frame_slot_vreg(reg_of(g.pick(2) == 0)),
        grjit_frame_slot_vreg(reg_of(g.pick(2) == 0))};
  };
  static const GRJIT_OpKind binary[] = {GRJIT_OP_ADD, GRJIT_OP_SUB, GRJIT_OP_MUL, GRJIT_OP_AND,
      GRJIT_OP_OR, GRJIT_OP_XOR, GRJIT_OP_SHL, GRJIT_OP_SHR, GRJIT_OP_SAR, GRJIT_OP_ROTL,
      GRJIT_OP_ROTR};
  static const GRJIT_OpKind unary[] = {GRJIT_OP_NEG, GRJIT_OP_NOT, GRJIT_OP_CLZ, GRJIT_OP_CTZ,
      GRJIT_OP_POPCNT, GRJIT_OP_EXT8_S, GRJIT_OP_EXT16_S};
  static const GRJIT_OpKind divisions[] = {GRJIT_OP_DIV_S, GRJIT_OP_DIV_U, GRJIT_OP_REM_S,
      GRJIT_OP_REM_U};
  const size_t steps = 8 + g.pick(24);
  for (size_t step = 0; step < steps; step++) {
    const bool narrow = g.pick(2) == 0;
    const GRJIT_Type T = narrow ? GRJIT_TYPE_I32 : GRJIT_TYPE_I64;
    auto & pool = narrow ? p32 : p64;
    switch (g.pick(8)) {
      case 0:
      case 1: {
        GRJIT_VReg d = fresh(narrow);
        b.bin(binary[g.pick(sizeof binary / sizeof binary[0])], d, operand(narrow), operand(narrow));
        pool.push_back(d);
        break;
      }
      case 2: {
        GRJIT_VReg d = fresh(narrow);
        GRJIT_OpKind k = unary[g.pick(sizeof unary / sizeof unary[0])];
        b.un(k, d, V(reg_of(narrow)));
        pool.push_back(d);
        break;
      }
      case 3: {
        // A comparison, into an i32 or an i64 register, of either width of operand.
        const bool result32 = g.pick(2) == 0;
        GRJIT_VReg d = fresh(result32);
        b.cmp(static_cast<GRJIT_Cmp>(g.pick(GRJIT_CMP_COUNT)), d, V(reg_of(narrow)), operand(narrow));
        (result32 ? p32 : p64).push_back(d);
        break;
      }
      case 4: {
        // The conversions.
        if (g.pick(2) == 0) {
          GRJIT_VReg d = fresh(true);
          b.un(GRJIT_OP_WRAP, d, V(reg_of(false)));
          p32.push_back(d);
        } else {
          GRJIT_VReg d = fresh(false);
          GRJIT_OpKind k = g.pick(3) == 0 ? GRJIT_OP_EXTEND_U : GRJIT_OP_EXTEND_S;
          b.un(k, d, V(reg_of(true)));
          p64.push_back(d);
        }
        break;
      }
      case 5: {
        GRJIT_VReg d = fresh(false);
        b.un(GRJIT_OP_EXT32_S, d, V(reg_of(false)));
        p64.push_back(d);
        break;
      }
      default: {
        // A division, with the guards a frontend writes. Half the divisors are made non-zero
        // first, so the program goes on after most of its divisions.
        GRJIT_OpKind k = divisions[g.pick(4)];
        GRJIT_VReg a = reg_of(narrow);
        GRJIT_VReg y = reg_of(narrow);
        if (g.pick(2) == 0) {
          GRJIT_VReg safe = fresh(narrow);
          b.bin(GRJIT_OP_OR, safe, V(y), I(1));
          y = safe;
          pool.push_back(safe);
        }
        const int64_t min = narrow ? INT32_MIN : INT64_MIN;
        GRJIT_VReg c = b.reg(T);
        switch (g.pick(3)) {
          case 0: b.cmp(GRJIT_CMP_NE, c, V(y), I(0)); b.guard(V(c), {7, ++site}, state()); break;
          case 1: b.guard(V(y), {7, ++site}, state()); break;
          default: b.cmp(GRJIT_CMP_UGT, c, V(y), I(0)); b.guard(V(c), {7, ++site}, state()); break;
        }
        if (k == GRJIT_OP_DIV_S) {
          if (g.pick(2) == 0) {
            GRJIT_VReg e1 = b.reg(T), e2 = b.reg(T), t = b.reg(T), fine = b.reg(T);
            b.cmp(GRJIT_CMP_EQ, e1, V(a), I(min));
            b.cmp(GRJIT_CMP_EQ, e2, V(y), I(-1));
            b.bin(GRJIT_OP_AND, t, V(e1), V(e2));
            b.cmp(GRJIT_CMP_EQ, fine, V(t), I(0));
            b.guard(V(fine), {7, ++site}, state());
          } else {
            GRJIT_VReg n1 = b.reg(T), n2 = b.reg(T), fine = b.reg(T);
            b.cmp(GRJIT_CMP_NE, n1, V(a), I(min));
            b.cmp(GRJIT_CMP_NE, n2, V(y), I(-1));
            b.bin(GRJIT_OP_OR, fine, V(n1), V(n2));
            b.guard(V(fine), {7, ++site}, state());
          }
        }
        GRJIT_VReg d = fresh(narrow);
        b.bin(k, d, V(a), V(y));
        pool.push_back(d);
        break;
      }
    }
  }
  // Fold every register into one result: i32s are extended and mixed in, so that each is seen.
  GRJIT_VReg acc = b.reg(GRJIT_TYPE_I64);
  b.cnst(acc, 0);
  for (GRJIT_VReg r : p64) {
    b.bin(GRJIT_OP_ROTL, acc, V(acc), I(5));
    b.bin(GRJIT_OP_XOR, acc, V(acc), V(r));
  }
  for (GRJIT_VReg r : p32) {
    GRJIT_VReg w = b.reg(GRJIT_TYPE_I64);
    b.un(GRJIT_OP_EXTEND_U, w, V(r));
    b.bin(GRJIT_OP_ROTL, acc, V(acc), I(7));
    b.bin(GRJIT_OP_XOR, acc, V(acc), V(w));
  }
  b.ret(V(acc));
  IntProgram prog;
  prog.params = n32 + n64;
  prog.f = b.finish();
  return prog;
}

} // namespace

TEST(Differential, GeneratedIntegerProgramsRunTheSameCompiledAndEvaluated) {
#if defined(__x86_64__) && defined(__linux__)
  GRJIT_REQUIRE_BACKEND();
#else
  GTEST_SKIP() << "the integer operations are emitted for x86-64 SysV only; the arm64 and Win64 refusals are tested in testInt_ir";
#endif
  JitWorld w;
  size_t returned = 0, deopted = 0, divisions = 0;
  for (uint64_t seed = 1; seed <= 3000; seed++) {
    IntGen g(seed * 0x9E3779B97F4A7C15ull);
    IntProgram prog = generate_int_program(g);
    Fn f(prog.f);
    char why[256] = {0};
    ASSERT_EQ(grjit_function_verify(f, nullptr, why, sizeof why), GRJIT_OK)
        << describe(f, seed, std::string("the generator made a function the verifier refuses: ") + why);
    for (size_t blk = 0; blk < grjit_function_block_count(f); blk++) {
      size_t n = 0;
      const GRJIT_Op * ops = grjit_function_block_ops(f, static_cast<GRJIT_BlockId>(blk), &n);
      for (size_t i = 0; i < n; i++) {
        divisions += ops[i].kind >= GRJIT_OP_DIV_S && ops[i].kind <= GRJIT_OP_REM_U;
      }
    }
    Compiled c(f, w.pages());
    ASSERT_TRUE(c) << describe(f, seed, grjit_result_string(c.result));
    for (int run = 0; run < 6; run++) {
      std::vector<uint64_t> args;
      for (size_t i = 0; i < prog.params; i++) {
        args.push_back(IntGen::edges(i < 3).at(g.pick(IntGen::edges(i < 3).size())));
      }
      auto compiled = intfx::run(c.code, w.ctx, args);
      ASSERT_EQ(compiled.fault, 0) << describe(f, seed, "the compiled code faulted in hardware");
      ireval::Result eval = ireval::eval(f, w.ctx, nullptr, args);
      ASSERT_EQ(compiled.exit, eval.exit) << describe(f, seed, "different exits");
      if (compiled.exit == GRJIT_EXIT_RETURNED) {
        ASSERT_EQ(compiled.out[0], eval.out[0]) << describe(f, seed, "different results");
        returned++;
      } else {
        ASSERT_EQ(compiled.exit, uint32_t{GRJIT_EXIT_DEOPT});
        const size_t slots = grjit_function_interp_slot_count(f);
        for (size_t i = 0; i < slots; i++) {
          ASSERT_EQ(compiled.out[i], eval.out[i]) << describe(f, seed, "different frame state slot");
        }
        const GRCORE_CodeSite * site = grcore_codemeta_find(
            grjit_code_meta(c.code), static_cast<uint32_t>(compiled.out[slots]));
        ASSERT_NE(site, nullptr);
        ASSERT_EQ(site->identity.offset, eval.guard_identity.offset) << describe(f, seed, "different guard");
        deopted++;
      }
    }
  }
  std::printf("integer differential: 3000 programs x 6 runs, %zu returned, %zu guard exits, %zu divisions\n",
      returned, deopted, divisions);
  EXPECT_GT(returned, 3000u);
  EXPECT_GT(deopted, 500u);
  EXPECT_GT(divisions, 3000u);
}

GRJIT_TEST_MAIN()
