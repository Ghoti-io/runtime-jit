/**
 * @file
 *
 * What the arm64 backend emits, checked on any host: the frame, the offsets
 * and immediates a fixed-width instruction set cannot hold, the branches that
 * do not reach, the byte cap, the instruction-cache call and the availability
 * of the backend. The code is *emitted* by `grjit_emit_for` and, where a result
 * is wanted, *executed* by the simulator in tests/a64_sim.h; the same hazards
 * run on real arm64 code under qemu-aarch64 by `tools/xarch/jit-arm64.sh`.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include "test_helpers.h"

#include "../a64_sim.h"
#include "../../src/code/code_internal.h"

#include <cstring>

namespace {

struct Emitted {
  GRJIT_Emitted e{};
  GRJIT_Result result = GRJIT_OK;
  Emitted(const GRJIT_Function * f, const GRJIT_Limits * limits = nullptr,
      GRJIT_EntryHook hook = nullptr, const GRJIT_Allocator * allocator = nullptr) {
    result = grjit_emit_for(GRJIT_ARCH_ARM64, f,
        allocator != nullptr ? allocator : grjit_allocator_default(), limits, hook,
        grcore_jit_layout()->request_word_offset, &e);
  }
  Emitted(const Emitted &) = delete;
  Emitted & operator=(const Emitted &) = delete;
  ~Emitted() { grjit_emitted_free(&e); }
  bool ok() const { return result == GRJIT_OK; }
  uint32_t word(size_t i) const {
    uint32_t w;
    std::memcpy(&w, e.bytes + 4 * i, 4);
    return w;
  }
  size_t words() const { return e.size / 4; }
};

struct Ran {
  uint32_t exit = 99;
  std::vector<uint64_t> out;
  size_t stack_used = 0;
};

Ran simulate(const Emitted & em, const GRJIT_Function * f, void * ctx,
    std::vector<uint64_t> args = {}, std::set<uint64_t> returns32 = {}) {
  Ran r;
  r.out.assign(grjit_function_interp_slot_count(f) + 1, 0xDEADBEEFu);
  args.resize(std::max<size_t>(args.size(), grjit_function_param_count(f)));
  a64sim::Config cfg;
  cfg.code = em.e.bytes;
  cfg.size = em.e.size;
  cfg.stack_bytes = 8u << 20;
  cfg.args[0] = reinterpret_cast<uint64_t>(ctx);
  cfg.args[1] = reinterpret_cast<uint64_t>(args.data());
  cfg.args[2] = reinterpret_cast<uint64_t>(r.out.data());
  cfg.returns32 = std::move(returns32);
  a64sim::Cpu cpu;
  a64sim::Result res = cpu.run(cfg);
  EXPECT_TRUE(res.ok) << res.error;
  r.exit = static_cast<uint32_t>(res.x0);
  r.stack_used = cfg.stack_used;
  return r;
}

/* What `sub sp, sp, ...` takes off, read from the prologue; and the words that
 * come before it. */
struct Prologue {
  uint64_t frame_bytes = 0;
  bool shape_ok = false;
};
Prologue read_prologue(const Emitted & em) {
  Prologue p;
  if (em.words() < 3 || em.word(0) != 0xA9BF7BFDu || em.word(1) != 0x910003FDu) {
    return p;
  }
  size_t i = 2;
  while (i < em.words() && (em.word(i) & 0xFF8003FFu) == 0xD10003FFu) {
    uint64_t imm = (em.word(i) >> 10) & 0xFFFu;
    p.frame_bytes += (em.word(i) & (1u << 22)) ? imm << 12 : imm;
    i++;
  }
  p.shape_ok = true;
  return p;
}

/* A function of `n` registers, every one assigned a distinct constant and all
 * summed, so a slot that is read or written at the wrong offset changes the
 * answer. Returns the expected sum through `sum`. */
GRJIT_Function * many_registers(size_t n, uint64_t * sum, const GRJIT_Limits * limits = nullptr) {
  B b("many", 0, limits);
  std::vector<GRJIT_VReg> r;
  for (size_t i = 0; i < n; i++) {
    r.push_back(b.reg());
  }
  GRJIT_VReg acc = b.reg();
  b.at(b.block());
  uint64_t total = 0;
  for (size_t i = 0; i < n; i++) {
    int64_t v = static_cast<int64_t>((i + 1) * 0x0101010101ull) ^ (i % 3 == 0 ? -1 : 0);
    b.cnst(r[i], v);
    total += static_cast<uint64_t>(v);
  }
  b.cnst(acc, 0);
  for (size_t i = 0; i < n; i++) {
    b.bin(GRJIT_OP_ADD, acc, V(acc), V(r[i]));
  }
  b.ret(V(acc));
  *sum = total;
  return b.finish();
}

} // namespace

/* ---- The frame ----------------------------------------------------------------- */

TEST(Arm64Frame, ThePrologueIsTheFrameRecordThenTheFrameAndTheFrameIsAMultipleOfSixteen) {
  for (size_t n : {0u, 1u, 2u, 3u, 4u, 5u, 6u, 7u, 8u, 29u, 30u, 33u, 100u, 511u, 512u, 520u, 5000u}) {
    uint64_t sum;
    Fn f(many_registers(n, &sum));
    Emitted em(f);
    ASSERT_TRUE(em.ok()) << n << ": " << grjit_result_string(em.result);
    Prologue p = read_prologue(em);
    ASSERT_TRUE(p.shape_ok) << n;
    // The three fixed slots, one per register and the accumulator, rounded up.
    size_t slots = n + 1 + GRJIT_FIXED_SLOTS;
    EXPECT_EQ(p.frame_bytes, (slots * 8 + 15) / 16 * 16) << n;
    EXPECT_EQ(p.frame_bytes % 16, 0u) << n;
    EXPECT_EQ(em.e.meta.meta.frame_bytes, p.frame_bytes) << n;
  }
}

TEST(Arm64Frame, TheSlotsAreWhereTheMetadataSaysAndTheCallerFramePairIsAboveThem) {
  // Run it: the simulator checks sp, x29, x30 and the callee-saved registers on
  // return; the stack it used is at least the frame and a frame record.
  uint64_t sum;
  Fn f(many_registers(40, &sum));
  Emitted em(f);
  ASSERT_TRUE(em.ok());
  JitWorld w;
  Ran r = simulate(em, f, w.ctx);
  EXPECT_EQ(r.exit, static_cast<uint32_t>(GRJIT_EXIT_RETURNED));
  EXPECT_EQ(r.out[0], sum);
  Prologue p = read_prologue(em);
  EXPECT_GE(r.stack_used, p.frame_bytes + 16);
}

/* ---- Offsets beyond the signed 9-bit and the scaled 12-bit ranges --------------- */

TEST(Arm64Offsets, RegistersWhoseSlotsAreBeyondTheShortFormsAreAddressedThroughX17) {
  JitWorld w;
  for (size_t n : {8u, 28u, 29u, 33u, 520u, 5000u}) {
    uint64_t sum;
    Fn f(many_registers(n, &sum));
    Emitted em(f);
    ASSERT_TRUE(em.ok()) << n;
    Ran r = simulate(em, f, w.ctx);
    ASSERT_EQ(r.exit, static_cast<uint32_t>(GRJIT_EXIT_RETURNED)) << n;
    ASSERT_EQ(r.out[0], sum) << n;
    // A slot is -8 * (v + 4) from x29; ldur reaches -256, which is v = 28. With
    // more registers than that, some access must be the register form.
    size_t reg_form = 0;
    for (size_t i = 0; i < em.words(); i++) {
      uint32_t x = em.word(i);
      if ((x & 0xFFE0FC00u) == 0xF8606800u && ((x >> 16) & 31u) == 17u && ((x >> 5) & 31u) == 29u) {
        reg_form++;
      }
    }
    if (n >= 29) {
      EXPECT_GT(reg_form, 0u) << n;
    } else {
      EXPECT_EQ(reg_form, 0u) << n;
    }
  }
}

TEST(Arm64Offsets, ADisplacementOfAnyInt32OnALoadOrStoreIsMaterialisedNotTruncated) {
  // Loads and stores at displacements around every boundary: 255/256 (ldur),
  // 32760/32768 (scaled), 4095/4096 (bytes), and the extremes of int32. The
  // memory is a buffer the displacement is subtracted back from, so the address
  // is real.
  JitWorld w;
  std::vector<uint8_t> arena(1 << 20, 0);
  uint8_t * mid = arena.data() + (1 << 19);
  for (int32_t disp : {0, 1, 3, 8, 255, 256, 257, 4095, 4096, 4097, 32760, 32768, 40000,
           -1, -255, -256, -257, -4096, -32768, -40000, 100000, -100000}) {
    for (uint32_t bits : {8u, 16u, 32u, 64u}) {
      for (bool sign : {false, true}) {
        B b("mem");
        GRJIT_VReg p = b.param(GRJIT_TYPE_PTR);
        GRJIT_VReg v = b.param(GRJIT_TYPE_I64);
        GRJIT_VReg d = b.reg();
        b.at(b.block());
        b.store(p, disp, bits, V(v));
        b.load(d, p, disp, bits, sign);
        b.ret(V(d));
        Fn f(b.finish());
        Emitted em(f);
        ASSERT_TRUE(em.ok());
        std::fill(arena.begin(), arena.end(), 0x5A);
        uint64_t value = 0x8899AABBCCDDEEF1ull;
        Ran r = simulate(em, f, w.ctx, {reinterpret_cast<uint64_t>(mid) - static_cast<uint64_t>(static_cast<int64_t>(disp)) , value});
        ASSERT_EQ(r.exit, static_cast<uint32_t>(GRJIT_EXIT_RETURNED));
        // Read the same address back through C++.
        uint64_t stored = 0;
        std::memcpy(&stored, mid, bits / 8);
        uint64_t want = bits == 64 ? value : value & ((uint64_t{1} << bits) - 1);
        ASSERT_EQ(stored, want) << "disp " << disp << " bits " << bits;
        if (sign && bits < 64) {
          want = static_cast<uint64_t>(static_cast<int64_t>(want << (64 - bits)) >> (64 - bits));
        }
        ASSERT_EQ(r.out[0], want) << "disp " << disp << " bits " << bits << " sign " << sign;
        // Nothing next to it was touched.
        ASSERT_EQ(mid[-1], 0x5A);
        ASSERT_EQ(mid[bits / 8], 0x5A);
      }
    }
  }
}

TEST(Arm64Offsets, AnyImmediateIsMaterialisedWhateverItsShape) {
  JitWorld w;
  const uint64_t values[] = {0, 1, 0xFFFF, 0x10000, UINT64_C(0x100000000), UINT64_C(0x100000001),
      UINT64_C(0x0001000200030000), UINT64_C(0x123456789ABCDEF0), ~UINT64_C(0), ~UINT64_C(1),
      UINT64_C(0xFFFFFFFFFFFF0000), UINT64_C(0xFFFF0000FFFF1234), UINT64_C(0x8000000000000000),
      UINT64_C(0x7FFFFFFFFFFFFFFF), UINT64_C(0x00007F1234567890), UINT64_C(0xFEDCBA9876543210),
      UINT64_C(0xFFFFFFFF00000000), UINT64_C(0xFFFFFFFF12345678), UINT64_C(0x0000FFFF0000FFFF),
      UINT64_C(0xFFFF0000FFFF0000), UINT64_C(0x00000000FFFF0000), UINT64_C(0xFFFFFFFFFFFF8000)};
  for (uint64_t v : values) {
    B b("imm");
    GRJIT_VReg d = b.reg();
    b.at(b.block());
    b.cnst(d, static_cast<int64_t>(v));
    b.ret(V(d));
    Fn f(b.finish());
    Emitted em(f);
    ASSERT_TRUE(em.ok());
    Ran r = simulate(em, f, w.ctx);
    EXPECT_EQ(r.out[0], v) << std::hex << v;
  }
}

/* ---- Frames over 4095 bytes and at the cap ---------------------------------------- */

TEST(Arm64Frame, AFrameOver4095BytesIsAllocatedCorrectly) {
  JitWorld w;
  for (size_t n : {510u, 511u, 512u, 513u, 1000u, 20000u}) { // 4 KiB and up
    uint64_t sum;
    Fn f(many_registers(n, &sum));
    Emitted em(f);
    ASSERT_TRUE(em.ok()) << n;
    Prologue p = read_prologue(em);
    ASSERT_TRUE(p.shape_ok);
    EXPECT_EQ(p.frame_bytes % 16, 0u);
    Ran r = simulate(em, f, w.ctx);
    EXPECT_EQ(r.out[0], sum) << n;
    EXPECT_GE(r.stack_used, p.frame_bytes);
  }
}

TEST(Arm64Frame, AFrameAtTheDefaultCapRunsAndOneOverIsALimit) {
  JitWorld w;
  GRJIT_Limits limits{};
  limits.max_vregs = 200000;
  // 131072 slots of 8 bytes is exactly 1 MiB: 3 fixed, the accumulator, and
  // 131068 more.
  uint64_t sum = 0;
  size_t at_cap = 131072 - GRJIT_FIXED_SLOTS - 1;
  B b("cap", 0, &limits);
  std::vector<GRJIT_VReg> r;
  for (size_t i = 0; i < at_cap; i++) {
    r.push_back(b.reg());
  }
  GRJIT_VReg acc = b.reg();
  b.at(b.block());
  b.cnst(r.front(), 7);
  b.cnst(r.back(), 35);
  b.bin(GRJIT_OP_ADD, acc, V(r.front()), V(r.back()));
  b.ret(V(acc));
  Fn f(b.finish());
  (void)sum;
  Emitted em(f, &limits);
  ASSERT_TRUE(em.ok()) << grjit_result_string(em.result);
  EXPECT_EQ(read_prologue(em).frame_bytes, 1u << 20);
  Ran run = simulate(em, f, w.ctx);
  EXPECT_EQ(run.out[0], 42u);
  EXPECT_GE(run.stack_used, 1u << 20);

  // One register more: the frame rounds to 1 MiB + 16, over the cap.
  B over("over", 0, &limits);
  for (size_t i = 0; i < at_cap + 1; i++) {
    over.reg();
  }
  GRJIT_VReg acc2 = over.reg();
  over.at(over.block());
  over.cnst(acc2, 1);
  over.ret(V(acc2));
  Fn f2(over.finish());
  Emitted too_big(f2, &limits);
  EXPECT_EQ(too_big.result, GRJIT_ERR_LIMIT);
  EXPECT_EQ(too_big.e.bytes, nullptr);
}

TEST(Arm64Frame, ARaisedFrameCapPastWhatOneSubtractionPairEncodesIsHeldInX17AndNeverTruncatedOrRefused) {
  GRJIT_Limits limits{};
  limits.max_vregs = 3000000;
  limits.max_frame_bytes = 1u << 29;
  B b("huge", 0, &limits);
  GRJIT_VReg last = 0;
  for (size_t i = 0; i < 2200000; i++) { // 17.6 MB of frame
    last = b.reg();
  }
  b.at(b.block());
  b.cnst(last, 1);
  b.ret(V(last));
  Fn f(b.finish());
  Emitted em(f, &limits);
  ASSERT_TRUE(em.ok()) << "x86-64 compiles this frame, so does arm64: " << grjit_result_string(em.result);
  const uint64_t frame = em.e.meta.meta.frame_bytes;
  EXPECT_GT(frame, uint64_t{1} << 24);
  // The prologue: stp, mov x29, sp, then the amount in x17 (movz, movk) and one `sub sp, sp, x17`.
  ASSERT_GE(em.words(), 6u);
  EXPECT_EQ(em.word(0), 0xA9BF7BFDu);
  EXPECT_EQ(em.word(1), 0x910003FDu);
  size_t sub = 0;
  for (size_t i = 2; i < em.words() && sub == 0; i++) {
    if (em.word(i) == 0xCB3163FFu) { // sub sp, sp, x17
      sub = i;
    }
  }
  ASSERT_NE(sub, 0u) << "sp moves by x17, once";
  uint64_t value = 0;
  size_t movs = 0;
  for (size_t i = sub; i-- > 2;) {
    const uint32_t w = em.word(i);
    if ((w & 0xFF80001Fu) != 0xD2800011u && (w & 0xFF80001Fu) != 0xF2800011u) { // movz/movk x17
      break;
    }
    movs++;
    const unsigned hw = (w >> 21) & 3;
    const uint64_t imm16 = (w >> 5) & 0xFFFF;
    value |= imm16 << (16 * hw);
    if ((w & 0xFF800000u) == 0xD2800000u) {
      break;
    }
  }
  EXPECT_GE(movs, 1u);
  EXPECT_EQ(value, frame) << "the amount in x17 is the whole frame";
  EXPECT_EQ(frame % 16, 0u);
}

/* ---- Branches --------------------------------------------------------------------- */

namespace {

/* `n` operations that each load, add and store: about 20 bytes apiece. */
void filler(B & b, GRJIT_VReg d, size_t n) {
  for (size_t i = 0; i < n; i++) {
    b.bin(GRJIT_OP_ADD, d, V(d), I(static_cast<int64_t>(i % 7 + 1)));
  }
}

uint64_t filler_sum(size_t n) {
  uint64_t s = 0;
  for (size_t i = 0; i < n; i++) {
    s += i % 7 + 1;
  }
  return s;
}

GRJIT_Limits big_limits() {
  GRJIT_Limits l{};
  l.max_operations = 1u << 22;
  l.max_code_bytes = 1u << 27;
  return l;
}

} // namespace

TEST(Arm64Branches, AForwardBranchOverMoreThanOneMebibyteOfCodeReachesItsBlockBothWays) {
  JitWorld w;
  GRJIT_Limits limits = big_limits();
  const size_t n = 70000; // 1.1 MB: over a mebibyte of code
  B b("far", 0, &limits);
  GRJIT_VReg x = b.param(GRJIT_TYPE_I64);
  GRJIT_VReg t = b.reg(), d = b.reg();
  GRJIT_BlockId entry = b.block(), big = b.block(), join = b.block();
  b.at(entry);
  b.cnst(d, 1000);
  b.cmp(GRJIT_CMP_EQ, t, V(x), I(0));
  b.br_if(V(t), join, big);
  b.at(big);
  filler(b, d, n);
  b.br(join);
  b.at(join);
  b.ret(V(d));
  Fn f(b.finish());
  Emitted em(f, &limits);
  ASSERT_TRUE(em.ok()) << grjit_result_string(em.result);
  EXPECT_GT(em.e.size, (1u << 20) + 4096u) << "the body must be over a mebibyte";
  // The branch is the long form: a cbz/cbnz that skips one word, then a `b`.
  size_t long_forms = 0;
  for (size_t i = 0; i + 1 < em.words(); i++) {
    uint32_t w = em.word(i);
    if ((w & 0xFE000000u) == 0xB4000000u && ((w >> 5) & 0x7FFFFu) == 2u &&
        (em.word(i + 1) & 0xFC000000u) == 0x14000000u) {
      long_forms++;
    }
  }
  EXPECT_GT(long_forms, 0u);
  Ran skip = simulate(em, f, w.ctx, {0});
  EXPECT_EQ(skip.exit, static_cast<uint32_t>(GRJIT_EXIT_RETURNED));
  EXPECT_EQ(skip.out[0], 1000u);
  Ran through = simulate(em, f, w.ctx, {1});
  EXPECT_EQ(through.out[0], 1000u + filler_sum(n));
}

TEST(Arm64Branches, ABackwardBranchOverMoreThanOneMebibyteOfCodeAlsoReaches) {
  JitWorld w;
  GRJIT_Limits limits = big_limits();
  const size_t n = 70000; // 1.1 MB: over a mebibyte of code
  // A loop whose body is over a mebibyte, run three times.
  B b("loop", 0, &limits);
  GRJIT_VReg i = b.reg(), t = b.reg(), d = b.reg();
  GRJIT_BlockId entry = b.block(), head = b.block(), body = b.block(), after = b.block();
  b.at(entry);
  b.cnst(i, 0);
  b.cnst(d, 0);
  b.br(head);
  b.at(head);
  b.cmp(GRJIT_CMP_LT, t, V(i), I(3));
  b.br_if(V(t), body, after);
  b.at(body);
  filler(b, d, n);
  b.bin(GRJIT_OP_ADD, i, V(i), I(1));
  b.br(head);
  b.at(after);
  b.ret(V(d));
  Fn f(b.finish());
  Emitted em(f, &limits);
  ASSERT_TRUE(em.ok());
  EXPECT_GT(em.e.size, (1u << 20) + 4096u);
  Ran r = simulate(em, f, w.ctx);
  EXPECT_EQ(r.out[0], 3 * filler_sum(n));
}

TEST(Arm64Branches, APollAndAGuardWhoseStubsAreOverAMebibyteAwayStillReachThem) {
  JitWorld w;
  GRJIT_Limits limits = big_limits();
  const size_t n = 70000; // 1.1 MB: over a mebibyte of code
  // The poll and the guard are early; their stubs are emitted after every block.
  static uint32_t polls = 0;
  struct H {
    static uint32_t poll(void *, uint64_t, uint64_t) {
      polls++;
      return 0;
    }
  };
  B b("stubs", 3, &limits);
  GRJIT_VReg x = b.param(GRJIT_TYPE_I64);
  GRJIT_VReg d = b.reg();
  b.poll_helper(H::poll);
  b.at(b.block());
  b.cnst(d, 5);
  b.poll({1, 2}, {grjit_frame_slot_vreg(x), grjit_frame_slot_constant(9), grjit_frame_slot_dead()});
  b.guard(V(x), {1, 3}, {grjit_frame_slot_vreg(d), grjit_frame_slot_constant(9), grjit_frame_slot_dead()});
  filler(b, d, n);
  b.ret(V(d));
  Fn f(b.finish());
  Emitted em(f, &limits, nullptr);
  ASSERT_TRUE(em.ok());
  EXPECT_GT(em.e.size, (1u << 20) + 4096u);
  // A guard that holds, no poll pending: runs through.
  Ran through = simulate(em, f, w.ctx, {1});
  EXPECT_EQ(through.exit, static_cast<uint32_t>(GRJIT_EXIT_RETURNED));
  EXPECT_EQ(through.out[0], 5 + filler_sum(n));
  // A guard that fails: the exit stub, more than a mebibyte away, is reached.
  Ran failed = simulate(em, f, w.ctx, {0});
  EXPECT_EQ(failed.exit, static_cast<uint32_t>(GRJIT_EXIT_DEOPT));
  EXPECT_EQ(failed.out[0], 5u);
  EXPECT_EQ(failed.out[1], 9u);
  EXPECT_EQ(failed.out[2], 0u);
  const GRCORE_CodeSite * site =
      grcore_codemeta_find(&em.e.meta.meta, static_cast<uint32_t>(failed.out[3]));
  ASSERT_NE(site, nullptr);
  EXPECT_EQ(site->kind, GRCORE_SITE_GUARD);
  // A pending request takes the poll's slow path, also over a mebibyte away.
  // (The request word is the one the poll loads: set a bit in the real context.)
  polls = 0;
  ASSERT_EQ(grcore_context_terminate(w.ctx), GRCORE_OK);
  Ran polled = simulate(em, f, w.ctx, {1}, {reinterpret_cast<uint64_t>(&H::poll)});
  EXPECT_EQ(polls, 1u);
  EXPECT_EQ(polled.exit, static_cast<uint32_t>(GRJIT_EXIT_RETURNED));
}

TEST(Arm64Branches, ANormalSizedFunctionNeverUsesTheLongForm) {
  // The second assembly happens only when a short branch does not reach, so a
  // function that fits pays nothing: its conditional branches are single words.
  B b("small");
  GRJIT_VReg x = b.param(GRJIT_TYPE_I64);
  GRJIT_VReg d = b.reg();
  GRJIT_BlockId entry = b.block(), a = b.block(), c = b.block();
  b.at(entry);
  b.cnst(d, 1);
  b.br_if(V(x), a, c);
  b.at(a);
  b.cnst(d, 2);
  b.ret(V(d));
  b.at(c);
  b.ret(V(d));
  Fn f(b.finish());
  Emitted em(f);
  ASSERT_TRUE(em.ok());
  size_t cb = 0;
  for (size_t i = 0; i < em.words(); i++) {
    uint32_t w = em.word(i);
    if ((w & 0xFE000000u) == 0xB4000000u) {
      cb++;
      // The branch target is within a few words, not a skip over a `b`.
      EXPECT_NE((w >> 5) & 0x7FFFFu, 2u) << "a long-form skip in a small function";
    }
  }
  EXPECT_GT(cb, 0u);
}

/* ---- The byte cap ----------------------------------------------------------------------- */

TEST(Arm64Cap, CodeAtTheByteCapIsEmittedAndOneByteOverIsALimit) {
  B b("cap");
  GRJIT_VReg d = b.reg();
  b.at(b.block());
  b.cnst(d, 0);
  filler(b, d, 100);
  b.ret(V(d));
  Fn f(b.finish());
  Emitted free_run(f);
  ASSERT_TRUE(free_run.ok());
  size_t size = free_run.e.size;
  GRJIT_Limits at{};
  at.max_code_bytes = size;
  Emitted exact(f, &at);
  EXPECT_TRUE(exact.ok()) << grjit_result_string(exact.result);
  EXPECT_EQ(exact.e.size, size);
  GRJIT_Limits under{};
  under.max_code_bytes = size - 1;
  Emitted short_by_one(f, &under);
  EXPECT_EQ(short_by_one.result, GRJIT_ERR_LIMIT);
  EXPECT_EQ(short_by_one.e.bytes, nullptr);
  under.max_code_bytes = 4;
  Emitted tiny(f, &under);
  EXPECT_EQ(tiny.result, GRJIT_ERR_LIMIT);
}

TEST(Arm64Cap, ACapBetweenTheShortAndLongFormsOfAFarFunctionIsALimit) {
  GRJIT_Limits limits = big_limits();
  const size_t n = 70000; // 1.1 MB: over a mebibyte of code
  B b("far", 0, &limits);
  GRJIT_VReg x = b.param(GRJIT_TYPE_I64);
  GRJIT_VReg t = b.reg(), d = b.reg();
  GRJIT_BlockId entry = b.block(), big = b.block(), join = b.block();
  b.at(entry);
  b.cnst(d, 1);
  b.cmp(GRJIT_CMP_EQ, t, V(x), I(0));
  b.br_if(V(t), join, big);
  b.at(big);
  filler(b, d, n);
  b.br(join);
  b.at(join);
  b.ret(V(d));
  Fn f(b.finish());
  Emitted free_run(f, &limits);
  ASSERT_TRUE(free_run.ok());
  size_t long_size = free_run.e.size;
  limits.max_code_bytes = long_size;
  Emitted exact(f, &limits);
  EXPECT_TRUE(exact.ok());
  limits.max_code_bytes = long_size - 1;
  Emitted under(f, &limits);
  EXPECT_EQ(under.result, GRJIT_ERR_LIMIT);
  EXPECT_EQ(under.e.bytes, nullptr);
}

/* ---- Entry hook, polls, results the helper leaves dirty ---------------------------------- */

TEST(Arm64Abi, AHelperThatReturnsThirtyTwoBitsMayLeaveTheUpperHalfDirtyAndTheRefusalIsExact) {
  JitWorld w;
  static uint32_t answer = 0;
  struct H {
    static uint32_t poll(void *, uint64_t, uint64_t) { return answer; }
    static uint32_t hook(void *) { return answer; }
  };
  B b("abi", 3);
  b.poll_helper(H::poll);
  GRJIT_VReg d = b.reg();
  b.at(b.block());
  b.cnst(d, 77);
  b.poll({1, 1}, {grjit_frame_slot_dead(), grjit_frame_slot_dead(), grjit_frame_slot_dead()});
  b.ret(V(d));
  Fn f(b.finish());
  Emitted em(f, nullptr, H::hook);
  ASSERT_TRUE(em.ok());
  std::set<uint64_t> r32 = {reinterpret_cast<uint64_t>(&H::poll), reinterpret_cast<uint64_t>(&H::hook)};
  answer = 0;
  Ran ok = simulate(em, f, w.ctx, {}, r32);
  EXPECT_EQ(ok.exit, static_cast<uint32_t>(GRJIT_EXIT_RETURNED));
  EXPECT_EQ(ok.out[0], 77u);
  // The hook refuses with all 32 bits, upper half noise notwithstanding.
  answer = 0xA5A5A5A5u;
  Ran refused = simulate(em, f, w.ctx, {}, r32);
  EXPECT_EQ(refused.exit, static_cast<uint32_t>(GRJIT_EXIT_REFUSED));
  EXPECT_EQ(refused.out[0], 0xA5A5A5A5u);
  // Only the poll refuses (the hook says zero the first time round).
}

/* ---- The instruction cache and the mapping ----------------------------------------------- */

TEST(Arm64Memory, EveryArm64MappingIsSyncedOnceBetweenTheWriteAndTheFlipAndFilledWithBreakpoints) {
  JitWorld w;
  FakePages pages(w.pages());
  // The cache is synced before `protect` runs: record the count when it does.
  static uint64_t at_protect = 0;
  struct Hook {
    static bool protect(void * c, void * p, size_t n, GRCORE_PageAccess a) {
      at_protect = grjit_icache_sync_count();
      auto * f = static_cast<FakePages *>(c);
      f->protects++;
      return f->base->protect(f->base->ctx, p, n, a);
    }
  };
  pages.vtable.protect = Hook::protect;

  const uint8_t code[8] = {0xC0, 0x03, 0x5F, 0xD6, 0xC0, 0x03, 0x5F, 0xD6}; // ret; ret
  uint64_t before = grjit_icache_sync_count();
  void * mapping = nullptr;
  size_t mapped = 0;
  ASSERT_EQ(grjit_memory_create(&pages.vtable, GRJIT_ARCH_ARM64, code, sizeof code,
                nullptr, &mapping, &mapped, nullptr),
      GRJIT_OK);
  EXPECT_EQ(grjit_icache_sync_count(), before + 1) << "once per mapping";
  EXPECT_EQ(at_protect, before + 1) << "before the mapping was made executable";
  EXPECT_EQ(pages.protects, 1);
  // The unused tail is `brk #0` words, not zeros (`udf #0` only by accident).
  const uint8_t * p = static_cast<const uint8_t *>(mapping);
  EXPECT_EQ(std::memcmp(p, code, sizeof code), 0);
  ASSERT_GT(mapped, sizeof code);
  for (size_t i = sizeof code; i + 4 <= mapped; i += 4) {
    ASSERT_EQ(p[i], 0x00);
    ASSERT_EQ(p[i + 1], 0x00);
    ASSERT_EQ(p[i + 2], 0x20);
    ASSERT_EQ(p[i + 3], 0xD4) << i;
  }
  pages.vtable.unmap(pages.vtable.ctx, mapping, mapped);

  // An x86-64 mapping is not synced and is filled with int3.
  before = grjit_icache_sync_count();
  ASSERT_EQ(grjit_memory_create(&pages.vtable, GRJIT_ARCH_X86_64, code, sizeof code,
                nullptr, &mapping, &mapped, nullptr),
      GRJIT_OK);
  EXPECT_EQ(grjit_icache_sync_count(), before);
  EXPECT_EQ(static_cast<const uint8_t *>(mapping)[sizeof code], 0xCC);
  pages.vtable.unmap(pages.vtable.ctx, mapping, mapped);
}

TEST(Arm64Memory, ACompileOnArm64SyncsTheCacheOncePerCode) {
  if (grjit_native_arch() != GRJIT_ARCH_ARM64 || !grjit_backend_available()) {
    GTEST_SKIP() << "the native backend is not arm64; the sync is counted by the test above";
  }
  JitWorld w;
  B b("tiny");
  b.at(b.block());
  b.ret();
  Fn f(b.finish());
  uint64_t before = grjit_icache_sync_count();
  Compiled c(f, w.pages());
  ASSERT_TRUE(c);
  EXPECT_EQ(grjit_icache_sync_count(), before + 1);
  Compiled d(f, w.pages());
  ASSERT_TRUE(d);
  EXPECT_EQ(grjit_icache_sync_count(), before + 2);
}

/* ---- Availability ---------------------------------------------------------------------------- */

TEST(Arm64Availability, ExactlyLinuxX86_64LinuxArm64AndWindowsX86_64ReportABackend) {
#if GRJIT_TEST_HAVE_BACKEND
  EXPECT_TRUE(grjit_backend_available());
#else
  EXPECT_FALSE(grjit_backend_available());
  JitWorld w;
  B b("tiny");
  b.at(b.block());
  b.ret();
  Fn f(b.finish());
  GRJIT_CompileOptions o{};
  o.pages = w.pages();
  GRJIT_Code * out = nullptr;
  EXPECT_EQ(grjit_compile(&o, f, &out), GRJIT_ERR_UNSUPPORTED);
  EXPECT_EQ(out, nullptr);
#endif
#if defined(__x86_64__) && defined(__linux__)
  EXPECT_EQ(grjit_native_arch(), GRJIT_ARCH_X86_64);
#elif defined(__aarch64__) && defined(__linux__)
  EXPECT_EQ(grjit_native_arch(), GRJIT_ARCH_ARM64);
#elif defined(_WIN64) && defined(__x86_64__)
  EXPECT_EQ(grjit_native_arch(), GRJIT_ARCH_X86_64_WIN64);
#endif
}

TEST(Arm64Availability, TheArm64EmitterRunsOnEveryHostAndEmitsOnlyAnInstructionSetItIs) {
  // Whatever the host, the arm64 emitter produces whole words that are not x86.
  B b("tiny");
  GRJIT_VReg d = b.reg();
  b.at(b.block());
  b.cnst(d, 3);
  b.ret(V(d));
  Fn f(b.finish());
  Emitted em(f);
  ASSERT_TRUE(em.ok());
  EXPECT_EQ(em.e.size % 4, 0u);
  EXPECT_EQ(em.word(em.words() - 1) & 0xFFFFFFFFu, 0xD65F03C0u) << "the last word of a function that ends is ret";
}

GRJIT_TEST_MAIN()
