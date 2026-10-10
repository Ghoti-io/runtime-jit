/**
 * @file
 *
 * What the integer tests and the Wasm spec-suite harness share (spec-runtime-float, story 2):
 *
 * - a call of compiled code that turns a hardware fault into a result (a divide that reached
 *   `idiv` with a zero divisor raises SIGFPE; a test must report that as a failure with a
 *   message, not die), and
 * - `build_wasm_op`, which turns the name of one WebAssembly numeric instruction into an IR
 *   function of that one operation, with the guards of a partial operation written in front
 *   of it as ordinary IR, exactly as a frontend would.
 *
 * Test-side only. It is not a Wasm frontend and no public API: it knows the integer table of
 * numeric-operations.md and nothing else.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#ifndef GHOTI_IO_GRJIT_TESTS_INT_FIXTURE_H
#define GHOTI_IO_GRJIT_TESTS_INT_FIXTURE_H

#include "test_helpers.h"

#include <algorithm>
#include <csetjmp>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace intfx {

/* ---- A call that survives a hardware fault ------------------------------------------------ */

#ifndef _WIN32
inline sigjmp_buf g_fault_env;
inline volatile std::sig_atomic_t g_fault_signal = 0;

inline void on_fault(int sig) {
  g_fault_signal = sig;
  siglongjmp(g_fault_env, 1);
}

/* Calls compiled code; returns 0, or the signal that stopped it (the exit and `out` are then
 * not meaningful). Only trivially destructible locals live across the setjmp. */
inline int guarded_call(const GRJIT_Code * code, void * context, const uint64_t * args,
    uint64_t * out, uint32_t * exit_status) {
  static const int signals[] = {SIGFPE, SIGSEGV, SIGILL, SIGBUS};
  struct sigaction mine;
  std::memset(&mine, 0, sizeof mine);
  mine.sa_handler = on_fault;
  sigemptyset(&mine.sa_mask);
  mine.sa_flags = SA_NODEFER;
  struct sigaction old[4];
  for (int i = 0; i < 4; i++) {
    sigaction(signals[i], &mine, &old[i]);
  }
  int caught = 0;
  g_fault_signal = 0;
  if (sigsetjmp(g_fault_env, 1) == 0) {
    *exit_status = grjit_code_call(code, context, args, out);
  } else {
    caught = static_cast<int>(g_fault_signal);
  }
  for (int i = 0; i < 4; i++) {
    sigaction(signals[i], &old[i], nullptr);
  }
  return caught;
}
#else
inline int guarded_call(const GRJIT_Code * code, void * context, const uint64_t * args,
    uint64_t * out, uint32_t * exit_status) {
  *exit_status = grjit_code_call(code, context, args, out);
  return 0;
}
#endif

struct Outcome {
  int fault = 0;          ///< The signal that stopped the code, or 0.
  uint32_t exit = 99;
  std::vector<uint64_t> out;
};

inline Outcome run(const GRJIT_Code * code, void * context, std::vector<uint64_t> args) {
  Outcome o;
  o.out.assign(grjit_code_out_words(code), 0xDEADBEEFu);
  args.resize(std::max<size_t>(args.size(), grjit_code_param_count(code)));
  o.fault = guarded_call(code, context, args.data(), o.out.data(), &o.exit);
  return o;
}

/* ---- One WebAssembly instruction as an IR function ---------------------------------------- */

/* The identity of the guard that fired says which trap it was (the story's rule: by the guard's
 * site, not by a new exit code). The function number is the same for every function built here. */
constexpr uint64_t kTrapFunction = 0x77;
constexpr uint64_t kSiteDivideByZero = 1;
constexpr uint64_t kSiteIntegerOverflow = 2;

inline const char * trap_name(uint64_t site_offset) {
  return site_offset == kSiteDivideByZero     ? "integer divide by zero"
         : site_offset == kSiteIntegerOverflow ? "integer overflow"
                                               : "?";
}

/* The guards a frontend emits in front of a partial operation. A test may leave one out. */
struct Guards {
  bool zero = true;     ///< the divisor is not zero (all four operations)
  bool overflow = true; ///< not (a == MIN and b == -1) (`div_s`)
};

struct WasmOp {
  GRJIT_Function * function = nullptr;
  GRJIT_Type result = GRJIT_TYPE_I64;
  size_t params = 0;
  bool partial = false;
};

/* Builds `instr` ("i32.add", "i64.rem_u", "i32.wrap_i64", ...). `function` is null for a name
 * this table does not have. */
inline WasmOp build_wasm_op(const std::string & instr, Guards guards = Guards{}) {
  WasmOp w;
  const size_t dot = instr.find('.');
  if (dot == std::string::npos) {
    return w;
  }
  const std::string ty = instr.substr(0, dot);
  const std::string name = instr.substr(dot + 1);
  if (ty != "i32" && ty != "i64") {
    return w;
  }
  const bool narrow = ty == "i32";
  const GRJIT_Type T = narrow ? GRJIT_TYPE_I32 : GRJIT_TYPE_I64;
  const int64_t MIN = narrow ? INT32_MIN : INT64_MIN;

  static const std::map<std::string, GRJIT_OpKind> binary = {{"add", GRJIT_OP_ADD},
      {"sub", GRJIT_OP_SUB}, {"mul", GRJIT_OP_MUL}, {"and", GRJIT_OP_AND}, {"or", GRJIT_OP_OR},
      {"xor", GRJIT_OP_XOR}, {"shl", GRJIT_OP_SHL}, {"shr_s", GRJIT_OP_SAR},
      {"shr_u", GRJIT_OP_SHR}, {"rotl", GRJIT_OP_ROTL}, {"rotr", GRJIT_OP_ROTR}};
  static const std::map<std::string, GRJIT_OpKind> divide = {{"div_s", GRJIT_OP_DIV_S},
      {"div_u", GRJIT_OP_DIV_U}, {"rem_s", GRJIT_OP_REM_S}, {"rem_u", GRJIT_OP_REM_U}};
  static const std::map<std::string, GRJIT_Cmp> compare = {{"eq", GRJIT_CMP_EQ},
      {"ne", GRJIT_CMP_NE}, {"lt_s", GRJIT_CMP_LT}, {"lt_u", GRJIT_CMP_ULT},
      {"gt_s", GRJIT_CMP_GT}, {"gt_u", GRJIT_CMP_UGT}, {"le_s", GRJIT_CMP_LE},
      {"le_u", GRJIT_CMP_ULE}, {"ge_s", GRJIT_CMP_GE}, {"ge_u", GRJIT_CMP_UGE}};
  static const std::map<std::string, GRJIT_OpKind> unary = {{"clz", GRJIT_OP_CLZ},
      {"ctz", GRJIT_OP_CTZ}, {"popcnt", GRJIT_OP_POPCNT}, {"extend8_s", GRJIT_OP_EXT8_S},
      {"extend16_s", GRJIT_OP_EXT16_S}, {"extend32_s", GRJIT_OP_EXT32_S}};

  B b(instr.c_str(), 0);
  if (name == "wrap_i64" && narrow) {
    GRJIT_VReg x = b.param(GRJIT_TYPE_I64);
    GRJIT_VReg r = b.reg(GRJIT_TYPE_I32);
    b.at(b.block());
    b.un(GRJIT_OP_WRAP, r, V(x));
    b.ret(V(r));
    w.result = GRJIT_TYPE_I32;
    w.params = 1;
    w.function = b.finish();
    return w;
  }
  if ((name == "extend_i32_s" || name == "extend_i32_u") && !narrow) {
    GRJIT_VReg x = b.param(GRJIT_TYPE_I32);
    GRJIT_VReg r = b.reg(GRJIT_TYPE_I64);
    b.at(b.block());
    b.un(name == "extend_i32_s" ? GRJIT_OP_EXTEND_S : GRJIT_OP_EXTEND_U, r, V(x));
    b.ret(V(r));
    w.result = GRJIT_TYPE_I64;
    w.params = 1;
    w.function = b.finish();
    return w;
  }
  if (name == "eqz") {
    GRJIT_VReg x = b.param(T);
    GRJIT_VReg r = b.reg(GRJIT_TYPE_I32);
    b.at(b.block());
    b.cmp(GRJIT_CMP_EQ, r, V(x), I(0));
    b.ret(V(r));
    w.result = GRJIT_TYPE_I32;
    w.params = 1;
    w.function = b.finish();
    return w;
  }
  auto c = compare.find(name);
  if (c != compare.end()) {
    GRJIT_VReg x = b.param(T);
    GRJIT_VReg y = b.param(T);
    GRJIT_VReg r = b.reg(GRJIT_TYPE_I32);
    b.at(b.block());
    b.cmp(c->second, r, V(x), V(y));
    b.ret(V(r));
    w.result = GRJIT_TYPE_I32;
    w.params = 2;
    w.function = b.finish();
    return w;
  }
  auto u = unary.find(name);
  if (u != unary.end()) {
    if (name == "extend32_s" && narrow) {
      return w;
    }
    GRJIT_VReg x = b.param(T);
    GRJIT_VReg r = b.reg(T);
    b.at(b.block());
    b.un(u->second, r, V(x));
    b.ret(V(r));
    w.result = T;
    w.params = 1;
    w.function = b.finish();
    return w;
  }
  auto bi = binary.find(name);
  if (bi != binary.end()) {
    GRJIT_VReg x = b.param(T);
    GRJIT_VReg y = b.param(T);
    GRJIT_VReg r = b.reg(T);
    b.at(b.block());
    b.bin(bi->second, r, V(x), V(y));
    b.ret(V(r));
    w.result = T;
    w.params = 2;
    w.function = b.finish();
    return w;
  }
  auto d = divide.find(name);
  if (d != divide.end()) {
    GRJIT_VReg x = b.param(T);
    GRJIT_VReg y = b.param(T);
    GRJIT_VReg r = b.reg(T);
    GRJIT_VReg not_zero = b.reg(T);
    b.at(b.block());
    if (guards.zero) {
      b.cmp(GRJIT_CMP_NE, not_zero, V(y), I(0));
      b.guard(V(not_zero), {kTrapFunction, kSiteDivideByZero});
    }
    if (d->second == GRJIT_OP_DIV_S && guards.overflow) {
      GRJIT_VReg is_min = b.reg(T), is_neg1 = b.reg(T), both = b.reg(T), fine = b.reg(T);
      b.cmp(GRJIT_CMP_EQ, is_min, V(x), I(MIN));
      b.cmp(GRJIT_CMP_EQ, is_neg1, V(y), I(-1));
      b.bin(GRJIT_OP_AND, both, V(is_min), V(is_neg1));
      b.cmp(GRJIT_CMP_EQ, fine, V(both), I(0));
      b.guard(V(fine), {kTrapFunction, kSiteIntegerOverflow});
    }
    b.bin(d->second, r, V(x), V(y));
    b.ret(V(r));
    w.result = T;
    w.params = 2;
    w.partial = true;
    w.function = b.finish();
    return w;
  }
  return w;
}

/* The names build_wasm_op knows, per width: the integer table of numeric-operations.md. */
inline std::vector<std::string> all_int_ops(const std::string & ty) {
  std::vector<std::string> v;
  for (const char * n : {"add", "sub", "mul", "and", "or", "xor", "shl", "shr_s", "shr_u",
           "rotl", "rotr", "clz", "ctz", "popcnt", "eqz", "eq", "ne", "lt_s", "lt_u", "gt_s",
           "gt_u", "le_s", "le_u", "ge_s", "ge_u", "extend8_s", "extend16_s", "div_s", "div_u",
           "rem_s", "rem_u"}) {
    v.push_back(ty + "." + n);
  }
  if (ty == "i64") {
    v.push_back("i64.extend32_s");
    v.push_back("i64.extend_i32_s");
    v.push_back("i64.extend_i32_u");
  } else {
    v.push_back("i32.wrap_i64");
  }
  return v;
}

} // namespace intfx

#endif
