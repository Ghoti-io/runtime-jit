/**
 * @file
 *
 * The arm64 half of spec-runtime-float's story 5 that is specific to one ABI: the test-only AArch64
 * simulator's own tests (tests/a64_sim.h), and, in the commits that follow, the structural scans of
 * the registers the assembler encoded, the AAPCS64 helper probe, the junk-above-bit-31 float
 * result, the callee-saved `d8`-`d15` sentinels and the `fpcr` read.
 *
 * The simulator is written by the same author as the emitter, so it can share a misreading of the
 * manual with it. It is a dev loop and an extra catch on every host, not a verdict: the instruments
 * that count for arm64 are qemu-aarch64 and the Raspberry Pi. So these tests hold it to hand-worked
 * answers (taken from the Arm Architecture Reference Manual's pseudocode, worked on paper) and, where
 * the host's own arithmetic must agree, to `<cmath>` and to the host's float conversions.
 *
 * Nothing here executes AArch64 code on the host; the simulator interprets the bytes the assembler
 * made.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include "test_helpers.h"

#include "../a64_sim.h"
#include "../../src/arm64/asm_internal.h"

#include <cmath>
#include <cstring>
#include <limits>
#include <random>

namespace {

using a64sim::Cpu;
using a64sim::Config;
using a64sim::Result;

uint64_t bits(double d) {
  uint64_t b;
  std::memcpy(&b, &d, 8);
  return b;
}
uint64_t bits(float f) {
  uint32_t b;
  std::memcpy(&b, &f, 4);
  return b;
}
double dbl(uint64_t b) {
  double d;
  std::memcpy(&d, &b, 8);
  return d;
}
float flt(uint64_t b) {
  uint32_t w = static_cast<uint32_t>(b);
  float f;
  std::memcpy(&f, &w, 4);
  return f;
}

constexpr double kInf = std::numeric_limits<double>::infinity();
constexpr uint64_t kQnan64 = 0x7FF8000000000000ull;
constexpr uint64_t kQnan32 = 0x7FC00000ull;

/* A program: assembled with the library's assembler, ended with `ret`, run from `x0..x2` and
 * `d0..d7`. */
struct Prog {
  GRJIT_A64Asm a;
  Prog() { grjit_a64_init(&a, grjit_allocator_default(), 1u << 20); }
  ~Prog() { grjit_a64_free(&a); }
  Prog(const Prog &) = delete;
  Prog & operator=(const Prog &) = delete;
  Result run(Config & cfg) {
    grjit_a64_ret(&a);
    grjit_a64_finish(&a);
    EXPECT_EQ(grjit_a64_status(&a), GRJIT_A64_OK);
    cfg.code = grjit_a64_bytes(&a);
    cfg.size = grjit_a64_size(&a);
    Cpu cpu;
    return cpu.run(cfg);
  }
  Result run(uint64_t x0 = 0, uint64_t x1 = 0, uint64_t x2 = 0) {
    Config cfg;
    cfg.args[0] = x0;
    cfg.args[1] = x1;
    cfg.args[2] = x2;
    return run(cfg);
  }
};

using Reg = GRJIT_A64Reg;
constexpr Reg X0 = GRJIT_A64_X0, X1 = GRJIT_A64_X1, X2 = GRJIT_A64_X2, X3 = GRJIT_A64_X3;

/* One three-register integer instruction on x1 and x2, the result in x0. */
uint64_t alu(GRJIT_A64Alu op, bool wide, uint64_t a, uint64_t b) {
  Prog p;
  if (wide) {
    grjit_a64_alu(&p.a, op, X0, X1, X2);
  } else {
    grjit_a64_alu32(&p.a, op, X0, X1, X2);
  }
  Result r = p.run(0xDEADBEEFDEADBEEFull, a, b);
  EXPECT_TRUE(r.ok) << r.error;
  return r.x0;
}

/* The scalar binary operation on d1 and d2 (or s1 and s2), the result's bits in d0. */
uint64_t fbin(GRJIT_A64FpBinary op, bool dbl_, uint64_t a, uint64_t b) {
  Prog p;
  const GRJIT_A64FSize sz = dbl_ ? GRJIT_A64_DOUBLE : GRJIT_A64_SINGLE;
  grjit_a64_fmov_from_gpr(&p.a, sz, GRJIT_A64_V1, X1);
  grjit_a64_fmov_from_gpr(&p.a, sz, GRJIT_A64_V2, X2);
  grjit_a64_fp_binary(&p.a, op, sz, GRJIT_A64_V0, GRJIT_A64_V1, GRJIT_A64_V2);
  Result r = p.run(0, a, b);
  EXPECT_TRUE(r.ok) << r.error;
  return r.d0;
}

uint64_t fun(GRJIT_A64FpUnary op, bool dbl_, uint64_t a) {
  Prog p;
  const GRJIT_A64FSize sz = dbl_ ? GRJIT_A64_DOUBLE : GRJIT_A64_SINGLE;
  grjit_a64_fmov_from_gpr(&p.a, sz, GRJIT_A64_V1, X1);
  grjit_a64_fp_unary(&p.a, op, sz, GRJIT_A64_V0, GRJIT_A64_V1);
  Result r = p.run(0, a);
  EXPECT_TRUE(r.ok) << r.error;
  return r.d0;
}

/* `fcmp` then `cset w0, cond` for each of the six conditions the emitter uses for IR comparisons:
 * eq, ne, mi (lt), ls (le), gt, ge. The result packs the six into bits 0-5. */
unsigned fcmp_conditions(bool dbl_, uint64_t a, uint64_t b) {
  Prog p;
  const GRJIT_A64FSize sz = dbl_ ? GRJIT_A64_DOUBLE : GRJIT_A64_SINGLE;
  grjit_a64_fmov_from_gpr(&p.a, sz, GRJIT_A64_V1, X1);
  grjit_a64_fmov_from_gpr(&p.a, sz, GRJIT_A64_V2, X2);
  grjit_a64_fcmp(&p.a, sz, GRJIT_A64_V1, GRJIT_A64_V2);
  const GRJIT_A64Cond conds[6] = {GRJIT_A64_EQ, GRJIT_A64_NE, GRJIT_A64_MI, GRJIT_A64_LS, GRJIT_A64_GT, GRJIT_A64_GE};
  grjit_a64_mov_ri(&p.a, X0, 0);
  for (unsigned i = 0; i < 6; i++) {
    // cset x3 then orr into x0 at bit i
    grjit_a64_cset(&p.a, X3, conds[i]);
    grjit_a64_mov_ri(&p.a, X2, i);
    grjit_a64_alu(&p.a, GRJIT_A64_LSLV, X3, X3, X2);
    grjit_a64_alu(&p.a, GRJIT_A64_ORR, X0, X0, X3);
  }
  Result r = p.run(0, a, b);
  EXPECT_TRUE(r.ok) << r.error;
  return static_cast<unsigned>(r.x0);
}

enum Cond { kEq = 1, kNe = 2, kLt = 4, kLe = 8, kGt = 16, kGe = 32 };

} // namespace

/* ---- The integer forms ---------------------------------------------------------------- */

TEST(Arm64Sim, TheThirtyTwoBitFormsComputeInThirtyTwoBitsAndClearTheUpperHalf) {
  // The inputs have garbage above bit 31 in x1/x2: a w form must ignore it and clear x0's.
  const uint64_t hi = 0xABCD000000000000ull;
  EXPECT_EQ(alu(GRJIT_A64_ADD, false, hi | 0xFFFFFFFFu, hi | 1), 0u);
  EXPECT_EQ(alu(GRJIT_A64_ADD, true, 0xFFFFFFFFu, 1), 0x100000000ull);
  EXPECT_EQ(alu(GRJIT_A64_SUB, false, 0, 1), 0xFFFFFFFFu);
  EXPECT_EQ(alu(GRJIT_A64_MUL, false, 0x10000, 0x10000), 0u) << "2^32 wraps to zero in 32 bits";
  EXPECT_EQ(alu(GRJIT_A64_MUL, true, 0x10000, 0x10000), 0x100000000ull);
  EXPECT_EQ(alu(GRJIT_A64_AND, false, hi | 0xF0F0u, 0xFF00u), 0xF000u);
  EXPECT_EQ(alu(GRJIT_A64_ORR, false, hi | 1u, 2u), 3u);
  EXPECT_EQ(alu(GRJIT_A64_EOR, false, 0xFFu, hi | 0x0Fu), 0xF0u);
  EXPECT_EQ(alu(GRJIT_A64_BIC, false, 0xFFFFu, 0x00F0u), 0xFF0Fu);
  EXPECT_EQ(alu(GRJIT_A64_BIC, true, ~0ull, 1), ~1ull);
}

TEST(Arm64Sim, ShiftsAndRotatesTakeTheCountModuloTheWidthOfTheForm) {
  EXPECT_EQ(alu(GRJIT_A64_LSLV, false, 1, 33), 2u) << "w: 33 mod 32 is 1";
  EXPECT_EQ(alu(GRJIT_A64_LSLV, true, 1, 33), 1ull << 33);
  EXPECT_EQ(alu(GRJIT_A64_LSRV, false, 0x80000000u, 31), 1u);
  EXPECT_EQ(alu(GRJIT_A64_ASRV, false, 0x80000000u, 31), 0xFFFFFFFFu) << "sign-fill in 32 bits, upper half clear";
  EXPECT_EQ(alu(GRJIT_A64_ASRV, true, 0x8000000000000000ull, 63), ~0ull);
  EXPECT_EQ(alu(GRJIT_A64_RORV, false, 0x80000001u, 1), 0xC0000000u);
  EXPECT_EQ(alu(GRJIT_A64_RORV, false, 0x12345678u, 0), 0x12345678u);
  EXPECT_EQ(alu(GRJIT_A64_RORV, false, 0x12345678u, 36), 0x81234567u) << "36 mod 32 is 4";
  EXPECT_EQ(alu(GRJIT_A64_RORV, true, 1, 1), 0x8000000000000000ull);
  EXPECT_EQ(alu(GRJIT_A64_RORV, true, 0x0123456789ABCDEFull, 68), 0xF0123456789ABCDEull) << "68 mod 64 is 4";
}

TEST(Arm64Sim, DivisionNeverTrapsAZeroDivisorGivesZeroAndMinOverMinusOneIsMin) {
  EXPECT_EQ(alu(GRJIT_A64_SDIV, false, 7, 0), 0u);
  EXPECT_EQ(alu(GRJIT_A64_UDIV, false, 7, 0), 0u);
  EXPECT_EQ(alu(GRJIT_A64_SDIV, true, 7, 0), 0u);
  EXPECT_EQ(alu(GRJIT_A64_SDIV, false, 0x80000000u, 0xFFFFFFFFu), 0x80000000u);
  EXPECT_EQ(alu(GRJIT_A64_SDIV, true, 0x8000000000000000ull, ~0ull), 0x8000000000000000ull);
  EXPECT_EQ(alu(GRJIT_A64_SDIV, false, static_cast<uint32_t>(-7), 2), static_cast<uint32_t>(-3)) << "truncating";
  EXPECT_EQ(alu(GRJIT_A64_UDIV, false, 0xFFFFFFFEu, 2), 0x7FFFFFFFu);
  EXPECT_EQ(alu(GRJIT_A64_SDIV, true, static_cast<uint64_t>(-9), 3), static_cast<uint64_t>(-3));
}

TEST(Arm64Sim, MsubGivesTheRemainderAndMinRemMinusOneIsZeroWithoutASpecialCase) {
  auto rem = [](bool wide, bool is_signed, uint64_t a, uint64_t b) {
    Prog p;
    if (wide) {
      grjit_a64_alu(&p.a, is_signed ? GRJIT_A64_SDIV : GRJIT_A64_UDIV, X3, X1, X2);
    } else {
      grjit_a64_alu32(&p.a, is_signed ? GRJIT_A64_SDIV : GRJIT_A64_UDIV, X3, X1, X2);
    }
    grjit_a64_msub(&p.a, X0, X3, X2, X1, wide);
    Result r = p.run(0, a, b);
    EXPECT_TRUE(r.ok) << r.error;
    return r.x0;
  };
  EXPECT_EQ(rem(false, true, static_cast<uint32_t>(-7), 3), static_cast<uint32_t>(-1));
  EXPECT_EQ(rem(false, true, 0x80000000u, 0xFFFFFFFFu), 0u) << "MIN % -1";
  EXPECT_EQ(rem(true, true, 0x8000000000000000ull, ~0ull), 0u);
  EXPECT_EQ(rem(false, false, 0xFFFFFFFFu, 10), 5u);
  EXPECT_EQ(rem(true, false, 100, 7), 2u);
}

TEST(Arm64Sim, CountLeadingZerosReverseBitsAndTheSignExtensionsWorkAtBothWidths) {
  auto bitsop = [](GRJIT_A64Bits op, bool wide, uint64_t a) {
    Prog p;
    grjit_a64_bits(&p.a, op, X0, X1, wide);
    Result r = p.run(0, a);
    EXPECT_TRUE(r.ok) << r.error;
    return r.x0;
  };
  EXPECT_EQ(bitsop(GRJIT_A64_CLZ, false, 0), 32u);
  EXPECT_EQ(bitsop(GRJIT_A64_CLZ, true, 0), 64u);
  EXPECT_EQ(bitsop(GRJIT_A64_CLZ, false, 0xFFFF000000000001ull), 31u) << "the w form reads only the low half";
  EXPECT_EQ(bitsop(GRJIT_A64_CLZ, true, 1), 63u);
  EXPECT_EQ(bitsop(GRJIT_A64_RBIT, false, 1), 0x80000000u);
  EXPECT_EQ(bitsop(GRJIT_A64_RBIT, true, 1), 0x8000000000000000ull);
  EXPECT_EQ(bitsop(GRJIT_A64_RBIT, false, 0), 0u);
  auto sext = [](GRJIT_A64Ext k, bool wide, uint64_t a) {
    Prog p;
    grjit_a64_sext(&p.a, k, X0, X1, wide);
    Result r = p.run(0, a);
    EXPECT_TRUE(r.ok) << r.error;
    return r.x0;
  };
  EXPECT_EQ(sext(GRJIT_A64_SXTB, false, 0x80), 0xFFFFFF80u) << "w: sign-extended in 32 bits, upper half clear";
  EXPECT_EQ(sext(GRJIT_A64_SXTB, true, 0x80), 0xFFFFFFFFFFFFFF80ull);
  EXPECT_EQ(sext(GRJIT_A64_SXTB, true, 0x17F), 0x7Full);
  EXPECT_EQ(sext(GRJIT_A64_SXTH, false, 0x8000), 0xFFFF8000u);
  EXPECT_EQ(sext(GRJIT_A64_SXTH, true, 0x18000), 0xFFFFFFFFFFFF8000ull);
  EXPECT_EQ(sext(GRJIT_A64_SXTW, true, 0x80000000u), 0xFFFFFFFF80000000ull);
  EXPECT_EQ(sext(GRJIT_A64_SXTW, true, 0x1FFFFFFFFull), 0xFFFFFFFFFFFFFFFFull);
  EXPECT_EQ(sext(GRJIT_A64_SXTW, true, 0x7FFFFFFFu), 0x7FFFFFFFull);
}

TEST(Arm64Sim, ANegationANotAndAThirtyTwoBitCompareAreTheWFormsAndSetTheFlagsOnThirtyTwoBits) {
  {
    Prog p;
    grjit_a64_neg32(&p.a, X0, X1);
    Result r = p.run(0, 0xAAAA000000000001ull);
    EXPECT_EQ(r.x0, 0xFFFFFFFFu);
  }
  {
    Prog p;
    grjit_a64_mvn32(&p.a, X0, X1);
    Result r = p.run(0, 0xAAAA00000000000Fu);
    EXPECT_EQ(r.x0, 0xFFFFFFF0u);
  }
  // cmp w1, w2 then cset w0, lt / lo: the upper halves differ and must not matter.
  auto cmp32 = [](GRJIT_A64Cond cond, uint64_t a, uint64_t b) {
    Prog p;
    grjit_a64_cmp32(&p.a, X1, X2);
    grjit_a64_cset(&p.a, X0, cond);
    Result r = p.run(0, a, b);
    EXPECT_TRUE(r.ok) << r.error;
    return r.x0;
  };
  EXPECT_EQ(cmp32(GRJIT_A64_LT, 0x1'0000'0000ull | 0xFFFFFFFFu, 1), 1u) << "-1 < 1 as signed 32-bit";
  EXPECT_EQ(cmp32(GRJIT_A64_LO, 0xFFFFFFFFu, 1), 0u) << "0xFFFFFFFF is not below 1 unsigned";
  EXPECT_EQ(cmp32(GRJIT_A64_EQ, 0x5'0000'0007ull, 0x9'0000'0007ull), 1u) << "equal in the low halves";
  EXPECT_EQ(cmp32(GRJIT_A64_GT, 0x80000000u, 0x7FFFFFFFu), 0u) << "INT_MIN is not above INT_MAX";
}

/* ---- Floating point: the arithmetic ------------------------------------------------------ */

TEST(Arm64Sim, ScalarArithmeticGivesTheWorkedAnswersInBothWidths) {
  EXPECT_EQ(fbin(GRJIT_A64_FADD, true, bits(1.5), bits(2.25)), bits(3.75));
  EXPECT_EQ(fbin(GRJIT_A64_FSUB, true, bits(1.5), bits(2.25)), bits(-0.75)) << "the operands in order: 1.5 - 2.25";
  EXPECT_EQ(fbin(GRJIT_A64_FMUL, true, bits(-3.0), bits(0.5)), bits(-1.5));
  EXPECT_EQ(fbin(GRJIT_A64_FDIV, true, bits(1.0), bits(8.0)), bits(0.125)) << "1 / 8, not 8 / 1";
  EXPECT_EQ(fbin(GRJIT_A64_FADD, false, bits(1.5f), bits(2.25f)), bits(3.75f));
  EXPECT_EQ(fbin(GRJIT_A64_FSUB, false, bits(1.5f), bits(2.25f)), bits(-0.75f));
  EXPECT_EQ(fbin(GRJIT_A64_FDIV, false, bits(1.0f), bits(8.0f)), bits(0.125f));
  // A single-precision result is the whole 64-bit register with the upper half clear.
  EXPECT_EQ(fbin(GRJIT_A64_FMUL, false, bits(-3.0f) | (0xFFFFull << 32), bits(0.5f)), bits(-1.5f))
      << "the upper half of the source register is not read, and the result's is clear";
  EXPECT_EQ(fbin(GRJIT_A64_FDIV, true, bits(1.0), bits(0.0)), bits(kInf)) << "a zero divisor is an infinity";
  EXPECT_EQ(fbin(GRJIT_A64_FDIV, true, bits(-1.0), bits(0.0)), bits(-kInf));
}

TEST(Arm64Sim, ANanOperandIsReturnedQuietenedAndAnyOtherNanIsThePositiveDefaultNan) {
  const uint64_t snan = 0x7FF0000000000001ull, qnan_payload = 0x7FF8000000000123ull;
  EXPECT_EQ(fbin(GRJIT_A64_FADD, true, snan, bits(1.0)), snan | (1ull << 51)) << "a signalling NaN comes back quiet, payload kept";
  EXPECT_EQ(fbin(GRJIT_A64_FADD, true, bits(1.0), qnan_payload), qnan_payload);
  EXPECT_EQ(fbin(GRJIT_A64_FADD, true, qnan_payload, snan), snan | (1ull << 51)) << "a signalling NaN is processed before a quiet one";
  EXPECT_EQ(fbin(GRJIT_A64_FADD, true, bits(kInf), bits(-kInf)), kQnan64) << "inf - inf is the default NaN, positive";
  EXPECT_EQ(fbin(GRJIT_A64_FMUL, true, bits(0.0), bits(kInf)), kQnan64);
  EXPECT_EQ(fbin(GRJIT_A64_FDIV, true, bits(0.0), bits(0.0)), kQnan64);
  EXPECT_EQ(fbin(GRJIT_A64_FDIV, false, bits(0.0f), bits(0.0f)), kQnan32);
  EXPECT_EQ(fun(GRJIT_A64_FSQRT, true, bits(-1.0)), kQnan64);
  EXPECT_EQ(fun(GRJIT_A64_FSQRT, true, bits(-0.0)), bits(-0.0)) << "sqrt(-0) is -0";
}

TEST(Arm64Sim, MinAndMaxOrderMinusZeroBelowPlusZeroAndAnyNanGivesANan) {
  EXPECT_EQ(fbin(GRJIT_A64_FMIN, true, bits(-0.0), bits(0.0)), bits(-0.0));
  EXPECT_EQ(fbin(GRJIT_A64_FMIN, true, bits(0.0), bits(-0.0)), bits(-0.0));
  EXPECT_EQ(fbin(GRJIT_A64_FMAX, true, bits(-0.0), bits(0.0)), bits(0.0));
  EXPECT_EQ(fbin(GRJIT_A64_FMAX, true, bits(0.0), bits(-0.0)), bits(0.0));
  EXPECT_EQ(fbin(GRJIT_A64_FMIN, false, bits(-0.0f), bits(0.0f)), bits(-0.0f));
  EXPECT_EQ(fbin(GRJIT_A64_FMAX, false, bits(-0.0f), bits(0.0f)), bits(0.0f));
  EXPECT_EQ(fbin(GRJIT_A64_FMIN, true, bits(1.0), bits(2.0)), bits(1.0));
  EXPECT_EQ(fbin(GRJIT_A64_FMAX, true, bits(1.0), bits(2.0)), bits(2.0));
  EXPECT_EQ(fbin(GRJIT_A64_FMIN, true, bits(-kInf), kQnan64), kQnan64) << "a quiet NaN is not ignored (that is fminnm)";
  EXPECT_EQ(fbin(GRJIT_A64_FMAX, true, kQnan64, bits(1.0)), kQnan64);
  EXPECT_EQ(fbin(GRJIT_A64_FMAX, false, bits(1.0f), kQnan32), kQnan32);
}

TEST(Arm64Sim, NegationAndAbsoluteValueTouchTheSignBitOnlyEvenOfASignallingNan) {
  const uint64_t snan = 0x7FF0000000000001ull;
  EXPECT_EQ(fun(GRJIT_A64_FNEG, true, snan), snan | (1ull << 63)) << "payload and the quiet bit are left alone";
  EXPECT_EQ(fun(GRJIT_A64_FABS, true, snan | (1ull << 63)), snan);
  EXPECT_EQ(fun(GRJIT_A64_FNEG, true, bits(0.0)), bits(-0.0));
  EXPECT_EQ(fun(GRJIT_A64_FNEG, false, bits(0.0f)), bits(-0.0f));
  EXPECT_EQ(fun(GRJIT_A64_FABS, false, bits(-2.5f)), bits(2.5f));
  EXPECT_EQ(fun(GRJIT_A64_FNEG, false, 0x7F800001ull), 0xFF800001ull) << "single: bit 31 only, bits 32-63 clear";
}

TEST(Arm64Sim, TheFourRoundingsAreToNearestEvenUpDownAndTowardZeroWithTheSignOfZeroKept) {
  struct Row { double x, n, p, m, z; };
  const Row rows[] = {
      {0.5, 0.0, 1.0, 0.0, 0.0},
      {1.5, 2.0, 2.0, 1.0, 1.0},
      {2.5, 2.0, 3.0, 2.0, 2.0},
      {-0.5, -0.0, -0.0, -1.0, -0.0},
      {-1.5, -2.0, -1.0, -2.0, -1.0},
      {-2.5, -2.0, -2.0, -3.0, -2.0},
      {-0.9, -1.0, -0.0, -1.0, -0.0},
      {4503599627370497.0, 4503599627370497.0, 4503599627370497.0, 4503599627370497.0, 4503599627370497.0},
      {-0.0, -0.0, -0.0, -0.0, -0.0},
  };
  for (const Row & r : rows) {
    EXPECT_EQ(fun(GRJIT_A64_FRINTN, true, bits(r.x)), bits(r.n)) << r.x;
    EXPECT_EQ(fun(GRJIT_A64_FRINTP, true, bits(r.x)), bits(r.p)) << r.x;
    EXPECT_EQ(fun(GRJIT_A64_FRINTM, true, bits(r.x)), bits(r.m)) << r.x;
    EXPECT_EQ(fun(GRJIT_A64_FRINTZ, true, bits(r.x)), bits(r.z)) << r.x;
  }
  EXPECT_EQ(fun(GRJIT_A64_FRINTN, false, bits(2.5f)), bits(2.0f));
  EXPECT_EQ(fun(GRJIT_A64_FRINTN, false, bits(8388609.0f)), bits(8388609.0f));
  EXPECT_EQ(fun(GRJIT_A64_FRINTP, true, bits(kInf)), bits(kInf));
  EXPECT_EQ(fun(GRJIT_A64_FRINTN, true, 0x7FF0000000000001ull), 0x7FF8000000000001ull) << "a signalling NaN comes back quiet";
}

/* ---- Comparison and conversion -------------------------------------------------------------- */

TEST(Arm64Sim, FcmpOrdersThePairAndAnUnorderedPairIsFalseForEqLtLeGtGeAndTrueForNe) {
  // The emitter maps the IR's EQ NE LT LE GT GE to eq ne mi ls gt ge: after `fcmp` an unordered pair
  // is NZCV = 0011, which makes the integer lt and le true and these false.
  for (bool d : {true, false}) {
    auto b = [&](double x) { return d ? bits(x) : bits(static_cast<float>(x)); };
    EXPECT_EQ(fcmp_conditions(d, b(1.0), b(2.0)), unsigned(kNe | kLt | kLe)) << d;
    EXPECT_EQ(fcmp_conditions(d, b(2.0), b(1.0)), unsigned(kNe | kGt | kGe)) << d;
    EXPECT_EQ(fcmp_conditions(d, b(2.0), b(2.0)), unsigned(kEq | kLe | kGe)) << d;
    EXPECT_EQ(fcmp_conditions(d, b(-0.0), b(0.0)), unsigned(kEq | kLe | kGe)) << d << ": -0 == +0";
    const uint64_t nan = d ? kQnan64 : kQnan32;
    EXPECT_EQ(fcmp_conditions(d, nan, b(1.0)), unsigned(kNe)) << d;
    EXPECT_EQ(fcmp_conditions(d, b(1.0), nan), unsigned(kNe)) << d;
    EXPECT_EQ(fcmp_conditions(d, nan, nan), unsigned(kNe)) << d;
    EXPECT_EQ(fcmp_conditions(d, b(INFINITY), b(-INFINITY)), unsigned(kNe | kGt | kGe)) << d;
  }
}

TEST(Arm64Sim, FcvtRoundsOnceNarrowingKeepsANansSignAndTopPayloadAndWideningIsExact) {
  auto fcvt = [](bool to_double, uint64_t a) {
    Prog p;
    grjit_a64_fmov_from_gpr(&p.a, to_double ? GRJIT_A64_SINGLE : GRJIT_A64_DOUBLE, GRJIT_A64_V1, X1);
    grjit_a64_fcvt(&p.a, to_double ? GRJIT_A64_DOUBLE : GRJIT_A64_SINGLE, GRJIT_A64_V0, GRJIT_A64_V1);
    Result r = p.run(0, a);
    EXPECT_TRUE(r.ok) << r.error;
    return r.d0;
  };
  EXPECT_EQ(fcvt(true, bits(0.1f)), bits(static_cast<double>(0.1f)));
  EXPECT_EQ(fcvt(false, bits(0.1)), bits(0.1f));
  EXPECT_EQ(fcvt(false, bits(1.0 + 0x1p-24)), bits(1.0f)) << "a tie to even";
  EXPECT_EQ(fcvt(false, bits(1.0 + 0x1p-24 + 0x1p-50)), bits(1.0f + 0x1p-23f)) << "just above a tie";
  EXPECT_EQ(fcvt(false, bits(1e300)), bits(INFINITY * 1.0f)) << "overflow is infinity";
  EXPECT_EQ(fcvt(true, 0x7F800001ull), 0x7FF8000020000000ull) << "a signalling single NaN: quiet, payload shifted up";
  EXPECT_EQ(fcvt(false, 0xFFF0000040000000ull), 0xFFC00002ull) << "sign kept, top 22 payload bits, made quiet";
  EXPECT_EQ(fcvt(true, bits(-0.0f)), bits(-0.0));
}

TEST(Arm64Sim, ConversionsFromIntegersRoundOnceAndThoseToIntegersTruncateAndSaturate) {
  auto scvtf = [](bool sgn, bool to_double, bool wide, uint64_t a) {
    Prog p;
    grjit_a64_cvt_to_fp(&p.a, sgn, to_double ? GRJIT_A64_DOUBLE : GRJIT_A64_SINGLE, GRJIT_A64_V0, X1, wide);
    Result r = p.run(0, a);
    EXPECT_TRUE(r.ok) << r.error;
    return r.d0;
  };
  EXPECT_EQ(scvtf(true, true, false, 0xFFFFFFFFu), bits(-1.0)) << "w signed: -1";
  EXPECT_EQ(scvtf(false, true, false, 0xFFFFFFFFu), bits(4294967295.0));
  EXPECT_EQ(scvtf(true, true, true, 0x8000000000000000ull), bits(-9223372036854775808.0));
  EXPECT_EQ(scvtf(false, true, true, 0xFFFFFFFFFFFFFFFFull), bits(18446744073709551616.0)) << "2^64 - 1 rounds up to 2^64";
  EXPECT_EQ(scvtf(false, true, true, 0x8000000000000001ull), bits(9223372036854775808.0));
  EXPECT_EQ(scvtf(false, true, true, (1ull << 53) + 1), bits(9007199254740992.0)) << "2^53 + 1: a tie to even";
  EXPECT_EQ(scvtf(false, false, true, 0xFFFFFFFFFFFFFFFFull), bits(18446744073709551616.0f));
  EXPECT_EQ(scvtf(true, false, false, 16777217), bits(16777216.0f)) << "2^24 + 1 in single: a tie to even";
  auto fcvtz = [](bool sgn, bool from_double, bool wide, uint64_t a) {
    Prog p;
    grjit_a64_fmov_from_gpr(&p.a, from_double ? GRJIT_A64_DOUBLE : GRJIT_A64_SINGLE, GRJIT_A64_V1, X1);
    grjit_a64_cvt_from_fp(&p.a, sgn, from_double ? GRJIT_A64_DOUBLE : GRJIT_A64_SINGLE, X0, GRJIT_A64_V1, wide);
    Result r = p.run(0, a);
    EXPECT_TRUE(r.ok) << r.error;
    return r.x0;
  };
  EXPECT_EQ(fcvtz(true, true, false, bits(-1.9)), 0xFFFFFFFFu) << "toward zero: -1";
  EXPECT_EQ(fcvtz(true, true, false, bits(3e9)), 0x7FFFFFFFu) << "saturates";
  EXPECT_EQ(fcvtz(true, true, false, bits(-3e9)), 0x80000000u);
  EXPECT_EQ(fcvtz(true, true, false, kQnan64), 0u) << "a NaN is 0";
  EXPECT_EQ(fcvtz(false, true, false, bits(-0.9)), 0u);
  EXPECT_EQ(fcvtz(false, true, false, bits(-5.0)), 0u) << "negative is 0 unsigned";
  EXPECT_EQ(fcvtz(false, true, false, bits(5e9)), 0xFFFFFFFFu);
  EXPECT_EQ(fcvtz(false, true, false, bits(4294967295.9)), 0xFFFFFFFFu);
  EXPECT_EQ(fcvtz(true, true, true, bits(9.3e18)), 0x7FFFFFFFFFFFFFFFull);
  EXPECT_EQ(fcvtz(true, true, true, bits(-9223372036854775808.0)), 0x8000000000000000ull);
  EXPECT_EQ(fcvtz(false, true, true, bits(1.8446744073709550e19)), 0xFFFFFFFFFFFFF800ull);
  EXPECT_EQ(fcvtz(false, true, true, bits(1.8446744073709552e19)), 0xFFFFFFFFFFFFFFFFull) << "2^64 saturates";
  EXPECT_EQ(fcvtz(false, true, true, bits(9223372036854775808.0)), 0x8000000000000000ull) << "2^63 unsigned";
  EXPECT_EQ(fcvtz(true, false, false, bits(-2147483648.0f)), 0x80000000u);
  EXPECT_EQ(fcvtz(true, false, true, bits(INFINITY * 1.0f)), 0x7FFFFFFFFFFFFFFFull);
}

TEST(Arm64Sim, FmovMovesTheBitsAndPopulationCountIsCntThenAddv) {
  {
    Prog p;
    grjit_a64_fmov_from_gpr(&p.a, GRJIT_A64_SINGLE, GRJIT_A64_V3, X1);
    grjit_a64_fmov_to_gpr(&p.a, GRJIT_A64_DOUBLE, X0, GRJIT_A64_V3);
    Result r = p.run(0, 0x1234567890ABCDEFull);
    EXPECT_EQ(r.x0, 0x90ABCDEFull) << "fmov s, w takes 32 bits and clears the rest of the register";
  }
  {
    Prog p;
    grjit_a64_fmov_from_gpr(&p.a, GRJIT_A64_DOUBLE, GRJIT_A64_V3, X1);
    grjit_a64_fmov_to_gpr(&p.a, GRJIT_A64_SINGLE, X0, GRJIT_A64_V3);
    Result r = p.run(0xFFFFFFFFFFFFFFFFull, 0x1234567890ABCDEFull);
    EXPECT_EQ(r.x0, 0x90ABCDEFull) << "fmov w, s clears the upper half of x";
  }
  auto popcnt = [](uint64_t a, bool wide) {
    Prog p;
    grjit_a64_fmov_from_gpr(&p.a, wide ? GRJIT_A64_DOUBLE : GRJIT_A64_SINGLE, GRJIT_A64_V0, X1);
    grjit_a64_cnt8b(&p.a, GRJIT_A64_V0, GRJIT_A64_V0);
    grjit_a64_addv8b(&p.a, GRJIT_A64_V0, GRJIT_A64_V0);
    grjit_a64_fmov_to_gpr(&p.a, GRJIT_A64_SINGLE, X0, GRJIT_A64_V0);
    Result r = p.run(0, a);
    EXPECT_TRUE(r.ok) << r.error;
    return r.x0;
  };
  EXPECT_EQ(popcnt(0, true), 0u);
  EXPECT_EQ(popcnt(~0ull, true), 64u);
  EXPECT_EQ(popcnt(~0ull, false), 32u) << "the s form reads 32 bits";
  EXPECT_EQ(popcnt(0x8000000000000001ull, true), 2u);
  EXPECT_EQ(popcnt(0xFFFFFFFF00000001ull, false), 1u);
}

/* ---- Memory ------------------------------------------------------------------------------- */

TEST(Arm64Sim, SAndDLoadsAndStoresUseAllThreeAddressingFormsAndASingleStoreWritesFourBytes) {
  alignas(16) static uint8_t big[16384];
  uint8_t * const mem = big + 8192;
  std::memset(big, 0xEE, sizeof big);
  const uint64_t v = 0x0102030405060708ull;
  std::memcpy(mem + 16, &v, 8);
  {
    // d: scaled load, unscaled load, register-offset load (an offset past ldur's reach goes through x17)
    Prog p;
    grjit_a64_load(&p.a, GRJIT_A64_MEM_F64, GRJIT_A64_VREG_AS_REG(GRJIT_A64_V4), X1, 16);
    grjit_a64_fmov_to_gpr(&p.a, GRJIT_A64_DOUBLE, X0, GRJIT_A64_V4);
    Result r = p.run(0, reinterpret_cast<uint64_t>(mem));
    EXPECT_EQ(r.x0, v);
  }
  {
    Prog p;
    grjit_a64_load(&p.a, GRJIT_A64_MEM_F64, GRJIT_A64_VREG_AS_REG(GRJIT_A64_V4), X1, -9); // ldur
    grjit_a64_fmov_to_gpr(&p.a, GRJIT_A64_DOUBLE, X0, GRJIT_A64_V4);
    Result r = p.run(0, reinterpret_cast<uint64_t>(mem + 25));
    EXPECT_EQ(r.x0, v);
  }
  {
    Prog p;
    grjit_a64_load(&p.a, GRJIT_A64_MEM_F64, GRJIT_A64_VREG_AS_REG(GRJIT_A64_V4), X1, -40000); // x17 form
    grjit_a64_fmov_to_gpr(&p.a, GRJIT_A64_DOUBLE, X0, GRJIT_A64_V4);
    Result r = p.run(0, reinterpret_cast<uint64_t>(mem + 16 + 40000));  // never dereferenced beyond mem + 16
    EXPECT_EQ(r.x0, v);
  }
  {
    // s: a load zero-extends into the register
    Prog p;
    grjit_a64_fmov_from_gpr(&p.a, GRJIT_A64_DOUBLE, GRJIT_A64_V4, X2);
    grjit_a64_load(&p.a, GRJIT_A64_MEM_F32, GRJIT_A64_VREG_AS_REG(GRJIT_A64_V4), X1, 16);
    grjit_a64_fmov_to_gpr(&p.a, GRJIT_A64_DOUBLE, X0, GRJIT_A64_V4);
    Result r = p.run(0, reinterpret_cast<uint64_t>(mem), ~0ull);
    EXPECT_EQ(r.x0, 0x05060708ull) << "ldr s clears bits 32-63 of d4";
  }
  {
    // a single store writes four bytes; a double store eight
    Prog p;
    grjit_a64_fmov_from_gpr(&p.a, GRJIT_A64_DOUBLE, GRJIT_A64_V5, X2);
    grjit_a64_store(&p.a, GRJIT_A64_MEM_F32, GRJIT_A64_VREG_AS_REG(GRJIT_A64_V5), X1, 32);
    grjit_a64_store(&p.a, GRJIT_A64_MEM_F64, GRJIT_A64_VREG_AS_REG(GRJIT_A64_V5), X1, 48);
    grjit_a64_add_imm(&p.a, X3, X1, 4095, false);
    grjit_a64_add_imm(&p.a, X3, X3, 1, false);
    grjit_a64_store(&p.a, GRJIT_A64_MEM_F32, GRJIT_A64_VREG_AS_REG(GRJIT_A64_V5), X3, -4096 + 40);
    Result r = p.run(0, reinterpret_cast<uint64_t>(mem), 0x1122334455667788ull);
    EXPECT_TRUE(r.ok) << r.error;
  }
  const uint8_t expect_single[8] = {0x88, 0x77, 0x66, 0x55, 0xEE, 0xEE, 0xEE, 0xEE};
  EXPECT_EQ(std::memcmp(mem + 32, expect_single, 8), 0) << "four bytes and no more";
  EXPECT_EQ(std::memcmp(mem + 40, expect_single, 4), 0) << "the store through x17 wrote the same four bytes";
  const uint8_t expect_double[8] = {0x88, 0x77, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11};
  EXPECT_EQ(std::memcmp(mem + 48, expect_double, 8), 0);
}

TEST(Arm64Sim, ALoadOfAnS8OrQRegisterOrAQuadwordFormStopsTheSimulator) {
  // ldr q0, [x1] (128-bit) and ldr b0, [x1] are vector forms the backend never emits.
  for (uint32_t w : {0x3DC00020u, 0x3D400020u, 0x7D400020u}) {
    Prog p;
    grjit_a64_word(&p.a, w);
    Result r = p.run(0, 0);
    EXPECT_FALSE(r.ok) << std::hex << w;
    EXPECT_NE(r.error.find("does not emit"), std::string::npos) << r.error;
  }
}

/* ---- Calls into the host ------------------------------------------------------------------ */

namespace {

double h_mix(uint64_t a, double b, uint64_t c, float d) {
  return static_cast<double>(a) * 1000.0 + b * 100.0 + static_cast<double>(c) * 10.0 + static_cast<double>(d);
}
float h_float_result(float x) { return x * 2.0f; }
uint32_t h_u32(uint64_t a) { return static_cast<uint32_t>(a) + 1u; }
double h_six_doubles(double a, double b, double c, double d, double e, double f) {
  return a + 2 * b + 3 * c + 4 * d + 5 * e + 6 * f;
}

a64sim::Sig sig(const char * args, char result) {
  a64sim::Sig s;
  s.args = args;
  s.result = result;
  return s;
}

struct Called {
  Result r;
  Config cfg;
};

/* A function that calls `fn` with what the entry registers hold, and returns. */
Called call_host(const void * fn, const a64sim::Sig & s, uint64_t x0, uint64_t x1, uint64_t x2,
    std::initializer_list<uint64_t> ds) {
  Prog p;
  grjit_a64_push_frame_record(&p.a);
  grjit_a64_mov_ri(&p.a, GRJIT_A64_X16, reinterpret_cast<uint64_t>(fn));
  grjit_a64_blr(&p.a, GRJIT_A64_X16);
  grjit_a64_pop_frame_record(&p.a);
  Called c;
  c.cfg.args[0] = x0;
  c.cfg.args[1] = x1;
  c.cfg.args[2] = x2;
  size_t i = 0;
  for (uint64_t d : ds) {
    c.cfg.d_args[i++] = d;
  }
  c.cfg.sigs[reinterpret_cast<uint64_t>(fn)] = s;
  c.r = p.run(c.cfg);
  return c;
}

} // namespace

TEST(Arm64Sim, AHelperWithMixedArgumentsTakesIntegersFromX0AndFloatsFromD0InOrderAmongThemselves) {
  if (!a64sim::kSignatures) {
    GTEST_SKIP() << "the simulator calls a host function with a signature only where the host's C ABI classifies as AAPCS64 does";
  }
  // (i64, f64, i64, f32) is x0, d0, x1, s1.
  Called c = call_host(reinterpret_cast<const void *>(h_mix), sig("idif", 'd'), 3, 7, 0,
      {bits(5.0), bits(0.5f) | (0xFFFFull << 32)});
  ASSERT_TRUE(c.r.ok) << c.r.error;
  EXPECT_EQ(c.r.d0, bits(3.0 * 1000 + 5.0 * 100 + 7.0 * 10 + 0.5)) << "a double result comes back whole in d0";
}

TEST(Arm64Sim, AFloatResultLeavesNoiseInBitsThirtyTwoToSixtyThreeOfD0AndTheCallerSavedVectorsAreNoise) {
  if (!a64sim::kSignatures) {
    GTEST_SKIP() << "no signatures on this host";
  }
  Called c = call_host(reinterpret_cast<const void *>(h_float_result), sig("f", 'f'), 0, 0, 0, {bits(1.25f)});
  ASSERT_TRUE(c.r.ok) << c.r.error;
  EXPECT_EQ(c.r.d0 & 0xFFFFFFFFull, bits(2.5f));
  EXPECT_NE(c.r.d0 >> 32, 0u) << "the upper half of d0 is not defined by the callee and is noise";
}

TEST(Arm64Sim, AnIntegerHelperThatReturnsThirtyTwoBitsLeavesNoiseAboveAndSixDoublesFillD0ToD5) {
  if (!a64sim::kSignatures) {
    GTEST_SKIP() << "no signatures on this host";
  }
  Called c = call_host(reinterpret_cast<const void *>(h_u32), sig("i", 'w'), 0xFFFFFFFFull, 0, 0, {});
  ASSERT_TRUE(c.r.ok) << c.r.error;
  EXPECT_EQ(c.r.x0 & 0xFFFFFFFFull, 0u);
  EXPECT_NE(c.r.x0 >> 32, 0u);
  Called six = call_host(reinterpret_cast<const void *>(h_six_doubles), sig("dddddd", 'd'), 0, 0, 0,
      {bits(1.0), bits(1.0), bits(1.0), bits(1.0), bits(1.0), bits(1.0)});
  ASSERT_TRUE(six.r.ok) << six.r.error;
  EXPECT_EQ(six.r.d0, bits(21.0));
}

/* ---- What the simulator checks of the code ------------------------------------------------ */

TEST(Arm64Sim, CodeThatChangesACalleeSavedVectorRegisterIsStoppedAtTheReturn) {
  for (GRJIT_A64VReg v : {GRJIT_A64_V8, GRJIT_A64_V12, GRJIT_A64_V15}) {
    Prog p;
    grjit_a64_fmov_from_gpr(&p.a, GRJIT_A64_DOUBLE, v, X1);
    Result r = p.run(0, 0x1234);
    EXPECT_FALSE(r.ok);
    EXPECT_NE(r.error.find("callee-saved d"), std::string::npos) << r.error;
  }
  // v16 and v7 are caller-saved: using them is fine.
  for (GRJIT_A64VReg v : {GRJIT_A64_V7, GRJIT_A64_V16, GRJIT_A64_V31}) {
    Prog p;
    grjit_a64_fmov_from_gpr(&p.a, GRJIT_A64_DOUBLE, v, X1);
    Result r = p.run(0, 0x1234);
    EXPECT_TRUE(r.ok) << r.error;
  }
}

TEST(Arm64Sim, AnInstructionTheBackendDoesNotEmitStopsTheSimulatorWhateverItIs) {
  struct Word { uint32_t w; const char * name; };
  const Word refused[] = {
      {0x1E264020u, "frinta d0, d1"},        // ties away: the planted defect's instruction, not an emitted one
      {0x1E27C020u, "frinti d0, d1"},        // follows FPCR
      {0x1E274020u, "frintx d0, d1"},
      {0x1E207820u, "fminnm s0, s1, s0"},
      {0x1F000000u, "fmadd s0, s0, s0, s0"},
      {0x1E202010u, "fcmpe s0, s0"},
      {0x1E240020u, "fcvtas w0, s1"},
      {0x1EE24000u, "a half-precision fcvt"},
      {0x13007C20u, "sxtw w0, w1"},
      {0x0E205820u | (1u << 30), "cnt v0.16b, v1.16b"},
      {0xD503201Fu, "nop"},
  };
  for (const Word & x : refused) {
    Prog p;
    grjit_a64_word(&p.a, x.w);
    Result r = p.run();
    EXPECT_FALSE(r.ok) << x.name;
    EXPECT_NE(r.error.find("does not emit"), std::string::npos) << x.name << ": " << r.error;
  }
}

TEST(Arm64Sim, TheNewFormsTouchingX18AreStopped) {
  for (const auto & emit : std::vector<std::function<void(GRJIT_A64Asm *)>>{
           [](GRJIT_A64Asm * a) { grjit_a64_alu32(a, GRJIT_A64_ADD, X0, GRJIT_A64_X18, X2); },
           [](GRJIT_A64Asm * a) { grjit_a64_alu(a, GRJIT_A64_SDIV, GRJIT_A64_X18, X1, X2); },
           [](GRJIT_A64Asm * a) { grjit_a64_msub(a, X0, X1, X2, GRJIT_A64_X18, true); },
           [](GRJIT_A64Asm * a) { grjit_a64_bits(a, GRJIT_A64_CLZ, X0, GRJIT_A64_X18, true); },
           [](GRJIT_A64Asm * a) { grjit_a64_sext(a, GRJIT_A64_SXTB, GRJIT_A64_X18, X1, false); },
           [](GRJIT_A64Asm * a) { grjit_a64_fmov_from_gpr(a, GRJIT_A64_DOUBLE, GRJIT_A64_V0, GRJIT_A64_X18); },
           [](GRJIT_A64Asm * a) { grjit_a64_fmov_to_gpr(a, GRJIT_A64_DOUBLE, GRJIT_A64_X18, GRJIT_A64_V0); },
           [](GRJIT_A64Asm * a) { grjit_a64_cvt_to_fp(a, true, GRJIT_A64_DOUBLE, GRJIT_A64_V0, GRJIT_A64_X18, true); },
           [](GRJIT_A64Asm * a) { grjit_a64_cvt_from_fp(a, true, GRJIT_A64_DOUBLE, GRJIT_A64_X18, GRJIT_A64_V0, true); },
       }) {
    Prog p;
    emit(&p.a);
    Result r = p.run();
    EXPECT_FALSE(r.ok);
    EXPECT_NE(r.error.find("x18"), std::string::npos) << r.error;
  }
}

/* ---- Against the host ---------------------------------------------------------------------- */

namespace {

bool same_bits_nan_equal(bool dbl_, uint64_t a, uint64_t b) {
  const a64sim::Fmt f = a64sim::Fmt::of(dbl_);
  return a == b || (f.is_nan(a) && f.is_nan(b));
}

uint64_t random_float_bits(std::mt19937_64 & g, bool dbl_) {
  static const double specials[] = {0.0, -0.0, 1.0, -1.0, 0.5, 2.5, 3.5, 1e-310, -1e-310, 4.9e-324,
      1.7976931348623157e308, 2.2250738585072014e-308, INFINITY, -INFINITY, 4503599627370496.0, 8388608.0};
  const unsigned pick = g() % 8;
  if (pick < 3) {
    const double x = specials[g() % (sizeof specials / sizeof *specials)];
    return dbl_ ? bits(x) : bits(static_cast<float>(x));
  }
  if (pick == 4) { // a NaN, either sign, any payload, quiet or signalling
    const uint64_t frac = 1 + g() % 0xFFFFF;
    const bool neg = (g() & 1) != 0;
    return dbl_ ? (0x7FF0000000000000ull | frac | (g() & 1 ? 0x0008000000000000ull : 0) | (neg ? 1ull << 63 : 0))
                : (0x7F800000ull | frac | (g() & 1 ? 0x400000ull : 0) | (neg ? 1ull << 31 : 0));
  }
  if (pick == 3) {
    return dbl_ ? bits(static_cast<double>(static_cast<int64_t>(g() % 2000) - 1000) / 8.0)
                : bits(static_cast<float>(static_cast<int64_t>(g() % 2000) - 1000) / 8.0f);
  }
  const uint64_t r = g();
  return dbl_ ? r : (r & 0xFFFFFFFFull);
}

} // namespace

TEST(Arm64Sim, ArithmeticAndRoundingAgreeWithTheHostOnRandomOperandsNansEqual) {
  std::mt19937_64 g(0xA64);
  size_t checked = 0, nans = 0;
  for (int i = 0; i < 4000; i++) {
    for (bool d : {true, false}) {
      const uint64_t a = random_float_bits(g, d), b = random_float_bits(g, d);
      const a64sim::Fmt f = a64sim::Fmt::of(d);
      nans += f.is_nan(a) + f.is_nan(b);
      auto host = [&](GRJIT_A64FpBinary op) -> uint64_t {
        volatile double x = f.to_double(a), y = f.to_double(b);
        volatile float fx = static_cast<float>(x), fy = static_cast<float>(y);
        if (d) {
          double r = op == GRJIT_A64_FADD ? x + y : op == GRJIT_A64_FSUB ? x - y : op == GRJIT_A64_FMUL ? x * y : x / y;
          return bits(r);
        }
        float r = op == GRJIT_A64_FADD ? fx + fy : op == GRJIT_A64_FSUB ? fx - fy : op == GRJIT_A64_FMUL ? fx * fy : fx / fy;
        return bits(r);
      };
      for (GRJIT_A64FpBinary op : {GRJIT_A64_FADD, GRJIT_A64_FSUB, GRJIT_A64_FMUL, GRJIT_A64_FDIV}) {
        ASSERT_TRUE(same_bits_nan_equal(d, fbin(op, d, a, b), host(op))) << "op " << op << " dbl " << d << " " << a << " " << b;
        checked++;
      }
      auto host_un = [&](GRJIT_A64FpUnary op) -> uint64_t {
        if (d) {
          double x = f.to_double(a);
          double r = op == GRJIT_A64_FSQRT ? std::sqrt(x) : op == GRJIT_A64_FRINTN ? std::nearbyint(x)
                   : op == GRJIT_A64_FRINTP ? std::ceil(x) : op == GRJIT_A64_FRINTM ? std::floor(x) : std::trunc(x);
          return bits(r);
        }
        float x = flt(a);
        float r = op == GRJIT_A64_FSQRT ? std::sqrt(x) : op == GRJIT_A64_FRINTN ? std::nearbyint(x)
                 : op == GRJIT_A64_FRINTP ? std::ceil(x) : op == GRJIT_A64_FRINTM ? std::floor(x) : std::trunc(x);
        return bits(r);
      };
      for (GRJIT_A64FpUnary op : {GRJIT_A64_FSQRT, GRJIT_A64_FRINTN, GRJIT_A64_FRINTP, GRJIT_A64_FRINTM, GRJIT_A64_FRINTZ}) {
        ASSERT_TRUE(same_bits_nan_equal(d, fun(op, d, a), host_un(op))) << "unary " << op << " dbl " << d << " " << a;
        checked++;
      }
    }
  }
  EXPECT_GT(checked, 50000u);
  EXPECT_GT(nans, 500u) << "the sweep reaches NaN operands";
}

TEST(Arm64Sim, ConversionsAgreeWithTheHostOnRandomOperands) {
  std::mt19937_64 g(0xC0DE);
  for (int i = 0; i < 3000; i++) {
    const uint64_t n = g();
    const int64_t s64 = static_cast<int64_t>(n >> (g() % 64));
    const uint64_t u64 = n >> (g() % 64);
    auto cvt = [&](bool sgn, bool to_double, bool wide, uint64_t a) {
      Prog p;
      grjit_a64_cvt_to_fp(&p.a, sgn, to_double ? GRJIT_A64_DOUBLE : GRJIT_A64_SINGLE, GRJIT_A64_V0, X1, wide);
      return p.run(0, a).d0;
    };
    ASSERT_EQ(cvt(true, true, true, static_cast<uint64_t>(s64)), bits(static_cast<double>(s64)));
    ASSERT_EQ(cvt(false, true, true, u64), bits(static_cast<double>(u64)));
    ASSERT_EQ(cvt(true, false, true, static_cast<uint64_t>(s64)), bits(static_cast<float>(s64)));
    ASSERT_EQ(cvt(false, false, true, u64), bits(static_cast<float>(u64)));
    ASSERT_EQ(cvt(true, true, false, static_cast<uint32_t>(s64)), bits(static_cast<double>(static_cast<int32_t>(s64))));
    ASSERT_EQ(cvt(false, false, false, static_cast<uint32_t>(u64)), bits(static_cast<float>(static_cast<uint32_t>(u64))));
    const double x = dbl(random_float_bits(g, true));
    if (std::isnan(x)) {
      continue;
    }
    auto trunc_s64 = [&](double v) -> int64_t {
      if (v >= 9223372036854775808.0) return INT64_MAX;
      if (v < -9223372036854775808.0) return INT64_MIN;
      return static_cast<int64_t>(v);
    };
    Prog p;
    grjit_a64_fmov_from_gpr(&p.a, GRJIT_A64_DOUBLE, GRJIT_A64_V1, X1);
    grjit_a64_cvt_from_fp(&p.a, true, GRJIT_A64_DOUBLE, X0, GRJIT_A64_V1, true);
    ASSERT_EQ(p.run(0, bits(x)).x0, static_cast<uint64_t>(trunc_s64(x))) << x;
  }
}

GRJIT_TEST_MAIN()
