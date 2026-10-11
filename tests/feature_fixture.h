/*
 * SPDX-License-Identifier: LGPL-3.0-only
 *
 * Copyright (C) 2026 Corey Pennycuff
 *
 * This file is part of Ghoti.io Runtime-jit.
 *
 * Ghoti.io Runtime-jit is free software: you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public License version
 * 3 as published by the Free Software Foundation.
 *
 * Ghoti.io Runtime-jit is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU Lesser
 * General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

/**
 * @file feature_fixture.h
 *
 * What the CPU-feature tests share (spec-runtime-float, story 4): the subsets of the host's detected
 * set as masks, the pinned bytes of each arm, a compile under a mask, the recorded set an operation
 * should have, and the skip-by-name macro with the counter that shows nothing was skipped on a host
 * that has every feature.
 */

#ifndef GHOTI_IO_GRJIT_TESTS_FEATURE_FIXTURE_H
#define GHOTI_IO_GRJIT_TESTS_FEATURE_FIXTURE_H

#include "test_helpers.h"
#include "int_fixture.h"
#include "float_fixture.h"
#include "ir_eval.h"
#include "cpu_pins.h"

#include "../src/code/code_internal.h"
#include "../src/code/features_internal.h"

#include <algorithm>
#include <atomic>
#include <map>
#include <memory>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace featfx {

using intfx::Outcome;
using intfx::run;

inline const char * feature_name(GRJIT_Features f) {
  switch (f) {
    case GRJIT_FEATURE_SSE4_1: return "SSE4.1";
    case GRJIT_FEATURE_POPCNT: return "POPCNT";
    case GRJIT_FEATURE_LZCNT: return "LZCNT";
    case GRJIT_FEATURE_BMI1: return "BMI1";
    default: return "?";
  }
}

/* The features the native backend can use: the host's detected set. (The Win64 flavour takes the same arms as SysV
 * under the same set; arm64 detects none.) */
inline GRJIT_Features detected() { return grjit_features_detected(); }

/* The disabled masks that give each subset of the host's detected set as the effective one: up to
 * sixteen, the first (subset 0) being "everything disabled". */
inline std::vector<GRJIT_Features> every_subset_masks() {
  std::vector<GRJIT_Features> masks;
  for (GRJIT_Features subset = 0; subset <= GRJIT_FEATURES_ALL; subset++) {
    if ((subset & ~detected()) == 0) {
      masks.push_back(GRJIT_FEATURES_ALL & ~subset);
    }
  }
  return masks;
}

/* The two arms every test of a feature-dependent operation runs: what the host has, and none of it. */
inline std::vector<GRJIT_Features> two_arms() { return {GRJIT_FEATURES_NONE, GRJIT_FEATURES_ALL}; }

/* Tests that need a feature the host lacks are skipped by name, and counted. */
inline std::atomic<long> g_skipped_for_missing_feature{0};

#define GRJIT_REQUIRE_FEATURE(f) \
  do { \
    if ((::featfx::detected() & (f)) == 0) { \
      ::featfx::g_skipped_for_missing_feature++; \
      GTEST_SKIP() << "the host lacks " << ::featfx::feature_name(f) \
                   << ", which this test needs to run its feature arm"; \
    } \
  } while (0)

/* The features an operation's feature arm uses, by the Wasm name of the operation, or none. */
inline GRJIT_Features need_of(const std::string & instr) {
  const std::string op = instr.substr(instr.find('.') + 1);
  if (op == "clz") return GRJIT_FEATURE_LZCNT;
  if (op == "ctz") return GRJIT_FEATURE_BMI1;
  if (op == "popcnt") return GRJIT_FEATURE_POPCNT;
  if (op == "ceil" || op == "floor" || op == "trunc" || op == "nearest") return GRJIT_FEATURE_SSE4_1;
  return GRJIT_FEATURES_NONE;
}

/* The fourteen operations whose arm depends on a feature. */
inline std::vector<std::string> feature_ops() {
  std::vector<std::string> v;
  for (const cpupins::Pin & p : cpupins::kPins) {
    v.push_back(p.instr);
  }
  return v;
}

inline std::string hex_of(const uint8_t * b, size_t n) {
  static const char * d = "0123456789abcdef";
  std::string s;
  for (size_t i = 0; i < n; i++) {
    s += d[b[i] >> 4];
    s += d[b[i] & 15];
  }
  return s;
}

/* An operation emitted for x86-64 SysV with a named effective set: its bytes as hex, and the set the
 * emitter recorded. */
struct Emit {
  GRJIT_Result result = GRJIT_ERR_INTERNAL;
  std::string bytes;
  GRJIT_Features used = ~GRJIT_Features{0};
};

inline Emit emit_with(GRJIT_Function * f, GRJIT_Features features, GRJIT_Arch arch = GRJIT_ARCH_X86_64) {
  Emit e;
  GRJIT_Emitted out;
  e.result = grjit_emit_for_features(
      arch, f, grjit_allocator_default(), nullptr, nullptr, 0x40, features, &out);
  if (e.result == GRJIT_OK) {
    e.bytes = hex_of(out.bytes, out.size);
    e.used = out.features_used;
    grjit_emitted_free(&out);
  }
  return e;
}

/* The values an operation is run on. The evaluator is the oracle for every one; a worked table
 * names a few answers outright. */
inline std::vector<uint64_t> integer_boundaries(bool narrow) {
  std::vector<uint64_t> v = {0, 1, 2, 0x80, 0x8000, 0x80000000ull, 0x7FFFFFFF, 0xFFFFFFFFull,
      0x100000000ull, 0x8000000000000000ull, ~uint64_t{0}, 0x5555555555555555ull, 0xAAAAAAAAAAAAAAAAull,
      0x55555555ull, 0xAAAAAAAAull, 0xFFFFFFFF00000000ull, 0xDEADBEEF00000010ull, 0x0000000100000000ull,
      0x7FFFFFFFFFFFFFFFull, 0x123456789ABCDEF0ull, 0x00000000F0F0F0F0ull, 0x0F0F0F0F0F0F0F0Full};
  for (unsigned bit = 0; bit < (narrow ? 32u : 64u); bit++) {
    v.push_back(uint64_t{1} << bit);
    v.push_back((uint64_t{1} << bit) | 1);
    v.push_back(~(uint64_t{1} << bit));
    // a dirty upper half on an i32 entry word must change nothing
    if (narrow) {
      v.push_back((uint64_t{1} << bit) | 0xABCD000000000000ull);
    }
  }
  return v;
}

inline std::vector<uint64_t> float_boundaries(bool narrow) {
  std::vector<uint64_t> v = narrow ? floatfx::f32_values() : floatfx::f64_values();
  auto add = [&](double d) { v.push_back(narrow ? floatfx::f32bits(static_cast<float>(d)) : floatfx::f64bits(d)); };
  for (double d : {0.0, -0.0, 0.25, -0.25, 0.5, -0.5, 1.5, -1.5, 2.5, -2.5, 3.5, -3.5, 0.49999997, 8388607.5,
           8388608.0, 8388609.0, -8388608.5, 9223372036854775808.0, -9223372036854775808.0}) {
    add(d);
  }
  if (!narrow) {
    for (double d : {0.49999999999999994, 4503599627370495.5, 4503599627370496.0, 4503599627370497.0,
             -4503599627370495.5, 9007199254740993.0}) {
      add(d);
    }
    for (uint64_t b : {0x0000000000000001ull, 0x000FFFFFFFFFFFFFull, 0x0010000000000000ull,
             0x7FEFFFFFFFFFFFFFull, 0x8000000000000001ull, 0x800FFFFFFFFFFFFFull, 0x7FF0000000000000ull,
             0xFFF0000000000000ull, 0x7FF8000000000000ull, 0xFFF8000000000000ull, 0x7FF0000000000001ull,
             0xFFF0000000000001ull, 0x7FF800000000ABCDull, 0xFFF800000000ABCDull, 0x7FF4000000000000ull,
             0xFFF4000000000123ull}) {
      v.push_back(b);
    }
  } else {
    for (uint64_t b : {0x00000001ull, 0x007FFFFFull, 0x00800000ull, 0x7F7FFFFFull, 0x80000001ull, 0x807FFFFFull,
             0x7F800000ull, 0xFF800000ull, 0x7FC00000ull, 0xFFC00000ull, 0x7F800001ull, 0xFF800001ull,
             0x7FC0ABCDull, 0xFFC0ABCDull, 0x7FA00000ull, 0xFFA00123ull}) {
      v.push_back(b);
    }
  }
  return v;
}

/* Compiled code made under a mask, with a run wrapper. */
struct Under {
  GRJIT_Function * function = nullptr;
  GRJIT_Code * code = nullptr;
  GRJIT_Result result = GRJIT_ERR_INTERNAL;
  GRJIT_Type result_type = GRJIT_TYPE_I64;
  size_t params = 0;

  Under(const Under &) = delete;
  Under & operator=(const Under &) = delete;
  Under(const std::string & instr, JitWorld & w, GRJIT_Features disabled) {
    floatfx::FloatOp op = floatfx::build_any_op(instr);
    function = op.function;
    result_type = op.result;
    params = op.params;
    if (function == nullptr) {
      return;
    }
    GRJIT_CompileOptions o{};
    o.pages = w.pages();
    o.disabled_features = disabled;
    result = grjit_compile(&o, function, &code);
  }
  ~Under() {
    grjit_code_destroy(code);
    grjit_function_destroy(function);
  }
  unsigned width() const {
    return result_type == GRJIT_TYPE_I32 || result_type == GRJIT_TYPE_F32 ? 32 : 64;
  }
  bool float_result() const { return result_type == GRJIT_TYPE_F32 || result_type == GRJIT_TYPE_F64; }
};

} // namespace featfx

#endif
