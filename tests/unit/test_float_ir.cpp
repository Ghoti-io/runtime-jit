/**
 * @file
 *
 * The float IR (spec-runtime-float, story 3): the `F32` and `F64` types and the float, conversion
 * and reinterpretation operations of numeric-operations.md, their typing rules, the verifier's rule
 * that a float-to-integer truncation is preceded by its range guard, and the code of each backend run
 * against hand-worked answers and the test evaluator: natively on x86-64 SysV and on arm64 (under
 * qemu-aarch64 or on a Raspberry Pi), under wine for Win64. Floats through helper calls (the SysV
 * classification is checked against an assembly probe here, the AAPCS64 and Microsoft x64 ones in
 * test_arm64_numeric.cpp and test_win64_numeric.cpp), through calls and tail calls between compiled
 * functions, and in frame states.
 *
 * The comparison rule, here and in the evaluator: floats are compared by their bits, with any two
 * NaNs equal. A NaN never equals a number, and +0 and -0 differ. A worked answer that is a NaN says
 * which class it must be (the suite's `nan:canonical`, the quiet bit and no other payload bit, or
 * `nan:arithmetic`, the quiet bit and any payload); everything else is bit for bit.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include "../float_fixture.h"
#include "../feature_fixture.h"

#include "../calls_fixture.h"

#include "../../src/code/code_internal.h"

#include <cfloat>
#if defined(__SSE__)
#include <xmmintrin.h>
#endif
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

using namespace floatfx;
using featfx::detected;
using featfx::need_of;
using featfx::two_arms;

/* Every backend runs these where it has a native host: x86-64 SysV and arm64 natively, Win64 under wine. The host's
 * floating-point environment must be the default the compiled code assumes, or the test says so and skips by name. */
#define FLOAT_RUN_HERE()                                                                   \
  do {                                                                                     \
    GRJIT_REQUIRE_BACKEND();                                                               \
    if (!floatfx::host_fp_is_default()) {                                                  \
      GTEST_SKIP() << "the host's MXCSR / FPCR is not the default the compiled code assumes"; \
    }                                                                                      \
  } while (0)

/* Helpers written in assembly so that the test can see what the compiled code left in the registers
 * (which a C++ function cannot say: the compiler may use the arguments as it likes) and can return a
 * float with junk above bit 31 of xmm0. They exist on x86-64 Linux only, where the SysV classification
 * under test is the one the compiled code uses. */
#if defined(__x86_64__) && defined(__linux__)
extern "C" {
uint64_t grjit_test_regs[14]; // rdi rsi rdx rcx r8 r9, then the low 64 bits of xmm0 to xmm7
uint64_t grjit_test_probe(void);
float grjit_test_probe_f32(void);
double grjit_test_probe_f64(void);
float grjit_test_junk_float(void);
}
asm(R"(
  .text
  .globl grjit_test_probe
  .type grjit_test_probe,@function
grjit_test_probe:
  leaq grjit_test_regs(%rip), %r11
  movq %rdi, 0(%r11)
  movq %rsi, 8(%r11)
  movq %rdx, 16(%r11)
  movq %rcx, 24(%r11)
  movq %r8, 32(%r11)
  movq %r9, 40(%r11)
  movq %xmm0, 48(%r11)
  movq %xmm1, 56(%r11)
  movq %xmm2, 64(%r11)
  movq %xmm3, 72(%r11)
  movq %xmm4, 80(%r11)
  movq %xmm5, 88(%r11)
  movq %xmm6, 96(%r11)
  movq %xmm7, 104(%r11)
  xorl %eax, %eax
  ret
  .size grjit_test_probe, .-grjit_test_probe

  .globl grjit_test_probe_f32
  .type grjit_test_probe_f32,@function
grjit_test_probe_f32:
  movabsq $0xDEADBEEF40200000, %rax
  movq %rax, %xmm0
  ret
  .size grjit_test_probe_f32, .-grjit_test_probe_f32

  .globl grjit_test_probe_f64
  .type grjit_test_probe_f64,@function
grjit_test_probe_f64:
  movabsq $0xC00C000000000001, %rax
  movq %rax, %xmm0
  ret
  .size grjit_test_probe_f64, .-grjit_test_probe_f64

  .globl grjit_test_junk_float
  .type grjit_test_junk_float,@function
grjit_test_junk_float:
  movabsq $0xDEADBEEF3FC00000, %rax
  movq %rax, %xmm0
  ret
  .size grjit_test_junk_float, .-grjit_test_junk_float
  .section .note.GNU-stack,"",@progbits
  .text
)");
#endif

namespace {

#define B64(x) f64bits(x)
#define B32(x) f32bits(x)

constexpr uint64_t M32 = 0xFFFFFFFFu;

std::string hex(uint64_t v) {
  char buf[40];
  std::snprintf(buf, sizeof buf, "0x%llx", static_cast<unsigned long long>(v));
  return buf;
}

/* What a NaN result must be. */
enum class Cls { Exact, NanCanonical, NanArithmetic };

bool nan_canonical(unsigned width, uint64_t bits) {
  const uint64_t quiet = width == 32 ? 0x00400000ull : 0x0008000000000000ull;
  const uint64_t payload = width == 32 ? 0x007FFFFFull : 0x000FFFFFFFFFFFFFull;
  return ireval::is_nan_bits(width, bits) && (bits & payload) == quiet;
}

bool nan_arithmetic(unsigned width, uint64_t bits) {
  const uint64_t quiet = width == 32 ? 0x00400000ull : 0x0008000000000000ull;
  return ireval::is_nan_bits(width, bits) && (bits & quiet) != 0;
}

/* Is `got` what `want` and `cls` ask for, for a result of `width` bits? */
bool result_ok(unsigned width, uint64_t got, uint64_t want, Cls cls) {
  switch (cls) {
    case Cls::NanCanonical: return nan_canonical(width, got);
    case Cls::NanArithmetic: return nan_arithmetic(width, got);
    default: return got == want;
  }
}

/* ---- Hand-worked answers -------------------------------------------------------------------- */

struct Worked {
  const char * instr;
  uint64_t a, b;
  uint64_t want;
  Cls cls;
};

constexpr Cls X = Cls::Exact;
constexpr Cls NC = Cls::NanCanonical;
constexpr Cls NA = Cls::NanArithmetic;
constexpr uint64_t PZ64 = 0, NZ64 = 0x8000000000000000ull;
constexpr uint64_t PZ32 = 0, NZ32 = 0x80000000u;
constexpr uint64_t INF64 = 0x7FF0000000000000ull, NINF64 = 0xFFF0000000000000ull;
constexpr uint64_t INF32 = 0x7F800000u, NINF32 = 0xFF800000u;
constexpr uint64_t QNAN64 = 0x7FF8000000000000ull, SNAN64 = 0x7FF0000000000001ull;
constexpr uint64_t QNAN32 = 0x7FC00000u, SNAN32 = 0x7F800001u;

/* Answers worked from the WebAssembly semantics by hand, never by the evaluator or the backend.
 * The result of an f32 operation is a zero-extended word. */
const Worked kWorked[] = {
    // arithmetic, the operand order, the signs of zero
    {"f64.add", B64(1.5), B64(2.25), B64(3.75), X},
    {"f64.add", NZ64, NZ64, NZ64, X},
    {"f64.add", NZ64, PZ64, PZ64, X},
    {"f64.add", PZ64, NZ64, PZ64, X},
    {"f64.sub", B64(5.0), B64(7.0), B64(-2.0), X},
    {"f64.sub", PZ64, PZ64, PZ64, X},
    {"f64.sub", NZ64, PZ64, NZ64, X},
    {"f64.sub", PZ64, NZ64, PZ64, X},
    {"f64.mul", NZ64, B64(5.0), NZ64, X},
    {"f64.mul", B64(3.0), B64(-0.5), B64(-1.5), X},
    {"f64.mul", INF64, PZ64, QNAN64, NC},
    {"f64.div", B64(1.0), B64(4.0), B64(0.25), X},
    {"f64.div", B64(4.0), B64(1.0), B64(4.0), X},
    {"f64.div", B64(1.0), PZ64, INF64, X},
    {"f64.div", B64(1.0), NZ64, NINF64, X},
    {"f64.div", B64(-1.0), PZ64, NINF64, X},
    {"f64.div", PZ64, PZ64, QNAN64, NC},
    {"f64.sqrt", B64(2.25), 0, B64(1.5), X},
    {"f64.sqrt", NZ64, 0, NZ64, X},
    {"f64.sqrt", B64(-1.0), 0, QNAN64, NC},
    {"f64.sqrt", INF64, 0, INF64, X},
    {"f32.add", B32(1.5f), B32(2.25f), B32(3.75f), X},
    {"f32.add", NZ32, NZ32, NZ32, X},
    {"f32.sub", B32(5.0f), B32(7.0f), B32(-2.0f), X},
    {"f32.sub", NZ32, PZ32, NZ32, X},
    {"f32.mul", NZ32, B32(5.0f), NZ32, X},
    {"f32.div", B32(1.0f), B32(4.0f), B32(0.25f), X},
    {"f32.div", B32(1.0f), PZ32, INF32, X},
    {"f32.div", PZ32, PZ32, QNAN32, NC},
    {"f32.sqrt", B32(2.25f), 0, B32(1.5f), X},
    {"f32.sqrt", B32(-1.0f), 0, QNAN32, NC},
    {"f32.sqrt", NZ32, 0, NZ32, X},
    // subnormals survive (default MXCSR: no flush to zero, no denormals-are-zero)
    {"f64.add", 1, 1, 2, X},
    {"f64.mul", 0x0010000000000000ull, B64(0.5), 0x0008000000000000ull, X},
    {"f64.sub", 0x0010000000000000ull, 1, 0x000FFFFFFFFFFFFFull, X},
    {"f64.div", 0x0008000000000000ull, B64(2.0), 0x0004000000000000ull, X},
    {"f32.add", 1, 1, 2, X},
    {"f32.mul", 0x00800000u, B32(0.5f), 0x00400000u, X},
    {"f32.sub", 0x00800000u, 1, 0x007FFFFFu, X},
    // min and max: the signs of zero, NaN on either side, the infinities
    {"f64.min", NZ64, PZ64, NZ64, X},
    {"f64.min", PZ64, NZ64, NZ64, X},
    {"f64.max", NZ64, PZ64, PZ64, X},
    {"f64.max", PZ64, NZ64, PZ64, X},
    {"f64.min", NZ64, NZ64, NZ64, X},
    {"f64.max", PZ64, PZ64, PZ64, X},
    {"f64.min", B64(1.0), B64(2.0), B64(1.0), X},
    {"f64.min", B64(2.0), B64(1.0), B64(1.0), X},
    {"f64.max", B64(-1.0), B64(-2.0), B64(-1.0), X},
    {"f64.min", QNAN64, B64(1.0), QNAN64, NC},
    {"f64.min", B64(1.0), QNAN64, QNAN64, NC},
    {"f64.max", QNAN64, B64(1.0), QNAN64, NC},
    {"f64.max", B64(1.0), QNAN64, QNAN64, NC},
    {"f64.min", SNAN64, B64(1.0), QNAN64, NA},
    {"f64.max", B64(1.0), SNAN64, QNAN64, NA},
    {"f64.min", NINF64, QNAN64, QNAN64, NC},
    {"f64.max", QNAN64, INF64, QNAN64, NC},
    {"f64.min", NINF64, INF64, NINF64, X},
    {"f64.max", NINF64, INF64, INF64, X},
    {"f32.min", NZ32, PZ32, NZ32, X},
    {"f32.min", PZ32, NZ32, NZ32, X},
    {"f32.max", NZ32, PZ32, PZ32, X},
    {"f32.max", PZ32, NZ32, PZ32, X},
    {"f32.min", QNAN32, B32(1.0f), QNAN32, NC},
    {"f32.max", B32(1.0f), SNAN32, QNAN32, NA},
    // the sign bit alone: a signalling NaN keeps its payload, and -(+0) is -0
    {"f64.neg", PZ64, 0, NZ64, X},
    {"f64.neg", NZ64, 0, PZ64, X},
    {"f64.neg", SNAN64, 0, 0xFFF0000000000001ull, X},
    {"f64.neg", 0xFFF0000000000001ull, 0, SNAN64, X},
    {"f64.neg", INF64, 0, NINF64, X},
    {"f64.abs", NZ64, 0, PZ64, X},
    {"f64.abs", 0xFFF0000000000001ull, 0, SNAN64, X},
    {"f64.abs", B64(-2.5), 0, B64(2.5), X},
    {"f64.copysign", B64(1.5), NZ64, B64(-1.5), X},
    {"f64.copysign", B64(-1.5), PZ64, B64(1.5), X},
    {"f64.copysign", SNAN64, NZ64, 0xFFF0000000000001ull, X},
    {"f64.copysign", PZ64, B64(-3.0), NZ64, X},
    {"f32.neg", PZ32, 0, NZ32, X},
    {"f32.neg", SNAN32, 0, 0xFF800001u, X},
    {"f32.neg", 0xFF800001u, 0, SNAN32, X},
    {"f32.abs", 0xFF800001u, 0, SNAN32, X},
    {"f32.abs", B32(-2.5f), 0, B32(2.5f), X},
    {"f32.copysign", B32(1.5f), NZ32, B32(-1.5f), X},
    {"f32.copysign", SNAN32, NZ32, 0xFF800001u, X},
    // rounding: the sign of a zero is kept, ties go to even, big values and infinities stay
    {"f64.ceil", B64(-0.5), 0, NZ64, X},
    {"f64.ceil", B64(0.5), 0, B64(1.0), X},
    {"f64.ceil", B64(-1.5), 0, B64(-1.0), X},
    {"f64.ceil", B64(1.5), 0, B64(2.0), X},
    {"f64.ceil", NZ64, 0, NZ64, X},
    {"f64.ceil", PZ64, 0, PZ64, X},
    {"f64.floor", B64(-0.5), 0, B64(-1.0), X},
    {"f64.floor", B64(0.5), 0, PZ64, X},
    {"f64.floor", NZ64, 0, NZ64, X},
    {"f64.floor", B64(-1.5), 0, B64(-2.0), X},
    {"f64.floor", B64(1.5), 0, B64(1.0), X},
    {"f64.trunc", B64(-0.9), 0, NZ64, X},
    {"f64.trunc", B64(0.9), 0, PZ64, X},
    {"f64.trunc", B64(-1.9), 0, B64(-1.0), X},
    {"f64.trunc", B64(1.9), 0, B64(1.0), X},
    {"f64.nearest", B64(0.5), 0, PZ64, X},
    {"f64.nearest", B64(1.5), 0, B64(2.0), X},
    {"f64.nearest", B64(2.5), 0, B64(2.0), X},
    {"f64.nearest", B64(3.5), 0, B64(4.0), X},
    {"f64.nearest", B64(-0.5), 0, NZ64, X},
    {"f64.nearest", B64(-1.5), 0, B64(-2.0), X},
    {"f64.nearest", B64(-2.5), 0, B64(-2.0), X},
    {"f64.nearest", B64(0.49999999999999994), 0, PZ64, X},
    {"f64.nearest", B64(0.5000000000000001), 0, B64(1.0), X},
    {"f64.nearest", B64(4503599627370495.5), 0, B64(4503599627370496.0), X},
    {"f64.nearest", 0x4330000000000001ull, 0, 0x4330000000000001ull, X},
    {"f64.nearest", INF64, 0, INF64, X},
    {"f64.nearest", NINF64, 0, NINF64, X},
    {"f64.nearest", SNAN64, 0, QNAN64, NA},
    {"f64.ceil", SNAN64, 0, QNAN64, NA},
    {"f64.floor", QNAN64, 0, QNAN64, NC},
    {"f64.trunc", 0xFFF0000000000001ull, 0, QNAN64, NA},
    {"f64.ceil", 0x7FEFFFFFFFFFFFFFull, 0, 0x7FEFFFFFFFFFFFFFull, X},
    {"f64.floor", 0x4330000000000001ull, 0, 0x4330000000000001ull, X},
    {"f32.ceil", B32(-0.5f), 0, NZ32, X},
    {"f32.floor", B32(-0.5f), 0, B32(-1.0f), X},
    {"f32.trunc", B32(-0.9f), 0, NZ32, X},
    {"f32.nearest", B32(0.5f), 0, PZ32, X},
    {"f32.nearest", B32(1.5f), 0, B32(2.0f), X},
    {"f32.nearest", B32(2.5f), 0, B32(2.0f), X},
    {"f32.nearest", B32(-2.5f), 0, B32(-2.0f), X},
    {"f32.nearest", B32(8388607.5f), 0, B32(8388608.0f), X},
    {"f32.nearest", 0x4B000001u, 0, 0x4B000001u, X},
    {"f32.nearest", SNAN32, 0, QNAN32, NA},
    {"f32.nearest", NINF32, 0, NINF32, X},
    {"f32.ceil", B32(1.5f), 0, B32(2.0f), X},
    {"f32.floor", B32(1.5f), 0, B32(1.0f), X},
    // comparison: ordered, a NaN is unordered, -0 equals +0
    {"f64.eq", NZ64, PZ64, 1, X},
    {"f64.eq", QNAN64, QNAN64, 0, X},
    {"f64.eq", B64(1.0), QNAN64, 0, X},
    {"f64.ne", QNAN64, QNAN64, 1, X},
    {"f64.ne", NZ64, PZ64, 0, X},
    {"f64.ne", B64(1.0), B64(2.0), 1, X},
    {"f64.lt", NZ64, PZ64, 0, X},
    {"f64.lt", B64(1.0), B64(2.0), 1, X},
    {"f64.lt", B64(2.0), B64(1.0), 0, X},
    {"f64.lt", QNAN64, B64(1.0), 0, X},
    {"f64.lt", B64(1.0), QNAN64, 0, X},
    {"f64.le", NZ64, PZ64, 1, X},
    {"f64.le", QNAN64, QNAN64, 0, X},
    {"f64.le", B64(1.0), B64(1.0), 1, X},
    {"f64.gt", INF64, B64(1.0), 1, X},
    {"f64.gt", B64(1.0), B64(2.0), 0, X},
    {"f64.gt", QNAN64, B64(1.0), 0, X},
    {"f64.ge", B64(2.0), B64(2.0), 1, X},
    {"f64.ge", B64(1.0), QNAN64, 0, X},
    {"f64.ge", NINF64, B64(1.0), 0, X},
    {"f32.eq", NZ32, PZ32, 1, X},
    {"f32.eq", QNAN32, QNAN32, 0, X},
    {"f32.ne", QNAN32, B32(1.0f), 1, X},
    {"f32.lt", B32(1.0f), B32(2.0f), 1, X},
    {"f32.lt", NZ32, PZ32, 0, X},
    {"f32.lt", QNAN32, B32(1.0f), 0, X},
    {"f32.le", NZ32, PZ32, 1, X},
    {"f32.gt", B32(2.0f), B32(1.0f), 1, X},
    {"f32.ge", B32(1.0f), QNAN32, 0, X},
    {"f32.ge", B32(2.0f), B32(2.0f), 1, X},
    // integer to float: one rounding, ties to even
    {"f64.convert_i32_s", 0xFFFFFFFFu, 0, B64(-1.0), X},
    {"f64.convert_i32_u", 0xFFFFFFFFu, 0, B64(4294967295.0), X},
    {"f64.convert_i32_s", 0x80000000u, 0, B64(-2147483648.0), X},
    {"f32.convert_i32_s", 0xFFFFFFFFu, 0, B32(-1.0f), X},
    {"f32.convert_i32_u", 0xFFFFFFFFu, 0, B32(4294967296.0f), X},
    {"f32.convert_i32_s", 0x01000001u, 0, B32(16777216.0f), X},
    {"f32.convert_i32_s", 0x01000003u, 0, B32(16777220.0f), X},
    {"f32.convert_i32_u", 0x80000080u, 0, B32(2147483648.0f), X},
    {"f32.convert_i32_u", 0x80000081u, 0, B32(2147483904.0f), X},
    {"f64.convert_i64_s", 0xFFFFFFFFFFFFFFFFull, 0, B64(-1.0), X},
    {"f64.convert_i64_s", 0x8000000000000000ull, 0, B64(-9223372036854775808.0), X},
    {"f64.convert_i64_s", 0x0020000000000001ull, 0, B64(9007199254740992.0), X},
    {"f64.convert_i64_s", 0x0020000000000003ull, 0, B64(9007199254740996.0), X},
    {"f64.convert_i64_u", 0xFFFFFFFFFFFFFFFFull, 0, B64(18446744073709551616.0), X},
    {"f64.convert_i64_u", 0x8000000000000001ull, 0, B64(9223372036854775808.0), X},
    {"f64.convert_i64_u", 0x8000000000000401ull, 0, 0x43E0000000000001ull, X},
    {"f64.convert_i64_u", 0x8000000000000400ull, 0, B64(9223372036854775808.0), X},
    {"f64.convert_i64_u", 0x8000000000000C00ull, 0, 0x43E0000000000002ull, X},
    {"f64.convert_i64_u", 0x7FFFFFFFFFFFFFFFull, 0, B64(9223372036854775808.0), X},
    {"f32.convert_i64_s", 0x8000000000000000ull, 0, B32(-9223372036854775808.0f), X},
    {"f32.convert_i64_u", 0xFFFFFFFFFFFFFFFFull, 0, B32(18446744073709551616.0f), X},
    {"f32.convert_i64_u", 0x8000008000000001ull, 0, 0x5F000001u, X},
    {"f32.convert_i64_u", 0x8000008000000000ull, 0, 0x5F000000u, X},
    {"f32.convert_i64_u", 0x8000018000000000ull, 0, 0x5F000002u, X},
    {"f32.convert_i64_u", 0, 0, 0, X},
    {"f64.convert_i32_u", 0, 0, 0, X},
    // float to integer, saturating and not
    {"i32.trunc_sat_f64_s", QNAN64, 0, 0, X},
    {"i32.trunc_sat_f64_s", B64(3e9), 0, 0x7FFFFFFF, X},
    {"i32.trunc_sat_f64_s", B64(-3e9), 0, 0x80000000u, X},
    {"i32.trunc_sat_f64_s", B64(-1.9), 0, 0xFFFFFFFFu, X},
    {"i32.trunc_sat_f64_s", INF64, 0, 0x7FFFFFFF, X},
    {"i32.trunc_sat_f64_s", NINF64, 0, 0x80000000u, X},
    {"i32.trunc_sat_f64_u", B64(-1.0), 0, 0, X},
    {"i32.trunc_sat_f64_u", B64(-0.9), 0, 0, X},
    {"i32.trunc_sat_f64_u", B64(5e9), 0, 0xFFFFFFFFu, X},
    {"i32.trunc_sat_f64_u", B64(4294967295.5), 0, 0xFFFFFFFFu, X},
    {"i32.trunc_sat_f64_u", NZ64, 0, 0, X},
    {"i32.trunc_sat_f64_u", SNAN64, 0, 0, X},
    {"i32.trunc_sat_f32_s", QNAN32, 0, 0, X},
    {"i32.trunc_sat_f32_s", B32(2147483648.0f), 0, 0x7FFFFFFF, X},
    {"i32.trunc_sat_f32_s", B32(-2147483648.0f), 0, 0x80000000u, X},
    {"i32.trunc_sat_f32_u", B32(4294967040.0f), 0, 0xFFFFFF00u, X},
    {"i32.trunc_sat_f32_u", B32(4294967296.0f), 0, 0xFFFFFFFFu, X},
    {"i64.trunc_sat_f64_s", QNAN64, 0, 0, X},
    {"i64.trunc_sat_f64_s", B64(1e19), 0, 0x7FFFFFFFFFFFFFFFull, X},
    {"i64.trunc_sat_f64_s", B64(-1e19), 0, 0x8000000000000000ull, X},
    {"i64.trunc_sat_f64_s", B64(-9223372036854775808.0), 0, 0x8000000000000000ull, X},
    {"i64.trunc_sat_f64_s", 0x43DFFFFFFFFFFFFFull, 0, 0x7FFFFFFFFFFFFC00ull, X},
    {"i64.trunc_sat_f64_s", B64(-1.9), 0, 0xFFFFFFFFFFFFFFFFull, X},
    {"i64.trunc_sat_f64_u", B64(1e19), 0, 0x8AC7230489E80000ull, X},
    {"i64.trunc_sat_f64_u", B64(18446744073709551616.0), 0, 0xFFFFFFFFFFFFFFFFull, X},
    {"i64.trunc_sat_f64_u", 0x43EFFFFFFFFFFFFFull, 0, 0xFFFFFFFFFFFFF800ull, X},
    {"i64.trunc_sat_f64_u", B64(9223372036854775808.0), 0, 0x8000000000000000ull, X},
    {"i64.trunc_sat_f64_u", 0x43DFFFFFFFFFFFFFull, 0, 0x7FFFFFFFFFFFFC00ull, X},
    {"i64.trunc_sat_f64_u", QNAN64, 0, 0, X},
    {"i64.trunc_sat_f64_u", NINF64, 0, 0, X},
    {"i64.trunc_sat_f64_u", INF64, 0, 0xFFFFFFFFFFFFFFFFull, X},
    {"i64.trunc_sat_f64_u", B64(-0.5), 0, 0, X},
    {"i64.trunc_sat_f32_s", B32(9223372036854775808.0f), 0, 0x7FFFFFFFFFFFFFFFull, X},
    {"i64.trunc_sat_f32_s", 0x5EFFFFFFu, 0, 0x7FFFFF8000000000ull, X},
    {"i64.trunc_sat_f32_u", B32(18446744073709551616.0f), 0, 0xFFFFFFFFFFFFFFFFull, X},
    {"i64.trunc_sat_f32_u", 0x5F7FFFFFu, 0, 0xFFFFFF0000000000ull, X},
    {"i64.trunc_sat_f32_u", QNAN32, 0, 0, X},
    {"i32.trunc_f64_s", B64(-2147483648.9), 0, 0x80000000u, X},
    {"i32.trunc_f64_s", B64(2147483647.9), 0, 0x7FFFFFFF, X},
    {"i32.trunc_f64_s", B64(-1.9), 0, 0xFFFFFFFFu, X},
    {"i32.trunc_f64_u", B64(-0.9), 0, 0, X},
    {"i32.trunc_f64_u", B64(4294967295.9), 0, 0xFFFFFFFFu, X},
    {"i32.trunc_f32_s", B32(-2147483648.0f), 0, 0x80000000u, X},
    {"i32.trunc_f32_s", B32(2147483520.0f), 0, 0x7FFFFF80, X},
    {"i32.trunc_f32_u", B32(4294967040.0f), 0, 0xFFFFFF00u, X},
    {"i32.trunc_f32_u", B32(-0.9f), 0, 0, X},
    {"i64.trunc_f64_s", B64(-9223372036854775808.0), 0, 0x8000000000000000ull, X},
    {"i64.trunc_f64_s", 0x43DFFFFFFFFFFFFFull, 0, 0x7FFFFFFFFFFFFC00ull, X},
    {"i64.trunc_f64_s", B64(-1.9), 0, 0xFFFFFFFFFFFFFFFFull, X},
    {"i64.trunc_f64_u", B64(9223372036854775808.0), 0, 0x8000000000000000ull, X},
    {"i64.trunc_f64_u", 0x43EFFFFFFFFFFFFFull, 0, 0xFFFFFFFFFFFFF800ull, X},
    {"i64.trunc_f64_u", B64(1e19), 0, 0x8AC7230489E80000ull, X},
    {"i64.trunc_f64_u", B64(-0.9), 0, 0, X},
    {"i64.trunc_f32_s", 0x5EFFFFFFu, 0, 0x7FFFFF8000000000ull, X},
    {"i64.trunc_f32_s", 0xDF000000u, 0, 0x8000000000000000ull, X},
    {"i64.trunc_f32_u", 0x5F7FFFFFu, 0, 0xFFFFFF0000000000ull, X},
    {"i64.trunc_f32_u", B32(9223372036854775808.0f), 0, 0x8000000000000000ull, X},
    // promotion, demotion, reinterpretation
    {"f64.promote_f32", B32(1.5f), 0, B64(1.5), X},
    {"f64.promote_f32", NZ32, 0, NZ64, X},
    {"f64.promote_f32", 1, 0, 0x36A0000000000000ull, X},
    {"f64.promote_f32", SNAN32, 0, QNAN64, NA},
    {"f64.promote_f32", INF32, 0, INF64, X},
    {"f64.promote_f32", QNAN32, 0, QNAN64, NC},
    {"f32.demote_f64", B64(1.5), 0, B32(1.5f), X},
    {"f32.demote_f64", B64(1e300), 0, INF32, X},
    {"f32.demote_f64", B64(-1e300), 0, NINF32, X},
    {"f32.demote_f64", 0x3FF0000010000000ull, 0, 0x3F800000u, X},
    {"f32.demote_f64", 0x3FF0000010000001ull, 0, 0x3F800001u, X},
    {"f32.demote_f64", 0x3FF0000030000000ull, 0, 0x3F800002u, X},
    {"f32.demote_f64", B64(1e-50), 0, PZ32, X},
    {"f32.demote_f64", B64(-1e-50), 0, NZ32, X},
    {"f32.demote_f64", SNAN64, 0, QNAN32, NA},
    {"f32.demote_f64", QNAN64, 0, QNAN32, NC},
    {"f32.demote_f64", 0x36A0000000000000ull, 0, 0x00000001u, X},
    {"f32.demote_f64", 0x3690000000000000ull, 0, 0x00000000u, X},
    {"f32.demote_f64", 0x3690000000000001ull, 0, 0x00000001u, X},
    {"f32.demote_f64", 0x47EFFFFFE0000000ull, 0, 0x7F7FFFFFu, X},
    {"f32.reinterpret_i32", 0x7FA00000u, 0, 0x7FA00000u, X},
    {"f32.reinterpret_i32", 0xFFFFFFFFu, 0, 0xFFFFFFFFu, X},
    {"f32.reinterpret_i32", 0x80000000u, 0, NZ32, X},
    {"f64.reinterpret_i64", 0xFFF0000000000001ull, 0, 0xFFF0000000000001ull, X},
    {"f64.reinterpret_i64", 1, 0, 1, X},
    {"i32.reinterpret_f32", SNAN32, 0, SNAN32, X},
    {"i32.reinterpret_f32", NZ32, 0, 0x80000000u, X},
    {"i64.reinterpret_f64", NZ64, 0, 0x8000000000000000ull, X},
    {"i64.reinterpret_f64", 0x7FF4000000000000ull, 0, 0x7FF4000000000000ull, X},
};

unsigned result_width(const std::string & instr) {
  // the width of the result word: f32.* and i32.* produce 32 bits (a comparison answers an i32)
  const std::string ty = instr.substr(0, instr.find('.'));
  const std::string name = instr.substr(instr.find('.') + 1);
  if (ty == "f64" && (name == "eq" || name == "ne" || name == "lt" || name == "le" || name == "gt" ||
                         name == "ge")) {
    return 32;
  }
  if (ty == "f32" && (name == "eq" || name == "ne" || name == "lt" || name == "le" || name == "gt" ||
                         name == "ge")) {
    return 32;
  }
  return ty == "f32" || ty == "i32" ? 32 : 64;
}

/* A compiled operation: its function, the code and the context it runs under. */
struct Built {
  FloatOp op;
  GRJIT_Code * code = nullptr;
  ~Built() {
    grjit_code_destroy(code);
    grjit_function_destroy(op.function);
  }
};

/* The optional CPU features this file's compiles switch off. The tests that run the four roundings (and
 * the integer bit counts, which have feature arms too) run them twice, with what the host has and with
 * every optional feature masked off, so that the v1 sequences, which a host with SSE4.1 never runs
 * otherwise, are run and a planted defect in one of them is caught (story 4). */
GRJIT_Features g_disabled = default_disabled_features();

struct ArmScope {
  GRJIT_Features saved;
  explicit ArmScope(GRJIT_Features d) : saved(g_disabled) { g_disabled = d; }
  ~ArmScope() { g_disabled = saved; }
};

const char * arm_name(GRJIT_Features disabled) {
  return disabled == GRJIT_FEATURES_NONE ? "the features the host has" : "every optional feature masked off";
}

GRJIT_Result compile(Built * b, const std::string & instr, JitWorld & w, TruncGuards g = TruncGuards{}) {
  b->op = build_any_op(instr, g);
  if (b->op.function == nullptr) {
    return GRJIT_ERR_INVALID;
  }
  GRJIT_CompileOptions o{};
  o.pages = w.pages();
  o.disabled_features = g_disabled;
  return grjit_compile(&o, b->op.function, &b->code);
}

const char * site_name(uint64_t offset) {
  return offset == kSiteInvalidConversion  ? "invalid conversion to integer"
         : offset == kSiteIntegerOverflow  ? "integer overflow"
                                           : "?";
}

/* What the code did: the value, or the trap its guard names. */
std::string said(const GRJIT_Code * code, const Outcome & o) {
  if (o.fault != 0) {
    return "HARDWARE FAULT (signal " + std::to_string(o.fault) + ")";
  }
  if (o.exit == GRJIT_EXIT_DEOPT) {
    const GRCORE_CodeSite * s =
        grcore_codemeta_find(grjit_code_meta(code), static_cast<uint32_t>(o.out[0]));
    return std::string("trap: ") + (s != nullptr ? site_name(s->identity.offset) : "no such site");
  }
  if (o.exit == GRJIT_EXIT_RETURNED) {
    return hex(o.out[0]);
  }
  return "exit " + std::to_string(o.exit);
}

std::string refused(GRJIT_Function * f) {
  GRJIT_Result r;
  std::string why = verify_reason(f, &r);
  return r == GRJIT_OK ? "" : (why.empty() ? "(no reason)" : why);
}

} // namespace

/* ---- Types and numbering ---------------------------------------------------------------------- */

TEST(FloatTypes, EveryValueThatExistedBeforeTheFloatsIsWhereItWasAndTheNewOnesAreAppended) {
  EXPECT_EQ(static_cast<int>(GRJIT_TYPE_I64), 0);
  EXPECT_EQ(static_cast<int>(GRJIT_TYPE_REF), 1);
  EXPECT_EQ(static_cast<int>(GRJIT_TYPE_PTR), 2);
  EXPECT_EQ(static_cast<int>(GRJIT_NATIVE_NO_RESULT), 3) << "the native-result sentinel did not move";
  EXPECT_EQ(static_cast<int>(GRJIT_TYPE_I32), 4);
  EXPECT_EQ(static_cast<int>(GRJIT_TYPE_F32), 5);
  EXPECT_EQ(static_cast<int>(GRJIT_TYPE_F64), 6);
  EXPECT_EQ(static_cast<int>(GRJIT_TYPE_COUNT), 7);
  EXPECT_EQ(static_cast<int>(GRJIT_OP_CLZ), 29);
  EXPECT_EQ(static_cast<int>(GRJIT_OP_EXTEND_U), 43);
  EXPECT_EQ(static_cast<int>(GRJIT_OP_FADD), 44) << "the float operations are appended after EXTEND_U";
  const GRJIT_OpKind order[] = {GRJIT_OP_FADD, GRJIT_OP_FSUB, GRJIT_OP_FMUL, GRJIT_OP_FDIV,
      GRJIT_OP_FMIN, GRJIT_OP_FMAX, GRJIT_OP_FCOPYSIGN, GRJIT_OP_FNEG, GRJIT_OP_FABS,
      GRJIT_OP_FSQRT, GRJIT_OP_FCEIL, GRJIT_OP_FFLOOR, GRJIT_OP_FTRUNC, GRJIT_OP_FNEAREST,
      GRJIT_OP_CONVERT_S, GRJIT_OP_CONVERT_U, GRJIT_OP_TRUNC_S, GRJIT_OP_TRUNC_U,
      GRJIT_OP_TRUNC_SAT_S, GRJIT_OP_TRUNC_SAT_U, GRJIT_OP_DEMOTE, GRJIT_OP_PROMOTE,
      GRJIT_OP_REINTERPRET};
  for (size_t i = 0; i < sizeof order / sizeof order[0]; i++) {
    EXPECT_EQ(static_cast<int>(order[i]), 44 + static_cast<int>(i)) << i;
  }
  EXPECT_EQ(static_cast<int>(GRJIT_OP_COUNT), 67);
}

TEST(FloatTypes, ABuilderMakesFloatRegistersAndParametersAndRefusesAnythingPastTheLastType) {
  B b("f");
  GRJIT_VReg v = GRJIT_NO_VREG;
  EXPECT_EQ(grjit_builder_param(b.b, GRJIT_TYPE_F32, &v), GRJIT_OK);
  EXPECT_EQ(grjit_builder_param(b.b, GRJIT_TYPE_F64, &v), GRJIT_OK);
  EXPECT_EQ(grjit_builder_vreg(b.b, GRJIT_TYPE_F32, &v), GRJIT_OK);
  EXPECT_EQ(grjit_builder_vreg(b.b, GRJIT_TYPE_F64, &v), GRJIT_OK);
  EXPECT_EQ(grjit_builder_vreg(b.b, static_cast<GRJIT_Type>(7), &v), GRJIT_ERR_INVALID);
  EXPECT_EQ(grjit_builder_vreg(b.b, static_cast<GRJIT_Type>(3), &v), GRJIT_ERR_INVALID);
  GRJIT_BlockId blk = 0;
  ASSERT_EQ(grjit_builder_block(b.b, &blk), GRJIT_OK);
  ASSERT_EQ(grjit_builder_set_block(b.b, blk), GRJIT_OK);
  // The generic builders take the new kinds, each the way it is shaped.
  for (GRJIT_OpKind k : {GRJIT_OP_FADD, GRJIT_OP_FSUB, GRJIT_OP_FMUL, GRJIT_OP_FDIV, GRJIT_OP_FMIN,
           GRJIT_OP_FMAX, GRJIT_OP_FCOPYSIGN}) {
    EXPECT_EQ(grjit_builder_binary(b.b, k, 2, V(0), V(0)), GRJIT_OK);
    EXPECT_EQ(grjit_builder_unary(b.b, k, 2, V(0)), GRJIT_ERR_INVALID);
  }
  for (GRJIT_OpKind k : {GRJIT_OP_FNEG, GRJIT_OP_FABS, GRJIT_OP_FSQRT, GRJIT_OP_FCEIL,
           GRJIT_OP_FFLOOR, GRJIT_OP_FTRUNC, GRJIT_OP_FNEAREST, GRJIT_OP_CONVERT_S,
           GRJIT_OP_CONVERT_U, GRJIT_OP_TRUNC_S, GRJIT_OP_TRUNC_U, GRJIT_OP_TRUNC_SAT_S,
           GRJIT_OP_TRUNC_SAT_U, GRJIT_OP_DEMOTE, GRJIT_OP_PROMOTE, GRJIT_OP_REINTERPRET}) {
    EXPECT_EQ(grjit_builder_unary(b.b, k, 2, V(0)), GRJIT_OK);
    EXPECT_EQ(grjit_builder_binary(b.b, k, 2, V(0), V(0)), GRJIT_ERR_INVALID);
  }
  EXPECT_EQ(grjit_builder_unary(b.b, GRJIT_OP_COUNT, 2, V(0)), GRJIT_ERR_INVALID);
  EXPECT_EQ(grjit_builder_binary(b.b, static_cast<GRJIT_OpKind>(GRJIT_OP_COUNT + 1), 2, V(0), V(0)),
      GRJIT_ERR_INVALID);
}

TEST(FloatTypes, NativesTakeAndReturnNoFloat) {
  NativeTab t;
  for (GRJIT_Type ft : {GRJIT_TYPE_F32, GRJIT_TYPE_F64}) {
    GRJIT_NativeDesc d{};
    d.address = 0x1000;
    GRJIT_Type params[] = {ft};
    d.params = params;
    d.param_count = 1;
    d.result = GRJIT_NATIVE_NO_RESULT;
    EXPECT_EQ(grjit_native_table_add(t.t, &d, nullptr), GRJIT_ERR_INVALID);
    d.param_count = 0;
    d.result = ft;
    EXPECT_EQ(grjit_native_table_add(t.t, &d, nullptr), GRJIT_ERR_INVALID);
  }
}

TEST(FloatPrint, TheTypesPrintAsF32AndF64AndTheOperationsByTheirWasmName) {
  struct Case {
    const char * instr;
    const char * fragment;
  };
  const Case cases[] = {
      {"f64.add", "v2 = f64.add v0, v1"},
      {"f32.sub", "v2 = f32.sub v0, v1"},
      {"f32.copysign", "v2 = f32.copysign v0, v1"},
      {"f64.nearest", "v1 = f64.nearest v0"},
      {"f32.sqrt", "v1 = f32.sqrt v0"},
      {"f64.lt", "v2 = cmp.f64.lt v0, v1"},
      {"f32.ne", "v2 = cmp.f32.ne v0, v1"},
      {"f32.convert_i64_u", "v1 = f32.convert_i64_u v0"},
      {"f64.convert_i32_s", "v1 = f64.convert_i32_s v0"},
      {"i32.trunc_f64_s", "i32.trunc_f64_s v0"},
      {"i64.trunc_f32_u", "i64.trunc_f32_u v0"},
      {"i32.trunc_sat_f32_u", "v1 = i32.trunc_sat_f32_u v0"},
      {"f32.demote_f64", "v1 = f32.demote_f64 v0"},
      {"f64.promote_f32", "v1 = f64.promote_f32 v0"},
      {"i32.reinterpret_f32", "v1 = i32.reinterpret_f32 v0"},
      {"f64.reinterpret_i64", "v1 = f64.reinterpret_i64 v0"},
  };
  for (const Case & c : cases) {
    SCOPED_TRACE(c.instr);
    Built b;
    b.op = build_any_op(c.instr);
    ASSERT_NE(b.op.function, nullptr);
    const std::string text = print(b.op.function);
    EXPECT_NE(text.find(c.fragment), std::string::npos) << text;
  }
  Built f;
  f.op = build_any_op("f32.add");
  EXPECT_NE(print(f.op.function).find("v0:f32, v1:f32"), std::string::npos);
  Built d;
  d.op = build_any_op("f64.sub");
  EXPECT_NE(print(d.op.function).find("v0:f64, v1:f64"), std::string::npos);
  // A float immediate is its bit pattern, in hex; a typed one names its width.
  B b("imm", 0);
  GRJIT_VReg x = b.param(GRJIT_TYPE_F64), y = b.reg(GRJIT_TYPE_F64), z = b.reg(GRJIT_TYPE_F32),
             w = b.reg(GRJIT_TYPE_I32);
  b.at(b.block());
  b.cnst(y, static_cast<int64_t>(B64(1.5)));
  b.bin(GRJIT_OP_FADD, y, V(x), I(static_cast<int64_t>(B64(2.0))));
  b.cnst(z, static_cast<int64_t>(B32(1.0f)));
  b.cmp(GRJIT_CMP_LT, w, V(x), I(static_cast<int64_t>(B64(-1.0))));
  GRJIT_Operand args[] = {grjit_operand_f32_imm(B32(1.0f)), grjit_operand_f64_imm(B64(2.0)), V(y)};
  EXPECT_EQ(grjit_builder_call(b.b, GRJIT_NO_VREG, 0x1000, GRJIT_CALL_NO_GC, GRCORE_SITE_GC_POINT_CALL,
                args, 3, GRCORE_PollIdentity{0, 0}, nullptr, 0),
      GRJIT_OK);
  b.ret(V(z));
  Fn fn(b.finish());
  const std::string text = print(fn);
  EXPECT_NE(text.find("const 0x3ff8000000000000"), std::string::npos) << text;
  EXPECT_NE(text.find("f64.add v0, #0x4000000000000000"), std::string::npos) << text;
  EXPECT_NE(text.find("const 0x3f800000"), std::string::npos) << text;
  EXPECT_NE(text.find("cmp.f64.lt v0, #0xbff0000000000000"), std::string::npos) << text;
  EXPECT_NE(text.find("#f32:0x3f800000, #f64:0x4000000000000000, v1"), std::string::npos) << text;
}

/* ---- The verifier: typing ---------------------------------------------------------------------- */

TEST(FloatVerify, EveryFloatOperationTakesFloatOperandsOfItsOwnWidthAndWritesThatType) {
  struct Case {
    const char * what;
    std::function<void(B &)> build;
    bool ok;
  };
  const GRJIT_Type f32 = GRJIT_TYPE_F32, f64 = GRJIT_TYPE_F64, i32 = GRJIT_TYPE_I32,
                   i64 = GRJIT_TYPE_I64;
  auto bin = [&](GRJIT_OpKind k, GRJIT_Type a, GRJIT_Type b2, GRJIT_Type d) {
    return [=](B & b) {
      GRJIT_VReg x = b.param(a), y = b.param(b2), r = b.reg(d);
      b.at(b.block());
      b.bin(k, r, V(x), V(y));
      b.ret(V(r));
    };
  };
  auto un = [&](GRJIT_OpKind k, GRJIT_Type a, GRJIT_Type d) {
    return [=](B & b) {
      GRJIT_VReg x = b.param(a), r = b.reg(d);
      b.at(b.block());
      b.un(k, r, V(x));
      b.ret(V(r));
    };
  };
  std::vector<Case> cases = {
      {"fadd f32 f32 -> f32", bin(GRJIT_OP_FADD, f32, f32, f32), true},
      {"fadd f64 f64 -> f64", bin(GRJIT_OP_FADD, f64, f64, f64), true},
      {"fadd f32 f32 -> f64", bin(GRJIT_OP_FADD, f32, f32, f64), false},
      {"fadd f64 f64 -> f32", bin(GRJIT_OP_FADD, f64, f64, f32), false},
      {"fadd f32 f64 -> f32", bin(GRJIT_OP_FADD, f32, f64, f32), false},
      {"fadd i64 i64 -> f64", bin(GRJIT_OP_FADD, i64, i64, f64), false},
      {"fadd f64 f64 -> i64", bin(GRJIT_OP_FADD, f64, f64, i64), false},
      {"fadd i32 i32 -> f32", bin(GRJIT_OP_FADD, i32, i32, f32), false},
      {"fmin f64 f64 -> f64", bin(GRJIT_OP_FMIN, f64, f64, f64), true},
      {"fcopysign f32 f64 -> f32", bin(GRJIT_OP_FCOPYSIGN, f32, f64, f32), false},
      {"fneg f64 -> f64", un(GRJIT_OP_FNEG, f64, f64), true},
      {"fneg f64 -> f32", un(GRJIT_OP_FNEG, f64, f32), false},
      {"fsqrt i64 -> f64", un(GRJIT_OP_FSQRT, i64, f64), false},
      {"fnearest f32 -> f32", un(GRJIT_OP_FNEAREST, f32, f32), true},
      {"add f64 f64 -> f64 (an integer operation on floats)", bin(GRJIT_OP_ADD, f64, f64, f64), false},
      {"add f32 f32 -> f32", bin(GRJIT_OP_ADD, f32, f32, f32), false},
      {"sub i64 f64 -> i64", bin(GRJIT_OP_SUB, i64, f64, i64), false},
      {"mul f64 i64 -> i64", bin(GRJIT_OP_MUL, f64, i64, i64), false},
      {"shl f64 i64 -> f64", bin(GRJIT_OP_SHL, f64, i64, f64), false},
      {"neg f64 -> f64 (the integer neg)", un(GRJIT_OP_NEG, f64, f64), false},
      {"not f32 -> f32", un(GRJIT_OP_NOT, f32, f32), false},
      {"clz f64 -> i64", un(GRJIT_OP_CLZ, f64, i64), false},
      {"wrap f64 -> i32", un(GRJIT_OP_WRAP, f64, i32), false},
      {"extend_s f32 -> i64", un(GRJIT_OP_EXTEND_S, f32, i64), false},
      {"rotl f64 f64 -> f64", bin(GRJIT_OP_ROTL, f64, f64, f64), false},
      {"div_s f64 f64 -> f64", bin(GRJIT_OP_DIV_S, f64, f64, f64), false},
      {"convert_s i32 -> f32", un(GRJIT_OP_CONVERT_S, i32, f32), true},
      {"convert_s i64 -> f64", un(GRJIT_OP_CONVERT_S, i64, f64), true},
      {"convert_u i64 -> f32", un(GRJIT_OP_CONVERT_U, i64, f32), true},
      {"convert_u f64 -> f64", un(GRJIT_OP_CONVERT_U, f64, f64), false},
      {"convert_s i64 -> i64", un(GRJIT_OP_CONVERT_S, i64, i64), false},
      {"convert_s ptr -> f64", un(GRJIT_OP_CONVERT_S, GRJIT_TYPE_PTR, f64), false},
      {"trunc_sat_s f32 -> i32", un(GRJIT_OP_TRUNC_SAT_S, f32, i32), true},
      {"trunc_sat_u f64 -> i64", un(GRJIT_OP_TRUNC_SAT_U, f64, i64), true},
      {"trunc_sat_s i64 -> i64", un(GRJIT_OP_TRUNC_SAT_S, i64, i64), false},
      {"trunc_sat_s f64 -> f64", un(GRJIT_OP_TRUNC_SAT_S, f64, f64), false},
      {"trunc_sat_s f64 -> ptr", un(GRJIT_OP_TRUNC_SAT_S, f64, GRJIT_TYPE_PTR), false},
      {"demote f64 -> f32", un(GRJIT_OP_DEMOTE, f64, f32), true},
      {"demote f32 -> f32", un(GRJIT_OP_DEMOTE, f32, f32), false},
      {"demote f64 -> f64", un(GRJIT_OP_DEMOTE, f64, f64), false},
      {"promote f32 -> f64", un(GRJIT_OP_PROMOTE, f32, f64), true},
      {"promote f64 -> f64", un(GRJIT_OP_PROMOTE, f64, f64), false},
      {"promote f32 -> f32", un(GRJIT_OP_PROMOTE, f32, f32), false},
      {"reinterpret f32 -> i32", un(GRJIT_OP_REINTERPRET, f32, i32), true},
      {"reinterpret i32 -> f32", un(GRJIT_OP_REINTERPRET, i32, f32), true},
      {"reinterpret f64 -> i64", un(GRJIT_OP_REINTERPRET, f64, i64), true},
      {"reinterpret i64 -> f64", un(GRJIT_OP_REINTERPRET, i64, f64), true},
      {"reinterpret f32 -> i64", un(GRJIT_OP_REINTERPRET, f32, i64), false},
      {"reinterpret f64 -> i32", un(GRJIT_OP_REINTERPRET, f64, i32), false},
      {"reinterpret i32 -> f64", un(GRJIT_OP_REINTERPRET, i32, f64), false},
      {"reinterpret f32 -> f32", un(GRJIT_OP_REINTERPRET, f32, f32), false},
      {"reinterpret ref -> f64", un(GRJIT_OP_REINTERPRET, GRJIT_TYPE_REF, f64), false},
      {"bitcast f64 -> i64", [&](B & b) {
         GRJIT_VReg x = b.param(f64), r = b.reg(i64);
         b.at(b.block()); b.bitcast(r, x); b.ret(V(r)); }, false},
      {"bitcast i64 -> f64", [&](B & b) {
         GRJIT_VReg x = b.param(i64), r = b.reg(f64);
         b.at(b.block()); b.bitcast(r, x); b.ret(V(r)); }, false},
      {"move f64 -> f64", [&](B & b) {
         GRJIT_VReg x = b.param(f64), r = b.reg(f64);
         b.at(b.block()); b.mov(r, V(x)); b.ret(V(r)); }, true},
      {"move f32 -> f32 from a 32-bit immediate", [&](B & b) {
         GRJIT_VReg r = b.reg(f32);
         b.at(b.block()); b.mov(r, I(0x3F800000)); b.ret(V(r)); }, true},
      {"move f32 from an immediate wider than 32 bits", [&](B & b) {
         GRJIT_VReg r = b.reg(f32);
         b.at(b.block()); b.mov(r, I(0x100000000ll)); b.ret(V(r)); }, false},
      {"move f32 from a negative immediate", [&](B & b) {
         GRJIT_VReg r = b.reg(f32);
         b.at(b.block()); b.mov(r, I(-1)); b.ret(V(r)); }, false},
      {"move f32 -> f64", [&](B & b) {
         GRJIT_VReg x = b.param(f32), r = b.reg(f64);
         b.at(b.block()); b.mov(r, V(x)); b.ret(V(r)); }, false},
      {"move f64 -> i64", [&](B & b) {
         GRJIT_VReg x = b.param(f64), r = b.reg(i64);
         b.at(b.block()); b.mov(r, V(x)); b.ret(V(r)); }, false},
      {"move i64 -> f64", [&](B & b) {
         GRJIT_VReg x = b.param(i64), r = b.reg(f64);
         b.at(b.block()); b.mov(r, V(x)); b.ret(V(r)); }, false},
      {"const f64 any pattern", [&](B & b) {
         GRJIT_VReg r = b.reg(f64);
         b.at(b.block()); b.cnst(r, INT64_MIN); b.ret(V(r)); }, true},
      {"const f32 of 0xFFFFFFFF", [&](B & b) {
         GRJIT_VReg r = b.reg(f32);
         b.at(b.block()); b.cnst(r, 0xFFFFFFFFll); b.ret(V(r)); }, true},
      {"const f32 of 2^32", [&](B & b) {
         GRJIT_VReg r = b.reg(f32);
         b.at(b.block()); b.cnst(r, 0x100000000ll); b.ret(V(r)); }, false},
      {"const f32 of -1", [&](B & b) {
         GRJIT_VReg r = b.reg(f32);
         b.at(b.block()); b.cnst(r, -1); b.ret(V(r)); }, false},
      {"fadd of an f32 immediate pattern", [&](B & b) {
         GRJIT_VReg x = b.param(f32), r = b.reg(f32);
         b.at(b.block()); b.bin(GRJIT_OP_FADD, r, V(x), I(0x3F800000)); b.ret(V(r)); }, true},
      {"fadd of an f32 immediate wider than 32 bits", [&](B & b) {
         GRJIT_VReg x = b.param(f32), r = b.reg(f32);
         b.at(b.block()); b.bin(GRJIT_OP_FADD, r, V(x), I(0x1FFFFFFFFll)); b.ret(V(r)); }, false},
      {"fadd of a typed f64 immediate on f32", [&](B & b) {
         GRJIT_VReg x = b.param(f32), r = b.reg(f32);
         b.at(b.block()); b.bin(GRJIT_OP_FADD, r, V(x), grjit_operand_f64_imm(0)); b.ret(V(r)); }, false},
      {"fadd of two immediates", [&](B & b) {
         GRJIT_VReg r = b.reg(f64);
         b.at(b.block()); b.bin(GRJIT_OP_FADD, r, I(1), I(2)); b.ret(V(r)); }, true},
      {"add of a typed immediate", [&](B & b) {
         GRJIT_VReg x = b.param(i64), r = b.reg(i64);
         b.at(b.block()); b.bin(GRJIT_OP_ADD, r, V(x), grjit_operand_f64_imm(1)); b.ret(V(r)); }, false},
      {"convert from an immediate (no width to say which)", [&](B & b) {
         GRJIT_VReg r = b.reg(f64);
         b.at(b.block()); b.un(GRJIT_OP_CONVERT_S, r, I(1)); b.ret(V(r)); }, false},
      {"trunc_sat from an immediate", [&](B & b) {
         GRJIT_VReg r = b.reg(i32);
         b.at(b.block()); b.un(GRJIT_OP_TRUNC_SAT_S, r, I(0x3F800000)); b.ret(V(r)); }, false},
      {"reinterpret of an immediate", [&](B & b) {
         GRJIT_VReg r = b.reg(f32);
         b.at(b.block()); b.un(GRJIT_OP_REINTERPRET, r, I(0x3F800000)); b.ret(V(r)); }, false},
  };
  for (const Case & c : cases) {
    SCOPED_TRACE(c.what);
    B b("t", 0);
    c.build(b);
    Fn f(b.finish());
    const std::string why = refused(f.f);
    if (c.ok) {
      EXPECT_EQ(why, "");
    } else {
      EXPECT_NE(why, "") << "accepted";
    }
  }
}

TEST(FloatVerify, AComparisonOfFloatsIsOrderedOfOneWidthAndNeverUnsigned) {
  struct Case {
    const char * what;
    GRJIT_Cmp cmp;
    GRJIT_Type a, b, dst;
    bool imm_b;
    bool ok;
  };
  const GRJIT_Type f32 = GRJIT_TYPE_F32, f64 = GRJIT_TYPE_F64, i32 = GRJIT_TYPE_I32,
                   i64 = GRJIT_TYPE_I64;
  const Case cases[] = {
      {"f64 lt -> i32", GRJIT_CMP_LT, f64, f64, i32, false, true},
      {"f64 lt -> i64", GRJIT_CMP_LT, f64, f64, i64, false, true},
      {"f32 ge -> i32", GRJIT_CMP_GE, f32, f32, i32, false, true},
      {"f32 eq with an immediate", GRJIT_CMP_EQ, f32, f32, i32, true, true},
      {"f64 ne with an immediate", GRJIT_CMP_NE, f64, f64, i64, true, true},
      {"f64 ult", GRJIT_CMP_ULT, f64, f64, i32, false, false},
      {"f64 ule", GRJIT_CMP_ULE, f64, f64, i32, false, false},
      {"f32 ugt", GRJIT_CMP_UGT, f32, f32, i32, false, false},
      {"f32 uge", GRJIT_CMP_UGE, f32, f32, i32, false, false},
      {"f32 vs f64", GRJIT_CMP_EQ, f32, f64, i32, false, false},
      {"f64 vs i64", GRJIT_CMP_EQ, f64, i64, i32, false, false},
      {"i64 vs f64", GRJIT_CMP_LT, i64, f64, i32, false, false},
      {"i32 vs f32", GRJIT_CMP_EQ, i32, f32, i32, false, false},
      {"f64 lt -> f64", GRJIT_CMP_LT, f64, f64, f64, false, false},
      {"f64 lt -> f32", GRJIT_CMP_LT, f64, f64, f32, false, false},
      {"f64 lt -> ptr", GRJIT_CMP_LT, f64, f64, GRJIT_TYPE_PTR, false, false},
  };
  for (const Case & c : cases) {
    SCOPED_TRACE(c.what);
    B b("t", 0);
    GRJIT_VReg x = b.param(c.a);
    GRJIT_VReg y = c.imm_b ? GRJIT_NO_VREG : b.param(c.b);
    GRJIT_VReg r = b.reg(c.dst);
    b.at(b.block());
    b.cmp(c.cmp, r, V(x), c.imm_b ? I(0x3F800000) : V(y));
    b.ret(V(r));
    Fn f(b.finish());
    const std::string why = refused(f.f);
    EXPECT_EQ(why.empty(), c.ok) << why;
  }
}

TEST(FloatVerify, AFloatIsNotAConditionAnAddressAnAccessOfTheWrongWidthOrAStackMapEntry) {
  const GRJIT_Type f32 = GRJIT_TYPE_F32, f64 = GRJIT_TYPE_F64;
  struct Case {
    const char * what;
    std::function<void(B &)> build;
    bool ok;
  };
  std::vector<Case> cases = {
      {"guard on an f64", [&](B & b) {
         GRJIT_VReg x = b.param(f64);
         b.at(b.block()); b.guard(V(x), {1, 1}); b.ret(V(x)); }, false},
      {"guard on an f32", [&](B & b) {
         GRJIT_VReg x = b.param(f32);
         b.at(b.block()); b.guard(V(x), {1, 1}); b.ret(V(x)); }, false},
      {"br_if on an f64", [&](B & b) {
         GRJIT_VReg x = b.param(f64);
         GRJIT_BlockId a = b.block(), c = b.block();
         b.at(a); b.br_if(V(x), c, c); b.at(c); b.ret(V(x)); }, false},
      {"a float as a memory base", [&](B & b) {
         GRJIT_VReg p = b.param(f64), r = b.reg(f64);
         b.at(b.block()); b.load(r, p, 0, 64); b.ret(V(r)); }, false},
      {"load f64 of 64 bits", [&](B & b) {
         GRJIT_VReg p = b.param(GRJIT_TYPE_PTR), r = b.reg(f64);
         b.at(b.block()); b.load(r, p, 0, 64); b.ret(V(r)); }, true},
      {"load f32 of 32 bits", [&](B & b) {
         GRJIT_VReg p = b.param(GRJIT_TYPE_PTR), r = b.reg(f32);
         b.at(b.block()); b.load(r, p, 0, 32); b.ret(V(r)); }, true},
      {"load f64 of 32 bits", [&](B & b) {
         GRJIT_VReg p = b.param(GRJIT_TYPE_PTR), r = b.reg(f64);
         b.at(b.block()); b.load(r, p, 0, 32); b.ret(V(r)); }, false},
      {"load f32 of 64 bits", [&](B & b) {
         GRJIT_VReg p = b.param(GRJIT_TYPE_PTR), r = b.reg(f32);
         b.at(b.block()); b.load(r, p, 0, 64); b.ret(V(r)); }, false},
      {"load f32 of 16 bits", [&](B & b) {
         GRJIT_VReg p = b.param(GRJIT_TYPE_PTR), r = b.reg(f32);
         b.at(b.block()); b.load(r, p, 0, 16); b.ret(V(r)); }, false},
      {"sign-extending load into an f32", [&](B & b) {
         GRJIT_VReg p = b.param(GRJIT_TYPE_PTR), r = b.reg(f32);
         b.at(b.block()); b.load(r, p, 0, 32, true); b.ret(V(r)); }, false},
      {"sign-extending load into an f64", [&](B & b) {
         GRJIT_VReg p = b.param(GRJIT_TYPE_PTR), r = b.reg(f64);
         b.at(b.block()); b.load(r, p, 0, 64, true); b.ret(V(r)); }, false},
      {"store f64 of 64 bits", [&](B & b) {
         GRJIT_VReg p = b.param(GRJIT_TYPE_PTR), x = b.param(f64);
         b.at(b.block()); b.store(p, 0, 64, V(x)); b.ret(V(x)); }, true},
      {"store f32 of 32 bits", [&](B & b) {
         GRJIT_VReg p = b.param(GRJIT_TYPE_PTR), x = b.param(f32);
         b.at(b.block()); b.store(p, 0, 32, V(x)); b.ret(V(x)); }, true},
      {"store f64 of 32 bits", [&](B & b) {
         GRJIT_VReg p = b.param(GRJIT_TYPE_PTR), x = b.param(f64);
         b.at(b.block()); b.store(p, 0, 32, V(x)); b.ret(V(x)); }, false},
      {"store f32 of 64 bits", [&](B & b) {
         GRJIT_VReg p = b.param(GRJIT_TYPE_PTR), x = b.param(f32);
         b.at(b.block()); b.store(p, 0, 64, V(x)); b.ret(V(x)); }, false},
  };
  for (const Case & c : cases) {
    SCOPED_TRACE(c.what);
    B b("t", 0);
    c.build(b);
    Fn f(b.finish());
    const std::string why = refused(f.f);
    EXPECT_EQ(why.empty(), c.ok) << why;
  }
}

TEST(FloatVerify, AFloatIsNeverADerivedPointerOrItsBase) {
  for (int which = 0; which < 2; which++) {
    SCOPED_TRACE(which);
    B b("t", 0);
    GRJIT_VReg a = b.param(which == 0 ? GRJIT_TYPE_REF : GRJIT_TYPE_F64);
    GRJIT_VReg d = b.reg(which == 0 ? GRJIT_TYPE_F64 : GRJIT_TYPE_PTR);
    b.at(b.block());
    b.ret(V(a));
    // Declaring it is refused by the builder or, failing that, by the verifier.
    GRJIT_Result declared = grjit_builder_derived(b.b, d, a, 8);
    if (declared != GRJIT_OK) {
      EXPECT_EQ(declared, GRJIT_ERR_INVALID);
      continue;
    }
    Fn f(b.finish());
    EXPECT_NE(refused(f.f), "");
  }
}

/* ---- The verifier: a truncation needs its range guard ----------------------------------------- */

namespace {

struct TruncKind {
  const char * instr;
  GRJIT_Type src, dst;
  bool is_signed;
};

const TruncKind kTruncs[] = {
    {"i32.trunc_f32_s", GRJIT_TYPE_F32, GRJIT_TYPE_I32, true},
    {"i32.trunc_f32_u", GRJIT_TYPE_F32, GRJIT_TYPE_I32, false},
    {"i32.trunc_f64_s", GRJIT_TYPE_F64, GRJIT_TYPE_I32, true},
    {"i32.trunc_f64_u", GRJIT_TYPE_F64, GRJIT_TYPE_I32, false},
    {"i64.trunc_f32_s", GRJIT_TYPE_F32, GRJIT_TYPE_I64, true},
    {"i64.trunc_f32_u", GRJIT_TYPE_F32, GRJIT_TYPE_I64, false},
    {"i64.trunc_f64_s", GRJIT_TYPE_F64, GRJIT_TYPE_I64, true},
    {"i64.trunc_f64_u", GRJIT_TYPE_F64, GRJIT_TYPE_I64, false},
};

/* The immediates of the exact bounds of a truncation, as the operand's bit pattern. */
struct Imm {
  int64_t lo, hi;
  GRJIT_Cmp lo_cmp; // GT, or GE where the exclusive bound is no number of the source type
};

Imm exact_imms(const TruncKind & k) {
  const bool dbl = k.src == GRJIT_TYPE_F64;
  const bool wide = k.dst == GRJIT_TYPE_I64;
  Bounds bd = trunc_bounds(k.is_signed, wide);
  double lo = bd.lo;
  bool closed = bd.lo_closed;
  if (!dbl && static_cast<double>(static_cast<float>(lo)) != lo) {
    lo = static_cast<double>(static_cast<float>(lo));
    closed = true;
  }
  return {static_cast<int64_t>(imm_bits(lo, dbl)), static_cast<int64_t>(imm_bits(bd.hi, dbl)),
      closed ? GRJIT_CMP_GE : GRJIT_CMP_GT};
}

/* A truncation function whose body between the parameter and the truncation is `body`. */
GRJIT_Function * trunc_function(const TruncKind & k, const std::function<void(B &, GRJIT_VReg)> & body,
    bool terminate = true, GRJIT_VReg * x_out = nullptr, std::function<void(B &, GRJIT_VReg)> after = nullptr) {
  B b("t", 0);
  GRJIT_VReg x = b.param(k.src), r = b.reg(k.dst);
  b.at(b.block());
  body(b, x);
  if (after) {
    after(b, x);
  }
  b.un(k.is_signed ? GRJIT_OP_TRUNC_S : GRJIT_OP_TRUNC_U, r, V(x));
  if (terminate) {
    b.ret(V(r));
  }
  if (x_out != nullptr) {
    *x_out = x;
  }
  return b.finish();
}

void lower_and_upper(B & b, GRJIT_VReg x, const Imm & m, GRJIT_Cmp lo_cmp, GRJIT_Cmp hi_cmp,
    bool join_with_or = false) {
  GRJIT_VReg l = b.reg(GRJIT_TYPE_I32), h = b.reg(GRJIT_TYPE_I32), ok = b.reg(GRJIT_TYPE_I32);
  b.cmp(lo_cmp, l, V(x), I(m.lo));
  b.cmp(hi_cmp, h, V(x), I(m.hi));
  b.bin(join_with_or ? GRJIT_OP_OR : GRJIT_OP_AND, ok, V(l), V(h));
  b.guard(V(ok), {kTrapFunction, kSiteIntegerOverflow});
}

} // namespace

TEST(FloatVerify, TheGoldenShapeOfEachTruncationIsAccepted) {
  for (const TruncKind & k : kTruncs) {
    SCOPED_TRACE(k.instr);
    Built b;
    b.op = build_float_op(k.instr);
    ASSERT_NE(b.op.function, nullptr);
    EXPECT_EQ(refused(b.op.function), "");
    EXPECT_TRUE(b.op.partial);
  }
}

TEST(FloatVerify, ATruncationWithNoRangeGuardIsRefusedForEachOperationAndWidth) {
  for (const TruncKind & k : kTruncs) {
    SCOPED_TRACE(k.instr);
    for (bool nan_guard : {false, true}) {
      Built b;
      b.op = build_float_op(k.instr, TruncGuards{nan_guard, false});
      ASSERT_NE(b.op.function, nullptr);
      const std::string why = refused(b.op.function);
      EXPECT_NE(why, "") << "accepted with" << (nan_guard ? "" : "out") << " the NaN guard";
      EXPECT_NE(why.find("range guard"), std::string::npos) << why;
      EXPECT_NE(why.find(k.instr), std::string::npos) << "the reason names the operation: " << why;
    }
  }
}

TEST(FloatVerify, TheNanGuardAloneIsNotTheRangeGuardAndTheRangeGuardAloneIs) {
  for (const TruncKind & k : kTruncs) {
    SCOPED_TRACE(k.instr);
    Built nan_only;
    nan_only.op = build_float_op(k.instr, TruncGuards{true, false});
    EXPECT_NE(refused(nan_only.op.function), "");
    Built range_only;
    range_only.op = build_float_op(k.instr, TruncGuards{false, true});
    EXPECT_EQ(refused(range_only.op.function), "")
        << "the range guard is built from ordered compares, so it also keeps a NaN out; the NaN guard in "
           "front of it is only what names the right trap";
  }
}

TEST(FloatVerify, ABoundOneUlpTooWideIsRefusedAndOneUlpNarrowerIsAccepted) {
  for (const TruncKind & k : kTruncs) {
    SCOPED_TRACE(k.instr);
    for (int widen : {1, 2}) {
      TruncGuards lo;
      lo.widen_lo = widen;
      Built a;
      a.op = build_float_op(k.instr, lo);
      EXPECT_NE(refused(a.op.function), "") << "the lower bound " << widen << " ulp too wide";
      TruncGuards hi;
      hi.widen_hi = widen;
      Built b;
      b.op = build_float_op(k.instr, hi);
      EXPECT_NE(refused(b.op.function), "") << "the upper bound " << widen << " ulp too wide";
    }
    TruncGuards narrow_lo;
    narrow_lo.widen_lo = -1;
    Built c;
    c.op = build_float_op(k.instr, narrow_lo);
    EXPECT_EQ(refused(c.op.function), "") << "a narrower guard is sound";
    TruncGuards narrow_hi;
    narrow_hi.widen_hi = -1;
    Built d;
    d.op = build_float_op(k.instr, narrow_hi);
    EXPECT_EQ(refused(d.op.function), "");
  }
}

TEST(FloatVerify, AnInclusiveLowerBoundWhereTheExactOneIsExclusiveIsRefused) {
  for (const TruncKind & k : kTruncs) {
    if (k.dst == GRJIT_TYPE_I64 && k.is_signed) {
      continue; // its exact bound is the inclusive -2^63
    }
    if (k.dst == GRJIT_TYPE_I32 && k.is_signed && k.src == GRJIT_TYPE_F32) {
      continue; // x >= -2^31 is the exact test on a float
    }
    SCOPED_TRACE(k.instr);
    TruncGuards g;
    g.lower_closed = true;
    Built a;
    a.op = build_float_op(k.instr, g);
    EXPECT_NE(refused(a.op.function), "") << "x >= bound lets the bound itself through";
  }
}

TEST(FloatVerify, TheGuardsMayBeTwoGuardsMirroredOrJoinedInEitherOrderButNotByOr) {
  for (const TruncKind & k : kTruncs) {
    SCOPED_TRACE(k.instr);
    const Imm m = exact_imms(k);
    const GRJIT_Cmp lo_cmp = m.lo_cmp;
    // two guards, one bound each
    {
      Fn f(trunc_function(k, [&](B & b, GRJIT_VReg x) {
        GRJIT_VReg l = b.reg(GRJIT_TYPE_I32), h = b.reg(GRJIT_TYPE_I32);
        b.cmp(lo_cmp, l, V(x), I(m.lo));
        b.guard(V(l), {kTrapFunction, kSiteIntegerOverflow});
        b.cmp(GRJIT_CMP_LT, h, V(x), I(m.hi));
        b.guard(V(h), {kTrapFunction, kSiteIntegerOverflow});
      }));
      EXPECT_EQ(refused(f.f), "") << "two guards, one bound each";
    }
    // the upper first, then the lower
    {
      Fn f(trunc_function(k, [&](B & b, GRJIT_VReg x) {
        GRJIT_VReg l = b.reg(GRJIT_TYPE_I32), h = b.reg(GRJIT_TYPE_I32);
        b.cmp(GRJIT_CMP_LT, h, V(x), I(m.hi));
        b.guard(V(h), {kTrapFunction, kSiteIntegerOverflow});
        b.cmp(lo_cmp, l, V(x), I(m.lo));
        b.guard(V(l), {kTrapFunction, kSiteIntegerOverflow});
      }));
      EXPECT_EQ(refused(f.f), "");
    }
    // mirrored: lo < x (or lo <= x) and hi > x
    {
      Fn f(trunc_function(k, [&](B & b, GRJIT_VReg x) {
        GRJIT_VReg l = b.reg(GRJIT_TYPE_I32), h = b.reg(GRJIT_TYPE_I32), ok = b.reg(GRJIT_TYPE_I32);
        b.cmp(lo_cmp == GRJIT_CMP_GT ? GRJIT_CMP_LT : GRJIT_CMP_LE, l, I(m.lo), V(x));
        b.cmp(GRJIT_CMP_GT, h, I(m.hi), V(x));
        b.bin(GRJIT_OP_AND, ok, V(h), V(l));
        b.guard(V(ok), {kTrapFunction, kSiteIntegerOverflow});
      }));
      EXPECT_EQ(refused(f.f), "") << "the mirrored compares, joined in the other order";
    }
    // a lower bound alone, an upper bound alone
    {
      Fn f(trunc_function(k, [&](B & b, GRJIT_VReg x) {
        GRJIT_VReg l = b.reg(GRJIT_TYPE_I32);
        b.cmp(lo_cmp, l, V(x), I(m.lo));
        b.guard(V(l), {kTrapFunction, kSiteIntegerOverflow});
      }));
      EXPECT_NE(refused(f.f), "") << "only the lower bound";
    }
    {
      Fn f(trunc_function(k, [&](B & b, GRJIT_VReg x) {
        GRJIT_VReg h = b.reg(GRJIT_TYPE_I32);
        b.cmp(GRJIT_CMP_LT, h, V(x), I(m.hi));
        b.guard(V(h), {kTrapFunction, kSiteIntegerOverflow});
      }));
      EXPECT_NE(refused(f.f), "") << "only the upper bound";
    }
    // OR instead of AND
    {
      Fn f(trunc_function(k, [&](B & b, GRJIT_VReg x) {
        lower_and_upper(b, x, m, lo_cmp, GRJIT_CMP_LT, true);
      }));
      EXPECT_NE(refused(f.f), "") << "an OR passes a value that satisfies one bound";
    }
    // a compare that is no bound: equality, not-equal, the wrong direction
    for (GRJIT_Cmp bad_hi : {GRJIT_CMP_EQ, GRJIT_CMP_NE, GRJIT_CMP_GT, GRJIT_CMP_GE}) {
      Fn f(trunc_function(k, [&](B & b, GRJIT_VReg x) {
        lower_and_upper(b, x, m, lo_cmp, bad_hi);
      }));
      EXPECT_NE(refused(f.f), "") << "the upper compare is " << bad_hi;
    }
    // the bounds in the wrong places: an empty range lets nothing through, which is sound
    {
      Fn f(trunc_function(k, [&](B & b, GRJIT_VReg x) {
        Imm swapped = {m.hi, m.lo, lo_cmp};
        lower_and_upper(b, x, swapped, lo_cmp, GRJIT_CMP_LT);
      }));
      EXPECT_EQ(refused(f.f), "") << "x > hi && x < lo is never true, so the truncation is never reached";
    }
  }
}

TEST(FloatVerify, AGuardOnAnotherRegisterOrAStaleOrAnotherBlocksGuardDoesNotCount) {
  for (const TruncKind & k : kTruncs) {
    SCOPED_TRACE(k.instr);
    const Imm m = exact_imms(k);
    // the guard is about a copy
    {
      B b("t", 0);
      GRJIT_VReg x = b.param(k.src), y = b.reg(k.src), r = b.reg(k.dst);
      b.at(b.block());
      b.mov(y, V(x));
      lower_and_upper(b, y, m, m.lo_cmp, GRJIT_CMP_LT);
      b.un(k.is_signed ? GRJIT_OP_TRUNC_S : GRJIT_OP_TRUNC_U, r, V(x));
      b.ret(V(r));
      Fn f(b.finish());
      EXPECT_NE(refused(f.f), "") << "a guard of a copy";
    }
    // the guard stands, then the operand is assigned
    {
      B b("t", 0);
      GRJIT_VReg x = b.param(k.src), z = b.param(k.src), r = b.reg(k.dst);
      b.at(b.block());
      lower_and_upper(b, x, m, m.lo_cmp, GRJIT_CMP_LT);
      b.mov(x, V(z));
      b.un(k.is_signed ? GRJIT_OP_TRUNC_S : GRJIT_OP_TRUNC_U, r, V(x));
      b.ret(V(r));
      Fn f(b.finish());
      EXPECT_NE(refused(f.f), "") << "the operand was assigned after the guard";
    }
    // the compares were made on a value that was then changed, before the guard
    {
      B b("t", 0);
      GRJIT_VReg x = b.param(k.src), z = b.param(k.src), r = b.reg(k.dst);
      GRJIT_VReg l = b.reg(GRJIT_TYPE_I32), h = b.reg(GRJIT_TYPE_I32), ok = b.reg(GRJIT_TYPE_I32);
      b.at(b.block());
      b.cmp(m.lo_cmp, l, V(x), I(m.lo));
      b.cmp(GRJIT_CMP_LT, h, V(x), I(m.hi));
      b.mov(x, V(z));
      b.bin(GRJIT_OP_AND, ok, V(l), V(h));
      b.guard(V(ok), {kTrapFunction, kSiteIntegerOverflow});
      b.un(k.is_signed ? GRJIT_OP_TRUNC_S : GRJIT_OP_TRUNC_U, r, V(x));
      b.ret(V(r));
      Fn f(b.finish());
      EXPECT_NE(refused(f.f), "") << "the compares read the old x";
    }
    // the flag is changed between its computation and its test
    {
      B b("t", 0);
      GRJIT_VReg x = b.param(k.src), r = b.reg(k.dst);
      GRJIT_VReg l = b.reg(GRJIT_TYPE_I32), h = b.reg(GRJIT_TYPE_I32), ok = b.reg(GRJIT_TYPE_I32);
      b.at(b.block());
      b.cmp(m.lo_cmp, l, V(x), I(m.lo));
      b.cmp(GRJIT_CMP_LT, h, V(x), I(m.hi));
      b.bin(GRJIT_OP_AND, ok, V(l), V(h));
      b.mov(ok, I(1));
      b.guard(V(ok), {kTrapFunction, kSiteIntegerOverflow});
      b.un(k.is_signed ? GRJIT_OP_TRUNC_S : GRJIT_OP_TRUNC_U, r, V(x));
      b.ret(V(r));
      Fn f(b.finish());
      EXPECT_NE(refused(f.f), "") << "the guard tests a constant";
    }
    // the guard is in another block
    {
      B b("t", 0);
      GRJIT_VReg x = b.param(k.src), r = b.reg(k.dst);
      GRJIT_BlockId first = b.block(), second = b.block();
      b.at(first);
      lower_and_upper(b, x, m, m.lo_cmp, GRJIT_CMP_LT);
      b.br(second);
      b.at(second);
      b.un(k.is_signed ? GRJIT_OP_TRUNC_S : GRJIT_OP_TRUNC_U, r, V(x));
      b.ret(V(r));
      Fn f(b.finish());
      EXPECT_NE(refused(f.f), "") << "a fact across blocks is not recognised, and the reason says so";
    }
    // the guard of an earlier truncation of the same operand serves a later one while it stands
    {
      B b("t", 0);
      GRJIT_VReg x = b.param(k.src), r = b.reg(k.dst), r2 = b.reg(k.dst);
      b.at(b.block());
      lower_and_upper(b, x, m, m.lo_cmp, GRJIT_CMP_LT);
      b.un(k.is_signed ? GRJIT_OP_TRUNC_S : GRJIT_OP_TRUNC_U, r, V(x));
      b.un(k.is_signed ? GRJIT_OP_TRUNC_S : GRJIT_OP_TRUNC_U, r2, V(x));
      b.ret(V(r2));
      Fn f(b.finish());
      EXPECT_EQ(refused(f.f), "");
    }
    // a wider later guard does not undo a narrower earlier one, and a narrower later one narrows
    {
      Imm wider = m;
      wider.hi = static_cast<int64_t>(imm_bits(
          step_ulps(trunc_bounds(k.is_signed, k.dst == GRJIT_TYPE_I64).hi, k.src == GRJIT_TYPE_F64, 1, INFINITY),
          k.src == GRJIT_TYPE_F64));
      B b("t", 0);
      GRJIT_VReg x = b.param(k.src), r = b.reg(k.dst);
      b.at(b.block());
      lower_and_upper(b, x, m, m.lo_cmp, GRJIT_CMP_LT);
      lower_and_upper(b, x, wider, m.lo_cmp, GRJIT_CMP_LT);
      b.un(k.is_signed ? GRJIT_OP_TRUNC_S : GRJIT_OP_TRUNC_U, r, V(x));
      b.ret(V(r));
      Fn f(b.finish());
      EXPECT_EQ(refused(f.f), "") << "the exact guard still stands after a wider one";
    }
  }
}

TEST(FloatVerify, AConstantOperandIsJudgedAsItStands) {
  for (const TruncKind & k : kTruncs) {
    SCOPED_TRACE(k.instr);
    const bool dbl = k.src == GRJIT_TYPE_F64;
    auto with_constant = [&](uint64_t bits) {
      B b("t", 0);
      GRJIT_VReg c = b.reg(k.src), r = b.reg(k.dst);
      b.at(b.block());
      b.cnst(c, static_cast<int64_t>(bits));
      b.un(k.is_signed ? GRJIT_OP_TRUNC_S : GRJIT_OP_TRUNC_U, r, V(c));
      b.ret(V(r));
      return refused(Fn(b.finish()).f);
    };
    EXPECT_EQ(with_constant(imm_bits(1.5, dbl)), "") << "1.5 truncates to 1";
    EXPECT_EQ(with_constant(imm_bits(0.0, dbl)), "");
    EXPECT_EQ(with_constant(imm_bits(-0.5, dbl)), "") << "-0.5 truncates to -0, which is 0";
    EXPECT_NE(with_constant(dbl ? 0x7FF8000000000000ull : 0x7FC00000u), "") << "a NaN";
    EXPECT_NE(with_constant(dbl ? 0x7FF0000000000000ull : 0x7F800000u), "") << "an infinity";
    EXPECT_NE(with_constant(imm_bits(1e30, dbl)), "") << "too big";
    EXPECT_NE(with_constant(imm_bits(-1e30, dbl)), "") << "too small";
    const Bounds bd = trunc_bounds(k.is_signed, k.dst == GRJIT_TYPE_I64);
    EXPECT_NE(with_constant(imm_bits(bd.hi, dbl)), "") << "the upper bound itself does not fit";
    if (!k.is_signed) {
      EXPECT_NE(with_constant(imm_bits(-1.0, dbl)), "") << "-1 does not fit an unsigned";
    }
    EXPECT_EQ(with_constant(imm_bits(step_ulps(bd.hi, dbl, -1, INFINITY), dbl)), "")
        << "the largest value below the upper bound fits";
  }
}

TEST(FloatVerify, AnAllocationThatFailsInTheRangeAnalysisIsOutOfMemoryAndLeaksNothing) {
  TrackingAllocator t;
  B b("t", 0, nullptr, t.get());
  GRJIT_VReg x = b.param(GRJIT_TYPE_F64), r = b.reg(GRJIT_TYPE_I32);
  b.at(b.block());
  const TruncKind k = kTruncs[2]; // i32.trunc_f64_s
  const Imm m = exact_imms(k);
  lower_and_upper(b, x, m, m.lo_cmp, GRJIT_CMP_LT);
  b.un(GRJIT_OP_TRUNC_S, r, V(x));
  b.ret(V(r));
  Fn f(b.finish());
  const long before = t.live;
  ASSERT_EQ(grjit_function_verify(f, nullptr, nullptr, 0), GRJIT_OK);
  bool saw_oom = false;
  for (long n = 1; n <= 12; n++) {
    t.calls = 0;
    t.fail_at = n;
    t.sticky = false;
    GRJIT_Result r2 = grjit_function_verify(f, nullptr, nullptr, 0);
    t.fail_at = 0;
    EXPECT_EQ(t.live, before) << "allocation " << n << " failed and something leaked";
    EXPECT_TRUE(r2 == GRJIT_OK || r2 == GRJIT_ERR_OOM) << n;
    saw_oom |= r2 == GRJIT_ERR_OOM;
  }
  EXPECT_TRUE(saw_oom);
}


/* ---- Running the code: the answers ------------------------------------------------------------ */

TEST(FloatRun, EveryOperationGivesTheWorkedAnswerFromTheCodeAndFromTheEvaluator) {
  FLOAT_RUN_HERE();
  JitWorld w;
  size_t nan_cases = 0;
  for (GRJIT_Features disabled : two_arms()) {
  SCOPED_TRACE(arm_name(disabled));
  ArmScope arm(disabled);
  std::map<std::string, std::unique_ptr<Built>> built;
  nan_cases = 0;
  for (const Worked & k : kWorked) {
    SCOPED_TRACE(std::string(k.instr) + " " + hex(k.a) + " " + hex(k.b));
    auto & slot = built[k.instr];
    if (!slot) {
      slot.reset(new Built);
      ASSERT_EQ(compile(slot.get(), k.instr, w), GRJIT_OK) << k.instr;
      if (disabled == GRJIT_FEATURES_ALL) {
        ASSERT_EQ(grjit_code_features(slot->code), GRJIT_FEATURES_NONE)
            << k.instr << ": the masked arm must have taken no feature arm";
      } else {
        ASSERT_EQ(grjit_code_features(slot->code), detected() & need_of(k.instr))
            << k.instr << ": the recorded set is the arm the host's features give";
      }
    }
    const unsigned width = result_width(k.instr);
    Outcome o = run(slot->code, w.ctx, {k.a, k.b});
    ASSERT_EQ(o.fault, 0) << "a hardware fault";
    ASSERT_EQ(o.exit, uint32_t{GRJIT_EXIT_RETURNED}) << said(slot->code, o);
    EXPECT_TRUE(result_ok(width, o.out[0], k.want, k.cls))
        << "got " << hex(o.out[0]) << ", want " << hex(k.want)
        << (k.cls == Cls::Exact ? "" : k.cls == Cls::NanCanonical ? " (a canonical NaN)" : " (an arithmetic NaN)");
    if (width == 32) {
      EXPECT_EQ(o.out[0] >> 32, 0u) << "a 32-bit result is zero-extended in its word";
    }
    ireval::Result e = ireval::eval(slot->op.function, w.ctx, nullptr, {k.a, k.b});
    ASSERT_EQ(e.exit, uint32_t{GRJIT_EXIT_RETURNED});
    EXPECT_TRUE(result_ok(width, e.out[0], k.want, k.cls))
        << "the evaluator got " << hex(e.out[0]) << ", want " << hex(k.want);
    nan_cases += k.cls != Cls::Exact;
  }
  EXPECT_GT(nan_cases, 20u) << "the NaN classes are exercised";
  }
}

namespace {

struct Sweep {
  const char * instr;
  bool float_args; ///< the arguments are floats (else integers)
  bool narrow_args; ///< 32-bit arguments (f32 or i32)
};

/* Runs `instr` over `xs` x `ys` through the code and through the evaluator and compares by the rule:
 * the same exit; for a value, equal bits (a float result: any two NaNs equal) and a zero upper
 * half for a 32-bit result; for a trap, the same site. Returns the number of runs. */
size_t sweep_one(JitWorld & w, const std::string & instr, const std::vector<uint64_t> & xs,
    const std::vector<uint64_t> & ys, size_t * traps) {
  Built b;
  EXPECT_EQ(compile(&b, instr, w), GRJIT_OK) << instr;
  if (b.code == nullptr) {
    return 0;
  }
  const GRJIT_Type rt = b.op.result;
  const unsigned rw = rt == GRJIT_TYPE_F32 || rt == GRJIT_TYPE_I32 ? 32 : 64;
  const bool float_result = rt == GRJIT_TYPE_F32 || rt == GRJIT_TYPE_F64;
  size_t n = 0;
  for (uint64_t x : xs) {
    for (uint64_t y : b.op.params == 2 ? ys : std::vector<uint64_t>{0}) {
      Outcome o = run(b.code, w.ctx, {x, y});
      ireval::Result e = ireval::eval(b.op.function, w.ctx, nullptr, {x, y});
      const std::string got = said(b.code, o);
      EXPECT_EQ(o.fault, 0) << instr << " " << hex(x) << " " << hex(y) << ": " << got;
      if (o.fault != 0) {
        return n;
      }
      if (o.exit != e.exit) {
        ADD_FAILURE() << instr << " " << hex(x) << " " << hex(y) << ": exit " << o.exit << " ("
                      << got << "), the evaluator " << e.exit;
        return n;
      }
      if (o.exit == GRJIT_EXIT_RETURNED) {
        const bool same = float_result ? ireval::same_float(rw, o.out[0], e.out[0]) : o.out[0] == e.out[0];
        if (!same) {
          ADD_FAILURE() << instr << " " << hex(x) << " " << hex(y) << ": got " << hex(o.out[0])
                        << ", the evaluator " << hex(e.out[0]);
          return n;
        }
        if (rw == 32 && (o.out[0] >> 32) != 0) {
          ADD_FAILURE() << instr << " " << hex(x) << " " << hex(y) << ": a 32-bit result has bits above 31: "
                        << hex(o.out[0]);
          return n;
        }
      } else {
        const GRCORE_CodeSite * s =
            grcore_codemeta_find(grjit_code_meta(b.code), static_cast<uint32_t>(o.out[0]));
        if (s == nullptr || s->identity.offset != e.guard_identity.offset) {
          ADD_FAILURE() << instr << " " << hex(x) << " " << hex(y) << ": " << got << ", the evaluator site "
                        << e.guard_identity.offset;
          return n;
        }
        (*traps)++;
      }
      n++;
    }
  }
  return n;
}

} // namespace

TEST(FloatRun, EveryOperationAtEveryWidthAgreesWithTheEvaluatorOnTheBoundaryValues) {
  FLOAT_RUN_HERE();
  JitWorld w;
  const std::vector<uint64_t> f64s = f64_values(), f32s = f32_values();
  size_t compared = 0, traps = 0;
  for (GRJIT_Features disabled : two_arms()) {
  SCOPED_TRACE(arm_name(disabled));
  ArmScope arm(disabled);
  for (const char * ty : {"f32", "f64"}) {
    const bool narrow = std::string(ty) == "f32";
    const std::vector<uint64_t> & vals = narrow ? f32s : f64s;
    for (const char * n : {"add", "sub", "mul", "div", "min", "max", "copysign", "eq", "ne", "lt", "gt",
             "le", "ge", "neg", "abs", "sqrt", "ceil", "floor", "trunc", "nearest"}) {
      SCOPED_TRACE(std::string(ty) + "." + n);
      compared += sweep_one(w, std::string(ty) + "." + n, vals, vals, &traps);
    }
    for (const char * from : {"i32", "i64"}) {
      for (const char * sign : {"s", "u"}) {
        const std::string c = std::string(ty) + ".convert_" + from + "_" + sign;
        SCOPED_TRACE(c);
        compared += sweep_one(w, c, int_values(std::string(from) == "i32"), {0}, &traps);
      }
    }
    for (const char * to : {"i32", "i64"}) {
      for (const char * sign : {"s", "u"}) {
        for (const char * kind : {"trunc_", "trunc_sat_"}) {
          const std::string t = std::string(to) + "." + kind + ty + "_" + sign;
          SCOPED_TRACE(t);
          compared += sweep_one(w, t, vals, {0}, &traps);
        }
      }
    }
    SCOPED_TRACE(ty);
    compared += sweep_one(w, narrow ? "f64.promote_f32" : "f32.demote_f64", vals, {0}, &traps);
    compared += sweep_one(w, narrow ? "i32.reinterpret_f32" : "i64.reinterpret_f64", vals, {0}, &traps);
    compared += sweep_one(w, narrow ? "f32.reinterpret_i32" : "f64.reinterpret_i64", int_values(narrow), {0}, &traps);
  }
  }
  std::printf("float boundary sweep: %zu runs compared with the evaluator (twice: with the host's features and "
              "with every optional feature masked off), %zu of them guard exits\n",
      compared, traps);
  EXPECT_GT(compared, 2 * 100000u);
  EXPECT_GT(traps, 2 * 240u) << "the truncations' guards were reached";
}

TEST(FloatRun, ATruncationNamesTheNanGuardTheRangeGuardOrReturnsTheTruncatedValue) {
  FLOAT_RUN_HERE();
  JitWorld w;
  const std::string NAN_T = "trap: invalid conversion to integer", OVF = "trap: integer overflow";
  struct Case {
    const char * instr;
    uint64_t in;
    std::string want;
  };
  const Case cases[] = {
      {"i32.trunc_f64_s", QNAN64, NAN_T},
      {"i32.trunc_f64_s", 0xFFF8000000000001ull, NAN_T},
      {"i32.trunc_f64_s", SNAN64, NAN_T},
      {"i32.trunc_f64_s", INF64, OVF},
      {"i32.trunc_f64_s", NINF64, OVF},
      {"i32.trunc_f64_s", B64(2147483648.0), OVF},
      {"i32.trunc_f64_s", B64(-2147483649.0), OVF},
      {"i32.trunc_f64_s", B64(-2147483648.9), hex(0x80000000u)},
      {"i32.trunc_f64_s", B64(2147483647.9), hex(0x7FFFFFFFu)},
      {"i32.trunc_f64_s", B64(-2147483648.0), hex(0x80000000u)},
      {"i32.trunc_f64_s", std::nextafter(2147483648.0, 0.0) > 0 ? B64(std::nextafter(2147483648.0, 0.0)) : 0, hex(0x7FFFFFFFu)},
      {"i32.trunc_f64_s", B64(std::nextafter(-2147483649.0, 0.0)) , hex(0x80000000u)},
      {"i32.trunc_f64_s", B64(std::nextafter(-2147483649.0, -INFINITY)), OVF},
      {"i32.trunc_f64_u", B64(-0.9), hex(0)},
      {"i32.trunc_f64_u", NZ64, hex(0)},
      {"i32.trunc_f64_u", B64(-1.0), OVF},
      {"i32.trunc_f64_u", B64(4294967296.0), OVF},
      {"i32.trunc_f64_u", B64(4294967295.9), hex(0xFFFFFFFFu)},
      {"i32.trunc_f32_s", B32(2147483648.0f), OVF},
      {"i32.trunc_f32_s", B32(-2147483648.0f), hex(0x80000000u)},
      {"i32.trunc_f32_s", B32(-2147483904.0f), OVF},
      {"i32.trunc_f32_s", B32(2147483520.0f), hex(0x7FFFFF80u)},
      {"i32.trunc_f32_s", QNAN32, NAN_T},
      {"i32.trunc_f32_u", B32(4294967040.0f), hex(0xFFFFFF00u)},
      {"i32.trunc_f32_u", B32(4294967296.0f), OVF},
      {"i32.trunc_f32_u", B32(-0.9f), hex(0)},
      {"i32.trunc_f32_u", B32(-1.0f), OVF},
      {"i32.trunc_f32_u", SNAN32, NAN_T},
      {"i64.trunc_f64_s", B64(9223372036854775808.0), OVF},
      {"i64.trunc_f64_s", B64(-9223372036854775808.0), hex(0x8000000000000000ull)},
      {"i64.trunc_f64_s", B64(std::nextafter(-9223372036854775808.0, -INFINITY)), OVF},
      {"i64.trunc_f64_s", 0x43DFFFFFFFFFFFFFull, hex(0x7FFFFFFFFFFFFC00ull)},
      {"i64.trunc_f64_s", QNAN64, NAN_T},
      {"i64.trunc_f64_u", B64(18446744073709551616.0), OVF},
      {"i64.trunc_f64_u", 0x43EFFFFFFFFFFFFFull, hex(0xFFFFFFFFFFFFF800ull)},
      {"i64.trunc_f64_u", B64(9223372036854775808.0), hex(0x8000000000000000ull)},
      {"i64.trunc_f64_u", B64(-1.0), OVF},
      {"i64.trunc_f64_u", B64(-0.99), hex(0)},
      {"i64.trunc_f64_u", NINF64, OVF},
      {"i64.trunc_f32_s", B32(9223372036854775808.0f), OVF},
      {"i64.trunc_f32_s", B32(-9223372036854775808.0f), hex(0x8000000000000000ull)},
      {"i64.trunc_f32_s", 0x5EFFFFFFu, hex(0x7FFFFF8000000000ull)},
      {"i64.trunc_f32_u", B32(18446744073709551616.0f), OVF},
      {"i64.trunc_f32_u", 0x5F7FFFFFu, hex(0xFFFFFF0000000000ull)},
      {"i64.trunc_f32_u", QNAN32, NAN_T},
      {"i64.trunc_f32_u", 0xFF800001u, NAN_T},
  };
  for (const Case & c : cases) {
    SCOPED_TRACE(std::string(c.instr) + " " + hex(c.in));
    Built b;
    ASSERT_EQ(compile(&b, c.instr, w), GRJIT_OK);
    Outcome o = run(b.code, w.ctx, {c.in});
    EXPECT_EQ(said(b.code, o), c.want) << "no hardware indefinite value is an answer";
    ireval::Result e = ireval::eval(b.op.function, w.ctx, nullptr, {c.in});
    if (c.want.rfind("trap", 0) == 0) {
      EXPECT_EQ(e.exit, uint32_t{GRJIT_EXIT_DEOPT});
      EXPECT_EQ(e.guard_identity.offset, c.want == NAN_T ? kSiteInvalidConversion : kSiteIntegerOverflow);
    } else {
      EXPECT_EQ(hex(e.out[0]), c.want);
    }
  }
}

TEST(FloatRun, AnF32EntersAndLeavesAsItsLow32BitsWhateverTheCallerLeftAbove) {
  FLOAT_RUN_HERE();
  JitWorld w;
  {
    // f(x: f32) = x: the entry masks the argument; the return is zero-extended.
    B b("ident", 0);
    GRJIT_VReg x = b.param(GRJIT_TYPE_F32);
    b.at(b.block());
    b.ret(V(x));
    Fn f(b.finish());
    Compiled c(f, w.pages());
    ASSERT_TRUE(c);
    for (uint64_t v : {uint64_t{0x3FC00000}, uint64_t{0xFFFFFFFF}, uint64_t{0xDEADBEEF3FC00000ull},
             uint64_t{0xFFFFFFFFFFFFFFFFull}, uint64_t{0x100000000ull}}) {
      Outcome o = run(c.code, w.ctx, {v});
      EXPECT_EQ(o.out[0], v & M32) << hex(v);
    }
  }
  // Every binary and unary f32 operation gives the same answer for a dirty argument as for a clean one.
  const std::vector<uint64_t> vals = f32_values();
  for (const char * n : {"add", "sub", "mul", "div", "min", "max", "copysign", "eq", "lt", "ne", "neg",
           "abs", "sqrt", "ceil", "floor", "trunc", "nearest"}) {
    SCOPED_TRACE(n);
    Built b;
    ASSERT_EQ(compile(&b, std::string("f32.") + n, w), GRJIT_OK);
    for (uint64_t x : vals) {
      for (uint64_t y : b.op.params == 2 ? vals : std::vector<uint64_t>{0}) {
        Outcome clean = run(b.code, w.ctx, {x, y});
        Outcome dirty = run(b.code, w.ctx, {x | 0xDEADBEEF00000000ull, y | 0x1234567800000000ull});
        ASSERT_EQ(clean.exit, dirty.exit);
        ASSERT_EQ(clean.out[0], dirty.out[0]) << hex(x) << " " << hex(y);
        ASSERT_EQ(dirty.out[0] >> 32, 0u);
      }
    }
  }
}

TEST(FloatRun, AFloatLoadedFromMemoryAndStoredToItIsItsBitsAtItsOwnWidth) {
  FLOAT_RUN_HERE();
  JitWorld w;
  for (bool dbl : {false, true}) {
    SCOPED_TRACE(dbl);
    const GRJIT_Type T = dbl ? GRJIT_TYPE_F64 : GRJIT_TYPE_F32;
    const unsigned bits = dbl ? 64 : 32;
    B b("mem", 0);
    GRJIT_VReg p = b.param(GRJIT_TYPE_PTR), q = b.param(GRJIT_TYPE_PTR), x = b.reg(T), y = b.reg(T);
    b.at(b.block());
    b.load(x, p, 8, bits);
    b.bin(GRJIT_OP_FADD, y, V(x), V(x));
    b.store(q, 8, bits, V(y));
    b.ret(V(x));
    Fn f(b.finish());
    Compiled code(f, w.pages());
    ASSERT_TRUE(code);
    uint8_t src[32], dst[32];
    std::memset(src, 0xAB, sizeof src);
    std::memset(dst, 0xCD, sizeof dst);
    const uint64_t v = dbl ? B64(1.25) : B32(1.25f);
    std::memcpy(src + 8, &v, bits / 8);
    uint64_t junk = 0xFEFEFEFEFEFEFEFEull;
    std::memcpy(src + 8 + bits / 8, &junk, 8);
    Outcome o = run(code.code, w.ctx, {reinterpret_cast<uint64_t>(src), reinterpret_cast<uint64_t>(dst)});
    ASSERT_EQ(o.exit, uint32_t{GRJIT_EXIT_RETURNED});
    EXPECT_EQ(o.out[0], v) << "a load is zero-extending: nothing above the width";
    uint64_t twice = dbl ? B64(2.5) : B32(2.5f);
    uint64_t stored = 0;
    std::memcpy(&stored, dst + 8, bits / 8);
    EXPECT_EQ(stored, twice);
    for (size_t i = 0; i < sizeof dst; i++) {
      if (i < 8 || i >= 8 + bits / 8) {
        EXPECT_EQ(dst[i], 0xCD) << "a store writes only its width; byte " << i;
      }
    }
  }
}

TEST(FloatRun, AConstantAndAMoveOfAnF32AreZeroExtendedAndAnF64ConstantIsAnyPattern) {
  FLOAT_RUN_HERE();
  JitWorld w;
  B b("k", 0);
  GRJIT_VReg a = b.reg(GRJIT_TYPE_F32), c = b.reg(GRJIT_TYPE_F32), d = b.reg(GRJIT_TYPE_F64),
             e = b.reg(GRJIT_TYPE_F64), s = b.reg(GRJIT_TYPE_F32), out = b.reg(GRJIT_TYPE_I64);
  b.at(b.block());
  b.cnst(a, 0xFFFFFFFFll);
  b.mov(c, V(a));
  b.cnst(d, static_cast<int64_t>(0xFFF0000000000001ull));
  b.mov(e, V(d));
  b.mov(s, I(0x80000000ll));
  b.un(GRJIT_OP_REINTERPRET, out, V(e));
  b.ret(V(out));
  Fn f(b.finish());
  Compiled code(f, w.pages());
  ASSERT_TRUE(code);
  EXPECT_EQ(run(code.code, w.ctx, {}).out[0], 0xFFF0000000000001ull) << "an f64 constant is exact";
  B g("k32", 0);
  GRJIT_VReg a2 = g.reg(GRJIT_TYPE_F32), c2 = g.reg(GRJIT_TYPE_F32);
  g.at(g.block());
  g.cnst(a2, 0xFFFFFFFFll);
  g.mov(c2, V(a2));
  g.ret(V(c2));
  Fn f2(g.finish());
  Compiled code2(f2, w.pages());
  ASSERT_TRUE(code2);
  EXPECT_EQ(run(code2.code, w.ctx, {}).out[0], 0xFFFFFFFFull);
}

TEST(FloatRun, TheOperationsAreRoundedOneAtATimeAndSubnormalsSurviveUnderTheDefaultMxcsr) {
  FLOAT_RUN_HERE();
  JitWorld w;
  // a * b + c as two operations: the product rounds to 1, so the sum is 0. A fused multiply-add
  // would give -2^-60 (-2^-26 for f32).
  {
    B b("fma64", 0);
    GRJIT_VReg a = b.param(GRJIT_TYPE_F64), c = b.param(GRJIT_TYPE_F64), d = b.param(GRJIT_TYPE_F64),
               t = b.reg(GRJIT_TYPE_F64), r = b.reg(GRJIT_TYPE_F64);
    b.at(b.block());
    b.bin(GRJIT_OP_FMUL, t, V(a), V(c));
    b.bin(GRJIT_OP_FADD, r, V(t), V(d));
    b.ret(V(r));
    Fn f(b.finish());
    Compiled code(f, w.pages());
    ASSERT_TRUE(code);
    const double x = 1.0 + std::ldexp(1.0, -30), y = 1.0 - std::ldexp(1.0, -30);
    Outcome o = run(code.code, w.ctx, {B64(x), B64(y), B64(-1.0)});
    EXPECT_EQ(o.out[0], B64(0.0)) << "rounded twice, as written";
    ireval::Result e = ireval::eval(f, w.ctx, nullptr, {B64(x), B64(y), B64(-1.0)});
    EXPECT_EQ(e.out[0], B64(0.0)) << "the evaluator is built with -ffp-contract=off as well";
  }
  {
    B b("fma32", 0);
    GRJIT_VReg a = b.param(GRJIT_TYPE_F32), c = b.param(GRJIT_TYPE_F32), d = b.param(GRJIT_TYPE_F32),
               t = b.reg(GRJIT_TYPE_F32), r = b.reg(GRJIT_TYPE_F32);
    b.at(b.block());
    b.bin(GRJIT_OP_FMUL, t, V(a), V(c));
    b.bin(GRJIT_OP_FADD, r, V(t), V(d));
    b.ret(V(r));
    Fn f(b.finish());
    Compiled code(f, w.pages());
    ASSERT_TRUE(code);
    const float x = 1.0f + std::ldexp(1.0f, -13), y = 1.0f - std::ldexp(1.0f, -13);
    Outcome o = run(code.code, w.ctx, {B32(x), B32(y), B32(-1.0f)});
    EXPECT_EQ(o.out[0], B32(0.0f));
    ireval::Result e = ireval::eval(f, w.ctx, nullptr, {B32(x), B32(y), B32(-1.0f)});
    EXPECT_EQ(e.out[0], B32(0.0f));
  }
  // The compiled code assumes the default MXCSR / FPCR (round to nearest, no flush to zero, no trap) and never
  // writes it.
#if defined(__SSE__)
  const unsigned before = _mm_getcsr();
  EXPECT_EQ(before & 0xE040u, 0u) << "the host runs the tests with round-to-nearest, flush-to-zero and denormals-are-zero off";
#elif defined(__aarch64__)
  uint64_t before;
  asm volatile("mrs %0, fpcr" : "=r"(before));
  EXPECT_EQ(before & 0x07C09F00u, 0u) << "the host runs the tests with the default FPCR (RMode, FZ, DN, AHP and the trap enables clear)";
#endif
  Built b;
  ASSERT_EQ(compile(&b, "f64.mul", w), GRJIT_OK);
  Outcome o = run(b.code, w.ctx, {0x0010000000000000ull, B64(0.5)});
  EXPECT_EQ(o.out[0], 0x0008000000000000ull) << "a subnormal result is not flushed";
  Built c;
  ASSERT_EQ(compile(&c, "f32.add", w), GRJIT_OK);
  EXPECT_EQ(run(c.code, w.ctx, {1, 1}).out[0], 2u) << "a subnormal operand is not flushed";
  Built d;
  ASSERT_EQ(compile(&d, "f64.nearest", w), GRJIT_OK);
  run(d.code, w.ctx, {B64(2.5)});
#if defined(__SSE__)
  EXPECT_EQ(_mm_getcsr(), before) << "MXCSR is untouched";
#elif defined(__aarch64__)
  uint64_t after;
  asm volatile("mrs %0, fpcr" : "=r"(after));
  EXPECT_EQ(after, before) << "FPCR is untouched";
#endif
}


namespace {
/* Used by the helper tests, and by the refusal tests on every host. */
float h_f32_result(float a, double b) {
  return a + static_cast<float>(b);
}
} // namespace

/* ---- Floats through a helper call (SysV) --------------------------------------------------------- */

#if defined(__x86_64__) && defined(__linux__)

namespace {

enum class Arg { I64, F64, F32, I64Imm, F64Imm, F32Imm };

bool is_float_arg(Arg a) { return a == Arg::F64 || a == Arg::F32 || a == Arg::F64Imm || a == Arg::F32Imm; }

/* The value passed for argument `i` of kind `a`: distinct in every position, so a swap shows. */
uint64_t arg_value(Arg a, size_t i) {
  switch (a) {
    case Arg::I64:
    case Arg::I64Imm: return 0x1111000000000000ull + 0x0101010101ull * (i + 1) + 0x7;
    case Arg::F64:
    case Arg::F64Imm: return 0x4000000000000000ull + 0x0001000100010001ull * (i + 1);
    default: return 0x40000000ull + 0x00010101ull * (i + 1);
  }
}

/* Builds f(params...) { call probe(args...) ; ret 0 } for the kinds, runs it, and compares the
 * registers with the SysV classification worked out here: integer-class arguments in rdi rsi rdx rcx r8
 * r9 among themselves, float-class ones in xmm0 to xmm7 among themselves. */
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
  b.call(r, reinterpret_cast<const void *>(&grjit_test_probe), args);
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
  std::memset(grjit_test_regs, 0xEE, sizeof grjit_test_regs);
  Outcome o = run(code.code, w.ctx, words);
  if (o.fault != 0 || o.exit != GRJIT_EXIT_RETURNED) {
    return ::testing::AssertionFailure() << "the probe did not return: " << said(code.code, o);
  }
  size_t ints = 0, floats = 0;
  for (size_t i = 0; i < kinds.size(); i++) {
    const uint64_t v = arg_value(kinds[i], i);
    if (is_float_arg(kinds[i])) {
      const bool single = kinds[i] == Arg::F32 || kinds[i] == Arg::F32Imm;
      const uint64_t got = grjit_test_regs[6 + floats];
      const uint64_t mask = single ? 0xFFFFFFFFull : ~uint64_t{0};
      if ((got & mask) != (v & mask)) {
        return ::testing::AssertionFailure()
               << "float argument " << i << " should be in xmm" << floats << " as " << hex(v)
               << " but xmm" << floats << " holds " << hex(got);
      }
      floats++;
    } else {
      if (grjit_test_regs[ints] != v) {
        return ::testing::AssertionFailure()
               << "integer argument " << i << " should be in integer register " << ints << " as "
               << hex(v) << " but it holds " << hex(grjit_test_regs[ints]);
      }
      ints++;
    }
  }
  return ::testing::AssertionSuccess();
}

} // namespace

TEST(FloatHelpers, EveryMixOfIntegerAndFloatArgumentsArrivesInItsRegisterBySysVClassification) {
  GRJIT_REQUIRE_BACKEND();
  JitWorld w;
  size_t patterns = 0;
  // All 3^n mixes of an integer, an f64 and an f32 register argument, for 0 to 6 arguments.
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
  // The same, with immediates in the arguments: plain (integer class) and typed (float class).
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
  std::printf("float helper ABI: %zu argument patterns checked against the SysV classification\n", patterns);
  EXPECT_GT(patterns, 2000u);
}

TEST(FloatHelpers, TheNamedClassificationFromTheStoryComesOutAsStated) {
  GRJIT_REQUIRE_BACKEND();
  JitWorld w;
  // (i64, f64, i64, f32) is rdi, xmm0, rsi, xmm1.
  ASSERT_TRUE(probe_pattern(w, {Arg::I64, Arg::F64, Arg::I64, Arg::F32}));
  EXPECT_EQ(grjit_test_regs[0], arg_value(Arg::I64, 0));
  EXPECT_EQ(grjit_test_regs[6], arg_value(Arg::F64, 1));
  EXPECT_EQ(grjit_test_regs[1], arg_value(Arg::I64, 2));
  EXPECT_EQ(grjit_test_regs[7] & 0xFFFFFFFFull, arg_value(Arg::F32, 3));
  // Six floats take xmm0 to xmm5; six of mixed widths too.
  ASSERT_TRUE(probe_pattern(w, {Arg::F64, Arg::F64, Arg::F64, Arg::F64, Arg::F64, Arg::F64}));
  ASSERT_TRUE(probe_pattern(w, {Arg::F64, Arg::F32, Arg::F64, Arg::F32, Arg::F64, Arg::F32}));
  ASSERT_TRUE(probe_pattern(w, {Arg::F32Imm, Arg::F64Imm, Arg::F32Imm, Arg::F64Imm, Arg::I64Imm, Arg::F64}));
}

TEST(FloatHelpers, AFloatResultIsTakenFromXmm0ByItsOwnWidthWhateverIsAboveBit31) {
  GRJIT_REQUIRE_BACKEND();
  JitWorld w;
  struct Case {
    const void * fn;
    GRJIT_Type result;
    uint64_t want;
  };
  const Case cases[] = {
      {reinterpret_cast<const void *>(&grjit_test_junk_float), GRJIT_TYPE_F32, 0x3FC00000u},
      {reinterpret_cast<const void *>(&grjit_test_probe_f32), GRJIT_TYPE_F32, 0x40200000u},
      {reinterpret_cast<const void *>(&grjit_test_probe_f64), GRJIT_TYPE_F64, 0xC00C000000000001ull},
  };
  for (const Case & c : cases) {
    SCOPED_TRACE(c.result);
    B b("result", 0);
    GRJIT_VReg r = b.reg(c.result), out = b.reg(c.result);
    b.at(b.block());
    b.call(r, c.fn, {});
    b.mov(out, V(r));
    b.ret(V(out));
    Fn f(b.finish());
    Compiled code(f, w.pages());
    ASSERT_TRUE(code);
    EXPECT_EQ(run(code.code, w.ctx, {}).out[0], c.want) << "an f32 is its low 32 bits, zero-extended";
  }
  // The result feeds an operation: 1.5f from the junk-laden register, plus itself, is 3.0f.
  B b("sum", 0);
  GRJIT_VReg r = b.reg(GRJIT_TYPE_F32), t = b.reg(GRJIT_TYPE_F32);
  b.at(b.block());
  b.call(r, reinterpret_cast<const void *>(&grjit_test_junk_float), {});
  b.bin(GRJIT_OP_FADD, t, V(r), V(r));
  b.ret(V(t));
  Fn f(b.finish());
  Compiled code(f, w.pages());
  ASSERT_TRUE(code);
  EXPECT_EQ(run(code.code, w.ctx, {}).out[0], uint64_t{0x40400000});
}

#else // not x86-64 Linux: the SysV probes, skipped by name

TEST(FloatHelpers, EveryMixOfIntegerAndFloatArgumentsArrivesInItsRegisterBySysVClassification) {
  GTEST_SKIP() << "the SysV classification is checked against an x86-64 assembly probe, on x86-64 Linux only; "
                  "the AAPCS64 and Microsoft x64 ones have probes of their own (test_arm64_numeric.cpp, "
                  "test_win64_numeric.cpp)";
}

TEST(FloatHelpers, TheNamedClassificationFromTheStoryComesOutAsStated) {
  GTEST_SKIP() << "x86-64 Linux only (a SysV probe)";
}

TEST(FloatHelpers, AFloatResultIsTakenFromXmm0ByItsOwnWidthWhateverIsAboveBit31) {
  GTEST_SKIP() << "x86-64 Linux only (a SysV probe)";
}

#endif // x86-64 Linux

/* ---- Floats through a helper call, on every backend ---------------------------------------------- */

namespace {

struct Seen {
  int64_t a = 0, c = 0;
  double b = 0, d = 0;
  float e = 0;
};
Seen g_seen;

double h_mixed(int64_t a, double b, int64_t c, float e) {
  g_seen.a = a;
  g_seen.b = b;
  g_seen.c = c;
  g_seen.e = e;
  return b * 2.0 + static_cast<double>(e) + static_cast<double>(a + c);
}

} // namespace

TEST(FloatHelpers, ACompiledCallOfARealCppHelperGetsItsArgumentsAndGivesItsResultExactly) {
  FLOAT_RUN_HERE();
  JitWorld w;
  {
    B b("mixed", 0);
    GRJIT_VReg a = b.param(GRJIT_TYPE_I64), x = b.param(GRJIT_TYPE_F64), c = b.param(GRJIT_TYPE_I64),
               e = b.param(GRJIT_TYPE_F32), r = b.reg(GRJIT_TYPE_F64);
    b.at(b.block());
    b.call(r, reinterpret_cast<const void *>(&h_mixed), {V(a), V(x), V(c), V(e)});
    b.ret(V(r));
    Fn f(b.finish());
    Compiled code(f, w.pages());
    ASSERT_TRUE(code);
    Outcome o = run(code.code, w.ctx, {7, B64(2.5), 11, B32(0.75f)});
    EXPECT_EQ(f64of(o.out[0]), 2.5 * 2.0 + 0.75 + 18.0);
    EXPECT_EQ(g_seen.a, 7);
    EXPECT_EQ(g_seen.b, 2.5);
    EXPECT_EQ(g_seen.c, 11);
    EXPECT_EQ(g_seen.e, 0.75f);
  }
  {
    B b("f32res", 0);
    GRJIT_VReg a = b.param(GRJIT_TYPE_F32), x = b.param(GRJIT_TYPE_F64), r = b.reg(GRJIT_TYPE_F32);
    b.at(b.block());
    b.call(r, reinterpret_cast<const void *>(&h_f32_result), {V(a), V(x)});
    b.ret(V(r));
    Fn f(b.finish());
    Compiled code(f, w.pages());
    ASSERT_TRUE(code);
    EXPECT_EQ(run(code.code, w.ctx, {B32(1.5f), B64(2.25)}).out[0], uint64_t{B32(3.75f)});
  }
  {
    // typed immediates as the arguments
    B b("imm", 0);
    GRJIT_VReg r = b.reg(GRJIT_TYPE_F64);
    b.at(b.block());
    b.call(r, reinterpret_cast<const void *>(&h_mixed),
        {I(40), grjit_operand_f64_imm(B64(1.25)), I(2), grjit_operand_f32_imm(B32(0.5f))});
    b.ret(V(r));
    Fn f(b.finish());
    Compiled code(f, w.pages());
    ASSERT_TRUE(code);
    EXPECT_EQ(f64of(run(code.code, w.ctx, {}).out[0]), 1.25 * 2.0 + 0.5 + 42.0);
  }
}

TEST(FloatHelpers, ACallOverTheLimitIsRefusedAsBeforeAndANativeTakesNoFloat) {
  B b("many", 0);
  GRJIT_VReg x = b.param(GRJIT_TYPE_F64);
  b.at(b.block());
  std::vector<GRJIT_Operand> args(7, V(x));
  EXPECT_EQ(grjit_builder_call(b.b, GRJIT_NO_VREG, 0x1000, GRJIT_CALL_NO_GC, GRCORE_SITE_GC_POINT_CALL,
                args.data(), args.size(), GRCORE_PollIdentity{0, 0}, nullptr, 0),
      GRJIT_ERR_LIMIT)
      << "no stack-passed float exists on SysV: more than six arguments is the limit it always was";
  // A float value named as a native's argument is refused by the verifier (the table has no float).
  NativeTab t;
  uint32_t id = t.add(reinterpret_cast<const void *>(&h_f32_result), {GRJIT_TYPE_I64}, GRJIT_TYPE_I64);
  B n("native", 0);
  GRJIT_CallHooks h{};
  h.deopt = [](void *, uint64_t) -> uint32_t { return 0; };
  n.callable(h);
  n.natives(t);
  GRJIT_VReg fx = n.param(GRJIT_TYPE_F64), r = n.reg(GRJIT_TYPE_I64);
  n.at(n.block());
  n.call_native(r, id, {V(fx)}, {1, 1}, {});
  n.ret(V(r));
  Fn f(n.finish());
  EXPECT_NE(refused(f.f), "") << "a float register is not the i64 the native takes";
}

/* A float argument and a float result of a helper that can reach a GC point: the walk start is stored
 * first, and the argument is read from its slot after it, so a collection under the helper sees the
 * frame; the live reference is the only stack-map entry, the float is raw in the frame state. */
namespace {
double gc_point_helper(double a, int64_t n) {
  return a * 4.0 + static_cast<double>(n);
}
} // namespace

TEST(FloatHelpers, ACallThatCanReachAGcPointTakesAndReturnsFloatsAndKeepsThemOutOfTheStackMap) {
  FLOAT_RUN_HERE();
  JitWorld w;
  B b("gcfloat", 3);
  GRJIT_CallHooks h{};
  h.deopt = [](void *, uint64_t) -> uint32_t { return 0; };
  b.callable(h);
  GRJIT_VReg x = b.param(GRJIT_TYPE_F64), n = b.param(GRJIT_TYPE_I64), r = b.param(GRJIT_TYPE_REF),
             y = b.reg(GRJIT_TYPE_F64), keep = b.reg(GRJIT_TYPE_I64);
  b.at(b.block());
  b.call_gc(y, reinterpret_cast<const void *>(&gc_point_helper), {V(x), V(n)}, {9, 2},
      {grjit_frame_slot_vreg(x), grjit_frame_slot_vreg(n), grjit_frame_slot_vreg(r)});
  b.bitcast(keep, r);
  b.ret(V(y));
  Fn f(b.finish());
  char why[256] = {0};
  ASSERT_EQ(grjit_function_verify(f, nullptr, why, sizeof why), GRJIT_OK) << why;
  Compiled c(f, w.pages());
  ASSERT_TRUE(c) << grjit_result_string(c.result);
  const GRCORE_CodeMeta * meta = grjit_code_meta(c.code);
  size_t sites = 0;
  for (size_t i = 0; i < meta->site_count; i++) {
    const GRCORE_CodeSite & s = meta->sites[i];
    if (s.kind != GRCORE_SITE_GC_POINT_CALL || s.identity.function != 9) {
      continue;
    }
    sites++;
    ASSERT_EQ(s.live_count, 1u);
    EXPECT_EQ(s.live[0].slot_kind, GRCORE_SLOT_VALUE);
    ASSERT_EQ(s.frame_state_count, 3u);
    EXPECT_EQ(s.frame_state[0].representation, GRCORE_REPR_F64);
    EXPECT_EQ(s.frame_state[0].slot_kind, GRCORE_SLOT_RAW);
    EXPECT_EQ(s.frame_state[1].representation, GRCORE_REPR_BITS);
  }
  EXPECT_EQ(sites, 1u);
  Outcome o = run(c.code, w.ctx, {B64(2.5), 3, 0});
  ASSERT_EQ(o.exit, uint32_t{GRJIT_EXIT_RETURNED}) << said(c.code, o);
  EXPECT_EQ(f64of(o.out[0]), 13.0);
}





/* ---- Frame states and stack maps -------------------------------------------------------------- */

TEST(FloatRun, AFloatInAFrameStateIsRecordedWithItsRepresentationAndRawNeverAReference) {
  FLOAT_RUN_HERE();
  JitWorld w;
  B b("state", 5);
  GRJIT_VReg x = b.param(GRJIT_TYPE_F32), y = b.param(GRJIT_TYPE_F64), z = b.param(GRJIT_TYPE_I64),
             s = b.reg(GRJIT_TYPE_F32), d = b.reg(GRJIT_TYPE_F64), one = b.reg(GRJIT_TYPE_I32);
  b.at(b.block());
  b.bin(GRJIT_OP_FADD, s, V(x), V(x));
  b.bin(GRJIT_OP_FMUL, d, V(y), V(y));
  b.cmp(GRJIT_CMP_NE, one, V(x), V(x)); // 0 for a number, 1 for a NaN
  b.guard(V(one), {5, 7},
      {grjit_frame_slot_vreg(x), grjit_frame_slot_vreg(y), grjit_frame_slot_vreg(z),
          grjit_frame_slot_vreg(s), grjit_frame_slot_vreg(d)});
  b.ret(V(s));
  Fn f(b.finish());
  Compiled code(f, w.pages());
  ASSERT_TRUE(code);
  Outcome o = run(code.code, w.ctx, {B32(1.5f), B64(3.0), 0x1111111122222222ull});
  ASSERT_EQ(o.exit, uint32_t{GRJIT_EXIT_DEOPT});
  EXPECT_EQ(o.out[0], uint64_t{B32(1.5f)}) << "the f32, raw, a whole word";
  EXPECT_EQ(o.out[1], B64(3.0));
  EXPECT_EQ(o.out[2], 0x1111111122222222ull);
  EXPECT_EQ(o.out[3], uint64_t{B32(3.0f)});
  EXPECT_EQ(o.out[4], B64(9.0));
  const GRCORE_CodeSite * site =
      grcore_codemeta_find(grjit_code_meta(code.code), static_cast<uint32_t>(o.out[5]));
  ASSERT_NE(site, nullptr);
  ASSERT_EQ(site->frame_state_count, 5u);
  const GRCORE_Representation want[] = {GRCORE_REPR_F32, GRCORE_REPR_F64, GRCORE_REPR_BITS,
      GRCORE_REPR_F32, GRCORE_REPR_F64};
  for (size_t i = 0; i < 5; i++) {
    EXPECT_EQ(site->frame_state[i].representation, want[i]) << i;
    EXPECT_EQ(site->frame_state[i].slot_kind, GRCORE_SLOT_RAW) << i;
    EXPECT_EQ(site->frame_state[i].kind, GRCORE_LOC_FRAME_SLOT) << i;
  }
}

namespace {
uint64_t float_live_native(void *, uint64_t v) { return v + 1; }
uint64_t float_live_helper(uint64_t v) { return v + 2; }
uint32_t float_no_poll(void *, uint64_t, uint64_t) { return 0; }
} // namespace

/* A float live across a poll, a call that can collect and a native call is a raw frame-state value,
 * never a stack-map entry: the live list of each site holds the reference and nothing else. */
TEST(FloatRun, AFloatLiveAcrossAPollACallAndANativeIsRawInTheFrameStateAndNeverInTheStackMap) {
  FLOAT_RUN_HERE();
  JitWorld w;
  NativeTab tab;
  uint32_t native = tab.add(reinterpret_cast<const void *>(&float_live_native), {GRJIT_TYPE_I64}, GRJIT_TYPE_I64);
  B b("live", 3);
  GRJIT_CallHooks h{};
  h.deopt = [](void *, uint64_t) -> uint32_t { return 0; };
  b.callable(h);
  b.natives(tab);
  b.poll_helper(&float_no_poll);
  GRJIT_VReg x = b.param(GRJIT_TYPE_F32), y = b.param(GRJIT_TYPE_F64), r = b.param(GRJIT_TYPE_REF);
  GRJIT_VReg u = b.reg(GRJIT_TYPE_I64), v = b.reg(GRJIT_TYPE_I64), t = b.reg(GRJIT_TYPE_F32),
             keep = b.reg(GRJIT_TYPE_I64);
  b.at(b.block());
  auto state = [&] {
    return std::vector<GRJIT_FrameSlot>{grjit_frame_slot_vreg(x), grjit_frame_slot_vreg(y),
        grjit_frame_slot_vreg(r)};
  };
  b.poll({9, 1}, state());
  b.call_gc(u, reinterpret_cast<const void *>(&float_live_helper), {I(5)}, {9, 2}, state());
  b.call_native(v, native, {V(u)}, {9, 3}, state());
  b.bin(GRJIT_OP_FADD, t, V(x), V(x));
  b.bitcast(keep, r);
  b.ret(V(t));
  Fn f(b.finish());
  char why[256] = {0};
  ASSERT_EQ(grjit_function_verify(f, nullptr, why, sizeof why), GRJIT_OK) << why;
  Compiled c(f, w.pages());
  ASSERT_TRUE(c) << grjit_result_string(c.result);
  const GRCORE_CodeMeta * meta = grjit_code_meta(c.code);
  size_t polls = 0, calls = 0;
  for (size_t i = 0; i < meta->site_count; i++) {
    const GRCORE_CodeSite & s = meta->sites[i];
    if ((s.kind != GRCORE_SITE_GC_POINT_POLL && s.kind != GRCORE_SITE_GC_POINT_CALL &&
            s.kind != GRCORE_SITE_GC_POINT_ALLOC_SLOW) ||
        s.identity.function != 9) {
      continue;
    }
    polls += s.kind == GRCORE_SITE_GC_POINT_POLL;
    calls += s.kind != GRCORE_SITE_GC_POINT_POLL;
    SCOPED_TRACE(s.identity.offset);
    ASSERT_EQ(s.live_count, 1u) << "only the reference is in the stack map";
    EXPECT_EQ(s.live[0].slot_kind, GRCORE_SLOT_VALUE);
    ASSERT_EQ(s.frame_state_count, 3u);
    EXPECT_EQ(s.frame_state[0].slot_kind, GRCORE_SLOT_RAW);
    EXPECT_EQ(s.frame_state[0].representation, GRCORE_REPR_F32);
    EXPECT_EQ(s.frame_state[1].slot_kind, GRCORE_SLOT_RAW);
    EXPECT_EQ(s.frame_state[1].representation, GRCORE_REPR_F64);
    EXPECT_EQ(s.frame_state[2].slot_kind, GRCORE_SLOT_VALUE);
    EXPECT_NE(s.frame_state[0].value, s.live[0].value);
    EXPECT_NE(s.frame_state[1].value, s.live[0].value);
  }
  EXPECT_EQ(polls, 1u);
  EXPECT_EQ(calls, 2u) << "the helper that can collect, and the native";
}

/* ---- Floats through calls and tail calls between compiled functions ---------------------------- */

namespace {

using fx::K;
using fx::P;

/* The types of the arguments of a call in the tests: a mix of the four register types. */
GRJIT_Type mixed_type(int i) {
  switch (i % 4) {
    case 0: return GRJIT_TYPE_F64;
    case 1: return GRJIT_TYPE_F32;
    case 2: return GRJIT_TYPE_I64;
    default: return GRJIT_TYPE_I32;
  }
}

/* The bits passed for argument i of type t. */
uint64_t mixed_value(int i, GRJIT_Type t) {
  switch (t) {
    case GRJIT_TYPE_F64: return B64(1.5 + i);
    case GRJIT_TYPE_F32: return B32(2.25f + static_cast<float>(i));
    case GRJIT_TYPE_I64: return 0x1234000000000000ull + static_cast<uint64_t>(i);
    default: return 0x80000000u + static_cast<uint64_t>(i);
  }
}

/* pick_j(a0 ... a{n-1}) returns a_j, whatever its type. */
int add_picker(fx::Engine & e, int n, int j) {
  std::vector<GRJIT_Type> params;
  for (int i = 0; i < n; i++) {
    params.push_back(mixed_type(i));
  }
  P p("pick", params);
  p.ret(j);
  int id = e.reserve();
  e.set(id, p.done());
  return id;
}

/* caller(): builds the n arguments (the floats by an addition of two constants, so the operation is
 * in the path), calls pick_j and returns what it gave back. */
int add_caller(fx::Engine & e, int n, int callee, bool tail) {
  P p(tail ? "tail_caller" : "caller", {});
  std::vector<int> args;
  for (int i = 0; i < n; i++) {
    const GRJIT_Type t = mixed_type(i);
    int a = p.local(t);
    if (t == GRJIT_TYPE_F64) {
      int c0 = p.local(t), c1 = p.local(t);
      p.cnst(c0, static_cast<int64_t>(B64(1.0)));
      p.cnst(c1, static_cast<int64_t>(B64(0.5 + i)));
      p.bin(K::FADD, a, c0, c1);
    } else if (t == GRJIT_TYPE_F32) {
      int c0 = p.local(t), c1 = p.local(t);
      p.cnst(c0, static_cast<int64_t>(B32(2.0f)));
      p.cnst(c1, static_cast<int64_t>(B32(0.25f + static_cast<float>(i))));
      p.bin(K::FADD, a, c0, c1);
    } else {
      p.cnst(a, static_cast<int64_t>(mixed_value(i, t)));
    }
    args.push_back(a);
  }
  if (tail) {
    p.tailcall(callee, args);
  } else {
    int r = p.local(mixed_type(0));
    p.call(r, callee, args);
    p.ret(r);
  }
  int id = e.reserve();
  e.set(id, p.done());
  return id;
}

} // namespace

TEST(FloatCalls, AFloatGoesThroughACallInARegisterAndOnTheStackAndComesBackExact) {
  FLOAT_RUN_HERE();
  fx::Engine e;
  for (int n : {1, 2, 4, 6, 7, 9, 12}) {
    SCOPED_TRACE(n);
    for (int j = 0; j < n; j++) {
      SCOPED_TRACE(j);
      // The call's result type is the picked parameter's, so the caller's local for it is retyped.
      std::vector<GRJIT_Type> params;
      for (int i = 0; i < n; i++) {
        params.push_back(mixed_type(i));
      }
      int callee = add_picker(e, n, j);
      P p("caller", {});
      std::vector<int> args;
      for (int i = 0; i < n; i++) {
        const GRJIT_Type t = mixed_type(i);
        int a = p.local(t);
        p.cnst(a, static_cast<int64_t>(mixed_value(i, t)));
        args.push_back(a);
      }
      int r = p.local(params[static_cast<size_t>(j)]);
      p.call(r, callee, args);
      p.ret(r);
      int caller = e.reserve();
      e.set(caller, p.done());
      fx::Outcome c = e.run_compiled(caller, {});
      ASSERT_TRUE(c.finished);
      EXPECT_EQ(c.exit, uint32_t{GRJIT_EXIT_RETURNED});
      EXPECT_EQ(c.value, mixed_value(j, params[static_cast<size_t>(j)]))
          << "the " << (j < 6 ? "register" : "stack") << " argument arrives and returns exact";
      if (params[static_cast<size_t>(j)] == GRJIT_TYPE_F32 || params[static_cast<size_t>(j)] == GRJIT_TYPE_I32) {
        EXPECT_EQ(c.value >> 32, 0u) << "a 32-bit result is cut at the receiving frame";
      }
      // The callee is also an entry: the adapter's words carry garbage above 32 bits for the 32-bit ones.
      std::vector<fx::u64> words;
      for (int i = 0; i < n; i++) {
        const GRJIT_Type t = mixed_type(i);
        const uint64_t v = mixed_value(i, t);
        words.push_back(t == GRJIT_TYPE_F32 || t == GRJIT_TYPE_I32 ? (v | 0xDEADBEEF00000000ull) : v);
      }
      fx::Outcome d = e.run_compiled(callee, words);
      ASSERT_TRUE(d.finished);
      EXPECT_EQ(d.value, mixed_value(j, params[static_cast<size_t>(j)])) << "an entry word's upper half is not part of the value";
    }
  }
  EXPECT_EQ(e.st.deopts, 0);
  EXPECT_EQ(e.st.pushes, e.st.pops);
}

TEST(FloatCalls, FloatArithmeticInTheCallerFeedsTheCalleeAndTheResultComesBack) {
  FLOAT_RUN_HERE();
  fx::Engine e;
  for (int n : {3, 5, 8, 11}) {
    SCOPED_TRACE(n);
    int callee = add_picker(e, n, 0); // an f64 comes back
    int caller = add_caller(e, n, callee, false);
    fx::Outcome c = e.run_compiled(caller, {});
    ASSERT_TRUE(c.finished);
    EXPECT_EQ(c.value, B64(1.0 + (0.5 + 0))) << "f64 constants added in the caller, passed, returned";
    int callee1 = add_picker(e, n, 1); // an f32 comes back
    P p("f32_caller", {});
    std::vector<int> args;
    for (int i = 0; i < n; i++) {
      const GRJIT_Type t = mixed_type(i);
      int a = p.local(t);
      if (t == GRJIT_TYPE_F32) {
        int c0 = p.local(t), c1 = p.local(t);
        p.cnst(c0, static_cast<int64_t>(B32(2.0f)));
        p.cnst(c1, static_cast<int64_t>(B32(0.25f + static_cast<float>(i))));
        p.bin(K::FMUL, a, c0, c1);
      } else {
        p.cnst(a, static_cast<int64_t>(mixed_value(i, t)));
      }
      args.push_back(a);
    }
    int r = p.local(GRJIT_TYPE_F32);
    p.call(r, callee1, args);
    p.ret(r);
    int f32_caller = e.reserve();
    e.set(f32_caller, p.done());
    fx::Outcome d = e.run_compiled(f32_caller, {});
    ASSERT_TRUE(d.finished);
    EXPECT_EQ(d.value, uint64_t{B32(2.0f * (0.25f + 1.0f))});
  }
}

TEST(FloatCalls, AFloatGoesThroughATailCallInARegisterAndOnTheStack) {
  FLOAT_RUN_HERE();
  fx::Engine e;
  for (int n : {2, 6, 8, 11}) {
    SCOPED_TRACE(n);
    for (int j : {0, 1, n - 1}) {
      SCOPED_TRACE(j);
      int callee = add_picker(e, n, j);
      int tail = add_caller(e, n, callee, true);
      fx::Outcome c = e.run_compiled(tail, {});
      ASSERT_TRUE(c.finished);
      EXPECT_EQ(c.exit, uint32_t{GRJIT_EXIT_RETURNED});
      // The picked argument: the caller built a float by addition, an integer by constant.
      const GRJIT_Type t = mixed_type(j);
      uint64_t want;
      if (t == GRJIT_TYPE_F64) {
        want = B64(1.0 + (0.5 + j));
      } else if (t == GRJIT_TYPE_F32) {
        want = B32(2.0f + (0.25f + static_cast<float>(j)));
      } else {
        want = mixed_value(j, t);
      }
      EXPECT_EQ(c.value, want);
    }
  }
  EXPECT_EQ(e.st.deopts, 0);
}

TEST(FloatCalls, AFloatGoesThroughACallPtrAndATailCallPtrInARegisterAndOnTheStack) {
  FLOAT_RUN_HERE();
  fx::Engine e;
  for (int n : {2, 9}) {
    SCOPED_TRACE(n);
    for (int j : {0, 1, n - 1}) {
      SCOPED_TRACE(j);
      int callee = add_picker(e, n, j);
      ASSERT_TRUE(e.compile_fn(callee));
      ASSERT_NE(e.code_of(callee).internal, 0u);
      auto build = [&](bool tail) {
        P p(tail ? "tail_ptr" : "via_ptr", {});
        int ptr = p.local(GRJIT_TYPE_PTR);
        p.entryof(ptr, callee);
        std::vector<int> args;
        for (int i = 0; i < n; i++) {
          const GRJIT_Type t = mixed_type(i);
          int a = p.local(t);
          p.cnst(a, static_cast<int64_t>(mixed_value(i, t)));
          args.push_back(a);
        }
        if (tail) {
          p.tailcallp(ptr, callee, args);
        } else {
          int r = p.local(mixed_type(j));
          p.callp(r, ptr, callee, args);
          p.ret(r);
        }
        int id = e.reserve();
        e.set(id, p.done());
        return id;
      };
      for (bool tail : {false, true}) {
        fx::Outcome c = e.run_compiled(build(tail), {});
        ASSERT_TRUE(c.finished);
        EXPECT_EQ(c.exit, uint32_t{GRJIT_EXIT_RETURNED});
        EXPECT_EQ(c.value, mixed_value(j, mixed_type(j))) << (tail ? "TAIL_CALL_PTR" : "CALL_PTR");
      }
    }
  }
  EXPECT_EQ(e.st.deopts, 0);
}

/* ---- Other backends refuse, by name ---------------------------------------------------------- */

namespace {

/* Functions that use a float only in a call: an f64 register argument, a typed immediate, a float result. */
GRJIT_Function * float_call_function(int which) {
  B b("callf", 0);
  GRJIT_VReg x = b.param(GRJIT_TYPE_F64);
  GRJIT_VReg r = b.reg(which == 2 ? GRJIT_TYPE_F32 : GRJIT_TYPE_I64);
  b.at(b.block());
  switch (which) {
    case 0: b.call(r, reinterpret_cast<const void *>(&h_f32_result), {V(x)}); break;
    case 1: b.call(r, reinterpret_cast<const void *>(&h_f32_result), {grjit_operand_f32_imm(B32(1.0f))}); break;
    default: b.call(r, reinterpret_cast<const void *>(&h_f32_result), {}); break;
  }
  b.ret(V(r));
  return b.finish();
}

} // namespace

namespace {

/* Every backend emits the float operations. */
bool emits_floats(GRJIT_Arch) {
  return true;
}

} // namespace

TEST(FloatRefusal, EveryBackendEmitsEveryFloatInstructionOfTheTables) {
  for (GRJIT_Arch arch : {GRJIT_ARCH_X86_64, GRJIT_ARCH_ARM64, GRJIT_ARCH_X86_64_WIN64}) {
    const int id = static_cast<int>(arch);
    const GRJIT_Result want = emits_floats(arch) ? GRJIT_OK : GRJIT_ERR_UNSUPPORTED;
    size_t checked = 0;
    for (const std::string & instr : every_float_instruction()) {
      Built b;
      b.op = build_any_op(instr);
      ASSERT_NE(b.op.function, nullptr) << instr;
      GRJIT_Emitted emitted;
      EXPECT_EQ(grjit_emit_for(arch, b.op.function, grjit_allocator_default(), nullptr, nullptr, 0, &emitted), want)
          << "arch " << id << " " << instr;
      if (want == GRJIT_OK) {
        EXPECT_GT(emitted.size, 0u) << instr;
      }
      grjit_emitted_free(&emitted);
      checked++;
    }
    EXPECT_EQ(checked, 70u) << "every instruction of the float tables";
    // A helper call with a float argument, a typed immediate and a float result.
    for (int which = 0; which < 3; which++) {
      Fn f(float_call_function(which));
      GRJIT_Emitted emitted;
      EXPECT_EQ(grjit_emit_for(arch, f, grjit_allocator_default(), nullptr, nullptr, 0, &emitted), want)
          << "arch " << id << " call " << which;
      grjit_emitted_free(&emitted);
    }
    // A float in a load, a store, a move, a constant, a compare and a return only.
    {
      B b("moves", 0);
      GRJIT_VReg p = b.param(GRJIT_TYPE_PTR), x = b.reg(GRJIT_TYPE_F64), y = b.reg(GRJIT_TYPE_F64),
                 z = b.reg(GRJIT_TYPE_I32);
      b.at(b.block());
      b.load(x, p, 0, 64);
      b.mov(y, V(x));
      b.cmp(GRJIT_CMP_LT, z, V(x), V(y));
      b.store(p, 8, 64, V(y));
      b.ret(V(y));
      Fn f(b.finish());
      GRJIT_Emitted emitted;
      EXPECT_EQ(grjit_emit_for(arch, f, grjit_allocator_default(), nullptr, nullptr, 0, &emitted), want)
          << "arch " << id << " a float that only moves";
      grjit_emitted_free(&emitted);
    }
  }
}

/* What no backend emits is refused before any of them is asked: by the verifier, which is shared code. Each of
 * these is a function the verifier rejects, so a compile of it on any target is refused with the same reason. */
TEST(FloatRefusal, WhatIsStillRefusedIsRefusedByTheVerifierWhicheverBackendWouldEmitIt) {
  {
    // a float register as an argument of a native call
    NativeTab t;
    uint32_t id = t.add(reinterpret_cast<const void *>(&h_f32_result), {GRJIT_TYPE_I64}, GRJIT_TYPE_I64);
    B n("native", 0);
    GRJIT_CallHooks h{};
    h.deopt = [](void *, uint64_t) -> uint32_t { return 0; };
    n.callable(h);
    n.natives(t);
    GRJIT_VReg fx = n.param(GRJIT_TYPE_F64), r = n.reg(GRJIT_TYPE_I64);
    n.at(n.block());
    n.call_native(r, id, {V(fx)}, {1, 1}, {});
    n.ret(V(r));
    Fn f(n.finish());
    EXPECT_NE(refused(f.f), "") << "a float register is not the i64 a native takes";
  }
  {
    // an unsigned condition on floats
    B b("ucmp", 0);
    GRJIT_VReg x = b.param(GRJIT_TYPE_F64), c = b.reg(GRJIT_TYPE_I32);
    b.at(b.block());
    b.cmp(GRJIT_CMP_ULT, c, V(x), V(x));
    b.ret(V(c));
    Fn f(b.finish());
    EXPECT_NE(refused(f.f), "");
  }
  {
    // a float as an address
    B b("addr", 0);
    GRJIT_VReg x = b.param(GRJIT_TYPE_F64), r = b.reg(GRJIT_TYPE_I64);
    b.at(b.block());
    b.load(r, x, 0, 64);
    b.ret(V(r));
    Fn f(b.finish());
    EXPECT_NE(refused(f.f), "");
  }
  {
    // a bitcast out of a float
    B b("bitcast", 0);
    GRJIT_VReg x = b.param(GRJIT_TYPE_F64), r = b.reg(GRJIT_TYPE_I64);
    b.at(b.block());
    b.bitcast(r, x);
    b.ret(V(r));
    Fn f(b.finish());
    EXPECT_NE(refused(f.f), "");
  }
}

TEST(FloatRefusal, AFunctionWithoutAFloatIsStillEmittedByAllThreeBackends) {
  B b("old", 0);
  GRJIT_VReg x = b.param(GRJIT_TYPE_I64), r = b.reg(GRJIT_TYPE_I64);
  b.at(b.block());
  b.bin(GRJIT_OP_ADD, r, V(x), I(1));
  b.call(r, reinterpret_cast<const void *>(&float_live_helper), {V(r), I(3)});
  b.ret(V(r));
  Fn f(b.finish());
  for (GRJIT_Arch arch : {GRJIT_ARCH_X86_64, GRJIT_ARCH_ARM64, GRJIT_ARCH_X86_64_WIN64}) {
    GRJIT_Emitted emitted;
    EXPECT_EQ(grjit_emit_for(arch, f, grjit_allocator_default(), nullptr, nullptr, 0, &emitted), GRJIT_OK);
    grjit_emitted_free(&emitted);
  }
}

TEST(FloatRefusal, ANativeCompileEmitsEveryFloatInstructionWhereTheBackendDoes) {
  const GRJIT_Arch native = grjit_native_arch();
  if (!grjit_backend_available()) {
    GTEST_SKIP() << "no native code backend on this target";
  }
  JitWorld w;
  for (const std::string & instr : every_float_instruction()) {
    SCOPED_TRACE(instr);
    Built b;
    b.op = build_any_op(instr);
    ASSERT_NE(b.op.function, nullptr);
    Compiled c(b.op.function, w.pages());
    if (emits_floats(native)) {
      EXPECT_EQ(c.result, GRJIT_OK);
      EXPECT_TRUE(c);
    } else {
      EXPECT_EQ(c.result, GRJIT_ERR_UNSUPPORTED);
      EXPECT_FALSE(c);
    }
  }
}

namespace {

/* Leaves 32 KiB of the stack below this frame full of ones, so that a function entered next finds garbage in
 * every slot it has not written. */
__attribute__((noinline)) void dirty_the_stack() {
  volatile uint64_t scratch[4096];
  for (size_t i = 0; i < 4096; i++) {
    scratch[i] = ~uint64_t{0};
  }
  __asm__ volatile("" : : "r"(scratch) : "memory");
}

} // namespace

/* An f32 result is a whole word with the upper half zero, however it got there: the register file's upper half after
 * a scalar single operation is not always zero, and a slot is not zero before it is written. Run over a stack full of
 * ones, every operation that makes an f32 returns a word whose bits 32-63 are clear. */
TEST(FloatRun, AnF32ResultIsAWholeWordWithTheUpperHalfZeroOverAStackThatHeldGarbage) {
  FLOAT_RUN_HERE();
  JitWorld w;
  size_t checked = 0;
  for (const std::string & instr : every_float_instruction()) {
    Built b;
    ASSERT_EQ(compile(&b, instr, w), GRJIT_OK) << instr;
    if (b.op.result != GRJIT_TYPE_F32) {
      continue;
    }
    SCOPED_TRACE(instr);
    for (uint64_t x : {uint64_t{0x3FC00000}, uint64_t{0x40490FDB}, uint64_t{0x80000000}, uint64_t{7}}) {
      dirty_the_stack();
      Outcome o = run(b.code, w.ctx, {x, 0x3F800000u});
      ASSERT_EQ(o.exit, uint32_t{GRJIT_EXIT_RETURNED}) << said(b.code, o);
      EXPECT_EQ(o.out[0] >> 32, 0u) << "bits 32-63 of an f32 result word: " << hex(o.out[0]);
      checked++;
    }
  }
  EXPECT_GT(checked, 70u) << "every f32-producing instruction was run";
}

/* A typed float immediate is accepted wherever a float operation takes an immediate of its own width:
 * on either side of FADD and of a float CMP, as the source of a MOVE, and as the bounds of a truncation
 * guard. The code and the evaluator agree, and a typed immediate of the other width is refused. */
TEST(FloatRun, ATypedFloatImmediateWorksOnEitherSideOfAnOperationAMoveAndATruncationGuard) {
  FLOAT_RUN_HERE();
  JitWorld w;
  for (bool dbl : {false, true}) {
    SCOPED_TRACE(dbl);
    const GRJIT_Type T = dbl ? GRJIT_TYPE_F64 : GRJIT_TYPE_F32;
    auto typed = [&](double v) {
      return dbl ? grjit_operand_f64_imm(B64(v)) : grjit_operand_f32_imm(static_cast<uint32_t>(B32(static_cast<float>(v))));
    };
    auto bits = [&](double v) { return dbl ? B64(v) : uint64_t{B32(static_cast<float>(v))}; };
    for (int side = 0; side < 2; side++) {
      B b("typed", 0);
      GRJIT_VReg x = b.param(T), s = b.reg(T), m = b.reg(T), c = b.reg(GRJIT_TYPE_I32),
             r = b.reg(dbl ? GRJIT_TYPE_I64 : GRJIT_TYPE_I32);
      b.at(b.block());
      b.bin(GRJIT_OP_FADD, s, side == 0 ? typed(1.5) : V(x), side == 0 ? V(x) : typed(1.5));
      b.mov(m, typed(2.5));
      b.cmp(GRJIT_CMP_LT, c, side == 0 ? typed(2.0) : V(s), side == 0 ? V(s) : typed(2.0));
      GRJIT_VReg sum = b.reg(T);
      b.bin(GRJIT_OP_FADD, sum, V(s), V(m));
      b.un(GRJIT_OP_REINTERPRET, r, V(sum));
      GRJIT_VReg cw = c;
      if (dbl) {
        cw = b.reg(GRJIT_TYPE_I64);
        b.un(GRJIT_OP_EXTEND_U, cw, V(c));
      }
      b.bin(GRJIT_OP_XOR, r, V(r), V(cw));
      b.ret(V(r));
      Fn f(b.finish());
      char why[256] = {0};
      ASSERT_EQ(grjit_function_verify(f, nullptr, why, sizeof why), GRJIT_OK) << why;
      Compiled code(f, w.pages());
      ASSERT_TRUE(code);
      for (double v : {0.25, 0.5, -3.0, 100.0}) {
        Outcome o = run(code.code, w.ctx, {bits(v)});
        ireval::Result e = ireval::eval(f, w.ctx, nullptr, {bits(v)});
        EXPECT_EQ(o.out[0], e.out[0]) << v;
      }
    }
    // the other width is refused
    B bad("bad", 0);
    GRJIT_VReg x = bad.param(T), r = bad.reg(T);
    bad.at(bad.block());
    bad.bin(GRJIT_OP_FADD, r, V(x), dbl ? grjit_operand_f32_imm(0) : grjit_operand_f64_imm(0));
    bad.ret(V(r));
    Fn f(bad.finish());
    EXPECT_NE(refused(f.f), "");
  }
  // The bounds of i32.trunc_f64_s written as typed immediates verify, and run.
  B b("trunc", 0);
  GRJIT_VReg x = b.param(GRJIT_TYPE_F64), l = b.reg(GRJIT_TYPE_I32), h = b.reg(GRJIT_TYPE_I32),
             ok = b.reg(GRJIT_TYPE_I32), r = b.reg(GRJIT_TYPE_I32);
  b.at(b.block());
  b.cmp(GRJIT_CMP_GT, l, V(x), grjit_operand_f64_imm(B64(-2147483649.0)));
  b.cmp(GRJIT_CMP_GT, h, grjit_operand_f64_imm(B64(2147483648.0)), V(x));
  b.bin(GRJIT_OP_AND, ok, V(l), V(h));
  b.guard(V(ok), {kTrapFunction, kSiteIntegerOverflow});
  b.un(GRJIT_OP_TRUNC_S, r, V(x));
  b.ret(V(r));
  Fn f(b.finish());
  char why[256] = {0};
  ASSERT_EQ(grjit_function_verify(f, nullptr, why, sizeof why), GRJIT_OK) << why;
  Compiled code(f, w.pages());
  ASSERT_TRUE(code);
  EXPECT_EQ(said(code.code, run(code.code, w.ctx, {B64(-2147483648.9)})), hex(0x80000000u));
  EXPECT_EQ(said(code.code, run(code.code, w.ctx, {B64(2147483648.0)})), "trap: integer overflow");
}

GRJIT_TEST_MAIN()
