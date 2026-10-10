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
 * @file features.h
 * @stability free
 *
 * The CPU features the x86-64 backend may use beyond x86-64-v1 (SSE2): which
 * the host has, how a host switches one off, and the text a code cache's stamp
 * carries for a set (spec-runtime-float, story 4).
 *
 * Four features, each used for one thing: SSE4.1 for `roundss`/`roundsd`
 * (`ceil`, `floor`, `trunc`, `nearest`), POPCNT for `popcnt`, LZCNT for `clz`
 * and BMI1 for `tzcnt` (`ctz`). A CPU without LZCNT or BMI1 does not fault on
 * `lzcnt` or `tzcnt`: it runs them as `bsr` and `bsf`, which give other answers
 * for a zero and for every non-zero input. So a feature wrongly believed present
 * gives wrong results rather than a crash, and detection is a function of the
 * raw CPUID words that a table can test on any host
 * (`grjit_features_from_cpuid`, internal).
 *
 * **Three sets, not one.** *Detected* is what the CPU has. *Effective* is
 * detected minus the mask a compile was given (::GRJIT_CompileOptions
 * `disabled_features`), and is all a compile may use: no mask can add a feature
 * the CPU lacks. *Recorded* (::grjit_code_features) is the subset of the
 * effective set whose instructions the compiled code contains. Code recorded as
 * needing `U` may run where the running context's effective set is `R` if and
 * only if `U` is a subset of `R` (::grjit_code_usable_with).
 *
 * On a target that is not x86-64 the detected set is empty: arm64 needs none
 * (`frint*`, `clz` and `cnt` are baseline ARMv8).
 *
 * The library never reads a mask, or anything else, from the environment.
 */

#ifndef GHOTI_IO_GRJIT_FEATURES_H
#define GHOTI_IO_GRJIT_FEATURES_H

#include <ghoti.io/runtime-jit/macros.h>

#include <ghoti.io/runtime-jit/core.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief A set of CPU features, one bit each. The bits above
 *  ::GRJIT_FEATURES_ALL are reserved: never produced, and ignored in a mask. A
 *  feature is only ever added at the next bit. */
typedef uint32_t GRJIT_Features;

/** @brief SSE4.1: `roundss` and `roundsd` (CPUID leaf 1, ECX bit 19). */
#define GRJIT_FEATURE_SSE4_1 ((GRJIT_Features)1)
/** @brief POPCNT: `popcnt` (CPUID leaf 1, ECX bit 23). */
#define GRJIT_FEATURE_POPCNT ((GRJIT_Features)2)
/** @brief LZCNT: `lzcnt` (CPUID leaf 0x80000001, ECX bit 5; leaf 1's bit 5 is VMX). */
#define GRJIT_FEATURE_LZCNT ((GRJIT_Features)4)
/** @brief BMI1: `tzcnt` (CPUID leaf 7 subleaf 0, EBX bit 3). */
#define GRJIT_FEATURE_BMI1 ((GRJIT_Features)8)
/** @brief No optional feature: x86-64-v1. */
#define GRJIT_FEATURES_NONE ((GRJIT_Features)0)
/** @brief Every feature this build knows. As a mask, "every optional feature
 *  off". */
#define GRJIT_FEATURES_ALL ((GRJIT_Features)15)

/** @brief The size of buffer ::grjit_features_stamp always fits in, with its
 *  terminator. */
#define GRJIT_FEATURES_STAMP_MAX 64

/**
 * @brief The features the host CPU has.
 *
 * On x86-64 (every operating system) it is read from CPUID once and kept; on
 * any other target it is ::GRJIT_FEATURES_NONE. Thread-safe, takes no lock and
 * allocates nothing. It is never taken from the compiler's own CPU-support
 * built-ins, the operating system's auxiliary vector, `/proc` or the
 * environment.
 */
GRJIT_API GRJIT_Features grjit_features_detected(void);

/**
 * @brief The features a compile with this mask may use:
 *   `detected & ~disabled & ::GRJIT_FEATURES_ALL`.
 *
 * Zero disables nothing. There is no way to name a feature the CPU lacks, so no
 * value of `disabled`, however wrong, enables one.
 */
GRJIT_API GRJIT_Features grjit_features_effective(GRJIT_Features disabled);

/** @brief Whether every feature in `a` is in `b`. */
GRJIT_API bool grjit_features_subset(GRJIT_Features a, GRJIT_Features b);

/**
 * @brief Writes the canonical text the cache stamp carries for a feature set.
 *
 * On x86-64 it is `x86_64-v1` for the empty set and, for the full one,
 * `x86_64-v1+sse4.1+popcnt+lzcnt+bmi1`: always in bit order, lowercase,
 * `+`-joined, NUL-terminated. On a target whose detected set is empty the
 * prefix is the target's own (`arm64-v8` on arm64) and no feature name is
 * accepted. ::GRJIT_FEATURES_STAMP_MAX bytes always suffice.
 *
 * @param features The set. A bit above ::GRJIT_FEATURES_ALL, or any bit on a
 *   target that has none, is ::GRJIT_ERR_INVALID.
 * @param buf Receives the text. Nothing is written to it unless it is whole.
 * @param size Its size in bytes.
 * @return ::GRJIT_OK; ::GRJIT_ERR_LIMIT if `buf` is too small (and, when `size`
 *   is not zero, `buf[0]` is set to NUL so nothing partial is mistaken for a
 *   stamp); ::GRJIT_ERR_INVALID.
 */
GRJIT_API GRJIT_Result grjit_features_stamp(
    GRJIT_Features features, char * buf, size_t size);

/**
 * @brief The inverse of ::grjit_features_stamp.
 *
 * Fails closed: an unknown feature name, a repeated one, one out of order, a
 * wrong prefix (an arm64 stamp on x86-64) or anything after the last name is
 * ::GRJIT_ERR_INVALID, so a stamp written by a build with a feature this one
 * lacks is refused and the code regenerated.
 *
 * @param stamp The text.
 * @param out Receives the set, only on success.
 * @return ::GRJIT_OK or ::GRJIT_ERR_INVALID.
 */
GRJIT_API GRJIT_Result grjit_features_from_stamp(
    const char * stamp, GRJIT_Features * out);

#ifdef __cplusplus
}
#endif

#endif /* GHOTI_IO_GRJIT_FEATURES_H */
