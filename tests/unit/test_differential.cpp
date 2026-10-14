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
#include "../float_fixture.h"
#include "../feature_fixture.h"

#include "../ir_eval.h"
#include "../a64_sim.h"
#include "../ir_gen.h"
#include "../../src/code/code_internal.h"

#include <cstdio>
#include <random>
#include <set>

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

/* The features whose arm an operation of `f` takes when it is allowed: the set a compile of `f` can
 * record at most. Read off the function's own operations, not off the compiled code. */
GRJIT_Features arms_possible(const GRJIT_Function * f) {
  GRJIT_Features need = GRJIT_FEATURES_NONE;
  for (size_t blk = 0; blk < grjit_function_block_count(f); blk++) {
    size_t n = 0;
    const GRJIT_Op * ops = grjit_function_block_ops(f, static_cast<GRJIT_BlockId>(blk), &n);
    for (size_t i = 0; i < n; i++) {
      switch (ops[i].kind) {
        case GRJIT_OP_CLZ: need |= GRJIT_FEATURE_LZCNT; break;
        case GRJIT_OP_CTZ: need |= GRJIT_FEATURE_BMI1; break;
        case GRJIT_OP_POPCNT: need |= GRJIT_FEATURE_POPCNT; break;
        case GRJIT_OP_FCEIL:
        case GRJIT_OP_FFLOOR:
        case GRJIT_OP_FTRUNC:
        case GRJIT_OP_FNEAREST: need |= GRJIT_FEATURE_SSE4_1; break;
        default: break;
      }
    }
  }
  return need;
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
    cfg.stack_bytes = 1u << 18; // the generated functions are small and shallow: a fresh stack per run
    cfg.args[0] = reinterpret_cast<uint64_t>(w.ctx);
    cfg.args[1] = reinterpret_cast<uint64_t>(w.ctx);
    cfg.args[2] = reinterpret_cast<uint64_t>(args.data());
    cfg.args[3] = reinterpret_cast<uint64_t>(r.out.data());
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

/* An immediate operand: any 64-bit pattern for an i64, a 32-bit pattern (0 to 0xFFFFFFFF, one of
 * the two spellings the verifier accepts) for an i32. */
int64_t imm_for(IntGen & g, bool narrow) {
  uint64_t v = g.edge(narrow);
  if (narrow && v >= 0x80000000u && g.pick(2) == 0) {
    return static_cast<int64_t>(v) - (int64_t{1} << 32); // the negative spelling of the same 32 bits
  }
  return static_cast<int64_t>(v);
}

bool imm_is_zero(bool narrow, int64_t v) { return narrow ? static_cast<uint32_t>(v) == 0 : v == 0; }
bool imm_is_min(bool narrow, int64_t v) {
  return narrow ? static_cast<uint32_t>(v) == 0x80000000u : v == INT64_MIN;
}
bool imm_is_neg1(bool narrow, int64_t v) { return narrow ? static_cast<uint32_t>(v) == 0xFFFFFFFFu : v == -1; }

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
  // A division with the guards a frontend writes. Half the register divisors are made non-zero first,
  // so the program goes on after most of its divisions; a divisor or a dividend may be a constant
  // (0 excepted for a divisor, which the verifier refuses), MIN and -1 among them.
  auto emit_division = [&](bool narrow, GRJIT_VReg d, bool record) {
    const GRJIT_Type T = narrow ? GRJIT_TYPE_I32 : GRJIT_TYPE_I64;
    auto & pool = narrow ? p32 : p64;
    const GRJIT_OpKind k = divisions[g.pick(4)];
    const int64_t min = narrow ? INT32_MIN : INT64_MIN;
    bool y_imm = g.pick(4) == 0, a_imm = g.pick(4) == 0;
    int64_t yv = 1, av = 0;
    if (y_imm) {
      yv = g.pick(3) == 0 ? -1 : imm_for(g, narrow);
      if (imm_is_zero(narrow, yv)) {
        yv = 3;
      }
    }
    if (a_imm) {
      av = g.pick(3) == 0 ? min : imm_for(g, narrow);
    }
    // MIN / x with a constant MIN and a register x is refused (a limit of the rule): use a register.
    if (k == GRJIT_OP_DIV_S && a_imm && imm_is_min(narrow, av) && (!y_imm || imm_is_neg1(narrow, yv))) {
      a_imm = false;
    }
    GRJIT_VReg a = a_imm ? GRJIT_NO_VREG : reg_of(narrow);
    GRJIT_VReg y = GRJIT_NO_VREG;
    if (!y_imm) {
      y = reg_of(narrow);
      if (g.pick(2) == 0) {
        GRJIT_VReg safe = fresh(narrow);
        b.bin(GRJIT_OP_OR, safe, V(y), I(1));
        y = safe;
        if (record) {
          pool.push_back(safe);
        }
      }
      GRJIT_VReg c = b.reg(T);
      switch (g.pick(3)) {
        case 0: b.cmp(GRJIT_CMP_NE, c, V(y), I(0)); b.guard(V(c), {7, ++site}, state()); break;
        case 1: b.guard(V(y), {7, ++site}, state()); break;
        default: b.cmp(GRJIT_CMP_UGT, c, V(y), I(0)); b.guard(V(c), {7, ++site}, state()); break;
      }
    }
    if (k == GRJIT_OP_DIV_S) {
      if (y_imm && !a_imm && imm_is_neg1(narrow, yv)) {
        GRJIT_VReg c = b.reg(T);
        b.cmp(GRJIT_CMP_NE, c, V(a), I(min));
        b.guard(V(c), {7, ++site}, state());
      } else if (!y_imm && !a_imm) {
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
    }
    b.bin(k, d, a_imm ? I(av) : V(a), y_imm ? I(yv) : V(y));
  };
  const size_t steps = 8 + g.pick(24);
  for (size_t step = 0; step < steps; step++) {
    const bool narrow = g.pick(2) == 0;
    auto & pool = narrow ? p32 : p64;
    switch (g.pick(10)) {
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
      case 8: {
        // A diamond: the two arms assign one register, and each may hold a division with its own
        // guards, which do not cross the branch.
        GRJIT_VReg cv = reg_of(g.pick(2) == 0);
        GRJIT_BlockId t_arm = b.block(), e_arm = b.block(), join = b.block();
        GRJIT_VReg d = fresh(narrow);
        b.br_if(V(cv), t_arm, e_arm);
        for (GRJIT_BlockId arm : {t_arm, e_arm}) {
          b.at(arm);
          if (g.pick(2) == 0) {
            emit_division(narrow, d, false);
          } else {
            b.bin(binary[g.pick(sizeof binary / sizeof binary[0])], d, operand(narrow), operand(narrow));
          }
          b.br(join);
        }
        b.at(join);
        pool.push_back(d);
        break;
      }
      default: {
        GRJIT_VReg d = fresh(narrow);
        emit_division(narrow, d, true);
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

/* What the program run by either target did, compared with the evaluator's: the exit, the result, and
 * for a guard that fired the frame state and the guard's identity. `float_slot` is the index of a frame
 * state slot that is a float, compared with any two NaNs equal (none for the integer programs). */
static bool same_as_evaluator(const GRJIT_Function * f, uint64_t seed, uint32_t exit,
    const std::vector<uint64_t> & out, const ireval::Result & eval, const GRCORE_CodeMeta * meta,
    int float_slot, size_t * returned, size_t * deopted) {
  bool ok = true;
  if (exit != eval.exit) {
    ADD_FAILURE() << describe(f, seed, "different exits");
    return false;
  }
  if (exit == GRJIT_EXIT_RETURNED) {
    if (out[0] != eval.out[0]) {
      ADD_FAILURE() << describe(f, seed, "different results");
      ok = false;
    }
    ++*returned;
    return ok;
  }
  if (exit != GRJIT_EXIT_DEOPT) {
    ADD_FAILURE() << describe(f, seed, "an exit that is neither a return nor a guard");
    return false;
  }
  const size_t slots = grjit_function_interp_slot_count(f);
  for (size_t i = 0; i < slots; i++) {
    const bool same = static_cast<int>(i) == float_slot ? ireval::same_float(64, out[i], eval.out[i])
                                                        : out[i] == eval.out[i];
    if (!same) {
      ADD_FAILURE() << describe(f, seed, "different frame state slot " + std::to_string(i));
      ok = false;
    }
  }
  const GRCORE_CodeSite * site = grcore_codemeta_find(meta, static_cast<uint32_t>(out[slots]));
  if (site == nullptr || site->identity.offset != eval.guard_identity.offset) {
    ADD_FAILURE() << describe(f, seed, "different guard");
    ok = false;
  }
  ++*deopted;
  return ok;
}

TEST(Differential, GeneratedIntegerProgramsRunTheSameCompiledAndEvaluated) {
  GRJIT_REQUIRE_BACKEND();
  JitWorld w;
  size_t returned = 0, deopted = 0, divisions = 0, const_divisors = 0, const_dividends = 0,
         negative_i32_imms = 0, multi_block = 0, arm_programs = 0;
  const size_t masks = featfx::every_subset_masks().size();
  // Under TSan the sweep runs 1/kTsanDivisor of the programs and its thresholds shrink with it (see the float sweep).
  for (uint64_t seed = 1; seed <= 3000 / kTsanDivisor; seed++) {
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
        const bool division = ops[i].kind >= GRJIT_OP_DIV_S && ops[i].kind <= GRJIT_OP_REM_U;
        divisions += division;
        const_divisors += division && ops[i].b.kind == GRJIT_OPERAND_IMM;
        const_dividends += division && ops[i].a.kind == GRJIT_OPERAND_IMM;
        negative_i32_imms += grjit_function_vreg_type(f, ops[i].dst) == GRJIT_TYPE_I32 &&
                             (ops[i].a.kind == GRJIT_OPERAND_IMM || ops[i].b.kind == GRJIT_OPERAND_IMM) &&
                             ((ops[i].a.kind == GRJIT_OPERAND_IMM && ops[i].a.imm < 0) ||
                                 (ops[i].b.kind == GRJIT_OPERAND_IMM && ops[i].b.imm < 0));
      }
    }
    multi_block += grjit_function_block_count(f) > 1;
    // The six argument sets are drawn once, in the order the runs always drew them, so that every mask
    // below runs the same ones.
    std::vector<std::vector<uint64_t>> arg_sets;
    for (int run = 0; run < 6; run++) {
      std::vector<uint64_t> args;
      for (size_t i = 0; i < prog.params; i++) {
        uint64_t v = IntGen::edges(i < 3).at(g.pick(IntGen::edges(i < 3).size()));
        if (i < 3 && g.pick(2) == 0) {
          v |= (g.rng() | 1) << 32; // an i32 argument with garbage above bit 31: the entry masks it
        }
        args.push_back(v);
      }
      arg_sets.push_back(args);
    }
    // Under every subset of the host's detected features: the results must be the evaluator's and so
    // each other's, and the recorded set exactly the arms the function's operations allow.
    for (GRJIT_Features mask : featfx::every_subset_masks()) {
    Compiled c(f, w.pages(), nullptr, nullptr, nullptr, mask);
    ASSERT_TRUE(c) << describe(f, seed, grjit_result_string(c.result));
    const GRJIT_Features effective = featfx::detected() & ~mask & GRJIT_FEATURES_ALL;
    ASSERT_EQ(grjit_code_features(c.code), effective & arms_possible(f))
        << describe(f, seed, "the recorded set is not the arms the effective set allows");
    arm_programs += grjit_code_features(c.code) != GRJIT_FEATURES_NONE;
    for (const std::vector<uint64_t> & args : arg_sets) {
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
  }
  // Run counts (each of the 3000 programs is run with 6 argument sets under each mask, so 18000 runs per mask, summed over the masks) and static counts (over the 3000 generated programs).
  std::printf("integer differential: per run: %zu returned, %zu guard exits; static, summed over the operations of the 3000 programs: "
              "%zu divisions, %zu with a constant divisor, %zu with a constant dividend, %zu i32 "
              "operations with a negative immediate; and %zu of the programs have more than one block\n",
      returned, deopted, divisions, const_divisors, const_dividends, negative_i32_imms, multi_block);
  std::printf("integer differential: every program run under each of the %zu subsets of the host's features; "
              "%zu compiles took a feature arm\n", masks, arm_programs);
  // Thresholds at about nine tenths of what the fixed seeds give (12267, 5733, 23328, 5876, 5454, 6425, 2531),
  // the run counts once per mask.
  EXPECT_GT(returned, 11000u * masks / kTsanDivisor);
  EXPECT_GT(deopted, 5000u * masks / kTsanDivisor);
  if (featfx::detected() != GRJIT_FEATURES_NONE) {
    EXPECT_GT(arm_programs, 0u) << "the integer programs reach clz, ctz or popcnt with a feature allowed";
  }
  EXPECT_GT(divisions, 21000u / kTsanDivisor);
  EXPECT_GT(const_divisors, 5200u / kTsanDivisor);
  EXPECT_GT(const_dividends, 4900u / kTsanDivisor);
  EXPECT_GT(negative_i32_imms, 5700u / kTsanDivisor);
  EXPECT_GT(multi_block, 2200u / kTsanDivisor);
}

/* The same programs, the same argument sets, against the same evaluator, with the arm64 code the emitter
 * makes for each executed by the simulator in tests/a64_sim.h on any host. It is a dev loop and an extra
 * catch, not the verdict: qemu-aarch64 and the Raspberry Pi run the real thing (the native test above, on
 * an arm64 host). */
TEST(Differential, GeneratedIntegerProgramsRunTheSameWhenArm64CodeIsSimulatedAndEvaluated) {
  JitWorld w;
  size_t returned = 0, deopted = 0;
  for (uint64_t seed = 1; seed <= 3000; seed++) {
    IntGen g(seed * 0x9E3779B97F4A7C15ull);
    IntProgram prog = generate_int_program(g);
    Fn f(prog.f);
    char why[256] = {0};
    ASSERT_EQ(grjit_function_verify(f, nullptr, why, sizeof why), GRJIT_OK)
        << describe(f, seed, std::string("the generator made a function the verifier refuses: ") + why);
    // The same six argument sets as the native test draws, in the same order.
    std::vector<std::vector<uint64_t>> arg_sets;
    for (int run = 0; run < 6; run++) {
      std::vector<uint64_t> args;
      for (size_t i = 0; i < prog.params; i++) {
        uint64_t v = IntGen::edges(i < 3).at(g.pick(IntGen::edges(i < 3).size()));
        if (i < 3 && g.pick(2) == 0) {
          v |= (g.rng() | 1) << 32; // an i32 argument with garbage above bit 31: the entry masks it
        }
        args.push_back(v);
      }
      arg_sets.push_back(args);
    }
    SimTarget c(f, w);
    ASSERT_TRUE(c.ok()) << describe(f, seed, c.why());
    for (const std::vector<uint64_t> & args : arg_sets) {
      SimTarget::Run run = c.run(w, args);
      ireval::Result eval = ireval::eval(f, w.ctx, nullptr, args);
      ASSERT_TRUE(same_as_evaluator(f, seed, run.exit, run.out, eval, c.meta(), -1, &returned, &deopted))
          << "(the arm64 code, simulated)";
    }
  }
  std::printf("integer differential, arm64 simulated: %zu returned, %zu guard exits\n", returned, deopted);
  EXPECT_GT(returned, 11000u);
  EXPECT_GT(deopted, 5000u);
}

/* ---- Generated float programs (spec-runtime-float, story 3) -----------------------------------
 *
 * The same comparison for the float types and operations: a random program over f32, f64, i32 and
 * i64 registers - the seven binary and seven unary float operations, comparisons into an i32 or an
 * i64, the conversions both ways (integer to float, saturating and guarded truncation, demote,
 * promote, reinterpret), constants and moves, and a diamond - whose operands are the boundary values
 * (zeros of both signs, subnormals, the smallest and largest normals, infinities, NaNs of both signs
 * and several payloads quiet and signalling, integers at the conversion boundaries, floats at the
 * truncation bounds and a unit in the last place either side) or earlier results. A truncation has
 * its guards in front of it, in a shape drawn from those the verifier accepts, so a value out of
 * range or a NaN is an exit with the frame state it names.
 *
 * Floats are compared by bits with any two NaNs equal: the compiled code and C++ may disagree on
 * which payload survives when both operands are NaN. So the program is made insensitive to a payload
 * where it leaves the float domain: a reinterpret, and the fold of every register into one result,
 * replace the bits of a NaN by one canonical pattern first. A frame state holds two integers and a
 * float, and the float is compared by the same rule. Own generator, so no draw order of ir_gen.h
 * moves. */

namespace {

struct FloatProgram {
  GRJIT_Function * f = nullptr;
  size_t params = 0;
  size_t truncs = 0, converts = 0, compares = 0, nan_values = 0;
};

FloatProgram generate_float_program(IntGen & g) {
  using namespace floatfx;
  static const std::vector<uint64_t> e32 = f32_values(), e64 = f64_values();
  static const std::vector<uint64_t> i32e = int_values(true), i64e = int_values(false);
  B b("floats", 3);
  std::vector<GRJIT_VReg> pf32, pf64, pi32, pi64; // every register of each type assigned so far
  for (int i = 0; i < 3; i++) pf32.push_back(b.param(GRJIT_TYPE_F32));
  for (int i = 0; i < 3; i++) pf64.push_back(b.param(GRJIT_TYPE_F64));
  for (int i = 0; i < 2; i++) pi32.push_back(b.param(GRJIT_TYPE_I32));
  for (int i = 0; i < 2; i++) pi64.push_back(b.param(GRJIT_TYPE_I64));
  b.at(b.block());
  uint64_t site = 0;
  FloatProgram prog;
  prog.params = 10;
  std::set<GRJIT_VReg> consts; // registers made by a CONST
  auto fresh = [&](GRJIT_Type t) { return b.reg(t); };
  auto pick = [&](const std::vector<GRJIT_VReg> & v) { return v[g.pick(v.size())]; };
  auto fimm = [&](bool dbl) -> int64_t { return static_cast<int64_t>(dbl ? e64[g.pick(e64.size())] : e32[g.pick(e32.size())]); };
  auto foperand = [&](bool dbl) -> GRJIT_Operand {
    return g.pick(5) == 0 ? I(fimm(dbl)) : V(pick(dbl ? pf64 : pf32));
  };
  auto state = [&]() {
    return std::vector<GRJIT_FrameSlot>{grjit_frame_slot_vreg(pick(pi64)), grjit_frame_slot_vreg(pick(pi32)),
        grjit_frame_slot_vreg(pick(pf64))};
  };
  /* The bits of a float register as an integer register, a NaN replaced by the canonical NaN, so that a
   * payload that is not fixed does not leave the float domain. */
  auto clean_bits = [&](GRJIT_VReg r, bool dbl) -> GRJIT_VReg {
    GRJIT_Type it = dbl ? GRJIT_TYPE_I64 : GRJIT_TYPE_I32;
    GRJIT_VReg flag = fresh(GRJIT_TYPE_I32), bits = fresh(it), mask = fresh(it), nm = fresh(it),
               kept = fresh(it), canon = fresh(it), out = fresh(it);
    b.cmp(GRJIT_CMP_NE, flag, V(r), V(r));
    b.un(GRJIT_OP_REINTERPRET, bits, V(r));
    if (dbl) {
      GRJIT_VReg wide = fresh(GRJIT_TYPE_I64);
      b.un(GRJIT_OP_EXTEND_U, wide, V(flag));
      b.un(GRJIT_OP_NEG, mask, V(wide));
    } else {
      b.un(GRJIT_OP_NEG, mask, V(flag));
    }
    b.un(GRJIT_OP_NOT, nm, V(mask));
    b.bin(GRJIT_OP_AND, kept, V(bits), V(nm));
    b.bin(GRJIT_OP_AND, canon, V(mask), I(dbl ? 0x7FF8000000000000ll : 0x7FC00000ll));
    b.bin(GRJIT_OP_OR, out, V(kept), V(canon));
    return out;
  };
  // copysign reads the sign of its second operand, and the sign of a NaN that an operation made is
  // not fixed (hardware gives the default NaN its sign, the evaluator another): so its second
  // operand is an immediate or a parameter, whose bits are the same on both sides. The other six
  // binary operations are free: a NaN in is a NaN out, and a NaN is a NaN to every consumer.
  const std::vector<GRJIT_VReg> params32(pf32.begin(), pf32.begin() + 3),
      params64(pf64.begin(), pf64.begin() + 3);
  auto sign_operand = [&](bool dbl) -> GRJIT_Operand {
    return g.pick(3) == 0 ? I(fimm(dbl)) : V(pick(dbl ? params64 : params32));
  };
  auto binary_op = [&](GRJIT_VReg d, bool dbl) {
    static const GRJIT_OpKind six[] = {GRJIT_OP_FADD, GRJIT_OP_FSUB, GRJIT_OP_FMUL, GRJIT_OP_FDIV,
        GRJIT_OP_FMIN, GRJIT_OP_FMAX};
    const uint64_t k = g.pick(7);
    if (k == 6) {
      b.bin(GRJIT_OP_FCOPYSIGN, d, foperand(dbl), sign_operand(dbl));
    } else {
      b.bin(six[k], d, foperand(dbl), foperand(dbl));
    }
  };
  static const GRJIT_OpKind unary[] = {GRJIT_OP_FNEG, GRJIT_OP_FABS, GRJIT_OP_FSQRT, GRJIT_OP_FCEIL,
      GRJIT_OP_FFLOOR, GRJIT_OP_FTRUNC, GRJIT_OP_FNEAREST};
  static const GRJIT_Cmp cmps[] = {GRJIT_CMP_EQ, GRJIT_CMP_NE, GRJIT_CMP_LT, GRJIT_CMP_LE, GRJIT_CMP_GT,
      GRJIT_CMP_GE};
  const size_t steps = 8 + g.pick(24);
  for (size_t step = 0; step < steps; step++) {
    const bool dbl = g.pick(2) == 0;
    auto & fpool = dbl ? pf64 : pf32;
    const GRJIT_Type FT = dbl ? GRJIT_TYPE_F64 : GRJIT_TYPE_F32;
    switch (g.pick(14)) {
      case 0:
      case 1:
      case 2: {
        GRJIT_VReg d = fresh(FT);
        binary_op(d, dbl);
        fpool.push_back(d);
        break;
      }
      case 3:
      case 4: {
        GRJIT_VReg d = fresh(FT);
        b.un(unary[g.pick(7)], d, foperand(dbl));
        fpool.push_back(d);
        break;
      }
      case 5: {
        const bool result32 = g.pick(2) == 0;
        GRJIT_VReg d = fresh(result32 ? GRJIT_TYPE_I32 : GRJIT_TYPE_I64);
        b.cmp(cmps[g.pick(6)], d, V(pick(fpool)), foperand(dbl));
        (result32 ? pi32 : pi64).push_back(d);
        prog.compares++;
        break;
      }
      case 6: {
        // integer to float
        const bool from64 = g.pick(2) == 0;
        GRJIT_VReg d = fresh(FT);
        b.un(g.pick(2) == 0 ? GRJIT_OP_CONVERT_S : GRJIT_OP_CONVERT_U, d, V(pick(from64 ? pi64 : pi32)));
        fpool.push_back(d);
        prog.converts++;
        break;
      }
      case 7: {
        // saturating truncation
        const bool to64 = g.pick(2) == 0;
        GRJIT_VReg d = fresh(to64 ? GRJIT_TYPE_I64 : GRJIT_TYPE_I32);
        b.un(g.pick(2) == 0 ? GRJIT_OP_TRUNC_SAT_S : GRJIT_OP_TRUNC_SAT_U, d, V(pick(fpool)));
        (to64 ? pi64 : pi32).push_back(d);
        prog.converts++;
        break;
      }
      case 8: {
        // a guarded truncation, in one of the shapes the verifier accepts
        const bool to64 = g.pick(2) == 0, is_signed = g.pick(2) == 0;
        // (A register a CONST set is judged as it stands by the verifier, so it is not a source here.)
        GRJIT_VReg x = pick(fpool), d = fresh(to64 ? GRJIT_TYPE_I64 : GRJIT_TYPE_I32);
        for (int tries = 0; tries < 20 && consts.count(x) != 0; tries++) {
          x = pick(fpool);
        }
        if (consts.count(x) != 0) {
          x = (dbl ? pf64 : pf32)[0];
        }
        const Bounds bd = trunc_bounds(is_signed, to64);
        double lo0 = bd.lo;
        bool closed = bd.lo_closed;
        if (!dbl && static_cast<double>(static_cast<float>(lo0)) != lo0) {
          lo0 = static_cast<double>(static_cast<float>(lo0));
          closed = true;
        }
        // sometimes a narrower guard: a unit in the last place in, at either end
        const double lo = step_ulps(lo0, dbl, g.pick(4) == 0 ? -1 : 0, -INFINITY);
        const double hi = step_ulps(bd.hi, dbl, g.pick(4) == 0 ? -1 : 0, INFINITY);
        if (g.pick(2) == 0) {
          GRJIT_VReg same = fresh(GRJIT_TYPE_I32);
          b.cmp(GRJIT_CMP_EQ, same, V(x), V(x));
          b.guard(V(same), {7, ++site}, state());
        }
        GRJIT_VReg l = fresh(GRJIT_TYPE_I32), h = fresh(GRJIT_TYPE_I32), ok = fresh(GRJIT_TYPE_I32);
        switch (g.pick(3)) {
          case 0:
            b.cmp(closed ? GRJIT_CMP_GE : GRJIT_CMP_GT, l, V(x), I(static_cast<int64_t>(imm_bits(lo, dbl))));
            b.cmp(GRJIT_CMP_LT, h, V(x), I(static_cast<int64_t>(imm_bits(hi, dbl))));
            b.bin(GRJIT_OP_AND, ok, V(l), V(h));
            break;
          case 1: // mirrored, joined in the other order
            b.cmp(closed ? GRJIT_CMP_LE : GRJIT_CMP_LT, l, I(static_cast<int64_t>(imm_bits(lo, dbl))), V(x));
            b.cmp(GRJIT_CMP_GT, h, I(static_cast<int64_t>(imm_bits(hi, dbl))), V(x));
            b.bin(GRJIT_OP_AND, ok, V(h), V(l));
            break;
          default: // the upper bound as <=, one unit in the last place in
            b.cmp(closed ? GRJIT_CMP_GE : GRJIT_CMP_GT, l, V(x), I(static_cast<int64_t>(imm_bits(lo, dbl))));
            b.cmp(GRJIT_CMP_LE, h, V(x), I(static_cast<int64_t>(imm_bits(step_ulps(hi, dbl, -1, INFINITY), dbl))));
            b.bin(GRJIT_OP_AND, ok, V(l), V(h));
            break;
        }
        b.guard(V(ok), {7, ++site}, state());
        b.un(is_signed ? GRJIT_OP_TRUNC_S : GRJIT_OP_TRUNC_U, d, V(x));
        (to64 ? pi64 : pi32).push_back(d);
        prog.truncs++;
        break;
      }
      case 9: {
        GRJIT_VReg d = fresh(dbl ? GRJIT_TYPE_F32 : GRJIT_TYPE_F64);
        b.un(dbl ? GRJIT_OP_DEMOTE : GRJIT_OP_PROMOTE, d, V(pick(dbl ? pf64 : pf32)));
        (dbl ? pf32 : pf64).push_back(d);
        break;
      }
      case 10: {
        // reinterpret: integer bits to a float, and a float's bits (payload-cleaned) to an integer
        if (g.pick(2) == 0) {
          GRJIT_VReg d = fresh(FT);
          b.un(GRJIT_OP_REINTERPRET, d, V(pick(dbl ? pi64 : pi32)));
          fpool.push_back(d);
        } else {
          (dbl ? pi64 : pi32).push_back(clean_bits(pick(fpool), dbl));
        }
        break;
      }
      case 11: {
        GRJIT_VReg d = fresh(FT);
        if (g.pick(2) == 0) {
          b.cnst(d, fimm(dbl));
          consts.insert(d);
        } else {
          b.mov(d, foperand(dbl));
        }
        fpool.push_back(d);
        break;
      }
      case 12: {
        // an integer operation beside the floats, so the two kinds of register mix in one function
        GRJIT_VReg d = fresh(GRJIT_TYPE_I64);
        b.bin(GRJIT_OP_ADD, d, V(pick(pi64)), I(static_cast<int64_t>(i64e[g.pick(i64e.size())])));
        pi64.push_back(d);
        GRJIT_VReg e = fresh(GRJIT_TYPE_I32);
        b.bin(GRJIT_OP_XOR, e, V(pick(pi32)), I(static_cast<int64_t>(i32e[g.pick(i32e.size())])));
        pi32.push_back(e);
        break;
      }
      default: {
        // a diamond on a comparison: the two arms assign one float register
        GRJIT_VReg c = fresh(GRJIT_TYPE_I32), d = fresh(FT);
        b.cmp(cmps[g.pick(6)], c, V(pick(fpool)), foperand(dbl));
        GRJIT_BlockId t_arm = b.block(), e_arm = b.block(), join = b.block();
        b.br_if(V(c), t_arm, e_arm);
        for (GRJIT_BlockId arm : {t_arm, e_arm}) {
          b.at(arm);
          if (g.pick(2) == 0) {
            binary_op(d, dbl);
          } else {
            b.un(unary[g.pick(7)], d, foperand(dbl));
          }
          b.br(join);
        }
        b.at(join);
        fpool.push_back(d);
        break;
      }
    }
  }
  // Fold every register into one i64: floats through their payload-cleaned bits, i32s extended.
  GRJIT_VReg acc = fresh(GRJIT_TYPE_I64);
  b.cnst(acc, 0);
  auto mix = [&](GRJIT_VReg v, bool narrow, unsigned rot) {
    GRJIT_VReg w = v;
    if (narrow) {
      w = fresh(GRJIT_TYPE_I64);
      b.un(GRJIT_OP_EXTEND_U, w, V(v));
    }
    b.bin(GRJIT_OP_ROTL, acc, V(acc), I(rot));
    b.bin(GRJIT_OP_XOR, acc, V(acc), V(w));
  };
  for (GRJIT_VReg r : pi64) mix(r, false, 5);
  for (GRJIT_VReg r : pi32) mix(r, true, 7);
  for (GRJIT_VReg r : pf64) mix(clean_bits(r, true), false, 11);
  for (GRJIT_VReg r : pf32) mix(clean_bits(r, false), true, 13);
  b.ret(V(acc));
  prog.f = b.finish();
  return prog;
}

} // namespace

TEST(Differential, GeneratedFloatProgramsRunTheSameCompiledAndEvaluatedWithNansEqual) {
  GRJIT_REQUIRE_BACKEND();
  if (!floatfx::host_fp_is_default()) {
    GTEST_SKIP() << "the host's MXCSR / FPCR is not the default the compiled code assumes";
  }
  JitWorld w;
  size_t returned = 0, deopted = 0, truncs = 0, converts = 0, compares = 0, multi_block = 0, arm_programs = 0;
  const size_t masks = featfx::every_subset_masks().size();
  // Under TSan the sweep runs 1/kTsanDivisor of the programs (the same seeds, the first ones) and its thresholds
  // shrink with it; every other build runs all 3000.
  for (uint64_t seed = 1; seed <= 3000 / kTsanDivisor; seed++) {
    IntGen g(seed * 0x9E3779B97F4A7C15ull + 0xF10A7);
    FloatProgram prog = generate_float_program(g);
    Fn f(prog.f);
    char why[256] = {0};
    ASSERT_EQ(grjit_function_verify(f, nullptr, why, sizeof why), GRJIT_OK)
        << describe(f, seed, std::string("the generator made a function the verifier refuses: ") + why);
    truncs += prog.truncs;
    converts += prog.converts;
    compares += prog.compares;
    multi_block += grjit_function_block_count(f) > 1;
    static const std::vector<uint64_t> e32 = floatfx::f32_values(), e64 = floatfx::f64_values();
    static const std::vector<uint64_t> i32e = floatfx::int_values(true), i64e = floatfx::int_values(false);
    std::vector<std::vector<uint64_t>> arg_sets;
    for (int run = 0; run < 6; run++) {
      std::vector<uint64_t> args;
      for (size_t i = 0; i < prog.params; i++) {
        uint64_t v;
        if (i < 3) {
          v = e32[g.pick(e32.size())] | (g.pick(2) == 0 ? (g.rng() | 1) << 32 : 0); // garbage above bit 31
        } else if (i < 6) {
          v = e64[g.pick(e64.size())];
        } else if (i < 8) {
          v = i32e[g.pick(i32e.size())] | (g.pick(2) == 0 ? (g.rng() | 1) << 32 : 0);
        } else {
          v = i64e[g.pick(i64e.size())];
        }
        args.push_back(v);
      }
      arg_sets.push_back(args);
    }
    // Under every subset of the host's detected features (up to sixteen): the same programs and the same
    // arguments, results equal to the evaluator's and to each other's by bits with any two NaNs equal, and
    // the recorded set exactly the arms the function's operations allow.
    for (GRJIT_Features mask : featfx::every_subset_masks()) {
    Compiled c(f, w.pages(), nullptr, nullptr, nullptr, mask);
    ASSERT_TRUE(c) << describe(f, seed, grjit_result_string(c.result));
    const GRJIT_Features effective = featfx::detected() & ~mask & GRJIT_FEATURES_ALL;
    ASSERT_EQ(grjit_code_features(c.code), effective & arms_possible(f))
        << describe(f, seed, "the recorded set is not the arms the effective set allows");
    arm_programs += grjit_code_features(c.code) != GRJIT_FEATURES_NONE;
    for (const std::vector<uint64_t> & args : arg_sets) {
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
        ASSERT_EQ(slots, 3u);
        ASSERT_EQ(compiled.out[0], eval.out[0]) << describe(f, seed, "different frame state slot 0");
        ASSERT_EQ(compiled.out[1], eval.out[1]) << describe(f, seed, "different frame state slot 1");
        ASSERT_TRUE(ireval::same_float(64, compiled.out[2], eval.out[2]))
            << describe(f, seed, "different frame state slot 2 (the float)");
        const GRCORE_CodeSite * site = grcore_codemeta_find(
            grjit_code_meta(c.code), static_cast<uint32_t>(compiled.out[slots]));
        ASSERT_NE(site, nullptr);
        ASSERT_EQ(site->identity.offset, eval.guard_identity.offset) << describe(f, seed, "different guard");
        deopted++;
      }
    }
    }
  }
  std::printf("float differential: per run: %zu returned, %zu guard exits; static, over the 3000 programs: "
              "%zu truncations, %zu conversions, %zu comparisons; %zu programs have more than one block\n",
      returned, deopted, truncs, converts, compares, multi_block);
  std::printf("float differential: every program run under each of the %zu subsets of the host's features; "
              "%zu compiles took a feature arm\n", masks, arm_programs);
  // Thresholds at about nine tenths of what the fixed seeds give (10649, 7351, 3937, 8222, 4108, 2196), the
  // run counts once per mask.
  EXPECT_GT(returned, 9500u * masks / kTsanDivisor);
  EXPECT_GT(deopted, 6600u * masks / kTsanDivisor);
  if (featfx::detected() != GRJIT_FEATURES_NONE) {
    EXPECT_GT(arm_programs, 0u) << "the float programs reach a rounding with SSE4.1 allowed";
  }
  EXPECT_GT(truncs, 3500u / kTsanDivisor);
  EXPECT_GT(converts, 7400u / kTsanDivisor);
  EXPECT_GT(compares, 3600u / kTsanDivisor);
  EXPECT_GT(multi_block, 1950u / kTsanDivisor);
}

/* The float programs again, with the arm64 code the emitter makes for each executed by the simulator (a dev
 * loop on any host; the native test above is the verdict on an arm64 host, under qemu-aarch64 or on a Pi). The
 * same programs and argument sets as the native test draws. */
TEST(Differential, GeneratedFloatProgramsRunTheSameWhenArm64CodeIsSimulatedAndEvaluatedWithNansEqual) {
  if (!floatfx::host_fp_is_default()) {
    GTEST_SKIP() << "the host's MXCSR / FPCR is not the default the simulator's arithmetic assumes";
  }
  JitWorld w;
  size_t returned = 0, deopted = 0;
  for (uint64_t seed = 1; seed <= 3000; seed++) {
    IntGen g(seed * 0x9E3779B97F4A7C15ull + 0xF10A7);
    FloatProgram prog = generate_float_program(g);
    Fn f(prog.f);
    char why[256] = {0};
    ASSERT_EQ(grjit_function_verify(f, nullptr, why, sizeof why), GRJIT_OK)
        << describe(f, seed, std::string("the generator made a function the verifier refuses: ") + why);
    static const std::vector<uint64_t> e32 = floatfx::f32_values(), e64 = floatfx::f64_values();
    static const std::vector<uint64_t> i32e = floatfx::int_values(true), i64e = floatfx::int_values(false);
    std::vector<std::vector<uint64_t>> arg_sets;
    for (int run = 0; run < 6; run++) {
      std::vector<uint64_t> args;
      for (size_t i = 0; i < prog.params; i++) {
        uint64_t v;
        if (i < 3) {
          v = e32[g.pick(e32.size())] | (g.pick(2) == 0 ? (g.rng() | 1) << 32 : 0); // garbage above bit 31
        } else if (i < 6) {
          v = e64[g.pick(e64.size())];
        } else if (i < 8) {
          v = i32e[g.pick(i32e.size())] | (g.pick(2) == 0 ? (g.rng() | 1) << 32 : 0);
        } else {
          v = i64e[g.pick(i64e.size())];
        }
        args.push_back(v);
      }
      arg_sets.push_back(args);
    }
    SimTarget c(f, w);
    ASSERT_TRUE(c.ok()) << describe(f, seed, c.why());
    for (const std::vector<uint64_t> & args : arg_sets) {
      SimTarget::Run run = c.run(w, args);
      ireval::Result eval = ireval::eval(f, w.ctx, nullptr, args);
      ASSERT_TRUE(same_as_evaluator(f, seed, run.exit, run.out, eval, c.meta(), 2, &returned, &deopted))
          << "(the arm64 code, simulated)";
    }
  }
  std::printf("float differential, arm64 simulated: %zu returned, %zu guard exits\n", returned, deopted);
  EXPECT_GT(returned, 9500u);
  EXPECT_GT(deopted, 6600u);
}

GRJIT_TEST_MAIN()
