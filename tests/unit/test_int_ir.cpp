/**
 * @file
 *
 * The integer IR (spec-runtime-float, story 2): the `I32` type and the integer operations of
 * numeric-operations.md at both widths, their typing rules, the verifier's rule that a divide or
 * a remainder is preceded by its guards, and the code of each backend run against hand-worked
 * answers and the test evaluator: natively on x86-64 SysV and on arm64 (under qemu-aarch64 or on a
 * Raspberry Pi), under wine for Win64. A laptop of another architecture runs what it can: the arm64
 * code through the simulator in the differential, and every backend's emission in the refusal
 * tests below.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include "../int_fixture.h"
#include "../feature_fixture.h"

#include "../calls_fixture.h"
#include "../ir_eval.h"

#include "../../src/code/code_internal.h"

#include <cstdint>
#include <string>
#include <vector>

using namespace intfx;
using featfx::detected;
using featfx::need_of;
using featfx::two_arms;

/* Every backend runs these where it has a native host: x86-64 SysV and arm64 natively, Win64 under wine. A
 * hardware fault in compiled code is turned into a result on POSIX hosts and not on Windows (there is no handler),
 * so no test reaches an operation with its guard missing on Win64: the verifier refuses that shape first. */
#define INT_RUN_HERE() GRJIT_REQUIRE_BACKEND()

namespace {

constexpr uint64_t M32 = 0xFFFFFFFFu;

/* ---- The boundary values ----------------------------------------------------------------- */

std::vector<uint64_t> boundary(bool narrow) {
  std::vector<uint64_t> v = {0, 1, 2, 3, 7, 8, 15, 16, 31, 32, 33, 63, 64, 65, 0x7F, 0x80, 0xFF,
      0x7FFF, 0x8000, 0xFFFF, 0x10000, 0x7FFFFFFE, 0x7FFFFFFF, 0x80000000u, 0x80000001u,
      0xFFFFFF80u, 0xFFFF8000u, 0xFFFFFFFEu, 0xFFFFFFFFu, 0x55555555u, 0xAAAAAAAAu, 0x12345678u};
  if (!narrow) {
    for (uint64_t x : std::vector<uint64_t>{0x100000000ull, 0x100000001ull, 0x7FFFFFFFFFFFFFFEull,
             0x7FFFFFFFFFFFFFFFull, 0x8000000000000000ull, 0x8000000000000001ull,
             0xFFFFFFFF80000000ull, 0xFFFFFFFFFFFFFF80ull, 0xFFFFFFFFFFFF8000ull,
             0xFFFFFFFFFFFFFFFEull, 0xFFFFFFFFFFFFFFFFull, 0x5555555555555555ull,
             0xAAAAAAAAAAAAAAAAull, 0x123456789ABCDEF0ull, 127, 128}) {
      v.push_back(x);
    }
  }
  return v;
}

struct Worked {
  const char * instr;
  uint64_t a, b;
  uint64_t want;
};

/* Answers worked from the WebAssembly semantics by hand, not by the evaluator or the backend. The
 * result of an i32 operation is the low 32 bits in a zero-extended word. Shift and rotate counts
 * are taken modulo the width; clz and ctz of 0 are the width; the comparisons answer 1 or 0. */
const Worked kWorked[] = {
    {"i32.add", 0xFFFFFFFF, 1, 0},
    {"i32.add", 0x7FFFFFFF, 1, 0x80000000},
    {"i32.sub", 0, 1, 0xFFFFFFFF},
    {"i32.sub", 0x80000000, 1, 0x7FFFFFFF},
    {"i32.sub", 5, 9, 0xFFFFFFFC},
    {"i32.mul", 0x10000, 0x10000, 0},
    {"i32.mul", 0xFFFFFFFF, 0xFFFFFFFF, 1},
    {"i32.mul", 0x80000000, 2, 0},
    {"i32.and", 0xF0F0F0F0, 0xFF00FF00, 0xF000F000},
    {"i32.or", 0xF0F0F0F0, 0x0F0F0F0F, 0xFFFFFFFF},
    {"i32.xor", 0xFFFFFFFF, 0x0F0F0F0F, 0xF0F0F0F0},
    {"i32.shl", 1, 31, 0x80000000},
    {"i32.shl", 1, 32, 1},
    {"i32.shl", 1, 33, 2},
    {"i32.shl", 0x80000001, 1, 2},
    {"i32.shl", 1, 0xFFFFFFFF, 0x80000000},
    {"i32.shr_u", 0x80000000, 31, 1},
    {"i32.shr_u", 0x80000000, 32, 0x80000000},
    {"i32.shr_u", 0xFFFFFFFF, 33, 0x7FFFFFFF},
    {"i32.shr_s", 0x80000000, 31, 0xFFFFFFFF},
    {"i32.shr_s", 0x80000000, 32, 0x80000000},
    {"i32.shr_s", 0x80000000, 1, 0xC0000000},
    {"i32.shr_s", 0x7FFFFFFF, 31, 0},
    {"i32.rotl", 0x80000001, 1, 3},
    {"i32.rotl", 0x12345678, 32, 0x12345678},
    {"i32.rotl", 0x12345678, 4, 0x23456781},
    {"i32.rotl", 0x12345678, 36, 0x23456781},
    {"i32.rotr", 0x12345678, 4, 0x81234567},
    {"i32.rotr", 3, 1, 0x80000001},
    {"i32.rotr", 0x12345678, 0, 0x12345678},
    {"i32.clz", 0, 0, 32},
    {"i32.clz", 1, 0, 31},
    {"i32.clz", 0x80000000, 0, 0},
    {"i32.clz", 0xFFFFFFFF, 0, 0},
    {"i32.clz", 0x00008000, 0, 16},
    {"i32.ctz", 0, 0, 32},
    {"i32.ctz", 1, 0, 0},
    {"i32.ctz", 0x80000000, 0, 31},
    {"i32.ctz", 0x00010000, 0, 16},
    {"i32.popcnt", 0, 0, 0},
    {"i32.popcnt", 0xFFFFFFFF, 0, 32},
    {"i32.popcnt", 0x55555555, 0, 16},
    {"i32.popcnt", 0x80000001, 0, 2},
    {"i32.eqz", 0, 0, 1},
    {"i32.eqz", 0x80000000u, 0, 0},
    {"i32.eq", 0x80000000, 0x80000000, 1},
    {"i32.ne", 0x80000000, 0x80000000, 0},
    {"i32.lt_s", 0x80000000, 0, 1},
    {"i32.lt_u", 0x80000000, 0, 0},
    {"i32.lt_s", 0xFFFFFFFF, 0, 1},
    {"i32.gt_s", 0x7FFFFFFF, 0x80000000, 1},
    {"i32.gt_u", 0x7FFFFFFF, 0x80000000, 0},
    {"i32.le_s", 0x80000000, 0x80000000, 1},
    {"i32.le_u", 1, 0, 0},
    {"i32.ge_s", 0, 0xFFFFFFFF, 1},
    {"i32.ge_u", 0, 0xFFFFFFFF, 0},
    {"i32.extend8_s", 0x7F, 0, 0x7F},
    {"i32.extend8_s", 0x80, 0, 0xFFFFFF80},
    {"i32.extend8_s", 0x1234FF, 0, 0xFFFFFFFF},
    {"i32.extend16_s", 0x7FFF, 0, 0x7FFF},
    {"i32.extend16_s", 0x8000, 0, 0xFFFF8000},
    {"i32.extend16_s", 0x12348001, 0, 0xFFFF8001},
    {"i32.div_s", 7, 2, 3},
    {"i32.div_s", 0xFFFFFFF9, 2, 0xFFFFFFFD},  // -7 / 2 = -3
    {"i32.div_s", 7, 0xFFFFFFFE, 0xFFFFFFFD},  // 7 / -2 = -3
    {"i32.div_s", 0x80000000, 2, 0xC0000000},
    {"i32.div_s", 0x80000000, 1, 0x80000000},
    {"i32.div_u", 0xFFFFFFFF, 2, 0x7FFFFFFF},
    {"i32.div_u", 7, 2, 3},
    {"i32.div_u", 0x80000000, 0xFFFFFFFF, 0},
    {"i32.rem_s", 7, 2, 1},
    {"i32.rem_s", 0xFFFFFFF9, 2, 0xFFFFFFFF},  // -7 % 2 = -1
    {"i32.rem_s", 7, 0xFFFFFFFE, 1},           // 7 % -2 = 1
    {"i32.rem_s", 0x80000000, 0xFFFFFFFF, 0},  // MIN % -1 = 0, no trap
    {"i32.rem_s", 5, 0xFFFFFFFF, 0},
    {"i32.rem_u", 0xFFFFFFFF, 2, 1},
    {"i32.rem_u", 0x80000000, 0xFFFFFFFF, 0x80000000},
    {"i32.wrap_i64", 0x1FFFFFFFFull, 0, 0xFFFFFFFF},
    {"i32.wrap_i64", 0x123456789ull, 0, 0x23456789},
    {"i32.wrap_i64", 0x8000000000000000ull, 0, 0},

    {"i64.add", 0xFFFFFFFFFFFFFFFFull, 1, 0},
    {"i64.add", 0x7FFFFFFFFFFFFFFFull, 1, 0x8000000000000000ull},
    {"i64.sub", 0, 1, 0xFFFFFFFFFFFFFFFFull},
    {"i64.mul", 0x100000000ull, 0x100000000ull, 0},
    {"i64.mul", 0xFFFFFFFFFFFFFFFFull, 0xFFFFFFFFFFFFFFFFull, 1},
    {"i64.and", 0xFF00FF00FF00FF00ull, 0xF0F0F0F0F0F0F0F0ull, 0xF000F000F000F000ull},
    {"i64.shl", 1, 63, 0x8000000000000000ull},
    {"i64.shl", 1, 64, 1},
    {"i64.shl", 1, 65, 2},
    {"i64.shl", 1, 32, 0x100000000ull},
    {"i64.shr_u", 0x8000000000000000ull, 63, 1},
    {"i64.shr_u", 0x8000000000000000ull, 64, 0x8000000000000000ull},
    {"i64.shr_s", 0x8000000000000000ull, 63, 0xFFFFFFFFFFFFFFFFull},
    {"i64.shr_s", 0x8000000000000000ull, 65, 0xC000000000000000ull},
    {"i64.rotl", 0x8000000000000001ull, 1, 3},
    {"i64.rotl", 0x123456789ABCDEF0ull, 64, 0x123456789ABCDEF0ull},
    {"i64.rotl", 0x123456789ABCDEF0ull, 4, 0x23456789ABCDEF01ull},
    {"i64.rotr", 0x123456789ABCDEF0ull, 4, 0x0123456789ABCDEFull},
    {"i64.rotr", 3, 1, 0x8000000000000001ull},
    {"i64.clz", 0, 0, 64},
    {"i64.clz", 1, 0, 63},
    {"i64.clz", 0x100000000ull, 0, 31},
    {"i64.clz", 0x8000000000000000ull, 0, 0},
    {"i64.ctz", 0, 0, 64},
    {"i64.ctz", 0x100000000ull, 0, 32},
    {"i64.ctz", 0x8000000000000000ull, 0, 63},
    {"i64.popcnt", 0, 0, 0},
    {"i64.popcnt", 0xFFFFFFFFFFFFFFFFull, 0, 64},
    {"i64.popcnt", 0x5555555555555555ull, 0, 32},
    {"i64.popcnt", 0x8000000000000001ull, 0, 2},
    {"i64.eqz", 0, 0, 1},
    {"i64.eqz", 0x100000000ull, 0, 0},
    {"i64.eq", 0x8000000000000000ull, 0x8000000000000000ull, 1},
    {"i64.lt_s", 0x8000000000000000ull, 0, 1},
    {"i64.lt_u", 0x8000000000000000ull, 0, 0},
    {"i64.gt_s", 0x7FFFFFFFFFFFFFFFull, 0x8000000000000000ull, 1},
    {"i64.gt_u", 0x7FFFFFFFFFFFFFFFull, 0x8000000000000000ull, 0},
    {"i64.le_s", 0x80000000u, 0xFFFFFFFF80000000ull, 0},  // 2^31 <= -2^31 is false
    {"i64.le_u", 0x80000000u, 0xFFFFFFFF80000000ull, 1},
    {"i64.ge_s", 0, 0xFFFFFFFFFFFFFFFFull, 1},
    {"i64.ge_u", 0, 0xFFFFFFFFFFFFFFFFull, 0},
    {"i64.extend8_s", 0x80, 0, 0xFFFFFFFFFFFFFF80ull},
    {"i64.extend8_s", 0x17F, 0, 0x7F},
    {"i64.extend16_s", 0x8000, 0, 0xFFFFFFFFFFFF8000ull},
    {"i64.extend32_s", 0x80000000u, 0, 0xFFFFFFFF80000000ull},
    {"i64.extend32_s", 0x17FFFFFFFull, 0, 0x7FFFFFFF},
    {"i64.div_s", 7, 2, 3},
    {"i64.div_s", 0xFFFFFFFFFFFFFFF9ull, 2, 0xFFFFFFFFFFFFFFFDull},
    {"i64.div_s", 0x8000000000000000ull, 2, 0xC000000000000000ull},
    {"i64.div_u", 0xFFFFFFFFFFFFFFFFull, 2, 0x7FFFFFFFFFFFFFFFull},
    {"i64.rem_s", 0xFFFFFFFFFFFFFFF9ull, 2, 0xFFFFFFFFFFFFFFFFull},
    {"i64.rem_s", 0x8000000000000000ull, 0xFFFFFFFFFFFFFFFFull, 0},
    {"i64.rem_u", 0xFFFFFFFFFFFFFFFFull, 10, 5},
    {"i64.extend_i32_s", 0x80000000u, 0, 0xFFFFFFFF80000000ull},
    {"i64.extend_i32_s", 0x7FFFFFFF, 0, 0x7FFFFFFF},
    {"i64.extend_i32_u", 0x80000000u, 0, 0x80000000u},
    {"i64.extend_i32_u", 0xFFFFFFFFu, 0, 0xFFFFFFFFu},
};

/* A compiled operation: its function, the code and the context it runs under. */
struct Built {
  WasmOp op;
  GRJIT_Code * code = nullptr;
  ~Built() {
    grjit_code_destroy(code);
    grjit_function_destroy(op.function);
  }
};

/* The optional CPU features this file's compiles switch off. The tests that run an operation with a
 * feature arm (clz, ctz, popcnt) run it twice, with what the host has and with every optional feature
 * masked off, so that the v1 sequences are run on a host that has the features (story 4). */
GRJIT_Features g_disabled = default_disabled_features();

struct ArmScope {
  GRJIT_Features saved;
  explicit ArmScope(GRJIT_Features d) : saved(g_disabled) { g_disabled = d; }
  ~ArmScope() { g_disabled = saved; }
};

const char * arm_name(GRJIT_Features disabled) {
  return disabled == GRJIT_FEATURES_NONE ? "the features the host has" : "every optional feature masked off";
}

GRJIT_Result compile(Built * b, const std::string & instr, JitWorld & w, Guards g = Guards{}) {
  b->op = build_wasm_op(instr, g);
  if (b->op.function == nullptr) {
    return GRJIT_ERR_INVALID;
  }
  GRJIT_CompileOptions o{};
  o.pages = w.pages();
  o.disabled_features = g_disabled;
  return grjit_compile(&o, b->op.function, &b->code);
}

/* What the code did, as the harness reads it: the value, or the trap its guard names. */
std::string said(const GRJIT_Code * code, const Outcome & o) {
  if (o.fault != 0) {
    return "HARDWARE FAULT (signal " + std::to_string(o.fault) + ")";
  }
  if (o.exit == GRJIT_EXIT_DEOPT) {
    const GRCORE_CodeSite * s =
        grcore_codemeta_find(grjit_code_meta(code), static_cast<uint32_t>(o.out[0]));
    return std::string("trap: ") + (s != nullptr ? trap_name(s->identity.offset) : "no such site");
  }
  if (o.exit == GRJIT_EXIT_RETURNED) {
    char buf[40];
    std::snprintf(buf, sizeof buf, "0x%llx", static_cast<unsigned long long>(o.out[0]));
    return buf;
  }
  return "exit " + std::to_string(o.exit);
}

std::string wanted(uint64_t v) {
  char buf[40];
  std::snprintf(buf, sizeof buf, "0x%llx", static_cast<unsigned long long>(v));
  return buf;
}

} // namespace

/* ---- Types and numbering ----------------------------------------------------------------- */

TEST(IntTypes, EveryValueThatExistedBeforeTheIntegerOperationsIsWhereItWas) {
  EXPECT_EQ(static_cast<int>(GRJIT_TYPE_I64), 0);
  EXPECT_EQ(static_cast<int>(GRJIT_TYPE_REF), 1);
  EXPECT_EQ(static_cast<int>(GRJIT_TYPE_PTR), 2);
  EXPECT_EQ(static_cast<int>(GRJIT_NATIVE_NO_RESULT), 3) << "the native-result sentinel did not move";
  EXPECT_EQ(static_cast<int>(GRJIT_TYPE_I32), 4);
  EXPECT_EQ(static_cast<int>(GRJIT_TYPE_F32), 5) << "the float types are appended (story 3)";
  EXPECT_EQ(static_cast<int>(GRJIT_TYPE_F64), 6);
  EXPECT_EQ(static_cast<int>(GRJIT_TYPE_COUNT), 7);
  const GRJIT_OpKind before[] = {GRJIT_OP_CONST, GRJIT_OP_MOVE, GRJIT_OP_ADD, GRJIT_OP_SUB,
      GRJIT_OP_MUL, GRJIT_OP_AND, GRJIT_OP_OR, GRJIT_OP_XOR, GRJIT_OP_SHL, GRJIT_OP_SHR,
      GRJIT_OP_SAR, GRJIT_OP_NEG, GRJIT_OP_NOT, GRJIT_OP_CMP, GRJIT_OP_LOAD, GRJIT_OP_LOAD_S,
      GRJIT_OP_STORE, GRJIT_OP_CALL, GRJIT_OP_POLL, GRJIT_OP_GUARD, GRJIT_OP_BR, GRJIT_OP_BR_IF,
      GRJIT_OP_RET, GRJIT_OP_BITCAST, GRJIT_OP_CALL_SLOT, GRJIT_OP_CALL_PTR,
      GRJIT_OP_TAIL_CALL_SLOT, GRJIT_OP_TAIL_CALL_PTR, GRJIT_OP_CALL_NATIVE};
  for (size_t i = 0; i < sizeof before / sizeof before[0]; i++) {
    EXPECT_EQ(static_cast<int>(before[i]), static_cast<int>(i)) << "operation " << i;
  }
  EXPECT_EQ(static_cast<int>(GRJIT_OP_CLZ), 29) << "the new operations are appended";
  EXPECT_EQ(static_cast<int>(GRJIT_OP_EXTEND_U), 43);
  EXPECT_EQ(static_cast<int>(GRJIT_OP_FADD), 44) << "the float operations follow the integer ones";
  EXPECT_EQ(static_cast<int>(GRJIT_CMP_EQ), 0);
  EXPECT_EQ(static_cast<int>(GRJIT_CMP_UGE), 9);
}

TEST(IntTypes, TheValueThreeIsTheNativeSentinelAndNoRegisterType) {
  B b("f");
  GRJIT_VReg v = GRJIT_NO_VREG;
  EXPECT_EQ(grjit_builder_vreg(b.b, static_cast<GRJIT_Type>(3), &v), GRJIT_ERR_INVALID);
  EXPECT_EQ(grjit_builder_vreg(b.b, GRJIT_TYPE_COUNT, &v), GRJIT_ERR_INVALID);
  EXPECT_EQ(grjit_builder_vreg(b.b, GRJIT_TYPE_I32, &v), GRJIT_OK);
  EXPECT_EQ(grjit_builder_param(b.b, GRJIT_TYPE_I32, &v), GRJIT_ERR_INVALID)
      << "a parameter after a register is refused, as before";
}

TEST(IntTypes, NativesTakeAndReturnNoI32) {
  NativeTab t;
  GRJIT_NativeDesc d{};
  d.address = 0x1000;
  GRJIT_Type i32[] = {GRJIT_TYPE_I32};
  d.params = i32;
  d.param_count = 1;
  d.result = GRJIT_NATIVE_NO_RESULT;
  EXPECT_EQ(grjit_native_table_add(t.t, &d, nullptr), GRJIT_ERR_INVALID);
  d.param_count = 0;
  d.result = GRJIT_TYPE_I32;
  EXPECT_EQ(grjit_native_table_add(t.t, &d, nullptr), GRJIT_ERR_INVALID);
}

TEST(IntPrint, TheNewTypeAndOperationsPrintByName) {
  Built b;
  b.op = build_wasm_op("i32.rotl");
  ASSERT_NE(b.op.function, nullptr);
  const std::string text = print(b.op.function);
  EXPECT_NE(text.find("v0:i32, v1:i32"), std::string::npos) << text;
  EXPECT_NE(text.find("v2 = rotl v0, v1"), std::string::npos) << text;
  Built d;
  d.op = build_wasm_op("i64.div_s");
  const std::string dt = print(d.op.function);
  EXPECT_NE(dt.find("div_s v0, v1"), std::string::npos) << dt;
  EXPECT_NE(dt.find("guard"), std::string::npos) << dt;
}

/* ---- The verifier: typing ---------------------------------------------------------------- */

namespace {

std::string refused(GRJIT_Function * f) {
  GRJIT_Result r;
  std::string why = verify_reason(f, &r);
  return r == GRJIT_OK ? "" : (why.empty() ? "(no reason)" : why);
}

} // namespace

TEST(IntVerify, TheWidthOfAnOperationIsItsDestinationsAndTheWidthsNeverMix) {
  struct Case {
    const char * what;
    std::function<void(B &)> build;
    bool ok;
  };
  const GRJIT_Type i32 = GRJIT_TYPE_I32, i64 = GRJIT_TYPE_I64;
  std::vector<Case> cases = {
      {"i32 + i32 -> i32", [&](B & b) {
         GRJIT_VReg x = b.param(i32), y = b.param(i32), r = b.reg(i32);
         b.at(b.block()); b.bin(GRJIT_OP_ADD, r, V(x), V(y)); b.ret(V(r)); }, true},
      {"i32 + i64 -> i32", [&](B & b) {
         GRJIT_VReg x = b.param(i32), y = b.param(i64), r = b.reg(i32);
         b.at(b.block()); b.bin(GRJIT_OP_ADD, r, V(x), V(y)); b.ret(V(r)); }, false},
      {"i32 + i32 -> i64", [&](B & b) {
         GRJIT_VReg x = b.param(i32), y = b.param(i32), r = b.reg(i64);
         b.at(b.block()); b.bin(GRJIT_OP_ADD, r, V(x), V(y)); b.ret(V(r)); }, false},
      {"i64 + i64 -> i64 (as before)", [&](B & b) {
         GRJIT_VReg x = b.param(i64), y = b.param(i64), r = b.reg(i64);
         b.at(b.block()); b.bin(GRJIT_OP_ADD, r, V(x), V(y)); b.ret(V(r)); }, true},
      {"i32 shifted by an i64 count", [&](B & b) {
         GRJIT_VReg x = b.param(i32), y = b.param(i64), r = b.reg(i32);
         b.at(b.block()); b.bin(GRJIT_OP_SHL, r, V(x), V(y)); b.ret(V(r)); }, false},
      {"i32 neg", [&](B & b) {
         GRJIT_VReg x = b.param(i32), r = b.reg(i32);
         b.at(b.block()); b.un(GRJIT_OP_NEG, r, V(x)); b.ret(V(r)); }, true},
      {"i32 clz of an i64", [&](B & b) {
         GRJIT_VReg x = b.param(i64), r = b.reg(i32);
         b.at(b.block()); b.un(GRJIT_OP_CLZ, r, V(x)); b.ret(V(r)); }, false},
      {"ext32_s makes an i64", [&](B & b) {
         GRJIT_VReg x = b.param(i64), r = b.reg(i64);
         b.at(b.block()); b.un(GRJIT_OP_EXT32_S, r, V(x)); b.ret(V(r)); }, true},
      {"ext32_s has no i32 form", [&](B & b) {
         GRJIT_VReg x = b.param(i32), r = b.reg(i32);
         b.at(b.block()); b.un(GRJIT_OP_EXT32_S, r, V(x)); b.ret(V(r)); }, false},
      {"wrap: i64 -> i32", [&](B & b) {
         GRJIT_VReg x = b.param(i64), r = b.reg(i32);
         b.at(b.block()); b.un(GRJIT_OP_WRAP, r, V(x)); b.ret(V(r)); }, true},
      {"wrap of an i32", [&](B & b) {
         GRJIT_VReg x = b.param(i32), r = b.reg(i32);
         b.at(b.block()); b.un(GRJIT_OP_WRAP, r, V(x)); b.ret(V(r)); }, false},
      {"wrap into an i64", [&](B & b) {
         GRJIT_VReg x = b.param(i64), r = b.reg(i64);
         b.at(b.block()); b.un(GRJIT_OP_WRAP, r, V(x)); b.ret(V(r)); }, false},
      {"extend_s: i32 -> i64", [&](B & b) {
         GRJIT_VReg x = b.param(i32), r = b.reg(i64);
         b.at(b.block()); b.un(GRJIT_OP_EXTEND_S, r, V(x)); b.ret(V(r)); }, true},
      {"extend_u of an i64", [&](B & b) {
         GRJIT_VReg x = b.param(i64), r = b.reg(i64);
         b.at(b.block()); b.un(GRJIT_OP_EXTEND_U, r, V(x)); b.ret(V(r)); }, false},
      {"extend_u into an i32", [&](B & b) {
         GRJIT_VReg x = b.param(i32), r = b.reg(i32);
         b.at(b.block()); b.un(GRJIT_OP_EXTEND_U, r, V(x)); b.ret(V(r)); }, false},
      {"move i32 -> i32", [&](B & b) {
         GRJIT_VReg x = b.param(i32), r = b.reg(i32);
         b.at(b.block()); b.mov(r, V(x)); b.ret(V(r)); }, true},
      {"move i32 -> i64", [&](B & b) {
         GRJIT_VReg x = b.param(i32), r = b.reg(i64);
         b.at(b.block()); b.mov(r, V(x)); b.ret(V(r)); }, false},
      {"move i64 -> i32", [&](B & b) {
         GRJIT_VReg x = b.param(i64), r = b.reg(i32);
         b.at(b.block()); b.mov(r, V(x)); b.ret(V(r)); }, false},
      {"bitcast into an i32", [&](B & b) {
         GRJIT_VReg x = b.param(i64), r = b.reg(i32);
         b.at(b.block()); b.bitcast(r, x); b.ret(V(r)); }, false},
      {"bitcast of an i32", [&](B & b) {
         GRJIT_VReg x = b.param(i32), r = b.reg(i64);
         b.at(b.block()); b.bitcast(r, x); b.ret(V(r)); }, false},
      {"i32 compare -> i32", [&](B & b) {
         GRJIT_VReg x = b.param(i32), y = b.param(i32), r = b.reg(i32);
         b.at(b.block()); b.cmp(GRJIT_CMP_LT, r, V(x), V(y)); b.ret(V(r)); }, true},
      {"i32 compare -> i64", [&](B & b) {
         GRJIT_VReg x = b.param(i32), y = b.param(i32), r = b.reg(i64);
         b.at(b.block()); b.cmp(GRJIT_CMP_LT, r, V(x), V(y)); b.ret(V(r)); }, true},
      {"i64 compare -> i32", [&](B & b) {
         GRJIT_VReg x = b.param(i64), y = b.param(i64), r = b.reg(i32);
         b.at(b.block()); b.cmp(GRJIT_CMP_LT, r, V(x), V(y)); b.ret(V(r)); }, true},
      {"i32 compared with an i64", [&](B & b) {
         GRJIT_VReg x = b.param(i32), y = b.param(i64), r = b.reg(i32);
         b.at(b.block()); b.cmp(GRJIT_CMP_EQ, r, V(x), V(y)); b.ret(V(r)); }, false},
      {"i32 compared with an immediate", [&](B & b) {
         GRJIT_VReg x = b.param(i32), r = b.reg(i32);
         b.at(b.block()); b.cmp(GRJIT_CMP_EQ, r, V(x), I(-1)); b.ret(V(r)); }, true},
      {"branch on an i32", [&](B & b) {
         GRJIT_VReg x = b.param(i32);
         GRJIT_BlockId e = b.block(), t = b.block(), f = b.block();
         b.at(e); b.br_if(V(x), t, f); b.at(t); b.ret(I(1)); b.at(f); b.ret(I(0)); }, true},
      {"guard on an i32", [&](B & b) {
         GRJIT_VReg x = b.param(i32);
         b.at(b.block()); b.guard(V(x), {1, 1, 0}); b.ret(V(x)); }, true},
      {"load into an i32, 32 bits", [&](B & b) {
         GRJIT_VReg p = b.param(GRJIT_TYPE_PTR), r = b.reg(i32);
         b.at(b.block()); b.load(r, p, 0, 32); b.ret(V(r)); }, true},
      {"load into an i32, 8 bits", [&](B & b) {
         GRJIT_VReg p = b.param(GRJIT_TYPE_PTR), r = b.reg(i32);
         b.at(b.block()); b.load(r, p, 0, 8); b.ret(V(r)); }, true},
      {"load into an i32, 64 bits", [&](B & b) {
         GRJIT_VReg p = b.param(GRJIT_TYPE_PTR), r = b.reg(i32);
         b.at(b.block()); b.load(r, p, 0, 64); b.ret(V(r)); }, false},
      {"sign-extending load into an i32", [&](B & b) {
         GRJIT_VReg p = b.param(GRJIT_TYPE_PTR), r = b.reg(i32);
         b.at(b.block()); b.load(r, p, 0, 16, true); b.ret(V(r)); }, false},
      {"store of an i32", [&](B & b) {
         GRJIT_VReg p = b.param(GRJIT_TYPE_PTR), x = b.param(i32);
         b.at(b.block()); b.store(p, 0, 32, V(x)); b.ret(V(x)); }, true},
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

TEST(IntVerify, AnImmediateOfAnI32MustFitThirtyTwoBitsInEitherSpelling) {
  for (int64_t imm : {int64_t{0}, int64_t{-1}, int64_t{INT32_MIN}, int64_t{INT32_MAX},
           int64_t{0x80000000}, int64_t{0xFFFFFFFF}}) {
    SCOPED_TRACE(imm);
    B b("t", 0);
    GRJIT_VReg r = b.reg(GRJIT_TYPE_I32);
    b.at(b.block());
    b.cnst(r, imm);
    b.ret(V(r));
    Fn f(b.finish());
    EXPECT_EQ(refused(f.f), "");
  }
  for (int64_t imm : {int64_t{0x100000000}, int64_t{-0x80000001ll}, INT64_MIN, INT64_MAX}) {
    SCOPED_TRACE(imm);
    B b("t", 0);
    GRJIT_VReg r = b.reg(GRJIT_TYPE_I32);
    GRJIT_VReg s = b.reg(GRJIT_TYPE_I32);
    b.at(b.block());
    b.cnst(r, imm);
    b.bin(GRJIT_OP_ADD, s, V(r), I(imm));
    b.ret(V(s));
    Fn f(b.finish());
    EXPECT_NE(refused(f.f).find("32"), std::string::npos) << "imm " << imm << ": [" << refused(f.f) << "]";
  }
  B b("t", 0);
  GRJIT_VReg x = b.param(GRJIT_TYPE_I32), r = b.reg(GRJIT_TYPE_I32);
  b.at(b.block());
  b.bin(GRJIT_OP_ADD, r, V(x), I(0x100000000));
  b.ret(V(r));
  Fn f(b.finish());
  EXPECT_NE(refused(f.f), "") << "an operand immediate wider than 32 bits";
}

TEST(IntVerify, AnI32IsNeverPassedToANativeOrNamedByARef) {
  NativeTab t;
  uint32_t id = t.add(reinterpret_cast<const void *>(0x1000), {GRJIT_TYPE_I64}, GRJIT_TYPE_I64);
  B b("t", 1);
  GRJIT_VReg x = b.param(GRJIT_TYPE_I32), r = b.reg(GRJIT_TYPE_I64);
  GRJIT_CallHooks h{};
  h.deopt = [](void *, uint64_t) -> uint32_t { return 0; };
  b.callable(h);
  b.natives(t);
  b.at(b.block());
  b.call_native(r, id, {V(x)}, {1, 1}, {grjit_frame_slot_dead()});
  b.ret(V(r));
  Fn f(b.finish());
  EXPECT_NE(refused(f.f), "") << "the native takes an i64, not an i32";
}

/* ---- The verifier: the guards of a partial operation ------------------------------------- */

namespace {

const GRJIT_OpKind kPartial[] = {GRJIT_OP_DIV_S, GRJIT_OP_DIV_U, GRJIT_OP_REM_S, GRJIT_OP_REM_U};

const char * name_of(GRJIT_OpKind k) {
  return k == GRJIT_OP_DIV_S ? "div_s" : k == GRJIT_OP_DIV_U ? "div_u"
       : k == GRJIT_OP_REM_S ? "rem_s" : "rem_u";
}

/* The guards of `div_s`, as a frontend writes them, and the building blocks of the shapes the
 * verifier accepts or refuses. `T` is the width's type. */
struct Parts {
  B & b;
  GRJIT_Type T;
  int64_t min;
  GRJIT_VReg x, y;
  Parts(B & bb, GRJIT_Type t) : b(bb), T(t), min(t == GRJIT_TYPE_I32 ? INT32_MIN : INT64_MIN) {
    x = b.param(T);
    y = b.param(T);
  }
  GRJIT_VReg guard_nonzero() {
    GRJIT_VReg c = b.reg(T);
    b.cmp(GRJIT_CMP_NE, c, V(y), I(0));
    b.guard(V(c), {9, 1, 0});
    return c;
  }
  void guard_overflow_and(GRJIT_VReg dividend, GRJIT_VReg divisor) {
    GRJIT_VReg e1 = b.reg(T), e2 = b.reg(T), t = b.reg(T), c = b.reg(T);
    b.cmp(GRJIT_CMP_EQ, e1, V(dividend), I(min));
    b.cmp(GRJIT_CMP_EQ, e2, V(divisor), I(-1));
    b.bin(GRJIT_OP_AND, t, V(e1), V(e2));
    b.cmp(GRJIT_CMP_EQ, c, V(t), I(0));
    b.guard(V(c), {9, 2, 0});
  }
  void guard_overflow_or(GRJIT_VReg dividend, GRJIT_VReg divisor) {
    GRJIT_VReg n1 = b.reg(T), n2 = b.reg(T), c = b.reg(T);
    b.cmp(GRJIT_CMP_NE, n1, V(dividend), I(min));
    b.cmp(GRJIT_CMP_NE, n2, V(divisor), I(-1));
    b.bin(GRJIT_OP_OR, c, V(n1), V(n2));
    b.guard(V(c), {9, 2, 0});
  }
};

} // namespace

TEST(IntVerify, ADivisionOrRemainderWithNoGuardIsRefusedForEachOperationAtEachWidth) {
  for (GRJIT_Type T : {GRJIT_TYPE_I32, GRJIT_TYPE_I64}) {
    for (GRJIT_OpKind k : kPartial) {
      SCOPED_TRACE(std::string(name_of(k)) + (T == GRJIT_TYPE_I32 ? " i32" : " i64"));
      B b("t", 0);
      Parts p(b, T);
      GRJIT_VReg r = b.reg(T);
      b.at(b.block());
      b.bin(k, r, V(p.x), V(p.y));
      b.ret(V(r));
      Fn f(b.finish());
      const std::string why = refused(f.f);
      EXPECT_NE(why.find("guard"), std::string::npos) << "reason: " << why;
      EXPECT_NE(why.find(name_of(k)), std::string::npos) << "reason: " << why;
    }
  }
}

TEST(IntVerify, TheDivisorGuardAloneSatisfiesAllButDivSAndDivSNeedsTheOverflowGuardToo) {
  for (GRJIT_Type T : {GRJIT_TYPE_I32, GRJIT_TYPE_I64}) {
    for (GRJIT_OpKind k : kPartial) {
      SCOPED_TRACE(std::string(name_of(k)) + (T == GRJIT_TYPE_I32 ? " i32" : " i64"));
      B b("t", 0);
      Parts p(b, T);
      GRJIT_VReg r = b.reg(T);
      b.at(b.block());
      p.guard_nonzero();
      b.bin(k, r, V(p.x), V(p.y));
      b.ret(V(r));
      Fn f(b.finish());
      const std::string why = refused(f.f);
      if (k == GRJIT_OP_DIV_S) {
        EXPECT_NE(why.find("MIN"), std::string::npos) << "div_s without the overflow guard: " << why;
      } else {
        EXPECT_EQ(why, "") << "rem_s needs no overflow guard (MIN % -1 is 0)";
      }
    }
  }
}

TEST(IntVerify, ADivSWithBothGuardsInEitherShapeIsAccepted) {
  for (GRJIT_Type T : {GRJIT_TYPE_I32, GRJIT_TYPE_I64}) {
    for (int shape = 0; shape < 2; shape++) {
      for (int order = 0; order < 2; order++) {
        SCOPED_TRACE(shape * 10 + order);
        B b("t", 0);
        Parts p(b, T);
        GRJIT_VReg r = b.reg(T);
        b.at(b.block());
        if (order == 0) {
          p.guard_nonzero();
        }
        shape == 0 ? p.guard_overflow_and(p.x, p.y) : p.guard_overflow_or(p.x, p.y);
        if (order == 1) {
          p.guard_nonzero();
        }
        b.bin(GRJIT_OP_DIV_S, r, V(p.x), V(p.y));
        b.ret(V(r));
        Fn f(b.finish());
        EXPECT_EQ(refused(f.f), "");
      }
    }
  }
}

TEST(IntVerify, TheOtherSpellingsOfANonZeroTestAreAccepted) {
  for (int spelling = 0; spelling < 5; spelling++) {
    SCOPED_TRACE(spelling);
    B b("t", 0);
    Parts p(b, GRJIT_TYPE_I32);
    GRJIT_VReg r = b.reg(GRJIT_TYPE_I32), c = b.reg(GRJIT_TYPE_I32);
    b.at(b.block());
    switch (spelling) {
      case 0: b.guard(V(p.y), {9, 1, 0}); break;                                // the divisor itself
      case 1: b.cmp(GRJIT_CMP_UGT, c, V(p.y), I(0)); b.guard(V(c), {9, 1, 0}); break;
      case 2: b.cmp(GRJIT_CMP_ULT, c, I(0), V(p.y)); b.guard(V(c), {9, 1, 0}); break;
      case 3: b.cmp(GRJIT_CMP_NE, c, I(0), V(p.y)); b.guard(V(c), {9, 1, 0}); break;
      default: b.cmp(GRJIT_CMP_NE, c, V(p.y), I(0)); b.guard(V(c), {9, 1, 0}); break;
    }
    b.bin(GRJIT_OP_DIV_U, r, V(p.x), V(p.y));
    b.ret(V(r));
    Fn f(b.finish());
    EXPECT_EQ(refused(f.f), "");
  }
}

TEST(IntVerify, AGuardThatDoesNotSayTheDivisorIsNotZeroIsRefused) {
  struct Case {
    const char * what;
    std::function<void(B &, Parts &)> before; // the guards, then the division is built
    bool ok;
  };
  std::vector<Case> cases = {
      {"a guard of the dividend", [](B & b, Parts & p) { b.guard(V(p.x), {9, 1, 0}); }, false},
      {"a compare with 1, not 0", [](B & b, Parts & p) {
         GRJIT_VReg c = b.reg(p.T); b.cmp(GRJIT_CMP_NE, c, V(p.y), I(1)); b.guard(V(c), {9, 1, 0}); }, false},
      {"EQ 0 (fires when the divisor is not zero)", [](B & b, Parts & p) {
         GRJIT_VReg c = b.reg(p.T); b.cmp(GRJIT_CMP_EQ, c, V(p.y), I(0)); b.guard(V(c), {9, 1, 0}); }, false},
      {"a compare of the dividend with zero", [](B & b, Parts & p) {
         GRJIT_VReg c = b.reg(p.T); b.cmp(GRJIT_CMP_NE, c, V(p.x), I(0)); b.guard(V(c), {9, 1, 0}); }, false},
      {"a constant guard", [](B & b, Parts &) { b.guard(I(1), {9, 1, 0}); }, false},
      {"the compare is of a register assigned after it", [](B & b, Parts & p) {
         GRJIT_VReg c = b.reg(p.T);
         b.cmp(GRJIT_CMP_NE, c, V(p.y), I(0));
         b.bin(GRJIT_OP_ADD, p.y, V(p.y), I(1));   // y is a different value now
         b.guard(V(c), {9, 1, 0}); }, false},
      {"the divisor assigned after the guard", [](B & b, Parts & p) {
         GRJIT_VReg c = b.reg(p.T);
         b.cmp(GRJIT_CMP_NE, c, V(p.y), I(0));
         b.guard(V(c), {9, 1, 0});
         b.bin(GRJIT_OP_ADD, p.y, V(p.y), I(0)); }, false},
      {"the flag assigned between the compare and the guard", [](B & b, Parts & p) {
         GRJIT_VReg c = b.reg(p.T);
         b.cmp(GRJIT_CMP_NE, c, V(p.y), I(0));
         b.cnst(c, 1);
         b.guard(V(c), {9, 1, 0}); }, false},
      {"the guard in an earlier block", [](B & b, Parts & p) {
         GRJIT_VReg c = b.reg(p.T);
         GRJIT_BlockId next = b.block();
         b.cmp(GRJIT_CMP_NE, c, V(p.y), I(0));
         b.guard(V(c), {9, 1, 0});
         b.br(next);
         b.at(next); }, false},
      {"the guard in the same block", [](B &, Parts & p) { p.guard_nonzero(); }, true},
      {"a guard that is itself guarded by nothing else", [](B & b, Parts & p) {
         GRJIT_VReg c = b.reg(p.T);
         b.cnst(p.x, 5);                                    // an unrelated assignment
         b.cmp(GRJIT_CMP_NE, c, V(p.y), I(0));
         b.guard(V(c), {9, 1, 0}); }, true},
  };
  for (const Case & c : cases) {
    SCOPED_TRACE(c.what);
    B b("t", 0);
    Parts p(b, GRJIT_TYPE_I32);
    GRJIT_VReg r = b.reg(GRJIT_TYPE_I32);
    b.at(b.block());
    c.before(b, p);
    b.bin(GRJIT_OP_DIV_U, r, V(p.x), V(p.y));
    b.ret(V(r));
    Fn f(b.finish());
    EXPECT_EQ(refused(f.f).empty(), c.ok) << refused(f.f);
  }
}

TEST(IntVerify, TheOverflowGuardMustBeAboutTheOperandsOfTheDivisionAndStillTrue) {
  struct Case {
    const char * what;
    std::function<void(B &, Parts &)> guards;
    bool ok;
  };
  std::vector<Case> cases = {
      {"about other registers", [](B & b, Parts & p) {
         GRJIT_VReg a = b.reg(p.T), c = b.reg(p.T);
         b.cnst(a, 7); b.cnst(c, 3);
         p.guard_nonzero();
         p.guard_overflow_and(a, c); }, false},
      {"about swapped operands", [](B &, Parts & p) {
         p.guard_nonzero();
         p.guard_overflow_and(p.y, p.x); }, false},
      {"the dividend assigned after the guard", [](B & b, Parts & p) {
         p.guard_nonzero();
         p.guard_overflow_and(p.x, p.y);
         b.bin(GRJIT_OP_ADD, p.x, V(p.x), I(1)); }, false},
      {"the divisor assigned after the guard", [](B & b, Parts & p) {
         p.guard_overflow_and(p.x, p.y);
         p.guard_nonzero();
         b.bin(GRJIT_OP_ADD, p.y, V(p.y), I(0)); }, false},
      {"the wrong constant for the dividend", [](B & b, Parts & p) {
         GRJIT_VReg e1 = b.reg(p.T), e2 = b.reg(p.T), t = b.reg(p.T), c = b.reg(p.T);
         p.guard_nonzero();
         b.cmp(GRJIT_CMP_EQ, e1, V(p.x), I(0));
         b.cmp(GRJIT_CMP_EQ, e2, V(p.y), I(-1));
         b.bin(GRJIT_OP_AND, t, V(e1), V(e2));
         b.cmp(GRJIT_CMP_EQ, c, V(t), I(0));
         b.guard(V(c), {9, 2, 0}); }, false},
      {"the wrong constant for the divisor", [](B & b, Parts & p) {
         GRJIT_VReg e1 = b.reg(p.T), e2 = b.reg(p.T), t = b.reg(p.T), c = b.reg(p.T);
         p.guard_nonzero();
         b.cmp(GRJIT_CMP_EQ, e1, V(p.x), I(p.min));
         b.cmp(GRJIT_CMP_EQ, e2, V(p.y), I(1));
         b.bin(GRJIT_OP_AND, t, V(e1), V(e2));
         b.cmp(GRJIT_CMP_EQ, c, V(t), I(0));
         b.guard(V(c), {9, 2, 0}); }, false},
      {"OR where AND is needed", [](B & b, Parts & p) {
         GRJIT_VReg e1 = b.reg(p.T), e2 = b.reg(p.T), t = b.reg(p.T), c = b.reg(p.T);
         p.guard_nonzero();
         b.cmp(GRJIT_CMP_EQ, e1, V(p.x), I(p.min));
         b.cmp(GRJIT_CMP_EQ, e2, V(p.y), I(-1));
         b.bin(GRJIT_OP_OR, t, V(e1), V(e2));
         b.cmp(GRJIT_CMP_EQ, c, V(t), I(0));
         b.guard(V(c), {9, 2, 0}); }, false},
      {"the flag of the first comparison assigned before the join", [](B & b, Parts & p) {
         GRJIT_VReg e1 = b.reg(p.T), e2 = b.reg(p.T), t = b.reg(p.T), c = b.reg(p.T);
         p.guard_nonzero();
         b.cmp(GRJIT_CMP_EQ, e1, V(p.x), I(p.min));
         b.cmp(GRJIT_CMP_EQ, e2, V(p.y), I(-1));
         b.cnst(e1, 0);
         b.bin(GRJIT_OP_AND, t, V(e1), V(e2));
         b.cmp(GRJIT_CMP_EQ, c, V(t), I(0));
         b.guard(V(c), {9, 2, 0}); }, false},
      {"a guard of the join itself (fires when MIN and -1: the wrong way round)", [](B & b, Parts & p) {
         GRJIT_VReg e1 = b.reg(p.T), e2 = b.reg(p.T), t = b.reg(p.T);
         p.guard_nonzero();
         b.cmp(GRJIT_CMP_EQ, e1, V(p.x), I(p.min));
         b.cmp(GRJIT_CMP_EQ, e2, V(p.y), I(-1));
         b.bin(GRJIT_OP_AND, t, V(e1), V(e2));
         b.guard(V(t), {9, 2, 0}); }, false},
      {"the right guards", [](B &, Parts & p) {
         p.guard_nonzero();
         p.guard_overflow_and(p.x, p.y); }, true},
  };
  for (GRJIT_Type T : {GRJIT_TYPE_I32, GRJIT_TYPE_I64}) {
    for (const Case & c : cases) {
      SCOPED_TRACE(std::string(c.what) + (T == GRJIT_TYPE_I32 ? " (i32)" : " (i64)"));
      B b("t", 0);
      Parts p(b, T);
      GRJIT_VReg r = b.reg(T);
      b.at(b.block());
      c.guards(b, p);
      b.bin(GRJIT_OP_DIV_S, r, V(p.x), V(p.y));
      b.ret(V(r));
      Fn f(b.finish());
      EXPECT_EQ(refused(f.f).empty(), c.ok) << refused(f.f);
    }
  }
}

TEST(IntVerify, AConstantDivisorIsJudgedAsItStands) {
  for (GRJIT_Type T : {GRJIT_TYPE_I32, GRJIT_TYPE_I64}) {
    for (GRJIT_OpKind k : kPartial) {
      SCOPED_TRACE(std::string(name_of(k)) + (T == GRJIT_TYPE_I32 ? " i32" : " i64"));
      auto build = [&](int64_t divisor, bool not_min_guard) {
        B b("t", 0);
        Parts p(b, T);
        GRJIT_VReg r = b.reg(T);
        b.at(b.block());
        if (not_min_guard) {
          GRJIT_VReg c = b.reg(T);
          b.cmp(GRJIT_CMP_NE, c, V(p.x), I(p.min));
          b.guard(V(c), {9, 2, 0});
        }
        b.bin(k, r, V(p.x), I(divisor));
        b.ret(V(r));
        Fn f(b.finish());
        return refused(f.f);
      };
      EXPECT_NE(build(0, true), "") << "a constant zero divisor";
      EXPECT_EQ(build(3, false), "") << "a constant divisor that is neither 0 nor -1";
      EXPECT_EQ(build(1, false), "");
      if (k == GRJIT_OP_DIV_S) {
        EXPECT_NE(build(-1, false), "") << "-1 can overflow";
        EXPECT_EQ(build(-1, true), "") << "unless the dividend is guarded not to be MIN";
      } else {
        EXPECT_EQ(build(-1, false), "");
      }
    }
  }
  // 0xFFFFFFFF is -1 for an i32.
  B b("t", 0);
  Parts p(b, GRJIT_TYPE_I32);
  GRJIT_VReg r = b.reg(GRJIT_TYPE_I32);
  b.at(b.block());
  b.bin(GRJIT_OP_DIV_S, r, V(p.x), I(0xFFFFFFFFll));
  b.ret(V(r));
  Fn f(b.finish());
  EXPECT_NE(refused(f.f), "");
}

TEST(IntVerify, TheGuardsOfOneDivisionServeTheNextOnlyWhileTheirOperandsStand) {
  B b("t", 0);
  Parts p(b, GRJIT_TYPE_I32);
  GRJIT_VReg r1 = b.reg(GRJIT_TYPE_I32), r2 = b.reg(GRJIT_TYPE_I32), r3 = b.reg(GRJIT_TYPE_I32);
  b.at(b.block());
  p.guard_nonzero();
  b.bin(GRJIT_OP_DIV_U, r1, V(p.x), V(p.y));
  b.bin(GRJIT_OP_REM_U, r2, V(r1), V(p.y));   // the same divisor, still guarded
  b.bin(GRJIT_OP_ADD, p.y, V(p.y), I(1));     // a new value
  b.bin(GRJIT_OP_REM_U, r3, V(r2), V(p.y));   // not guarded
  b.ret(V(r3));
  Fn f(b.finish());
  const std::string why = refused(f.f);
  EXPECT_NE(why.find("op 5"), std::string::npos) << why;
}

TEST(IntVerify, AnAllocationThatFailsInTheGuardAnalysisIsOutOfMemoryAndLeaksNothing) {
  TrackingAllocator t;
  B b("t", 0, nullptr, t.get());
  Parts p(b, GRJIT_TYPE_I64);
  GRJIT_VReg r = b.reg(GRJIT_TYPE_I64);
  b.at(b.block());
  p.guard_nonzero();
  p.guard_overflow_and(p.x, p.y);
  b.bin(GRJIT_OP_DIV_S, r, V(p.x), V(p.y));
  b.ret(V(r));
  Fn f(b.finish());
  const long before = t.live;
  GRJIT_Result first = grjit_function_verify(f, nullptr, nullptr, 0);
  ASSERT_EQ(first, GRJIT_OK);
  bool saw_oom = false;
  for (long k = 1; k <= 12; k++) {
    t.calls = 0;
    t.fail_at = k;
    t.sticky = false;
    GRJIT_Result r2 = grjit_function_verify(f, nullptr, nullptr, 0);
    t.fail_at = 0;
    EXPECT_EQ(t.live, before) << "allocation " << k << " failed and something leaked";
    EXPECT_TRUE(r2 == GRJIT_OK || r2 == GRJIT_ERR_OOM) << k;
    saw_oom |= r2 == GRJIT_ERR_OOM;
  }
  EXPECT_TRUE(saw_oom) << "the analysis allocates, and a refusal of one is an out-of-memory answer";
}

/* ---- Running the code: the answers ------------------------------------------------------- */

TEST(IntRun, EveryOperationGivesTheWorkedAnswerAndAnI32ResultIsZeroExtended) {
  INT_RUN_HERE();
  JitWorld w;
  // Twice: the arm of clz, ctz and popcnt the host's features give, and the v1 sequences of the same
  // operations (a planted defect in a sequence only the fallback runs is caught by the second).
  for (GRJIT_Features disabled : two_arms()) {
    SCOPED_TRACE(arm_name(disabled));
    ArmScope arm(disabled);
    std::map<std::string, std::unique_ptr<Built>> built;
    for (const Worked & k : kWorked) {
      SCOPED_TRACE(std::string(k.instr) + " " + wanted(k.a) + " " + wanted(k.b));
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
      Outcome o = run(slot->code, w.ctx, {k.a, k.b});
      ASSERT_EQ(o.fault, 0) << "a hardware fault";
      ASSERT_EQ(o.exit, uint32_t{GRJIT_EXIT_RETURNED}) << said(slot->code, o);
      EXPECT_EQ(o.out[0], k.want) << "got " << wanted(o.out[0]) << ", want " << wanted(k.want);
    }
  }
}

TEST(IntRun, EveryOperationAtEveryWidthAgreesWithTheEvaluatorOnTheBoundaryValues) {
  INT_RUN_HERE();
  JitWorld w;
  size_t compared = 0, traps = 0;
  for (GRJIT_Features disabled : two_arms()) {
  SCOPED_TRACE(arm_name(disabled));
  ArmScope arm(disabled);
  for (const char * ty : {"i32", "i64"}) {
    const bool narrow = std::string(ty) == "i32";
    const std::vector<uint64_t> vals = boundary(narrow);
    for (const std::string & instr : all_int_ops(ty)) {
      SCOPED_TRACE(instr);
      Built b;
      ASSERT_EQ(compile(&b, instr, w), GRJIT_OK) << instr;
      const bool takes_i32 = instr == "i64.extend_i32_s" || instr == "i64.extend_i32_u";
      const bool takes_i64 = instr == "i32.wrap_i64";
      const std::vector<uint64_t> xs = takes_i64 ? boundary(false) : vals;
      for (uint64_t x : xs) {
        for (uint64_t y : b.op.params == 2 ? vals : std::vector<uint64_t>{0}) {
          const uint64_t xa = ((narrow && !takes_i64) || takes_i32) ? (x & M32) : x;
          const uint64_t ya = narrow ? (y & M32) : y;
          Outcome o = run(b.code, w.ctx, {xa, ya});
          ireval::Result e = ireval::eval(b.op.function, w.ctx, nullptr, {xa, ya});
          const std::string got = said(b.code, o);
          ASSERT_EQ(o.fault, 0) << instr << " " << wanted(xa) << " " << wanted(ya) << ": " << got;
          ASSERT_EQ(o.exit, e.exit) << instr << " " << wanted(xa) << " " << wanted(ya) << ": " << got;
          if (o.exit == GRJIT_EXIT_RETURNED) {
            ASSERT_EQ(o.out[0], e.out[0]) << instr << " " << wanted(xa) << " " << wanted(ya);
            if (b.op.result == GRJIT_TYPE_I32) {
              ASSERT_EQ(o.out[0] >> 32, 0u) << "an i32 result is zero-extended";
            }
          } else {
            const GRCORE_CodeSite * s =
                grcore_codemeta_find(grjit_code_meta(b.code), static_cast<uint32_t>(o.out[0]));
            ASSERT_NE(s, nullptr);
            ASSERT_EQ(s->identity.offset, e.guard_identity.offset)
                << instr << " " << wanted(xa) << " " << wanted(ya);
            traps++;
          }
          compared++;
        }
      }
    }
  }
  }
  std::printf("integer boundary sweep: %zu runs compared with the evaluator (twice: with the host's features and "
              "with every optional feature masked off), %zu of them guard exits\n",
      compared, traps);
  // Floors at about nine tenths of the fixed sweep (83,872 runs, 322 guard exits), twice.
  EXPECT_GT(compared, 2 * 75000u);
  EXPECT_GT(traps, 2 * 290u) << "the traps of the four divisions were reached";
}

TEST(IntRun, DivideByZeroAndSignedOverflowFireTheirGuardsAndMinRemMinusOneIsZero) {
  INT_RUN_HERE();
  JitWorld w;
  for (const char * ty : {"i32", "i64"}) {
    const bool narrow = std::string(ty) == "i32";
    const uint64_t min = narrow ? 0x80000000u : 0x8000000000000000ull;
    const uint64_t neg1 = narrow ? 0xFFFFFFFFu : 0xFFFFFFFFFFFFFFFFull;
    for (const char * op : {"div_s", "div_u", "rem_s", "rem_u"}) {
      const std::string instr = std::string(ty) + "." + op;
      SCOPED_TRACE(instr);
      Built b;
      ASSERT_EQ(compile(&b, instr, w), GRJIT_OK);
      Outcome zero = run(b.code, w.ctx, {7, 0});
      EXPECT_EQ(said(b.code, zero), "trap: integer divide by zero");
      Outcome zero_min = run(b.code, w.ctx, {min, 0});
      EXPECT_EQ(said(b.code, zero_min), "trap: integer divide by zero");
      Outcome min_by_neg1 = run(b.code, w.ctx, {min, neg1});
      if (std::string(op) == "div_s") {
        EXPECT_EQ(said(b.code, min_by_neg1), "trap: integer overflow") << "MIN / -1";
      } else if (std::string(op) == "rem_s") {
        EXPECT_EQ(said(b.code, min_by_neg1), "0x0") << "MIN % -1 is 0, with no trap and no fault";
      } else if (std::string(op) == "div_u") {
        EXPECT_EQ(said(b.code, min_by_neg1), "0x0");
      } else {
        EXPECT_EQ(said(b.code, min_by_neg1), wanted(min));
      }
      Outcome plain = run(b.code, w.ctx, {neg1, 1});
      EXPECT_EQ(plain.exit, uint32_t{GRJIT_EXIT_RETURNED}) << "-1 / 1 and its kin trap on nothing";
    }
  }
}

TEST(IntRun, AnI32EntersAndLeavesAsItsLow32BitsWhateverTheCallerLeftAbove) {
  INT_RUN_HERE();
  JitWorld w;
  {
    // f(x: i32) = x: the entry masks the argument; the return is zero-extended.
    B b("ident", 0);
    GRJIT_VReg x = b.param(GRJIT_TYPE_I32);
    b.at(b.block());
    b.ret(V(x));
    Fn f(b.finish());
    Compiled c(f, w.pages());
    ASSERT_TRUE(c);
    for (uint64_t v : {uint64_t{5}, uint64_t{0xFFFFFFFF}, uint64_t{0xDEADBEEF00000005ull},
             uint64_t{0xFFFFFFFFFFFFFFFFull}, uint64_t{0x100000000ull}}) {
      Outcome o = run(c.code, w.ctx, {v});
      EXPECT_EQ(o.out[0], v & M32) << wanted(v);
    }
  }
  {
    // g(x: i32, y: i32) = x + y: the carry out of bit 31 does not reach the upper half.
    Built b;
    ASSERT_EQ(compile(&b, "i32.add", w), GRJIT_OK);
    Outcome o = run(b.code, w.ctx, {0xFFFFFFFFull | 0xAB00000000ull, 1});
    EXPECT_EQ(o.out[0], 0u);
    o = run(b.code, w.ctx, {0xFFFFFFFFull, 0xFFFFFFFFull});
    EXPECT_EQ(o.out[0], 0xFFFFFFFEull);
  }
  {
    // A constant and a move: the upper half of a negative immediate is not carried in.
    B b("k", 0);
    GRJIT_VReg a = b.reg(GRJIT_TYPE_I32), c = b.reg(GRJIT_TYPE_I32), s = b.reg(GRJIT_TYPE_I32);
    b.at(b.block());
    b.cnst(a, -1);
    b.mov(c, V(a));
    b.bin(GRJIT_OP_ADD, s, V(c), I(-2));
    b.ret(V(c));
    Fn f(b.finish());
    Compiled code(f, w.pages());
    ASSERT_TRUE(code);
    EXPECT_EQ(run(code.code, w.ctx, {}).out[0], 0xFFFFFFFFull);
  }
  {
    // An immediate operand of an i32 operation, negative: h(x) = x + (-2) = x - 2.
    B b("imm", 0);
    GRJIT_VReg x = b.param(GRJIT_TYPE_I32), s = b.reg(GRJIT_TYPE_I32);
    b.at(b.block());
    b.bin(GRJIT_OP_ADD, s, V(x), I(-2));
    b.ret(V(s));
    Fn f(b.finish());
    Compiled code(f, w.pages());
    ASSERT_TRUE(code);
    EXPECT_EQ(run(code.code, w.ctx, {1}).out[0], 0xFFFFFFFFull);
    EXPECT_EQ(run(code.code, w.ctx, {2}).out[0], 0u);
  }
}

TEST(IntRun, AnI32LoadedFromMemoryAndStoredToItIsItsLow32Bits) {
  INT_RUN_HERE();
  JitWorld w;
  B b("mem", 0);
  GRJIT_VReg p = b.param(GRJIT_TYPE_PTR);
  GRJIT_VReg a = b.reg(GRJIT_TYPE_I32), c = b.reg(GRJIT_TYPE_I32), d = b.reg(GRJIT_TYPE_I32),
             s = b.reg(GRJIT_TYPE_I32);
  b.at(b.block());
  b.load(a, p, 0, 32);
  b.load(c, p, 1, 8);
  b.load(d, p, 0, 16);
  b.bin(GRJIT_OP_ADD, s, V(a), V(c));
  b.store(p, 8, 32, V(s));
  b.store(p, 16, 16, V(d));
  b.ret(V(a));
  Fn f(b.finish());
  Compiled code(f, w.pages());
  ASSERT_TRUE(code);
  uint8_t mem[32];
  std::memset(mem, 0xEE, sizeof mem);
  const uint32_t word = 0x8091A2B3u;
  std::memcpy(mem, &word, 4);
  Outcome o = run(code.code, w.ctx, {reinterpret_cast<uintptr_t>(mem)});
  EXPECT_EQ(o.out[0], uint64_t{0x8091A2B3u});
  uint32_t stored;
  std::memcpy(&stored, mem + 8, 4);
  EXPECT_EQ(stored, uint32_t{0x8091A2B3u} + 0xA2u);
  uint16_t half;
  std::memcpy(&half, mem + 16, 2);
  EXPECT_EQ(half, uint16_t{0xA2B3});
}

namespace {
uint64_t helper_with_dirty_upper_half(uint64_t x) {
  return 0xABCD000000000000ull | x; // a C helper returning uint32_t leaves rax like this
}
} // namespace

TEST(IntRun, AHelperResultReceivedAsAnI32IsItsLow32Bits) {
  INT_RUN_HERE();
  JitWorld w;
  B b("helper", 0);
  GRJIT_VReg x = b.param(GRJIT_TYPE_I32), r = b.reg(GRJIT_TYPE_I32);
  b.at(b.block());
  b.call(r, reinterpret_cast<const void *>(&helper_with_dirty_upper_half), {V(x)});
  b.ret(V(r));
  Fn f(b.finish());
  Compiled code(f, w.pages());
  ASSERT_TRUE(code);
  EXPECT_EQ(run(code.code, w.ctx, {0x80000001u}).out[0], uint64_t{0x80000001u});
  ireval::Result e = ireval::eval(f, w.ctx, nullptr, {0x80000001u});
  EXPECT_EQ(e.out[0], uint64_t{0x80000001u});
}

TEST(IntRun, AnI32InAFrameStateIsRecordedWithTheRepresentationI32AndAnI64WithBits) {
  INT_RUN_HERE();
  JitWorld w;
  B b("state", 3);
  GRJIT_VReg x = b.param(GRJIT_TYPE_I32), y = b.param(GRJIT_TYPE_I64), z = b.reg(GRJIT_TYPE_I32);
  b.at(b.block());
  b.bin(GRJIT_OP_ADD, z, V(x), I(1));
  b.guard(V(x), {5, 7, 0},
      {grjit_frame_slot_vreg(x), grjit_frame_slot_vreg(y), grjit_frame_slot_vreg(z)});
  b.ret(V(z));
  Fn f(b.finish());
  Compiled code(f, w.pages());
  ASSERT_TRUE(code);
  Outcome o = run(code.code, w.ctx, {0, 0x1111111122222222ull});
  ASSERT_EQ(o.exit, uint32_t{GRJIT_EXIT_DEOPT});
  EXPECT_EQ(o.out[0], 0u);
  EXPECT_EQ(o.out[1], 0x1111111122222222ull);
  EXPECT_EQ(o.out[2], 1u);
  const GRCORE_CodeSite * s =
      grcore_codemeta_find(grjit_code_meta(code.code), static_cast<uint32_t>(o.out[3]));
  ASSERT_NE(s, nullptr);
  ASSERT_EQ(s->frame_state_count, 3u);
  EXPECT_EQ(s->frame_state[0].representation, GRCORE_REPR_I32);
  EXPECT_EQ(s->frame_state[1].representation, GRCORE_REPR_BITS);
  EXPECT_EQ(s->frame_state[2].representation, GRCORE_REPR_I32);
  for (size_t i = 0; i < 3; i++) {
    EXPECT_EQ(s->frame_state[i].slot_kind, GRCORE_SLOT_RAW);
  }
}

TEST(IntRun, ABranchOnAnI32AndAGuardOnOneReadTheWholeWord) {
  INT_RUN_HERE();
  JitWorld w;
  B b("branch", 0);
  GRJIT_VReg x = b.param(GRJIT_TYPE_I32);
  GRJIT_BlockId e = b.block(), t = b.block(), f2 = b.block();
  b.at(e);
  b.br_if(V(x), t, f2);
  b.at(t);
  b.ret(I(11));
  b.at(f2);
  b.ret(I(22));
  Fn f(b.finish());
  Compiled code(f, w.pages());
  ASSERT_TRUE(code);
  EXPECT_EQ(run(code.code, w.ctx, {0}).out[0], 22u);
  EXPECT_EQ(run(code.code, w.ctx, {0x100000000ull}).out[0], 22u) << "the upper half is not part of an i32";
  EXPECT_EQ(run(code.code, w.ctx, {0x80000000u}).out[0], 11u);
}

/* ---- What each backend emits -------------------------------------------------------------- */

namespace {

GRJIT_Function * one_of_each_kind(int which) {
  switch (which) {
    case 0: return build_wasm_op("i32.add").function;      // only an I32 register is new
    case 1: return build_wasm_op("i64.clz").function;      // an I64 function with a new operation
    case 2: return build_wasm_op("i64.rotl").function;
    case 3: return build_wasm_op("i32.div_s").function;
    case 4: return build_wasm_op("i64.rem_u").function;
    case 5: return build_wasm_op("i64.extend_i32_u").function;
    default: {
      B b("only_a_param", 0);
      GRJIT_VReg x = b.param(GRJIT_TYPE_I32);
      b.at(b.block());
      b.ret(V(x));
      return b.finish();
    }
  }
}

/* Every backend emits the integer operations. */
bool emits_integers(GRJIT_Arch) {
  return true;
}

} // namespace

TEST(IntRefusal, EveryBackendEmitsEveryInstructionOfTheIntegerTables) {
  for (GRJIT_Arch arch : {GRJIT_ARCH_X86_64, GRJIT_ARCH_ARM64, GRJIT_ARCH_X86_64_WIN64}) {
    for (int which = 0; which < 7; which++) {
      SCOPED_TRACE(which);
      Fn f(one_of_each_kind(which));
      GRJIT_Emitted emitted;
      GRJIT_Result r = grjit_emit_for(arch, f, grjit_allocator_default(), nullptr, nullptr, 0, &emitted);
      EXPECT_EQ(r, emits_integers(arch) ? GRJIT_OK : GRJIT_ERR_UNSUPPORTED) << static_cast<int>(arch);
      grjit_emitted_free(&emitted);
    }
  }
  // Every instruction of the table, at both widths, not only a sample.
  size_t emitted_count = 0, table_size = 0;
  for (const char * ty : {"i32", "i64"}) {
    table_size += all_int_ops(ty).size();
  }
  for (GRJIT_Arch arch : {GRJIT_ARCH_X86_64, GRJIT_ARCH_ARM64, GRJIT_ARCH_X86_64_WIN64}) {
    for (const char * ty : {"i32", "i64"}) {
      for (const std::string & instr : all_int_ops(ty)) {
        WasmOp op = build_wasm_op(instr);
        Fn f(op.function);
        bool new_things = false;
        for (size_t v = 0; v < grjit_function_vreg_count(f); v++) {
          new_things |= grjit_function_vreg_type(f, static_cast<GRJIT_VReg>(v)) == GRJIT_TYPE_I32;
        }
        for (size_t blk = 0; blk < grjit_function_block_count(f); blk++) {
          size_t n = 0;
          const GRJIT_Op * ops = grjit_function_block_ops(f, static_cast<GRJIT_BlockId>(blk), &n);
          for (size_t i = 0; i < n; i++) {
            new_things |= ops[i].kind >= GRJIT_OP_CLZ;
          }
        }
        GRJIT_Emitted emitted;
        const GRJIT_Result r = grjit_emit_for(arch, f, grjit_allocator_default(), nullptr, nullptr, 0, &emitted);
        EXPECT_EQ(r, (new_things && !emits_integers(arch)) ? GRJIT_ERR_UNSUPPORTED : GRJIT_OK)
            << instr << " on " << static_cast<int>(arch);
        if (r == GRJIT_OK) {
          emitted_count++;
          EXPECT_GT(emitted.size, 0u) << instr;
        }
        grjit_emitted_free(&emitted);
      }
    }
  }
  EXPECT_GT(table_size, 60u);
  EXPECT_EQ(emitted_count, 3 * table_size) << "the whole table was emitted by SysV, by arm64 and by Win64";
}

TEST(IntRefusal, AFunctionWithoutTheNewTypeIsStillEmittedByAllThreeBackends) {
  B b("old", 0);
  GRJIT_VReg x = b.param(GRJIT_TYPE_I64), r = b.reg(GRJIT_TYPE_I64);
  b.at(b.block());
  b.bin(GRJIT_OP_ADD, r, V(x), I(1));
  b.ret(V(r));
  Fn f(b.finish());
  for (GRJIT_Arch arch : {GRJIT_ARCH_X86_64, GRJIT_ARCH_ARM64, GRJIT_ARCH_X86_64_WIN64}) {
    GRJIT_Emitted emitted;
    EXPECT_EQ(grjit_emit_for(arch, f, grjit_allocator_default(), nullptr, nullptr, 0, &emitted), GRJIT_OK);
    grjit_emitted_free(&emitted);
  }
}

TEST(IntRefusal, ACompileOnTheNativeBackendEmitsTheIntegerOperationsWhereTheBackendDoes) {
  const GRJIT_Arch native = grjit_native_arch();
  if (!grjit_backend_available()) {
    GTEST_SKIP() << "no native code backend on this target";
  }
  JitWorld w;
  for (int which = 0; which < 7; which++) {
    SCOPED_TRACE(which);
    Fn f(one_of_each_kind(which));
    Compiled c(f, w.pages());
    if (emits_integers(native)) {
      EXPECT_EQ(c.result, GRJIT_OK);
      EXPECT_TRUE(c);
    } else {
      EXPECT_EQ(c.result, GRJIT_ERR_UNSUPPORTED);
      EXPECT_FALSE(c);
    }
  }
}

/* ---- I32 across calls and tail calls --------------------------------------- */

namespace {

using fx::K;
using fx::P;

/* sum_n(a0 .. a{n-1}) = sum of (i + 1) * a_i, in 32-bit arithmetic, and caller_n(x) = 1 + sum_n(x,
 * x + 1, ...), all i32. Returns the ids. */
void add_i32_pair(fx::Engine & e, int n, int * callee, int * caller) {
  *callee = e.reserve();
  {
    std::vector<GRJIT_Type> params(static_cast<size_t>(n), GRJIT_TYPE_I32);
    P p("sum", params);
    int acc = p.local(GRJIT_TYPE_I32), c = p.local(GRJIT_TYPE_I32), t = p.local(GRJIT_TYPE_I32);
    p.cnst(acc, 0);
    for (int i = 0; i < n; i++) {
      p.cnst(c, i + 1);
      p.bin(K::MUL, t, i, c);
      p.bin(K::ADD, acc, acc, t);
    }
    p.ret(acc);
    e.set(*callee, p.done());
  }
  *caller = e.reserve();
  {
    P p("caller", {GRJIT_TYPE_I32});
    int x = 0, one = p.local(GRJIT_TYPE_I32), r = p.local(GRJIT_TYPE_I32);
    std::vector<int> args;
    p.cnst(one, 1);
    for (int i = 0; i < n; i++) {
      int a = p.local(GRJIT_TYPE_I32), k = p.local(GRJIT_TYPE_I32);
      p.cnst(k, i);
      p.bin(K::ADD, a, x, k);
      args.push_back(a);
    }
    p.call(r, *callee, args);
    p.bin(K::ADD, r, r, one);
    p.ret(r);
    e.set(*caller, p.done());
  }
}

uint32_t i32_pair_ref(int n, uint32_t x) {
  uint32_t acc = 0;
  for (int i = 0; i < n; i++) {
    acc += static_cast<uint32_t>(i + 1) * (x + static_cast<uint32_t>(i));
  }
  return acc + 1;
}

} // namespace

TEST(IntCalls, AnI32GoesThroughACallInARegisterAndOnTheStackAndComesBackZeroExtended) {
  INT_RUN_HERE();
  fx::Engine e;
  for (int n : {1, 2, 5, 6, 7, 9, 12}) {
    SCOPED_TRACE(n);
    int callee, caller;
    add_i32_pair(e, n, &callee, &caller);
    for (uint32_t x : {0u, 1u, 5u, 0x7FFFFFF0u, 0x80000000u, 0xFFFFFFF0u, 0xFFFFFFFFu}) {
      SCOPED_TRACE(x);
      fx::Outcome c = e.run_compiled(caller, {x});
      ASSERT_TRUE(c.finished);
      EXPECT_EQ(c.exit, uint32_t{GRJIT_EXIT_RETURNED});
      EXPECT_EQ(c.value, uint64_t{i32_pair_ref(n, x)}) << "wraps at 32 bits, upper half zero";
      // The callee is also an entry: the adapter's argument words carry garbage above 32 bits.
      std::vector<fx::u64> args;
      for (int k = 0; k < n; k++) {
        args.push_back(0xDEADBEEF00000000ull | (x + static_cast<uint32_t>(k)));
      }
      fx::Outcome d = e.run_compiled(callee, args);
      ASSERT_TRUE(d.finished);
      EXPECT_EQ(d.value, uint64_t{i32_pair_ref(n, x) - 1}) << "an i32 argument is its low 32 bits";
    }
  }
  EXPECT_EQ(e.st.deopts, 0);
  EXPECT_EQ(e.st.pushes, e.st.pops);
}

TEST(IntCalls, AnI32ParameterReturnedWholeIsItsLow32BitsInARegisterAndOnTheStackWhateverTheEntryWordHeld) {
  INT_RUN_HERE();
  // A sum of i32 parameters cannot see a dirty upper half (the 32-bit forms ignore it), so this callee returns one
  // parameter as it is: the word in its slot, not its arithmetic. Entered with garbage above bit 31 in every word,
  // each parameter, in a register or on the stack, must come back as its low 32 bits and nothing more.
  fx::Engine e;
  for (int n : {1, 2, 6, 7, 9, 12}) {
    SCOPED_TRACE(n);
    for (int j = 0; j < n; j++) {
      SCOPED_TRACE(j);
      std::vector<GRJIT_Type> params(static_cast<size_t>(n), GRJIT_TYPE_I32);
      P p("pick", params);
      p.ret(j);
      int callee = e.reserve();
      e.set(callee, p.done());
      std::vector<fx::u64> words;
      for (int k = 0; k < n; k++) {
        words.push_back(0xDEADBEEF00000000ull | (0x80000000u + static_cast<uint32_t>(k)));
      }
      fx::Outcome d = e.run_compiled(callee, words);
      ASSERT_TRUE(d.finished);
      EXPECT_EQ(d.value, uint64_t{0x80000000u + static_cast<uint32_t>(j)})
          << "the " << (j < 6 ? "register" : "stack") << " parameter is its low 32 bits";
    }
  }
}

TEST(IntCalls, AnI32GoesThroughATailCallInARegisterAndOnTheStack) {
  INT_RUN_HERE();
  fx::Engine e;
  for (int n : {2, 6, 8, 11}) {
    SCOPED_TRACE(n);
    int callee, caller;
    add_i32_pair(e, n, &callee, &caller);
    int tail = e.reserve();
    {
      // tail(x) = tail call sum_n(x, x + 1, ...)
      P p("tail", {GRJIT_TYPE_I32});
      int x = 0;
      std::vector<int> args;
      for (int i = 0; i < n; i++) {
        int a = p.local(GRJIT_TYPE_I32), k = p.local(GRJIT_TYPE_I32);
        p.cnst(k, i);
        p.bin(K::ADD, a, x, k);
        args.push_back(a);
      }
      p.tailcall(callee, args);
      e.set(tail, p.done());
    }
    for (uint32_t x : {3u, 0x80000000u, 0xFFFFFFFFu, 0xFFFFFFF0u}) {
      SCOPED_TRACE(x);
      fx::Outcome c = e.run_compiled(tail, {x});
      ASSERT_TRUE(c.finished);
      EXPECT_EQ(c.exit, uint32_t{GRJIT_EXIT_RETURNED});
      EXPECT_EQ(c.value, uint64_t{i32_pair_ref(n, x) - 1});
    }
  }
  EXPECT_EQ(e.st.deopts, 0);
}

TEST(IntCalls, AnI32GoesThroughACallPtrAndATailCallPtrInARegisterAndOnTheStack) {
  INT_RUN_HERE();
  fx::Engine e;
  for (int n : {2, 9}) {
    SCOPED_TRACE(n);
    int callee, unused_caller;
    add_i32_pair(e, n, &callee, &unused_caller);
    ASSERT_TRUE(e.compile_fn(callee));
    ASSERT_NE(e.code_of(callee).internal, 0u);
    auto args_of = [&](P & p, int x) {
      std::vector<int> args;
      for (int i = 0; i < n; i++) {
        int a = p.local(GRJIT_TYPE_I32), k = p.local(GRJIT_TYPE_I32);
        p.cnst(k, i);
        p.bin(K::ADD, a, x, k);
        args.push_back(a);
      }
      return args;
    };
    int by_call = e.reserve(), by_tail = e.reserve();
    {
      P p("via_ptr", {GRJIT_TYPE_I32});
      int ptr = p.local(GRJIT_TYPE_PTR), r = p.local(GRJIT_TYPE_I32);
      p.entryof(ptr, callee);
      std::vector<int> args = args_of(p, 0);
      p.callp(r, ptr, callee, args);
      p.ret(r);
      e.set(by_call, p.done());
    }
    {
      P p("tail_ptr", {GRJIT_TYPE_I32});
      int ptr = p.local(GRJIT_TYPE_PTR);
      p.entryof(ptr, callee);
      std::vector<int> args = args_of(p, 0);
      p.tailcallp(ptr, callee, args);
      e.set(by_tail, p.done());
    }
    for (uint32_t x : {0u, 3u, 0x80000000u, 0xFFFFFFF0u, 0xFFFFFFFFu}) {
      SCOPED_TRACE(x);
      uint32_t want = i32_pair_ref(n, x) - 1;
      for (int fn : {by_call, by_tail}) {
        fx::Outcome c = e.run_compiled(fn, {0xDEADBEEF00000000ull | x});
        ASSERT_TRUE(c.finished);
        EXPECT_EQ(c.exit, uint32_t{GRJIT_EXIT_RETURNED});
        EXPECT_EQ(c.value, uint64_t{want}) << (fn == by_call ? "CALL_PTR" : "TAIL_CALL_PTR");
      }
    }
  }
  EXPECT_EQ(e.st.deopts, 0);
}

namespace {
uint64_t live_native(void *, uint64_t v) { return v + 1; }
uint64_t live_helper(uint64_t v) { return v + 2; }
uint32_t no_poll(void *, uint64_t, uint64_t) { return 0; }
} // namespace

/* An I32 live across a poll, a call that can collect and a native call is a raw frame-state value,
 * never a stack-map entry: the live list of each site holds the reference and nothing else, and the
 * frame state names the I32 with the representation I32 and the reference as a value. */
TEST(IntRun, AnI32LiveAcrossAPollACallAndANativeIsRawInTheFrameStateAndNeverInTheStackMap) {
  INT_RUN_HERE();
  JitWorld w;
  NativeTab tab;
  uint32_t native = tab.add(reinterpret_cast<const void *>(&live_native), {GRJIT_TYPE_I64}, GRJIT_TYPE_I64);
  B b("live", 2);
  GRJIT_CallHooks h{};
  h.deopt = [](void *, uint64_t) -> uint32_t { return 0; };
  b.callable(h);
  b.natives(tab);
  b.poll_helper(&no_poll);
  GRJIT_VReg x = b.param(GRJIT_TYPE_I32), r = b.param(GRJIT_TYPE_REF);
  GRJIT_VReg u = b.reg(GRJIT_TYPE_I64), v = b.reg(GRJIT_TYPE_I64), t = b.reg(GRJIT_TYPE_I32),
             keep = b.reg(GRJIT_TYPE_I64);
  b.at(b.block());
  auto state = [&] { return std::vector<GRJIT_FrameSlot>{grjit_frame_slot_vreg(x), grjit_frame_slot_vreg(r)}; };
  b.poll({9, 1, 0}, state());
  b.call_gc(u, reinterpret_cast<const void *>(&live_helper), {I(5)}, {9, 2}, state());
  b.call_native(v, native, {V(u)}, {9, 3}, state());
  b.bin(GRJIT_OP_ADD, t, V(x), I(1));
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
    if (s.kind != GRCORE_SITE_GC_POINT_POLL && s.kind != GRCORE_SITE_GC_POINT_CALL &&
        s.kind != GRCORE_SITE_GC_POINT_ALLOC_SLOW) {
      continue;
    }
    if (s.identity.function != 9) {
      continue;
    }
    polls += s.kind == GRCORE_SITE_GC_POINT_POLL;
    calls += s.kind != GRCORE_SITE_GC_POINT_POLL;
    SCOPED_TRACE(s.identity.offset);
    // The stack map: the reference, and no I32.
    ASSERT_EQ(s.live_count, 1u);
    EXPECT_EQ(s.live[0].slot_kind, GRCORE_SLOT_VALUE);
    EXPECT_EQ(s.live[0].representation, GRCORE_REPR_BITS);
    // The frame state: the I32 is raw and says so, the reference is a value.
    ASSERT_EQ(s.frame_state_count, 2u);
    EXPECT_EQ(s.frame_state[0].kind, GRCORE_LOC_FRAME_SLOT);
    EXPECT_EQ(s.frame_state[0].slot_kind, GRCORE_SLOT_RAW);
    EXPECT_EQ(s.frame_state[0].representation, GRCORE_REPR_I32);
    EXPECT_EQ(s.frame_state[1].slot_kind, GRCORE_SLOT_VALUE);
    EXPECT_NE(s.frame_state[0].value, s.live[0].value) << "the I32's slot is not the reference's";
  }
  EXPECT_EQ(polls, 1u);
  EXPECT_EQ(calls, 2u) << "the helper that can collect, and the native";
}

GRJIT_TEST_MAIN()
