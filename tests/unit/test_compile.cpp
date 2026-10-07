/**
 * @file
 *
 * Compiling and running small functions: each operation against what C++
 * says it computes, the control flow, the calls, and the refusals.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <pthread.h>
#include "test_helpers.h"

#include "../../src/code/code_internal.h"

#include <cstdint>
#include <cstring>

namespace {

uint64_t helper_sum2(uint64_t a, uint64_t b) { return a + b * 3; }
uint64_t helper_six(uint64_t a, uint64_t b, uint64_t c, uint64_t d, uint64_t e,
    uint64_t f) {
  return a + 2 * b + 3 * c + 4 * d + 5 * e + 6 * f;
}
uint64_t helper_none() { return 0x1234; }
uint64_t g_last = 0;
uint64_t helper_record(uint64_t a) {
  g_last = a;
  return a ^ 0xFF;
}

/* A function of two I64 parameters that computes `dst = a OP b` and returns it. */
Compiled binary(JitWorld & w, GRJIT_OpKind op, GRJIT_Function ** keep) {
  B b("bin");
  GRJIT_VReg x = b.param(GRJIT_TYPE_I64);
  GRJIT_VReg y = b.param(GRJIT_TYPE_I64);
  GRJIT_VReg d = b.reg();
  b.at(b.block());
  b.bin(op, d, V(x), V(y));
  b.ret(V(d));
  *keep = b.finish();
  return Compiled(*keep, w.pages());
}

const int64_t kEdges[] = {0, 1, -1, INT64_MIN, INT64_MAX, 2, -2, 63, 64,
    0x123456789ABCDEF0, -0x123456789ABCDEF0};

uint64_t reference(GRJIT_OpKind op, uint64_t a, uint64_t b) {
  switch (op) {
    case GRJIT_OP_ADD: return a + b;
    case GRJIT_OP_SUB: return a - b;
    case GRJIT_OP_MUL: return a * b;
    case GRJIT_OP_AND: return a & b;
    case GRJIT_OP_OR: return a | b;
    case GRJIT_OP_XOR: return a ^ b;
    case GRJIT_OP_SHL: return a << (b & 63);
    case GRJIT_OP_SHR: return a >> (b & 63);
    case GRJIT_OP_SAR: return static_cast<uint64_t>(static_cast<int64_t>(a) >> (b & 63));
    default: return 0;
  }
}

} // namespace

TEST(Compile, TheBackendExistsOnTheGatedTarget) {
  GRJIT_REQUIRE_BACKEND();
}

TEST(Compile, EveryBinaryOperationMatchesCxxOnTheEdgeValues) {
  GRJIT_REQUIRE_BACKEND();
  JitWorld w;
  for (GRJIT_OpKind op : {GRJIT_OP_ADD, GRJIT_OP_SUB, GRJIT_OP_MUL, GRJIT_OP_AND,
           GRJIT_OP_OR, GRJIT_OP_XOR, GRJIT_OP_SHL, GRJIT_OP_SHR, GRJIT_OP_SAR}) {
    GRJIT_Function * f;
    Compiled c = binary(w, op, &f);
    Fn own(f);
    ASSERT_TRUE(c) << grjit_result_string(c.result);
    for (int64_t a : kEdges) {
      for (int64_t b : kEdges) {
        auto r = c.run(w.ctx, {static_cast<uint64_t>(a), static_cast<uint64_t>(b)});
        ASSERT_EQ(r.exit, static_cast<uint32_t>(GRJIT_EXIT_RETURNED));
        ASSERT_EQ(r.out[0], reference(op, a, b)) << "op " << op << " " << a << " " << b;
      }
    }
  }
}

TEST(Compile, ShiftCountsAreTakenModuloSixtyFourAsTheIrSays) {
  GRJIT_REQUIRE_BACKEND();
  JitWorld w;
  GRJIT_Function * f;
  Compiled c = binary(w, GRJIT_OP_SHL, &f);
  Fn own(f);
  ASSERT_TRUE(c);
  EXPECT_EQ(c.run(w.ctx, {1, 64}).out[0], 1u);   // 64 mod 64 = 0
  EXPECT_EQ(c.run(w.ctx, {1, 65}).out[0], 2u);
  EXPECT_EQ(c.run(w.ctx, {1, 63}).out[0], uint64_t{1} << 63);
}

TEST(Compile, ImmediateOperandsOnBothSides) {
  GRJIT_REQUIRE_BACKEND();
  JitWorld w;
  B b("imm");
  GRJIT_VReg x = b.param(GRJIT_TYPE_I64);
  GRJIT_VReg d = b.reg();
  b.at(b.block());
  b.bin(GRJIT_OP_SUB, d, I(1000), V(x));
  b.bin(GRJIT_OP_ADD, d, V(d), I(INT64_MIN));
  b.bin(GRJIT_OP_XOR, d, V(d), I(-1));
  b.ret(V(d));
  Fn f(b.finish());
  Compiled c(f, w.pages());
  ASSERT_TRUE(c);
  uint64_t x0 = 7;
  uint64_t want = ((1000 - x0) + static_cast<uint64_t>(INT64_MIN)) ^ ~uint64_t{0};
  EXPECT_EQ(c.run(w.ctx, {x0}).out[0], want);
}

TEST(Compile, NegNotAndMoveAndConst) {
  GRJIT_REQUIRE_BACKEND();
  JitWorld w;
  B b("unary");
  GRJIT_VReg x = b.param(GRJIT_TYPE_I64);
  GRJIT_VReg n = b.reg(), m = b.reg(), k = b.reg(), s = b.reg();
  b.at(b.block());
  b.un(GRJIT_OP_NEG, n, V(x));
  b.un(GRJIT_OP_NOT, m, V(x));
  b.cnst(k, INT64_MIN);
  b.bin(GRJIT_OP_ADD, s, V(n), V(m));
  b.bin(GRJIT_OP_ADD, s, V(s), V(k));
  b.ret(V(s));
  Fn f(b.finish());
  Compiled c(f, w.pages());
  ASSERT_TRUE(c);
  for (int64_t x0 : kEdges) {
    uint64_t u = static_cast<uint64_t>(x0);
    EXPECT_EQ(c.run(w.ctx, {u}).out[0], (0 - u) + ~u + static_cast<uint64_t>(INT64_MIN));
  }
}

TEST(Compile, EveryComparisonConditionSignedAndUnsigned) {
  GRJIT_REQUIRE_BACKEND();
  JitWorld w;
  struct Case { GRJIT_Cmp c; bool (*ref)(int64_t, int64_t); };
  const Case cases[] = {
      {GRJIT_CMP_EQ, [](int64_t a, int64_t b) { return a == b; }},
      {GRJIT_CMP_NE, [](int64_t a, int64_t b) { return a != b; }},
      {GRJIT_CMP_LT, [](int64_t a, int64_t b) { return a < b; }},
      {GRJIT_CMP_LE, [](int64_t a, int64_t b) { return a <= b; }},
      {GRJIT_CMP_GT, [](int64_t a, int64_t b) { return a > b; }},
      {GRJIT_CMP_GE, [](int64_t a, int64_t b) { return a >= b; }},
      {GRJIT_CMP_ULT, [](int64_t a, int64_t b) { return uint64_t(a) < uint64_t(b); }},
      {GRJIT_CMP_ULE, [](int64_t a, int64_t b) { return uint64_t(a) <= uint64_t(b); }},
      {GRJIT_CMP_UGT, [](int64_t a, int64_t b) { return uint64_t(a) > uint64_t(b); }},
      {GRJIT_CMP_UGE, [](int64_t a, int64_t b) { return uint64_t(a) >= uint64_t(b); }},
  };
  for (const Case & k : cases) {
    B b("cmp");
    GRJIT_VReg x = b.param(GRJIT_TYPE_I64);
    GRJIT_VReg y = b.param(GRJIT_TYPE_I64);
    GRJIT_VReg d = b.reg();
    b.at(b.block());
    b.cmp(k.c, d, V(x), V(y));
    b.ret(V(d));
    Fn f(b.finish());
    Compiled c(f, w.pages());
    ASSERT_TRUE(c);
    for (int64_t a : kEdges) {
      for (int64_t bb : kEdges) {
        auto r = c.run(w.ctx, {uint64_t(a), uint64_t(bb)});
        ASSERT_EQ(r.out[0], k.ref(a, bb) ? 1u : 0u) << "cmp " << k.c << " " << a << " " << bb;
      }
    }
  }
}

TEST(Compile, ACountingLoopSums) {
  GRJIT_REQUIRE_BACKEND();
  JitWorld w;
  B b("sum");
  GRJIT_VReg n = b.param(GRJIT_TYPE_I64);
  GRJIT_VReg i = b.reg(), sum = b.reg(), t = b.reg();
  GRJIT_BlockId entry = b.block(), head = b.block(), body = b.block(), done = b.block();
  b.at(entry);
  b.cnst(i, 0);
  b.cnst(sum, 0);
  b.br(head);
  b.at(head);
  b.cmp(GRJIT_CMP_LT, t, V(i), V(n));
  b.br_if(V(t), body, done);
  b.at(body);
  b.bin(GRJIT_OP_ADD, sum, V(sum), V(i));
  b.bin(GRJIT_OP_ADD, i, V(i), I(1));
  b.br(head);
  b.at(done);
  b.ret(V(sum));
  Fn f(b.finish());
  Compiled c(f, w.pages());
  ASSERT_TRUE(c);
  EXPECT_EQ(c.run(w.ctx, {0}).out[0], 0u);
  EXPECT_EQ(c.run(w.ctx, {1000}).out[0], 499500u);
}

TEST(Compile, ReturnWithNoValueLeavesOutAlone) {
  GRJIT_REQUIRE_BACKEND();
  JitWorld w;
  B b("void");
  b.at(b.block());
  b.ret();
  Fn f(b.finish());
  Compiled c(f, w.pages());
  ASSERT_TRUE(c);
  auto r = c.run(w.ctx);
  EXPECT_EQ(r.exit, static_cast<uint32_t>(GRJIT_EXIT_RETURNED));
  EXPECT_EQ(r.out[0], 0xDEADBEEFu);
  EXPECT_EQ(grjit_code_out_words(c.code), 1u);
}

TEST(Compile, MemoryWidthsLoadStoreAndSignExtension) {
  GRJIT_REQUIRE_BACKEND();
  JitWorld w;
  for (uint32_t bits : {8u, 16u, 32u, 64u}) {
    for (int32_t disp : {-16, 0, 5, 200, 100000}) {
      B b("mem");
      GRJIT_VReg base = b.param(GRJIT_TYPE_PTR);
      GRJIT_VReg val = b.param(GRJIT_TYPE_I64);
      GRJIT_VReg u = b.reg(), s = b.reg(), r = b.reg();
      b.at(b.block());
      b.store(base, disp, bits, V(val));
      b.load(u, base, disp, bits, false);
      b.load(s, base, disp, bits, true);
      b.bin(GRJIT_OP_XOR, r, V(u), V(s));
      b.store(base, disp + 0, 64 == bits ? 64 : bits, I(0));
      b.ret(V(r));
      Fn f(b.finish());
      Compiled c(f, w.pages());
      ASSERT_TRUE(c);
      std::vector<unsigned char> arena(200000, 0xAA);
      unsigned char * where = arena.data() + 50000;
      uint64_t value = 0x8899AABBCCDDEEF1ull;
      auto run = c.run(w.ctx, {reinterpret_cast<uint64_t>(where), value});
      uint64_t mask = bits == 64 ? ~uint64_t{0} : (uint64_t{1} << bits) - 1;
      uint64_t zero = value & mask;
      uint64_t sign = bits == 64 ? zero
                                 : ((zero >> (bits - 1)) & 1 ? zero | ~mask : zero);
      ASSERT_EQ(run.out[0], zero ^ sign) << bits << " " << disp;
      // The final store of zero cleared exactly `bits` bits and no neighbour.
      for (size_t k = 0; k < 16; k++) {
        unsigned char got = where[disp + static_cast<ptrdiff_t>(k)];
        unsigned char want = k < bits / 8 ? 0 : 0xAA;
        ASSERT_EQ(got, want) << bits << " " << disp << " " << k;
      }
    }
  }
}

TEST(Compile, NativeHelperCallsWithZeroToSixArguments) {
  GRJIT_REQUIRE_BACKEND();
  JitWorld w;
  B b("calls");
  GRJIT_VReg a = b.param(GRJIT_TYPE_I64);
  GRJIT_VReg r0 = b.reg(), r2 = b.reg(), r6 = b.reg(), s = b.reg();
  b.at(b.block());
  b.call(r0, reinterpret_cast<const void *>(helper_none));
  b.call(r2, reinterpret_cast<const void *>(helper_sum2), {V(a), I(10)});
  b.call(r6, reinterpret_cast<const void *>(helper_six),
      {V(a), I(1), I(2), I(3), V(r0), V(r2)});
  b.bin(GRJIT_OP_ADD, s, V(r0), V(r2));
  b.bin(GRJIT_OP_ADD, s, V(s), V(r6));
  b.ret(V(s));
  Fn f(b.finish());
  Compiled c(f, w.pages());
  ASSERT_TRUE(c);
  uint64_t a0 = 5;
  uint64_t v0 = helper_none();
  uint64_t v2 = helper_sum2(a0, 10);
  uint64_t v6 = helper_six(a0, 1, 2, 3, v0, v2);
  EXPECT_EQ(c.run(w.ctx, {a0}).out[0], v0 + v2 + v6);
}

TEST(Compile, ACallWithNoResultStillCalls) {
  GRJIT_REQUIRE_BACKEND();
  JitWorld w;
  B b("noresult");
  GRJIT_VReg a = b.param(GRJIT_TYPE_I64);
  b.at(b.block());
  b.call(GRJIT_NO_VREG, reinterpret_cast<const void *>(helper_record), {V(a)});
  b.ret();
  Fn f(b.finish());
  Compiled c(f, w.pages());
  ASSERT_TRUE(c);
  g_last = 0;
  c.run(w.ctx, {77});
  EXPECT_EQ(g_last, 77u);
}

TEST(Compile, ABranchOverMoreThanOneHundredTwentySevenBytesUsesRel32) {
  GRJIT_REQUIRE_BACKEND();
  JitWorld w;
  // A block of well over 200 bytes between the branch and its target.
  B b("long");
  GRJIT_VReg x = b.param(GRJIT_TYPE_I64);
  GRJIT_VReg t = b.reg(), d = b.reg();
  GRJIT_BlockId entry = b.block(), big = b.block(), join = b.block();
  b.at(entry);
  b.cnst(d, 1);
  b.cmp(GRJIT_CMP_EQ, t, V(x), I(0));
  b.br_if(V(t), join, big);
  b.at(big);
  for (int i = 0; i < 40; i++) {
    b.bin(GRJIT_OP_ADD, d, V(d), I(i + 2));
  }
  b.br(join);
  b.at(join);
  b.ret(V(d));
  Fn f(b.finish());
  Compiled c(f, w.pages());
  ASSERT_TRUE(c);
  EXPECT_GT(grjit_code_size(c.code), 400u);
  uint64_t add = 0;
  for (int i = 0; i < 40; i++) {
    add += i + 2;
  }
  EXPECT_EQ(c.run(w.ctx, {0}).out[0], 1u);
  EXPECT_EQ(c.run(w.ctx, {1}).out[0], 1u + add);
}

TEST(Compile, ACallOfNullCodeOrOfCodeMadeForAnotherLayoutIsRefusedAndNothingRuns) {
  GRJIT_REQUIRE_BACKEND();
  JitWorld w;
  B b("seven");
  b.at(b.block());
  GRJIT_VReg d = b.reg();
  b.cnst(d, 7);
  b.ret(V(d));
  Fn f(b.finish());
  Compiled c(f, w.pages());
  ASSERT_TRUE(c) << grjit_result_string(c.result);
  EXPECT_EQ(c.run(w.ctx).exit, static_cast<uint32_t>(GRJIT_EXIT_RETURNED));

  // A NULL code, with and without somewhere to put the reason.
  uint64_t out[2] = {0xDEADBEEF, 0xDEADBEEF};
  EXPECT_EQ(grjit_code_call(nullptr, w.ctx, nullptr, out),
      static_cast<uint32_t>(GRJIT_EXIT_REFUSED));
  EXPECT_EQ(out[0], static_cast<uint64_t>(GRCORE_ERR_INVALID));
  EXPECT_EQ(grjit_code_call(nullptr, w.ctx, nullptr, nullptr),
      static_cast<uint32_t>(GRJIT_EXIT_REFUSED));

  // The same code, as if a different core had compiled it: no instruction of
  // it runs (the function would have written 7 to out[0]).
  uint32_t * offset = &c.code->request_offset;
  uint32_t real = *offset;
  *offset = real + 8;
  out[0] = 0xDEADBEEF;
  EXPECT_EQ(grjit_code_call(c.code, w.ctx, nullptr, out), static_cast<uint32_t>(GRJIT_EXIT_REFUSED));
  EXPECT_EQ(out[0], static_cast<uint64_t>(GRCORE_ERR_INVALID));
  *offset = real;
  EXPECT_EQ(c.run(w.ctx).exit, static_cast<uint32_t>(GRJIT_EXIT_RETURNED));
}

TEST(Compile, AFunctionThatFailsTheVerifierIsRefused) {
  GRJIT_REQUIRE_BACKEND();
  JitWorld w;
  B b("bad");
  GRJIT_VReg x = b.reg();
  GRJIT_VReg y = b.reg();
  b.at(b.block());
  b.bin(GRJIT_OP_ADD, y, V(x), I(1)); // x never assigned
  b.ret(V(y));
  Fn f(b.finish());
  uint64_t bytes = w.bytes_in_use();
  Compiled c(f, w.pages());
  EXPECT_EQ(c.result, GRJIT_ERR_INVALID);
  EXPECT_EQ(c.code, nullptr);
  EXPECT_EQ(w.bytes_in_use(), bytes);
}

TEST(Compile, RefusesBadArgumentsAndAnOutputIsWrittenOnlyOnSuccess) {
  GRJIT_REQUIRE_BACKEND();
  JitWorld w;
  B b("ok");
  b.at(b.block());
  b.ret();
  Fn f(b.finish());
  GRJIT_CompileOptions o{};
  o.pages = w.pages();
  GRJIT_Code * out = reinterpret_cast<GRJIT_Code *>(1);
  EXPECT_EQ(grjit_compile(nullptr, f, &out), GRJIT_ERR_INVALID);
  EXPECT_EQ(grjit_compile(&o, nullptr, &out), GRJIT_ERR_INVALID);
  EXPECT_EQ(grjit_compile(&o, f, nullptr), GRJIT_ERR_INVALID);
  GRJIT_CompileOptions none{};
  EXPECT_EQ(grjit_compile(&none, f, &out), GRJIT_ERR_INVALID);
  EXPECT_EQ(out, reinterpret_cast<GRJIT_Code *>(1));
}

TEST(Compile, AFrameOverTheLimitIsALimitErrorAndNothingIsMapped) {
  GRJIT_REQUIRE_BACKEND();
  JitWorld w;
  // 70000 registers is a 560 KiB frame: over a 256 KiB cap, under the default.
  GRJIT_Limits limits{};
  limits.max_frame_bytes = 256 * 1024;
  limits.max_vregs = 100000;
  B b("big", 0, &limits);
  GRJIT_VReg r = 0;
  for (int i = 0; i < 70000; i++) {
    r = b.reg();
  }
  b.at(b.block());
  b.cnst(r, 1);
  b.ret(V(r));
  Fn f(b.finish());
  uint64_t blocks = w.blocks_in_use();
  Compiled c(f, w.pages(), nullptr, &limits);
  EXPECT_EQ(c.result, GRJIT_ERR_LIMIT);
  EXPECT_EQ(w.blocks_in_use(), blocks);
  // The same function compiles under the default frame cap (1 MiB), with the
  // register cap raised so that the verifier agrees.
  GRJIT_Limits roomy{};
  roomy.max_vregs = 100000;
  Compiled ok(f, w.pages(), nullptr, &roomy);
  ASSERT_TRUE(ok) << grjit_result_string(ok.result);
  EXPECT_EQ(ok.run(w.ctx).out[0], 1u);
}

namespace {
uint64_t helper_zero() { return 0; }

/* `sites` GC-point calls, a reference live across each: `sites` entries. */
GRJIT_Function * with_sites(int sites, const GRJIT_Limits * limits) {
  B b("sites", 0, limits);
  GRJIT_VReg acc = b.reg(), k = b.reg(), t = b.reg();
  b.at(b.block());
  b.cnst(acc, 0);
  for (int i = 0; i < sites; i++) {
    GRJIT_VReg r = b.reg(GRJIT_TYPE_REF);
    b.cnst(k, i);
    b.bitcast(r, k);
    b.call_gc(GRJIT_NO_VREG, reinterpret_cast<const void *>(helper_zero), {},
        GRCORE_PollIdentity{1, static_cast<uint64_t>(i)}, {});
    b.bitcast(t, r);
    b.bin(GRJIT_OP_ADD, acc, V(acc), V(t));
  }
  b.ret(V(acc));
  return b.finish();
}
} // namespace

TEST(Compile, TheRegistersRecordedAsLiveOverAllSitesAreCappedAndNothingIsMapped) {
  GRJIT_REQUIRE_BACKEND();
  JitWorld w;
  constexpr int kSites = 50;
  Fn f(with_sites(kSites, nullptr));
  // Fifty sites with one live reference each: fifty entries. At the cap it
  // compiles; one under it is a limit error from the verifier's own liveness
  // as much as from the emitter's.
  GRJIT_Limits at{};
  at.max_site_entries = kSites;
  Compiled ok(f, w.pages(), nullptr, &at);
  ASSERT_TRUE(ok) << grjit_result_string(ok.result);
  EXPECT_EQ(ok.run(w.ctx).out[0], static_cast<uint64_t>(kSites * (kSites - 1) / 2));
  GRJIT_Limits under{};
  under.max_site_entries = kSites - 1;
  uint64_t blocks = w.blocks_in_use();
  Compiled refused(f, w.pages(), nullptr, &under);
  EXPECT_EQ(refused.result, GRJIT_ERR_LIMIT);
  EXPECT_EQ(w.blocks_in_use(), blocks);
  EXPECT_EQ(grjit_function_verify(f, &under, nullptr, 0), GRJIT_ERR_LIMIT);
  EXPECT_EQ(grjit_function_verify(f, &at, nullptr, 0), GRJIT_OK);
}

TEST(Compile, ATwoMebibyteFrameIsALimitError) {
  GRJIT_REQUIRE_BACKEND();
  JitWorld w;
  GRJIT_Limits limits{};
  limits.max_vregs = 300000;
  B b("huge", 0, &limits);
  GRJIT_VReg r = 0;
  for (int i = 0; i < 270000; i++) { // 2.1 MB of frame
    r = b.reg();
  }
  b.at(b.block());
  b.cnst(r, 1);
  b.ret(V(r));
  Fn f(b.finish());
  Compiled c(f, w.pages(), nullptr, &limits);
  EXPECT_EQ(c.result, GRJIT_ERR_LIMIT);
  EXPECT_EQ(w.blocks_in_use(), 0u);
}

namespace {
/* A frame of more than 16 MiB, in a thread of its own with a stack to hold it. */
struct BigFrame {
  size_t vregs;
  bool ok = false;
  int result = 0;
  uint64_t value = 0;
  bool arm64_through_x17 = false; // the arm64 code adjusts sp by x17 (a frame of 2^24 bytes or more)
  int cross_result[2] = {0, 0};   // grjit_emit_for for x86-64 and arm64, whatever this host is
};

void * run_big_frame(void * arg) {
  BigFrame * bf = static_cast<BigFrame *>(arg);
  JitWorld w;
  GRJIT_Limits limits{};
  limits.max_vregs = bf->vregs + 8;
  limits.max_frame_bytes = size_t{64} << 20;
  B b("huge", 0, &limits);
  GRJIT_VReg first = b.reg();
  GRJIT_VReg last = first;
  for (size_t i = 1; i < bf->vregs; i++) {
    last = b.reg();
  }
  GRJIT_VReg r = b.reg();
  b.at(b.block());
  b.cnst(first, 5);
  b.cnst(last, 7);
  b.bin(GRJIT_OP_ADD, r, V(first), V(last));
  b.ret(V(r));
  Fn f(b.finish());
  {
    const GRJIT_Arch arches[2] = {GRJIT_ARCH_X86_64, GRJIT_ARCH_ARM64};
    for (int k = 0; k < 2; k++) {
      GRJIT_Emitted e;
      bf->cross_result[k] = grjit_emit_for(arches[k], f, grjit_allocator_default(), &limits, nullptr, 0x40, &e);
      if (k == 1 && bf->cross_result[k] == GRJIT_OK) {
        for (size_t i = 0; i + 4 <= e.size; i += 4) {
          uint32_t word;
          std::memcpy(&word, e.bytes + i, 4);
          bf->arm64_through_x17 = bf->arm64_through_x17 || word == 0xCB3163FFu; // sub sp, sp, x17
        }
      }
      grjit_emitted_free(&e);
    }
  }
  Compiled c(f, w.pages(), nullptr, &limits);
  bf->result = c.result;
  if (c) {
    bf->value = c.run(w.ctx).out[0];
    bf->ok = true;
  }
  return nullptr;
}
} // namespace

TEST(Compile, AFrameOfMoreThanSixteenMebibytesCompilesAndRunsOnEveryTargetAsOneUnderItDoes) {
  GRJIT_REQUIRE_BACKEND();
  // 2^24 bytes of frame is 2,097,152 words: three fixed slots and one per register. One word under it and a
  // few over it, so the arm64 frame adjustment and the slot offsets cross the point where an add/sub
  // immediate (24 bits) stops holding them.
  for (size_t vregs : {size_t{2097100}, size_t{2097200}, size_t{2300000}}) {
    SCOPED_TRACE(vregs);
    BigFrame bf;
    bf.vregs = vregs;
    pthread_attr_t attr;
    ASSERT_EQ(pthread_attr_init(&attr), 0);
    ASSERT_EQ(pthread_attr_setstacksize(&attr, size_t{96} << 20), 0);
    pthread_t th;
    ASSERT_EQ(pthread_create(&th, &attr, run_big_frame, &bf), 0);
    ASSERT_EQ(pthread_join(th, nullptr), 0);
    pthread_attr_destroy(&attr);
    EXPECT_EQ(bf.cross_result[0], GRJIT_OK) << "x86-64 emits it";
    EXPECT_EQ(bf.cross_result[1], GRJIT_OK) << "and so does arm64, on any host";
    // 2^24 bytes of frame is 2,097,152 words less the three fixed ones: the first count is under it.
    EXPECT_EQ(bf.arm64_through_x17, vregs + 1 + 3 >= (size_t{1} << 21) - 1) << "sp moves through x17 from 2^24 bytes";
    ASSERT_TRUE(bf.ok) << "a frame is a limit of the cap, not of the target: "
                       << grjit_result_string(static_cast<GRJIT_Result>(bf.result));
    EXPECT_EQ(bf.value, 12u);
  }
}

TEST(Compile, CodeOverTheByteCapIsALimitErrorAndNothingIsMapped) {
  GRJIT_REQUIRE_BACKEND();
  JitWorld w;
  B b("long");
  GRJIT_VReg d = b.reg();
  b.at(b.block());
  b.cnst(d, 0);
  for (int i = 0; i < 100; i++) {
    b.bin(GRJIT_OP_ADD, d, V(d), I(i));
  }
  b.ret(V(d));
  Fn f(b.finish());
  GRJIT_Limits limits{};
  limits.max_code_bytes = 500;
  Compiled small(f, w.pages(), nullptr, &limits);
  EXPECT_EQ(small.result, GRJIT_ERR_LIMIT);
  EXPECT_EQ(w.blocks_in_use(), 0u);
  EXPECT_EQ(w.bytes_in_use(), 0u);
  Compiled ok(f, w.pages());
  EXPECT_TRUE(ok);
}

TEST(Compile, UnreachableBlocksAndUnusedRegistersCompile) {
  GRJIT_REQUIRE_BACKEND();
  JitWorld w;
  B b("dead");
  GRJIT_VReg unused = b.reg();
  (void)unused;
  GRJIT_BlockId entry = b.block(), never = b.block();
  GRJIT_VReg v = b.reg();
  b.at(entry);
  b.cnst(v, 9);
  b.ret(V(v));
  b.at(never);
  GRJIT_VReg q = b.reg();
  b.bin(GRJIT_OP_ADD, q, V(q), I(1)); // would be unassigned, but unreachable
  b.ret(V(q));
  Fn f(b.finish());
  Compiled c(f, w.pages());
  ASSERT_TRUE(c) << grjit_result_string(c.result);
  EXPECT_EQ(c.run(w.ctx).out[0], 9u);
}

TEST(Compile, TheUnusedTailOfTheLastPageIsATrapNotZeroBytes) {
  GRJIT_REQUIRE_BACKEND();
  JitWorld w;
  B b("tiny");
  b.at(b.block());
  b.ret();
  Fn f(b.finish());
  Compiled c(f, w.pages());
  ASSERT_TRUE(c);
  const unsigned char * code = static_cast<const unsigned char *>(grjit_code_address(c.code));
  ASSERT_GT(grjit_code_mapped_size(c.code), grjit_code_size(c.code));
#if defined(__aarch64__)
  // `brk #0` words (0xD4200000, little-endian) on arm64, where a zero word is
  // `udf #0` and traps only by accident.
  ASSERT_EQ(grjit_code_size(c.code) % 4, 0u);
  for (size_t i = grjit_code_size(c.code); i + 4 <= grjit_code_mapped_size(c.code); i += 4) {
    ASSERT_EQ(code[i], 0x00) << i;
    ASSERT_EQ(code[i + 1], 0x00) << i;
    ASSERT_EQ(code[i + 2], 0x20) << i;
    ASSERT_EQ(code[i + 3], 0xD4) << i;
  }
#elif defined(_WIN64) && defined(__x86_64__)
  // `int3` on Windows x86-64 too, except where the unwind information is: a
  // 12-byte RUNTIME_FUNCTION at the next 4-byte boundary after the code, then
  // an UNWIND_INFO of at most 16 bytes (Win64Memory in test_win64.cpp reads them). The padding before them and everything after them is `int3`.
  size_t table_at = (grjit_code_size(c.code) + 3) / 4 * 4;
  for (size_t i = grjit_code_size(c.code); i < table_at; i++) {
    ASSERT_EQ(code[i], 0xCC) << i;
  }
  for (size_t i = table_at + 12 + 16; i < grjit_code_mapped_size(c.code); i++) {
    ASSERT_EQ(code[i], 0xCC) << i;
  }
#else
  for (size_t i = grjit_code_size(c.code); i < grjit_code_mapped_size(c.code); i++) {
    ASSERT_EQ(code[i], 0xCC) << i;
  }
#endif
}

TEST(Compile, TheCodeCarriesItsLayoutAndAddressAndIsOneMapping) {
  GRJIT_REQUIRE_BACKEND();
  JitWorld w;
  B b("tiny");
  b.at(b.block());
  b.ret();
  Fn f(b.finish());
  uint64_t before = w.bytes_in_use();
  Compiled c(f, w.pages());
  ASSERT_TRUE(c);
  EXPECT_NE(grjit_code_address(c.code), nullptr);
  EXPECT_EQ(grjit_code_mapped_size(c.code) % w.pages()->page_size, 0u);
  EXPECT_GE(grjit_code_mapped_size(c.code), grjit_code_size(c.code));
  EXPECT_EQ(w.bytes_in_use(), before + grjit_code_mapped_size(c.code));
  EXPECT_EQ(w.blocks_in_use(), 1u);
  EXPECT_NE(grjit_code_entry(c.code), nullptr);
}

GRJIT_TEST_MAIN()

TEST(Compile, ABitcastComputesOnATaggedValueAndTagsTheResult) {
  GRJIT_REQUIRE_BACKEND();
  JitWorld w;
  // A tagged small integer (n << 4 | 1) in, n + 5 tagged out: the shape of a
  // baseline JIT's integer add.
  B b("tagged");
  GRJIT_VReg x = b.param(GRJIT_TYPE_REF);
  GRJIT_VReg raw = b.reg(GRJIT_TYPE_I64), n = b.reg(GRJIT_TYPE_I64);
  GRJIT_VReg out = b.reg(GRJIT_TYPE_REF);
  b.at(b.block());
  b.bitcast(raw, x);
  b.bin(GRJIT_OP_SAR, n, V(raw), I(4));
  b.bin(GRJIT_OP_ADD, n, V(n), I(5));
  b.bin(GRJIT_OP_SHL, n, V(n), I(4));
  b.bin(GRJIT_OP_OR, n, V(n), I(1));
  b.bitcast(out, n);
  b.ret(V(out));
  Fn f(b.finish());
  Compiled c(f, w.pages());
  ASSERT_TRUE(c);
  for (int64_t v : {int64_t{0}, int64_t{1}, int64_t{-1}, int64_t{123456789}, int64_t{-987654321}}) {
    uint64_t in = (static_cast<uint64_t>(v) << 4) | 1u;
    uint64_t want = (static_cast<uint64_t>(v + 5) << 4) | 1u;
    EXPECT_EQ(c.run(w.ctx, {in}).out[0], want) << v;
  }
}

TEST(Compile, ABitcastToPtrAndBackKeepsEveryBit) {
  GRJIT_REQUIRE_BACKEND();
  JitWorld w;
  B b("roundtrip");
  GRJIT_VReg x = b.param(GRJIT_TYPE_I64);
  GRJIT_VReg p = b.reg(GRJIT_TYPE_PTR), r = b.reg(GRJIT_TYPE_REF), d = b.reg(GRJIT_TYPE_I64);
  b.at(b.block());
  b.bitcast(p, x);
  b.bitcast(r, p);
  b.bitcast(d, r);
  b.ret(V(d));
  Fn f(b.finish());
  Compiled c(f, w.pages());
  ASSERT_TRUE(c);
  for (int64_t v : kEdges) {
    EXPECT_EQ(c.run(w.ctx, {static_cast<uint64_t>(v)}).out[0], static_cast<uint64_t>(v));
  }
}

TEST(Compile, ARefBitcastToI64IsNotInTheStackMapButItsRefSourceIsWhileLive) {
  GRJIT_REQUIRE_BACKEND();
  JitWorld w;
  // The REF parameter is named by the call's frame state, so it is live at the
  // site; the I64 computed from it is not a reference and is never mapped.
  B b("maps", 2);
  GRJIT_VReg x = b.param(GRJIT_TYPE_REF);
  GRJIT_VReg raw = b.reg(GRJIT_TYPE_I64), out = b.reg(GRJIT_TYPE_I64);
  b.at(b.block());
  b.bitcast(raw, x);
  b.call_gc(out, reinterpret_cast<const void *>(helper_record), {V(raw)}, {1, 2},
      {grjit_frame_slot_vreg(x), grjit_frame_slot_vreg(raw)});
  b.ret(V(out));
  Fn f(b.finish());
  Compiled c(f, w.pages());
  ASSERT_TRUE(c);
  const GRCORE_CodeMeta * meta = grjit_code_meta(c.code);
  ASSERT_EQ(meta->site_count, 1u);
  const GRCORE_CodeSite & s = meta->sites[0];
  ASSERT_EQ(s.live_count, 1u);
  EXPECT_EQ(s.live[0].slot_kind, GRCORE_SLOT_VALUE);
  ASSERT_EQ(s.frame_state_count, 2u);
  EXPECT_EQ(s.frame_state[0].slot_kind, GRCORE_SLOT_VALUE);
  EXPECT_EQ(s.frame_state[1].slot_kind, GRCORE_SLOT_RAW);
  EXPECT_NE(s.live[0].value, s.frame_state[1].value);
  EXPECT_EQ(s.live[0].value, s.frame_state[0].value);
}
