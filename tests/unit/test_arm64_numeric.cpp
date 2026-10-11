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

#define FX_ASM_SENTINELS
#define FX_ASM_CLOBBER
#include "../calls_asm.h"
#include "test_helpers.h"

#include "../float_fixture.h"
#include "../sentinel_fixture.h"
#include "../a64_sim.h"
#include "../ir_eval.h"
#include "../../src/arm64/asm_internal.h"
#include "../../src/code/code_internal.h"

#include <cmath>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <map>
#include <random>
#include <unistd.h>

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

/* ---- What was encoded: no callee-saved vector register, for any instruction of the tables ----------
 *
 * Only the low 64 bits of v8-v15 are callee-saved under AAPCS64, and the compiled code does not use them at
 * all. The assembler records the vector registers it encodes; this asks the emitter for arm64, on any host,
 * for every instruction of the integer and float tables, the helper calls, and a function whose slots are
 * beyond the short offset forms, and reads the record. The record is also shown able to see: a planted use of
 * d8 (defect 86) and the assembler tests in test_asm_arm64.cpp that name a high register. */

namespace {

constexpr uint32_t kCalleeSavedV = 0x0000FF00u; // v8 .. v15

/* One instruction as an IR function, owned. */
struct Built {
  floatfx::FloatOp op;
  ~Built() { grjit_function_destroy(op.function); }
};

/* A function with a helper call of the given float argument pattern and result, so a register past v1 is used. */
GRJIT_Function * call_with_floats(size_t floats) {
  B b("callf", 0);
  std::vector<GRJIT_VReg> p;
  for (size_t i = 0; i < floats; i++) {
    p.push_back(b.param(i % 2 == 0 ? GRJIT_TYPE_F64 : GRJIT_TYPE_F32));
  }
  GRJIT_VReg r = b.reg(GRJIT_TYPE_F64);
  b.at(b.block());
  std::vector<GRJIT_Operand> args;
  for (GRJIT_VReg v : p) {
    args.push_back(V(v));
  }
  b.call(r, reinterpret_cast<const void *>(&h_six_doubles), args);
  b.ret(V(r));
  return b.finish();
}

/* n F64 registers, each assigned and all summed: slots far past the 32 an `ldur` reaches. */
GRJIT_Function * many_floats(size_t n) {
  B b("manyf", 0);
  GRJIT_VReg x = b.param(GRJIT_TYPE_F64);
  std::vector<GRJIT_VReg> r;
  for (size_t i = 0; i < n; i++) {
    r.push_back(b.reg(GRJIT_TYPE_F64));
  }
  GRJIT_VReg acc = b.reg(GRJIT_TYPE_F64);
  b.at(b.block());
  for (size_t i = 0; i < n; i++) {
    b.bin(GRJIT_OP_FADD, r[i], V(x), I(static_cast<int64_t>(floatfx::f64bits(static_cast<double>(i) + 0.25))));
  }
  b.mov(acc, I(static_cast<int64_t>(floatfx::f64bits(0.0))));
  for (size_t i = 0; i < n; i++) {
    b.bin(GRJIT_OP_FADD, acc, V(acc), V(r[i]));
  }
  b.ret(V(acc));
  return b.finish();
}

} // namespace

TEST(Arm64Registers, NoCalleeSavedVectorRegisterIsEncodedForAnyInstructionOfTheTablesAndTheScratchOnesAreAllUsed) {
  uint32_t seen = 0;
  size_t functions = 0;
  auto scan = [&](const GRJIT_Function * f, const std::string & what) {
    GRJIT_Emitted e;
    ASSERT_EQ(grjit_emit_for(GRJIT_ARCH_ARM64, f, grjit_allocator_default(), nullptr, nullptr, 0, &e), GRJIT_OK)
        << what;
    EXPECT_EQ(e.fp_regs_used & kCalleeSavedV, 0u) << what << ": a callee-saved vector register is encoded";
    EXPECT_EQ(e.fp_regs_used & ~uint32_t{0xFF}, 0u) << what << ": the emitter names v0 to v7 only";
    seen |= e.fp_regs_used;
    functions++;
    grjit_emitted_free(&e);
  };
  for (const std::string & instr : floatfx::every_float_instruction()) {
    Built b;
    b.op = floatfx::build_any_op(instr);
    ASSERT_NE(b.op.function, nullptr) << instr;
    scan(b.op.function, instr);
  }
  for (const char * ty : {"i32", "i64"}) {
    for (const std::string & instr : intfx::all_int_ops(ty)) {
      intfx::WasmOp op = intfx::build_wasm_op(instr);
      Fn f(op.function);
      scan(f, instr);
    }
  }
  for (size_t n = 0; n <= 6; n++) {
    Fn f(call_with_floats(n));
    scan(f, "a helper call with " + std::to_string(n) + " float arguments");
  }
  {
    Fn f(many_floats(120));
    scan(f, "120 float registers");
  }
  EXPECT_GT(functions, 140u);
  EXPECT_EQ(seen, 0x3Fu) << "v0 to v5 are what the emitter uses (six float arguments reach v5), each of them by something";
}

TEST(Arm64Registers, TheDisassemblerAgreesThatNoCalleeSavedVectorRegisterAppearsInAnyFloatFunction) {
  const char * tool = std::getenv("GRJIT_AARCH64_OBJDUMP");
  if (tool == nullptr || *tool == 0) {
    GTEST_SKIP() << "GRJIT_AARCH64_OBJDUMP is not set: the structural scan above reads the assembler's record; this "
                    "reads the bytes with the real disassembler, where suite/tools/xarch/jit-arm64.sh sets it";
  }
  auto has_callee_saved_vector = [](const std::string & text) {
    // a token b, h, s, d, q or v followed by 8 to 15 and not part of a longer word
    for (size_t i = 0; i + 1 < text.size(); i++) {
      const char c = text[i];
      if ((c == 'b' || c == 'h' || c == 's' || c == 'd' || c == 'q' || c == 'v') &&
          (i == 0 || !(std::isalnum(static_cast<unsigned char>(text[i - 1])) || text[i - 1] == '_'))) {
        size_t j = i + 1;
        unsigned n = 0, digits = 0;
        while (j < text.size() && std::isdigit(static_cast<unsigned char>(text[j])) && digits < 3) {
          n = n * 10 + static_cast<unsigned>(text[j] - '0');
          j++;
          digits++;
        }
        if (digits > 0 && n >= 8 && n <= 15 && (j == text.size() || !std::isalnum(static_cast<unsigned char>(text[j])))) {
          return true;
        }
      }
    }
    return false;
  };
  // The checker must be able to fail: it sees d8 and v12.8b, and not x29, #0x8 or 0xd8.
  EXPECT_TRUE(has_callee_saved_vector("fmov d8, d0"));
  EXPECT_TRUE(has_callee_saved_vector("cnt v12.8b, v0.8b"));
  EXPECT_FALSE(has_callee_saved_vector("ldur d0, [x29, #-8]; mov x0, #0xd8; mov w1, #8; fadd d1, d0, d7"));
  size_t checked = 0;
  for (const std::string & instr : floatfx::every_float_instruction()) {
    Built b;
    b.op = floatfx::build_any_op(instr);
    GRJIT_Emitted e;
    ASSERT_EQ(grjit_emit_for(GRJIT_ARCH_ARM64, b.op.function, grjit_allocator_default(), nullptr, nullptr, 0, &e),
        GRJIT_OK);
    char path[] = "/tmp/grjit-a64v-XXXXXX";
    int fd = mkstemp(path);
    ASSERT_GE(fd, 0);
    ASSERT_EQ(write(fd, e.bytes, e.size), static_cast<ssize_t>(e.size));
    close(fd);
    std::string cmd = std::string(tool) + " -D -b binary -m aarch64 --no-show-raw-insn " + path;
    FILE * pipe = popen(cmd.c_str(), "r");
    ASSERT_NE(pipe, nullptr);
    char line[512];
    std::string all;
    while (std::fgets(line, sizeof line, pipe) != nullptr) {
      std::string l(line);
      auto colon = l.find(":\t");
      if (colon != std::string::npos && l.back() != ':') {
        all += l.substr(colon + 2);
      }
    }
    EXPECT_EQ(pclose(pipe), 0);
    unlink(path);
    EXPECT_FALSE(has_callee_saved_vector(all)) << instr << ":\n" << all;
    EXPECT_FALSE(all.empty()) << instr << ": the disassembly read nothing";
    grjit_emitted_free(&e);
    checked++;
  }
  EXPECT_EQ(checked, 70u);
}

/* ---- Compiled functions run by the simulator, on any host -------------------------------------
 *
 * The simulator is given a stack full of a pattern, the callee's float result register with noise above bit 31 and
 * the caller-saved vector registers trashed after a call: an f32 stored with fewer bits than its word, a float
 * result taken whole, a helper argument in the wrong register each show. These are the tests defects 81, 84 and 85
 * are caught by on a host that cannot run arm64 code. */

namespace {

struct SimRun {
  uint32_t exit = 99;
  std::vector<uint64_t> out;
};

SimRun sim_function(const GRJIT_Function * f, JitWorld & w, std::vector<uint64_t> args,
    const std::map<uint64_t, a64sim::Sig> & sigs = {}) {
  SimRun r;
  GRJIT_Emitted e{};
  EXPECT_EQ(grjit_emit_for(GRJIT_ARCH_ARM64, f, grjit_allocator_default(), nullptr, nullptr,
                grcore_jit_layout()->request_word_offset, &e),
      GRJIT_OK);
  r.out.assign(grjit_function_interp_slot_count(f) + 1, 0xDEADBEEFu);
  args.resize(std::max<size_t>(args.size(), grjit_function_param_count(f)));
  Config cfg;
  cfg.code = e.bytes;
  cfg.size = e.size;
  cfg.stack_bytes = 1u << 18;
  cfg.args[0] = reinterpret_cast<uint64_t>(w.ctx);
  cfg.args[1] = reinterpret_cast<uint64_t>(args.data());
  cfg.args[2] = reinterpret_cast<uint64_t>(r.out.data());
  cfg.sigs = sigs;
  Cpu cpu;
  Result res = cpu.run(cfg);
  EXPECT_TRUE(res.ok) << res.error;
  r.exit = static_cast<uint32_t>(res.x0);
  grjit_emitted_free(&e);
  return r;
}

} // namespace

TEST(Arm64SimCompiled, AnF32ResultIsAWholeWordWithTheUpperHalfZeroOverAStackThatHeldGarbage) {
  JitWorld w;
  size_t checked = 0;
  for (const std::string & instr : floatfx::every_float_instruction()) {
    Built b;
    b.op = floatfx::build_any_op(instr);
    ASSERT_NE(b.op.function, nullptr) << instr;
    if (b.op.result != GRJIT_TYPE_F32) {
      continue;
    }
    SCOPED_TRACE(instr);
    for (uint64_t x : {uint64_t{0x3FC00000}, uint64_t{0x40490FDB}, uint64_t{0x80000000}, uint64_t{7}}) {
      SimRun r = sim_function(b.op.function, w, {x, 0x3F800000u});
      ASSERT_EQ(r.exit, uint32_t{GRJIT_EXIT_RETURNED});
      EXPECT_EQ(r.out[0] >> 32, 0u) << "bits 32-63 of an f32 result word";
      ireval::Result e = ireval::eval(b.op.function, w.ctx, nullptr, {x, 0x3F800000u});
      EXPECT_TRUE(ireval::same_float(32, r.out[0], e.out[0])) << std::hex << r.out[0] << " vs " << e.out[0];
      checked++;
    }
  }
  EXPECT_GT(checked, 70u);
}

TEST(Arm64SimCompiled, AHelperGetsEachFloatAndIntegerArgumentInItsAapcs64RegisterAndAFloatResultIsTakenByItsWidth) {
  if (!a64sim::kSignatures) {
    GTEST_SKIP() << "the simulator calls a host function with a signature only where the host's C ABI classifies as AAPCS64 does";
  }
  JitWorld w;
  // h_mix(i64, f64, i64, f32) -> f64: x0, d0, x1, s1
  {
    B b("mixed", 0);
    GRJIT_VReg a = b.param(GRJIT_TYPE_I64), x = b.param(GRJIT_TYPE_F64), c = b.param(GRJIT_TYPE_I64),
               e = b.param(GRJIT_TYPE_F32), r = b.reg(GRJIT_TYPE_F64);
    b.at(b.block());
    b.call(r, reinterpret_cast<const void *>(&h_mix), {V(a), V(x), V(c), V(e)});
    b.ret(V(r));
    Fn f(b.finish());
    std::map<uint64_t, a64sim::Sig> sigs;
    sigs[reinterpret_cast<uint64_t>(&h_mix)] = sig("idif", 'd');
    SimRun r2 = sim_function(f, w, {3, floatfx::f64bits(5.0), 7, floatfx::f32bits(0.5f)}, sigs);
    ASSERT_EQ(r2.exit, uint32_t{GRJIT_EXIT_RETURNED});
    EXPECT_EQ(dbl(r2.out[0]), 3.0 * 1000 + 5.0 * 100 + 7.0 * 10 + 0.5);
  }
  // an f32 result with noise in bits 32-63 of d0 comes out as the low 32 bits only
  {
    B b("f32res", 0);
    GRJIT_VReg a = b.param(GRJIT_TYPE_F32), r = b.reg(GRJIT_TYPE_F32);
    b.at(b.block());
    b.call(r, reinterpret_cast<const void *>(&h_float_result), {V(a)});
    b.ret(V(r));
    Fn f(b.finish());
    std::map<uint64_t, a64sim::Sig> sigs;
    sigs[reinterpret_cast<uint64_t>(&h_float_result)] = sig("f", 'f');
    SimRun r2 = sim_function(f, w, {floatfx::f32bits(1.25f)}, sigs);
    ASSERT_EQ(r2.exit, uint32_t{GRJIT_EXIT_RETURNED});
    EXPECT_EQ(r2.out[0], floatfx::f32bits(2.5f)) << "the word is the float and nothing above it";
  }
  // typed immediates as the arguments
  {
    B b("imm", 0);
    GRJIT_VReg r = b.reg(GRJIT_TYPE_F64);
    b.at(b.block());
    b.call(r, reinterpret_cast<const void *>(&h_mix),
        {I(40), grjit_operand_f64_imm(floatfx::f64bits(1.25)), I(2), grjit_operand_f32_imm(static_cast<uint32_t>(floatfx::f32bits(0.5f)))});
    b.ret(V(r));
    Fn f(b.finish());
    std::map<uint64_t, a64sim::Sig> sigs;
    sigs[reinterpret_cast<uint64_t>(&h_mix)] = sig("idif", 'd');
    SimRun r2 = sim_function(f, w, {}, sigs);
    ASSERT_EQ(r2.exit, uint32_t{GRJIT_EXIT_RETURNED});
    EXPECT_EQ(dbl(r2.out[0]), 40.0 * 1000 + 1.25 * 100 + 2.0 * 10 + 0.5);
  }
  // an i32 result with noise in the upper half of x0 is cut
  {
    B b("u32", 0);
    GRJIT_VReg a = b.param(GRJIT_TYPE_I64), r = b.reg(GRJIT_TYPE_I32);
    b.at(b.block());
    b.call(r, reinterpret_cast<const void *>(&h_u32), {V(a)});
    b.ret(V(r));
    Fn f(b.finish());
    std::map<uint64_t, a64sim::Sig> sigs;
    sigs[reinterpret_cast<uint64_t>(&h_u32)] = sig("i", 'w');
    SimRun r2 = sim_function(f, w, {0x41}, sigs);
    EXPECT_EQ(r2.out[0], 0x42u) << "the callee leaves noise above bit 31; the slot does not hold it";
  }
}

/* ---- Slots beyond the short offset forms, run by the simulator on any host ------------------------ */

TEST(Arm64Offsets, ManyFloatRegistersAreAddressedThroughTheRegisterFormAndEveryValueIsExact) {
  const size_t n = 120;
  Fn f(many_floats(n));
  GRJIT_Emitted e;
  ASSERT_EQ(grjit_emit_for(GRJIT_ARCH_ARM64, f, grjit_allocator_default(), nullptr, nullptr, 0, &e), GRJIT_OK);
  // The slots past the first 32 are out of ldur's reach (-256): the register form `ldr d, [x29, x17]` appears.
  size_t register_forms = 0;
  for (size_t i = 0; i + 4 <= e.size; i += 4) {
    uint32_t w;
    std::memcpy(&w, e.bytes + i, 4);
    register_forms += (w & 0xFFE0FC00u) == 0xFC606800u && ((w >> 16) & 31) == 17; // ldr/str d, [xn, x17]
  }
  EXPECT_GT(register_forms, n) << "a slot beyond the short forms is read through x17";
  // Run by the simulator, against the evaluator.
  JitWorld w;
  Config cfg;
  std::vector<uint64_t> args = {floatfx::f64bits(1.5)};
  std::vector<uint64_t> out(grjit_function_interp_slot_count(f) + 1, 0);
  cfg.code = e.bytes;
  cfg.size = e.size;
  cfg.args[0] = reinterpret_cast<uint64_t>(w.ctx);
  cfg.args[1] = reinterpret_cast<uint64_t>(args.data());
  cfg.args[2] = reinterpret_cast<uint64_t>(out.data());
  Cpu cpu;
  Result r = cpu.run(cfg);
  ASSERT_TRUE(r.ok) << r.error;
  ireval::Result want = ireval::eval(f, w.ctx, nullptr, args);
  ASSERT_EQ(static_cast<uint32_t>(r.x0), want.exit);
  EXPECT_EQ(out[0], want.out[0]);
  double expect = 0;
  for (size_t i = 0; i < n; i++) {
    expect += 1.5 + (static_cast<double>(i) + 0.25);
  }
  EXPECT_EQ(dbl(out[0]), expect);
  grjit_emitted_free(&e);
}

/* ---- Real arm64: the AAPCS64 helper probe, the float result with junk above bit 31, FPCR -------- */

#if defined(__aarch64__) && defined(__linux__)

extern "C" {
uint64_t grjit_test_a64_regs[16]; // x0 to x7, then the low 64 bits of d0 to d7
uint64_t grjit_test_a64_probe(void);
float grjit_test_a64_junk_float(void);
float grjit_test_a64_f32(void);
double grjit_test_a64_f64(void);
}
asm(R"(
  .text
  .globl grjit_test_a64_probe
  .type grjit_test_a64_probe,%function
grjit_test_a64_probe:
  adrp x16, grjit_test_a64_regs
  add x16, x16, :lo12:grjit_test_a64_regs
  stp x0, x1, [x16, #0]
  stp x2, x3, [x16, #16]
  stp x4, x5, [x16, #32]
  stp x6, x7, [x16, #48]
  str d0, [x16, #64]
  str d1, [x16, #72]
  str d2, [x16, #80]
  str d3, [x16, #88]
  str d4, [x16, #96]
  str d5, [x16, #104]
  str d6, [x16, #112]
  str d7, [x16, #120]
  mov x0, xzr
  ret
  .size grjit_test_a64_probe, .-grjit_test_a64_probe

  .globl grjit_test_a64_junk_float
  .type grjit_test_a64_junk_float,%function
grjit_test_a64_junk_float:
  movz x0, #0x3FC0, lsl #16
  movk x0, #0xBEEF, lsl #32
  movk x0, #0xDEAD, lsl #48
  fmov d0, x0
  ret
  .size grjit_test_a64_junk_float, .-grjit_test_a64_junk_float

  .globl grjit_test_a64_f32
  .type grjit_test_a64_f32,%function
grjit_test_a64_f32:
  movz x0, #0x4020, lsl #16
  movk x0, #0xBEEF, lsl #32
  movk x0, #0xDEAD, lsl #48
  fmov d0, x0
  ret
  .size grjit_test_a64_f32, .-grjit_test_a64_f32

  .globl grjit_test_a64_f64
  .type grjit_test_a64_f64,%function
grjit_test_a64_f64:
  movz x0, #0x0001
  movk x0, #0xC00C, lsl #48
  fmov d0, x0
  ret
  .size grjit_test_a64_f64, .-grjit_test_a64_f64
  .section .note.GNU-stack,"",%progbits
  .text
)");

namespace {

enum class Arg { I64, F64, F32, I64Imm, F64Imm, F32Imm };

bool is_float_arg(Arg a) { return a == Arg::F64 || a == Arg::F32 || a == Arg::F64Imm || a == Arg::F32Imm; }

uint64_t arg_value(Arg a, size_t i) {
  switch (a) {
    case Arg::I64:
    case Arg::I64Imm: return 0x1111000000000000ull + 0x0101010101ull * (i + 1) + 0x7;
    case Arg::F64:
    case Arg::F64Imm: return 0x4000000000000000ull + 0x0001000100010001ull * (i + 1);
    default: return 0x40000000ull + 0x00010101ull * (i + 1);
  }
}

/* Builds f(params...) { call probe(args...) ; ret 0 } for the kinds, runs it, and compares the registers with the
 * AAPCS64 classification worked out here: integer-class arguments in x0-x5 among themselves, float-class ones in
 * d0-d7 among themselves (an f32 in the low 32 bits of its register). */
::testing::AssertionResult probe_pattern(JitWorld & w, const std::vector<Arg> & kinds) {
  B b("probe", 0);
  std::vector<GRJIT_VReg> params;
  std::vector<GRJIT_Operand> args;
  std::vector<uint64_t> words;
  for (size_t i = 0; i < kinds.size(); i++) {
    const uint64_t v = arg_value(kinds[i], i);
    switch (kinds[i]) {
      case Arg::I64: params.push_back(b.param(GRJIT_TYPE_I64)); words.push_back(v); break;
      case Arg::F64: params.push_back(b.param(GRJIT_TYPE_F64)); words.push_back(v); break;
      case Arg::F32: params.push_back(b.param(GRJIT_TYPE_F32)); words.push_back(v); break;
      default: break;
    }
  }
  GRJIT_VReg r = b.reg(GRJIT_TYPE_I64);
  b.at(b.block());
  size_t next_param = 0;
  for (size_t i = 0; i < kinds.size(); i++) {
    const uint64_t v = arg_value(kinds[i], i);
    switch (kinds[i]) {
      case Arg::I64Imm: args.push_back(I(static_cast<int64_t>(v))); break;
      case Arg::F64Imm: args.push_back(grjit_operand_f64_imm(v)); break;
      case Arg::F32Imm: args.push_back(grjit_operand_f32_imm(static_cast<uint32_t>(v))); break;
      default: args.push_back(V(params[next_param++])); break;
    }
  }
  b.call(r, reinterpret_cast<const void *>(&grjit_test_a64_probe), args);
  b.ret(V(r));
  Fn f(b.finish());
  char why[256] = {0};
  if (grjit_function_verify(f, nullptr, why, sizeof why) != GRJIT_OK) {
    return ::testing::AssertionFailure() << "the verifier refuses the probe: " << why;
  }
  Compiled code(f, w.pages());
  if (!code) {
    return ::testing::AssertionFailure() << "the compile failed: " << grjit_result_string(code.result);
  }
  std::memset(grjit_test_a64_regs, 0xEE, sizeof grjit_test_a64_regs);
  floatfx::Outcome o = floatfx::run(code.code, w.ctx, words);
  if (o.fault != 0 || o.exit != GRJIT_EXIT_RETURNED) {
    return ::testing::AssertionFailure() << "the probe did not return";
  }
  size_t ints = 0, floats = 0;
  char buf[200];
  for (size_t i = 0; i < kinds.size(); i++) {
    const uint64_t v = arg_value(kinds[i], i);
    if (is_float_arg(kinds[i])) {
      const bool single = kinds[i] == Arg::F32 || kinds[i] == Arg::F32Imm;
      const uint64_t got = grjit_test_a64_regs[8 + floats];
      const uint64_t mask = single ? 0xFFFFFFFFull : ~uint64_t{0};
      if ((got & mask) != (v & mask)) {
        std::snprintf(buf, sizeof buf, "float argument %zu should be in d%zu as 0x%llx but d%zu holds 0x%llx", i,
            floats, static_cast<unsigned long long>(v), floats, static_cast<unsigned long long>(got));
        return ::testing::AssertionFailure() << buf;
      }
      floats++;
    } else {
      if (grjit_test_a64_regs[ints] != v) {
        std::snprintf(buf, sizeof buf, "integer argument %zu should be in x%zu as 0x%llx but it holds 0x%llx", i,
            ints, static_cast<unsigned long long>(v), static_cast<unsigned long long>(grjit_test_a64_regs[ints]));
        return ::testing::AssertionFailure() << buf;
      }
      ints++;
    }
  }
  return ::testing::AssertionSuccess();
}

} // namespace

TEST(Arm64Helpers, EveryMixOfIntegerAndFloatArgumentsArrivesInItsRegisterByAapcs64Classification) {
  GRJIT_REQUIRE_BACKEND();
  JitWorld w;
  size_t patterns = 0;
  for (size_t n = 0; n <= 6; n++) {
    size_t total = 1;
    for (size_t i = 0; i < n; i++) {
      total *= 3;
    }
    for (size_t code = 0; code < total; code++) {
      std::vector<Arg> kinds;
      size_t c = code;
      for (size_t i = 0; i < n; i++) {
        kinds.push_back(c % 3 == 0 ? Arg::I64 : c % 3 == 1 ? Arg::F64 : Arg::F32);
        c /= 3;
      }
      ASSERT_TRUE(probe_pattern(w, kinds)) << "pattern " << patterns;
      patterns++;
    }
  }
  for (size_t n = 1; n <= 4; n++) {
    size_t total = 1;
    for (size_t i = 0; i < n; i++) {
      total *= 6;
    }
    for (size_t code = 0; code < total; code++) {
      std::vector<Arg> kinds;
      size_t c = code;
      for (size_t i = 0; i < n; i++) {
        kinds.push_back(static_cast<Arg>(c % 6));
        c /= 6;
      }
      ASSERT_TRUE(probe_pattern(w, kinds)) << "pattern " << patterns;
      patterns++;
    }
  }
  std::printf("float helper ABI (arm64): %zu argument patterns checked against the AAPCS64 classification\n", patterns);
  EXPECT_GT(patterns, 2000u);
}

TEST(Arm64Helpers, TheNamedClassificationFromTheStoryComesOutAsStated) {
  GRJIT_REQUIRE_BACKEND();
  JitWorld w;
  // (i64, f64, i64, f32) is x0, d0, x1, s1.
  ASSERT_TRUE(probe_pattern(w, {Arg::I64, Arg::F64, Arg::I64, Arg::F32}));
  EXPECT_EQ(grjit_test_a64_regs[0], arg_value(Arg::I64, 0));
  EXPECT_EQ(grjit_test_a64_regs[8], arg_value(Arg::F64, 1));
  EXPECT_EQ(grjit_test_a64_regs[1], arg_value(Arg::I64, 2));
  EXPECT_EQ(grjit_test_a64_regs[9] & 0xFFFFFFFFull, arg_value(Arg::F32, 3));
  ASSERT_TRUE(probe_pattern(w, {Arg::F64, Arg::F64, Arg::F64, Arg::F64, Arg::F64, Arg::F64}));
  ASSERT_TRUE(probe_pattern(w, {Arg::F64, Arg::F32, Arg::F64, Arg::F32, Arg::F64, Arg::F32}));
  ASSERT_TRUE(probe_pattern(w, {Arg::F32Imm, Arg::F64Imm, Arg::F32Imm, Arg::F64Imm, Arg::I64Imm, Arg::F64}));
}

TEST(Arm64Helpers, AFloatResultIsTakenFromD0ByItsOwnWidthWhateverIsAboveBit31) {
  GRJIT_REQUIRE_BACKEND();
  JitWorld w;
  struct Case {
    const void * fn;
    GRJIT_Type result;
    uint64_t want;
  };
  const Case cases[] = {
      {reinterpret_cast<const void *>(&grjit_test_a64_junk_float), GRJIT_TYPE_F32, 0x3FC00000u},
      {reinterpret_cast<const void *>(&grjit_test_a64_f32), GRJIT_TYPE_F32, 0x40200000u},
      {reinterpret_cast<const void *>(&grjit_test_a64_f64), GRJIT_TYPE_F64, 0xC00C000000000001ull},
  };
  for (const Case & c : cases) {
    SCOPED_TRACE(c.result);
    B b("result", 0);
    GRJIT_VReg r = b.reg(c.result);
    b.at(b.block());
    b.call(r, c.fn, {});
    b.ret(V(r)); // the call's own result word is returned: a MOVE would cut an f32 and hide a result taken whole
    Fn f(b.finish());
    Compiled code(f, w.pages());
    ASSERT_TRUE(code);
    EXPECT_EQ(floatfx::run(code.code, w.ctx, {}).out[0], c.want) << "an f32 is its low 32 bits, zero-extended";
  }
  B b("sum", 0);
  GRJIT_VReg r = b.reg(GRJIT_TYPE_F32), t = b.reg(GRJIT_TYPE_F32);
  b.at(b.block());
  b.call(r, reinterpret_cast<const void *>(&grjit_test_a64_junk_float), {});
  b.bin(GRJIT_OP_FADD, t, V(r), V(r));
  b.ret(V(t));
  Fn f(b.finish());
  Compiled code(f, w.pages());
  ASSERT_TRUE(code);
  EXPECT_EQ(floatfx::run(code.code, w.ctx, {}).out[0], uint64_t{0x40400000});
}

TEST(Arm64Fpcr, TheHostRunsWithTheDefaultFpcrAndCompiledFloatCodeLeavesItAlone) {
  GRJIT_REQUIRE_BACKEND();
  uint64_t before, after;
  asm volatile("mrs %0, fpcr" : "=r"(before));
  EXPECT_EQ(before & 0x07C09F00u, 0u) << "RMode, FZ, DN, AHP and the trap enables are clear: the default the compiled code assumes";
  JitWorld w;
  Built b;
  b.op = floatfx::build_any_op("f64.sqrt");
  Compiled code(b.op.function, w.pages());
  ASSERT_TRUE(code);
  floatfx::Outcome o = floatfx::run(code.code, w.ctx, {floatfx::f64bits(2.25)});
  EXPECT_EQ(o.out[0], floatfx::f64bits(1.5));
  asm volatile("mrs %0, fpcr" : "=r"(after));
  EXPECT_EQ(after, before) << "compiled code never writes FPCR";
}

#else // not arm64 Linux: the same tests, skipped by name

TEST(Arm64Helpers, EveryMixOfIntegerAndFloatArgumentsArrivesInItsRegisterByAapcs64Classification) {
  GTEST_SKIP() << "the AAPCS64 helper ABI is checked against an AArch64 assembly probe, on arm64 Linux only";
}

TEST(Arm64Helpers, TheNamedClassificationFromTheStoryComesOutAsStated) {
  GTEST_SKIP() << "arm64 Linux only (an AAPCS64 probe)";
}

TEST(Arm64Helpers, AFloatResultIsTakenFromD0ByItsOwnWidthWhateverIsAboveBit31) {
  GTEST_SKIP() << "arm64 Linux only (an AAPCS64 probe)";
}

TEST(Arm64Fpcr, TheHostRunsWithTheDefaultFpcrAndCompiledFloatCodeLeavesItAlone) {
  GTEST_SKIP() << "FPCR is read on an arm64 host only";
}

#endif // arm64 Linux

/* ---- The callee-saved vector registers: sentinels ---------------------------------------------------
 *
 * A caller sets the low halves of d8-d15 (and x19-x29) to known values, runs compiled code that uses every family
 * of float operation, and reads them back. Only the low 64 bits are asserted: the upper halves of the v registers
 * are caller-saved, and a test that compared 128 bits would fail on correct code. The runs: a return, a guard exit,
 * a poll whose slow path runs, a poll the helper refuses, and a helper that trashes every caller-saved vector
 * register (a conforming callee: it saves and restores the callee-saved ones it uses) with a value live across it,
 * which is exact because it is in a slot. A sentinel cannot show that code never uses a callee-saved register, only
 * that it did not change one on the paths run; the structural scan above shows never, and the sentinel shows the scan
 * is looking at the right registers (defect 86 is caught by both). */

#if FX_HAVE_CALLS_ASM && FX_ASM_AARCH64

namespace {

/* Takes a double, trashes every caller-saved vector register, saves and restores the callee-saved ones it
 * clobbers (the compiler does, being told), and returns the double. */
__attribute__((noinline)) double fp_clobber(double x) {
  double r = x;
  asm volatile(
      "movi v0.16b, #0x5A\n movi v1.16b, #0x5A\n movi v2.16b, #0x5A\n movi v3.16b, #0x5A\n"
      "movi v4.16b, #0x5A\n movi v5.16b, #0x5A\n movi v6.16b, #0x5A\n movi v7.16b, #0x5A\n"
      "movi v8.16b, #0x5A\n movi v9.16b, #0x5A\n movi v10.16b, #0x5A\n movi v11.16b, #0x5A\n"
      "movi v12.16b, #0x5A\n movi v13.16b, #0x5A\n movi v14.16b, #0x5A\n movi v15.16b, #0x5A\n"
      "movi v16.16b, #0x5A\n movi v17.16b, #0x5A\n movi v18.16b, #0x5A\n movi v19.16b, #0x5A\n"
      "movi v20.16b, #0x5A\n movi v21.16b, #0x5A\n movi v22.16b, #0x5A\n movi v23.16b, #0x5A\n"
      "movi v24.16b, #0x5A\n movi v25.16b, #0x5A\n movi v26.16b, #0x5A\n movi v27.16b, #0x5A\n"
      "movi v28.16b, #0x5A\n movi v29.16b, #0x5A\n movi v30.16b, #0x5A\n movi v31.16b, #0x5A\n"
      :
      :
      : "v0", "v1", "v2", "v3", "v4", "v5", "v6", "v7", "v8", "v9", "v10", "v11", "v12", "v13", "v14", "v15", "v16",
        "v17", "v18", "v19", "v20", "v21", "v22", "v23", "v24", "v25", "v26", "v27", "v28", "v29", "v30", "v31",
        "memory");
  return r;
}

} // namespace

using sentfx::SentinelWorld;
using sentfx::SentinelRun;
using sentfx::run_with_sentinels;
using sentfx::g_polls;
using sentfx::g_answer;

TEST(Arm64Sentinels, FloatCodeLeavesEveryCalleeSavedRegisterAloneOnAReturnAGuardExitAPollAndATrashingHelper) {
  GRJIT_REQUIRE_BACKEND();
  SentinelWorld w;
  Fn f(sentfx::sentinel_function(reinterpret_cast<const void *>(&fp_clobber)));
  char why[256] = {0};
  ASSERT_EQ(grjit_function_verify(f, nullptr, why, sizeof why), GRJIT_OK) << why;
  Compiled c(f, w.pages());
  ASSERT_TRUE(c) << grjit_result_string(c.result);
  alignas(16) uint8_t mem[64] = {0};
  const std::vector<uint64_t> args = {floatfx::f64bits(2.75), floatfx::f32bits(1.5f), 1,
      reinterpret_cast<uint64_t>(mem)};
  // 1. a return, with the poll fast path: the answer is the evaluator's
  {
    SentinelRun r = run_with_sentinels(w, c.code, args, "a return");
    ASSERT_EQ(r.exit, uint32_t{GRJIT_EXIT_RETURNED});
    EXPECT_TRUE(r.intact);
    alignas(16) uint8_t mem2[64] = {0};
    std::vector<uint64_t> same = args;
    same[3] = reinterpret_cast<uint64_t>(mem2);
    Fn twin(sentfx::sentinel_function(nullptr));
    ireval::Result e = ireval::eval(twin, w.ctx, nullptr, same);
    ASSERT_EQ(e.exit, uint32_t{GRJIT_EXIT_RETURNED});
    EXPECT_EQ(r.out[0], e.out[0]) << "the helper that trashed the vector registers cost the live float nothing (the twin has no call)";
    EXPECT_EQ(g_polls, 0);
  }
  // 2. a guard exit (mode 0)
  {
    std::vector<uint64_t> zero = args;
    zero[2] = 0;
    SentinelRun r = run_with_sentinels(w, c.code, zero, "a guard exit");
    EXPECT_EQ(r.exit, uint32_t{GRJIT_EXIT_DEOPT});
    EXPECT_TRUE(r.intact);
  }
  // 3. the poll's slow path runs (a request pending) and the helper lets the function go on
  {
    w.post();
    SentinelRun r = run_with_sentinels(w, c.code, args, "a poll's slow path");
    EXPECT_EQ(r.exit, uint32_t{GRJIT_EXIT_RETURNED});
    EXPECT_TRUE(r.intact);
    EXPECT_EQ(g_polls, 1) << "the slow path ran";
  }
  // 4. the helper refuses
  {
    g_answer = 7;
    SentinelRun r = run_with_sentinels(w, c.code, args, "a poll refusal");
    EXPECT_EQ(r.exit, uint32_t{GRJIT_EXIT_REFUSED});
    EXPECT_TRUE(r.intact);
    EXPECT_EQ(g_polls, 2);
  }
}

#else

TEST(Arm64Sentinels, FloatCodeLeavesEveryCalleeSavedRegisterAloneOnAReturnAGuardExitAPollAndATrashingHelper) {
  GTEST_SKIP() << "the sentinel caller loads and reads d8-d15 and x19-x29: arm64 Linux only";
}

#endif // FX_HAVE_CALLS_ASM && FX_ASM_AARCH64

GRJIT_TEST_MAIN()
