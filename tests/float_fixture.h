/**
 * @file
 *
 * Shared by the float tests (spec-runtime-float, story 3): the float operations of
 * numeric-operations.md as single-instruction IR functions, built the way a frontend builds
 * them (the guards of a truncation written in front of it as ordinary IR), and the bit-pattern
 * helpers the tests speak in. The integer table of int_fixture.h answers for the instructions
 * that are not floats (`i64.extend_i32_s`, `i32.wrap_i64`), which conversions.wast also holds.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#ifndef GHOTI_IO_GRJIT_TESTS_FLOAT_FIXTURE_H
#define GHOTI_IO_GRJIT_TESTS_FLOAT_FIXTURE_H

#include "int_fixture.h"
#include "ir_eval.h"

#include <cmath>
#include <cstdint>
#include <limits>
#include <map>
#include <string>
#include <vector>

namespace floatfx {

using intfx::Outcome;
using intfx::kTrapFunction;
using intfx::kSiteIntegerOverflow;
using intfx::run;

/* The identity of the NaN guard of a truncation: WebAssembly's "invalid conversion to integer". */
constexpr uint64_t kSiteInvalidConversion = 3;

/* The name of a trap by the site of the guard that fired: the three of WebAssembly's numeric traps. */
inline const char * trap_name(uint64_t site_offset) {
  return site_offset == kSiteInvalidConversion      ? "invalid conversion to integer"
         : site_offset == intfx::kSiteDivideByZero  ? "integer divide by zero"
         : site_offset == intfx::kSiteIntegerOverflow ? "integer overflow"
                                                    : "?";
}

inline uint64_t f32bits(float f) { return ireval::bits_of(f); }
inline uint64_t f64bits(double d) { return ireval::bits_of(d); }
inline float f32of(uint64_t b) { return ireval::f32_of(b); }
inline double f64of(uint64_t b) { return ireval::f64_of(b); }

/* The guards a frontend writes in front of a truncation. A test leaves one out, or widens the range
 * by `widen_ulps` units in the last place at the lower and at the upper bound. */
struct TruncGuards {
  bool nan = true;       ///< the operand is not a NaN (`CMP EQ x, x`): "invalid conversion to integer"
  bool range = true;     ///< the operand's truncation fits: "integer overflow"
  int widen_lo = 0;      ///< move the lower bound this many ulps further out (negative: in)
  int widen_hi = 0;      ///< the same for the upper bound
  bool lower_closed = false; ///< force `>=` (a wider test than the exact `>` for a bound)
};

/* The same record the integer table answers with: the function, its result type and parameter
 * count, and whether it is partial (here, a truncation, which has guards to leave out). */
using FloatOp = intfx::WasmOp;

/* The exact bounds of `x` for a truncation, as the real numbers the guard compares with: the
 * lower is exclusive except for the signed 64-bit one, whose exclusive bound is not a double
 * (see ir.h), and the upper is exclusive. */
struct Bounds {
  double lo, hi;
  bool lo_closed;
};

inline Bounds trunc_bounds(bool is_signed, bool wide) {
  if (is_signed) {
    return wide ? Bounds{-9223372036854775808.0, 9223372036854775808.0, true}
                : Bounds{-2147483649.0, 2147483648.0, false};
  }
  return Bounds{-1.0, wide ? 18446744073709551616.0 : 4294967296.0, false};
}

/* The next double (or float) `ulps` away in the direction of `toward`. */
inline double step(double v, bool dbl, int ulps, double toward) {
  for (int i = 0; i < (ulps < 0 ? -ulps : ulps); i++) {
    const double dir = ulps > 0 ? toward : -toward;
    v = dbl ? std::nextafter(v, dir)
            : static_cast<double>(std::nextafterf(static_cast<float>(v), static_cast<float>(dir)));
  }
  return v;
}

inline uint64_t imm_bits(double v, bool dbl) {
  return dbl ? f64bits(v) : f32bits(static_cast<float>(v));
}

inline FloatOp build_float_op(const std::string & instr, TruncGuards guards = TruncGuards{}) {
  FloatOp w;
  const size_t dot = instr.find('.');
  if (dot == std::string::npos) {
    return w;
  }
  const std::string ty = instr.substr(0, dot);
  const std::string name = instr.substr(dot + 1);
  B b(instr.c_str(), 0);
  auto done = [&](GRJIT_Type result, size_t params, bool partial = false) {
    w.result = result;
    w.params = params;
    w.partial = partial;
    w.function = b.finish();
    return w;
  };
  if (ty == "f32" || ty == "f64") {
    const GRJIT_Type T = ty == "f32" ? GRJIT_TYPE_F32 : GRJIT_TYPE_F64;
    static const std::map<std::string, GRJIT_OpKind> binary = {{"add", GRJIT_OP_FADD},
        {"sub", GRJIT_OP_FSUB}, {"mul", GRJIT_OP_FMUL}, {"div", GRJIT_OP_FDIV},
        {"min", GRJIT_OP_FMIN}, {"max", GRJIT_OP_FMAX}, {"copysign", GRJIT_OP_FCOPYSIGN}};
    static const std::map<std::string, GRJIT_OpKind> unary = {{"neg", GRJIT_OP_FNEG},
        {"abs", GRJIT_OP_FABS}, {"sqrt", GRJIT_OP_FSQRT}, {"ceil", GRJIT_OP_FCEIL},
        {"floor", GRJIT_OP_FFLOOR}, {"trunc", GRJIT_OP_FTRUNC}, {"nearest", GRJIT_OP_FNEAREST}};
    static const std::map<std::string, GRJIT_Cmp> compare = {{"eq", GRJIT_CMP_EQ},
        {"ne", GRJIT_CMP_NE}, {"lt", GRJIT_CMP_LT}, {"gt", GRJIT_CMP_GT}, {"le", GRJIT_CMP_LE},
        {"ge", GRJIT_CMP_GE}};
    auto bi = binary.find(name);
    if (bi != binary.end()) {
      GRJIT_VReg x = b.param(T), y = b.param(T), r = b.reg(T);
      b.at(b.block());
      b.bin(bi->second, r, V(x), V(y));
      b.ret(V(r));
      return done(T, 2);
    }
    auto un = unary.find(name);
    if (un != unary.end()) {
      GRJIT_VReg x = b.param(T), r = b.reg(T);
      b.at(b.block());
      b.un(un->second, r, V(x));
      b.ret(V(r));
      return done(T, 1);
    }
    auto c = compare.find(name);
    if (c != compare.end()) {
      GRJIT_VReg x = b.param(T), y = b.param(T), r = b.reg(GRJIT_TYPE_I32);
      b.at(b.block());
      b.cmp(c->second, r, V(x), V(y));
      b.ret(V(r));
      return done(GRJIT_TYPE_I32, 2);
    }
    const bool to_f32 = ty == "f32";
    static const std::map<std::string, std::pair<GRJIT_Type, bool>> convert = {
        {"convert_i32_s", {GRJIT_TYPE_I32, true}}, {"convert_i32_u", {GRJIT_TYPE_I32, false}},
        {"convert_i64_s", {GRJIT_TYPE_I64, true}}, {"convert_i64_u", {GRJIT_TYPE_I64, false}}};
    auto cv = convert.find(name);
    if (cv != convert.end()) {
      GRJIT_VReg x = b.param(cv->second.first), r = b.reg(T);
      b.at(b.block());
      b.un(cv->second.second ? GRJIT_OP_CONVERT_S : GRJIT_OP_CONVERT_U, r, V(x));
      b.ret(V(r));
      return done(T, 1);
    }
    if (name == "demote_f64" && to_f32) {
      GRJIT_VReg x = b.param(GRJIT_TYPE_F64), r = b.reg(T);
      b.at(b.block());
      b.un(GRJIT_OP_DEMOTE, r, V(x));
      b.ret(V(r));
      return done(T, 1);
    }
    if (name == "promote_f32" && !to_f32) {
      GRJIT_VReg x = b.param(GRJIT_TYPE_F32), r = b.reg(T);
      b.at(b.block());
      b.un(GRJIT_OP_PROMOTE, r, V(x));
      b.ret(V(r));
      return done(T, 1);
    }
    if ((name == "reinterpret_i32" && to_f32) || (name == "reinterpret_i64" && !to_f32)) {
      GRJIT_VReg x = b.param(to_f32 ? GRJIT_TYPE_I32 : GRJIT_TYPE_I64), r = b.reg(T);
      b.at(b.block());
      b.un(GRJIT_OP_REINTERPRET, r, V(x));
      b.ret(V(r));
      return done(T, 1);
    }
    return w;
  }
  if (ty == "i32" || ty == "i64") {
    const GRJIT_Type D = ty == "i32" ? GRJIT_TYPE_I32 : GRJIT_TYPE_I64;
    const bool wide = ty == "i64";
    if ((name == "reinterpret_f32" && !wide) || (name == "reinterpret_f64" && wide)) {
      GRJIT_VReg x = b.param(wide ? GRJIT_TYPE_F64 : GRJIT_TYPE_F32), r = b.reg(D);
      b.at(b.block());
      b.un(GRJIT_OP_REINTERPRET, r, V(x));
      b.ret(V(r));
      return done(D, 1);
    }
    // trunc_f32_s, trunc_f64_u, trunc_sat_f32_s, ...
    const bool sat = name.rfind("trunc_sat_", 0) == 0;
    const bool plain = !sat && name.rfind("trunc_", 0) == 0;
    if (sat || plain) {
      const std::string rest = name.substr(sat ? 10 : 6); // "f32_s"
      if (rest.size() != 5 || rest[0] != 'f' || rest[3] != '_') {
        return w;
      }
      const bool dbl = rest.substr(0, 3) == "f64";
      if (!dbl && rest.substr(0, 3) != "f32") {
        return w;
      }
      const bool is_signed = rest[4] == 's';
      if (!is_signed && rest[4] != 'u') {
        return w;
      }
      const GRJIT_Type S = dbl ? GRJIT_TYPE_F64 : GRJIT_TYPE_F32;
      GRJIT_VReg x = b.param(S), r = b.reg(D);
      b.at(b.block());
      if (sat) {
        b.un(is_signed ? GRJIT_OP_TRUNC_SAT_S : GRJIT_OP_TRUNC_SAT_U, r, V(x));
        b.ret(V(r));
        return done(D, 1);
      }
      if (guards.nan) {
        GRJIT_VReg same = b.reg(GRJIT_TYPE_I32);
        b.cmp(GRJIT_CMP_EQ, same, V(x), V(x));
        b.guard(V(same), {kTrapFunction, kSiteInvalidConversion});
      }
      if (guards.range) {
        const Bounds bd = trunc_bounds(is_signed, wide);
        double lo0 = bd.lo;
        bool closed = bd.lo_closed || guards.lower_closed;
        if (!dbl && static_cast<double>(static_cast<float>(lo0)) != lo0) {
          // -2^31 - 1 is no float: the exact test on a float is x >= -2^31
          lo0 = static_cast<double>(static_cast<float>(lo0));
          closed = true;
        }
        const double lo = step(lo0, dbl, guards.widen_lo, -INFINITY);
        const double hi = step(bd.hi, dbl, guards.widen_hi, INFINITY);
        GRJIT_VReg l = b.reg(GRJIT_TYPE_I32), h = b.reg(GRJIT_TYPE_I32), ok = b.reg(GRJIT_TYPE_I32);
        b.cmp(closed ? GRJIT_CMP_GE : GRJIT_CMP_GT, l, V(x),
            I(static_cast<int64_t>(imm_bits(lo, dbl))));
        b.cmp(GRJIT_CMP_LT, h, V(x), I(static_cast<int64_t>(imm_bits(hi, dbl))));
        b.bin(GRJIT_OP_AND, ok, V(l), V(h));
        b.guard(V(ok), {kTrapFunction, kSiteIntegerOverflow});
      }
      b.un(is_signed ? GRJIT_OP_TRUNC_S : GRJIT_OP_TRUNC_U, r, V(x));
      b.ret(V(r));
      return done(D, 1, true);
    }
  }
  return w;
}

/* The float instruction table, then the integer one (for `i64.extend_i32_s` and the others of
 * conversions.wast that are integer operations). `function` is null for an unknown name. */
inline FloatOp build_any_op(const std::string & instr, TruncGuards guards = TruncGuards{}) {
  FloatOp w = build_float_op(instr, guards);
  if (w.function != nullptr) {
    return w;
  }
  intfx::WasmOp i = intfx::build_wasm_op(instr);
  w.function = i.function;
  w.result = i.result;
  w.params = i.params;
  w.partial = false; // an integer partial operation is not truncated here
  return w;
}

} // namespace floatfx

#endif
