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

GRJIT_TEST_MAIN()
