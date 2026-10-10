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
 * @file
 *
 * CPU feature detection, the mask that can only remove a feature, the subset
 * rule and the stamp text (spec-runtime-float, story 4). See features.h.
 */

#include <ghoti.io/runtime-jit/macros.h>

#include <ghoti.io/runtime-jit/features.h>

#include "features_internal.h"

#include <stdatomic.h>
#include <string.h>

#if defined(__x86_64__) || defined(_M_X64)
#define GRJIT_FEATURES_X86 1
#include <cpuid.h>
#else
#define GRJIT_FEATURES_X86 0
#endif

GRJIT_Features grjit_features_from_cpuid(uint32_t max_basic, uint32_t leaf1_ecx,
    uint32_t max_ext, uint32_t ext1_ecx, uint32_t leaf7_ebx) {
  GRJIT_Features f = GRJIT_FEATURES_NONE;
  /* Leaf 1 exists on every x86-64 CPU, so no maximum is checked for it. */
  if ((leaf1_ecx >> 19 & 1u) != 0) {
    f |= GRJIT_FEATURE_SSE4_1;
  }
  if ((leaf1_ecx >> 23 & 1u) != 0) {
    f |= GRJIT_FEATURE_POPCNT;
  }
#if defined(GRJIT_TEST_PLANT_BUG) && GRJIT_TEST_PLANT_BUG == 56
  /* Planted defect 56 (tests only): LZCNT is read from leaf 1, ECX bit 5, which is VMX. */
  if ((leaf1_ecx >> 5 & 1u) != 0) {
    f |= GRJIT_FEATURE_LZCNT;
  }
  (void)max_ext;
  (void)ext1_ecx;
#else
  if (max_ext >= UINT32_C(0x80000001) && (ext1_ecx >> 5 & 1u) != 0) {
    f |= GRJIT_FEATURE_LZCNT;
  }
#endif
#if defined(GRJIT_TEST_PLANT_BUG) && GRJIT_TEST_PLANT_BUG == 57
  /* Planted defect 57 (tests only): leaf 7 is read without checking the maximum basic leaf. */
  if ((leaf7_ebx >> 3 & 1u) != 0) {
    f |= GRJIT_FEATURE_BMI1;
  }
  (void)max_basic;
#else
  if (max_basic >= 7 && (leaf7_ebx >> 3 & 1u) != 0) {
    f |= GRJIT_FEATURE_BMI1;
  }
#endif
  return f;
}

GRJIT_Features grjit_features_apply_mask(GRJIT_Features detected, GRJIT_Features disabled) {
#if defined(GRJIT_TEST_PLANT_BUG) && GRJIT_TEST_PLANT_BUG == 59
  /* Planted defect 59 (tests only): the mask is applied to "every feature" and not to the
   * detected set, so a feature the CPU lacks appears. */
  (void)detected;
  return ~disabled & GRJIT_FEATURES_ALL;
#else
  return detected & ~disabled & GRJIT_FEATURES_ALL;
#endif
}

GRJIT_Features grjit_features_read_cpu(void) {
#if GRJIT_FEATURES_X86
  unsigned int a = 0, b = 0, c = 0, d = 0;
  /* The leaves beyond the maximum are read all the same and left to the decoder to ignore:
   * what a CPU returns for one is its own business, so the rule that it is not trusted is
   * exercised by the real instruction and not only by a table. */
  __cpuid_count(0, 0, a, b, c, d);
  const uint32_t max_basic = a;
  __cpuid_count(1, 0, a, b, c, d);
  const uint32_t leaf1_ecx = c;
  __cpuid_count(7, 0, a, b, c, d);
  const uint32_t leaf7_ebx = b;
  __cpuid_count(UINT32_C(0x80000000), 0, a, b, c, d);
  const uint32_t max_ext = a;
  __cpuid_count(UINT32_C(0x80000001), 0, a, b, c, d);
  const uint32_t ext1_ecx = c;
  return grjit_features_from_cpuid(max_basic, leaf1_ecx, max_ext, ext1_ecx, leaf7_ebx);
#else
  return GRJIT_FEATURES_NONE;
#endif
}

/* The detected set, computed once. A racing first call computes the same word and stores it
 * again; nothing else is written after that, and there is no lock. The top bit says "not yet
 * computed" (a feature set never has it: it is reserved). */
#define GRJIT_FEATURES_UNSET UINT32_C(0x80000000)
static _Atomic uint32_t g_detected = GRJIT_FEATURES_UNSET;

GRJIT_Features grjit_features_detected(void) {
  uint32_t v = atomic_load_explicit(&g_detected, memory_order_relaxed);
  if (v == GRJIT_FEATURES_UNSET) {
    v = grjit_features_read_cpu();
    atomic_store_explicit(&g_detected, v, memory_order_relaxed);
  }
  return v;
}

GRJIT_Features grjit_features_effective(GRJIT_Features disabled) {
#if defined(GRJIT_TEST_PLANT_BUG) && GRJIT_TEST_PLANT_BUG == 58
  /* Planted defect 58 (tests only): the mask is ignored. */
  (void)disabled;
  return grjit_features_detected();
#else
  return grjit_features_apply_mask(grjit_features_detected(), disabled);
#endif
}

bool grjit_features_subset(GRJIT_Features a, GRJIT_Features b) {
  return (a & ~b) == 0;
}

/* ---- The stamp --------------------------------------------------------------------------- */

typedef struct StampName {
  GRJIT_Features bit;
  const char * name;
} StampName;

/* In bit order, which is the canonical order of a stamp. */
static const StampName stamp_names[] = {
    {GRJIT_FEATURE_SSE4_1, "sse4.1"},
    {GRJIT_FEATURE_POPCNT, "popcnt"},
    {GRJIT_FEATURE_LZCNT, "lzcnt"},
    {GRJIT_FEATURE_BMI1, "bmi1"},
};
#define STAMP_NAME_COUNT (sizeof stamp_names / sizeof stamp_names[0])

#if GRJIT_FEATURES_X86
#define STAMP_PREFIX "x86_64-v1"
#define STAMP_FEATURES GRJIT_FEATURES_ALL
#elif defined(__aarch64__)
#define STAMP_PREFIX "arm64-v8"
#define STAMP_FEATURES GRJIT_FEATURES_NONE
#else
#define STAMP_PREFIX "generic-v0"
#define STAMP_FEATURES GRJIT_FEATURES_NONE
#endif

GRJIT_Result grjit_features_stamp(GRJIT_Features features, char * buf, size_t size) {
  if ((features & ~(GRJIT_Features)STAMP_FEATURES) != 0) {
    return GRJIT_ERR_INVALID;
  }
  char text[GRJIT_FEATURES_STAMP_MAX];
  size_t n = 0;
  memcpy(text, STAMP_PREFIX, sizeof STAMP_PREFIX - 1);
  n = sizeof STAMP_PREFIX - 1;
  for (size_t k = 0; k < STAMP_NAME_COUNT; k++) {
#if defined(GRJIT_TEST_PLANT_BUG) && GRJIT_TEST_PLANT_BUG == 68
    /* Planted defect 68 (tests only): the names are written in the wrong order, and the last
     * is dropped. */
    const size_t i = STAMP_NAME_COUNT - 1 - k;
    if (k == STAMP_NAME_COUNT - 1) {
      break;
    }
#else
    const size_t i = k;
#endif
    if ((features & stamp_names[i].bit) != 0) {
      const size_t len = strlen(stamp_names[i].name);
      text[n++] = '+';
      memcpy(text + n, stamp_names[i].name, len);
      n += len;
    }
  }
  text[n] = '\0';
  if (buf == NULL || size < n + 1) {
    if (buf != NULL && size != 0) {
      buf[0] = '\0';
    }
    return GRJIT_ERR_LIMIT;
  }
  memcpy(buf, text, n + 1);
  return GRJIT_OK;
}

GRJIT_Result grjit_features_from_stamp(const char * stamp, GRJIT_Features * out) {
  if (stamp == NULL || out == NULL) {
    return GRJIT_ERR_INVALID;
  }
  const size_t prefix_len = sizeof STAMP_PREFIX - 1;
  if (strncmp(stamp, STAMP_PREFIX, prefix_len) != 0) {
    return GRJIT_ERR_INVALID;
  }
  const char * p = stamp + prefix_len;
  GRJIT_Features got = GRJIT_FEATURES_NONE;
  size_t next = 0; /* names before this index are used up: the order is the bit order */
  while (*p != '\0') {
    if (*p != '+') {
      return GRJIT_ERR_INVALID; /* trailing junk, or a name run into the prefix */
    }
    p++;
    size_t found = STAMP_NAME_COUNT;
    for (size_t i = 0; i < STAMP_NAME_COUNT; i++) {
      const size_t len = strlen(stamp_names[i].name);
      if (strncmp(p, stamp_names[i].name, len) == 0 && (p[len] == '\0' || p[len] == '+')) {
        found = i;
        p += len;
        break;
      }
    }
    /* Unknown, repeated (its index is behind `next`) or out of order. On a target with no
     * feature every name is refused. */
    if (found == STAMP_NAME_COUNT || found < next ||
        (stamp_names[found].bit & (GRJIT_Features)STAMP_FEATURES) == 0) {
      return GRJIT_ERR_INVALID;
    }
    got |= stamp_names[found].bit;
    next = found + 1;
  }
  *out = got;
  return GRJIT_OK;
}
