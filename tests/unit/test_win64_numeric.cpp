/**
 * @file
 *
 * The Win64 half of spec-runtime-float's story 5 that is specific to its ABI: what the Windows x86-64 flavour of
 * the x86-64 backend encodes for the numeric operations, on every host (emission runs everywhere, so a Linux
 * laptop checks the Win64 bytes it emits), and, under wine, the Microsoft x64 helper ABI against a probe in MinGW
 * assembly, a float result with junk above bit 31, and sentinels in the callee-saved registers.
 *
 * Win64's callee-saved registers are `rbx`, `rbp`, `rdi`, `rsi`, `r12`-`r15` and `xmm6`-`xmm15`; the flavour
 * encodes none of them (`rbp` is the frame register, saved and restored by the prologue and the epilogue).
 *
 * Copyright 2026 by Corey Pennycuff
 */

#define FX_ASM_SENTINELS
#include "../calls_asm.h"
#include "test_helpers.h"

#include "../float_fixture.h"
#include "../int_fixture.h"
#include "../sentinel_fixture.h"

#include "../../src/code/code_internal.h"
#include "../../src/x86_64/asm_internal.h"

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

namespace {

constexpr uint32_t kRequestOffset = 0x40;

constexpr uint32_t kCalleeSavedGpr = (1u << GRJIT_RBX) | (1u << GRJIT_RSI) | (1u << GRJIT_RDI) |
    (1u << GRJIT_R12) | (1u << GRJIT_R13) | (1u << GRJIT_R14) | (1u << GRJIT_R15);
constexpr uint32_t kScratchGpr = (1u << GRJIT_RAX) | (1u << GRJIT_RCX) | (1u << GRJIT_RDX) |
    (1u << GRJIT_R8) | (1u << GRJIT_R9) | (1u << GRJIT_R10) | (1u << GRJIT_R11);

struct Emitted {
  GRJIT_Emitted e{};
  GRJIT_Result result;
  Emitted(const GRJIT_Function * f, GRJIT_Arch arch) {
    result = grjit_emit_for(arch, f, grjit_allocator_default(), nullptr, nullptr, kRequestOffset, &e);
  }
  ~Emitted() { grjit_emitted_free(&e); }
  Emitted(const Emitted &) = delete;
  Emitted & operator=(const Emitted &) = delete;
};

} // namespace

TEST(Win64Numeric, NoCalleeSavedGeneralRegisterIsEncodedForAnyIntegerInstructionAtEitherWidth) {
  uint32_t seen = 0;
  size_t checked = 0;
  for (const char * ty : {"i32", "i64"}) {
    for (const std::string & instr : intfx::all_int_ops(ty)) {
      intfx::WasmOp op = intfx::build_wasm_op(instr);
      Fn f(op.function);
      Emitted w(f, GRJIT_ARCH_X86_64_WIN64);
      ASSERT_EQ(w.result, GRJIT_OK) << instr;
      EXPECT_EQ(w.e.regs_used & kCalleeSavedGpr, 0u) << instr << ": rbx, rsi, rdi or r12-r15 was encoded";
      EXPECT_EQ(w.e.regs_used & ~(kScratchGpr | (1u << GRJIT_RSP) | (1u << GRJIT_RBP)), 0u) << instr;
      seen |= w.e.regs_used;
      checked++;
    }
  }
  EXPECT_GT(checked, 60u);
  EXPECT_NE(seen & (1u << GRJIT_RAX), 0u);
  EXPECT_NE(seen & (1u << GRJIT_RCX), 0u);
  EXPECT_NE(seen & (1u << GRJIT_RDX), 0u) << "the divisions and the bit counts use rdx";
}

/* ---- No callee-saved vector register, for any instruction of the tables ------------------------
 *
 * The scan is the assembler's record of the `xmm` registers it encoded (the same record the arm64 half reads of `v`).
 * Microsoft x64 keeps `xmm6` to `xmm15` for the caller: the assembler has `xmm0` to `xmm7` only, so the question is
 * `xmm6` and `xmm7`, which the SSE2 rounding sequence named until the Win64 variant took two dead registers. The
 * control: the SysV flavour of the same functions, without the SSE4.1 arm, does name them. */

namespace {

constexpr uint32_t kCalleeSavedXmm = (1u << 6) | (1u << 7); // the only callee-saved ones the assembler can encode

struct Built {
  floatfx::FloatOp op;
  ~Built() { grjit_function_destroy(op.function); }
};

/* A helper call with `n` arguments of mixed class, to reach xmm up to the fourth slot. */
GRJIT_Function * call_with_mixed(size_t n) {
  B b("callm", 0);
  std::vector<GRJIT_VReg> p;
  for (size_t i = 0; i < n; i++) {
    p.push_back(b.param(i % 3 == 0 ? GRJIT_TYPE_I64 : i % 3 == 1 ? GRJIT_TYPE_F64 : GRJIT_TYPE_F32));
  }
  GRJIT_VReg r = b.reg(GRJIT_TYPE_F64);
  b.at(b.block());
  std::vector<GRJIT_Operand> args;
  for (GRJIT_VReg v : p) {
    args.push_back(V(v));
  }
  b.call(r, reinterpret_cast<const void *>(0x10000), args);
  b.ret(V(r));
  return b.finish();
}

} // namespace

TEST(Win64Numeric, NoCalleeSavedVectorRegisterIsEncodedForAnyFloatInstructionAndTheScratchOnesAreAllUsed) {
  uint32_t seen = 0;
  size_t checked = 0;
  // Under no feature (the v1 sequences, the Win64 rounding variant among them) and under all four (the arms).
  auto scan = [&](const GRJIT_Function * f, const std::string & what) {
    for (GRJIT_Features set : {GRJIT_FEATURES_NONE, GRJIT_FEATURES_ALL}) {
      GRJIT_Emitted e{};
      ASSERT_EQ(grjit_emit_for_features(GRJIT_ARCH_X86_64_WIN64, f, grjit_allocator_default(), nullptr, nullptr,
                    kRequestOffset, set, &e),
          GRJIT_OK)
          << what;
      EXPECT_EQ(e.fp_regs_used & kCalleeSavedXmm, 0u) << what << ": xmm6 or xmm7 was encoded (set " << set << ")";
      EXPECT_EQ(e.regs_used & kCalleeSavedGpr, 0u) << what;
      if (set == GRJIT_FEATURES_NONE) {
        seen |= e.fp_regs_used;
      }
      grjit_emitted_free(&e);
      checked++;
    }
  };
  for (const std::string & instr : floatfx::every_float_instruction()) {
    Built b;
    b.op = floatfx::build_any_op(instr);
    ASSERT_NE(b.op.function, nullptr) << instr;
    scan(b.op.function, instr);
  }
  for (size_t n = 0; n <= 6; n++) {
    Fn f(call_with_mixed(n));
    scan(f, "a helper call with " + std::to_string(n) + " mixed arguments");
  }
  EXPECT_GT(checked, 140u);
  EXPECT_EQ(seen, 0x3Fu) << "xmm0 to xmm5 are what the Win64 flavour uses, each of them by something";
}

TEST(Win64Numeric, TheSysVFlavourWithoutTheSse41ArmDoesNameXmm6AndXmm7SoTheScanCanFail) {
  // The control: the v1 rounding in the SysV flavour names xmm6 and xmm7 (MASK and ONE); the same functions under
  // Win64 do not, and the SSE4.1 arm (`roundss`/`roundsd` on xmm0) names neither.
  uint32_t sysv_v1 = 0, sysv_sse41 = 0, win = 0;
  for (const char * ty : {"f32", "f64"}) {
    for (const char * op : {"ceil", "floor", "trunc", "nearest"}) {
      Built b;
      b.op = floatfx::build_any_op(std::string(ty) + "." + op);
      GRJIT_Emitted e{};
      ASSERT_EQ(grjit_emit_for_features(GRJIT_ARCH_X86_64, b.op.function, grjit_allocator_default(), nullptr, nullptr,
                    kRequestOffset, GRJIT_FEATURES_NONE, &e),
          GRJIT_OK);
      sysv_v1 |= e.fp_regs_used;
      grjit_emitted_free(&e);
      ASSERT_EQ(grjit_emit_for_features(GRJIT_ARCH_X86_64, b.op.function, grjit_allocator_default(), nullptr, nullptr,
                    kRequestOffset, GRJIT_FEATURE_SSE4_1, &e),
          GRJIT_OK);
      sysv_sse41 |= e.fp_regs_used;
      grjit_emitted_free(&e);
      ASSERT_EQ(grjit_emit_for_features(GRJIT_ARCH_X86_64_WIN64, b.op.function, grjit_allocator_default(), nullptr,
                    nullptr, kRequestOffset, GRJIT_FEATURES_NONE, &e),
          GRJIT_OK);
      win |= e.fp_regs_used;
      grjit_emitted_free(&e);
    }
  }
  EXPECT_EQ(sysv_v1 & kCalleeSavedXmm, kCalleeSavedXmm) << "the SysV v1 rounding uses xmm6 and xmm7";
  EXPECT_EQ(sysv_sse41 & kCalleeSavedXmm, 0u) << "the SSE4.1 arm uses xmm0 only";
  EXPECT_EQ(win & kCalleeSavedXmm, 0u) << "the Win64 variant reuses dead registers";
  EXPECT_NE(win & (1u << 2), 0u) << "and those are xmm2 and xmm3";
  EXPECT_NE(win & (1u << 3), 0u);
}

/* ---- Helper arguments by position -------------------------------------------------------------
 *
 * Microsoft x64: argument i < 4 is in the register of position i (`rcx rdx r8 r9` for an integer, `xmm0`-`xmm3` for a
 * float, the slot number being the same for both); the fifth and sixth are stack words at `[rsp + 32]` and
 * `[rsp + 40]`, above the 32 bytes of shadow space. The bytes of the argument loads are checked here, on any host;
 * the probe below runs them under wine. (i64, f64, i64, f32) is rcx, xmm1, r8, xmm3. */

namespace {

size_t count_bytes(const uint8_t * p, size_t n, std::initializer_list<uint8_t> pat) {
  size_t found = 0;
  for (size_t i = 0; i + pat.size() <= n; i++) {
    if (std::equal(pat.begin(), pat.end(), p + i)) {
      found++;
    }
  }
  return found;
}

/* The number of stores of a general register (`mov [rsp + d], r`, opcode 89) or of a float (`movsd` / `movss`,
 * 0F 11) to one of the four shadow-space words, [rsp + 0], [rsp + 8], [rsp + 16], [rsp + 24]. It reads the
 * encoding: optional F2 / F3, optional REX, the opcode, a ModRM with rm = 4 (a SIB) and mod = 0 or 1, the SIB
 * 0x24 (base rsp, no index) and the 8-bit displacement. */
size_t shadow_space_stores(const uint8_t * p, size_t n) {
  size_t found = 0;
  for (size_t i = 0; i < n; i++) {
    size_t j = i;
    bool is_float = false;
    if (p[j] == 0xF2 || p[j] == 0xF3) {
      is_float = true;
      j++;
    }
    if (j < n && p[j] >= 0x40 && p[j] <= 0x4F) {
      j++;
    }
    if (j + 1 < n && p[j] == 0x0F && p[j + 1] == 0x11 && is_float) {
      j += 2;
    } else if (j < n && p[j] == 0x89 && !is_float) {
      j++;
    } else {
      continue;
    }
    if (j + 1 >= n || (p[j] & 7) != 4 || p[j + 1] != 0x24) {
      continue;
    }
    const unsigned mod = p[j] >> 6;
    if (mod == 0) {
      found++; // [rsp + 0]
      i = j + 1; // the instruction is counted once, not again from its opcode byte
    } else if (mod == 1 && j + 2 < n && p[j + 2] < 32 && p[j + 2] % 8 == 0) {
      found++;
      i = j + 2;
    }
  }
  return found;
}

} // namespace

TEST(Win64Numeric, AHelperCallLoadsEachArgumentIntoTheRegisterOfItsPositionAndTheFifthAndSixthIntoTheStackWords) {
  // f(a: i64, b: f64, c: i64, d: f32, e: f64, g: f32): call(a, b, c, d, e, g)
  B b("pos", 0);
  GRJIT_VReg a = b.param(GRJIT_TYPE_I64), x = b.param(GRJIT_TYPE_F64), c = b.param(GRJIT_TYPE_I64),
             d = b.param(GRJIT_TYPE_F32), e5 = b.param(GRJIT_TYPE_F64), g = b.param(GRJIT_TYPE_F32),
             r = b.reg(GRJIT_TYPE_I64);
  b.at(b.block());
  b.call(r, reinterpret_cast<const void *>(0x10000), {V(a), V(x), V(c), V(d), V(e5), V(g)});
  b.ret(V(r));
  Fn f(b.finish());
  Emitted w(f, GRJIT_ARCH_X86_64_WIN64);
  ASSERT_EQ(w.result, GRJIT_OK);
  const uint8_t * code = w.e.bytes;
  const size_t n = w.e.size;
  // The slots are below rbp: disp8 for the first sixteen, disp32 beyond. movsd xmm1, [rbp + d] is F2 0F 10 4D / 8D;
  // movss xmm3, [rbp + d] is F3 0F 10 5D / 9D; mov rcx, [rbp + d] is 48 8B 4D / 8D; mov r8, [rbp + d] is 4C 8B 45 / 85.
  EXPECT_GE(count_bytes(code, n, {0x48, 0x8B, 0x4D}) + count_bytes(code, n, {0x48, 0x8B, 0x8D}), 1u)
      << "the first argument (an integer) is loaded into rcx";
  EXPECT_GE(count_bytes(code, n, {0xF2, 0x0F, 0x10, 0x4D}) + count_bytes(code, n, {0xF2, 0x0F, 0x10, 0x8D}), 1u)
      << "the second argument (an f64) is loaded into xmm1, the register of position 1";
  EXPECT_EQ(count_bytes(code, n, {0xF2, 0x0F, 0x10, 0x45}) + count_bytes(code, n, {0xF2, 0x0F, 0x10, 0x85}), 0u)
      << "and nothing is loaded into xmm0 as a double: a class numbering would put it there";
  EXPECT_GE(count_bytes(code, n, {0x4C, 0x8B, 0x45}) + count_bytes(code, n, {0x4C, 0x8B, 0x85}), 1u)
      << "the third (an integer) is loaded into r8, not into rdx";
  EXPECT_GE(count_bytes(code, n, {0xF3, 0x0F, 0x10, 0x5D}) + count_bytes(code, n, {0xF3, 0x0F, 0x10, 0x9D}), 1u)
      << "the fourth (an f32) is loaded into xmm3 as a single";
  // the fifth and sixth go to [rsp + 32] and [rsp + 40]: mov [rsp + 32], rax is 48 89 44 24 20
  EXPECT_EQ(count_bytes(code, n, {0x48, 0x89, 0x44, 0x24, 0x20}), 1u) << "the fifth argument is at [rsp + 32]";
  EXPECT_EQ(count_bytes(code, n, {0x48, 0x89, 0x44, 0x24, 0x28}), 1u) << "the sixth argument is at [rsp + 40]";
  EXPECT_EQ(shadow_space_stores(code, n), 0u) << "and nothing is stored into the four shadow-space words [rsp + 0..24]";
  // The scan must be able to fail: it sees a store of rax, of rcx, of a double and of a single there, and not one at
  // [rsp + 32] or a load from [rsp + 8].
  const uint8_t rax0[] = {0x48, 0x89, 0x04, 0x24}, rcx8[] = {0x48, 0x89, 0x4C, 0x24, 0x08},
                d16[] = {0xF2, 0x0F, 0x11, 0x44, 0x24, 0x10}, s24[] = {0xF3, 0x0F, 0x11, 0x5C, 0x24, 0x18},
                r9_8[] = {0x4C, 0x89, 0x4C, 0x24, 0x08}, at32[] = {0x48, 0x89, 0x44, 0x24, 0x20},
                load8[] = {0x48, 0x8B, 0x4C, 0x24, 0x08};
  EXPECT_EQ(shadow_space_stores(rax0, sizeof rax0), 1u);
  EXPECT_EQ(shadow_space_stores(rcx8, sizeof rcx8), 1u);
  EXPECT_EQ(shadow_space_stores(d16, sizeof d16), 1u);
  EXPECT_EQ(shadow_space_stores(s24, sizeof s24), 1u);
  EXPECT_EQ(shadow_space_stores(r9_8, sizeof r9_8), 1u);
  EXPECT_EQ(shadow_space_stores(at32, sizeof at32), 0u);
  EXPECT_EQ(shadow_space_stores(load8, sizeof load8), 0u);
}

/* ---- Under wine: the Microsoft x64 helper probe, a float result with junk above bit 31, the sentinels -----------
 *
 * A probe in MinGW assembly records `rcx rdx r8 r9`, `xmm0`-`xmm3` and the two stack words above the shadow space, so
 * the registers are read as the callee sees them (a C++ function cannot say: the compiler may use its arguments as it
 * likes). Every mix of integer, f64 and f32 arguments up to six is checked against the position rule. */

#if defined(_WIN64) && defined(__x86_64__)

extern "C" {
uint64_t grjit_test_w64_regs[10]; // rcx rdx r8 r9, then xmm0 to xmm3 (low 64 bits), then [rsp+40] and [rsp+48] at entry
uint64_t grjit_test_w64_probe(void);
float grjit_test_w64_junk_float(void);
float grjit_test_w64_f32(void);
double grjit_test_w64_f64(void);
}
asm(R"(
  .text
  .globl grjit_test_w64_probe
grjit_test_w64_probe:
  leaq grjit_test_w64_regs(%rip), %r11
  movq %rcx, 0(%r11)
  movq %rdx, 8(%r11)
  movq %r8, 16(%r11)
  movq %r9, 24(%r11)
  movq %xmm0, 32(%r11)
  movq %xmm1, 40(%r11)
  movq %xmm2, 48(%r11)
  movq %xmm3, 56(%r11)
  movq 40(%rsp), %rax
  movq %rax, 64(%r11)
  movq 48(%rsp), %rax
  movq %rax, 72(%r11)
  xorl %eax, %eax
  ret

  .globl grjit_test_w64_junk_float
grjit_test_w64_junk_float:
  movabsq $0xDEADBEEF3FC00000, %rax
  movq %rax, %xmm0
  ret

  .globl grjit_test_w64_f32
grjit_test_w64_f32:
  movabsq $0xDEADBEEF40200000, %rax
  movq %rax, %xmm0
  ret

  .globl grjit_test_w64_f64
grjit_test_w64_f64:
  movabsq $0xC00C000000000001, %rax
  movq %rax, %xmm0
  ret
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
  b.call(r, reinterpret_cast<const void *>(&grjit_test_w64_probe), args);
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
  std::memset(grjit_test_w64_regs, 0xEE, sizeof grjit_test_w64_regs);
  floatfx::Outcome o = floatfx::run(code.code, w.ctx, words);
  if (o.fault != 0 || o.exit != GRJIT_EXIT_RETURNED) {
    return ::testing::AssertionFailure() << "the probe did not return";
  }
  char buf[220];
  for (size_t i = 0; i < kinds.size(); i++) {
    const uint64_t v = arg_value(kinds[i], i);
    const bool fl = is_float_arg(kinds[i]);
    const bool single = kinds[i] == Arg::F32 || kinds[i] == Arg::F32Imm;
    const uint64_t mask = single ? 0xFFFFFFFFull : ~uint64_t{0};
    uint64_t got;
    const char * where;
    if (i < 4) {
      got = fl ? grjit_test_w64_regs[4 + i] : grjit_test_w64_regs[i];
      where = fl ? "xmm" : "the integer register";
    } else {
      got = grjit_test_w64_regs[8 + (i - 4)];
      where = "the stack word";
    }
    if ((fl || i >= 4 ? got & mask : got) != (fl || i >= 4 ? v & mask : v)) {
      std::snprintf(buf, sizeof buf, "argument %zu (%s) should be in %s of position %zu as 0x%llx but it holds 0x%llx", i,
          fl ? "a float" : "an integer", where, i, static_cast<unsigned long long>(v),
          static_cast<unsigned long long>(got));
      return ::testing::AssertionFailure() << buf;
    }
  }
  return ::testing::AssertionSuccess();
}

} // namespace

TEST(Win64Helpers, EveryMixOfIntegerAndFloatArgumentsArrivesInTheRegisterOfItsPositionAndTheFifthAndSixthOnTheStack) {
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
  // Six arguments with immediates in the stack positions.
  ASSERT_TRUE(probe_pattern(w, {Arg::I64, Arg::F64, Arg::I64, Arg::F32, Arg::F64Imm, Arg::F32Imm}));
  ASSERT_TRUE(probe_pattern(w, {Arg::F64, Arg::F64, Arg::F64, Arg::F64, Arg::I64Imm, Arg::F64}));
  std::printf("float helper ABI (win64): %zu argument patterns checked against the Microsoft x64 positions\n", patterns);
  EXPECT_GT(patterns, 2000u);
}

TEST(Win64Helpers, TheNamedPositionFromTheStoryComesOutAsStated) {
  GRJIT_REQUIRE_BACKEND();
  JitWorld w;
  // (i64, f64, i64, f32) is rcx, xmm1, r8, xmm3.
  ASSERT_TRUE(probe_pattern(w, {Arg::I64, Arg::F64, Arg::I64, Arg::F32}));
  EXPECT_EQ(grjit_test_w64_regs[0], arg_value(Arg::I64, 0));
  EXPECT_EQ(grjit_test_w64_regs[5], arg_value(Arg::F64, 1));
  EXPECT_EQ(grjit_test_w64_regs[2], arg_value(Arg::I64, 2));
  EXPECT_EQ(grjit_test_w64_regs[7] & 0xFFFFFFFFull, arg_value(Arg::F32, 3));
}

TEST(Win64Helpers, AFloatResultIsTakenFromXmm0ByItsOwnWidthWhateverIsAboveBit31) {
  GRJIT_REQUIRE_BACKEND();
  JitWorld w;
  struct Case {
    const void * fn;
    GRJIT_Type result;
    uint64_t want;
  };
  const Case cases[] = {
      {reinterpret_cast<const void *>(&grjit_test_w64_junk_float), GRJIT_TYPE_F32, 0x3FC00000u},
      {reinterpret_cast<const void *>(&grjit_test_w64_f32), GRJIT_TYPE_F32, 0x40200000u},
      {reinterpret_cast<const void *>(&grjit_test_w64_f64), GRJIT_TYPE_F64, 0xC00C000000000001ull},
  };
  for (const Case & c : cases) {
    SCOPED_TRACE(c.result);
    B b("result", 0);
    GRJIT_VReg r = b.reg(c.result);
    b.at(b.block());
    b.call(r, c.fn, {});
    b.ret(V(r)); // the call's own word is returned: a MOVE would cut an f32 and hide a result taken whole
    Fn f(b.finish());
    Compiled code(f, w.pages());
    ASSERT_TRUE(code);
    EXPECT_EQ(floatfx::run(code.code, w.ctx, {}).out[0], c.want) << "an f32 is its low 32 bits, zero-extended";
  }
}

#else // not Win64: the same tests, skipped by name

TEST(Win64Helpers, EveryMixOfIntegerAndFloatArgumentsArrivesInTheRegisterOfItsPositionAndTheFifthAndSixthOnTheStack) {
  GTEST_SKIP() << "the Microsoft x64 helper ABI is checked against a MinGW assembly probe, on Windows x86-64 only (wine)";
}

TEST(Win64Helpers, TheNamedPositionFromTheStoryComesOutAsStated) {
  GTEST_SKIP() << "Windows x86-64 only (a Microsoft x64 probe)";
}

TEST(Win64Helpers, AFloatResultIsTakenFromXmm0ByItsOwnWidthWhateverIsAboveBit31) {
  GTEST_SKIP() << "Windows x86-64 only (a Microsoft x64 probe)";
}

#endif // Win64

/* The callee-saved xmm6-xmm15, asserted by the sentinel caller of calls_asm.h (`fx_call_with_sentinels`, which loads
 * rbx rbp rdi rsi r12-r15 and xmm6-xmm15 with known values, calls compiled code and reads them back): across a
 * return, a guard exit, a poll whose slow path runs, a poll the helper refuses, and a helper that trashes every
 * caller-saved xmm register with a float live across it. The structural scan above shows `never`; this shows the scan is
 * looking at the right registers (defect 88 is caught by both). */

#if FX_HAVE_CALLS_ASM && FX_ASM_WIN64

namespace {

/* Takes a double, trashes every caller-saved xmm register (xmm0-xmm5 under Microsoft x64), saves and restores the
 * callee-saved ones it clobbers (the compiler does, being told), and returns the double. */
__attribute__((noinline)) double fp_clobber(double x) {
  double r = x;
  asm volatile(
      "pcmpeqd %%xmm0, %%xmm0\n pcmpeqd %%xmm1, %%xmm1\n pcmpeqd %%xmm2, %%xmm2\n pcmpeqd %%xmm3, %%xmm3\n"
      "pcmpeqd %%xmm4, %%xmm4\n pcmpeqd %%xmm5, %%xmm5\n pcmpeqd %%xmm6, %%xmm6\n pcmpeqd %%xmm7, %%xmm7\n"
      "pcmpeqd %%xmm8, %%xmm8\n pcmpeqd %%xmm9, %%xmm9\n pcmpeqd %%xmm10, %%xmm10\n pcmpeqd %%xmm11, %%xmm11\n"
      "pcmpeqd %%xmm12, %%xmm12\n pcmpeqd %%xmm13, %%xmm13\n pcmpeqd %%xmm14, %%xmm14\n pcmpeqd %%xmm15, %%xmm15\n"
      :
      :
      : "xmm0", "xmm1", "xmm2", "xmm3", "xmm4", "xmm5", "xmm6", "xmm7", "xmm8", "xmm9", "xmm10", "xmm11", "xmm12",
        "xmm13", "xmm14", "xmm15", "memory");
  return r;
}

} // namespace

TEST(Win64Sentinels, FloatCodeLeavesEveryCalleeSavedRegisterAloneOnAReturnAGuardExitAPollAndATrashingHelper) {
  GRJIT_REQUIRE_BACKEND();
  sentfx::SentinelWorld w;
  Fn f(sentfx::sentinel_function(reinterpret_cast<const void *>(&fp_clobber)));
  char why[256] = {0};
  ASSERT_EQ(grjit_function_verify(f, nullptr, why, sizeof why), GRJIT_OK) << why;
  /* Every CPU feature disabled: the host's own set would pick the SSE4.1 rounding and leave the SSE2
   * sequence, which has the vector registers to get right, unemitted on a host that has the feature. */
  Compiled c(f, w.pages(), nullptr, nullptr, nullptr, GRJIT_FEATURES_ALL);
  ASSERT_TRUE(c) << grjit_result_string(c.result);
  alignas(16) uint8_t mem[64] = {0};
  const std::vector<uint64_t> args = {floatfx::f64bits(2.75), floatfx::f32bits(1.5f), 1,
      reinterpret_cast<uint64_t>(mem)};
  {
    sentfx::SentinelRun r = sentfx::run_with_sentinels(w, c.code, args, "a return");
    ASSERT_EQ(r.exit, uint32_t{GRJIT_EXIT_RETURNED});
    EXPECT_TRUE(r.intact);
    alignas(16) uint8_t mem2[64] = {0};
    std::vector<uint64_t> same = args;
    same[3] = reinterpret_cast<uint64_t>(mem2);
    Fn twin(sentfx::sentinel_function(nullptr));
    ireval::Result e = ireval::eval(twin, w.ctx, nullptr, same);
    ASSERT_EQ(e.exit, uint32_t{GRJIT_EXIT_RETURNED});
    EXPECT_EQ(r.out[0], e.out[0]) << "the helper that trashed the xmm registers cost the live float nothing";
    EXPECT_EQ(sentfx::g_polls, 0);
  }
  {
    std::vector<uint64_t> zero = args;
    zero[2] = 0;
    sentfx::SentinelRun r = sentfx::run_with_sentinels(w, c.code, zero, "a guard exit");
    EXPECT_EQ(r.exit, uint32_t{GRJIT_EXIT_DEOPT});
    EXPECT_TRUE(r.intact);
  }
  {
    w.post();
    sentfx::SentinelRun r = sentfx::run_with_sentinels(w, c.code, args, "a poll's slow path");
    EXPECT_EQ(r.exit, uint32_t{GRJIT_EXIT_RETURNED});
    EXPECT_TRUE(r.intact);
    EXPECT_EQ(sentfx::g_polls, 1) << "the slow path ran";
  }
  {
    sentfx::g_answer = 7;
    sentfx::SentinelRun r = sentfx::run_with_sentinels(w, c.code, args, "a poll refusal");
    EXPECT_EQ(r.exit, uint32_t{GRJIT_EXIT_REFUSED});
    EXPECT_TRUE(r.intact);
    EXPECT_EQ(sentfx::g_polls, 2);
  }
}

#else

TEST(Win64Sentinels, FloatCodeLeavesEveryCalleeSavedRegisterAloneOnAReturnAGuardExitAPollAndATrashingHelper) {
  GTEST_SKIP() << "the sentinel caller loads and reads xmm6-xmm15 and rbx rbp rdi rsi r12-r15: Windows x86-64 only (wine)";
}

#endif // FX_HAVE_CALLS_ASM && FX_ASM_WIN64

GRJIT_TEST_MAIN()
