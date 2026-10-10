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
 * @file features_internal.h
 *
 * The pure parts of CPU feature handling that the tests reach directly: the
 * decoder over raw CPUID words and the mask algebra over a synthetic detected
 * set. Never installed.
 */

#ifndef GHOTI_IO_GRJIT_SRC_CODE_FEATURES_INTERNAL_H
#define GHOTI_IO_GRJIT_SRC_CODE_FEATURES_INTERNAL_H

#include <ghoti.io/runtime-jit/macros.h>

#include <ghoti.io/runtime-jit/features.h>

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Decodes the CPUID words into a feature set. The assembly that executes CPUID
 * is the only part of detection a table cannot test, so every rule is here:
 *
 * - SSE4.1 is leaf 1 ECX bit 19 and POPCNT is leaf 1 ECX bit 23;
 * - LZCNT is leaf `0x80000001` ECX bit 5, and never leaf 1 ECX bit 5 (that is
 *   VMX); it needs `max_ext >= 0x80000001`;
 * - BMI1 is leaf 7 subleaf 0 EBX bit 3, and only if `max_basic >= 7`: below that
 *   the leaf-7 word is not defined, whatever it holds;
 * - no feature is inferred from another (POPCNT is not taken from SSE4.2 or
 *   from LZCNT).
 *
 * @param max_basic EAX of leaf 0.
 * @param leaf1_ecx ECX of leaf 1.
 * @param max_ext EAX of leaf `0x80000000`.
 * @param ext1_ecx ECX of leaf `0x80000001`.
 * @param leaf7_ebx EBX of leaf 7, subleaf 0.
 */
GRJIT_INTERNAL_API GRJIT_Features grjit_features_from_cpuid(uint32_t max_basic,
    uint32_t leaf1_ecx, uint32_t max_ext, uint32_t ext1_ecx, uint32_t leaf7_ebx);

/** `detected & ~disabled & ::GRJIT_FEATURES_ALL`, for any `detected`: the rule
 *  ::grjit_features_effective applies to the host's set, testable against a
 *  synthetic one. */
GRJIT_INTERNAL_API GRJIT_Features grjit_features_apply_mask(
    GRJIT_Features detected, GRJIT_Features disabled);

/** The CPUID words of this host, read by the instruction itself and decoded by
 *  ::grjit_features_from_cpuid; ::GRJIT_FEATURES_NONE off x86-64. Not cached:
 *  ::grjit_features_detected is. A test compares the two. */
GRJIT_INTERNAL_API GRJIT_Features grjit_features_read_cpu(void);

#ifdef __cplusplus
}
#endif

#endif /* GHOTI_IO_GRJIT_SRC_CODE_FEATURES_INTERNAL_H */
